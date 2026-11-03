/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/semantics.h"
#include "ia64/fp.h"
#include "ia64/hostfp.h"
#include "ia64/profile.h"

/* These properties depend only on the opcode. Fold the classification
 * into the existing read-only metadata instead of repeating its range
 * and equality tests for every executed instruction. */
#define PREDICATE_PAIR(op)                                                                         \
    (((op) >= IA64_OP_CMP_EQ && (op) <= IA64_OP_CMP_LTU_IMM) ||                                    \
     ((op) >= IA64_OP_CMP4_EQ && (op) <= IA64_OP_CMP4_LTU_IMM) ||                                  \
     ((op) >= IA64_OP_CMP4_EQ_IMM && (op) <= IA64_OP_CMP4_NE_OR_IMM) || (op) == IA64_OP_TBIT_Z ||  \
     (op) == IA64_OP_TBIT_NZ || (op) == IA64_OP_TBIT_Z_OR_ANDCM ||                                 \
     (op) == IA64_OP_TBIT_NZ_OR_ANDCM || (op) == IA64_OP_TNAT_Z || (op) == IA64_OP_TNAT_NZ ||      \
     (op) == IA64_OP_TNAT_NZ_AND || (op) == IA64_OP_FCMP || (op) == IA64_OP_FCLASS ||              \
     (op) == IA64_OP_TF_Z || (op) == IA64_OP_TF_NZ)
#define LOOP_BRANCH(op)                                                                            \
    ((op) == IA64_OP_BR_CLOOP || (op) == IA64_OP_BR_CTOP || (op) == IA64_OP_BR_CEXIT ||            \
     (op) == IA64_OP_BR_WTOP || (op) == IA64_OP_BR_WEXIT)

static const struct ia64_opcode_info opcode_info[IA64_OP_COUNT] = {
#define IA64_OPCODE(name, family, gr, nat)                                                         \
    [IA64_OP_##name] = {#name, IA64_FAMILY_##family,           IA64_NAT_##nat,                     \
                        gr,    PREDICATE_PAIR(IA64_OP_##name), LOOP_BRANCH(IA64_OP_##name)},
#include "ia64/decode/opcode.inc"
#undef IA64_OPCODE
};
#undef PREDICATE_PAIR
#undef LOOP_BRANCH
const struct ia64_opcode_info *ia64_opcode_info(Ia64Opcode opcode)
{
    return &opcode_info[(unsigned)opcode < IA64_OP_COUNT ? opcode : IA64_OP_ILLEGAL];
}

bool ia64_opcode_supported(Ia64Opcode opcode)
{
    switch (opcode) {
#define IA64_SUPPORTED(name) case IA64_OP_##name:
#include "ia64/decode/supported.inc"
#undef IA64_SUPPORTED
        return true;
    default:
        return false;
    }
}

/* Legacy enum names include assembler aliases and placeholders. Keep the
 * enum stable, but count only instructions the decoder can return. */
bool ia64_opcode_reachable(Ia64Opcode opcode)
{
    switch (opcode) {
    case IA64_OP_CMP_GEU:
    case IA64_OP_SHR_IMM:
    case IA64_OP_SHRU_IMM:
    case IA64_OP_FMOV:
    case IA64_OP_TBIT_Z_OR_ANDCM:
    case IA64_OP_TBIT_NZ_OR_ANDCM:
    case IA64_OP_TNAT_NZ_AND:
    case IA64_OP_MPYSH:
    case IA64_OP_MPYUH:
    case IA64_OP_MUX:
    case IA64_OP_MOV_PKRGR:
    case IA64_OP_MOV_GRPKR:
    case IA64_OP_MOV_IBRGR:
    case IA64_OP_MOV_GRIBR:
    case IA64_OP_MOV_DBRGR:
    case IA64_OP_MOV_GRDBR:
    case IA64_OP_MOV_PMCGR:
    case IA64_OP_MOV_GRPMC:
    case IA64_OP_MOV_PMDGR:
    case IA64_OP_MOV_GRPMD:
    case IA64_OP_MOV_CPUID:
    case IA64_OP_MOV_IP:
    case IA64_OP_SUM:
        return false;
    default:
        return (unsigned)opcode < IA64_OP_COUNT;
    }
}

struct ia64_result ia64_execute(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64CommonOperands *op = &insn->operands.common;
    const struct ia64_opcode_info *meta = ia64_opcode_info(insn->opcode);
    if (!insn->valid || insn->encoding_class == IA64_ENCODING_RESERVED)
        return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
    /* SDM Vol.3 alloc: this check precedes the qualifying predicate. */
    if (insn->opcode == IA64_OP_ALLOC && insn->qp)
        return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
    bool qp = ia64_pr_read(s, insn->qp);
    bool pair = meta->predicate_pair;
    if (insn->compare_unc && pair && op->auxiliary1 == op->auxiliary2)
        return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
    bool loop = meta->loop_branch;
    if (!qp && !loop) {
        if (insn->compare_unc && pair) {
            ia64_pr_write(s, op->auxiliary1, false);
            ia64_pr_write(s, op->auxiliary2, false);
        }
        if (insn->clear_p2_before_predicate)
            ia64_pr_write(s, op->auxiliary2, false);
        return ia64_complete();
    }
    if (insn->placement_illegal || insn->opcode == IA64_OP_ILLEGAL || (loop && insn->slot != 2) ||
        (pair && op->auxiliary1 == op->auxiliary2))
        return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
    if (meta->writes_gr && insn->opcode != IA64_OP_ALLOC &&
        (op->destination == 0 ||
         (op->destination >= 32 && op->destination - 32u >= (s->cfm & 127))))
        return ia64_fault(IA64_FAULT_ILLEGAL, op->destination);
    if (insn->reserved_field)
        return ia64_fault(IA64_FAULT_RESERVED, insn->raw);
    /* Fork translate.c: placement, reserved fields, then model features. */
    if (!ia64_model_instruction(v->cfg.cpu_model, insn->opcode))
        return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
    switch (meta->family) {
    case IA64_FAMILY_INTEGER:
        return ia64_integer(v, insn);
    case IA64_FAMILY_SIMD:
        return ia64_simd(v, insn);
    case IA64_FAMILY_BRANCH:
        return ia64_branch(v, insn);
    case IA64_FAMILY_SYSTEM:
        if (insn->opcode == IA64_OP_FSETC || insn->opcode == IA64_OP_FCLRF ||
            insn->opcode == IA64_OP_FCHKF)
            return ia64_fp(v, insn);
        return ia64_system(v, insn);
    case IA64_FAMILY_MEMORY:
        return ia64_memory(v, insn);
    case IA64_FAMILY_FP:
        return ia64_hostfp(v, insn);
    default:
        return ia64_unimplemented(insn);
    }
}
