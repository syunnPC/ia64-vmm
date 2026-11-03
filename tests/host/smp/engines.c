/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "engine/cache/cache.h"
#include "engine/cache/coherence-inline.h"
#include "engine/interp/interp.h"
#include "engine/tier0/internal.h"
#include "ia64/smp.h"
#include "ia64/tlb.h"
#include "ia64/rse.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/platform.h"

#define BASE UINT64_C(0x100000)
#define ROUNDS 10000u
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "FAIL %s:%u: %s\n", __FILE__, __LINE__, #expr);                        \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
#include "watchdog.h"
static _Alignas(16) uint64_t memory[8192 / 8];
static _Alignas(16) uint8_t spare[4096];
static _Alignas(16) uint64_t late_ram[2][4096 / 8];
static struct vcpu *cpus[2];
static bool capture_write_pending;
static uint64_t captured_replacement;
static atomic_uint decision_phase;
static enum tier0_test_stage decision_before;
static bool decision_seen[2][2];
static void decision_hook(const struct vcpu *v, enum tier0_test_stage stage)
{
    if (stage != decision_before && stage != decision_before + 1)
        return;
    unsigned cpu = v->cfg.index, ready = stage != decision_before;
    if (decision_seen[cpu][ready])
        return;
    decision_seen[cpu][ready] = true;
    /* A keeps its resolved decision/pointer across B's update. Private
     * executor slots preserve A's value; shared slots deterministically
     * expose B's armed level or stacked-register homes to A. */
    if (!ready && cpu)
        while (atomic_load_explicit(&decision_phase, memory_order_acquire) < 1)
            sched_yield();
    if (ready && !cpu) {
        atomic_store_explicit(&decision_phase, 1, memory_order_release);
        while (atomic_load_explicit(&decision_phase, memory_order_acquire) < 2)
            sched_yield();
    }
    if (ready && cpu)
        atomic_store_explicit(&decision_phase, 2, memory_order_release);
}
static void decision_begin(enum tier0_test_stage before)
{
    decision_before = before;
    memset(decision_seen, 0, sizeof(decision_seen));
    atomic_store(&decision_phase, 0);
    tier0_set_test_hook(decision_hook);
}
static unsigned locked_counter;
static atomic_uint phase, hook_active, snapshots;
static atomic_int allocations, fail_after = -1;
void *__real_hal_alloc_pages(size_t pages, unsigned flags);
void __real_hal_free_pages(void *ptr, size_t pages);
void *__wrap_hal_alloc_pages(size_t pages, unsigned flags)
{
    int left = atomic_load(&fail_after);
    if (left >= 0 && atomic_fetch_sub(&fail_after, 1) == 0)
        return NULL;
    void *ptr = __real_hal_alloc_pages(pages, flags);
    if (ptr)
        atomic_fetch_add(&allocations, 1);
    return ptr;
}
void __wrap_hal_free_pages(void *ptr, size_t pages)
{
    if (ptr)
        atomic_fetch_sub(&allocations, 1);
    __real_hal_free_pages(ptr, pages);
}
static void hook(gpa_t page, enum code_test_stage stage)
{
    (void)page;
    CHECK(code_lock_held());
    if (stage != CODE_TEST_SNAPSHOT)
        return;
    CHECK(atomic_fetch_add(&hook_active, 1) == 0);
    sched_yield();
    atomic_fetch_add(&snapshots, 1);
    CHECK(atomic_fetch_sub(&hook_active, 1) == 1);
}
static void *lock_worker(void *unused)
{
    (void)unused;
    for (unsigned n = 0; n < ROUNDS; ++n) {
        code_lock();
        CHECK(code_lock_held());
        unsigned before = locked_counter;
        sched_yield();
        locked_counter = before + 1;
        code_unlock();
        CHECK(!code_lock_held());
        if (!((n + 1) % 1024))
            watchdog_progress();
    }
    return NULL;
}
static void join_pair(void *(*a)(void *), void *(*b)(void *))
{
    pthread_t threads[2];
    CHECK(!pthread_create(&threads[0], NULL, a, NULL));
    CHECK(!pthread_create(&threads[1], NULL, b, NULL));
    CHECK(!pthread_join(threads[0], NULL));
    CHECK(!pthread_join(threads[1], NULL));
}
static void epochs(void)
{
    for (unsigned n = 0; n < ROUNDS; ++n) {
        code_quiescent(cpus[0]);
        code_quiescent(cpus[1]);
        code_lock();
        uint64_t stamp = code_retire_epoch();
        CHECK(stamp == code_epoch());
        CHECK(!code_epoch_reached(stamp));
        code_unlock();
        code_quiescent(cpus[0]);
        code_lock();
        CHECK(!code_epoch_reached(stamp));
        code_unlock();
        __atomic_store_n(&cpus[1]->parked, 1, __ATOMIC_SEQ_CST);
        code_lock();
        CHECK(code_epoch_reached(stamp));
        code_unlock();
        __atomic_store_n(&cpus[1]->parked, 0, __ATOMIC_SEQ_CST);
        code_lock();
        CHECK(!code_epoch_reached(stamp));
        code_unlock();
        code_quiescent(cpus[1]);
        code_lock();
        CHECK(code_epoch_reached(stamp));
        code_unlock();
        if (!((n + 1) % 1024))
            watchdog_progress();
    }
}
static uint64_t add_bundle(unsigned source)
{
    uint64_t add = (UINT64_C(8) << 37) | (UINT64_C(8) << 6) | ((uint64_t)source << 13);
    return 1 | (add << 5) | ((UINT64_C(1) << 27) << 46);
}
bool __real_tier0_check_code(struct vcpu *v, gpa_t gpa);
bool __wrap_tier0_check_code(struct vcpu *v, gpa_t gpa)
{
    bool result = __real_tier0_check_code(v, gpa);
    /* Place a peer's tracked CPU write exactly between page capture and
     * the real decoded fetch. The shadow must replay the selected bundle. */
    if (result && capture_write_pending && !tier0_is_shadow(v)) {
        capture_write_pending = false;
        CHECK(ia64_physical_write(cpus[1], BASE, 8, captured_replacement).kind ==
              IA64_STEP_COMPLETE);
    }
    return result;
}
static void *code_writer(void *unused)
{
    (void)unused;
    for (unsigned n = 1; n <= ROUNDS; ++n) {
        while (atomic_load_explicit(&phase, memory_order_acquire) != 2 * n - 2)
            sched_yield();
        CHECK(ia64_physical_write(cpus[0], BASE, 8, add_bundle(n & 1 ? 9 : 10)).kind ==
              IA64_STEP_COMPLETE);
        atomic_store_explicit(&phase, 2 * n - 1, memory_order_release);
    }
    return NULL;
}
static void *code_executor(void *unused)
{
    (void)unused;
    struct vcpu *v = cpus[1];
    v->s.gr[9] = 111;
    v->s.gr[10] = 222;
    Ia64Instruction serial = {.valid = true, .opcode = IA64_OP_SRLZ, .unit = IA64_UNIT_M};
    for (unsigned n = 1; n <= ROUNDS; ++n) {
        while (atomic_load_explicit(&phase, memory_order_acquire) != 2 * n - 1)
            sched_yield();
        CHECK(ia64_execute(v, &serial).kind == IA64_STEP_COMPLETE);
        v->s.ip = BASE;
        v->s.slot = 0;
        CHECK(interp_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.gr[8] == (n & 1 ? 111 : 222));
        atomic_store_explicit(&phase, 2 * n, memory_order_release);
        if (!(n % 1024))
            watchdog_progress();
    }
    return NULL;
}
static void *source_writer(void *unused)
{
    (void)unused;
    for (unsigned n = 1; n <= ROUNDS; ++n) {
        CHECK(ia64_physical_write(cpus[0], BASE, 8, n).kind == IA64_STEP_COMPLETE);
        uint64_t low, high, version;
        CHECK(code_source_read(cpus[0], BASE, &low, &high, &version).kind == IA64_STEP_COMPLETE);
        CHECK(high == UINT64_C(1) << 50);
        sched_yield();
        if (!(n % 1024))
            watchdog_progress();
    }
    return NULL;
}
static void *source_reader(void *unused)
{
    (void)unused;
    for (unsigned n = 0; n < ROUNDS; ++n) {
        uint64_t low, high, version;
        CHECK(code_source_read_physical(BASE, &low, &high, &version).kind == IA64_STEP_COMPLETE);
        CHECK(high == UINT64_C(1) << 50);
        CHECK(low <= ROUNDS);
        (void)code_source_publish(cpus[1], BASE, version);
        struct ia64_result entered = code_page_enter(cpus[1], BASE, false);
        /* The deliberately continuous writer may exhaust the bounded
         * comparison retries. Completion is required again after it stops. */
        CHECK(entered.kind == IA64_STEP_COMPLETE ||
              (entered.kind == IA64_STEP_EXIT && entered.exit == VCPU_EXIT_BAD_ACCESS &&
               !strcmp(entered.detail, "code bundles changed repeatedly at entry")));
        sched_yield();
        if (!((n + 1) % 1024))
            watchdog_progress();
    }
    return NULL;
}
static bool smp_checks;
static bool stacked_test;
bool ia64_smp_active(void)
{
    return smp_checks;
}
/* The hook runs after invalidation and before the actual effect. These
 * guest producers must retain code_lock through that interval, including
 * RSE's bound fast path and the deferred PAL service outside engine_run. */
static unsigned producer_stores;
static void producer_hook(gpa_t page, enum code_test_stage stage)
{
    (void)page;
    if (stage != CODE_TEST_STORE)
        return;
    CHECK(code_lock_held());
    ++producer_stores;
}
static void tracked_producers(void)
{
    struct vcpu *v = cpus[0];
    uint64_t low, high, version;
    for (unsigned kind = 0; kind < 3; ++kind) {
        vcpu_reset(v, NULL);
        v->s.psr = IA64_PSR_IC;
        v->s.ip = BASE;
        CHECK(code_source_read_physical(BASE + 4096, &low, &high, &version).kind ==
              IA64_STEP_COMPLETE);
        CHECK(code_page_tracked_fast(BASE + 4096));
        producer_stores = 0;
        code_validity_set_test_hook(producer_hook);
        uint64_t expected;
        if (kind == 0) {
            /* Two spills exercise reuse of the direct page binding. */
            v->s.rse_dirty = 2;
            v->s.rse_bof = 2;
            v->s.ar[AR_BSPSTORE] = BASE + 4096;
            v->s.gr[32] = expected = 123;
            v->s.gr[33] = 456;
            Ia64Instruction flush = {.valid = true, .opcode = IA64_OP_FLUSHRS};
            CHECK(ia64_rse(v, &flush).kind == IA64_STEP_COMPLETE);
            CHECK(memory[512] == expected && memory[513] == 456 && !v->s.rse_dirty);
        } else if (kind == 1) {
            v->s.tr[0][0] = (struct ia64_tlb_entry){
                .valid = true, .pte = 789, .va = 0x400000, .rid = 7, .page_shift = 12};
            expected = 789;
            ia64_pal_tr_select(v, 0);
            ia64_pal_tr_read(v, BASE + 4096);
            ia64_pal_complete_services(v);
            CHECK(!ia64_pal_tr_status(v) && ia64_pal_tr_valid(v) == 15);
        } else {
            expected = 321;
            CHECK(gm_write(BASE + 4096, &expected, 8));
        }
        CHECK(producer_stores >= 1 && !code_lock_held());
        code_validity_set_test_hook(NULL);
        CHECK(code_source_read_physical(BASE + 4096, &low, &high, &version).kind ==
              IA64_STEP_COMPLETE);
        CHECK(low == expected);
    }
    vcpu_reset(v, NULL);
    puts("smp tracked producers: bound RSE, deferred PAL, gm_write brackets passed");
}
static void write_bracket_range(void)
{
    uint64_t low, high, version;
    CHECK(code_source_read_physical(BASE + 4096, &low, &high, &version).kind == IA64_STEP_COMPLETE);
    CHECK(!code_page_tracked_fast(BASE) && code_page_tracked_fast(BASE + 4096));
    /* The public bracket accepts a range: a tracked middle page must take
     * the lock even when its endpoints are untracked. */
    bool acquired = code_cpu_write_begin(BASE, 3 * 4096);
    CHECK(acquired && code_lock_held());
    code_cpu_write_end(acquired);
    CHECK(!code_lock_held());
    CHECK(!code_cpu_write_begin(BASE + 0x100000, 3 * 4096));
}
static void *tier0_executor(void *arg)
{
    struct vcpu *v = cpus[(uintptr_t)arg];
    unsigned bof = v->cfg.index ? 47 : 3;
    unsigned source = stacked_test ? 33 + bof : 9, destination = stacked_test ? 32 + bof : 8;
    uint64_t expected = 123 + 17 * v->cfg.index;
    if (stacked_test) {
        v->s.cfm = 2;
        v->s.rse_bof = bof;
    }
    for (unsigned n = 0; n < ROUNDS; ++n) {
        v->s.gr[source] = expected + n;
        v->s.gr[destination] = 0;
        v->s.ip = BASE;
        v->s.slot = 0;
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.gr[destination] == expected + n);
        if (!(n % 17))
            code_notify_cpu_write(BASE, 16);
        if (!((n + 1) % 1024))
            watchdog_progress();
    }
    code_quiescent(v);
    __atomic_store_n(&v->parked, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void threaded_tier0(bool stacked)
{
    stacked_test = stacked;
    code_notify_cpu_write(BASE, 16);
    memory[0] =
        stacked ? (add_bundle(33) & ~(UINT64_C(127) << 11)) | (UINT64_C(32) << 11) : add_bundle(9);
    memory[1] = (UINT64_C(1) << 9) | (UINT64_C(1) << 50);
    if (stacked) {
        /* Both scheduling points must hold the same sealed region/serial,
         * so a shared cache selects the same slot with different BOFs. */
        for (unsigned cpu = 0; cpu < 2; ++cpu) {
            struct vcpu *v = cpus[cpu];
            unsigned bof = cpu ? 47 : 3;
            v->cfg.engine = ENGINE_TIER0;
            v->s.cfm = 2;
            v->s.rse_bof = bof;
            v->s.gr[33 + bof] = 123 + 17 * cpu;
            for (unsigned turn = 0; turn < 2; ++turn) {
                v->s.ip = BASE;
                v->s.slot = 0;
                CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
            }
        }
        decision_begin(TIER0_TEST_HOMES_BEFORE);
    }
    struct tier0_stats before;
    tier0_get_stats(&before);
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i) {
        cpus[i]->cfg.engine = ENGINE_TIER0;
        CHECK(!pthread_create(&threads[i], NULL, tier0_executor, (void *)(uintptr_t)i));
    }
    for (unsigned i = 0; i < 2; ++i)
        CHECK(!pthread_join(threads[i], NULL));
    if (stacked) {
        tier0_set_test_hook(NULL);
        CHECK(atomic_load(&decision_phase) == 2);
    }
    struct tier0_stats totals;
    tier0_get_stats(&totals);
    uint64_t interpreted = 0;
    for (unsigned i = 0; i < TIER0_INTERP_REASON_COUNT; ++i)
        interpreted += totals.interpreter_instructions[i] - before.interpreter_instructions[i];
    CHECK(totals.executed_instructions - before.executed_instructions + interpreted ==
          2 * ROUNDS * 3);
    CHECK(totals.executed_instructions > before.executed_instructions);
    for (unsigned i = 0; i < 2; ++i)
        __atomic_store_n(&cpus[i]->parked, 0, __ATOMIC_SEQ_CST);
    CHECK(totals.translations > 0 && totals.hits > 0);
}
/* Executor 0's boundary entry matched its region and
 * executor 1 then rewrites the code, which retires the region, and
 * translates the new code at its own loop top. The slot is not reclaimed
 * before executor 0 leaves, and enter() refuses the retired region:
 * executor 0 runs the new code through the full path. */
static atomic_uint race_phase;
static void race_hook(const struct vcpu *v, enum tier0_test_stage stage)
{
    if (stage != TIER0_TEST_BOUNDARY_HIT || v->cfg.index || atomic_load(&race_phase))
        return;
    atomic_store(&race_phase, 1);
    while (atomic_load(&race_phase) < 2)
        sched_yield();
}
static unsigned race_slot;
static uint64_t race_serial;
static void *race_hitter(void *unused)
{
    (void)unused;
    struct vcpu *v = cpus[0];
    v->s.ip = BASE;
    v->s.slot = 0;
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
    return NULL;
}
static void *race_retirer(void *unused)
{
    (void)unused;
    struct vcpu *v = cpus[1];
    while (atomic_load(&race_phase) < 1)
        sched_yield();
    bool acquired = code_cpu_write_begin(BASE, 8);
    __atomic_store_n(&memory[0], add_bundle(10), __ATOMIC_RELAXED);
    code_cpu_write_end(acquired);
    const struct tier0_region *r = &tier0_arena.regions[race_slot];
    CHECK(!r->valid && r->serial == race_serial);
    v->s.ip = BASE;
    v->s.slot = 0;
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->s.gr[8] == v->s.gr[10]);
    CHECK(r->serial == race_serial);
    atomic_store(&race_phase, 2);
    return NULL;
}
static void boundary_race(void)
{
    code_cache_flush();
    code_quiescent(cpus[0]);
    code_quiescent(cpus[1]);
    tier0_reclaim();
    memory[0] = add_bundle(9);
    memory[1] = (UINT64_C(1) << 9) | (UINT64_C(1) << 50);
    struct vcpu *v = cpus[0];
    v->s.gr[9] = 5;
    v->s.gr[10] = 7;
    cpus[1]->s.gr[10] = 11;
    /* A translation may wait for both executors' quiescent points (code
     * cache evictions), so both run before v's entry is checked. */
    for (unsigned turn = 0; turn < 3; ++turn)
        for (unsigned cpu = 0; cpu < 2; ++cpu) {
            cpus[cpu]->s.ip = BASE;
            cpus[cpu]->s.slot = 0;
            CHECK(engine_run(cpus[cpu], 3).reason == VCPU_EXIT_BUDGET);
        }
    struct tier0_stats before, after;
    tier0_get_stats(&before);
    v->s.ip = BASE;
    v->s.slot = 0;
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->s.gr[8] == 5);
    tier0_get_stats(&after);
    CHECK(after.boundary_hits == before.boundary_hits + 1);
    race_slot = tier0_arena.capacity;
    for (unsigned i = 0; i < tier0_arena.capacity; ++i)
        if (tier0_arena.regions[i].valid && tier0_arena.regions[i].key.ip == BASE)
            race_slot = i;
    CHECK(race_slot < tier0_arena.capacity);
    race_serial = tier0_arena.regions[race_slot].serial;
    tier0_vcpu_state_restored(cpus[1]); /* executor 1 misses: one stale entry, executor 0's */
    tier0_get_stats(&before);
    atomic_store(&race_phase, 0);
    tier0_set_test_hook(race_hook);
    join_pair(race_hitter, race_retirer);
    tier0_set_test_hook(NULL);
    tier0_get_stats(&after);
    CHECK(atomic_load(&race_phase) == 2 && v->s.gr[8] == 7);
    CHECK(after.boundary_stale == before.boundary_stale + 1);
    code_quiescent(cpus[0]);
    code_quiescent(cpus[1]);
    tier0_reclaim();
    CHECK(tier0_arena.regions[race_slot].serial != race_serial);
}
/* Chaining publishes quiescence while a peer replaces the Tier0 record.
 * The next lookup must probe for the new serial even at the same head and
 * sequence. Executor 1 allocates in a fresh arena after both publications,
 * so its replacement reuses the retired metadata slot. */
#define HOLD_LINK UINT64_C(0x51)
static const uint64_t hold_sequence = 1;
static atomic_uint hold_phase;
static unsigned hold_slot, hold_probes[2];
static uint64_t hold_serial;
static void hold_wait(unsigned phase)
{
    while (atomic_load(&hold_phase) < phase)
        sched_yield();
}
static uint64_t hold_probe(struct vcpu *v, gpa_t pa)
{
    (void)pa;
    ++hold_probes[v->cfg.index];
    return HOLD_LINK;
}
static enum tier0_upper_hot hold_hot(struct vcpu *v, gpa_t pa, bool loop_head)
{
    (void)v;
    (void)pa;
    (void)loop_head;
    return TIER0_HOT_REJECTED;
}
static void hold_reclaim(void) {}
static enum tier0_upper_status hold_enter(struct vcpu *v, uint64_t *link, uint64_t budget,
                                          struct vcpu_exit *out)
{
    (void)budget;
    if (v->cfg.index)
        return TIER0_UPPER_DECLINED;
    if (atomic_load(&hold_phase) == 4) {
        CHECK(*link == HOLD_LINK);
        *out = (struct vcpu_exit){VCPU_EXIT_DEBUG, v->s.ip, v->s.slot, 0, "upper replacement"};
        return TIER0_UPPER_STOP;
    }
    unsigned heads = 0;
    for (unsigned i = 0; i < tier0_arena.capacity; ++i)
        if (tier0_arena.regions[i].valid && tier0_arena.regions[i].key.ip == BASE) {
            hold_slot = i;
            ++heads;
        }
    CHECK(heads == 1 && *link == HOLD_LINK);
    hold_serial = tier0_arena.regions[hold_slot].serial;
    atomic_store(&hold_phase, 1);
    hold_wait(2);
    code_quiescent_fast(v);
    atomic_store(&hold_phase, 3);
    hold_wait(4);
    *link = 0;
    *out = (struct vcpu_exit){VCPU_EXIT_DEBUG, v->s.ip, v->s.slot, 0, "upper hold"};
    return TIER0_UPPER_STOP;
}
static const unsigned hold_due = 1;
static const struct tier0_upper_ops hold_ops = {.probe = hold_probe,
                                                .enter = hold_enter,
                                                .hot = hold_hot,
                                                .reclaim = hold_reclaim,
                                                .sequence = &hold_sequence,
                                                .heads = &hold_sequence,
                                                .head_mask = 0,
                                                .reclaim_due = &hold_due};
/* The warm-up's upper tier links nothing; regions are keyed by whether one
 * is registered, so the head region it leaves is the one hold_ops links
 * (another sequence value makes Tier0 probe again). */
static const uint64_t warm_sequence = 2;
static uint64_t warm_probe(struct vcpu *v, gpa_t pa)
{
    (void)v;
    (void)pa;
    return 0;
}
static enum tier0_upper_status warm_enter(struct vcpu *v, uint64_t *link, uint64_t budget,
                                          struct vcpu_exit *out)
{
    (void)v;
    (void)link;
    (void)budget;
    (void)out;
    return TIER0_UPPER_DECLINED;
}
static const struct tier0_upper_ops warm_ops = {.probe = warm_probe,
                                                .enter = warm_enter,
                                                .hot = hold_hot,
                                                .reclaim = hold_reclaim,
                                                .sequence = &warm_sequence,
                                                .heads = &warm_sequence,
                                                .head_mask = 0,
                                                .reclaim_due = &hold_due};
static void *hold_entry(void *unused)
{
    (void)unused;
    struct vcpu *v = cpus[0];
    v->s.ip = BASE;
    v->s.slot = 0;
    struct vcpu_exit e = engine_run(v, 3);
    CHECK(e.reason == VCPU_EXIT_DEBUG && e.detail && !strcmp(e.detail, "upper hold"));
    v->s.ip = BASE;
    v->s.slot = 0;
    e = engine_run(v, 3);
    CHECK(e.reason == VCPU_EXIT_DEBUG && e.detail && !strcmp(e.detail, "upper replacement"));
    return NULL;
}
static void *hold_retirer(void *unused)
{
    (void)unused;
    struct vcpu *v = cpus[1];
    hold_wait(1);
    bool acquired = code_cpu_write_begin(BASE, 8);
    __atomic_store_n(&memory[0], add_bundle(10), __ATOMIC_RELAXED);
    code_cpu_write_end(acquired);
    const struct tier0_region *r = &tier0_arena.regions[hold_slot];
    CHECK(!r->valid && !atomic_load(&tier0_arena.tags[hold_slot]));
    atomic_store(&hold_phase, 2);
    hold_wait(3);
    /* Executor 0 published after the retirement; with this executor's
     * publication the slot is reclaimed and allocated next. */
    code_quiescent(v);
    tier0_reclaim();
    code_lock();
    CHECK(tier0_arena.available == hold_slot + 1);
    code_unlock();
    v->s.ip = BASE;
    v->s.slot = 0;
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->s.gr[8] == v->s.gr[10]);
    CHECK(r->valid && r->key.ip == BASE && r->serial != hold_serial);
    atomic_store(&hold_phase, 4);
    return NULL;
}
static void upper_hold(void)
{
    code_cache_flush();
    code_quiescent(cpus[0]);
    code_quiescent(cpus[1]);
    tier0_reclaim();
    for (unsigned i = 0; i < 2; ++i)
        code_unregister_vcpu(cpus[i]);
    tier0_destroy();
    CHECK(tier0_configure(1048576));
    for (unsigned i = 0; i < 2; ++i)
        code_register_vcpu(cpus[i]);
    code_notify_cpu_write(BASE, 16);
    memory[0] = add_bundle(9);
    memory[1] = (UINT64_C(1) << 9) | (UINT64_C(1) << 50);
    cpus[0]->s.gr[9] = 5;
    cpus[1]->s.gr[10] = 11;
    tier0_set_upper(&warm_ops);
    for (unsigned turn = 0; turn < 3; ++turn)
        for (unsigned cpu = 0; cpu < 2; ++cpu) {
            cpus[cpu]->s.ip = BASE;
            cpus[cpu]->s.slot = 0;
            CHECK(engine_run(cpus[cpu], 3).reason == VCPU_EXIT_BUDGET);
        }
    atomic_store(&hold_phase, 0);
    tier0_set_upper(&hold_ops);
    join_pair(hold_entry, hold_retirer);
    tier0_set_upper(NULL);
    const struct tier0_region *r = &tier0_arena.regions[hold_slot];
    CHECK(atomic_load(&hold_phase) == 4 && r->valid && r->serial != hold_serial);
    CHECK(hold_probes[0] == 2 && hold_probes[1] == 1);
    CHECK(r->entries == 2);
    code_cache_flush();
    code_quiescent(cpus[0]);
    code_quiescent(cpus[1]);
    tier0_reclaim();
}
/* Two contracts share a Tier0 region. Each executor retains its own link
 * until publication moves the head sequence; enter still repairs a mismatch. */
static uint64_t private_sequence = 11;
static unsigned private_probes[2], private_repairs[2];
static atomic_uint private_turn;
static uint64_t private_link(const struct vcpu *v)
{
    return (private_sequence << 8) | (v->cfg.index + 1);
}
static uint64_t private_probe(struct vcpu *v, gpa_t pa)
{
    CHECK(pa == BASE);
    ++private_probes[v->cfg.index];
    return private_link(v);
}
static enum tier0_upper_status private_enter(struct vcpu *v, uint64_t *link, uint64_t budget,
                                             struct vcpu_exit *out)
{
    CHECK(budget == 1);
    if (*link != private_link(v)) {
        ++private_repairs[v->cfg.index];
        *link = private_link(v);
    }
    ++v->retired;
    *out = (struct vcpu_exit){VCPU_EXIT_BUDGET, v->s.ip, v->s.slot, 0, NULL};
    return TIER0_UPPER_STOP;
}
static const struct tier0_upper_ops private_ops = {
    .probe = private_probe,
    .enter = private_enter,
    .hot = hold_hot,
    .reclaim = hold_reclaim,
    .sequence = &private_sequence,
    .heads = &private_sequence,
    .reclaim_due = &hold_due,
};
static void *private_executor(void *arg)
{
    struct vcpu *v = cpus[(uintptr_t)arg];
    for (unsigned n = 0; n < 128; ++n) {
        while (atomic_load_explicit(&private_turn, memory_order_acquire) != 2 * n + v->cfg.index)
            sched_yield();
        if (!v->cfg.index && n == 64)
            ++private_sequence;
        v->s.ip = BASE;
        v->s.slot = 0;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        atomic_fetch_add_explicit(&private_turn, 1, memory_order_release);
    }
    return NULL;
}
static void upper_private(void)
{
    tier0_set_upper(&private_ops);
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i) {
        vcpu_reset(cpus[i], NULL);
        cpus[i]->s.psr = IA64_PSR_IC;
        CHECK(!pthread_create(&threads[i], NULL, private_executor, (void *)(uintptr_t)i));
    }
    for (unsigned i = 0; i < 2; ++i)
        CHECK(!pthread_join(threads[i], NULL));
    CHECK(!private_repairs[0] && !private_repairs[1]);
    CHECK(private_probes[0] == 2 && private_probes[1] == 2);
    tier0_set_upper(NULL);
    code_cache_flush();
    code_quiescent(cpus[0]);
    code_quiescent(cpus[1]);
    tier0_reclaim();
}
/* More live heads than link-cache slots must retain promotion progress and
 * pending holds, including the two heads that collide on every round. */
static unsigned collision_hot, collision_repairs;
static enum tier0_upper_hot collision_promote(struct vcpu *v, gpa_t pa, bool loop_head)
{
    (void)v;
    (void)pa;
    (void)loop_head;
    ++collision_hot;
    return TIER0_HOT_PENDING;
}
static uint64_t collision_probe(struct vcpu *v, gpa_t pa)
{
    (void)v;
    return pa;
}
static enum tier0_upper_status collision_enter(struct vcpu *v, uint64_t *link, uint64_t budget,
                                               struct vcpu_exit *out)
{
    CHECK(budget == 1);
    if (*link != v->s.ip) {
        ++collision_repairs;
        *link = v->s.ip;
    }
    ++v->retired;
    *out = (struct vcpu_exit){VCPU_EXIT_BUDGET, v->s.ip, v->s.slot, 0, NULL};
    return TIER0_UPPER_STOP;
}
static const struct tier0_upper_ops collision_link_ops = {
    .probe = collision_probe,
    .enter = collision_enter,
    .hot = hold_hot,
    .reclaim = hold_reclaim,
    .sequence = &private_sequence,
    .heads = &private_sequence,
    .reclaim_due = &hold_due,
};
static const struct tier0_upper_ops collision_ops = {
    .probe = warm_probe,
    .enter = warm_enter,
    .hot = collision_promote,
    .reclaim = hold_reclaim,
    .sequence = &private_sequence,
    .heads = &private_sequence,
    .reclaim_due = &hold_due,
    .loop_threshold = 4,
    .trace_threshold = 4,
};
static void upper_collisions(void)
{
    code_notify_cpu_write(BASE, sizeof(memory));
    for (unsigned n = 0; n < 257; ++n) {
        memory[2 * n] = add_bundle(9);
        memory[2 * n + 1] = UINT64_C(1) << 50;
    }
    tier0_set_upper(&collision_ops);
    struct vcpu *v = cpus[0];
    vcpu_reset(v, NULL);
    v->s.psr = IA64_PSR_IC;
    for (unsigned round = 0; round < 8; ++round) {
        for (unsigned n = 0; n < 257; ++n) {
            v->s.ip = BASE + 16 * n;
            v->s.slot = 0;
            CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        }
        CHECK(collision_hot == (round >= 3 ? 257 : 0));
    }
    ++private_sequence;
    tier0_set_upper(&collision_link_ops);
    for (unsigned round = 0; round < 2; ++round)
        for (unsigned n = 0; n < 257; ++n) {
            v->s.ip = BASE + 16 * n;
            v->s.slot = 0;
            CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        }
    CHECK(!collision_repairs);
    tier0_set_upper(NULL);
    code_cache_flush();
    code_quiescent(cpus[0]);
    code_quiescent(cpus[1]);
    tier0_reclaim();
}
static void chunk_quarantine(void)
{
    /* Hold both executor epochs while every old RX chunk is evicted. The
     * staged candidate cannot reuse any bytes until both readers leave. */
    const Ia64Instruction nop = {
        .valid = true, .opcode = IA64_OP_NOP, .unit = IA64_UNIT_M, .address = BASE};
    code_cache_flush();
    code_quiescent(cpus[0]);
    code_quiescent(cpus[1]);
    tier0_reclaim();
    for (unsigned i = 0; i < 2; ++i)
        code_unregister_vcpu(cpus[i]);
    tier0_destroy();
    CHECK(tier0_configure(1048576));
    for (unsigned i = 0; i < 2; ++i)
        code_register_vcpu(cpus[i]);
    code_lock();
    for (unsigned n = 0; n < tier0_arena.nchunks; ++n) {
        struct tier0_region *r = tier0_region_allocate();
        CHECK(r);
        *r = (struct tier0_region){.valid = true, .count = 1, .slots = {1}};
        CHECK(tier0_stage(r, &nop, &cpus[0]->s));
        tier0_arena.chunks[r->chunk].used = TIER0_CHUNK_BYTES;
        tier0_seal(cpus[0], r->chunk);
    }
    struct tier0_region *candidate = tier0_region_allocate();
    CHECK(candidate);
    *candidate = (struct tier0_region){.valid = true, .count = 1, .slots = {1}};
    CHECK(!tier0_stage(candidate, &nop, &cpus[0]->s));
    for (unsigned n = 0; n < tier0_arena.nchunks; ++n)
        CHECK(tier0_arena.chunks[n].executable && tier0_arena.chunks[n].regions);
    code_unlock();
    code_quiescent(cpus[0]);
    CHECK(!tier0_stage(candidate, &nop, &cpus[0]->s));
    code_quiescent(cpus[1]);
    CHECK(tier0_stage(candidate, &nop, &cpus[0]->s));
    CHECK(!tier0_arena.chunks[candidate->chunk].executable);
    tier0_region_retire(candidate, false);
    code_quiescent(cpus[0]);
    code_quiescent(cpus[1]);
    tier0_reclaim();
}
static void *debug_executor(void *arg)
{
    struct vcpu *v = cpus[(uintptr_t)arg];
    struct tier0_stats *stats = tier0_local_stats();
    uint64_t before = stats->interpreter_instructions[TIER0_INTERP_DEBUG];
    __atomic_store_n(&v->parked, 0, __ATOMIC_SEQ_CST);
    for (unsigned n = 0; n < ROUNDS; ++n) {
        v->s.ip = BASE;
        v->s.slot = 0;
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
        if (!((n + 1) % 1024))
            watchdog_progress();
    }
    CHECK(stats->interpreter_instructions[TIER0_INTERP_DEBUG] - before ==
          (v->cfg.index ? 0 : 3 * ROUNDS));
    code_quiescent(v);
    __atomic_store_n(&v->parked, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void threaded_debug(void)
{
    code_notify_cpu_write(BASE, 16);
    memory[0] = add_bundle(9);
    memory[1] = (UINT64_C(1) << 9) | (UINT64_C(1) << 50);
    decision_begin(TIER0_TEST_DEBUG_BEFORE);
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; ++i) {
        vcpu_reset(cpus[i], NULL);
        cpus[i]->cfg.engine = ENGINE_TIER0;
        cpus[i]->s.psr = IA64_PSR_IC | IA64_PSR_DB;
        cpus[i]->s.ibr[0] = BASE + 0x1000;
        cpus[i]->s.ibr[1] =
            i ? 0 : (UINT64_C(1) << 63) | (UINT64_C(1) << 56) | UINT64_C(0x00ffffffffffffff);
        CHECK(!pthread_create(&threads[i], NULL, debug_executor, (void *)(uintptr_t)i));
    }
    for (unsigned i = 0; i < 2; ++i)
        CHECK(!pthread_join(threads[i], NULL));
    tier0_set_test_hook(NULL);
    CHECK(atomic_load(&decision_phase) == 2);
    for (unsigned i = 0; i < 2; ++i)
        __atomic_store_n(&cpus[i]->parked, 0, __ATOMIC_SEQ_CST);
}
void ia64_smp_global_purge(struct vcpu *v, uint32_t rid, uint64_t va, unsigned shift, bool alat)
{
    (void)rid;
    (void)va;
    (void)shift;
    (void)alat;
    __asm__ volatile("mfence" ::: "memory");
    if (smp_checks) {
        CHECK(!tier0_is_shadow(v));
        ia64_tlb_remote_purge(v, 7, 0x400000, 12, false);
        ia64_smp_purge_hook(v, 7, 0x400000, 12, false);
    }
}

/* A sync.i waiter acknowledges a peer's purge (core/smp.c). A shadow
 * has no executor and never waits. */
void ia64_smp_sync_i(struct vcpu *v)
{
    if (!smp_checks || tier0_is_shadow(v))
        return;
    ia64_tlb_remote_purge(v, 7, 0x400000, 12, false);
    ia64_smp_purge_hook(v, 7, 0x400000, 12, false);
}

static void access_replay(void)
{
    struct vcpu *v = cpus[0];
    smp_checks = true;
    v->cfg.engine = ENGINE_LOCKSTEP;
    for (unsigned kind = 0; kind < 11; ++kind) {
        vcpu_reset(v, NULL);
        v->s.ip = BASE;
        v->s.slot = 0;
        v->s.gr[10] = BASE + 4096;
        v->s.gr[9] = 77;
        v->s.ar[AR_CCV] = kind == 4 ? 0x100 : 55;
        v->s.ar[AR_CSD] = 99;
        memory[512] = 55;
        memory[513] = 66;
        Ia64Instruction i = {.valid = true,
                             .unit = IA64_UNIT_M,
                             .address = BASE,
                             .opcode = kind == 0   ? IA64_OP_LD8
                                       : kind == 1 ? IA64_OP_ST8
                                       : kind == 2 ? IA64_OP_XCHG8
                                       : kind == 3 ? IA64_OP_FETCHADD8
                                       : kind == 4 ? IA64_OP_CMPXCHG1
                                       : kind == 5 ? IA64_OP_LD16
                                       : kind == 6 ? IA64_OP_ST16
                                       : kind == 7 ? IA64_OP_CMP8XCHG16
                                       : kind == 8 ? IA64_OP_LDFE
                                       : kind == 9 ? IA64_OP_PTC_G
                                                   : IA64_OP_SYNC_I};
        i.operands.memory = (IA64MemoryOperands){.destination = 8, .source = 9, .base = 10};
        if (kind >= 9) {
            if (kind == 9)
                i.operands.system = (IA64SystemOperands){.register_index = 10, .source = 9};
            v->s.gr[9] = 12 << 2;
            v->s.tc[0][0] =
                (struct ia64_tlb_entry){.valid = true, .rid = 7, .va = 0x400000, .page_shift = 12};
        }
        struct tier0_region r = {
            .valid = true, .count = 1, .insns = &i, .slots = {1}, .key.ip = BASE};
        struct tier0_execution e = {.region = &r, .check = tier0_check_begin(v)};
        CHECK(e.check && v->access_hook);
        (void)tier0_helper(v, &i, &e);
        CHECK(!e.stop && !e.fault);
        /* A peer can replace the whole datum between the real access and
         * reference. Replayed values/writes must still compare exactly. */
        __atomic_store_n(&memory[512], 9999, __ATOMIC_RELAXED);
        __atomic_store_n(&memory[513], 8888, __ATOMIC_RELAXED);
        CHECK(tier0_check_finish(&e, v));
        CHECK(memory[512] == 9999 && memory[513] == 8888); /* replay wrote no RAM */
        tier0_check_destroy(e.check);
    }
    smp_checks = false;
}

static void boundary_source_replay(void)
{
    struct vcpu *v = cpus[0];
    code_notify_cpu_write(BASE, 16);
    memory[0] = add_bundle(9);
    memory[1] = UINT64_C(1) << 50;
    vcpu_reset(v, NULL);
    v->cfg.engine = ENGINE_LOCKSTEP;
    v->s.ip = BASE;
    v->s.psr = IA64_PSR_IC;
    v->s.gr[9] = 111;
    v->s.gr[10] = 222;
    captured_replacement = add_bundle(10);
    capture_write_pending = true;
    smp_checks = true;
    bool checked;
    struct vcpu_exit out = tier0_check_debug_boundary(v, &checked);
    smp_checks = false;
    if (capture_write_pending || !checked || out.reason != VCPU_EXIT_BUDGET)
        fprintf(stderr, "source replay pending=%u checked=%u reason=%u detail=%s retired=%llu\n",
                capture_write_pending, checked, out.reason, out.detail ? out.detail : "",
                (unsigned long long)v->retired);
    CHECK(!capture_write_pending && checked && out.reason == VCPU_EXIT_BUDGET);
    CHECK(v->s.gr[8] == 222 && v->retired == 1);
    puts("smp source replay: peer write after capture checked");
    fflush(stdout);
    vcpu_reset(v, NULL);
    v->cfg.engine = ENGINE_INTERP;
}

static unsigned late_invalidations;
static void late_invalidate(struct code_dependency *dependency)
{
    (void)dependency;
    CHECK(code_lock_held());
    ++late_invalidations;
}
static void late_backing(void)
{
    const gpa_t low_page = 0x40000, high_page = 0x1000000;
    uint64_t low, high, version;
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, 1048576));
    CHECK(code_source_read_physical(BASE, &low, &high, &version).kind == IA64_STEP_COMPLETE);
    CHECK(code_page_tracked_fast(BASE));
    int before = atomic_load(&allocations);
    CHECK(gm_add_region(&(struct gm_region){
        .base = high_page, .size = 4096, .kind = GM_RAM, .hva = (void *)late_ram[1]}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = low_page, .size = 4096, .kind = GM_RAM, .hva = (void *)late_ram[0]}));
    /* The second new span fails after one unpublished allocation. Earlier
     * published spans/bits must survive, and neither addition may leak. */
    atomic_store(&fail_after, 1);
    CHECK(code_source_read_physical(low_page, &low, &high, &version).exit == VCPU_EXIT_BAD_ACCESS);
    CHECK(atomic_load(&allocations) == before);
    CHECK(code_page_tracked_fast(BASE));
    CHECK(!code_page_tracked_fast(low_page) && !code_page_tracked_fast(high_page));
    atomic_store(&fail_after, -1);
    CHECK(code_source_read_physical(low_page, &low, &high, &version).kind == IA64_STEP_COMPLETE);
    CHECK(code_source_read_physical(high_page, &low, &high, &version).kind == IA64_STEP_COMPLETE);
    CHECK(code_page_tracked_fast(low_page) && code_page_tracked_fast(high_page));
    struct code_dependency dependency = {.invalidate = late_invalidate};
    code_dependency_attach(low_page, &dependency);
    uint64_t value = 99;
    CHECK(gm_write(low_page, &value, sizeof(value)));
    CHECK(late_invalidations == 1 && !dependency.page);
    CHECK(code_source_read_physical(low_page, &low, &high, &version).kind == IA64_STEP_COMPLETE &&
          low == value);
    code_cache_flush();
    CHECK(atomic_load(&allocations) == 0);
    puts(
        "smp late backing: sorted additions, partial failure preservation and tracked write passed");
}

int main(void)
{
    const char *seconds = getenv("SMP_ENGINE_WATCHDOG_SECONDS");
    watchdog_start("smp-engines", seconds ? (unsigned)atoi(seconds) : 60);
    struct gm_region region = {
        .base = BASE, .size = sizeof(memory), .kind = GM_RAM, .hva = (void *)memory};
    CHECK(gm_add_region(&region));
    region = (struct gm_region){
        .base = BASE + 0x700000, .size = sizeof(spare), .kind = GM_RAM, .hva = spare};
    CHECK(gm_add_region(&region));
    /* A partially built bitmap must leave no allocation or published span
     * behind when a later span allocation fails. The next read can retry. */
    uint64_t failed_low, failed_high, failed_version;
    atomic_store(&fail_after, 1);
    CHECK(code_source_read_physical(BASE, &failed_low, &failed_high, &failed_version).exit ==
          VCPU_EXIT_BAD_ACCESS);
    CHECK(atomic_load(&allocations) == 0);
    CHECK(!code_page_tracked_fast(BASE));
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, 8192));
    for (unsigned i = 0; i < 2; ++i) {
        struct vcpu_config cfg = {.engine = ENGINE_INTERP, .index = i, .itc_icount = true};
        CHECK((cpus[i] = vcpu_create(&cfg)) != NULL);
        CHECK(code_cache_vcpu_init(cpus[i]));
        code_register_vcpu(cpus[i]);
    }
    CHECK(cpus[0]->decoded != cpus[1]->decoded);
    boundary_source_replay();
    join_pair(lock_worker, lock_worker);
    CHECK(locked_counter == 2 * ROUNDS);
    epochs();
    code_notify_cpu_write(BASE, 16);
    memory[0] = add_bundle(9);
    memory[1] = UINT64_C(1) << 50;
    const struct ia64_bundle *a, *b;
    CHECK(code_get_bundle(cpus[0], BASE, &a).kind == IA64_STEP_COMPLETE);
    CHECK(code_get_bundle(cpus[1], BASE, &b).kind == IA64_STEP_COMPLETE);
    CHECK(a != b);
    CHECK(code_page_tracked_fast(BASE));
    CHECK(!code_page_tracked_fast(BASE + 4096));
    CHECK(!code_page_tracked_fast(UINT64_C(0x80000000)));
    uint64_t saved = a->low;
    code_notify_cpu_write(BASE, 8);
    CHECK(a->low == saved && b->low == saved); /* Notification writes no bundles. */
    code_validity_set_test_hook(hook);
    join_pair(code_writer, code_executor);
    code_notify_cpu_write(BASE, 8);
    __atomic_store_n(&memory[0], 0, __ATOMIC_RELAXED);
    join_pair(source_writer, source_reader);
    CHECK(code_page_enter(cpus[1], BASE, false).kind == IA64_STEP_COMPLETE);
    CHECK(atomic_load(&snapshots) > 0);
    code_validity_set_test_hook(NULL);
    /* A full snapshot pool retires its victim, yields, and reuses it only
     * after BOTH executors publish the new epoch. */
    uint64_t low, high, version;
    CHECK(code_source_read_physical(BASE + 4096, &low, &high, &version).exit == VCPU_EXIT_BUDGET);
    CHECK(!code_page_has_snapshot(BASE + 4096));
    code_quiescent(cpus[0]);
    CHECK(code_source_read_physical(BASE + 4096, &low, &high, &version).exit == VCPU_EXIT_BUDGET);
    code_quiescent(cpus[1]);
    CHECK(code_source_read_physical(BASE + 4096, &low, &high, &version).kind == IA64_STEP_COMPLETE);
    /* A live flush may invalidate, but cannot free a stay's record. */
    code_cache_flush();
    CHECK(!code_page_tracked_fast(BASE));
    for (unsigned i = 0; i < 2; ++i) {
        code_unregister_vcpu(cpus[i]);
        vcpu_destroy(cpus[i]);
    }
    code_cache_flush();
    CHECK(atomic_load(&allocations) == 0);
    late_backing();
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, 1048576));
    CHECK(tier0_configure(1048576));
    for (unsigned i = 0; i < 2; ++i) {
        struct vcpu_config cfg = {.engine = ENGINE_TIER0, .index = i, .itc_icount = true};
        CHECK((cpus[i] = vcpu_create(&cfg)) != NULL);
        CHECK(engine_vcpu_attach(cpus[i])); /* product registration path */
    }
    write_bracket_range();
    tracked_producers();
    threaded_tier0(false);
    threaded_tier0(true);
    boundary_race();
    upper_hold();
    upper_private();
    upper_collisions();
    threaded_debug();
    chunk_quarantine();
    access_replay();
    for (unsigned i = 0; i < 2; ++i) {
        engine_vcpu_detach(cpus[i]);
        vcpu_destroy(cpus[i]);
    }
    code_cache_flush();
    tier0_destroy();
    CHECK(atomic_load(&allocations) == 0);
    alarm(0);
    printf(
        "smp-engine foundations: lock, per-vCPU cache, 10000 epoch rounds, 10000 cross-code rounds, 20000 source reads; concurrent Tier0/debug; boundary hit racing retirement; upper links, collisions and reclaimed heads; RX quarantine; lockstep RAM and PURGE replay passed\n");
    return 0;
}
