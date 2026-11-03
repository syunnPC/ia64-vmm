/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Two-pass region formation from an immutable compile request, never a live
 * vCPU. Scan snapshots with code_source_read, decode with ia64_decode_bundle
 * and rebase instruction addresses to guest IP. Keep at most T1_LOOP_EXTEND
 * instructions past the first header backedge, ending at the farthest such
 * branch; discard lookahead instructions and pages. Admit one bounded
 * straight-line side path back to the head, or a captured branch
 * tail with two exits, then classify edges and leaders.
 *
 * The executor snapshots contracts and side-effect-free I-TLB-hit translations;
 * formation may derive callee pages from those translations. Resolve names
 * against the complete CFM with rse_bof=0: static GRs stay physical, stacked
 * GRs become frame offsets E_STK(o). Runtime home access applies the actual
 * entry BOF, so regions can be shared across frame bases.
 */
#include "engine/tier1/form.h"
#include "engine/tier1/helpers.h"
#include "ia64/fp.h"
#include "engine/cache/cache.h"
#include "engine/cache/validity.h"
#include "ia64/mmu.h"
#include "ia64/tlb.h"
#include "ia64/profile.h"
#include "ia64/semantics.h"
#include "ia64vmm/hal.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/string.h"

/* Opcode -> class of the Tier1 subset; -1 = not in the subset. */
int t1_opcode_class(Ia64Opcode op)
{
    switch (op) {
    case IA64_OP_ADDP4:
    case IA64_OP_ADDP4_IMM:
    case IA64_OP_SHLADDP4:
    case IA64_OP_ADD:
    case IA64_OP_ADD_ONE:
    case IA64_OP_SUB:
    case IA64_OP_SUB_ONE:
    case IA64_OP_SUB_IMM:
    case IA64_OP_ADDS:
    case IA64_OP_ADDL:
    case IA64_OP_AND:
    case IA64_OP_ANDCM:
    case IA64_OP_OR:
    case IA64_OP_XOR:
    case IA64_OP_AND_IMM:
    case IA64_OP_ANDCM_IMM:
    case IA64_OP_OR_IMM:
    case IA64_OP_XOR_IMM:
    case IA64_OP_SHLADD:
    case IA64_OP_SHL:
    case IA64_OP_SHR:
    case IA64_OP_SHRU:
    case IA64_OP_SHL_IMM:
    case IA64_OP_EXTR:
    case IA64_OP_EXTRU:
    case IA64_OP_DEPZ:
    case IA64_OP_DEPZ_IMM:
    case IA64_OP_SXT1:
    case IA64_OP_SXT2:
    case IA64_OP_SXT4:
    case IA64_OP_ZXT1:
    case IA64_OP_ZXT2:
    case IA64_OP_ZXT4:
    case IA64_OP_MOVL:
    case IA64_OP_MOV_CURRENT_IP:
        return T1_IC_ALU;
    /* Normal and parallel compares (ia64/decode/compare.inc rows with AND,
     * OR or OR_ANDCM updates) and tbit: native predicate logic (lift.c). */
    case IA64_OP_CMP_EQ:
    case IA64_OP_CMP_LT:
    case IA64_OP_CMP_LTU:
    case IA64_OP_CMP_EQ_IMM:
    case IA64_OP_CMP_LT_IMM:
    case IA64_OP_CMP_LTU_IMM:
    case IA64_OP_CMP4_EQ:
    case IA64_OP_CMP4_LT:
    case IA64_OP_CMP4_LTU:
    case IA64_OP_CMP4_EQ_IMM:
    case IA64_OP_CMP4_LT_IMM:
    case IA64_OP_CMP4_LTU_IMM:
    case IA64_OP_CMP_EQ_AND:
    case IA64_OP_CMP_NE_AND:
    case IA64_OP_CMP_GT_AND:
    case IA64_OP_CMP_LE_AND:
    case IA64_OP_CMP_GE_AND:
    case IA64_OP_CMP_LT_AND:
    case IA64_OP_CMP_EQ_OR:
    case IA64_OP_CMP_NE_OR:
    case IA64_OP_CMP_GT_OR:
    case IA64_OP_CMP_LE_OR:
    case IA64_OP_CMP_GE_OR:
    case IA64_OP_CMP_LT_OR:
    case IA64_OP_CMP_EQ_OR_ANDCM:
    case IA64_OP_CMP_NE_OR_ANDCM:
    case IA64_OP_CMP_GT_OR_ANDCM:
    case IA64_OP_CMP_LE_OR_ANDCM:
    case IA64_OP_CMP_GE_OR_ANDCM:
    case IA64_OP_CMP_LT_OR_ANDCM:
    case IA64_OP_CMP_EQ_AND_IMM:
    case IA64_OP_CMP_NE_AND_IMM:
    case IA64_OP_CMP_EQ_OR_IMM:
    case IA64_OP_CMP_NE_OR_IMM:
    case IA64_OP_CMP_EQ_OR_ANDCM_IMM:
    case IA64_OP_CMP_NE_OR_ANDCM_IMM:
    case IA64_OP_CMP4_EQ_AND:
    case IA64_OP_CMP4_NE_AND:
    case IA64_OP_CMP4_EQ_OR:
    case IA64_OP_CMP4_NE_OR:
    case IA64_OP_CMP4_EQ_OR_ANDCM:
    case IA64_OP_CMP4_NE_OR_ANDCM:
    case IA64_OP_CMP4_GT_AND:
    case IA64_OP_CMP4_LE_AND:
    case IA64_OP_CMP4_GE_AND:
    case IA64_OP_CMP4_LT_AND:
    case IA64_OP_CMP4_GT_OR:
    case IA64_OP_CMP4_LE_OR:
    case IA64_OP_CMP4_GE_OR:
    case IA64_OP_CMP4_LT_OR:
    case IA64_OP_CMP4_GT_OR_ANDCM:
    case IA64_OP_CMP4_LE_OR_ANDCM:
    case IA64_OP_CMP4_GE_OR_ANDCM:
    case IA64_OP_CMP4_LT_OR_ANDCM:
    case IA64_OP_CMP4_EQ_OR_ANDCM_IMM:
    case IA64_OP_CMP4_NE_OR_ANDCM_IMM:
    case IA64_OP_CMP4_EQ_AND_IMM:
    case IA64_OP_CMP4_NE_AND_IMM:
    case IA64_OP_CMP4_EQ_OR_IMM:
    case IA64_OP_CMP4_NE_OR_IMM:
    case IA64_OP_TBIT_Z:
    case IA64_OP_TBIT_NZ:
    case IA64_OP_TBIT_Z_OR_ANDCM:
    case IA64_OP_TBIT_NZ_OR_ANDCM:
        return T1_IC_CMP;
    /* tnat reads the NaT bit itself: the in-region exact helper. */
    case IA64_OP_TNAT_Z:
    case IA64_OP_TNAT_NZ:
    case IA64_OP_TNAT_NZ_AND:
        return T1_IC_HELPER;
    /* Speculative, advanced and check loads read as ld does on the pages a
     * window maps (whole WB RAM): the ALAT has no entries (ld.c reloads,
     * ld.a allocates nothing) and every deferral, fault or other page
     * exits before effect; classify keeps ld.s/ld.sa under PSR.dt = 0,
     * which always defer, as helpers. */
    case IA64_OP_LD1:
    case IA64_OP_LD2:
    case IA64_OP_LD4:
    case IA64_OP_LD8:
    case IA64_OP_LD1S:
    case IA64_OP_LD2S:
    case IA64_OP_LD4S:
    case IA64_OP_LD8S:
    case IA64_OP_LD1A:
    case IA64_OP_LD2A:
    case IA64_OP_LD4A:
    case IA64_OP_LD8A:
    case IA64_OP_LD1SA:
    case IA64_OP_LD2SA:
    case IA64_OP_LD4SA:
    case IA64_OP_LD8SA:
    case IA64_OP_LD1C_CLR:
    case IA64_OP_LD2C_CLR:
    case IA64_OP_LD4C_CLR:
    case IA64_OP_LD8C_CLR:
    case IA64_OP_LD1C_NC:
    case IA64_OP_LD2C_NC:
    case IA64_OP_LD4C_NC:
    case IA64_OP_LD8C_NC:
        return T1_IC_LOAD;
    case IA64_OP_ST1:
    case IA64_OP_ST2:
    case IA64_OP_ST4:
    case IA64_OP_ST8:
    case IA64_OP_ST1REL:
    case IA64_OP_ST2REL:
    case IA64_OP_ST4REL:
    case IA64_OP_ST8REL:
        return T1_IC_STORE;
    case IA64_OP_BR_COND:
        return T1_IC_BR;
    case IA64_OP_BR_CLOOP:
        return T1_IC_CLOOP;
    case IA64_OP_MF:
        return T1_IC_FENCE;
    case IA64_OP_NOP:
    case IA64_OP_BRP:
        return T1_IC_NOP;
    /* The register stack (SDM Vol.2 §6, Vol.3 alloc, br): frames,
     * direct/guarded calls, matched/observed returns and BR moves. */
    case IA64_OP_ALLOC:
        return T1_IC_ALLOC;
    case IA64_OP_BR_CALL:
    case IA64_OP_BRL_CALL:
    case IA64_OP_BR_CALL_INDIRECT:
        return T1_IC_CALL;
    case IA64_OP_BR_RET:
        return T1_IC_RET;
    case IA64_OP_MOV_BRGR:
    case IA64_OP_MOV_GRBR:
        return T1_IC_BRMOV;
    case IA64_OP_CHK_S:
        return T1_IC_CHK;
    case IA64_OP_CHK_A:
    case IA64_OP_CHK_A_CLR:
        return T1_IC_CHKA;
    default:
        return t1_helper_row(op) ? T1_IC_GENERAL : -1;
    }
}

unsigned t1_end_class(const Ia64Instruction *in)
{
    switch (in->opcode) {
    case IA64_OP_BR_CALL:
    case IA64_OP_BRL_CALL:
    case IA64_OP_BR_CALL_INDIRECT:
        return T1_END_CALL;
    case IA64_OP_BR_RET:
        return T1_END_RET;
    case IA64_OP_ALLOC:
        return T1_END_ALLOC;
    case IA64_OP_COVER:
    case IA64_OP_FLUSHRS:
    case IA64_OP_LOADRS:
        return T1_END_RSE;
    case IA64_OP_MOV_ARGR:
    case IA64_OP_MOV_GRAR:
    case IA64_OP_MOV_IMMAR: {
        unsigned ar = in->operands.common.source1;
        return ar == AR_RSC || ar == AR_BSP || ar == AR_BSPSTORE || ar == AR_RNAT ? T1_END_RSE
                                                                                  : T1_END_OTHER;
    }
    case IA64_OP_CHK_S:
        return T1_END_NAT;
    default:
        return T1_END_OTHER;
    }
}

static bool outside_frame(uint64_t cfm, unsigned reg)
{
    return reg >= 32 && reg - 32u >= (cfm & 127);
}

/*
 * The checks of ia64_execute/ia64_memory that Tier1 makes region
 * boundaries, applied regardless of qp (the lower tier evaluates
 * qp itself). Returns the class or -1 with a reason.
 */
static int classify(const Ia64Instruction *in, uint64_t cfm, unsigned model, uint64_t psr,
                    const char **why)
{
    int cls = t1_opcode_class(in->opcode);
    const IA64CommonOperands *op = &in->operands.common;
    if (!in->valid) {
        *why = "invalid encoding";
        return -1;
    }
    if (in->encoding_class != IA64_ENCODING_DEFINED) {
        *why = "encoding class";
        return -1;
    }
    if (in->placement_illegal) {
        *why = "illegal placement";
        return -1;
    }
    if (in->reserved_field) {
        *why = "reserved field";
        return -1;
    }
    /* frcpa/frsqrta clear p2 for a false qp: their exact helper does. */
    if (in->clear_p2_before_predicate &&
        !(cls == T1_IC_GENERAL && t1_helper_row(in->opcode)->cls == T1_HC_A)) {
        *why = "clears p2 before qp";
        return -1;
    }
    if (!ia64_model_instruction(model, in->opcode)) {
        *why = "not in the CPU profile";
        return -1;
    }
    if (cls < 0) {
        *why = "not in the v1 subset";
        return -1;
    }
    /* FP loads and stores of single, double and integer data are native
     * window accesses with the register-format conversion (lift.c);
     * speculative and advanced forms stay exact memory helpers. */
    bool fp_access = t1_fp_access(in->opcode) && !in->fp_load_speculative && !in->fp_load_advanced;
    if (fp_access) {
        bool store =
            in->opcode == IA64_OP_STFS || in->opcode == IA64_OP_STFD || in->opcode == IA64_OP_STF8;
        unsigned fr = store ? in->operands.memory.source : in->operands.memory.destination;
        if (!store && fr < 2) {
            *why = "FR destination f0/f1";
            return -1;
        }
        if (fr >= 2 && (psr & (fr >= 32 ? IA64_PSR_DFH : IA64_PSR_DFL))) {
            *why = "disabled FR bank";
            return -1;
        }
        cls = store ? T1_IC_STORE : T1_IC_LOAD;
    }
    /* Only wrapping packed arithmetic has a pure-IR lowering. Keep all
     * saturating forms on their existing exact-helper path. The normal
     * encoding/model and destination checks still apply to both paths. */
    if (!(in->operands.integer.immediate & 3)) {
        switch (in->opcode) {
        case IA64_OP_PADD1:
        case IA64_OP_PADD2:
        case IA64_OP_PADD4:
        case IA64_OP_PSUB1:
        case IA64_OP_PSUB2:
        case IA64_OP_PSUB4:
            cls = T1_IC_ALU;
            break;
        default:
            break;
        }
    }
    if (cls == T1_IC_GENERAL) {
        if (in->opcode == IA64_OP_MOV_ARGR || in->opcode == IA64_OP_MOV_GRAR ||
            in->opcode == IA64_OP_MOV_IMMAR) {
            unsigned ar = op->source1;
            /* CPL is fixed by the entry contract (SDM Vol.3 mov ar).
             * Value-dependent reserved-field faults remain exact helpers. */
            if (in->opcode != IA64_OP_MOV_ARGR && (ar <= 7 || ar == AR_CFLG) && ((psr >> 32) & 3)) {
                *why = "privileged AR write";
                return -1;
            }
            if (ar == AR_LC) {
                if (in->unit != IA64_UNIT_I) {
                    *why = "ar.lc in the M bank";
                    return -1;
                }
                cls = T1_IC_LC;
            }
            if (ar == AR_RSC || ar == AR_BSP || ar == AR_BSPSTORE || ar == AR_RNAT ||
                ar == AR_ITC || ar == AR_EFLAG) {
                *why = "AR changes the contract or clock";
                return -1;
            }
        }
    }
    if (cls == T1_IC_GENERAL) {
        const struct t1_helper_row *h = t1_helper_row(in->opcode);
        for (unsigned w = 0; w < 2; ++w) {
            const struct t1_hset *sets = w ? h->write : h->read;
            unsigned n = w ? h->nwrite : h->nread;
            for (unsigned k = 0; k < n; ++k)
                if (sets[k].loc == T1_HL_FR) {
                    unsigned fr = sets[k].slot == T1_HS_R1   ? op->destination
                                  : sets[k].slot == T1_HS_R2 ? op->source1
                                  : sets[k].slot == T1_HS_R3 ? op->source2
                                                             : op->auxiliary1;
                    if (fr >= 2 && (psr & (fr >= 32 ? IA64_PSR_DFH : IA64_PSR_DFL))) {
                        *why = "disabled FR bank";
                        return -1;
                    }
                }
        }
    }
    if (cls == T1_IC_LOAD && !(psr & IA64_PSR_DT) && t1_load_speculative(in->opcode))
        cls = T1_IC_GENERAL;
    if (cls == T1_IC_GENERAL && t1_helper_row(in->opcode)->cls == T1_HC_F) {
        bool getf = in->opcode == IA64_OP_GETF_SIG || in->opcode == IA64_OP_GETF_EXP ||
                    in->opcode == IA64_OP_GETF_S || in->opcode == IA64_OP_GETF_D;
        if (!getf && op->destination < 2) {
            *why = "FR destination f0/f1";
            return -1;
        }
        cls = T1_IC_FPI;
    }
    if (cls == T1_IC_ALLOC) {
        /* SDM Vol.3 alloc (ia64/rse.c): a predicate, a frame larger than
         * the stacked registers, sol > sof, a rotating region beyond sof,
         * r0 or a destination outside the new frame fault (Illegal
         * Operation), and so does a change of sor while a rotating base is
         * not zero (Reserved Register/Field). */
        unsigned sof = in->operands.system.immediate & 127,
                 sol = (in->operands.system.immediate >> 7) & 127;
        unsigned sor = (in->operands.system.immediate >> 14) & 15, r1 = op->destination;
        if (in->qp) {
            *why = "alloc under a predicate";
            return -1;
        }
        if (sof > IA64_N_STACKED_PHYS || sol > sof || sor * 8 > sof || !r1 ||
            (r1 >= 32 && r1 - 32u >= sof)) {
            *why = "alloc faults";
            return -1;
        }
        if (sor != ((cfm >> 14) & 15) && (cfm & (UINT64_C(0xfffff) << 18))) {
            *why = "alloc changes sor";
            return -1;
        }
        return cls;
    }
    if (cls == T1_IC_CHK && in->check_fp && op->source1 >= 2 &&
        (psr & (op->source1 >= 32 ? IA64_PSR_DFH : IA64_PSR_DFL))) {
        *why = "disabled FR bank";
        return -1;
    }
    const struct ia64_opcode_info *meta = ia64_opcode_info(in->opcode);
    if (cls == T1_IC_GENERAL && t1_helper_row(in->opcode)->cls == T1_HC_A) {
        const struct t1_helper_row *h = t1_helper_row(in->opcode);
        if (h->write[0].loc == T1_HL_FR && op->destination < 2) {
            *why = "FR destination f0/f1";
            return -1;
        }
        if (meta->predicate_pair && op->auxiliary1 == op->auxiliary2) {
            *why = "compare with p1 == p2";
            return -1;
        }
    }
    if (meta->writes_gr && (op->destination == 0 || outside_frame(cfm, op->destination))) {
        *why = "GR destination r0 or outside the frame";
        return -1;
    }
    if ((cls == T1_IC_CMP || cls == T1_IC_HELPER) && op->auxiliary1 == op->auxiliary2) {
        *why = "compare with p1 == p2";
        return -1;
    }
    if (cls == T1_IC_LOAD || cls == T1_IC_STORE) {
        const IA64MemoryOperands *m = &in->operands.memory;
        bool update = in->reg_base_update || in->imm_base_update;
        if (cls == T1_IC_STORE && in->reg_base_update) {
            *why = "store register update";
            return -1;
        }
        if (update && (!m->base || outside_frame(cfm, m->base))) {
            *why = "post-increment base r0 or outside the frame";
            return -1;
        }
        if (cls == T1_IC_LOAD && update && m->base == m->destination && !fp_access) {
            *why = "load post-increment base == destination";
            return -1;
        }
    }
    if (cls == T1_IC_CLOOP && in->slot != 2) {
        *why = "br.cloop not in slot 2";
        return -1;
    }
    return cls;
}

/* Fetch translation of a page without walking. */
static bool page_pa(struct vcpu *v, uint64_t ip, struct t1_compile_page *page, uint8_t *end)
{
    struct ia64_state *s = &v->s;
    const struct ia64_tlb_entry *e = NULL;
    if ((s->psr & IA64_PSR_IT) && !(e = ia64_tlb_find(s, ip, 0))) {
        *end = T1_PAGES_NO_ITLB;
        return false;
    }
    struct ia64_translation t;
    struct ia64_result r = ia64_translate(v, ip, IA64_REF_FETCH, IA64_ACCESS_EXECUTE,
                                          (unsigned)((s->psr >> 32) & 3), false, &t);
    if (r.kind != IA64_STEP_COMPLETE) {
        *end = T1_PAGES_FETCH_FAULT;
        return false;
    }
    gpa_t pa = t.pa & ~UINT64_C(4095);
    *page = (struct t1_compile_page){.va = ip,
                                     .pa = pa,
                                     .pte = e ? t.pte : 0,
                                     .itir = e ? e->itir : 0,
                                     .rr = e ? s->rr[ip >> 61] : 0,
                                     .shift = e ? e->page_shift : 12,
                                     .generation = code_source_generation(pa)};
    return true;
}

static const char *const pages_end_reason[] = {
    [T1_PAGES_LIMIT] = "page limit",
    [T1_PAGES_NO_ITLB] = "no I-TLB entry",
    [T1_PAGES_FETCH_FAULT] = "fetch translation faults",
};

/* names: the request's frame with rse_bof = 0, so ia64_gr_index yields
 * 32 + o for a stacked register (o after rotation by rrb.gr) and
 * ia64_pr_index the rotated predicate (rrb.pr), both contract constants. */
static void resolve_registers(const struct ia64_state *s, struct t1_insn *ti)
{
    const Ia64Instruction *in = &ti->insn;
    const IA64CommonOperands *op = &in->operands.common;
    ti->qp = (uint8_t)ia64_pr_index(s, in->qp);
    ti->p1 = (uint8_t)ia64_pr_index(s, op->auxiliary1 & 63);
    ti->p2 = (uint8_t)ia64_pr_index(s, op->auxiliary2 & 63);
    ti->fr[0] = ia64_fr_index(s, op->destination & 127);
    ti->fr[1] = ia64_fr_index(s, op->source1 & 127);
    ti->fr[2] = ia64_fr_index(s, op->source2 & 127);
    ti->fr[3] = ia64_fr_index(s, op->auxiliary1 & 127);
    ti->dst = (uint8_t)ia64_gr_index(s, op->destination & 127);
    ti->src1 = (uint8_t)ia64_gr_index(s, op->source1 & 127);
    ti->src2 = (uint8_t)ia64_gr_index(s, op->source2 & 127);
    if (ti->cls == T1_IC_LOAD || ti->cls == T1_IC_STORE) {
        const IA64MemoryOperands *m = &in->operands.memory;
        ti->dst = (uint8_t)ia64_gr_index(s, m->destination & 127);
        ti->base = (uint8_t)ia64_gr_index(s, m->base & 127);
        ti->source = (uint8_t)ia64_gr_index(s, m->source & 127);
    }
    if (ti->cls == T1_IC_CHK)
        ti->source = in->check_fp ? ia64_fr_index(s, in->operands.memory.source & 127)
                                  : (uint8_t)ia64_gr_index(s, in->operands.memory.source & 127);
    if (ti->cls == T1_IC_CALL)
        ti->breg = in->operands.branch.link & 7;
    if (ti->cls == T1_IC_RET)
        ti->breg = in->operands.branch.target & 7;
    if (ti->cls == T1_IC_BRMOV)
        ti->breg = in->operands.system.source & 7;
}

static bool page_listed(const struct t1_compile_request *rq, uint64_t ip)
{
    for (unsigned k = 0; k < rq->npages; ++k)
        if (rq->page[k].va == (ip & ~UINT64_C(4095)))
            return true;
    return false;
}

/*
 * Derive an unlisted transfer-target page from rq alone (SDM Vol.2 §4.1):
 * strip bit 63 in physical mode, or reuse a listed I-TLB translation covering
 * the same region/page in virtual mode. This preserves its checked rights,
 * key and attributes. Entry fetch guards and code dependencies cover the
 * new page normally. Return false if no listed translation covers it.
 */
static bool derive_page(const struct t1_compile_request *rq, uint64_t ip, gpa_t *pa)
{
    uint64_t va = ip & ~UINT64_C(4095);
    if (!(rq->psr & IA64_PSR_IT)) {
        *pa = va & ~(UINT64_C(1) << 63);
        return !(*pa >> (IA64_IMPL_PA_MSB + 1));
    }
    for (unsigned k = 0; k < rq->npages; ++k) {
        const struct t1_compile_page *p = &rq->page[k];
        if (p->shift < 12 || p->shift > 61)
            continue;
        uint64_t mask = (UINT64_C(1) << p->shift) - 1;
        if ((p->va & ~mask) == (va & ~mask)) {
            *pa = p->pa + (va - p->va);
            return true;
        }
    }
    return false;
}

/* A zero-locals caller keeps BOF/BSP/dirty unchanged and needs no fills,
 * even when its frame grows (SDM Vol.2 Table 6-2). The entry dirty guard
 * prevents output overflow (§6.5.5). */
static bool observed_return(const struct t1_compile_request *rq, const Ia64Instruction *in)
{
    uint64_t caller = rq->return_cfm;
    unsigned sof = caller & 127, sor = ((caller >> 14) & 15) * 8, rrb = (caller >> 18) & 127;
    unsigned b = in->operands.branch.target & 7;
    gpa_t pa;
    return rq->return_observed && !in->qp && (rq->br_observed & (1u << b)) && !(caller >> 38) &&
           !((caller >> 7) & 127) && sof <= IA64_N_STACKED_PHYS && sor <= sof &&
           (sor ? rrb < sor : !rrb) && ((caller >> 25) & 127) < 96 && ((caller >> 32) & 63) < 48 &&
           (page_listed(rq, rq->br_target[b]) || derive_page(rq, rq->br_target[b], &pa));
}

/* Bundles the executor reads from the head for transfer-target pages
 * (request_targets). */
#define T1_CALLEE_SCAN 32u

/*
 * Add resident fetch translations for unconditional calls and observed
 * returns on the head's straight path, within T1_CALLEE_SCAN bundles.
 * Direct calls reuse covering translations; indirect calls and returns
 * list the target page so formation can snapshot it. When full,
 * replace a page after the entry's second page. An ordinary unconditional
 * branch may capture one separate target translation for branch_tail;
 * it must not become a listed page that the main scan follows freely.
 * Reads architectural state and changes only rq and derived MMU caches.
 */
static void request_targets(struct vcpu *v, struct t1_compile_request *rq)
{
    unsigned sequential = rq->npages;
    uint64_t ip = rq->ip;
    unsigned slot = rq->slot;
    for (unsigned n = 0; n < T1_CALLEE_SCAN; ++n, ip += 16, slot = 0) {
        unsigned k = 0;
        while (k < rq->npages && rq->page[k].va != (ip & ~UINT64_C(4095)))
            ++k;
        if (k == rq->npages)
            return;
        gpa_t pa = rq->page[k].pa + (ip & 4095);
        uint64_t low, high, generation;
        if (code_source_read_physical(pa, &low, &high, &generation).kind != IA64_STEP_COMPLETE)
            return;
        struct ia64_bundle bundle;
        ia64_decode_bundle(low, high, pa, &bundle);
        if (!ia64_template_info(bundle.template_code)->defined)
            return;
        for (; slot < 3 && !(bundle.long_instruction && slot == 2); ++slot) {
            const Ia64Instruction *in = &bundle.insn[slot];
            Ia64Opcode op = in->opcode;
            bool indirect = op == IA64_OP_BR_CALL_INDIRECT;
            bool ret = op == IA64_OP_BR_RET && rq->return_observed;
            if ((op == IA64_OP_BR_CALL || op == IA64_OP_BRL_CALL || indirect || ret) && !in->qp) {
                uint64_t target = indirect || ret ? rq->br_target[in->operands.branch.target & 7]
                                                  : ip + (uint64_t)in->operands.branch.displacement;
                gpa_t callee;
                struct t1_compile_page page;
                uint8_t end;
                if (!page_listed(rq, target) &&
                    (indirect || ret || !derive_page(rq, target, &callee)) &&
                    (rq->npages < T1_MAX_PAGES || sequential > 2) &&
                    page_pa(v, target & ~UINT64_C(4095), &page, &end)) {
                    if (rq->npages == T1_MAX_PAGES) {
                        rq->page[--sequential] = page;
                        rq->pages_end = T1_PAGES_LIMIT;
                    } else
                        rq->page[rq->npages++] = page;
                }
            }
            if (in->valid && op == IA64_OP_BR_COND && !in->qp) {
                uint64_t target = ip + (uint64_t)in->operands.branch.displacement;
                gpa_t pa;
                uint8_t end;
                if (!page_listed(rq, target) && !derive_page(rq, target, &pa))
                    rq->tail_observed = page_pa(v, target & ~UINT64_C(4095), &rq->tail_page, &end);
            }
            bool always = !in->qp && op != IA64_OP_BR_CALL && op != IA64_OP_BRL_CALL && !indirect;
            /* Do not snapshot a loop's lookahead pages. */
            if ((op == IA64_OP_BR_COND || op == IA64_OP_BRL_COND || op == IA64_OP_BR_CLOOP) &&
                in->operands.branch.displacement <= 0)
                return;
            if (op == IA64_OP_BR_RET || op == IA64_OP_BR_IA || op == IA64_OP_BR_INDIRECT ||
                op == IA64_OP_RFI || (always && (op == IA64_OP_BR_COND || op == IA64_OP_BRL_COND)))
                return;
        }
    }
}

bool t1_request_from_vcpu(struct vcpu *v, const struct t1_compile_options *opt,
                          unsigned hostwin_bits, struct t1_compile_request *rq, const char **reason)
{
    const struct ia64_state *s = &v->s;
    *reason = NULL;
    /* Entry preconditions; the dispatcher checks them before lookup too. */
    if (s->slot > 2 || (s->ip & 15)) {
        *reason = "invalid ip/slot";
        return false;
    }
    if (s->psr & (IA64_PSR_BE | IA64_PSR_IS | T1_PSR_LOWER_TIER)) {
        *reason = "PSR requires the lower tier";
        return false;
    }
    /* Incomplete register-stack frame (dispatch.c rse_incomplete): the fetch
     * translations below would perform the mandatory RSE loads. */
    if (s->rse_cfle || s->rse_missing) {
        *reason = "incomplete RSE frame";
        return false;
    }
    *rq = (struct t1_compile_request){.ip = s->ip,
                                      .psr = s->psr & T1_CONTRACT_PSR_MASK,
                                      .cfm = s->cfm,
                                      .slot = (uint8_t)s->slot,
                                      .cpu_model = v->cfg.cpu_model,
                                      .icount = v->cfg.itc_icount,
                                      .hostwin_bits = hostwin_bits,
                                      .opt = *opt,
                                      .pages_end = T1_PAGES_LIMIT,
                                      .mmu_seq = v->mmu_seq,
                                      .vcpu = v->cfg.index};
    uint64_t page_ip = s->ip & ~UINT64_C(4095);
    while (rq->npages < T1_MAX_PAGES &&
           page_pa(v, page_ip + 4096u * rq->npages, &rq->page[rq->npages], &rq->pages_end))
        ++rq->npages;
    if (!rq->npages) {
        *reason = pages_end_reason[rq->pages_end];
        return false;
    }
    rq->entry_pa = rq->page[0].pa + (s->ip & 4095);
    for (unsigned b = 0; b < 8; ++b)
        rq->br_target[b] = s->br[b] & ~UINT64_C(15);
    rq->br_observed = UINT8_MAX;
    rq->return_cfm = s->ar[AR_PFS] & ((UINT64_C(1) << 38) - 1);
    rq->return_observed = true;
    request_targets(v, rq);
    /* No frame base: requests at every BOF of one (header, CFM) coalesce. */
    uint64_t h = ((uint64_t)rq->cpu_model << 51) ^ rq->entry_pa ^ rq->ip ^ (rq->psr << 7) ^
                 (rq->cfm << 13) ^ ((uint64_t)rq->slot << 3) ^ ((uint64_t)rq->opt.flags << 40) ^
                 ((uint64_t)rq->icount << 2) ^ ((uint64_t)rq->hostwin_bits << 47);
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    rq->key = h ? h : 1;
    return true;
}

/* The class formation takes an instruction of rq's region with, or -1
 * (*why): classify, a store compiled as an exact memory helper
 * (slow_memory), and the end before a class M helper in a checked region,
 * whose undo ring describes native stores only. */
static int take(const Ia64Instruction *in, const struct t1_compile_request *rq, uint64_t cfm,
                const char **why)
{
    int cls = classify(in, cfm, rq->cpu_model, rq->psr & T1_CONTRACT_PSR_MASK, why);
    if (cls < 0)
        return -1;
    if (in->opcode == IA64_OP_BR_CALL_INDIRECT) {
        unsigned b = in->operands.branch.target & 7;
        gpa_t pa;
        if (in->qp || !(rq->br_observed & (1u << b)) ||
            (!page_listed(rq, rq->br_target[b]) && !derive_page(rq, rq->br_target[b], &pa))) {
            *why = "indirect call target not admitted";
            return -1;
        }
    }
    if (rq->slow_memory && cls == T1_IC_STORE)
        cls = T1_IC_GENERAL;
    const struct t1_helper_row *hr = t1_helper_row(in->opcode);
    if (cls == T1_IC_GENERAL && hr && hr->cls == T1_HC_M && (rq->opt.flags & T1_COMPILE_UNDO_LOG)) {
        *why = "checked memory helper boundary";
        return -1;
    }
    return cls;
}

bool t1_request_head_allowed(const struct t1_compile_request *rq, unsigned *stop)
{
    uint64_t low, high, generation;
    if (code_source_read_physical(rq->entry_pa, &low, &high, &generation).kind !=
        IA64_STEP_COMPLETE)
        return true;
    struct ia64_bundle bundle;
    ia64_decode_bundle(low, high, rq->entry_pa, &bundle);
    *stop = IA64_OP_COUNT;
    if (!ia64_template_info(bundle.template_code)->defined ||
        (bundle.long_instruction && rq->slot == 2))
        return false;
    Ia64Instruction in = bundle.insn[rq->slot];
    in.address = rq->ip;
    const char *why;
    /* A head return needs the observed caller specialization. */
    int cls = take(&in, rq, rq->cfm, &why);
    if (cls >= 0 && (cls != T1_IC_RET || observed_return(rq, &in)))
        return true;
    *stop = in.opcode;
    return false;
}

bool t1_trace_head_allowed(struct vcpu *v, const struct t1_compile_options *opt)
{
    struct t1_compile_request rq;
    const char *why;
    unsigned stop;
    return t1_request_from_vcpu(v, opt, 0, &rq, &why) && t1_request_head_allowed(&rq, &stop);
}

enum t1_compile_status t1_form(struct vcpu *v, uint32_t flags, unsigned max_insns,
                               struct t1_form *f, const char **reason)
{
    struct t1_compile_request rq;
    const struct t1_compile_options opt = {.flags = flags};
    f->ninsns = f->npages = 0;
    f->loop = false;
    f->transient = false;
    f->end_reason = NULL;
    f->end_class = T1_END_LIMIT;
    if (!t1_request_from_vcpu(v, &opt, 0, &rq, reason))
        return T1_REJECTED;
    size_t pages = (sizeof(struct ia64_state) + 4095) / 4096;
    struct ia64_state *names = t1_workspace_alloc(pages, HAL_PAGE_ZERO);
    if (!names) {
        *reason = "no memory";
        return T1_NO_MEMORY;
    }
    enum t1_compile_status st = t1_form_request(&rq, names, max_insns, f, reason);
    t1_workspace_free(names, pages);
    return st;
}

/* Instructions that transfer control: their edges are classified in pass 2. */
static bool is_branch(const struct t1_insn *ti)
{
    return ti->cls == T1_IC_BR || ti->cls == T1_IC_CLOOP || ti->cls == T1_IC_CALL ||
           ti->cls == T1_IC_RET || ti->cls == T1_IC_CHK || ti->cls == T1_IC_CHKA;
}
/* Always taken: a branch, call or return under p0 has no fall-through. */
static bool unconditional(const struct t1_insn *ti)
{
    return (ti->cls == T1_IC_BR || ti->cls == T1_IC_CALL || ti->cls == T1_IC_RET ||
            ti->cls == T1_IC_CHKA) &&
           ti->insn.qp == 0;
}
/* The frame state on the taken path of a branch. */
static uint8_t taken_fs(const struct t1_insn *ti)
{
    return ti->cls == T1_IC_CALL || ti->cls == T1_IC_RET ? ti->fs_taken : ti->fs;
}

/*
 * Pass 2 classifies edges and leaders. Search bundle-start targets in scan
 * order: stitched targets may lie below entry IP. Internal edges require
 * matching frame state. Accept backward candidates only when the target
 * dominates the source; unreachable blocks dominate nothing. Header backedges
 * were found in pass 1; calls/returns are never backedges. Other edges exit.
 */
static void classify_edges(struct t1_form *f)
{
    unsigned n = f->ninsns;
    uint16_t cand[T1_MAX_INSNS];
    bool start[T1_MAX_INSNS];
    for (unsigned i = 0; i < n; ++i) {
        struct t1_insn *ti = &f->insn[i];
        cand[i] = T1_NO_INSN;
        start[i] = i == 0;
        if (!is_branch(ti))
            continue;
        if (ti->edge == T1_EDGE_BACK) {
            ti->target_insn = 0;
            continue;
        }
        ti->edge = T1_EDGE_EXIT;
        uint8_t fs = taken_fs(ti);
        for (unsigned j = i + 1; j < n; ++j)
            if (f->insn[j].insn.address == ti->target && f->insn[j].insn.slot == 0 &&
                f->insn[j].fs == fs) {
                ti->edge = T1_EDGE_INTERNAL;
                ti->target_insn = (uint16_t)j;
                break;
            }
        if (ti->edge == T1_EDGE_EXIT && ti->cls != T1_IC_CALL && ti->cls != T1_IC_RET)
            for (unsigned j = i + 1; j-- > 0;)
                if (f->insn[j].insn.address == ti->target && f->insn[j].insn.slot == 0) {
                    if (f->insn[j].fs == fs)
                        cand[i] = (uint16_t)j;
                    break;
                }
    }
    for (unsigned i = 0; i < n; ++i) {
        if (!is_branch(&f->insn[i]))
            continue;
        if (i + 1 < n)
            start[i + 1] = true;
        if (f->insn[i].edge == T1_EDGE_INTERNAL)
            start[f->insn[i].target_insn] = true;
        if (cand[i] != T1_NO_INSN)
            start[cand[i]] = true;
    }
    uint8_t block_of[T1_MAX_INSNS];
    unsigned nb = 0;
    for (unsigned i = 0; i < n; ++i) {
        nb += start[i];
        block_of[i] = (uint8_t)(nb - 1);
    }
    /* Beyond 64 blocks the region does not lift anyway: no candidate. */
    if (nb <= 64) {
        uint64_t pred[64] = {0}, all = nb == 64 ? ~UINT64_C(0) : (UINT64_C(1) << nb) - 1;
        for (unsigned i = 0; i < n; ++i) {
            if (i + 1 < n && !start[i + 1])
                continue; /* not the last instruction of its block */
            const struct t1_insn *ti = &f->insn[i];
            uint64_t from = UINT64_C(1) << block_of[i];
            bool falls = i + 1 < n && !ti->fall_exit && !unconditional(ti);
            if (is_branch(ti)) {
                if (ti->edge == T1_EDGE_BACK)
                    pred[0] |= from;
                else if (ti->edge == T1_EDGE_INTERNAL)
                    pred[block_of[ti->target_insn]] |= from;
                else if (cand[i] != T1_NO_INSN)
                    pred[block_of[cand[i]]] |= from;
            }
            if (falls)
                pred[block_of[i + 1]] |= from;
        }
        uint64_t reach = 1;
        for (bool grew = true; grew;) {
            grew = false;
            for (unsigned b = 1; b < nb; ++b)
                if (!(reach & (UINT64_C(1) << b)) && (pred[b] & reach)) {
                    reach |= UINT64_C(1) << b;
                    grew = true;
                }
        }
        uint64_t dom[64];
        for (unsigned b = 0; b < nb; ++b)
            dom[b] = b == 0 ? 1 : (reach & (UINT64_C(1) << b)) ? all : 0;
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned b = 1; b < nb; ++b) {
                if (!(reach & (UINT64_C(1) << b)))
                    continue;
                uint64_t d = all;
                for (unsigned p = 0; p < nb; ++p)
                    if ((pred[b] & reach & (UINT64_C(1) << p)))
                        d &= dom[p];
                d |= UINT64_C(1) << b;
                if (d != dom[b]) {
                    dom[b] = d;
                    changed = true;
                }
            }
        }
        for (unsigned i = 0; i < n; ++i)
            if (cand[i] != T1_NO_INSN && ((dom[block_of[i]] >> block_of[cand[i]]) & 1)) {
                f->insn[i].edge = T1_EDGE_BACK;
                f->insn[i].target_insn = cand[i];
            }
    }
    f->insn[0].leader = true;
    for (unsigned i = 0; i < n; ++i) {
        const struct t1_insn *ti = &f->insn[i];
        if (!is_branch(ti))
            continue;
        if (i + 1 < n)
            f->insn[i + 1].leader = true;
        if (ti->edge == T1_EDGE_INTERNAL || ti->edge == T1_EDGE_BACK)
            f->insn[ti->target_insn].leader = true;
    }
}

/* The index of frame state (cfm, delta, call), added if new; from `first`
 * on (1 when the entry's state is not a known frame, the alloc head of a
 * contract that leaves sof, sol and sor open). -1: the table is full. */
static int frame_state(struct t1_form *f, uint64_t cfm, unsigned delta, unsigned call,
                       unsigned first)
{
    for (unsigned k = first; k < f->nfs; ++k)
        if (f->fs[k].cfm == cfm && f->fs[k].delta == delta && f->fs[k].call == call)
            return (int)k;
    if (f->nfs == T1_MAX_FSTATES)
        return -1;
    f->fs[f->nfs] = (struct t1_fstate){cfm, (uint8_t)delta, (uint8_t)call};
    return (int)f->nfs++;
}

/* A short taken path can close the existing head loop even when linear
 * lookahead never reaches it. Read only request translations whose pages
 * already have snapshots; a failed probe must not track unrelated pages.
 * No frame changes or intermediate transfers are admitted. */
#define T1_SIDE_INSNS 12u
static bool side_path(const struct t1_compile_request *rq, struct ia64_state *names,
                      unsigned max_insns, struct t1_form *f, uint64_t ip)
{
    unsigned n = f->ninsns, pages = f->npages, count = 0;
    while (count < T1_SIDE_INSNS && n + count < max_insns) {
        uint64_t page_ip = ip & ~UINT64_C(4095);
        unsigned pg = 0;
        while (pg < f->npages && f->page[pg].ip_base != page_ip)
            ++pg;
        if (pg == f->npages) {
            unsigned source = 0;
            while (source < rq->npages && rq->page[source].va != page_ip)
                ++source;
            if (source == rq->npages || pg == T1_MAX_PAGES ||
                !code_page_has_snapshot(rq->page[source].pa))
                break;
            f->page[f->npages++] = (struct t1_page){page_ip, rq->page[source].pa, 0};
        }
        struct t1_page *page = &f->page[pg];
        uint64_t low, high, generation;
        gpa_t pa = page->pa + (ip & 4095);
        if (code_source_read_physical(pa, &low, &high, &generation).kind != IA64_STEP_COMPLETE)
            break;
        if (!page->generation)
            page->generation = generation;
        else if (page->generation != generation)
            break;
        struct ia64_bundle bundle;
        ia64_decode_bundle(low, high, pa, &bundle);
        if (!ia64_template_info(bundle.template_code)->defined)
            break;
        for (unsigned slot = 0; slot < 3; ++slot) {
            if (count == T1_SIDE_INSNS || n + count == max_insns)
                goto reject;
            const char *why;
            Ia64Instruction in = bundle.insn[slot];
            in.address = ip;
            int cls = take(&in, rq, f->fs[0].cfm, &why);
            bool back = cls == T1_IC_BR && slot == 2 &&
                        ip + (uint64_t)in.operands.branch.displacement == f->c.ip;
            if (!back && cls != T1_IC_NOP && cls != T1_IC_ALU && cls != T1_IC_CMP &&
                cls != T1_IC_LOAD && cls != T1_IC_STORE)
                goto reject;
            unsigned step = bundle.long_instruction && slot == 1 ? 2 : 1;
            struct t1_insn *ti = &f->insn[n + count++];
            memset(ti, 0, sizeof(*ti));
            ti->insn = in;
            ti->bundle_pa = pa;
            ti->cls = (uint8_t)cls;
            ti->page = (uint8_t)pg;
            ti->next_slot = (uint8_t)((slot + step) % 3);
            ti->next_ip = slot + step >= 3 ? ip + 16 : ip;
            names->cfm = f->fs[0].cfm;
            names->rse_bof = 0;
            resolve_registers(names, ti);
            if (back) {
                ti->edge = T1_EDGE_BACK;
                ti->target = f->c.ip;
                f->insn[n - 1].fall_exit = true;
                f->ninsns = n + count;
                return true;
            }
            slot += step - 1;
        }
        ip += 16;
    }
reject:
    f->npages = pages;
    return false;
}

static void include_side_path(const struct t1_compile_request *rq, struct ia64_state *names,
                              unsigned max_insns, struct t1_form *f)
{
    if (!f->loop || f->nfs != 1 || f->c.slot || f->c.cfm_mask != ~UINT64_C(0))
        return;
    for (unsigned i = 0; i < f->ninsns; ++i)
        if (f->insn[i].cls == T1_IC_GENERAL || f->insn[i].cls == T1_IC_HELPER ||
            f->insn[i].cls == T1_IC_ALLOC || f->insn[i].cls == T1_IC_CALL ||
            f->insn[i].cls == T1_IC_RET)
            return;
    for (unsigned i = 0; i < f->ninsns; ++i) {
        const struct t1_insn *ti = &f->insn[i];
        if (ti->cls != T1_IC_BR || !ti->qp)
            continue;
        unsigned j = 0;
        while (j < f->ninsns && f->insn[j].insn.address != ti->target)
            ++j;
        if (j == f->ninsns && side_path(rq, names, max_insns, f, ti->target))
            return;
    }
}

/* A transfer may include one already captured discriminator bundle,
 * using only request translations. Both outcomes remain exits. */
static bool branch_tail(const struct t1_compile_request *rq, struct ia64_state *names,
                        unsigned max_insns, struct t1_form *f, uint64_t ip)
{
    if (f->loop || f->nfs != 1 || f->c.cfm_mask != ~UINT64_C(0) || f->npages == T1_MAX_PAGES ||
        f->ninsns > max_insns || max_insns - f->ninsns < 3)
        return false;
    for (unsigned k = 0; k < f->ninsns; ++k)
        if (f->insn[k].cls == T1_IC_GENERAL || f->insn[k].cls == T1_IC_HELPER ||
            f->insn[k].cls == T1_IC_ALLOC || f->insn[k].cls == T1_IC_CALL ||
            f->insn[k].cls == T1_IC_RET)
            return false;
    gpa_t page;
    if (!derive_page(rq, ip, &page)) {
        if (!rq->tail_observed || rq->tail_page.va != (ip & ~UINT64_C(4095)))
            return false;
        page = rq->tail_page.pa;
    }
    uint64_t low, high, generation;
    bool take_lock = !code_lock_held();
    if (take_lock)
        code_lock();
    /* Keep the ready/tracked check and read together: a concurrent
     * eviction must not turn this probe into a new snapshot or tracking. */
    bool ready = code_page_has_snapshot(page) && code_page_tracked_fast(page) &&
                 code_source_read_physical(page + (ip & 4095), &low, &high, &generation).kind ==
                     IA64_STEP_COMPLETE;
    if (take_lock)
        code_unlock();
    if (!ready)
        return false;
    struct ia64_bundle bundle;
    ia64_decode_bundle(low, high, page + (ip & 4095), &bundle);
    if (bundle.long_instruction || !ia64_template_info(bundle.template_code)->defined)
        return false;
    struct t1_insn tail[3];
    for (unsigned slot = 0; slot < 3; ++slot) {
        Ia64Instruction in = bundle.insn[slot];
        in.address = ip;
        const char *why;
        int cls = take(&in, rq, f->fs[0].cfm, &why);
        if (slot < 2 ? cls != T1_IC_NOP && cls != T1_IC_ALU && cls != T1_IC_CMP
                     : cls != T1_IC_BR || !in.qp)
            return false;
        struct t1_insn *ti = &tail[slot];
        memset(ti, 0, sizeof(*ti));
        ti->insn = in;
        ti->bundle_pa = page + (ip & 4095);
        ti->cls = (uint8_t)cls;
        ti->page = (uint8_t)f->npages;
        ti->next_slot = (uint8_t)((slot + 1) % 3);
        ti->next_ip = slot == 2 ? ip + 16 : ip;
        names->cfm = f->fs[0].cfm;
        names->rse_bof = 0;
        resolve_registers(names, ti);
    }
    tail[2].target = ip + (uint64_t)tail[2].insn.operands.branch.displacement;
    if (tail[2].target == ip)
        return false;
    for (unsigned k = 0; k < f->ninsns; ++k)
        if (f->insn[k].insn.address == tail[2].target)
            return false;
    /* No partial path or dependency survives a failed probe. */
    f->page[f->npages++] = (struct t1_page){ip & ~UINT64_C(4095), page, generation};
    memcpy(&f->insn[f->ninsns], tail, sizeof(tail));
    f->ninsns += 3;
    return true;
}

enum t1_compile_status t1_form_request(const struct t1_compile_request *rq,
                                       struct ia64_state *names, unsigned max_insns,
                                       struct t1_form *f, const char **reason)
{
    const char *why = NULL;
    f->ninsns = f->npages = 0;
    f->loop = false;
    f->transient = false;
    f->end_reason = NULL;
    f->end_class = T1_END_LIMIT;
    f->c = (struct t1_contract){.ip = rq->ip,
                                .entry_pa = rq->entry_pa,
                                .slot = rq->slot,
                                .psr = rq->psr & T1_CONTRACT_PSR_MASK,
                                .psr_mask = T1_CONTRACT_PSR_MASK,
                                .cfm = rq->cfm,
                                .cfm_mask = ~UINT64_C(0),
                                .cpu_model = rq->cpu_model,
                                .abi = T1_ABI_VERSION,
                                .flags = rq->opt.flags,
                                .hostwin_bits = rq->hostwin_bits,
                                .icount = rq->icount};
    f->nfs = 1;
    f->fs[0] = (struct t1_fstate){rq->cfm, 0, 0};
    f->calls = false;
    f->end_fs = 0;
    if (max_insns > T1_MAX_INSNS)
        max_insns = T1_MAX_INSNS;
    *reason = NULL;
    /* The request's own preconditions (t1_request_from_vcpu), for requests
     * that did not come from there. */
    if (rq->slot > 2 || (rq->ip & 15) || !rq->npages || rq->npages > T1_MAX_PAGES ||
        (rq->psr & (IA64_PSR_BE | IA64_PSR_IS | T1_PSR_LOWER_TIER))) {
        *reason = "malformed request";
        return T1_REJECTED;
    }

    uint64_t bundle_ip = rq->ip;
    unsigned slot = rq->slot;
    bool stop = false;
    /* Instructions through the farthest back-edge, and the scan limit
     * T1_LOOP_EXTEND past the first. */
    unsigned through_back = 0, extend_limit = max_insns;
    /* The frame state scanned in, the first state a new one may share
     * (frame_state), and the calls followed into their callees: the return
     * address, the callee, the call's index and the caller's state. */
    unsigned cur = 0, first_fs = 0, depth = 0, ncalls = 0;
    struct {
        uint64_t ret_ip, target;
        unsigned at;
        uint8_t fs;
    } calls[T1_MAX_CALL_DEPTH];
    /* The scan stopped after a call or return it did not follow. */
    bool left = false;
    /* Callee pages the request does not list (derive_page). */
    struct t1_compile_page derived[T1_MAX_PAGES] = {{0}};
    unsigned nderived = 0;
    while (!stop) {
        uint64_t page_ip = bundle_ip & ~UINT64_C(4095);
        unsigned pg = 0;
        while (pg < f->npages && f->page[pg].ip_base != page_ip)
            ++pg;
        bool fresh = pg == f->npages;
        if (fresh) {
            if (f->npages == T1_MAX_PAGES) {
                why = "page limit";
                break;
            }
            unsigned source = 0, d = 0;
            while (source < rq->npages && rq->page[source].va != page_ip)
                ++source;
            while (source == rq->npages && d < nderived && derived[d].va != page_ip)
                ++d;
            if (source == rq->npages && d == nderived) {
                why = pages_end_reason[rq->pages_end];
                break;
            }
            gpa_t page_pa = source < rq->npages ? rq->page[source].pa : derived[d].pa;
            /* Past a back-edge the scan is lookahead: reading a page would
             * track it (snapshot, write-protected aliases) although it may
             * hold only data. */
            if (f->loop && !code_page_tracked_fast(page_pa)) {
                why = "lookahead page not tracked";
                break;
            }
            f->page[pg] = (struct t1_page){page_ip, page_pa, 0};
            ++f->npages;
        }
        struct t1_page *page = &f->page[pg];
        gpa_t bundle_pa = page->pa + (bundle_ip & 4095);
        uint64_t low, high, generation;
        struct ia64_result r = code_source_read_physical(bundle_pa, &low, &high, &generation);
        if (r.kind != IA64_STEP_COMPLETE) {
            why = "code_source_read failed";
            f->transient = true;
            if (fresh)
                --f->npages;
            break;
        }
        /* Compilation needs a snapshot (validity mode != NONE). */
        if (!code_page_has_snapshot(page->pa)) {
            f->transient = true;
            *reason = "no code snapshot";
            return T1_REJECTED;
        }
        if (!page->generation)
            page->generation = generation;
        else if (page->generation != generation) {
            *reason = "page changed";
            return T1_RETRY;
        }

        struct ia64_bundle bundle;
        ia64_decode_bundle(low, high, bundle_pa, &bundle);
        if (!ia64_template_info(bundle.template_code)->defined) {
            why = "undefined template";
            break;
        }
        bool stitched = false;
        for (; slot < 3; ++slot) {
            if (bundle.long_instruction && slot == 2) {
                why = "slot 2 of an MLX bundle";
                stop = true;
                break;
            }
            if (f->ninsns >= max_insns) {
                why = "size limit";
                stop = true;
                break;
            }
            if (f->ninsns >= extend_limit) {
                why = "loop extension limit";
                stop = true;
                break;
            }
            struct t1_insn *ti = &f->insn[f->ninsns];
            memset(ti, 0, sizeof(*ti));
            ti->insn = bundle.insn[slot];
            ti->insn.address = bundle_ip; /* rebase */
            const struct t1_fstate *st = &f->fs[cur];
            int cls = take(&ti->insn, rq, st->cfm, &why);
            if (cls == T1_IC_RET && !depth && !observed_return(rq, &ti->insn)) {
                why = "return caller not admitted";
                cls = -1;
            }
            if (cls < 0) {
                t1_form_stop_note(ti->insn.opcode, !f->ninsns);
                f->end_class = (uint8_t)t1_end_class(&ti->insn);
                stop = true;
                break;
            }
            ti->cls = (uint8_t)cls;
            ti->bundle_pa = bundle_pa;
            ti->page = (uint8_t)pg;
            ti->fs = ti->fs_taken = (uint8_t)cur;
            unsigned step = bundle.long_instruction && slot == 1 ? 2 : 1;
            ti->next_slot = (uint8_t)((slot + step) % 3);
            ti->next_ip = slot + step >= 3 ? bundle_ip + 16 : bundle_ip;
            uint64_t cfm = st->cfm;
            unsigned delta = st->delta, sof = cfm & 127, sol = (cfm >> 7) & 127;
            if (cls == T1_IC_ALLOC) {
                /* The new frame: sof, sol and sor from the immediate, the
                 * rotating bases kept (ia64/rse.c). At the head, with the
                 * bases zero, the frame does not depend on the entry's:
                 * the contract leaves sof, sol and sor open. */
                uint64_t imm = (uint64_t)ti->insn.operands.system.immediate;
                unsigned nsof = imm & 127;
                if (delta + nsof > IA64_N_STACKED_PHYS) {
                    why = "alloc frame above the stacked registers";
                    t1_form_stop_note(ti->insn.opcode, !f->ninsns);
                    f->end_class = T1_END_ALLOC;
                    stop = true;
                    break;
                }
                if (!f->ninsns && !(cfm & ~T1_CFM_FRAME)) {
                    f->c.cfm_mask = ~T1_CFM_FRAME;
                    f->c.cfm = rq->cfm & f->c.cfm_mask;
                    first_fs = 1;
                }
                int next = frame_state(f, (cfm & ~T1_CFM_FRAME) | (imm & T1_CFM_FRAME), delta,
                                       st->call, first_fs);
                if (next < 0) {
                    why = "frame state limit";
                    stop = true;
                    break;
                }
                ti->fs_taken = (uint8_t)next;
                cur = (unsigned)next;
            }
            if (cls == T1_IC_CALL) {
                /* SDM Vol.3 br.call (ia64/rse.c ia64_rse_call): the callee
                 * frame is the caller's outputs, sol registers above. */
                if (ncalls == UINT8_MAX) {
                    why = "frame state limit";
                    stop = true;
                    break;
                }
                int next = frame_state(f, sof - sol, delta + sol, ++ncalls, first_fs);
                if (next < 0) {
                    why = "frame state limit";
                    stop = true;
                    break;
                }
                ti->fs_taken = (uint8_t)next;
                f->calls = true;
                if (ti->insn.qp)
                    t1_form_call_note(T1_CALL_OTHER);
            }
            if (cls == T1_IC_RET) {
                if (depth) {
                    ti->fs_taken = calls[depth - 1].fs;
                    ti->target = calls[depth - 1].ret_ip;
                } else {
                    int next = frame_state(f, rq->return_cfm, delta, st->call, first_fs);
                    if (next < 0) {
                        why = "frame state limit";
                        stop = true;
                        break;
                    }
                    ti->fs_taken = (uint8_t)next;
                    ti->target = rq->br_target[ti->insn.operands.branch.target & 7];
                    f->calls = true;
                }
            }
            /* Register names in the frame the instruction writes in: an
             * alloc's destination is in the new frame. */
            names->cfm = f->fs[cls == T1_IC_ALLOC ? ti->fs_taken : ti->fs].cfm;
            names->rse_bof = f->fs[cls == T1_IC_ALLOC ? ti->fs_taken : ti->fs].delta;
            resolve_registers(names, ti);
            ++f->ninsns;
            if (cls == T1_IC_BR || cls == T1_IC_CLOOP || cls == T1_IC_CALL || cls == T1_IC_CHK ||
                cls == T1_IC_CHKA) {
                int64_t disp = cls == T1_IC_CHK || cls == T1_IC_CHKA
                                   ? ti->insn.operands.memory.immediate
                                   : ti->insn.operands.branch.displacement;
                ti->target = bundle_ip + (uint64_t)disp;
                if (ti->insn.opcode == IA64_OP_BR_CALL_INDIRECT)
                    ti->target = rq->br_target[ti->insn.operands.branch.target & 7];
                if (cls != T1_IC_CALL && ti->target == f->c.ip && f->c.slot == 0 && !cur &&
                    f->c.cfm_mask == ~UINT64_C(0)) {
                    ti->edge = T1_EDGE_BACK;
                    if (!f->loop && max_insns - f->ninsns > T1_LOOP_EXTEND)
                        extend_limit = f->ninsns + T1_LOOP_EXTEND;
                    f->loop = true;
                    through_back = f->ninsns;
                }
            }
            /* A p0 IP-relative branch has no fall-through.
             * Follow it without decoding skipped bundles.
             * A target scanned before in the same frame state is not
             * scanned again: pass 2 makes the branch a back-edge (the
             * head, or an inner header that dominates it) or an exit. A
             * call under p0 is followed into its callee (a new state), a
             * matched return under p0 back to its caller; one not
             * followed ends the region at its taken path. */
            if (unconditional(ti)) {
                unsigned visited = 0, fs = taken_fs(ti);
                while (visited < f->ninsns &&
                       (f->insn[visited].insn.address != ti->target || f->insn[visited].fs != fs))
                    ++visited;
                /* The target's page: listed, or a callee page derived
                 * before; a call may derive its callee's page, which needs
                 * a code snapshot, as every page formation reads. */
                unsigned d = 0;
                while (d < nderived && derived[d].va != (ti->target & ~UINT64_C(4095)))
                    ++d;
                bool listed = page_listed(rq, ti->target) || d < nderived;
                gpa_t callee_pa = 0;
                bool derive = false;
                if (cls == T1_IC_CALL && !listed && nderived < T1_MAX_PAGES &&
                    derive_page(rq, ti->target, &callee_pa) && code_page_has_snapshot(callee_pa))
                    derive = listed = true;
                if (cls == T1_IC_BR && !listed && !depth &&
                    branch_tail(rq, names, max_insns, f, ti->target)) {
                    bundle_ip = ti->target + 16;
                    slot = 0;
                    why = "branch tail";
                    f->end_class = T1_END_LIMIT;
                    stop = true;
                    break;
                }
                bool follow = visited == f->ninsns && listed;
                if (cls == T1_IC_CALL) {
                    /* Recursion: the head or a callee the scan is in. */
                    bool recursive = ti->target == f->c.ip;
                    for (unsigned k = 0; k < depth; ++k)
                        recursive = recursive || calls[k].target == ti->target;
                    follow = follow && depth < T1_MAX_CALL_DEPTH && !recursive;
                    t1_form_call_note(follow    ? T1_CALL_FOLLOWED
                                      : !listed ? T1_CALL_PAGE
                                                : T1_CALL_OTHER);
                }
                if (follow) {
                    if (derive)
                        derived[nderived++] = (struct t1_compile_page){
                            .va = ti->target & ~UINT64_C(4095), .pa = callee_pa};
                    if (cls == T1_IC_CALL) {
                        calls[depth].ret_ip = bundle_ip + 16;
                        calls[depth].target = ti->target;
                        calls[depth].at = f->ninsns - 1;
                        calls[depth++].fs = (uint8_t)cur;
                    }
                    if (cls == T1_IC_RET && depth)
                        --depth;
                    cur = taken_fs(ti);
                    bundle_ip = ti->target;
                    slot = 0;
                    stitched = true;
                    break;
                }
                if (cls != T1_IC_BR && cls != T1_IC_CHKA) {
                    why = cls == T1_IC_CALL ? "call not followed" : "return not followed";
                    f->end_class = cls == T1_IC_CALL ? T1_END_CALL : T1_END_RET;
                    left = true;
                    stop = true;
                    break;
                }
            }
            slot += step - 1;
        }
        if (stop)
            break;
        if (stitched)
            continue;
        bundle_ip += 16;
        slot = 0;
    }
    if (!f->ninsns) {
        *reason = why ? why : "empty region";
        return T1_REJECTED;
    }
    /* The cuts: a loop's lookahead instructions go; without a loop, a
     * callee the scan did not return from goes and the region leaves at
     * its call. The pages only they reached go with them (pages are
     * numbered in scan order). */
    if (f->loop || depth) {
        f->ninsns = f->loop ? through_back : calls[0].at + 1;
        f->end_class = f->loop ? T1_END_LIMIT : T1_END_CALL;
        why = f->loop ? "back-edge" : "callee did not return";
        left = !f->loop;
        unsigned pages = 0;
        for (unsigned i = 0; i < f->ninsns; ++i)
            if (f->insn[i].page + 1u > pages)
                pages = f->insn[i].page + 1u;
        f->npages = pages;
    }
    /* The end: where the scan stopped (after a followed transfer, at its
     * target), the taken path of a call or return it did not follow, or
     * the fall-through of the loop's back-edge. */
    const struct t1_insn *last = &f->insn[f->ninsns - 1];
    if (f->loop) {
        f->end_ip = last->next_ip;
        f->end_slot = last->next_slot;
        f->end_fs = last->fs;
    } else if (left) {
        f->end_ip = last->target;
        f->end_slot = 0;
        f->end_fs = last->fs_taken;
    } else {
        f->end_ip = bundle_ip;
        f->end_slot = (uint8_t)slot;
        f->end_fs = (uint8_t)cur;
    }
    f->end_reason = why;
    f->calls = false;
    for (unsigned i = 0; i < f->ninsns; ++i)
        if (f->insn[i].cls == T1_IC_CALL || f->insn[i].cls == T1_IC_RET)
            f->calls = true;

    include_side_path(rq, names, max_insns, f);
    classify_edges(f);
    /* A later read may have evicted and re-snapshotted an earlier page. */
    for (unsigned p = 0; p < f->npages; ++p)
        if (!code_page_has_snapshot(f->page[p].pa) ||
            code_source_generation(f->page[p].pa) != f->page[p].generation) {
            *reason = "page changed";
            return T1_RETRY;
        }
    return T1_COMPILED;
}
