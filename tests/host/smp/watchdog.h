/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SMP_TEST_WATCHDOG_H
#define SMP_TEST_WATCHDOG_H

#define TEST_WATCHDOG_CHECK CHECK
#include "tests/common/watchdog.h"
#undef TEST_WATCHDOG_CHECK

#define watchdog_round test_watchdog_counts[0]
#define watchdog_mode test_watchdog_counts[1]
static inline void watchdog_progress(void)
{
    test_watchdog_progress();
}
static inline void watchdog_start(const char *test, unsigned seconds)
{
    test_watchdog_start("SMP watchdog: test=", test, seconds, "round", "mode");
}

#endif
