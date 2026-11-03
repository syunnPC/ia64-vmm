/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/semantics.h"
#include "ia64/profile.h"

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

/* SDM Vol.3 cmp/cmp4/tbit/tnat. Parallel AND/OR writes the same
 * condition to both targets. Assembler pseudo-ops reverse the relation
 * and/or destinations, so no separate orcm/andcm helper is needed. */
static void predicate_result(struct ia64_state *s, unsigned p1, unsigned p2, bool value, bool nat,
                             Ia64PredicateUpdate update)
{
    uint64_t first = p1 ? UINT64_C(1) << ia64_pr_index(s, p1) : 0;
    uint64_t second = p2 ? UINT64_C(1) << ia64_pr_index(s, p2) : 0;
    uint64_t active = 0 - (uint64_t)(value && !nat);
    switch (update) {
    case IA64_PRED_UPDATE_AND:
        s->pr &= ~((first | second) & ~active);
        break;
    case IA64_PRED_UPDATE_OR:
        s->pr |= (first | second) & active;
        break;
    case IA64_PRED_UPDATE_OR_ANDCM:
        s->pr = (s->pr | (first & active)) & ~(second & active);
        break;
    default:
        /* The second write wins if diagnostic operands alias. p0 has a
         * zero mask, so neither target can clear the constant predicate. */
        s->pr = (s->pr & ~(first | second)) | (first & ~second & active) |
                (second & (0 - (uint64_t)(!value && !nat)));
        break;
    }
    s->pr |= 1;
}

static bool relation_value(enum relation rel, uint64_t a, uint64_t b)
{
    /* Sign-bit bias gives signed ordering using only unsigned arithmetic. */
    uint64_t sa = a ^ (UINT64_C(1) << 63), sb = b ^ (UINT64_C(1) << 63);
    switch (rel) {
    case REL_EQ:
        return a == b;
    case REL_NE:
        return a != b;
    case REL_LT:
        return sa < sb;
    case REL_LE:
        return sa <= sb;
    case REL_GT:
        return sa > sb;
    case REL_GE:
        return sa >= sb;
    case REL_LTU:
        return a < b;
    case REL_LEU:
        return a <= b;
    case REL_GTU:
        return a > b;
    case REL_GEU:
        return a >= b;
    }
    return false;
}

static uint64_t sign_extend(uint64_t value, unsigned bits)
{
    if (bits == 64)
        return value;
    uint64_t sign = UINT64_C(1) << (bits - 1);
    return ((value & ia64_low_mask(bits)) ^ sign) - sign;
}
static uint64_t sar(uint64_t value, unsigned shift)
{
    if (!shift)
        return value;
    return (value >> shift) | ((value >> 63) ? ~ia64_low_mask(64 - shift) : 0);
}

struct ia64_result ia64_integer(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64IntegerOperands *op = &insn->operands.integer;
    struct ia64_gr_value left = ia64_gr_read_value(s, op->source1);
    struct ia64_gr_value right = ia64_gr_read_value(s, op->source2);
    uint64_t a = left.value, b = right.value;
    uint64_t imm = (uint64_t)op->immediate, value = 0;
    const struct compare_desc *cmp = &compares[insn->opcode];
    if (cmp->valid) {
        bool nat = right.nat || (!cmp->immediate && left.nat);
        if (cmp->immediate)
            a = imm;
        if (cmp->word) {
            bool signed_rel = cmp->relation >= REL_LT && cmp->relation <= REL_GE;
            a = signed_rel ? sign_extend(a, 32) : (uint32_t)a;
            b = signed_rel ? sign_extend(b, 32) : (uint32_t)b;
        }
        predicate_result(s, op->predicate1, op->predicate2, relation_value(cmp->relation, a, b),
                         nat, cmp->update);
        return ia64_complete();
    }
    switch (insn->opcode) {
    case IA64_OP_TF_Z:
    case IA64_OP_TF_NZ: {
        bool bit = (ia64_model_cpuid(v->cfg.cpu_model, 4) >> (imm & 63)) & 1;
        predicate_result(s, op->predicate1, op->predicate2,
                         insn->opcode == IA64_OP_TF_NZ ? bit : !bit, false, insn->pred_update);
        return ia64_complete();
    }
    case IA64_OP_TBIT_Z:
    case IA64_OP_TBIT_NZ:
    case IA64_OP_TBIT_Z_OR_ANDCM:
    case IA64_OP_TBIT_NZ_OR_ANDCM:
    case IA64_OP_TNAT_Z:
    case IA64_OP_TNAT_NZ:
    case IA64_OP_TNAT_NZ_AND: {
        bool nat_test = insn->opcode == IA64_OP_TNAT_Z || insn->opcode == IA64_OP_TNAT_NZ ||
                        insn->opcode == IA64_OP_TNAT_NZ_AND;
        bool nz = insn->opcode == IA64_OP_TBIT_NZ || insn->opcode == IA64_OP_TBIT_NZ_OR_ANDCM ||
                  insn->opcode == IA64_OP_TNAT_NZ || insn->opcode == IA64_OP_TNAT_NZ_AND;
        bool nat = right.nat;
        bool condition = nat_test ? nat : ((b >> (imm & 63)) & 1);
        Ia64PredicateUpdate update = insn->pred_update;
        if (insn->opcode == IA64_OP_TBIT_Z_OR_ANDCM || insn->opcode == IA64_OP_TBIT_NZ_OR_ANDCM)
            update = IA64_PRED_UPDATE_OR_ANDCM;
        if (insn->opcode == IA64_OP_TNAT_NZ_AND)
            update = IA64_PRED_UPDATE_AND;
        predicate_result(s, op->predicate1, op->predicate2, nz ? condition : !condition,
                         nat && !nat_test, update);
        return ia64_complete();
    }
    case IA64_OP_ADDS:
    case IA64_OP_ADDL:
        value = b + imm;
        break;
    case IA64_OP_ADD:
        value = a + b;
        break;
    case IA64_OP_ADD_ONE:
        value = a + b + 1;
        break;
    case IA64_OP_SUB:
        value = a - b;
        break;
    case IA64_OP_SUB_ONE:
        value = a - b - 1;
        break;
    case IA64_OP_SUB_IMM:
        value = imm - b;
        break;
    case IA64_OP_AND:
        value = a & b;
        break;
    case IA64_OP_ANDCM:
        value = a & ~b;
        break;
    case IA64_OP_OR:
        value = a | b;
        break;
    case IA64_OP_XOR:
        value = a ^ b;
        break;
    case IA64_OP_AND_IMM:
        value = imm & b;
        break;
    case IA64_OP_ANDCM_IMM:
        value = imm & ~b;
        break;
    case IA64_OP_OR_IMM:
        value = imm | b;
        break;
    case IA64_OP_XOR_IMM:
        value = imm ^ b;
        break;
    case IA64_OP_SHLADD:
        value = (a << imm) + b;
        break;
    case IA64_OP_ADDP4:
    case IA64_OP_ADDP4_IMM:
    case IA64_OP_SHLADDP4:
        if (insn->opcode == IA64_OP_ADDP4_IMM)
            a = imm;
        if (insn->opcode == IA64_OP_SHLADDP4)
            a <<= imm;
        value = (uint32_t)(a + b) | (((b >> 30) & 3) << 61);
        break;
    case IA64_OP_SHL:
        value = a >= 64 ? 0 : b << a;
        break;
    case IA64_OP_SHRU:
        value = a >= 64 ? 0 : b >> a;
        break;
    case IA64_OP_SHR:
        value = sar(b, a >= 64 ? 63 : (unsigned)a);
        break;
    case IA64_OP_SHL_IMM:
        value = b << (imm & 63);
        break;
    case IA64_OP_SHRU_IMM:
        value = b >> (imm & 63);
        break;
    case IA64_OP_SHR_IMM:
        value = sar(b, imm & 63);
        break;
    case IA64_OP_DEPZ:
    case IA64_OP_DEPZ_IMM:
    case IA64_OP_DEP:
    case IA64_OP_DEP_IMM: {
        unsigned pos = imm & 63, len = (imm >> 6) & 127;
        uint64_t mask = ia64_deposit_mask(pos, len);
        if (insn->opcode == IA64_OP_DEPZ_IMM)
            a = sign_extend(imm >> 13, 8);
        if (insn->opcode == IA64_OP_DEP_IMM)
            a = 0 - ((imm >> 13) & 1);
        value = (a << pos) & mask;
        if (insn->opcode == IA64_OP_DEP || insn->opcode == IA64_OP_DEP_IMM)
            value |= b & ~mask;
        break;
    }
    case IA64_OP_EXTRU:
    case IA64_OP_EXTR: {
        unsigned pos = imm & 63;
        unsigned len = ia64_bitfield_effective_len(pos, imm >> 6);
        value = (b >> pos) & ia64_low_mask(len);
        if (insn->opcode == IA64_OP_EXTR)
            value = sign_extend(value, len);
        break;
    }
    case IA64_OP_SXT1:
        value = sign_extend(b, 8);
        break;
    case IA64_OP_SXT2:
        value = sign_extend(b, 16);
        break;
    case IA64_OP_SXT4:
        value = sign_extend(b, 32);
        break;
    case IA64_OP_ZXT1:
        value = (uint8_t)b;
        break;
    case IA64_OP_ZXT2:
        value = (uint16_t)b;
        break;
    case IA64_OP_ZXT4:
        value = (uint32_t)b;
        break;
    case IA64_OP_SHRP_IMM:
        value = (imm & 63) ? (b >> (imm & 63)) | (a << (64 - (imm & 63))) : b;
        break;
    case IA64_OP_MOVL:
        value = imm;
        break;
    case IA64_OP_POPCNT:
        /* Retain a cheap path for zero and one-bit values. */
        if (!(b & (b - 1))) {
            value = b != 0;
            break;
        }
        /* Sum bit counts in 2-, 4-, then 8-bit lanes. Each byte is at
         * most 8, so multiplying by ones collects the total in the top
         * byte without a carry between partial sums (the total is <=64).
         * Fixed work for denser values, without POPCNT or a libgcc call. */
        b -= (b >> 1) & UINT64_C(0x5555555555555555);
        b = (b & UINT64_C(0x3333333333333333)) + ((b >> 2) & UINT64_C(0x3333333333333333));
        b = (b + (b >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
        value = (b * UINT64_C(0x0101010101010101)) >> 56;
        break;
    default:
        return ia64_unimplemented(insn);
    }
    bool nat = false;
    switch (ia64_opcode_info(insn->opcode)->nat_rule) {
    case IA64_NAT_SOURCE_R2:
        nat = left.nat;
        break;
    case IA64_NAT_SOURCE_R3:
        nat = right.nat;
        break;
    case IA64_NAT_SOURCES_R1_R2_R3:
        nat = ia64_nat_read(s, op->destination);
        /* fall through */
    case IA64_NAT_SOURCES_R2_R3:
        nat |= left.nat || right.nat;
        break;
    default:
        break;
    }
    ia64_gr_write(s, op->destination, value, nat);
    return ia64_complete();
}
