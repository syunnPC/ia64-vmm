/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/cache/background.h"
#include "engine/tier0/internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"
#include <stdatomic.h>

struct tier0_arena tier0_arena;
static void (*reclaim_submit)(void);
static size_t align16(size_t n)
{
    return (n + 15) & ~(size_t)15;
}
static void stopped(void)
{
    if (tier0_execution_active())
        hal_panic("Tier0 reclamation/publication during execution or checking");
}
static bool tier0_configure_locked(uint64_t bytes)
{
    stopped();
    if (bytes < 1048576 || bytes > UINT64_C(1024) * 1048576 || bytes % 1048576)
        return false;
    tier0_destroy();
    struct tier0_arena *a = &tier0_arena;
    /* Metadata capacity exceeds the number of ordinary packed regions;
     * chunk eviction, rather than a global flush, is the normal limit. */
    a->capacity = (unsigned)(bytes / 1024);
    a->hash_size = 1;
    while (a->hash_size < a->capacity * 2)
        a->hash_size *= 2;
    a->nchunks = (unsigned)(bytes / TIER0_CHUNK_BYTES);
    size_t region_bytes = align16(a->capacity * sizeof(*a->regions));
    size_t tag_bytes = align16(a->capacity * sizeof(*a->tags));
    size_t chunk_bytes = align16(a->nchunks * sizeof(*a->chunks));
    size_t metadata_bytes =
        region_bytes + tag_bytes + chunk_bytes + a->hash_size * 2 * sizeof(unsigned);
    a->code_pages = (size_t)bytes / 4096;
    a->metadata_pages = (metadata_bytes + 4095) / 4096;
    a->code = hal_alloc_pages(a->code_pages, HAL_PAGE_EXEC);
    a->metadata = hal_alloc_pages(a->metadata_pages, HAL_PAGE_ZERO);
    if (!a->code || !a->metadata) {
        LOGE("Tier0 arena unavailable: tier0_code_mib=%llu metadata_bytes=%zu",
             (unsigned long long)(bytes / 1048576), metadata_bytes);
        tier0_destroy();
        return false;
    }
    a->regions = a->metadata;
    a->tags = (void *)((uint8_t *)a->metadata + region_bytes);
    a->chunks = (void *)((uint8_t *)a->tags + tag_bytes);
    a->buckets = (void *)((uint8_t *)a->chunks + chunk_bytes);
    a->sources = (void *)(a->buckets + a->hash_size);
    for (unsigned i = 0; i < a->capacity; ++i)
        a->regions[i].retired_next = i + 1 == a->capacity ? 0 : i + 2;
    a->available = 1;
    a->free_regions = a->capacity;
    a->open_chunk = UINT32_MAX;
    LOGI("Tier0 arena code_mib=%llu chunks=%u region_capacity=%u metadata_bytes=%zu",
         (unsigned long long)(bytes / 1048576), a->nchunks, a->capacity, metadata_bytes);
    return true;
}
uint64_t tier0_default_code_bytes(uint64_t reserve_bytes, bool tier1)
{
    uint64_t share = reserve_bytes / (tier1 ? 8 : 4) / 1048576 * 1048576;
    return share < TIER0_CODE_DEFAULT_BYTES       ? TIER0_CODE_DEFAULT_BYTES
           : share > TIER0_CODE_MAX_DEFAULT_BYTES ? TIER0_CODE_MAX_DEFAULT_BYTES
                                                  : share;
}
static bool token(const char *at, const char *end, const char *text)
{
    return (size_t)(end - at) == strlen(text) && !strncmp(at, text, (size_t)(end - at));
}
bool tier0_configure_cmdline(const char *cmdline)
{
    const char key[] = "tier0_code_mib=";
    bool tier1 = false, chosen = false;
    uint64_t bytes = 0;
    while (*cmdline) {
        while (*cmdline == ' ' || *cmdline == '\t')
            ++cmdline;
        const char *end = cmdline;
        while (*end && *end != ' ' && *end != '\t')
            ++end;
        tier1 |= token(cmdline, end, "engine=tier1") || token(cmdline, end, "tier1=on");
        if ((size_t)(end - cmdline) >= sizeof(key) - 1 && !strncmp(cmdline, key, sizeof(key) - 1)) {
            const char *value = cmdline + sizeof(key) - 1;
            uint64_t mib = 0;
            if (value == end)
                goto invalid;
            for (; value < end; ++value) {
                if (*value < '0' || *value > '9' || mib > 1024)
                    goto invalid;
                mib = mib * 10 + (unsigned)(*value - '0');
            }
            if (!mib || mib > 1024)
                goto invalid;
            bytes = mib * 1048576;
            chosen = true;
        }
        cmdline = end;
    }
    struct hal_memory_stats memory;
    if (!chosen)
        bytes =
            tier0_default_code_bytes(hal_memory_get_stats(&memory) ? memory.total_bytes : 0, tier1);
    return tier0_configure(bytes);
invalid:
    LOGE("invalid tier0_code_mib (expected 1..1024)");
    return false;
}
bool tier0_arena_ensure(void)
{
    return tier0_arena.code || tier0_configure(TIER0_CODE_DEFAULT_BYTES);
}
bool tier0_arena_initialized(void)
{
    return tier0_arena.code != NULL;
}
static void tier0_region_retire_locked(struct tier0_region *r, bool eviction)
{
    if (!r->valid)
        return;
    code_dependency_remove(&r->dependency);
    tier0_unlink(r);
    r->valid = false;
    if (r->allocation)
        --tier0_arena.chunks[r->chunk].live;
    atomic_store_explicit(&tier0_arena.tags[r - tier0_arena.regions], 0, memory_order_release);
    r->retirement = code_retire_epoch();
    r->retired_next = tier0_arena.retired;
    tier0_arena.retired = (unsigned)(r - tier0_arena.regions) + 1;
    ++tier0_arena.retired_count;
    if (eviction)
        TIER0_ADD(tier0_local_stats(), evictions, 1);
    else
        TIER0_ADD(tier0_local_stats(), invalidations, 1);
}
static void tier0_reclaim_locked(unsigned limit)
{
    struct tier0_arena *a = &tier0_arena;
    /* Unregistered instruction fixtures use the diagnostic execution pin. */
    if (!code_executors_registered() && tier0_execution_active())
        return;
    unsigned head = a->retired;
    unsigned *previous = &head;
    uint64_t horizon = code_epoch_horizon();
    while (*previous && limit) {
        unsigned at = *previous;
        struct tier0_region *r = &a->regions[at - 1];
        if (r->retirement > horizon) {
            previous = &r->retired_next;
            continue;
        }
        *previous = r->retired_next;
        --a->retired_count;
        --limit;
        if (r->allocation) {
            struct tier0_chunk *c = &a->chunks[r->chunk];
            if (r->chunk_previous)
                a->regions[r->chunk_previous - 1].chunk_next = r->chunk_next;
            else
                c->regions = r->chunk_next;
            if (r->chunk_next)
                a->regions[r->chunk_next - 1].chunk_previous = r->chunk_previous;
            TIER0_ADD(&tier0_stats, arena_bytes, -(r->arena_bytes));
            TIER0_ADD(tier0_local_stats(), reclaimed, 1);
        }
        *r = (struct tier0_region){.retired_next = a->available};
        a->available = at;
        ++a->free_regions;
    }
    a->retired = head;
}
/* Retires the live regions of a chunk that holds regions, stamping it, and
 * reclaims what the executors' horizon allows. A chunk without regions
 * (untouched or reclaimed) needs no grace period and is left as it is. */
static void evict_chunk(unsigned chunk)
{
    struct tier0_arena *a = &tier0_arena;
    struct tier0_chunk *c = &a->chunks[chunk];
    if (!c->regions)
        return;
    if (c->live) {
        for (unsigned link = c->regions; link; link = a->regions[link - 1].chunk_next)
            tier0_region_retire(&a->regions[link - 1], true);
        if (!c->retirement)
            c->retirement = code_retire_epoch();
    }
    tier0_reclaim();
}
/* Region metadata running out before code space: evict the oldest chunk
 * with live regions (the next ring positions are oldest; the open writer
 * and chunks without regions are passed over), only one, since its regions
 * are reclaimed only after every executor's next quiescent boundary. An
 * older chunk whose regions all await that boundary ends the walk:
 * evicting another chunk would not free a record sooner. */
static void evict_oldest_live(void)
{
    struct tier0_arena *a = &tier0_arena;
    for (unsigned n = 0; n < a->nchunks; ++n) {
        unsigned chunk = (a->next_chunk + n) % a->nchunks;
        if (chunk == a->open_chunk || !a->chunks[chunk].regions)
            continue;
        if (a->chunks[chunk].live)
            evict_chunk(chunk);
        return;
    }
}
static struct tier0_region *tier0_region_allocate_locked(void)
{
    tier0_reclaim();
    struct tier0_arena *a = &tier0_arena;
    /* Without a free record the caller retries after its own boundary
     * (lookup) rather than evicting the whole ring: records already retired
     * (reclaimed above where possible) only await their grace period. */
    if (!a->available && !a->retired_count)
        evict_oldest_live();
    if (!a->available)
        return NULL;
    unsigned at = a->available;
    struct tier0_region *r = &a->regions[at - 1];
    a->available = r->retired_next;
    /* Keep a reserve of free or retired records, evicted ahead of need as
     * code space is (open_chunk), so peers pass their boundaries before it
     * runs out. */
    if (--a->free_regions + a->retired_count < TIER0_EVICT_AHEAD(a->capacity))
        evict_oldest_live();
    *r = (struct tier0_region){0};
    return r;
}
/* All mappings of the chunk remain RW/NX until the batch is sealed. A
 * staged region uses the exact C helpers on its first cold execution;
 * lookup of a staged region again seals the batch before generated entry.
 * This amortizes shootdowns without ever reopening a live RX page.
 * The owner remains pinned until lockstep and fault handling return. */
static void tier0_seal_locked(struct vcpu *v, unsigned chunk)
{
    struct tier0_arena *a = &tier0_arena;
    struct tier0_chunk *c = &a->chunks[chunk];
    if (c->executable || !c->used)
        return;
    hal_code_set_executable(a->code + chunk * TIER0_CHUNK_BYTES, TIER0_CHUNK_BYTES / 4096);
    c->executable = true;
    TIER0_ADD(tier0_local_stats(), protection_changes, 1);
    for (unsigned link = c->regions; link; link = a->regions[link - 1].chunk_next) {
        struct tier0_region *r = &a->regions[link - 1];
#ifdef IA64VMM_HOSTED
        if (r->valid && r->source_generation)
            code_validity_test_hook(r->key.gpa & ~UINT64_C(4095), CODE_TEST_SEAL);
#endif
        if (r->valid && r->source_generation &&
            !code_dependency_publish(v, r->key.gpa, code_source_generation(r->key.gpa),
                                     &r->dependency))
            tier0_region_retire(r, false);
        if (r->valid)
            TIER0_ADD(tier0_local_stats(), publications, 1);
    }
    if (a->open_chunk == chunk)
        a->open_chunk = UINT32_MAX;
}
/* The open chunk, or the oldest one (FIFO); false while that chunk's
 * evicted regions await the grace period. */
static bool open_chunk(void)
{
    struct tier0_arena *a = &tier0_arena;
    if (a->open_chunk != UINT32_MAX)
        return true;
    unsigned chunk = a->next_chunk;
    struct tier0_chunk *candidate = &a->chunks[chunk];
    /* Untouched chunks need no grace period. Every used chunk is evicted
     * once, then quarantined before RX can become RW. Its regions are
     * reclaimed after every executor's next quiescent boundary, so each
     * opening also evicts the chunk TIER0_EVICT_AHEAD positions later:
     * other executors pass their boundaries before the ring reaches it.
     * When the oldest chunk still waits, the caller retries after its own
     * boundary (lookup) instead of evicting further chunks, which would
     * empty the whole ring. */
    if (candidate->used)
        evict_chunk(chunk);
    if (candidate->regions || (candidate->retirement && !code_epoch_reached(candidate->retirement)))
        return false;
    unsigned ahead = (chunk + TIER0_EVICT_AHEAD(a->nchunks)) % a->nchunks;
    if (a->chunks[ahead].used)
        evict_chunk(ahead);
    a->next_chunk = (chunk + 1) % a->nchunks;
    struct tier0_chunk *c = &a->chunks[chunk];
    if (c->executable) {
        hal_code_set_writable(a->code + chunk * TIER0_CHUNK_BYTES, TIER0_CHUNK_BYTES / 4096);
        TIER0_ADD(tier0_local_stats(), protection_changes, 1);
    }
    *c = (struct tier0_chunk){0};
    a->open_chunk = chunk;
    return true;
}
/* One emission pass, directly into the open chunk after the region's
 * instruction copy (whose addresses the code embeds). A region that does
 * not fit seals the chunk and is emitted again into a new one; one that
 * does not fit an empty chunk is not staged. */
static bool tier0_stage_locked(struct tier0_region *r, const Ia64Instruction *insns,
                               const struct ia64_state *s)
{
    struct tier0_arena *a = &tier0_arena;
    size_t offset = align16(r->count * sizeof(*insns));
    struct x86_buffer b;
    /* A region owns arena bytes (allocation, chunk) only once its code is
     * complete: retirement unlinks an owner from its chunk's list. */
    for (;;) {
        if (!open_chunk())
            return false;
        struct tier0_chunk *c = &a->chunks[a->open_chunk];
        size_t room = TIER0_CHUNK_BYTES - c->used;
        if (room > offset) {
            uint8_t *allocation = a->code + a->open_chunk * TIER0_CHUNK_BYTES + c->used;
            memcpy(allocation, insns, r->count * sizeof(*insns));
            r->insns = (const Ia64Instruction *)allocation;
            b = (struct x86_buffer){.data = allocation + offset, .capacity = room - offset};
            tier0_emit(&b, r, s);
            r->insns = insns;
            if (!b.failed) {
                r->chunk = a->open_chunk;
                r->allocation = allocation;
                r->insns = (const Ia64Instruction *)allocation;
                r->entry = allocation + offset;
                break;
            }
        }
        if (!c->used)
            return false;
        tier0_seal(NULL, a->open_chunk);
    }
    struct tier0_chunk *c = &a->chunks[r->chunk];
    size_t bytes = align16(offset + b.size);
    r->arena_bytes = bytes;
    r->bytes = b.size;
    unsigned at = (unsigned)(r - a->regions) + 1;
    r->chunk_next = c->regions;
    if (c->regions)
        a->regions[c->regions - 1].chunk_previous = at;
    c->regions = at;
    ++c->live;
    c->used += bytes;
    TIER0_ADD(tier0_local_stats(), translations, 1);
    TIER0_ADD(tier0_local_stats(), bytes, b.size);
    TIER0_ADD(tier0_local_stats(), allocated_bytes, bytes);
    TIER0_ADD(&tier0_stats, arena_bytes, bytes);
    if (tier0_stats.arena_bytes > tier0_stats.arena_high_water)
        atomic_store_explicit(&tier0_stats.arena_high_water, tier0_stats.arena_bytes,
                              memory_order_relaxed);
    return true;
}
static void tier0_destroy_locked(void)
{
    struct tier0_arena *a = &tier0_arena;
    tier0_flush();
    tier0_reclaim();
    tier0_diagnostics_destroy();
    tier0_executors_destroy();
    if (a->code)
        hal_free_pages(a->code, a->code_pages);
    if (a->metadata)
        hal_free_pages(a->metadata, a->metadata_pages);
    *a = (struct tier0_arena){0};
}

bool tier0_configure(uint64_t bytes)
{
    code_background_quiesce();
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool ok = tier0_configure_locked(bytes);
    if (take)
        code_unlock();
    code_background_resume();
    return ok;
}
void tier0_region_retire(struct tier0_region *r, bool eviction)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    tier0_region_retire_locked(r, eviction);
    if (take)
        code_unlock();
}
void tier0_reclaim(void)
{
    if (!tier0_arena.retired)
        return;
    /* A post wakes a helper CPU; allocation and eviction reclaim inline. */
    if (!code_lock_held() && reclaim_submit) {
        if (atomic_load_explicit(&tier0_arena.retired_count, memory_order_relaxed) >=
            TIER0_RECLAIM_POST)
            reclaim_submit();
        return;
    }
    bool take = !code_lock_held();
    if (take)
        code_lock();
    tier0_reclaim_locked(UINT32_MAX);
    if (take)
        code_unlock();
}
struct tier0_region *tier0_region_allocate(void)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    struct tier0_region *r = tier0_region_allocate_locked();
    if (take)
        code_unlock();
    return r;
}
void tier0_seal(struct vcpu *v, unsigned chunk)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    tier0_seal_locked(v, chunk);
    if (take)
        code_unlock();
}
bool tier0_stage(struct tier0_region *r, const Ia64Instruction *insns, const struct ia64_state *s)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool ok = tier0_stage_locked(r, insns, s);
    if (take)
        code_unlock();
    return ok;
}
void tier0_destroy(void)
{
    code_background_quiesce();
    bool take = !code_lock_held();
    if (take)
        code_lock();
    stopped();
    if (code_executors_registered())
        hal_panic("Tier0 teardown with registered executors");
    tier0_destroy_locked();
    if (take)
        code_unlock();
    code_background_resume();
}

void tier0_set_reclaim_submit(void (*submit)(void))
{
    reclaim_submit = submit;
}
void tier0_reclaim_batch(unsigned limit)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    tier0_reclaim_locked(limit);
    if (take)
        code_unlock();
}
