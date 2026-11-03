/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_TIME_MATH_H
#define VMM_AMD64_TIME_MATH_H
#include "ia64vmm/types.h"
#include "host/amd64/udiv128.h"
/* Freestanding 128/64 scaling, saturating when the result cannot fit.
 * GCC's __udivti3 is not available in the bare-metal link. */
static inline uint64_t amd64_scale_ticks(uint64_t value, uint64_t rate, uint64_t divisor, bool ceil)
{
    __uint128_t product = (__uint128_t)value * rate;
    uint64_t result, remainder;
    if (!amd64_udiv128(product >> 64, (uint64_t)product, divisor, &result, &remainder))
        return UINT64_MAX;
    return ceil && remainder && result != UINT64_MAX ? result + 1 : result;
}
#endif
