/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The product path (product dispatch): suite "upper" of build/tier1-test, on the `tier0+tier1`
 * engine (engine_tier0t1.h). Every run is compared with the stepping
 * reference under the comparison relation.
 *
 *   forward-progress  a loop whose header is a load from an
 *                     MMIO register (each read returns the reads so far), 10^4
 *                     iterations: with an explicitly compiled region every entry
 *                     leaves before effect (RAN_PENDING) and Tier0 executes the
 *                     load exactly once; with a region compiled by hotness the
 *                     site's exits demote it and hold the head. A NaT-guard
 *                     decline (every entry; by hotness: the NaT demotion), and a
 *                     budget decline (budget < max_path). A watchdog thread
 *                     requests an exit if a run makes no progress.
 *   tag-keep          site tags kept across entries while the
 *                     bind epoch is unchanged: repeated entries into one loop have
 *                     no warm miss; two loops alternating with a shared and a
 *                     disjoint tag; a third loop rebinding every slot to another
 *                     page at the same payload offset between entries (ABA across
 *                     slot reuse: the old pointer would read the other page).
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime, nanosleep under -std=c11 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include "core/log_internal.h"
#include "tests/hal/hal_linux.h"
#include "tests/tier1/engine_tier0t1.h"
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/programs.h"
#include "tests/tier1/provider.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/schedule.h"
#include "tests/tier1/results.h"
#include "tests/tier1/snapshot.h"
#include "tests/tier1/watchdog.h"
#include "tests/tier1/storelog.h"
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"
#include "engine/hostwin/hostwin.h"
#include "engine/tier0/tier0.h"
#include "engine/cache/cache.h"
#include "ia64/tlb.h"
#include "ia64/semantics.h"
#include "ia64/smp.h"
#include "ia64/decode/decode.h"
#include "ia64/interrupt.h"
#include "engine/interp/interp.h"
#include "ia64/debug.h"
#include "ia64/fp.h"
#include "ia64vmm/guest_mem.h"

#define BIG_BUDGET UINT64_C(50000000)
#define VRN(r) ((uint64_t)(r) << 61)

static unsigned comparisons;

/* ---- the machine: a reference and a test vCPU from one snapshot -------- */

struct machine {
    struct vcpu *ref, *v;
    struct t1h_logs ref_logs, logs;
    struct snapshot snap;
    struct run_result reference, actual;
};
static struct machine M;

static void machine_open(const char *image, const struct ia64_entry_regs *regs)
{
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    M.ref = vcpu_create(&cfg);
    M.v = vcpu_create(&cfg);
    T1H_CHECK(M.ref && M.v);
    t1h_bind(M.ref, &M.ref_logs);
    t1h_bind(M.v, &M.logs);
    M.ref_logs.stores_on = M.logs.stores_on = false;
    t1h_load_image(image);
    vcpu_reset(M.ref, regs);
    M.ref->s.cr[CR_IVA] = T1H_IVT;
}

static void machine_close(void)
{
    t1h_unbind(M.ref);
    t1h_unbind(M.v);
    vcpu_destroy(M.ref);
    vcpu_destroy(M.v);
    M.ref = M.v = NULL;
}

static void compare_or_fail(const struct vcpu_exit *e, const char *label)
{
    result_take(&M.actual, e, M.v);
    if (result_compare(&M.reference, &M.actual, false, label)) {
        fprintf(stderr, "upper: %s differs from the reference\n", label);
        T1H_CHECK(!"product-path run differs from the reference");
    }
    ++comparisons;
}

/* A run that makes no progress never returns by itself: after `seconds`
 * the watchdog requests an exit (VCPU_EXIT_DEBUG without the end marker). */
struct watchdog {
    struct vcpu *v;
    unsigned seconds;
    int done;
    bool fired;
};
static void *watchdog_thread(void *arg)
{
    struct watchdog *w = arg;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (__atomic_load_n(&w->done, __ATOMIC_ACQUIRE))
            return NULL;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec >= (time_t)w->seconds)
            break;
        struct timespec nap = {0, 1000000};
        nanosleep(&nap, NULL);
    }
    w->fired = true;
    vcpu_request_exit(w->v, VCPU_EXIT_DEBUG);
    return NULL;
}
static struct vcpu_exit guarded_run(struct t1h_engine_vcpu *ev, uint64_t budget, unsigned seconds,
                                    bool *fired)
{
    struct watchdog w = {ev->v, seconds, 0, false};
    pthread_t thread;
    T1H_CHECK(!pthread_create(&thread, NULL, watchdog_thread, &w));
    struct vcpu_exit e = t1h_engine_run(ev, budget);
    __atomic_store_n(&w.done, 1, __ATOMIC_RELEASE);
    T1H_CHECK(!pthread_join(thread, NULL));
    *fired = w.fired;
    return e;
}

static struct t1_stats stats_now(void)
{
    struct t1_stats s;
    t1_get_stats(&s);
    return s;
}
#define DELTA(a, b, f) ((b).f - (a).f)

/* ---- the counting MMIO register (forward-progress) ---------------------------- */

#define COUNTER_PA UINT64_C(0x600000)
static uint64_t counter_reads;
static uint64_t counter_read(void *opaque, struct vcpu *v, uint64_t offset, unsigned size)
{
    (void)opaque;
    (void)v;
    (void)offset;
    (void)size;
    return ++counter_reads;
}
static void counter_write(void *opaque, struct vcpu *v, uint64_t offset, uint64_t value,
                          unsigned size)
{
    (void)opaque;
    (void)v;
    (void)offset;
    (void)value;
    (void)size;
}
static const struct gm_mmio_ops counter_ops = {counter_read, counter_write};

static void counter_init(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    T1H_CHECK(!gm_lookup(COUNTER_PA));
    T1H_CHECK(gm_add_region(&(struct gm_region){.base = COUNTER_PA,
                                                .size = 4096,
                                                .kind = GM_EMULATED,
                                                .ops = &counter_ops,
                                                .name = "upper-counter"}));
}

/* ---- forward-progress -------------------------------------------------------- */

#define PROGRESS_N 10000u

/* The machine at the loop header of `image` (second arrival), the
 * reference run to the end, and the reads the device saw from the header. */
static uint64_t progress_open(const char *image, const struct ia64_entry_regs *regs,
                              uint64_t *header, uint64_t *reads_at_header)
{
    machine_open(image, regs);
    counter_reads = 0;
    T1H_CHECK(t1h_run_to_loop_header(M.ref, 100000, header));
    *reads_at_header = counter_reads;
    snapshot_take(&M.snap, M.ref);
    struct vcpu_exit e = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    result_take(&M.reference, &e, M.ref);
    return counter_reads - *reads_at_header;
}

/* One run of `tier0+tier1` from the header snapshot with production
 * regions; explicit: the region at the header compiled first by t1_compile
 * (no demotion applies), else by Tier0's hot(). */
static struct vcpu_exit progress_run(bool explicit, uint64_t reads_at_header, const char *label)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_tier0t1_set_flags(0);
    /* Fresh Tier0 regions (entries, links) and Tier1 regions: each case
     * starts the hotness schedule from zero. */
    tier0_flush();
    t1_flush();
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    counter_reads = reads_at_header;
    if (explicit) {
        struct t1_compile_report rep;
        struct t1_compile_options opt = {0};
        T1H_CHECK(t1h_provider_real()->compile(ev.ctx, &opt, &rep) == T1_COMPILED);
    }
    bool fired;
    struct vcpu_exit e = guarded_run(&ev, BIG_BUDGET, 20, &fired);
    if (fired) {
        fprintf(stderr, "upper: %s: no progress (watchdog after 20 s at %#llx:%u, retired %llu)\n",
                label, (unsigned long long)M.v->s.ip, M.v->s.slot,
                (unsigned long long)M.v->retired);
        T1H_CHECK(!"forward-progress: the run made no progress");
    }
    T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && M.logs.last_break_imm == T1H_BREAK_END);
    compare_or_fail(&e, label);
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    return e;
}

static void forward_progress_mmio(void)
{
    counter_init();
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[2] = COUNTER_PA;
    regs.gr[8] = PROGRESS_N - 1;
    uint64_t header, at_header;
    uint64_t reads = progress_open("mmio_progress.bin", &regs, &header, &at_header);
    T1H_CHECK(reads + at_header == PROGRESS_N);
    /* The guest's sum: every value 1..n read exactly once. */
    uint64_t sum = (uint64_t)PROGRESS_N * (PROGRESS_N + 1) / 2;
    T1H_CHECK(M.reference.v.s.gr[14] == sum);

    /* An explicitly compiled region: every entry leaves at the MMIO load. */
    t1_flush();
    struct t1_stats s0 = stats_now();
    progress_run(true, at_header, "forward-progress mmio explicit");
    struct t1_stats s1 = stats_now();
    T1H_CHECK(counter_reads == PROGRESS_N && M.v->s.gr[14] == sum);
    uint64_t pending = DELTA(s0, s1, upper_ran_pending), entries = DELTA(s0, s1, entries);
    T1H_CHECK(pending + 2 >= PROGRESS_N && entries == pending && DELTA(s0, s1, tier1_retired) == 0);
    T1H_CHECK(DELTA(s0, s1, exits_by_kind[T1_PT_SITE]) == pending && DELTA(s0, s1, demotions) == 0);
    printf(
        "upper: forward-progress mmio, explicit region: %llu Tier1 entries, each left before the MMIO load "
        "(RAN_PENDING) and Tier0 executed it: %u reads, each once, retired %llu as the reference\n",
        (unsigned long long)entries, PROGRESS_N, (unsigned long long)M.v->retired);

    /* Compiled by hotness: the site's exits demote the region and hold its
     * head; holds expire and the head is compiled and demoted again. */
    t1_flush();
    s0 = stats_now();
    progress_run(false, at_header, "forward-progress mmio hot");
    s1 = stats_now();
    T1H_CHECK(counter_reads == PROGRESS_N && M.v->s.gr[14] == sum);
    uint64_t demoted = DELTA(s0, s1, demote_site);
    entries = DELTA(s0, s1, entries);
    if (getenv("T1H_UPPER_TRACE"))
        fprintf(
            stderr,
            "hot: entries %llu pending %llu demote_site %llu holds %llu hot_calls %llu linked %llu "
            "held %llu rejected %llu compiles %llu decline_link %llu enters %llu\n",
            (unsigned long long)entries, (unsigned long long)DELTA(s0, s1, upper_ran_pending),
            (unsigned long long)demoted, (unsigned long long)DELTA(s0, s1, holds),
            (unsigned long long)DELTA(s0, s1, hot_calls),
            (unsigned long long)DELTA(s0, s1, hot_linked),
            (unsigned long long)DELTA(s0, s1, hot_held),
            (unsigned long long)DELTA(s0, s1, hot_rejected),
            (unsigned long long)DELTA(s0, s1, compiles),
            (unsigned long long)DELTA(s0, s1, decline_link),
            (unsigned long long)DELTA(s0, s1, upper_enters));
    T1H_CHECK(demoted >= 1 && DELTA(s0, s1, holds) == demoted && DELTA(s0, s1, hot_held) >= 1);
    /* A head that keeps exiting backs off: 3 compile/demote cycles at 10^4 reads before the Tier0
     * hotness restart, 5 after; 10^6 reads: 4 and 6. A flat schedule would make hundreds. */
    T1H_CHECK(demoted <= 8);
    T1H_CHECK(entries == DELTA(s0, s1, upper_ran_pending) && entries == demoted * 17 &&
              entries < PROGRESS_N / 10);
    printf(
        "upper: forward-progress mmio, by hotness: %llu Tier1 entries, %llu site demotions (17 exits each), %llu "
        "hot() calls answered by a hold, %u reads, each once\n",
        (unsigned long long)entries, (unsigned long long)demoted,
        (unsigned long long)DELTA(s0, s1, hot_held), PROGRESS_N);
    machine_close();
}

static void forward_progress_nat(void)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[20] = 0x1234;
    regs.gr[8] = PROGRESS_N - 1;
    uint64_t header;
    /* vcpu_reset clears every NaT: set r20's after the reset. */
    machine_open("nat_guard_decline.bin", &regs);
    M.ref->s.nat[20] = true;
    T1H_CHECK(t1h_run_to_loop_header(M.ref, 100000, &header));
    snapshot_take(&M.snap, M.ref);
    struct vcpu_exit e = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    T1H_CHECK(M.ref->s.nat[14]);
    result_take(&M.reference, &e, M.ref);

    t1_flush();
    struct t1_stats s0 = stats_now();
    progress_run(true, 0, "forward-progress NaT guard explicit");
    struct t1_stats s1 = stats_now();
    uint64_t declined = DELTA(s0, s1, decline_nat);
    T1H_CHECK(declined + 2 >= PROGRESS_N && DELTA(s0, s1, upper_enters) == declined &&
              DELTA(s0, s1, entries) == 0);
    printf(
        "upper: forward-progress NaT guard, explicit region: %llu entries declined by the guard (r20 NaT), Tier0 "
        "ran the loop, equal to the reference\n",
        (unsigned long long)declined);

    t1_flush();
    s0 = stats_now();
    progress_run(false, 0, "forward-progress NaT guard hot");
    s1 = stats_now();
    declined = DELTA(s0, s1, decline_nat);
    uint64_t demoted = DELTA(s0, s1, demote_nat);
    if (getenv("T1H_UPPER_TRACE"))
        fprintf(
            stderr,
            "nat hot: declined %llu demote_nat %llu holds %llu hot_calls %llu linked %llu held %llu "
            "rejected %llu compiles %llu rejects %llu decline_link %llu enters %llu\n",
            (unsigned long long)declined, (unsigned long long)demoted,
            (unsigned long long)DELTA(s0, s1, holds), (unsigned long long)DELTA(s0, s1, hot_calls),
            (unsigned long long)DELTA(s0, s1, hot_linked),
            (unsigned long long)DELTA(s0, s1, hot_held),
            (unsigned long long)DELTA(s0, s1, hot_rejected),
            (unsigned long long)DELTA(s0, s1, compiles), (unsigned long long)DELTA(s0, s1, rejects),
            (unsigned long long)DELTA(s0, s1, decline_link),
            (unsigned long long)DELTA(s0, s1, upper_enters));
    T1H_CHECK(demoted >= 1 && declined == demoted * 129 && DELTA(s0, s1, entries) == 0);
    printf(
        "upper: forward-progress NaT guard, by hotness: %llu declines, %llu NaT demotions (129 failures of 256 "
        "attempts each), equal to the reference\n",
        (unsigned long long)declined, (unsigned long long)demoted);
    machine_close();
}

/* chk.s on r20 with its NaT bit set and on f6 holding NaTVal at every
 * entry (programs/chk_s_nat.S): the region at the header reads them at run time,
 * propagates r20's bit through an add, clears both targets of a cmp on it
 * (SDM Vol.3 add, cmp) and leaves at each chk.s to its recovery code (SDM
 * Vol.3 chk); no entry is declined at the NaT guard and every one ran Tier1
 * code. Every iteration leaves at the GR's chk.s at most once, so more NaT
 * exits than iterations include exits at the FR's. */
static void forward_progress_chk(void)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[20] = 0x1234;
    regs.gr[8] = PROGRESS_N - 1;
    uint64_t header;
    machine_open("chk_s_nat.bin", &regs);
    M.ref->s.nat[20] = true;
    ia64_fr_write(&M.ref->s, 6, ia64_fp_nat());
    T1H_CHECK(t1h_run_to_loop_header(M.ref, 100000, &header));
    snapshot_take(&M.snap, M.ref);
    struct vcpu_exit e = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    T1H_CHECK(M.ref->s.gr[14] == PROGRESS_N && M.ref->s.gr[15] == PROGRESS_N &&
              M.ref->s.gr[19] == PROGRESS_N && M.ref->s.nat[16] && !M.ref->s.gr[17] &&
              !M.ref->s.gr[18]);
    result_take(&M.reference, &e, M.ref);
    t1_flush();
    struct t1_stats s0 = stats_now();
    progress_run(true, 0, "forward-progress chk.s on an entry NaT");
    struct t1_stats s1 = stats_now();
    uint64_t entries = DELTA(s0, s1, entries), nat_exits = DELTA(s0, s1, exits_by_end[T1_END_NAT]);
    if (DELTA(s0, s1, decline_nat) || entries + 2 < PROGRESS_N || nat_exits <= PROGRESS_N)
        fprintf(
            stderr,
            "upper: forward-progress chk.s: %llu entries, %llu NaT declines, exits limit/branch/nat %llu/%llu/%llu, "
            "upper enters %llu\n",
            (unsigned long long)entries, (unsigned long long)DELTA(s0, s1, decline_nat),
            (unsigned long long)DELTA(s0, s1, exits_by_end[T1_END_LIMIT]),
            (unsigned long long)DELTA(s0, s1, exits_by_end[T1_END_BRANCH]),
            (unsigned long long)nat_exits, (unsigned long long)DELTA(s0, s1, upper_enters));
    T1H_CHECK(!DELTA(s0, s1, decline_nat) && entries + 2 >= PROGRESS_N && nat_exits > PROGRESS_N);
    printf(
        "upper: forward-progress chk.s, explicit region: %llu entries with r20's NaT and f6's NaTVal set, %llu left "
        "to the recovery code, 0 declined at the NaT guard, equal to the reference\n",
        (unsigned long long)entries, (unsigned long long)nat_exits);
    machine_close();
}

/* br.ret to a less privileged level (programs/return_cpl.S; SDM Vol.3 br.ret:
 * PSR.cpl = PFS.ppl when PFS.ppl > PSR.cpl): the region at the header
 * follows the call and returns natively while PFS.ppl = 0; at the
 * iteration whose callee sets PFS.ppl = 3 it leaves before the br.ret
 * (return class) and the lower tier returns at cpl 3. */
static void forward_progress_retpl(void)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[8] = PROGRESS_N - 1;
    regs.gr[9] = PROGRESS_N / 2;
    uint64_t header;
    machine_open("return_cpl.bin", &regs);
    T1H_CHECK(t1h_run_to_loop_header(M.ref, 100000, &header));
    snapshot_take(&M.snap, M.ref);
    struct vcpu_exit e = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    T1H_CHECK(M.ref->s.gr[14] == PROGRESS_N && M.ref->s.gr[15] == PROGRESS_N &&
              ((M.ref->s.psr >> 32) & 3) == 3);
    result_take(&M.reference, &e, M.ref);
    t1_flush();
    struct t1_stats s0 = stats_now();
    progress_run(true, 0, "forward-progress return to cpl 3");
    struct t1_stats s1 = stats_now();
    uint64_t ret_exits = DELTA(s0, s1, exits_by_end[T1_END_RET]);
    T1H_CHECK(ret_exits >= 1 && DELTA(s0, s1, tier1_retired) >= PROGRESS_N);
    printf(
        "upper: forward-progress return to cpl 3, explicit region: %llu Tier1 instructions, %llu exits before a "
        "br.ret, PSR.cpl 3 at the end, equal to the reference\n",
        (unsigned long long)DELTA(s0, s1, tier1_retired), (unsigned long long)ret_exits);
    machine_close();
}

/* A budget below the region's max_path declines; Tier0 retires exactly the
 * budget. */
static void forward_progress_budget(void)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[20] = 0x1234;
    regs.gr[8] = 99;
    machine_open("nat_guard_decline.bin", &regs);
    uint64_t header;
    T1H_CHECK(t1h_run_to_loop_header(M.ref, 100000, &header));
    snapshot_take(&M.snap, M.ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_tier0t1_set_flags(0);
    t1_flush();
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    struct t1_compile_report rep;
    struct t1_compile_options opt = {0};
    T1H_CHECK(t1h_provider_real()->compile(ev.ctx, &opt, &rep) == T1_COMPILED);
    const struct t1_region *r = t1_region_get(rep.region_id);
    T1H_CHECK(r && r->max_path > 1);
    unsigned checked = 0;
    for (uint64_t k = 1; k < r->max_path; ++k) {
        snapshot_restore(&M.snap, M.v);
        t1h_engine_restored(&ev);
        snapshot_restore(&M.snap, M.ref);
        struct vcpu_exit re = ref_run(M.ref, k, NULL);
        result_take(&M.reference, &re, M.ref);
        struct t1_stats s0 = stats_now();
        struct vcpu_exit e = t1h_engine_run(&ev, k);
        struct t1_stats s1 = stats_now();
        T1H_CHECK(M.v->retired == M.snap.v.retired + k);
        T1H_CHECK(DELTA(s0, s1, decline_budget) >= 1 && DELTA(s0, s1, entries) == 0);
        char label[64];
        snprintf(label, sizeof(label), "forward-progress budget %llu", (unsigned long long)k);
        compare_or_fail(&e, label);
        ++checked;
    }
    printf(
        "upper: forward-progress budget: %u budgets below max_path %u at the header declined, Tier0 retired exactly "
        "each budget\n",
        checked, r->max_path);
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    machine_close();
}

/* ---- tag-keep ---------------------------------------------------------- */

/* One window offset for every page, so a slot's host address for A's page
 * is the same as for any page bound to that slot later (ABA). */
#define TAG_OFFSET UINT64_C(0x200008)
#define TAGS_C 8u
static uint64_t tag_va(unsigned vrn, unsigned high)
{
    return VRN(vrn) | ((uint64_t)high << 40) | TAG_OFFSET;
}
/* A, B, S, then C1..C8: VRN 1, 2, 3; VRN 4..7 with bit 40 clear and set. */
static uint64_t tagkeep_va(unsigned k)
{
    return k < 3 ? tag_va(1 + k, 0) : tag_va(4 + (k - 3) % 4, (k - 3) / 4);
}
static gpa_t tagkeep_pa(unsigned k)
{
    return T1H_TABLE + k * UINT64_C(4096) + (TAG_OFFSET & 4095);
}
static uint64_t tagkeep_word(unsigned k)
{
    return UINT64_C(0x1000000) << (k % 24) | (k + 1);
}

static void tc_insert(struct vcpu *v, uint64_t va, gpa_t pa)
{
    ia64_tc_insert(v, va, 1, (pa & ~UINT64_C(4095)) | 1 | 0x20 | 0x40 | (UINT64_C(2) << 9),
                   12u << 2);
}

static void tagkeep_open(unsigned phase, unsigned inner, unsigned outer)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE,
        .psr = IA64_PSR_IC | IA64_PSR_BN | IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_RT,
        .bsp = T1H_BSP,
        .bspstore = T1H_BSP,
    };
    regs.gr[2] = tagkeep_va(0);
    regs.gr[3] = tagkeep_va(1);
    regs.gr[4] = tagkeep_va(2);
    regs.gr[8] = inner - 1;
    regs.gr[9] = phase;
    regs.gr[10] = outer;
    machine_open("tag_keep.bin", &regs);
    struct vcpu *v = M.ref;
    for (unsigned k = 0; k < 3 + TAGS_C; ++k) {
        uint64_t x = tagkeep_word(k);
        memcpy(t1h_ram() + (tagkeep_pa(k) - T1H_RAM_BASE), &x, 8);
    }
    code_notify_vmm_write(T1H_TABLE, (3 + TAGS_C) * UINT64_C(4096));
    for (unsigned r = 0; r < 8; ++r)
        v->s.rr[r] = ((UINT64_C(0x200) + r) << 8) | (12u << 2);
    /* Code and IVT: ITR0, VRN 0, 64 KiB identity. */
    v->s.tr[0][0] = (struct ia64_tlb_entry){.va = T1H_CODE,
                                            .pte = T1H_CODE | 1 | 0x20 | 0x40 | (UINT64_C(3) << 9),
                                            .itir = 16u << 2,
                                            .rid = 0x200,
                                            .page_shift = 16,
                                            .valid = true};
    for (unsigned k = 0; k < 3 + TAGS_C; ++k)
        tc_insert(v, tagkeep_va(k), tagkeep_pa(k));
    snapshot_take(&M.snap, v);
}

/* The reference to the end; with marks: retired at every arrival at the
 * outer loop's head (T1H_CODE + 0x10, after the first bundle). */
static unsigned tagkeep_reference(uint64_t *marks, unsigned max)
{
    unsigned n = 0;
    snapshot_restore(&M.snap, M.ref);
    struct vcpu_exit e;
    for (;;) {
        if (marks && M.ref->s.ip == T1H_CODE + 0x10 && M.ref->s.slot == 0 && n < max)
            marks[n++] = M.ref->retired;
        e = ref_run(M.ref, 1, NULL);
        if (e.reason != VCPU_EXIT_BUDGET)
            break;
    }
    T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    result_take(&M.reference, &e, M.ref);
    return n;
}

struct tagkeep_counts {
    uint64_t entries, misses, binds, rebinds;
};
static struct tagkeep_counts tagkeep_delta(const struct t1_stats *a, const struct t1_stats *b,
                                           const struct hostwin_stats *ha,
                                           const struct hostwin_stats *hb)
{
    return (struct tagkeep_counts){b->entries - a->entries, b->window_misses - a->window_misses,
                                   hb->binds - ha->binds, hb->rebinds - ha->rebinds};
}

/* The sites of every live region entered since the flush: with a slot
 * range per region and no rebinding, the most window misses a run can
 * take (one cold miss per site), however many entries it makes. */
static uint64_t entered_sites(void)
{
    uint64_t n = 0;
    for (uint32_t id = 1; id <= t1_cfg.max_regions; ++id) {
        const struct t1_region *r = t1_region_get(id);
        if (r && r->valid && r->entries)
            n += r->nsites;
    }
    return n;
}

static void t_tag_keep(void)
{
    static const char *const engines[] = {"tier0+tier1", "tier1"};
    for (unsigned which = 0; which < 2; ++which) {
        const struct t1h_engine *engine = t1h_engine_named(engines[which]);
        /* Production regions (verification regions reset their sites at
         * every entry), at the production thresholds: only the two loops
         * become regions. */
        t1_test_set_thresholds(64, 1024);
        for (unsigned phase = 0; phase < 3; ++phase) {
            unsigned outer = 40, inner = 16;
            tagkeep_open(phase == 1 ? 1 : 0, inner, outer);
            static uint64_t marks[64];
            unsigned nmarks = tagkeep_reference(marks, 64);
            T1H_CHECK(nmarks == outer);
            uint64_t sum = 0;
            for (unsigned i = 0; i < outer * inner; ++i)
                sum += tagkeep_word(0) + tagkeep_word(2) +
                       (phase == 1 ? tagkeep_word(1) + tagkeep_word(2) : 0);
            T1H_CHECK(M.reference.v.s.gr[14] == sum);
            tier0_flush();
            t1_flush();
            struct t1h_engine_vcpu ev;
            t1h_engine_bind(&ev, engine, M.v);
            t1h_tier1_set_flags(0);
            t1h_tier0t1_set_flags(0);
            snapshot_restore(&M.snap, M.v);
            t1h_engine_restored(&ev);
            struct hostwin_vcpu *hw = t1h_tier1_hostwin(ev.ctx);
            struct t1_stats s0 = stats_now(), s1;
            struct hostwin_stats h0, h1;
            hostwin_get_stats(hw, &h0);
            struct vcpu_exit e;
            uint64_t aba = 0, aba_entries = 0;
            if (phase < 2) {
                e = t1h_engine_run(&ev, BIG_BUDGET);
            } else {
                /* Between two runs (no Tier1 code active): rebind every
                 * slot to C1..C8 and map their pages at the offset A's
                 * slot used, as another region's misses would. */
                for (unsigned i = 1; i < nmarks; ++i) {
                    struct t1_stats r0 = stats_now();
                    e = t1h_engine_run(&ev, marks[i] - M.v->retired);
                    struct t1_stats r1 = stats_now();
                    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && M.v->retired == marks[i]);
                    /* A run after a rebinding that entered A missed on both
                     * of A's sites: the entry found the epoch moved. */
                    if (i > 1 && r1.entries > r0.entries) {
                        T1H_CHECK(r1.window_misses - r0.window_misses >= 2);
                        ++aba_entries;
                    }
                    for (unsigned k = 3; k < 3 + TAGS_C; ++k) {
                        struct hostwin_tag tmp;
                        enum hostwin_exit_reason why;
                        T1H_CHECK(hostwin_bind(hw, M.v, tagkeep_va(k), &tmp) == HOSTWIN_OK);
                        T1H_CHECK(hostwin_resolve(hw, M.v, tagkeep_va(k), 8, false, &why) ==
                                  HOSTWIN_MAPPED);
                        ++aba;
                    }
                }
                e = t1h_engine_run(&ev, BIG_BUDGET);
            }
            s1 = stats_now();
            hostwin_get_stats(hw, &h1);
            struct tagkeep_counts c = tagkeep_delta(&s0, &s1, &h0, &h1);
            static const char *const names[] = {"one loop re-entered", "two loops alternating",
                                                "slots rebound between entries (ABA)"};
            char label[96];
            snprintf(label, sizeof(label), "tag-keep %s/%s", names[phase], engine->name);
            T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && M.logs.last_break_imm == T1H_BREAK_END);
            compare_or_fail(&e, label);
            if (getenv("T1H_UPPER_TRACE"))
                fprintf(stderr,
                        "tag-keep %s/%s: entries %llu misses %llu binds %llu rebinds %llu\n",
                        names[phase], engine->name, (unsigned long long)c.entries,
                        (unsigned long long)c.misses, (unsigned long long)c.binds,
                        (unsigned long long)c.rebinds);
            T1H_CHECK(c.entries >= outer / 2);
            uint64_t sites = entered_sites();
            if (phase == 0) {
                /* Repeated entries into one loop: only cold misses, one per
                 * site of each region (the product path compiles only the
                 * loop; t1_run also enters a tail region with the loads). */
                T1H_CHECK(c.binds == 2 && c.rebinds == 0 && c.misses >= 2 && c.misses <= sites &&
                          (which == 1 || c.misses == 2));
            } else if (phase == 1) {
                /* A and B alternate: each region's sites use their own tag
                 * slots, so alternation costs no miss: only the cold misses
                 * (on the product path the two sites of each loop); three
                 * windows are bound, nothing is rebound. */
                T1H_CHECK(c.binds == 3 && c.rebinds == 0 && c.misses >= 4 && c.misses <= sites &&
                          (which == 1 || c.misses == 4) && c.misses < c.entries);
            } else {
                /* Every entry after a rebinding finds the epoch moved and
                 * misses on both sites: none uses the old pointer, which
                 * would read C's page (the final sum shows it). */
                T1H_CHECK(c.rebinds >= aba && aba_entries >= nmarks / 2);
            }
            printf(
                "upper: tag-keep %-36s %-11s: %llu entries, %llu window misses, %llu binds, %llu rebinds%s\n",
                names[phase], engine->name, (unsigned long long)c.entries,
                (unsigned long long)c.misses, (unsigned long long)c.binds,
                (unsigned long long)c.rebinds, phase == 2 ? ", sums equal" : "");
            t1h_tier1_set_flags(T1_COMPILE_VERIFY);
            t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
            t1h_engine_release(&ev);
            machine_close();
        }
        t1_test_set_thresholds(0, 0);
    }
}

/* ---- interrupt kicks on the product path ---------------------------
 *
 * A platform interrupt source publishes a vector in its route's mailbox and
 * calls vcpu_kick (platform/interrupts.c inject(): MSI messages and IOSAPIC
 * INTx messages alike); the executor drains the mailbox before each engine
 * run (platform_irq_poll, core/vm.c). The engine's part: a Tier1 region
 * leaves at its next event checkpoint, Tier0's loop top consumes the kick
 * as a budget-like yield after any requested exit, and the next run
 * delivers. The tests model the source and the executor with the same
 * two steps (mailbox + vcpu_kick; drain with vcpu_raise_external). */

static uint64_t mailbox[4];
static void publish(struct vcpu *v, unsigned vector)
{
    __atomic_fetch_or(&mailbox[vector / 64], UINT64_C(1) << (vector % 64), __ATOMIC_SEQ_CST);
    vcpu_kick(v);
}
static void drain(struct vcpu *v)
{
    for (unsigned i = 0; i < 4; ++i)
        for (uint64_t bits = __atomic_exchange_n(&mailbox[i], 0, __ATOMIC_ACQ_REL); bits;
             bits &= bits - 1)
            vcpu_raise_external(v, 64 * i + (unsigned)__builtin_ctzll(bits));
}
/* The executor's loop for one slice: drain, run, and on a kick yield
 * drain and run again; *yields counts the yields. */
static struct vcpu_exit executor(struct t1h_engine_vcpu *ev, uint64_t budget, unsigned *yields)
{
    uint64_t start = ev->v->retired;
    for (;;) {
        drain(ev->v);
        struct vcpu_exit e = t1h_tier0t1_slice(ev->ctx, ev->v, budget - (ev->v->retired - start));
        if (!t1h_kick_yield(&e))
            return e;
        ++*yields;
    }
}

/* A source thread: once Tier1 code runs (an entry counted), wait `delay_us`
 * and act. */
enum source_action { SRC_PUBLISH, SRC_KICK_THEN_EXIT, SRC_EXIT_THEN_KICK };
struct source {
    struct vcpu *v;
    uint64_t entries0;
    unsigned delay_us, vector;
    enum source_action action;
    int stop;
    bool acted;
    bool during; /* a Tier1 entry was running right after the action */
    uint64_t before, after;
};
/* Tier1 entries of every thread (the counters are per thread). */
static uint64_t entries_total(void)
{
    struct t1_stats s;
    t1_get_stats(&s);
    return s.entries;
}
static void *source_thread(void *arg)
{
    struct source *s = arg;
    while (entries_total() == s->entries0)
        if (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE))
            return NULL;
    /* The delay spins (no sleep: a wake-up can take longer than the loop). */
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        if (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE))
            return NULL;
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((uint64_t)(t.tv_sec - t0.tv_sec) * 1000000000u + (uint64_t)(t.tv_nsec - t0.tv_nsec) <
             (uint64_t)s->delay_us * 1000);
    s->before = __atomic_load_n(&s->v->retired, __ATOMIC_RELAXED);
    if (s->action == SRC_PUBLISH)
        publish(s->v, s->vector);
    else if (s->action == SRC_KICK_THEN_EXIT) {
        vcpu_kick(s->v);
        vcpu_request_exit(s->v, VCPU_EXIT_SHUTDOWN);
    } else {
        vcpu_request_exit(s->v, VCPU_EXIT_SHUTDOWN);
        vcpu_kick(s->v);
    }
    s->during = t1_test_running(s->v);
    s->after = __atomic_load_n(&s->v->retired, __ATOMIC_RELAXED);
    s->acted = true;
    return NULL;
}

/* The cases below need the source's action to land inside a Tier1 entry
 * (the shladd_xor loop runs about a millisecond in Tier1). On a loaded host the
 * source thread may wake only after the entry ended; such an attempt is
 * not the case under test (src.during false), so it is run again from the
 * snapshot and from cold Tier0 and Tier1 caches (no state carried over).
 * A progress watchdog bounds starvation; every attempt that did land is
 * checked in full. */
static unsigned late_attempts;
static void source_reset(void)
{
    for (unsigned i = 0; i < 4; ++i)
        __atomic_store_n(&mailbox[i], 0, __ATOMIC_RELEASE);
}

#define SHLADD_N 2000000u /* 12M instructions: one Tier1 entry runs for milliseconds */

static void shladd_xor_open(bool interrupts_on)
{
    const struct guest_program *p = guest_program("shladd_xor");
    struct guest_params q = guest_completion_params(p);
    q.n = SHLADD_N;
    struct vcpu_config cfg = guest_vcpu_config(p);
    M.ref = vcpu_create(&cfg);
    M.v = vcpu_create(&cfg);
    T1H_CHECK(M.ref && M.v);
    t1h_bind(M.ref, &M.ref_logs);
    t1h_bind(M.v, &M.logs);
    M.ref_logs.stores_on = M.logs.stores_on = false;
    guest_setup(p, &q, M.ref);
    if (!interrupts_on)
        M.ref->s.psr &= ~IA64_PSR_I;
    snapshot_take(&M.snap, M.ref);
}

/* The reference from the snapshot with `vector` raised at retired `at`. */
static void reference_raised(unsigned vector, uint64_t at)
{
    static struct schedule sched;
    schedule_clear(&sched);
    schedule_add(&sched, (struct injection){.at = at, .kind = INJ_RAISE, .address = vector});
    snapshot_restore(&M.snap, M.ref);
    struct vcpu_exit e = ref_run(M.ref, BIG_BUDGET, &sched);
    T1H_CHECK(schedule_complete(&sched));
    T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    result_take(&M.reference, &e, M.ref);
}

static void kick_long_loop(void)
{
    /* MSI-like and INTx-like vectors (the engine sees a mailbox vector and
     * a kick either way), three delays each. */
    static const unsigned vectors[] = {0x41, 0x52};
    unsigned cases = 0;
    for (unsigned vi = 0; vi < 2; ++vi)
        for (unsigned delay = 200; delay <= 600; delay += 200) {
            shladd_xor_open(true);
            struct t1h_engine_vcpu ev;
            struct t1_stats s0, s1;
            struct source src;
            unsigned yields;
            struct vcpu_exit e;
            char test[80];
            snprintf(test, sizeof(test), "kick vector %#x delay %u us", vectors[vi], delay);
            t1h_watchdog_start(test, t1h_engine_tier0t1()->name);
            for (;;) {
                t1h_watchdog_trial();
                source_reset();
                tier0_flush(); /* every attempt from the same cold start */
                t1_flush();
                t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
                t1h_tier0t1_set_flags(0);
                snapshot_restore(&M.snap, M.v);
                t1h_engine_restored(&ev);
                s0 = stats_now();
                src = (struct source){M.v, s0.entries, delay, vectors[vi], SRC_PUBLISH,
                                      0,   false,      false, 0,           0};
                pthread_t thread;
                T1H_CHECK(!pthread_create(&thread, NULL, source_thread, &src));
                yields = 0;
                e = executor(&ev, BIG_BUDGET, &yields);
                __atomic_store_n(&src.stop, 1, __ATOMIC_RELEASE);
                T1H_CHECK(!pthread_join(thread, NULL));
                s1 = stats_now();
                T1H_CHECK(s1.entries > s0.entries && "kick no Tier1 entries");
                t1h_watchdog_source(src.acted, src.during, src.delay_us, src.before, src.after);
                if (src.acted && !t1h_bank0(M.v, 24))
                    t1h_watchdog_undelivered();
                if (src.acted && src.during)
                    break;
                ++late_attempts;
                t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
                t1h_engine_release(&ev);
            }
            T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && M.logs.last_break_imm == T1H_BREAK_END);
            T1H_CHECK(src.acted && yields == 1);
            T1H_CHECK(t1h_bank0(M.v, 24) == 1 && t1h_bank0(M.v, 26) == vectors[vi]);
            uint64_t d = t1h_bank0(M.v, 25) - M.v->clock_offset;
            /* One Tier1 entry ran the loop until the kick; it left at its
             * event checkpoint (the only checkpoint exit of the run: icount
             * mode, the budget never runs low), Tier0 yielded, the next run
             * delivered at that boundary, and after the handler's rfi a
             * second entry ran the loop to its end. */
            if (getenv("T1H_UPPER_TRACE"))
                fprintf(
                    stderr,
                    "kick: checkpoint exits %llu entries %llu exit exits %llu site %llu helper %llu "
                    "d %llu snap %llu yields %u\n",
                    (unsigned long long)DELTA(s0, s1, exits_by_kind[T1_PT_CHECKPOINT]),
                    (unsigned long long)DELTA(s0, s1, entries),
                    (unsigned long long)DELTA(s0, s1, exits_by_kind[T1_PT_EXIT]),
                    (unsigned long long)DELTA(s0, s1, exits_by_kind[T1_PT_SITE]),
                    (unsigned long long)DELTA(s0, s1, exits_by_kind[T1_PT_HELPER]),
                    (unsigned long long)d, (unsigned long long)M.snap.v.retired, yields);
            T1H_CHECK(DELTA(s0, s1, exits_by_kind[T1_PT_CHECKPOINT]) == 1 &&
                      DELTA(s0, s1, entries) == 2 &&
                      DELTA(s0, s1, exits_by_kind[T1_PT_EXIT]) == 1 && d > M.snap.v.retired);
            reference_raised(vectors[vi], d);
            char label[64];
            snprintf(label, sizeof(label), "kick vector %#x delay %u us", vectors[vi], delay);
            compare_or_fail(&e, label);
            ++cases;
            t1h_watchdog_progress();
            t1h_watchdog_stop();
            t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
            t1h_engine_release(&ev);
            machine_close();
        }
    printf(
        "upper: kick %u interrupts published (mailbox + vcpu_kick) during one Tier1 entry of shladd_xor: the "
        "entry left at its event checkpoint, Tier0 yielded once, the next run delivered; equal to the "
        "reference raised at the delivery boundary\n",
        cases);
}

/* The ITM timer (icount) inside the long loop: Tier0 limits the region
 * budget to the match, so the entry leaves before it, Tier0 retires up to
 * it, polls and delivers exactly there. */
static void kick_timer(void)
{
    static const uint64_t at[] = {1000000, 4321987, 9000001};
    for (unsigned i = 0; i < 3; ++i) {
        shladd_xor_open(true);
        M.snap.v.s.cr[CR_ITV] = 0xef;
        M.snap.v.s.cr[CR_ITM] = ia64_itc_read(&M.snap.v) + at[i];
        ia64_timer_program(&M.snap.v);
        snapshot_restore(&M.snap, M.ref);
        struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
        T1H_CHECK(t1h_bank0(M.ref, 24) == 1 && t1h_bank0(M.ref, 26) == 0xef &&
                  t1h_bank0(M.ref, 25) == at[i]);
        result_take(&M.reference, &re, M.ref);
        struct t1h_engine_vcpu ev;
        t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
        t1h_tier0t1_set_flags(0);
        snapshot_restore(&M.snap, M.v);
        t1h_engine_restored(&ev);
        struct t1_stats s0 = stats_now();
        unsigned yields = 0;
        struct vcpu_exit e = executor(&ev, BIG_BUDGET, &yields);
        struct t1_stats s1 = stats_now();
        T1H_CHECK(yields == 0 && DELTA(s0, s1, entries) == 2);
        char label[64];
        snprintf(label, sizeof(label), "kick ITM at %llu", (unsigned long long)at[i]);
        compare_or_fail(&e, label);
        t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
        t1h_engine_release(&ev);
        machine_close();
    }
    printf(
        "upper: kick ITM matches inside the Tier1 loop of shladd_xor (icount): delivered exactly at the match, two "
        "entries each, equal to the reference\n");
}

/* PSR.i = 0: the published vector stays pending; the entry still leaves at
 * its checkpoint and Tier0 yields, then the loop runs on in Tier1. */
static void kick_masked(void)
{
    shladd_xor_open(false);
    snapshot_restore(&M.snap, M.ref);
    vcpu_raise_external(M.ref, 0x41); /* never delivered: its point does not matter */
    struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && t1h_bank0(M.ref, 24) == 0);
    result_take(&M.reference, &re, M.ref);
    struct t1h_engine_vcpu ev;
    struct t1_stats s0, s1;
    struct source src;
    unsigned yields;
    struct vcpu_exit e;
    t1h_watchdog_start("kick PSR.i = 0", t1h_engine_tier0t1()->name);
    for (;;) {
        t1h_watchdog_trial();
        source_reset();
        tier0_flush();
        t1_flush();
        t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
        t1h_tier0t1_set_flags(0);
        snapshot_restore(&M.snap, M.v);
        t1h_engine_restored(&ev);
        s0 = stats_now();
        src = (struct source){M.v, s0.entries, 300, 0x41, SRC_PUBLISH, 0, false, false, 0, 0};
        pthread_t thread;
        T1H_CHECK(!pthread_create(&thread, NULL, source_thread, &src));
        yields = 0;
        e = executor(&ev, BIG_BUDGET, &yields);
        __atomic_store_n(&src.stop, 1, __ATOMIC_RELEASE);
        T1H_CHECK(!pthread_join(thread, NULL));
        s1 = stats_now();
        T1H_CHECK(s1.entries > s0.entries && "kick no Tier1 entries");
        t1h_watchdog_source(src.acted, src.during, src.delay_us, src.before, src.after);
        if (src.acted && src.during)
            break;
        ++late_attempts;
        t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
        t1h_engine_release(&ev);
    }
    T1H_CHECK(src.acted && yields == 1 && t1h_bank0(M.v, 24) == 0);
    T1H_CHECK(DELTA(s0, s1, exits_by_kind[T1_PT_CHECKPOINT]) == 1 && DELTA(s0, s1, entries) == 2);
    compare_or_fail(&e, "kick PSR.i = 0");
    t1h_watchdog_progress();
    t1h_watchdog_stop();
    printf(
        "upper: kick PSR.i = 0: the vector stays pending (cr.irr), the Tier1 entry left at the kick's "
        "checkpoint, Tier0 yielded once, Tier1 ran the rest; equal to the reference\n");
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    machine_close();
}

/* A kick racing a requested exit: the exit wins (it is returned, the kick
 * stays pending), and the next run yields to the kick. */
static void kick_vs_request(void)
{
    for (unsigned order = 0; order < 2; ++order) {
        shladd_xor_open(true);
        snapshot_restore(&M.snap, M.ref);
        struct t1h_engine_vcpu ev;
        struct source src;
        struct vcpu_exit e;
        t1h_watchdog_start(order ? "kick exit then kick" : "kick kick then exit",
                           t1h_engine_tier0t1()->name);
        for (;;) {
            t1h_watchdog_trial();
            tier0_flush();
            t1_flush();
            t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
            t1h_tier0t1_set_flags(0);
            snapshot_restore(&M.snap, M.v);
            t1h_engine_restored(&ev);
            struct t1_stats s0 = stats_now();
            src = (struct source){
                M.v, s0.entries, 300,   0, order ? SRC_EXIT_THEN_KICK : SRC_KICK_THEN_EXIT,
                0,   false,      false, 0, 0};
            pthread_t thread;
            T1H_CHECK(!pthread_create(&thread, NULL, source_thread, &src));
            e = t1h_tier0t1_slice(ev.ctx, M.v, BIG_BUDGET);
            __atomic_store_n(&src.stop, 1, __ATOMIC_RELEASE);
            T1H_CHECK(!pthread_join(thread, NULL));
            T1H_CHECK(stats_now().entries > s0.entries && "kick no Tier1 entries");
            t1h_watchdog_source(src.acted, src.during, src.delay_us, src.before, src.after);
            if (src.acted && src.during)
                break;
            ++late_attempts;
            t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
            t1h_engine_release(&ev);
        }
        T1H_CHECK(src.acted && e.reason == VCPU_EXIT_SHUTDOWN && M.v->irq_kick == 1);
        /* The reference: the same request at the same boundary. */
        struct vcpu_exit re;
        T1H_CHECK(ref_run_to(M.ref, M.v->retired, NULL, &re));
        vcpu_request_exit(M.ref, VCPU_EXIT_SHUTDOWN);
        re = ref_run(M.ref, BIG_BUDGET, NULL);
        result_take(&M.reference, &re, M.ref);
        compare_or_fail(&e, order ? "kick exit then kick" : "kick kick then exit");
        uint64_t at = M.v->retired;
        e = t1h_tier0t1_slice(ev.ctx, M.v, BIG_BUDGET);
        T1H_CHECK(t1h_kick_yield(&e) && M.v->retired == at && M.v->irq_kick == 0);
        e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
        re = ref_run(M.ref, BIG_BUDGET, NULL);
        T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && re.reason == VCPU_EXIT_DEBUG);
        result_take(&M.reference, &re, M.ref);
        compare_or_fail(&e, "kick after the requested exit");
        t1h_watchdog_progress();
        t1h_watchdog_stop();
        t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
        t1h_engine_release(&ev);
        machine_close();
    }
    printf(
        "upper: kick a kick racing a requested exit (both orders, during a Tier1 entry): the exit is "
        "returned, the kick stays and the next run yields to it without retiring\n");
}

/* The interpreter fallback never consumes a kick of an ENGINE_TIER1 vCPU
 * (interp.c: only ENGINE_INTERP's own loop does). */
static void kick_interpreter(void)
{
    shladd_xor_open(true);
    snapshot_restore(&M.snap, M.v);
    M.v->cfg.engine = ENGINE_TIER1;
    vcpu_kick(M.v);
    uint64_t before = M.v->retired;
    struct vcpu_exit e = interp_run(M.v, 1);
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && M.v->retired == before + 1 && M.v->irq_kick == 1);
    /* Its own engine kind consumes it instead of retiring. */
    M.v->cfg.engine = ENGINE_INTERP;
    e = interp_run(M.v, 1);
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && M.v->retired == before + 1 && M.v->irq_kick == 0);
    machine_close();

    /* PSR.ss: every boundary goes to Tier0's interpreter fallback; a kick
     * published meanwhile is consumed by Tier0's loop top (one yield). */
    const struct guest_program *p = guest_program("gather_phys_ss");
    struct guest_params q = guest_completion_params(p);
    struct vcpu_config cfg = guest_vcpu_config(p);
    M.ref = vcpu_create(&cfg);
    M.v = vcpu_create(&cfg);
    t1h_bind(M.ref, &M.ref_logs);
    t1h_bind(M.v, &M.logs);
    M.ref_logs.stores_on = M.logs.stores_on = false;
    guest_setup(p, &q, M.ref);
    snapshot_take(&M.snap, M.ref);
    snapshot_restore(&M.snap, M.ref);
    struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
    result_take(&M.reference, &re, M.ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    unsigned yields = 0;
    struct tier0_stats t0;
    tier0_get_stats(&t0);
    uint64_t debug0 = t0.interpreter_instructions[TIER0_INTERP_DEBUG];
    uint64_t half = M.reference.v.retired / 2;
    e = executor(&ev, half, &yields);
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && yields == 0);
    vcpu_kick(M.v); /* arrives while the fallback owns the boundaries */
    e = executor(&ev, BIG_BUDGET, &yields);
    tier0_get_stats(&t0);
    T1H_CHECK(yields == 1 && t0.interpreter_instructions[TIER0_INTERP_DEBUG] > debug0);
    compare_or_fail(&e, "kick under the interpreter fallback");
    t1h_engine_release(&ev);
    machine_close();
    printf(
        "upper: kick the interpreter fallback retires instead of consuming an ENGINE_TIER1 kick; under "
        "PSR.ss (fallback at every boundary) Tier0's loop top consumed it: one yield\n");
}

/* A kick while a Tier0 region runs (an MMIO read kicks): the region
 * completes and Tier0's loop top yields before the next one. Then wake
 * from HALT: PAL_HALT_LIGHT returns VCPU_EXIT_HALT; while the executor
 * would idle, an interrupt is published (mailbox + kick); the next run
 * yields to the kick without retiring, the executor drains, and the run
 * after it delivers at the halt's boundary and resumes. */
#define KICKER_PA (COUNTER_PA + 4096)
static uint64_t kicker_read(void *opaque, struct vcpu *v, uint64_t offset, unsigned size)
{
    (void)opaque;
    (void)offset;
    (void)size;
    /* The engine under test's vCPU only: the stepping reference models the
     * guest, not an executor that yields. */
    if (v == M.v)
        vcpu_kick(v);
    return 7;
}
static const struct gm_mmio_ops kicker_ops = {kicker_read, counter_write};
static void kick_halt(void)
{
    static bool registered;
    if (!registered) {
        registered = true;
        counter_init();
        T1H_CHECK(gm_add_region(&(struct gm_region){.base = KICKER_PA,
                                                    .size = 4096,
                                                    .kind = GM_EMULATED,
                                                    .ops = &kicker_ops,
                                                    .name = "upper-kicker"}));
    }
    struct vcpu_config cfg = {
        .engine = ENGINE_INTERP, .itc_icount = true, .pal_entry = T1H_CODE + 0x200};
    M.ref = vcpu_create(&cfg);
    M.v = vcpu_create(&cfg);
    T1H_CHECK(M.ref && M.v);
    t1h_bind(M.ref, &M.ref_logs);
    t1h_bind(M.v, &M.logs);
    M.ref_logs.stores_on = M.logs.stores_on = false;
    t1h_load_image("outcome.bin");
    struct ia64_entry_regs regs = {.ip = T1H_CODE,
                                   .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN,
                                   .bsp = T1H_BSP,
                                   .bspstore = T1H_BSP};
    regs.gr[2] = KICKER_PA;
    regs.gr[3] = 0x55;
    regs.gr[14] = T1H_UNMAPPED;
    vcpu_reset(M.ref, &regs);
    M.ref->s.cr[CR_IVA] = T1H_IVT;
    snapshot_take(&M.snap, M.ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    struct vcpu_exit e = t1h_tier0t1_slice(ev.ctx, M.v, 1000);
    T1H_CHECK(t1h_kick_yield(&e) && M.v->retired > M.snap.v.retired && M.v->irq_kick == 0);
    e = t1h_tier0t1_slice(ev.ctx, M.v, 1000);
    T1H_CHECK(e.reason == VCPU_EXIT_HALT);
    snapshot_restore(&M.snap, M.ref);
    struct vcpu_exit re = ref_run(M.ref, 1000, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_HALT && re.ip == e.ip && M.ref->retired == M.v->retired);
    result_take(&M.reference, &re, M.ref);
    compare_or_fail(&e, "kick kick during a Tier0 region, then HALT");
    /* Wake from HALT. */
    uint64_t at = M.v->retired;
    publish(M.v, 0x41);
    unsigned yields = 0;
    e = executor(&ev, 1000, &yields);
    T1H_CHECK(yields == 1 && e.reason == VCPU_EXIT_BAD_ACCESS);
    T1H_CHECK(t1h_bank0(M.v, 24) == 1 && t1h_bank0(M.v, 26) == 0x41 &&
              t1h_bank0(M.v, 25) - M.v->clock_offset == at);
    vcpu_raise_external(M.ref, 0x41);
    re = ref_run(M.ref, 1000, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_BAD_ACCESS);
    result_take(&M.reference, &re, M.ref);
    compare_or_fail(&e, "kick wake from HALT");
    t1h_engine_release(&ev);
    machine_close();
    printf(
        "upper: kick a kick during a Tier0 region (MMIO read): Tier0 yielded after the region; wake from "
        "HALT: a published interrupt's kick was consumed as a yield without retiring, then delivered at "
        "the halt's boundary\n");
}

/* ---- links --------------------------------- */

/* A br.ctop loop: every iteration enters the header under other rotating
 * bases; one Tier0 region (its key masks the rotation) links to one Tier1
 * region per rotation, and enter re-probes on a mismatch. */
static void links_rotation(void)
{
    struct ia64_entry_regs regs = {.ip = T1H_CODE,
                                   .psr = IA64_PSR_IC | IA64_PSR_BN,
                                   .bsp = T1H_BSP,
                                   .bspstore = T1H_BSP,
                                   .rsc = 3};
    regs.gr[2] = 0x3b9aca07;
    regs.gr[10] = 1500;
    machine_open("rotating_links.bin", &regs);
    snapshot_take(&M.snap, M.ref);
    struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
    if (re.reason != VCPU_EXIT_DEBUG || M.ref_logs.last_break_imm != T1H_BREAK_END)
        fprintf(
            stderr,
            "upper: rotating_links reference: exit %u at %#llx:%u (%s), break %#llx, retired %llu\n",
            re.reason, (unsigned long long)re.ip, re.slot, re.detail ? re.detail : "-",
            (unsigned long long)M.ref_logs.last_break_imm, (unsigned long long)M.ref->retired);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    result_take(&M.reference, &re, M.ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_tier0t1_set_flags(0);
    /* Keep the 11-instruction body profitable while testing rotating links. */
    uint32_t saved_min = t1_cfg.min_insns_per_entry;
    t1_cfg.min_insns_per_entry = 8;
    /* Production thresholds: only the header becomes hot (the outer loop's
     * trace heads would be compiled and demoted at the harness's 8). */
    t1_test_set_thresholds(64, 1024);
    tier0_flush();
    t1_flush();
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    struct t1_stats s0 = stats_now();
    struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
    struct t1_stats s1 = stats_now();
    t1_test_set_thresholds(0, 0);
    t1_cfg.min_insns_per_entry = saved_min;
    compare_or_fail(&e, "links: rotating bases");
    if (getenv("T1H_UPPER_TRACE"))
        fprintf(
            stderr,
            "rot: compiles %llu rejects %llu enters %llu ran %llu relinks %llu decline_link %llu hot %llu "
            "linked %llu rejected %llu probes %llu links %llu demote_profit %llu variant_compiles %llu\n",
            (unsigned long long)DELTA(s0, s1, compiles), (unsigned long long)DELTA(s0, s1, rejects),
            (unsigned long long)DELTA(s0, s1, upper_enters),
            (unsigned long long)DELTA(s0, s1, upper_ran),
            (unsigned long long)DELTA(s0, s1, relinks),
            (unsigned long long)DELTA(s0, s1, decline_link),
            (unsigned long long)DELTA(s0, s1, hot_calls),
            (unsigned long long)DELTA(s0, s1, hot_linked),
            (unsigned long long)DELTA(s0, s1, hot_rejected),
            (unsigned long long)DELTA(s0, s1, probes),
            (unsigned long long)DELTA(s0, s1, probe_links),
            (unsigned long long)DELTA(s0, s1, demote_profit),
            (unsigned long long)DELTA(s0, s1, variant_compiles));
    /* Both rotations compiled (the second by enter's variant compile);
     * then every entry re-probes (one hash lookup) and runs the region of
     * its rotation. */
    /* (Other heads -- the outer loop's trace heads -- also reach 1024
     * entries and compile.) Of the 3000 header entries, those after the
     * first ~64 iterations re-probe and run. */
    T1H_CHECK(DELTA(s0, s1, compiles) >= 2 && DELTA(s0, s1, variant_compiles) == 1);
    T1H_CHECK(DELTA(s0, s1, relinks) > 2800 && DELTA(s0, s1, upper_ran) > 2800);
    printf(
        "upper: links: one header entered under two rotations alternately (1500 x clrrrb, br.ctop): %llu "
        "regions compiled (%llu by enter for the second rotation), %llu entries, %llu of %llu enter calls "
        "re-probed to the other rotation's region, %llu found none; equal to the reference\n",
        (unsigned long long)DELTA(s0, s1, compiles),
        (unsigned long long)DELTA(s0, s1, variant_compiles),
        (unsigned long long)DELTA(s0, s1, entries), (unsigned long long)DELTA(s0, s1, relinks),
        (unsigned long long)DELTA(s0, s1, upper_enters),
        (unsigned long long)DELTA(s0, s1, decline_link));
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    machine_close();
}

/* A link names (generation, id): after its region is reclaimed and the id
 * reused, even by a region with the same contract, the link is dead. */
static void links_generation(void)
{
    const struct guest_program *p = guest_program("shladd_xor");
    struct guest_params q = guest_completion_params(p);
    struct vcpu_config cfg = guest_vcpu_config(p);
    M.ref = vcpu_create(&cfg);
    M.v = vcpu_create(&cfg);
    T1H_CHECK(M.ref && M.v);
    t1h_bind(M.ref, &M.ref_logs);
    t1h_bind(M.v, &M.logs);
    guest_setup(p, &q, M.ref);
    uint64_t header;
    T1H_CHECK(t1h_run_to_loop_header(M.ref, 100000, &header));
    snapshot_take(&M.snap, M.ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    struct t1_vcpu *tv = t1h_tier1_vcpu(ev.ctx);
    struct t1_compile_options opt = {0};
    t1_test_set_compile_options(tv, &opt);
    t1_flush();
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    M.v->upper = tv;
    M.v->cfg.engine = ENGINE_TIER1;
    struct t1_compile_report rep;
    T1H_CHECK(t1_compile(tv, &opt, &rep) == T1_COMPILED);
    const struct t1_region *r1 = t1_region_get(rep.region_id);
    gpa_t pa = r1->c.entry_pa;
    uint32_t id = r1->id;
    uint64_t link = t1_upper_probe(M.v, pa);
    T1H_CHECK(link && t1_region_from_link(link) == r1);
    t1_flush(); /* retired and reclaimed: the record is gone */
    T1H_CHECK(!t1_region_from_link(link));
    T1H_CHECK(t1_compile(tv, &opt, &rep) == T1_COMPILED &&
              rep.region_id == id); /* the id, reused */
    T1H_CHECK(!t1_region_from_link(link));
    struct t1_stats s0 = stats_now();
    struct vcpu_exit out;
    uint64_t stale = link;
    T1H_CHECK(t1_upper_enter(M.v, &stale, BIG_BUDGET, &out) == T1_UPPER_DECLINED && stale == 0);
    struct t1_stats s1 = stats_now();
    T1H_CHECK(DELTA(s0, s1, decline_link) == 1 && DELTA(s0, s1, entries) == 0 &&
              M.v->retired == M.snap.v.retired);
    uint64_t fresh = t1_upper_probe(M.v, pa);
    T1H_CHECK(fresh && fresh != link && t1_region_from_link(fresh) == t1_region_get(id));
    T1H_CHECK(t1_upper_enter(M.v, &fresh, BIG_BUDGET, &out) == T1_UPPER_RAN &&
              M.v->retired > M.snap.v.retired);
    M.v->upper = NULL;
    M.v->cfg.engine = ENGINE_INTERP;
    printf(
        "upper: links: a link to a reclaimed region stays dead when its id is reused by a region with the "
        "same contract (generation); the fresh probe links and runs it\n");
    t1h_engine_release(&ev);
    machine_close();
}

/* ---- PSR.db states run in Tier0 --------- */

/* Tier0 runs PSR.db states in its own regions when no breakpoint
 * pair is armed at the CPL, so they reach its lookup. Tier1 hands them to
 * the lower tier (dispatch.c lower_tier_psr), and Tier0 does not
 * consult it in them (tier0.h lower_psr). mmio_progress.S with r2 at a RAM word:
 * 10^4 iterations of one load. Run A (PSR.db = 0) compiles and enters the
 * loop's region; run B (PSR.db = 1, fresh Tier0 regions, A's Tier1 region
 * kept) must run entirely in Tier0 regions: no probe, hot() call, link,
 * compile or entry. Then at the header: a live link
 * to A's region, PSR.db set, enter declines for the PSR state (not through
 * the contract mismatch) and keeps the link; probe and hot() refuse. */
#define DB_WORD UINT64_C(0x5a5a0123)
static void psr_db_case(bool db, const char *label, struct t1_stats *s0, struct t1_stats *s1,
                        struct tier0_stats *z0, struct tier0_stats *z1)
{
    snapshot_restore(&M.snap, M.ref);
    if (db)
        M.ref->s.psr |= IA64_PSR_DB;
    else
        M.ref->s.psr &= ~IA64_PSR_DB;
    T1H_CHECK(!(ia64_debug_armed(&M.ref->s) & 0xff)); /* no pair armed at any level */
    struct snapshot start;
    snapshot_init(&start);
    snapshot_take(&start, M.ref);
    struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    T1H_CHECK(M.ref->s.gr[14] == (uint64_t)PROGRESS_N * DB_WORD);
    result_take(&M.reference, &re, M.ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_tier0t1_set_flags(0);
    t1_test_set_thresholds(64, 1024);
    tier0_flush(); /* Tier1's regions stay */
    snapshot_restore(&start, M.v);
    t1h_engine_restored(&ev);
    *s0 = stats_now();
    tier0_get_stats(z0);
    struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
    *s1 = stats_now();
    tier0_get_stats(z1);
    t1_test_set_thresholds(0, 0);
    compare_or_fail(&e, label);
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    snapshot_free(&start);
}

static void psr_db_tier0(void)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[2] = T1H_TABLE;
    regs.gr[8] = PROGRESS_N - 1;
    machine_open("mmio_progress.bin", &regs);
    uint64_t word = DB_WORD;
    memcpy(t1h_ram() + (T1H_TABLE - T1H_RAM_BASE), &word, 8);
    code_notify_vmm_write(T1H_TABLE, 8);
    snapshot_take(&M.snap, M.ref);
    t1_flush();
    const uint64_t header = T1H_CODE + 0x10; /* .Lloop */
    struct t1_stats a0, a1, b0, b1;
    struct tier0_stats y0, y1, z0, z1;

    psr_db_case(false, "PSR.db: run A (PSR.db = 0)", &a0, &a1, &y0, &y1);
    T1H_CHECK(DELTA(a0, a1, compiles) >= 1 && DELTA(a0, a1, entries) >= 1);
    T1H_CHECK(DELTA(a0, a1, lower_refusals) == 0 && DELTA(a0, a1, decline_lower) == 0);
    const struct t1_region *loop = NULL;
    for (uint32_t id = 1; id <= t1_cfg.max_regions && !loop; ++id) {
        const struct t1_region *r = t1_region_get(id);
        if (r && r->valid && r->c.ip == header && !(r->c.psr & IA64_PSR_DB))
            loop = r;
    }
    T1H_CHECK(loop != NULL);

    psr_db_case(true, "PSR.db: run B (PSR.db = 1, no pair armed)", &b0, &b1, &z0, &z1);
    uint64_t t0_insns = z1.executed_instructions - z0.executed_instructions;
    uint64_t t0_debug = z1.interpreter_instructions[TIER0_INTERP_DEBUG] -
                        z0.interpreter_instructions[TIER0_INTERP_DEBUG];
    if (getenv("T1H_UPPER_TRACE"))
        fprintf(
            stderr,
            "psr.db: tier0 insns %llu interp-debug %llu; t1 entries %llu compiles %llu probes %llu "
            "links %llu enters %llu hot %llu lower_refusals %llu decline_lower %llu\n",
            (unsigned long long)t0_insns, (unsigned long long)t0_debug,
            (unsigned long long)DELTA(b0, b1, entries), (unsigned long long)DELTA(b0, b1, compiles),
            (unsigned long long)DELTA(b0, b1, probes),
            (unsigned long long)DELTA(b0, b1, probe_links),
            (unsigned long long)DELTA(b0, b1, upper_enters),
            (unsigned long long)DELTA(b0, b1, hot_calls),
            (unsigned long long)DELTA(b0, b1, lower_refusals),
            (unsigned long long)DELTA(b0, b1, decline_lower));
    /* Tier0 regions ran the PSR.db state (the interpreter only for its
     * usual reasons, never for debug); Tier0 did not consult Tier1 there
     * (the upper tier's lower-tier PSR bits, tier0.h). */
    T1H_CHECK(t0_debug == 0 && t0_insns >= 3 * (uint64_t)PROGRESS_N);
    T1H_CHECK(DELTA(b0, b1, entries) == 0 && DELTA(b0, b1, compiles) == 0 &&
              DELTA(b0, b1, probe_links) == 0 && DELTA(b0, b1, upper_enters) == 0 &&
              DELTA(b0, b1, variant_compiles) == 0);
    T1H_CHECK(DELTA(b0, b1, probes) == 0 && DELTA(b0, b1, hot_calls) == 0 &&
              DELTA(b0, b1, lower_refusals) == 0);

    /* enter with a live link at a PSR.db state. Tier0's key holds the whole
     * PSR, so no Tier0 region of a PSR.db state carries a link today; the
     * hooks are called here as Tier0 calls them (the guard's direct test). */
    snapshot_restore(&M.snap, M.v);
    uint64_t at;
    T1H_CHECK(t1h_run_to_loop_header(M.v, 100000, &at) && at == header);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_engine_restored(&ev);
    M.v->upper = t1h_tier1_vcpu(ev.ctx);
    M.v->cfg.engine = ENGINE_TIER1;
    gpa_t pa = loop->c.entry_pa;
    uint64_t link = t1_upper_probe(M.v, pa);
    T1H_CHECK(link && t1_region_from_link(link) == loop);
    M.v->s.psr |= IA64_PSR_DB;
    uint64_t retired = M.v->retired, kept = link;
    struct vcpu_exit out;
    struct t1_stats c0 = stats_now();
    uint64_t lower_bits[64];
    for (unsigned bit = 0; bit < 64; ++bit)
        lower_bits[bit] = t1_psr_bits(T1_PSR_LOWER, bit);
    T1H_CHECK(t1_upper_enter(M.v, &kept, BIG_BUDGET, &out) == T1_UPPER_DECLINED && kept == link);
    T1H_CHECK(t1_upper_probe(M.v, pa) == 0);
    T1H_CHECK(t1_upper_hot(M.v, pa, true) == T1_HOT_REJECTED);
    struct t1_stats c1 = stats_now();
    T1H_CHECK(DELTA(c0, c1, decline_lower) == 1 && DELTA(c0, c1, decline_link) == 0 &&
              DELTA(c0, c1, variant_compiles) == 0 && DELTA(c0, c1, compiles) == 0 &&
              DELTA(c0, c1, entries) == 0 && DELTA(c0, c1, lower_refusals) == 2 &&
              M.v->retired == retired);
    /* Each of the three refusals names PSR.db (bit 24) and no other bit. */
    for (unsigned bit = 0; bit < 64; ++bit)
        T1H_CHECK(t1_psr_bits(T1_PSR_LOWER, bit) - lower_bits[bit] == (bit == 24 ? 3u : 0u));
    M.v->s.psr &= ~IA64_PSR_DB; /* the same link runs once PSR.db is clear */
    T1H_CHECK(t1_upper_enter(M.v, &kept, BIG_BUDGET, &out) == T1_UPPER_RAN &&
              M.v->retired > retired);
    M.v->upper = NULL;
    M.v->cfg.engine = ENGINE_INTERP;
    t1h_engine_release(&ev);
    printf(
        "upper: PSR.db without an armed pair: %llu instructions in Tier0 regions (0 in the interpreter for "
        "debug), Tier1 not consulted (0 probes, hot() calls, links, compiles, entries); with PSR.db = 0 "
        "the same loop ran in Tier1 (%llu entries); enter at a live link declines for PSR.db and keeps the "
        "link; equal to the reference\n",
        (unsigned long long)t0_insns, (unsigned long long)DELTA(a0, a1, entries));
    machine_close();
}

/* ---- PSR.be runs in the interpreter --------------------------------------
 *
 * UM.be = 1 makes data references big-endian (SDM Vol.1 §3.2.3). Tier1
 * refuses the state (T1_PSR_LOWER) and Tier0 steps it in the interpreter
 * (tier0.c needs_interpreter). mmio_progress.S with r2 at a RAM word: run A
 * (PSR.be = 0) compiles and enters the loop's region; run B (PSR.be = 1)
 * loads the byte-reversed word, equals the reference, runs every
 * instruction in the interpreter and never consults Tier1. */
static void psr_be_lower(void)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[2] = T1H_TABLE;
    regs.gr[8] = PROGRESS_N - 1;
    machine_open("mmio_progress.bin", &regs);
    uint64_t word = DB_WORD;
    memcpy(t1h_ram() + (T1H_TABLE - T1H_RAM_BASE), &word, 8);
    code_notify_vmm_write(T1H_TABLE, 8);
    snapshot_take(&M.snap, M.ref);
    t1_flush();
    uint64_t entries[2], compiles[2], interpreted[2];
    for (unsigned be = 0; be < 2; ++be) {
        snapshot_restore(&M.snap, M.ref);
        if (be)
            M.ref->s.psr |= IA64_PSR_BE;
        struct snapshot start;
        snapshot_init(&start);
        snapshot_take(&start, M.ref);
        struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
        T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
        T1H_CHECK(M.ref->s.gr[14] ==
                  (uint64_t)PROGRESS_N * (be ? __builtin_bswap64(DB_WORD) : DB_WORD));
        result_take(&M.reference, &re, M.ref);
        struct t1h_engine_vcpu ev;
        t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
        t1h_tier0t1_set_flags(0);
        t1_test_set_thresholds(64, 1024);
        tier0_flush();
        snapshot_restore(&start, M.v);
        t1h_engine_restored(&ev);
        struct t1_stats s0 = stats_now(), s1;
        struct tier0_stats z0, z1;
        tier0_get_stats(&z0);
        struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
        s1 = stats_now();
        tier0_get_stats(&z1);
        t1_test_set_thresholds(0, 0);
        compare_or_fail(&e, be ? "PSR.be: run B (PSR.be = 1)" : "PSR.be: run A (PSR.be = 0)");
        t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
        t1h_engine_release(&ev);
        snapshot_free(&start);
        entries[be] = DELTA(s0, s1, entries);
        compiles[be] = DELTA(s0, s1, compiles) + DELTA(s0, s1, probes) + DELTA(s0, s1, hot_calls);
        interpreted[be] = z1.interpreter_instructions[TIER0_INTERP_UNSUPPORTED_STATE] -
                          z0.interpreter_instructions[TIER0_INTERP_UNSUPPORTED_STATE];
    }
    T1H_CHECK(entries[0] >= 1 && interpreted[0] == 0);
    T1H_CHECK(entries[1] == 0 && compiles[1] == 0 && interpreted[1] >= 3 * (uint64_t)PROGRESS_N);
    printf(
        "upper: PSR.be: run A entered Tier1 %llu times; run B loaded big-endian, ran %llu instructions in "
        "the interpreter and never consulted Tier1; both equal to the reference\n",
        (unsigned long long)entries[0], (unsigned long long)interpreted[1]);
    machine_close();
}

/* ---- PSR fields outside the contract ----------------
 *
 * The contract compares the PSR fields that compiled code and formation
 * read and the lower-tier bits; up, ac, mfl, mfh, i, sp, pp, di, si, mc and
 * ri it does not. mmio_progress.S with r2 at a RAM word: run A compiles the loop's
 * region; each run below starts from the same state with PSR fields
 * changed and equals the reference. Changed fields outside the contract
 * enter A's region without a compile: each such bit alone; PSR.ac with r2
 * unaligned (the region's site exits before effect and the lower tier
 * raises Unaligned Data Reference); PSR.i with a vector pending (delivered
 * at a Tier0 boundary, then the loop resumes in the region); a PSR.ri that
 * does not name the slot, as rfi leaves it. PSR.ic, which decides whether
 * the region writes the last successful bundle, compiles another region. */
#define PSR_FIELD_VECTOR 0x41u
static struct t1_stats psr_field_run(const struct snapshot *from, uint64_t flip, uint64_t r2,
                                     bool pending, const char *label)
{
    snapshot_restore(from, M.ref);
    M.ref->s.psr ^= flip;
    M.ref->s.gr[2] = r2;
    if (pending)
        vcpu_raise_external(M.ref, PSR_FIELD_VECTOR);
    struct snapshot start;
    snapshot_init(&start);
    snapshot_take(&start, M.ref);
    struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG);
    result_take(&M.reference, &re, M.ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_tier0t1_set_flags(0);
    t1_test_set_thresholds(64, 1024);
    snapshot_restore(&start, M.v);
    t1h_engine_restored(&ev);
    struct t1_stats s0 = stats_now(), s1;
    struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
    s1 = stats_now();
    t1_test_set_thresholds(0, 0);
    compare_or_fail(&e, label);
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    snapshot_free(&start);
    struct t1_stats d;
    uint64_t *out = (uint64_t *)&d;
    const uint64_t *a = (const uint64_t *)&s0, *b = (const uint64_t *)&s1;
    for (unsigned k = 0; k < sizeof(d) / sizeof(*out); ++k)
        out[k] = b[k] - a[k];
    return d;
}

static void psr_fields(void)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[2] = T1H_TABLE;
    regs.gr[8] = PROGRESS_N - 1;
    machine_open("mmio_progress.bin", &regs);
    uint64_t word = DB_WORD;
    memcpy(t1h_ram() + (T1H_TABLE - T1H_RAM_BASE), &word, 8);
    code_notify_vmm_write(T1H_TABLE, 8);
    snapshot_take(&M.snap, M.ref);
    tier0_flush();
    t1_flush();
    struct t1_stats d = psr_field_run(&M.snap, 0, T1H_TABLE, false, "PSR fields: run A");
    T1H_CHECK(d.compiles >= 1 && d.entries >= 1);
    static const struct {
        uint64_t bits;
        const char *name;
    } outside[] = {{UINT64_C(1) << 2, "up"},    {UINT64_C(1) << 3, "ac"},
                   {IA64_PSR_I, "i"},           {UINT64_C(1) << 20, "sp"},
                   {UINT64_C(1) << 21, "pp"},   {IA64_PSR_DI, "di"},
                   {IA64_PSR_SI, "si"},         {UINT64_C(1) << 35, "mc"},
                   {UINT64_C(1) << 41, "ri 1"}, {UINT64_C(2) << 41, "ri 2"}};
    uint64_t entered = 0;
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(outside); ++k) {
        char label[64];
        snprintf(label, sizeof(label), "PSR fields: %s", outside[k].name);
        d = psr_field_run(&M.snap, outside[k].bits, T1H_TABLE, false, label);
        if (d.compiles || !d.entries)
            fprintf(stderr, "upper: PSR.%s changed: %llu compiles, %llu entries\n", outside[k].name,
                    (unsigned long long)d.compiles, (unsigned long long)d.entries);
        T1H_CHECK(d.compiles == 0 && d.variant_compiles == 0 && d.entries >= 1);
        entered += d.entries;
    }
    /* From the loop header, where Tier0's region carries the link. */
    snapshot_restore(&M.snap, M.ref);
    uint64_t header;
    T1H_CHECK(t1h_run_to_loop_header(M.ref, 100000, &header));
    struct snapshot at_header;
    snapshot_init(&at_header);
    snapshot_take(&at_header, M.ref);
    d = psr_field_run(&at_header, UINT64_C(1) << 3, T1H_TABLE + 4, false,
                      "PSR fields: ac, unaligned");
    snapshot_free(&at_header);
    T1H_CHECK(M.reference.exit.reason == VCPU_EXIT_DEBUG &&
              M.ref_logs.last_break_imm == T1H_BREAK_UNEXPECTED);
    T1H_CHECK(d.compiles == 0 && d.site_exits >= 1);
    d = psr_field_run(&M.snap, IA64_PSR_I, T1H_TABLE, true, "PSR fields: i, pending vector");
    T1H_CHECK(t1h_bank0(M.ref, 24) == 1 && t1h_bank0(M.v, 24) == 1);
    T1H_CHECK(d.compiles == 0 && d.entries >= 1);
    d = psr_field_run(&M.snap, IA64_PSR_IC, T1H_TABLE, false, "PSR fields: ic");
    T1H_CHECK(d.compiles >= 1 && d.entries >= 1);
    printf(
        "upper: PSR fields outside the contract (up, ac, i, sp, pp, di, si, mc, ri) enter the region compiled "
        "without them (%llu entries, no compile), also with an unaligned site under PSR.ac and a vector "
        "delivered under PSR.i; PSR.ic compiles another region; equal to the reference\n",
        (unsigned long long)entered);
    machine_close();
}

/* ---- per-head re-probe (tier0.h steps 1-2) --------------
 *
 * Tier0 re-probes a region's link when the upper tier's counter of the
 * region's head moved (a publication there, or a finished request for it),
 * and a PENDING hold ends with that counter or, should a completion be
 * lost, once the global sequence moved TIER0_UPPER_HOLD_RELEASE times
 * since the hold. mmio_progress.S with r2 at a RAM word, every run from the loop
 * header, requests going to a queue that only stores them: the first run
 * queues the header's request (PENDING); publishing a region at another
 * head re-probes nothing at the header; compiling the stored request
 * re-probes and links it. Then, each from a fresh hold: a request that
 * never completes keeps the hold through TIER0_UPPER_HOLD_RELEASE - 2
 * global moves and loses it after more; a compile cancelled after it began
 * (the helper's pause) and a request the queue dropped release it at once.
 * Every run equals the reference. */
static struct t1_compile_request stored_rq;
static unsigned stored_requests;
static bool store_submit(const struct t1_compile_request *rq, void *arg)
{
    (void)arg;
    stored_rq = *rq;
    ++stored_requests;
    return true;
}

static struct t1_stats head_run(const char *label)
{
    snapshot_restore(&M.snap, M.ref);
    struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    result_take(&M.reference, &re, M.ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_tier0t1_set_flags(0);
    t1_test_set_thresholds(64, 1024);
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    struct t1_stats s0 = stats_now();
    struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
    struct t1_stats s1 = stats_now();
    t1_test_set_thresholds(0, 0);
    compare_or_fail(&e, label);
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    struct t1_stats d;
    uint64_t *out = (uint64_t *)&d;
    const uint64_t *a = (const uint64_t *)&s0, *b = (const uint64_t *)&s1;
    for (unsigned k = 0; k < sizeof(d) / sizeof(*out); ++k)
        out[k] = b[k] - a[k];
    return d;
}

/* A fresh hold at the header: new Tier0 and Tier1 regions, the request
 * queued and stored. */
static void head_hold(void)
{
    tier0_flush();
    t1_flush();
    stored_requests = 0;
    struct t1_stats d = head_run("per-head: hold");
    T1H_CHECK(d.hot_pending == 1 && stored_requests == 1 && !d.entries);
}

static void head_sequences(void)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[2] = T1H_TABLE;
    regs.gr[8] = PROGRESS_N - 1;
    machine_open("mmio_progress.bin", &regs);
    uint64_t word = DB_WORD;
    memcpy(t1h_ram() + (T1H_TABLE - T1H_RAM_BASE), &word, 8);
    code_notify_vmm_write(T1H_TABLE, 8);
    struct snapshot start;
    snapshot_init(&start);
    snapshot_take(&start, M.ref);
    uint64_t header;
    T1H_CHECK(t1h_run_to_loop_header(M.ref, 100000, &header));
    snapshot_take(&M.snap, M.ref);
    struct t1_compile_ctx *ctx = t1_compile_ctx_create();
    T1H_CHECK(ctx);
    t1_submit_fn saved = t1_cfg.submit;
    t1_cfg.submit = store_submit;
    head_hold();
    T1H_CHECK(stored_rq.ip == header);

    /* A region at another head: no re-probe at the header. */
    snapshot_restore(&start, M.v);
    struct t1_compile_request other;
    const char *why;
    struct t1_compile_options production = {0};
    T1H_CHECK(t1_request_from_vcpu(M.v, &production, 0, &other, &why) && other.ip != header);
    struct t1_compile_report rep;
    T1H_CHECK(t1_compile_request(ctx, &other, &rep) == T1_COMPILED);
    struct t1_stats d = head_run("per-head: another head published");
    T1H_CHECK(d.probes == 0 && d.hot_calls == 0 && d.entries == 0);
    /* The stored request completes: the header re-probes and links. */
    T1H_CHECK(t1_compile_request(ctx, &stored_rq, &rep) == T1_COMPILED);
    d = head_run("per-head: header published");
    T1H_CHECK(d.probes >= 1 && d.probe_links >= 1 && d.entries >= 1 && d.compiles == 0);

    /* A lost completion: the fallback after TIER0_UPPER_HOLD_RELEASE
     * global moves. */
    head_hold();
    __atomic_add_fetch(&t1_sequence, TIER0_UPPER_HOLD_RELEASE - 2, __ATOMIC_RELEASE);
    d = head_run("per-head: lost completion, held");
    T1H_CHECK(d.probes == 0 && d.hot_calls == 0);
    __atomic_add_fetch(&t1_sequence, 3, __ATOMIC_RELEASE);
    d = head_run("per-head: lost completion, released");
    T1H_CHECK(d.probes == 1);

    /* A compile cancelled after it began. */
    head_hold();
    T1H_CHECK(t1_compile_begin(ctx, &stored_rq, true));
    t1_compile_cancel(ctx);
    d = head_run("per-head: cancelled");
    T1H_CHECK(d.probes == 1);

    /* A request the queue dropped without compiling. */
    head_hold();
    t1_compile_dropped(&stored_rq);
    d = head_run("per-head: dropped");
    T1H_CHECK(d.probes == 1);

    t1_cfg.submit = saved;
    t1_compile_ctx_destroy(ctx);
    snapshot_free(&start);
    tier0_flush();
    t1_flush();
    printf(
        "upper: per-head re-probe: a publication at another head re-probes nothing at a held header; its "
        "own completion, a cancelled compile, a dropped request and %u global moves release the hold\n",
        (unsigned)TIER0_UPPER_HOLD_RELEASE + 1);
    machine_close();
}

/* ---- helper rejects take a strike -----------------------
 *
 * A queued request whose compile is rejected for a reason that recurs
 * (here an optimizer pass made to leave invalid IR) takes a strike on the
 * compiling thread, as an executor's rejected compile does: the head's next
 * hot() call is held and submits nothing. A transient formation reject (no
 * code snapshot, code_validity=none) takes none: the next call submits.
 * mmio_progress.S at its loop header, requests going to the storing queue and
 * compiled on a background context as a helper does. */
static void break_after_fold(struct t1_ir *ir, const char *pass);
static enum t1_compile_status compile_stored(struct t1_compile_ctx *ctx,
                                             struct t1_compile_report *rep)
{
    struct t1_stats s0 = stats_now();
    T1H_CHECK(t1_compile_begin(ctx, &stored_rq, true));
    while (t1_compile_step(ctx, rep)) {
    }
    t1_compile_flush(ctx);
    struct t1_stats s1 = stats_now();
    return DELTA(s0, s1, compiles)  ? T1_COMPILED
           : DELTA(s0, s1, rejects) ? T1_REJECTED
                                    : T1_NO_MEMORY;
}

static void helper_strike(void)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[2] = T1H_TABLE;
    regs.gr[8] = PROGRESS_N - 1;
    machine_open("mmio_progress.bin", &regs);
    uint64_t header;
    T1H_CHECK(t1h_run_to_loop_header(M.ref, 100000, &header));
    snapshot_take(&M.snap, M.ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_tier0t1_set_flags(0);
    struct t1_compile_ctx *ctx = t1_compile_ctx_create();
    T1H_CHECK(ctx);
    t1_submit_fn saved = t1_cfg.submit;
    t1_cfg.submit = store_submit;
    for (unsigned transient = 0; transient < 2; ++transient) {
        t1_flush();
        t1_holds_clear();
        if (transient)
            T1H_CHECK(code_validity_configure(CODE_VALIDITY_NONE, CODE_SNAPSHOT_DEFAULT_BYTES));
        snapshot_restore(&M.snap, M.v);
        t1h_engine_restored(&ev);
        M.v->upper = t1h_tier1_vcpu(ev.ctx);
        M.v->cfg.engine = ENGINE_TIER1;
        stored_requests = 0;
        T1H_CHECK(t1_upper_hot(M.v, header, true) == T1_HOT_PENDING && stored_requests == 1);
        struct t1_stats s0 = stats_now();
        struct t1_compile_report rep;
        if (!transient)
            t1_test_pass_hook = break_after_fold;
        T1H_CHECK(compile_stored(ctx, &rep) == T1_REJECTED &&
                  rep.reject_transient == (transient != 0));
        t1_test_pass_hook = NULL;
        struct t1_stats s1 = stats_now();
        T1H_CHECK(DELTA(s0, s1, reject_holds) == (transient ? 0u : 1u));
        enum t1_hot_status next = t1_upper_hot(M.v, header, true);
        struct t1_stats s2 = stats_now();
        if (transient)
            T1H_CHECK(next == T1_HOT_PENDING && stored_requests == 2);
        else
            T1H_CHECK(next == T1_HOT_REJECTED && stored_requests == 1 &&
                      DELTA(s1, s2, hot_held) == 1);
        if (transient)
            T1H_CHECK(
                code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
        M.v->upper = NULL;
        M.v->cfg.engine = ENGINE_INTERP;
    }
    t1_cfg.submit = saved;
    t1_compile_ctx_destroy(ctx);
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    t1_holds_clear();
    t1_flush();
    puts(
        "upper: a queued compile rejected for a recurring reason holds its head (next hot() held, nothing "
        "submitted); a transient reject does not");
    machine_close();
}

/* ---- the verification policy (tier1_verify, t1_config.verify_final) -------
 *
 * A pass that leaves invalid IR (here: after folding, a live op made to use
 * its own result, which no later pass removes) is rejected under both
 * policies: under passes by the check after that pass, under final by the
 * check of the optimized IR. Without the corruption both compile. */
static uint32_t broken_op;
static void break_after_fold(struct t1_ir *ir, const char *pass)
{
    if (strcmp(pass, "fold"))
        return;
    broken_op = T1_NO_OP;
    for (uint32_t o = ir->block[T1_HEADER_BLOCK].first; o != T1_NO_OP; o = ir->op[o].next)
        if ((ir->op[o].code == T1_IR_ADD || ir->op[o].code == T1_IR_SUB) && ir->op[o].nuse == 2) {
            ir->use[ir->op[o].use_first].value = T1_VALUE(o, 0);
            broken_op = o;
            return;
        }
}
static void verify_policy(void)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[20] = 0x1234;
    regs.gr[8] = 99;
    machine_open("nat_guard_decline.bin", &regs);
    uint64_t header;
    T1H_CHECK(t1h_run_to_loop_header(M.ref, 100000, &header));
    snapshot_take(&M.snap, M.ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_tier0t1_set_flags(0);
    bool saved = t1_cfg.verify_final;
    static const char *const expect[2] = {"optimize: fold: ", "verify: "};
    for (unsigned final = 0; final < 2; ++final) {
        t1_cfg.verify_final = final != 0;
        for (unsigned broken = 0; broken < 2; ++broken) {
            t1_flush();
            snapshot_restore(&M.snap, M.v);
            t1h_engine_restored(&ev);
            t1_test_pass_hook = broken ? break_after_fold : NULL;
            struct t1_compile_report rep;
            struct t1_compile_options opt = {0};
            enum t1_compile_status st = t1h_provider_real()->compile(ev.ctx, &opt, &rep);
            t1_test_pass_hook = NULL;
            if (!broken) {
                T1H_CHECK(st == T1_COMPILED);
                continue;
            }
            T1H_CHECK(broken_op != T1_NO_OP && st == T1_REJECTED && rep.reject_reason &&
                      !strncmp(rep.reject_reason, expect[final], strlen(expect[final])));
        }
    }
    t1_cfg.verify_final = saved;
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    t1_flush();
    printf(
        "upper: tier1_verify: a pass leaving invalid IR is rejected after that pass (passes) and at the "
        "optimized IR (final); valid IR compiles under both\n");
    machine_close();
}

/* ---- rejected heads are held ----------------------------
 *
 * A hot head whose compile is rejected (the flushrs of rse_return.bin's
 * .Lfunc, outside the subset) takes a hold like a demoted one: of the
 * following hot() calls, the first 2^(strikes - 1) are answered REJECTED
 * without a compile. 40 calls compile at calls 1, 3, 6, 11, 20 and 37. */
#define TRSE_FLUSHRS (T1H_CODE + 0x80)
static void reject_hold(void)
{
    struct ia64_entry_regs regs = {
        .ip = TRSE_FLUSHRS, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    machine_open("rse_return.bin", &regs);
    vcpu_reset(M.v, &regs);
    M.v->s.cr[CR_IVA] = T1H_IVT;
    t1_flush();
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_engine_restored(&ev);
    M.v->upper = t1h_tier1_vcpu(ev.ctx);
    M.v->cfg.engine = ENGINE_TIER1;
    struct t1_stats s0 = stats_now();
    unsigned compiled_at[8], n = 0;
    for (unsigned call = 1; call <= 40; ++call) {
        uint64_t before = stats_now().rejects;
        T1H_CHECK(t1_upper_hot(M.v, TRSE_FLUSHRS, false) == T1_HOT_REJECTED);
        if (stats_now().rejects != before && n < 8)
            compiled_at[n++] = call;
    }
    struct t1_stats s1 = stats_now();
    static const unsigned expected[] = {1, 3, 6, 11, 20, 37};
    T1H_CHECK(n == 6 && !memcmp(compiled_at, expected, sizeof(expected)));
    T1H_CHECK(DELTA(s0, s1, rejects) == 6 && DELTA(s0, s1, reject_holds) == 6 &&
              DELTA(s0, s1, hot_held) == 34 && DELTA(s0, s1, compiles) == 0 &&
              DELTA(s0, s1, holds) == 0);
    M.v->upper = NULL;
    M.v->cfg.engine = ENGINE_INTERP;
    t1h_engine_release(&ev);
    t1_holds_clear();
    printf(
        "upper: rejected head held: 40 hot() calls, 6 compiles (calls 1, 3, 6, 11, 20, 37), 34 held\n");
    machine_close();
}

/* ---- Link resolution without code_lock -------------------
 *
 * Link resolution never waits for code_lock: while another thread holds it
 * (up to five seconds), a live region's link resolves to the region and a
 * retired region's link to nothing. */
static int lock_phase;
static bool lock_timed_out;
static void *hold_code_lock(void *arg)
{
    (void)arg;
    code_lock();
    __atomic_store_n(&lock_phase, 1, __ATOMIC_RELEASE);
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (__atomic_load_n(&lock_phase, __ATOMIC_ACQUIRE) != 2) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec >= 5) {
            lock_timed_out = true;
            break;
        }
    }
    code_unlock();
    return NULL;
}

static void link_without_lock(void)
{
    struct t1_region *live = t1_region_new(), *dead = t1_region_new();
    T1H_CHECK(live && dead && t1_region_publish(live) && t1_region_publish(dead));
    uint64_t live_link = t1_region_link(live), dead_link = t1_region_link(dead);
    t1_region_retire(dead);
    lock_phase = 0;
    lock_timed_out = false;
    pthread_t holder;
    T1H_CHECK(!pthread_create(&holder, NULL, hold_code_lock, NULL));
    while (!__atomic_load_n(&lock_phase, __ATOMIC_ACQUIRE))
        sched_yield();
    struct t1_region *found = t1_region_from_link(live_link),
                     *gone = t1_region_from_link(dead_link);
    __atomic_store_n(&lock_phase, 2, __ATOMIC_RELEASE);
    T1H_CHECK(!pthread_join(holder, NULL));
    T1H_CHECK(!lock_timed_out && found == live && !gone);
    t1_region_retire(live);
    t1_reclaim();
    puts(
        "upper: link resolution while another thread holds code_lock: the live region, no retired one");
}

/* ---- the profitability window ---------------------
 *
 * A policy region that holds no loop is demoted at the 256th entry of its
 * window when it retired fewer than t1_config.min_insns_per_entry
 * (tier1_min_insns=) instructions per entry in it, and kept at that many or
 * more; a loop region is kept. */
static void profit_threshold(void)
{
    uint32_t saved = t1_cfg.min_insns_per_entry;
    for (uint32_t min = 4; min <= 32; min *= 2)
        for (unsigned kind = 0; kind < 3; ++kind) {
            t1_cfg.min_insns_per_entry = min;
            uint64_t per_entry = kind == 1 ? min : min - 1;
            struct t1_region *r = t1_region_new();
            T1H_CHECK(r);
            r->c.entry_pa = T1H_CODE;
            r->c.ip = T1H_CODE;
            r->loop = kind == 2;
            T1H_CHECK(t1_region_publish(r));
            r->policy = true;
            const struct t1_point exit_point = {.kind = T1_PT_EXIT};
            static struct t1_windows windows;
            unsigned entries = 0;
            bool demoted = false;
            while (entries < 256 && !demoted) {
                demoted = t1_demote_after_exit(&windows, r, &exit_point, per_entry);
                ++entries;
            }
            bool expected = kind == 0;
            T1H_CHECK(demoted == expected && entries == 256 && r->valid == !expected);
            if (r->valid)
                t1_region_retire(r);
        }
    t1_cfg.min_insns_per_entry = saved;
    t1_holds_clear();
    t1_reclaim();
    puts(
        "upper: profitability window: a non-loop region under tier1_min_insns per entry is demoted at its "
        "256th entry, at the threshold or as a loop it is kept (thresholds 4 to 32)");
}

/* Window counts per executor: two
 * executors A and B, a non-loop policy region at threshold 8. A counts 10
 * entries of no instruction, B closes the window with 256 entries of 8
 * (kept). A's 16th entry folds its 16 into the closed window's generation:
 * dropped. The next window then needs 256 entries of B: 240 leave it open,
 * 16 more close it, kept. Carried, A's empty entries would have closed it
 * at B's 240th with too few instructions. A slot another region takes is
 * folded first: 10 entries wait in a slot until a region with the
 * same slot index enters through another executor. */
static void window_generations(void)
{
    uint32_t saved = t1_cfg.min_insns_per_entry;
    t1_cfg.min_insns_per_entry = 8;
    struct t1_region *r = t1_region_new();
    T1H_CHECK(r);
    r->c.entry_pa = r->c.ip = T1H_CODE;
    T1H_CHECK(t1_region_publish(r));
    r->policy = true;
    const struct t1_point exit_point = {.kind = T1_PT_EXIT};
    static struct t1_windows a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    for (unsigned k = 0; k < 10; ++k)
        T1H_CHECK(!t1_demote_after_exit(&a, r, &exit_point, 0));
    for (unsigned k = 0; k < 256; ++k)
        T1H_CHECK(!t1_demote_after_exit(&b, r, &exit_point, 8));
    T1H_CHECK(T1_WINDOW_GEN(r->profit_window) == 1 && !T1_PROFIT_ENTRIES(r->profit_window));
    for (unsigned k = 0; k < 6; ++k)
        T1H_CHECK(!t1_demote_after_exit(&a, r, &exit_point, 0));
    T1H_CHECK(T1_WINDOW_GEN(r->profit_window) == 1 && !T1_PROFIT_ENTRIES(r->profit_window));
    for (unsigned k = 0; k < 240; ++k)
        T1H_CHECK(!t1_demote_after_exit(&b, r, &exit_point, 8));
    T1H_CHECK(T1_WINDOW_GEN(r->profit_window) == 1 && T1_PROFIT_ENTRIES(r->profit_window) == 240);
    for (unsigned k = 0; k < 16; ++k)
        T1H_CHECK(!t1_demote_after_exit(&b, r, &exit_point, 8));
    T1H_CHECK(r->valid && T1_WINDOW_GEN(r->profit_window) == 2);
    /* Folded when another region takes the slot (an executor C whose slot
     * starts in the current window). */
    static struct t1_windows c;
    memset(&c, 0, sizeof(c));
    for (unsigned k = 0; k < 10; ++k)
        T1H_CHECK(!t1_demote_after_exit(&c, r, &exit_point, 8));
    T1H_CHECK(!T1_PROFIT_ENTRIES(r->profit_window));
    static struct t1_region *spare[1024];
    struct t1_region *other = NULL;
    unsigned nspare = 0;
    while (!other) {
        struct t1_region *q = t1_region_new();
        T1H_CHECK(q && nspare < IA64VMM_ARRAY_SIZE(spare));
        if (q->id % T1_WINDOW_SLOTS == r->id % T1_WINDOW_SLOTS)
            other = q;
        else
            spare[nspare++] = q;
    }
    other->c.entry_pa = other->c.ip = T1H_CODE + 16;
    T1H_CHECK(t1_region_publish(other));
    other->policy = true;
    T1H_CHECK(!t1_demote_after_exit(&c, other, &exit_point, 8));
    T1H_CHECK(T1_PROFIT_ENTRIES(r->profit_window) == 10);
    for (unsigned k = 0; k < nspare; ++k)
        t1_region_discard(spare[k]);
    t1_region_retire(other);
    t1_region_retire(r);
    t1_cfg.min_insns_per_entry = saved;
    t1_holds_clear();
    t1_reclaim();
    puts(
        "upper: window counts per executor: a fold into a closed window's generation is dropped, never "
        "carried; a slot another region takes is folded first");
}

/* ---- the head pre-check ---------------------------------
 *
 * Formation rejects a request at its head instruction for reasons only the
 * head bundle and the request decide (an instruction outside the subset, an
 * undefined template, slot 2 of an MLX bundle, a class M helper of a
 * checked region). t1_request_head_allowed reads that bundle and must agree
 * with t1_form_request at every slot of the guest programs and their IVT,
 * for production and checked flags, with and without exact memory
 * helpers. With requests submitted to a queue, hot() at a head the
 * pre-check refuses answers REJECTED with no request and no hold, and
 * counts the head stop; at another head it submits. */
static unsigned precheck_submitted;
static bool precheck_submit(const struct t1_compile_request *rq, void *arg)
{
    (void)rq;
    (void)arg;
    ++precheck_submitted;
    return true;
}

static void head_precheck(void)
{
    static const char *const images[] = {
        "rse_return.bin",      "mmio_progress.bin",    "gather_load_after_store.bin",
        "mmu_log_writers.bin", "coherence_matrix.bin", "gather_rot.bin",
        "outcome.bin",         "rotating_links.bin"};
    static const uint32_t flags[] = {0, T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG};
    struct t1_form *f = calloc(1, sizeof(*f));
    struct ia64_state *names = calloc(1, sizeof(*names));
    T1H_CHECK(f && names);
    unsigned heads = 0, refused = 0;
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(images); ++i) {
        struct ia64_entry_regs regs = {
            .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
        machine_open(images[i], &regs);
        vcpu_reset(M.v, &regs);
        /* A bundle of each undefined template, beyond the program. */
        for (unsigned t = 0; t < 8; ++t) {
            static const uint8_t undefined[] = {0x06, 0x07, 0x14, 0x15, 0x1a, 0x1b, 0x1e, 0x1f};
            uint8_t bundle[16] = {undefined[t]};
            memcpy(t1h_ram() + (T1H_CODE + 0x7000 + 16 * t - T1H_RAM_BASE), bundle, 16);
        }
        code_notify_vmm_write(T1H_CODE + 0x7000, 128);
        for (uint64_t at = 0; at < 0x10000; at += 16) {
            if (at == 0x2000)
                at = 0x7000; /* the program, those bundles, the IVT */
            if (at == 0x7080)
                at = 0x8000;
            for (unsigned slot = 0; slot < 3; ++slot)
                for (unsigned k = 0; k < 4; ++k) {
                    M.v->s.ip = T1H_CODE + at;
                    M.v->s.slot = slot;
                    struct t1_compile_options opt = {.flags = flags[k & 1]};
                    struct t1_compile_request rq;
                    const char *why;
                    if (!t1_request_from_vcpu(M.v, &opt, 0, &rq, &why))
                        continue;
                    rq.slow_memory = k >= 2;
                    unsigned stop = 0;
                    bool allowed = t1_request_head_allowed(&rq, &stop);
                    enum t1_compile_status st = t1_form_request(&rq, names, T1_MAX_INSNS, f, &why);
                    bool head_reject = st == T1_REJECTED && !f->ninsns && !f->transient;
                    if (allowed == head_reject)
                        fprintf(
                            stderr,
                            "upper: %s +%#llx slot %u flags %u slow %u: pre-check %s, formation %d "
                            "(%u instructions, %s)\n",
                            images[i], (unsigned long long)at, slot, flags[k & 1], k >= 2,
                            allowed ? "allows" : "refuses", (int)st, f->ninsns, why ? why : "-");
                    T1H_CHECK(allowed != head_reject);
                    ++heads;
                    refused += !allowed;
                }
        }
        machine_close();
    }
    free(f);
    free(names);
    T1H_CHECK(refused > 0 && refused < heads);

    struct ia64_entry_regs regs = {
        .ip = TRSE_FLUSHRS, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    machine_open("rse_return.bin", &regs);
    vcpu_reset(M.v, &regs);
    M.v->s.cr[CR_IVA] = T1H_IVT;
    t1_flush();
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_engine_restored(&ev);
    M.v->upper = t1h_tier1_vcpu(ev.ctx);
    M.v->cfg.engine = ENGINE_TIER1;
    t1_submit_fn saved = t1_cfg.submit;
    t1_cfg.submit = precheck_submit;
    precheck_submitted = 0;
    struct t1_stats s0 = stats_now();
    uint64_t flushrs_stops = t1_form_stops(IA64_OP_FLUSHRS, true);
    for (unsigned call = 0; call < 4; ++call)
        T1H_CHECK(t1_upper_hot(M.v, TRSE_FLUSHRS, false) == T1_HOT_REJECTED); /* flushrs */
    struct t1_stats s1 = stats_now();
    T1H_CHECK(!precheck_submitted && DELTA(s0, s1, hot_head_rejects) == 4 &&
              DELTA(s0, s1, reject_holds) == 0 && DELTA(s0, s1, hot_held) == 0 &&
              DELTA(s0, s1, rejects) == 0 &&
              t1_form_stops(IA64_OP_FLUSHRS, true) == flushrs_stops + 4);
    M.v->s.ip = T1H_CODE + 0x10; /* nop.m */
    T1H_CHECK(t1_upper_hot(M.v, T1H_CODE + 0x10, false) == T1_HOT_PENDING &&
              precheck_submitted == 1);
    t1_cfg.submit = saved;
    M.v->upper = NULL;
    M.v->cfg.engine = ENGINE_INTERP;
    t1h_engine_release(&ev);
    machine_close();
    printf(
        "upper: head pre-check agrees with formation at %u heads (%u refused); hot() at the flushrs head of "
        "rse_return.S submits nothing and takes no hold\n",
        heads, refused);
}

/* A transient formation reject (no code snapshot: code_validity=none here)
 * takes no hold: every hot() call compiles again. */
static void transient_reject_retried(void)
{
    T1H_CHECK(code_validity_configure(CODE_VALIDITY_NONE, CODE_SNAPSHOT_DEFAULT_BYTES));
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    machine_open("rse_return.bin", &regs);
    vcpu_reset(M.v, &regs);
    M.v->s.cr[CR_IVA] = T1H_IVT;
    t1_flush();
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_engine_restored(&ev);
    M.v->upper = t1h_tier1_vcpu(ev.ctx);
    M.v->cfg.engine = ENGINE_TIER1;
    struct t1_stats s0 = stats_now();
    for (unsigned call = 1; call <= 40; ++call)
        T1H_CHECK(t1_upper_hot(M.v, T1H_CODE, false) == T1_HOT_REJECTED);
    struct t1_stats s1 = stats_now();
    T1H_CHECK(DELTA(s0, s1, rejects) == 40 && DELTA(s0, s1, reject_holds) == 0 &&
              DELTA(s0, s1, hot_held) == 0);
    M.v->upper = NULL;
    M.v->cfg.engine = ENGINE_INTERP;
    t1h_engine_release(&ev);
    t1_holds_clear();
    machine_close();
    T1H_CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    printf("upper: transient reject (no code snapshot) not held: 40 hot() calls, 40 compiles\n");
}

/* ---- the fetch-guard cache ------------------------------ */

/* gather-2page in virtual mode: the loop's region spans code pages 0x100000 and
 * 0x101000. The harness maps page 0 by ITR0 (4 KiB), the IVT by ITR1
 * (32 KiB) and page 0x101000 by an I-side TC, so the logged writer
 * ia64_tc_insert can remap that page alone. Run A: entries find the guard
 * in the cache while the MMU log stands still. Run B: after half the run
 * the TC of page 0x101000 is replaced (the same call on the reference at
 * the same retired count) by one mapping an empty image page filled with
 * the program's end bundle (break.i 0x1ffffe). The next entry must miss,
 * the guard must decline, and Tier0 must stop at the break in that page as
 * the reference does; a stale hit would run the old page's code in Tier1. */
#define FETCH_X UINT64_C(0x106000) /* an empty page of the image */
static uint64_t code_pte(gpa_t pa)
{
    return pa | 1 | 0x20 | 0x40 | (UINT64_C(3) << 9);
}

static void fetch_remap(struct vcpu *v)
{
    ia64_tc_insert(v, T1H_CODE + 0x1000, 0, code_pte(FETCH_X), 12u << 2);
}

/* With separate TCs (SDM Vol.2 §4.1.1.3), D-side replacements cannot
 * change a resident I-TLB translation. Shared-log overflow keeps that proof. */
static void fetch_data_change(struct vcpu *v)
{
    for (unsigned n = 0; n <= IA64_MMU_LOG; ++n)
        ia64_tc_insert(v, UINT64_C(0x4000000), 1, code_pte(FETCH_X), 12u << 2);
}

static struct t1_stats fetch_case(uint64_t remap_at, void (*change)(struct vcpu *),
                                  const char *label, uint64_t *end_ip)
{
    snapshot_restore(&M.snap, M.ref);
    struct vcpu_exit re;
    if (remap_at) {
        re = ref_run(M.ref, remap_at, NULL);
        T1H_CHECK(re.reason == VCPU_EXIT_BUDGET && M.ref->retired == M.snap.v.retired + remap_at);
        change(M.ref);
    }
    re = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    result_take(&M.reference, &re, M.ref);
    *end_ip = M.ref->s.ip;
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_tier0t1_set_flags(0);
    tier0_flush();
    t1_flush();
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    struct t1_compile_report rep;
    struct t1_compile_options opt = {0};
    T1H_CHECK(t1h_provider_real()->compile(ev.ctx, &opt, &rep) == T1_COMPILED);
    const struct t1_region *r = t1_region_get(rep.region_id);
    T1H_CHECK(r && r->npages == 2 && r->page[1].pa == T1H_CODE + 0x1000);
    uint64_t r0 = r->entries;
    /* Production thresholds: few other regions; the explicit one is never
     * demoted (not a policy region) and stays valid. */
    t1_test_set_thresholds(64, 1024);
    struct t1_stats s0 = stats_now(), s1;
    struct vcpu_exit e;
    if (remap_at) {
        e = t1h_engine_run(&ev, remap_at);
        T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && M.v->retired == M.snap.v.retired + remap_at);
        change(M.v);
    }
    e = t1h_engine_run(&ev, BIG_BUDGET);
    s1 = stats_now();
    t1_test_set_thresholds(0, 0);
    compare_or_fail(&e, label);
    T1H_CHECK(r->valid);
    uint64_t region_entries = r->entries - r0;
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    struct t1_stats d;
    memset(&d, 0, sizeof(d));
    d.entries = region_entries; /* the two-page region's */
    d.fetch_guards = DELTA(s0, s1, fetch_guards);
    d.fetch_cache_hits = DELTA(s0, s1, fetch_cache_hits);
    d.decline_fetch = DELTA(s0, s1, decline_fetch);
    return d;
}

/* Execute the architectural writers, with the M-unit system encodings
 * from SDM Vol.3 §4.4. Each case starts with an actually checked proof. */
static void fetch_mmu_op(struct vcpu *v, unsigned x6, Ia64Opcode opcode)
{
    uint64_t raw = UINT64_C(1) << 37 | (uint64_t)x6 << 27 | UINT64_C(2) << 20 | UINT64_C(3) << 13;
    Ia64Instruction insn = ia64_decode_insn(IA64_UNIT_M, raw, v->s.ip, 0);
    T1H_CHECK(insn.valid && insn.opcode == opcode);
    uint64_t psr = v->s.psr;
    v->s.psr &= ~IA64_PSR_IC;
    T1H_CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
    v->s.psr = psr;
}

static void fetch_cache_keys(void)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_tier0t1_set_flags(0);
    tier0_flush();
    t1_flush();
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    struct t1_compile_report rep;
    struct t1_compile_options opt = {0};
    T1H_CHECK(t1h_provider_real()->compile(ev.ctx, &opt, &rep) == T1_COMPILED);
    const struct t1_region *r = t1_region_get(rep.region_id);
    T1H_CHECK(r && r->npages == 2);
    T1H_CHECK(t1h_engine_run(&ev, 200).reason == VCPU_EXIT_BUDGET);
    struct t1_vcpu *tv = t1h_tier1_vcpu(ev.ctx);
    struct t1_fetch_ok *cache = &tv->fetch_ok[r->id % T1_FETCH_CACHE];
    struct t1_fetch_ok proof = *cache;
    struct vcpu *v = M.v, *saved = malloc(sizeof(*saved)), *before = malloc(sizeof(*before));
    T1H_CHECK(saved && before && proof.link == r->link);
    *saved = *v;
    enum {
        DTC,
        DTR,
        DPTR,
        ITC,
        ITR,
        IPTR,
        PTC_L,
        PTC_G,
        PTC_GA,
        PTC_E,
        REMOTE,
        RR,
        PKR,
        PKR_DUP,
        RIGHTS,
        PRESENT,
        ACCESSED,
        NAT_PAGE,
        RESET,
        RESTORE,
        PSR_IC,
        PSR_PK,
        PSR_IA,
        CPL1,
        CPL2,
        LINK,
        PTA,
        DT,
        PHYSICAL,
        NKEYS
    };
    for (unsigned test = 0; test < NKEYS; ++test) {
        *v = *saved;
        t1_vcpu_state_restored(tv);
        *cache = proof;
        uint64_t second = T1H_CODE + 0x1000;
        uint64_t other = UINT64_C(0x4000000);
        struct t1_region target = *r;
        ia64_gr_write(&v->s, 2, second, false);
        ia64_gr_write(&v->s, 3, 12u << 2, false);
        switch (test) {
        case DTC:
            fetch_data_change(v);
            break;
        case DTR:
        case ITR:
            v->s.cr[CR_IFA] = other;
            v->s.cr[CR_ITIR] = 12u << 2;
            ia64_gr_write(&v->s, 2, 2, false);
            ia64_gr_write(&v->s, 3, code_pte(FETCH_X), false);
            fetch_mmu_op(v, test == DTR ? 0x0e : 0x0f, test == DTR ? IA64_OP_ITR_D : IA64_OP_ITR_I);
            break;
        case DPTR:
            fetch_data_change(v);
            ia64_gr_write(&v->s, 2, other, false);
            fetch_mmu_op(v, 0x0c, IA64_OP_PTR_D);
            break;
        case ITC:
            fetch_remap(v);
            break;
        case IPTR:
            ia64_gr_write(&v->s, 2, r->c.ip, false);
            fetch_mmu_op(v, 0x0d, IA64_OP_PTR_I);
            break;
        case PTC_L:
            fetch_mmu_op(v, 0x09, IA64_OP_PTC_L);
            break;
        case PTC_G:
            fetch_mmu_op(v, 0x0a, IA64_OP_PTC_G);
            break;
        case PTC_GA:
            fetch_mmu_op(v, 0x0b, IA64_OP_PTC_GA);
            break;
        case PTC_E:
            fetch_mmu_op(v, 0x34, IA64_OP_PTC_E);
            break;
        case REMOTE:
            ia64_tlb_remote_purge(v, (v->s.rr[0] >> 8) & 0xffffff, second, 12, false);
            break;
        case RR:
            ia64_gr_write(&v->s, 2, 0, false);
            ia64_gr_write(&v->s, 3, v->s.rr[0] ^ 0x100, false);
            fetch_mmu_op(v, 0x00, IA64_OP_MOV_GRRR);
            break;
        case PKR:
        case PKR_DUP:
            ia64_gr_write(&v->s, 2, 0, false);
            ia64_gr_write(&v->s, 3, 9, false); /* valid key 0, execute disabled */
            fetch_mmu_op(v, 0x03, IA64_OP_MOV_GRPKR_INDEXED);
            if (test == PKR_DUP) {
                ia64_gr_write(&v->s, 2, 1, false);
                fetch_mmu_op(v, 0x03, IA64_OP_MOV_GRPKR_INDEXED);
                T1H_CHECK(!(v->s.pkr[0] & 1));
            }
            break;
        case RIGHTS:
        case PRESENT:
        case ACCESSED:
        case NAT_PAGE: {
            uint64_t pte = code_pte(second);
            if (test == RIGHTS)
                pte &= ~(UINT64_C(7) << 9);
            else if (test == PRESENT)
                pte &= ~UINT64_C(1);
            else if (test == ACCESSED)
                pte &= ~UINT64_C(32);
            else
                pte |= UINT64_C(7) << 2;
            ia64_tc_insert(v, second, 0, pte, 12u << 2);
            break;
        }
        case RESET:
            vcpu_reset(v, NULL);
            v->s.psr = saved->s.psr;
            break;
        case RESTORE:
            t1_vcpu_state_restored(tv);
            break;
        case PSR_IC:
            v->s.psr ^= IA64_PSR_IC;
            break;
        case PSR_PK:
            v->s.psr ^= IA64_PSR_PK;
            break;
        case PSR_IA:
            v->s.psr ^= IA64_PSR_IA;
            break;
        case CPL1:
            v->s.psr ^= UINT64_C(1) << 32;
            break;
        case CPL2:
            v->s.psr ^= UINT64_C(2) << 32;
            break;
        case LINK:
            ++target.link;
            break;
        case PTA:
            v->s.cr[CR_PTA] ^= UINT64_C(0x8000);
            break;
        case DT:
            v->s.psr ^= IA64_PSR_DT;
            break;
        case PHYSICAL:
            v->s.psr &= ~IA64_PSR_IT;
            break;
        }
        /* Query a future target while the current IP has no translation.
         * The proof lookup may neither translate nor mutate the vCPU. */
        v->s.ip = VRN(7) | other;
        *before = *v;
        struct code_fetch fetch;
        bool keep = test == DTC || test == DTR || test == DPTR || test == PTA || test == DT ||
                    test == PHYSICAL;
        bool found = t1_fetch_context_cached(tv, &target, &fetch);
        if (found != keep)
            fprintf(stderr, "fetch cache key %u: found %u, expected %u\n", test, found, keep);
        T1H_CHECK(found == keep && !memcmp(v, before, sizeof(*v)));
        if (found)
            T1H_CHECK(fetch.psr == v->s.psr &&
                      fetch.pte == (test == PHYSICAL ? 0 : proof.entry_pte) &&
                      fetch.rr == (test == PHYSICAL ? 0 : v->s.rr[r->c.ip >> 61]) &&
                      fetch.pta == (test == PHYSICAL ? 0 : v->s.cr[CR_PTA]));
    }
    *v = *saved;
    t1_vcpu_state_restored(tv);
    free(before);
    free(saved);
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    printf("upper: fetch-guard keys: %u writer/context cases; future-target lookup is read-only\n",
           NKEYS);
}

/* Run C: a state restore may bring back an I-side sequence the cache saw
 * with other contents (a snapshot holds the vCPU by value). The engine
 * runs a few entries from the header; the cache then holds (region, q),
 * q the sequence of the last guard run. A state at the header with the
 * second page remapped and its sequence set to q (another history that
 * reached q) is restored. The entry after the restore must run the guard
 * (and decline), not hit. */
static void fetch_cache_restore(void)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    t1h_tier0t1_set_flags(0);
    tier0_flush();
    t1_flush();
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    struct t1_compile_report rep;
    struct t1_compile_options opt = {0};
    T1H_CHECK(t1h_provider_real()->compile(ev.ctx, &opt, &rep) == T1_COMPILED);
    const struct t1_region *r = t1_region_get(rep.region_id);
    T1H_CHECK(r && r->npages == 2);
    struct t1_stats s0 = stats_now();
    struct vcpu_exit e = t1h_engine_run(&ev, 200);
    struct t1_stats s1 = stats_now();
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && DELTA(s0, s1, entries) >= 1 &&
              DELTA(s0, s1, fetch_guards) >= 1);
    const struct t1_fetch_ok *cached = &t1h_tier1_vcpu(ev.ctx)->fetch_ok[r->id % T1_FETCH_CACHE];
    T1H_CHECK(cached->link == t1_region_link(r));
    uint64_t q = cached->mmu_iseq;
    /* The other state, and the reference from it. */
    struct snapshot other;
    snapshot_init(&other);
    snapshot_restore(&M.snap, M.ref);
    fetch_remap(M.ref);
    M.ref->mmu_iseq = q;
    snapshot_take(&other, M.ref);
    struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    T1H_CHECK((M.ref->s.ip >> 12) == ((T1H_CODE + 0x1000) >> 12));
    result_take(&M.reference, &re, M.ref);
    snapshot_restore(&other, M.v);
    t1h_engine_restored(&ev);
    T1H_CHECK(M.v->mmu_iseq == q && r->valid);
    s0 = stats_now();
    e = t1h_engine_run(&ev, BIG_BUDGET);
    s1 = stats_now();
    if (getenv("T1H_UPPER_TRACE"))
        fprintf(stderr, "fetch C: entries %llu guards %llu hits %llu declines %llu\n",
                (unsigned long long)DELTA(s0, s1, entries),
                (unsigned long long)DELTA(s0, s1, fetch_guards),
                (unsigned long long)DELTA(s0, s1, fetch_cache_hits),
                (unsigned long long)DELTA(s0, s1, decline_fetch));
    compare_or_fail(&e, "fetch cache: run C (restored state, same sequence, page remapped)");
    T1H_CHECK(DELTA(s0, s1, fetch_cache_hits) == 0 && DELTA(s0, s1, fetch_guards) >= 1 &&
              DELTA(s0, s1, decline_fetch) >= 1);
    printf(
        "upper: fetch-guard cache: a restored state with the cached I-side sequence but the second page "
        "remapped: the entry ran the guard (no hit) and declined; equal to the reference\n");
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    snapshot_free(&other);
}

/* Run D: the cache names the region (id and generation). Region B is the
 * loop compiled at its VRN 4 alias; its second page's alias mapping is
 * then remapped (sequence q), so B's guard fails at q. Region A, the loop
 * at VRN 0, is compiled into the same cache slot (records are allocated
 * until A's id is congruent to B's) and entered at q: the slot holds
 * (A, q). B's entry at the same q and PSR must run its own guard and
 * decline. The hooks are called as Tier0 calls them; between the two
 * entries v's registers return to the header (the MMU state, sequence q,
 * is unchanged, so this is not a restore the engine must hear of). */
static void fetch_cache_collision(void)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    struct t1_vcpu *tv = t1h_tier1_vcpu(ev.ctx);
    struct t1_compile_options opt = {0};
    t1_test_set_compile_options(tv, &opt);
    tier0_flush();
    t1_flush();
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    M.v->upper = tv;
    M.v->cfg.engine = ENGINE_TIER1;
    uint64_t header = M.v->s.ip;
    for (unsigned k = 0; k < 2; ++k)
        ia64_tc_insert(M.v, VRN(4) | (T1H_CODE + k * 0x1000), 0, code_pte(T1H_CODE + k * 0x1000),
                       12u << 2);
    M.v->s.ip = VRN(4) | header;
    struct t1_compile_report rep;
    T1H_CHECK(t1_compile(tv, &opt, &rep) == T1_COMPILED);
    struct t1_region *b = t1_region_get(rep.region_id);
    T1H_CHECK(b && b->npages == 2 && b->page[1].pa == T1H_CODE + 0x1000);
    ia64_tc_insert(M.v, VRN(4) | (T1H_CODE + 0x1000), 0, code_pte(FETCH_X), 12u << 2);
    M.v->s.ip = header;
    struct snapshot at_q;
    snapshot_init(&at_q);
    snapshot_take(&at_q, M.v);
    uint64_t q = M.v->mmu_seq;
    struct t1_region *hold[4 * T1_FETCH_CACHE];
    unsigned nhold = 0;
    for (;;) {
        struct t1_region *d = t1_region_new();
        T1H_CHECK(d && nhold < 4 * T1_FETCH_CACHE);
        if (d->id % T1_FETCH_CACHE == b->id % T1_FETCH_CACHE) {
            t1_region_discard(d); /* its id is the next one taken */
            break;
        }
        hold[nhold++] = d;
    }
    T1H_CHECK(t1_compile(tv, &opt, &rep) == T1_COMPILED);
    struct t1_region *a = t1_region_get(rep.region_id);
    for (unsigned k = 0; k < nhold; ++k)
        t1_region_discard(hold[k]);
    T1H_CHECK(a && a != b && a->npages == 2 && a->id % T1_FETCH_CACHE == b->id % T1_FETCH_CACHE);
    struct vcpu_exit out;
    struct t1_stats s0 = stats_now();
    uint64_t la = t1_upper_probe(M.v, a->c.entry_pa);
    T1H_CHECK(t1_region_from_link(la) == a);
    enum t1_upper_status st = t1_upper_enter(M.v, &la, BIG_BUDGET, &out);
    struct t1_stats s1 = stats_now();
    T1H_CHECK((st == T1_UPPER_RAN || st == T1_UPPER_RAN_PENDING) && M.v->mmu_seq == q &&
              DELTA(s0, s1, fetch_guards) == 1 && DELTA(s0, s1, fetch_cache_hits) == 0);
    snapshot_restore(&at_q, M.v); /* registers only: MMU and sequence as cached */
    M.v->s.ip = VRN(4) | header;
    uint64_t lb = t1_region_link(b), retired = M.v->retired;
    T1H_CHECK(t1_upper_enter(M.v, &lb, BIG_BUDGET, &out) == T1_UPPER_DECLINED &&
              M.v->retired == retired);
    struct t1_stats s2 = stats_now();
    T1H_CHECK(DELTA(s1, s2, fetch_cache_hits) == 0 && DELTA(s1, s2, fetch_guards) == 1 &&
              DELTA(s1, s2, decline_fetch) == 1 && DELTA(s1, s2, entries) == 0);
    unsigned ida = a->id, idb = b->id;
    M.v->upper = NULL;
    M.v->cfg.engine = ENGINE_INTERP;
    t1h_engine_release(&ev);
    snapshot_free(&at_q);
    t1_flush();
    printf(
        "upper: fetch-guard cache: regions %u and %u share a cache slot; after the loop's entry at VRN 0 "
        "filled it at sequence %llu, the VRN 4 alias region (second page remapped) at the same sequence "
        "ran its own guard and declined\n",
        ida, idb, (unsigned long long)q);
}

/* ---- validator stays over a region's page set -------
 *
 * gather-2page: the loop's region has source pages 0x100000 and 0x101000.
 * The entry that begins a stay covers both pages, so in steady state
 * neither the loop's entries nor the code between them on either page
 * begin stays, and bundles are compared only at their first execution in
 * the stay (a few, not one per entry as when each entry compared its
 * second page). */
static const struct t1_region *two_page_region(void)
{
    for (uint32_t id = 1; id <= t1_cfg.max_regions; ++id) {
        const struct t1_region *r = t1_region_get(id);
        if (r && r->valid && r->npages == 2 && r->entries)
            return r;
    }
    return NULL;
}

static void stay_page_set(void)
{
    static const struct guest_program prog = {"gather_2page", "gather_2page.bin", GUEST_GATHER, 0};
    struct guest_params q = guest_completion_params(&prog);
    q.passes = 160;
    q.bench = true; /* the output restarts every pass */
    struct vcpu_config cfg = guest_vcpu_config(&prog);
    M.ref = vcpu_create(&cfg);
    M.v = vcpu_create(&cfg);
    T1H_CHECK(M.ref && M.v);
    t1h_bind(M.ref, &M.ref_logs);
    t1h_bind(M.v, &M.logs);
    M.ref_logs.stores_on = M.logs.stores_on = false;
    guest_setup(&prog, &q, M.ref);
    snapshot_take(&M.snap, M.ref);
    struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    uint64_t total = M.ref->retired - M.snap.v.retired;
    result_take(&M.reference, &re, M.ref);
    static const char *const engines[] = {"tier0+tier1", "tier1"};
    for (unsigned which = 0; which < 2; ++which) {
        const struct t1h_engine *engine = t1h_engine_named(engines[which]);
        t1_flush();
        struct t1h_engine_vcpu ev;
        t1h_engine_bind(&ev, engine, M.v);
        t1h_tier1_set_flags(0);
        t1h_tier0t1_set_flags(0);
        snapshot_restore(&M.snap, M.v);
        t1h_engine_restored(&ev);
        T1H_CHECK(t1h_engine_run(&ev, total / 2).reason == VCPU_EXIT_BUDGET);
        const struct t1_region *r = two_page_region();
        T1H_CHECK(r != NULL);
        uint64_t entries0 = r->entries;
        struct code_stay_stats a, b;
        struct code_validity_stats ca, cb;
        code_stay_get_stats(&a);
        code_validity_get_stats(&ca);
        T1H_CHECK(t1h_engine_run(&ev, total / 4).reason == VCPU_EXIT_BUDGET);
        code_stay_get_stats(&b);
        code_validity_get_stats(&cb);
        uint64_t stays = (b.page + b.reset + b.event + b.epoch + b.fetch) -
                         (a.page + a.reset + a.event + a.epoch + a.fetch);
        uint64_t entries = r->entries - entries0;
        char label[96];
        snprintf(label, sizeof(label), "page set gather_2page/%s", engine->name);
        if (getenv("T1H_UPPER_TRACE"))
            fprintf(stderr,
                    "%s: %llu entries, %llu stays, %llu bundles compared, %llu comparisons\n",
                    label, (unsigned long long)entries, (unsigned long long)stays,
                    (unsigned long long)(b.bundles_compared - a.bundles_compared),
                    (unsigned long long)(cb.comparisons - ca.comparisons));
        uint64_t comparisons = cb.comparisons - ca.comparisons;
        T1H_CHECK(entries >= 16 && stays == 0 && 4 * comparisons < entries);
        struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
        T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && M.logs.last_break_imm == T1H_BREAK_END);
        compare_or_fail(&e, label);
        printf("upper: %s: %llu entries of the two-page region in steady state, 0 new stays, %llu "
               "comparisons; equal to the reference\n",
               label, (unsigned long long)entries, (unsigned long long)comparisons);
        t1h_tier1_set_flags(T1_COMPILE_VERIFY);
        t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
        t1h_engine_release(&ev);
    }
    t1_flush();
    machine_close();
}

static void fetch_cache(void)
{
    static const struct guest_program prog = {"gather_2page_virt", "gather_2page.bin", GUEST_GATHER,
                                              GUEST_VIRT};
    struct guest_params q = guest_completion_params(&prog);
    q.passes = 12;
    struct vcpu_config cfg = guest_vcpu_config(&prog);
    M.ref = vcpu_create(&cfg);
    M.v = vcpu_create(&cfg);
    T1H_CHECK(M.ref && M.v);
    t1h_bind(M.ref, &M.ref_logs);
    t1h_bind(M.v, &M.logs);
    M.ref_logs.stores_on = M.logs.stores_on = false;
    guest_setup(&prog, &q, M.ref);
    struct vcpu *v = M.ref;
    /* Page 0 by ITR0 (4 KiB), the IVT by ITR1 (32 KiB), page 0x101000 by a TC. */
    uint64_t rid = (v->s.rr[0] >> 8) & 0xffffff;
    v->s.tr[0][0] = (struct ia64_tlb_entry){.va = T1H_CODE,
                                            .pte = code_pte(T1H_CODE),
                                            .itir = 12u << 2,
                                            .rid = rid,
                                            .page_shift = 12,
                                            .valid = true};
    v->s.tr[0][1] = (struct ia64_tlb_entry){.va = T1H_IVT,
                                            .pte = code_pte(T1H_IVT),
                                            .itir = 15u << 2,
                                            .rid = rid,
                                            .page_shift = 15,
                                            .valid = true};
    ia64_tc_insert(v, T1H_CODE + 0x1000, 0, code_pte(T1H_CODE + 0x1000), 12u << 2);
    uint64_t header;
    T1H_CHECK(t1h_run_to_loop_header(v, 100000, &header));
    struct snapshot start;
    snapshot_init(&start);
    snapshot_take(&start, v);
    /* The end bundle (the reference's stop), copied over the empty page. */
    struct vcpu_exit re = ref_run(v, BIG_BUDGET, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    uint64_t end = v->s.ip, total = v->retired - start.v.retired;
    T1H_CHECK((end >> 12) == ((T1H_CODE + 0x1000) >> 12));
    snapshot_restore(&start, v);
    uint8_t *ram = t1h_ram();
    for (unsigned k = 0; k < 4096; k += 16)
        memcpy(ram + (FETCH_X - T1H_RAM_BASE) + k, ram + (end - T1H_RAM_BASE), 16);
    code_notify_vmm_write(FETCH_X, 4096);
    snapshot_take(&M.snap, v);
    snapshot_free(&start);

    uint64_t end_a, end_b;
    struct t1_stats a = fetch_case(0, fetch_remap, "fetch cache: run A", &end_a);
    if (getenv("T1H_UPPER_TRACE"))
        fprintf(stderr, "fetch A: entries %llu guards %llu hits %llu declines %llu\n",
                (unsigned long long)a.entries, (unsigned long long)a.fetch_guards,
                (unsigned long long)a.fetch_cache_hits, (unsigned long long)a.decline_fetch);
    T1H_CHECK(end_a == end && a.entries >= 8 && a.decline_fetch == 0);
    T1H_CHECK(a.fetch_cache_hits >= 1 && a.fetch_guards >= 1 &&
              a.fetch_guards + a.fetch_cache_hits >= a.entries);
    /* The remap at a header arrival after half the run: the next thing the
     * engine does is a Tier1 entry through the cached guard. */
    snapshot_restore(&M.snap, v);
    T1H_CHECK(ref_run(v, total / 2, NULL).reason == VCPU_EXIT_BUDGET);
    while (v->s.ip != header || v->s.slot)
        T1H_CHECK(ref_run(v, 1, NULL).reason == VCPU_EXIT_BUDGET);
    uint64_t remap_at = v->retired - M.snap.v.retired;
    struct t1_stats b =
        fetch_case(remap_at, fetch_remap, "fetch cache: run B (page remapped)", &end_b);
    if (getenv("T1H_UPPER_TRACE"))
        fprintf(stderr, "fetch B: entries %llu guards %llu hits %llu declines %llu end %#llx\n",
                (unsigned long long)b.entries, (unsigned long long)b.fetch_guards,
                (unsigned long long)b.fetch_cache_hits, (unsigned long long)b.decline_fetch,
                (unsigned long long)end_b);
    /* The reference stopped in the remapped page, before the old end. */
    T1H_CHECK(end_b != end && (end_b >> 12) == ((T1H_CODE + 0x1000) >> 12));
    T1H_CHECK(b.entries >= 2 && b.fetch_cache_hits >= 1 && b.decline_fetch >= 1);
    printf(
        "upper: fetch-guard cache (gather-2page virtual, two code pages): run A %llu entries, %llu guard runs, "
        "%llu cache hits; run B, the second page remapped by an I-side TC insert at the header after %llu "
        "instructions: %llu entries before it, the next entry missed and the guard declined (%llu), Tier0 "
        "stopped at the remapped page's break; both equal to the reference\n",
        (unsigned long long)a.entries, (unsigned long long)a.fetch_guards,
        (unsigned long long)a.fetch_cache_hits, (unsigned long long)remap_at,
        (unsigned long long)b.entries, (unsigned long long)b.decline_fetch);
    uint64_t end_d;
    struct t1_stats d =
        fetch_case(remap_at, fetch_data_change, "fetch cache: D-side replacements", &end_d);
    T1H_CHECK(end_d == end && d.entries >= 8 && d.decline_fetch == 0 &&
              d.fetch_guards == a.fetch_guards && d.fetch_cache_hits >= 1);
    printf("upper: fetch-guard cache: D-side replacements and log overflow retain the proof\n");
    fetch_cache_keys();
    fetch_cache_restore();
    fetch_cache_collision();
    machine_close();
}

/* ---- the arena -------------------------------- */

#define CSUITE_STACK UINT64_C(0x1f0000)
/* The executor is registered for the code epochs, as in the product. */
static bool csuite_registered;
/* upper_memory_tests: a deferred head is asked again at the harness
 * threshold (every few entries), a doubling schedule only a few times per
 * kernel; the small pool holds the tables of a few regions, and a working
 * set many times larger compiles only through eviction. */
#define MEMORY_DEFERRED_MIN 100000u
#define MEMORY_ROOM (UINT64_C(256) << 10)
#define MEMORY_COMPILES_MIN 100u
/* Each kernel of the compiled-C suite (tests/bench/csuite) from one image
 * load, `rounds` times, on tier0+tier1 with production regions, compared
 * with the interpreter at the end. Loading the image once keeps the code
 * pages unchanged: no invalidation, so every retirement is the arena's. */
static void csuite_rounds(unsigned rounds, uint64_t iters, const uint32_t *flags, const char *what)
{
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    M.ref = vcpu_create(&cfg);
    M.v = vcpu_create(&cfg);
    T1H_CHECK(M.ref && M.v);
    t1h_bind(M.ref, &M.ref_logs);
    t1h_bind(M.v, &M.logs);
    M.ref_logs.stores_on = M.logs.stores_on = false;
    t1h_load_image("csuite.bin");
    struct ia64_entry_regs regs = {.ip = T1H_CODE,
                                   .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN,
                                   .bsp = T1H_BSP,
                                   .bspstore = T1H_BSP};
    regs.gr[3] = iters;
    regs.gr[12] = CSUITE_STACK;
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    if (csuite_registered)
        code_register_vcpu(M.v);
    for (unsigned round = 0; round < rounds; ++round)
        for (unsigned k = 0; k < 6; ++k) {
            t1h_tier0t1_set_flags(flags[round]);
            regs.gr[2] = k;
            vcpu_reset(M.ref, &regs);
            M.ref->s.cr[CR_IVA] = T1H_IVT;
            snapshot_take(&M.snap, M.ref);
            struct vcpu_exit re = interp_run(M.ref, BIG_BUDGET);
            T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END &&
                      M.ref->s.gr[8]);
            result_take(&M.reference, &re, M.ref);
            /* Replay the same kernel before moving to the next contract.
             * This exercises surviving relocated code without depending on
             * the compiled byte sizes of six different kernels fitting a
             * particular six-round eviction schedule. */
            for (unsigned replay = 0; replay < 2; ++replay) {
                snapshot_restore(&M.snap, M.v);
                t1h_engine_restored(&ev);
                struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
                char label[96];
                snprintf(label, sizeof(label), "%s: csuite kernel %u round %u replay %u", what, k,
                         round, replay);
                compare_or_fail(&e, label);
            }
            /* The data the kernel changed back to the image's (code pages
             * are unchanged and stay valid). */
            snapshot_restore(&M.snap, M.ref);
        }
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    if (csuite_registered)
        code_unregister_vcpu(M.v);
    t1h_engine_release(&ev);
    machine_close();
}

/* t1_init again with a changed configuration (stopped executors), keeping
 * the harness's other settings; restored by the second call. */
static struct t1_config saved_cfg;
static void tier1_reconfigure(uint64_t code_bytes, uint32_t max_regions)
{
    saved_cfg = t1_cfg;
    struct t1_config c = t1_cfg;
    c.code_bytes = code_bytes;
    c.max_regions = max_regions;
    tier0_flush();
    t1_shutdown();
    T1H_CHECK(t1_init(&c));
}
static void tier1_restore_config(void)
{
    tier0_flush();
    t1_shutdown();
    T1H_CHECK(t1_init(&saved_cfg));
}

static void arena_pressure(void)
{
    /* tier1_code_mib=1 (the minimum): 15 chunks, of which 8 are held raw,
     * so 7 serve the compiled-C working set, about half of it. */
    tier1_reconfigure(UINT64_C(1) << 20, 4096);
    uint8_t *held[8];
    for (unsigned k = 0; k < 8; ++k)
        T1H_CHECK((held[k] = t1_arena_alloc(1)) != NULL);
    struct t1_stats s0 = stats_now();
    /* Six rounds over six kernels and four compile contracts exceed the
     * arena. Returning to earlier contracts exercises eviction, re-placement
     * and recompilation without code invalidation. */
    static const uint32_t rounds6[] = {
        0, T1_COMPILE_VERIFY, T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG, T1_COMPILE_STORE_LOG,
        0, T1_COMPILE_VERIFY};
    csuite_rounds(6, 3, rounds6, "arena 1 MiB");
    for (unsigned k = 0; k < 8; ++k)
        t1_arena_free(held[k], 1);
    struct t1_stats s1 = stats_now();
    if (getenv("T1H_UPPER_TRACE"))
        fprintf(stderr, "arena: bytes used %llu arena_bytes stat %llu\n",
                (unsigned long long)t1_arena_bytes_used(), (unsigned long long)s1.arena_bytes);
    printf(
        "upper: arena tier1_code_mib=1, compiled C (6 kernels, 6 rounds of 4 compile-flag sets): %llu compiles, %llu entries, %llu "
        "seals, %llu chunk recycles, %llu evictions, %llu re-placements (%llu entries into re-placed code), "
        "%llu recompiles of evicted contracts, %llu invalidations; equal to the interpreter\n",
        (unsigned long long)DELTA(s0, s1, compiles), (unsigned long long)DELTA(s0, s1, entries),
        (unsigned long long)DELTA(s0, s1, seals), (unsigned long long)DELTA(s0, s1, chunk_recycles),
        (unsigned long long)DELTA(s0, s1, evictions),
        (unsigned long long)DELTA(s0, s1, replacements),
        (unsigned long long)DELTA(s0, s1, moved_entries),
        (unsigned long long)DELTA(s0, s1, recompiles),
        (unsigned long long)DELTA(s0, s1, invalidations));
    /* A working set larger than the arena, moving on to new hot code with
     * no invalidation: evictions make the room; hot regions move instead of
     * being recompiled, and their moved code runs. */
    T1H_CHECK(DELTA(s0, s1, invalidations) == 0 && DELTA(s0, s1, evictions) > 0);
    T1H_CHECK(DELTA(s0, s1, replacements) > 0 && DELTA(s0, s1, moved_entries) > 0);
    T1H_CHECK(DELTA(s0, s1, recompiles) > 0 && DELTA(s0, s1, chunk_recycles) > 0);
    T1H_CHECK(DELTA(s0, s1, seals) <= DELTA(s0, s1, compiles) + DELTA(s0, s1, replacements));
    tier1_restore_config();

    /* Many small regions: 32 records for the same work. */
    tier1_reconfigure(UINT64_C(8) << 20, 32);
    s0 = stats_now();
    static const uint32_t round0[] = {0};
    csuite_rounds(1, 3, round0, "32 region records");
    s1 = stats_now();
    printf(
        "upper: arena max_regions=32, compiled C: %llu compiles, %llu evictions (record pressure), %llu "
        "invalidations; equal to the interpreter\n",
        (unsigned long long)DELTA(s0, s1, compiles), (unsigned long long)DELTA(s0, s1, evictions),
        (unsigned long long)DELTA(s0, s1, invalidations));
    T1H_CHECK(DELTA(s0, s1, compiles) > 32 && DELTA(s0, s1, evictions) > 0 &&
              DELTA(s0, s1, invalidations) == 0);
    tier1_restore_config();
}

/* ---- engine=tier1 setup errors ---------------------- */

static void setup_errors(void)
{
    /* Options and the validity mode are checked before anything is set up. */
    static const char *const bad[] = {"tier1_code_mib=0",    "tier1_code_mib=1025",
                                      "tier1_code_mib=x",    "tier1_hot=0",
                                      "tier1_slice=0",       "hostwin_check=2",
                                      "tier1_hot=",          "tier1_verify=",
                                      "tier1_verify=x",      "tier1_verify=pass",
                                      "tier1_verify=finals", "tier1_verify=FINAL",
                                      "tier1_check_every=x", "tier1_check_every=4294967296",
                                      "tier1_check_after=1", "tier1_chain=",
                                      "tier1_chain=x",       "tier1_chain=On",
                                      "tier1_chain=offf",    "tier1_regions=0",
                                      "tier1_regions=",      "tier1_regions=1048576",
                                      "tier1_regions=x",     "tier1_min_insns=0",
                                      "tier1_min_insns=",    "tier1_min_insns=257",
                                      "tier1_min_insns=y"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        T1H_CHECK(!engine_tier1_configure(bad[i], CODE_VALIDITY_COMPARE_ALWAYS));
    T1H_CHECK(!engine_tier1_configure("", CODE_VALIDITY_NONE));
    /* A host without a window backend reports a setup error, never Tier1
     * without windows. This binary gets its backend from the Linux
     * definition of hostwin_host_backend. */
    void *ctx;
    bool backend = hostwin_host_backend(&ctx) != NULL;
    if (!backend)
        T1H_CHECK(!engine_tier1_configure("", CODE_VALIDITY_COMPARE_ALWAYS));
    if (backend) {
        pid_t smp = fork();
        T1H_CHECK(smp >= 0);
        if (!smp) {
            t1_shutdown();
            T1H_CHECK(!hal_linux_configure(true, 2, 1, NULL, 0));
            static const struct {
                enum engine_kind engine;
                const char *options;
                bool replay;
            } cases[] = {{ENGINE_TIER1, "tier1_check_every=2", true},
                         {ENGINE_LOCKSTEP, "", true},
                         {ENGINE_TIER1, "", false}};
            for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
                T1H_CHECK(
                    cases[i].engine == ENGINE_LOCKSTEP
                        ? engine_tier1_lockstep_configure(cases[i].options,
                                                          CODE_VALIDITY_COMPARE_ALWAYS)
                        : engine_tier1_configure(cases[i].options, CODE_VALIDITY_COMPARE_ALWAYS));
                const struct t1_engine_glue *g = engine_tier1_glue();
                struct vcpu *cpu[2];
                struct t1_vcpu *tv[2];
                for (unsigned k = 0; k < 2; ++k) {
                    struct vcpu_config cfg = {.engine = cases[i].engine, .index = k};
                    cpu[k] = vcpu_create(&cfg);
                    T1H_CHECK(cpu[k]);
                    tv[k] = g->vcpu_create(cpu[k]);
                    uint32_t required =
                        cases[i].replay
                            ? T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG | T1_COMPILE_ACCESS_LOG
                            : 0;
                    T1H_CHECK(tv[k] && tv[k]->opt.flags == required);
                }
                for (unsigned k = 0; k < 2; ++k) {
                    g->vcpu_destroy(tv[k]);
                    vcpu_destroy(cpu[k]);
                }
                engine_tier1_destroy();
            }
            _exit(0);
        }
        int smp_status;
        T1H_CHECK(waitpid(smp, &smp_status, 0) == smp && WIFEXITED(smp_status) &&
                  !WEXITSTATUS(smp_status));
        pid_t pid = fork();
        T1H_CHECK(pid >= 0);
        if (!pid) {
            t1_shutdown();
            T1H_CHECK(engine_tier1_configure("tier1_check_every=2", CODE_VALIDITY_COMPARE_ALWAYS));
            char lines[2][LOG_MATCH_COLUMNS + 1];
            size_t kept;
            T1H_CHECK(log_ring_matching(log_get_ring(), "[warn] tier1 replay coverage:", lines, 2,
                                        &kept) == 1);
            T1H_CHECK(kept == 1 && strstr(lines[0], "store preloads/logging") &&
                      strstr(lines[0], "256-instruction budget") &&
                      strstr(lines[0], "memory-helper boundaries"));
            struct vcpu_config cfg = {.engine = ENGINE_TIER1};
            struct vcpu *v = vcpu_create(&cfg);
            T1H_CHECK(v);
            const struct t1_engine_glue *g = engine_tier1_glue();
            struct t1_vcpu *tv = g->vcpu_create(v);
            T1H_CHECK(tv && tv->opt.flags == (T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG));
            g->vcpu_destroy(tv);
            vcpu_destroy(v);
            t1_shutdown();
            _exit(0);
        }
        int status;
        T1H_CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
        /* tier1_chain=on|off, default on. */
        pid_t chain = fork();
        T1H_CHECK(chain >= 0);
        if (!chain) {
            static const struct {
                const char *option;
                bool off;
            } cases[] = {{"", false}, {"tier1_chain=on", false}, {"tier1_chain=off", true}};
            for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
                t1_shutdown();
                T1H_CHECK(engine_tier1_configure(cases[i].option, CODE_VALIDITY_COMPARE_ALWAYS));
                T1H_CHECK(t1_cfg.chain_off == cases[i].off);
                engine_tier1_destroy();
            }
            _exit(0);
        }
        T1H_CHECK(waitpid(chain, &status, 0) == chain && WIFEXITED(status) && !WEXITSTATUS(status));
        /* tier1_regions=N: the region table's size; without
         * it the table keeps the default until the pool is sized. */
        pid_t regions = fork();
        T1H_CHECK(regions >= 0);
        if (!regions) {
            static const struct {
                const char *option;
                uint32_t capacity;
            } cases[] = {{"", T1_MAX_REGIONS_DEFAULT},
                         {"tier1_regions=100 tier1_code_mib=17", 100},
                         {"tier1_regions=1048575", T1_MAX_REGIONS_LIMIT}};
            for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
                t1_shutdown();
                t1_memory_set_limit(UINT64_C(48) << 20);
                T1H_CHECK(engine_tier1_configure(cases[i].option, CODE_VALIDITY_COMPARE_ALWAYS));
                struct t1_stats st;
                t1_get_stats(&st);
                T1H_CHECK(t1_cfg.max_regions == cases[i].capacity &&
                          st.region_capacity == cases[i].capacity);
                /* Sizing the pool sizes a default table: two pages per
                 * record of the allowance (48 MiB for unknown capacity), at least the
                 * default; a configured size stays. */
                engine_tier1_memory_budget(UINT64_C(1) << 40, 0, 0);
                uint32_t sized = i ? cases[i].capacity : (48u << 20) / T1_REGION_MIN_BYTES;
                t1_get_stats(&st);
                T1H_CHECK(t1_cfg.max_regions == sized && st.region_capacity == sized);
                engine_tier1_memory_budget(UINT64_C(16) << 20, 0, 0);
                T1H_CHECK(t1_cfg.max_regions == sized);
                uint64_t code_bytes = t1_cfg.code_bytes;
                uint64_t workspace = st.memory_reserve;
                T1H_CHECK(i != 1 || code_bytes == (17u << 20));
                engine_tier1_memory_budget(UINT64_C(512) << 20, 0, UINT64_C(1) << 30);
                t1_get_stats(&st);
                sized = i ? cases[i].capacity : (128u << 20) / T1_REGION_MIN_BYTES;
                T1H_CHECK(t1_cfg.max_regions == sized && st.region_capacity == sized);
                T1H_CHECK(t1_cfg.code_bytes == code_bytes && st.memory_reserve == workspace);
                T1H_CHECK(st.memory_budget == st.memory_bytes + (128u << 20));
                struct vcpu_config cfg = {.engine = ENGINE_INTERP};
                struct vcpu *v = vcpu_create(&cfg);
                T1H_CHECK(v && code_cache_vcpu_init(v));
                uint64_t lazy = code_cache_reserve_bytes();
                T1H_CHECK(lazy);
                engine_tier1_memory_budget(lazy + (64u << 20), 32u << 20, UINT64_C(1) << 30);
                t1_get_stats(&st);
                T1H_CHECK(st.memory_budget == st.memory_bytes + (24u << 20));
                engine_tier1_memory_budget(lazy, UINT64_MAX, UINT64_C(1) << 30);
                t1_get_stats(&st);
                T1H_CHECK(st.memory_budget == st.memory_bytes);
                vcpu_destroy(v);
                engine_tier1_destroy();
            }
            t1_memory_set_limit(UINT64_C(48) << 20);
            T1H_CHECK(engine_tier1_configure("", CODE_VALIDITY_COMPARE_ALWAYS));
            engine_tier1_memory_budget(UINT64_C(8) << 30, 0, UINT64_C(16) << 30);
            T1H_CHECK(t1_cfg.max_regions == (UINT64_C(1) << 30) / T1_REGION_MIN_BYTES);
            engine_tier1_destroy();
            t1_memory_set_limit(UINT64_C(48) << 20);
            T1H_CHECK(engine_tier1_configure("", CODE_VALIDITY_COMPARE_ALWAYS));
            struct t1_memory_stats before, after;
            t1_memory_get_stats(&before);
            engine_tier1_memory_budget(UINT64_C(64) << 20, 0, UINT64_C(16) << 30);
            t1_memory_get_stats(&after);
            T1H_CHECK(after.used_bytes > before.used_bytes);
            uint64_t left = (UINT64_C(64) << 20) - (after.used_bytes - before.used_bytes);
            T1H_CHECK(after.budget_bytes == after.used_bytes + ((left * 3 / 4) & ~UINT64_C(4095)));
            engine_tier1_destroy();
            _exit(0);
        }
        T1H_CHECK(waitpid(regions, &status, 0) == regions && WIFEXITED(status) &&
                  !WEXITSTATUS(status));
        /* tier1_min_insns=N: the profitability threshold. */
        pid_t min = fork();
        T1H_CHECK(min >= 0);
        if (!min) {
            static const struct {
                const char *option;
                uint32_t value;
            } cases[] = {{"", T1_MIN_INSNS_DEFAULT},
                         {"tier1_min_insns=1", 1},
                         {"tier1_min_insns=24", 24},
                         {"tier1_min_insns=256", 256}};
            for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
                t1_shutdown();
                T1H_CHECK(engine_tier1_configure(cases[i].option, CODE_VALIDITY_COMPARE_ALWAYS));
                T1H_CHECK(t1_cfg.min_insns_per_entry == cases[i].value);
                engine_tier1_destroy();
            }
            _exit(0);
        }
        T1H_CHECK(waitpid(min, &status, 0) == min && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    printf(
        "upper: engine=tier1 setup errors: %zu invalid options and code_validity=none refused%s\n",
        sizeof(bad) / sizeof(bad[0]),
        backend ? "" : "; no hostwin backend in this binary: refused");
}

/* ---- chainable exits ------------------------------------
 * An EXIT exit is chainable when the next Tier1 entry starts at its target
 * with no instruction retired in between. The test observes that directly:
 * the restore observer sees every exit of a verification region (its point
 * and v at the target), the entry hook every entry. Their count must equal
 * the counter, on gather (regions alternating in a loop: some exits chainable)
 * and on rse-return (calls and returns run in Tier0 between regions: fewer than
 * the exits). Every run equals the reference. */
static struct {
    uint64_t retired, ip;
    unsigned slot;
    bool valid;
    uint64_t exits, follows, ends[T1_END_CLASSES];
} chain_obs;
static void chain_observe(struct t1_vcpu *tv, const struct vcpu *v,
                          const struct t1_restore_record *r, void *arg)
{
    (void)tv;
    (void)arg;
    if (!r->materialized)
        return;
    chain_obs.valid = false;
    for (unsigned i = 0; i < t1_point_count(r->region_id); ++i) {
        struct t1_point_info pi;
        if (t1_point_get(r->region_id, i, &pi) && pi.point_id == r->point_id &&
            pi.kind == T1_PT_EXIT) {
            chain_obs.valid = true;
            ++chain_obs.exits;
            T1H_CHECK(pi.end < T1_END_CLASSES);
            ++chain_obs.ends[pi.end];
        }
    }
    chain_obs.retired = v->retired;
    chain_obs.ip = v->s.ip;
    chain_obs.slot = v->s.slot;
}
static void chain_entry(struct t1_vcpu *tv, uint32_t region, void *arg)
{
    (void)region;
    (void)arg;
    const struct vcpu *v = tv->v;
    if (chain_obs.valid && chain_obs.retired == v->retired && chain_obs.ip == v->s.ip &&
        chain_obs.slot == v->s.slot)
        ++chain_obs.follows;
    chain_obs.valid = false;
}
static struct t1_stats chain_count_run(const char *program, unsigned passes, const char *label)
{
    const struct guest_program *p = guest_program(program);
    struct guest_params q = guest_completion_params(p);
    if (passes) {
        q.passes = passes;
        q.bench = true;
    }
    struct vcpu_config cfg = guest_vcpu_config(p);
    M.ref = vcpu_create(&cfg);
    M.v = vcpu_create(&cfg);
    T1H_CHECK(M.ref && M.v);
    t1h_bind(M.ref, &M.ref_logs);
    t1h_bind(M.v, &M.logs);
    M.ref_logs.stores_on = M.logs.stores_on = false;
    guest_setup(p, &q, M.ref);
    snapshot_take(&M.snap, M.ref);
    struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    result_take(&M.reference, &re, M.ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    tier0_flush();
    t1_flush();
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    struct t1_compile_options opt = {T1_COMPILE_VERIFY, 0, 0};
    t1_test_set_compile_options(t1h_tier1_vcpu(ev.ctx), &opt);
    t1_test_set_restore_observer(chain_observe, NULL);
    t1_test_set_entry_hook(chain_entry, NULL);
    memset(&chain_obs, 0, sizeof(chain_obs));
    struct t1_stats s0 = stats_now();
    struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
    struct t1_stats s1 = stats_now();
    t1_test_set_restore_observer(NULL, NULL);
    t1_test_set_entry_hook(NULL, NULL);
    compare_or_fail(&e, label);
    uint64_t retired = M.v->retired;
    t1h_engine_release(&ev);
    machine_close();
    T1H_CHECK(chain_obs.exits == DELTA(s0, s1, exits_by_kind[T1_PT_EXIT]));
    T1H_CHECK(chain_obs.follows == DELTA(s0, s1, chainable));
    /* Exits by the class of their target, and the returns to Tier0 at
     * them: every return at an instruction Tier1 does not execute is
     * followed by at least that instruction in the lower tier, and every
     * instruction counted in a gap ran outside Tier1. */
    struct t1_stats d = {.exits_by_kind = {DELTA(s0, s1, exits_by_kind[T1_PT_EXIT])},
                         .chainable = DELTA(s0, s1, chainable)};
    uint64_t gaps = 0;
    for (unsigned c = 0; c < T1_END_CLASSES; ++c) {
        d.exits_by_end[c] = DELTA(s0, s1, exits_by_end[c]);
        d.returns_by_end[c] = DELTA(s0, s1, returns_by_end[c]);
        d.gap_by_end[c] = DELTA(s0, s1, gap_by_end[c]);
        if (chain_obs.ends[c] != d.exits_by_end[c])
            fprintf(stderr, "upper: %s: class %u: %llu exits observed, %llu counted\n", label, c,
                    (unsigned long long)chain_obs.ends[c], (unsigned long long)d.exits_by_end[c]);
        T1H_CHECK(chain_obs.ends[c] == d.exits_by_end[c] &&
                  d.returns_by_end[c] <= d.exits_by_end[c]);
        if (c >= T1_END_CALL)
            T1H_CHECK(d.gap_by_end[c] >= d.returns_by_end[c]);
        gaps += d.gap_by_end[c];
    }
    T1H_CHECK(gaps <= retired - DELTA(s0, s1, tier1_retired));
    return d;
}
static void chain_counts(void)
{
    struct t1_stats gather = chain_count_run("gather_phys", 200, "chainable: gather");
    struct t1_stats rse = chain_count_run("rse_return", 0, "chainable: rse-return");
    T1H_CHECK(gather.chainable > 0 && rse.exits_by_kind[T1_PT_EXIT] > rse.chainable);
    /* rse-return: the callee of .Lcall's br.call starts with flushrs, so it
     * does not return within a region: the region at .Lcall ends at the
     * call. */
    T1H_CHECK(rse.exits_by_end[T1_END_CALL] > 0 && rse.returns_by_end[T1_END_CALL] > 0);
    printf(
        "upper: chainable exits: gather %llu of %llu EXIT exits, rse-return %llu of %llu, each equal to the exits the "
        "restore observer saw followed by an entry at the target with nothing retired in between; rse-return exits "
        "by end limit/branch/call/ret/alloc/rse/nat/other %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu, %llu "
        "instructions after %llu returns at calls\n",
        (unsigned long long)gather.chainable, (unsigned long long)gather.exits_by_kind[T1_PT_EXIT],
        (unsigned long long)rse.chainable, (unsigned long long)rse.exits_by_kind[T1_PT_EXIT],
        (unsigned long long)rse.exits_by_end[0], (unsigned long long)rse.exits_by_end[1],
        (unsigned long long)rse.exits_by_end[2], (unsigned long long)rse.exits_by_end[3],
        (unsigned long long)rse.exits_by_end[4], (unsigned long long)rse.exits_by_end[5],
        (unsigned long long)rse.exits_by_end[6], (unsigned long long)rse.exits_by_end[7],
        (unsigned long long)rse.gap_by_end[T1_END_CALL],
        (unsigned long long)rse.returns_by_end[T1_END_CALL]);
}

/* ---- call-return: calls, returns and alloc in Tier1 regions ------------------
 *
 * programs/call_return.S on the product path with verification regions, equal to the
 * stepping reference:
 * - a region follows a call into its callee and returns into the caller
 *   (one holds a call and a return);
 * - .Lsum, entered at its head from frames of 4 and 2 outputs (predicated
 *   calls leave the region at its head, SDM Vol.3 br.call: CFM.sof = sof -
 *   sol of the caller), has one region, compiled once and entered from
 *   both: alloc makes its frame (Vol.3 alloc), and the contract leaves the
 *   entry's sof, sol and sor open;
 * - .Lb0 returns to b0 + 16 and .Lpfs with another PFM: those returns
 *   leave before effect (br.ret class), and so do the taken predicated
 *   calls (br.call class);
 * - chk.s on ld8.s's NaT (physical mode defers) leaves to the recovery code;
 * - 20 levels of .Lrec: allocs that spill run in the region, and returns
 *   that need mandatory loads run in the lower tier (SDM Vol.2 §6.5). */
#define CALL_PROG_SUM (T1H_CODE + 0x400)
static struct {
    unsigned compiles, calling, nframes;
    uint64_t frames[8];
} call_prog;
static void call_prog_compiled(const struct vcpu *v, enum t1_compile_status st,
                               const struct t1_compile_report *rep, void *arg)
{
    (void)arg;
    if (st != T1_COMPILED)
        return;
    if (v->s.ip == CALL_PROG_SUM && v->s.slot == 0)
        ++call_prog.compiles;
    const struct t1_region *g = t1_region_get(rep->region_id);
    if (g && g->calls)
        ++call_prog.calling;
}
static void call_prog_entry(struct t1_vcpu *tv, uint32_t region, void *arg)
{
    (void)arg;
    const struct t1_region *g = t1_region_get(region);
    if (!g || g->c.ip != CALL_PROG_SUM || g->c.slot)
        return;
    uint64_t cfm = tv->v->s.cfm;
    for (unsigned k = 0; k < call_prog.nframes; ++k)
        if (call_prog.frames[k] == cfm)
            return;
    if (call_prog.nframes < 8)
        call_prog.frames[call_prog.nframes++] = cfm;
}
/* guest program `name` with its completion parameters on the product path
 * (verification regions, every compile and entry seen by the hooks),
 * compared with the stepping reference; the counters the run moved. */
static struct t1_stats program_run(const char *name, const char *label, t1_compile_fn compiled,
                                   t1_entry_hook_fn entered)
{
    const struct guest_program *p = guest_program(name);
    struct guest_params q = guest_completion_params(p);
    struct vcpu_config cfg = guest_vcpu_config(p);
    M.ref = vcpu_create(&cfg);
    M.v = vcpu_create(&cfg);
    T1H_CHECK(M.ref && M.v);
    t1h_bind(M.ref, &M.ref_logs);
    t1h_bind(M.v, &M.logs);
    M.ref_logs.stores_on = M.logs.stores_on = false;
    guest_setup(p, &q, M.ref);
    snapshot_take(&M.snap, M.ref);
    struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    result_take(&M.reference, &re, M.ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), M.v);
    tier0_flush();
    t1_flush();
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(&ev);
    struct t1_compile_options opt = {T1_COMPILE_VERIFY, 0, 0};
    t1_test_set_compile_options(t1h_tier1_vcpu(ev.ctx), &opt);
    t1_test_set_compile_observer(compiled, NULL);
    t1_test_set_entry_hook(entered, NULL);
    struct t1_stats s0 = stats_now();
    struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
    struct t1_stats s1 = stats_now();
    t1_test_set_compile_observer(NULL, NULL);
    t1_test_set_entry_hook(NULL, NULL);
    compare_or_fail(&e, label);
    t1h_engine_release(&ev);
    machine_close();
    struct t1_stats d = {.tier1_retired = DELTA(s0, s1, tier1_retired),
                         .entries = DELTA(s0, s1, entries),
                         .spill_exits = DELTA(s0, s1, spill_exits),
                         .invalidations = DELTA(s0, s1, invalidations),
                         .compiles = DELTA(s0, s1, compiles)};
    for (unsigned c = 0; c < T1_END_CLASSES; ++c)
        d.exits_by_end[c] = DELTA(s0, s1, exits_by_end[c]);
    return d;
}
static void t_call_return(void)
{
    memset(&call_prog, 0, sizeof(call_prog));
    uint64_t allocs = t1_helper_calls(IA64_OP_ALLOC);
    struct t1_stats d =
        program_run("call_return", "call-return", call_prog_compiled, call_prog_entry);
    allocs = t1_helper_calls(IA64_OP_ALLOC) - allocs;
    bool two_frames = call_prog.nframes == 2 &&
                      call_prog.frames[0] + call_prog.frames[1] == 4 + 2 &&
                      (call_prog.frames[0] == 4 || call_prog.frames[0] == 2);
    if (call_prog.compiles != 1 || !two_frames || !call_prog.calling || !allocs ||
        !d.exits_by_end[T1_END_RET] || !d.exits_by_end[T1_END_CALL] || !d.exits_by_end[T1_END_NAT])
        fprintf(
            stderr,
            "upper: call-return: %u compiles at .Lsum, entered in %u frames (%#llx %#llx), %u calling "
            "regions, %llu allocs that spilled in regions, exits call/ret/nat "
            "%llu/%llu/%llu\n",
            call_prog.compiles, call_prog.nframes, (unsigned long long)call_prog.frames[0],
            (unsigned long long)call_prog.frames[1], call_prog.calling, (unsigned long long)allocs,
            (unsigned long long)d.exits_by_end[T1_END_CALL],
            (unsigned long long)d.exits_by_end[T1_END_RET],
            (unsigned long long)d.exits_by_end[T1_END_NAT]);
    T1H_CHECK(call_prog.compiles == 1 && two_frames && call_prog.calling && allocs);
    T1H_CHECK(d.exits_by_end[T1_END_RET] && d.exits_by_end[T1_END_CALL] &&
              d.exits_by_end[T1_END_NAT]);
    printf(
        "upper: call-return: %llu Tier1 instructions in %llu entries, %u regions holding calls; .Lsum compiled once "
        "and entered from frames of 4 and 2 outputs; exits at calls/returns/chk.s %llu/%llu/%llu; %llu allocs "
        "spilled inside regions, no entry declined; equal to the reference\n",
        (unsigned long long)d.tier1_retired, (unsigned long long)d.entries, call_prog.calling,
        (unsigned long long)d.exits_by_end[T1_END_CALL],
        (unsigned long long)d.exits_by_end[T1_END_RET],
        (unsigned long long)d.exits_by_end[T1_END_NAT], (unsigned long long)allocs);
}

/* ---- rse-spill: allocs that spill inside Tier1 regions --------------------
 *
 * programs/rse_spill.S (SDM Vol.2 §6.5, §6.6, Vol.3 alloc) on the product path with
 * verification regions, equal to the stepping reference: the alloc whose
 * frame does not fit beside the dirty registers spills them in the region
 * (in-region alloc helpers), at a function head and after a call, and no
 * entry is declined for it. With the backing store translated and its
 * pages purged every iteration (rse_spill_virt), mandatory stores to a page
 * without a translation take their faults: the region leaves at the alloc
 * before effect and the lower tier executes it (spill_exits). */
static void rse_spill(void)
{
    static const char *const names[] = {"rse_spill", "rse_spill_virt"};
    for (unsigned k = 0; k < 2; ++k) {
        uint64_t allocs = t1_helper_calls(IA64_OP_ALLOC);
        char label[32];
        snprintf(label, sizeof(label), "rse-spill %s", names[k]);
        struct t1_stats d = program_run(names[k], label, NULL, NULL);
        allocs = t1_helper_calls(IA64_OP_ALLOC) - allocs;
        bool virt = k == 1;
        if (!allocs || allocs <= d.spill_exits || (virt && !d.spill_exits) ||
            (!virt && d.spill_exits))
            fprintf(stderr, "upper: %s: %llu alloc helper calls, %llu left before effect\n", label,
                    (unsigned long long)allocs, (unsigned long long)d.spill_exits);
        T1H_CHECK(allocs && allocs > d.spill_exits && (virt ? d.spill_exits != 0 : !d.spill_exits));
        printf(
            "upper: %s: %llu Tier1 instructions in %llu entries; %llu allocs spilled inside regions, %llu left "
            "before effect for the lower tier; equal to the reference\n",
            label, (unsigned long long)d.tier1_retired, (unsigned long long)d.entries,
            (unsigned long long)(allocs - d.spill_exits), (unsigned long long)d.spill_exits);
    }
}

/* ---- callee-pages: IP-relative calls to other code pages ----------------------
 *
 * programs/callee_pages.S on the product path with verification regions, equal to the
 * stepping reference: a region at the loop on code page 1 holds its calls
 * into .Lfar1 on page 0 and .Lfar2 on page 5 (the callee pages join the
 * region's pages: fetch translation, validation, invalidation), the
 * rewrite of .Lfar1 retires the regions that hold it, and every page of
 * every compiled region is the fetch translation of its VA when compiled
 * (SDM Vol.2 §4.1: the I-TLB entry that maps it; physical mode: the VA
 * without bit 63). After the remap page 0 has an ITR of its own that maps
 * it to the copy at image offset 0x6000, which no other listed page's
 * translation covers: in callee_pages_early, remapped before the loop is hot,
 * the loop's regions hold page 0 at the copy (the executor translated it
 * into the request). */
#define CALLEE_PAGES_PAGE(n) (T1H_CODE + 0x1000u * (n))
static struct {
    unsigned loop_regions, far_regions, copy_regions, stale;
} callee_pages;
static void callee_pages_compiled(const struct vcpu *v, enum t1_compile_status st,
                                  const struct t1_compile_report *rep, void *arg)
{
    (void)arg;
    if (st != T1_COMPILED)
        return;
    const struct t1_region *g = t1_region_get(rep->region_id);
    if (!g)
        return;
    bool page0 = false, page5 = false, copy = false;
    for (unsigned k = 0; k < g->npages; ++k) {
        uint64_t va = g->page[k].ip_base;
        gpa_t pa = va & ~(UINT64_C(1) << 63);
        if (g->c.psr & IA64_PSR_IT) {
            const struct ia64_tlb_entry *e = ia64_tlb_find(&v->s, va, 0);
            pa = e ? (e->pte & UINT64_C(0x0003fffffffff000) & ~ia64_low_mask(e->page_shift)) |
                         (va & ia64_low_mask(e->page_shift))
                   : ~UINT64_C(0);
        }
        if (pa != g->page[k].pa)
            ++callee_pages.stale;
        page0 = page0 || (va & ia64_low_mask(61)) == CALLEE_PAGES_PAGE(0);
        page5 = page5 || (va & ia64_low_mask(61)) == CALLEE_PAGES_PAGE(5);
        copy = copy || ((va & ia64_low_mask(61)) == CALLEE_PAGES_PAGE(0) &&
                        g->page[k].pa == CALLEE_PAGES_PAGE(6));
    }
    if ((g->c.ip & ia64_low_mask(61) & ~UINT64_C(4095)) != CALLEE_PAGES_PAGE(1))
        return;
    ++callee_pages.loop_regions;
    if (page0 && page5)
        ++callee_pages.far_regions;
    if (copy)
        ++callee_pages.copy_regions;
}
static void t_callee_pages(void)
{
    static const char *const names[] = {"callee_pages", "callee_pages_virt", "callee_pages_early"};
    for (unsigned k = 0; k < 3; ++k) {
        memset(&callee_pages, 0, sizeof(callee_pages));
        char label[32];
        snprintf(label, sizeof(label), "callee-pages %s", names[k]);
        struct t1_stats d = program_run(names[k], label, callee_pages_compiled, NULL);
        bool early = k == 2;
        /* callee_pages_early rewrites a page no region holds after the remap. */
        if (!callee_pages.far_regions || callee_pages.stale || (!early && !d.invalidations) ||
            early != (callee_pages.copy_regions != 0))
            fprintf(
                stderr,
                "upper: %s: %u regions at the loop, %u holding pages 0 and 5, %u page 0 at the copy, "
                "%u pages not the fetch translation, %llu invalidations\n",
                label, callee_pages.loop_regions, callee_pages.far_regions,
                callee_pages.copy_regions, callee_pages.stale, (unsigned long long)d.invalidations);
        T1H_CHECK(callee_pages.far_regions && !callee_pages.stale && (early || d.invalidations) &&
                  early == (callee_pages.copy_regions != 0));
        printf(
            "upper: %s: %llu Tier1 instructions in %llu entries; %u of %u regions at the loop hold the callee "
            "pages 0 and 5, %u page 0 at the copy; %llu invalidations; equal to the reference\n",
            label, (unsigned long long)d.tier1_retired, (unsigned long long)d.entries,
            callee_pages.far_regions, callee_pages.loop_regions, callee_pages.copy_regions,
            (unsigned long long)d.invalidations);
    }
}

void upper_tests(void);
void upper_tests(void)
{
    snapshot_init(&M.snap);
    result_init(&M.reference);
    result_init(&M.actual);
    setup_errors();
    forward_progress_mmio();
    forward_progress_nat();
    forward_progress_chk();
    forward_progress_retpl();
    forward_progress_budget();
    t_tag_keep();
    kick_long_loop();
    kick_timer();
    kick_masked();
    kick_vs_request();
    kick_interpreter();
    kick_halt();
    links_rotation();
    links_generation();
    psr_db_tier0();
    psr_be_lower();
    psr_fields();
    head_sequences();
    helper_strike();
    reject_hold();
    link_without_lock();
    profit_threshold();
    window_generations();
    head_precheck();
    transient_reject_retried();
    verify_policy();
    stay_page_set();
    fetch_cache();
    chain_counts();
    t_call_return();
    rse_spill();
    t_callee_pages();
    arena_pressure();
    printf(
        "upper: %u comparisons with the reference; %u kick source attempts landed outside a Tier1 entry "
        "and were run again\n",
        comparisons, late_attempts);
    result_free(&M.reference);
    result_free(&M.actual);
    snapshot_free(&M.snap);
}

/* The product path under pool pressure, with the executor
 * registered as in the product. A compile refused memory is deferred, not
 * rejected, and its head is due again after the hotness threshold.
 * Maintenance at Tier0's loop top evicts cold regions and reclaims them
 * after the executor's next boundary, so compiling continues inside a small
 * pool and resumes when the pressure ends. Every run equals the
 * interpreter. */
/* A memory refusal is never a rejection: no rejected compile names one. */
static unsigned memory_rejects;
static void count_memory_rejects(const struct vcpu *v, enum t1_compile_status st,
                                 const struct t1_compile_report *rep, void *arg)
{
    (void)v;
    (void)arg;
    static const char *const memory[] = {"memory", "region record", "region tables", "frame tables",
                                         "code arena"};
    if (st != T1_REJECTED || !rep->reject_reason)
        return;
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(memory); ++k)
        if (strstr(rep->reject_reason, memory[k]))
            ++memory_rejects;
}

void upper_memory_tests(void);
void upper_memory_tests(void)
{
    snapshot_init(&M.snap);
    result_init(&M.reference);
    result_init(&M.actual);
    /* Establish the harness engine before replacing its configuration. */
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *v = vcpu_create(&cfg);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), v);
    t1h_engine_release(&ev);
    vcpu_destroy(v);
    tier1_reconfigure(UINT64_C(8) << 20, 4096);
    /* Tier0's arena exists before an executor registers. */
    if (!tier0_arena_initialized())
        T1H_CHECK(tier0_configure(TIER0_CODE_DEFAULT_BYTES));
    csuite_registered = true;
    memory_rejects = 0;
    t1_test_set_compile_observer(count_memory_rejects, NULL);
    static const uint32_t flags[] = {0, 0};
    struct t1_memory_stats mem;
    t1_memory_get_stats(&mem);
    uint64_t cap = mem.budget_bytes;
    /* No metadata room: every hot head is deferred, Tier0 runs everything. */
    t1_memory_set_limit(mem.used_bytes + mem.reserve_bytes);
    struct t1_stats s0 = stats_now();
    csuite_rounds(1, 3, flags, "no Tier1 memory");
    struct t1_stats s1 = stats_now();
    printf("memory: no room: %llu hot heads deferred, %llu hot calls\n",
           (unsigned long long)DELTA(s0, s1, hot_deferred),
           (unsigned long long)DELTA(s0, s1, hot_calls));
    T1H_CHECK(s1.compiles == s0.compiles && s1.entries == s0.entries);
    T1H_CHECK(s1.rejects == s0.rejects && s1.hot_rejected == s0.hot_rejected);
    T1H_CHECK(DELTA(s0, s1, hot_deferred) >= MEMORY_DEFERRED_MIN);
    /* hot() defers for an outstanding byte request before compiling, or
     * because the compile it ran was deferred (maintenance at the loop top
     * gives up a request eviction cannot cover, so mostly the latter). */
    T1H_CHECK(DELTA(s0, s1, hot_deferrals[T1_DEFER_ID]) == 0 &&
              DELTA(s0, s1, hot_deferrals[T1_DEFER_CHUNK]) == 0);
    T1H_CHECK(DELTA(s0, s1, hot_deferred) ==
              DELTA(s0, s1, hot_deferrals[T1_DEFER_BYTES]) + DELTA(s0, s1, memory_deferrals));
    /* Room for a few regions' tables: compiles continue by eviction. */
    t1_memory_get_stats(&mem);
    t1_memory_set_limit(mem.used_bytes + mem.reserve_bytes + MEMORY_ROOM);
    csuite_rounds(2, 3, flags, "small Tier1 pool");
    struct t1_stats s2 = stats_now();
    printf(
        "memory: small pool: %llu compiles, %llu evictions, %llu deferrals, %llu rejects, %llu Tier1 "
        "instructions\n",
        (unsigned long long)DELTA(s1, s2, compiles), (unsigned long long)DELTA(s1, s2, evictions),
        (unsigned long long)DELTA(s1, s2, memory_deferrals),
        (unsigned long long)DELTA(s1, s2, rejects),
        (unsigned long long)DELTA(s1, s2, tier1_retired));
    T1H_CHECK(DELTA(s1, s2, compiles) >= MEMORY_COMPILES_MIN && DELTA(s1, s2, evictions) > 0);
    T1H_CHECK(DELTA(s1, s2, memory_deferrals) > 0 && !memory_rejects);
    t1_memory_get_stats(&mem);
    T1H_CHECK(mem.peak_bytes <= mem.budget_bytes);
    /* The pressure ends: the next round compiles without deferral. */
    t1_memory_set_limit(cap);
    csuite_rounds(1, 3, flags, "pressure lifted");
    struct t1_stats s3 = stats_now();
    T1H_CHECK(DELTA(s2, s3, compiles) > 0 && DELTA(s2, s3, tier1_retired) > 0);
    T1H_CHECK(s3.memory_deferrals == s2.memory_deferrals && s3.hot_deferred == s2.hot_deferred &&
              !memory_rejects);
    /* A table of 32 records, fewer than the kernels' regions: ids run
     * out, maintenance evicts cold regions in batches and compiling
     * continues. */
    tier1_restore_config();
    tier1_reconfigure(UINT64_C(8) << 20, 32);
    struct t1_stats s4 = stats_now();
    csuite_rounds(2, 3, flags, "32 region records");
    struct t1_stats s5 = stats_now();
    uint64_t id_deferrals = DELTA(s4, s5, deferrals_refused[T1_DEFER_ID]) +
                            DELTA(s4, s5, deferrals_pressure[T1_DEFER_ID]) +
                            DELTA(s4, s5, hot_deferrals[T1_DEFER_ID]);
    printf(
        "memory: 32 region records: %llu compiles, %llu evictions, %llu deferrals for a region id\n",
        (unsigned long long)DELTA(s4, s5, compiles), (unsigned long long)DELTA(s4, s5, evictions),
        (unsigned long long)id_deferrals);
    T1H_CHECK(DELTA(s4, s5, compiles) > 32 && DELTA(s4, s5, evictions) > 0 && id_deferrals > 0 &&
              !memory_rejects);
    t1_test_set_compile_observer(NULL, NULL);
    csuite_registered = false;
    tier1_restore_config();
    result_free(&M.reference);
    result_free(&M.actual);
    snapshot_free(&M.snap);
    puts(
        "memory: registered executor under pool pressure: deferred heads retried at the threshold, compiles "
        "continue by eviction and resume when the pressure ends; six C kernels equal to the interpreter");
}

void fetch_cache_tests(void);
void fetch_cache_tests(void)
{
    snapshot_init(&M.snap);
    result_init(&M.reference);
    result_init(&M.actual);
    fetch_cache();
    result_free(&M.reference);
    result_free(&M.actual);
    snapshot_free(&M.snap);
}
