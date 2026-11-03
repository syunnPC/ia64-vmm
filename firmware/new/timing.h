/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_TIMING_H
#define VMM_TIMING_H
#include <stdint.h>
/* The handoff's fixed 400 MHz ITC: round delays up, elapsed time down. */
static inline uint64_t itc_ns_to_ticks(uint64_t ns)
{
    return ns / 5 * 2 + (ns % 5 * 2 + 4) / 5;
}
static inline uint64_t itc_ticks_to_ns(uint64_t ticks)
{
    uint64_t fraction = ticks % 2 * 5 / 2;
    return ticks / 2 > (UINT64_MAX - fraction) / 5 ? UINT64_MAX : ticks / 2 * 5 + fraction;
}
static inline uint64_t timer_effective_period(uint64_t requested)
{
    return requested && requested < 10000 ? 10000 : requested;
}
#endif
