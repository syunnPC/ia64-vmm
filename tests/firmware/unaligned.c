/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "firmware/new/unaligned.h"
static uint64_t instruction(unsigned major, unsigned x6, unsigned target, unsigned base,
                            int increment)
{
    uint64_t raw = ((uint64_t)major << 37) | ((uint64_t)x6 << 30) | ((uint64_t)base << 20);
    bool store = x6 >= 0x30;
    raw |= (uint64_t)target << (store ? 13 : 6);
    if (major == 5 || major == 7) {
        unsigned imm = (unsigned)increment & 511;
        raw |= (uint64_t)(imm & 127) << (store ? 6 : 13);
        raw |= (uint64_t)((imm >> 7) & 1) << 27;
        raw |= (uint64_t)((imm >> 8) & 1) << 36;
    }
    return raw;
}
int main(void)
{
    uint8_t memory[32];
    struct unaligned_frame f;
    for (unsigned size = 1; size < 4; ++size) {
        unsigned width = 1u << size;
        for (unsigned slot = 0; slot < 3; ++slot) {
            memset(&f, 0, sizeof(f));
            memset(memory, 0xcc, sizeof(memory));
            f.ipsr = (uint64_t)slot << 41;
            f.ifa = 0x1001;
            f.iip = 0x2000;
            f.gr[16] = 0x1001;
            f.gr[8] = 0x8877665544332211ULL;
            assert(unaligned_emulate(&f, instruction(5, 0x30 + size, 8, 16, -3), memory + 1));
            assert(memory[0] == 0xcc && memory[width + 1] == 0xcc && f.gr[16] == 0xffe);
            for (unsigned i = 0; i < width; ++i)
                assert(memory[1 + i] == (uint8_t)(f.gr[8] >> (8 * i)));
            assert(f.iip == (slot == 2 ? 0x2010 : 0x2000));
            assert(((f.ipsr >> 41) & 3) == (slot == 2 ? 0 : slot + 1));
            f.ipsr = 0;
            f.iip = 0x2000;
            f.gr[16] = 0x1001;
            f.gr[9] = 0;
            f.nat = 1u << 9;
            assert(unaligned_emulate(&f, instruction(5, size, 9, 16, 5), memory + 1));
            uint64_t mask = width == 8 ? UINT64_MAX : (UINT64_C(1) << (width * 8)) - 1;
            assert(f.gr[9] == (0x8877665544332211ULL & mask) && !(f.nat & (1u << 9)) &&
                   f.gr[16] == 0x1006);
        }
    }
    memset(&f, 0, sizeof(f));
    f.ifa = 0x1001;
    f.gr[16] = 0x1001;
    f.ipsr = 8;
    struct unaligned_frame before = f;
    assert(!unaligned_emulate(&f, instruction(4, 3, 8, 16, 0), memory));
    assert(memcmp(&f, &before, sizeof(f)) == 0);
    f.ipsr = 0;
    assert(!unaligned_emulate(&f, instruction(4, 3, 40, 16, 0), memory));
    assert(unaligned_emulate(&f, instruction(4, 7, 8, 16, 0), memory)); /* Physical ld8.s defers. */
    assert(f.nat & (1u << 8));
    assert(!unaligned_emulate(&f, instruction(4, 0x33, 8, 16, 0) | (1ULL << 36), memory));
    f.nat = 1u << 8;
    assert(!unaligned_emulate(&f, instruction(4, 0x33, 8, 16, 0), memory));
    /* Unit placement comes from the entire template table, not a range. */
    for (unsigned t = 0; t < 32; ++t)
        for (unsigned slot = 0; slot < 3; ++slot) {
            bool valid =
                t != 6 && t != 7 && t != 20 && t != 21 && t != 26 && t != 27 && t != 30 && t != 31;
            bool expected = valid && ((slot == 0 && t != 22 && t != 23) ||
                                      (slot == 1 && (t == 8 || t == 9 || t == 10 || t == 11 ||
                                                     t == 14 || t == 15 || t == 24 || t == 25)));
            assert(unaligned_m_slot(t, slot) == expected);
        }
    /* Integer selectors: normal, speculative, advanced, acquire, bias,
     * check loads and release stores. Distinct source/base/increment GRs. */
    for (unsigned x6 = 1; x6 < 64; ++x6) {
        bool load = x6 <= 0x17 || (x6 >= 0x20 && x6 <= 0x2b);
        bool store = x6 >= 0x30 && x6 <= 0x37;
        if ((!load && !store) || !(x6 & 3))
            continue;
        for (unsigned reg = 8; reg < 127; reg += 17)
            for (unsigned major = 4; major <= 5; ++major) {
                memset(&f, 0, sizeof(f));
                memset(memory, 0xa5, sizeof(memory));
                f.cfm = 96;
                f.ifa = 0x1001;
                f.gr[127] = 0x1001;
                f.gr[reg] = 0x1234567887654321ULL;
                unsigned width = 1u << (x6 & 3);
                uint64_t raw = instruction(major, x6, reg, 127, 3);
                uint8_t original[sizeof(memory)];
                memcpy(original, memory, sizeof(memory));
                /* Test generator's store classification includes release stores. */
                assert(unaligned_emulate(&f, raw, memory + 1));
                assert(memory[0] == 0xa5 && memory[width + 1] == 0xa5);
                uint64_t expected = 0;
                for (unsigned i = 0; i < width; ++i)
                    expected |= (uint64_t)memory[1 + i] << (8 * i);
                if (load) {
                    assert(memcmp(memory, original, sizeof(memory)) == 0);
                    bool deferred = x6 >= 4 && x6 <= 15 && (x6 & 4);
                    assert(f.gr[reg] == (deferred ? 0 : expected));
                    assert(((reg < 64 ? f.nat : f.nat_hi) >> (reg & 63) & 1) == deferred);
                } else
                    assert(expected == (0x1234567887654321ULL &
                                        (width == 8 ? UINT64_MAX : (1ULL << (width * 8)) - 1)));
                assert(f.gr[127] == (major == 5 ? 0x1004 : 0x1001));
            }
    }
    /* Independent SDM representation vectors: +1, signed zero, smallest
     * denormal, infinity and quiet NaN. The expected 82-bit spill images are
     * constants, so a symmetric load/store conversion bug cannot pass. */
    const struct {
        unsigned kind;
        uint64_t bits[2], spill[2];
    } fp_vectors[] = {
        {0, {0x8000000000000000ULL, 0x3fff}, {0x8000000000000000ULL, 0xffff}},
        {0, {0, 0x8000}, {0, 0x20000}},
        {0, {1, 0}, {1, 0}},
        {0, {0x8000000000000000ULL, 0x7fff}, {0x8000000000000000ULL, 0x1ffff}},
        {0, {0xc000000000000000ULL, 0x7fff}, {0xc000000000000000ULL, 0x1ffff}},
        {1, {0x0123456789abcdefULL, 0}, {0x0123456789abcdefULL, 0x1003e}},
        {1, {0x8000000000000000ULL, 0}, {0x8000000000000000ULL, 0x1003e}},
        {2, {0x3f800000, 0}, {0x8000000000000000ULL, 0xffff}},
        {2, {0x80000000, 0}, {0, 0x20000}},
        {2, {1, 0}, {0x10000000000ULL, 0xff81}},
        {2, {0x7f800000, 0}, {0x8000000000000000ULL, 0x1ffff}},
        {2, {0x7fc00000, 0}, {0xc000000000000000ULL, 0x1ffff}},
        {3, {0x3ff0000000000000ULL, 0}, {0x8000000000000000ULL, 0xffff}},
        {3, {0x8000000000000000ULL, 0}, {0, 0x20000}},
        {3, {1, 0}, {0x800, 0xfc01}},
        {3, {0x7ff0000000000000ULL, 0}, {0x8000000000000000ULL, 0x1ffff}},
        {3, {0x7ff8000000000000ULL, 0}, {0xc000000000000000ULL, 0x1ffff}},
    };
    for (unsigned v = 0; v < sizeof(fp_vectors) / sizeof(fp_vectors[0]); ++v)
        for (unsigned reg = 2; reg < 128; reg += 25)
            for (unsigned major = 6; major <= 7; ++major) {
                unsigned kind = fp_vectors[v].kind, width = kind == 0 ? 10 : kind == 2 ? 4 : 8;
                memset(&f, 0, sizeof(f));
                memset(memory, 0xcc, sizeof(memory));
                f.ifa = 0x1001;
                f.gr[16] = 0x1001;
                for (unsigned i = 0; i < width; ++i)
                    memory[1 + i] = (uint8_t)(fp_vectors[v].bits[i / 8] >> (8 * (i % 8)));
                uint8_t original[sizeof(memory)];
                memcpy(original, memory, sizeof(memory));
                assert(unaligned_emulate(&f, instruction(major, kind, reg, 16, -3), memory + 1));
                assert(memcmp(memory, original, sizeof(memory)) == 0);
                assert(f.fp[reg][0] == fp_vectors[v].spill[0] &&
                       f.fp[reg][1] == fp_vectors[v].spill[1]);
                assert(f.gr[16] == (major == 7 ? 0xffe : 0x1001));
                assert(f.ipsr & (1ULL << (reg < 32 ? 4 : 5)));
                f.ifa = 0x1001;
                f.gr[16] = 0x1001;
                assert(unaligned_emulate(&f, instruction(major, 0x30 + kind, reg, 16, 255),
                                         memory + 1));
                for (unsigned i = 0; i < width; ++i)
                    assert(memory[1 + i] == (uint8_t)(fp_vectors[v].bits[i / 8] >> (8 * (i % 8))));
                assert(f.gr[16] == (major == 7 ? 0x1100 : 0x1001));
                assert(memory[0] == 0xcc && memory[width + 1] == 0xcc);
            }
    /* Every FP selector, including speculative/advanced/check and spill/fill,
     * register and signed-immediate updates, in both register banks. */
    for (unsigned x6 = 0; x6 < 64; ++x6) {
        bool load = x6 <= 15 || (x6 >= 0x20 && x6 <= 0x27) || x6 == 0x1b;
        bool store = (x6 >= 0x30 && x6 <= 0x33) || x6 == 0x3b;
        if (!load && !store)
            continue;
        for (unsigned reg = 8; reg <= 104; reg += 96)
            for (unsigned update = 0; update < 3; ++update) {
                unsigned kind = x6 & 3, width = x6 == 0x1b || x6 == 0x3b ? 16
                                                : kind == 0              ? 10
                                                : kind == 2              ? 4
                                                                         : 8;
                memset(&f, 0, sizeof(f));
                memset(memory, 0, sizeof(memory));
                f.ifa = 0x1001;
                f.gr[16] = 0x1001;
                f.gr[17] = 7;
                f.fp[reg][0] = 0x8000000000000000ULL;
                f.fp[reg][1] = 0xffff;
                if (kind == 0) {
                    memory[8] = 0x80;
                    memory[9] = 0xff;
                    memory[10] = 0x3f;
                } else if (kind == 2) {
                    memory[3] = 0x80;
                    memory[4] = 0x3f;
                } else if (kind == 3 && width != 16) {
                    memory[7] = 0xf0;
                    memory[8] = 0x3f;
                } else {
                    memory[8] = 0x80;
                    if (width == 16) {
                        memory[9] = 0xff;
                        memory[10] = 0xff;
                    }
                }
                memory[0] = 0xa5;
                memory[width + 1] = 0xa5;
                uint64_t raw = instruction(update == 2 ? 7 : 6, x6, reg, 16, -256);
                if (update == 1 && load)
                    raw |= (1ULL << 36) | (17ULL << 13);
                assert(unaligned_emulate(&f, raw, memory + 1));
                bool deferred = x6 >= 4 && x6 <= 15 && (x6 & 4);
                if (load) {
                    assert(f.fp[reg][0] == (deferred ? 0 : 0x8000000000000000ULL));
                    assert(f.fp[reg][1] == (deferred ? 0x1fffe : kind == 1 ? 0x1003e : 0xffff));
                }
                assert(f.gr[16] == (update == 2 ? 0xf01 : update == 1 && load ? 0x1008 : 0x1001));
                assert(memory[0] == 0xa5 && memory[width + 1] == 0xa5);
            }
    }
    /* Integer fill/spill preserves the corresponding ar.unat bit. */
    memset(&f, 0, sizeof(f));
    memset(memory, 0xa5, sizeof(memory));
    f.ifa = 0x1001;
    f.gr[16] = 0x1001;
    f.gr[8] = 123;
    f.nat = 1ULL << 8;
    assert(unaligned_emulate(&f, instruction(5, 0x3b, 8, 16, 0), memory + 1));
    assert(f.unat & 1);
    assert(memory[1] == 0 && memory[8] == 0);
    assert(unaligned_emulate(&f, instruction(5, 0x1b, 9, 16, 0), memory + 1));
    assert(f.nat & (1ULL << 9));
    /* Register update propagates NaT without consuming the loaded result. */
    memset(&f, 0, sizeof(f));
    f.ifa = 0x1001;
    f.gr[16] = 0x1001;
    f.gr[17] = 7;
    f.nat = 1ULL << 17;
    assert(unaligned_emulate(&f, instruction(4, 3, 8, 16, 0) | (1ULL << 36) | (17ULL << 13),
                             memory + 1));
    assert(f.gr[16] == 0x1008 && (f.nat & (1ULL << 16)) && !(f.nat & (1ULL << 8)));
    /* Semaphore encodings, disabled FP and NaTVal consumption are rejected
     * without changing any register or memory. */
    memset(&f, 0, sizeof(f));
    f.ifa = 0x1001;
    f.gr[16] = 0x1001;
    before = f;
    assert(!unaligned_emulate(&f, instruction(4, 3, 8, 16, 0) | (1ULL << 27), memory + 1));
    assert(memcmp(&f, &before, sizeof(f)) == 0);
    f.ipsr = 1ULL << 18;
    assert(!unaligned_emulate(&f, instruction(6, 3, 8, 16, 0), memory + 1));
    f.ipsr = 0;
    f.fp[8][1] = 0x1fffe;
    assert(!unaligned_emulate(&f, instruction(6, 0x33, 8, 16, 0), memory + 1));
    memset(&f, 0, sizeof(f));
    f.ifa = 0x1001;
    f.gr[16] = 0x1001;
    f.isr = 1ULL << 43;
    assert(unaligned_emulate(&f, instruction(4, 7, 8, 16, 0), NULL));
    assert((f.ipsr & (1ULL << 43)) && f.iip == 0); /* ED retry, no instruction advance. */
    /* PSR.ac=1 (the SAL loader hand-off): ld2.s/ld8.s still defer, ld2 faults. */
    memset(&f, 0, sizeof(f));
    f.ifa = 0x1001;
    f.gr[16] = 0x1001;
    f.ipsr = 8;
    assert(unaligned_emulate(&f, instruction(4, 5, 8, 16, 0), NULL));
    assert((f.nat & (1u << 8)) && !f.gr[8]);
    memset(&f, 0, sizeof(f));
    f.ifa = 0x1001;
    f.gr[16] = 0x1001;
    f.ipsr = 8;
    f.isr = 1ULL << 43;
    assert(unaligned_emulate(&f, instruction(4, 7, 8, 16, 0), NULL));
    assert((f.ipsr & (1ULL << 43)) && f.iip == 0);
    memset(&f, 0, sizeof(f));
    f.ifa = 0x1001;
    f.gr[16] = 0x1001;
    f.ipsr = 8;
    before = f;
    assert(!unaligned_emulate(&f, instruction(4, 1, 8, 16, 0), memory + 1));
    assert(memcmp(&f, &before, sizeof(f)) == 0);
    puts(
        "unaligned: integer/FP forms, reference bit patterns, updates, NaT, templates and guards passed");
    return 0;
}
