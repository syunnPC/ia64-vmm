/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared oracle/ABBA harness for scalar IA-64 SIMD helpers. The reference
 * symbols come from a separately compiled and renamed baseline object. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ia64/simd-ops.h"

#define DECLARE(name) extern uint64_t baseline_ia64_simd_##name(uint32_t, uint64_t, uint64_t)
DECLARE(pavg_value);
DECLARE(pcmp_value);
DECLARE(pminmax_value);
DECLARE(mix_value);
DECLARE(unpack_value);
DECLARE(pack_value);
extern uint64_t baseline_ia64_simd_czx_value(uint32_t, uint64_t);
extern uint64_t baseline_ia64_simd_mux_value(uint32_t, uint64_t, uint32_t);
extern uint64_t baseline_ia64_simd_psad1_value(uint64_t, uint64_t);
#undef DECLARE

enum operation { CZX, PAVG, PCMP, MINMAX, MIX, UNPACK, PACK, MUX, PSAD };
static const char *const names[] = {"czx",    "pavg", "pcmp", "minmax", "mix",
                                    "unpack", "pack", "mux",  "psad"};
static const unsigned selectors[] = {4, 6, 6, 4, 6, 6, 3, 272, 1};
static uint64_t random_state = UINT64_C(0x739caecd38f16a27);
static uint64_t random_word(void)
{
    uint64_t x = random_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return random_state = x;
}
static uint64_t call(enum operation op, unsigned baseline, unsigned sel, uint64_t a, uint64_t b)
{
#define CALL(name) (baseline ? baseline_ia64_simd_##name(sel, a, b) : ia64_simd_##name(sel, a, b))
    switch (op) {
    case PAVG:
        return CALL(pavg_value);
    case PCMP:
        return CALL(pcmp_value);
    case MINMAX:
        return CALL(pminmax_value);
    case MIX:
        return CALL(mix_value);
    case UNPACK:
        return CALL(unpack_value);
    case PACK:
        return CALL(pack_value);
    case CZX:
        return baseline ? baseline_ia64_simd_czx_value(sel, a) : ia64_simd_czx_value(sel, a);
    case MUX:
        return baseline ? baseline_ia64_simd_mux_value(sel >= 16, a, sel < 16 ? sel : sel - 16)
                        : ia64_simd_mux_value(sel >= 16, a, sel < 16 ? sel : sel - 16);
    case PSAD:
        return baseline ? baseline_ia64_simd_psad1_value(a, b) : ia64_simd_psad1_value(a, b);
    }
    abort();
#undef CALL
}
static uint64_t checks;
static void check(enum operation op, unsigned sel, uint64_t a, uint64_t b)
{
    uint64_t expected = call(op, 1, sel, a, b), actual = call(op, 0, sel, a, b);
    if (actual != expected) {
        fprintf(stderr, "%s sel=%u a=%016llx b=%016llx expected=%016llx actual=%016llx\n",
                names[op], sel, (unsigned long long)a, (unsigned long long)b,
                (unsigned long long)expected, (unsigned long long)actual);
        abort();
    }
    ++checks;
}
static uint64_t nanoseconds(clockid_t clock)
{
    struct timespec now;
    assert(clock_gettime(clock, &now) == 0);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static bool timed_selector(enum operation op, unsigned s)
{
    if (op != MUX || s < 16)
        return true;
    /* Check all 256 word permutations, but time representative broadcasts,
     * identity/reversal, repeated pairs and mixed selectors. */
    static const unsigned imms[] = {0x00, 0x55, 0xaa, 0xff, 0xe4, 0x1b, 0x44, 0xee,
                                    0x05, 0x50, 0xa5, 0x5a, 0x39, 0x93, 0x4e, 0xb1};
    for (unsigned i = 0; i < sizeof(imms) / sizeof(*imms); ++i)
        if (s - 16 == imms[i])
            return true;
    return false;
}
int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3)
        return 2;
    unsigned which;
    for (which = 0; which < sizeof(names) / sizeof(*names); ++which)
        if (!strcmp(argv[1], names[which]))
            break;
    if (which == sizeof(names) / sizeof(*names))
        return 2;
    enum operation op = (enum operation)which;
    unsigned pairs = argc == 3 ? (unsigned)strtoul(argv[2], NULL, 10) : 9;
    if (pairs > 100)
        return 2;
    /* All byte operand pairs, with different adjacent lanes so carries or
     * false zero bits cannot hide behind an identical-lane broadcast. */
    for (unsigned a = 0; a < 256; ++a)
        for (unsigned b = 0; b < 256; ++b)
            for (unsigned s = 0; s < selectors[op]; ++s) {
                uint64_t av =
                    (UINT64_C(0x0100010001000100) * a) | (UINT64_C(0x0001000100010001) * b);
                uint64_t bv =
                    (UINT64_C(0x0001000100010001) * a) | (UINT64_C(0x0100010001000100) * (255 - b));
                check(op, s, av, bv);
            }
    /* Every 16-bit lane value, moved through all four lane positions. */
    for (unsigned x = 0; x < 65536; ++x)
        for (unsigned position = 0; position < 4; ++position)
            for (unsigned s = 0; s < selectors[op]; ++s) {
                unsigned shift = position * 16;
                uint64_t av = (UINT64_C(0x000100008000ffff) & ~(UINT64_C(0xffff) << shift)) |
                              ((uint64_t)x << shift);
                check(op, s, av, ~av);
            }
    for (unsigned x = 0; x < 100000; ++x) {
        uint64_t a = random_word(), b = random_word();
        for (unsigned s = 0; s < selectors[op]; ++s)
            check(op, s, a, b);
    }
    printf("oracle-checks=%llu\n", (unsigned long long)checks);
    enum { INPUTS = 1024, CALLS = 1024 * 1024 };
    uint64_t inputs[4][INPUTS][2];
    static const char *const patterns[] = {"zero", "nonzero", "mixed", "random"};
    for (unsigned i = 0; i < INPUTS; ++i) {
        inputs[0][i][0] = 0;
        inputs[0][i][1] = 0;
        inputs[1][i][0] = random_word() | UINT64_C(0x0101010101010101);
        inputs[1][i][1] = random_word() | UINT64_C(0x0101010101010101);
        inputs[2][i][0] =
            (random_word() | UINT64_C(0x0101010101010101)) & ~(UINT64_C(0xffff) << ((i & 3) * 16));
        inputs[2][i][1] = random_word();
        inputs[3][i][0] = random_word();
        inputs[3][i][1] = random_word();
    }
    uint64_t expected[4][272] = {{0}};
    for (unsigned p = 0; p < 4; ++p)
        for (unsigned s = 0; s < selectors[op]; ++s) {
            for (unsigned i = 0; i < INPUTS; ++i)
                expected[p][s] += call(op, 1, s, inputs[p][i][0], inputs[p][i][1]);
            expected[p][s] *= CALLS / INPUTS;
        }
    for (unsigned pair = 0; pair < pairs; ++pair)
        for (unsigned segment = 0; segment < 4; ++segment) {
            unsigned candidate = (pair & 1) ^ (segment == 1 || segment == 2);
            for (unsigned s = 0; s < selectors[op]; ++s) {
                if (!timed_selector(op, s))
                    continue;
                for (unsigned p = 0; p < 4; ++p) {
                    uint64_t sum = 0, cpu = nanoseconds(CLOCK_THREAD_CPUTIME_ID),
                             wall = nanoseconds(CLOCK_MONOTONIC);
                    for (unsigned i = 0; i < CALLS; ++i)
                        sum += call(op, !candidate, s, inputs[p][i & (INPUTS - 1)][0],
                                    inputs[p][i & (INPUTS - 1)][1]);
                    wall = nanoseconds(CLOCK_MONOTONIC) - wall;
                    cpu = nanoseconds(CLOCK_THREAD_CPUTIME_ID) - cpu;
                    assert(sum == expected[p][s]);
                    printf("row %u %u %u %u %s %u %llu %llu %llu\n", pair, segment, candidate, s,
                           patterns[p], CALLS, (unsigned long long)cpu, (unsigned long long)wall,
                           (unsigned long long)sum);
                }
            }
        }
    return 0;
}
