/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_AMD64_UDIV128_H
#define IA64VMM_AMD64_UDIV128_H
#include <stdbool.h>
#include <stdint.h>

/* Divide high:low by divisor without compiler runtime helpers. Return false
 * for a zero divisor or a quotient exceeding 64 bits. */
static inline bool amd64_udiv128(uint64_t high, uint64_t low, uint64_t divisor, uint64_t *quotient,
                                 uint64_t *remainder)
{
    if (high >= divisor)
        return false;
    uint64_t q, r;
    __asm__("divq %[divisor]"
            : "=a"(q), "=d"(r)
            : "a"(low), "d"(high), [divisor] "rm"(divisor)
            : "cc");
    *quotient = q;
    *remainder = r;
    return true;
}
#endif
