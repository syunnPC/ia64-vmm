/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "firmware/new/timing.h"
int main(void)
{
    for (uint64_t ns = 0; ns < 1000000; ++ns) {
        uint64_t ticks = itc_ns_to_ticks(ns);
        assert(ticks * 5 >= ns * 2 && (!ticks || (ticks - 1) * 5 < ns * 2));
        assert(itc_ticks_to_ns(ticks) == ticks * 5 / 2);
    }
    assert(itc_ns_to_ticks(UINT64_MAX) == UINT64_MAX / 5 * 2);
    assert(itc_ticks_to_ns(UINT64_MAX) == UINT64_MAX);
    assert(itc_ticks_to_ns(UINT64_MAX / 5 * 2) == UINT64_MAX);
    assert(timer_effective_period(0) == 0 && timer_effective_period(1) == 10000);
    assert(timer_effective_period(9999) == 10000 && timer_effective_period(10001) == 10001);
    /* Unsigned elapsed arithmetic remains valid across ITC wrap. */
    uint64_t start = UINT64_MAX - 10, end = 9;
    assert(end - start == 20 && (int64_t)((start + 40) - end) > 0);
    puts("timer arithmetic: rounding, saturation, period floor, wrap passed");
    return 0;
}
