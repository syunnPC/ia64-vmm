/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bare-metal windows. Guest backing comes from the same
 * GM_RAM HHDM pages used by helpers, not an assumed GPA/HPA identity.
 * Local invalidation is delegated to amd64_window_flush; behaviour reference:
 * Asteria Source/Platform/Amd64/Memory/Amd64LocalTlbInvalidator.cpp.
 * Remote completion reuses the SMP mailbox/IPI, with no receiver-side lock. */
#include "host/amd64/hostwin_amd64.h"
#include "host/amd64/amd64.h"
#include "host/amd64/smp.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"

#define PAGE UINT64_C(4096)
#define PTE_P UINT64_C(1)
#define PTE_W UINT64_C(2)
#define PTE_NX (UINT64_C(1) << 63)
#define PTE_ADDR UINT64_C(0x000ffffffffff000)
struct window {
    struct amd64_shadow_root shadow;
    unsigned cpu, lock, w, slots;
    uintptr_t base, end, poison;
    bool reserved;
    struct amd64_hostwin_stats stats;
};
/* One context per executor's host CPU (per-CPU private root):
 * its single reservation lies in that CPU's private root, so every context
 * reuses one range, slots << w bytes from 2^w and the poison page after
 * them. CPU 0 manages on the boot root and has none. Only that CPU fills
 * its context; any CPU write-protects or unmaps it. Each mutation
 * completes its target flush before returning, so FLUSH_ALL has no
 * deferred work. Each context holds at most an equal share of the shadow
 * table pool, so a share's exhaustion is cured by dropping that window. */
static struct window windows[BOOT_MAX_CPUS];
/* One page can need a PDPT, a PD and a PT. */
#define PAGE_TABLES 3u
static void lock_window(struct window *win)
{
    amd64_window_lock(&win->lock, win->cpu, "reservation lock");
}
static void unlock_window(struct window *win)
{
    __atomic_store_n(&win->lock, 0, __ATOMIC_RELEASE);
}
void *amd64_hostwin_context(unsigned cpu)
{
    unsigned executors = hal_vcpu_count();
    bool executor = false;
    for (unsigned i = 0; i < executors; ++i)
        executor |= hal_vcpu_cpu(i) == cpu;
    if (!cpu || cpu >= amd64_cpu_count || !executor)
        return NULL;
    struct window *win = &windows[cpu];
    if (!win->reserved) {
        win->cpu = cpu;
        win->shadow = (struct amd64_shadow_root){amd64_cpus[cpu].root, UINT32_MAX, 0,
                                                 amd64_shadow_capacity() / executors};
    }
    return win;
}
static void *cpu_context(void *ctx, unsigned cpu)
{
    (void)ctx;
    return amd64_hostwin_context(cpu);
}
static bool range(struct window *win, uintptr_t page, uint64_t bytes)
{
    return win && win->reserved && bytes && !((page | bytes) & (PAGE - 1)) && page >= win->base &&
           page < win->end && bytes <= win->end - page;
}
static bool reserve(void *ctx, unsigned w, unsigned slots, uintptr_t *base, uintptr_t *poison)
{
    struct window *win = ctx;
    /* w<=H-4 leaves a full guard slot below slot 0, plus room for poison. */
    if (!win || win->reserved || w < 12 || w > 43 || !slots || slots > HOSTWIN_MAX_SLOTS ||
        win->shadow.share < PAGE_TABLES)
        return false;
    if (!amd64_shadow_init())
        return false;
    win->w = w;
    win->slots = slots;
    win->base = UINT64_C(1) << w;
    win->end = win->base + ((uint64_t)slots << w);
    win->poison = win->end;
    win->reserved = true;
    *base = win->base;
    *poison = win->poison;
    return true;
}

static void flush_locked(struct window *win, uintptr_t address)
{
    /* Stronger than async: acknowledgement also makes table reclamation
     * safe before another fill can consume the freed shadow-table pages. */
    amd64_window_flush(win->cpu, address, true);
    ++win->stats.flushes;
    if (address == HOSTWIN_FLUSH_ALL)
        ++win->stats.all_flushes;
    else
        ++win->stats.page_flushes;
    amd64_shadow_reap(&win->shadow);
}
static bool unmap(void *ctx, uintptr_t page, uint64_t bytes)
{
    struct window *win = ctx;
    if (!range(win, page, bytes))
        return false;
    lock_window(win);
    if (win->shadow.root)
        amd64_shadow_unmap(&win->shadow, page, bytes);
    /* Detaching a paging structure needs context invalidation (SDM Vol.3
     * §4.10.4); a leaf alone needs only INVLPG. */
    flush_locked(win,
                 bytes == PAGE && win->shadow.retired == UINT32_MAX ? page : HOSTWIN_FLUSH_ALL);
    ++win->stats.unmaps;
    unlock_window(win);
    return true;
}
static void release(void *ctx, uintptr_t base, unsigned w, unsigned slots)
{
    struct window *win = ctx;
    if (!win || !win->reserved || base != win->base || w != win->w || slots != win->slots)
        hal_panic("hostwin release of unknown reservation");
    unmap(ctx, base, (uint64_t)slots << w);
    win->reserved = false;
}
static enum hostwin_map_status map(void *ctx, uintptr_t page, gpa_t pa, bool writable)
{
    struct window *win = ctx;
    if (!range(win, page, PAGE) || (pa & (PAGE - 1)))
        return HOSTWIN_MAP_ERROR;
    const struct gm_region *r = gm_lookup(pa);
    if (!r || r->kind != GM_RAM || pa < r->base || PAGE > r->size - (pa - r->base) || !r->hva)
        return HOSTWIN_MAP_UNBACKED;
    uint64_t hva = (uintptr_t)r->hva + pa - r->base;
    uint64_t hpa;
    if ((hva & (PAGE - 1)) || !amd64_memory_physical(hva, &hpa) || (hpa & ~PTE_ADDR))
        return HOSTWIN_MAP_UNBACKED;
    lock_window(win);
    /* A fill elsewhere would land in a root its faulting CPU never uses. */
    if (win->cpu != hal_cpu_index()) {
        ++win->stats.failures;
        unlock_window(win);
        LOGE("hostwin: root of CPU%u, resolve on CPU%u; fixed executor mismatch", win->cpu,
             hal_cpu_index());
        return HOSTWIN_MAP_WRONG_CPU;
    }
    uint64_t *pte = amd64_shadow_leaf(&win->shadow, page, true);
    if (!pte) {
        ++win->stats.failures;
        LOGE("hostwin shadow table pool exhausted: live=%u", amd64_shadow_used());
        unlock_window(win);
        return HOSTWIN_MAP_EXHAUSTED;
    }
    bool present = (__atomic_load_n(pte, __ATOMIC_RELAXED) & PTE_P) != 0;
    __atomic_store_n(pte, hpa | PTE_P | PTE_NX | (writable ? PTE_W : 0), __ATOMIC_RELEASE);
    if (present)
        flush_locked(win, page);
    ++win->stats.maps;
    unlock_window(win);
    return HOSTWIN_MAP_OK;
}
static bool protect_ro(void *ctx, uintptr_t page)
{
    struct window *win = ctx;
    if (!range(win, page, PAGE))
        return false;
    lock_window(win);
    uint64_t *pte = win->shadow.root ? amd64_shadow_leaf(&win->shadow, page, false) : NULL;
    if (pte)
        __atomic_fetch_and(pte, ~PTE_W, __ATOMIC_RELEASE);
    flush_locked(win, page);
    ++win->stats.protects;
    unlock_window(win);
    return true;
}
static void flush(void *ctx, uintptr_t address, bool sync)
{
    /* Every mutation already completed a synchronous target invalidation
     * and reaped its tables. There is no deferred work, even for ALL. */
    (void)ctx;
    (void)address;
    (void)sync;
}
static const struct hostwin_backend backend = {
    .reserve = reserve,
    .release = release,
    .map = map,
    .unmap = unmap,
    .protect_ro = protect_ro,
    .flush = flush,
    .cpu_context = cpu_context,
};
const struct hostwin_backend *hostwin_host_backend(void **ctx)
{
    *ctx = NULL; /* instances take cpu_context's per-CPU contexts */
    return &backend;
}
void amd64_hostwin_get_stats(void *ctx, struct amd64_hostwin_stats *stats)
{
    *stats = (struct amd64_hostwin_stats){0};
    unsigned count = ctx ? 1 : amd64_cpu_count;
    for (unsigned i = 0; i < count; ++i) {
        struct window *win = ctx ? ctx : &windows[i];
        lock_window(win);
        stats->maps += win->stats.maps;
        stats->unmaps += win->stats.unmaps;
        stats->protects += win->stats.protects;
        stats->flushes += win->stats.flushes;
        stats->failures += win->stats.failures;
        stats->page_flushes += win->stats.page_flushes;
        stats->all_flushes += win->stats.all_flushes;
        unlock_window(win);
    }
    stats->shadow_tables = amd64_shadow_used();
}
