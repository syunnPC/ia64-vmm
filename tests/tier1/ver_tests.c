/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * checked execution: suites of
 * build/tier1-test, every run compared with the stepping reference under
 * the comparison relation.
 *
 *   pairing        store pairing (store-once) with Tier0 as the outer loop
 *                  on `tier0+tier1` the harness's logging
 *                  proxy sees every Tier0 store at Tier0's helper boundary
 *                  (tier0_helper sets ip/slot before ia64_execute; with the
 *                  memory hook installed v->physical_region stays NULL, so
 *                  tier0_memory_pointer never returns an inline pointer),
 *                  and every other store is a native Tier1 record: the two
 *                  together are exactly the reference's stores, in order.
 *   compile_ticks  engine_compile_ticks: Tier0's lookup-
 *                  miss translations plus Tier1's compile requests, timed
 *                  only behind the statistics gates.
 *   undo           T1_COMPILE_UNDO_LOG: the pre-image of every
 *                  native store equals the bytes the reference's store
 *                  replaced.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "tests/tier1/engines.h"
#include "tests/tier1/engine_tier0t1.h"
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/programs.h"
#include "tests/tier1/provider.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/results.h"
#include "tests/tier1/snapshot.h"
#include "tests/tier1/storelog.h"
#include "tests/tier1/watchdog.h"
#include "ia64/interrupt.h"
#include "engine/tier0/tier0.h"
#include "engine/tier1/tier1.h"
#include "engine/tier1/region.h"
#include "engine/hostwin/internal.h"
#include "tests/tier1/schedule.h"
#include "engine/cache/cache.h"
#include "ia64vmm/engine.h"
#include "ia64vmm/hal.h"

#define BIG_BUDGET UINT64_C(50000000)
#define CHECK T1H_CHECK

#ifndef IA64VMM_TIER1_STATS
#define IA64VMM_TIER1_STATS 0
#endif

static struct vcpu *ref, *v;
static struct t1h_logs ref_logs, logs;
static struct snapshot snap;
static struct run_result reference, actual;

static void open_vcpus(const struct vcpu_config *cfg)
{
    if (!snap.ram) {
        snapshot_init(&snap);
        result_init(&reference);
        result_init(&actual);
    }
    ref = vcpu_create(cfg);
    v = vcpu_create(cfg);
    CHECK(ref && v);
    t1h_bind(ref, &ref_logs);
    t1h_bind(v, &logs);
}

/* The snapshot is ref's state now; the reference runs from it to the end. */
static void run_reference(bool stores)
{
    snapshot_take(&snap, ref);
    ref_logs.stores_on = logs.stores_on = stores;
    snapshot_restore(&snap, ref);
    struct vcpu_exit e = ref_run(ref, BIG_BUDGET, NULL);
    CHECK(e.reason == VCPU_EXIT_DEBUG && ref_logs.last_break_imm == T1H_BREAK_END);
    CHECK(t1h_logs_ok(&ref_logs));
    result_take(&reference, &e, ref);
}

/* A distinct 8-byte pattern per address of the output array (the guest
 * loops store into it and never read it), so that every replaced word
 * differs from every other: a pre-image taken from the wrong place or the
 * wrong time differs too. */
static void fill_output(void)
{
    uint8_t *out = t1h_ram() + (T1H_OUT - T1H_RAM_BASE);
    for (uint64_t off = 0; off < T1H_OUT_PAGES * UINT64_C(4096); off += 8) {
        uint64_t x = (T1H_OUT + off) * UINT64_C(0xff51afd7ed558ccd) ^ UINT64_C(0x5bd1e9955bd1e995);
        memcpy(out + off, &x, 8);
    }
    code_notify_vmm_write(T1H_OUT, T1H_OUT_PAGES * UINT64_C(4096));
}

static void open_program_filled(const char *name, struct guest_params q, bool stores, bool filled)
{
    const struct guest_program *p = guest_program(name);
    CHECK(p != NULL);
    struct vcpu_config cfg = guest_vcpu_config(p);
    open_vcpus(&cfg);
    guest_setup(p, &q, ref);
    if (filled)
        fill_output();
    run_reference(stores);
}

static void open_program(const char *name, struct guest_params q, bool stores)
{
    open_program_filled(name, q, stores, false);
}

/* programs/store_pairing.S: n iterations over the output array (harness.h T1H_OUT). */
#define PAIR_N 300u
static void open_pair(void)
{
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    open_vcpus(&cfg);
    t1h_load_image("store_pairing.bin");
    uint8_t *out = t1h_ram() + (T1H_OUT - T1H_RAM_BASE);
    for (unsigned k = 0; k <= PAIR_N; ++k) {
        uint64_t x = UINT64_C(0x9e3779b97f4a7c15) * (k + 1);
        memcpy(out + 8 * k, &x, 8);
    }
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[2] = T1H_OUT;
    regs.gr[8] = PAIR_N - 1;
    vcpu_reset(ref, &regs);
    ref->s.cr[CR_IVA] = T1H_IVT;
    run_reference(true);
}

static void close_program(void)
{
    t1h_unbind(ref);
    t1h_unbind(v);
    vcpu_destroy(ref);
    vcpu_destroy(v);
    ref = v = NULL;
}

/* ---- pairing ------------------------------------------------------------ */

static void pairing_case(const char *name)
{
    if (!strcmp(name, "store_pairing"))
        open_pair();
    else
        open_program(name, guest_completion_params(guest_program(name)), true);
    CHECK(ref_logs.store_n > 0 && ref_logs.paired_stepped == ref_logs.store_n);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), v);
    uint32_t flags = t1h_tier0t1_flags();
    t1h_tier0t1_set_flags(0);
    tier0_flush();
    t1_flush();
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    /* Loop heads compile after 8 Tier0 entries: Tier0 runs the first
     * iterations' stores itself, Tier1 the rest. */
    t1_test_set_thresholds(8, 128);
    struct tier0_stats t0a, t0b;
    struct t1_stats t1a, t1b;
    tier0_get_stats(&t0a);
    t1_get_stats(&t1a);
    struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
    tier0_get_stats(&t0b);
    t1_get_stats(&t1b);
    t1_test_set_thresholds(0, 0);
    result_take(&actual, &e, v);
    /* Premise 1: with the hook installed the per-vCPU page cache stays
     * empty, so Tier0 has no physical page to access inline. */
    CHECK(v->memory_hook != NULL && v->physical_region == NULL);
    /* Every store of the run is in the merged log once: Tier0's through
     * the proxy, paired in its callback at Tier0's helper boundary
     * (premise 3; a store whose (ip, slot) did not decode to a store of
     * that width is a harness error, t1h_logs_ok), Tier1's as native
     * records. Both kinds occur. */
    uint64_t native = logs.store_n - logs.paired_callback - logs.paired_stepped;
    if (!t1h_logs_ok(&logs) || logs.paired_stepped || !logs.paired_callback || !native)
        fprintf(
            stderr,
            "pairing: %s: stores %zu (reference %zu), stepped %llu, callback %llu, native %llu; %s\n",
            name, logs.store_n, ref_logs.store_n, (unsigned long long)logs.paired_stepped,
            (unsigned long long)logs.paired_callback, (unsigned long long)native,
            logs.errors ? logs.error : "no harness error");
    CHECK(t1h_logs_ok(&logs));
    CHECK(!logs.paired_stepped && logs.paired_callback > 0 && native > 0);
    CHECK(logs.store_n == ref_logs.store_n);
    CHECK(t0b.executed_instructions > t0a.executed_instructions && t1b.entries > t1a.entries);
    CHECK(result_compare(&reference, &actual, true, name) == 0);
    printf(
        "pairing: %s: %zu stores, %llu by Tier0 helpers (paired at the helper boundary), %llu native "
        "Tier1 records; Tier0 retired %llu, Tier1 %llu entries\n",
        name, logs.store_n, (unsigned long long)logs.paired_callback, (unsigned long long)native,
        (unsigned long long)(t0b.executed_instructions - t0a.executed_instructions),
        (unsigned long long)(t1b.entries - t1a.entries));
    t1h_tier0t1_set_flags(flags);
    t1h_engine_release(&ev);
    close_program();
}

static void pairing_tests(void)
{
    /* gather_alias: post-increment stores through two VRNs aliasing one PA;
     * gather_load_after_store: a load of the word the loop stored (S3/S4); gather_virt: the
     * walker-filled table. */
    pairing_case("gather_alias");
    pairing_case("gather_load_after_store");
    pairing_case("gather_virt");
    pairing_case("gather_phys");
    /* Stores without base update, which Tier0 would execute inline
     * (premise 2: tier0_memory_pointer returns NULL under the hook). */
    pairing_case("store_pairing");
    /* Without the hook, the same program fills the per-vCPU page cache on
     * Tier0 and its plain stores take the inline path (counted in
     * statistics builds): the hook is what turns them into helpers. */
    open_pair();
    struct tier0_stats a, b;
    tier0_flush();
    snapshot_restore(&snap, v);
    storelog_detach(v);
    tier0_get_stats(&a);
    struct vcpu_exit e = tier0_run(v, BIG_BUDGET);
    tier0_get_stats(&b);
    CHECK(e.reason == VCPU_EXIT_DEBUG && logs.last_break_imm == T1H_BREAK_END);
    CHECK(v->memory_hook == NULL && v->physical_region != NULL);
    CHECK(!IA64VMM_TIER0_STATS || b.inlined[IA64_OP_ST8] - a.inlined[IA64_OP_ST8] >= PAIR_N - 16);
    printf("pairing: store_pairing on Tier0 without the hook: page cache filled%s\n",
           IA64VMM_TIER0_STATS ? ", st8 inline" : "");
    close_program();
}

/* ---- compile_ticks ------------------------------------------------------ */

static uint64_t observed_ticks, observed_compiles;
static void count_compile(const struct vcpu *cv, enum t1_compile_status st,
                          const struct t1_compile_report *rep, void *arg)
{
    (void)cv;
    (void)st;
    (void)arg;
    observed_ticks += rep->compile_ticks;
    ++observed_compiles;
}

static void compile_ticks_tests(void)
{
    const struct guest_program *p = guest_program("gather_virt");
    open_program("gather_virt", guest_sweep_params(p), false);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), v);
    uint32_t flags = t1h_tier0t1_flags();
    t1h_tier0t1_set_flags(0);
    tier0_flush();
    t1_flush();
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    observed_ticks = observed_compiles = 0;
    t1_test_set_compile_observer(count_compile, NULL);
    struct tier0_stats t0a, t0b;
    struct t1_stats t1a, t1b;
    tier0_get_stats(&t0a);
    t1_get_stats(&t1a);
    uint64_t before = 0, after = 0;
    bool timed_before = engine_compile_ticks(&before);
    uint64_t start = hal_counter();
    struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
    uint64_t elapsed = hal_counter() - start;
    bool timed = engine_compile_ticks(&after);
    tier0_get_stats(&t0b);
    t1_get_stats(&t1b);
    t1_test_set_compile_observer(NULL, NULL);
    result_take(&actual, &e, v);
    CHECK(result_compare(&reference, &actual, false, "compile_ticks") == 0);
    uint64_t compiles = (t1b.compiles + t1b.rejects) - (t1a.compiles + t1a.rejects);
    uint64_t t0_ticks = t0b.translate_ticks - t0a.translate_ticks,
             t1_ticks = t1b.compile_ticks - t1a.compile_ticks;
    CHECK(compiles > 0 && compiles == observed_compiles);
    CHECK(t0b.translations > t0a.translations);
    /* Tier1's total is the sum of its requests' compile_ticks. */
    CHECK(t1_ticks == observed_ticks);
    if (IA64VMM_TIER0_STATS && IA64VMM_TIER1_STATS) {
        /* Both parts measured, inside the run, and engine_compile_ticks is
         * their total. */
        CHECK(timed_before && timed);
        CHECK(t0_ticks > 0 && t1_ticks > 0 && t0_ticks + t1_ticks <= elapsed);
        CHECK(after - before == t0_ticks + t1_ticks);
        CHECK(after == t0b.translate_ticks + t1b.compile_ticks);
    } else {
        /* Ungated parts read no clock; a total without Tier1's (Tier1
         * compiled, untimed) or without Tier0's is not reported. */
        CHECK(IA64VMM_TIER0_STATS || t0_ticks == 0);
        CHECK(IA64VMM_TIER1_STATS || (t1_ticks == 0 && observed_ticks == 0));
        CHECK(!timed);
    }
    printf(
        "compile_ticks: TIER0_STATS=%d TIER1_STATS=%d: %llu Tier0 translations in %llu ticks, %llu Tier1 "
        "compile requests in %llu ticks, run %llu ticks; engine_compile_ticks %s\n",
        IA64VMM_TIER0_STATS, IA64VMM_TIER1_STATS,
        (unsigned long long)(t0b.translations - t0a.translations), (unsigned long long)t0_ticks,
        (unsigned long long)compiles, (unsigned long long)t1_ticks, (unsigned long long)elapsed,
        timed ? "reports the total" : "reports nothing");
    t1h_tier0t1_set_flags(flags);
    t1h_engine_release(&ev);
    close_program();
}

/* ---- undo ---------------------------------------------------------------
 * T1_COMPILE_UNDO_LOG: every native store record of a
 * region compiled with the undo log carries the bytes its store replaced.
 * Checked against the reference's: its logging proxy reads them right
 * before each write, and the merged log of the product path is the
 * reference's store by store (pairing above), so record i of both runs is
 * the same store. Window pages are cold at first, so store sites fault at
 * their pre-load and resume there after the fill (trap.c T1_RIP_PRE). */

static uint64_t undo_case(const char *name)
{
    if (!strcmp(name, "store_pairing"))
        open_pair();
    else
        open_program_filled(name, guest_completion_params(guest_program(name)), true, true);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), v);
    uint32_t flags = t1h_tier0t1_flags();
    t1h_tier0t1_set_flags(T1_COMPILE_UNDO_LOG); /* run() adds STORE_LOG with the store log */
    tier0_flush();
    t1_flush();
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    struct t1_stats a, b;
    t1_get_stats(&a);
    struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
    t1_get_stats(&b);
    result_take(&actual, &e, v);
    CHECK(t1h_logs_ok(&logs) && logs.store_n == ref_logs.store_n);
    CHECK(result_compare(&reference, &actual, true, name) == 0);
    uint64_t native = 0, proxy = 0;
    for (size_t i = 0; i < logs.store_n; ++i) {
        CHECK(ref_logs.store_pre_kind[i] == T1H_PRE_MEMORY);
        if (logs.store_pre_kind[i] == T1H_PRE_UNDO)
            ++native;
        else if (logs.store_pre_kind[i] == T1H_PRE_MEMORY)
            ++proxy;
        if (logs.store_pre_kind[i] == T1H_PRE_NONE || logs.store_pre[i] != ref_logs.store_pre[i]) {
            fprintf(
                stderr,
                "undo: %s: store %zu (gva %#llx, width %u): pre-image %#llx (%s), reference %#llx\n",
                name, i, (unsigned long long)logs.stores[i].gva, logs.stores[i].width,
                (unsigned long long)logs.store_pre[i],
                logs.store_pre_kind[i] == T1H_PRE_UNDO     ? "undo log"
                : logs.store_pre_kind[i] == T1H_PRE_MEMORY ? "proxy"
                                                           : "none",
                (unsigned long long)ref_logs.store_pre[i]);
            CHECK(!"pre-image differs from the reference's");
        }
    }
    CHECK(native > 0 && native + proxy == logs.store_n);
    uint64_t pre_traps = b.undo_pre_traps - a.undo_pre_traps;
    printf(
        "undo: %s: %llu native records with their pre-images equal to the reference's, %llu Tier0 stores; "
        "%llu faults at a pre-load\n",
        name, (unsigned long long)native, (unsigned long long)proxy, (unsigned long long)pre_traps);
    t1h_tier0t1_set_flags(flags);
    t1h_engine_release(&ev);
    close_program();
    return pre_traps;
}

static void undo_tests(void)
{
    uint64_t pre_traps = 0;
    /* Widths 8 and 4 (store_pairing), post-increment and predicated stores, two
     * VRNs of one PA, a walker-filled table, one page per S2 occurrence. */
    static const char *const programs[] = {
        "store_pairing", "gather_phys", "gather_load_after_store",
        "gather_alias",  "gather_virt", "gather_virt_stride",
        "gather_2page",  "gather_rot"};
    for (unsigned k = 0; k < sizeof(programs) / sizeof(programs[0]); ++k)
        pre_traps += undo_case(programs[k]);
    CHECK(pre_traps > 0);
}

/* ---- checked -------------------------------------------------------------
 * Checked lockstep (engine/tier1/checked.c) on the
 * engine `lockstep+tier1`: Tier0 regions checked by Tier0's lockstep, every
 * Tier1 execution by Tier1's checker. Each run must end as the reference
 * (comparison relation, final state and RAM) with no divergence, with Tier1 entered
 * and every entry checked. Without the store log: Tier0's lockstep owns
 * the vCPU's memory hook. */

struct check_counts {
    uint64_t entries, checked, retired, unchecked, divergences, captures, t0_checked;
};
static struct check_counts check_counts(void)
{
    struct t1_stats s;
    struct tier0_stats t0;
    t1_get_stats(&s);
    tier0_get_stats(&t0);
    return (struct check_counts){s.entries,     s.checked,        s.checked_retired, s.unchecked,
                                 s.divergences, s.check_captures, t0.checked_regions};
}

/* One run of the checked engine from the snapshot; *d gets the deltas. */
static struct vcpu_exit checked_run(struct check_counts *d)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_checked(), v);
    tier0_flush();
    t1_flush();
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    struct check_counts a = check_counts();
    struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
    struct check_counts b = check_counts();
    *d = (struct check_counts){b.entries - a.entries,         b.checked - a.checked,
                               b.retired - a.retired,         b.unchecked - a.unchecked,
                               b.divergences - a.divergences, b.captures - a.captures,
                               b.t0_checked - a.t0_checked};
    t1h_engine_release(&ev);
    return e;
}

static struct check_counts checked_totals;

static void checked_case(const char *name)
{
    if (!strcmp(name, "store_pairing"))
        open_pair();
    else
        open_program(name, guest_completion_params(guest_program(name)), false);
    struct check_counts d;
    struct vcpu_exit e = checked_run(&d);
    result_take(&actual, &e, v);
    if (result_compare(&reference, &actual, false, name) || d.divergences || d.unchecked ||
        !d.checked || d.checked != d.entries)
        fprintf(stderr,
                "checked: %s: exit %u (%s), Tier1 entries %llu, checked %llu, unchecked %llu, "
                "divergences %llu\n",
                name, e.reason, e.detail ? e.detail : "", (unsigned long long)d.entries,
                (unsigned long long)d.checked, (unsigned long long)d.unchecked,
                (unsigned long long)d.divergences);
    CHECK(result_compare(&reference, &actual, false, name) == 0);
    CHECK(!d.divergences && !d.unchecked && d.checked > 0 && d.checked == d.entries &&
          d.t0_checked > 0);
    printf(
        "checked: %s: %llu Tier1 executions checked (%llu instructions, %llu of %llu retired), 0 unchecked, "
        "%llu C-side page captures; Tier0 lockstep %llu regions\n",
        name, (unsigned long long)d.checked, (unsigned long long)d.retired,
        (unsigned long long)d.retired, (unsigned long long)v->retired,
        (unsigned long long)d.captures, (unsigned long long)d.t0_checked);
    checked_totals.checked += d.checked;
    checked_totals.retired += d.retired;
    checked_totals.captures += d.captures;
    close_program();
}

/* programs/checked_capture.S on gather-virt's guest MMU: its code over the gather-virt image
 * (the IVT stays), the word at VHPT page 0 offset 0x100 (no table page
 * hashes there) and the table's first 8 pages (VHPT page 0 entries). */
#define CKCAP_N 8u
static void open_ckcap(void)
{
    const struct guest_program *p = guest_program("gather_virt");
    struct vcpu_config cfg = guest_vcpu_config(p);
    open_vcpus(&cfg);
    struct guest_params q = guest_sweep_params(p);
    guest_setup(p, &q, ref);
    FILE *f = fopen(t1h_image("checked_capture.bin"), "rb");
    CHECK(f != NULL);
    size_t n = fread(t1h_ram() + (T1H_CODE - T1H_RAM_BASE), 1, T1H_IVT - T1H_CODE, f);
    CHECK(n > 0 && feof(f) && !ferror(f));
    fclose(f);
    code_notify_vmm_write(T1H_CODE, T1H_IVT - T1H_CODE);
    uint64_t word = UINT64_C(0x1234500000000000);
    memcpy(t1h_ram() + (T1H_VHPT + 0x100 - T1H_RAM_BASE), &word, 8);
    code_notify_vmm_write(T1H_VHPT + 0x100, 8);
    ref->s.gr[2] = (UINT64_C(2) << 61) | (T1H_VHPT + 0x100);
    ref->s.gr[3] = (UINT64_C(2) << 61) | T1H_TABLE_VA;
    ref->s.gr[8] = CKCAP_N - 1;
    run_reference(false);
}

/* The access log's mutation test and the capture's undo: each altered execution is
 * reported as a divergence and stops the run (VCPU_EXIT_DEBUG). */
static void mutation_case(enum t1_check_mutation m, const char *what, bool ckcap)
{
    if (ckcap)
        open_ckcap();
    else
        open_pair();
    printf("checked: mutation %s: a divergence report is expected next\n", what);
    fflush(stdout);
    t1_test_check_mutate(m, 0);
    struct check_counts d;
    struct vcpu_exit e = checked_run(&d);
    t1_test_check_mutate(T1_MUTATE_NONE, 0);
    CHECK(e.reason == VCPU_EXIT_DEBUG && e.detail &&
          !strcmp(e.detail, "tier1 lockstep divergence"));
    CHECK(d.divergences == 1);
    printf("checked: mutation %s: divergence after %llu checked executions\n", what,
           (unsigned long long)d.checked);
    close_program();
}

/* Checked lockstep with chaining: a mutation armed when a
 * region is published at a chain point is reported by that execution's own
 * check: no other region is published between the arming and the stop. */
static struct {
    unsigned seen, after;
    bool armed;
} chained_mutation;
static void chained_publish(struct vcpu *pv, const struct t1_region *r)
{
    (void)r;
    const struct t1_vcpu *tv = pv->upper;
    if (chained_mutation.armed) {
        ++chained_mutation.after;
        return;
    }
    if (!tv || !tv->chained || ++chained_mutation.seen < 8)
        return;
    chained_mutation.armed = true;
    t1_test_check_mutate(T1_MUTATE_STATE, 0);
}

static void chained_mutation_case(void)
{
    struct guest_params q = guest_completion_params(guest_program("gather_phys"));
    q.passes = 30;
    q.bench = true;
    open_program("gather_phys", q, false);
    printf("checked: mutation in a chained region: a divergence report is expected next\n");
    fflush(stdout);
    memset(&chained_mutation, 0, sizeof(chained_mutation));
    t1_test_publish_hook = chained_publish;
    struct check_counts d;
    struct vcpu_exit e = checked_run(&d);
    t1_test_publish_hook = NULL;
    t1_test_check_mutate(T1_MUTATE_NONE, 0);
    CHECK(chained_mutation.armed && !chained_mutation.after);
    CHECK(e.reason == VCPU_EXIT_DEBUG && e.detail &&
          !strcmp(e.detail, "tier1 lockstep divergence"));
    CHECK(d.divergences == 1);
    printf(
        "checked: mutation in the region entered at the 8th chain point: reported by its own check after %llu "
        "checked executions\n",
        (unsigned long long)d.checked);
    close_program();
}

/* gather-virt with every table page unmapped half-way (the guest's ptc.l and a
 * VHPT entry with p = 0, schedule.c INJ_UNMAP): Tier1 sites then fault,
 * hostwin_resolve's access check walks the VHPT, which installs the
 * not-present entry and raises Page Not Present, and the execution leaves
 * at the site with that TC entry in place; the checker mirrors the access
 * check on the shadow (checked.c), and the guest's handler refills. */
static void unmap_case(void)
{
    const struct guest_program *p = guest_program("gather_virt");
    struct guest_params q = guest_completion_params(p);
    struct vcpu_config cfg = guest_vcpu_config(p);
    open_vcpus(&cfg);
    guest_setup(p, &q, ref);
    run_reference(false);
    uint64_t total = ref->retired - snap.v.retired;
    uint64_t refills = t1h_bank0(ref, 20) + t1h_bank0(ref, 29);
    static struct schedule sched;
    schedule_clear(&sched);
    for (unsigned page = 0; page < 40; ++page)
        schedule_add(&sched, (struct injection){.at = snap.v.retired + total / 2,
                                                .kind = INJ_UNMAP,
                                                .address = (UINT64_C(2) << 61) |
                                                           (T1H_TABLE_VA + page * UINT64_C(4096))});
    snapshot_restore(&snap, ref);
    struct vcpu_exit re = ref_run(ref, BIG_BUDGET, &sched);
    CHECK(schedule_complete(&sched));
    CHECK(re.reason == VCPU_EXIT_DEBUG && ref_logs.last_break_imm == T1H_BREAK_END);
    CHECK(t1h_bank0(ref, 20) + t1h_bank0(ref, 29) > refills);
    result_take(&reference, &re, ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_checked(), v);
    tier0_flush();
    t1_flush();
    schedule_rearm(&sched);
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    struct check_counts a = check_counts();
    struct t1_stats sa, sb;
    t1_get_stats(&sa);
    struct vcpu_exit e = t1h_engine_run_sched(&ev, BIG_BUDGET, &sched);
    struct check_counts b = check_counts();
    t1_get_stats(&sb);
    CHECK(schedule_complete(&sched));
    result_take(&actual, &e, v);
    CHECK(result_compare(&reference, &actual, false, "gather_virt unmapped") == 0);
    CHECK(b.divergences == a.divergences && b.unchecked == a.unchecked && b.checked > a.checked &&
          b.checked - a.checked == b.entries - a.entries);
    /* Tier1 executions left at sites whose access check faulted. */
    CHECK(sb.site_exits > sa.site_exits);
    printf(
        "checked: gather_virt, table unmapped half-way: %llu Tier1 executions checked, %llu site exits, the "
        "guest refilled %llu pages; equal to the reference\n",
        (unsigned long long)(b.checked - a.checked),
        (unsigned long long)(sb.site_exits - sa.site_exits),
        (unsigned long long)(t1h_bank0(v, 20) + t1h_bank0(v, 29) - refills));
    t1h_engine_release(&ev);
    close_program();
}

static void kick_case(bool at_entry, unsigned vector);
/* A sampled replay must start from that execution's memory, even after
 * unchecked stores; an injected wrong result must still stop the run. */
static void sampled_case(unsigned every, uint64_t after, bool corrupt)
{
    open_program("gather_virt", guest_completion_params(guest_program("gather_virt")), false);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), v);
    tier0_flush();
    t1_flush();
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    uint32_t old_flags = t1h_tier0t1_flags();
    t1h_tier0t1_set_flags(T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG);
    t1_cfg.check_every = every;
    t1_cfg.check_after = after;
    const struct gm_region *(*memory_hook)(struct vcpu *, gpa_t, unsigned) = v->memory_hook;
    struct check_counts a = check_counts();
    if (corrupt)
        t1_test_check_mutate(T1_MUTATE_STATE, 0);
    struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
    t1_test_check_mutate(T1_MUTATE_NONE, 0);
    struct check_counts b = check_counts();
    if (corrupt) {
        CHECK(e.reason == VCPU_EXIT_DEBUG && e.detail &&
              !strcmp(e.detail, "tier1 lockstep divergence"));
        CHECK(b.divergences == a.divergences + 1);
    } else {
        result_take(&actual, &e, v);
        CHECK(result_compare(&reference, &actual, false, "sampled gather_virt") == 0);
        CHECK(b.divergences == a.divergences);
        CHECK(b.entries - a.entries == b.checked - a.checked + b.unchecked - a.unchecked);
        if (after == UINT64_MAX)
            CHECK(b.checked == a.checked);
        else
            CHECK(b.checked > a.checked);
    }
    CHECK(b.unchecked > a.unchecked);
    CHECK(v->memory_hook == memory_hook);
    printf(
        "checked: sampling every=%u after=%llu: %llu checked, %llu unchecked, %llu divergences%s\n",
        every, (unsigned long long)after, (unsigned long long)(b.checked - a.checked),
        (unsigned long long)(b.unchecked - a.unchecked),
        (unsigned long long)(b.divergences - a.divergences), corrupt ? " (injected)" : "");
    t1_cfg.check_every = t1_cfg.check_after = 0;
    t1h_tier0t1_set_flags(old_flags);
    t1h_engine_release(&ev);
    close_program();
}

/* Alternating contracts must both be sampled, including when the period
 * shares a factor with the number of regions. shladd_xor uses only static GRs. */
static void sampled_regions(void)
{
    open_program("shladd_xor", guest_completion_params(guest_program("shladd_xor")), false);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), v);
    tier0_flush();
    t1_flush();
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    uint64_t header;
    CHECK(t1h_run_to_loop_header(v, 100000, &header));
    struct ia64_state entry = v->s;
    struct t1_vcpu *tv = t1h_tier1_vcpu(ev.ctx);
    struct t1_compile_options opt = {T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG, 0, 0};
    uint64_t links[2], checks[2] = {0}, skips[2] = {0};
    t1_test_set_compile_options(tv, &opt);
    for (unsigned k = 0; k < 2; ++k) {
        v->s.cfm = k;
        struct t1_compile_report rep;
        CHECK(t1_compile(tv, &opt, &rep) == T1_COMPILED);
        links[k] = t1_region_link(t1_region_get(rep.region_id));
    }
    CHECK(links[0] != links[1]);
    v->upper = tv;
    v->cfg.engine = ENGINE_TIER1;
    t1_cfg.check_every = 2;
    for (unsigned n = 0; n < 12; ++n) {
        unsigned k = n % 2;
        v->s = entry;
        v->s.cfm = k;
        struct check_counts a = check_counts();
        struct vcpu_exit out;
        CHECK(t1_upper_enter(v, &links[k], 256, &out) == T1_UPPER_RAN);
        struct check_counts b = check_counts();
        CHECK(b.entries == a.entries + 1 && b.divergences == a.divergences);
        checks[k] += b.checked - a.checked;
        skips[k] += b.unchecked - a.unchecked;
    }
    for (unsigned k = 0; k < 2; ++k)
        CHECK(checks[k] > 0 && skips[k] > 0);
    t1_cfg.check_every = 0;
    v->upper = NULL;
    v->cfg.engine = ENGINE_INTERP;
    t1h_engine_release(&ev);
    close_program();
    printf("checked: alternating regions both sampled and skipped at period 2\n");
}

static enum hostwin_map_status stopped_map_status;
static unsigned stopped_map_calls;
static enum hostwin_map_status stopped_map(void *ctx, uintptr_t host_page, gpa_t pa_page,
                                           bool writable)
{
    (void)ctx;
    (void)host_page;
    (void)pa_page;
    (void)writable;
    ++stopped_map_calls;
    return stopped_map_status;
}

/* The window backend's terminal failures stop before the access. Replay
 * must still release its memory hook before returning that stop. */
static void checked_backend_stop(enum hostwin_map_status status)
{
    open_program("gather_virt", guest_completion_params(guest_program("gather_virt")), false);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), v);
    tier0_flush();
    t1_flush();
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    uint32_t old_flags = t1h_tier0t1_flags();
    t1h_tier0t1_set_flags(T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG);
    struct t1_vcpu *tv = t1h_tier1_vcpu(ev.ctx);
    const struct hostwin_backend *backend = tv->hw->backend;
    struct hostwin_backend failed = *backend;
    failed.map = stopped_map;
    tv->hw->backend = &failed;
    stopped_map_status = status;
    stopped_map_calls = 0;
    const struct gm_region *(*memory_hook)(struct vcpu *, gpa_t, unsigned) = v->memory_hook;
    struct check_counts a = check_counts();
    /* Replaying this terminal execution would report a divergence and
     * hide the mapping stop. Cancellation must leave this mutation unused. */
    t1_test_check_mutate(T1_MUTATE_STATE, 0);
    struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
    t1_test_check_mutate(T1_MUTATE_NONE, 0);
    struct check_counts b = check_counts();
    CHECK(e.reason == VCPU_EXIT_BAD_ACCESS && e.detail == hostwin_stop_detail(tv->hw));
    CHECK(stopped_map_calls == (status == HOSTWIN_MAP_EXHAUSTED ? 2u : 1u));
    CHECK(b.entries > a.entries && b.checked - a.checked + 1 == b.entries - a.entries &&
          b.divergences == a.divergences);
    CHECK(v->memory_hook == memory_hook && !tv->checking);
    tv->hw->backend = backend;
    t1h_tier0t1_set_flags(old_flags);
    t1h_engine_release(&ev);
    close_program();
    printf("checked: backend stop %u: %u map attempts, replay hook restored\n", status,
           stopped_map_calls);
}

static const char *const checked_programs[] = {"gather_phys",
                                               "gather_virt",
                                               "gather_virt_stride",
                                               "gather_rot",
                                               "gather_rotp",
                                               "gather_2page",
                                               "gather_load_after_store",
                                               "gather_alias",
                                               "gather_alias_phys63",
                                               "shladd_xor",
                                               "mid_block",
                                               "window_tags",
                                               "rse_return",
                                               "rse_recursion",
                                               "mmu_log_writers",
                                               "speed",
                                               "store_pairing",
                                               "call_return"};

static void checked_tests(void)
{
    struct t1_stats s0, s1;
    t1_get_stats(&s0);
    for (unsigned k = 0; k < sizeof(checked_programs) / sizeof(checked_programs[0]); ++k)
        checked_case(checked_programs[k]);
    t1_get_stats(&s1);
    CHECK(checked_totals.captures > 0);
    /* Chaining is on under lockstep unless T1H_CHAIN=off. */
    const char *chain = getenv("T1H_CHAIN");
    bool chaining = !(chain && !strcmp(chain, "off"));
    CHECK(chaining == (s1.chain_links > s0.chain_links));
    printf("checked: %llu regions entered at chain points, every one checked\n",
           (unsigned long long)(s1.chain_links - s0.chain_links));
    printf("checked: %llu Tier1 executions checked, %llu instructions, 0 divergences\n",
           (unsigned long long)checked_totals.checked, (unsigned long long)checked_totals.retired);
    unmap_case();

    /* The capture program unaltered: equal, with C-side captures. */
    open_ckcap();
    struct check_counts d;
    struct vcpu_exit e = checked_run(&d);
    result_take(&actual, &e, v);
    CHECK(result_compare(&reference, &actual, false, "checked_capture") == 0);
    CHECK(!d.divergences && !d.unchecked && d.checked > 0 && d.checked == d.entries &&
          d.captures > 0);
    printf(
        "checked: checked_capture: %llu Tier1 executions checked, %llu C-side page captures after native stores "
        "to the page\n",
        (unsigned long long)d.checked, (unsigned long long)d.captures);
    close_program();
    mutation_case(T1_MUTATE_STORE_VALUE, "store value", false);
    mutation_case(T1_MUTATE_PRE_IMAGE, "pre-image", false);
    mutation_case(T1_MUTATE_DROP_RECORD, "record dropped", false);
    mutation_case(T1_MUTATE_GPA, "record GPA", false);
    mutation_case(T1_MUTATE_STATE, "r1 after the execution", false);
    mutation_case(T1_MUTATE_CAPTURE_UNDO, "capture without undo", true);
    if (chaining)
        chained_mutation_case();
    kick_case(true, 0x41);
    kick_case(false, 0x52);
    sampled_case(7, 0, false);
    sampled_case(1, UINT64_MAX, false);
    sampled_case(3, 2000, false);
    sampled_case(3, 2000, true);
    sampled_regions();
    checked_backend_stop(HOSTWIN_MAP_WRONG_CPU);
    checked_backend_stop(HOSTWIN_MAP_EXHAUSTED);
}

void replay_review_tests(void)
{
    sampled_regions();
    checked_backend_stop(HOSTWIN_MAP_WRONG_CPU);
    checked_backend_stop(HOSTWIN_MAP_EXHAUSTED);
}

/* ---- interrupt kicks under checked lockstep ----------------------------------------
 * an asynchronous notification during checked lockstep must
 * neither split a checked execution nor be lost. The vector is published
 * in a mailbox and the vCPU kicked (platform/interrupts.c inject(); the
 * executor drains before each run, as in suite upper), (a) as a checked
 * execution begins (the entry hook, after Tier0's loop top and Tier1's
 * guards): its region code leaves at the event checkpoint and the shorter
 * execution is checked whole; (b) from another thread while the checker
 * replays an execution: the shadow has no kick, the real vCPU keeps it.
 * Either way Tier0's loop top yields exactly once and the next run
 * delivers: equal to the reference raised at the delivery boundary, no
 * divergence, every Tier1 execution checked. */

static uint64_t mailbox[4];
static void drain(struct vcpu *dv)
{
    for (unsigned i = 0; i < 4; ++i)
        for (uint64_t bits = __atomic_exchange_n(&mailbox[i], 0, __ATOMIC_ACQ_REL); bits;
             bits &= bits - 1)
            vcpu_raise_external(dv, 64 * i + (unsigned)__builtin_ctzll(bits));
}
struct ck_source {
    struct vcpu *v;
    uint64_t entries0;
    unsigned delay_us, vector;
    int stop;
    bool acted, in_region, in_check;
    uint64_t before, after;
};
/* Tier1 entries of every thread (the counters are per thread). */
static uint64_t entries_total(void)
{
    struct t1_stats s;
    t1_get_stats(&s);
    return s.entries;
}
static void *ck_source_thread(void *arg)
{
    struct ck_source *s = arg;
    while (entries_total() == s->entries0)
        if (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE))
            return NULL;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        if (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE))
            return NULL;
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((uint64_t)(t.tv_sec - t0.tv_sec) * 1000000000u + (uint64_t)(t.tv_nsec - t0.tv_nsec) <
             (uint64_t)s->delay_us * 1000);
    /* Then while the checker runs (most of a checked execution's time). */
    while (!t1_test_checking())
        if (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE))
            return NULL;
    s->before = __atomic_load_n(&s->v->retired, __ATOMIC_RELAXED);
    __atomic_fetch_or(&mailbox[s->vector / 64], UINT64_C(1) << (s->vector % 64), __ATOMIC_SEQ_CST);
    vcpu_kick(s->v);
    s->in_region = t1_test_running(s->v);
    s->in_check = t1_test_checking();
    s->after = __atomic_load_n(&s->v->retired, __ATOMIC_RELAXED);
    s->acted = true;
    return NULL;
}

#define KICK_SHLADD_N 200000u
/* (a) at the entry hook: the kick arrives after Tier0's loop top and Tier1's
 * entry guards, before the checked execution begins (regions compiled
 * with T1_COMPILE_VERIFY too, whose entry hook runs there). */
struct kick_at_entry {
    struct vcpu *v;
    unsigned vector;
    uint64_t countdown;
    bool fired;
};
static void kick_entry_hook(struct t1_vcpu *tv, uint32_t region, void *arg)
{
    (void)tv;
    (void)region;
    struct kick_at_entry *k = arg;
    if (k->fired || --k->countdown)
        return;
    __atomic_fetch_or(&mailbox[k->vector / 64], UINT64_C(1) << (k->vector % 64), __ATOMIC_SEQ_CST);
    vcpu_kick(k->v);
    k->fired = true;
}

static void kick_case(bool at_entry, unsigned vector)
{
    const struct guest_program *p = guest_program("shladd_xor");
    struct guest_params q = guest_completion_params(p);
    q.n = KICK_SHLADD_N;
    struct vcpu_config cfg = guest_vcpu_config(p);
    open_vcpus(&cfg);
    guest_setup(p, &q, ref);
    snapshot_take(&snap, ref);
    ref_logs.stores_on = logs.stores_on = false;
    struct ck_source src = {0};
    struct kick_at_entry hook = {0};
    struct t1h_engine_vcpu ev;
    struct check_counts a, b;
    struct t1_stats sa, sb;
    struct vcpu_exit e;
    unsigned yields, attempt;
    char test[80];
    snprintf(test, sizeof(test), "kick checked vector %#x %s", vector,
             at_entry ? "entry" : "replay");
    t1h_watchdog_start(test, t1h_engine_checked()->name);
    for (attempt = 0;; ++attempt) {
        t1h_watchdog_trial();
        for (unsigned i = 0; i < 4; ++i)
            __atomic_store_n(&mailbox[i], 0, __ATOMIC_RELEASE);
        tier0_flush();
        t1_flush();
        t1h_engine_bind(&ev, t1h_engine_checked(), v);
        snapshot_restore(&snap, v);
        t1h_engine_restored(&ev);
        struct t1_compile_options opt = {
            T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG | (at_entry ? T1_COMPILE_VERIFY : 0), 0, 0};
        t1_test_set_compile_options(t1h_tier1_vcpu(ev.ctx), &opt);
        t1_test_set_store_observer(NULL, NULL);
        a = check_counts();
        t1_get_stats(&sa);
        pthread_t thread;
        if (at_entry) {
            hook = (struct kick_at_entry){v, vector, 50, false};
            t1_test_set_entry_hook(kick_entry_hook, &hook);
        } else {
            src = (struct ck_source){
                v, entries_total(), 100 + 37 * (attempt % 16), vector, 0, false, false, false, 0,
                0};
            CHECK(!pthread_create(&thread, NULL, ck_source_thread, &src));
        }
        yields = 0;
        uint64_t start = v->retired;
        for (;;) {
            drain(v);
            e = t1h_checked_slice(ev.ctx, v, BIG_BUDGET - (v->retired - start));
            if (!t1h_kick_yield(&e))
                break;
            ++yields;
        }
        if (at_entry)
            t1_test_set_entry_hook(NULL, NULL);
        else {
            __atomic_store_n(&src.stop, 1, __ATOMIC_RELEASE);
            CHECK(!pthread_join(thread, NULL));
        }
        t1h_checked_release(v);
        b = check_counts();
        t1_get_stats(&sb);
        /* A divergence is a failure whatever the source did. */
        CHECK(b.divergences == a.divergences);
        CHECK(sb.entries > sa.entries && "kick no Tier1 entries");
        if (at_entry) {
            CHECK(hook.fired);
            break;
        }
        t1h_watchdog_source(src.acted, src.in_check, src.delay_us, src.before, src.after);
        if (src.acted && !t1h_bank0(v, 24))
            t1h_watchdog_undelivered();
        if (src.acted && src.in_check)
            break;
        t1h_engine_release(&ev);
    }
    CHECK(e.reason == VCPU_EXIT_DEBUG && logs.last_break_imm == T1H_BREAK_END);
    CHECK(yields == 1);
    CHECK(t1h_bank0(v, 24) == 1 && t1h_bank0(v, 26) == vector);
    uint64_t at = t1h_bank0(v, 25) - v->clock_offset;
    CHECK(b.unchecked == a.unchecked && b.checked - a.checked == b.entries - a.entries);
    /* (a): the execution that began with the kick pending left at its
     * event checkpoint before its budget ran low. */
    CHECK(!at_entry || sb.exits_by_kind[T1_PT_CHECKPOINT] > sa.exits_by_kind[T1_PT_CHECKPOINT]);
    static struct schedule sched;
    schedule_clear(&sched);
    schedule_add(&sched, (struct injection){.at = at, .kind = INJ_RAISE, .address = vector});
    snapshot_restore(&snap, ref);
    struct vcpu_exit re = ref_run(ref, BIG_BUDGET, &sched);
    CHECK(schedule_complete(&sched));
    CHECK(re.reason == VCPU_EXIT_DEBUG && ref_logs.last_break_imm == T1H_BREAK_END);
    result_take(&reference, &re, ref);
    result_take(&actual, &e, v);
    CHECK(result_compare(&reference, &actual, false, "kick checked") == 0);
    t1h_watchdog_progress();
    t1h_watchdog_stop();
    printf("checked: kick vector %#x published and kicked %s (attempt %u): one yield, delivered at "
           "retired %llu as the reference; %llu Tier1 executions checked, 0 divergences\n",
           vector,
           at_entry ? "as the 50th Tier1 execution began"
                    : "while the checker replayed an execution",
           attempt, (unsigned long long)at, (unsigned long long)(b.checked - a.checked));
    t1h_engine_release(&ev);
    close_program();
}

void pairing_tests_suite(void)
{
    pairing_tests();
}
void checked_suite(void)
{
    checked_tests();
}
void undo_suite(void)
{
    undo_tests();
}
void compile_ticks_suite(void)
{
    compile_ticks_tests();
}

void ordered_suite(void)
{
    uint32_t flags = t1h_tier0t1_flags();
    t1h_tier0t1_set_flags(flags | T1_COMPILE_ACCESS_LOG);
    for (unsigned k = 0; k < sizeof(checked_programs) / sizeof(checked_programs[0]); ++k)
        checked_case(checked_programs[k]);
    unmap_case();
    t1h_tier0t1_set_flags(flags);
}
