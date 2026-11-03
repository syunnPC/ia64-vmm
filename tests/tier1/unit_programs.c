/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The guest programs under Tier1 through the test harness: each program runs to completion under the Tier1 engine
 * with the store log on and the in-place restore observer installed; every
 * materialized point is compared with the stepping reference at the same
 * retired count, the end state as a whole, and the run must show that
 * Tier1 really executed it -- region entries, most instructions retired by
 * generated code, and for the virtual variants native window fills.
 */
#include <stdio.h>
#include <stdlib.h>
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/programs.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/results.h"
#include "tests/tier1/snapshot.h"
#include "tests/tier1/storelog.h"
#include "engine/tier1/tier1.h"
#include "engine/tier1/ir.h"
#include "engine/hostwin/hostwin.h"

#define BIG_BUDGET UINT64_C(50000000)
#define CAPTURES 1024u

extern t1_ir_out_fn t1_test_mir_print;
static void print_line(void *arg, const char *text)
{
    (void)arg;
    fprintf(stderr, "%s\n", text);
}

void unit_programs_tests(void);
void unit_programs_tests(void)
{
    if (getenv("T1_UNIT_MIR_DUMP"))
        t1_test_mir_print = print_line;
    static struct t1h_logs ref_logs, logs;
    static struct snapshot snap;
    static struct run_result reference, actual;
    static struct observer obs;
    const struct t1h_engine *engine = t1h_engine_tier1 ? t1h_engine_tier1() : NULL;
    T1H_CHECK(engine != NULL);
    snapshot_init(&snap);
    result_init(&reference);
    result_init(&actual);
    observe_init(&obs, CAPTURES, NULL);
    unsigned programs = 0, captures = 0;
    for (unsigned i = 0; i < guest_program_count; ++i) {
        const struct guest_program *p = &guest_programs[i];
        if (p->kind == GUEST_SPEED || (p->flags & GUEST_SINGLE_STEP))
            continue; /* speed: speed.S; SS: lower tier only */
        struct guest_params q = guest_completion_params(p);
        struct vcpu_config cfg = guest_vcpu_config(p);
        struct vcpu *ref = vcpu_create(&cfg), *v = vcpu_create(&cfg);
        T1H_CHECK(ref && v);
        t1h_bind(ref, &ref_logs);
        t1h_bind(v, &logs);
        guest_setup(p, &q, ref);
        snapshot_take(&snap, ref);
        ref_logs.stores_on = true;
        snapshot_restore(&snap, ref);
        struct vcpu_exit e = ref_run(ref, BIG_BUDGET, NULL);
        result_take(&reference, &e, ref);

        struct t1h_engine_vcpu ev;
        t1h_engine_bind(&ev, engine, v);
        logs.stores_on = true;
        snapshot_restore(&snap, v);
        t1h_engine_restored(&ev);
        observe_reset(&obs);
        t1h_tier1_set_observer(&obs);
        struct t1_stats before, after;
        struct hostwin_stats hw_before, hw_after;
        t1_get_stats(&before);
        hostwin_get_stats(t1h_tier1_hostwin(ev.ctx), &hw_before);
        e = t1h_engine_run(&ev, BIG_BUDGET);
        t1_get_stats(&after);
        hostwin_get_stats(t1h_tier1_hostwin(ev.ctx), &hw_after);
        t1h_tier1_set_observer(NULL);
        result_take(&actual, &e, v);
        char label[96];
        snprintf(label, sizeof(label), "unit_programs %s/tier1", p->name);
        char why[256];
        if (!guest_check(p, &q, v, &e, &logs, why, sizeof(why))) {
            fprintf(stderr, "%s: %s\n", label, why);
            T1H_CHECK(!"guest program check under Tier1");
        }
        T1H_CHECK(result_compare(&reference, &actual, true, label) == 0);
        /* In place: every materialized point against the reference. */
        unsigned bad = observe_verify(&obs, &snap, ref, NULL, label);
        if (bad)
            fprintf(stderr, "%s: %u in-place capture(s) differ\n", label, bad);
        T1H_CHECK(bad == 0);
        /* Tier1 executed it. */
        uint64_t entries = after.entries - before.entries;
        uint64_t t1_retired = after.tier1_retired - before.tier1_retired;
        uint64_t fills = hw_after.fills - hw_before.fills;
        if (!entries || t1_retired * 2 < reference.v.retired || after.rejects != before.rejects)
            fprintf(stderr, "%s: entries %llu, tier1 retired %llu of %llu, rejects %llu\n", label,
                    (unsigned long long)entries, (unsigned long long)t1_retired,
                    (unsigned long long)reference.v.retired,
                    (unsigned long long)(after.rejects - before.rejects));
        T1H_CHECK(entries > 0 && t1_retired * 2 >= reference.v.retired);
        T1H_CHECK(after.rejects == before.rejects);
        if (p->kind == GUEST_GATHER && !(p->flags & GUEST_PHYS63))
            T1H_CHECK(fills > 0);
        printf(
            "unit_programs: %-16s %6llu instructions, %5.1f%% by Tier1 in %3llu entries (exits %llu/%llu/%llu/%llu), "
            "%3u in-place captures, %llu window fills, %llu traps, %llu misses, %llu site exits\n",
            p->name, (unsigned long long)reference.v.retired,
            100.0 * (double)t1_retired / (double)reference.v.retired, (unsigned long long)entries,
            (unsigned long long)(after.exits_by_kind[0] - before.exits_by_kind[0]),
            (unsigned long long)(after.exits_by_kind[1] - before.exits_by_kind[1]),
            (unsigned long long)(after.exits_by_kind[2] - before.exits_by_kind[2]),
            (unsigned long long)(after.exits_by_kind[3] - before.exits_by_kind[3]), obs.n,
            (unsigned long long)fills, (unsigned long long)(after.traps - before.traps),
            (unsigned long long)(after.window_misses - before.window_misses),
            (unsigned long long)(after.site_exits - before.site_exits));
        captures += obs.n;
        ++programs;
        storelog_detach(v);
        t1h_engine_release(&ev);
        storelog_detach(ref);
        t1h_unbind(ref);
        t1h_unbind(v);
        vcpu_destroy(ref);
        vcpu_destroy(v);
    }
    printf(
        "unit_programs: %u guest programs under Tier1 equal to the reference at the end and at %u in-place captures\n",
        programs, captures);
    observe_free(&obs);
    result_free(&reference);
    result_free(&actual);
    snapshot_free(&snap);
}
