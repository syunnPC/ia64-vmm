/* SPDX-License-Identifier: GPL-2.0-or-later */
static size_t compare_work;
#define CHECK_BYTES_WORK(n) (compare_work += (n))
#include "engine/tier0/check-bytes.h"
/* Count actual comparison operands, not seconds. A full state equality
 * takes one operand per eight bytes, with at most seven byte-tail operands.
 * Every byte must still detect a mismatch, including unaligned short tails. */
static void test_check_bytes(void)
{
    static uint8_t left[sizeof(struct ia64_state) + 16], right[sizeof(left)];
    for (unsigned offset = 0; offset < 8; ++offset) {
        size_t bytes = sizeof(struct ia64_state) + offset;
        compare_work = 0;
        CHECK(same_bytes(left + offset, right + offset, bytes));
        CHECK(compare_work == bytes / 8 + bytes % 8);
        CHECK(compare_work <= bytes / 8 + 7);
        for (size_t at = 0; at < bytes; ++at) {
            right[offset + at] = 1;
            CHECK(!same_bytes(left + offset, right + offset, bytes));
            right[offset + at] = 0;
        }
    }
    for (size_t bytes = 0; bytes < 40; ++bytes) {
        compare_work = 0;
        CHECK(same_bytes(left + 1, right + 3, bytes));
        CHECK(compare_work == bytes / 8 + bytes % 8);
    }
}
