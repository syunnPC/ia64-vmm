/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CPU ordering, timer and cache regression tests. One binary, one suite per item.
 */
#ifndef TESTS_REVIEW_H
#define TESTS_REVIEW_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern unsigned long long review_checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++review_checks;                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "review: %s:%d: check failed: %s\n", __FILE__, __LINE__, #c);          \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

/* Mock clock for the timer suites (the binary links with
 * --wrap=hal_counter,--wrap=hal_counter_hz); off = the real hosted clock. */
extern int review_mock_clock;
extern uint64_t review_mock_counter, review_mock_hz;

/* Each suite registers itself; main runs them in link order. */
void review_register(const char *name, void (*run)(void));
#define REVIEW_SUITE(name, fn)                                                                     \
    __attribute__((constructor)) static void register_##fn(void)                                   \
    {                                                                                              \
        review_register(name, fn);                                                                 \
    }
#endif
