/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The Tier1 pool: the reserve-derived cap,
 * the workspace reservation, refusal without eviction, maintenance that
 * evicts until the request is covered, reclamation after every registered
 * executor's quiescence, a compile context's workspace block, and the
 * deferral of a compile refused memory at any of its allocation sites. */
#include <stdio.h>
#include <string.h>
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"
#include "tests/tier1/harness.h"
#include "tests/tier1/provider.h"
#include "tests/tier1/programs.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/results.h"
#include "tests/tier1/snapshot.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/memory_policy.h"

/* Without a backoff the harness asks every few back edges; with one, a
 * handful of times over the program. */
#define HARNESS_DEFERRALS_MIN 32u

static struct t1_memory_stats usage(void)
{
    struct t1_memory_stats s;
    t1_memory_get_stats(&s);
    return s;
}

static uint64_t bytes_of(size_t pages)
{
    return (uint64_t)pages * 4096;
}
static size_t record_pages(void)
{
    return (sizeof(struct t1_region) + 4095) / 4096;
}

/* A cap that leaves `bytes` of metadata room above the current use (a
 * context's block within the reservation leaves the room unchanged). */
static void metadata_room(uint64_t bytes)
{
    struct t1_memory_stats s = usage();
    T1H_CHECK(s.workspace_bytes <= s.reserve_bytes);
    t1_memory_set_limit(s.used_bytes - s.workspace_bytes + s.reserve_bytes + bytes);
}

/* One compile context's block: t1_compile_workspace_bytes in pages. */
static uint64_t block_bytes(void)
{
    return (t1_compile_workspace_bytes() + 4095) / 4096 * 4096;
}

/* A published region with `meta` restore-table pages and no code. */
static struct t1_region *published(unsigned meta)
{
    struct t1_region *r = t1_region_new();
    T1H_CHECK(r);
    r->meta_pages = meta;
    r->meta = t1_memory_alloc(meta, HAL_PAGE_ZERO);
    T1H_CHECK(r->meta && t1_region_publish(r));
    return r;
}

/* Records freed so far; a record not yet counted stays allocated. */
static uint64_t reclaimed(void)
{
    struct t1_stats s;
    t1_get_stats(&s);
    return s.reclaimed;
}

static void end_pressure(void)
{
    t1_memory_set_limit(48u << 20);
    t1_reclaim();
    T1H_CHECK(!t1_memory_pressure());
}

/* The cap is three quarters of the free reserve left after the snapshot
 * budget and the lower tiers' lazy allowances, bounded by the reserve-derived
 * limit (48 MiB when capacity is unknown), above what
 * Tier1 already holds; re-initialization keeps it. */
static void budget(void)
{
    const uint64_t mib = UINT64_C(1) << 20, gib = UINT64_C(1) << 30;
    const struct {
        uint64_t reserve, allowance;
    } limits[] = {
        {0, 48 * mib},
        {1, 48 * mib},
        {384 * mib - 1, 48 * mib},
        {384 * mib, 48 * mib},
        {384 * mib + 32767, 48 * mib},
        {384 * mib + 32768, 48 * mib + 4096},
        {512 * mib, 64 * mib},
        {gib, 128 * mib},
        {gib + 32767, 128 * mib},
        {gib + 32768, 128 * mib + 4096},
        {1022 * mib, 127 * mib + 3 * mib / 4},
        {8 * gib - 1, gib - 4096},
        {8 * gib, gib},
        {UINT64_MAX, gib},
    };
    uint64_t used = usage().used_bytes, workspace = usage().reserve_bytes;
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(limits); ++k) {
        T1H_CHECK(t1_memory_allowance(UINT64_MAX, 0, limits[k].reserve) == limits[k].allowance);
        t1_memory_configure(UINT64_MAX, 0, limits[k].reserve);
        T1H_CHECK(usage().budget_bytes == used + limits[k].allowance);
        T1H_CHECK(usage().reserve_bytes == workspace);
    }
    T1H_CHECK(vmm_cache_budget_limit(gib) == 64 * mib);
    T1H_CHECK(vmm_tier1_code_bytes(gib) == 32 * mib);
    T1H_CHECK(t1_memory_allowance(5460, 0, gib) == 0);
    T1H_CHECK(t1_memory_allowance(5461, 0, gib) == 4096);
    T1H_CHECK(t1_memory_allowance(UINT64_MAX, UINT64_MAX - 5461, gib) == 4096);
    t1_memory_configure(16u << 20, 12u << 20, 0);
    T1H_CHECK(usage().budget_bytes == used + (3u << 20));
    t1_memory_configure(16u << 20, 16u << 20, 0);
    T1H_CHECK(usage().budget_bytes == used);
    T1H_CHECK(!t1_memory_alloc(1, 0) && !t1_workspace_alloc(1, 0));
    t1_memory_configure(16u << 20, UINT64_MAX, 0);
    T1H_CHECK(usage().budget_bytes == used);
    t1_memory_configure(UINT64_C(1) << 40, 0, 0);
    T1H_CHECK(usage().budget_bytes == used + (48u << 20));
    t1_memory_configure(UINT64_C(64) << 20, UINT64_C(32) << 20, 0);
    t1_memory_configure(UINT64_C(8) << 30, 0, UINT64_C(16) << 30);
    T1H_CHECK(usage().budget_bytes == used + (UINT64_C(1) << 30));
    t1_memory_configure(UINT64_C(512) << 20, 0, UINT64_C(1) << 30);
    T1H_CHECK(usage().budget_bytes == used + (UINT64_C(128) << 20));
    t1_memory_configure(UINT64_C(32) << 20, UINT64_C(16) << 20, UINT64_C(16) << 30);
    T1H_CHECK(usage().budget_bytes == used + (UINT64_C(12) << 20));
    uint64_t configured = usage().budget_bytes;
    t1_shutdown();
    T1H_CHECK(t1_init(NULL));
    T1H_CHECK(usage().budget_bytes == configured);
    end_pressure();
}

/* Metadata stops short of the workspace reservation; the reservation
 * stays available to a compile. A refusal is counted and recorded. */
static void reservation(void)
{
    struct t1_memory_stats s = usage();
    T1H_CHECK(s.reserve_bytes >= t1_compile_workspace_bytes() && !s.workspace_bytes);
    metadata_room(bytes_of(8));
    void *m = t1_memory_alloc(8, 0);
    T1H_CHECK(m && !t1_memory_alloc(1, 0));
    T1H_CHECK(t1_memory_wanted() == 4096 && usage().refusals == s.refusals + 1);
    T1H_CHECK(t1_memory_pressure());
    size_t pages = s.reserve_bytes / 4096;
    void *w = t1_workspace_alloc(pages, 0);
    T1H_CHECK(w && usage().workspace_bytes == s.reserve_bytes);
    T1H_CHECK(!t1_workspace_alloc(1, 0));
    t1_workspace_free(w, pages);
    t1_memory_free(m, 8);
    T1H_CHECK(usage().used_bytes == s.used_bytes && !usage().workspace_bytes);
    T1H_CHECK(!t1_memory_pressure() && !t1_memory_wanted());
    end_pressure();
}

/* The pool counts what it holds: records allocated, and the bytes of the
 * records and tables each publication adds. */
static void region_gauges(void)
{
    struct t1_stats s0, s1, s2;
    t1_get_stats(&s0);
    T1H_CHECK(s0.region_capacity == t1_cfg.max_regions);
    struct t1_region *r = published(3);
    t1_get_stats(&s1);
    T1H_CHECK(s1.regions == s0.regions + 1 && s1.region_capacity == s0.region_capacity);
    T1H_CHECK(s1.published_bytes == s0.published_bytes + bytes_of(record_pages() + 3));
    T1H_CHECK(s1.memory_workspace == usage().workspace_bytes);
    t1_region_retire(r);
    t1_reclaim();
    t1_get_stats(&s2);
    T1H_CHECK(s2.regions == s0.regions && s2.published_bytes == s1.published_bytes);
}

/* A full region table with registered executors: a record refused for want
 * of an id evicts nothing and leaves an outstanding id request (pressure of
 * kind id); one maintenance pass evicts a 64th of the table's cold regions,
 * so after the retirement epoch that many records can be allocated, not
 * one. */
static void region_ids(void)
{
    enum { CAPACITY = 256, BATCH = CAPACITY / 64 };
    struct t1_config c = t1_cfg;
    c.max_regions = CAPACITY;
    t1_shutdown();
    T1H_CHECK(t1_init(&c));
    uint64_t base = usage().used_bytes;
    struct vcpu_config cfg = {0};
    struct vcpu *v = vcpu_create(&cfg);
    T1H_CHECK(v);
    code_register_vcpu(v);
    static uint64_t link[CAPACITY];
    for (unsigned k = 0; k < CAPACITY; ++k) {
        struct t1_region *r = published(1);
        r->referenced = false;
        link[k] = t1_region_link(r);
    }
    T1H_CHECK(!t1_region_new());
    T1H_CHECK(t1_memory_pressure() == 1 + T1_DEFER_ID && t1_reclaim_pending());
    for (unsigned k = 0; k < CAPACITY; ++k)
        T1H_CHECK(t1_region_from_link(link[k]));
    uint64_t freed = reclaimed();
    t1_reclaim();
    unsigned evicted = 0;
    for (unsigned k = 0; k < CAPACITY; ++k)
        evicted += !t1_region_from_link(link[k]);
    T1H_CHECK(evicted == BATCH);
    /* The evicted records wait for the executor's quiescence. */
    T1H_CHECK(reclaimed() == freed);
    T1H_CHECK(!t1_region_new() && t1_memory_pressure() == 1 + T1_DEFER_ID);
    code_quiescent(v);
    t1_reclaim();
    T1H_CHECK(!t1_memory_pressure());
    struct t1_region *fresh[BATCH];
    for (unsigned k = 0; k < BATCH; ++k)
        T1H_CHECK((fresh[k] = t1_region_new()) != NULL);
    T1H_CHECK(!t1_region_new());
    for (unsigned k = 0; k < BATCH; ++k)
        t1_region_discard(fresh[k]);
    code_unregister_vcpu(v);
    vcpu_destroy(v);
    t1_flush();
    T1H_CHECK(usage().used_bytes == base);
    struct t1_config d = t1_cfg;
    d.max_regions = 0;
    t1_shutdown();
    T1H_CHECK(t1_init(&d) && t1_cfg.max_regions == T1_MAX_REGIONS_DEFAULT);
    end_pressure();
}

/* Maintenance posted to helpers (t1_config.submit_reclaim): the executor
 * posts it, and the maintenance word asks for it, only for an outstanding
 * request or once T1_RECLAIM_POST records await reclamation, not for every
 * retirement. Inline maintenance (no helpers) runs for every retirement. */
static unsigned maintenance_posts;
static void count_post(void)
{
    ++maintenance_posts;
}
static void post_threshold(void)
{
    uint64_t base = usage().used_bytes;
    void (*saved)(void) = t1_cfg.submit_reclaim;
    t1_reclaim();
    T1H_CHECK(!*t1_upper_reclaim_due());
    t1_cfg.submit_reclaim = count_post;
    maintenance_posts = 0;
    for (unsigned k = 0; k + 1 < T1_RECLAIM_POST; ++k)
        t1_region_retire(published(1));
    t1_upper_reclaim();
    T1H_CHECK(!maintenance_posts && !*t1_upper_reclaim_due());
    t1_region_retire(published(1));
    T1H_CHECK(*t1_upper_reclaim_due());
    t1_upper_reclaim();
    T1H_CHECK(maintenance_posts == 1);
    t1_reclaim();
    T1H_CHECK(!*t1_upper_reclaim_due() && usage().used_bytes == base);
    metadata_room(0);
    T1H_CHECK(!t1_memory_alloc(1, 0) && *t1_upper_reclaim_due());
    t1_upper_reclaim();
    T1H_CHECK(maintenance_posts == 2);
    end_pressure();
    t1_cfg.submit_reclaim = saved;
    t1_region_retire(published(1));
    T1H_CHECK(*t1_upper_reclaim_due());
    t1_reclaim();
    T1H_CHECK(!*t1_upper_reclaim_due() && usage().used_bytes == base);
}

static bool reject_submit(const struct t1_compile_request *request, void *context)
{
    (void)request;
    (void)context;
    return false;
}

/* Only the oldest record is permitted here: the others were retired after
 * quiescence. A helper pass must bound visits, then resume behind the
 * blocked prefix even when more retirements arrive at its head. */
static void bounded_reclamation(void)
{
    uint64_t base = usage().used_bytes, freed = reclaimed();
    struct vcpu_config cfg = {0};
    struct vcpu *v = vcpu_create(&cfg);
    T1H_CHECK(v);
    code_register_vcpu(v);
    struct t1_region *old = published(1);
    unsigned old_id = old->id;
    uint64_t old_link = old->link;
    t1_region_retire(old);
    code_quiescent(v);
    for (unsigned k = 0; k < 128; ++k)
        t1_region_retire(published(1));
    bool (*saved)(const struct t1_compile_request *, void *) = t1_cfg.submit;
    t1_cfg.submit = reject_submit;
    t1_reclaim_passed();
    T1H_CHECK(reclaimed() == freed && t1_region_get(old_id) == old);
    for (unsigned k = 0; k < 64; ++k)
        t1_region_retire(published(1));
    t1_reclaim_passed();
    T1H_CHECK(reclaimed() == freed && t1_region_get(old_id) == old);
    for (unsigned k = 0; k < 64; ++k)
        t1_region_retire(published(1));
    t1_reclaim_passed();
    T1H_CHECK(reclaimed() == freed + 1 && !t1_region_get(old_id));
    /* Reusing the reclaimed id must not revive its retired link. */
    struct t1_region *fresh = published(1);
    T1H_CHECK(fresh->id == old_id && !t1_region_from_link(old_link));
    t1_reclaim_passed();
    T1H_CHECK(reclaimed() == freed + 1);
    code_quiescent(v);
    /* A synchronous full pass starts at the head, including records before
     * the helper's saved scan position. */
    t1_cfg.submit = saved;
    t1_reclaim_passed();
    T1H_CHECK(reclaimed() == freed + 257);
    /* A batch that frees every visited record leaves its position at the
     * head. New blocked retirements must not displace that position. */
    t1_cfg.submit = reject_submit;
    for (unsigned k = 0; k < 130; ++k)
        t1_region_retire(published(1));
    code_quiescent(v);
    t1_reclaim_passed();
    T1H_CHECK(reclaimed() == freed + 321);
    for (unsigned k = 0; k < 64; ++k)
        t1_region_retire(published(1));
    t1_reclaim_passed();
    T1H_CHECK(reclaimed() == freed + 385);
    for (unsigned k = 0; k < 64; ++k)
        t1_region_retire(published(1));
    t1_reclaim_passed();
    T1H_CHECK(reclaimed() == freed + 387);
    code_unregister_vcpu(v);
    vcpu_destroy(v);
    /* Unregistered flush drains all batches even with helpers configured. */
    t1_flush();
    t1_cfg.submit = saved;
    T1H_CHECK(usage().used_bytes == base);
}

/* Once a helper starts a scan it finishes it even below the posting
 * threshold. One blocked head plus 64 eligible records leaves an eligible
 * tail after the first 64 visits; the second pass must not keep reposting
 * the blocked head alone. */
static void reclamation_continuation(void)
{
    uint64_t base = usage().used_bytes, freed = reclaimed();
    t1_submit_fn saved_submit = t1_cfg.submit;
    void (*saved_post)(void) = t1_cfg.submit_reclaim;
    t1_cfg.submit = reject_submit;
    t1_cfg.submit_reclaim = count_post;
    maintenance_posts = 0;
    struct vcpu_config cfg = {0};
    struct vcpu *v = vcpu_create(&cfg);
    T1H_CHECK(v);
    code_register_vcpu(v);
    unsigned oldest = 0;
    for (unsigned k = 0; k < 64; ++k) {
        struct t1_region *r = published(1);
        if (!k)
            oldest = r->id;
        t1_region_retire(r);
    }
    code_quiescent(v);
    struct t1_region *young = published(1);
    unsigned young_id = young->id;
    t1_region_retire(young);
    t1_reclaim();
    T1H_CHECK(reclaimed() == freed + 63 && t1_region_get(oldest) &&
              t1_region_get(young_id) == young);
    T1H_CHECK(t1_maintenance_wanted() && *t1_upper_reclaim_due());
    t1_upper_reclaim();
    T1H_CHECK(maintenance_posts == 1);
    t1_reclaim();
    T1H_CHECK(reclaimed() == freed + 64 && !t1_region_get(oldest) &&
              t1_region_get(young_id) == young);
    T1H_CHECK(!t1_maintenance_wanted() && !*t1_upper_reclaim_due());
    t1_upper_reclaim();
    T1H_CHECK(maintenance_posts == 1);
    code_quiescent(v);
    t1_reclaim();
    T1H_CHECK(reclaimed() == freed + 65 && !t1_maintenance_wanted());
    code_unregister_vcpu(v);
    vcpu_destroy(v);
    t1_cfg.submit = saved_submit;
    t1_cfg.submit_reclaim = saved_post;
    T1H_CHECK(usage().used_bytes == base);
}

/* A refusal evicts nothing; maintenance evicts as many cold regions as the
 * request needs, not one per refused compile. */
static void maintenance(void)
{
    uint64_t base = usage().used_bytes;
    size_t unit = record_pages() + 8;
    metadata_room(bytes_of(3 * unit));
    struct t1_region *r[3];
    uint64_t link[3];
    for (unsigned k = 0; k < 3; ++k) {
        r[k] = published(8);
        link[k] = t1_region_link(r[k]);
    }
    size_t want = 2 * unit + 1;
    uint64_t freed = reclaimed();
    T1H_CHECK(!t1_memory_alloc(want, 0));
    for (unsigned k = 0; k < 3; ++k)
        T1H_CHECK(t1_region_from_link(link[k]) == r[k] && r[k]->valid);
    T1H_CHECK(t1_memory_pressure() == 1 + T1_DEFER_BYTES && t1_reclaim_pending());
    t1_reclaim();
    /* No executor is registered: the evicted records are freed at once. */
    T1H_CHECK(reclaimed() == freed + 3);
    for (unsigned k = 0; k < 3; ++k)
        T1H_CHECK(!t1_region_from_link(link[k]));
    T1H_CHECK(!t1_memory_pressure());
    void *p = t1_memory_alloc(want, 0);
    T1H_CHECK(p);
    t1_memory_free(p, want);
    t1_flush();
    T1H_CHECK(usage().used_bytes == base);
    end_pressure();
}

/* Publication marks a region referenced: a region nobody has entered yet
 * is not the clock's first victim. */
static void second_chance(void)
{
    t1_shutdown();
    T1H_CHECK(t1_init(NULL));
    uint64_t base = usage().used_bytes;
    size_t unit = record_pages() + 8;
    metadata_room(bytes_of(2 * unit));
    /* The new region holds the slot the clock reaches first. */
    struct t1_region *young = t1_region_new(), *old = t1_region_new();
    T1H_CHECK(young && old);
    young->meta_pages = old->meta_pages = 8;
    young->meta = t1_memory_alloc(8, HAL_PAGE_ZERO);
    old->meta = t1_memory_alloc(8, HAL_PAGE_ZERO);
    T1H_CHECK(young->meta && old->meta && t1_region_publish(old));
    old->referenced = false;
    T1H_CHECK(t1_region_publish(young));
    uint64_t ly = t1_region_link(young), lo = t1_region_link(old);
    uint64_t freed = reclaimed();
    T1H_CHECK(!t1_memory_alloc(1, 0));
    t1_reclaim();
    T1H_CHECK(reclaimed() == freed + 1);
    T1H_CHECK(!t1_region_from_link(lo) && t1_region_from_link(ly) == young && young->valid);
    t1_flush();
    T1H_CHECK(usage().used_bytes == base);
    end_pressure();
}

/* With registered executors, retired records wait for each executor's
 * next quiescent boundary. A record retired by an invalidation does not
 * stop eviction: maintenance counts the bytes awaiting reclamation. */
static void quiescence(void)
{
    struct vcpu_config cfg = {0};
    struct vcpu *v = vcpu_create(&cfg);
    cfg.index = 1;
    struct vcpu *other = vcpu_create(&cfg);
    T1H_CHECK(v && other);
    uint64_t base = usage().used_bytes;
    size_t unit = record_pages() + 8;
    metadata_room(bytes_of(4 * unit));
    code_register_vcpu(v);
    code_register_vcpu(other);
    struct t1_region *d = published(8);
    uint64_t ld = t1_region_link(d);
    t1_region_retire(d);
    struct t1_region *r[3];
    for (unsigned k = 0; k < 3; ++k)
        r[k] = published(8);
    uint64_t full = usage().used_bytes, freed = reclaimed();
    size_t want = 2 * unit + 1;
    T1H_CHECK(!t1_memory_alloc(want, 0));
    t1_reclaim();
    T1H_CHECK(reclaimed() == freed && usage().used_bytes == full);
    unsigned retired = 0;
    for (unsigned k = 0; k < 3; ++k)
        retired += r[k]->retired;
    T1H_CHECK(retired == 2 && d->retired && t1_memory_pressure());
    T1H_CHECK(!t1_memory_alloc(want, 0) && usage().used_bytes == full);
    code_quiescent(v);
    t1_reclaim();
    T1H_CHECK(reclaimed() == freed && usage().used_bytes == full);
    code_quiescent(other);
    /* The refused allocation reclaims what both executors passed. */
    void *p = t1_memory_alloc(want, 0);
    T1H_CHECK(p && reclaimed() == freed + 3 && !t1_region_from_link(ld));
    T1H_CHECK(!t1_memory_pressure());
    t1_memory_free(p, want);
    code_unregister_vcpu(v);
    code_unregister_vcpu(other);
    vcpu_destroy(v);
    vcpu_destroy(other);
    t1_flush();
    T1H_CHECK(usage().used_bytes == base);
    end_pressure();
}

/* Code-arena pressure with registered executors: live code is never moved;
 * cold code is retired by maintenance and its chunk reused after the
 * retirement epoch. */
static void arena_quiescence(void)
{
    t1_arena_shutdown();
    T1H_CHECK(t1_arena_init(1u << 20));
    struct vcpu_config cfg = {0};
    struct vcpu *v = vcpu_create(&cfg);
    T1H_CHECK(v);
    uint8_t *buf = t1_workspace_alloc(16, HAL_PAGE_ZERO);
    T1H_CHECK(buf);
    struct t1_emitted e = {.buf = buf, .size = 16};
    struct t1_region *empty = t1_region_new();
    T1H_CHECK(empty && t1_arena_place(empty, &e) && t1_region_publish(empty));
    t1_region_evict(empty);
    t1_reclaim();
    /* Eviction can empty the open chunk before it is closed. */
    code_register_vcpu(v);
    e.size = T1_CHUNK_BYTES;
    struct t1_region *placed[15];
    uint8_t *original = NULL;
    /* One MiB holds fifteen chunks. */
    for (unsigned k = 0; k < 15; ++k) {
        placed[k] = t1_region_new();
        T1H_CHECK(placed[k] && t1_arena_place(placed[k], &e) && t1_region_publish(placed[k]));
    }
    struct t1_region *first = placed[0];
    original = first->code;
    /* A full arena: the placement requests a chunk and evicts nothing. */
    struct t1_region *r = t1_region_new();
    T1H_CHECK(r && !t1_arena_place(r, &e));
    for (unsigned k = 0; k < 15; ++k)
        T1H_CHECK(placed[k]->valid);
    T1H_CHECK(t1_memory_pressure() == 1 + T1_DEFER_CHUNK && t1_reclaim_pending());
    T1H_CHECK(first->code == original);
    t1_reclaim();
    T1H_CHECK(first->code == original && t1_memory_pressure());
    code_quiescent(v);
    t1_reclaim();
    T1H_CHECK(!t1_memory_pressure());
    T1H_CHECK(t1_arena_place(r, &e) && t1_region_publish(r));
    code_unregister_vcpu(v);
    vcpu_destroy(v);
    t1_workspace_free(buf, 16);
    t1_flush();
    end_pressure();
}

/* A context's block (memory.h struct t1_workspace): one charge however
 * often it is taken, arrays carved in stack order and freed by a reset,
 * cleared only when asked, a request beyond the room or without a
 * workspace served and returned by the pool, release; a block the
 * allocator does not give or the cap does not admit is no refusal. */
static void workspace_block(void)
{
    struct t1_memory_stats s = usage();
    T1H_CHECK(!s.workspace_bytes);
    bool poison = t1_test_workspace_poison;
    t1_test_workspace_poison = true;
    void *p = t1_ws_alloc(NULL, 2, 0);
    T1H_CHECK(p && usage().workspace_bytes == bytes_of(2));
    t1_ws_free(NULL, p, 2);
    T1H_CHECK(!usage().workspace_bytes);
    struct t1_workspace ws = {0};
    T1H_CHECK(t1_ws_acquire(&ws) && ws.base && usage().workspace_bytes == block_bytes());
    uint8_t *base = ws.base;
    T1H_CHECK(t1_ws_acquire(&ws) && ws.base == base && usage().workspace_bytes == block_bytes());
    uint8_t *a = t1_ws_alloc(&ws, 3, HAL_PAGE_ZERO), *b = t1_ws_alloc(&ws, 2, 0);
    T1H_CHECK(a == base && b == base + bytes_of(3));
    for (size_t k = 0; k < bytes_of(3); ++k)
        T1H_CHECK(!a[k]);
    for (size_t k = 0; k < bytes_of(2); ++k)
        T1H_CHECK(b[k] == T1_WORKSPACE_POISON);
    memset(b, 1, bytes_of(2));
    t1_ws_free(&ws, b, 2);
    uint8_t *c = t1_ws_alloc(&ws, 1, HAL_PAGE_ZERO);
    T1H_CHECK(c == b && !c[0] && !c[4095] && b[4096] == 1);
    /* A free below the top waits for the reset. */
    t1_ws_free(&ws, a, 3);
    T1H_CHECK(t1_ws_alloc(&ws, 1, 0) == c + 4096);
    t1_ws_reset(&ws);
    T1H_CHECK(t1_ws_alloc(&ws, 1, 0) == base && usage().workspace_bytes == block_bytes());
    /* Beyond the room: the pool, and back to it; the stack is unchanged. */
    size_t big = ws.pages;
    uint8_t *o = t1_ws_alloc(&ws, big, 0);
    T1H_CHECK(o && (o + bytes_of(big) <= base || o >= base + bytes_of(ws.pages)));
    T1H_CHECK(usage().workspace_bytes == block_bytes() + bytes_of(big) &&
              o[0] == T1_WORKSPACE_POISON);
    t1_ws_free(&ws, o, big);
    T1H_CHECK(usage().workspace_bytes == block_bytes() && t1_ws_alloc(&ws, 1, 0) == base + 4096);
    uint64_t refusals = t1_memory_refusals_here();
    t1_memory_test_refuse(1);
    T1H_CHECK(!t1_ws_alloc(&ws, big, 0) && t1_memory_refusals_here() == refusals + 1);
    t1_ws_release(&ws);
    T1H_CHECK(!ws.base && !usage().workspace_bytes);
    t1_ws_release(&ws);
    /* No block: nothing held, no refusal, nothing recorded for maintenance
     * (the allocator's failure is injected; the cap's is a low limit). */
    t1_reclaim();
    T1H_CHECK(!t1_memory_wanted());
    refusals = t1_memory_refusals_here();
    uint64_t total = usage().refusals;
    t1_memory_test_refuse(1);
    T1H_CHECK(!t1_ws_acquire(&ws) && !ws.base && !usage().workspace_bytes);
    t1_memory_set_limit(usage().used_bytes + bytes_of(1));
    T1H_CHECK(!t1_ws_acquire(&ws) && !ws.base && !usage().workspace_bytes);
    T1H_CHECK(t1_memory_refusals_here() == refusals && usage().refusals == total);
    T1H_CHECK(!t1_memory_wanted() && !t1_memory_pressure());
    t1_memory_set_limit(48u << 20);
    /* Hosted: no block, every array from the pool. */
    t1_memory_test_block(0);
    T1H_CHECK(!t1_ws_acquire(&ws) && !ws.base);
    p = t1_ws_alloc(&ws, 1, 0);
    T1H_CHECK(p && usage().workspace_bytes == 4096);
    t1_ws_free(&ws, p, 1);
    t1_memory_test_block(SIZE_MAX);
    t1_test_workspace_poison = poison;
    t1_reclaim();
    T1H_CHECK(!t1_memory_pressure() && usage().used_bytes == s.used_bytes &&
              !usage().workspace_bytes);
}

static void memory_compile_tests(void);

#include "tests/tier1/arena_occupancy.inc"
#include "tests/tier1/arena_spans.inc"

void memory_tests(void);
void memory_tests(void)
{
    bool ready = t1_ready;
    struct t1_config saved = t1_cfg;
    struct t1_memory_stats start = usage();
    t1_shutdown();
    T1H_CHECK(t1_init(NULL));
    uint64_t setup_bytes = usage().used_bytes;
    t1_shutdown();
    struct t1_compile_ctx *ctx = t1_compile_ctx_create();
    T1H_CHECK(ctx);
    uint64_t context_bytes = usage().used_bytes;
    T1H_CHECK(context_bytes > 0 && t1_init(NULL));
    T1H_CHECK(usage().used_bytes == setup_bytes + context_bytes);
    t1_compile_ctx_destroy(ctx);
    T1H_CHECK(usage().used_bytes == setup_bytes);
    budget();
    reservation();
    workspace_block();
    region_gauges();
    region_ids();
    post_threshold();
    bounded_reclamation();
    reclamation_continuation();
    maintenance();
    second_chance();
    quiescence();
    arena_quiescence();
    arena_occupancy_tests();
    T1H_CHECK(usage().peak_bytes <= (48u << 20));
    t1_shutdown();
    T1H_CHECK(usage().used_bytes == 0);
    t1_memory_set_limit(start.budget_bytes);
    if (ready)
        T1H_CHECK(t1_init(&saved));
    puts(
        "memory: reserve-derived cap, workspace reservation, context block, refusal without eviction, "
        "maintenance covering the request, second chance at publication, reclamation after quiescence passed");
    memory_compile_tests();
}

/* ---- compiles refused memory -------------------------------------------- */

struct site {
    const char *stage, *reason;
    bool seen;
};

/* A refusal at each allocation of a compile defers it: T1_NO_MEMORY, no
 * rejection, nothing published, every page returned except the context's
 * block. The compile succeeds once memory is available again. block_pages:
 * the executor context's block (0: none, every stage array from the pool;
 * SIZE_MAX: the full block, which serves every stage array, so only the
 * region's tables are refused). */
static void deferral_at_each_site(const struct t1h_provider *pv, void *ctx, struct vcpu *v,
                                  size_t block_pages)
{
    struct t1_compile_options opt = {.flags = T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG};
    struct t1_compile_report rep;
    t1_flush();
    t1_compile_shutdown();
    t1_memory_test_block(block_pages);
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_COMPILED);
    t1_flush();
    struct t1_memory_stats base = usage();
    uint64_t block = block_pages == SIZE_MAX ? block_bytes() : bytes_of(block_pages);
    T1H_CHECK(base.workspace_bytes == block);
    t1_memory_test_reset_peaks();
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_COMPILED && rep.code_bytes);
    /* The reservation covers what one compile holds at once. */
    T1H_CHECK(usage().workspace_peak <=
              (block_pages ? block + t1_compile_workspace_bytes() : block_bytes()));
    if (block_pages == SIZE_MAX)
        T1H_CHECK(usage().workspace_peak == block);
    t1_flush();
    T1H_CHECK(usage().used_bytes == base.used_bytes);
    struct site sites[] = {
        {"lift", "lifting", false},       {"optimize", "optimize:", false}, {"isel", "isel", false},
        {"regalloc", "regalloc", false},  {"raverify", "raverify", false},  {"emit", "emit", false},
        {"frame", "frame tables", false},
    };
    unsigned refused = 0;
    for (unsigned nth = 1;; ++nth) {
        T1H_CHECK(nth < 4096);
        struct t1_stats before;
        t1_get_stats(&before);
        t1_memory_test_refuse(nth);
        enum t1_compile_status st = pv->compile(ctx, &opt, &rep);
        t1_memory_test_refuse(0);
        struct t1_stats after;
        t1_get_stats(&after);
        if (st == T1_COMPILED)
            break;
        if (st != T1_NO_MEMORY || after.rejects != before.rejects)
            fprintf(stderr, "memory: refusal %u: status %d, reason %s\n", nth, (int)st,
                    rep.reject_reason ? rep.reject_reason : "(none)");
        T1H_CHECK(st == T1_NO_MEMORY && after.rejects == before.rejects);
        T1H_CHECK(after.memory_deferrals == before.memory_deferrals + 1 &&
                  after.compiles == before.compiles);
        /* A refused allocation of the compile's own: bytes. */
        for (unsigned k = 0; k < T1_DEFER_KINDS; ++k)
            T1H_CHECK(after.deferrals_refused[k] ==
                          before.deferrals_refused[k] + (k == T1_DEFER_BYTES) &&
                      after.deferrals_pressure[k] == before.deferrals_pressure[k]);
        T1H_CHECK(usage().used_bytes == base.used_bytes && usage().workspace_bytes == block);
        T1H_CHECK(!t1_region_lookup(v, &opt, hostwin_bits(pv->hostwin(ctx)), NULL));
        T1H_CHECK(rep.reject_reason);
        for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(sites); ++k)
            if (strstr(rep.reject_reason, sites[k].reason))
                sites[k].seen = true;
        /* Memory is available: maintenance clears the request. */
        t1_reclaim();
        T1H_CHECK(!t1_memory_pressure());
        ++refused;
    }
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(sites); ++k) {
        bool stage = k < 6;
        if (block_pages == SIZE_MAX && stage) {
            if (sites[k].seen)
                fprintf(stderr, "memory: a stage array of %s came from the pool\n", sites[k].stage);
            T1H_CHECK(!sites[k].seen);
            continue;
        }
        if (!block_pages || !stage) {
            if (!sites[k].seen)
                fprintf(stderr, "memory: no refusal in %s\n", sites[k].stage);
            T1H_CHECK(sites[k].seen);
        }
    }
    t1_flush();
    T1H_CHECK(usage().used_bytes == base.used_bytes);
    /* An outstanding request defers before any compiler stage runs. */
    metadata_room(0);
    T1H_CHECK(!t1_memory_alloc(1, 0) && t1_memory_pressure() == 1 + T1_DEFER_BYTES);
    t1_memory_test_reset_peaks();
    struct t1_stats before;
    t1_get_stats(&before);
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_NO_MEMORY);
    struct t1_stats after;
    t1_get_stats(&after);
    for (unsigned k = 0; k < T1_DEFER_KINDS; ++k)
        T1H_CHECK(after.deferrals_pressure[k] ==
                      before.deferrals_pressure[k] + (k == T1_DEFER_BYTES) &&
                  after.deferrals_refused[k] == before.deferrals_refused[k]);
    T1H_CHECK(usage().workspace_peak == block && usage().used_bytes == base.used_bytes);
    end_pressure();
    t1_compile_shutdown();
    t1_memory_test_block(SIZE_MAX);
    printf(
        "memory: block of %llu pages: a compile refused memory at each of its %u allocations is deferred, "
        "nothing published, every page but the block returned\n",
        (unsigned long long)(block / 4096), refused);
}

/* A compile on a context without a block takes one and keeps it across
 * compiles; an outstanding request defers the compile with no block held;
 * a block it cannot take leaves every stage array to the pool and records
 * no request; shutting the context down returns the block. */
static void context_block(const struct t1h_provider *pv, void *ctx)
{
    struct t1_compile_options opt = {.flags = T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG};
    struct t1_compile_report rep;
    t1_flush();
    t1_compile_shutdown();
    struct t1_memory_stats base = usage();
    T1H_CHECK(!base.workspace_bytes);
    struct t1_compile_ctx *probe = t1_compile_ctx_create();
    T1H_CHECK(probe);
    uint64_t context = usage().used_bytes - base.used_bytes;
    t1_compile_ctx_destroy(probe);
    /* Room for the executor's context, and a request it does not cover. */
    metadata_room(context);
    T1H_CHECK(!t1_memory_alloc(context / 4096 + 1, 0) && t1_memory_pressure());
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_NO_MEMORY && !usage().workspace_bytes);
    T1H_CHECK(usage().used_bytes == base.used_bytes + context);
    end_pressure();
    /* The block attempt fails (injected; in the product: no contiguous
     * run): the compile takes its arrays from the pool and returns them. */
    struct t1_stats before, after;
    t1_get_stats(&before);
    uint64_t total = usage().refusals;
    t1_memory_test_reset_peaks();
    t1_memory_test_refuse(1);
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_COMPILED);
    t1_memory_test_refuse(0);
    t1_get_stats(&after);
    T1H_CHECK(after.compiles == before.compiles + 1 &&
              after.memory_deferrals == before.memory_deferrals);
    T1H_CHECK(usage().refusals == total && !t1_memory_wanted() && !t1_memory_pressure());
    T1H_CHECK(!usage().workspace_bytes && usage().workspace_peak > 0);
    t1_flush();
    T1H_CHECK(usage().used_bytes == base.used_bytes + context);
    t1_memory_test_reset_peaks();
    for (unsigned k = 0; k < 3; ++k) {
        T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_COMPILED);
        t1_flush();
        T1H_CHECK(usage().workspace_bytes == block_bytes() &&
                  usage().workspace_peak == block_bytes());
        T1H_CHECK(usage().used_bytes == base.used_bytes + context + block_bytes());
    }
    t1_compile_shutdown();
    T1H_CHECK(usage().used_bytes == base.used_bytes && !usage().workspace_bytes);
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_COMPILED);
    t1_flush();
    puts(
        "memory: a context takes its block at its first compile and keeps it (one charge); an outstanding "
        "request defers with no block held; without the block the compile uses the pool and records no "
        "request; shutdown returns it");
}

/* Placement in a full code arena after the region record exists: the
 * record and its tables are discarded. */
static void deferral_at_placement(const struct t1h_provider *pv, void *ctx, struct vcpu *v)
{
    struct t1_compile_options opt = {.flags = T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG};
    struct t1_compile_report rep;
    t1_flush();
    t1_arena_shutdown();
    T1H_CHECK(t1_arena_init(1u << 20));
    uint8_t *stubs = t1_arena_stub_page();
    T1H_CHECK(t1_stubs_build(stubs, 4096));
    hal_code_set_executable(stubs, 1);
    uint8_t *raw[16];
    unsigned nraw = 0;
    while (nraw < 16 && (raw[nraw] = t1_arena_alloc(1)))
        ++nraw;
    struct t1_memory_stats base = usage();
    struct t1_stats before;
    t1_get_stats(&before);
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_NO_MEMORY);
    struct t1_stats after;
    t1_get_stats(&after);
    T1H_CHECK(rep.reject_reason && strstr(rep.reject_reason, "code arena full"));
    T1H_CHECK(after.rejects == before.rejects &&
              after.memory_deferrals == before.memory_deferrals + 1);
    T1H_CHECK(after.deferrals_refused[T1_DEFER_CHUNK] ==
              before.deferrals_refused[T1_DEFER_CHUNK] + 1);
    T1H_CHECK(usage().used_bytes == base.used_bytes &&
              usage().workspace_bytes == base.workspace_bytes);
    T1H_CHECK(!t1_region_lookup(v, &opt, hostwin_bits(pv->hostwin(ctx)), NULL));
    while (nraw)
        t1_arena_free(raw[--nraw], 1);
    t1_reclaim();
    T1H_CHECK(!t1_memory_pressure());
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_COMPILED);
    t1_flush();
    t1_arena_shutdown();
    T1H_CHECK(t1_arena_init(t1_cfg.code_bytes));
    stubs = t1_arena_stub_page();
    T1H_CHECK(t1_stubs_build(stubs, 4096));
    hal_code_set_executable(stubs, 1);
    puts(
        "memory: a full code arena defers the compile after region_new; the record and its tables are freed");
}

/* A full region table: the compile that finds no id is deferred (refused,
 * kind id) and records the request; the next one is deferred before
 * compiling (pressure, kind id); after maintenance it compiles. */
static void deferral_for_region_id(const struct t1h_provider *pv, void *ctx)
{
    struct t1_compile_options opt = {.flags = T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG};
    struct t1_compile_report rep;
    struct t1_config saved = t1_cfg, c = t1_cfg;
    c.max_regions = 2;
    t1_shutdown();
    T1H_CHECK(t1_init(&c));
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_COMPILED);
    published(1);
    struct t1_stats s0, s1, s2;
    t1_get_stats(&s0);
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_NO_MEMORY);
    t1_get_stats(&s1);
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_NO_MEMORY);
    t1_get_stats(&s2);
    for (unsigned k = 0; k < T1_DEFER_KINDS; ++k) {
        T1H_CHECK(s1.deferrals_refused[k] == s0.deferrals_refused[k] + (k == T1_DEFER_ID) &&
                  s1.deferrals_pressure[k] == s0.deferrals_pressure[k]);
        T1H_CHECK(s2.deferrals_pressure[k] == s1.deferrals_pressure[k] + (k == T1_DEFER_ID) &&
                  s2.deferrals_refused[k] == s1.deferrals_refused[k]);
    }
    T1H_CHECK(s2.rejects == s0.rejects && s2.memory_deferrals == s0.memory_deferrals + 2);
    t1_reclaim();
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_COMPILED);
    t1_shutdown();
    T1H_CHECK(t1_init(&saved));
    puts(
        "memory: a full region table defers a compile (refused, kind id) and the next one before it compiles "
        "(pressure, kind id); after maintenance it compiles");
}

/* A deferred compile asks for its record and tables together: one
 * maintenance pass makes room for all of them. */
static void deferral_covers_region(const struct t1h_provider *pv, void *ctx)
{
    struct t1_compile_options opt = {.flags = T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG};
    struct t1_compile_report rep;
    t1_flush();
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_COMPILED);
    const struct t1_region *compiled = t1_region_get(rep.region_id);
    size_t need = record_pages() + compiled->meta_pages;
    t1_flush();
    uint64_t base = usage().used_bytes;
    /* Cold record-only regions fill the room. */
    size_t unit = record_pages();
    unsigned n = (unsigned)((need + unit - 1) / unit);
    T1H_CHECK(need > unit);
    metadata_room(bytes_of(n * unit));
    for (unsigned k = 0; k < n; ++k) {
        struct t1_region *r = t1_region_new();
        T1H_CHECK(r && t1_region_publish(r));
        r->referenced = false;
    }
    T1H_CHECK(!t1_memory_headroom());
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_NO_MEMORY);
    t1_reclaim();
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_COMPILED);
    t1_flush();
    T1H_CHECK(usage().used_bytes == base);
    end_pressure();
    printf(
        "memory: a compile deferred for its region tables (%zu pages) compiles after one maintenance pass\n",
        need);
}

/* The harness engine keeps no backoff for a deferred head: it asks again
 * at the hotness threshold while memory stays short. */
static void harness_deferral(const struct t1h_provider *pv, void *ctx, struct vcpu *v,
                             const struct snapshot *snap)
{
    snapshot_restore(snap, v);
    pv->restored(ctx);
    t1_flush();
    metadata_room(0);
    struct t1_stats before;
    t1_get_stats(&before);
    struct vcpu_exit e = pv->run(ctx, UINT64_C(50000000));
    T1H_CHECK(e.reason == VCPU_EXIT_DEBUG);
    struct t1_stats after;
    t1_get_stats(&after);
    T1H_CHECK(after.compiles == before.compiles && after.rejects == before.rejects);
    printf("memory: harness engine without room: %llu compiles deferred\n",
           (unsigned long long)(after.memory_deferrals - before.memory_deferrals));
    T1H_CHECK(after.memory_deferrals - before.memory_deferrals >= HARNESS_DEFERRALS_MIN);
    end_pressure();
}

/* Eviction after the executor left a region's code and before it restored
 * from the region's tables: the record outlives every maintenance pass
 * until the executor is quiescent. */
static unsigned before_materialize;
static uint64_t evicted_at;
static void evict_before_materialize(struct vcpu *v, void *arg)
{
    (void)v;
    struct t1_region *r = t1_region_get(*(const uint32_t *)arg);
    T1H_CHECK(r && r->valid);
    evicted_at = reclaimed();
    t1_region_evict(r);
    t1_reclaim();
    T1H_CHECK(reclaimed() == evicted_at && r->retired);
    ++before_materialize;
}

static void eviction_before_materialization(const struct t1h_provider *pv, void *ctx,
                                            struct vcpu *v, struct vcpu *ref,
                                            const struct snapshot *snap)
{
    struct run_result reference, actual;
    result_init(&reference);
    result_init(&actual);
    snapshot_restore(snap, ref);
    struct vcpu_exit re = ref_run(ref, UINT64_C(50000000), NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG);
    result_take(&reference, &re, ref);
    snapshot_restore(snap, v);
    pv->restored(ctx);
    struct t1_compile_options opt = {.flags = T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG};
    struct t1_compile_report rep;
    T1H_CHECK(pv->compile(ctx, &opt, &rep) == T1_COMPILED);
    uint32_t region = rep.region_id, site = UINT32_MAX;
    for (unsigned k = 0; k < pv->point_count(ctx, region) && site == UINT32_MAX; ++k) {
        struct t1_point_info p;
        T1H_CHECK(pv->point_get(ctx, region, k, &p));
        if (p.kind == T1_PT_SITE)
            site = p.point_id;
    }
    T1H_CHECK(site != UINT32_MAX);
    T1H_CHECK(
        pv->force(ctx, region, site, 2, T1_FORCE_TRAP_EXIT, evict_before_materialize, &region));
    code_register_vcpu(v);
    before_materialize = 0;
    struct vcpu_exit e = pv->run(ctx, UINT64_C(50000000));
    result_take(&actual, &e, v);
    T1H_CHECK(before_materialize == 1);
    T1H_CHECK(!result_compare(&reference, &actual, false, "eviction before materialization"));
    code_quiescent(v);
    t1_reclaim();
    T1H_CHECK(reclaimed() > evicted_at);
    code_unregister_vcpu(v);
    result_free(&reference);
    result_free(&actual);
    puts(
        "memory: a region evicted between its exit and materialization is reclaimed only after the "
        "executor's next quiescent boundary; the run equals the reference");
}

static void memory_compile_tests(void)
{
    const struct t1h_provider *pv = t1h_provider_real();
    const struct guest_program *p = guest_program("gather_load_after_store");
    struct vcpu_config cfg = guest_vcpu_config(p);
    struct vcpu *ref = vcpu_create(&cfg), *v = vcpu_create(&cfg);
    T1H_CHECK(ref && v);
    struct t1h_logs ref_logs, logs;
    t1h_bind(ref, &ref_logs);
    t1h_bind(v, &logs);
    guest_setup(p, &(struct guest_params){.n = 7, .passes = 64}, ref);
    uint64_t header;
    T1H_CHECK(t1h_run_to_loop_header(ref, 100000, &header));
    struct snapshot snap;
    snapshot_init(&snap);
    snapshot_take(&snap, ref);
    snapshot_restore(&snap, v);
    void *ctx = pv->create(v);
    T1H_CHECK(ctx);
    deferral_at_each_site(pv, ctx, v, 0);
    deferral_at_each_site(pv, ctx, v, 64);
    deferral_at_each_site(pv, ctx, v, SIZE_MAX);
    context_block(pv, ctx);
    deferral_at_placement(pv, ctx, v);
    deferral_for_region_id(pv, ctx);
    deferral_covers_region(pv, ctx);
    harness_deferral(pv, ctx, v, &snap);
    eviction_before_materialization(pv, ctx, v, ref, &snap);
    t1_flush();
    pv->destroy(ctx);
    t1h_unbind(ref);
    t1h_unbind(v);
    vcpu_destroy(ref);
    vcpu_destroy(v);
    snapshot_free(&snap);
}
