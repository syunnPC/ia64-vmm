/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/semantics.h"
#include "ia64/simd-ops.h"

static int64_t signed_lane(uint64_t value, unsigned pos, unsigned bits)
{
    uint64_t mask = ia64_low_mask(bits), sign = UINT64_C(1) << (bits - 1);
    return (int64_t)((((value >> pos) & mask) ^ sign) - sign);
}
static int64_t clamp(int64_t value, int64_t low, int64_t high)
{
    return value < low ? low : value > high ? high : value;
}

static uint64_t lane_mask(uint64_t high_bits, unsigned bits)
{
    /* Each selected high bit expands to its whole lane. The high lane's
     * shift may wrap modulo 2^64, which gives the same all-ones mask. */
    return (high_bits << 1) - (high_bits >> (bits - 1));
}

static uint64_t packed_addsub(uint64_t a, uint64_t b, unsigned bits, unsigned sat, bool sub)
{
    uint64_t high = bits == 8    ? UINT64_C(0x8080808080808080)
                    : bits == 16 ? UINT64_C(0x8000800080008000)
                                 : UINT64_C(0x8000000080000000);
    /* Guard bits stop carry/borrow from crossing a lane boundary. Restore
     * each lane's high result bit separately after its low-bit arithmetic. */
    uint64_t result = sub ? ((a | high) - (b & ~high)) ^ ((a ^ ~b) & high)
                          : ((a & ~high) + (b & ~high)) ^ ((a ^ b) & high);
    if (!sat)
        return result;
    if (sat == 1) {
        uint64_t overflow = (sub ? (a ^ b) : ~(a ^ b)) & (a ^ result) & high;
        uint64_t replace = lane_mask(overflow, bits);
        uint64_t limit = high - (high >> (bits - 1)) + ((a & high) >> (bits - 1));
        return result ^ ((result ^ limit) & replace);
    }
    uint64_t carry =
        (sub ? ((~a & b) | ((~a | b) & result)) : ((a & b) | ((a | b) & ~result))) & high;
    if (sat == 2)
        return sub ? result & ~lane_mask(carry, bits) : result | lane_mask(carry, bits);
    /* With an unsigned left and signed right input, carry/borrow XOR the
     * right sign identifies either saturation. Select all ones only for
     * positive overflow; negative overflow selects zero. */
    uint64_t sign = b & high;
    uint64_t overflow = lane_mask(carry ^ sign, bits);
    uint64_t maximum = lane_mask(sub ? ~carry & sign : carry & ~sign, bits);
    return (result & ~overflow) | maximum;
}

struct ia64_result ia64_simd(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64SimdOperands *op = &insn->operands.simd;
    struct ia64_gr_value left_value = ia64_gr_read_value(s, op->source1);
    struct ia64_gr_value right_value = ia64_gr_read_value(s, op->source2);
    uint64_t a = left_value.value, b = right_value.value;
    uint64_t value = 0;
    bool nat = left_value.nat || right_value.nat;
    switch (insn->opcode) {
    case IA64_OP_PADD1:
    case IA64_OP_PADD2:
    case IA64_OP_PADD4:
    case IA64_OP_PSUB1:
    case IA64_OP_PSUB2:
    case IA64_OP_PSUB4: {
        unsigned bits = insn->opcode == IA64_OP_PADD1 || insn->opcode == IA64_OP_PSUB1   ? 8
                        : insn->opcode == IA64_OP_PADD2 || insn->opcode == IA64_OP_PSUB2 ? 16
                                                                                         : 32;
        bool sub = insn->opcode == IA64_OP_PSUB1 || insn->opcode == IA64_OP_PSUB2 ||
                   insn->opcode == IA64_OP_PSUB4;
        value = packed_addsub(a, b, bits, op->immediate & 3, sub);
        break;
    }
    case IA64_OP_PAVG1:
        value = ia64_simd_pavg_value(op->immediate ? 4 : 0, a, b);
        break;
    case IA64_OP_PAVG2:
        value = ia64_simd_pavg_value(op->immediate ? 5 : 1, a, b);
        break;
    case IA64_OP_PAVGSUB1:
        value = ia64_simd_pavg_value(2, a, b);
        break;
    case IA64_OP_PAVGSUB2:
        value = ia64_simd_pavg_value(3, a, b);
        break;
    case IA64_OP_PCMP1_EQ:
        value = ia64_simd_pcmp_value(0, a, b);
        break;
    case IA64_OP_PCMP1_GT:
        value = ia64_simd_pcmp_value(1, a, b);
        break;
    case IA64_OP_PCMP2_EQ:
        value = ia64_simd_pcmp_value(2, a, b);
        break;
    case IA64_OP_PCMP2_GT:
        value = ia64_simd_pcmp_value(3, a, b);
        break;
    case IA64_OP_PCMP4_EQ:
        value = ia64_simd_pcmp_value(4, a, b);
        break;
    case IA64_OP_PCMP4_GT:
        value = ia64_simd_pcmp_value(5, a, b);
        break;
    case IA64_OP_PMAX1_U:
        value = ia64_simd_pminmax_value(0, a, b);
        break;
    case IA64_OP_PMIN1_U:
        value = ia64_simd_pminmax_value(1, a, b);
        break;
    case IA64_OP_PMAX2:
        value = ia64_simd_pminmax_value(2, a, b);
        break;
    case IA64_OP_PMIN2:
        value = ia64_simd_pminmax_value(3, a, b);
        break;
    case IA64_OP_PMPY2_L:
        value = ia64_simd_pmpy_value(0, a, b, 0);
        break;
    case IA64_OP_PMPY2_R:
        value = ia64_simd_pmpy_value(1, a, b, 0);
        break;
    case IA64_OP_PMPYSH2:
        value = ia64_simd_pmpy_value(2, a, b, op->immediate);
        break;
    case IA64_OP_PMPYSH2_U:
        value = ia64_simd_pmpy_value(3, a, b, op->immediate);
        break;
    case IA64_OP_PSHL2:
    case IA64_OP_PSHL4:
    case IA64_OP_PSHR2:
    case IA64_OP_PSHR2_U:
    case IA64_OP_PSHR4:
    case IA64_OP_PSHR4_U: {
        bool left = insn->opcode == IA64_OP_PSHL2 || insn->opcode == IA64_OP_PSHL4;
        unsigned bits = insn->opcode == IA64_OP_PSHL2 || insn->opcode == IA64_OP_PSHR2 ||
                                insn->opcode == IA64_OP_PSHR2_U
                            ? 16
                            : 32;
        bool logical = left || insn->opcode == IA64_OP_PSHR2_U || insn->opcode == IA64_OP_PSHR4_U;
        uint64_t count = op->immediate >= 0 ? (uint64_t)op->immediate : left ? b : a;
        uint64_t src = left ? a : b;
        if (op->immediate >= 0)
            nat = left ? left_value.nat : right_value.nat;
        if (count >= bits) {
            if (logical)
                break;
            count = bits - 1;
        }
        /* Replicate a shifted lane mask without overlapping partial
         * products. It removes bits that the word shift moves across lanes. */
        uint64_t ones = bits == 16 ? UINT64_C(0x0001000100010001) : UINT64_C(0x0000000100000001);
        uint64_t low = bits == 16 ? UINT16_MAX : UINT32_MAX;
        uint64_t mask = (low >> count) * ones;
        if (left)
            value = (src << count) & (mask << count);
        else {
            value = (src >> count) & mask;
            if (!logical) {
                uint64_t sign = src & (ones << (bits - 1));
                uint64_t negative = (sign - (sign >> (bits - 1))) | sign;
                value |= negative & ~mask;
            }
        }
        break;
    }
    case IA64_OP_PSHLADD2:
        value = ia64_simd_pshladd2_value(a, b, op->immediate);
        break;
    case IA64_OP_PSHRADD2: {
        if ((uint64_t)op->immediate <= 3) {
            const uint64_t high = UINT64_C(0x8000800080008000);
            const uint64_t ones = UINT64_C(0x0001000100010001);
            const uint64_t low = UINT64_C(0x7fff7fff7fff7fff);
            unsigned count = (unsigned)op->immediate;
            uint64_t bias = high >> count;
            uint64_t mask = (bias << 1) - ones;
            /* Bias each signed lane before the logical shift, then restore
             * its signed representation without borrowing between lanes. */
            uint64_t shifted = ((((a ^ high) >> count) & mask) + (high - bias)) ^ high;
            uint64_t different_signs = shifted ^ b;
            uint64_t sum = ((shifted & low) + (b & low)) ^ (different_signs & high);
            uint64_t overflow = ~different_signs & (shifted ^ sum) & high;
            uint64_t replace = (overflow << 1) - (overflow >> 15);
            /* The shift keeps each lane's sign, so a signed overflow of the
             * add saturates to 0x7fff plus the sign bit of a. */
            uint64_t limit = low + ((a & high) >> 15);

            value = sum ^ ((sum ^ limit) & replace);
        } else {
            /* Decoded counts are 1..3; retain other diagnostic calls. */
            for (unsigned pos = 0; pos < 64; pos += 16) {
                int64_t lane = (signed_lane(a, pos, 16) >> op->immediate) + signed_lane(b, pos, 16);
                value |= ((uint64_t)clamp(lane, INT16_MIN, INT16_MAX) & 0xffff) << pos;
            }
        }
        break;
    }
    case IA64_OP_PSAD1:
        value = ia64_simd_psad1_value(a, b);
        break;
    case IA64_OP_MUX1:
    case IA64_OP_MUX2:
        value = ia64_simd_mux_value(insn->opcode == IA64_OP_MUX2, a, op->immediate);
        nat = left_value.nat;
        break;
    case IA64_OP_MIX1_L:
        value = ia64_simd_mix_value(0, a, b);
        break;
    case IA64_OP_MIX1_R:
        value = ia64_simd_mix_value(1, a, b);
        break;
    case IA64_OP_MIX2_L:
        value = ia64_simd_mix_value(2, a, b);
        break;
    case IA64_OP_MIX2_R:
        value = ia64_simd_mix_value(3, a, b);
        break;
    case IA64_OP_MIX4_L:
        value = ia64_simd_mix_value(4, a, b);
        break;
    case IA64_OP_MIX4_R:
        value = ia64_simd_mix_value(5, a, b);
        break;
    case IA64_OP_UNPACK1_H:
        value = ia64_simd_unpack_value(0, a, b);
        break;
    case IA64_OP_UNPACK1_L:
        value = ia64_simd_unpack_value(1, a, b);
        break;
    case IA64_OP_UNPACK2_H:
        value = ia64_simd_unpack_value(2, a, b);
        break;
    case IA64_OP_UNPACK2_L:
        value = ia64_simd_unpack_value(3, a, b);
        break;
    case IA64_OP_UNPACK4_H:
        value = ia64_simd_unpack_value(4, a, b);
        break;
    case IA64_OP_UNPACK4_L:
        value = ia64_simd_unpack_value(5, a, b);
        break;
    case IA64_OP_PACK2_SSS:
        value = ia64_simd_pack_value(0, a, b);
        break;
    case IA64_OP_PACK2_USS:
        value = ia64_simd_pack_value(1, a, b);
        break;
    case IA64_OP_PACK4_SSS:
        value = ia64_simd_pack_value(2, a, b);
        break;
    case IA64_OP_CZX1_L:
        value = ia64_simd_czx_value(0, b);
        nat = right_value.nat;
        break;
    case IA64_OP_CZX1_R:
        value = ia64_simd_czx_value(1, b);
        nat = right_value.nat;
        break;
    case IA64_OP_CZX2_L:
        value = ia64_simd_czx_value(2, b);
        nat = right_value.nat;
        break;
    case IA64_OP_CZX2_R:
        value = ia64_simd_czx_value(3, b);
        nat = right_value.nat;
        break;
    default:
        return ia64_unimplemented(insn);
    }
    ia64_gr_write(s, op->destination, value, nat);
    return ia64_complete();
}
