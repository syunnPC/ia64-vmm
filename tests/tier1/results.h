/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The finished state of one run -- outer exit, vCPU, RAM, logs -- kept
 * aside so the next run can reuse the single test RAM, and compared with
 * the comparison relation (compare.c). All suites compare through here.
 */
#ifndef IA64VMM_TESTS_TIER1_RESULTS_H
#define IA64VMM_TESTS_TIER1_RESULTS_H
#include "tests/tier1/harness.h"

struct run_result {
    struct vcpu_exit exit;
    struct vcpu v;
    uint8_t *ram;
    struct t1h_logs logs;
};

void result_init(struct run_result *r);
void result_free(struct run_result *r);
/* Copies exit, *v, the live RAM and v's bound logs. */
void result_take(struct run_result *r, const struct vcpu_exit *exit, const struct vcpu *v);
/* Comparison relation. with_stores: compare the store logs and out-of-range
 * counts (both runs had the store log attached). Prints up to 8 differences
 * under `label`; returns
 * the number of differences (0 = equal). Harness errors in either run
 * count as a difference. */
unsigned result_compare(const struct run_result *reference, const struct run_result *actual,
                        bool with_stores, const char *label);
#endif
