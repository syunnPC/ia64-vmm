/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Concurrent Tier0+Tier1 over shared guest RAM, using Linux HAL executor
 * threads and a compiler helper. Each mode runs in one process with loops
 * from tests/tier1/programs/smp_regions.S. A/B/C are vCPUs 0/1/2; outside
 * shared mode, B runs guest code and test threads execute A/C instructions.
 *
 * Modes:
 *   shared     concurrent first faults and fills use each executor's context
 *   xmod       a code store retires B's region and kicks it out
 *   pair       retirement between publication and validation declines the entry
 *   kick       a published entry yields within one loop iteration
 *   flag       st, sync.i, flag-store ordering (SDM Vol.2 Part II Fig. 2-9),
 *              with the retirement kick held until sync.i waits
 *   sync2      concurrent sync.i calls both wait for B to leave retired code
 *   parked     sync.i does not wait for B after it leaves and parks
 *   ptcg       ptc.g completes while its acknowledgement waits behind sync.i
 *   purge      after remapping and the flag, B accesses only the new page
 *   pin        unregistered execution pins a retired record until return
 *   width      all window instances of a VM have the same width
 *   probe      probes without code_lock during publication and reclamation
 *   chainxmod  retiring a chained successor kicks B out
 *   chainkick  a kick at a chain point yields before entering the successor
 *   chainpurge VHPT redirection and ptc.g prevent chaining to the old page;
 *              fetch guards must check mmu_seq
 *   checked*   ordered width-specific accesses with peer mutations, VHPT walks,
 *              long-format PTE byte orders, private executor sequences and
 *              sampling across vCPUs; unbacked trailing loads exit before effect
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "core/helper.h"
#include "core/smp.h"
#include "engine/cache/background.h"
#include "engine/hostwin/internal.h"
#include "engine/tier0/tier0.h"
#include "engine/tier1/region.h"
#include "engine/upper.h"
#include "ia64/semantics.h"
#include "ia64/memory.h"
#include "ia64/smp.h"
#include "ia64/tlb.h"
#include "ia64vmm/engine.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/interrupts.h"
#include "ia64vmm/platform.h"
#include "tests/hal/hal_linux.h"
#include "tests/tier1/engine_tier0t1.h"
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/engines.h"
#include "tests/tier1/harness.h"
#include "tests/tier1/hostwin_linux.h"

#define BIG (UINT64_C(1) << 40)
#define TIER1_SLICE 16384u /* the tier1_slice default (engine/engine.c) */
#define WAIT_SECONDS 10u
#define SHARED_PAGES 4u
#define SHARED_ITERATIONS 2000000u
#define FLAG_GPA T1H_STATUS
#define RESULT_GPA (T1H_STATUS + 8)
#define DATA_VA ((UINT64_C(2) << 61) | 0x40000)
#define DATA_RID 0x102u
#define OLD_PAGE T1H_TABLE
#define NEW_PAGE (T1H_TABLE + 0x1000)
#define XMOD_LOOP (T1H_CODE + 0x410) /* smp_regions.S .Lxmod: the head of B's loop region */
#define CHAIN_S (T1H_CODE + 0x2000)  /* smp_regions.S .Lchain_s: the head of S */

enum mode {
    SHARED,
    XMOD,
    PAIR,
    KICK,
    FLAG,
    SYNC2,
    PARKED,
    PTCG,
    PURGE,
    PIN,
    WIDTH,
    CHAINXMOD,
    CHAINKICK,
    CHAINPURGE,
    PROBE,
    CHECKED,
    CHECKED_WALK,
    CHECKED_PARTIAL,
    CHECKED_SHARED,
    CHECKED_LONG,
    CHECKED_LONG_BE,
    CHECKED_SAMPLED,
    CHECKED_FP,
    MODES
};
static const char *const names[MODES] = {"shared",
                                         "xmod",
                                         "pair",
                                         "kick",
                                         "flag",
                                         "sync2",
                                         "parked",
                                         "ptcg",
                                         "purge",
                                         "pin",
                                         "width",
                                         "chainxmod",
                                         "chainkick",
                                         "chainpurge",
                                         "probe",
                                         "checked",
                                         "checked-walk",
                                         "checked-partial",
                                         "checked-shared",
                                         "checked-long",
                                         "checked-long-be",
                                         "checked-sampled",
                                         "checked-fp"};
static enum mode mode;
static enum t1_check_mutation checked_mutation;
struct executor {
    struct vcpu *v;
    struct t1_vcpu *tv;
    struct t1h_logs logs;
    unsigned index, cpu, kicks, done;
    uint64_t stores, verification_entries;
    struct vcpu_exit exit;
};
static struct executor ex[3];
static struct vm *vm;
static _Thread_local struct executor *self;

static uint64_t seconds(void)
{
    return hal_counter() / hal_counter_hz();
}
static void wait_for(const unsigned *flag, const char *what)
{
    uint64_t end = seconds() + WAIT_SECONDS;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (seconds() > end) {
            fprintf(stderr, "smp-tier1: no %s within %u s\n", what, WAIT_SECONDS);
            t1h_fail(__FILE__, __LINE__, what);
        }
        sched_yield();
    }
}
static uint8_t *ram_at(gpa_t gpa)
{
    return t1h_ram() + (gpa - T1H_RAM_BASE);
}
static uint64_t ram_word(gpa_t gpa)
{
    return __atomic_load_n((uint64_t *)ram_at(gpa), __ATOMIC_ACQUIRE);
}
static void set_word(gpa_t gpa, uint64_t x)
{
    __atomic_store_n((uint64_t *)ram_at(gpa), x, __ATOMIC_RELEASE);
}
static struct t1_stats stats(void)
{
    struct t1_stats s;
    t1_get_stats(&s);
    return s;
}
static uint64_t sync_waits(void)
{
    return stats().sync_waits;
}
/* A test thread executes e's instructions; the boundary after each one
 * publishes e's quiescent epoch, as an executor's loop does, which
 * another vCPU's sync.i waits for. */
static void sync_i(struct executor *e)
{
    Ia64Instruction insn = {.opcode = IA64_OP_SYNC_I, .unit = IA64_UNIT_M, .address = T1H_CODE};
    T1H_CHECK(ia64_memory(e->v, &insn).kind == IA64_STEP_COMPLETE);
    code_quiescent(e->v);
}

/* ---- executors ---------------------------------------------------------- */

/* B's kick-yield boundary: outside region code, its own thread. */
static struct {
    unsigned armed, measured;
    bool pending;
    uint64_t retired, delta;
    unsigned max_path;
} kick;
static struct {
    unsigned armed, parked, release;
} park;
static void at_kick(struct executor *e)
{
    if (e != &ex[1])
        return;
    if (kick.pending) {
        kick.pending = false;
        kick.delta = e->v->retired - kick.retired;
        __atomic_store_n(&kick.measured, 1, __ATOMIC_RELEASE);
    }
    if (__atomic_exchange_n(&park.armed, 0, __ATOMIC_ACQ_REL)) {
        vm_cpu_park(e->v);
        __atomic_store_n(&park.parked, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&park.release, __ATOMIC_ACQUIRE))
            sched_yield();
        vm_cpu_unpark(e->v);
    }
}
static void run(void *arg)
{
    struct executor *e = self = arg;
    for (;;) {
        code_quiescent(e->v);
        ia64_smp_apply_purges(e->v);
        struct vcpu_exit out = tier0_run(e->v, BIG);
        if (t1h_kick_yield(&out)) {
            __atomic_add_fetch(&e->kicks, 1, __ATOMIC_ACQ_REL);
            at_kick(e);
            continue;
        }
        e->exit = out;
        break;
    }
    __atomic_store_n(&vm->cpu[e->index].state, VM_CPU_STOPPED, __ATOMIC_RELEASE);
    __atomic_store_n(&e->done, 1, __ATOMIC_RELEASE);
}
static void start(struct executor *e)
{
    T1H_CHECK(hal_cpu_start(e->cpu, run, e));
}
static void finish(struct executor *e)
{
    wait_for(&e->done, "executor end");
    T1H_CHECK(e->exit.reason == VCPU_EXIT_DEBUG && e->logs.last_break_imm == T1H_BREAK_END);
}
/* The executor runs a Tier1 region (its running region). */
static void wait_running(struct executor *e)
{
    uint64_t end = seconds() + WAIT_SECONDS;
    while (!t1_test_running(e->v)) {
        if (__atomic_load_n(&e->done, __ATOMIC_ACQUIRE))
            fprintf(stderr, "smp-tier1: executor %u left: reason %u ip %#llx slot %u %s\n",
                    e->index, (unsigned)e->exit.reason, (unsigned long long)e->exit.ip,
                    e->exit.slot, e->exit.detail ? e->exit.detail : "");
        T1H_CHECK(!__atomic_load_n(&e->done, __ATOMIC_ACQUIRE));
        T1H_CHECK(seconds() <= end);
        sched_yield();
    }
}

/* shared: Linux's single address space uses separate ranges for per-CPU
 * contexts representing private roots. Wrong-CPU or unknown-context fills
 * are counted but allowed, so shared() reports them; bare metal refuses them. */
const struct hostwin_backend *__real_hostwin_host_backend(void **ctx);
const struct hostwin_backend *__wrap_hostwin_host_backend(void **ctx);
struct root {
    unsigned cpu, fills;
};
static struct root roots[4];
static unsigned foreign_fills;
static const struct hostwin_backend *linux_backend;
static void *linux_ctx;
static struct root *root_of(void *ctx)
{
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(roots); ++i)
        if (ctx == &roots[i])
            return &roots[i];
    return NULL;
}
static void *rooted_context(void *ctx, unsigned cpu)
{
    (void)ctx;
    return cpu < IA64VMM_ARRAY_SIZE(roots) ? &roots[cpu] : NULL;
}
static bool rooted_reserve(void *ctx, unsigned w, unsigned slots, uintptr_t *base,
                           uintptr_t *poison)
{
    (void)ctx;
    return linux_backend->reserve(linux_ctx, w, slots, base, poison);
}
static void rooted_release(void *ctx, uintptr_t base, unsigned w, unsigned slots)
{
    (void)ctx;
    linux_backend->release(linux_ctx, base, w, slots);
}
static enum hostwin_map_status rooted_map(void *ctx, uintptr_t page, gpa_t pa, bool writable)
{
    struct root *r = root_of(ctx);
    if (r && r->cpu == hal_cpu_index())
        __atomic_add_fetch(&r->fills, 1, __ATOMIC_RELAXED);
    else
        __atomic_add_fetch(&foreign_fills, 1, __ATOMIC_RELAXED);
    return linux_backend->map(linux_ctx, page, pa, writable);
}
static bool rooted_unmap(void *ctx, uintptr_t page, uint64_t bytes)
{
    (void)ctx;
    return linux_backend->unmap(linux_ctx, page, bytes);
}
static bool rooted_protect(void *ctx, uintptr_t page)
{
    (void)ctx;
    return linux_backend->protect_ro(linux_ctx, page);
}
static void rooted_flush(void *ctx, uintptr_t page, bool sync)
{
    (void)ctx;
    linux_backend->flush(linux_ctx, page, sync);
}
static const struct hostwin_backend rooted = {
    .reserve = rooted_reserve,
    .release = rooted_release,
    .map = rooted_map,
    .unmap = rooted_unmap,
    .protect_ro = rooted_protect,
    .flush = rooted_flush,
    .cpu_context = rooted_context,
};
const struct hostwin_backend *__wrap_hostwin_host_backend(void **ctx)
{
    const struct hostwin_backend *real = __real_hostwin_host_backend(ctx);
    if (mode != SHARED || !real)
        return real;
    linux_backend = real;
    linux_ctx = *ctx;
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(roots); ++i)
        roots[i].cpu = i;
    return &rooted;
}

static void make(struct executor *e, unsigned index, bool engine)
{
    e->index = index;
    e->cpu = 1 + index;
    struct vcpu_config cfg = {.engine = ENGINE_TIER1,
                              .itc_hz = IA64VMM_ITC_HZ,
                              .index = index,
                              .lid = (uint64_t)index << 24};
    e->v = vcpu_create(&cfg);
    T1H_CHECK(e->v);
    t1h_bind(e->v, &e->logs);
    vm->cpu[index].v = e->v;
    vm->cpu[index].host_cpu = e->cpu;
    vm->cpu[index].state = VM_CPU_RUNNING;
    T1H_CHECK(platform_irq_bind(e->v, index, cfg.lid, e->cpu));
    if (!engine)
        return;
    if (mode == SHARED) {
        /* The product path: the instance takes its host CPU's context. */
        e->tv = engine_tier1_glue()->vcpu_create(e->v);
        T1H_CHECK(e->tv && e->tv->hw->ctx == &roots[e->cpu]);
    } else {
        e->tv = t1h_tier1_vcpu(t1h_engine_tier1()->create(e->v));
    }
    e->v->upper = e->tv;
    t1_test_set_compile_options(e->tv, &(struct t1_compile_options){0});
    code_register_vcpu(e->v);
}
static void reset(struct executor *e, unsigned offset, uint64_t psr, const uint64_t gr[32])
{
    struct ia64_entry_regs regs = {.ip = T1H_CODE + offset,
                                   .psr = psr | IA64_PSR_IC | IA64_PSR_BN,
                                   .bsp = T1H_BSP + e->index * 0x1000,
                                   .bspstore = T1H_BSP + e->index * 0x1000};
    memcpy(regs.gr, gr, sizeof(regs.gr));
    vcpu_reset(e->v, &regs);
}
/* B at the xmod loop: r14 counts the bundle at 0x430, r15 its replacement
 * (r12 each). */
static void reset_xmod(struct executor *b)
{
    uint64_t gr[32] = {0};
    gr[9] = 1;
    gr[12] = 1000;
    gr[21] = FLAG_GPA;
    reset(b, 0x400, 0, gr);
}
static void reset_flag(struct executor *b)
{
    uint64_t gr[32] = {0};
    gr[21] = FLAG_GPA;
    gr[22] = RESULT_GPA;
    reset(b, 0x800, 0, gr);
}

/* ---- shared ------------------------------------------------------------ */

/* First Tier1 fault per executor: hold B until A faults in the same region,
 * then resolve B before A. A shared trap context would overwrite B's fault
 * with A's before B resolves. */
bool __real_hostwin_trap(struct hostwin_trap *t);
bool __wrap_hostwin_trap(struct hostwin_trap *t);
static unsigned b_held, a_held, b_resolved;
static _Thread_local bool first_seen;
bool __wrap_hostwin_trap(struct hostwin_trap *t)
{
    if ((mode != SHARED && mode != CHECKED_SHARED && mode != CHECKED_SAMPLED) || !self ||
        first_seen)
        return __real_hostwin_trap(t);
    first_seen = true;
    if (self == &ex[1]) {
        __atomic_store_n(&b_held, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&a_held, __ATOMIC_ACQUIRE))
            sched_yield();
        uint64_t rip = t->rip;
        bool accepted = __real_hostwin_trap(t);
        T1H_CHECK(accepted && self->tv->trap_rip == rip);
        __atomic_store_n(&b_resolved, 1, __ATOMIC_RELEASE);
        return accepted;
    }
    __atomic_store_n(&a_held, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&b_resolved, __ATOMIC_ACQUIRE))
        sched_yield();
    return __real_hostwin_trap(t);
}
static uint64_t shared_sum(gpa_t base, uint64_t increment)
{
    uint64_t sum = 0, value[SHARED_PAGES];
    for (unsigned k = 0; k < SHARED_PAGES; ++k)
        value[k] = ram_word(base + k * 4096);
    for (uint64_t i = 0; i < SHARED_ITERATIONS; ++i) {
        sum += value[i % SHARED_PAGES];
        value[i % SHARED_PAGES] += increment;
    }
    return sum;
}
static void shared(void)
{
    uint64_t expected[2];
    for (unsigned i = 0; i < 2; ++i) {
        gpa_t base = T1H_TABLE + i * 0x8000;
        for (unsigned k = 0; k < SHARED_PAGES; ++k)
            set_word(base + k * 4096, 100 * (i + 1) + k);
        uint64_t gr[32] = {0};
        gr[9] = 3 + 2 * i;
        gr[20] = SHARED_ITERATIONS;
        gr[21] = base;
        gr[23] = SHARED_PAGES;
        gr[24] = 4096;
        expected[i] = shared_sum(base, gr[9]);
        reset(&ex[i], 0, 0, gr);
    }
    start(&ex[1]);
    wait_for(&b_held, "Tier1 host fault of B");
    start(&ex[0]);
    for (unsigned i = 0; i < 2; ++i) {
        finish(&ex[i]);
        T1H_CHECK(ex[i].v->s.gr[14] == expected[i]);
    }
    /* Each executor's faults filled its own CPU's context, never another's. */
    T1H_CHECK(roots[ex[0].cpu].fills && roots[ex[1].cpu].fills && !foreign_fills);
}

/* ---- xmod, pair -------------------------------------------------------- */

/* A guest store: two 8-byte stores bracketed as ia64/memory.c ram_write does. */
static void patch(unsigned offset, unsigned replacement)
{
    uint64_t *to = (uint64_t *)ram_at(T1H_CODE + offset);
    const uint64_t *from = (const uint64_t *)ram_at(T1H_CODE + replacement);
    bool acquired = code_cpu_write_begin(T1H_CODE + offset, 16);
    for (unsigned i = 0; i < 2; ++i)
        __atomic_store_n(&to[i], __atomic_load_n(&from[i], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    code_cpu_write_end(acquired);
}
static void xmod(void)
{
    struct executor *b = &ex[1];
    reset_xmod(b);
    start(b);
    wait_running(b);
    patch(0x430, 0x1400);
    wait_for(&b->kicks, "kick of the executor running the retired region");
    set_word(FLAG_GPA, 1);
    finish(b);
    T1H_CHECK(b->v->s.gr[15] >= 1000);
}

/* The chain modes (below): B at a chain point after R, B entered S at a
 * chain point (chainpurge arms only then), the old bundle's count r14 at
 * the purge. */
static struct {
    unsigned at_s, purge_seen, linked;
    uint64_t r14;
} chain;

/* B's entries of its xmod loop region, between their publication and their
 * valid check: pair stops one there; kick kicks one there. */
static struct {
    unsigned armed, stalled, go;
} pair_entry;
static void publish_hook(struct vcpu *v, const struct t1_region *r)
{
    /* chainpurge: S was entered at a chain point (and its fetch row passed). */
    if (self == &ex[1] && r->c.ip == CHAIN_S && ((const struct t1_vcpu *)v->upper)->chained)
        chain.linked = 1;
    if (self != &ex[1] || r->c.ip != XMOD_LOOP)
        return;
    if (__atomic_exchange_n(&pair_entry.armed, 0, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&pair_entry.stalled, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&pair_entry.go, __ATOMIC_ACQUIRE))
            sched_yield();
    }
    if (__atomic_exchange_n(&kick.armed, 0, __ATOMIC_ACQ_REL)) {
        kick.pending = true;
        kick.retired = v->retired;
        kick.max_path = r->max_path;
        vcpu_kick(v);
    }
}
static void pair(void)
{
    struct executor *b = &ex[1];
    reset_xmod(b);
    uint64_t declined = stats().decline_retired;
    __atomic_store_n(&pair_entry.armed, 1, __ATOMIC_RELEASE);
    start(b);
    wait_for(&pair_entry.stalled, "entry stopped after its publication");
    /* B is at a boundary: r14 counts every execution of the old bundle. */
    uint64_t old_runs = b->v->s.gr[14];
    uint64_t entry = __atomic_load_n(&b->tv->entry_seq, __ATOMIC_SEQ_CST);
    patch(0x430, 0x1400);
    /* The retirement found the published entry and marked it. */
    T1H_CHECK(__atomic_load_n(&b->tv->stale_seq, __ATOMIC_SEQ_CST) == entry);
    __atomic_store_n(&pair_entry.go, 1, __ATOMIC_RELEASE);
    wait_for(&b->kicks, "kick of the entry the retirement found");
    set_word(FLAG_GPA, 1);
    finish(b);
    /* The old bundle never ran after the store: the entry declined. */
    T1H_CHECK(b->v->s.gr[14] == old_runs && b->v->s.gr[15] >= 1000);
    T1H_CHECK(stats().decline_retired > declined);
}

/* ---- kick -------------------------------------------------------------- */

static void kick_latency(void)
{
    struct executor *b = &ex[1];
    reset_xmod(b);
    start(b);
    wait_running(b);
    __atomic_store_n(&kick.armed, 1, __ATOMIC_RELEASE);
    wait_for(&kick.measured, "end of the kicked entry");
    set_word(FLAG_GPA, 1);
    finish(b);
    printf("smp-tier1 kick: %llu instructions after the kick (max_path %u, tier1_slice %u)\n",
           (unsigned long long)kick.delta, kick.max_path, TIER1_SLICE);
    T1H_CHECK(kick.delta <= kick.max_path && kick.delta < TIER1_SLICE);
}

/* ---- flag, sync2, parked, ptcg ---------------------------------------- */

void __real_vcpu_kick(struct vcpu *v);
void __wrap_vcpu_kick(struct vcpu *v);
static unsigned hold_kicks, held_kicks, released;
void __wrap_vcpu_kick(struct vcpu *v)
{
    if (__atomic_load_n(&hold_kicks, __ATOMIC_ACQUIRE) && v == ex[1].v) {
        __atomic_add_fetch(&held_kicks, 1, __ATOMIC_ACQ_REL);
        return;
    }
    __real_vcpu_kick(v);
}
/* Releases B's held kicks once `waits` sync.i waits began and `kicks` kicks
 * were held, or once the test gave up waiting (stop). */
static struct {
    uint64_t before;
    unsigned waits, kicks, stop;
} release_when;
static void *release_kick(void *arg)
{
    (void)arg;
    while ((sync_waits() - release_when.before < release_when.waits ||
            __atomic_load_n(&held_kicks, __ATOMIC_ACQUIRE) < release_when.kicks) &&
           !__atomic_load_n(&release_when.stop, __ATOMIC_ACQUIRE))
        sched_yield();
    __atomic_store_n(&released, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&hold_kicks, 0, __ATOMIC_RELEASE);
    __real_vcpu_kick(ex[1].v);
    return NULL;
}
/* B runs the flag loop's region; the retirement of A's store is held at B. */
static void hold_and_patch(unsigned waits, unsigned kicks, pthread_t *releaser)
{
    struct executor *b = &ex[1];
    reset_flag(b);
    start(b);
    wait_running(b);
    release_when.before = sync_waits();
    release_when.waits = waits;
    release_when.kicks = kicks;
    __atomic_store_n(&hold_kicks, 1, __ATOMIC_RELEASE);
    /* A: st [P] = new (a notified CPU store retires B's region) ... */
    patch(0x820, 0x1440);
    T1H_CHECK(__atomic_load_n(&held_kicks, __ATOMIC_ACQUIRE));
    T1H_CHECK(!pthread_create(releaser, NULL, release_kick, NULL));
}
/* ... st [flag] = 1; a kick still held (sync.i did not wait) is released
 * only afterwards, so B's retired region would see the flag. */
static void flag_end(pthread_t releaser)
{
    set_word(FLAG_GPA, 1);
    __atomic_store_n(&release_when.stop, 1, __ATOMIC_RELEASE);
    T1H_CHECK(!pthread_join(releaser, NULL));
    finish(&ex[1]);
    T1H_CHECK(ram_word(RESULT_GPA) == 2);
}
static void flag(void)
{
    pthread_t releaser;
    hold_and_patch(1, 1, &releaser);
    /* ... fc.i (no effect here), sync.i, mf ... */
    sync_i(&ex[0]);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    flag_end(releaser);
}

/* sync.i that waits returns only after B left, which needs the release. */
static void *sync_after_release(void *arg)
{
    sync_i(arg);
    T1H_CHECK(__atomic_load_n(&released, __ATOMIC_ACQUIRE));
    return NULL;
}
static void sync2(void)
{
    pthread_t releaser, thread[2];
    hold_and_patch(2, 1, &releaser);
    T1H_CHECK(!pthread_create(&thread[0], NULL, sync_after_release, &ex[0]));
    T1H_CHECK(!pthread_create(&thread[1], NULL, sync_after_release, &ex[2]));
    for (unsigned i = 0; i < 2; ++i)
        T1H_CHECK(!pthread_join(thread[i], NULL));
    flag_end(releaser);
}

static void parked(void)
{
    struct executor *b = &ex[1];
    reset_flag(b);
    start(b);
    wait_running(b);
    __atomic_store_n(&park.armed, 1, __ATOMIC_RELEASE);
    patch(0x820, 0x1440);
    wait_for(&park.parked, "B parked after the retirement's kick");
    /* The retirement marked B's entry, which has ended. */
    T1H_CHECK(!t1_test_running(b->v));
    T1H_CHECK(__atomic_load_n(&b->tv->stale_seq, __ATOMIC_SEQ_CST) ==
              __atomic_load_n(&b->tv->entry_seq, __ATOMIC_SEQ_CST));
    uint64_t waits = sync_waits();
    sync_i(&ex[0]);
    T1H_CHECK(sync_waits() == waits);
    __atomic_store_n(&park.release, 1, __ATOMIC_RELEASE);
    set_word(FLAG_GPA, 1);
    finish(b);
    T1H_CHECK(ram_word(RESULT_GPA) == 2);
}

/* A: sync.i, then its boundaries acknowledge purges until C's completes. */
static unsigned purge_done;
static void *sync_then_acknowledge(void *arg)
{
    struct executor *a = arg;
    sync_after_release(a);
    while (!__atomic_load_n(&purge_done, __ATOMIC_ACQUIRE)) {
        ia64_smp_apply_purges(a->v);
        sched_yield();
    }
    return NULL;
}
static void *global_purge(void *arg)
{
    struct executor *c = arg;
    ia64_smp_global_purge(c->v, DATA_RID, DATA_VA & ~UINT64_C(4095), 12, false);
    code_quiescent(c->v);
    __atomic_store_n(&purge_done, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void ptcg(void)
{
    pthread_t releaser, a, c;
    /* The release needs A's wait and the purge's kick of B. */
    hold_and_patch(1, 2, &releaser);
    T1H_CHECK(!pthread_create(&a, NULL, sync_then_acknowledge, &ex[0]));
    uint64_t end = seconds() + WAIT_SECONDS;
    while (sync_waits() == release_when.before) {
        T1H_CHECK(seconds() <= end);
        sched_yield();
    }
    T1H_CHECK(!pthread_create(&c, NULL, global_purge, &ex[2]));
    T1H_CHECK(!pthread_join(c, NULL) && !pthread_join(a, NULL));
    flag_end(releaser);
}

/* ---- purge ------------------------------------------------------------- */

static uint64_t low_mask(unsigned bits)
{
    return (UINT64_C(1) << bits) - 1;
}
static void tr(struct vcpu *v, unsigned side, unsigned slot, uint64_t va, gpa_t pa, unsigned ps,
               uint64_t flags)
{
    v->s.tr[side][slot] = (struct ia64_tlb_entry){.va = va & low_mask(61) & ~low_mask(ps),
                                                  .pte = pa | flags,
                                                  .itir = (uint64_t)ps << 2,
                                                  .rid = (v->s.rr[va >> 61] >> 8) & 0xffffff,
                                                  .page_shift = ps,
                                                  .valid = true};
    ia64_mmu_changed(v, IA64_MMU_TR, side, slot);
}
static void set_rr(struct vcpu *v, unsigned r, uint64_t value)
{
    v->s.rr[r] = value;
    ia64_mmu_changed(v, IA64_MMU_RR, 0, r);
}
static uint64_t vhpt_offset(uint64_t va)
{
    return ((va & low_mask(61)) >> 12) * 8 & 0x7fff;
}
static void maps_old(uint64_t gva_page, gpa_t pa_page, bool writable, void *arg)
{
    (void)gva_page;
    (void)writable;
    if (pa_page == OLD_PAGE)
        *(bool *)arg = true;
}
static void purge(void)
{
    struct executor *a = &ex[0], *b = &ex[1];
    const uint64_t present = 1 | 0x20 | 0x40, rw = present | (UINT64_C(2) << 9);
    set_word(OLD_PAGE, 1);
    set_word(NEW_PAGE, 2);
    memset(ram_at(T1H_VHPT), 0, 0x1000);
    set_word(T1H_VHPT + vhpt_offset(DATA_VA), OLD_PAGE | rw);
    code_notify_vmm_write(T1H_VHPT, 0x1000);
    uint64_t gr[32] = {0};
    gr[20] = DATA_VA;
    gr[21] = FLAG_GPA;
    gr[22] = RESULT_GPA;
    reset(b, 0xc00, IA64_PSR_IT | IA64_PSR_DT, gr);
    struct vcpu *v = b->v;
    set_rr(v, 0, (UINT64_C(0x100) << 8) | (16u << 2));
    set_rr(v, 2, ((uint64_t)DATA_RID << 8) | (12u << 2) | 1);
    v->s.cr[CR_PTA] = T1H_VHPT | (15u << 2) | 1; /* 32 KiB, short format, walker on */
    tr(v, 0, 0, T1H_CODE, T1H_CODE, 16, rw | (UINT64_C(3) << 9));
    tr(v, 1, 0, FLAG_GPA, FLAG_GPA, 12, rw);
    tr(v, 1, 1, (UINT64_C(2) << 61) | T1H_VHPT, T1H_VHPT, 12, rw);
    start(b);
    /* B's window maps the old page through the walker's TC entry. */
    uint64_t end = seconds() + WAIT_SECONDS;
    for (bool mapped = false; !mapped;) {
        T1H_CHECK(seconds() <= end && !__atomic_load_n(&b->done, __ATOMIC_ACQUIRE));
        hostwin_test_shadows(b->tv->hw, maps_old, &mapped);
        sched_yield();
    }
    /* A: the page-table update, ptc.g (completes when B purged), mf, flag. */
    set_word(T1H_VHPT + vhpt_offset(DATA_VA), NEW_PAGE | rw);
    ia64_smp_global_purge(a->v, DATA_RID, DATA_VA & ~UINT64_C(4095), 12, false);
    set_word(FLAG_GPA, 1);
    finish(b);
    T1H_CHECK(ram_word(RESULT_GPA) == 2);
}

/* ---- chainxmod, chainkick, chainpurge ------------------------------------ */

#define CHAIN_PASSES 200000u
#define CHAIN_NEW (T1H_TABLE + 0x3000) /* chainpurge: S's page with its bundle replaced */
#define CHAIN_RID 0x100u
/* B at smp_regions.S .Lchain: R and S run 4 iterations a pass; S adds r9 = 1 to
 * r14, its replacement r12 = 1000 to r15. */
static void reset_chain(struct executor *b, uint64_t psr)
{
    uint64_t gr[32] = {0};
    gr[9] = 1;
    gr[11] = 4;
    gr[12] = 1000;
    gr[19] = CHAIN_PASSES;
    gr[20] = 4;
    gr[21] = FLAG_GPA;
    reset(b, 0x1800, psr, gr);
}
/* B's chain points after R (the target is S's head) once S is R's
 * successor in B's cache. The hook ends the run soon after its case: it
 * sets the pass counter r19, or (chainxmod) r16 and r17 so that S of this
 * pass runs until the flag. */
static bool successor_known(uint64_t from)
{
    const struct t1_vcpu *tv = ex[1].tv;
    for (unsigned k = 0; k < T1_SUCCESSORS; ++k)
        if (tv->successors[k].from == from && t1_region_from_link(tv->successors[k].to))
            return true;
    return false;
}
static void chain_hook(struct vcpu *v, uint64_t from, bool resolved)
{
    if (self != &ex[1] || v->s.ip != CHAIN_S || !successor_known(from))
        return;
    if (mode == CHAINXMOD && resolved && !chain.at_s) {
        v->s.gr[16] = UINT64_MAX;
        v->s.gr[17] = 0;
        __atomic_store_n(&chain.at_s, 1, __ATOMIC_RELEASE);
    }
    if (resolved)
        return;
    if (mode == CHAINKICK && __atomic_exchange_n(&kick.armed, 0, __ATOMIC_ACQ_REL)) {
        kick.pending = true;
        kick.retired = v->retired;
        v->s.gr[19] = 2;
        vcpu_kick(v);
    }
    if (mode == CHAINPURGE && chain.linked && !chain.purge_seen) {
        /* A queues the purge and kicks B, then waits for B's acknowledgement;
         * later passes chain from R again. */
        chain.purge_seen = 1;
        chain.r14 = v->s.gr[14];
        v->s.gr[19] = 10;
        __atomic_store_n(&chain.at_s, 1, __ATOMIC_RELEASE);
        uint64_t end = seconds() + WAIT_SECONDS;
        while (!__atomic_load_n(&v->irq_kick, __ATOMIC_ACQUIRE)) {
            T1H_CHECK(seconds() <= end);
            sched_yield();
        }
    }
}
static void chainxmod(void)
{
    struct executor *b = &ex[1];
    reset_chain(b, 0);
    start(b);
    wait_for(&chain.at_s, "B at the chain point after R");
    /* B publishes S as its running region and stays in it until the
     * flag; the retirement of A's store kicks it out. */
    wait_running(b);
    patch(0x2020, 0x2400);
    wait_for(&b->kicks, "kick of the executor running the retired region");
    set_word(FLAG_GPA, 1);
    finish(b);
    T1H_CHECK(b->v->s.gr[15] >= 1000 && stats().chain_links > 0);
}

static void chainkick(void)
{
    struct executor *b = &ex[1];
    reset_chain(b, 0);
    set_word(FLAG_GPA, 1);
    __atomic_store_n(&kick.armed, 1, __ATOMIC_RELEASE);
    start(b);
    wait_for(&kick.measured, "B's yield after the kick at its chain point");
    finish(b);
    printf(
        "smp-tier1 chainkick: %llu instructions between the kick at the chain point and the yield\n",
        (unsigned long long)kick.delta);
    T1H_CHECK(kick.delta == 0 && stats().chain_links > 0);
}
static void chainpurge(void)
{
    struct executor *a = &ex[0], *b = &ex[1];
    const uint64_t present = 1 | 0x20 | 0x40, rw = present | (UINT64_C(2) << 9),
                   rwx = present | (UINT64_C(3) << 9);
    /* S's page with the bundle at 0x020 replaced by the one at 0x400. */
    memcpy(ram_at(CHAIN_NEW), ram_at(CHAIN_S), 0x1000);
    memcpy(ram_at(CHAIN_NEW + 0x20), ram_at(CHAIN_S + 0x400), 16);
    code_notify_vmm_write(CHAIN_NEW, 0x1000);
    memset(ram_at(T1H_VHPT), 0, 0x8000);
    set_word(T1H_VHPT + vhpt_offset(CHAIN_S), CHAIN_S | rwx);
    code_notify_vmm_write(T1H_VHPT, 0x8000);
    reset_chain(b, IA64_PSR_IT | IA64_PSR_DT);
    struct vcpu *v = b->v;
    set_rr(v, 0, ((uint64_t)CHAIN_RID << 8) | (12u << 2) | 1);
    v->s.cr[CR_PTA] = T1H_VHPT | (15u << 2) | 1; /* 32 KiB, short format, walker on */
    tr(v, 0, 0, T1H_CODE + 0x1000, T1H_CODE + 0x1000, 12, rwx);
    tr(v, 0, 1, T1H_IVT, T1H_IVT, 15, rwx);
    tr(v, 1, 0, FLAG_GPA, FLAG_GPA, 12, rw);
    tr(v, 1, 1, T1H_VHPT, T1H_VHPT, 12, rw);
    set_word(FLAG_GPA, 1);
    start(b);
    wait_for(&chain.at_s, "B at the chain point after R");
    /* A: the page-table update, ptc.g (completes when B purged). */
    set_word(T1H_VHPT + vhpt_offset(CHAIN_S), CHAIN_NEW | rwx);
    ia64_smp_global_purge(a->v, CHAIN_RID, CHAIN_S, 12, false);
    finish(b);
    /* No execution of the old bundle after the purge point; the new one ran. */
    T1H_CHECK(b->v->s.gr[14] == chain.r14 && b->v->s.gr[15] >= 1000 && stats().chain_links > 0);
}

/* ---- ordered checked accesses ------------------------------------------ */

#define CHECKED_SHARED_ITERATIONS 200u
static void shared_checked_store(const struct t1_store_record *r, void *arg)
{
    (void)arg;
    T1H_CHECK(self && r->seq == self->stores++ && r->width == 8);
}
static void shared_checked_restore(struct t1_vcpu *tv, const struct vcpu *v,
                                   const struct t1_restore_record *record, void *arg)
{
    (void)arg;
    if (!record->materialized)
        return;
    const struct t1_region *r = tv->x.region;
    unsigned kind = r->point[record->point_id].kind;
    uint64_t backedges = CHECKED_SHARED_ITERATIONS - v->s.gr[20];
    if (kind == T1_PT_EXIT)
        --backedges;
    if (kind == T1_PT_CHECKPOINT || kind == T1_PT_EXIT)
        T1H_CHECK(record->iteration == self->verification_entries + backedges);
}
static void shared_checked_entry(struct t1_vcpu *tv, uint32_t region, void *arg)
{
    (void)tv;
    (void)region;
    (void)arg;
    ++self->verification_entries;
}
static void checked_shared(void)
{
    bool sampled = mode == CHECKED_SAMPLED;
    t1_cfg.check_every = sampled ? 3 : 0;
    struct t1_compile_options opt = {T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG |
                                         T1_COMPILE_UNDO_LOG | T1_COMPILE_ACCESS_LOG,
                                     0, 0};
    uint64_t expected[2];
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t gr[32] = {0};
        gpa_t base = T1H_TABLE + i * 0x8000;
        gr[9] = i + 1;
        gr[17] = gr[21] = base;
        gr[20] = CHECKED_SHARED_ITERATIONS;
        gr[23] = 1;
        gr[24] = 8;
        set_word(base, 100 + i);
        expected[i] = CHECKED_SHARED_ITERATIONS * (100 + i) +
                      gr[9] * CHECKED_SHARED_ITERATIONS * (CHECKED_SHARED_ITERATIONS - 1) / 2;
        reset(&ex[i], 0x10, 0, gr);
        ex[i].v->cfg.engine = sampled ? ENGINE_TIER1 : ENGINE_LOCKSTEP;
        t1_test_set_compile_options(ex[i].tv, &opt);
    }
    struct t1_compile_report report;
    T1H_CHECK(t1_compile(ex[1].tv, &opt, &report) == T1_COMPILED);
    t1_test_set_entry_hook(shared_checked_entry, NULL);
    t1_test_set_restore_observer(shared_checked_restore, NULL);
    t1_test_set_store_observer(shared_checked_store, NULL);
    struct t1_stats before = stats();
    start(&ex[1]);
    wait_for(&b_held, "first checked executor's host fault");
    start(&ex[0]);
    for (unsigned i = 0; i < 2; ++i) {
        finish(&ex[i]);
        T1H_CHECK(ex[i].v->s.gr[14] == expected[i] && ex[i].stores == CHECKED_SHARED_ITERATIONS);
    }
    struct t1_stats after = stats();
    uint64_t entries = after.entries - before.entries, checks = after.checked - before.checked;
    T1H_CHECK(after.divergences == before.divergences && checks > 0 &&
              checks + after.unchecked - before.unchecked == entries);
    T1H_CHECK(checks == (sampled ? (entries + 2) / 3 : entries));
    t1_test_set_restore_observer(NULL, NULL);
    t1_test_set_store_observer(NULL, NULL);
    t1_test_set_entry_hook(NULL, NULL);
}

static const uint64_t fp_memory_values[] = {UINT64_C(0x3fc00000), UINT64_C(0xc002000000000000),
                                            UINT64_C(0xfedcba9876543210)};
static void fp_memory_store(const struct t1_store_record *record, void *arg)
{
    (void)arg;
    uint64_t n = self->stores++;
    T1H_CHECK(n < 3 && record->seq == n && record->gva == T1H_OUT + n * 8 &&
              record->width == (n ? 8 : 4) && record->value == fp_memory_values[n]);
}
static void checked_fp(void)
{
    struct executor *b = &ex[1];
    uint64_t gr[32] = {0};
    gr[21] = T1H_TABLE;
    gr[22] = T1H_OUT;
    reset(b, 0x2b00, 0, gr);
    b->v->cfg.engine = ENGINE_LOCKSTEP;
    for (unsigned k = 0; k < 3; ++k) {
        set_word(T1H_TABLE + k * 8, fp_memory_values[k]);
        set_word(T1H_OUT + k * 8, UINT64_C(0x1122334455667788));
    }
    struct t1_compile_options opt = {
        T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG | T1_COMPILE_ACCESS_LOG, 0, 0};
    t1_test_set_compile_options(b->tv, &opt);
    struct t1_compile_report report;
    T1H_CHECK(t1_compile(b->tv, &opt, &report) == T1_COMPILED && report.sites == 6);
    t1_test_set_store_observer(fp_memory_store, NULL);
    struct t1_stats before = stats();
    start(b);
    finish(b);
    struct t1_stats after = stats();
    T1H_CHECK(after.divergences == before.divergences && after.checked == before.checked + 1 &&
              after.entries == before.entries + 1 && b->stores == 3);
    T1H_CHECK(ram_word(T1H_OUT) == UINT64_C(0x112233443fc00000) &&
              ram_word(T1H_OUT + 8) == fp_memory_values[1] &&
              ram_word(T1H_OUT + 16) == fp_memory_values[2]);
    T1H_CHECK(b->v->s.gr[21] == T1H_TABLE + 24 && b->v->s.gr[22] == T1H_OUT + 24);
    t1_test_set_store_observer(NULL, NULL);
}

static unsigned checked_phase, checked_release;
static uint64_t checked_stores;
static bool checked_walk_mode(void)
{
    return mode == CHECKED_WALK || mode == CHECKED_LONG || mode == CHECKED_LONG_BE;
}
static void checked_gate(struct t1_vcpu *tv, const struct vcpu *v,
                         const struct t1_restore_record *r, void *arg)
{
    (void)tv;
    (void)v;
    (void)arg;
    if (!r->forced || r->outcome != T1_OUTCOME_RETRY)
        return;
    unsigned phase = __atomic_add_fetch(&checked_phase, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&checked_release, __ATOMIC_ACQUIRE) < phase)
        sched_yield();
}
static void checked_store(const struct t1_store_record *r, void *arg)
{
    (void)arg;
    T1H_CHECK(r->gva == (checked_walk_mode() ? DATA_VA + 8 : RESULT_GPA) && r->width == 8 &&
              r->value == UINT64_C(0x8877665544332211));
    ++checked_stores;
}
static void peer_word(gpa_t pa, uint64_t value)
{
    T1H_CHECK(ia64_physical_write(ex[0].v, pa, 8, value).kind == IA64_STEP_COMPLETE);
}
static void checked(void)
{
    struct executor *b = &ex[1];
    uint64_t gr[32] = {0};
    bool walk = checked_walk_mode(), partial = mode == CHECKED_PARTIAL;
    bool long_format = mode == CHECKED_LONG || mode == CHECKED_LONG_BE;
    bool big = mode == CHECKED_LONG_BE;
    gpa_t first_entry = T1H_VHPT + vhpt_offset(DATA_VA);
    gr[21] = walk ? DATA_VA : FLAG_GPA;
    gr[22] = walk ? DATA_VA + 8 : RESULT_GPA;
    gr[23] = partial ? T1H_UNMAPPED : walk ? DATA_VA + 0x1010 : FLAG_GPA + 16;
    set_word(FLAG_GPA, UINT64_C(0x1020304050607080));
    set_word(RESULT_GPA, 0);
    set_word(FLAG_GPA + 16, 0);
    reset(b, 0x2800, walk ? IA64_PSR_DT : 0, gr);
    if (walk) {
        uint64_t rw = 1 | 0x20 | 0x40 | (UINT64_C(2) << 9);
        set_rr(b->v, 2, ((uint64_t)DATA_RID << 8) | (12u << 2) | 1);
        b->v->s.cr[CR_PTA] = T1H_VHPT | (15u << 2) | 1 | (long_format ? 0x100 : 0);
        b->v->s.cr[CR_DCR] = big ? 2 : 0;
        for (unsigned page = 0; page < 2; ++page) {
            uint64_t va = DATA_VA + page * 0x1000;
            gpa_t entry = long_format ? ia64_thash(&b->v->s, va) : T1H_VHPT + vhpt_offset(va);
            if (!page)
                first_entry = entry;
            uint64_t pte = FLAG_GPA | rw;
            set_word(entry, big ? __builtin_bswap64(pte) : pte);
            if (long_format) {
                uint64_t itir = 12u << 2, tag = ia64_ttag(&b->v->s, va);
                set_word(entry + 8, big ? __builtin_bswap64(itir) : itir);
                set_word(entry + 16, big ? __builtin_bswap64(tag) : tag);
            }
        }
        code_notify_vmm_write(T1H_VHPT, 0x8000);
        if (long_format)
            tr(b->v, 1, 0, T1H_VHPT, T1H_VHPT, 15, rw);
        else
            tr(b->v, 1, 0, (UINT64_C(2) << 61) | T1H_VHPT, T1H_VHPT, 12, rw);
    }
    b->v->cfg.engine = ENGINE_LOCKSTEP;
    struct t1_compile_options opt = {T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG |
                                         T1_COMPILE_UNDO_LOG | T1_COMPILE_ACCESS_LOG,
                                     0, 0};
    t1_test_set_compile_options(b->tv, &opt);
    struct t1_compile_report report;
    b->v->s.ip = T1H_CODE + 0x2a00;
    T1H_CHECK(t1_compile(b->tv, &opt, &report) == T1_REJECTED);
    T1H_CHECK(report.reject_reason &&
              !strcmp(report.reject_reason, "checked memory helper boundary"));
    b->v->s.ip = T1H_CODE + 0x2800;
    T1H_CHECK(t1_compile(b->tv, &opt, &report) == T1_COMPILED);
    unsigned armed = 0;
    for (unsigned i = 0; i < t1_point_count(report.region_id); ++i) {
        struct t1_point_info p;
        T1H_CHECK(t1_point_get(report.region_id, i, &p));
        if (p.kind != T1_PT_SITE || (p.ip != T1H_CODE + 0x2810 && p.ip != T1H_CODE + 0x2850))
            continue;
        T1H_CHECK(
            t1_test_force(b->tv, report.region_id, p.point_id, 1, T1_FORCE_TRAP_RETRY, NULL, NULL));
        ++armed;
    }
    T1H_CHECK(armed == 2);
    t1_test_set_restore_observer(checked_gate, NULL);
    t1_test_set_store_observer(checked_store, NULL);
    if (checked_mutation)
        t1_test_check_mutate(checked_mutation, checked_mutation == T1_MUTATE_STORE_VALUE ? 4 : 0);
    struct t1_stats before = stats();
    start(b);
    wait_for(&checked_phase, "first native load");
    peer_word(FLAG_GPA, UINT64_C(0x8877665544332211));
    __atomic_store_n(&checked_release, 1, __ATOMIC_RELEASE);
    uint64_t end = seconds() + WAIT_SECONDS;
    while (__atomic_load_n(&checked_phase, __ATOMIC_ACQUIRE) != 2) {
        T1H_CHECK(seconds() <= end);
        sched_yield();
    }
    T1H_CHECK(ram_word(RESULT_GPA) == UINT64_C(0x8877665544332211));
    peer_word(RESULT_GPA, UINT64_C(0xfedcba9876543210));
    peer_word(FLAG_GPA + 16, UINT64_C(0xabcdef0123456789));
    peer_word(FLAG_GPA + 24, 0x31415926);
    if (walk)
        peer_word(first_entry, 0);
    patch(0x2860, 0x2900);
    __atomic_store_n(&checked_release, 2, __ATOMIC_RELEASE);
    wait_for(&b->done, "checked executor end");
    struct t1_stats after = stats();
    T1H_CHECK(b->exit.reason ==
                  (partial && !checked_mutation ? VCPU_EXIT_BAD_ACCESS : VCPU_EXIT_DEBUG) &&
              after.checked == before.checked + (checked_mutation ? 0 : 1));
    if (checked_mutation) {
        T1H_CHECK(after.divergences == before.divergences + 1 && b->exit.detail &&
                  !strcmp(b->exit.detail, "tier1 lockstep divergence"));
    } else {
        T1H_CHECK(after.divergences == before.divergences &&
                  b->logs.last_break_imm == (partial ? 0 : T1H_BREAK_END));
        T1H_CHECK(b->v->s.gr[10] == 0x80 && b->v->s.gr[11] == 0x2211 &&
                  b->v->s.gr[12] == 0x44332211 && b->v->s.gr[13] == UINT64_C(0x8877665544332211) &&
                  b->v->s.gr[14] == (partial ? 0 : UINT64_C(0xabcdef0123456789)) &&
                  b->v->s.gr[15] == (partial ? 0 : 17));
        T1H_CHECK(ram_word(RESULT_GPA) == UINT64_C(0xfedcba9876543210) &&
                  ram_word(FLAG_GPA + 24) == 0x31415926 && checked_stores == 1);
    }
    t1_test_set_restore_observer(NULL, NULL);
    t1_test_set_store_observer(NULL, NULL);
}

/* ---- pin --------------------------------------------------------------- */

static unsigned pin_executing, pin_release;
static struct t1_region *pinned;
static void pin_gate(void)
{
    __atomic_store_n(&pin_executing, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&pin_release, __ATOMIC_ACQUIRE))
        sched_yield();
}
static void *pin_executor(void *arg)
{
    (void)arg;
    static struct t1_exec x;
    t1_set_current(&x);
    ((void (*)(void))(uintptr_t)pinned->code)();
    t1_set_current(NULL);
    return NULL;
}
static void pin_accounting(void)
{
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .index = 0};
    struct vcpu *own = vcpu_create(&cfg), *alias = vcpu_create(&cfg);
    cfg.index = 1;
    struct vcpu *other = vcpu_create(&cfg);
    T1H_CHECK(own && alias && other && !t1_current_count && !t1_current());
    code_register_vcpu(own);
    struct t1_exec x = {.v = own};
    t1_set_current(&x);
    T1H_CHECK(t1_current() == &x && !t1_current_count);
    t1_set_current(&x);
    T1H_CHECK(!t1_current_count);
    x.v = NULL;
    t1_set_current(NULL);
    T1H_CHECK(!t1_current_count && !t1_current());
    struct vcpu *unregistered[] = {other, alias, NULL};
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(unregistered); ++k) {
        x.v = unregistered[k];
        t1_set_current(&x);
        T1H_CHECK(t1_current() == &x && t1_current_count == 1);
        t1_set_current(&x);
        T1H_CHECK(t1_current_count == 1);
        /* Exit balances the pin taken at entry, even if the context's
         * vCPU pointer changes before it is cleared. No code runs here. */
        x.v = own;
        t1_set_current(NULL);
        T1H_CHECK(!t1_current_count && !t1_current());
        t1_set_current(NULL);
        T1H_CHECK(!t1_current_count);
    }
    code_unregister_vcpu(own);
    x.v = own;
    t1_set_current(&x);
    T1H_CHECK(t1_current_count == 1);
    t1_set_current(NULL);
    T1H_CHECK(!t1_current_count);
    vcpu_destroy(own);
    vcpu_destroy(alias);
    vcpu_destroy(other);
}

static void pin(void)
{
    T1H_CHECK(t1_init(NULL));
    pin_accounting();
    /* AMD64 SysV: align the stack, call the gate, return through the
     * region's code. */
    uint8_t bytes[] = {0x48, 0x83, 0xec, 8,    0x48, 0xb8, 0,    0,    0, 0,   0,
                       0,    0,    0,    0xff, 0xd0, 0x48, 0x83, 0xc4, 8, 0xc3};
    uintptr_t gate = (uintptr_t)pin_gate;
    memcpy(bytes + 6, &gate, sizeof(gate));
    struct t1_emitted e = {.buf = bytes, .size = sizeof(bytes)};
    pinned = t1_region_new();
    T1H_CHECK(pinned && t1_arena_place(pinned, &e));
    t1_arena_seal_for_entry(pinned);
    T1H_CHECK(t1_region_publish(pinned));
    uint32_t id = pinned->id;
    pthread_t thread;
    T1H_CHECK(!pthread_create(&thread, NULL, pin_executor, NULL));
    wait_for(&pin_executing, "region code on the executor");
    t1_flush();
    T1H_CHECK(t1_region_get(id) == pinned && pinned->retired);
    __atomic_store_n(&pin_release, 1, __ATOMIC_RELEASE);
    T1H_CHECK(!pthread_join(thread, NULL));
    t1_flush();
    T1H_CHECK(!t1_region_get(id));
    t1_shutdown();
}

/* ---- width ------------------------------------------------------------- */

static void width(void)
{
    T1H_CHECK(engine_tier1_configure("", CODE_VALIDITY_COMPARE_ALWAYS));
    const struct t1_engine_glue *g = engine_tier1_glue();
    struct vcpu *v[3];
    for (unsigned i = 0; i < 3; ++i) {
        struct vcpu_config cfg = {
            .engine = ENGINE_TIER1, .itc_hz = IA64VMM_ITC_HZ, .index = i, .lid = (uint64_t)i << 24};
        T1H_CHECK((v[i] = vcpu_create(&cfg)) != NULL);
    }
    /* The first instance takes 40 bits, or 36 where the host has no room
     * (a sanitizer's shadow); a later vCPU refused that width gets no
     * instance, not another width. */
    struct t1_vcpu *first = g->vcpu_create(v[0]);
    T1H_CHECK(first);
    unsigned w = hostwin_bits(first->hw);
    T1H_CHECK(w == 40 || w == 36);
    hostwin_linux_refuse_width(w);
    T1H_CHECK(!g->vcpu_create(v[1]));
    /* With no instance left the width is chosen again: 36 bits when 40 is
     * refused, then 36 for every later instance. */
    g->vcpu_destroy(first);
    hostwin_linux_refuse_width(40);
    first = g->vcpu_create(v[1]);
    T1H_CHECK(first && hostwin_bits(first->hw) == 36);
    hostwin_linux_refuse_width(0);
    struct t1_vcpu *later = g->vcpu_create(v[2]);
    T1H_CHECK(later && hostwin_bits(later->hw) == 36);
    g->vcpu_destroy(later);
    g->vcpu_destroy(first);
    engine_tier1_destroy();
    for (unsigned i = 0; i < 3; ++i)
        vcpu_destroy(v[i]);
}

void smp_tier1_probe_test(void);

int main(int argc, char **argv)
{
    alarm(120);
    T1H_CHECK(argc >= 3);
    unsigned m = 0;
    while (m < MODES && strcmp(argv[1], names[m]))
        ++m;
    T1H_CHECK(m < MODES);
    mode = (enum mode)m;
    const char *mutation = getenv("T1_CHECKED_MUTATION");
    if (mutation) {
        if (!strcmp(mutation, "load"))
            checked_mutation = T1_MUTATE_LOAD_VALUE;
        else if (!strcmp(mutation, "store"))
            checked_mutation = T1_MUTATE_STORE_VALUE;
        else if (!strcmp(mutation, "drop"))
            checked_mutation = T1_MUTATE_DROP_RECORD;
        else if (!strcmp(mutation, "gpa"))
            checked_mutation = T1_MUTATE_GPA;
        else if (!strcmp(mutation, "extra"))
            checked_mutation = T1_MUTATE_APPEND_RECORD;
        else
            T1H_CHECK(false);
    }
    /* Product windows: no MMU-log completeness check. */
    setenv("T1H_HOSTWIN_CHECK", "0", 1);
    T1H_CHECK(!hal_linux_configure(true, 3, 1, NULL, 0));
    T1H_CHECK(hostwin_linux_init());
    if (mode == PROBE) {
        smp_tier1_probe_test();
        printf("smp-tier1 probe: passed\n");
        return 0;
    }
    if (mode == PIN || mode == WIDTH) {
        if (mode == PIN)
            pin();
        else
            width();
        printf("smp-tier1 %s: passed\n", names[mode]);
        return 0;
    }
    t1h_set_images(argc - 2, argv + 2);
    t1h_init();
    t1h_load_image("smp_regions.bin");
    T1H_CHECK(vm_purge_configure("purge_panic_ms=5000"));
    T1H_CHECK(tier0_configure(TIER0_CODE_DEFAULT_BYTES));
    unsigned count = mode == SYNC2 || mode == PTCG ? 3 : 2;
    vm = vm_create(count, NULL, false);
    T1H_CHECK(vm);
    if (mode == SHARED)
        T1H_CHECK(engine_tier1_configure("tier1_hot=2", CODE_VALIDITY_COMPARE_ALWAYS));
    make(&ex[0], 0, mode == SHARED || mode == CHECKED_SHARED || mode == CHECKED_SAMPLED);
    make(&ex[1], 1, true);
    if (count == 3)
        make(&ex[2], 2, false);
    tier0_set_upper(engine_tier1_ops());
    /* Kicks, not slices, end the long loops: a region left only at its
     * checkpoint. kick measures against the default. */
    if (mode != SHARED)
        t1_cfg.wallclock_slice = mode == KICK ? TIER1_SLICE : BIG;
    t1_test_publish_hook = publish_hook;
    t1_test_chain_hook = chain_hook;
    t1_cfg.submit = helper_submit_t1;
    T1H_CHECK(helper_init(hal_helper_count()));
    code_background_set(helper_quiesce, helper_resume);
    switch (mode) {
    case CHECKED_FP:
        checked_fp();
        break;
    case CHECKED_SHARED:
    case CHECKED_SAMPLED:
        checked_shared();
        break;
    case CHECKED:
    case CHECKED_WALK:
    case CHECKED_PARTIAL:
    case CHECKED_LONG:
    case CHECKED_LONG_BE:
        checked();
        break;
    case SHARED:
        shared();
        break;
    case XMOD:
        xmod();
        break;
    case PAIR:
        pair();
        break;
    case KICK:
        kick_latency();
        break;
    case FLAG:
        flag();
        break;
    case SYNC2:
        sync2();
        break;
    case PARKED:
        parked();
        break;
    case PTCG:
        ptcg();
        break;
    case CHAINXMOD:
        chainxmod();
        break;
    case CHAINKICK:
        chainkick();
        break;
    case CHAINPURGE:
        chainpurge();
        break;
    default:
        purge();
        break;
    }
    helper_shutdown();
    code_background_set(NULL, NULL);
    t1_cfg.submit = NULL;
    printf("smp-tier1 %s: passed\n", names[mode]);
    return 0;
}
