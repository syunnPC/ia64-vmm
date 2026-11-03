/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Integer arithmetic ported from the syunnPC/qemu-system-ia64 target/ia64/arch/fp.c.
 * QEMU and SoftFloat dependencies removed. SDM Vol.1 section 5.4.8:
 * retain the exact product until after addition, including cancellation. */
#include "ia64/fp.h"

static unsigned clz64(uint64_t v)
{
    return v ? (unsigned)__builtin_clzll(v) : 64;
}
typedef struct {
    bool sign;
    int32_t exp;
    uint64_t mant;
    bool unnormal;
} FPFormat;
typedef struct {
    __uint128_t high, low;
} IA64FPWide;

static bool ia64_fp_static_format_operand(const struct ia64_fr *raw, FPFormat *value)
{
    uint32_t normalize;

    if (raw->exponent == IA64_FP_SPECIAL_EXP) {
        return false;
    }
    value->sign = raw->sign;
    if (raw->significand == 0) {
        value->exp = 0;
        value->mant = 0;
        value->unnormal = raw->exponent != 0;
        return true;
    }
    value->exp = raw->exponent == 0 ? 0x0c001 : (int32_t)raw->exponent;
    value->mant = raw->significand;
    normalize = clz64(value->mant);
    value->mant <<= normalize;
    value->exp -= normalize;
    value->unnormal = normalize != 0;
    return true;
}

static void ia64_fp_raw_set_fraction(struct ia64_fp_raw *raw, uint64_t fraction, bool tail)
{
    const uint64_t half = 1ULL << 63;

    raw->tail = fraction != 0 || tail;
    raw->half_greater = fraction > half || (fraction == half && tail);
    raw->half_equal = fraction == half && !tail;
}

static bool ia64_fp_wide_is_zero(const IA64FPWide *value)
{
    return value->high == 0 && value->low == 0;
}

static int ia64_fp_wide_compare(const IA64FPWide *left, const IA64FPWide *right)
{
    if (left->high != right->high) {
        return left->high > right->high ? 1 : -1;
    }
    if (left->low != right->low) {
        return left->low > right->low ? 1 : -1;
    }
    return 0;
}

static IA64FPWide ia64_fp_wide_shift_right(IA64FPWide value, uint32_t shift, bool *discarded)
{
    IA64FPWide result = {0};
    __uint128_t mask;
    uint32_t high_shift;

    if (shift == 0) {
        *discarded = false;
        return value;
    }
    if (shift < 128) {
        mask = (((__uint128_t)1 << shift) - 1);
        *discarded = (value.low & mask) != 0;
        result.high = value.high >> shift;
        result.low = (value.high << (128 - shift)) | (value.low >> shift);
        return result;
    }
    if (shift == 128) {
        *discarded = value.low != 0;
        result.low = value.high;
        return result;
    }
    if (shift < 256) {
        high_shift = shift - 128;
        mask = (((__uint128_t)1 << high_shift) - 1);
        *discarded = value.low != 0 || (value.high & mask) != 0;
        result.low = value.high >> high_shift;
        return result;
    }
    *discarded = !ia64_fp_wide_is_zero(&value);
    return result;
}

static IA64FPWide ia64_fp_wide_add(IA64FPWide left, IA64FPWide right, bool *carry)
{
    IA64FPWide result;
    __uint128_t high_sum;
    bool low_carry;

    result.low = left.low + right.low;
    low_carry = result.low < left.low;
    high_sum = left.high + right.high;
    *carry = high_sum < left.high;
    result.high = high_sum + low_carry;
    *carry |= result.high < high_sum;
    return result;
}

static IA64FPWide ia64_fp_wide_sub(IA64FPWide left, IA64FPWide right)
{
    IA64FPWide result;
    bool borrow = left.low < right.low;

    result.low = left.low - right.low;
    result.high = left.high - right.high - borrow;
    return result;
}

static void ia64_fp_wide_decrement(IA64FPWide *value)
{
    if (value->low-- == 0) {
        value->high--;
    }
}

static uint32_t ia64_fp_wide_clz(const IA64FPWide *value)
{
    uint64_t word;

    word = value->high >> 64;
    if (word != 0) {
        return clz64(word);
    }
    word = value->high;
    if (word != 0) {
        return 64 + clz64(word);
    }
    word = value->low >> 64;
    if (word != 0) {
        return 128 + clz64(word);
    }
    return 192 + clz64((uint64_t)value->low);
}

static IA64FPWide ia64_fp_wide_shift_left(IA64FPWide value, uint32_t shift)
{
    IA64FPWide result = {0};

    if (shift == 0) {
        return value;
    }
    if (shift < 128) {
        result.high = (value.high << shift) | (value.low >> (128 - shift));
        result.low = value.low << shift;
    } else if (shift == 128) {
        result.high = value.low;
    } else if (shift < 256) {
        result.high = value.low << (shift - 128);
    }
    return result;
}

static void ia64_fp_wide_to_raw(IA64FPWide value, int32_t exp, bool sign, bool fractional_tail,
                                struct ia64_fp_raw *raw)
{
    uint32_t normalize = ia64_fp_wide_clz(&value);

    value = ia64_fp_wide_shift_left(value, normalize);
    raw->sign = sign;
    raw->exp = exp - normalize;
    raw->mant = value.high >> 64;
    ia64_fp_raw_set_fraction(raw, (uint64_t)value.high, value.low != 0 || fractional_tail);
}

bool ia64_fp_muladd(const struct ia64_fr *addend_raw, const struct ia64_fr *left_raw,
                    const struct ia64_fr *right_raw, unsigned op, unsigned rc,
                    struct ia64_fp_raw *raw)
{
    FPFormat addend;
    FPFormat left;
    FPFormat right;
    IA64FPWide product = {0};
    IA64FPWide addend_value = {0};
    IA64FPWide product_aligned;
    IA64FPWide addend_aligned;
    IA64FPWide result;
    __uint128_t exact_product;
    int32_t product_exp;
    int32_t result_exp;
    int magnitude_compare;
    bool product_sign;
    bool addend_sign;
    bool product_discarded;
    bool addend_discarded;
    bool fractional_tail;
    bool carry;

    if (!ia64_fp_static_format_operand(left_raw, &left) ||
        !ia64_fp_static_format_operand(right_raw, &right) ||
        !ia64_fp_static_format_operand(addend_raw, &addend)) {
        return false;
    }

    exact_product = (__uint128_t)left.mant * right.mant;
    product_sign = left.sign ^ right.sign ^ (op == 2);
    addend_sign = addend.sign ^ (op == 1);
    if (exact_product == 0) {
        if (addend.mant != 0) {
            raw->sign = addend_sign;
            raw->exp = addend.exp;
            raw->mant = addend.mant;
        } else {
            raw->sign = product_sign == addend_sign ? product_sign : rc == 1;
        }
        return true;
    }
    product_exp = left.exp + right.exp - IA64_FP_BIAS;
    if (exact_product >> 127) {
        product.high = exact_product;
        product_exp++;
    } else {
        product.high = exact_product << 1;
    }

    if (addend.mant == 0) {
        ia64_fp_wide_to_raw(product, product_exp, product_sign, false, raw);
        return true;
    }
    addend_value.high = (__uint128_t)addend.mant << 64;
    result_exp = (product_exp > addend.exp ? product_exp : addend.exp);
    product_aligned =
        ia64_fp_wide_shift_right(product, result_exp - product_exp, &product_discarded);
    addend_aligned =
        ia64_fp_wide_shift_right(addend_value, result_exp - addend.exp, &addend_discarded);

    if (product_sign == addend_sign) {
        result = ia64_fp_wide_add(product_aligned, addend_aligned, &carry);
        fractional_tail = product_discarded || addend_discarded;
        if (carry) {
            raw->sign = product_sign;
            raw->exp = result_exp + 1;
            raw->mant = IA64_FP_INTEGER_BIT | (uint64_t)(result.high >> 65);
            ia64_fp_raw_set_fraction(raw, result.high >> 1,
                                     (result.high & 1) != 0 || result.low != 0 || fractional_tail);
        } else {
            ia64_fp_wide_to_raw(result, result_exp, product_sign, fractional_tail, raw);
        }
        return true;
    }

    magnitude_compare = product_exp != addend.exp ? (product_exp > addend.exp ? 1 : -1)
                                                  : ia64_fp_wide_compare(&product, &addend_value);
    if (magnitude_compare == 0) {
        raw->sign = rc == 1;
        return true;
    }
    if (magnitude_compare > 0) {
        result = ia64_fp_wide_sub(product_aligned, addend_aligned);
        if (addend_discarded) {
            ia64_fp_wide_decrement(&result);
        }
        raw->sign = product_sign;
    } else {
        result = ia64_fp_wide_sub(addend_aligned, product_aligned);
        if (product_discarded) {
            ia64_fp_wide_decrement(&result);
        }
        raw->sign = addend_sign;
    }
    fractional_tail = product_discarded || addend_discarded;
    ia64_fp_wide_to_raw(result, result_exp, raw->sign, fractional_tail, raw);
    return true;
}
