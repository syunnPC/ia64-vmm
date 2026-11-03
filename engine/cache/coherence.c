/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/cache/background.h"
#include "engine/cache/coherence-inline.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/platform.h"
#ifndef IA64VMM_TIER0_STATS
#define IA64VMM_TIER0_STATS 0
#endif

/* Registration changes only with stopped executors. Epoch readers never
 * dereference an unregistered vCPU. */
static _Atomic(struct vcpu *) executors[IA64VMM_MAX_CPUS];
uint64_t code_retirement_epoch = 1;
static unsigned mutation_lock;
static struct {
    struct code_lock_stats counters;
} IA64VMM_ALIGNED(64) lock_stats[256];
#ifdef IA64VMM_HOSTED
/* The shim also supports ordinary pthread unit tests without CPU placement. */
static _Thread_local bool owns_lock;
#else
static bool owns_lock[256];
#endif
static bool *ownership(void)
{
#ifdef IA64VMM_HOSTED
    return &owns_lock;
#else
    unsigned cpu = hal_cpu_index();
    if (cpu >= IA64VMM_ARRAY_SIZE(owns_lock))
        hal_panic("code_lock CPU index out of range");
    return &owns_lock[cpu];
#endif
}
bool code_lock_held(void)
{
    return *ownership();
}
void code_lock(void)
{
    if (code_lock_held())
        hal_panic("recursive code_lock");
    /* Do not mask host interrupts: TRACKED and W^X shootdowns acknowledge
     * on a spinning receiver in host interrupt context. */
    uint64_t start = 0;
    while (__atomic_exchange_n(&mutation_lock, 1, __ATOMIC_ACQUIRE)) {
        if (IA64VMM_TIER0_STATS && !start)
            start = hal_counter();
        while (__atomic_load_n(&mutation_lock, __ATOMIC_RELAXED))
            __asm__ volatile("pause");
    }
    *ownership() = true;
    if (IA64VMM_TIER0_STATS) {
        struct code_lock_stats *s = &lock_stats[code_counter_index()].counters;
        __atomic_store_n(&s->acquisitions, __atomic_load_n(&s->acquisitions, __ATOMIC_RELAXED) + 1,
                         __ATOMIC_RELAXED);
        if (start)
            __atomic_store_n(&s->wait_ticks,
                             __atomic_load_n(&s->wait_ticks, __ATOMIC_RELAXED) + hal_counter() -
                                 start,
                             __ATOMIC_RELAXED);
    }
}
void code_unlock(void)
{
    if (!code_lock_held())
        hal_panic("code_unlock without ownership");
    *ownership() = false;
    __atomic_store_n(&mutation_lock, 0, __ATOMIC_RELEASE);
}
uint64_t code_epoch(void)
{
    if (!code_lock_held())
        hal_panic("code_epoch without code_lock");
    return __atomic_load_n(&code_retirement_epoch, __ATOMIC_RELAXED);
}
uint64_t code_retire_epoch(void)
{
    if (!code_lock_held())
        hal_panic("code retirement without code_lock");
    return __atomic_add_fetch(&code_retirement_epoch, 1, __ATOMIC_SEQ_CST);
}
void code_quiescent(struct vcpu *v)
{
    code_quiescent_fast(v);
}
uint64_t code_epoch_sample(void)
{
    return __atomic_load_n(&code_retirement_epoch, __ATOMIC_SEQ_CST);
}
uint64_t code_epoch_horizon(void)
{
    if (!code_lock_held())
        hal_panic("code_epoch_horizon without code_lock");
    uint64_t horizon = UINT64_MAX;
    for (unsigned i = 0; i < IA64VMM_MAX_CPUS; ++i) {
        const struct vcpu *v = executors[i];
        if (!v || __atomic_load_n(&v->parked, __ATOMIC_SEQ_CST))
            continue;
        uint64_t quiescent = __atomic_load_n(&v->code_quiescent, __ATOMIC_SEQ_CST);
        if (quiescent < horizon)
            horizon = quiescent;
    }
    return horizon;
}
bool code_epoch_reached(uint64_t stamp)
{
    if (!code_lock_held())
        hal_panic("code_epoch_reached without code_lock");
    return code_epoch_horizon() >= stamp;
}
bool code_executors_registered(void)
{
    if (!code_lock_held())
        hal_panic("executor inventory without code_lock");
    for (unsigned i = 0; i < IA64VMM_MAX_CPUS; ++i)
        if (executors[i])
            return true;
    return false;
}
bool code_vcpu_is_registered(const struct vcpu *v)
{
    return v->cfg.index < IA64VMM_MAX_CPUS &&
           __atomic_load_n((struct vcpu **)&executors[v->cfg.index], __ATOMIC_ACQUIRE) == v;
}
void code_register_vcpu(struct vcpu *v)
{
    code_lock();
    if (v->cfg.index >= IA64VMM_MAX_CPUS ||
        (executors[v->cfg.index] && executors[v->cfg.index] != v))
        hal_panic("duplicate code epoch executor index");
    /* Cached code pointers cannot span an unregistered interval: live
     * code may move while every executor is stopped. */
    code_retire_epoch();
    code_quiescent(v);
    executors[v->cfg.index] = v;
    code_unlock();
}
void code_unregister_vcpu(struct vcpu *v)
{
    code_lock();
    if (v->cfg.index < IA64VMM_MAX_CPUS && executors[v->cfg.index] == v) {
        code_retire_epoch();
        executors[v->cfg.index] = NULL;
    }
    code_unlock();
}

unsigned code_counter_index(void)
{
#ifdef IA64VMM_HOSTED
    static unsigned next;
    static _Thread_local unsigned slot_plus_one;
    if (!slot_plus_one)
        slot_plus_one = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) + 1;
    unsigned slot = slot_plus_one - 1;
#else
    unsigned slot = hal_cpu_index();
#endif
    if (slot >= 256)
        hal_panic("code counter slot exhausted");
    return slot;
}

void code_lock_get_stats(struct code_lock_stats *out)
{
    *out = (struct code_lock_stats){0};
    for (unsigned i = 0; i < 256; ++i) {
        out->acquisitions +=
            __atomic_load_n(&lock_stats[i].counters.acquisitions, __ATOMIC_RELAXED);
        out->wait_ticks += __atomic_load_n(&lock_stats[i].counters.wait_ticks, __ATOMIC_RELAXED);
    }
}

static void (*background_quiesce)(void), (*background_resume)(void);
static unsigned background_depth;
void code_background_set(void (*quiesce)(void), void (*resume)(void))
{
    background_quiesce = quiesce;
    background_resume = resume;
}
void code_background_quiesce(void)
{
    if (!background_quiesce)
        return;
    if (!background_depth && code_lock_held())
        hal_panic("helper quiescence must precede code_lock");
    if (!background_depth++)
        background_quiesce();
}
void code_background_resume(void)
{
    if (!background_resume)
        return;
    if (!background_depth)
        hal_panic("unbalanced helper quiescence");
    if (!--background_depth)
        background_resume();
}
