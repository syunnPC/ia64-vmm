/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/fp.h"

/* Both lanes compute into a private context. A fault in either lane must
 * preserve the entire guest destination and FPSR (SDM Vol.1 section 5.4.1).
 * Sharing scalar helpers also keeps NaN selection and rounding identical. */
struct ia64_result ia64_fp_parallel(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64FloatingOperands *op = &insn->operands.floating;
    bool four = insn->opcode == IA64_OP_FPMA || insn->opcode == IA64_OP_FPMS ||
                insn->opcode == IA64_OP_FPNMA;
    bool sqrt = insn->opcode == IA64_OP_FPRSQRTA;
    bool unary = sqrt || insn->opcode == IA64_OP_FPABS || insn->opcode == IA64_OP_FPNEG ||
                 insn->opcode == IA64_OP_FPNEGABS || insn->opcode == IA64_OP_FPCVT;
    bool approx = sqrt || insn->opcode == IA64_OP_FPRCPA;
    if (op->destination < 2)
        return ia64_fault(IA64_FAULT_ILLEGAL, op->destination);
    struct ia64_result result =
        ia64_fp_disabled(s, op->destination, sqrt ? 0 : op->source1,
                         unary && !sqrt ? 0 : op->source2, four ? op->auxiliary1 : 0);
    if (result.kind != IA64_STEP_COMPLETE)
        return result;
    struct ia64_fr a = ia64_fr_read(s, op->source1), b = ia64_fr_read(s, op->source2),
                   c = ia64_fr_read(s, op->auxiliary1);
    if ((sqrt ? ia64_fr_is_nat(b) : ia64_fr_is_nat(a)) || (!unary && ia64_fr_is_nat(b)) ||
        (four && ia64_fr_is_nat(c))) {
        ia64_fr_write(s, op->destination, ia64_fp_nat());
        if (approx)
            ia64_pr_write(s, op->auxiliary2, false);
        return ia64_complete();
    }
    uint64_t data = 0;
    switch (insn->opcode) {
    case IA64_OP_FPABS:
        data = a.significand & UINT64_C(0x7fffffff7fffffff);
        break;
    case IA64_OP_FPNEG:
        data = a.significand ^ UINT64_C(0x8000000080000000);
        break;
    case IA64_OP_FPNEGABS:
        data = a.significand | UINT64_C(0x8000000080000000);
        break;
    case IA64_OP_FPMERGE:
    case IA64_OP_FPMERGE_S:
    case IA64_OP_FPMERGE_SE: {
        uint64_t mask = insn->opcode == IA64_OP_FPMERGE_SE ? UINT64_C(0xff800000ff800000)
                                                           : UINT64_C(0x8000000080000000);
        data = (a.significand & mask) | (b.significand & ~mask);
        if (insn->opcode == IA64_OP_FPMERGE)
            data ^= mask;
        break;
    }
    default:
        goto arithmetic;
    }
    ia64_fr_write(s, op->destination, (struct ia64_fr){data, IA64_FP_INTEGER_EXP, false});
    return ia64_complete();

arithmetic:;
    unsigned sf = op->status_field & 3, shift = 6 + 13 * sf;
    unsigned ctl = (s->ar[AR_FPSR] >> shift) & 0x7f;
    unsigned masks = sf && (ctl & 64) ? 63 : s->ar[AR_FPSR] & 63;
    unsigned lane_flags[2] = {0}, trap_codes[2] = {0};
    bool predicates[2] = {false}, fpa[2] = {false};
    if (approx) {
        /* SDM permits software assistance for difficult inputs. Keep packed
         * denormal approximation responses explicit until their limit-check
         * cases have independent reference coverage. NaTVal was handled above. */
        uint64_t operands[] = {sqrt ? b.significand : a.significand, b.significand};
        unsigned swa = 0;
        for (unsigned k = 0; k < (sqrt ? 1u : 2u); ++k)
            for (unsigned l = 0; l < 2; ++l) {
                uint32_t bits = operands[k] >> (32 * l);
                if ((bits & 0x7fffffff) && !(bits & 0x7f800000))
                    swa |= l ? 8 : 128;
                /* Rev.2.3 Vol.3 fprcpa: dangerous refinement limits need
                 * p2=0. Vol.1 §5.4.1.2 permits a precise SWA instead; let
                 * FPSWA compute the quotient for |den| >= 2^126. */
                unsigned exponent = bits & 0x7f800000;
                if (!sqrt && k == 1 && exponent >= 0x7e800000 && exponent < 0x7f800000)
                    swa |= l ? 8 : 128;
            }
        if (swa)
            return ia64_fault(IA64_FAULT_FP, swa);
    }
    struct ia64_fp_context lane = {0};
    for (unsigned part = 0; part < 2; ++part) {
        unsigned bits = part * 32;
        lane.a = ia64_fp_from_ieee((uint32_t)(a.significand >> bits), true);
        lane.b = ia64_fp_from_ieee((uint32_t)(b.significand >> bits), true);
        lane.c = ia64_fp_from_ieee((uint32_t)(c.significand >> bits), true);
        /* Parallel instructions ignore PC/WRE. Run immediate exceptions
         * masked, then arbitrate both lanes together before guest effects. */
        lane.fpsr = (masks | 7) | ((uint64_t)(ctl & ~2u) << shift);
        Ia64Instruction scalar = {.valid = true,
                                  .unit = IA64_UNIT_F,
                                  .operands.floating = {.destination = 6,
                                                        .source1 = four && !op->source1 ? 0 : 7,
                                                        .source2 = 8,
                                                        .auxiliary1 = 9,
                                                        .auxiliary2 = 11,
                                                        .precision = 1,
                                                        .status_field = sf,
                                                        .immediate = op->immediate}};
        bool cmp = false, cvt = false;
        switch (insn->opcode) {
        case IA64_OP_FPMA:
            scalar.opcode = IA64_OP_FMA;
            break;
        case IA64_OP_FPMS:
            scalar.opcode = IA64_OP_FMS;
            break;
        case IA64_OP_FPNMA:
            scalar.opcode = IA64_OP_FNMA;
            break;
        case IA64_OP_FPMIN:
            scalar.opcode = IA64_OP_FMIN;
            break;
        case IA64_OP_FPMAX:
            scalar.opcode = IA64_OP_FMAX;
            break;
        case IA64_OP_FPAMIN:
            scalar.opcode = IA64_OP_FAMIN;
            break;
        case IA64_OP_FPAMAX:
            scalar.opcode = IA64_OP_FAMAX;
            break;
        case IA64_OP_FPRCPA:
            scalar.opcode = IA64_OP_FRCPA;
            break;
        case IA64_OP_FPRSQRTA:
            scalar.opcode = IA64_OP_FRSQRTA;
            break;
        case IA64_OP_FPCMP:
            cmp = true;
            scalar.opcode = IA64_OP_FCMP;
            scalar.operands.floating.auxiliary1 = 10;
            scalar.operands.floating.immediate &= 3;
            break;
        case IA64_OP_FPCVT:
            cvt = true;
            scalar.opcode = op->immediate & 1 ? IA64_OP_FCVT_FXU : IA64_OP_FCVT_FX;
            break;
        default:
            return ia64_unimplemented(insn);
        }
        result = ia64_fp_compute(&lane, &scalar);
        if (result.kind == IA64_STEP_EXIT ||
            (result.kind == IA64_STEP_FAULT && result.fault != IA64_FAULT_FP_TRAP))
            return result;
        lane_flags[part] = (lane.fpsr >> (shift + 7)) & 63;
        if (result.fault == IA64_FAULT_FP_TRAP) {
            trap_codes[part] = (result.info >> 8) & 0x38;
            fpa[part] = (result.info >> 14) & 1;
        }
        uint32_t value;
        if (cmp)
            value = lane.pr1 ^ ((op->immediate & 4) != 0) ? UINT32_MAX : 0;
        else if (cvt) {
            uint64_t integer = lane.out.significand;
            bool invalid = op->immediate & 1 ? integer > UINT32_MAX
                                             : integer + UINT64_C(0x80000000) > UINT32_MAX;
            if (invalid || (lane_flags[part] & FP_V)) {
                lane_flags[part] = FP_V;
                trap_codes[part] = 0;
                fpa[part] = false;
                value = 0x80000000;
            } else
                value = integer;
        } else {
            struct ia64_fr converted = lane.out;
            if (approx && converted.significand && converted.exponent != IA64_FP_SPECIAL_EXP) {
                struct ia64_fp_raw raw = {.sign = converted.sign,
                                          .exp = converted.exponent,
                                          .mant = converted.significand};
                unsigned ignored = 0;
                bool rounded = false;
                ia64_fp_round(&raw, 0, 63, 1, &converted, &ignored, &rounded);
            }
            if (trap_codes[part] & (FP_O | FP_U)) {
                /* SDM Rev.2.3 Vol.1 §§5.4.2–3: trap-enabled lanes use the
                 * true single biased exponent modulo 2^8. Removing the
                 * scalar register bias commutes with its modulo 2^17. */
                unsigned exponent = (converted.exponent - IA64_FP_BIAS + 127) & 255;
                value = ((uint32_t)converted.sign << 31) | (exponent << 23) |
                        ((converted.significand >> 40) & 0x7fffff);
            } else
                value = ia64_fp_to_ieee(converted, true);
        }
        if (sqrt && !lane.b.significand)
            value = ((uint32_t)lane.b.sign << 31) | 0x7f800000;
        else if (sqrt && lane.b.exponent == IA64_FP_SPECIAL_EXP &&
                 lane.b.significand == IA64_FP_INTEGER_BIT && !lane.b.sign)
            value = 0;
        if (approx) {
            predicates[part] = lane.pr2;
            struct ia64_fr num = sqrt ? lane.b : lane.a;
            unsigned normalize = num.significand ? __builtin_clzll(num.significand) : 0;
            int ne = (int)num.exponent - (int)normalize;
            if (ne <= IA64_FP_BIAS - 127 + 24)
                predicates[part] = false;
            if (!sqrt) {
                struct ia64_fr den = lane.b;
                int de =
                    (int)den.exponent - (den.significand ? __builtin_clzll(den.significand) : 0);
                int diff = ne - de;
                if (diff <= 2 - 127 || diff >= 127)
                    predicates[part] = false;
            }
        }
        data |= (uint64_t)value << bits;
    }
    unsigned lo_fault = lane_flags[0] & ~masks & 7, hi_fault = lane_flags[1] & ~masks & 7;
    if (lo_fault || hi_fault)
        return ia64_fault(IA64_FAULT_FP, hi_fault | (lo_fault << 4));
    s->ar[AR_FPSR] |= (uint64_t)(lane_flags[0] | lane_flags[1]) << (shift + 7);
    ia64_fr_write(s, op->destination, (struct ia64_fr){data, IA64_FP_INTEGER_EXP, false});
    if (approx)
        ia64_pr_write(s, op->auxiliary2, predicates[0] && predicates[1]);
    if (trap_codes[0] || trap_codes[1]) {
        uint64_t code = ((uint64_t)trap_codes[0] << 4) | ((uint64_t)trap_codes[1] << 8) |
                        (fpa[0] ? 1u << 10 : 0) | (fpa[1] ? 1u << 14 : 0);
        return ia64_fault(IA64_FAULT_FP_TRAP, code | 1);
    }
    return ia64_complete();
}
