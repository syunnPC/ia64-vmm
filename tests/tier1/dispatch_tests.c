/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Compare interpreter, Tier0 and Tier1 dispatch with the stepping reference
 * at boundaries selected by budget sweeps, split runs and interrupt injection.
 * Asynchronous kick delivery uses the boundary read from ar.itc by the guest
 * handler and must stay within the engine's kick_bound.
 *
 * Lower-tier steps cover both fault delivery with a retired handler instruction
 * and single-step completion traps. rse-return tests require mandatory RSE
 * loads during lower-tier fetch before any Tier1 entry (SDM Vol.2 §6.8):
 * entry hooks must never see CFLE or rse_missing set.
 * Budget and split sweeps run with and without the store log to exercise
 * Tier0's inline physical accesses too.
 */
#define _POSIX_C_SOURCE 200809L /* sigaction under -std=c11 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/engines.h"
#include "tests/tier1/programs.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/results.h"
#include "tests/tier1/snapshot.h"
#include "tests/tier1/storelog.h"
#include "ia64/interrupt.h"
#include "engine/tier1/tier1.h"
#include "tests/tier1/provider.h"
#include "tests/tier1/watchdog.h"

#define BIG_BUDGET UINT64_C(50000000)
#define INT_VECTOR 0x40u
#define TIMER_VECTOR 0xefu
#define WINDOW 120u /* prologue + > 3 iterations of the sweep programs */

struct machine {
    const struct guest_program *p;
    struct guest_params q;
    struct vcpu *ref, *v;
    struct t1h_logs ref_logs, logs;
    struct snapshot snap;
    struct run_result reference, actual;
    uint8_t *ref_ram;
    uint64_t total; /* retired at the end of the whole reference run */
    unsigned compared;
};

static struct machine m;

static void machine_open(const struct guest_program *p, struct guest_params q)
{
    struct vcpu_config cfg = guest_vcpu_config(p);
    m.p = p;
    m.q = q;
    m.ref = vcpu_create(&cfg);
    m.v = vcpu_create(&cfg);
    T1H_CHECK(m.ref && m.v);
    t1h_bind(m.ref, &m.ref_logs);
    t1h_bind(m.v, &m.logs);
    guest_setup(p, &q, m.ref);
    snapshot_take(&m.snap, m.ref);
}

static void machine_close(void)
{
    t1h_unbind(m.ref);
    t1h_unbind(m.v);
    vcpu_destroy(m.ref);
    vcpu_destroy(m.v);
    m.ref = m.v = NULL;
}

/* The whole reference run under `sched` (NULL: none) into m.reference. */
static struct vcpu_exit reference_whole(struct schedule *sched, bool stores)
{
    if (sched)
        schedule_rearm(sched);
    m.ref_logs.stores_on = stores;
    snapshot_restore(&m.snap, m.ref);
    struct vcpu_exit e = ref_run(m.ref, BIG_BUDGET, sched);
    T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && m.ref_logs.last_break_imm == T1H_BREAK_END);
    T1H_CHECK(schedule_complete(sched));
    result_take(&m.reference, &e, m.ref);
    return e;
}

static void restore_test(struct t1h_engine_vcpu *ev, bool stores)
{
    m.logs.stores_on = stores;
    snapshot_restore(&m.snap, m.v);
    t1h_engine_restored(ev);
}

static void compare_or_fail(const struct run_result *actual, bool stores, const char *test,
                            const struct t1h_engine *e, uint64_t k)
{
    char label[128];
    snprintf(label, sizeof(label), "%s %s/%s%s k=%llu", test, m.p->name, e->name,
             stores ? "+stores" : "", (unsigned long long)k);
    if (result_compare(&m.reference, actual, stores, label)) {
        fprintf(stderr, "dispatch: %s differs from the reference\n", label);
        T1H_CHECK(!"dispatch test differs from the reference");
    }
    ++m.compared;
}

/* ---- budget-sweep / lower-tier-step ------------------------------------------------ */

struct lower_events {
    uint64_t fault_and_retire, retire_and_trap;
};

static bool in_ivt(uint64_t ip)
{
    uint64_t pa = ip & ~(UINT64_C(1) << 63);
    return pa >= T1H_IVT && pa < T1H_IVT + 0x8000;
}

static void d_budget(const struct t1h_engine *engine, bool stores, struct lower_events *events)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, engine, m.v);
    m.ref_logs.stores_on = stores;
    snapshot_restore(&m.snap, m.ref);
    ram_save(m.ref_ram);
    struct vcpu_exit ref_exit = ref_run(m.ref, 0, NULL);
    for (uint64_t k = 0; k <= m.total + 2; ++k) {
        /* The reference at k: m.ref, m.ref_ram, m.ref_logs, ref_exit. */
        restore_test(&ev, stores);
        struct vcpu_exit e = t1h_engine_run(&ev, k);
        uint64_t expected = k < m.total ? k : m.total;
        if (m.v->retired != expected) {
            fprintf(stderr, "budget-sweep %s/%s: budget %llu retired %llu (expected %llu)\n",
                    m.p->name, engine->name, (unsigned long long)k,
                    (unsigned long long)m.v->retired, (unsigned long long)expected);
            T1H_CHECK(!"exact budget contract");
        }
        result_take(&m.actual, &e, m.v);
        uint8_t *own = m.reference.ram;
        m.reference.exit = ref_exit;
        m.reference.v = *m.ref;
        m.reference.logs = m.ref_logs;
        m.reference.ram = m.ref_ram;
        compare_or_fail(&m.actual, stores, "budget-sweep", engine, k);
        m.reference.ram = own;
        if (ref_exit.reason == VCPU_EXIT_BUDGET) {
            /* Advance the reference by one boundary on its own RAM. */
            ram_load(m.ref_ram);
            uint64_t ip = m.ref->s.ip;
            ref_exit = ref_run(m.ref, 1, NULL);
            if (events && !in_ivt(ip) && in_ivt(m.ref->s.ip)) {
                /* Into the IVT within one step: either a delivery followed
                 * by the handler's first instruction, or a retirement
                 * followed by a completion trap. */
                if (m.ref->s.ip == ((T1H_IVT + 0x6000) | (ip & (UINT64_C(1) << 63))))
                    ++events->retire_and_trap;
                else
                    ++events->fault_and_retire;
            }
            ram_save(m.ref_ram);
        }
    }
    t1h_engine_release(&ev);
}

/* ---- split-run ----------------------------------------------------------- */

static void d_split(const struct t1h_engine *engine, bool stores)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, engine, m.v);
    reference_whole(NULL, stores);
    for (uint64_t k = 0; k <= m.total; ++k) {
        restore_test(&ev, stores);
        struct vcpu_exit e = t1h_engine_run(&ev, k);
        if (e.reason == VCPU_EXIT_BUDGET)
            e = t1h_engine_run(&ev, BIG_BUDGET);
        result_take(&m.actual, &e, m.v);
        compare_or_fail(&m.actual, stores, "split-run", engine, k);
    }
    t1h_engine_release(&ev);
}

/* ---- split-interrupt, timer-stop ---------------------------------------------------- */

static void d_int(const struct t1h_engine *engine)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, engine, m.v);
    static struct schedule sched;
    for (uint64_t k = 0; k <= WINDOW && k < m.total; ++k) {
        schedule_clear(&sched);
        schedule_add(&sched, (struct injection){.at = k, .kind = INJ_RAISE, .address = INT_VECTOR});
        reference_whole(&sched, true);
        /* Delivered at k (PSR.i = 1 throughout gather-phys): the handler's
         * first instruction read ar.itc = retired = k (icount mode). */
        T1H_CHECK(t1h_bank0(m.ref, 24) == 1 && t1h_bank0(m.ref, 26) == INT_VECTOR);
        T1H_CHECK(t1h_bank0(m.ref, 25) == k);
        schedule_rearm(&sched);
        restore_test(&ev, true);
        struct vcpu_exit e = t1h_engine_run_sched(&ev, BIG_BUDGET, &sched);
        result_take(&m.actual, &e, m.v);
        compare_or_fail(&m.actual, true, "split-interrupt", engine, k);
    }
    t1h_engine_release(&ev);
}

static void d_timer(const struct t1h_engine *engine)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, engine, m.v);
    struct snapshot base = m.snap; /* shares the RAM buffer: vCPU only */
    for (uint64_t k = 0; k <= WINDOW && k < m.total; ++k) {
        /* Arm ITM = k through the interpreter's own path (ia64/interrupt.c),
         * so timer_last/armed/fired are those of a guest `mov cr.itm`. */
        m.snap.v = base.v;
        m.snap.v.s.cr[CR_ITV] = TIMER_VECTOR;
        m.snap.v.s.cr[CR_ITM] = ia64_itc_read(&m.snap.v) + k;
        ia64_timer_program(&m.snap.v);
        reference_whole(NULL, true);
        T1H_CHECK(t1h_bank0(m.ref, 24) == 1 && t1h_bank0(m.ref, 26) == TIMER_VECTOR);
        T1H_CHECK(t1h_bank0(m.ref, 25) == k);
        restore_test(&ev, true);
        struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
        result_take(&m.actual, &e, m.v);
        compare_or_fail(&m.actual, true, "timer-stop", engine, k);
    }
    m.snap.v = base.v;
    t1h_engine_release(&ev);
}

/* ---- kick-delivery ------------------------------------------------------------ */

struct kicker {
    struct t1h_engine_vcpu *ev;
    uint64_t target, before, after;
    int stop, ready;
    bool raised;
};

static void *kick_thread(void *arg)
{
    struct kicker *k = arg;
    struct vcpu *v = k->ev->v;
    __atomic_store_n(&k->ready, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&v->retired, __ATOMIC_RELAXED) < k->target)
        if (__atomic_load_n(&k->stop, __ATOMIC_ACQUIRE))
            return NULL;
    k->before = __atomic_load_n(&v->retired, __ATOMIC_RELAXED);
    vcpu_raise_external(v, INT_VECTOR);
    if (k->ev->e->kick)
        k->ev->e->kick(k->ev->ctx);
    k->after = __atomic_load_n(&v->retired, __ATOMIC_RELAXED);
    k->raised = true;
    return NULL;
}

static void kick_delivery(const struct t1h_engine *engine, unsigned trials)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, engine, m.v);
    static struct schedule sched;
    uint64_t seed = 12345, exercised = 0, attempted = 0;
    /* Preserve the original minimum coverage, regardless of missed raises. */
    unsigned required = trials / 2 + trials % 2;
    char label[64];
    snprintf(label, sizeof(label), "kick-delivery %s", m.p->name);
    t1h_watchdog_start(label, engine->name);
    while (exercised < required) {
        ++attempted;
        t1h_watchdog_trial();
        uint64_t next_seed = seed * UINT64_C(6364136223846793005) + 1442695040888963407u;
        struct kicker k = {.ev = &ev, .target = 1 + (next_seed >> 33) % (m.total - m.total / 8)};
        restore_test(&ev, true);
        pthread_t thread;
        T1H_CHECK(!pthread_create(&thread, NULL, kick_thread, &k));
        /* The raising thread is spinning before the run starts. */
        while (!__atomic_load_n(&k.ready, __ATOMIC_ACQUIRE)) {
        }
        struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
        __atomic_store_n(&k.stop, 1, __ATOMIC_RELEASE);
        T1H_CHECK(!pthread_join(thread, NULL));
        bool delivered = t1h_bank0(m.v, 24) != 0;
        t1h_watchdog_source(k.raised, k.raised && k.after < m.total, k.target, k.before, k.after);
        if (k.raised && !delivered) {
            t1h_watchdog_undelivered();
            if (!(m.p->flags & GUEST_VIRT) && k.after < m.total &&
                engine->kick_bound < m.total - k.after) {
                fprintf(stderr,
                        "kick-delivery %s/%s: target %llu raised between %llu and %llu, "
                        "not delivered by %llu (bound %llu)\n",
                        m.p->name, engine->name, (unsigned long long)k.target,
                        (unsigned long long)k.before, (unsigned long long)k.after,
                        (unsigned long long)m.total, (unsigned long long)engine->kick_bound);
                T1H_CHECK(!"kick-delivery raised but undelivered");
            }
        }
        if (!k.raised || !delivered)
            continue; /* too late to require delivery */
        uint64_t d = t1h_bank0(m.v, 25) - m.v->clock_offset;
        /* In gather-virt a raise during a guest fault handler (PSR.i = 0) waits
         * for its rfi: the latency bound applies to gather-phys only. */
        uint64_t bound = (m.p->flags & GUEST_VIRT) ? UINT64_MAX - k.after : engine->kick_bound;
        if (d < k.before || d > k.after + bound) {
            fprintf(stderr,
                    "kick-delivery %s/%s: raised between %llu and %llu, delivered at %llu "
                    "(bound %llu)\n",
                    m.p->name, engine->name, (unsigned long long)k.before,
                    (unsigned long long)k.after, (unsigned long long)d,
                    (unsigned long long)engine->kick_bound);
            T1H_CHECK(!"kick-delivery delivery bound");
        }
        result_take(&m.actual, &e, m.v);
        schedule_clear(&sched);
        schedule_add(&sched, (struct injection){.at = d, .kind = INJ_RAISE, .address = INT_VECTOR});
        reference_whole(&sched, true);
        compare_or_fail(&m.actual, true, "kick-delivery", engine, d);
        ++exercised;
        seed = next_seed; /* retry missed targets without changing the sequence */
        t1h_watchdog_progress();
    }
    t1h_watchdog_stop();
    printf("dispatch: kick-delivery %s/%s: %llu exercised/%llu attempted\n", m.p->name,
           engine->name, (unsigned long long)exercised, (unsigned long long)attempted);
    t1h_engine_release(&ev);
}

/* ---- rse-dispatch ---------------------------------------------------- */

static unsigned rse_entries, rse_bad_entries;
static void rse_hook(struct t1_vcpu *tv, uint32_t region, void *arg)
{
    (void)tv;
    (void)region;
    const struct vcpu *v = arg;
    ++rse_entries;
    if (v->s.rse_cfle || v->s.rse_missing)
        ++rse_bad_entries;
}

static void d_rse(void)
{
    const struct guest_program *p = guest_program("rse_return");
    T1H_CHECK(p != NULL);
    machine_open(p, guest_completion_params(p));
    reference_whole(NULL, true);
    m.total = m.reference.v.retired;
    /* Header arrivals on the reference, and those right after a br.ret
     * (the caller frame's 8 registers unloaded, CFLE set). */
    static uint64_t arrival[1024];
    unsigned narrival = 0, incomplete = 0;
    uint64_t header, first_incomplete = UINT64_MAX;
    snapshot_restore(&m.snap, m.ref);
    T1H_CHECK(t1h_run_to_loop_header(m.ref, 100000, &header));
    snapshot_restore(&m.snap, m.ref);
    for (uint64_t step = 0; step < m.total; ++step) {
        if (m.ref->s.ip == header && m.ref->s.slot == 0) {
            if (narrival < 1024)
                arrival[narrival++] = m.ref->retired;
            if (m.ref->s.rse_missing == 8 && m.ref->s.rse_cfle && !incomplete++)
                first_incomplete = m.ref->retired;
        }
        if (ref_run(m.ref, 1, NULL).reason != VCPU_EXIT_BUDGET)
            break;
    }
    T1H_CHECK(incomplete == m.q.passes && narrival == m.q.passes * m.q.n);
    unsigned before = m.compared;
    static struct schedule sched;
    for (unsigned i = 0; i < t1h_engine_count(); ++i) {
        const struct t1h_engine *e = t1h_engine_at(i);
        struct t1h_engine_vcpu ev;
        t1h_engine_bind(&ev, e, m.v);
        rse_entries = rse_bad_entries = 0;
        t1_test_set_entry_hook(rse_hook, m.v);
        reference_whole(NULL, true);
        restore_test(&ev, true);
        struct vcpu_exit x = t1h_engine_run(&ev, BIG_BUDGET);
        result_take(&m.actual, &x, m.v);
        compare_or_fail(&m.actual, true, "rse-dispatch", e, 0);
        for (unsigned a = 0; a < narrival; ++a) {
            uint64_t k = arrival[a];
            schedule_clear(&sched);
            schedule_add(&sched,
                         (struct injection){.at = k, .kind = INJ_RAISE, .address = INT_VECTOR});
            reference_whole(&sched, true);
            T1H_CHECK(t1h_bank0(m.ref, 24) == 1 && t1h_bank0(m.ref, 25) == k);
            schedule_rearm(&sched);
            restore_test(&ev, true);
            x = t1h_engine_run_sched(&ev, BIG_BUDGET, &sched);
            result_take(&m.actual, &x, m.v);
            compare_or_fail(&m.actual, true, "rse-dispatch interrupt", e, k);
        }
        t1_test_set_entry_hook(NULL, NULL);
        /* tier0+tier1: Tier0's fetch translation performs
         * the mandatory loads before the lookup; enter checks rse_missing. */
        if (!strcmp(e->name, "tier1") || !strcmp(e->name, "tier0+tier1")) {
            /* An explicit compile at the header with the frame unloaded is
             * rejected and performs none of the mandatory loads. */
            restore_test(&ev, false);
            T1H_CHECK(ref_run(m.v, first_incomplete, NULL).reason == VCPU_EXIT_BUDGET);
            T1H_CHECK(m.v->s.ip == header && m.v->s.rse_missing == 8 && m.v->s.rse_cfle);
            uint64_t bspstore = m.v->s.ar[AR_BSPSTORE];
            struct t1_compile_report rep;
            struct t1_compile_options opt = {.flags = T1_COMPILE_VERIFY};
            T1H_CHECK(t1h_provider_real()->compile(ev.ctx, &opt, &rep) == T1_REJECTED);
            T1H_CHECK(rep.reject_reason && !strcmp(rep.reject_reason, "incomplete RSE frame"));
            T1H_CHECK(m.v->s.rse_missing == 8 && m.v->s.rse_cfle &&
                      m.v->s.ar[AR_BSPSTORE] == bspstore);
            if (rse_bad_entries)
                fprintf(
                    stderr,
                    "rse-dispatch: %u of %u Tier1 region entries with an incomplete RSE frame\n",
                    rse_bad_entries, rse_entries);
            T1H_CHECK(rse_entries > 0 && rse_bad_entries == 0);
            printf(
                "dispatch: rse-dispatch %s/%s: %u region entries, none with CFLE set or rse_missing != 0; "
                "a compile at the header with 8 registers unloaded is rejected without loading\n",
                p->name, e->name, rse_entries);
        }
        t1h_engine_release(&ev);
    }
    printf(
        "dispatch: rse-dispatch %s: %u returns onto the loop header with 8 registers unloaded, an interrupt "
        "returning there by rfi at each of %u header arrivals: %u comparisons\n",
        p->name, incomplete, narrival, m.compared - before);
    machine_close();
}

/* ---- exit outcomes --------------------------------------------- */

static struct run_result outcome_results[2];

/* Runs the test engine through `calls` calls of `budget` (re-entering after
 * each exit), then the reference through the same calls, and compares the
 * outer exit and state after every call. */
static void outcome_case(const struct t1h_engine *engine, const char *what, uint64_t budget,
                         unsigned calls)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, engine, m.v);
    T1H_CHECK(calls <= 2);
    restore_test(&ev, false);
    for (unsigned i = 0; i < calls; ++i) {
        struct vcpu_exit e = t1h_engine_run(&ev, budget);
        result_take(&outcome_results[i], &e, m.v);
    }
    m.ref_logs.stores_on = false;
    snapshot_restore(&m.snap, m.ref);
    for (unsigned i = 0; i < calls; ++i) {
        struct vcpu_exit r = ref_run(m.ref, budget, NULL);
        result_take(&m.reference, &r, m.ref);
        compare_or_fail(&outcome_results[i], false, what, engine, i);
        /* Negative: the same state with another exit reason differs. */
        struct vcpu_exit good = outcome_results[i].exit;
        outcome_results[i].exit.reason =
            good.reason == VCPU_EXIT_BUDGET ? VCPU_EXIT_HALT : VCPU_EXIT_BUDGET;
        T1H_CHECK(
            result_compare(&m.reference, &outcome_results[i], false, T1H_EXPECTED_DIFFERENCE) == 1);
        outcome_results[i].exit = good;
    }
    t1h_engine_release(&ev);
}

static void outcomes(const struct t1h_engine *engine)
{
    /* Budget 0 and a requested exit, on gather-phys. */
    machine_open(guest_program("gather_phys"), guest_sweep_params(guest_program("gather_phys")));
    outcome_case(engine, "outcome budget=0", 0, 1);
    T1H_CHECK(m.reference.exit.reason == VCPU_EXIT_BUDGET && m.reference.v.retired == 0);
    vcpu_request_exit(&m.snap.v, VCPU_EXIT_SHUTDOWN);
    outcome_case(engine, "outcome requested", 100, 1);
    T1H_CHECK(m.reference.exit.reason == VCPU_EXIT_SHUTDOWN);
    machine_close();

    /* PAL_HALT_LIGHT (VCPU_EXIT_HALT), then an access outside every region
     * (VCPU_EXIT_BAD_ACCESS), with device effects before them. */
    static const struct guest_program outcome = {"outcome", "outcome.bin", GUEST_SHLADD_XOR, 0};
    struct vcpu_config cfg = {
        .engine = ENGINE_INTERP, .itc_icount = true, .pal_entry = T1H_CODE + 0x200};
    m.p = &outcome;
    m.ref = vcpu_create(&cfg);
    m.v = vcpu_create(&cfg);
    T1H_CHECK(m.ref && m.v);
    t1h_bind(m.ref, &m.ref_logs);
    t1h_bind(m.v, &m.logs);
    t1h_load_image("outcome.bin");
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[2] = T1H_DEVICE + 0x18;
    regs.gr[3] = UINT64_C(0x1122334455667788);
    regs.gr[14] = T1H_UNMAPPED;
    vcpu_reset(m.ref, &regs);
    snapshot_take(&m.snap, m.ref);
    outcome_case(engine, "outcome halt+bad_access", 1000, 2);
    T1H_CHECK(outcome_results[0].exit.reason == VCPU_EXIT_HALT);
    T1H_CHECK(m.reference.exit.reason == VCPU_EXIT_BAD_ACCESS &&
              m.reference.exit.info == T1H_UNMAPPED && m.reference.logs.device_n == 2);
    machine_close();
}

/* ---- driver ------------------------------------------------------------ */

static uint64_t
    product_entries; /* tier0+tier1: Tier1 runs from Tier0's lookups in budget-sweep/split-run */
static void sweep_program(const struct guest_program *p, struct lower_events *events)
{
    machine_open(p, guest_sweep_params(p));
    reference_whole(NULL, true);
    m.total = m.reference.v.retired;
    for (unsigned i = 0; i < t1h_engine_count(); ++i) {
        const struct t1h_engine *e = t1h_engine_at(i);
        T1H_CHECK(e->exact_budget);
        struct t1_stats s0, s1;
        t1_get_stats(&s0);
        for (unsigned stores = 0; stores < 2; ++stores) {
            d_budget(e, stores, i == 0 && stores ? events : NULL);
            d_split(e, stores);
        }
        t1_get_stats(&s1);
        /* The product path ran Tier1 code from Tier0's lookups, not
         * only Tier0. */
        if (!strcmp(e->name, "tier0+tier1")) {
            uint64_t ran =
                s1.upper_ran + s1.upper_ran_pending - s0.upper_ran - s0.upper_ran_pending;
            T1H_CHECK(ran > 0);
            product_entries += ran;
        }
    }
    machine_close();
}

void dispatch_tests(void);
void dispatch_tests(void)
{
    snapshot_init(&m.snap);
    result_init(&m.reference);
    result_init(&m.actual);
    m.ref_ram = calloc(1, T1H_RAM_SIZE);
    T1H_CHECK(m.ref_ram != NULL);
    result_init(&outcome_results[0]);
    result_init(&outcome_results[1]);

    for (unsigned i = 0; i < guest_program_count; ++i) {
        const struct guest_program *p = &guest_programs[i];
        if (p->kind == GUEST_SPEED)
            continue; /* 9M instructions: program_tests only */
        struct lower_events events = {0};
        unsigned before = m.compared;
        sweep_program(p, &events);
        if (p->flags & GUEST_VIRT)
            T1H_CHECK(events.fault_and_retire > 0);
        if (p->flags & GUEST_SINGLE_STEP)
            T1H_CHECK(events.retire_and_trap > 0);
        printf("dispatch: budget-sweep/split-run %-16s k = 0..%llu: %u comparisons%s\n", p->name,
               (unsigned long long)m.total + 2, m.compared - before,
               (p->flags & (GUEST_VIRT | GUEST_SINGLE_STEP)) ? " (lower-tier-step)" : "");
        if (p->flags & (GUEST_VIRT | GUEST_SINGLE_STEP))
            printf("dispatch: lower-tier-step %s: %llu steps delivered and retired a handler "
                   "instruction, %llu retired and delivered a completion trap\n",
                   p->name, (unsigned long long)events.fault_and_retire,
                   (unsigned long long)events.retire_and_trap);
    }

    /* stitched-outer: exits at both back-edges of a region;
     * loop-nest: at the inner and the outer back-edge of a loop nest. */
    static const char *const async_programs[] = {"gather_phys", "gather_virt", "stitched_outer",
                                                 "loop_nest"};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(async_programs); ++i) {
        const struct guest_program *p = guest_program(async_programs[i]);
        machine_open(p, guest_sweep_params(p));
        reference_whole(NULL, true);
        m.total = m.reference.v.retired;
        unsigned before = m.compared;
        for (unsigned k = 0; k < t1h_engine_count(); ++k) {
            /* gather-virt handlers run with PSR.i = 0, so delivery there is
             * deferred to the rfi: split-interrupt/timer-stop check exact delivery on
             * gather-phys, kick-delivery covers both. */
            if (!(p->flags & GUEST_VIRT)) {
                d_int(t1h_engine_at(k));
                d_timer(t1h_engine_at(k));
            }
        }
        machine_close();
        machine_open(p, guest_completion_params(p));
        reference_whole(NULL, true);
        m.total = m.reference.v.retired;
        for (unsigned k = 0; k < t1h_engine_count(); ++k)
            kick_delivery(t1h_engine_at(k), 24);
        machine_close();
        printf("dispatch: %s %s: %u comparisons\n",
               (p->flags & GUEST_VIRT) ? "kick-delivery"
                                       : "split-interrupt/timer-stop/kick-delivery",
               p->name, m.compared - before);
    }

    d_rse();

    unsigned before = m.compared;
    for (unsigned k = 0; k < t1h_engine_count(); ++k)
        outcomes(t1h_engine_at(k));
    printf("dispatch: exit outcomes (budget 0, requested, HALT, BAD_ACCESS) and their "
           "negative comparisons: %u comparisons\n",
           m.compared - before);
    printf(
        "dispatch: %u comparisons over %u engine(s), all equal to the reference; tier0+tier1 ran Tier1 "
        "regions %llu times from Tier0's lookups in budget-sweep/split-run\n",
        m.compared, t1h_engine_count(), (unsigned long long)product_entries);
    free(m.ref_ram);
    result_free(&outcome_results[0]);
    result_free(&outcome_results[1]);
    result_free(&m.reference);
    result_free(&m.actual);
    snapshot_free(&m.snap);
}
