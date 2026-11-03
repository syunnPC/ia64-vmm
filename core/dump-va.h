/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_DUMP_VA_H
#define VMM_DUMP_VA_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* One dump_va range. Advance past the whole token before parsing so
 * failures can be skipped. */
static inline bool vmm_dump_va_range(const char *text, size_t length, size_t *cursor,
                                     uint64_t *address, uint64_t *bytes, const char **error)
{
    size_t i = *cursor, end = i;
    while (end < length && text[end] != ',')
        ++end;
    *cursor = end < length ? end + 1 : end;
    length = end;
    uint64_t va = 0, size = 0;
    unsigned digits = 0;
    for (; i < length && text[i] != ':'; ++i, ++digits) {
        char c = text[i];
        unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                     : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                                            : 16;
        if (d == 16 || digits == 16) {
            *error = "bad address";
            return false;
        }
        va = va << 4 | d;
    }
    if (i == length || !digits) {
        *error = "expected <hex va>:<bytes>";
        return false;
    }
    ++i;
    for (digits = 0; i < length && text[i] != ','; ++i, ++digits) {
        if (text[i] < '0' || text[i] > '9' ||
            (size = size * 10 + (unsigned)(text[i] - '0')) > 65536) {
            *error = "bad length (decimal, at most 65536)";
            return false;
        }
    }
    if (!digits) {
        *error = "missing length";
        return false;
    }
    if (size > UINT64_MAX - va) {
        *error = "range wraps";
        return false;
    }
    *address = va;
    *bytes = size;
    return true;
}
#endif
