/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TESTS_COMMON_WATCHDOG_H
#define TESTS_COMMON_WATCHDOG_H

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* Define TEST_WATCHDOG_CHECK and optionally TEST_WATCHDOG_DETAILS before
 * including. Labels and counter names must outlive the armed watchdog. */
static unsigned test_watchdog_seconds;
static const char *test_watchdog_prefix, *test_watchdog_label;
static size_t test_watchdog_prefix_len, test_watchdog_label_len;
static const char *test_watchdog_names[2];
static size_t test_watchdog_name_lens[2];
static volatile sig_atomic_t test_watchdog_counts[2];

static inline void test_watchdog_write(const char *s, size_t n)
{
    ssize_t written = write(STDERR_FILENO, s, n);
    (void)written;
}

static inline void test_watchdog_number(sig_atomic_t value)
{
    char digits[sizeof(sig_atomic_t) * 3];
    unsigned n = 0, v = (unsigned)value;
    do {
        digits[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    for (unsigned i = 0; i < n / 2; ++i) {
        char c = digits[i];
        digits[i] = digits[n - 1 - i];
        digits[n - 1 - i] = c;
    }
    test_watchdog_write(digits, n);
}

static inline void test_watchdog_increment(volatile sig_atomic_t *count)
{
    if (*count < SIG_ATOMIC_MAX)
        ++*count;
}

static void test_watchdog_expired(int signal)
{
    (void)signal;
    test_watchdog_write(test_watchdog_prefix, test_watchdog_prefix_len);
    test_watchdog_write(test_watchdog_label, test_watchdog_label_len);
    for (unsigned i = 0; i < 2; ++i) {
        if (!test_watchdog_names[i] || test_watchdog_counts[i] < 0)
            continue;
        test_watchdog_write(" ", 1);
        test_watchdog_write(test_watchdog_names[i], test_watchdog_name_lens[i]);
        test_watchdog_write("=", 1);
        test_watchdog_number(test_watchdog_counts[i]);
    }
    test_watchdog_write(" (no progress)", 14);
#ifdef TEST_WATCHDOG_DETAILS
    TEST_WATCHDOG_DETAILS();
#endif
    test_watchdog_write("\n", 1);
    _exit(124);
}

/* Re-arm only on completed work, never on a retry, spin or repeated wake. */
static inline void test_watchdog_progress(void)
{
    alarm(test_watchdog_seconds);
}
static inline void test_watchdog_stop(void)
{
    alarm(0);
}

static inline void test_watchdog_start(const char *prefix, const char *label, unsigned seconds,
                                       const char *counter0, const char *counter1)
{
    test_watchdog_stop();
    test_watchdog_prefix = prefix;
    test_watchdog_prefix_len = strlen(prefix);
    test_watchdog_label = label;
    test_watchdog_label_len = strlen(label);
    test_watchdog_seconds = seconds;
    test_watchdog_names[0] = counter0;
    test_watchdog_names[1] = counter1;
    for (unsigned i = 0; i < 2; ++i) {
        test_watchdog_name_lens[i] = test_watchdog_names[i] ? strlen(test_watchdog_names[i]) : 0;
        test_watchdog_counts[i] = -1;
    }
    struct sigaction action = {.sa_handler = test_watchdog_expired};
    sigemptyset(&action.sa_mask);
    TEST_WATCHDOG_CHECK(!sigaction(SIGALRM, &action, NULL));
    test_watchdog_progress();
}

#endif
