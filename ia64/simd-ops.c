/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Ported from the syunnPC/qemu-system-ia64 QEMU target, commit 5f67e29.
 * QEMU dependencies removed for ia64-vmm.
 *
 * Pure IA-64 packed-integer operations.  This file deliberately has no CPU,
 * translator, helper-ABI, or accelerator dependency.
 */

#include "ia64vmm/types.h"
#include "ia64/simd-ops.h"

static uint64_t simd_lane(uint64_t value, int index, int bits)
{
    return (value >> (index * bits)) & (((uint64_t)1 << bits) - 1);
}

static int64_t simd_signed_lane(uint64_t value, int index, int bits)
{
    uint64_t lane = simd_lane(value, index, bits);

    return (int64_t)(lane << (64 - bits)) >> (64 - bits);
}

uint64_t ia64_simd_pavg_value(uint32_t op_sel, uint64_t a, uint64_t b)
{
    bool sub = op_sel == 2 || op_sel == 3;
    bool raz = op_sel == 4 || op_sel == 5;
    bool byte = op_sel == 0 || op_sel == 2 || op_sel == 4;
    uint64_t low = byte ? UINT64_C(0x7f7f7f7f7f7f7f7f) : UINT64_C(0x7fff7fff7fff7fff);
    uint64_t ones = byte ? UINT64_C(0x0101010101010101) : UINT64_C(0x0001000100010001);
    if (sub)
        b = ~b;
    uint64_t different = a ^ b, odd = different & ones;
    /* floor((a+b)/2), lane-wise: common bits plus half the differing
     * bits. Mask away the preceding lane's low bit after the shift.
     * The sum fits in each lane, so no carry crosses a lane boundary. */
    uint64_t mean = (a & b) + ((different >> 1) & low);
    if (sub) {
        /* floor((a-b)/2) modulo a lane equals ceil((a+~b)/2) minus
         * its sign bit. Subtraction of that bit modulo the lane is XOR.
         * Inexact pavgs* results round to odd, not to +infinity. */
        return ((mean + odd) ^ ~low) | (~odd & ones);
    }
    return raz ? mean + odd : mean | odd;
}

uint64_t ia64_simd_pcmp_value(uint32_t op_sel, uint64_t a, uint64_t b)
{
    unsigned bits = op_sel <= 1 ? 8 : op_sel <= 3 ? 16 : 32;
    uint64_t high = bits == 8    ? UINT64_C(0x8080808080808080)
                    : bits == 16 ? UINT64_C(0x8000800080008000)
                                 : UINT64_C(0x8000000080000000);
    uint64_t different = a ^ b, selected;
    if (op_sel & 1) {
        /* Isolate each low-bit subtraction with a forced high bit, so
         * no borrow crosses a lane. Equal signs use that comparison;
         * opposite signs select the nonnegative a and negative b. */
        uint64_t difference = (b | high) - (a & ~high);
        selected = ((b & ~a) | (~different & ~difference)) & high;
    } else {
        /* Exact zero-lane high bits, without neighboring borrow marks. */
        uint64_t low = ~high;
        selected = ~(((different & low) + low) | different | low);
    }
    /* Expand each selected high bit into all ones in that lane. */
    return selected | (selected - (selected >> (bits - 1)));
}

uint64_t ia64_simd_pminmax_value(uint32_t op_sel, uint64_t a, uint64_t b)
{
    unsigned bits = op_sel <= 1 ? 8 : 16;
    bool is_max = op_sel == 0 || op_sel == 2;
    uint64_t high = bits == 8 ? UINT64_C(0x8080808080808080) : UINT64_C(0x8000800080008000);
    uint64_t different = a ^ b;
    /* The forced high bits isolate b-a in each lane. Equal signs use
     * the low-bit comparison; different signs use the requested signed
     * (word) or unsigned (byte) ordering. */
    uint64_t difference = (b | high) - (a & ~high);
    uint64_t sign_greater = bits == 8 ? a & ~b : b & ~a;
    uint64_t greater = (sign_greater | (~different & ~difference)) & high;
    uint64_t mask = greater | (greater - (greater >> (bits - 1)));
    return (is_max ? b : a) ^ (different & mask);
}

static uint64_t simd_pmpy2_result(uint64_t a, uint64_t b, bool right_form)
{
    /* Normalize both inputs once, then use fixed 0/32-bit lane positions. */
    unsigned first_shift = right_form ? 0 : 16;
    a >>= first_shift;
    b >>= first_shift;
    int32_t low = (int16_t)a * (int16_t)b;
    int32_t high = (int16_t)(a >> 32) * (int16_t)(b >> 32);

    return (uint32_t)low | ((uint64_t)(uint32_t)high << 32);
}

static inline uint64_t simd_pmpysh2_lane(uint64_t a, uint64_t b, uint32_t shift, bool signed_form)
{
    /* Keep products 64 bits wide for defined diagnostic shifts 0..63. */
    uint64_t product;

    if (signed_form) {
        int64_t signed_product = (int64_t)(int16_t)a * (int16_t)b;
        product = signed_product >> shift;
    } else {
        product = ((a & UINT16_MAX) * (b & UINT16_MAX)) >> shift;
    }
    return product & UINT16_MAX;
}

uint64_t ia64_simd_pmpy_value(uint32_t op_sel, uint64_t a, uint64_t b, uint32_t shift)
{
    if (op_sel <= 1)
        return simd_pmpy2_result(a, b, op_sel == 1);
    /* Constant lane positions remove loop shifts and repeated sign tests. */
    if (op_sel == 2) {
        return simd_pmpysh2_lane(a, b, shift, true) |
               (simd_pmpysh2_lane(a >> 16, b >> 16, shift, true) << 16) |
               (simd_pmpysh2_lane(a >> 32, b >> 32, shift, true) << 32) |
               (simd_pmpysh2_lane(a >> 48, b >> 48, shift, true) << 48);
    }
    return simd_pmpysh2_lane(a, b, shift, false) |
           (simd_pmpysh2_lane(a >> 16, b >> 16, shift, false) << 16) |
           (simd_pmpysh2_lane(a >> 32, b >> 32, shift, false) << 32) |
           (simd_pmpysh2_lane(a >> 48, b >> 48, shift, false) << 48);
}

uint64_t ia64_simd_psad1_value(uint64_t a, uint64_t b)
{
    const uint64_t high = UINT64_C(0x8080808080808080);
    uint64_t different = a ^ b;
    uint64_t difference = (b | high) - (a & ~high);
    uint64_t greater = ((a & ~b) | (~different & ~difference)) & high;
    uint64_t mask = greater | (greater - (greater >> 7));
    uint64_t max = b ^ (different & mask), min = a ^ (different & mask);
    /* Every max byte is at least its min byte, so the whole-word
     * subtraction has no inter-byte borrow. Sum into 16-bit lanes,
     * then add those lanes; the total is at most 8 * 255 = 2040. */
    uint64_t delta = max - min;
    uint64_t sums =
        (delta & UINT64_C(0x00ff00ff00ff00ff)) + ((delta >> 8) & UINT64_C(0x00ff00ff00ff00ff));
    return (sums * UINT64_C(0x0001000100010001)) >> 48;
}

uint64_t ia64_simd_pshladd2_value(uint64_t a, uint64_t b, uint32_t count)
{
    if (count <= 3) {
        const uint64_t high = UINT64_C(0x8000800080008000);
        const uint64_t ones = UINT64_C(0x0001000100010001);
        const uint64_t low = UINT64_C(0x7fff7fff7fff7fff);
        uint64_t sign = a & high;
        uint64_t sign_bit = sign >> 15;
        uint64_t negative = sign - sign_bit;
        uint64_t upper = high - (high >> count);
        uint64_t different = (a ^ negative) & upper;
        uint64_t shift_overflow = (different + low) & high;
        uint64_t low_bits = (ones << count) - ones;
        uint64_t shifted = (a << count) & ~low_bits;
        uint64_t different_signs = shifted ^ b;
        uint64_t sum = ((shifted & ~high) + (b & ~high)) ^ (different_signs & high);
        uint64_t add_overflow = ~different_signs & (shifted ^ sum) & high;
        uint64_t overflow = shift_overflow | add_overflow;
        uint64_t replace = (overflow << 1) - (overflow >> 15);
        uint64_t limit = low + sign_bit;

        /* An overflowing shift ignores b. Otherwise shifted has a's sign,
         * so either kind of overflow uses the same signed saturation limit.
         * Guard bits keep both the addition and mask expansion lane-local. */
        return sum ^ ((sum ^ limit) & replace);
    }

    uint64_t result = 0;

    /* The decoder produces 1..3; retain the scalar path for diagnostic calls. */
    for (int i = 0; i < 4; i++) {
        int64_t left = simd_signed_lane(a, i, 16);
        int64_t right = simd_signed_lane(b, i, 16);
        int64_t shifted = left * (INT64_C(1) << count);
        int64_t lane;

        if (shifted > INT16_MAX) {
            lane = INT16_MAX;
        } else if (shifted < INT16_MIN) {
            lane = INT16_MIN;
        } else {
            lane = shifted + right;
            lane = lane < INT16_MAX ? lane : INT16_MAX;
            lane = lane > INT16_MIN ? lane : INT16_MIN;
        }
        result |= ((uint64_t)lane & UINT16_MAX) << (i * 16);
    }
    return result;
}

uint64_t ia64_simd_mux_value(uint32_t op_sel, uint64_t value, uint32_t imm)
{
    if (op_sel == 0) {
        uint64_t swap;
        switch (imm & 15) {
        case 8: /* @mix: exchange byte pairs 1/4 and 3/6. */
            swap = ((value >> 24) ^ value) & UINT64_C(0x00000000ff00ff00);
            return value ^ swap ^ (swap << 24);
        case 9: /* @shuf: exchange middle words, then inner bytes. */
            swap = ((value >> 16) ^ value) & UINT64_C(0x00000000ffff0000);
            value ^= swap ^ (swap << 16);
            swap = ((value >> 8) ^ value) & UINT64_C(0x0000ff000000ff00);
            return value ^ swap ^ (swap << 8);
        case 10: /* @alt is the inverse exchange network. */
            swap = ((value >> 8) ^ value) & UINT64_C(0x0000ff000000ff00);
            value ^= swap ^ (swap << 8);
            swap = ((value >> 16) ^ value) & UINT64_C(0x00000000ffff0000);
            return value ^ swap ^ (swap << 16);
        case 11:
            return __builtin_bswap64(value); /* @rev */
        default:
            /* Preserve the old zero-filled table entries for diagnostic
             * selectors as well as the architectural @brcst selector. */
            return (value & UINT8_MAX) * UINT64_C(0x0101010101010101);
        }
    }
    /* Fixed output positions avoid the variable output shifts and loop
     * control of the four-lane form; all 256 selectors remain valid. */
#define MUX2_LANE(n) (((value >> (((imm >> (2 * (n))) & 3) * 16)) & UINT16_MAX) << (16 * (n)))
    return MUX2_LANE(0) | MUX2_LANE(1) | MUX2_LANE(2) | MUX2_LANE(3);
#undef MUX2_LANE
}

uint64_t ia64_simd_mix_value(uint32_t op_sel, uint64_t a, uint64_t b)
{
    int bits;
    bool left;
    uint64_t even;

    switch (op_sel) {
    case 0:
    case 1:
        bits = 8;
        even = UINT64_C(0x00ff00ff00ff00ff);
        left = op_sel == 0;
        break;
    case 2:
    case 3:
        bits = 16;
        even = UINT64_C(0x0000ffff0000ffff);
        left = op_sel == 2;
        break;
    default:
        bits = 32;
        even = UINT64_C(0x00000000ffffffff);
        left = op_sel == 4;
        break;
    }
    /* Place the selected a lanes in odd positions and b lanes in even
     * positions. A whole-word shift moves every selected lane at once. */
    return left ? (a & ~even) | ((b >> bits) & even) : ((a & even) << bits) | (b & even);
}

uint64_t ia64_simd_unpack_value(uint32_t op_sel, uint64_t a, uint64_t b)
{
    int bits;
    bool low;

    switch (op_sel) {
    case 0:
    case 1:
        bits = 8;
        low = op_sel == 1;
        break;
    case 2:
    case 3:
        bits = 16;
        low = op_sel == 3;
        break;
    default:
        bits = 32;
        low = op_sel == 5;
        break;
    }
    a = low ? (uint32_t)a : a >> 32;
    b = low ? (uint32_t)b : b >> 32;
    if (bits == 32)
        return (a << 32) | b;
    /* Spread the selected 32-bit halves into alternate lane positions.
     * Each stage inserts zero gaps without mixing neighboring lanes. */
    a = (a | (a << 16)) & UINT64_C(0x0000ffff0000ffff);
    b = (b | (b << 16)) & UINT64_C(0x0000ffff0000ffff);
    if (bits == 8) {
        a = (a | (a << 8)) & UINT64_C(0x00ff00ff00ff00ff);
        b = (b | (b << 8)) & UINT64_C(0x00ff00ff00ff00ff);
    }
    return (a << bits) | b;
}

static uint64_t simd_pack_compact_bytes(uint64_t value)
{
    value &= UINT64_C(0x00ff00ff00ff00ff);
    value = (value | (value >> 8)) & UINT64_C(0x0000ffff0000ffff);
    return (value | (value >> 16)) & UINT32_MAX;
}

static uint64_t simd_pack_signed_bytes(uint64_t value)
{
    uint64_t sign = value & UINT64_C(0x8000800080008000);
    uint64_t negative = sign - (sign >> 15);
    /* A signed byte fits iff bits 7..14 equal the input sign. Each
     * mismatch field is <= 0x7f80, so adding 0x7fff cannot carry out of
     * its 16-bit lane. Its high bit is set exactly when it is nonzero. */
    uint64_t different = (value ^ negative) & UINT64_C(0x7f807f807f807f80);
    uint64_t overflow = (different + UINT64_C(0x7fff7fff7fff7fff)) & UINT64_C(0x8000800080008000);
    uint64_t replace = (overflow >> 7) - (overflow >> 15);
    uint64_t limit = UINT64_C(0x007f007f007f007f) + (sign >> 15);
    return simd_pack_compact_bytes(value ^ ((value ^ limit) & replace));
}

static uint64_t simd_pack_unsigned_bytes(uint64_t value)
{
    uint64_t sign = value & UINT64_C(0x8000800080008000);
    uint64_t negative = sign - (sign >> 15);
    /* For a nonnegative signed word, adding 0x7f00 sets the high bit
     * exactly above 255, and cannot carry into the next lane. Negative
     * words clear their low 15 bits independently before compaction. */
    uint64_t overflow = ((value & UINT64_C(0x7fff7fff7fff7fff)) + UINT64_C(0x7f007f007f007f00)) &
                        UINT64_C(0x8000800080008000);
    uint64_t replace = (overflow >> 7) - (overflow >> 15);
    return simd_pack_compact_bytes((value | replace) & ~negative);
}

static uint64_t simd_pack_signed_word(uint64_t value)
{
    int32_t lane = (int32_t)value;
    return (uint16_t)(lane < INT16_MIN ? INT16_MIN : lane > INT16_MAX ? INT16_MAX : lane);
}

uint64_t ia64_simd_pack_value(uint32_t op_sel, uint64_t a, uint64_t b)
{
    if (op_sel == 0)
        return simd_pack_signed_bytes(a) | (simd_pack_signed_bytes(b) << 32);
    if (op_sel == 1)
        return simd_pack_unsigned_bytes(a) | (simd_pack_unsigned_bytes(b) << 32);
    /* Four fixed 32-bit input positions avoid the generic loop's
     * variable-width extraction and insertion, including diagnostic
     * selectors which have always used this default form. */
    return simd_pack_signed_word(a) | (simd_pack_signed_word(a >> 32) << 16) |
           (simd_pack_signed_word(b) << 32) | (simd_pack_signed_word(b >> 32) << 48);
}

uint64_t ia64_simd_czx_value(uint32_t op_sel, uint64_t value)
{
    unsigned lane_shift = op_sel <= 1 ? 3 : 4;
    uint64_t low = lane_shift == 3 ? UINT64_C(0x7f7f7f7f7f7f7f7f) : UINT64_C(0x7fff7fff7fff7fff);
    /* Each low lane plus its all-ones low mask fits in that lane. The
     * resulting top bit is set exactly when its low bits were nonzero;
     * OR with the original top bit excludes nonzero high-bit-only lanes.
     * Unlike the subtract-one zero detector, this mask has no adjacent
     * lane false positives, so either scan direction is valid. */
    uint64_t zero = ~(((value & low) + low) | value | low);
    if (!zero)
        return 64 >> lane_shift;
    unsigned count = op_sel == 0 || op_sel == 2 ? (unsigned)__builtin_clzll(zero)
                                                : (unsigned)__builtin_ctzll(zero);
    return count >> lane_shift;
}
