/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Region lookup compares the full entry contract in t1_region_matches;
 * value properties and frame base are excluded (stacked names are offsets).
 * Exact source-bundle dependencies retire overlapping regions on writes/DMA.
 * Code is shared; reclaim only after every registered executor passes the
 * retirement epoch, or under the unregistered single-executor pin.
 *
 * Pool allocation never evicts. Maintenance uses a second-chance clock until
 * free space plus pending reclamation covers the request. Publication sets
 * the reference bit so an unentered region is not the first victim.
 *
 * Links are (generation, slot+1). Reuse slots only after reclamation and assign
 * a fresh generation from the global 44-bit counter; stale links can alias
 * only after 2^44 allocations.
 */
#include "engine/cache/background.h"
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"

#define GEN_BITS (64u - T1_LINK_ID_BITS)
#define GEN_MASK ((UINT64_C(1) << GEN_BITS) - 1)

static struct t1_region **regions; /* [capacity], NULL = free slot */
static uint64_t *slot_gen;         /* [capacity] */
static uint32_t *free_ids;         /* stack of free ids */
static unsigned capacity, nfree_ids, nregions, hand;
static _Atomic unsigned nretired;
static uint64_t awaiting; /* pool bytes of retired records */
static _Atomic bool want_id;
static _Atomic unsigned want_chunk; /* largest refused contiguous page count */
static size_t regions_pages, gen_pages, free_pages, bucket_pages;
static uint64_t generations;
static struct t1_region *retired_list;
static struct t1_region **reclaim_cursor;
static _Atomic bool reclaim_continuation;
static _Atomic(struct t1_region *) *buckets;
static unsigned hash_size;
uint64_t t1_sequence;
static uint64_t head_sequences[T1_HEAD_SEQUENCES];
static unsigned reclaim_due;
#ifdef IA64VMM_HOSTED
void (*t1_test_region_hook)(const struct t1_region *r, unsigned event);
#define REGION_TEST(r, event)                                                                      \
    do {                                                                                           \
        if (t1_test_region_hook)                                                                   \
            t1_test_region_hook(r, event);                                                         \
    } while (0)
#else
#define REGION_TEST(r, event) ((void)0)
#endif

void t1_head_moved(uint64_t ip, unsigned slot)
{
    __atomic_add_fetch(&head_sequences[((ip >> 2) | slot) & (T1_HEAD_SEQUENCES - 1u)], 1,
                       __ATOMIC_RELEASE);
}

const uint64_t *t1_upper_heads(void)
{
    return head_sequences;
}

/* A release store: maintenance takes the word with an exchange, so it sees
 * the work recorded before the store. */
void t1_maintenance_due(void)
{
    __atomic_store_n(&reclaim_due, 1, __ATOMIC_RELEASE);
}

const unsigned *t1_upper_reclaim_due(void)
{
    return &reclaim_due;
}

static size_t pages_for(size_t bytes)
{
    return (bytes + 4095) / 4096;
}

static unsigned hash(uint64_t ip, unsigned slot)
{
    uint64_t h = (ip >> 4) * UINT64_C(0x9e3779b97f4a7c15) + slot;
    return (unsigned)(h >> 40) & (hash_size - 1);
}

bool t1_regions_init(unsigned n)
{
    if (regions)
        return true;
    if (!n || n > T1_MAX_REGIONS_LIMIT)
        return false;
    capacity = n;
    hash_size = 1024;
    while (hash_size < capacity)
        hash_size *= 2;
    regions_pages = pages_for(capacity * sizeof(*regions));
    gen_pages = pages_for(capacity * sizeof(*slot_gen));
    free_pages = pages_for(capacity * sizeof(*free_ids));
    bucket_pages = pages_for(hash_size * sizeof(*buckets));
    regions = t1_memory_alloc(regions_pages, HAL_PAGE_ZERO);
    slot_gen = t1_memory_alloc(gen_pages, HAL_PAGE_ZERO);
    free_ids = t1_memory_alloc(free_pages, HAL_PAGE_ZERO);
    buckets = t1_memory_alloc(bucket_pages, HAL_PAGE_ZERO);
    if (!regions || !slot_gen || !free_ids || !buckets) {
        t1_regions_shutdown();
        return false;
    }
    memset(regions, 0, capacity * sizeof(*regions));
    memset(slot_gen, 0, capacity * sizeof(*slot_gen));
    memset(buckets, 0, hash_size * sizeof(*buckets));
    /* Lowest ids first (popped from the end). */
    for (unsigned k = 0; k < capacity; ++k)
        free_ids[k] = capacity - k;
    nfree_ids = capacity;
    nregions = nretired = hand = 0;
    awaiting = 0;
    want_id = false;
    want_chunk = 0;
    retired_list = NULL;
    reclaim_cursor = NULL;
    atomic_store_explicit(&reclaim_continuation, false, memory_order_relaxed);
    return true;
}

void t1_regions_shutdown(void)
{
    if (regions)
        t1_memory_free(regions, regions_pages);
    if (slot_gen)
        t1_memory_free(slot_gen, gen_pages);
    if (free_ids)
        t1_memory_free(free_ids, free_pages);
    if (buckets)
        t1_memory_free(buckets, bucket_pages);
    regions = NULL;
    slot_gen = NULL;
    free_ids = NULL;
    buckets = NULL;
    capacity = nfree_ids = nregions = nretired = 0;
    retired_list = NULL;
    reclaim_cursor = NULL;
    atomic_store_explicit(&reclaim_continuation, false, memory_order_relaxed);
}

bool t1_regions_resize(unsigned n)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool resized = regions && !nregions && n && n <= T1_MAX_REGIONS_LIMIT;
    if (resized) {
        unsigned old = capacity;
        t1_regions_shutdown();
        resized = t1_regions_init(n);
        if (!resized && !t1_regions_init(old))
            hal_panic("tier1: region table lost on resize");
    }
    if (take)
        code_unlock();
    return resized;
}

static void t1_reclaim_locked(void);

static struct t1_region *t1_region_new_locked(void)
{
    if (!regions)
        return NULL;
    /* A full table: records every executor has passed, else a request. */
    if (!nfree_ids)
        t1_reclaim_locked();
    if (!nfree_ids) {
        t1_request_region_id();
        return NULL;
    }
    struct t1_region *r = t1_memory_alloc(pages_for(sizeof(*r)), HAL_PAGE_ZERO);
    if (!r)
        return NULL;
    memset(r, 0, sizeof(*r));
    uint32_t id = free_ids[--nfree_ids];
    __atomic_store_n(&regions[id - 1], r, __ATOMIC_RELEASE);
    if (!(generations = (generations + 1) & GEN_MASK))
        generations = 1;
    __atomic_store_n(&slot_gen[id - 1], generations, __ATOMIC_RELEASE);
    __atomic_store_n(&nregions, nregions + 1, __ATOMIC_RELAXED); /* read by t1_region_counts */
    r->id = id;
    r->link = (uint64_t)generations << T1_LINK_ID_BITS | id;
    return r;
}

static struct t1_region *t1_region_get_locked(uint32_t id)
{
    return id && id <= capacity ? regions[id - 1] : NULL;
}

/* Without code_lock: a retirement clears
 * the slot's generation before it stamps its epoch, so a generation that
 * still matches means the record was not retired when it was read; its
 * reclamation then waits for the caller's next quiescent publication.
 * The record is read only after the match. */
struct t1_region *t1_region_from_link(uint64_t link)
{
    uint32_t id = (uint32_t)(link & ((UINT64_C(1) << T1_LINK_ID_BITS) - 1));
    if (!id || id > capacity)
        return NULL;
    if (__atomic_load_n(&slot_gen[id - 1], __ATOMIC_SEQ_CST) != link >> T1_LINK_ID_BITS)
        return NULL;
    struct t1_region *r = __atomic_load_n(&regions[id - 1], __ATOMIC_ACQUIRE);
    return r && r->valid ? r : NULL;
}

static void unlink_region(struct t1_region *r)
{
    _Atomic(struct t1_region *) *p = &buckets[hash(r->c.ip, r->c.slot)];
    struct t1_region *next;
    while ((next = atomic_load_explicit(p, memory_order_relaxed)) && next != r)
        p = &next->hash_next;
    if (next)
        atomic_store_explicit(p, atomic_load_explicit(&r->hash_next, memory_order_relaxed),
                              memory_order_release);
    /* Old epoch readers can still hold r and need its successor. */
}

/* The pool bytes a record holds with its tables. */
static uint64_t region_bytes(const struct t1_region *r)
{
    return (pages_for(sizeof(*r)) + (r->meta ? r->meta_pages : 0)) * 4096;
}

static void retire(struct t1_region *r)
{
    r->valid = false;
    t1_retired_running(r);
    r->retired = true;
    /* Both resolver paths stop finding r before its epoch is stamped. */
    unlink_region(r);
    __atomic_store_n(&slot_gen[r->id - 1], 0, __ATOMIC_SEQ_CST);
    r->retirement = code_retire_epoch();
    REGION_TEST(r, T1_REGION_TEST_RETIRED);
    r->retired_next = retired_list;
    /* Preserve an unfinished scan when retirements prepend new work. */
    if (retired_list && reclaim_cursor == &retired_list)
        reclaim_cursor = &r->retired_next;
    retired_list = r;
    ++nretired;
    if (!t1_cfg.submit_reclaim || nretired >= T1_RECLAIM_POST)
        t1_maintenance_due();
    awaiting += region_bytes(r);
    for (unsigned p = 0; p < r->npages; ++p)
        if (r->dep_attached[p]) {
            code_dependency_remove(&r->dep[p]);
            r->dep_attached[p] = false;
        }
}

static void t1_region_retire_locked(struct t1_region *r)
{
    if (!r->valid)
        return;
    retire(r);
    T1_COUNT(invalidations, 1);
}

/* Recompiles after eviction: the contracts of
 * evicted regions, direct-mapped by hash. A later publication of the same
 * contract counts as a recompile; a collision overwrites, so the count is a
 * lower bound. */
#define EVICTED_KEYS 1024u
static uint64_t evicted[EVICTED_KEYS];
static uint64_t contract_key(const struct t1_contract *c)
{
    uint64_t h = ((uint64_t)c->cpu_model << 51) ^ c->ip ^ (c->entry_pa << 7) ^
                 (c->cfm * UINT64_C(0x9e3779b97f4a7c15)) ^ (c->psr << 3) ^
                 ((uint64_t)c->slot << 1) ^ ((uint64_t)c->flags << 59) ^ c->icount ^
                 (c->hostwin_bits << 45);
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    return h | 1;
}

static void t1_region_evict_locked(struct t1_region *r)
{
    if (!r->valid)
        return;
    uint64_t k = contract_key(&r->c);
    evicted[k % EVICTED_KEYS] = k;
    retire(r);
    T1_COUNT(evictions, 1);
}

static void t1_region_demote_locked(struct t1_region *r)
{
    if (!r->valid)
        return;
    retire(r);
    T1_COUNT(demotions, 1);
}

/* O(1) retirement from the dependency: dep[k] lies inside its region. */
#define DEP_INVALIDATE(k)                                                                          \
    static void dep_invalidate##k(struct code_dependency *d)                                       \
    {                                                                                              \
        t1_region_retire((struct t1_region *)((uint8_t *)d - offsetof(struct t1_region, dep[k]))); \
    }
DEP_INVALIDATE(0)
DEP_INVALIDATE(1)
DEP_INVALIDATE(2)
DEP_INVALIDATE(3)
_Static_assert(T1_MAX_PAGES == 4, "one dependency callback per page index");
static void (*const dep_invalidate[T1_MAX_PAGES])(struct code_dependency *) = {
    dep_invalidate0,
    dep_invalidate1,
    dep_invalidate2,
    dep_invalidate3,
};

static void free_region(struct t1_region *r)
{
    t1_arena_release(r);
    if (r->meta)
        t1_memory_free(r->meta, r->meta_pages);
    __atomic_store_n(&regions[r->id - 1], NULL, __ATOMIC_RELAXED);
    free_ids[nfree_ids++] = r->id;
    __atomic_store_n(&nregions, nregions - 1, __ATOMIC_RELAXED);
    t1_memory_free(r, pages_for(sizeof(*r)));
}

static void t1_region_discard_locked(struct t1_region *r)
{
    /* Unpublished regions only (compile failures): never linked, never on
     * the retired list. */
    if (r->valid || r->retired)
        hal_panic("tier1: discard of a published region");
    free_region(r);
}

static void t1_reclaim_locked(void)
{
    if (!nretired)
        return;
    /* Registered executors: each must have passed the retirement epoch,
     * whoever reclaims, including an executor between a region's exit and
     * its materialization. Unregistered fixtures run one executor: its
     * execution pin. */
    bool registered = code_executors_registered();
    if (!registered && t1_any_current())
        return;
    bool background = t1_background_work();
    unsigned budget = background ? 64u : UINT32_MAX;
    /* Registration and retirement share code_lock. Sampled quiescence
     * stays sufficient for these retirements; parked executors hold no
     * code pointers when they resume. */
    uint64_t horizon = registered ? code_epoch_horizon() : UINT64_MAX;
    struct t1_region **previous = background && reclaim_cursor ? reclaim_cursor : &retired_list;
    while (*previous && budget) {
        struct t1_region *r = *previous;
        --budget;
        if (r->retirement > horizon) {
            previous = &r->retired_next;
            continue;
        }
        *previous = r->retired_next;
        T1_COUNT(reclaimed, 1);
        --nretired;
        awaiting -= region_bytes(r);
        free_region(r);
    }
    /* The cursor always names the head or a retained record's next link.
     * Full passes include the prefix skipped by a bounded helper pass. */
    reclaim_cursor = *previous ? previous : NULL;
    /* Scheduling reads this hint without code_lock; the cursor stays locked. */
    atomic_store_explicit(&reclaim_continuation, reclaim_cursor != NULL, memory_order_relaxed);
}

static bool t1_region_evict_one_locked(void)
{
    for (unsigned n = 0; nregions && n < 2 * capacity; ++n) {
        struct t1_region *r = regions[hand];
        hand = (hand + 1) % capacity;
        if (!r || !r->valid)
            continue;
        if (r->referenced) {
            r->referenced = false;
            continue;
        }
        t1_region_evict_locked(r);
        return true;
    }
    return false;
}

/* Clears the requests the current room satisfies. give_up: also those
 * eviction can no longer cover (a later refusal records them again).
 * Returns the pressure that remains (t1_memory_pressure). */
static unsigned settle_locked(bool give_up)
{
    uint64_t want = t1_memory_wanted();
    if (want &&
        (t1_memory_headroom() >= want || (give_up && t1_memory_headroom() + awaiting < want)))
        t1_memory_satisfied(want);
    if (want_id && (nfree_ids || (give_up && !nretired)))
        want_id = false;
    if (want_chunk && (t1_arena_has_space(want_chunk) || (give_up && !nretired)))
        want_chunk = 0;
    want = t1_memory_wanted();
    return want_id                               ? 1u + T1_DEFER_ID
           : want_chunk                          ? 1u + T1_DEFER_CHUNK
           : want && t1_memory_headroom() < want ? 1u + T1_DEFER_BYTES
                                                 : 0;
}

/* Batch ids because eviction must pass retirement before a slot is reusable;
 * one eviction per pass would repeatedly defer compilation. */
static unsigned id_batch(void)
{
    return capacity / 64 ? capacity / 64 : 1;
}

static void maintain_locked(void)
{
    t1_reclaim_locked();
    if (!settle_locked(false))
        return;
    while (want_id && nfree_ids + nretired < id_batch() && t1_region_evict_one_locked()) {
    }
    if (want_chunk && !t1_arena_has_space(want_chunk))
        t1_arena_recycle_one();
    uint64_t want = t1_memory_wanted();
    while (want && t1_memory_headroom() + awaiting < want && t1_region_evict_one_locked()) {
    }
    t1_reclaim_locked();
    settle_locked(true);
}

void t1_flush(void)
{
    code_background_quiesce();
    bool take = !code_lock_held();
    if (take)
        code_lock();
    for (unsigned k = 0; k < capacity; ++k)
        if (regions[k])
            t1_region_retire(regions[k]);
    if (code_executors_registered() || t1_any_current())
        t1_reclaim_locked();
    else
        while (nretired)
            t1_reclaim_locked();
    t1_holds_clear();
    if (take)
        code_unlock();
    code_background_resume();
}

bool t1_region_matches_at(const struct t1_region *r, const struct vcpu *v, uint32_t flags,
                          unsigned bits, uint64_t ip, unsigned slot)
{
    const struct t1_contract *c = &r->c;
    const struct ia64_state *s = &v->s;
    return c->cpu_model == v->cfg.cpu_model && c->ip == ip && c->slot == slot &&
           c->psr == (s->psr & c->psr_mask) && c->cfm == (s->cfm & c->cfm_mask) &&
           c->abi == T1_ABI_VERSION && c->icount == v->cfg.itc_icount && c->hostwin_bits == bits &&
           c->flags == flags;
}

bool t1_region_matches(const struct t1_region *r, const struct vcpu *v, uint32_t flags,
                       unsigned bits)
{
    return t1_region_matches_at(r, v, flags, bits, v->s.ip, v->s.slot);
}

static struct t1_region *t1_region_lookup_locked(const struct vcpu *v,
                                                 const struct t1_compile_options *opt,
                                                 unsigned hostwin_bits, struct t1_region *after)
{
    /* A retired record's saved chain belongs only to its epoch readers. */
    if (after && !after->valid)
        return NULL;
    struct t1_region *r = atomic_load_explicit(
        after ? &after->hash_next : &buckets[hash(v->s.ip, v->s.slot)], memory_order_acquire);
    for (; r; r = atomic_load_explicit(&r->hash_next, memory_order_acquire))
        if (r->valid && t1_region_matches(r, v, opt->flags, hostwin_bits))
            return r;
    return NULL;
}

static bool t1_region_publish_locked(struct t1_region *r)
{
    /* code_dependency_publish needs no vCPU (it reads none): publication
     * may run on a thread that is not an executor (tier1.h). Each
     * dependency carries its page's exact bundle mask (compile.c
     * source_masks), checked here a last time and attached with it, so
     * writes retire the region only when they overlap its bundles. */
    for (unsigned p = 0; p < r->npages; ++p)
        if (!code_dependency_publish(NULL, r->page[p].pa, r->page[p].generation, &r->dep[p]))
            return false;
    for (unsigned p = 0; p < r->npages; ++p) {
        r->dep[p].invalidate = dep_invalidate[p];
        code_dependency_attach(r->page[p].pa, &r->dep[p]);
        r->dep_attached[p] = true;
    }
    r->valid = true;
    /* Not the clock's first victim before anyone could enter it. */
    r->referenced = true;
    unsigned h = hash(r->c.ip, r->c.slot);
    atomic_store_explicit(&r->hash_next, atomic_load_explicit(&buckets[h], memory_order_relaxed),
                          memory_order_relaxed);
    REGION_TEST(r, T1_REGION_TEST_PUBLISH);
    atomic_store_explicit(&buckets[h], r, memory_order_release);
    uint64_t k = contract_key(&r->c);
    if (evicted[k % EVICTED_KEYS] == k) {
        evicted[k % EVICTED_KEYS] = 0;
        T1_COUNT(recompiles, 1);
    }
    T1_COUNT(published_bytes, region_bytes(r));
    t1_head_moved(r->c.ip, r->c.slot);
    return true;
}

/* ---- point enumeration (tier1.h) ---- */

unsigned t1_point_count(uint32_t region_id)
{
    const struct t1_region *r = t1_region_get(region_id);
    return r ? r->nlive_points : 0;
}

bool t1_point_get(uint32_t region_id, unsigned index, struct t1_point_info *out)
{
    const struct t1_region *r = t1_region_get(region_id);
    if (!r)
        return false;
    for (uint32_t k = 0; k < r->npoints; ++k) {
        const struct t1_point *p = &r->point[k];
        if (p->kind == T1_DEAD_POINT)
            continue;
        if (index--)
            continue;
        *out = (struct t1_point_info){k,
                                      (enum t1_point_kind)p->kind,
                                      p->ip,
                                      p->slot,
                                      p->insn,
                                      p->access_size,
                                      p->access_write != 0,
                                      p->end};
        return true;
    }
    return false;
}

#ifdef IA64VMM_HOSTED
bool t1_test_region_code(uint32_t region_id, const uint8_t **code, size_t *size, size_t *loop_start,
                         size_t *loop_end)
{
    const struct t1_region *r = t1_region_get(region_id);
    if (!r || !r->code)
        return false;
    *code = r->code;
    *size = r->code_size;
    *loop_start = r->loop_start;
    *loop_end = r->loop_end;
    return true;
}
#endif

struct t1_region *t1_region_new(void)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    struct t1_region *result = t1_region_new_locked();
    if (take)
        code_unlock();
    return result;
}

struct t1_region *t1_region_get(uint32_t id)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    struct t1_region *result = t1_region_get_locked(id);
    if (take)
        code_unlock();
    return result;
}

void t1_region_retire(struct t1_region *r)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    t1_region_retire_locked(r);
    if (take)
        code_unlock();
}

void t1_region_evict(struct t1_region *r)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    t1_region_evict_locked(r);
    if (take)
        code_unlock();
}

void t1_region_demote(struct t1_region *r)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    t1_region_demote_locked(r);
    if (take)
        code_unlock();
}

void t1_region_discard(struct t1_region *r)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    t1_region_discard_locked(r);
    if (take)
        code_unlock();
}

void t1_reclaim(void)
{
    /* Taken before the check: work recorded after it sets the word again. */
    __atomic_exchange_n(&reclaim_due, 0, __ATOMIC_ACQ_REL);
    if (!t1_reclaim_pending())
        return;
    bool take = !code_lock_held();
    if (take)
        code_lock();
    maintain_locked();
    if (take)
        code_unlock();
    if (t1_maintenance_wanted())
        t1_maintenance_due();
}

void t1_reclaim_passed(void)
{
    if (!nretired)
        return;
    bool take = !code_lock_held();
    if (take)
        code_lock();
    t1_reclaim_locked();
    if (take)
        code_unlock();
}

unsigned t1_memory_pressure(void)
{
    if (!t1_memory_wanted() && !want_id && !want_chunk)
        return 0;
    bool take = !code_lock_held();
    if (take)
        code_lock();
    t1_reclaim_locked();
    unsigned pressure = settle_locked(false);
    if (take)
        code_unlock();
    return pressure;
}

void t1_region_counts(uint64_t *records, uint64_t *table)
{
    *records = __atomic_load_n(&nregions, __ATOMIC_RELAXED);
    *table = __atomic_load_n(&capacity, __ATOMIC_RELAXED);
}

void t1_request_region_id(void)
{
    want_id = true;
    t1_maintenance_due();
}
void t1_request_code_chunk(unsigned pages)
{
    if (!pages || pages > T1_CHUNK_BYTES / 4096)
        hal_panic("tier1: invalid code span request");
    if (pages > want_chunk)
        want_chunk = pages;
    t1_maintenance_due();
}

void t1_code_chunk_satisfied(unsigned pages)
{
    if (want_chunk && pages >= want_chunk)
        want_chunk = 0;
}

bool t1_region_evict_one(void)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool result = t1_region_evict_one_locked();
    if (take)
        code_unlock();
    return result;
}

struct t1_region *t1_region_lookup(const struct vcpu *v, const struct t1_compile_options *opt,
                                   unsigned hostwin_bits, struct t1_region *after)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    struct t1_region *result = t1_region_lookup_locked(v, opt, hostwin_bits, after);
    if (take)
        code_unlock();
    return result;
}

/* Unlink precedes the SC retirement stamp. A reader's SC epoch sample
 * acquires every earlier unlink, so its acquire chain loads cannot reach
 * records already reclaimable at that sample. Later retirements wait for
 * its next boundary, including records reached through saved next links.
 * Contracts stay immutable after release publication. Table replacement
 * and shutdown require stopped executors. */
struct t1_region *t1_region_probe(const struct vcpu *v, const struct t1_compile_options *opt,
                                  unsigned hostwin_bits, gpa_t entry_pa)
{
    bool take = !code_lock_held() &&
                (!code_vcpu_is_registered(v) || __atomic_load_n(&v->parked, __ATOMIC_SEQ_CST));
    if (take)
        code_lock();
    struct t1_region *r =
        atomic_load_explicit(&buckets[hash(v->s.ip, v->s.slot)], memory_order_acquire);
    for (; r; r = atomic_load_explicit(&r->hash_next, memory_order_acquire)) {
        REGION_TEST(r, T1_REGION_TEST_PROBE);
        if (r->valid && t1_region_matches(r, v, opt->flags, hostwin_bits) &&
            r->c.entry_pa == entry_pa)
            break;
    }
    if (take)
        code_unlock();
    return r;
}

uint64_t t1_region_psr_difference(const struct vcpu *v, uint32_t flags, unsigned hostwin_bits)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    uint64_t difference = 0;
    for (const struct t1_region *r =
             atomic_load_explicit(&buckets[hash(v->s.ip, v->s.slot)], memory_order_acquire);
         r && !difference; r = atomic_load_explicit(&r->hash_next, memory_order_acquire)) {
        const struct t1_contract *c = &r->c;
        if (r->valid && c->cpu_model == v->cfg.cpu_model && c->ip == v->s.ip &&
            c->slot == v->s.slot && c->cfm == (v->s.cfm & c->cfm_mask) &&
            c->abi == T1_ABI_VERSION && c->icount == v->cfg.itc_icount &&
            c->hostwin_bits == hostwin_bits && c->flags == flags)
            difference = c->psr ^ (v->s.psr & c->psr_mask);
    }
    if (take)
        code_unlock();
    return difference;
}

bool t1_region_publish(struct t1_region *r)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool result = t1_region_publish_locked(r);
    if (take)
        code_unlock();
    return result;
}

bool t1_reclaim_pending(void)
{
    return nretired || t1_memory_wanted() || want_id || want_chunk;
}

bool t1_maintenance_wanted(void)
{
    return t1_memory_wanted() || want_id || want_chunk ||
           atomic_load_explicit(&reclaim_continuation, memory_order_relaxed) ||
           nretired >= (t1_cfg.submit_reclaim ? T1_RECLAIM_POST : 1u);
}

bool t1_epoch_reclaim(void)
{
    return t1_cfg.submit || code_executors_registered();
}

/* Submitted compilation uses background placement and reclamation.
 * Registered executors also require epoch reclamation without submission.
 * Inline relocation and immediate reclamation need one unregistered executor. */
bool t1_background_work(void)
{
    return t1_cfg.submit != NULL;
}
