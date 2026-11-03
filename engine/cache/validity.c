/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/cache/background.h"
#include "engine/cache/validity.h"
#include "engine/cache/cache.h"
#include "ia64/tlb.h"
#include "engine/tier0/tier0.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/memory_policy.h"
#include "ia64vmm/log.h"
#include "ia64vmm/platform.h"
#include "ia64vmm/string.h"
#include <stdatomic.h>

#define PAGE_BYTES 4096u
#define HASH_SIZE 1024u
typedef uint64_t validity_word __attribute__((may_alias));
struct PageValidity {
    _Atomic(gpa_t) base;
    _Atomic(uint64_t) validated_epoch, generation;
    uint8_t *snapshot;
    _Atomic(const uint8_t *) source;
    _Atomic(unsigned) begin, end;
    _Atomic(bool) ready, tracked;
    bool invalidating;
    _Atomic(uint64_t) covered[4], fresh[4], stay_generation;
    uint64_t retirement, retirement_preference;
    const struct vcpu *retirement_owner;
    unsigned retirement_worker;
    _Atomic(uint64_t) entry_tick;
    /* Reference counts keep exact coverage without walking a hot source chain. */
    unsigned references[CODE_GRANULES];
    struct code_dependency *dependencies;
    struct PageValidity *hash_next, *older, *newer, *free_next;
};
#define SLAB_RECORDS ((PAGE_BYTES - sizeof(void *)) / sizeof(struct PageValidity))
struct validity_slab {
    struct validity_slab *next;
    struct PageValidity records[SLAB_RECORDS];
};
static struct validity_slab *slabs;
static struct PageValidity *available, *oldest, *newest, *clock_hand;
static struct PageValidity *initial_buckets[HASH_SIZE], **buckets = initial_buckets;
static unsigned hash_size = HASH_SIZE;
static enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS;
static uint64_t budget_bytes = CODE_SNAPSHOT_DEFAULT_BYTES;
static bool derived_budget;
static uint64_t storage_epoch, generation = 1;
static _Atomic(uint64_t) entry_generation = 1;
static struct code_validity_stats stats; /* locked snapshot accounting */
static struct {
    struct code_validity_stats counters;
} IA64VMM_ALIGNED(64) executor_stats[256];
static struct code_validity_stats *local_stats(void)
{
    return &executor_stats[code_counter_index()].counters;
}
static bool allocation_wait;
/* Engine-private per-vCPU stay history: no
 * architectural-state ABI change. Executors sharing an index (test copies)
 * start new stays when ownership changes. A stay holds up to
 * CODE_STAY_PAGES source pages. Per page: its admission to the stay (never
 * 0; the low two bits are the record's index; unique per index across
 * stays), the recency of its last entry the validator observed, the stay
 * generation it was admitted with, the entry fetch translation once a
 * region entry on that page recorded one, and the bundles compared. */
_Static_assert(CODE_STAY_PAGES == 4, "admissions keep the record index in two bits");
struct code_stay_page {
    struct PageValidity *page;
    uint64_t generation, admission, entered;
    bool fetched;
    struct code_fetch fetch;
    uint64_t validated[4];
};
/* token: the stay (v->code_entry_generation while it lasts); epoch and iseq:
 * the storage epoch and v's instruction-side MMU sequence it began with;
 * admissions and clock: admission and recency counters. */
struct code_stay {
    const struct vcpu *owner;
    uint64_t token, epoch, iseq, admissions, clock;
    unsigned npages;
    struct code_stay_page page[CODE_STAY_PAGES];
};
static struct code_stay stays[IA64VMM_MAX_CPUS];
static struct {
    struct code_stay_stats counters;
} IA64VMM_ALIGNED(64) stay_stats[256];
/* Owner-only counter slot: published without a locked RMW. */
static void stay_count(uint64_t *counter, uint64_t n)
{
    __atomic_store_n(counter, __atomic_load_n(counter, __ATOMIC_RELAXED) + n, __ATOMIC_RELAXED);
}
void code_stay_get_stats(struct code_stay_stats *out)
{
    *out = (struct code_stay_stats){0};
    uint64_t *dst = (void *)out;
    for (unsigned cpu = 0; cpu < 256; ++cpu) {
        const uint64_t *src = (const void *)&stay_stats[cpu].counters;
        for (unsigned word = 0; word < sizeof(*out) / sizeof(*dst); ++word)
            dst[word] += __atomic_load_n(&src[word], __ATOMIC_RELAXED);
    }
}
static code_track_hook_fn track_hook;
/* Published spans stay immutable until stopped teardown. Stopped setup may
 * add backing after earlier source reads (the hosted suites do this); the
 * next preparation appends its spans without disturbing existing readers.
 * One bit per backed physical page: 32 KiB per GiB. */
struct tracked_span {
    gpa_t first, last;
    uint64_t *bits;
    size_t pages;
};
static struct tracked_span tracked_spans[GM_MAX_REGIONS];
static _Atomic(unsigned) tracked_count;
static unsigned tracked_regions;
static _Atomic(bool) tracked_ready;
static void bitmap_destroy(void);
static bool bitmap_init(void)
{
    unsigned regions = gm_region_count();
    if (tracked_ready && tracked_regions == regions)
        return true;
    unsigned count = atomic_load_explicit(&tracked_count, memory_order_relaxed);
    unsigned original = count;
    /* gm_add_region inserts in address order, so inspect all regions when
     * the stopped map grows, rather than treating its indices as stable. */
    for (unsigned i = 0; i < regions; ++i) {
        const struct gm_region *r = gm_region_at(i);
        if (r->kind != GM_RAM && r->kind != GM_FIRMWARE)
            continue;
        gpa_t first = r->base >> 12, last = (r->base + r->size - 1) >> 12;
        unsigned span;
        for (span = 0; span < count; ++span)
            if (tracked_spans[span].first == first && tracked_spans[span].last == last)
                break;
        if (span < count)
            continue;
        size_t pages = (size_t)((last - first + 64) / 64 * 8 + 4095) / 4096;
        uint64_t *bits = hal_alloc_pages(pages, HAL_PAGE_ZERO);
        if (!bits) {
            /* Only unpublished additions can be freed. Earlier spans and
             * their tracked bits remain available if allocation fails. */
            while (count > original) {
                struct tracked_span *s = &tracked_spans[--count];
                hal_free_pages(s->bits, s->pages);
            }
            return false;
        }
        tracked_spans[count++] = (struct tracked_span){first, last, bits, pages};
    }
    atomic_store_explicit(&tracked_count, count, memory_order_release);
    tracked_regions = regions;
    tracked_ready = true;
    return true;
}
static void bitmap_destroy(void)
{
    tier0_code_spans_reset();
    for (unsigned i = 0; i < tracked_count; ++i)
        hal_free_pages(tracked_spans[i].bits, tracked_spans[i].pages);
    tracked_count = 0;
    tracked_regions = 0;
    tracked_ready = false;
}
bool code_page_tracked_fast(gpa_t page)
{
    /* No source can be tracked before bitmap_init publishes the spans;
     * prepare establishes them before marking or reading its first bundle. */
    if (!tracked_ready)
        return false;
    gpa_t at = page >> 12;
    unsigned count = atomic_load_explicit(&tracked_count, memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) {
        const struct tracked_span *s = &tracked_spans[i];
        if (at < s->first || at > s->last)
            continue;
        uint64_t offset = at - s->first;
        return (__atomic_load_n(&s->bits[offset / 64], __ATOMIC_ACQUIRE) >> (offset % 64)) & 1;
    }
    return false;
}
bool code_page_span(gpa_t page, struct code_page_span *out)
{
    if (mode == CODE_VALIDITY_NONE || !tracked_ready)
        return false;
    gpa_t at = page >> 12;
    unsigned count = atomic_load_explicit(&tracked_count, memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) {
        const struct tracked_span *s = &tracked_spans[i];
        if (at < s->first || at > s->last)
            continue;
        *out = (struct code_page_span){s->first, s->last - s->first + 1, s->bits};
        return true;
    }
    return false;
}
static void bitmap_track(gpa_t page, bool value)
{
    gpa_t at = page >> 12;
    /* Partial regions may share their boundary page. Mirror every span. */
    for (unsigned i = 0; i < tracked_count; ++i) {
        struct tracked_span *s = &tracked_spans[i];
        if (at < s->first || at > s->last)
            continue;
        uint64_t offset = at - s->first, bit = UINT64_C(1) << (offset % 64);
        uint64_t old = __atomic_load_n(&s->bits[offset / 64], __ATOMIC_RELAXED);
        __atomic_store_n(&s->bits[offset / 64], value ? old | bit : old & ~bit, __ATOMIC_RELEASE);
    }
}
void code_validity_set_track_hook(code_track_hook_fn hook)
{
    track_hook = hook;
}
#ifdef IA64VMM_HOSTED
static code_test_hook_fn test_hook;
void code_validity_set_test_hook(code_test_hook_fn hook)
{
    test_hook = hook;
}
void code_validity_test_hook(gpa_t page, enum code_test_stage stage)
{
    if (test_hook)
        test_hook(page, stage);
}
#endif

static unsigned hash(gpa_t base)
{
    return ((base >> 12) ^ (base >> 24)) & (hash_size - 1);
}
/* Hash links are only read under code_lock; record addresses and source
 * versions stay unchanged across a resize, including registered executors. */
static bool resize_hash(uint64_t budget)
{
    unsigned n = HASH_SIZE;
    while (n < 32768 && (uint64_t)n * (CODE_SNAPSHOT_DEFAULT_BYTES / HASH_SIZE) < budget)
        n *= 2;
    if (n == hash_size)
        return true;
    size_t pages = n * sizeof(*buckets) / PAGE_BYTES;
    struct PageValidity **next =
        n == HASH_SIZE ? initial_buckets : hal_alloc_pages(pages, HAL_PAGE_ZERO);
    if (!next)
        return false;
    if (next == initial_buckets)
        memset(next, 0, sizeof(initial_buckets));
    struct PageValidity **previous = buckets;
    unsigned previous_size = hash_size;
    buckets = next;
    hash_size = n;
    for (struct PageValidity *p = oldest; p; p = p->newer) {
        p->hash_next = buckets[hash(p->base)];
        buckets[hash(p->base)] = p;
    }
    if (previous != initial_buckets) {
        size_t old_pages = previous_size * sizeof(*buckets) / PAGE_BYTES;
        hal_free_pages(previous, old_pages);
        stats.metadata_bytes -= old_pages * PAGE_BYTES;
    }
    if (next != initial_buckets)
        stats.metadata_bytes += pages * PAGE_BYTES;
    return true;
}
static struct PageValidity *find(gpa_t base)
{
    for (struct PageValidity *p = buckets[hash(base)]; p; p = p->hash_next)
        if (p->base == base)
            return p;
    return NULL;
}
bool code_validity_active(void)
{
    return mode != CODE_VALIDITY_NONE;
}
static bool any(const uint64_t mask[4])
{
    return (mask[0] | mask[1] | mask[2] | mask[3]) != 0;
}
static bool intersects(const uint64_t a[4], const uint64_t b[4])
{
    return ((a[0] & b[0]) | (a[1] & b[1]) | (a[2] & b[2]) | (a[3] & b[3])) != 0;
}
void code_bundle_mask(gpa_t page, gpa_t gpa, uint64_t len, uint64_t mask[4])
{
    memset(mask, 0, 4 * sizeof(*mask));
    if (!len)
        return;
    gpa_t last = len - 1 > UINT64_MAX - gpa ? UINT64_MAX : gpa + len - 1;
    if (last < page || gpa > (page | 4095))
        return;
    unsigned first = gpa > page ? (unsigned)((gpa - page) >> 4) : 0;
    unsigned end = last < (page | 4095) ? (unsigned)((last - page) >> 4) : 255;
    for (unsigned word = first / 64; word <= end / 64; ++word) {
        unsigned low = word == first / 64 ? first % 64 : 0;
        unsigned high = word == end / 64 ? end % 64 : 63;
        mask[word] = (UINT64_MAX << low) & (UINT64_MAX >> (63 - high));
    }
}
static void tracked(struct PageValidity *p, bool value)
{
    if (p->tracked == value)
        return;
    p->tracked = value;
    bitmap_track(p->base, value);
    if (!value) {
        /* Untracked pages take inline CPU stores without notification
         * (code_page_has_code): no bundle stays fresh, and a formation that
         * read one before the last dependent left fails publication. */
        for (unsigned word = 0; word < 4; ++word)
            p->fresh[word] = 0;
        p->generation = ++generation;
    }
    if (track_hook)
        track_hook(p->base, value ? CODE_PAGE_TRACKED : CODE_PAGE_UNTRACKED);
}
static void code_dependency_remove_locked(struct code_dependency *d)
{
    struct PageValidity *p = d->page;
    if (!p)
        return;
    *d->previous = d->next;
    if (d->next)
        d->next->previous = d->previous;
    for (unsigned word = 0; word < 4; ++word) {
        uint64_t bits = d->mask[word];
        while (bits) {
            unsigned bit = (unsigned)__builtin_ctzll(bits);
            if (!--p->references[64 * word + bit])
                p->covered[word] &= ~(UINT64_C(1) << bit);
            bits &= bits - 1;
        }
    }
    p->stay_generation = ++entry_generation;
    d->page = NULL;
    d->next = NULL;
    d->previous = NULL;
    if (!p->invalidating && !(p->covered[0] | p->covered[1] | p->covered[2] | p->covered[3]))
        tracked(p, false);
}
static void code_dependency_attach_locked(gpa_t base, struct code_dependency *d)
{
    base &= ~UINT64_C(4095);
    code_dependency_remove(d);
    struct PageValidity *p = find(base);
    if (!p || mode == CODE_VALIDITY_NONE)
        return;
    /* A dependency with an empty mask covers the whole page (the
     * conservative contract of callers that pass no masks). */
    if (!any(d->mask))
        for (unsigned word = 0; word < 4; ++word)
            d->mask[word] = UINT64_MAX;
    tracked(p, true);
    d->page = p;
    d->next = p->dependencies;
    d->previous = &p->dependencies;
    if (d->next)
        d->next->previous = &d->next;
    p->dependencies = d;
    for (unsigned word = 0; word < 4; ++word) {
        uint64_t bits = d->mask[word];
        p->covered[word] |= bits;
        while (bits) {
            unsigned bit = (unsigned)__builtin_ctzll(bits);
            ++p->references[64 * word + bit];
            bits &= bits - 1;
        }
    }
}
static void code_dependency_extend_locked(struct code_dependency *d, unsigned granule)
{
    if (granule >= CODE_GRANULES)
        return;
    uint64_t bit = UINT64_C(1) << (granule % 64);
    if (d->mask[granule / 64] & bit)
        return;
    d->mask[granule / 64] |= bit;
    if (d->page) {
        ++d->page->references[granule];
        d->page->covered[granule / 64] |= bit;
    }
}
static void invalidate_mask(struct PageValidity *p, const uint64_t mask[4])
{
    bool hit = ((mask[0] & p->covered[0]) | (mask[1] & p->covered[1]) | (mask[2] & p->covered[2]) |
                (mask[3] & p->covered[3]));
    struct code_dependency *retired = NULL;
    p->invalidating = true;
    /* Detach every overlapping dependent before any callback runs: a
     * callback may re-attach its surviving bundles (decoded cache) or
     * retire other links of its owner without disturbing this walk. */
    if (hit)
        for (struct code_dependency *d = p->dependencies, *next; d; d = next) {
            next = d->next;
            if (!intersects(mask, d->mask))
                continue;
            for (unsigned word = 0; word < 4; ++word)
                d->invalidated[word] = mask[word] & d->mask[word];
            code_dependency_remove(d);
            d->next = retired;
            retired = d;
        }
    while (retired) {
        struct code_dependency *d = retired;
        retired = d->next;
        d->next = NULL;
        d->invalidate(d);
    }
    p->invalidating = false;
    for (unsigned word = 0; word < 4; ++word)
        p->fresh[word] &= ~mask[word];
    /* Even a write to an uncovered bundle can race an unpublished compile.
     * Warm dependents use their own mask, rather than this compile version. */
    p->generation = ++generation;
    p->stay_generation = ++entry_generation;
    if (hit)
        __atomic_add_fetch(&local_stats()->invalidations, 1, __ATOMIC_RELAXED);
    if (!(p->covered[0] | p->covered[1] | p->covered[2] | p->covered[3]))
        tracked(p, false);
}
static void invalidate_page(struct PageValidity *p)
{
    const uint64_t all[4] = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    invalidate_mask(p, all);
    p->ready = false;
    tracked(p, false);
}
static void unlist(struct PageValidity *p)
{
    if (p->older)
        p->older->newer = p->newer;
    else
        oldest = p->newer;
    if (p->newer)
        p->newer->older = p->older;
    else
        newest = p->older;
}
static inline __attribute__((always_inline)) void touch(struct PageValidity *p)
{
    /* Entry paths never relink the shared eviction list. */
    if (!atomic_load_explicit(&p->entry_tick, memory_order_relaxed))
        atomic_store_explicit(&p->entry_tick, 1, memory_order_relaxed);
}
static struct PageValidity *evict_snapshot(const struct vcpu *v)
{
    struct PageValidity *p;
    if (!oldest)
        return NULL;
    p = clock_hand ? clock_hand : oldest;
    struct PageValidity *start = p;
    unsigned passes = 0;
    for (;;) {
#ifdef IA64VMM_HOSTED
        code_validity_test_hook(p->base, CODE_TEST_CLOCK);
#endif
        clock_hand = p->newer ? p->newer : oldest;
        if (p->retirement || !atomic_exchange_explicit(&p->entry_tick, 0, memory_order_relaxed))
            break;
        p = clock_hand;
        if (p == start && ++passes == 2)
            break;
    }
    if (!p->retirement) {
        invalidate_page(p);
        p->retirement = code_retire_epoch();
        p->retirement_preference = 0;
        p->retirement_owner = v;
        p->retirement_worker = code_counter_index();
    }
    if (!code_epoch_reached(p->retirement)) {
        /* Retry the quarantined victim rather than retiring another
         * live page on each executor boundary. */
        clock_hand = p;
        allocation_wait = true;
        return NULL;
    }
    struct PageValidity **link = &buckets[hash(p->base)];
    while (*link != p)
        link = &(*link)->hash_next;
    *link = p->hash_next;
    if (clock_hand == p)
        clock_hand = p->newer ? p->newer : p->older;
    unlist(p);
    __atomic_add_fetch(&local_stats()->evictions, 1, __ATOMIC_RELAXED);
    return p;
}
static struct PageValidity *allocate(const struct vcpu *v, gpa_t base)
{
    allocation_wait = false;
    struct PageValidity *p = NULL;
    uint8_t *snapshot = NULL;
    uint64_t used = stats.snapshot_bytes + stats.metadata_bytes;
    uint64_t need = available ? PAGE_BYTES : 2 * PAGE_BYTES;
    if (used <= budget_bytes && need <= budget_bytes - used) {
        if (!available) {
            struct validity_slab *s = hal_alloc_pages(1, HAL_PAGE_ZERO);
            if (s) {
                stats.metadata_bytes += PAGE_BYTES;
                s->next = slabs;
                slabs = s;
                for (unsigned i = 0; i < SLAB_RECORDS; ++i) {
                    s->records[i].free_next = available;
                    available = &s->records[i];
                }
            }
        }
        if (available)
            snapshot = hal_alloc_pages(1, 0);
        if (snapshot) {
            p = available;
            available = p->free_next;
            stats.snapshot_bytes += PAGE_BYTES;
        } else if (derived_budget) {
            /* Learn the allocator's limit once; later misses reuse storage
             * instead of scanning a full reserve under code_lock. */
            budget_bytes = stats.snapshot_bytes + stats.metadata_bytes;
            LOGI("code snapshot budget lowered_bytes=%llu after allocation failure",
                 (unsigned long long)budget_bytes);
        }
    }
    if (!p) {
        /* Exhaustion below the cap has the same lifetime rules as eviction
         * at the cap: reuse both allocations only after the stamped grace. */
        p = evict_snapshot(v);
        if (!p)
            return NULL;
        snapshot = p->snapshot;
    }
    /* Atomic metadata remains readable by stale stay pointers even after
     * a grace period: publish a different generation before the new base. */
    p->stay_generation = ++entry_generation;
    p->ready = false;
    p->tracked = false;
    p->base = base;
    p->validated_epoch = 0;
    p->generation = 0;
    p->source = NULL;
    p->begin = p->end = 0;
    for (unsigned word = 0; word < 4; ++word)
        p->covered[word] = p->fresh[word] = 0;
    p->snapshot = snapshot;
    p->invalidating = false;
    p->dependencies = NULL;
    memset(p->references, 0, sizeof(p->references));
    p->hash_next = buckets[hash(base)];
    p->older = newest;
    p->newer = NULL;
    p->retirement = 0;
    touch(p);
    buckets[hash(base)] = p;
    if (newest)
        newest->newer = p;
    else
        oldest = p;
    newest = p;
    return p;
}
/* The page record of gpa's page, its snapshot of gpa's bundle read; r is
 * the region holding gpa's 16 bytes (NULL: none). */
static struct ia64_result prepare_region(const struct vcpu *v, const struct gm_region *r, gpa_t gpa,
                                         struct PageValidity **out)
{
    if (!code_lock_held())
        hal_panic("code source preparation without code_lock");
    if (!bitmap_init())
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "code tracked bitmap allocation failed");
    if (!r)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unmapped instruction fetch");
    if (r->kind != GM_RAM && r->kind != GM_FIRMWARE)
        return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, gpa, "instruction fetch from MMIO");
    const uint8_t *raw = r->hva + (size_t)(gpa - r->base);
    if ((uintptr_t)raw & 15)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unaligned bundle backing");
    gpa_t base = gpa & ~UINT64_C(4095);
    struct PageValidity *p = find(base);
    if (!p)
        p = allocate(v, base);
    if (!p)
        return ia64_exit(allocation_wait ? VCPU_EXIT_BUDGET : VCPU_EXIT_BAD_ACCESS, gpa,
                         allocation_wait ? "code snapshot awaiting quiescence"
                                         : "code snapshot allocation failed");
    if (p->retirement) {
        if (!code_epoch_reached(p->retirement))
            return ia64_exit(VCPU_EXIT_BUDGET, gpa, "code snapshot awaiting quiescence");
        /* Give the evictor its first retry (including a redirect here),
         * then let peers revive after one further grace. Otherwise a
         * halted evictor can strand this page without any new allocations.
         * A vCPU-free compiler uses its worker identity instead. */
        if (p->retirement_owner && v ? p->retirement_owner != v
                                     : p->retirement_worker != code_counter_index()) {
            if (!p->retirement_preference)
                p->retirement_preference = code_retire_epoch();
            if (!code_epoch_reached(p->retirement_preference))
                return ia64_exit(VCPU_EXIT_BUDGET, gpa, "code snapshot awaiting quiescence");
        }
        p->retirement = 0;
    }
    unsigned begin = r->base > base ? (unsigned)(r->base - base) : 0;
    unsigned end = r->size - (gpa - r->base) < PAGE_BYTES - (gpa - base)
                       ? (unsigned)(gpa - base + r->size - (gpa - r->base))
                       : PAGE_BYTES;
    const uint8_t *source = r->hva + (size_t)(base + begin - r->base);
    /* Partial RAM regions are allowed by gm_*. Changing the backed span
     * invalidates all page dependencies before selecting the new span. */
    if (p->ready && (p->begin != begin || p->end != end || p->source != source))
        invalidate_page(p);
    if (!p->ready) {
        p->begin = begin;
        p->end = end;
        p->source = source;
        for (unsigned word = 0; word < 4; ++word)
            p->fresh[word] = 0;
        p->validated_epoch = __atomic_load_n(&storage_epoch, __ATOMIC_ACQUIRE);
        p->generation = ++generation;
        p->ready = true;
    }
    /* Protect aliases before the first atomic source read, including after
     * the last dependent was retired while the snapshot allocation survived. */
    if (!p->tracked)
        for (unsigned word = 0; word < 4; ++word)
            p->fresh[word] = 0;
    tracked(p, true);
    unsigned granule = (gpa & 4095) >> 4;
    uint64_t bit = UINT64_C(1) << (granule % 64);
    if (!(p->fresh[granule / 64] & bit)) {
        uint64_t low, high;
        /* Latch BEFORE reading: adding coverage cannot validate older bundles
         * against a newer storage epoch. */
        uint64_t epoch = __atomic_load_n(&storage_epoch, __ATOMIC_ACQUIRE);
        if (!(p->fresh[0] | p->fresh[1] | p->fresh[2] | p->fresh[3]))
            p->validated_epoch = epoch;
        if (!ia64_atomic_read16((void *)raw, &low, &high))
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, gpa, "host cmpxchg16b unavailable");
        __atomic_store_n((validity_word *)(p->snapshot + granule * 16), low, __ATOMIC_RELEASE);
        __atomic_store_n((validity_word *)(p->snapshot + granule * 16 + 8), high, __ATOMIC_RELEASE);
        p->fresh[granule / 64] |= bit;
#ifdef IA64VMM_HOSTED
        code_validity_test_hook(base, CODE_TEST_SNAPSHOT);
#endif
    }
    *out = p;
    return ia64_complete();
}
static struct ia64_result prepare(struct vcpu *v, gpa_t gpa, struct PageValidity **out)
{
    return prepare_region(v, ia64_physical_region(v, gpa, 16), gpa, out);
}
/* The region of a vCPU-free source read: ia64_physical_region's lookup
 * without a vCPU's memory hook or region cache. Code lies in RAM or
 * firmware, which gm_lookup finds; the platform's unclaimed apertures are
 * never code (prepare_region rejects them either way). */
static const struct gm_region *source_region(gpa_t gpa)
{
    if (gpa >> (IA64_IMPL_PA_MSB + 1))
        return NULL;
    const struct gm_region *r = gm_lookup(gpa);
    return r && 16 <= r->size - (gpa - r->base) ? r : NULL;
}
/* slot: the caller's code_counter_index(), for the owner-only counters. */
static bool validate(struct PageValidity *p, const uint64_t requested[4], unsigned slot)
{
    uint64_t serial = p->stay_generation;
    gpa_t base = p->base;
    uint64_t epoch = __atomic_load_n(&storage_epoch, __ATOMIC_ACQUIRE);
    if (mode == CODE_VALIDITY_EPOCH_COMPARE && p->validated_epoch == epoch)
        return true;
    uint64_t changed[4] = {0}, compared = 0;
    uint64_t start = 0;
    if (IA64VMM_TIER0_STATS)
        start = hal_counter();
    for (unsigned word = 0; word < 4; ++word) {
        uint64_t bits = requested[word];
        /* validated_epoch vouches for every fresh bundle, including the
         * provisional reads of a formation that has not attached yet. */
        if (mode == CODE_VALIDITY_EPOCH_COMPARE)
            bits |= p->fresh[word];
        while (bits) {
            unsigned bit = (unsigned)__builtin_ctzll(bits), at = (64 * word + bit) * 16;
            if (at < p->begin || at + 16 > p->end) {
                bits &= bits - 1;
                continue;
            }
            /* Freshness is required by the publisher; existing dependents
             * always own fresh bundles until their invalidation callback. */
            if ((p->fresh[word] >> bit) & 1) {
                const uint8_t *source = p->source;
                unsigned begin = p->begin;
                if (!source || at < begin)
                    return false;
                const validity_word *live = (const void *)(source + at - begin);
                const validity_word *saved = (const void *)(p->snapshot + at);
                ++compared;
                if (__atomic_load_n(&live[0], __ATOMIC_RELAXED) !=
                        __atomic_load_n(&saved[0], __ATOMIC_ACQUIRE) ||
                    __atomic_load_n(&live[1], __ATOMIC_RELAXED) !=
                        __atomic_load_n(&saved[1], __ATOMIC_ACQUIRE))
                    changed[word] |= UINT64_C(1) << bit;
            }
            bits &= bits - 1;
        }
    }
    struct code_validity_stats *counters = &executor_stats[slot].counters;
    if (IA64VMM_TIER0_STATS)
        __atomic_add_fetch(&counters->comparison_ticks, hal_counter() - start, __ATOMIC_RELAXED);
    /* The counter slot belongs to this host executor. Publish for live
     * readers without a locked RMW, as for the owner-only Tier0 counters. */
    __atomic_store_n(&counters->comparisons,
                     __atomic_load_n(&counters->comparisons, __ATOMIC_RELAXED) + 1,
                     __ATOMIC_RELAXED);
    stay_count(&stay_stats[slot].counters.bundles_compared, compared);
    /* Changed bundles lose freshness and their dependents; the requester
     * fails only if one of its own bundles changed. Every remaining fresh
     * bundle was compared at epoch. */
    if (serial != p->stay_generation || base != p->base)
        return false;
    if (any(changed)) {
        bool take = !code_lock_held();
        if (take)
            code_lock();
        if (serial != p->stay_generation || base != p->base) {
            if (take)
                code_unlock();
            return false;
        }
        __atomic_add_fetch(&local_stats()->mismatches, 1, __ATOMIC_RELAXED);
        invalidate_mask(p, changed);
        if (take)
            code_unlock();
    }
    /* Preparation latches the storage epoch before reading a new bundle.
     * Snapshots added after this comparison therefore have an epoch at
     * least this large. A late older store only causes another comparison;
     * successful entry comparison stays lock-free. */
    if (mode == CODE_VALIDITY_EPOCH_COMPARE && serial == p->stay_generation && base == p->base)
        atomic_store_explicit(&p->validated_epoch, epoch, memory_order_relaxed);
    return !intersects(changed, requested);
}
/* Entry hot path helpers, inlined into every entry check. */
#define STAY_INLINE static inline __attribute__((always_inline))
STAY_INLINE bool fetch_equal(const struct code_fetch *a, const struct code_fetch *b)
{
    return !((a->psr ^ b->psr) & ~CODE_PSR_RUNTIME) && a->rr == b->rr && a->pta == b->pta &&
           a->pte == b->pte;
}
STAY_INLINE bool stay_live(const struct code_stay *h, const struct vcpu *v, uint64_t epoch)
{
    return h->owner == v && v->code_entry_generation && v->code_entry_generation == h->token &&
           h->epoch == epoch && h->iseq == v->mmu_iseq;
}
/* p's page of the stay, or NULL (another page, or an event touched p). */
STAY_INLINE struct code_stay_page *stay_page(struct code_stay *h, const struct PageValidity *p)
{
    for (unsigned i = 0; i < h->npages; ++i)
        if (h->page[i].page == p)
            return h->page[i].generation == p->stay_generation ? &h->page[i] : NULL;
    return NULL;
}
/* The whole stay ends on another owner, a cleared or
 * other entry generation (engine_run returns, interruption delivery and
 * device accesses clear it), a storage-epoch change or an instruction-side
 * MMU change. Otherwise a page leaves alone when an event touched it (store,
 * mismatch, dependency removal or eviction: stay_generation) or when a
 * region entry on it brings another fetch translation, and pages the
 * stay does not hold are admitted, the least recently entered page the
 * entry does not need leaving when four are held. Bytes never depend on the
 * translation; the interpreter's per-bundle entries (fetch == NULL) do not
 * compare it.
 *
 * stay() continues or begins v's stay for code whose source pages are
 * pages[0..n-1] (the entry page first); slots[i] is pages[i]'s record. A
 * peer may bump a page's stay generation at any time: the slots are the ones
 * this decision used. */
static void admit(struct code_stay *h, struct code_stay_page *s, struct PageValidity *p)
{
    *s = (struct code_stay_page){.page = p,
                                 .generation = p->stay_generation,
                                 .admission = (++h->admissions << 2) | (uint64_t)(s - h->page)};
}
static struct code_stay *stay(struct vcpu *v, struct PageValidity *const *pages, unsigned n,
                              bool region_entry, const struct code_fetch *fetch,
                              struct code_stay_page **slots, unsigned slot)
{
    if (v->cfg.index >= IA64VMM_MAX_CPUS)
        hal_panic("validator stay index out of range");
    if (!n || n > CODE_STAY_PAGES)
        hal_panic("validator stay page count out of range");
    struct code_stay *h = &stays[v->cfg.index];
    uint64_t epoch = __atomic_load_n(&storage_epoch, __ATOMIC_ACQUIRE);
    struct code_stay_stats *c = &stay_stats[slot].counters;
    uint64_t *ended = NULL;
    if (h->owner != v || !v->code_entry_generation || v->code_entry_generation != h->token)
        ended = &c->reset;
    else if (h->epoch != epoch)
        ended = &c->epoch;
    else if (h->iseq != v->mmu_iseq)
        ended = &c->mmu;
    if (ended) {
        stay_count(ended, 1);
        /* Stay tokens are compared only within this owner/index. Keep
         * their sequence private; record versions still use the unique
         * shared allocator so recycled records cannot revive an old stay.
         * Cleared records match no hint. */
        uint64_t token = h->token + 1;
        if (!token)
            token = 1;
        h->owner = v;
        h->token = token;
        h->epoch = epoch;
        h->iseq = v->mmu_iseq;
        h->npages = 0;
        for (unsigned k = 0; k < CODE_STAY_PAGES; ++k)
            h->page[k] = (struct code_stay_page){0};
        v->code_page = pages[0]->base;
        v->code_entry_generation = token;
    }
    unsigned needed = 0; /* records of the entry's pages */
    bool entry_admitted = false;
    for (unsigned i = 0; i < n; ++i) {
        unsigned k = 0;
        while (k < h->npages && h->page[k].page != pages[i])
            ++k;
        slots[i] = NULL;
        if (k == h->npages)
            continue;
        slots[i] = &h->page[k];
        needed |= 1u << k;
        if (h->page[k].generation != pages[i]->stay_generation) {
            stay_count(&c->event, 1);
            admit(h, slots[i], pages[i]);
            entry_admitted |= !i;
        }
    }
    bool admitted = false;
    for (unsigned i = 0; i < n; ++i) {
        if (slots[i])
            continue;
        unsigned k = h->npages;
        if (k < CODE_STAY_PAGES)
            ++h->npages;
        else {
            for (unsigned j = 0; j < CODE_STAY_PAGES; ++j)
                if (!((needed >> j) & 1) &&
                    (k == CODE_STAY_PAGES || h->page[j].entered < h->page[k].entered))
                    k = j;
            stay_count(&c->evicted, 1);
        }
        stay_count(&c->page, 1);
        slots[i] = &h->page[k];
        needed |= 1u << k;
        admit(h, slots[i], pages[i]);
        admitted = true;
        entry_admitted |= !i;
    }
    /* A page joining as the entry page records the entry's translation; an
     * interpreter fetch records none (zero), so a later region entry on that
     * page compares with it. A held entry page with another translation
     * leaves and joins again. */
    struct code_stay_page *e = slots[0];
    if (!entry_admitted && fetch && e->fetched && !fetch_equal(&e->fetch, fetch)) {
        stay_count(&c->fetch, 1);
        admit(h, e, pages[0]);
        entry_admitted = admitted = true;
    }
    if (entry_admitted) {
        e->fetched = true;
        if (fetch)
            e->fetch = *fetch;
    } else if (fetch && !e->fetched) {
        e->fetch = *fetch;
        e->fetched = true;
    }
    for (unsigned i = 0; i < n; ++i)
        slots[i]->entered = ++h->clock;
    if (region_entry && (ended || admitted))
        tier0_count_page_entry(slot);
    return h;
}
/* Compares the bundles of mask not yet compared in this stay. */
static bool enter_mask(struct code_stay_page *s, struct PageValidity *p, const uint64_t mask[4],
                       unsigned slot)
{
    uint64_t needed[4];
    for (unsigned word = 0; word < 4; ++word)
        needed[word] = mask[word] & ~s->validated[word];
    if (any(needed) && !validate(p, needed, slot))
        return false;
    for (unsigned word = 0; word < 4; ++word)
        s->validated[word] |= mask[word];
    touch(p);
    return true;
}
/* An exact-mask dependent was built from fresh bundles and is retired by
 * any event that clears their freshness. If that invariant is ever broken,
 * retire it instead of skipping the comparison of a stale bundle. */
static bool exact_fresh(struct PageValidity *p, const struct code_dependency *d)
{
    uint64_t stale[4];
    for (unsigned word = 0; word < 4; ++word)
        stale[word] = d->mask[word] & ~p->fresh[word];
    if (!any(stale))
        return true;
    bool take = !code_lock_held();
    if (take)
        code_lock();
    /* A peer may have detached this link or refreshed the page while we
     * acquired the mutation lock. Never apply an obsolete stale mask. */
    if (d->page == p) {
        for (unsigned word = 0; word < 4; ++word)
            stale[word] = d->mask[word] & ~p->fresh[word];
        if (any(stale)) {
            __atomic_add_fetch(&local_stats()->mismatches, 1, __ATOMIC_RELAXED);
            invalidate_mask(p, stale);
        }
    }
    if (take)
        code_unlock();
    return false;
}
/* The entry's pages, or false (a page detached or not ready); then the
 * stay (continued or begun) and the comparison of what it lacks. */
static __attribute__((noinline)) bool
dependencies_enter_slow(struct vcpu *v, struct code_dependency *deps, unsigned n,
                        const struct code_fetch *fetch, struct code_stay_hint *hint)
{
    if (!n || n > CODE_STAY_PAGES)
        hal_panic("validator: region page count out of range");
    struct PageValidity *pages[CODE_STAY_PAGES];
    for (unsigned i = 0; i < n; ++i) {
        pages[i] = deps[i].page;
        if (!pages[i] || !pages[i]->ready)
            return false;
    }
    for (unsigned i = 0; i < n; ++i)
        if (!exact_fresh(pages[i], &deps[i]))
            return false;
    struct code_stay_page *slots[CODE_STAY_PAGES];
    unsigned slot = code_counter_index();
    stay(v, pages, n, true, fetch, slots, slot);
#ifdef IA64VMM_HOSTED
    for (unsigned i = 0; i < n; ++i)
        code_validity_test_hook(pages[i]->base, CODE_TEST_STAY);
#endif
    for (unsigned i = 0; i < n; ++i)
        if (!enter_mask(slots[i], pages[i], deps[i].mask, slot))
            return false;
    if (hint)
        *hint = (struct code_stay_hint){pages[0], slots[0]->admission, slots[0]->generation};
    return true;
}
/* The bundles of d's mask were compared in the stay page s. */
STAY_INLINE bool stay_has(const struct code_stay_page *s, const struct code_dependency *d)
{
    const uint64_t *m = d->mask;
    return s && !((m[0] & ~s->validated[0]) | (m[1] & ~s->validated[1]) |
                  (m[2] & ~s->validated[2]) | (m[3] & ~s->validated[3]));
}
/* Hot path of every region entry: the stay continues (stay() checks),
 * holds every page, and the bundles were compared in it, so they are
 * still fresh: any event clearing freshness bumps stay_generation. Tokens
 * are never 0. One page (Tier0 regions, most Tier1 regions) first. */
static bool dependency_enter(struct vcpu *v, struct code_dependency *d,
                             const struct code_fetch *fetch, struct code_stay_hint *hint)
{
    struct PageValidity *p = d->page;
    if (!p || !p->ready)
        return false;
    struct code_stay *h = &stays[v->cfg.index];
    if (stay_live(h, v, __atomic_load_n(&storage_epoch, __ATOMIC_ACQUIRE))) {
        struct code_stay_page *s = h->page, *end = s + h->npages;
        while (s != end && s->page != p)
            ++s;
        if (s != end && s->generation == p->stay_generation && s->fetched &&
            fetch_equal(&s->fetch, fetch) &&
            !((d->mask[0] & ~s->validated[0]) | (d->mask[1] & ~s->validated[1]) |
              (d->mask[2] & ~s->validated[2]) | (d->mask[3] & ~s->validated[3]))) {
            s->entered = ++h->clock;
            touch(p);
            if (hint)
                *hint = (struct code_stay_hint){p, s->admission, s->generation};
            return true;
        }
    }
    return dependencies_enter_slow(v, d, 1, fetch, hint);
}
bool code_dependency_enter(struct vcpu *v, struct code_dependency *d,
                           const struct code_fetch *fetch)
{
    return mode == CODE_VALIDITY_NONE || dependency_enter(v, d, fetch, NULL);
}
/* The hint's record still holds its admission (the page has not left the
 * stay since: records are cleared when the stay ends) and the page its
 * generation. */
bool code_dependency_reenter(struct vcpu *v, const struct code_dependency *d,
                             const struct code_stay_hint *hint)
{
    if (mode == CODE_VALIDITY_NONE)
        return true;
    struct PageValidity *p = d->page;
    struct code_stay *h = &stays[v->cfg.index];
    if (!p || p != hint->page || !p->ready || !hint->admission ||
        !stay_live(h, v, __atomic_load_n(&storage_epoch, __ATOMIC_ACQUIRE)) ||
        p->stay_generation != hint->generation)
        return false;
    struct code_stay_page *s = &h->page[hint->admission & (CODE_STAY_PAGES - 1)];
    if (s->admission != hint->admission)
        return false;
    s->entered = ++h->clock;
    touch(p);
    return true;
}
bool code_dependency_enter_hint(struct vcpu *v, struct code_dependency *d,
                                const struct code_fetch *fetch, struct code_stay_hint *hint)
{
    *hint = (struct code_stay_hint){0};
    return mode == CODE_VALIDITY_NONE || dependency_enter(v, d, fetch, hint);
}
void code_stay_hint_take(const struct vcpu *v, const struct code_dependency *d,
                         struct code_stay_hint *hint)
{
    *hint = (struct code_stay_hint){0};
    const struct PageValidity *p = d->page;
    const struct code_stay *h = &stays[v->cfg.index];
    if (mode == CODE_VALIDITY_NONE || !p ||
        !stay_live(h, v, __atomic_load_n(&storage_epoch, __ATOMIC_ACQUIRE)))
        return;
    for (unsigned i = 0; i < h->npages; ++i) {
        const struct code_stay_page *s = &h->page[i];
        if (s->page != p)
            continue;
        if (s->fetched && !((d->mask[0] & ~s->validated[0]) | (d->mask[1] & ~s->validated[1]) |
                            (d->mask[2] & ~s->validated[2]) | (d->mask[3] & ~s->validated[3])))
            *hint = (struct code_stay_hint){p, s->admission, s->generation};
        return;
    }
}

static bool witness_live(const struct vcpu *v, const struct code_stay_witness *w,
                         const struct code_fetch *fetch)
{
    return w->owner == v && w->admission && fetch && v->code_entry_generation == w->token &&
           v->mmu_iseq == w->iseq && fetch_equal(&w->fetch, fetch) &&
           atomic_load_explicit(w->page_word, memory_order_acquire) == w->page &&
           atomic_load_explicit(w->ready_word, memory_order_acquire) &&
           __atomic_load_n(w->epoch_word, __ATOMIC_ACQUIRE) == w->epoch &&
           atomic_load_explicit(w->generation_word, memory_order_acquire) == w->generation &&
           *w->admission_word == w->admission;
}

static bool witness_take(const struct vcpu *v, const struct code_dependency *d,
                         const struct code_fetch *fetch, bool entry, struct code_stay_witness *out)
{
    if (mode == CODE_VALIDITY_NONE || !fetch || !code_vcpu_is_registered(v))
        goto fail;
    struct PageValidity *p = atomic_load_explicit(&d->page, memory_order_acquire);
    struct code_stay *h = &stays[v->cfg.index];
    if (!p || !p->ready || !stay_live(h, v, __atomic_load_n(&storage_epoch, __ATOMIC_ACQUIRE)))
        goto fail;
    struct code_stay_page *s = stay_page(h, p);
    if (!stay_has(s, d) || (entry && (!s->fetched || !fetch_equal(&s->fetch, fetch))))
        goto fail;
    *out = (struct code_stay_witness){
        .owner = v,
        .page_word = &d->page,
        .page = p,
        .ready_word = &p->ready,
        .epoch_word = &storage_epoch,
        .generation_word = &p->stay_generation,
        .admission_word = &s->admission,
        .epoch = h->epoch,
        .generation = s->generation,
        .admission = s->admission,
        .token = h->token,
        .iseq = h->iseq,
        .fetch = entry ? s->fetch : *fetch,
        .clock = &h->clock,
        .entered = &s->entered,
        .entry_tick = &p->entry_tick,
    };
    /* A peer's event after stay_live/stay_page must reject the old proof,
     * never replace it with the event's new epoch or page generation. */
#ifdef IA64VMM_HOSTED
    code_validity_test_hook(p->base, CODE_TEST_WITNESS);
#endif
    if (!witness_live(v, out, fetch))
        goto fail;
    return true;
fail:
    __builtin_memset(__builtin_assume_aligned(out, _Alignof(struct code_stay_witness)), 0,
                     sizeof(*out));
    return false;
}

bool code_stay_witness_take(const struct vcpu *v, const struct code_dependency *d,
                            const struct code_fetch *fetch, struct code_stay_witness *out)
{
    return witness_take(v, d, fetch, true, out);
}

bool code_stay_witness_reenter(struct vcpu *v, const struct code_stay_witness *w,
                               const struct code_fetch *fetch)
{
    if (!witness_live(v, w, fetch))
        return false;
    *w->entered = ++*w->clock;
    if (!atomic_load_explicit(w->entry_tick, memory_order_relaxed))
        atomic_store_explicit(w->entry_tick, 1, memory_order_relaxed);
    return true;
}

static bool witness_group_live(const struct vcpu *v, const struct code_stay_witness_group *group,
                               const struct code_fetch *fetch)
{
    if (!group->n || group->n > CODE_STAY_WITNESS_PAGES || !fetch)
        return false;
    for (unsigned i = 0; i < group->n; ++i)
        if (!witness_live(v, &group->page[i], fetch))
            return false;
    return true;
}

bool code_stay_witness_group_take(const struct vcpu *v, const struct code_dependency *deps,
                                  unsigned n, const struct code_fetch *fetch,
                                  struct code_stay_witness_group *out)
{
    __builtin_memset(__builtin_assume_aligned(out, _Alignof(struct code_stay_witness_group)), 0,
                     sizeof(*out));
    if (!n || n > CODE_STAY_WITNESS_PAGES || !fetch)
        return false;
    out->n = n;
    for (unsigned i = 0; i < n; ++i)
        if (!witness_take(v, &deps[i], fetch, i == 0, &out->page[i]))
            goto fail;
#ifdef IA64VMM_HOSTED
    code_validity_test_hook(out->page[0].page->base, CODE_TEST_WITNESS_GROUP);
#endif
    if (!witness_group_live(v, out, fetch))
        goto fail;
    return true;
fail:
    __builtin_memset(__builtin_assume_aligned(out, _Alignof(struct code_stay_witness_group)), 0,
                     sizeof(*out));
    return false;
}

bool code_stay_witness_group_reenter(struct vcpu *v, const struct code_stay_witness_group *group,
                                     const struct code_fetch *fetch)
{
    if (!witness_group_live(v, group, fetch))
        return false;
    for (unsigned i = 0; i < group->n; ++i) {
        const struct code_stay_witness *w = &group->page[i];
        *w->entered = ++*w->clock;
    }
    for (unsigned i = 0; i < group->n; ++i) {
        const struct code_stay_witness *w = &group->page[i];
        if (!atomic_load_explicit(w->entry_tick, memory_order_relaxed))
            atomic_store_explicit(w->entry_tick, 1, memory_order_relaxed);
    }
    return true;
}

bool code_dependencies_enter(struct vcpu *v, struct code_dependency *deps, unsigned n,
                             const struct code_fetch *fetch)
{
    if (n == 1)
        return code_dependency_enter(v, deps, fetch);
    if (mode == CODE_VALIDITY_NONE)
        return true;
    struct code_stay *h = &stays[v->cfg.index];
    if (n && n <= CODE_STAY_PAGES &&
        stay_live(h, v, __atomic_load_n(&storage_epoch, __ATOMIC_ACQUIRE))) {
        unsigned i = 0;
        for (; i < n; ++i) {
            const struct PageValidity *q = deps[i].page;
            if (!q || !q->ready)
                break;
            struct code_stay_page *s = stay_page(h, q);
            if (!stay_has(s, &deps[i]) || (!i && !(s->fetched && fetch_equal(&s->fetch, fetch))))
                break;
            s->entered = ++h->clock;
        }
        if (i == n) {
            for (i = 0; i < n; ++i)
                touch(deps[i].page);
            return true;
        }
    }
    return dependencies_enter_slow(v, deps, n, fetch, NULL);
}
struct ia64_result code_page_enter(struct vcpu *v, gpa_t gpa, bool region_entry)
{
    /* Lockstep replays a step the real vCPU fetched and validated just
     * before, on a private copy whose memory view is the check overlay
     * (engine/tier0/lockstep.c). Its fetch must neither start a stay nor
     * point a record's source at check-workspace memory. */
    if (mode == CODE_VALIDITY_NONE || (v->check_context && tier0_is_shadow(v)))
        return ia64_complete();
    gpa_t base = gpa & ~UINT64_C(4095);
    if (!region_entry) {
        /* Interpreter hot path: this bundle was compared in the continuing
         * stay, so it is still fresh and its decoded entry current; a
         * reused record has a new stay_generation (allocate). */
        struct code_stay *h = &stays[v->cfg.index];
        unsigned at = (unsigned)(gpa & 4095) >> 4;
        if (stay_live(h, v, __atomic_load_n(&storage_epoch, __ATOMIC_ACQUIRE)))
            for (unsigned i = 0; i < h->npages; ++i) {
                struct code_stay_page *s = &h->page[i];
                if (s->page->base == base && s->generation == s->page->stay_generation &&
                    ((s->validated[at / 64] >> (at % 64)) & 1)) {
                    s->entered = ++h->clock;
                    touch(s->page);
                    return ia64_complete();
                }
            }
    }
    /* Tier1 region entries compare their fetch translation; the
     * interpreter validates the one bundle it executes. */
    struct code_fetch fetch = {0};
    if (region_entry && (v->s.psr & IA64_PSR_IT)) {
        const struct ia64_tlb_entry *t = ia64_tlb_lookup(v, v->s.ip, 0);
        fetch = (struct code_fetch){.psr = v->s.psr,
                                    .rr = v->s.rr[v->s.ip >> 61],
                                    .pta = v->s.cr[CR_PTA],
                                    .pte = t ? t->pte : 0};
    } else if (region_entry)
        fetch.psr = v->s.psr;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        struct PageValidity *p;
        bool take = !code_lock_held();
        if (take)
            code_lock();
        struct ia64_result result = prepare(v, gpa, &p);
        if (take)
            code_unlock();
        if (result.kind != IA64_STEP_COMPLETE)
            return result;
        struct code_stay_page *slot;
        unsigned counters = code_counter_index();
        stay(v, &p, 1, region_entry, region_entry ? &fetch : NULL, &slot, counters);
        uint64_t mask[4];
        code_bundle_mask(p->base, gpa, 16, mask);
        /* Legacy Tier1 page entry remains conservative: every covered
         * bundle. Its dependencies carry all-ones masks (attach) and only
         * the bundles they read are fresh; validate() compares those. */
        if (region_entry)
            for (unsigned word = 0; word < 4; ++word)
                mask[word] |= p->covered[word];
        if (enter_mask(slot, p, mask, counters))
            return ia64_complete();
    }
    return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "code bundles changed repeatedly at entry");
}
static bool code_dependency_publish_locked(struct vcpu *v, gpa_t gpa, uint64_t version,
                                           const struct code_dependency *d)
{
    (void)v;
    if (mode == CODE_VALIDITY_NONE)
        return true;
    struct PageValidity *p = find(gpa & ~UINT64_C(4095));
    if (!p || !p->ready || p->generation != version)
        return false;
    for (unsigned word = 0; word < 4; ++word)
        if (d->mask[word] & ~p->fresh[word])
            return false;
    return validate(p, d->mask, code_counter_index());
}
static bool code_page_is_tracked_locked(gpa_t base)
{
    const struct PageValidity *p = find(base);
    /* A source read protects a provisional compile before dependency attach.
     * Abandoned readers keep that protection until a store clears freshness. */
    return p && p->tracked;
}
static bool code_page_has_dependencies_locked(gpa_t base)
{
    const struct PageValidity *p = find(base);
    return p && (p->covered[0] | p->covered[1] | p->covered[2] | p->covered[3]);
}
static struct ia64_result code_source_read_locked(struct vcpu *v, gpa_t gpa, uint64_t *low,
                                                  uint64_t *high, uint64_t *version)
{
    if (mode == CODE_VALIDITY_NONE) {
        const struct gm_region *r = ia64_physical_region(v, gpa, 16);
        if (!r)
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unmapped instruction fetch");
        *version = 0;
        if (!ia64_atomic_read16(r->hva + (size_t)(gpa - r->base), low, high))
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, gpa, "host cmpxchg16b unavailable");
        return ia64_complete();
    }
    struct PageValidity *p;
    struct ia64_result result = prepare(v, gpa, &p);
    if (result.kind != IA64_STEP_COMPLETE)
        return result;
    *version = p->generation;
    memcpy(low, p->snapshot + (gpa & 4095), 8);
    memcpy(high, p->snapshot + (gpa & 4095) + 8, 8);
    return ia64_complete();
}
static struct ia64_result code_source_read_physical_locked(gpa_t gpa, uint64_t *low, uint64_t *high,
                                                           uint64_t *version)
{
    const struct gm_region *r = source_region(gpa);
    if (mode == CODE_VALIDITY_NONE) {
        /* prepare_region's checks, without a snapshot. */
        if (!r)
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unmapped instruction fetch");
        if (r->kind != GM_RAM && r->kind != GM_FIRMWARE)
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, gpa, "instruction fetch from MMIO");
        if ((uintptr_t)(r->hva + (size_t)(gpa - r->base)) & 15)
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unaligned bundle backing");
        *version = 0;
        if (!ia64_atomic_read16(r->hva + (size_t)(gpa - r->base), low, high))
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, gpa, "host cmpxchg16b unavailable");
        return ia64_complete();
    }
    struct PageValidity *p;
    struct ia64_result result = prepare_region(NULL, r, gpa, &p);
    if (result.kind != IA64_STEP_COMPLETE)
        return result;
    *version = p->generation;
    memcpy(low, p->snapshot + (gpa & 4095), 8);
    memcpy(high, p->snapshot + (gpa & 4095) + 8, 8);
    return ia64_complete();
}
static bool code_source_publish_locked(struct vcpu *v, gpa_t gpa, uint64_t version)
{
    (void)v;
    if (mode == CODE_VALIDITY_NONE)
        return true;
    struct PageValidity *p = find(gpa & ~UINT64_C(4095));
    if (!p || !p->ready || p->generation != version)
        return false;
    uint64_t mask[4];
    code_bundle_mask(p->base, gpa, 16, mask);
    for (unsigned word = 0; word < 4; ++word)
        mask[word] |= p->fresh[word];
    return validate(p, mask, code_counter_index());
}
static uint64_t code_source_generation_locked(gpa_t gpa)
{
    struct PageValidity *p = find(gpa & ~UINT64_C(4095));
    return p ? p->generation : 0;
}
static bool code_page_has_snapshot_locked(gpa_t base)
{
    const struct PageValidity *p = find(base);
    return p && p->ready;
}
static void code_validity_invalidate_locked(gpa_t gpa, uint64_t len)
{
    if (!len)
        return;
    /* Untracking cleared every provisional snapshot (tracked()). A record
     * without those snapshots or dependents needs no generation change. */
    gpa_t last = len - 1 > UINT64_MAX - gpa ? UINT64_MAX : gpa + len - 1;
    gpa_t first_page = gpa & ~UINT64_C(4095), last_page = last & ~UINT64_C(4095);
    if ((last_page - first_page) / PAGE_BYTES < hash_size) {
        for (gpa_t base = first_page;; base += PAGE_BYTES) {
            struct PageValidity *p = find(base);
            if (p && p->tracked) {
                uint64_t mask[4];
                code_bundle_mask(base, gpa, len, mask);
                invalidate_mask(p, mask);
            }
            if (base == last_page)
                break;
        }
        return;
    }
    for (struct PageValidity *p = oldest; p; p = p->newer)
        if (p->tracked && p->base >= first_page && p->base <= last_page) {
            uint64_t mask[4];
            code_bundle_mask(p->base, gpa, len, mask);
            invalidate_mask(p, mask);
        }
}
static void code_validity_flush_locked(void)
{
    if (code_executors_registered()) {
        for (struct PageValidity *p = oldest; p; p = p->newer)
            invalidate_page(p);
        return;
    }
    while (oldest) {
        struct PageValidity *p = oldest;
        invalidate_page(p);
        oldest = p->newer;
        hal_free_pages(p->snapshot, 1);
    }
    while (slabs) {
        struct validity_slab *s = slabs;
        slabs = s->next;
        hal_free_pages(s, 1);
    }
    newest = available = clock_hand = NULL;
    bitmap_destroy();
    if (buckets != initial_buckets)
        hal_free_pages(buckets, hash_size * sizeof(*buckets) / PAGE_BYTES);
    buckets = initial_buckets;
    hash_size = HASH_SIZE;
    memset(initial_buckets, 0, sizeof(initial_buckets));
    memset(stays, 0, sizeof(stays));
    stats.snapshot_bytes = stats.metadata_bytes = 0;
    ++entry_generation;
}
bool code_validity_configure(enum code_validity_mode selected, uint64_t budget)
{
    if ((unsigned)selected > CODE_VALIDITY_HW_DIRTY || budget < PAGE_BYTES || (budget & 4095))
        return false;
    code_cache_flush();
    code_lock();
    if (code_executors_registered()) {
        code_unlock();
        return false;
    }
    bitmap_destroy();
    if (!bitmap_init() || !resize_hash(budget)) {
        bitmap_destroy();
        code_unlock();
        return false;
    }
    if (selected == CODE_VALIDITY_HW_DIRTY) {
        LOGW("code_validity=hw-dirty unavailable; falling back to compare-always");
        selected = CODE_VALIDITY_COMPARE_ALWAYS;
    }
    mode = selected;
    budget_bytes = budget;
    derived_budget = false;
    code_unlock();
    return true;
}
uint64_t code_validity_reserve_budget(uint64_t free_bytes, uint64_t reserve_bytes)
{
    /* Reserve all remaining decoded pages and bounded compile/check
     * workspaces first; half the remainder funds snapshots AND slabs. */
    uint64_t late = code_cache_reserve_bytes(), upper = engine_tier1_reserve_bytes();
    late = upper > UINT64_MAX - late ? UINT64_MAX : late + upper;
    uint64_t remaining = free_bytes > late ? free_bytes - late : 0;
    uint64_t budget = (remaining / 2) & ~UINT64_C(4095);
    uint64_t limit = vmm_cache_budget_limit(reserve_bytes);
    if (budget > limit)
        budget = limit;
    if (budget < 2 * PAGE_BYTES)
        budget = 2 * PAGE_BYTES;
    code_lock();
    if (!resize_hash(budget)) {
        uint64_t hash_limit = (uint64_t)hash_size * (CODE_SNAPSHOT_DEFAULT_BYTES / HASH_SIZE);
        if (budget > hash_limit)
            budget = hash_limit;
    }
    budget_bytes = budget;
    derived_budget = true;
    code_unlock();
    LOGI("code snapshot late_reserve_bytes=%llu", (unsigned long long)late);
    return budget;
}
bool code_validity_parse_mode(const char *value, size_t length, enum code_validity_mode *out)
{
    static const char *const names[] = {"compare-always", "epoch-compare", "none", "hw-dirty"};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(names); ++i)
        if (strlen(names[i]) == length && !strncmp(value, names[i], length)) {
            *out = (enum code_validity_mode)i;
            return true;
        }
    return false;
}
void code_epoch_bump(enum code_device_class device_class)
{
    if (device_class != CODE_DEVICE_STORAGE)
        return;
    __atomic_add_fetch(&storage_epoch, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&local_stats()->epoch_bumps, 1, __ATOMIC_RELAXED);
}
static void code_validity_get_stats_locked(struct code_validity_stats *out)
{
    *out = (struct code_validity_stats){.snapshot_bytes = stats.snapshot_bytes,
                                        .metadata_bytes = stats.metadata_bytes};
    uint64_t *dst = (void *)out;
    for (unsigned cpu = 0; cpu < 256; ++cpu) {
        const uint64_t *src = (const void *)&executor_stats[cpu].counters;
        for (unsigned word = 0; word < sizeof(*out) / sizeof(*dst); ++word)
            dst[word] += __atomic_load_n(&src[word], __ATOMIC_RELAXED);
    }
}
void code_validity_dump_stats(void)
{
    /* The management shutdown path already calls this after the executor
     * has stopped, including budget/watchdog exits with no engine dump. */
    if (tier0_arena_initialized())
        tier0_dump_stats();
    struct code_validity_stats s;
    code_validity_get_stats(&s);
    LOGI(
        "validator comparisons=%llu mismatches=%llu invalidations=%llu snapshot_bytes=%llu epoch_bumps=%llu evictions=%llu comparison_ticks=%llu counter_hz=%llu",
        (unsigned long long)s.comparisons, (unsigned long long)s.mismatches,
        (unsigned long long)s.invalidations, (unsigned long long)s.snapshot_bytes,
        (unsigned long long)s.epoch_bumps, (unsigned long long)s.evictions,
        (unsigned long long)s.comparison_ticks, (unsigned long long)hal_counter_hz());
    struct code_stay_stats st;
    code_stay_get_stats(&st);
    LOGI(
        "validator stays page=%llu reset=%llu event=%llu epoch=%llu fetch=%llu bundles_compared=%llu mmu=%llu evicted=%llu",
        (unsigned long long)st.page, (unsigned long long)st.reset, (unsigned long long)st.event,
        (unsigned long long)st.epoch, (unsigned long long)st.fetch,
        (unsigned long long)st.bundles_compared, (unsigned long long)st.mmu,
        (unsigned long long)st.evicted);
}

/* Public slow/mutation APIs compose with a caller-owned critical section. */
void code_dependency_remove(struct code_dependency *d)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    code_dependency_remove_locked(d);
    if (take)
        code_unlock();
}

void code_dependency_attach(gpa_t base, struct code_dependency *d)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    code_dependency_attach_locked(base, d);
    if (take)
        code_unlock();
}

void code_dependency_extend(struct code_dependency *d, unsigned granule)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    code_dependency_extend_locked(d, granule);
    if (take)
        code_unlock();
}

bool code_dependency_publish(struct vcpu *v, gpa_t gpa, uint64_t version,
                             const struct code_dependency *d)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool result = code_dependency_publish_locked(v, gpa, version, d);
    if (take)
        code_unlock();
    return result;
}

bool code_page_is_tracked(gpa_t base)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool result = code_page_is_tracked_locked(base);
    if (take)
        code_unlock();
    return result;
}

bool code_page_has_dependencies(gpa_t base)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool result = code_page_has_dependencies_locked(base);
    if (take)
        code_unlock();
    return result;
}

struct ia64_result code_source_read(struct vcpu *v, gpa_t gpa, uint64_t *low, uint64_t *high,
                                    uint64_t *version)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    struct ia64_result result = code_source_read_locked(v, gpa, low, high, version);
    if (take)
        code_unlock();
    return result;
}

struct ia64_result code_source_read_physical(gpa_t gpa, uint64_t *low, uint64_t *high,
                                             uint64_t *version)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    struct ia64_result result = code_source_read_physical_locked(gpa, low, high, version);
    if (take)
        code_unlock();
    return result;
}

bool code_source_publish(struct vcpu *v, gpa_t gpa, uint64_t version)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool result = code_source_publish_locked(v, gpa, version);
    if (take)
        code_unlock();
    return result;
}

uint64_t code_source_generation(gpa_t gpa)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    uint64_t result = code_source_generation_locked(gpa);
    if (take)
        code_unlock();
    return result;
}

bool code_page_has_snapshot(gpa_t base)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool result = code_page_has_snapshot_locked(base);
    if (take)
        code_unlock();
    return result;
}

void code_validity_invalidate(gpa_t gpa, uint64_t len)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    code_validity_invalidate_locked(gpa, len);
    if (take)
        code_unlock();
}

void code_validity_flush(void)
{
    code_background_quiesce();
    bool take = !code_lock_held();
    if (take)
        code_lock();
    code_validity_flush_locked();
    if (take)
        code_unlock();
    code_background_resume();
}

void code_validity_get_stats(struct code_validity_stats *out)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    code_validity_get_stats_locked(out);
    if (take)
        code_unlock();
}
