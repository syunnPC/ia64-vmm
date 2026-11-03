/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Executors with threads and no guest: core/smp.c's executor loop, reset-hold, park bracket,
 * run lock, first-terminal exits, stop-all and management loop on the
 * threaded hosted HAL, with the real CPU model (vcpu_create, IRR, halt
 * wake test) and interrupt routes. engine_run is this file's (linked with
 * --wrap=engine_run): a per-vCPU script, so no guest code runs.
 *
 * Every case runs in a child process of its own (the HAL mode is
 * process-wide and HAL_CPU_STOP retires a host CPU for good) under an
 * alarm: a hang is a failure, not a stuck test. Arguments select cases.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "core/log_internal.h"
#include "core/smp.h"
#include "core/vm.h"
#include "engine/cache/validity.h"
#include "tests/hal/hal_linux.h"
#include "ia64/smp.h"
#include "ia64/state.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/interrupts.h"
#include "platform/platform.h"

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                        \
            _exit(1);                                                                              \
        }                                                                                          \
    } while (0)
#include "watchdog.h"

#define MS UINT64_C(1000000) /* hosted hal_counter() ticks are nanoseconds */
#define LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define WAIT_FOR(cond)                                                                             \
    do {                                                                                           \
        while (!(cond)) {                                                                          \
            __builtin_ia32_pause();                                                                \
            sched_yield();                                                                         \
        }                                                                                          \
        watchdog_progress();                                                                       \
    } while (0)

bool smp_entry_abi_test(void);

/* --- the engine script --------------------------------------------------- */

enum behaviour {
    B_RUN,  /* a full slice: VCPU_EXIT_BUDGET */
    B_HALT, /* IRR taken, then PAL_HALT_LIGHT: VCPU_EXIT_HALT */
    B_UNIMPLEMENTED,
    B_BAD_ACCESS,
    B_AFTER_STOP, /* VCPU_EXIT_BAD_ACCESS once vm.stop is set, B_RUN before */
    B_BARRIER,    /* wait for the barrier, then a terminal exit by index parity */
    B_SHUTDOWN,   /* VCPU_EXIT_SHUTDOWN: a reset request (the platform's reset_* say which) */
    B_REGION,     /* as a Tier0 region entry: no boundary until region_release, kicks or not */
};
struct mock {
    unsigned behaviour, armed, then; /* armed: switch to then */
    uint64_t calls;
    uint64_t first_irr0, first_irr3;     /* IRR when engine_run first ran */
    uint64_t busy_ns;                    /* busy time inside each engine_run */
    unsigned inside;                     /* inside engine_run now */
    uint64_t released, waits, permitted; /* deterministic lock-test handshakes */
    uint64_t short_slices, full_slices;  /* budgets VM_SERIAL_SLICE_BUDGET, VM_SLICE_BUDGET */
};
static struct mock mocks[IA64VMM_MAX_CPUS];
static unsigned barrier;
static unsigned inside, max_inside; /* executors inside engine_run at once */
static unsigned *lock_log, lock_log_cap, lock_log_len;
static unsigned controlled, idle_gates;
static uint64_t retry_us;
static unsigned retry_check, retry_timed[2], retry_infinite[2];
static unsigned deciding, decision_entered, decision_release;
static unsigned stop_started, stop_done;
/* sync.i cases: B_REGION's entry and release; a B_RUN executor with a pending
 * instruction executes it inside its slice, after go (sync.i: ia64_smp_sync_i;
 * purge: ptc.g of one page). */
enum instruction { P_NONE, P_SYNC, P_PURGE };
static unsigned region_entered, region_release, go;
static unsigned pending[IA64VMM_MAX_CPUS], ready[IA64VMM_MAX_CPUS], returned[IA64VMM_MAX_CPUS];
static unsigned saw_release[IA64VMM_MAX_CPUS];
bool __real_platform_reset_restarts(const struct platform_state *s);
bool __wrap_platform_reset_restarts(const struct platform_state *s);
bool __wrap_platform_reset_restarts(const struct platform_state *s)
{
    if (LOAD(&deciding) && LOAD(&vm_current()->first_terminal)) {
        STORE(&decision_entered, 1);
        WAIT_FOR(LOAD(&decision_release));
    }
    return __real_platform_reset_restarts(s);
}

static unsigned critical_ns; /* busy time inside engine_run */

static struct vcpu_exit exit_of(const struct vcpu *v, enum vcpu_exit_reason reason,
                                const char *detail)
{
    return (struct vcpu_exit){reason, v->s.ip, v->s.slot, 0, detail};
}
struct vcpu_exit __wrap_engine_run(struct vcpu *v, uint64_t budget);
struct vcpu_exit __wrap_engine_run(struct vcpu *v, uint64_t budget)
{
    CHECK(budget);
    /* No executor runs its engine parked. */
    CHECK(!LOAD(&v->parked));
    unsigned i = v->cfg.index;
    struct mock *m = &mocks[i];
    STORE(&m->inside, 1);
    if (budget == VM_SERIAL_SLICE_BUDGET)
        __atomic_fetch_add(&m->short_slices, 1, __ATOMIC_ACQ_REL);
    if (budget == VM_SLICE_BUDGET)
        __atomic_fetch_add(&m->full_slices, 1, __ATOMIC_ACQ_REL);
    /* The IRR snapshot is published before the first call is counted:
     * the test reads it once it sees calls != 0 (only this executor counts). */
    if (!LOAD(&m->calls)) {
        STORE(&m->first_irr0, __atomic_load_n(&v->s.cr[CR_IRR0], __ATOMIC_ACQUIRE));
        STORE(&m->first_irr3, __atomic_load_n(&v->s.cr[CR_IRR0 + 3], __ATOMIC_ACQUIRE));
    }
    __atomic_fetch_add(&m->calls, 1, __ATOMIC_ACQ_REL);
    unsigned now = __atomic_add_fetch(&inside, 1, __ATOMIC_SEQ_CST), max = LOAD(&max_inside);
    while (now > max && !__atomic_compare_exchange_n(&max_inside, &max, now, false,
                                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    }
    if (lock_log) {
        unsigned at = __atomic_fetch_add(&lock_log_len, 1, __ATOMIC_ACQ_REL);
        if (at < lock_log_cap)
            lock_log[at] = i;
    }
    /* Hold the real run lock until management has observed the waiter. */
    if (LOAD(&controlled)) {
        while (LOAD(&controlled) && LOAD(&m->released) < LOAD(&m->calls)) {
            sched_yield();
        }
    }
    uint64_t busy = LOAD(&m->busy_ns) ? LOAD(&m->busy_ns) : critical_ns;
    if (busy) {
        uint64_t until = hal_counter() + busy;
        while (hal_counter() < until)
            __builtin_ia32_pause();
    }
    __atomic_sub_fetch(&inside, 1, __ATOMIC_SEQ_CST);
    STORE(&m->inside, 0);
    /* As the engines do: a requested exit first, then a kick. */
    unsigned requested = __atomic_exchange_n(&v->requested_exit, 0, __ATOMIC_ACQ_REL);
    if (requested)
        return exit_of(v, (enum vcpu_exit_reason)(requested - 1), "requested exit");
    if (__atomic_exchange_n(&v->irq_kick, 0, __ATOMIC_ACQ_REL))
        return exit_of(v, VCPU_EXIT_BUDGET, "kick");
    unsigned b = LOAD(&m->armed) ? LOAD(&m->then) : LOAD(&m->behaviour);
    switch (b) {
    case B_HALT:
        /* As a guest's idle loop: take what is pending (cr.ivr), then halt. */
        for (unsigned k = 0; k < 4; ++k)
            __atomic_store_n(&v->s.cr[CR_IRR0 + k], 0, __ATOMIC_RELEASE);
        return exit_of(v, VCPU_EXIT_HALT, "halt");
    case B_UNIMPLEMENTED:
        return exit_of(v, VCPU_EXIT_UNIMPLEMENTED, "script");
    case B_BAD_ACCESS:
        return exit_of(v, VCPU_EXIT_BAD_ACCESS, "script");
    case B_SHUTDOWN:
        return exit_of(v, VCPU_EXIT_SHUTDOWN, "script");
    case B_AFTER_STOP:
        if (LOAD(&vm_current()->stop))
            return exit_of(v, VCPU_EXIT_BAD_ACCESS, "after stop");
        return exit_of(v, VCPU_EXIT_BUDGET, NULL);
    case B_BARRIER:
        while (!LOAD(&barrier))
            __builtin_ia32_pause();
        return exit_of(v, i & 1 ? VCPU_EXIT_BAD_ACCESS : VCPU_EXIT_UNIMPLEMENTED, "barrier");
    case B_REGION:
        STORE(&region_entered, 1);
        WAIT_FOR(LOAD(&region_release));
        STORE(&m->behaviour, B_RUN);
        return exit_of(v, VCPU_EXIT_BUDGET, NULL);
    default:
        break;
    }
    unsigned instruction = __atomic_exchange_n(&pending[i], P_NONE, __ATOMIC_ACQ_REL);
    if (instruction != P_NONE) {
        STORE(&ready[i], 1);
        WAIT_FOR(LOAD(&go));
        if (instruction == P_SYNC)
            ia64_smp_sync_i(v);
        else
            ia64_smp_global_purge(v, 0x100, 0x40000, 12, false);
        STORE(&saw_release[i], LOAD(&region_release));
        STORE(&returned[i], 1);
    }
    return exit_of(v, VCPU_EXIT_BUDGET, NULL);
}
/* A running executor of the controlled cases, which never halt, idles
 * indefinitely only after taking a waiting ticket (reset-hold idles have
 * their own state). Preserve the real HAL wait and wake; delay its return
 * so the releaser can queue its next ticket before the waiter acquires the
 * lock. The halt-retry check counts the parked idles of executors that
 * ran, in a VM without the run lock. */
void __real_hal_idle_until(uint64_t deadline);
void __wrap_hal_idle_until(uint64_t deadline);
void __wrap_hal_idle_until(uint64_t deadline)
{
    struct vm *vm = vm_current();
    unsigned cpu = hal_cpu_index();
    if (LOAD(&retry_check) && vm && cpu >= 1 && cpu <= 2 && LOAD(&mocks[cpu - 1].calls) &&
        LOAD(&vm->cpu[cpu - 1].v->parked)) {
        if (deadline != UINT64_MAX) {
            uint64_t now = hal_counter();
            CHECK(deadline <= now || deadline - now <= retry_us * 1000);
        }
        unsigned *count = deadline == UINT64_MAX ? retry_infinite : retry_timed;
        __atomic_fetch_add(&count[cpu - 1], 1, __ATOMIC_RELEASE);
    }
    struct mock *m = NULL;
    uint64_t event = 0;
    if (LOAD(&controlled) && vm && cpu >= 1 && cpu <= vm->count && deadline == UINT64_MAX &&
        LOAD(&vm->cpu[cpu - 1].state) == VM_CPU_RUNNING) {
        m = &mocks[cpu - 1];
        event = __atomic_add_fetch(&m->waits, 1, __ATOMIC_ACQ_REL);
    }
    __real_hal_idle_until(deadline);
    if (m && LOAD(&idle_gates)) {
        while (LOAD(&controlled) && LOAD(&m->permitted) < event) {
            sched_yield();
        }
    }
}

static void script(unsigned i, unsigned behaviour, unsigned then)
{
    mocks[i] = (struct mock){.behaviour = behaviour, .then = then};
}

/* --- VM fixture ------------------------------------------------------- */

static struct platform_state platform;
static const struct vm_exec_ops no_ops;
/* First-terminal checks: what each executor's stopped hook, which runs
 * after its own CAS, saw in first_terminal. */
static unsigned seen_first[IA64VMM_MAX_CPUS];
static void record_first(void *ctx, struct vm_cpu *c)
{
    (void)ctx;
    STORE(&seen_first[c->index], LOAD(&vm_current()->first_terminal));
}
static const struct vm_exec_ops first_ops = {.stopped = record_first};
static struct vm_exec exec_now(void)
{
    return (struct vm_exec){.ops = &no_ops, .start = hal_counter(), .hz = hal_counter_hz()};
}
static struct vcpu *new_vcpu(unsigned i, bool icount)
{
    struct vcpu_config cfg = {.index = i,
                              .lid = IA64VMM_CPU_LID(i),
                              .engine = ENGINE_INTERP,
                              .itc_hz = IA64VMM_ITC_HZ,
                              .itc_icount = icount};
    return vcpu_create(&cfg);
}
/* vCPU i on host CPU first_cpu + i, every route bound (as core/vm.c). */
static struct vm *new_vm(unsigned count, bool serialize, unsigned first_cpu)
{
    struct vm *vm = vm_create(count, &platform, serialize);
    CHECK(vm);
    for (unsigned i = 0; i < count; ++i) {
        struct vcpu *v = new_vcpu(i, false);
        CHECK(v);
        vm->cpu[i].v = v;
        vm->cpu[i].host_cpu = first_cpu + i;
        CHECK(platform_irq_bind(v, i, IA64VMM_CPU_LID(i), first_cpu + i));
    }
    return vm;
}
static void free_vm(struct vm *vm)
{
    for (unsigned i = 0; i < vm->count; ++i)
        platform_irq_unbind(i);
    for (unsigned i = 0; i < vm->count; ++i)
        vcpu_destroy(vm->cpu[i].v);
    vm_destroy(vm);
    CHECK(!vm_current());
}
static unsigned state(struct vm *vm, unsigned i)
{
    return LOAD(&vm->cpu[i].state);
}
static void all_stopped(struct vm *vm)
{
    for (unsigned i = 0; i < vm->count; ++i)
        CHECK(state(vm, i) == VM_CPU_STOPPED && !hal_cpu_busy(vm->cpu[i].host_cpu));
}
static void wake(unsigned i, unsigned vector, unsigned mode)
{
    CHECK(platform_irq_inject(i, vector, mode));
}

/* The log ring since `from` contains text. */
static bool logged(uint64_t from, const char *text)
{
    const struct log_ring *ring = log_get_ring();
    uint64_t end = ring->write_pos, n = end - from;
    if (n > LOG_RING_SIZE)
        return false;
    char *copy = malloc(n + 1);
    CHECK(copy);
    for (uint64_t k = 0; k < n; ++k)
        copy[k] = ring->data[(from + k) % LOG_RING_SIZE];
    copy[n] = 0;
    bool found = strstr(copy, text) != NULL;
    free(copy);
    return found;
}
static uint64_t log_now(void)
{
    return log_get_ring()->write_pos;
}

/* --- cases ------------------------------------------------------------- */

/* Stop-all with executors running, held, idle and already terminal: vCPU 3's
 * terminal exit is the first and ends the VM; management stops the others;
 * the held AP never ran a slice. */
static void case_states(void)
{
    uint64_t from = log_now();
    struct vm *vm = new_vm(5, false, 1);
    script(0, B_RUN, B_RUN);
    script(2, B_HALT, B_HALT);
    script(3, B_RUN, B_UNIMPLEMENTED);
    script(4, B_RUN, B_RUN);
    struct vm_exec e = exec_now();
    CHECK(vm_start(vm, &e));
    WAIT_FOR(state(vm, 0) == VM_CPU_RUNNING);
    for (unsigned i = 1; i < 5; ++i)
        WAIT_FOR(state(vm, i) == VM_CPU_RESET_HOLD);
    wake(2, 0xff, IA64_IRQ_INT);
    wake(3, 0xff, IA64_IRQ_INT);
    wake(4, 0xff, IA64_IRQ_INT);
    WAIT_FOR(LOAD(&mocks[0].calls) > 10 && LOAD(&mocks[3].calls) > 10 &&
             LOAD(&mocks[4].calls) > 10);
    /* vCPU 2 idles parked; vCPU 1 is held, parked, and ran nothing. */
    WAIT_FOR(LOAD(&mocks[2].calls) && LOAD(&vm->cpu[2].v->parked));
    CHECK(state(vm, 1) == VM_CPU_RESET_HOLD && LOAD(&vm->cpu[1].v->parked) &&
          !LOAD(&mocks[1].calls));
    STORE(&mocks[3].armed, 1);
    CHECK(vm_wait(vm, UINT64_MAX) == VMM_EXIT_UNIMPLEMENTED);
    all_stopped(vm);
    CHECK(vm->first_terminal == 4 && vm->cpu[3].code == VMM_EXIT_UNIMPLEMENTED &&
          vm->cpu[3].exit.reason == VCPU_EXIT_UNIMPLEMENTED);
    for (unsigned i = 0; i < 5; ++i)
        if (i != 3)
            CHECK(vm->cpu[i].code == VMM_EXIT_TIMEOUT);
    CHECK(!LOAD(&mocks[1].calls) && !vm->cpu[2].v->parked);
    CHECK(logged(from, "vCPU0 stopped: vCPU3 ended the VM") &&
          logged(from, "vCPU1 stopped: vCPU3 ended the VM") &&
          logged(from, "vCPU2 stopped: vCPU3 ended the VM") &&
          logged(from, "vCPU3 guest exit reason=3"));
    free_vm(vm);
}

/* --- sync.i and the lower tier ----------------------------- */

/* count Tier0 executors, every AP released from reset-hold and running. */
static struct vm *sync_vm(unsigned count, enum engine_kind engine)
{
    struct vm *vm = new_vm(count, false, 1);
    for (unsigned i = 0; i < count; ++i) {
        script(i, B_RUN, B_UNIMPLEMENTED);
        vm->cpu[i].v->cfg.engine = engine;
    }
    struct vm_exec e = exec_now();
    CHECK(vm_start(vm, &e));
    for (unsigned i = 1; i < count; ++i) {
        WAIT_FOR(state(vm, i) == VM_CPU_RESET_HOLD);
        wake(i, 0xff, IA64_IRQ_INT);
    }
    for (unsigned i = 0; i < count; ++i)
        WAIT_FOR(LOAD(&mocks[i].calls) > 2);
    return vm;
}
static void sync_end(struct vm *vm)
{
    STORE(&region_release, 1);
    STORE(&mocks[0].armed, 1);
    CHECK(vm_wait(vm, UINT64_MAX) == VMM_EXIT_UNIMPLEMENTED);
    free_vm(vm);
}
/* B (vCPU 1) inside a region whose entry precedes a retirement. */
static void enter_region(void)
{
    STORE(&mocks[1].behaviour, B_REGION);
    WAIT_FOR(LOAD(&region_entered));
}
static void retire(void)
{
    code_lock();
    code_retire_epoch();
    code_unlock();
}
/* The executors in `who` take their pending instruction and stop at go. */
static void prepare(const unsigned *who, unsigned n, enum instruction what)
{
    for (unsigned k = 0; k < n; ++k)
        STORE(&pending[who[k]], what);
    for (unsigned k = 0; k < n; ++k)
        WAIT_FOR(LOAD(&ready[who[k]]));
}
/* None of `who` returns within 50 ms. */
static void still_waiting(const unsigned *who, unsigned n)
{
    uint64_t until = hal_counter() + 50 * MS;
    while (hal_counter() < until) {
        for (unsigned k = 0; k < n; ++k)
            CHECK(!LOAD(&returned[who[k]]));
        sched_yield();
    }
}

/* A's sync.i waits for B, which entered a region before A's store retired
 * it, kicks B, and returns only after B left; C passes boundaries. */
static void case_sync_region(void)
{
    struct vm *vm = sync_vm(3, ENGINE_TIER0);
    enter_region();
    const unsigned a[] = {0};
    prepare(a, 1, P_SYNC);
    retire();
    STORE(&go, 1);
    still_waiting(a, 1);
    CHECK(LOAD(&vm->cpu[1].v->irq_kick));
    STORE(&region_release, 1);
    WAIT_FOR(LOAD(&returned[0]));
    CHECK(LOAD(&saw_release[0]));
    sync_end(vm);
}
/* B entered its region after the retirement and C parked: A's sync.i
 * returns while B is still inside. */
static void case_sync_quiet(void)
{
    struct vm *vm = sync_vm(3, ENGINE_TIER0);
    retire();
    STORE(&mocks[2].behaviour, B_HALT);
    WAIT_FOR(LOAD(&vm->cpu[2].v->parked));
    /* B's slice that enters the region begins at a boundary after it. */
    uint64_t epoch = code_epoch_sample();
    WAIT_FOR(__atomic_load_n(&vm->cpu[1].v->code_quiescent, __ATOMIC_SEQ_CST) >= epoch);
    enter_region();
    const unsigned a[] = {0};
    prepare(a, 1, P_SYNC);
    STORE(&go, 1);
    WAIT_FOR(LOAD(&returned[0]));
    CHECK(!LOAD(&saw_release[0]));
    sync_end(vm);
}
/* The interpreter has no region to wait for, even one retired after B's
 * entry. */
static void case_sync_interp(void)
{
    struct vm *vm = sync_vm(2, ENGINE_INTERP);
    enter_region();
    const unsigned a[] = {0};
    prepare(a, 1, P_SYNC);
    retire();
    STORE(&go, 1);
    WAIT_FOR(LOAD(&returned[0]));
    CHECK(!LOAD(&saw_release[0]));
    sync_end(vm);
}
/* A and C execute sync.i at once, both inside the slice that ends with it,
 * while B runs a retired region: both wait for B, neither for the other. */
static void case_sync_pair(void)
{
    struct vm *vm = sync_vm(3, ENGINE_TIER0);
    enter_region();
    const unsigned ac[] = {0, 2};
    prepare(ac, 2, P_SYNC);
    retire();
    STORE(&go, 1);
    still_waiting(ac, 2);
    STORE(&region_release, 1);
    WAIT_FOR(LOAD(&returned[0]) && LOAD(&returned[2]));
    CHECK(LOAD(&saw_release[0]) && LOAD(&saw_release[2]));
    sync_end(vm);
}
/* C's ptc.g waits for A's acknowledgement while A's sync.i waits for C:
 * the waiter acknowledges, the purge completes and so does the sync.i. */
static void case_sync_purge(void)
{
    struct vm *vm = sync_vm(2, ENGINE_TIER0);
    const unsigned a[] = {0}, c[] = {1};
    prepare(a, 1, P_SYNC);
    prepare(c, 1, P_PURGE);
    retire();
    STORE(&go, 1);
    WAIT_FOR(LOAD(&returned[0]) && LOAD(&returned[1]));
    sync_end(vm);
}

/* The watchdog stops running, held and idle executors; each reports where it
 * stood (the single executor's lines, naming the vCPU). */
static void case_watchdog(void)
{
    uint64_t from = log_now();
    struct vm *vm = new_vm(3, false, 1);
    script(0, B_RUN, B_RUN);
    script(2, B_HALT, B_HALT);
    struct vm_exec e = exec_now();
    CHECK(vm_start(vm, &e));
    WAIT_FOR(state(vm, 2) == VM_CPU_RESET_HOLD);
    wake(2, 0xff, IA64_IRQ_INT);
    WAIT_FOR(LOAD(&mocks[2].calls) && LOAD(&vm->cpu[2].v->parked) && LOAD(&mocks[0].calls) > 10);
    CHECK(vm_wait(vm, hal_counter() + 50 * MS) == VMM_EXIT_TIMEOUT);
    all_stopped(vm);
    CHECK(logged(from, "vCPU1 stopped by management in reset-hold"));
    CHECK(logged(from, "vCPU0 guest stopped at an engine boundary by management") ||
          logged(from, "vCPU0 management watchdog stopped guest at ip="));
    CHECK(logged(from, "vCPU2 guest stopped at an engine boundary by management") ||
          logged(from, "vCPU2 management watchdog stopped guest at ip="));
    CHECK(!logged(from, "ended the VM"));
    free_vm(vm);
}

/* Deterministic stop-all before any executor took its work
 * sends HAL_CPU_WAKE, not HAL_CPU_STOP (a stopped CPU refuses the start),
 * and each executor sees vm.stop before its first slice. */
static void noop(void *arg)
{
    (void)arg;
}
static void case_unstarted(void)
{
    struct vm *vm = new_vm(3, false, 1);
    for (unsigned i = 0; i < 3; ++i)
        script(i, B_RUN, B_RUN);
    for (unsigned i = 0; i < 3; ++i)
        CHECK(state(vm, i) == VM_CPU_UNSTARTED);
    vm_stop_all(vm, false);
    struct vm_exec e = exec_now();
    CHECK(vm_start(vm, &e));
    /* Observe the executors' own vm.stop check before vm_wait can send
     * another STOP and hide an unwanted first slice. */
    for (unsigned i = 0; i < 3; ++i)
        WAIT_FOR(state(vm, i) == VM_CPU_STOPPED);
    CHECK(vm_wait(vm, UINT64_MAX) == VMM_EXIT_TIMEOUT);
    all_stopped(vm);
    for (unsigned i = 0; i < 3; ++i)
        CHECK(!LOAD(&mocks[i].calls));
    free_vm(vm);
}

/* Stop-all race, RACE_ITERATIONS times over all children: stop-all
 * right after the start, a random 0-80 us later. Either the executor had
 * not taken its work (HAL_CPU_WAKE; its CPU takes new work afterwards) or it
 * had (HAL_CPU_STOP; the CPU is retired). Every iteration must end; both
 * outcomes must occur for the race to count. */
#define RACE_ITERATIONS 10000u
struct race_totals {
    uint64_t iterations, woken, stopped;
};
static struct race_totals *race;
static void case_race(void)
{
    log_set_level(LOG_ERROR); /* each stopped executor dumps its vCPU */
    struct vcpu *v = new_vcpu(0, false);
    CHECK(v);
    unsigned seed = (unsigned)getpid();
    for (unsigned cpu = 1; cpu < 256 && race->iterations < RACE_ITERATIONS;) {
        struct vm *vm = vm_create(1, &platform, false);
        CHECK(vm);
        vm->cpu[0].v = v;
        vm->cpu[0].host_cpu = cpu;
        STORE(&v->requested_exit, 0);
        CHECK(platform_irq_bind(v, 0, 0, cpu));
        script(0, B_RUN, B_RUN);
        struct vm_exec e = exec_now();
        CHECK(vm_start(vm, &e));
        uint64_t until = hal_counter() + (uint64_t)(rand_r(&seed) % 80000);
        while (hal_counter() < until)
            __builtin_ia32_pause();
        vm_stop_all(vm, false);
        CHECK(vm_wait(vm, UINT64_MAX) == VMM_EXIT_TIMEOUT);
        all_stopped(vm);
        platform_irq_unbind(0);
        vm_destroy(vm);
        if (hal_cpu_start(cpu, noop, NULL)) {
            ++race->woken;
            WAIT_FOR(!hal_cpu_busy(cpu));
        } else {
            ++race->stopped;
            ++cpu;
        }
        ++race->iterations;
        watchdog_round = (sig_atomic_t)race->iterations;
        watchdog_progress();
    }
}

/* The first terminal exit decides the restart. The
 * watchdog firing while a restart stops the VM sends no HAL_CPU_STOP (both
 * host CPUs take new work afterwards); a reset request that arrives after a
 * shutdown exit does not turn it into a restart (the running AP's CPU is
 * retired). AP 1 is inside a 200 ms slice when management stops the VM. */
static void case_restart_decision(void)
{
    for (unsigned shutdown_first = 0; shutdown_first < 2; ++shutdown_first) {
        struct vm *vm = new_vm(2, false, 1 + shutdown_first * 2);
        platform.panic = false;
        platform.reset_requested = true;
        platform.reset_type = shutdown_first ? 2 : 0;
        platform.reset_status = 0;
        script(0, B_RUN, B_SHUTDOWN);
        script(1, B_RUN, B_RUN);
        struct vm_exec e = exec_now();
        CHECK(vm_start(vm, &e));
        WAIT_FOR(state(vm, 1) == VM_CPU_RESET_HOLD);
        wake(1, 0xff, IA64_IRQ_INT);
        WAIT_FOR(LOAD(&mocks[1].calls) && LOAD(&mocks[0].calls));
        STORE(&mocks[1].busy_ns, 200 * MS);
        WAIT_FOR(LOAD(&mocks[1].inside));
        STORE(&mocks[0].armed, 1);
        WAIT_FOR(state(vm, 0) == VM_CPU_STOPPED);
        /* A later CF9h-style request after a shutdown exit. */
        if (shutdown_first)
            platform.reset_type = 0;
        int code = vm_wait(vm, hal_counter());
        all_stopped(vm);
        CHECK(vm->first_terminal == 1 && vm_restart_decided(vm) == !shutdown_first);
        CHECK(code == VMM_EXIT_SUCCESS);
        CHECK(hal_cpu_start(vm->cpu[0].host_cpu, noop, NULL));
        WAIT_FOR(!hal_cpu_busy(vm->cpu[0].host_cpu));
        bool started = hal_cpu_start(vm->cpu[1].host_cpu, noop, NULL);
        CHECK(started == !shutdown_first);
        if (started)
            WAIT_FOR(!hal_cpu_busy(vm->cpu[1].host_cpu));
        STORE(&mocks[1].busy_ns, 0);
        STORE(&mocks[0].armed, 0);
        free_vm(vm);
    }
}

/* Stop-all must wait for an in-progress terminal decision: otherwise it
 * can retire an executor CPU before the reset decision is published. */
static void *stop_during_decision(void *arg)
{
    STORE(&stop_started, 1);
    vm_stop_all(arg, true);
    STORE(&stop_done, 1);
    return NULL;
}
static void case_restart_publication(void)
{
    struct vm *vm = new_vm(2, false, 1);
    platform.reset_requested = true;
    platform.reset_type = 0;
    script(0, B_RUN, B_SHUTDOWN);
    script(1, B_RUN, B_RUN);
    struct vm_exec e = exec_now();
    CHECK(vm_start(vm, &e));
    WAIT_FOR(state(vm, 1) == VM_CPU_RESET_HOLD);
    wake(1, 0xff, IA64_IRQ_INT);
    WAIT_FOR(LOAD(&mocks[1].calls));
    STORE(&deciding, 1);
    STORE(&mocks[0].armed, 1);
    WAIT_FOR(LOAD(&decision_entered));
    pthread_t stopper;
    CHECK(!pthread_create(&stopper, NULL, stop_during_decision, vm));
    WAIT_FOR(LOAD(&stop_started));
    /* Give management time to attempt stop-all while the terminal
     * decision is suspended. No irreversible stop may escape it. */
    usleep(20000);
    CHECK(!LOAD(&stop_done));
    STORE(&decision_release, 1);
    CHECK(!pthread_join(stopper, NULL));
    CHECK(vm_wait(vm, UINT64_MAX) == VMM_EXIT_SUCCESS);
    CHECK(vm_restart_decided(vm));
    all_stopped(vm);
    for (unsigned i = 0; i < vm->count; ++i) {
        CHECK(hal_cpu_start(vm->cpu[i].host_cpu, noop, NULL));
        WAIT_FOR(!hal_cpu_busy(vm->cpu[i].host_cpu));
    }
    free_vm(vm);
}

/* First terminal executor wins (CAS): vCPU 0 exits first; AP 1 returns a
 * terminal exit only after it saw vm.stop, i.e. after vCPU 0's CAS. */
static void case_cas_order(void)
{
    struct vm *vm = new_vm(2, false, 1);
    script(0, B_RUN, B_UNIMPLEMENTED);
    script(1, B_AFTER_STOP, B_AFTER_STOP);
    struct vm_exec e = exec_now();
    CHECK(vm_start(vm, &e));
    WAIT_FOR(state(vm, 1) == VM_CPU_RESET_HOLD);
    wake(1, 0xff, IA64_IRQ_INT);
    WAIT_FOR(LOAD(&mocks[1].calls) > 10);
    STORE(&mocks[0].armed, 1);
    CHECK(vm_wait(vm, UINT64_MAX) == VMM_EXIT_UNIMPLEMENTED);
    all_stopped(vm);
    CHECK(vm->first_terminal == 1 && vm->cpu[1].code == VMM_EXIT_TIMEOUT);
    free_vm(vm);
}

/* Simultaneous terminal exits of 8 executors, 28 times on fresh host CPUs,
 * and a ninth that leaves only after it saw vm.stop: exactly one winner,
 * which left on its own terminal exit, the VM's code is its code, and
 * first_terminal never changed after it was set. */
static void case_cas_simultaneous(void)
{
    enum { N = 9 };
    for (unsigned rep = 0; rep < 28; ++rep) {
        struct vm *vm = new_vm(N, false, 1 + rep * N);
        STORE(&barrier, 0);
        for (unsigned i = 0; i < N - 1; ++i)
            script(i, B_BARRIER, B_BARRIER);
        script(N - 1, B_AFTER_STOP, B_AFTER_STOP);
        struct vm_exec e = exec_now();
        e.ops = &first_ops;
        CHECK(vm_start(vm, &e));
        for (unsigned i = 1; i < N; ++i) {
            WAIT_FOR(state(vm, i) == VM_CPU_RESET_HOLD);
            wake(i, 0xff, IA64_IRQ_INT);
        }
        for (unsigned i = 0; i < N; ++i)
            WAIT_FOR(LOAD(&mocks[i].calls));
        STORE(&barrier, 1);
        int code = vm_wait(vm, UINT64_MAX);
        all_stopped(vm);
        unsigned first = vm->first_terminal;
        CHECK(first >= 1 && first < N);
        const struct vm_cpu *w = &vm->cpu[first - 1];
        CHECK(code == w->code && code != VMM_EXIT_TIMEOUT);
        CHECK(code == ((first - 1) & 1 ? VMM_EXIT_BAD_ACCESS : VMM_EXIT_UNIMPLEMENTED));
        CHECK(vm->cpu[N - 1].code == VMM_EXIT_TIMEOUT);
        /* Once set, first_terminal never changes: every executor saw it. */
        for (unsigned i = 0; i < N; ++i)
            CHECK(LOAD(&seen_first[i]) == first);
        free_vm(vm);
    }
}

/* smp_serialize: never two executors inside engine_run; the
 * ticket lock serves in order, so no executor starves over 1e5 slices. */
#define LOCK_SLICES 100000u
static void case_run_lock(void)
{
    enum { N = 4 };
    lock_log_cap = LOCK_SLICES + 4096;
    lock_log = calloc(lock_log_cap, sizeof(*lock_log));
    CHECK(lock_log);
    critical_ns = 1000;
    struct vm *vm = new_vm(N, true, 1);
    for (unsigned i = 0; i < N; ++i)
        script(i, B_RUN, B_UNIMPLEMENTED);
    struct vm_exec e = exec_now();
    CHECK(vm_start(vm, &e));
    for (unsigned i = 1; i < N; ++i) {
        WAIT_FOR(state(vm, i) == VM_CPU_RESET_HOLD);
        wake(i, 0xff, IA64_IRQ_INT);
    }
    unsigned progress = LOAD(&lock_log_len);
    while (progress < LOCK_SLICES) {
        unsigned current = LOAD(&lock_log_len);
        if (current != progress) {
            progress = current;
            watchdog_progress();
        }
        sched_yield();
    }
    STORE(&mocks[0].armed, 1);
    CHECK(vm_wait(vm, UINT64_MAX) == VMM_EXIT_UNIMPLEMENTED);
    all_stopped(vm);
    CHECK(LOAD(&max_inside) == 1);
    /* From the first slice at which all N had started: shares and the
     * longest run of other executors' slices between two of one's own. */
    unsigned len = lock_log_len < lock_log_cap ? lock_log_len : lock_log_cap, begin = 0;
    for (unsigned seen = 0; begin < len && seen != (1u << N) - 1; ++begin)
        seen |= 1u << lock_log[begin];
    unsigned count[N] = {0}, last[N], gap = 0;
    for (unsigned i = 0; i < N; ++i)
        last[i] = begin;
    for (unsigned k = begin; k < len; ++k) {
        unsigned i = lock_log[k];
        if (k - last[i] > gap)
            gap = k - last[i];
        last[i] = k + 1;
        ++count[i];
    }
    fprintf(stderr, "run lock: %u slices, shares %u %u %u %u, longest wait %u slices\n",
            len - begin, count[0], count[1], count[2], count[3], gap);
    /* Host preemption before taking the next ticket can lengthen the
     * slice gap. Shares check starvation; the alarm checks lost wakes. */
    for (unsigned i = 0; i < N; ++i)
        CHECK(count[i] >= (len - begin) / N / 2);
    free_vm(vm);
}

/* smp_serialize: a waiter for the run lock drains an interrupt for its
 * vCPU while it waits, outside the lock, and consumes the kick with it:
 * the slice it gets then runs guest code at once. */
static void case_lock_drain(void)
{
    struct vm *vm = new_vm(2, true, 1);
    script(0, B_RUN, B_RUN);
    script(1, B_RUN, B_RUN);
    STORE(&controlled, 1);
    struct vm_exec e = exec_now();
    CHECK(vm_start(vm, &e));
    WAIT_FOR(state(vm, 1) == VM_CPU_RESET_HOLD);
    WAIT_FOR(LOAD(&mocks[0].calls) == 1);
    wake(1, 0xff, IA64_IRQ_INT);
    WAIT_FOR(LOAD(&mocks[1].waits));
    struct vcpu *ap = vm->cpu[1].v;
    CHECK(!LOAD(&mocks[1].calls));
    wake(1, 0x40, IA64_IRQ_INT);
    /* A kick published after the waiter's exchange carries another wake.
     * IRR publication alone need not mean that later kick was consumed. */
    WAIT_FOR((__atomic_load_n(&ap->s.cr[CR_IRR0 + 1], __ATOMIC_ACQUIRE) & 1) &&
             !LOAD(&ap->irq_kick));
    CHECK(!LOAD(&mocks[1].calls) && LOAD(&mocks[0].inside) && LOAD(&mocks[0].calls) == 1);
    CHECK(!LOAD(&ap->irq_kick));
    STORE(&controlled, 0);
    vm_stop_all(vm, false);
    CHECK(vm_wait(vm, UINT64_MAX) == VMM_EXIT_TIMEOUT);
    all_stopped(vm);
    free_vm(vm);
}

/* smp_serialize: a waiter for the run lock is parked until it holds
 * the lock, so code reclamation's epoch horizon skips it: a
 * retirement the holder's executor has passed is reached while the waiter
 * waits, as no peer runs code meanwhile. */
static void case_lock_park(void)
{
    struct vm *vm = new_vm(2, true, 1);
    for (unsigned i = 0; i < 2; ++i)
        code_register_vcpu(vm->cpu[i].v);
    script(0, B_RUN, B_RUN);
    script(1, B_RUN, B_RUN);
    STORE(&controlled, 1);
    struct vm_exec e = exec_now();
    CHECK(vm_start(vm, &e));
    WAIT_FOR(state(vm, 1) == VM_CPU_RESET_HOLD);
    WAIT_FOR(LOAD(&mocks[0].calls) == 1);
    wake(1, 0xff, IA64_IRQ_INT);
    WAIT_FOR(LOAD(&mocks[1].waits));
    struct vcpu *ap = vm->cpu[1].v;
    CHECK(LOAD(&ap->parked) && !LOAD(&mocks[1].calls));
    /* As the holder's executor at a boundary after a retirement. */
    code_lock();
    uint64_t stamp = code_retire_epoch();
    code_quiescent(vm->cpu[0].v);
    CHECK(code_epoch_reached(stamp));
    code_unlock();
    STORE(&mocks[0].released, LOAD(&mocks[0].calls));
    WAIT_FOR(LOAD(&mocks[1].calls) == 1);
    STORE(&controlled, 0);
    vm_stop_all(vm, false);
    CHECK(vm_wait(vm, UINT64_MAX) == VMM_EXIT_TIMEOUT);
    all_stopped(vm);
    free_vm(vm);
}

/* smp_serialize: full slices while no other executor waits for the run
 * lock; short slices while one does. */
static void case_lock_slices(void)
{
    struct vm *vm = new_vm(2, true, 1);
    script(0, B_RUN, B_RUN);
    script(1, B_RUN, B_RUN);
    STORE(&controlled, 1);
    STORE(&idle_gates, 1);
    struct vm_exec e = exec_now();
    CHECK(vm_start(vm, &e));
    WAIT_FOR(state(vm, 1) == VM_CPU_RESET_HOLD);
    /* Each snapshot is stable: the engine is gated after counting its
     * budget. No other executor can hold a waiting ticket in reset-hold. */
    for (uint64_t call = 1; call <= 200; ++call) {
        WAIT_FOR(LOAD(&mocks[0].calls) == call);
        CHECK(!LOAD(&mocks[0].short_slices) && LOAD(&mocks[0].full_slices) == call);
        if (call < 200)
            STORE(&mocks[0].released, call);
    }
    wake(1, 0xff, IA64_IRQ_INT);
    unsigned holder = 0, waiter = 1;
    uint64_t seen[2] = {0, 0}, shorts[2] = {0, 0};
    for (unsigned turn = 0; turn < 200; ++turn) {
        WAIT_FOR(LOAD(&mocks[waiter].waits) > seen[waiter]);
        seen[waiter] = LOAD(&mocks[waiter].waits);
        uint64_t calls = LOAD(&mocks[waiter].calls);
        uint64_t full = LOAD(&mocks[waiter].full_slices);
        STORE(&mocks[holder].released, LOAD(&mocks[holder].calls));
        /* The waiter is held in its HAL return until the former owner
         * has queued. Thus contention is true before budget selection. */
        WAIT_FOR(LOAD(&mocks[holder].waits) > seen[holder]);
        STORE(&mocks[waiter].permitted, seen[waiter]);
        WAIT_FOR(LOAD(&mocks[waiter].calls) == calls + 1);
        CHECK(LOAD(&mocks[waiter].short_slices) == ++shorts[waiter]);
        CHECK(LOAD(&mocks[waiter].full_slices) == full);
        holder = waiter;
        waiter = 1 - holder;
    }
    CHECK(shorts[0] == 100 && shorts[1] == 100);
    fprintf(stderr,
            "lock slices: 200 full without a waiter, 100 short per vCPU with a queued waiter\n");
    STORE(&controlled, 0);
    vm_stop_all(vm, false);
    CHECK(vm_wait(vm, UINT64_MAX) == VMM_EXIT_TIMEOUT);
    all_stopped(vm);
    free_vm(vm);
}

/* Reset-hold: an AP executes nothing before its first
 * wake, stays parked, and leaves on any source PAL_HALT_LIGHT wakes on
 * (SDM Vol.2 Table 5-8 with TPR at reset: a vector >= 16, an NMI, an
 * ExtINT), the vector still in IRR; one never woken stays held; an INIT
 * stops the VM, also with a firmware PALE_INIT. */
static void case_hold(void)
{
    uint64_t from = log_now();
    struct vm *vm = new_vm(6, false, 1);
    for (unsigned i = 0; i < 6; ++i)
        script(i, i ? B_HALT : B_RUN, B_RUN);
    /* A firmware with PALE_INIT: the INIT still stops a held AP, which has
     * no firmware context. */
    vm->cpu[5].v->cfg.pale_init = 0x7000;
    struct vm_exec e = exec_now();
    CHECK(vm_start(vm, &e));
    for (unsigned i = 1; i < 6; ++i)
        WAIT_FOR(state(vm, i) == VM_CPU_RESET_HOLD);
    usleep(50000);
    for (unsigned i = 1; i < 6; ++i)
        CHECK(state(vm, i) == VM_CPU_RESET_HOLD && LOAD(&vm->cpu[i].v->parked) &&
              !LOAD(&mocks[i].calls));
    wake(1, 16, IA64_IRQ_INT);
    wake(2, 0xff, IA64_IRQ_INT);
    wake(3, 0, IA64_IRQ_NMI);
    wake(4, 0, IA64_IRQ_EXTINT);
    for (unsigned i = 1; i < 5; ++i)
        WAIT_FOR(LOAD(&mocks[i].calls));
    CHECK(LOAD(&mocks[1].first_irr0) >> 16 & 1);
    CHECK(LOAD(&mocks[2].first_irr3) >> 63 & 1);
    CHECK(LOAD(&mocks[3].first_irr0) >> 2 & 1);
    CHECK(LOAD(&mocks[4].first_irr0) & 1);
    for (unsigned i = 1; i < 5; ++i)
        CHECK(!vm->cpu[i].v->purge_seen);
    usleep(50000);
    CHECK(state(vm, 5) == VM_CPU_RESET_HOLD && !LOAD(&mocks[5].calls) &&
          LOAD(&vm->cpu[5].v->parked));
    wake(5, 0, IA64_IRQ_INIT);
    CHECK(vm_wait(vm, UINT64_MAX) == VMM_EXIT_UNIMPLEMENTED);
    all_stopped(vm);
    CHECK(vm->first_terminal == 6 && !LOAD(&mocks[5].calls));
    CHECK(logged(from, "vCPU5 INIT/PMI in reset-hold: no firmware context") &&
          logged(from, "vCPU5 guest exit reason=3") &&
          logged(from, "requested exit in reset-hold"));
    free_vm(vm);
}

/* halt_retry_us: only vCPU 0 before ExitBootServices
 * retries PAL_HALT_LIGHT every millisecond; an AP, and vCPU 0 after it,
 * sleeps until an interrupt or its ITM. */
static void case_halt_retry(void)
{
    struct vm *vm = new_vm(2, false, 1);
    script(0, B_HALT, B_HALT);
    script(1, B_HALT, B_HALT);
    struct vm_exec e = exec_now();
    e.halt_retry_us = retry_us = 1000;
    STORE(&retry_check, 1);
    CHECK(vm_start(vm, &e));
    WAIT_FOR(state(vm, 1) == VM_CPU_RESET_HOLD);
    wake(1, 0xff, IA64_IRQ_INT);
    WAIT_FOR(LOAD(&retry_timed[0]) >= 5 && LOAD(&retry_infinite[1]));
    CHECK(!LOAD(&retry_timed[1]));
    STORE(&platform.boot_services_exited, true);
    hal_cpu_request(1, HAL_CPU_WAKE);
    WAIT_FOR(LOAD(&retry_infinite[0]));
    unsigned timed = LOAD(&retry_timed[0]);
    for (unsigned round = 0; round < 5; ++round) {
        unsigned before = LOAD(&retry_infinite[0]);
        hal_cpu_request(1, HAL_CPU_WAKE);
        WAIT_FOR(LOAD(&retry_infinite[0]) > before);
        CHECK(LOAD(&retry_timed[0]) == timed && !LOAD(&retry_timed[1]));
    }
    vm_stop_all(vm, false);
    CHECK(vm_wait(vm, UINT64_MAX) == VMM_EXIT_TIMEOUT);
    all_stopped(vm);
    STORE(&platform.boot_services_exited, false);
    free_vm(vm);
}

/* Park bracket: parked = 1 around the idle; a kick published before
 * the bracket makes the idle return at once; nothing pending: it sleeps. */
static void case_park(void)
{
    struct vcpu *v = new_vcpu(0, false);
    CHECK(v);
    hal_cpu_poll();
    uint64_t start = hal_counter();
    vm_cpu_park(v);
    CHECK(LOAD(&v->parked) == 1);
    while (hal_counter() < start + 50 * MS)
        hal_idle_until(start + 50 * MS);
    vm_cpu_unpark(v);
    CHECK(hal_counter() - start >= 50 * MS && !LOAD(&v->parked));
    hal_cpu_poll();
    STORE(&v->irq_kick, 1);
    vm_cpu_park(v);
    CHECK(hal_cpu_poll() & HAL_CPU_WAKE);
    hal_cpu_request(hal_cpu_index(), HAL_CPU_WAKE);
    hal_idle_until(UINT64_MAX);
    vm_cpu_unpark(v);
    CHECK(!LOAD(&v->parked));
    vcpu_destroy(v);
}

/* ITC: one VM-wide origin for every vCPU, whenever it was
 * created or reset; itc_icount with more than one vCPU is a setup error.
 * Outside a VM each reset takes its own origin. */
static void case_itc(void)
{
    struct vm *vm = vm_create(2, &platform, true);
    CHECK(vm && ia64_smp_active());
    usleep(10000);
    struct vcpu *v0 = new_vcpu(0, false);
    usleep(10000);
    struct vcpu *v1 = new_vcpu(1, false);
    CHECK(v0 && v1 && v0->clock_origin == vm->itc_origin && v1->clock_origin == vm->itc_origin);
    usleep(10000);
    vcpu_reset(v1, &(struct ia64_entry_regs){.ip = 0x100000});
    CHECK(v1->clock_origin == vm->itc_origin);
    uint64_t a = ia64_itc_read(v0), b = ia64_itc_read(v1);
    CHECK(b >= a); /* Equal origins above, monotonic reads despite preemption. */
    CHECK(!new_vcpu(1, true));
    vcpu_destroy(v0);
    vcpu_destroy(v1);
    uint64_t origin = vm->itc_origin;
    vm_destroy(vm);
    CHECK(!ia64_smp_active());
    struct vcpu *alone = new_vcpu(0, true);
    CHECK(alone && alone->clock_origin > origin);
    vcpu_destroy(alone);
    vm = vm_create(1, &platform, false);
    CHECK(vm && !ia64_smp_active());
    alone = new_vcpu(0, true);
    CHECK(alone && alone->clock_origin == vm->itc_origin);
    vcpu_destroy(alone);
    vm_destroy(vm);
}

/* 1 + vcpus + helpers host CPUs fit boot_info.cpus[]. */
static void case_hal_bound(void)
{
    CHECK(hal_linux_configure(true, 200, 64, NULL, 0) != NULL);
}
static void case_hal_bound_fits(void)
{
    CHECK(hal_linux_configure(true, 191, 64, NULL, 0) == NULL);
}

static void case_entry_abi(void)
{
    CHECK(smp_entry_abi_test());
}

/* --- driver ------------------------------------------------------------- */

struct test_case {
    const char *name;
    void (*run)(void);
    unsigned vcpus;   /* threaded HAL vCPU count; 0: the case configures */
    unsigned seconds; /* alarm */
};
static const struct test_case cases[] = {
    {"states", case_states, 5, 30},
    {"watchdog", case_watchdog, 3, 30},
    {"unstarted", case_unstarted, 3, 30},
    {"race", case_race, 255, 300},
    {"cas-order", case_cas_order, 2, 30},
    {"restart-decision", case_restart_decision, 4, 30},
    {"restart-publication", case_restart_publication, 2, 30},
    {"cas-simultaneous", case_cas_simultaneous, 255, 120},
    {"run-lock", case_run_lock, 4, 300},
    {"lock-drain", case_lock_drain, 2, 60},
    {"lock-slices", case_lock_slices, 2, 60},
    {"lock-park", case_lock_park, 2, 60},
    {"hold", case_hold, 6, 30},
    {"halt-retry", case_halt_retry, 2, 30},
    {"park", case_park, 1, 30},
    {"itc", case_itc, 2, 30},
    {"hal-bound", case_hal_bound, 0, 30},
    {"hal-bound-fits", case_hal_bound_fits, 0, 30},
    {"entry-abi", case_entry_abi, 0, 30},
    {"sync-region", case_sync_region, 3, 30},
    {"sync-quiet", case_sync_quiet, 3, 30},
    {"sync-interp", case_sync_interp, 2, 30},
    {"sync-pair", case_sync_pair, 3, 30},
    {"sync-purge", case_sync_purge, 2, 30},
};

static bool run_case(const struct test_case *c)
{
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        /* SMP_EXEC_ALARM=N caps every alarm (tests/host/smp/mutants.py). */
        const char *cap = getenv("SMP_EXEC_ALARM");
        unsigned seconds = cap && (unsigned)atoi(cap) && (unsigned)atoi(cap) < c->seconds
                               ? (unsigned)atoi(cap)
                               : c->seconds;
        watchdog_start(c->name, seconds);
        if (c->vcpus)
            CHECK(!hal_linux_configure(true, c->vcpus, 0, NULL, 0));
        c->run();
        _exit(0);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    if (WIFEXITED(status) && !WEXITSTATUS(status))
        return true;
    if (WIFSIGNALED(status))
        fprintf(stderr, "test-smp-exec: %s killed by signal %d%s\n", c->name, WTERMSIG(status),
                WTERMSIG(status) == SIGALRM ? " (alarm: a hang)" : "");
    return false;
}

int main(int argc, char **argv)
{
    race = mmap(NULL, sizeof(*race), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(race != MAP_FAILED);
    unsigned failed = 0;
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        const struct test_case *c = &cases[k];
        bool selected = argc == 1;
        for (int a = 1; a < argc; ++a)
            selected |= !strcmp(argv[a], c->name);
        if (!selected)
            continue;
        uint64_t start = hal_counter();
        bool ok = true;
        if (!strcmp(c->name, "race")) {
            /* Children until RACE_ITERATIONS; each retires up to 255 CPUs. */
            *race = (struct race_totals){0};
            for (unsigned child = 0; ok && race->iterations < RACE_ITERATIONS; ++child)
                ok = child < 200 && run_case(c);
            ok = ok && race->woken >= 100 && race->stopped >= 100;
            fprintf(stderr,
                    "race: %llu iterations, %llu stopped before the executor started (WAKE), "
                    "%llu after (STOP)\n",
                    (unsigned long long)race->iterations, (unsigned long long)race->woken,
                    (unsigned long long)race->stopped);
        } else
            ok = run_case(c);
        double load[3];
        CHECK(getloadavg(load, 3) == 3);
        printf("test-smp-exec: %s %s (%.2fs; load=%.2f %.2f %.2f)\n", c->name,
               ok ? "passed" : "FAILED", (double)(hal_counter() - start) / 1e9, load[0], load[1],
               load[2]);
        fflush(stdout);
        failed += !ok;
    }
    return failed ? 1 : 0;
}
