/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Compare the closed-form RSE cursor of ia64/rse.c against the per-slot loops
 * it replaced, kept here verbatim as the reference: every starting slot
 * (and every low-bit pattern), counts 0..1024, bases that wrap through 0
 * in both directions, and seeded random addresses with large counts.
 */
#include <stdbool.h>
#include "tests/review/review.h"
#include "ia64/rse.h"

uint64_t ia64_rse_test_retreat(uint64_t address, unsigned count); /* ia64/rse.c, hosted only */

static bool reference_collection(uint64_t address)
{
    return ((address >> 3) & 63) == 63;
}
static uint64_t reference_advance(uint64_t address, unsigned count)
{
    if (reference_collection(address))
        address += 8;
    while (count--) {
        address += 8;
        if (reference_collection(address))
            address += 8;
    }
    return address;
}
static uint64_t reference_retreat(uint64_t address, unsigned count)
{
    while (count--) {
        address -= 8;
        if (reference_collection(address))
            address -= 8;
    }
    return address;
}

static void compare(uint64_t address, unsigned count)
{
    uint64_t want = reference_advance(address, count), got = ia64_rse_advance(address, count);
    if (got != want)
        fprintf(stderr, "review: advance(%016llx, %u) = %016llx, reference %016llx\n",
                (unsigned long long)address, count, (unsigned long long)got,
                (unsigned long long)want);
    CHECK(got == want);
    want = reference_retreat(address, count);
    got = ia64_rse_test_retreat(address, count);
    if (got != want)
        fprintf(stderr, "review: retreat(%016llx, %u) = %016llx, reference %016llx\n",
                (unsigned long long)address, count, (unsigned long long)got,
                (unsigned long long)want);
    CHECK(got == want);
}

static void review_rse_cursor(void)
{
    /* 512-byte aligned bases: zero (retreat wraps below it), ordinary
     * backing-store addresses, a region-7 address, and the top of the
     * address space (advance wraps through zero). */
    static const uint64_t bases[] = {
        0,
        UINT64_C(0x100000),
        UINT64_C(0x2000000000000000),
        UINT64_C(0xe000000000fffe00),
        UINT64_C(0xfffffffffffffe00),
        UINT64_C(0xfffffffffffffc00),
    };
    for (unsigned b = 0; b < sizeof(bases) / sizeof(bases[0]); ++b)
        for (unsigned slot = 0; slot < 64; ++slot)
            for (unsigned low = 0; low < 8; low += 7) /* aligned, and bits 2:0 set */
                for (unsigned count = 0; count <= 1024; ++count)
                    compare(bases[b] + slot * 8 + low, count);
    /* Seeded random cursors (any 64-bit address, including unaligned low
     * bits and both wrap directions) with counts up to 70,000 slots. */
    uint64_t x = UINT64_C(0x9507b16aa224c3d9);
    for (unsigned i = 0; i < 20000; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        uint64_t address = x;
        unsigned count = (unsigned)((x >> 40) % 70000);
        compare(address, count);
    }
    /* Both directions are exact inverses on data slots. */
    for (unsigned slot = 0; slot < 63; ++slot)
        for (unsigned count = 0; count <= 4096; count += 17) {
            uint64_t a = UINT64_C(0x400000) + slot * 8;
            CHECK(ia64_rse_test_retreat(ia64_rse_advance(a, count), count) == a);
        }
}
REVIEW_SUITE("rse-cursor", review_rse_cursor)
