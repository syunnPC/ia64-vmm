/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Ported from the syunnPC/qemu-system-ia64 target/ia64/arch/fp.c, without QEMU/SoftFloat.
 * SDM Vol.1 Figure 5-12: bounded underflow re-rounds the exact value. */
#include "ia64/fp.h"

__uint128_t ia64_fp_round_right(const struct ia64_fp_raw *raw, uint32_t shift, uint32_t rounding,
                                bool *inexact, bool *fpa)
{
    __uint128_t upper;
    uint64_t discarded = 0;
    uint64_t half = 0;
    bool round_up = false;

    if (shift == 0) {
        upper = raw->mant;
        *inexact = raw->tail;
        if (rounding == 0 && raw->tail) {
            round_up = raw->half_greater || (raw->half_equal && (raw->mant & 1));
        }
    } else if (shift < 64) {
        upper = raw->mant >> shift;
        discarded = raw->mant & ((1ULL << shift) - 1);
        half = 1ULL << (shift - 1);
        *inexact = discarded != 0 || raw->tail;
        if (rounding == 0 && *inexact) {
            round_up =
                discarded > half || (discarded == half && (raw->tail || ((uint64_t)upper & 1)));
        }
    } else if (shift == 64) {
        upper = 0;
        discarded = raw->mant;
        half = 1ULL << 63;
        *inexact = discarded != 0 || raw->tail;
        if (rounding == 0 && *inexact) {
            round_up = discarded > half || (discarded == half && raw->tail);
        }
    } else {
        upper = 0;
        *inexact = raw->mant != 0 || raw->tail;
    }

    if (*inexact && rounding != 0) {
        round_up = (rounding == 1 && raw->sign) || (rounding == 2 && !raw->sign);
    }
    if (round_up) {
        upper++;
    }
    *fpa = round_up;
    return upper;
}

bool ia64_fp_round(const struct ia64_fp_raw *input, unsigned controls, unsigned masks,
                   unsigned precision, struct ia64_fr *out, unsigned *flags, bool *rounded_up)
{
    struct ia64_fp_raw raw = *input;
    uint32_t precision_bits;
    uint32_t precision_shift;
    uint32_t rounding = (controls >> 4) & 3;
    int32_t min_exp;
    int32_t max_exp;
    int32_t result_exp;
    __uint128_t rounded;
    uint64_t result_mant;
    int soft = 0;
    bool inexact;
    bool fpa;
    bool masked;

    if (raw.mant == 0) {
        *out = (struct ia64_fr){.sign = raw.sign};
        return true;
    }

    if (precision == 1) {
        precision_bits = 24;
        if (controls & 2) {
            min_exp = 0x00001;
            max_exp = 0x1fffe;
        } else {
            min_exp = 0x0ff81;
            max_exp = 0x1007e;
        }
    } else if (precision == 2) {
        precision_bits = 53;
        if (controls & 2) {
            min_exp = 0x00001;
            max_exp = 0x1fffe;
        } else {
            min_exp = 0x0fc01;
            max_exp = 0x103fe;
        }
    } else if (precision == 0) {
        switch ((controls >> 2) & 3) {
        case 0:
            precision_bits = 24;
            break;
        case 2:
            precision_bits = 53;
            break;
        case 3:
            precision_bits = 64;
            break;
        default:
            return false;
        }
        if (controls & 2) {
            min_exp = 0x00001;
            max_exp = 0x1fffe;
        } else {
            min_exp = 0x0c001;
            max_exp = 0x13ffe;
        }
    } else {
        return false;
    }
    precision_shift = 64 - precision_bits;

    /* First round with unbounded exponent range, as required by Figure 5-12. */
    rounded = ia64_fp_round_right(&raw, precision_shift, rounding, &inexact, &fpa);
    result_exp = raw.exp;
    if (rounded == ((__uint128_t)1 << precision_bits)) {
        rounded >>= 1;
        result_exp++;
    }
    result_mant = (uint64_t)rounded << precision_shift;

    if (result_exp > max_exp) {
        masked = (masks & FP_O) != 0;
        soft = FP_O;
        if (!masked) {
            /* Enabled overflow returns the unbounded exponent modulo 2^17. */
            if (inexact) {
                soft |= FP_I;
            }
        } else {
            /* The bounded overflow response is always inexact. */
            soft |= FP_I;
            if (rounding == 0 || (rounding == 1 && raw.sign) || (rounding == 2 && !raw.sign)) {
                result_exp = IA64_FP_SPECIAL_EXP;
                result_mant = IA64_FP_INTEGER_BIT;
                fpa = true;
            } else {
                result_exp = max_exp;
                result_mant = UINT64_MAX << precision_shift;
                fpa = false;
            }
        }
    } else if (result_exp < min_exp) {
        masked = (masks & FP_U) != 0;
        if (!masked) {
            /* Enabled underflow returns the unbounded exponent modulo 2^17. */
            soft = FP_U;
            if (inexact) {
                soft |= FP_I;
            }
        } else if (controls & 1) {
            /* FTZ applies even when the tiny result would otherwise be exact. */
            result_exp = 0;
            result_mant = 0;
            soft = FP_U | FP_I;
            fpa = false;
        } else {
            uint32_t exponent_shift = (uint32_t)(min_exp - raw.exp);
            uint32_t denormal_shift = exponent_shift > UINT32_MAX - precision_shift
                                          ? UINT32_MAX
                                          : precision_shift + exponent_shift;

            /* Re-round the exact input once for the bounded exponent range. */
            rounded = ia64_fp_round_right(&raw, denormal_shift, rounding, &inexact, &fpa);
            if (rounded == ((__uint128_t)1 << (precision_bits - 1))) {
                result_mant = IA64_FP_INTEGER_BIT;
                result_exp = min_exp;
            } else {
                result_mant = (uint64_t)rounded << precision_shift;
                result_exp = result_mant == 0 ? 0 : min_exp;
                /* Double-extended denormals have the exponent-zero island. */
                if (precision == 0 && !(controls & 2))
                    result_exp = 0;
            }
            if (inexact) {
                soft = FP_U | FP_I;
            }
        }
    } else if (inexact) {
        soft = FP_I;
    }

    *out = (struct ia64_fr){
        .sign = raw.sign, .exponent = (uint32_t)result_exp & 0x1ffff, .significand = result_mant};
    *rounded_up = fpa;
    *flags |= soft;
    return true;
}
