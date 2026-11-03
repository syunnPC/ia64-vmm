/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Amd64/Time/Amd64TscClock.cpp
 */
#include "host/amd64/calibration.h"
/* The minimum of three 50 ms measurement windows limits
 * interruption skew. Bounded flat-counter detection handles gated timers. */
static uint64_t measure(const struct amd64_clock_reference *r, uint32_t mask, uint64_t hz)
{
    uint64_t goal = hz / 20;
    if (!goal || goal > mask)
        return 0;
    uint64_t before = r->tsc(r->context);
    uint32_t start = r->counter(r->context) & mask;
    uint64_t after = r->tsc(r->context);
    if (after < before)
        return 0;
    uint64_t stamp = before + (after - before) / 2;
    uint32_t previous = start;
    unsigned unchanged = 0;
    for (unsigned i = 0; i < 1000000; ++i) {
        before = r->tsc(r->context);
        uint32_t current = r->counter(r->context) & mask;
        after = r->tsc(r->context);
        if (after < before)
            return 0;
        unchanged = current == previous ? unchanged + 1 : 0;
        if (unchanged >= 1024)
            return 0;
        previous = current;
        uint32_t elapsed = (current - start) & mask;
        if (elapsed >= goal) {
            uint64_t end = before + (after - before) / 2;
            if (end <= stamp || elapsed > goal * 10)
                return 0;
            return amd64_calibration_hz(end - stamp, hz, elapsed);
        }
    }
    return 0;
}
uint64_t amd64_measure_reference(const struct amd64_clock_reference *r, uint32_t mask, uint64_t hz)
{
    uint64_t best = 0;
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t value = measure(r, mask, hz);
        if (!value)
            return 0;
        if (!best || value < best)
            best = value;
    }
    return best;
}
