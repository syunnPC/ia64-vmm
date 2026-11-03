/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TIER0_CHECK_BYTES_H
#define TIER0_CHECK_BYTES_H
#ifndef CHECK_BYTES_WORK
#define CHECK_BYTES_WORK(n) ((void)0)
#endif
/* Full-state replay compares tens of KiB at every fetch and region. The
 * freestanding byte-at-a-time memcmp made real firmware exhaust its
 * watchdog, unlike the hosted libc. Compare every byte in integer words;
 * no host SIMD state is touched and short tails never overread. */
typedef uint64_t check_word __attribute__((may_alias, aligned(1)));
static inline bool same_bytes(const void *a, const void *b, size_t bytes)
{
    const uint8_t *x = a, *y = b;
    while (bytes >= 4 * sizeof(check_word)) {
        CHECK_BYTES_WORK(4);
        const check_word *left = (const check_word *)x, *right = (const check_word *)y;
        if ((left[0] ^ right[0]) | (left[1] ^ right[1]) | (left[2] ^ right[2]) |
            (left[3] ^ right[3]))
            return false;
        x += 4 * sizeof(check_word);
        y += 4 * sizeof(check_word);
        bytes -= 4 * sizeof(check_word);
    }
    while (bytes >= sizeof(check_word)) {
        CHECK_BYTES_WORK(1);
        if (*(const check_word *)x != *(const check_word *)y)
            return false;
        x += sizeof(check_word);
        y += sizeof(check_word);
        bytes -= sizeof(check_word);
    }
    while (bytes--) {
        CHECK_BYTES_WORK(1);
        if (*x++ != *y++)
            return false;
    }
    return true;
}

#endif
