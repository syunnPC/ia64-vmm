/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Finished runs and their comparison; see results.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/results.h"
#include "tests/tier1/snapshot.h"

void result_init(struct run_result *r)
{
    memset(r, 0, sizeof(*r));
    r->ram = calloc(1, T1H_RAM_SIZE);
    T1H_CHECK(r->ram != NULL);
}

void result_free(struct run_result *r)
{
    free(r->ram);
    r->ram = NULL;
}

void result_take(struct run_result *r, const struct vcpu_exit *exit, const struct vcpu *v)
{
    r->exit = *exit;
    r->v = *v;
    ram_save(r->ram);
    const struct t1h_logs *logs = t1h_logs_of(v);
    T1H_CHECK(logs != NULL);
    r->logs = *logs;
}

unsigned result_compare(const struct run_result *a, const struct run_result *b, bool with_stores,
                        const char *label)
{
    struct compare_run x = {&a->exit,         &a->v, a->ram, T1H_RAM_SIZE, a->logs.device,
                            a->logs.device_n, NULL,  0,      NULL,         0};
    struct compare_run y = {&b->exit,         &b->v, b->ram, T1H_RAM_SIZE, b->logs.device,
                            b->logs.device_n, NULL,  0,      NULL,         0};
    if (with_stores) {
        x.stores = a->logs.stores;
        x.store_count = a->logs.store_n;
        y.stores = b->logs.stores;
        y.store_count = b->logs.store_n;
    }
    struct t1h_diff_printer printer = {label, 0, 8};
    unsigned n = compare_runs(&x, &y, t1h_print_diff, &printer);
    if (with_stores && a->logs.stores_outside != b->logs.stores_outside) {
        if (!t1h_quiet(label))
            fprintf(stderr, "  %s: out-of-range stores: reference %llu, actual %llu\n", label,
                    (unsigned long long)a->logs.stores_outside,
                    (unsigned long long)b->logs.stores_outside);
        ++n;
    }
    if (!t1h_logs_ok(&a->logs) || !t1h_logs_ok(&b->logs))
        ++n;
    if (n && !t1h_quiet(label))
        fprintf(stderr, "  %s: %u difference(s)\n", label, n);
    return n;
}
