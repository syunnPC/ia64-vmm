/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A VM of N vCPUs: one executor
 * per vCPU on the host CPU the HAL placed it on, reset-hold of the APs, the park
 * bracket around guest idle, the smp_serialize run lock, first-terminal
 * exits, stop-all and the management loop. core/vm.c sets the VM up and
 * supplies the guest-specific steps (struct vm_exec_ops); this file never
 * reads the loader observation state.
 */
#include "core/smp.h"
#include "core/helper.h"
#include "core/profile.h"
#pragma weak profile_tick
#include "engine/cache/validity.h"
#include "core/log_internal.h"
#include "persist/persist.h"
#include "platform/ahci.h"
extern void platform_ahci_poll(void) __attribute__((weak));
#include "core/vm.h"
#include "ia64/smp.h"
#include "engine/tier1/tier1.h"
#include "ia64/state.h"
#include "ia64/tlb.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/interrupts.h"
#include "ia64vmm/string.h"
#include "platform/platform.h"
#include <stdatomic.h>

static struct vm vm_storage;
static struct vm *current;
static struct vm_exec exec_config; /* written by vm_start before any executor starts */

/* Why the VM stops: a peer stopped by another executor's terminal exit
 * stays quiet (that exit is the diagnosis), one stopped by the watchdog or
 * by management reports where it stood, as the single executor always did. */
enum { STOP_NONE, STOP_TERMINAL, STOP_WATCHDOG, STOP_MANAGEMENT };
static unsigned stop_cause;
/* The first terminal exit decides whether the guest restarts; later reset
 * requests and the watchdog do not change it. */
static bool restart_decided;
/* Serialize terminal publication with irreversible host CPU stops. */
static unsigned decision_lock;
static void decision_acquire(void)
{
    while (__atomic_exchange_n(&decision_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static void decision_release(void)
{
    __atomic_store_n(&decision_lock, 0, __ATOMIC_RELEASE);
}

/* smp_serialize=1: a VM-wide ticket lock around engine_run.
 * A waiter publishes its ticket and host CPU, then sleeps in hal_idle_until;
 * the releaser wakes the CPU of the next ticket. Each executor holds at
 * most one ticket, so the IA64VMM_MAX_CPUS slots are never reused while
 * their ticket is outstanding; the ticket in the slot's upper half tells a
 * stale entry from the awaited one. */
static struct {
    _Alignas(64) unsigned next;
    _Alignas(64) unsigned serving;
    _Alignas(64) uint64_t waiter[IA64VMM_MAX_CPUS]; /* ticket << 32 | host CPU + 1 */
} run_lock;
/* HAL requests a run-lock waiter consumed while it slept, merged into the
 * executor's next poll so that no HAL_CPU_STOP is lost. Owner thread only. */
static unsigned carried[IA64VMM_MAX_CPUS];

struct vm *vm_current(void)
{
    return __atomic_load_n(&current, __ATOMIC_ACQUIRE);
}

struct vm_cpu *vm_cpu_of(const struct vcpu *v)
{
    struct vm *vm = vm_current();
    if (!vm || !v || v->cfg.index >= vm->count)
        return NULL;
    struct vm_cpu *c = &vm->cpu[v->cfg.index];
    return c->v == v ? c : NULL;
}

/* The purge ring has multiple producers and one consumer per vCPU.
 * A short lock protects publication and copies, including reuse of a slot:
 * checking seq alone cannot make a concurrent struct copy safe in C. No
 * purge, hook, kick, acknowledgement wait or host idle holds this lock. */
#define SMP_PURGE_LOG 64u
struct smp_purge {
    uint64_t seq, va;
    uint32_t rid;
    uint8_t shift, alat, issuer;
};
static struct smp_purge purge_log[SMP_PURGE_LOG];
static uint64_t purge_next;
static unsigned purge_lock;
static uint32_t purge_panic_ms = 30000;

static void purge_lock_take(void)
{
    while (__atomic_exchange_n(&purge_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static void purge_lock_drop(void)
{
    __atomic_store_n(&purge_lock, 0, __ATOMIC_RELEASE);
}
static uint64_t purge_tail(void)
{
    return __atomic_load_n(&purge_next, __ATOMIC_SEQ_CST);
}

bool vm_purge_configure(const char *cmdline)
{
    const char *value;
    size_t length;
    purge_panic_ms = 30000;
    if (!platform_cmdline_value(cmdline, "purge_panic_ms", &value, &length))
        return true;
    uint32_t ms = 0;
    if (!length)
        goto invalid;
    for (size_t i = 0; i < length; ++i) {
        unsigned d = (unsigned)(value[i] - '0');
        if (d > 9 || ms > (UINT32_MAX - d) / 10)
            goto invalid;
        ms = ms * 10 + d;
    }
    purge_panic_ms = ms;
    return true;
invalid:
    LOGE("purge_panic_ms must be a decimal number from 0 to 4294967295");
    return false;
}

static void apply_purges(struct vcpu *v, bool waiting)
{
    if (!vm_cpu_of(v))
        return;
    uint64_t seen = __atomic_load_n(&v->purge_seen, __ATOMIC_RELAXED);
    uint64_t tail = purge_tail();
    while (seen < tail) {
        struct smp_purge e;
        purge_lock_take();
        /* The tail is reserved before seq is published, but both operations
         * finish under this lock. No unpublished hole looks like overrun. */
        uint64_t published = purge_tail();
        e = purge_log[seen % SMP_PURGE_LOG];
        bool all = published - seen > SMP_PURGE_LOG || e.seq != seen + 1;
        purge_lock_drop();
        if (all) {
            ia64_tlb_remote_purge(v, 0, 0, 0, true);
            if (waiting)
                ia64_smp_purge_hook(v, 0, 0, 0, true);
            seen = tail = published;
        } else {
            ia64_tlb_remote_purge(v, e.rid, e.va, e.shift, false);
            /* The model reports a zero-entry ALAT (SDM Vol.1 §4.4.5.3.2),
             * so both purge forms have the same target operation. */
            if (waiting && e.issuer != v->cfg.index)
                ia64_smp_purge_hook(v, e.rid, e.va, e.shift, false);
            ++seen;
        }
        __atomic_store_n(&v->purge_seen, seen, __ATOMIC_RELEASE);
    }
}

void vm_cpu_park(struct vcpu *v)
{
    code_quiescent(v);
    __atomic_store_n(&v->parked, 1, __ATOMIC_SEQ_CST);
    /* As amd64_tlb_lazy_enter: an injector publishes the route mailbox
     * and then sets irq_kick; a purge issuer appends to the log and
     * then reads parked. Either it saw parked == 0 (and kicks or waits), or
     * this re-read sees its entry and the idle that follows returns at once. */
    if (__atomic_load_n(&v->irq_kick, __ATOMIC_SEQ_CST) ||
        __atomic_load_n(&v->purge_seen, __ATOMIC_SEQ_CST) != purge_tail())
        hal_cpu_request(hal_cpu_index(), HAL_CPU_WAKE);
}

void vm_cpu_unpark(struct vcpu *v)
{
    __atomic_store_n(&v->parked, 0, __ATOMIC_SEQ_CST);
    ia64_smp_apply_purges(v);
}

static void run_lock_take(struct vcpu *v, bool boundary)
{
    struct vm *vm = vm_current();
    if (!vm || !vm->serialize)
        return;
    unsigned ticket = __atomic_fetch_add(&run_lock.next, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&run_lock.serving, __ATOMIC_ACQUIRE) == ticket)
        return;
    /* Store-load pairing with vm_run_unlock: either the releaser reads this
     * slot after its serving store, or the load below sees that store. */
    __atomic_store_n(&run_lock.waiter[ticket % IA64VMM_MAX_CPUS],
                     (uint64_t)ticket << 32 | (hal_cpu_index() + 1), __ATOMIC_SEQ_CST);
    struct vm_cpu *c = vm_cpu_of(v);
    /* At the executor's boundary the waiter holds no code pointer: parked
     * it holds neither code reclamation nor purge issuers,
     * and its caller applies the purges posted meanwhile. A purge wait's
     * reacquisition is inside an instruction and stays unparked. */
    bool park = boundary && c;
    if (park)
        vm_cpu_park(v);
    while (__atomic_load_n(&run_lock.serving, __ATOMIC_SEQ_CST) != ticket) {
        hal_idle_until(UINT64_MAX);
        /* hal_idle_until returns while any request is pending: consume it
         * and hand it to the executor loop. */
        unsigned requests = hal_cpu_poll();
        if (c)
            carried[c->index] |= requests;
        /* An interrupt for this vCPU (an IPI from the holder, say) is
         * drained into IRR here, outside the lock as every poll is;
         * its kick is consumed with it, so the slice this waiter gets
         * runs guest code instead of returning at once to drain. A kick
         * after the exchange comes with its own wake. */
        if (boundary && c && (requests & HAL_CPU_WAKE)) {
            __atomic_exchange_n(&v->irq_kick, 0, __ATOMIC_ACQ_REL);
            platform_irq_poll(v);
        }
    }
    if (park)
        __atomic_store_n(&v->parked, 0, __ATOMIC_SEQ_CST);
}
void vm_run_lock(struct vcpu *v)
{
    run_lock_take(v, true);
}
/* smp_serialize: another executor holds a ticket behind the caller, who
 * holds the lock. */
static bool run_lock_contended(void)
{
    return __atomic_load_n(&run_lock.next, __ATOMIC_SEQ_CST) -
               __atomic_load_n(&run_lock.serving, __ATOMIC_RELAXED) >
           1;
}

void vm_run_unlock(struct vcpu *v)
{
    (void)v;
    struct vm *vm = vm_current();
    if (!vm || !vm->serialize)
        return;
    unsigned next = __atomic_load_n(&run_lock.serving, __ATOMIC_RELAXED) + 1;
    __atomic_store_n(&run_lock.serving, next, __ATOMIC_SEQ_CST);
    uint64_t waiter = __atomic_load_n(&run_lock.waiter[next % IA64VMM_MAX_CPUS], __ATOMIC_SEQ_CST);
    if ((unsigned)(waiter >> 32) == next && (uint32_t)waiter)
        hal_cpu_request((uint32_t)waiter - 1, HAL_CPU_WAKE);
}

/* ia64/smp.h. */
bool ia64_smp_active(void)
{
    struct vm *vm = vm_current();
    return vm && vm->count > 1;
}
void ia64_smp_global_purge(struct vcpu *v, uint32_t rid, uint64_t va, unsigned shift, bool alat)
{
    /* Release includes coalesced stores (SDM Vol.2 §4.4.5, §4.4.7).
     * MFENCE drains the host WC buffers as well as ordering WB accesses
     * (Intel SDM Vol.3A §11.2.2, §14.3); a compiler fence cannot do this.
     * One vCPU still owes this release, including WC visibility to devices. */
    __asm__ volatile("mfence" ::: "memory");
    struct vm_cpu *self = vm_cpu_of(v);
    struct vm *vm = vm_current();
    if (!self || vm->count == 1)
        return; /* no route: lockstep shadow */
    purge_lock_take();
    uint64_t s = __atomic_fetch_add(&purge_next, 1, __ATOMIC_SEQ_CST);
    struct smp_purge *e = &purge_log[s % SMP_PURGE_LOG];
    e->va = va;
    e->rid = rid;
    e->shift = shift;
    e->alat = alat;
    e->issuer = self->index;
    __atomic_store_n(&e->seq, s + 1, __ATOMIC_RELEASE);
    purge_lock_drop();
    /* Read parked only after the SC post: paired with vm_cpu_park's
     * store then tail check. */
    for (unsigned i = 0; i < vm->count; ++i) {
        struct vm_cpu *c = &vm->cpu[i];
        if (c != self && __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == VM_CPU_RUNNING &&
            !__atomic_load_n(&c->v->parked, __ATOMIC_SEQ_CST))
            platform_irq_kick(i);
    }
    /* smp_serialize: targets need the run lock to finish their boundary.
     * Reacquire before returning to the instruction's engine caller. */
    vm_run_unlock(v);
    uint64_t start = hal_counter(), last = start, hz = hal_counter_hz();
    for (;;) {
        apply_purges(v, true); /* concurrent issuers serve each other */
        bool done = true;
        for (unsigned i = 0; i < vm->count; ++i) {
            struct vm_cpu *c = &vm->cpu[i];
            if (c != self && __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == VM_CPU_RUNNING &&
                !__atomic_load_n(&c->v->parked, __ATOMIC_SEQ_CST) &&
                __atomic_load_n(&c->v->purge_seen, __ATOMIC_ACQUIRE) < s + 1)
                done = false;
        }
        if (done)
            break;
        uint64_t now = hal_counter(), elapsed = now - start;
        if (now - last >= hz) {
            for (unsigned i = 0; i < vm->count; ++i) {
                struct vm_cpu *c = &vm->cpu[i];
                struct vcpu *peer = c->v;
                LOGE("purge wait vCPU%u ip=%llx slot=%u state=%u parked=%u purge_seen=%llu "
                     "await=%llu rid=%x va=%llx ps=%u alat=%u",
                     i, (unsigned long long)__atomic_load_n(&peer->s.ip, __ATOMIC_RELAXED),
                     __atomic_load_n(&peer->s.slot, __ATOMIC_RELAXED),
                     __atomic_load_n(&c->state, __ATOMIC_ACQUIRE),
                     __atomic_load_n(&peer->parked, __ATOMIC_SEQ_CST),
                     (unsigned long long)__atomic_load_n(&peer->purge_seen, __ATOMIC_ACQUIRE),
                     (unsigned long long)(s + 1), rid, (unsigned long long)va, shift, alat);
            }
            last = now;
        }
        uint64_t ms = elapsed / hz * 1000 + elapsed % hz * 1000 / hz;
        if (purge_panic_ms && ms >= purge_panic_ms)
            hal_panic("global purge acknowledgement timeout");
        /* Interrupts stay enabled; no HAL request is consumed here (the
         * executor must still handle STOP and WAKE after the instruction). */
        __asm__ volatile("pause");
    }
    /* Consume HAL requests only into carried. IRQ polling and kick
     * consumption belong to the next executor/engine boundary. */
    run_lock_take(v, false);
    apply_purges(v, true); /* include entries posted during run-lock reacquisition */
}
void ia64_smp_apply_purges(struct vcpu *v)
{
    apply_purges(v, false);
}
/* sync.i completes only after every other running executor
 * has left the translated code a retirement before it ended.
 * - Tier1: the entries of other vCPUs whose region was retired
 *   (t1_sync_i_*: the slot marks and kicks).
 * - Tier0 holds no running-region slot. An executor publishes its quiescent
 *   epoch before each region entry, and a region retired before the
 *   entry fails its tag check, so only an executor whose epoch is below the
 *   one sampled here can still run a region retired before the sample. The
 *   waiter kicks it and waits until it publishes that epoch, parks or stops.
 *   Executors in sync.i themselves are skipped: sync.i ends every Tier0
 *   region, and two waiters would otherwise wait for each other. A purge
 *   issuer waits for this vCPU without publishing, so the waiter
 *   acknowledges purges meanwhile.
 * Both waits hold nothing, log the vCPUs they wait for once a second and
 * panic after purge_panic_ms. The interpreter has no translated code that
 * outlives an instruction boundary, and under smp_serialize no other
 * executor runs while this one holds the run lock. */
#pragma weak t1_sync_i_sample
#pragma weak t1_sync_i_held
#pragma weak t1_sync_i_vcpu
/* epoch: the lower-tier wait's sample, NULL for a Tier1 entry. */
static void sync_i_log(struct vcpu *peer, const uint64_t *epoch)
{
    struct vm_cpu *c = vm_cpu_of(peer);
    LOGE(
        "sync.i wait vCPU%u%s ip=%llx slot=%u state=%u parked=%u kick=%u quiescent=%llu epoch=%llu",
        peer->cfg.index, epoch ? " (Tier0)" : "",
        (unsigned long long)__atomic_load_n(&peer->s.ip, __ATOMIC_RELAXED),
        __atomic_load_n(&peer->s.slot, __ATOMIC_RELAXED),
        c ? __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) : 0u,
        __atomic_load_n(&peer->parked, __ATOMIC_SEQ_CST),
        __atomic_load_n(&peer->irq_kick, __ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&peer->code_quiescent, __ATOMIC_SEQ_CST),
        (unsigned long long)(epoch ? *epoch : code_epoch_sample()));
}
static bool sync_i_expired(uint64_t start, uint64_t *last)
{
    uint64_t now = hal_counter(), elapsed = now - start, hz = hal_counter_hz();
    uint64_t ms = elapsed / hz * 1000 + elapsed % hz * 1000 / hz;
    if (purge_panic_ms && ms >= purge_panic_ms)
        hal_panic("sync.i wait timeout");
    if (now - *last < hz)
        return false;
    *last = now;
    return true;
}
static void sync_i_upper(struct vcpu *v)
{
    struct t1_sync_wait w;
    if (!t1_sync_i_sample || !t1_sync_i_sample(v, &w))
        return;
    uint64_t start = hal_counter(), last = start;
    for (unsigned i = 0; i < w.n;) {
        if (!t1_sync_i_held(&w, i)) {
            ++i;
            continue;
        }
        if (sync_i_expired(start, &last))
            for (unsigned k = i; k < w.n; ++k)
                if (t1_sync_i_held(&w, k))
                    sync_i_log(t1_sync_i_vcpu(&w, k), NULL);
        __asm__ volatile("pause");
    }
}
/* Executor c may still run a Tier0 region retired at or below epoch. */
static bool sync_i_lower_held(const struct vm_cpu *c, uint64_t epoch)
{
    const struct vcpu *peer = c->v;
    return __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == VM_CPU_RUNNING &&
           !__atomic_load_n(&peer->parked, __ATOMIC_SEQ_CST) &&
           !__atomic_load_n(&peer->sync_i_waiting, __ATOMIC_SEQ_CST) &&
           __atomic_load_n(&peer->code_quiescent, __ATOMIC_SEQ_CST) < epoch;
}
static void sync_i_lower(struct vcpu *v, struct vm *vm, struct vm_cpu *self)
{
    uint64_t epoch = code_epoch_sample(), start = hal_counter(), last = start;
    bool kicked[IA64VMM_MAX_CPUS] = {false};
    for (;;) {
        bool done = true;
        for (unsigned i = 0; i < vm->count; ++i) {
            struct vm_cpu *c = &vm->cpu[i];
            if (c == self || !sync_i_lower_held(c, epoch))
                continue;
            done = false;
            if (!kicked[i]) {
                kicked[i] = true;
                platform_irq_kick(i);
            }
        }
        if (done)
            return;
        apply_purges(v, true);
        if (sync_i_expired(start, &last))
            for (unsigned i = 0; i < vm->count; ++i)
                if (&vm->cpu[i] != self && sync_i_lower_held(&vm->cpu[i], epoch))
                    sync_i_log(vm->cpu[i].v, &epoch);
        __asm__ volatile("pause");
    }
}
void ia64_smp_sync_i(struct vcpu *v)
{
    struct vm *vm = vm_current();
    struct vm_cpu *self = vm ? vm_cpu_of(v) : NULL;
    if (!self || vm->count == 1)
        return;
    /* Before the samples: a concurrent waiter either sees this vCPU waiting
     * or this vCPU sees it (both stores and loads sequentially consistent). */
    __atomic_store_n(&v->sync_i_waiting, 1, __ATOMIC_SEQ_CST);
    sync_i_upper(v);
    if (v->cfg.engine != ENGINE_INTERP && !vm->serialize)
        sync_i_lower(v, vm, self);
    __atomic_store_n(&v->sync_i_waiting, 0, __ATOMIC_SEQ_CST);
}
uint64_t ia64_smp_itc_origin(void)
{
    struct vm *vm = vm_current();
    return vm ? vm->itc_origin : hal_counter();
}

struct vm *vm_create(unsigned count, struct platform_state *platform, bool serialize)
{
    if (!count || count > IA64VMM_MAX_CPUS || vm_current())
        return NULL;
    struct vm *vm = &vm_storage;
    memset(vm, 0, sizeof(*vm));
    vm->count = count;
    vm->platform = platform;
    vm->serialize = serialize;
    for (unsigned i = 0; i < count; ++i) {
        vm->cpu[i].index = i;
        vm->cpu[i].host_cpu = UINT32_MAX;
        vm->cpu[i].state = VM_CPU_UNSTARTED;
        vm->cpu[i].code = VMM_EXIT_FAILURE;
    }
    memset(&run_lock, 0, sizeof(run_lock));
    memset(carried, 0, sizeof(carried));
    memset(purge_log, 0, sizeof(purge_log));
    __atomic_store_n(&purge_next, 0, __ATOMIC_SEQ_CST);
    purge_lock = 0;
    stop_cause = STOP_NONE;
    restart_decided = false;
    decision_lock = 0;
    /* One origin for every ITC, taken before the first vCPU exists. */
    vm->itc_origin = hal_counter();
    __atomic_store_n(&current, vm, __ATOMIC_RELEASE);
    return vm;
}

void vm_destroy(struct vm *vm)
{
    if (vm && vm == vm_current())
        __atomic_store_n(&current, NULL, __ATOMIC_RELEASE);
}

static int exit_code(const struct platform_state *p, struct vcpu_exit exit)
{
    switch (exit.reason) {
    case VCPU_EXIT_SHUTDOWN:
        /* A restart request succeeds whatever its status;
         * a shutdown request only with status 0. */
        if (p && platform_reset_restarts(p))
            return VMM_EXIT_SUCCESS;
        return p && !p->panic && p->reset_requested && p->reset_type == 2 && !p->reset_status
                   ? VMM_EXIT_SUCCESS
                   : VMM_EXIT_FAILURE;
    case VCPU_EXIT_UNIMPLEMENTED:
        return VMM_EXIT_UNIMPLEMENTED;
    case VCPU_EXIT_BAD_ACCESS:
        return VMM_EXIT_BAD_ACCESS;
    default:
        return VMM_EXIT_FAILURE;
    }
}

static void exit_line(struct vcpu *v, const char *who, int code, struct vcpu_exit exit)
{
    if (!code)
        return;
    vmm_log_fb_enable(true);
    LOGE("%sguest exit reason=%u ip=%llx slot=%u info=%llx %s", who, exit.reason,
         (unsigned long long)exit.ip, exit.slot, (unsigned long long)exit.info,
         exit.detail ? exit.detail : "");
    vcpu_dump(v);
}

/* Stopped by vm.stop or HAL_CPU_STOP (the line names the vCPU when the
 * VM has more than one). */
enum stop_point { STOP_AT_BOUNDARY, STOP_AFTER_SLICE, STOP_IN_HOLD };
static void stopped_line(struct vm *vm, struct vm_cpu *c, const char *who, enum stop_point at,
                         uint64_t ip)
{
    unsigned first = __atomic_load_n(&vm->first_terminal, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&stop_cause, __ATOMIC_SEQ_CST) == STOP_TERMINAL && first &&
        first != c->index + 1) {
        LOGI("%sstopped: vCPU%u ended the VM", who, first - 1);
        return;
    }
    if (at == STOP_IN_HOLD) {
        LOGE("%sstopped by management in reset-hold", who);
        return;
    }
    if (at == STOP_AFTER_SLICE)
        LOGE("%smanagement watchdog stopped guest at ip=%llx", who, (unsigned long long)ip);
    else
        LOGE("%sguest stopped at an engine boundary by management", who);
    vcpu_dump(c->v);
}

/* An AP in reset-hold executes nothing until
 * its first wake: any pending external source PAL_HALT_LIGHT would wake on
 * (vcpu_halt_wake_pending, independent of PSR.i: NMI always, ExtINT unless
 * TPR.mmi, vectors 16-255 above TPR; SDM Vol.2 Table 5-8). It
 * stays parked throughout. INIT and PMI stop the VM: a held AP has
 * no firmware context. Returns true when the AP leaves
 * for VM_CPU_RUNNING; the waking vector stays in IRR for the firmware's
 * cr.ivr read. */
static bool reset_hold(struct vm *vm, struct vm_cpu *c, const char *who, int *code,
                       struct vcpu_exit *exit)
{
    struct vcpu *v = c->v;
    vm_cpu_park(v);
    for (;;) {
        code_quiescent(v);
        unsigned requests = hal_cpu_poll();
        if (__atomic_load_n(&vm->stop, __ATOMIC_SEQ_CST) || (requests & HAL_CPU_STOP)) {
            *code = VMM_EXIT_TIMEOUT;
            stopped_line(vm, c, who, STOP_IN_HOLD, 0);
            return false;
        }
        /* No engine runs to consume kicks yet; a kick after this
         * exchange comes with a HAL wake, so the idle below returns. */
        __atomic_exchange_n(&v->irq_kick, 0, __ATOMIC_ACQ_REL);
        platform_irq_poll(v);
        if (vcpu_pal_interrupt_pending(v)) {
            LOGE("%sINIT/PMI in reset-hold: no firmware context", who);
            vcpu_request_exit(v, VCPU_EXIT_UNIMPLEMENTED);
        }
        /* An INIT or PMI the vCPU cannot take requests VCPU_EXIT_UNIMPLEMENTED;
         * without engine_run the hold consumes the request. A stop-all
         * request is seen through vm.stop, which management stores first. */
        unsigned requested = __atomic_exchange_n(&v->requested_exit, 0, __ATOMIC_ACQ_REL);
        if (requested && __atomic_load_n(&vm->stop, __ATOMIC_SEQ_CST))
            continue;
        if (requested) {
            unsigned slot;
            uint64_t ip = vcpu_instruction_pointer(v, &slot);
            *exit = (struct vcpu_exit){(enum vcpu_exit_reason)(requested - 1), ip, slot, 0,
                                       "requested exit in reset-hold"};
            *code = exit_code(vm->platform, *exit);
            exit_line(v, who, *code, *exit);
            return false;
        }
        if (vcpu_halt_wake_pending(v))
            break;
        uint64_t idle = hal_counter();
        hal_idle_until(UINT64_MAX);
        c->idle_ticks += hal_counter() - idle;
    }
    /* Its TC is empty, so no purge posted so far concerns it. */
    __atomic_store_n(&v->purge_seen, purge_tail(), __ATOMIC_RELEASE);
    vm_cpu_unpark(v);
    __atomic_store_n(&c->state, VM_CPU_RUNNING, __ATOMIC_SEQ_CST);
    LOGI("%sleaves reset-hold", who);
    return true;
}

/* First terminal executor wins (CAS), then the VM-wide stop: a peer that
 * sees vm.stop also sees first_terminal and the cause. */
static void finish(struct vm *vm, struct vm_cpu *c, int code, struct vcpu_exit exit)
{
    const struct vm_exec *e = &exec_config;
    c->code = code;
    c->exit = exit;
    decision_acquire();
    unsigned none = 0;
    if (__atomic_compare_exchange_n(&vm->first_terminal, &none, c->index + 1, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        unsigned cause = STOP_NONE;
        /* A watchdog stop already sent HAL_CPU_STOP: no restart then. */
        if (__atomic_compare_exchange_n(&stop_cause, &cause, STOP_TERMINAL, false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST))
            __atomic_store_n(&restart_decided,
                             exit.reason == VCPU_EXIT_SHUTDOWN && vm->platform &&
                                 platform_reset_restarts(vm->platform),
                             __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(&vm->stop, 1, __ATOMIC_SEQ_CST);
    decision_release();
    if (vm->count > 1)
        hal_cpu_request(0, HAL_CPU_WAKE); /* management stops the peers */
    if (e->ops && e->ops->stopped)
        e->ops->stopped(e->ctx, c);
    hal_timer_cancel();
    /* GOP is WC on the executing AP. A release store to ordinary RAM does
     * not drain WC writes before the management CPU observes completion. */
    if (vm->platform && vm->platform->guest_gop_owned)
        __asm__ volatile("sfence" ::: "memory");
    code_unregister_vcpu(c->v); /* no code pointer survives the stopped callback */
    __atomic_store_n(&c->state, VM_CPU_STOPPED, __ATOMIC_RELEASE);
    hal_cpu_request(0, HAL_CPU_WAKE);
}

static void executor(void *arg)
{
    struct vm_cpu *c = arg;
    struct vm *vm = vm_current();
    const struct vm_exec *e = &exec_config;
    struct vcpu *v = c->v;
    char who[16] = "";
    if (vm->count > 1)
        vmm_snprintf(who, sizeof(who), "vCPU%u ", c->index);
    /* Published on entry, before the first poll. Store-load pairing
     * with vm_stop_all: either stop-all reads this state and
     * sends HAL_CPU_STOP, or the first vm.stop test below sees its store. */
    __atomic_store_n(&c->state, c->index ? VM_CPU_RESET_HOLD : VM_CPU_RUNNING, __ATOMIC_SEQ_CST);
    LOGI("vCPU%u engine executor host CPU%u", c->index, hal_cpu_index());
    int code = VMM_EXIT_FAILURE;
    struct vcpu_exit exit = {0};
    bool running = !c->index || reset_hold(vm, c, who, &code, &exit);
    while (running) {
        code_quiescent(v);
        unsigned requests = hal_cpu_poll() | carried[c->index];
        carried[c->index] = 0;
        if (__atomic_load_n(&vm->stop, __ATOMIC_SEQ_CST) || (requests & HAL_CPU_STOP)) {
            code = VMM_EXIT_TIMEOUT;
            stopped_line(vm, c, who, STOP_AT_BOUNDARY, 0);
            break;
        }
        hal_timer_arm(vcpu_next_timer_deadline(v));
        if (e->ops && e->ops->sample)
            e->ops->sample(e->ctx, c); /* Before the deadline test: the last line precedes it. */
        if (e->timeout && (hal_counter() - e->start) / e->hz >= e->timeout) {
            vmm_log_fb_enable(true);
            LOGE("%sguest timeout after %llu sec", who, (unsigned long long)e->timeout);
            vcpu_dump(v);
            code = VMM_EXIT_TIMEOUT;
            break;
        }
        platform_irq_poll(v);
        if (vm->count > 1)
            ia64_smp_apply_purges(v);
        uint64_t budget = e->ops && e->ops->slice ? e->ops->slice(e->ctx, c) : VM_SLICE_BUDGET;
        /* The run lock also covers the PAL service completion: it writes
         * guest RAM through the code validator, engine state. */
        vm_run_lock(v);
        if (vm->count > 1)
            ia64_smp_apply_purges(v); /* entries posted while waiting for the run lock */
        /* A guest vCPU that spins on another one progresses only when the
         * lock passes: while another executor waits, slices are short. */
        if (vm->serialize && budget > VM_SERIAL_SLICE_BUDGET && run_lock_contended())
            budget = VM_SERIAL_SLICE_BUDGET;
        uint64_t slice = hal_counter();
        exit = engine_run(v, budget);
        c->exec_ticks += hal_counter() - slice;
        ia64_pal_complete_services(v);
        vm_run_unlock(v);
        if (__atomic_load_n(&vm->stop, __ATOMIC_SEQ_CST)) {
            code = VMM_EXIT_TIMEOUT;
            stopped_line(vm, c, who, STOP_AFTER_SLICE, exit.ip);
            break;
        }
        if (exit.reason == VCPU_EXIT_DEBUG && e->ops && e->ops->debug_exit &&
            e->ops->debug_exit(e->ctx, c))
            continue;
        if (exit.reason == VCPU_EXIT_BUDGET)
            continue;
        if (exit.reason == VCPU_EXIT_HALT) {
            /* PAL_HALT_LIGHT wakes on an unmasked source even with PSR.i=0.
             * The optional retry supports guest ITC polling
             * before ExitBootServices; an AP, or vCPU 0 after it, waits for an
             * interrupt or its ITM. */
            if (vcpu_halt_wake_pending(v))
                continue;
            uint64_t until = UINT64_MAX;
            if (e->halt_retry_us && !c->index &&
                !(vm->platform && atomic_load_explicit(&vm->platform->boot_services_exited,
                                                       memory_order_relaxed))) {
                uint64_t now = hal_counter();
                uint64_t tick = e->hz / 1000000 * e->halt_retry_us +
                                (e->hz % 1000000 * e->halt_retry_us + 999999) / 1000000;
                if (!tick)
                    tick = 1;
                until = tick > UINT64_MAX - now ? UINT64_MAX : now + tick;
            }
            uint64_t timer = vcpu_next_timer_deadline(v), idle = hal_counter();
            vm_cpu_park(v);
            hal_idle_until(timer < until ? timer : until);
            vm_cpu_unpark(v);
            c->idle_ticks += hal_counter() - idle;
            continue;
        }
        code = exit_code(vm->platform, exit);
        exit_line(v, who, code, exit);
        break;
    }
    const struct engine_ia32_ops *native = engine_ia32_glue();
    if (native)
        native->detach(v);
    finish(vm, c, code, exit);
}

bool vm_restart_decided(const struct vm *vm)
{
    (void)vm;
    return __atomic_load_n(&restart_decided, __ATOMIC_SEQ_CST);
}
void vm_stop_all(struct vm *vm, bool watchdog)
{
    decision_acquire();
    unsigned none = STOP_NONE;
    __atomic_compare_exchange_n(&stop_cause, &none, watchdog ? STOP_WATCHDOG : STOP_MANAGEMENT,
                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    __atomic_store_n(&vm->stop, 1, __ATOMIC_SEQ_CST);
    /* A restart reuses the executors' host CPUs: no permanent HAL stop,
     * also when the watchdog fires while the restart stops the VM. */
    unsigned request = vm_restart_decided(vm) ? HAL_CPU_WAKE : HAL_CPU_STOP;
    decision_release();
    for (unsigned i = 0; i < vm->count; ++i) {
        struct vm_cpu *c = &vm->cpu[i];
        unsigned state = __atomic_load_n(&c->state, __ATOMIC_SEQ_CST);
        if (state == VM_CPU_STOPPED)
            continue;
        /* engine_run consumes this request at an instruction boundary; the
         * IPI also wakes a halted or held executor. */
        vcpu_request_exit(c->v, VCPU_EXIT_DEBUG);
        hal_cpu_request(c->host_cpu, state == VM_CPU_UNSTARTED ? HAL_CPU_WAKE : request);
    }
}

static bool all_stopped(struct vm *vm)
{
    for (unsigned i = 0; i < vm->count; ++i) {
        const struct vm_cpu *c = &vm->cpu[i];
        if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != VM_CPU_STOPPED ||
            (c->host_cpu != UINT32_MAX && hal_cpu_busy(c->host_cpu)))
            return false;
    }
    return true;
}

bool vm_start(struct vm *vm, const struct vm_exec *exec)
{
    exec_config = *exec;
    unsigned self = hal_cpu_index();
    for (unsigned i = 0; i < vm->count; ++i) {
        struct vm_cpu *c = &vm->cpu[i];
        if (c->host_cpu != UINT32_MAX && c->host_cpu != self &&
            hal_cpu_start(c->host_cpu, executor, c))
            continue;
        LOGE("vCPU%u: no executor on host CPU%u", i, c->host_cpu);
        /* Never started: nothing to stop. The started ones are stopped. */
        for (unsigned j = i; j < vm->count; ++j) {
            vm->cpu[j].code = VMM_EXIT_SETUP;
            __atomic_store_n(&vm->cpu[j].state, VM_CPU_STOPPED, __ATOMIC_RELEASE);
        }
        if (i) {
            vm_stop_all(vm, false);
            vm_wait(vm, UINT64_MAX);
        }
        return false;
    }
    return true;
}

int vm_wait(struct vm *vm, uint64_t watchdog)
{
    bool stopping = false;
    while (!all_stopped(vm)) {
        unsigned requests = hal_cpu_poll();
        if (platform_ahci_poll)
            platform_ahci_poll();
        log_drain();
        hal_input_poll();
#if !defined(IA64VMM_HOSTED) && !defined(IA64VMM_AMD64_HOST_ONLY)
        plog_poll(false);
        uint64_t watch = media_watch();
#endif
        enum hal_shutdown_kind kind;
        int requested_code;
        if (hal_shutdown_requested(&kind, &requested_code))
            hal_shutdown(kind, requested_code);
        /* A terminal executor stored vm.stop and woke this CPU. */
        if (!stopping && __atomic_load_n(&vm->stop, __ATOMIC_SEQ_CST)) {
            vm_stop_all(vm, false);
            stopping = true;
        }
        if (hal_counter() >= watchdog) {
            vm_stop_all(vm, true);
            stopping = true;
            watchdog = UINT64_MAX;
        }
        uint64_t deadline = watchdog;
#if !defined(IA64VMM_HOSTED) && !defined(IA64VMM_AMD64_HOST_ONLY)
        uint64_t media = plog_deadline();
        if (media < deadline)
            deadline = media;
        if (watch < deadline)
            deadline = watch;
#endif
        uint64_t profile = profile_tick ? profile_tick(vm) : UINT64_MAX;
        if (profile < deadline)
            deadline = profile;
        bool work = false;
        if (!stopping && !requests)
            work = helper_step();
        /* A consumed wake can name queued work. Re-poll duties before the
         * first step, without sleeping away the only wake for that queue. */
        if (!all_stopped(vm) && !work && !requests)
            hal_idle_until(deadline);
    }
    helper_shutdown();
    /* Executors never wait for their log: drain once more after the last. */
    log_drain();
    unsigned first = __atomic_load_n(&vm->first_terminal, __ATOMIC_ACQUIRE);
    return first ? vm->cpu[first - 1].code : VMM_EXIT_FAILURE;
}

int vm_run(struct vm *vm, const struct vm_exec *exec, uint64_t watchdog)
{
    struct vm_cpu *bsp = &vm->cpu[0];
    if (vm->count == 1 && bsp->host_cpu == hal_cpu_index()) {
        exec_config = *exec;
        executor(bsp); /* single-thread unit-test harness */
        return bsp->code;
    }
    if (!vm_start(vm, exec))
        return VMM_EXIT_SETUP;
    /* Only management drains mirrors, draws diagnostics and coordinates
     * shutdown. The executors own all synchronous guest operations. */
    LOGI("management host CPU%u: log drain, watchdog, shutdown", hal_cpu_index());
    return vm_wait(vm, watchdog);
}
