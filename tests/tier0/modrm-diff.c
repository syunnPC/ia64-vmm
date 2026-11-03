/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compare actual baseline/current encoders, including partial writes and
 * measuring passes. The baseline object's exported symbols are renamed. */
#include "engine/tier0/x86emit.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DECLARE(name) __typeof__(name) name##_baseline
DECLARE(x86_mov_imm);
DECLARE(x86_mov);
DECLARE(x86_bt_reg);
DECLARE(x86_alu);
DECLARE(x86_imm);
DECLARE(x86_unary);
DECLARE(x86_shift);
DECLARE(x86_bt);
DECLARE(x86_shiftx);
DECLARE(x86_andn);
DECLARE(x86_bextr);
DECLARE(x86_lzcnt);
DECLARE(x86_rorx);
DECLARE(x86_shrd);
DECLARE(x86_setcc);
DECLARE(x86_load);
DECLARE(x86_store);
DECLARE(x86_load_signed);
DECLARE(x86_add_memory);
DECLARE(x86_cmp_load);
DECLARE(x86_sub_load);
DECLARE(x86_add_load);
DECLARE(x86_test_byte);
DECLARE(x86_cmp_byte);
#undef DECLARE

static const uint64_t values[] = {0,
                                  1,
                                  2,
                                  127,
                                  128,
                                  255,
                                  256,
                                  UINT32_MAX,
                                  UINT64_MAX,
                                  UINT64_MAX - 127,
                                  UINT64_MAX - 128,
                                  INT32_MAX,
                                  UINT64_C(0xffffffff80000000),
                                  UINT64_C(0x123456789abcdef0),
                                  UINT64_C(1) << 32,
                                  UINT64_C(1) << 63};
static const unsigned counts[] = {0,   1,   2,   3,   31,         32, 63, 64,
                                  127, 128, 255, 256, UINT32_MAX, 5,  7,  13};

static void emit(bool baseline, struct x86_buffer *out, unsigned kind, unsigned a, unsigned b,
                 unsigned p)
{
    const unsigned operations[] = {0x01, 0x09, 0x21, 0x29, 0x31, 0x39, 0x85};
    const unsigned groups[] = {4, 5, 7, 0};
    uint64_t imm = values[p];
    int32_t disp = (int32_t)imm;
#define CALL(name, ...)                                                                            \
    do {                                                                                           \
        if (baseline)                                                                              \
            name##_baseline(out, __VA_ARGS__);                                                     \
        else                                                                                       \
            name(out, __VA_ARGS__);                                                                \
    } while (0)
    switch (kind) {
    case 0:
        CALL(x86_mov_imm, a, imm);
        break;
    case 1:
        CALL(x86_mov, a, b);
        break;
    case 2:
        CALL(x86_bt_reg, a, b);
        break;
    case 3:
        CALL(x86_alu, operations[p % 7], a, b, p >= 8);
        break;
    case 4:
        CALL(x86_imm, b & 7, a, disp);
        break;
    case 5:
        CALL(x86_unary, b & 7, a);
        break;
    case 6:
        CALL(x86_shift, b & 7, a, counts[p], b >= 8);
        break;
    case 7:
        CALL(x86_bt, b & 7, a, counts[p]);
        break;
    case 8:
        CALL(x86_shiftx, groups[b % 4], a, b, p);
        break;
    case 9:
        CALL(x86_andn, a, p, b);
        break;
    case 10:
        CALL(x86_bextr, a, b, p);
        break;
    case 11:
        CALL(x86_lzcnt, a, b);
        break;
    case 12:
        CALL(x86_rorx, a, b, counts[p]);
        break;
    case 13:
        CALL(x86_shrd, a, b, counts[p]);
        break;
    case 14:
        CALL(x86_setcc, p, a);
        break;
    /* Unchanged memory forms also exercise RSP/RBP, high base registers,
     * displacement boundaries, byte prefixes and the generic helper. */
    case 15:
        CALL(x86_load, a, b, disp, 1u << (p % 4));
        break;
    case 16:
        CALL(x86_store, b, disp, a, 1u << (p % 4));
        break;
    case 17:
        CALL(x86_load_signed, a, b, disp, 1u << (p % 3));
        break;
    case 18:
        CALL(x86_add_memory, b, disp, (int8_t)imm);
        break;
    case 19:
        CALL(x86_cmp_load, a, b, disp);
        break;
    case 20:
        CALL(x86_sub_load, a, b, disp);
        break;
    case 21:
        CALL(x86_add_load, a, b, disp);
        break;
    case 22:
        CALL(x86_test_byte, b, disp, (uint8_t)imm);
        break;
    case 23:
        CALL(x86_cmp_byte, b, disp, (uint8_t)imm);
        break;
    default:
        abort();
    }
#undef CALL
}

static uint64_t checks;
static void compare(unsigned kind, unsigned a, unsigned b, unsigned p)
{
    for (size_t capacity = 0; capacity <= 17; ++capacity)
        for (unsigned start = 0; start < 5; ++start)
            for (unsigned measuring = 0; measuring < 2; ++measuring)
                for (unsigned failed = 0; failed < 2; ++failed) {
                    uint8_t bytes[2][64];
                    memset(bytes, 0xa5, sizeof(bytes));
                    size_t initial = start == 0   ? 0
                                     : start == 1 ? 1
                                     : start == 2 ? 7
                                     : start == 3 ? capacity + 1
                                                  : SIZE_MAX;
                    struct x86_buffer out[2];
                    for (unsigned variant = 0; variant < 2; ++variant) {
                        out[variant] =
                            (struct x86_buffer){.data = measuring ? NULL : bytes[variant] + 8,
                                                .size = initial,
                                                .capacity = capacity,
                                                .failed = failed};
                        emit(variant == 0, &out[variant], kind, a, b, p);
                    }
                    ++checks;
                    if (out[0].size != out[1].size || out[0].failed != out[1].failed ||
                        out[0].capacity != out[1].capacity ||
                        memcmp(bytes[0], bytes[1], sizeof(bytes[0]))) {
                        fprintf(
                            stderr,
                            "kind=%u a=%u b=%u p=%u capacity=%zu initial=%zu measure=%u failed=%u\n",
                            kind, a, b, p, capacity, initial, measuring, failed);
                        abort();
                    }
                }
}

int main(void)
{
    for (unsigned kind = 0; kind < 24; ++kind)
        for (unsigned a = 0; a < 16; ++a)
            for (unsigned b = 0; b < 16; ++b) {
                unsigned parameters = kind == 1 || kind == 2 || kind == 5 || kind == 11 ? 1 : 16;
                for (unsigned p = 0; p < parameters; ++p)
                    compare(kind, a, b, p);
            }
    /* VEX invalid-register handling can fail after writing a prefix.
     * Preserve its exact flag, size and partial-byte behavior too. */
    for (unsigned kind = 8; kind <= 12; ++kind)
        if (kind != 11)
            for (unsigned p = 0; p < 16; ++p) {
                compare(kind, 16, 0, p);
                compare(kind, 0, 31, p);
            }
    printf("{\"comparisons\":%" PRIu64 ",\"result\":\"pass\"}\n", checks);
    return 0;
}
