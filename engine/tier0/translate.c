/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/tier0/internal.h"
#include "ia64/mmu.h"
#include "ia64vmm/host_isa.h"
#include "ia64vmm/hal.h"

#define S(member) ((int32_t)offsetof(struct vcpu, s.member))
#define V(member) ((int32_t)offsetof(struct vcpu, member))
#define H(member) ((int32_t)offsetof(struct tier0_homes, member))
#define E(member) ((int32_t)offsetof(struct tier0_execution, member))

enum relation {
    REL_EQ,
    REL_NE,
    REL_LT,
    REL_LE,
    REL_GT,
    REL_GE,
    REL_LTU,
    REL_LEU,
    REL_GTU,
    REL_GEU
};
struct compare_desc {
    bool valid, word, immediate;
    enum relation relation;
    Ia64PredicateUpdate update;
};
static const struct compare_desc compares[IA64_OP_COUNT] = {
#include "ia64/decode/compare.inc"
};
static const unsigned relation_cc[] = {X_E, X_NE, X_L, X_LE, X_G, X_GE, X_B, X_BE, X_A, X_AE};
/* Reversing CMP operands reverses each ordered relation. */
static const unsigned reversed_relation_cc[] = {X_E,  X_NE, X_G,  X_GE, X_L,
                                                X_LE, X_A,  X_AE, X_B,  X_BE};

static void home_index(struct x86_buffer *b, struct tier0_region *r, unsigned reg)
{
    r->gr_used[(reg - 32) / 64] |= UINT64_C(1) << ((reg - 32) % 64);
    x86_load(b, X_R11, X_R15, H(gr_index) + 4 * (reg - 32), 4);
}
static void read_gr(struct x86_buffer *b, struct tier0_region *r, unsigned dst, unsigned reg)
{
    if (!reg)
        x86_mov_imm(b, dst, 0);
    else if (reg < 32)
        x86_load(b, dst, X_R12, S(gr) + 8 * reg, 8);
    else {
        home_index(b, r, reg);
        x86_load_index(b, dst, X_R12, X_R11, 3, S(gr), 8);
    }
}
static void read_nat(struct x86_buffer *b, struct tier0_region *r, unsigned dst, unsigned reg)
{
    if (!reg)
        x86_mov_imm(b, dst, 0);
    else if (reg < 32)
        x86_load(b, dst, X_R12, S(nat) + reg, 1);
    else {
        home_index(b, r, reg);
        x86_load_index(b, dst, X_R12, X_R11, 0, S(nat), 1);
    }
}
/* A GR home is ordinary executor-local memory. Read only the low bytes
 * needed by sxt/zxt and extend in the load (Intel SDM Vol.2 MOVSX/MOVZX),
 * using the same stacked home for the value and its independent NaT. */
static void read_gr_extended(struct x86_buffer *b, struct tier0_region *r, unsigned reg,
                             unsigned size, bool sign, unsigned nat)
{
    if (!reg) {
        x86_mov_imm(b, X_RAX, 0);
        x86_alu(b, 0x31, nat, nat, true);
    } else if (reg < 32) {
        if (sign)
            x86_load_signed(b, X_RAX, X_R12, S(gr) + 8 * reg, size);
        else
            x86_load(b, X_RAX, X_R12, S(gr) + 8 * reg, size);
        read_nat(b, r, nat, reg);
    } else {
        home_index(b, r, reg);
        if (sign)
            x86_load_index_signed(b, X_RAX, X_R12, X_R11, 3, S(gr), size);
        else
            x86_load_index(b, X_RAX, X_R12, X_R11, 3, S(gr), size);
        x86_load_index(b, nat, X_R12, X_R11, 0, S(nat), 1);
    }
}
/* Integer emission has no helper between its operand reads and write.
 * Keep NaTs in R8/R9 while RAX/RCX/RDX calculate the value, so a stacked
 * operand resolves its home once for both the value and its NaT. */
static void read_integer_operand(struct x86_buffer *b, struct tier0_region *r, unsigned value,
                                 unsigned nat, unsigned reg)
{
    if (!reg) {
        x86_mov_imm(b, value, 0);
        x86_alu(b, 0x31, nat, nat, true);
    } else if (reg < 32) {
        read_gr(b, r, value, reg);
        read_nat(b, r, nat, reg);
    } else {
        home_index(b, r, reg);
        x86_load_index(b, value, X_R12, X_R11, 3, S(gr), 8);
        x86_load_index(b, nat, X_R12, X_R11, 0, S(nat), 1);
    }
}
static void write_gr_at_home(struct x86_buffer *b, unsigned value, unsigned nat)
{
    x86_store_index(b, X_R12, X_R11, 3, S(gr), value, 8);
    x86_store_index(b, X_R12, X_R11, 0, S(nat), nat, 1);
}
/* A system transfer must reject NaT before its architectural write. A
 * stacked value can be captured with that NaT from one executor-local
 * home; the exact fault helper still runs before any visible effect. */
static void read_system_operand(struct x86_buffer *b, struct tier0_region *r, unsigned reg,
                                size_t *fallback, unsigned *nfallback)
{
    unsigned nat = reg >= 32 ? X_RDX : X_RAX;
    if (reg >= 32)
        read_integer_operand(b, r, X_RAX, nat, reg);
    else
        read_nat(b, r, nat, reg);
    x86_alu(b, 0x85, nat, nat, false);
    fallback[(*nfallback)++] = x86_jcc(b, X_NE);
    if (reg < 32)
        read_gr(b, r, X_RAX, reg);
}
static void write_gr(struct x86_buffer *b, struct tier0_region *r, unsigned reg, unsigned value,
                     unsigned nat)
{
    if (reg < 32) {
        x86_store(b, X_R12, S(gr) + 8 * reg, value, 8);
        x86_store(b, X_R12, S(nat) + reg, nat, 1);
    } else {
        home_index(b, r, reg);
        write_gr_at_home(b, value, nat);
    }
}
static void predicate_mask(struct x86_buffer *b, struct tier0_region *r, unsigned p)
{
    r->pr_used |= UINT64_C(1) << p;
    x86_load(b, X_R11, X_R15, H(pr_mask) + 8 * (p - 16), 8);
}
static void predicate_zero(struct x86_buffer *b, struct tier0_region *r, unsigned p)
{
    if (!p)
        return;
    if (p < 16)
        x86_bt(b, 6, X_RAX, p);
    else {
        predicate_mask(b, r, p);
        x86_unary(b, 2, X_R11);
        x86_alu(b, 0x21, X_RAX, X_R11, false);
    }
}
static void predicate_set(struct x86_buffer *b, struct tier0_region *r, unsigned p, unsigned value)
{
    if (!p)
        return;
    if (p < 16) {
        x86_shift(b, 4, value, p, false);
        x86_alu(b, 0x09, X_RAX, value, false);
    } else {
        x86_alu(b, 0x85, value, value, false);
        size_t skip = x86_jcc(b, X_E);
        predicate_mask(b, r, p);
        x86_alu(b, 0x09, X_RAX, X_R11, false);
        x86_patch(b, skip, b->size);
    }
}
static void predicate_clear(struct x86_buffer *b, struct tier0_region *r, unsigned p1, unsigned p2)
{
    x86_load(b, X_RAX, X_R12, S(pr), 8);
    predicate_zero(b, r, p1);
    predicate_zero(b, r, p2);
    x86_bt(b, 5, X_RAX, 0);
    x86_store(b, X_R12, S(pr), X_RAX, 8);
}
static bool predicate_op(Ia64Opcode op)
{
    return compares[op].valid || op == IA64_OP_TBIT_Z || op == IA64_OP_TBIT_NZ ||
           op == IA64_OP_TNAT_Z || op == IA64_OP_TNAT_NZ;
}
static Ia64PredicateUpdate predicate_update(const Ia64Instruction *i)
{
    return compares[i->opcode].valid ? compares[i->opcode].update : i->pred_update;
}
/* I-unit moves to and from ar.lc and ar.pfs (SDM Vol.3 mov ar). */
static bool ar_move(const Ia64Instruction *i)
{
    return (i->opcode == IA64_OP_MOV_GRAR || i->opcode == IA64_OP_MOV_IMMAR ||
            i->opcode == IA64_OP_MOV_ARGR) &&
           i->unit == IA64_UNIT_I &&
           (i->operands.system.source == AR_LC || i->operands.system.source == AR_PFS);
}
/* SDM Vol.1 §3.1.8.12: writes of these ar.pfs fields fault. */
#define PFS_RESERVED ((UINT64_C(15) << 58) | (UINT64_C(0x3fff) << 38))
static bool fast_opcode(Ia64Opcode op)
{
    if (predicate_op(op))
        return true;
    switch (op) {
    case IA64_OP_NOP:
    case IA64_OP_ADD:
    case IA64_OP_ADD_ONE:
    case IA64_OP_ADDS:
    case IA64_OP_ADDL:
    case IA64_OP_SUB:
    case IA64_OP_SUB_ONE:
    case IA64_OP_SUB_IMM:
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
    case IA64_OP_SHR_IMM:
    case IA64_OP_SHRU_IMM:
    case IA64_OP_DEPZ:
    case IA64_OP_DEPZ_IMM:
    case IA64_OP_EXTR:
    case IA64_OP_EXTRU:
    case IA64_OP_ZXT1:
    case IA64_OP_ZXT2:
    case IA64_OP_ZXT4:
    case IA64_OP_SXT1:
    case IA64_OP_SXT2:
    case IA64_OP_SXT4:
    case IA64_OP_MOVL:
    case IA64_OP_BR_COND:
    case IA64_OP_BR_CLOOP:
    case IA64_OP_MOV_IP:
    case IA64_OP_MOV_CURRENT_IP:
    case IA64_OP_ADDP4:
    case IA64_OP_ADDP4_IMM:
    case IA64_OP_SHLADDP4:
    case IA64_OP_MOV_GRAR:
    case IA64_OP_MOV_IMMAR:
    case IA64_OP_MOV_ARGR:
    case IA64_OP_MOV_GRBR:
    case IA64_OP_MOV_BRGR:
    case IA64_OP_HINT_M:
    case IA64_OP_HINT_I:
    case IA64_OP_HINT_F:
    case IA64_OP_HINT_X:
    case IA64_OP_LD1:
    case IA64_OP_LD2:
    case IA64_OP_LD4:
    case IA64_OP_LD8:
    case IA64_OP_LD1S:
    case IA64_OP_LD2S:
    case IA64_OP_LD4S:
    case IA64_OP_LD8S:
    case IA64_OP_ST1:
    case IA64_OP_ST2:
    case IA64_OP_ST4:
    case IA64_OP_ST8:
    case IA64_OP_ST1REL:
    case IA64_OP_ST2REL:
    case IA64_OP_ST4REL:
    case IA64_OP_ST8REL:
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
    case IA64_OP_LFETCH:
    case IA64_OP_LFETCH_FAULT:
    case IA64_OP_DEP:
    case IA64_OP_DEP_IMM:
    case IA64_OP_SHRP_IMM:
    case IA64_OP_MOV_PRGR:
    case IA64_OP_MOV_GRPR:
    case IA64_OP_MOV_PR_ROT_IMM:
    case IA64_OP_CHK_S:
    case IA64_OP_MF:
    case IA64_OP_MF_A:
    case IA64_OP_BR_CALL:
    case IA64_OP_BR_CALL_INDIRECT:
    case IA64_OP_BR_RET:
    case IA64_OP_ALLOC:
        return true;
    default:
        return false;
    }
}
/* Native forms that set IP themselves. */
static bool native_branch(Ia64Opcode op)
{
    return op == IA64_OP_BR_COND || op == IA64_OP_BR_CLOOP || op == IA64_OP_BR_CALL ||
           op == IA64_OP_BR_CALL_INDIRECT || op == IA64_OP_BR_RET;
}
/* cfm: the frame of i, which an alloc earlier in the region may have set. */
static bool eligible(const Ia64Instruction *i, const struct ia64_state *s, uint64_t cfm)
{
    const struct ia64_opcode_info *m = ia64_opcode_info(i->opcode);
    if (!i->valid || !fast_opcode(i->opcode) || i->encoding_class != IA64_ENCODING_DEFINED ||
        i->placement_illegal || i->reserved_field || i->clear_p2_before_predicate)
        return false;
    if (m->writes_gr && i->opcode != IA64_OP_ALLOC &&
        (!i->operands.common.destination || (i->operands.common.destination >= 32 &&
                                             i->operands.common.destination - 32u >= (cfm & 127))))
        return false;
    /* SDM Vol.3 alloc: a qualifying predicate, a frame shape beyond the
     * physical registers and r1 outside the new frame fault (Illegal
     * Operation) before any effect. */
    if (i->opcode == IA64_OP_ALLOC) {
        uint64_t frame = (uint64_t)i->operands.system.immediate;
        unsigned sof = frame & 127, sol = (frame >> 7) & 127, sor = (frame >> 14) & 15;
        unsigned dest = i->operands.system.destination;
        if (i->qp || sof > IA64_N_STACKED_PHYS || sol > sof || sor * 8 > sof || !dest ||
            (dest >= 32 && dest - 32u >= sof))
            return false;
    }
    /* A call preserves sol of sof registers: frames no alloc makes keep the helper. */
    if ((i->opcode == IA64_OP_BR_CALL || i->opcode == IA64_OP_BR_CALL_INDIRECT) &&
        ((cfm & 127) > IA64_N_STACKED_PHYS || ((cfm >> 7) & 127) > (cfm & 127)))
        return false;
    if (predicate_op(i->opcode) && i->operands.integer.predicate1 == i->operands.integer.predicate2)
        return false;
    /* SDM Vol.3 ld.s, Vol.2 §4.4.6 Table 4-12: physical addressing and
     * nonspeculative memory always defer. A virtual write-back page the data
     * translation cache holds is accessed as by ld (NaT clear); every other
     * case, including a NaT base, takes the exact helper. */
    if (((i->opcode >= IA64_OP_LD1S && i->opcode <= IA64_OP_LD8S) ||
         (i->opcode >= IA64_OP_LD1SA && i->opcode <= IA64_OP_LD8SA)) &&
        !(s->psr & IA64_PSR_DT))
        return false;
    /* SDM Vol.3 lfetch: the faulting form translates like a load; only a
     * virtual page the data translation cache holds is native. */
    if (i->opcode == IA64_OP_LFETCH_FAULT && !(s->psr & IA64_PSR_DT))
        return false;
    /* chk.s with an FR source reads NaTVal behind PSR.dfl/dfh: helper. */
    if (i->opcode == IA64_OP_CHK_S && i->check_fp)
        return false;
    /* SDM Vol.3 br.cloop: slot 2 only; the qualifying predicate is not read. */
    if (i->opcode == IA64_OP_BR_CLOOP && (i->slot != 2 || i->qp))
        return false;
    if ((i->opcode == IA64_OP_MOV_GRAR || i->opcode == IA64_OP_MOV_IMMAR ||
         i->opcode == IA64_OP_MOV_ARGR) &&
        !ar_move(i))
        return false;
    if (i->opcode == IA64_OP_MOV_IMMAR && i->operands.system.source == AR_PFS &&
        ((uint64_t)i->operands.system.immediate & PFS_RESERVED))
        return false;
    /* Hints with a base update keep the exact helper. */
    if ((i->opcode == IA64_OP_HINT_M || i->opcode == IA64_OP_HINT_I ||
         i->opcode == IA64_OP_HINT_F || i->opcode == IA64_OP_HINT_X) &&
        (i->hint_m_reg_increment || i->imm_base_update))
        return false;
    if (i->reg_base_update || i->imm_base_update) {
        const IA64MemoryOperands *op = &i->operands.memory;
        /* SDM Vol.3 ld/st: illegal update destinations precede NaT and
         * memory faults. The helper also handles a false qualifier exactly. */
        if (m->family != IA64_FAMILY_MEMORY || !op->base ||
            (op->base >= 32 && op->base - 32u >= (cfm & 127)) ||
            (!ia64_opcode_is_store(i->opcode) && op->base == op->destination))
            return false;
    }
    return true;
}
static void arguments(struct x86_buffer *b, const Ia64Instruction *i)
{
    /* Generated code names the arena copy of the instruction; host
     * pointers have one width, so a region's size never depends on where
     * its operands live. */
    x86_mov(b, X_RDI, X_R12);
    x86_movabs(b, X_RSI, (uintptr_t)i);
    x86_mov(b, X_RDX, X_R14);
}
static void count_inline(struct x86_buffer *b, Ia64Opcode opcode)
{
    if (!IA64VMM_TIER0_STATS)
        return;
    x86_load(b, X_R11, X_R14, offsetof(struct tier0_execution, stats), 8);
    x86_add_memory(b, X_R11, offsetof(struct tier0_stats, inlined) + opcode * sizeof(uint64_t), 1);
}
static void next_ip(struct x86_buffer *b, const Ia64Instruction *i, unsigned slots)
{
    unsigned slot = i->slot + slots;
    uint64_t ip = i->address;
    if (slot == 3) {
        slot = 0;
        ip += 16;
    }
    x86_mov_imm(b, X_RAX, ip);
    x86_store(b, X_R12, S(ip), X_RAX, 8);
    x86_mov_imm(b, X_RAX, slot);
    x86_store(b, X_R12, S(slot), X_RAX, 4);
}
/* RCX = the condition of a compare, tbit or tnat: 0 or 1. When a stacked
 * operand is used, capture each value with its NaT to resolve its home once.
 * Reading executor-local GR storage is safe even when its NaT is set; the
 * exact helper still handles every NaT compare/tbit before predicates change. */
/* The caller only retains a producer when this unqualified predicate
 * consumes it. Keep this additional emission path out of the ordinary
 * condition/region loop so non-forwarded regions keep their small path. */
static __attribute__((noinline)) void
condition_forwarded(struct x86_buffer *b, struct tier0_region *r, const Ia64Instruction *i,
                    size_t *fallback, unsigned *nfallback, unsigned forwarded)
{
    const IA64IntegerOperands *op = &i->operands.integer;
    const struct compare_desc *d = &compares[i->opcode];
    bool tnat = i->opcode == IA64_OP_TNAT_Z || i->opcode == IA64_OP_TNAT_NZ;
    if (tnat) {
        x86_mov_imm(b, X_RCX, 0);
        x86_alu(b, 0x85, X_R8, X_R8, false);
        x86_setcc(b, i->opcode == IA64_OP_TNAT_NZ ? X_NE : X_E, X_RCX);
        return;
    }
    if (d->valid) {
        bool register_compare = !d->immediate;
        bool same = register_compare && op->source1 == op->source2;
        bool immediate =
            d->immediate && (d->word || op->immediate == (int64_t)(int32_t)op->immediate);
        if (same || immediate) {
            /* The retained value and NaT suffice for either folded relation.
             * Keep the exact NaT exit before changing predicate state. */
            x86_alu(b, 0x85, X_R8, X_R8, false);
            fallback[(*nfallback)++] = x86_jcc(b, X_NE);
            if (same) {
                bool equal = d->relation == REL_EQ || d->relation == REL_LE ||
                             d->relation == REL_GE || d->relation == REL_LEU ||
                             d->relation == REL_GEU;
                x86_mov_imm(b, X_RCX, equal);
            } else {
                x86_mov_imm(b, X_RCX, 0);
                x86_cmp_imm(b, X_RAX, (int32_t)op->immediate, d->word);
                x86_setcc(b, reversed_relation_cc[d->relation], X_RCX);
            }
            return;
        }
        bool first = register_compare && op->source1 == forwarded;
        bool second = op->source2 == forwarded;
        /* Save the retained second input before a different first input
         * overwrites RAX/R8. Equal inputs share the same retained NaT. */
        if (second) {
            x86_mov(b, X_RDX, X_RAX);
            if (register_compare && !first)
                x86_mov(b, X_R9, X_R8);
        }
        if (d->immediate)
            x86_mov_imm(b, X_RAX, (uint64_t)op->immediate);
        else if (!first)
            read_integer_operand(b, r, X_RAX, X_R8, op->source1);
        if (!second)
            read_integer_operand(b, r, X_RDX, X_R9, op->source2);
        if (register_compare && !(first && second))
            x86_alu(b, 0x09, X_R8, X_R9, false);
    }
    /* TBIT has only the retained second input; no value/NaT read is needed.
     * Both TBIT and compare still take the exact NaT helper before writes. */
    x86_alu(b, 0x85, X_R8, X_R8, false);
    fallback[(*nfallback)++] = x86_jcc(b, X_NE);
    x86_mov_imm(b, X_RCX, 0);
    if (!d->valid) {
        x86_bt(b, 4, X_RAX, (unsigned)op->immediate & 63);
        x86_setcc(b, i->opcode == IA64_OP_TBIT_NZ ? X_B : X_AE, X_RCX);
    } else {
        x86_alu(b, 0x39, X_RAX, X_RDX, d->word);
        x86_setcc(b, relation_cc[d->relation], X_RCX);
    }
}
static void condition(struct x86_buffer *b, struct tier0_region *r, const Ia64Instruction *i,
                      size_t *fallback, unsigned *nfallback, unsigned forwarded)
{
    if (forwarded) {
        condition_forwarded(b, r, i, fallback, nfallback, forwarded);
        return;
    }
    const IA64IntegerOperands *op = &i->operands.integer;
    const struct compare_desc *d = &compares[i->opcode];
    bool tnat = i->opcode == IA64_OP_TNAT_Z || i->opcode == IA64_OP_TNAT_NZ;
    bool register_compare = d->valid && !d->immediate;
    if (register_compare && op->source1 == op->source2) {
        /* Equal architectural inputs give a constant relation, but NaT
         * still takes the exact predicate-update helper. GR0 has no NaT. */
        if (op->source2) {
            read_nat(b, r, X_RAX, op->source2);
            x86_alu(b, 0x85, X_RAX, X_RAX, false);
            fallback[(*nfallback)++] = x86_jcc(b, X_NE);
        }
        bool equal = d->relation == REL_EQ || d->relation == REL_LE || d->relation == REL_GE ||
                     d->relation == REL_LEU || d->relation == REL_GEU;
        x86_mov_imm(b, X_RCX, equal);
        return;
    }
    /* CMP imm32 sign-extends in the 64-bit form. CMP4 observes only the
     * low 32 bits, including diagnostic immediates wider than the decoder. */
    bool immediate_compare =
        d->valid && d->immediate && (d->word || op->immediate == (int64_t)(int32_t)op->immediate);
    bool captured = !tnat && (op->source2 >= 32 || (register_compare && op->source1 >= 32));
    if (!tnat) {
        if (captured) {
            if (!d->valid)
                read_integer_operand(b, r, X_RAX, X_R8, op->source2);
            else {
                if (d->immediate) {
                    if (!immediate_compare)
                        x86_mov_imm(b, X_RAX, (uint64_t)op->immediate);
                } else
                    read_integer_operand(b, r, X_RAX, X_R8, op->source1);
                read_integer_operand(b, r, X_RDX, d->immediate ? X_R8 : X_R9, op->source2);
                if (register_compare)
                    x86_alu(b, 0x09, X_R8, X_R9, false);
            }
            x86_alu(b, 0x85, X_R8, X_R8, false);
        } else {
            /* Static operands have no home lookup to share; preserve the
             * existing load order and register choices on that path. */
            read_nat(b, r, X_RAX, op->source2);
            if (register_compare) {
                read_nat(b, r, X_RDX, op->source1);
                x86_alu(b, 0x09, X_RAX, X_RDX, false);
            }
            x86_alu(b, 0x85, X_RAX, X_RAX, false);
        }
        fallback[(*nfallback)++] = x86_jcc(b, X_NE);
    }
    switch (i->opcode) {
    case IA64_OP_TBIT_Z:
    case IA64_OP_TBIT_NZ:
        if (!captured)
            read_gr(b, r, X_RAX, op->source2);
        x86_mov_imm(b, X_RCX, 0);
        x86_bt(b, 4, X_RAX, (unsigned)op->immediate & 63);
        x86_setcc(b, i->opcode == IA64_OP_TBIT_NZ ? X_B : X_AE, X_RCX);
        return;
    case IA64_OP_TNAT_Z:
    case IA64_OP_TNAT_NZ:
        read_nat(b, r, X_RAX, op->source2);
        x86_mov_imm(b, X_RCX, 0);
        x86_alu(b, 0x85, X_RAX, X_RAX, false);
        x86_setcc(b, i->opcode == IA64_OP_TNAT_NZ ? X_NE : X_E, X_RCX);
        return;
    default:
        break;
    }
    if (!captured) {
        if (d->immediate) {
            if (!immediate_compare)
                x86_mov_imm(b, X_RAX, (uint64_t)op->immediate);
        } else
            read_gr(b, r, X_RAX, op->source1);
        read_gr(b, r, X_RDX, op->source2);
    }
    x86_mov_imm(b, X_RCX, 0);
    if (immediate_compare) {
        x86_cmp_imm(b, X_RDX, (int32_t)op->immediate, d->word);
        x86_setcc(b, reversed_relation_cc[d->relation], X_RCX);
    } else {
        x86_alu(b, 0x39, X_RAX, X_RDX, d->word);
        x86_setcc(b, relation_cc[d->relation], X_RCX);
    }
}
static void predicate_one(struct x86_buffer *b, struct tier0_region *r, unsigned p)
{
    if (!p)
        return;
    if (p < 16)
        x86_bt(b, 5, X_RAX, p);
    else {
        predicate_mask(b, r, p);
        x86_alu(b, 0x09, X_RAX, X_R11, false);
    }
}
/* Writes the targets from the condition in RCX (SDM Vol.3 cmp, Table
 * "Comparison Types"): normal writes it and its complement; and clears
 * both when it is false, or sets both and or.andcm sets p1 and clears p2
 * when it is true. NaT sources took the exact helper. */
static void predicate_write(struct x86_buffer *b, struct tier0_region *r, const Ia64Instruction *i)
{
    const IA64IntegerOperands *op = &i->operands.integer;
    Ia64PredicateUpdate update = predicate_update(i);
    size_t skip = 0;
    if (update != IA64_PRED_UPDATE_NORMAL) {
        x86_alu(b, 0x85, X_RCX, X_RCX, false);
        skip = x86_jcc(b, update == IA64_PRED_UPDATE_AND ? X_NE : X_E);
    }
    x86_load(b, X_RAX, X_R12, S(pr), 8);
    switch (update) {
    case IA64_PRED_UPDATE_AND:
        predicate_zero(b, r, op->predicate1);
        predicate_zero(b, r, op->predicate2);
        break;
    case IA64_PRED_UPDATE_OR:
        predicate_one(b, r, op->predicate1);
        predicate_one(b, r, op->predicate2);
        break;
    case IA64_PRED_UPDATE_OR_ANDCM:
        predicate_one(b, r, op->predicate1);
        predicate_zero(b, r, op->predicate2);
        break;
    default:
        predicate_zero(b, r, op->predicate1);
        predicate_zero(b, r, op->predicate2);
        /* Preserve the two old predicate values until both conditions have
         * been computed, including when qp aliases a destination. */
        x86_mov(b, X_RDX, X_RCX);
        x86_imm(b, 6, X_RDX, 1);
        predicate_set(b, r, op->predicate1, X_RCX);
        predicate_set(b, r, op->predicate2, X_RDX);
        break;
    }
    x86_bt(b, 5, X_RAX, 0);
    x86_store(b, X_R12, S(pr), X_RAX, 8);
    if (skip)
        x86_patch(b, skip, b->size);
}
static void mask(struct x86_buffer *b, uint64_t m)
{
    x86_mov_imm(b, X_RDX, m);
    x86_alu(b, 0x21, X_RAX, X_RDX, false);
}
static bool immediate_alu(Ia64Opcode opcode)
{
    return opcode == IA64_OP_ADDS || opcode == IA64_OP_ADDL || opcode == IA64_OP_AND_IMM ||
           opcode == IA64_OP_OR_IMM || opcode == IA64_OP_XOR_IMM;
}
/* Group 1's imm32 sign-extends to 64 bits. The decoder's immediate forms
 * fit, but diagnostic instructions can supply any 64-bit value. */
static void alu_immediate(struct x86_buffer *b, unsigned group, uint64_t imm)
{
    /* These forms preserve the loaded value and NaT. Host arithmetic
     * flags are dead here; the instruction's stores and checkpoints stay. */
    if ((!imm && (group == 0 || group == 1 || group == 6)) || (group == 4 && imm == UINT64_MAX))
        return;
    if (imm == (uint64_t)(int64_t)(int32_t)imm)
        x86_imm(b, group, X_RAX, (int32_t)imm);
    else {
        x86_mov_imm(b, X_RDX, imm);
        x86_alu(b, 0x01 | (group << 3), X_RAX, X_RDX, false);
    }
}
static bool empty_hint(Ia64Opcode opcode)
{
    return opcode == IA64_OP_NOP || opcode == IA64_OP_HINT_M || opcode == IA64_OP_HINT_I ||
           opcode == IA64_OP_HINT_F || opcode == IA64_OP_HINT_X;
}
static bool forwards_hint(const Ia64Instruction *i)
{
    /* Qualifier tests use RAX. Updating hints use the exact helper, which
     * may change the retained register and clobber all caller-saved GPRs. */
    return empty_hint(i->opcode) && !i->qp && !i->hint_m_reg_increment && !i->reg_base_update &&
           !i->imm_base_update;
}
/* An unqualified integer or predicate can consume RAX/R8 from the previous integer,
 * including across empty hints. Unforwarded extensions keep their memory form.
 * Eligibility is still checked by the main loop before any use. */
static bool reads_integer_result(const Ia64Instruction *i, unsigned reg)
{
    const struct ia64_opcode_info *m = ia64_opcode_info(i->opcode);
    if (i->qp || m->family != IA64_FAMILY_INTEGER)
        return false;
    /* Preserve the common GR consumer's existing decision path. Predicate
     * operations do not write a GR, so inspect that metadata only here. */
    if (!m->writes_gr) {
        if (!m->predicate_pair)
            return false;
        const struct compare_desc *d = &compares[i->opcode];
        return i->operands.integer.source2 == reg ||
               (d->valid && !d->immediate && i->operands.integer.source1 == reg);
    }
    enum ia64_nat_rule rule = m->nat_rule;
    return ((rule == IA64_NAT_SOURCE_R2 || rule == IA64_NAT_SOURCES_R2_R3) &&
            i->operands.integer.source1 == reg) ||
           ((rule == IA64_NAT_SOURCE_R3 || rule == IA64_NAT_SOURCES_R2_R3) &&
            i->operands.integer.source2 == reg);
}
static bool reads_after_hints(const struct tier0_region *r, unsigned at, unsigned reg)
{
    while (at < r->count && forwards_hint(&r->insns[at]))
        ++at;
    return at < r->count && reads_integer_result(&r->insns[at], reg);
}
/* The forwarded value is already in RAX. MOVSX/MOVZX/MOVSXD or a
 * 32-bit self move extend it without reloading its committed GR home. */
static void extend_forwarded(struct x86_buffer *b, unsigned size, bool sign)
{
    if (sign)
        x86_u8(b, 0x48);
    if (size == 4)
        x86_u8(b, sign ? 0x63 : 0x89);
    else {
        x86_u8(b, 0x0f);
        x86_u8(b, (sign ? 0xbe : 0xb6) + (size == 2));
    }
    x86_u8(b, 0xc0);
}
static void integer(struct x86_buffer *b, struct tier0_region *r, const Ia64Instruction *i,
                    unsigned forwarded, bool retain)
{
    const IA64IntegerOperands *op = &i->operands.integer;
    uint64_t imm = (uint64_t)op->immediate;
    bool same_identity =
        (i->opcode == IA64_OP_AND || i->opcode == IA64_OP_OR) && op->source1 == op->source2;
    bool identity = same_identity ||
                    ((i->opcode == IA64_OP_ADDS || i->opcode == IA64_OP_ADDL ||
                      i->opcode == IA64_OP_OR_IMM || i->opcode == IA64_OP_XOR_IMM) &&
                     !imm) ||
                    (i->opcode == IA64_OP_AND_IMM && imm == UINT64_MAX) ||
                    ((i->opcode == IA64_OP_SHL_IMM || i->opcode == IA64_OP_SHR_IMM ||
                      i->opcode == IA64_OP_SHRU_IMM) &&
                     !(imm & 63));
    if (identity && op->destination == op->source2) {
        /* The architectural value and NaT already equal the result.
         * Capture them only when the next integer needs a retained pair;
         * the normal predicate, budget and postamble remain outside. */
        if (retain && forwarded != op->destination)
            read_integer_operand(b, r, X_RAX, X_R8, op->source2);
        return;
    }
    if (same_identity) {
        if (!forwarded || forwarded != op->source2)
            read_integer_operand(b, r, X_RAX, X_R8, op->source2);
        write_gr(b, r, op->destination, X_RAX, X_R8);
        return;
    }
    enum ia64_nat_rule rule = ia64_opcode_info(i->opcode)->nat_rule;
    bool r2 = rule == IA64_NAT_SOURCE_R2 || rule == IA64_NAT_SOURCES_R2_R3;
    bool r3 = rule == IA64_NAT_SOURCE_R3 || rule == IA64_NAT_SOURCES_R2_R3;
    bool same_source = r2 && r3 && op->source1 == op->source2;
    bool extend = i->opcode >= IA64_OP_SXT1 && i->opcode <= IA64_OP_ZXT4;
    bool extract = i->opcode == IA64_OP_EXTR || i->opcode == IA64_OP_EXTRU;
    bool immediate_shift = i->opcode == IA64_OP_SHL_IMM || i->opcode == IA64_OP_SHR_IMM ||
                           i->opcode == IA64_OP_SHRU_IMM;
    bool zero_shift = immediate_shift && !(imm & 63);
    bool first = forwarded && r2 && op->source1 == forwarded;
    bool second = forwarded && r3 && op->source2 == forwarded;
    unsigned source2 =
        immediate_alu(i->opcode) || extract || extend || immediate_shift ? X_RAX : X_RDX;
    /* A retained stacked destination leaves its home in R11. Empty hints
     * and budget checks preserve it; statistics-enabled code reloads because
     * count_inline uses R11. A different stacked input replaces the home. */
    unsigned home = !IA64VMM_TIER0_STATS && forwarded >= 32 ? forwarded : 0;
    if (same_source && (i->opcode == IA64_OP_SUB || i->opcode == IA64_OP_SUB_ONE ||
                        i->opcode == IA64_OP_XOR || i->opcode == IA64_OP_ANDCM)) {
        /* Equal values produce a constant, but their NaT still propagates.
         * Only read the NaT; keep a loaded stacked home for an aliasing
         * store. A forwarded input supplies its NaT in R8. */
        if (!first) {
            read_nat(b, r, X_R8, op->source1);
            if (op->source1 >= 32)
                home = op->source1;
        }
        if (i->opcode == IA64_OP_SUB_ONE)
            x86_mov_imm(b, X_RAX, UINT64_MAX);
        else
            x86_alu(b, 0x31, X_RAX, X_RAX, true);
        if (home && op->destination == home)
            write_gr_at_home(b, X_RAX, X_R8);
        else
            write_gr(b, r, op->destination, X_RAX, X_R8);
        return;
    }
    /* Save a forwarded second input before a different first input can
     * overwrite RAX/R8. The ordinary operand reads use only R11 scratch. */
    if (second) {
        if (source2 != X_RAX)
            x86_mov(b, source2, X_RAX);
        if (r2 && !same_source)
            x86_mov(b, X_R9, X_R8);
    }
    if (r2 && !first) {
        read_integer_operand(b, r, X_RAX, X_R8, op->source1);
        if (op->source1 >= 32)
            home = op->source1;
    }
    if (r3 && !extend && !second) {
        /* Both operands see the same executor-local GR and NaT; no write
         * or helper intervenes. Give arithmetic its second value register
         * without resolving and reading the same home again. */
        if (same_source)
            x86_mov(b, X_RDX, X_RAX);
        else {
            read_integer_operand(b, r, source2, r2 ? X_R9 : X_R8, op->source2);
            if (op->source2 >= 32)
                home = op->source2;
        }
    }
    switch (i->opcode) {
    case IA64_OP_MOVL:
        x86_mov_imm(b, X_RAX, imm);
        break;
    case IA64_OP_MOV_IP:
    case IA64_OP_MOV_CURRENT_IP:
        x86_mov_imm(b, X_RAX, i->address);
        break;
    case IA64_OP_ADDS:
    case IA64_OP_ADDL:
        alu_immediate(b, 0, imm);
        break;
    case IA64_OP_ADD:
    case IA64_OP_ADD_ONE:
        x86_alu(b, 0x01, X_RAX, X_RDX, false);
        if (i->opcode == IA64_OP_ADD_ONE)
            x86_imm(b, 0, X_RAX, 1);
        break;
    case IA64_OP_SUB_IMM:
        x86_mov_imm(b, X_RAX, imm); /* fall through */
    case IA64_OP_SUB:
    case IA64_OP_SUB_ONE:
        x86_alu(b, 0x29, X_RAX, X_RDX, false);
        if (i->opcode == IA64_OP_SUB_ONE)
            x86_imm(b, 5, X_RAX, 1);
        break;
    case IA64_OP_AND_IMM:
        alu_immediate(b, 4, imm);
        break;
    case IA64_OP_OR_IMM:
        alu_immediate(b, 1, imm);
        break;
    case IA64_OP_XOR_IMM:
        alu_immediate(b, 6, imm);
        break;
    case IA64_OP_ANDCM_IMM:
        x86_mov_imm(b, X_RAX, imm); /* fall through */
    case IA64_OP_AND:
    case IA64_OP_ANDCM:
    case IA64_OP_OR:
    case IA64_OP_XOR:
        if ((i->opcode == IA64_OP_ANDCM || i->opcode == IA64_OP_ANDCM_IMM) &&
            (hal_host_isa() & HOST_ISA_BMI1)) {
            x86_andn(b, X_RAX, X_RDX, X_RAX);
            break;
        }
        if (i->opcode == IA64_OP_ANDCM || i->opcode == IA64_OP_ANDCM_IMM)
            x86_unary(b, 2, X_RDX);
        x86_alu(b,
                i->opcode == IA64_OP_OR || i->opcode == IA64_OP_OR_IMM     ? 0x09
                : i->opcode == IA64_OP_XOR || i->opcode == IA64_OP_XOR_IMM ? 0x31
                                                                           : 0x21,
                X_RAX, X_RDX, false);
        break;
    case IA64_OP_ADDP4_IMM:
    case IA64_OP_SHLADDP4:
    case IA64_OP_ADDP4:
        /* SDM Vol.3 addp4/shladdp4: 32-bit sum, r3 bits 31:30 into 62:61. */
        if (i->opcode == IA64_OP_ADDP4_IMM)
            x86_mov_imm(b, X_RAX, imm);
        if (i->opcode == IA64_OP_SHLADDP4)
            x86_shift(b, 4, X_RAX, imm & 63, false);
        x86_alu(b, 0x01, X_RAX, X_RDX, true);
        x86_mov(b, X_RCX, X_RDX);
        x86_shift(b, 5, X_RCX, 30, false);
        x86_imm(b, 4, X_RCX, 3);
        x86_shift(b, 4, X_RCX, 61, false);
        x86_alu(b, 0x09, X_RAX, X_RCX, false);
        break;
    case IA64_OP_SHLADD:
        /* Scale 16 is outside AMD64's SIB range (SDM Vol.3 shladd). */
        if (imm <= 3)
            x86_lea(b, X_RAX, X_RDX, X_RAX, imm);
        else {
            x86_shift(b, 4, X_RAX, imm, false);
            x86_alu(b, 0x01, X_RAX, X_RDX, false);
        }
        break;
    case IA64_OP_SHL:
    case IA64_OP_SHR:
    case IA64_OP_SHRU: {
        if (hal_host_isa() & HOST_ISA_BMI2) {
            /* IA-64 saturates the full count; BMI2 masks it. Keep that
             * guard, but no longer require RCX or destroy the source. */
            x86_imm(b, 7, X_RAX, 64);
            size_t small = x86_jcc(b, X_B);
            if (i->opcode == IA64_OP_SHR)
                x86_mov_imm(b, X_RAX, 63);
            else
                x86_mov_imm(b, X_RDX, 0);
            x86_patch(b, small, b->size);
            x86_shiftx(b,
                       i->opcode == IA64_OP_SHL   ? 4
                       : i->opcode == IA64_OP_SHR ? 7
                                                  : 5,
                       X_RAX, X_RDX, X_RAX);
            break;
        }
        x86_mov(b, X_RCX, X_RAX);
        x86_mov(b, X_RAX, X_RDX);
        x86_imm(b, 7, X_RCX, 64);
        size_t small = x86_jcc(b, X_B);
        if (i->opcode == IA64_OP_SHR)
            x86_mov_imm(b, X_RCX, 63);
        else
            x86_mov_imm(b, X_RAX, 0);
        x86_patch(b, small, b->size);
        x86_shift(b,
                  i->opcode == IA64_OP_SHL   ? 4
                  : i->opcode == IA64_OP_SHR ? 7
                                             : 5,
                  X_RAX, 0, true);
        break;
    }
    case IA64_OP_SHL_IMM:
    case IA64_OP_SHR_IMM:
    case IA64_OP_SHRU_IMM:
        /* A masked-zero count preserves the captured value and NaT. */
        if (!zero_shift)
            x86_shift(b,
                      i->opcode == IA64_OP_SHL_IMM   ? 4
                      : i->opcode == IA64_OP_SHR_IMM ? 7
                                                     : 5,
                      X_RAX, imm & 63, false);
        break;
    case IA64_OP_DEPZ:
    case IA64_OP_DEPZ_IMM: {
        if (i->opcode == IA64_OP_DEPZ_IMM)
            x86_mov_imm(b, X_RAX, (uint64_t)ia64_sign_extend(imm >> 13, 8));
        x86_shift(b, 4, X_RAX, imm & 63, false);
        mask(b, ia64_deposit_mask(imm & 63, (imm >> 6) & 127));
        break;
    }
    case IA64_OP_EXTR:
    case IA64_OP_EXTRU: {
        unsigned pos = imm & 63, len = ia64_bitfield_effective_len(pos, imm >> 6);
        bool sign = i->opcode == IA64_OP_EXTR;
        /* Put the field's high bit at bit 63, then shift the field down.
         * SAR supplies signed extraction's extension in the same step.
         * A field that already ends at bit 63 only needs the right shift;
         * the full register needs neither shift. */
        if (!len)
            x86_mov_imm(b, X_RAX, 0);
        else if (!sign && pos + len < 64 && (hal_host_isa() & HOST_ISA_BMI1)) {
            x86_mov_imm(b, X_RCX, pos | (len << 8));
            x86_bextr(b, X_RAX, X_RAX, X_RCX);
        } else {
            if (pos + len < 64)
                x86_shift(b, 4, X_RAX, 64 - pos - len, false);
            if (len < 64)
                x86_shift(b, sign ? 7 : 5, X_RAX, 64 - len, false);
        }
        break;
    }
    case IA64_OP_DEP:
    case IA64_OP_DEP_IMM: {
        /* SDM Vol.3 dep: the field of r3 takes r2 (or the sign-extended
         * 1-bit immediate) shifted to pos; the mask truncates at bit 63. */
        uint64_t field = ia64_deposit_mask(imm & 63, (imm >> 6) & 127);
        if (i->opcode == IA64_OP_DEP_IMM)
            x86_mov_imm(b, X_RAX, (imm >> 13) & 1 ? field : 0);
        else {
            x86_shift(b, 4, X_RAX, imm & 63, false);
            x86_mov_imm(b, X_RCX, field);
            x86_alu(b, 0x21, X_RAX, X_RCX, false);
        }
        x86_mov_imm(b, X_RCX, ~field);
        x86_alu(b, 0x21, X_RDX, X_RCX, false);
        x86_alu(b, 0x09, X_RAX, X_RDX, false);
        break;
    }
    case IA64_OP_SHRP_IMM:
        /* SDM Vol.3 shrp: the low 64 bits of r2:r3 >> count (SHRD); equal
         * sources rotate (RORX where the host has BMI2). */
        if (op->source1 == op->source2 && (hal_host_isa() & HOST_ISA_BMI2))
            x86_rorx(b, X_RAX, X_RDX, imm & 63);
        else {
            x86_shrd(b, X_RDX, X_RAX, imm & 63);
            x86_mov(b, X_RAX, X_RDX);
        }
        break;
    case IA64_OP_SXT1:
    case IA64_OP_SXT2:
    case IA64_OP_SXT4:
    case IA64_OP_ZXT1:
    case IA64_OP_ZXT2:
    case IA64_OP_ZXT4: {
        bool sign = i->opcode <= IA64_OP_SXT4;
        unsigned size = 1u << (i->opcode - (sign ? IA64_OP_SXT1 : IA64_OP_ZXT1));
        if (second)
            extend_forwarded(b, size, sign);
        else {
            read_gr_extended(b, r, op->source2, size, sign, r2 ? X_R9 : X_R8);
            if (op->source2 >= 32)
                home = op->source2;
        }
        break;
    }
    default:
        break;
    }
    /* Source NaTs were captured with their value loads, before a
     * destination that aliases either source is written. */
    if (r2) {
        if (r3 && !same_source)
            x86_alu(b, 0x09, X_R8, X_R9, false);
    } else if (!r3)
        x86_mov_imm(b, retain ? X_R8 : X_RDX, 0);
    /* Integer calculation leaves the last source or retained destination
     * home in R11, including extended loads. A static/GR0 input does not
     * replace it. Reuse that index for an aliasing stacked destination;
     * its home is already in gr_used. No helper or postamble occurs before
     * these stores. */
    unsigned nat = r2 || r3 || retain ? X_R8 : X_RDX;
    if (home && op->destination == home)
        write_gr_at_home(b, X_RAX, nat);
    else
        write_gr(b, r, op->destination, X_RAX, nat);
}
/* The store's page (RDI, a PA) holds no code: an inline
 * test of the code-page bitmap span the executor cached (a set bit takes the
 * exact helper); a page outside it, or any page while code validity is
 * inactive (whose check scans the decoded caches under code_lock), asks
 * tier0_store_tracked, which also caches the page's span. R10 survives. */
static void store_code_check(struct x86_buffer *b, size_t *fallback, unsigned *nfallback)
{
#define SPAN(member) ((int32_t)offsetof(struct tier0_code_span, member))
    x86_load(b, X_R11, X_R14, E(span), 8);
    x86_mov(b, X_RAX, X_RDI);
    x86_shift(b, 5, X_RAX, 12, false);
    x86_sub_load(b, X_RAX, X_R11, SPAN(first));
    x86_cmp_load(b, X_RAX, X_R11, SPAN(pages));
    size_t slow = x86_jcc(b, X_AE);
    x86_load(b, X_RDX, X_R11, SPAN(bits), 8);
    x86_mov(b, X_RCX, X_RAX);
    x86_shift(b, 5, X_RCX, 6, false);
    x86_load_index(b, X_RDX, X_RDX, X_RCX, 3, 0, 8);
    x86_bt_reg(b, X_RDX, X_RAX);
    fallback[(*nfallback)++] = x86_jcc(b, X_B);
    size_t done = x86_jmp(b);
    x86_patch(b, slow, b->size);
    x86_store(b, X_RSP, 0, X_R10, 8);
    x86_mov(b, X_RSI, X_R11);
    x86_call(b, (uintptr_t)tier0_store_tracked);
    x86_load(b, X_R10, X_RSP, 0, 8);
    x86_alu(b, 0x85, X_RAX, X_RAX, false);
    fallback[(*nfallback)++] = x86_jcc(b, X_NE);
    x86_patch(b, done, b->size);
#undef SPAN
}
/* A cached WB physical span is immutable while executors run (guest_mem.h).
 * Every failing guard goes to the exact helper, preserving NaT/PA/alignment
 * fault priority and MMIO width. Checking keeps its capture helper. */
static void physical_pointer(struct x86_buffer *b, struct tier0_region *r, const Ia64Instruction *i,
                             size_t *fallback, unsigned *nfallback)
{
    const IA64MemoryOperands *op = &i->operands.memory;
    bool store = ia64_opcode_is_store(i->opcode);
    unsigned size = tier0_access_size(i->opcode);
#define FAIL(cc) (fallback[(*nfallback)++] = x86_jcc(b, cc))
    /* Read a stacked base and its NaT through one resolved home. The
     * early value read is executor-local; no guest access precedes NaT. */
    if (op->base >= 32)
        read_integer_operand(b, r, X_R10, X_RAX, op->base);
    else
        read_nat(b, r, X_RAX, op->base);
    if (store) {
        read_nat(b, r, X_RDX, op->source);
        x86_alu(b, 0x09, X_RAX, X_RDX, false);
    }
    x86_alu(b, 0x85, X_RAX, X_RAX, false);
    FAIL(X_NE);
    if (op->base < 32)
        read_gr(b, r, X_R10, op->base);
    x86_mov(b, X_RAX, X_R10);
    x86_shift(b, 5, X_RAX, IA64_IMPL_PA_MSB + 1, false);
    x86_alu(b, 0x85, X_RAX, X_RAX, false);
    FAIL(X_NE);
    if (size > 1) {
        x86_mov(b, X_RAX, X_R10);
        x86_imm(b, 4, X_RAX, size - 1);
        FAIL(X_NE);
    }
    x86_load(b, X_RAX, X_R12, V(memory_hook), 8);
    x86_alu(b, 0x85, X_RAX, X_RAX, false);
    FAIL(X_NE);
    /* Any address of the cached region qualifies: the region table is
     * immutable while executors run. */
    x86_load(b, X_R9, X_R12, V(physical_region), 8);
    x86_alu(b, 0x85, X_R9, X_R9, false);
    FAIL(X_E);
    x86_load(b, X_RAX, X_R9, offsetof(struct gm_region, kind), 4);
    x86_imm(b, 7, X_RAX, GM_FIRMWARE);
    FAIL(X_A);
    /* offset = address - base, unsigned: an address below the base wraps
     * above any region size and fails the span check. */
    x86_load(b, X_RDX, X_R9, offsetof(struct gm_region, base), 8);
    x86_mov(b, X_RAX, X_R10);
    x86_alu(b, 0x29, X_RAX, X_RDX, false);
    x86_load(b, X_R8, X_R9, offsetof(struct gm_region, size), 8);
    x86_imm(b, 7, X_R8, size);
    FAIL(X_B);
    x86_imm(b, 5, X_R8, size);
    x86_alu(b, 0x39, X_RAX, X_R8, false);
    FAIL(X_A);
    /* The store's own page for the code check below. */
    if (store) {
        x86_mov(b, X_RDI, X_R10);
        x86_imm(b, 4, X_RDI, -4096);
    }
    x86_load(b, X_R10, X_R9, offsetof(struct gm_region, hva), 8);
    x86_alu(b, 0x01, X_R10, X_RAX, false);
    if (size > 1) {
        x86_mov(b, X_RAX, X_R10);
        x86_imm(b, 4, X_RAX, size - 1);
        FAIL(X_NE);
    }
    if (store)
        store_code_check(b, fallback, nfallback);
#undef FAIL
}
/* Virtual references (PSR.dt) through the executor's data translation
 * cache (internal.h): the page's entry must be this vCPU's, at its data-side MMU
 * sequence and the region's PSR class, with the access's right. A miss asks
 * tier0_data_pointer, which translates exactly and refills; NULL from it,
 * like every failed guard, runs the exact helper. NaT and alignment come
 * first, as for physical references; stores keep the code-page check. */
static void virtual_pointer(struct x86_buffer *b, struct tier0_region *r, const Ia64Instruction *i,
                            const struct ia64_state *s, size_t *fallback, unsigned *nfallback)
{
    const IA64MemoryOperands *op = &i->operands.memory;
    bool store = ia64_opcode_is_store(i->opcode);
    unsigned size = tier0_access_size(i->opcode);
#define FAIL(cc) (fallback[(*nfallback)++] = x86_jcc(b, cc))
#define DTC(member) ((int32_t)offsetof(struct tier0_dtc_entry, member))
    if (op->base >= 32)
        read_integer_operand(b, r, X_R10, X_RAX, op->base);
    else
        read_nat(b, r, X_RAX, op->base);
    if (store) {
        read_nat(b, r, X_RDX, op->source);
        x86_alu(b, 0x09, X_RAX, X_RDX, false);
    }
    x86_alu(b, 0x85, X_RAX, X_RAX, false);
    FAIL(X_NE);
    if (op->base < 32)
        read_gr(b, r, X_R10, op->base);
    if (size > 1) {
        x86_mov(b, X_RAX, X_R10);
        x86_imm(b, 4, X_RAX, size - 1);
        FAIL(X_NE);
    }
    x86_load(b, X_RAX, X_R12, V(memory_hook), 8);
    x86_alu(b, 0x85, X_RAX, X_RAX, false);
    FAIL(X_NE);
    x86_mov(b, X_RAX, X_R10);
    x86_shift(b, 5, X_RAX, 12, false);
    x86_mov(b, X_RDX, X_RAX);
    x86_imm(b, 4, X_RDX, TIER0_DTC_SLOTS - 1);
    x86_shift(b, 4, X_RDX, 6, false);
    x86_load(b, X_R9, X_R14, E(dtc), 8);
    x86_alu(b, 0x01, X_R9, X_RDX, false);
    size_t miss[5];
    unsigned nmiss = 0;
    x86_cmp_load(b, X_RAX, X_R9, DTC(page));
    miss[nmiss++] = x86_jcc(b, X_NE);
    x86_cmp_load(b, X_R12, X_R9, DTC(owner));
    miss[nmiss++] = x86_jcc(b, X_NE);
    x86_load(b, X_RAX, X_R12, V(mmu_dseq), 8);
    x86_cmp_load(b, X_RAX, X_R9, DTC(mmu_dseq));
    miss[nmiss++] = x86_jcc(b, X_NE);
    x86_cmp_byte(b, X_R9, DTC(psr_class), (uint8_t)tier0_dtc_class(s->psr));
    miss[nmiss++] = x86_jcc(b, X_NE);
    x86_test_byte(b, X_R9, DTC(rights), store ? IA64_ACCESS_WRITE : IA64_ACCESS_READ);
    miss[nmiss++] = x86_jcc(b, X_E);
    x86_imm(b, 4, X_R10, 4095);
    x86_add_load(b, X_R10, X_R9, DTC(host));
    if (store) {
        x86_load(b, X_RDI, X_R9, DTC(pa), 8);
        store_code_check(b, fallback, nfallback);
    }
    size_t done = x86_jmp(b);
    for (unsigned k = 0; k < nmiss; ++k)
        x86_patch(b, miss[k], b->size);
    arguments(b, i);
    x86_call(b, (uintptr_t)tier0_data_pointer);
    x86_alu(b, 0x85, X_RAX, X_RAX, false);
    FAIL(X_E);
    x86_mov(b, X_R10, X_RAX);
    x86_patch(b, done, b->size);
#undef DTC
#undef FAIL
}
static unsigned inline_requested(struct vcpu *v, const Ia64Instruction *i,
                                 struct tier0_execution *e)
{
    unsigned requested = tier0_take_notification(&v->requested_exit);
    if (!native_branch(i->opcode)) {
        unsigned at = (unsigned)(i - e->region->insns);
        v->s.ip = i->address;
        v->s.slot = i->slot + e->region->slots[at];
        if (v->s.slot == 3) {
            v->s.slot = 0;
            v->s.ip += 16;
        }
    }
    if (!requested)
        return 0;
    e->out = (struct vcpu_exit){requested - 1, i->address, i->slot, 0, "requested exit"};
    e->stop = true;
    return 1;
}
static void account(struct x86_buffer *b, unsigned pending)
{
    if (!pending)
        return;
    x86_add_memory(b, X_R12, V(retired), pending);
    x86_imm(b, 5, X_R13, pending);
}
/* A taken branch to the address in reg, from the region's last instruction. */
static void branch_to(struct x86_buffer *b, unsigned reg)
{
    x86_store(b, X_R12, S(ip), reg, 8);
    x86_store(b, X_R14, E(result.info), reg, 8);
    x86_mov_imm(b, X_RAX, IA64_STEP_BRANCH);
    x86_store(b, X_R14, E(result.kind), X_RAX, 4);
    x86_mov_imm(b, X_RAX, 0);
    x86_store(b, X_R12, S(slot), X_RAX, 4);
}
/* RSI = (reg >= 63) + (reg >= 126), reg <= 188: the RNAT collection slots
 * (one after every 63 data slots, SDM Vol.2 §6.1) a backing-store cursor
 * crosses. Clobbers RDI. */
static void collections(struct x86_buffer *b, unsigned reg)
{
    x86_mov_imm(b, X_RSI, 0);
    x86_mov_imm(b, X_RDI, 0);
    x86_imm(b, 7, reg, 63);
    x86_setcc(b, X_AE, X_RSI);
    x86_imm(b, 7, reg, 126);
    x86_setcc(b, X_AE, X_RDI);
    x86_alu(b, 0x01, X_RSI, X_RDI, false);
}
/* SDM Vol.3 br.call, Vol.2 §6.1 and Table 6-2 (ia64/rse.c): PFS = CFM | EC
 * << 52 | CPL << 62; the caller's sol locals are preserved: BSP advances by
 * them and the collection slots they cross (a BSP on a collection slot first
 * steps past it), BOF by sol modulo 96 (rse_bof < 96) and the dirty count by
 * sol; CFM becomes the sof - sol outputs and the link register the next
 * bundle. The frame shape and CPL are static (region key); the target is
 * read before the link register is written. */
static void emit_call(struct x86_buffer *b, const Ia64Instruction *i, const struct ia64_state *s,
                      uint64_t cfm)
{
    const IA64BranchOperands *op = &i->operands.branch;
    unsigned sof = cfm & 127, sol = (cfm >> 7) & 127;
    uint64_t cpl = (s->psr >> 32) & 3;
    if (i->opcode == IA64_OP_BR_CALL_INDIRECT) {
        x86_load(b, X_R8, X_R12, S(br) + 8 * (op->target & 7), 8);
        x86_imm(b, 4, X_R8, -16);
    } else
        x86_mov_imm(b, X_R8, i->address + (uint64_t)op->displacement);
    x86_load(b, X_RAX, X_R12, S(ar[AR_EC]), 8);
    x86_imm(b, 4, X_RAX, 63);
    x86_shift(b, 4, X_RAX, 52, false);
    x86_load(b, X_RDX, X_R12, S(cfm), 8);
    x86_alu(b, 0x09, X_RAX, X_RDX, false);
    if (cpl) {
        x86_mov_imm(b, X_RDX, cpl << 62);
        x86_alu(b, 0x09, X_RAX, X_RDX, false);
    }
    x86_store(b, X_R12, S(ar[AR_PFS]), X_RAX, 8);
    x86_load(b, X_RAX, X_R12, S(ar[AR_BSP]), 8);
    x86_mov(b, X_RCX, X_RAX);
    x86_shift(b, 5, X_RCX, 3, false);
    x86_imm(b, 4, X_RCX, 63);
    x86_imm(b, 7, X_RCX, 63);
    size_t data = x86_jcc(b, X_NE);
    x86_imm(b, 0, X_RAX, 8);
    x86_mov_imm(b, X_RCX, 0);
    x86_patch(b, data, b->size);
    if (sol) {
        x86_imm(b, 0, X_RCX, (int32_t)sol);
        collections(b, X_RCX);
        x86_shift(b, 4, X_RSI, 3, false);
        x86_alu(b, 0x01, X_RAX, X_RSI, false);
        x86_imm(b, 0, X_RAX, (int32_t)(8 * sol));
        x86_load(b, X_RDX, X_R12, S(rse_bof), 4);
        x86_imm(b, 0, X_RDX, (int32_t)sol);
        x86_imm(b, 7, X_RDX, IA64_N_STACKED_PHYS);
        size_t below = x86_jcc(b, X_B);
        x86_imm(b, 5, X_RDX, IA64_N_STACKED_PHYS);
        x86_patch(b, below, b->size);
        x86_store(b, X_R12, S(rse_bof), X_RDX, 4);
        x86_load(b, X_RDX, X_R12, S(rse_dirty), 4);
        x86_imm(b, 0, X_RDX, (int32_t)sol);
        x86_store(b, X_R12, S(rse_dirty), X_RDX, 4);
    }
    x86_store(b, X_R12, S(ar[AR_BSP]), X_RAX, 8);
    x86_mov_imm(b, X_RAX, sof - sol);
    x86_store(b, X_R12, S(cfm), X_RAX, 8);
    x86_mov_imm(b, X_RAX, i->address + 16);
    x86_store(b, X_R12, S(br) + 8 * (op->link & 7), X_RAX, 8);
    branch_to(b, X_R8);
}
/* SDM Vol.3 br.ret, Vol.2 §6.1, §6.5.5 and Table 6-2 (ia64/rse.c): CFM =
 * PFS.pfm, or zero if its outputs overlap the pre-return dirty partition;
 * EC = PFS.pec; BSP (over the collection slots), BOF (modulo 96) and the
 * dirty count move back by PFS.sol, which leaves the frame complete
 * (rse_missing 0, CFLE clear). The helper takes a return that needs
 * mandatory loads (PFS.sol above the dirty count) or more than the 96
 * physical registers, or lowers the privilege (PFS.ppl > CPL, CPL static). */
static void emit_return(struct x86_buffer *b, const Ia64Instruction *i, const struct ia64_state *s,
                        size_t *fallback, unsigned *nfallback)
{
    unsigned cpl = (s->psr >> 32) & 3;
    x86_load(b, X_R9, X_R12, S(ar[AR_PFS]), 8);
    if (cpl < 3) {
        x86_mov(b, X_RAX, X_R9);
        x86_shift(b, 5, X_RAX, 62, false);
        x86_imm(b, 7, X_RAX, (int32_t)cpl);
        fallback[(*nfallback)++] = x86_jcc(b, X_A);
    }
    x86_mov(b, X_RCX, X_R9);
    x86_shift(b, 5, X_RCX, 7, false);
    x86_imm(b, 4, X_RCX, 127);
    x86_imm(b, 7, X_RCX, IA64_N_STACKED_PHYS);
    fallback[(*nfallback)++] = x86_jcc(b, X_A);
    x86_load(b, X_RDX, X_R12, S(rse_dirty), 4);
    x86_alu(b, 0x39, X_RDX, X_RCX, false);
    fallback[(*nfallback)++] = x86_jcc(b, X_B);
    x86_mov(b, X_RAX, X_R9);
    x86_shift(b, 4, X_RAX, 26, false);
    x86_shift(b, 5, X_RAX, 26, false);
    x86_mov(b, X_R10, X_RAX);
    x86_imm(b, 4, X_RAX, 127);
    x86_alu(b, 0x29, X_RAX, X_RCX, true);
    x86_mov_imm(b, X_RSI, IA64_N_STACKED_PHYS);
    x86_alu(b, 0x29, X_RSI, X_RDX, true);
    x86_alu(b, 0x39, X_RAX, X_RSI, false);
    size_t fits = x86_jcc(b, X_BE);
    x86_mov_imm(b, X_R10, 0);
    x86_patch(b, fits, b->size);
    x86_load(b, X_R8, X_R12, S(br) + 8 * (i->operands.branch.target & 7), 8);
    x86_imm(b, 4, X_R8, -16);
    x86_alu(b, 0x29, X_RDX, X_RCX, false);
    x86_store(b, X_R12, S(rse_dirty), X_RDX, 4);
    x86_load(b, X_RDX, X_R12, S(rse_bof), 4);
    x86_alu(b, 0x29, X_RDX, X_RCX, false);
    size_t no_wrap = x86_jcc(b, X_AE);
    x86_imm(b, 0, X_RDX, IA64_N_STACKED_PHYS);
    x86_patch(b, no_wrap, b->size);
    x86_store(b, X_R12, S(rse_bof), X_RDX, 4);
    /* BSP - 8 * (sol + (sol + 62 - BSP{8:3}) / 63); BSP stays for sol 0. */
    x86_alu(b, 0x85, X_RCX, X_RCX, false);
    size_t none = x86_jcc(b, X_E);
    x86_load(b, X_RAX, X_R12, S(ar[AR_BSP]), 8);
    x86_mov(b, X_RDX, X_RAX);
    x86_shift(b, 5, X_RDX, 3, false);
    x86_imm(b, 4, X_RDX, 63);
    x86_unary(b, 3, X_RDX);
    x86_alu(b, 0x01, X_RDX, X_RCX, false);
    x86_imm(b, 0, X_RDX, 62);
    collections(b, X_RDX);
    x86_alu(b, 0x01, X_RSI, X_RCX, false);
    x86_shift(b, 4, X_RSI, 3, false);
    x86_alu(b, 0x29, X_RAX, X_RSI, false);
    x86_store(b, X_R12, S(ar[AR_BSP]), X_RAX, 8);
    x86_patch(b, none, b->size);
    x86_store(b, X_R12, S(cfm), X_R10, 8);
    x86_mov(b, X_RAX, X_R9);
    x86_shift(b, 5, X_RAX, 52, false);
    x86_imm(b, 4, X_RAX, 63);
    x86_store(b, X_R12, S(ar[AR_EC]), X_RAX, 8);
    x86_mov_imm(b, X_RAX, 0);
    x86_store(b, X_R12, S(rse_missing), X_RAX, 4);
    x86_store(b, X_R12, S(rse_cfle), X_RAX, 1);
    branch_to(b, X_R8);
}
/* SDM Vol.3 alloc (static checks in eligible): CFM takes sof, sol and sor
 * and keeps the rotating bases; GR[r1] = PFS, NaT clear. The helper takes an
 * incomplete frame (rse_missing: undefined), a SOR change while a rotating
 * base is nonzero (Reserved Register/Field fault) and a frame that needs a
 * spill (dirty count + sof > 96, Vol.2 §6.5). Resolved homes stay valid: a
 * SOR change implies zero bases, and homes do not depend on sof. */
static void emit_alloc(struct x86_buffer *b, struct tier0_region *r, const Ia64Instruction *i,
                       uint64_t cfm, size_t *fallback, unsigned *nfallback)
{
    const IA64SystemOperands *op = &i->operands.system;
    uint64_t frame = (uint64_t)op->immediate & 0x3ffff;
    x86_load(b, X_RAX, X_R12, S(rse_missing), 4);
    x86_alu(b, 0x85, X_RAX, X_RAX, false);
    fallback[(*nfallback)++] = x86_jcc(b, X_NE);
    x86_load(b, X_RAX, X_R12, S(cfm), 8);
    if (((frame ^ cfm) >> 14) & 15) {
        x86_mov(b, X_RDX, X_RAX);
        x86_shift(b, 5, X_RDX, 18, false);
        x86_imm(b, 4, X_RDX, 0xfffff);
        fallback[(*nfallback)++] = x86_jcc(b, X_NE);
    }
    x86_load(b, X_RDX, X_R12, S(rse_dirty), 4);
    x86_imm(b, 7, X_RDX, (int32_t)(IA64_N_STACKED_PHYS - (frame & 127)));
    fallback[(*nfallback)++] = x86_jcc(b, X_A);
    x86_imm(b, 4, X_RAX, ~0x3ffff);
    x86_imm(b, 1, X_RAX, (int32_t)frame);
    x86_store(b, X_R12, S(cfm), X_RAX, 8);
    x86_load(b, X_RAX, X_R12, S(ar[AR_PFS]), 8);
    x86_mov_imm(b, X_RDX, 0);
    write_gr(b, r, op->destination, X_RAX, X_RDX);
}
void tier0_emit(struct x86_buffer *b, struct tier0_region *r, const struct ia64_state *s)
{
    size_t exits[TIER0_MAX_INSNS * 5];
    unsigned nexits = 0;
    size_t budgets[TIER0_MAX_INSNS];
    unsigned budget_pending[TIER0_MAX_INSNS], pending = 0;
    /* leave: a taken branch or chk.s from the middle of the region jumps to
     * its stub, which retires `retire` instructions and branches to target. */
    struct cold_stub {
        size_t fallback[16], request, continuation, leave;
        uint64_t target;
        unsigned nfallback, pending, retire;
        bool native, branch, leaves;
    } cold[TIER0_MAX_INSNS];
    x86_push(b, X_R12);
    x86_push(b, X_R13);
    x86_push(b, X_R14);
    x86_push(b, X_R15);
    x86_imm(b, 5, X_RSP, 8);
    x86_mov(b, X_R12, X_RDI);
    x86_mov(b, X_R13, X_RSI);
    x86_mov(b, X_R14, X_RDX);
    x86_mov(b, X_R15, X_RCX);
    /* The frame of each instruction: an alloc in the region (tier0.c
     * ends_region) sets it for the instructions after it. */
    uint64_t cfm = s->cfm, last_cfm = cfm;
    unsigned forwarded = 0;
    for (unsigned at = 0; at < r->count; ++at) {
        const Ia64Instruction *i = &r->insns[at];
        unsigned previous = forwarded;
        forwarded = 0;
        last_cfm = cfm;
        /* Checked virtual references keep the exact helper. */
        bool native = !r->key.tracing && eligible(i, s, cfm) &&
                      !(r->key.checking && (s->psr & IA64_PSR_DT) &&
                        ia64_opcode_info(i->opcode)->family == IA64_FAMILY_MEMORY);
        bool call = i->opcode == IA64_OP_BR_CALL || i->opcode == IA64_OP_BR_CALL_INDIRECT ||
                    i->opcode == IA64_OP_BR_RET;
        bool deferred = native && !predicate_op(i->opcode) && i->opcode != IA64_OP_MOV_GRAR &&
                        i->opcode != IA64_OP_MOV_GRBR && i->opcode != IA64_OP_MOV_GRPR && !call &&
                        i->opcode != IA64_OP_ALLOC &&
                        ia64_opcode_info(i->opcode)->family != IA64_FAMILY_MEMORY;
        /* Helpers can observe ITC/retired and fault after the prefix. Keep
         * their boundary exact; pure arithmetic runs account once. */
        if (!deferred) {
            account(b, pending);
            pending = 0;
        }
        budget_pending[at] = pending;
        x86_imm(b, 7, X_R13, pending + 1);
        budgets[at] = x86_jcc(b, X_B);
        struct cold_stub *c = &cold[at];
        c->native = native;
        /* The active paths assign every other field before its guarded
         * read. Only leaves needs a default when no side exit is emitted;
         * unused fallback entries and inactive records are never read. */
        c->leaves = false;
        size_t *fallback = c->fallback;
        unsigned nfallback = 0;
        if (native) {
            size_t qp_false = 0;
            if (i->qp) {
                x86_load(b, X_RAX, X_R12, S(pr), 8);
                if (i->qp < 16) {
                    x86_bt(b, 4, X_RAX, i->qp);
                    qp_false = x86_jcc(b, X_AE);
                } else {
                    predicate_mask(b, r, i->qp);
                    x86_alu(b, 0x85, X_RAX, X_R11, false);
                    qp_false = x86_jcc(b, X_E);
                }
            }
            bool branch = i->opcode == IA64_OP_BR_COND || i->opcode == IA64_OP_BR_CLOOP;
            /* Only a region's last instruction branches inline; others leave
             * through their stub (a not-taken one continues the region). */
            bool last = at + 1 == r->count;
            c->branch = (branch || call) && last;
            bool memory = ia64_opcode_info(i->opcode)->family == IA64_FAMILY_MEMORY;
            if (predicate_op(i->opcode)) {
                condition(b, r, i, fallback, &nfallback, i->qp ? 0 : previous);
                predicate_write(b, r, i);
            } else if (ar_move(i)) {
                const IA64SystemOperands *op = &i->operands.system;
                int32_t ar = S(ar) + 8 * op->source;
                if (i->opcode == IA64_OP_MOV_ARGR) {
                    x86_load(b, X_RAX, X_R12, ar, 8);
                    x86_mov_imm(b, X_RDX, 0);
                    write_gr(b, r, op->destination, X_RAX, X_RDX);
                } else {
                    if (i->opcode == IA64_OP_MOV_IMMAR)
                        x86_mov_imm(b, X_RAX, (uint64_t)op->immediate);
                    else {
                        read_system_operand(b, r, op->destination, fallback, &nfallback);
                        if (op->source == AR_PFS) {
                            x86_mov_imm(b, X_RDX, PFS_RESERVED);
                            x86_alu(b, 0x85, X_RAX, X_RDX, false);
                            fallback[nfallback++] = x86_jcc(b, X_NE);
                        }
                    }
                    x86_store(b, X_R12, ar, X_RAX, 8);
                }
            } else if (i->opcode == IA64_OP_MOV_GRBR || i->opcode == IA64_OP_MOV_BRGR) {
                /* SDM Vol.3 mov b: BR[b] = GR[r] (a NaT source faults), GR[r] = BR[b]. */
                const IA64SystemOperands *op = &i->operands.system;
                int32_t br = S(br) + 8 * (op->source & 7);
                if (i->opcode == IA64_OP_MOV_BRGR) {
                    x86_load(b, X_RAX, X_R12, br, 8);
                    x86_mov_imm(b, X_RDX, 0);
                    write_gr(b, r, op->destination, X_RAX, X_RDX);
                } else {
                    read_system_operand(b, r, op->destination, fallback, &nfallback);
                    x86_store(b, X_R12, br, X_RAX, 8);
                }
            } else if (i->opcode == IA64_OP_CHK_S) {
                /* SDM Vol.3 chk.s (GR form): a set NaT bit branches to the
                 * recovery code; the cold stub leaves the region there. */
                read_nat(b, r, X_RAX, i->operands.memory.source);
                x86_alu(b, 0x85, X_RAX, X_RAX, false);
                c->leave = x86_jcc(b, X_NE);
                c->leaves = true;
                c->retire = 1;
                c->target = i->address + (uint64_t)i->operands.memory.immediate;
            } else if (i->opcode == IA64_OP_MF || i->opcode == IA64_OP_MF_A) {
                /* SDM Vol.3 mf, mf.a: the helper's full fence (MFENCE). */
                x86_mfence(b);
            } else if (i->opcode == IA64_OP_MOV_PRGR) {
                /* SDM Vol.3 mov r=pr: all 64 predicates, pr0 reads 1. */
                x86_load(b, X_RAX, X_R12, S(pr), 8);
                x86_imm(b, 1, X_RAX, 1);
                x86_mov_imm(b, X_RDX, 0);
                write_gr(b, r, i->operands.system.destination, X_RAX, X_RDX);
            } else if (i->opcode == IA64_OP_MOV_GRPR || i->opcode == IA64_OP_MOV_PR_ROT_IMM) {
                /* SDM Vol.3 mov pr=r,mask17 (a NaT source faults) and
                 * mov pr.rot=imm44: the masked predicates, never pr0. */
                const IA64SystemOperands *op = &i->operands.system;
                uint64_t field =
                    (i->opcode == IA64_OP_MOV_GRPR ? (uint64_t)op->immediate : ~UINT64_C(0xffff)) &
                    ~UINT64_C(1);
                if (i->opcode == IA64_OP_MOV_GRPR) {
                    read_system_operand(b, r, op->destination, fallback, &nfallback);
                } else
                    x86_mov_imm(b, X_RAX, (uint64_t)op->immediate);
                x86_mov_imm(b, X_RCX, field);
                x86_alu(b, 0x21, X_RAX, X_RCX, false);
                x86_unary(b, 2, X_RCX);
                x86_load(b, X_RDX, X_R12, S(pr), 8);
                x86_alu(b, 0x21, X_RDX, X_RCX, false);
                x86_alu(b, 0x09, X_RAX, X_RDX, false);
                x86_imm(b, 1, X_RAX, 1);
                x86_store(b, X_R12, S(pr), X_RAX, 8);
            } else if (i->opcode == IA64_OP_LFETCH || i->opcode == IA64_OP_LFETCH_FAULT) {
                /* SDM Vol.3 lfetch: no access. The faulting form needs the
                 * page's translation and read right (the DTC entry); a NaT
                 * base faults there and would make a post-increment NaT. */
                const IA64MemoryOperands *op = &i->operands.memory;
                bool update = i->reg_base_update || i->imm_base_update;
                if (i->opcode == IA64_OP_LFETCH_FAULT)
                    virtual_pointer(b, r, i, s, fallback, &nfallback);
                else if (update) {
                    read_nat(b, r, X_RAX, op->base);
                    x86_alu(b, 0x85, X_RAX, X_RAX, false);
                    fallback[nfallback++] = x86_jcc(b, X_NE);
                }
                if (update) {
                    read_gr(b, r, X_R8, op->base);
                    if (i->reg_base_update)
                        read_integer_operand(b, r, X_RAX, X_R9, op->source);
                    else {
                        x86_mov_imm(b, X_RAX, (uint64_t)op->immediate);
                        x86_mov_imm(b, X_R9, 0);
                    }
                    x86_alu(b, 0x01, X_R8, X_RAX, false);
                    write_gr(b, r, op->base, X_R8, X_R9);
                }
            } else if (memory) {
                if (!(s->psr & IA64_PSR_DT) && !r->key.checking) {
                    physical_pointer(b, r, i, fallback, &nfallback);
                } else if (!r->key.checking) {
                    virtual_pointer(b, r, i, s, fallback, &nfallback);
                } else {
                    arguments(b, i);
                    x86_call(b, (uintptr_t)tier0_memory_pointer);
                    x86_alu(b, 0x85, X_RAX, X_RAX, false);
                    fallback[nfallback++] = x86_jcc(b, X_E);
                    x86_mov(b, X_R10, X_RAX);
                }
                const IA64MemoryOperands *op = &i->operands.memory;
                bool store = ia64_opcode_is_store(i->opcode);
                unsigned size = tier0_access_size(i->opcode);
                bool update = i->reg_base_update || i->imm_base_update;
                if (update) {
                    /* Capture the old base/increment before a load writes
                     * its destination, which may alias the increment GR. */
                    read_gr(b, r, X_R8, op->base);
                    if (i->reg_base_update) {
                        read_integer_operand(b, r, X_RAX, X_R9, op->source);
                    } else {
                        x86_mov_imm(b, X_RAX, (uint64_t)op->immediate);
                        x86_mov_imm(b, X_R9, 0);
                    }
                    x86_alu(b, 0x01, X_R8, X_RAX, false);
                }
                if (store) {
                    read_gr(b, r, X_RAX, op->source);
                    /* st.rel: the helper's fence (ia64_physical_write_ordered). */
                    if (i->mem_release)
                        x86_sfence(b);
                    x86_store(b, X_R10, 0, X_RAX, size);
                } else {
                    x86_load(b, X_RAX, X_R10, 0, size);
                    x86_mov_imm(b, X_RDX, 0);
                    write_gr(b, r, op->destination, X_RAX, X_RDX);
                }
                if (update)
                    write_gr(b, r, op->base, X_R8, X_R9);
            } else if (branch) {
                /* SDM Vol.3 br.cloop: taken while LC != 0, decrementing it. */
                size_t not_taken = 0;
                if (i->opcode == IA64_OP_BR_CLOOP) {
                    x86_load(b, X_RAX, X_R12, S(ar[AR_LC]), 8);
                    x86_alu(b, 0x85, X_RAX, X_RAX, false);
                    not_taken = x86_jcc(b, X_E);
                    x86_imm(b, 5, X_RAX, 1);
                    x86_store(b, X_R12, S(ar[AR_LC]), X_RAX, 8);
                }
                if (!last) {
                    /* Deferred like arithmetic: the stub retires the prefix too. */
                    c->leave = x86_jmp(b);
                    c->leaves = true;
                    c->retire = pending + 1;
                    c->target = i->address + (uint64_t)i->operands.branch.displacement;
                    if (not_taken)
                        x86_patch(b, not_taken, b->size);
                } else {
                    x86_mov_imm(b, X_RAX, i->address + (uint64_t)i->operands.branch.displacement);
                    x86_store(b, X_R12, S(ip), X_RAX, 8);
                    x86_store(b, X_R14, E(result.info), X_RAX, 8);
                    x86_mov_imm(b, X_RAX, IA64_STEP_BRANCH);
                    x86_store(b, X_R14, E(result.kind), X_RAX, 4);
                    x86_mov_imm(b, X_RAX, 0);
                    x86_store(b, X_R12, S(slot), X_RAX, 4);
                    if (not_taken) {
                        size_t done = x86_jmp(b);
                        x86_patch(b, not_taken, b->size);
                        next_ip(b, i, r->slots[at]);
                        x86_patch(b, done, b->size);
                    }
                }
            } else if (call) {
                /* A call or return taken from the middle of the region
                 * keeps the exact helper, which leaves the region. */
                if (!last)
                    fallback[nfallback++] = x86_jmp(b);
                else if (i->opcode == IA64_OP_BR_RET)
                    emit_return(b, i, s, fallback, &nfallback);
                else
                    emit_call(b, i, s, cfm);
            } else if (i->opcode == IA64_OP_ALLOC) {
                emit_alloc(b, r, i, cfm, fallback, &nfallback);
            } else if (empty_hint(i->opcode)) {
                if (forwards_hint(i))
                    forwarded = previous;
            } else {
                if (!i->qp && reads_after_hints(r, at + 1, i->operands.integer.destination))
                    forwarded = i->operands.integer.destination;
                integer(b, r, i, i->qp ? 0 : previous, forwarded != 0);
            }
            size_t taken = 0;
            if (c->branch && i->qp)
                taken = x86_jmp(b);
            size_t skip = 0;
            if (qp_false) {
                if (!branch && i->compare_unc)
                    skip = x86_jmp(b);
                x86_patch(b, qp_false, b->size);
                if (i->compare_unc)
                    predicate_clear(b, r, i->operands.integer.predicate1,
                                    i->operands.integer.predicate2);
                if (skip)
                    x86_patch(b, skip, b->size);
            }
            if (c->branch && i->qp)
                next_ip(b, i, r->slots[at]);
            if (taken)
                x86_patch(b, taken, b->size);
            count_inline(b, i->opcode);
            /* PSR is part of the region key, so PSR.ic is known here. The
             * previous instruction of the same bundle, native or helper,
             * already recorded this bundle when it completed. */
            /* Retained RAX/R8 already have committed GR/NaT stores. Only
             * these two postamble operations normally clobber RAX; use
             * RDX until the consumer. Stats use R11, accounting
             * uses R13, and request/budget cold stubs exit the region. */
            unsigned scratch = forwarded ? X_RDX : X_RAX;
            if ((s->psr & IA64_PSR_IC) && (!at || r->insns[at - 1].address != i->address)) {
                x86_mov_imm(b, scratch, i->address);
                x86_store(b, X_R12, V(last_successful_bundle), scratch, 8);
            }
            /* A fallback helper retires this instruction before rejoining.
             * Deferring it would count it again in a later prefix flush. */
            if (deferred && nfallback)
                b->failed = true;
            if (deferred)
                ++pending;
            else
                account(b, 1);
            x86_load(b, scratch, X_R12, V(requested_exit), 4);
            x86_alu(b, 0x85, scratch, scratch, false);
            c->request = x86_jcc(b, X_NE);
            c->pending = pending;
            c->nfallback = nfallback;
        } else {
            arguments(b, i);
            x86_call(b, (uintptr_t)tier0_helper_for(i));
            x86_alu(b, 0x85, X_RAX, X_RAX, false);
            exits[nexits++] = x86_jcc(b, X_NE);
            x86_imm(b, 5, X_R13, 1);
        }
        c->continuation = b->size;
        if (i->opcode == IA64_OP_ALLOC)
            cfm = (cfm & ~UINT64_C(0x3ffff)) | ((uint64_t)i->operands.system.immediate & 0x3ffff);
    }
    account(b, pending);
    const Ia64Instruction *last = &r->insns[r->count - 1];
    if (eligible(last, s, last_cfm) && !native_branch(last->opcode))
        next_ip(b, last, r->slots[r->count - 1]);
    exits[nexits++] = x86_jmp(b);
    /* Budget exits materialize the first unexecuted instruction. Helper
     * exits already carry a current fault PC or committed resume PC. */
    for (unsigned at = 0; at < r->count; ++at) {
        const Ia64Instruction *i = &r->insns[at];
        unsigned end = at;
        /* Consecutive pending counts prove that no account/helper
         * boundary intervened. Within a bundle of single-slot insns,
         * R13 on the first failed check equals the completed pending
         * prefix; its slot is R13 + (first slot - first pending count).
         * Keep distinct stubs at long instructions and accounting cuts. */
        while (end + 1 < r->count && r->slots[end] == 1 && r->slots[end + 1] == 1 &&
               r->insns[end + 1].address == i->address &&
               r->insns[end + 1].slot == r->insns[end].slot + 1 &&
               budget_pending[end + 1] == budget_pending[end] + 1)
            ++end;
        for (unsigned n = at; n <= end; ++n)
            x86_patch(b, budgets[n], b->size);
        if (end != at)
            x86_add_store(b, X_R12, V(retired), X_R13);
        else
            account(b, budget_pending[at]);
        x86_mov_imm(b, X_RAX, i->address);
        x86_store(b, X_R12, S(ip), X_RAX, 8);
        if (end != at) {
            x86_mov(b, X_RAX, X_R13);
            int32_t delta = (int32_t)i->slot - (int32_t)budget_pending[at];
            if (delta)
                x86_imm(b, 0, X_RAX, delta);
        } else
            x86_mov_imm(b, X_RAX, i->slot);
        x86_store(b, X_R12, S(slot), X_RAX, 4);
        exits[nexits++] = x86_jmp(b);
        at = end;
    }
    /* Only exceptional memory/NaT paths and notification consumption need
     * helper stubs. The ordinary native path falls through contiguously. */
    for (unsigned at = 0; at < r->count; ++at) {
        struct cold_stub *c = &cold[at];
        const Ia64Instruction *i = &r->insns[at];
        if (!c->native)
            continue;
        if (c->nfallback) {
            for (unsigned k = 0; k < c->nfallback; ++k)
                x86_patch(b, c->fallback[k], b->size);
            arguments(b, i);
            x86_call(b, (uintptr_t)tier0_helper_for(i));
            x86_alu(b, 0x85, X_RAX, X_RAX, false);
            exits[nexits++] = x86_jcc(b, X_NE);
            x86_imm(b, 5, X_R13, 1);
            size_t resume = x86_jmp(b);
            x86_patch(b, resume, c->continuation);
        }
        if (c->leaves) {
            /* A taken branch or chk.s retires with the prefix before it
             * (chk.s, a memory-family form, has none pending), records its
             * bundle and branches. */
            x86_patch(b, c->leave, b->size);
            account(b, c->retire);
            if (s->psr & IA64_PSR_IC) {
                x86_mov_imm(b, X_RAX, i->address);
                x86_store(b, X_R12, V(last_successful_bundle), X_RAX, 8);
            }
            x86_mov_imm(b, X_RAX, c->target);
            x86_store(b, X_R12, S(ip), X_RAX, 8);
            x86_store(b, X_R14, E(result.info), X_RAX, 8);
            x86_mov_imm(b, X_RAX, IA64_STEP_BRANCH);
            x86_store(b, X_R14, E(result.kind), X_RAX, 4);
            x86_mov_imm(b, X_RAX, 0);
            x86_store(b, X_R12, S(slot), X_RAX, 4);
            exits[nexits++] = x86_jmp(b); /* the branch's exit */
        }
        x86_patch(b, c->request, b->size);
        account(b, c->pending);
        arguments(b, i);
        x86_call(b, (uintptr_t)inline_requested);
        /* The helper commits the next IP of non-branch forms even if a
         * future multiple-consumer design returns no notification. A
         * middle, not-taken branch still needs its fallthrough IP here;
         * final native branches already committed their IP in the hot path. */
        if (!c->branch && native_branch(i->opcode))
            next_ip(b, i, r->slots[at]);
        exits[nexits++] = x86_jmp(b);
    }
    for (unsigned k = 0; k < nexits; ++k)
        x86_patch(b, exits[k], b->size);
    x86_imm(b, 0, X_RSP, 8);
    x86_pop(b, X_R15);
    x86_pop(b, X_R14);
    x86_pop(b, X_R13);
    x86_pop(b, X_R12);
    x86_u8(b, 0xc3);
}
