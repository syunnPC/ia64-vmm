/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef T1H_WATCHDOG_H
#define T1H_WATCHDOG_H

#include <stdio.h>
#include "tests/tier1/harness.h"

static void t1h_watchdog_details(void);
#define TEST_WATCHDOG_CHECK T1H_CHECK
#define TEST_WATCHDOG_DETAILS t1h_watchdog_details
#include "tests/common/watchdog.h"
#undef TEST_WATCHDOG_DETAILS
#undef TEST_WATCHDOG_CHECK

#ifndef T1H_WATCHDOG_SECONDS
#define T1H_WATCHDOG_SECONDS 60u
#endif

static char t1h_watchdog_label[192];
static volatile sig_atomic_t t1h_not_acted, t1h_outside, t1h_undelivered;
static volatile sig_atomic_t t1h_target, t1h_before, t1h_after;

static void t1h_watchdog_details(void)
{
#define DETAIL(name, value)                                                                        \
    do {                                                                                           \
        test_watchdog_write(" " name "=", sizeof(" " name "=") - 1);                               \
        test_watchdog_number(value);                                                               \
    } while (0)
    DETAIL("not-acted", t1h_not_acted);
    DETAIL("outside-entry", t1h_outside);
    DETAIL("raised-undelivered", t1h_undelivered);
    DETAIL("target", t1h_target);
    DETAIL("before", t1h_before);
    DETAIL("after", t1h_after);
#undef DETAIL
}

static inline void t1h_watchdog_start(const char *test, const char *engine)
{
    test_watchdog_stop();
    int n = snprintf(t1h_watchdog_label, sizeof(t1h_watchdog_label), "%s engine=%s", test, engine);
    T1H_CHECK(n >= 0 && (size_t)n < sizeof(t1h_watchdog_label));
    t1h_not_acted = t1h_outside = t1h_undelivered = 0;
    t1h_target = t1h_before = t1h_after = 0;
    test_watchdog_start("Tier1 watchdog: test=", t1h_watchdog_label, T1H_WATCHDOG_SECONDS, "trials",
                        "exercised");
    test_watchdog_counts[0] = test_watchdog_counts[1] = 0;
}

static inline void t1h_watchdog_trial(void)
{
    test_watchdog_increment(&test_watchdog_counts[0]);
}

static inline sig_atomic_t t1h_watchdog_count(uint64_t value)
{
    return value > SIG_ATOMIC_MAX ? SIG_ATOMIC_MAX : (sig_atomic_t)value;
}

static inline void t1h_watchdog_source(bool acted, bool during, uint64_t target, uint64_t before,
                                       uint64_t after)
{
    t1h_target = t1h_watchdog_count(target);
    t1h_before = t1h_watchdog_count(before);
    t1h_after = t1h_watchdog_count(after);
    if (!acted)
        test_watchdog_increment(&t1h_not_acted);
    else if (!during)
        test_watchdog_increment(&t1h_outside);
}

static inline void t1h_watchdog_undelivered(void)
{
    test_watchdog_increment(&t1h_undelivered);
}
static inline void t1h_watchdog_progress(void)
{
    test_watchdog_increment(&test_watchdog_counts[1]);
    test_watchdog_progress();
}
static inline void t1h_watchdog_stop(void)
{
    test_watchdog_stop();
}

#endif
