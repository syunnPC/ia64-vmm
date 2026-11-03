/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Tier1 pool (memory.h).
 *
 * One 64-bit word holds the metadata pages (low half) and the workspace
 * pages (high half), so admission against both limits is one atomic
 * update: workspace while metadata + workspace stays within the cap,
 * metadata while metadata + max(workspace, reservation) does. The
 * reservation holds one workspace during synchronous setup, and
 * T1_COMPILE_SLOTS workspaces once background submission is configured.
 *
 * A refused request is retried once after reclaiming records every
 * executor has passed (t1_reclaim's epoch rule), then recorded: the
 * largest outstanding size in `wanted`, which maintenance covers by
 * eviction (region.c). It is cleared once the metadata room covers it or a
 * metadata allocation of at least that size succeeds.
 *
 * A compile context's block is one workspace allocation; arrays carved
 * from it are not counted again. Taking it is an attempt: without it the
 * arrays are pool workspace allocations, refused and recorded as usual.
 */
#include "engine/tier1/memory.h"
#include "engine/tier1/region.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/memory_policy.h"
#include "ia64vmm/log.h"
#include "ia64vmm/platform.h"
#include "ia64vmm/string.h"

#define DEFAULT_BYTES (UINT64_C(48) << 20)
#define LOW UINT64_C(0xffffffff)
static _Atomic uint64_t pages_used; /* metadata | workspace << 32 */
static _Atomic uint64_t budget_pages = DEFAULT_BYTES / 4096;
static _Atomic uint64_t peak_pages, workspace_peak_pages, refusals, wanted;
static _Atomic uint64_t cpu_refusals[IA64VMM_MAX_CPUS];
#ifdef IA64VMM_HOSTED
static _Atomic unsigned refuse_nth;
static size_t test_block = SIZE_MAX;
bool t1_test_workspace_poison;
static void poison(void *p, size_t pages, unsigned flags)
{
    if (p && t1_test_workspace_poison && !(flags & HAL_PAGE_ZERO))
        memset(p, T1_WORKSPACE_POISON, pages * 4096);
}
#else
static void poison(void *p, size_t pages, unsigned flags)
{
    (void)p;
    (void)pages;
    (void)flags;
}
#endif

static uint64_t reserve_pages(void)
{
    static _Atomic uint64_t pages;
    uint64_t n = atomic_load_explicit(&pages, memory_order_relaxed);
    if (!n)
        atomic_store_explicit(&pages, n = (t1_compile_workspace_bytes() + 4095) / 4096,
                              memory_order_relaxed);
    /* Submission changes only with stopped workers. Setup can allocate a
     * large region table before the final pool allowance is configured. */
    return n * (t1_cfg.submit ? T1_COMPILE_SLOTS : 1u);
}

static void raise_to(_Atomic uint64_t *peak, uint64_t value)
{
    uint64_t seen = atomic_load_explicit(peak, memory_order_relaxed);
    while (value > seen && !atomic_compare_exchange_weak_explicit(
                               peak, &seen, value, memory_order_relaxed, memory_order_relaxed)) {
    }
}

static bool admit(uint64_t pages, bool workspace)
{
    uint64_t cap = atomic_load_explicit(&budget_pages, memory_order_relaxed),
             reserve = reserve_pages();
    uint64_t cur = atomic_load_explicit(&pages_used, memory_order_relaxed), next;
    do {
        uint64_t meta = cur & LOW, work = cur >> 32;
        uint64_t held = workspace ? work : (work > reserve ? work : reserve);
        if (pages > cap || meta + held > cap - pages ||
            (workspace ? work + pages : meta + pages) > LOW)
            return false;
        next = cur + (workspace ? pages << 32 : pages);
    } while (!atomic_compare_exchange_weak_explicit(&pages_used, &cur, next, memory_order_acq_rel,
                                                    memory_order_relaxed));
    raise_to(&peak_pages, (next & LOW) + (next >> 32));
    if (workspace)
        raise_to(&workspace_peak_pages, next >> 32);
    return true;
}

static void release(uint64_t pages, bool workspace)
{
    atomic_fetch_sub_explicit(&pages_used, workspace ? pages << 32 : pages, memory_order_release);
}

/* Hosted: the refusal t1_memory_test_refuse armed for this attempt. */
static bool injected_refusal(void)
{
#ifdef IA64VMM_HOSTED
    unsigned nth = atomic_load_explicit(&refuse_nth, memory_order_relaxed);
    return nth && atomic_fetch_sub_explicit(&refuse_nth, 1, memory_order_relaxed) == 1;
#else
    return false;
#endif
}

static void *allocate(size_t pages, unsigned flags, bool workspace)
{
    if (!pages || pages > LOW)
        return NULL;
    bool injected = injected_refusal();
    for (unsigned attempt = 0; !injected && attempt < 2; ++attempt) {
        if (admit(pages, workspace)) {
            void *p = hal_alloc_pages(pages, flags);
            if (p) {
                /* The retried metadata request (or a larger one) got its memory. */
                uint64_t want = atomic_load_explicit(&wanted, memory_order_relaxed);
                if (!workspace && want && want <= (uint64_t)pages * 4096)
                    t1_memory_satisfied(want);
                return p;
            }
            release(pages, workspace);
        }
        if (attempt)
            break;
        t1_reclaim_passed();
    }
    t1_memory_refuse((uint64_t)pages * 4096);
    return NULL;
}

void t1_memory_refuse(uint64_t bytes)
{
    atomic_fetch_add_explicit(&refusals, 1, memory_order_relaxed);
    unsigned cpu = hal_cpu_index();
    if (cpu < IA64VMM_MAX_CPUS)
        atomic_fetch_add_explicit(&cpu_refusals[cpu], 1, memory_order_relaxed);
    raise_to(&wanted, bytes);
    t1_maintenance_due();
}

void *t1_memory_alloc(size_t pages, unsigned flags)
{
    return allocate(pages, flags, false);
}
void *t1_workspace_alloc(size_t pages, unsigned flags)
{
    void *p = allocate(pages, flags, true);
    poison(p, pages, flags);
    return p;
}

void t1_memory_free(void *p, size_t pages)
{
    if (!p)
        return;
    hal_free_pages(p, pages);
    release(pages, false);
}

void t1_workspace_free(void *p, size_t pages)
{
    if (!p)
        return;
    hal_free_pages(p, pages);
    release(pages, true);
}

/* An attempt, not a request: a block the cap does not admit, or for which
 * the allocator has no contiguous run, is neither a refusal nor recorded
 * for maintenance (eviction cannot promise a contiguous run). */
bool t1_ws_acquire(struct t1_workspace *ws)
{
    if (ws->base)
        return true;
    size_t pages = (t1_compile_workspace_bytes() + 4095) / 4096;
#ifdef IA64VMM_HOSTED
    if (test_block != SIZE_MAX)
        pages = test_block;
    if (!pages)
        return false;
#endif
    if (injected_refusal() || !admit(pages, true))
        return false;
    void *p = hal_alloc_pages(pages, 0);
    if (!p) {
        release(pages, true);
        return false;
    }
    *ws = (struct t1_workspace){p, pages, 0};
    return true;
}

void t1_ws_release(struct t1_workspace *ws)
{
    if (ws->base)
        t1_workspace_free(ws->base, ws->pages);
    *ws = (struct t1_workspace){0};
}

void t1_ws_reset(struct t1_workspace *ws)
{
    ws->top = 0;
}

void *t1_ws_alloc(struct t1_workspace *ws, size_t pages, unsigned flags)
{
    if (!ws || !ws->base || !pages || pages > ws->pages - ws->top)
        return t1_workspace_alloc(pages, flags);
    uint8_t *p = ws->base + ws->top * 4096;
    ws->top += pages;
    if (flags & HAL_PAGE_ZERO)
        memset(p, 0, pages * 4096);
    else
        poison(p, pages, flags);
    return p;
}

void t1_ws_free(struct t1_workspace *ws, void *p, size_t pages)
{
    uint8_t *q = p;
    if (ws && ws->base && q >= ws->base && q < ws->base + ws->pages * 4096) {
        if (q + pages * 4096 == ws->base + ws->top * 4096)
            ws->top -= pages;
        return;
    }
    t1_workspace_free(p, pages);
}

uint64_t t1_memory_refusals_here(void)
{
    unsigned cpu = hal_cpu_index();
    return cpu < IA64VMM_MAX_CPUS ? atomic_load_explicit(&cpu_refusals[cpu], memory_order_relaxed)
                                  : 0;
}

uint64_t t1_memory_headroom(void)
{
    uint64_t cap = atomic_load_explicit(&budget_pages, memory_order_relaxed),
             reserve = reserve_pages();
    uint64_t cur = atomic_load_explicit(&pages_used, memory_order_relaxed);
    uint64_t meta = cur & LOW, work = cur >> 32;
    uint64_t held = meta + (work > reserve ? work : reserve);
    return held < cap ? (cap - held) * 4096 : 0;
}

uint64_t t1_memory_wanted(void)
{
    return atomic_load_explicit(&wanted, memory_order_relaxed);
}

void t1_memory_satisfied(uint64_t bytes)
{
    /* A larger request recorded meanwhile stays outstanding. */
    atomic_compare_exchange_strong_explicit(&wanted, &bytes, 0, memory_order_relaxed,
                                            memory_order_relaxed);
}

void t1_memory_get_stats(struct t1_memory_stats *out)
{
    uint64_t cur = atomic_load_explicit(&pages_used, memory_order_relaxed);
    *out = (struct t1_memory_stats){
        .budget_bytes = atomic_load_explicit(&budget_pages, memory_order_relaxed) * 4096,
        .reserve_bytes = reserve_pages() * 4096,
        .used_bytes = ((cur & LOW) + (cur >> 32)) * 4096,
        .workspace_bytes = (cur >> 32) * 4096,
        .peak_bytes = atomic_load_explicit(&peak_pages, memory_order_relaxed) * 4096,
        .workspace_peak = atomic_load_explicit(&workspace_peak_pages, memory_order_relaxed) * 4096,
        .refusals = atomic_load_explicit(&refusals, memory_order_relaxed),
    };
}

void t1_memory_reset(void)
{
    /* Helpers may create their compile contexts before Tier1 setup. */
    uint64_t cur = atomic_load_explicit(&pages_used, memory_order_relaxed);
    atomic_store_explicit(&peak_pages, (cur & LOW) + (cur >> 32), memory_order_relaxed);
    atomic_store_explicit(&workspace_peak_pages, cur >> 32, memory_order_relaxed);
    atomic_store_explicit(&refusals, 0, memory_order_relaxed);
    atomic_store_explicit(&wanted, 0, memory_order_relaxed);
}

uint64_t t1_memory_allowance(uint64_t free_bytes, uint64_t unavailable, uint64_t reserve_bytes)
{
    uint64_t remaining = free_bytes > unavailable ? free_bytes - unavailable : 0;
    /* Leave a quarter for other lazy host allocations. Existing Tier1
     * tables are already absent from free_bytes: count them only once. */
    uint64_t allowance = (remaining - remaining / 4) / 4096;
    uint64_t limit = vmm_tier1_pool_limit(reserve_bytes) / 4096;
    return (allowance > limit ? limit : allowance) * 4096;
}

void t1_memory_configure(uint64_t free_bytes, uint64_t unavailable, uint64_t reserve_bytes)
{
    uint64_t allowance = t1_memory_allowance(free_bytes, unavailable, reserve_bytes) / 4096;
    uint64_t cur = atomic_load_explicit(&pages_used, memory_order_relaxed);
    uint64_t used = (cur & LOW) + (cur >> 32);
    atomic_store_explicit(&budget_pages, used + allowance, memory_order_relaxed);
    LOGI("tier1 memory budget_bytes=%llu reserve_bytes=%llu used_bytes=%llu",
         (unsigned long long)((used + allowance) * 4096),
         (unsigned long long)(reserve_pages() * 4096), (unsigned long long)(used * 4096));
    if (allowance < reserve_pages())
        LOGW(
            "tier1 memory: the budget does not hold the compile workspace reservation; compiles defer");
}

#ifdef IA64VMM_HOSTED
void t1_memory_test_block(size_t pages)
{
    test_block = pages;
}
void t1_memory_set_limit(uint64_t bytes)
{
    atomic_store_explicit(&budget_pages, bytes / 4096, memory_order_relaxed);
}
void t1_memory_test_refuse(unsigned nth)
{
    atomic_store_explicit(&refuse_nth, nth, memory_order_relaxed);
}
void t1_memory_test_reset_peaks(void)
{
    uint64_t cur = atomic_load_explicit(&pages_used, memory_order_relaxed);
    atomic_store_explicit(&peak_pages, (cur & LOW) + (cur >> 32), memory_order_relaxed);
    atomic_store_explicit(&workspace_peak_pages, cur >> 32, memory_order_relaxed);
}
#endif
