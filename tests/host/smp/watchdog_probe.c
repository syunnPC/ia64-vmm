/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Link only into watchdog regressions. A passing case must renew its
 * initial alarm; the stalled variant stops at a renewal. */
static unsigned arms, minimum = 2;
unsigned __real_alarm(unsigned seconds);
static void finished(void)
{
    if (__atomic_load_n(&arms, __ATOMIC_RELAXED) < minimum) {
        fputs("watchdog probe: no progress renewal\n", stderr);
        _exit(1);
    }
}
__attribute__((constructor)) static void probe_start(void)
{
    const char *value = getenv("SMP_WATCHDOG_PROBE_MIN_ARMS");
    if (value)
        minimum = (unsigned)atoi(value);
    if (atexit(finished))
        _exit(1);
}
unsigned __wrap_alarm(unsigned seconds)
{
    if (!seconds)
        return __real_alarm(0);
    unsigned n = __atomic_add_fetch(&arms, 1, __ATOMIC_RELAXED);
    if (getenv("SMP_WATCHDOG_PROBE_STALL")) {
        unsigned left = __real_alarm(1);
        if (n == minimum)
            for (;;)
                pause();
        return left;
    }
    return __real_alarm(seconds);
}
