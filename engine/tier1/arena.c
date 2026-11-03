/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tier1 W^X arena: runtime stubs precede 64 KiB chunks. Each compiler bumps
 * its private open chunk. First entry seals pages through the region's end;
 * full chunks seal entirely. Closed chunks reuse page spans only after all
 * overlapping regions are released. Placement makes only that span writable,
 * copies and fixes code, and reseals it before unlocking.
 *
 * Shared operations hold code_lock. With epoch reclamation, live code never
 * moves: maintenance evicts cold chunks and waits for retirement epochs before
 * reuse. Only one unregistered executor may recycle inline, outside region code.
 * Its second-chance clock copies referenced regions into an empty reserve,
 * fixes rel32 fields and preserves ids/generations; other regions are retired.
 * A chunk's survivors fit the reserve, and cleared reference bits bound the
 * scan to two turns. Region-table pressure evicts without moving survivors.
 */
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

enum chunk_state {
    CHUNK_FREE,
    CHUNK_OPEN,
    CHUNK_CLOSED,
    CHUNK_RAW /* t1_arena_alloc (unit tests) */
};
enum { CHUNK_PAGES = T1_CHUNK_BYTES / 4096 };
_Static_assert(CHUNK_PAGES <= 16, "arena page bitmap width");
struct chunk {
    uint32_t used;   /* bump offset: next placement (>= sealed) */
    uint32_t sealed; /* [0, sealed) is RX; page aligned */
    uint8_t state;
    uint8_t max_span;
    uint16_t occupied;
    unsigned live; /* regions placed here and not yet released */
    struct t1_region *regions;
    uint16_t page_refs[CHUNK_PAGES];
#if IA64VMM_TIER1_STATS
    uint16_t padding[T1_CHUNK_BYTES / 4096]; /* page-rounding gaps at sealing */
#endif
};

#define NONE UINT32_MAX
static uint8_t *base;
static size_t npages;
static struct chunk *chunks;
static size_t chunk_pages;
static unsigned nchunks, open_chunk = NONE, hand, nfree;
static unsigned span_hand, span_counts[CHUNK_PAGES + 1];
static uint64_t bytes_used;

static uint8_t *chunk_base(unsigned c)
{
    return base + 4096 + (size_t)c * T1_CHUNK_BYTES;
}
static uint32_t page_up(uint32_t n)
{
    return (n + 4095u) & ~4095u;
}

static unsigned empty_pages(const struct chunk *k)
{
    return k->state == CHUNK_CLOSED ? ((1u << (k->sealed / 4096)) - 1u) & ~k->occupied : 0;
}

/* Counts by largest run keep pressure queries independent of arena size. */
static void update_span(unsigned c)
{
    struct chunk *k = &chunks[c];
    if (k->max_span)
        --span_counts[k->max_span];
    unsigned longest = 0;
    for (unsigned mask = empty_pages(k); mask; mask &= mask << 1)
        ++longest;
    k->max_span = (uint8_t)longest;
    if (longest)
        ++span_counts[longest];
}

static bool span_available(unsigned pages)
{
    for (unsigned n = pages; n <= CHUNK_PAGES; ++n)
        if (span_counts[n])
            return true;
    return false;
}

/* Pending and retired code keeps its references until actual release. */
static void reference_pages(unsigned c, const struct t1_region *r, bool add)
{
    struct chunk *k = &chunks[c];
    unsigned start = (unsigned)(r->code - chunk_base(c));
    unsigned end = start + ((r->code_size + 15u) & ~15u);
    for (unsigned p = start / 4096; p <= (end - 1) / 4096; ++p) {
        if (add) {
            if (k->page_refs[p] == UINT16_MAX)
                hal_panic("tier1: code page reference overflow");
            ++k->page_refs[p];
            k->occupied |= (uint16_t)(1u << p);
        } else {
            if (!k->page_refs[p])
                hal_panic("tier1: missing code page reference");
            if (!--k->page_refs[p])
                k->occupied &= (uint16_t)~(1u << p);
        }
    }
    if (k->state == CHUNK_CLOSED)
        update_span(c);
}

bool t1_arena_init(uint64_t bytes)
{
    if (base)
        return true;
    npages = (size_t)(bytes / 4096);
    if (bytes < 4096 + 2 * (uint64_t)T1_CHUNK_BYTES)
        return false; /* stubs, open chunk, reserve */
    nchunks = (unsigned)((bytes - 4096) / T1_CHUNK_BYTES);
    base = hal_alloc_pages(npages, HAL_PAGE_EXEC);
    chunk_pages = (nchunks * sizeof(struct chunk) + 4095) / 4096;
    chunks = t1_memory_alloc(chunk_pages, HAL_PAGE_ZERO);
    if (!base || !chunks) {
        t1_arena_shutdown();
        return false;
    }
    memset(chunks, 0, nchunks * sizeof(struct chunk));
    open_chunk = NONE;
    hand = 0;
    span_hand = 0;
    memset(span_counts, 0, sizeof(span_counts));
    nfree = nchunks;
    bytes_used = 4096;
    return true;
}

void t1_arena_shutdown(void)
{
    if (base) {
        hal_code_set_writable(base, npages);
        hal_free_pages(base, npages);
    }
    if (chunks)
        t1_memory_free(chunks, chunk_pages);
    base = NULL;
    chunks = NULL;
    npages = chunk_pages = 0;
    nchunks = nfree = 0;
    span_hand = 0;
    memset(span_counts, 0, sizeof(span_counts));
    open_chunk = NONE;
    bytes_used = 0;
}

uint8_t *t1_arena_stub_page(void)
{
    return base;
}

static uint64_t t1_arena_bytes_used_locked(void)
{
    return bytes_used;
}

static void seal_range(unsigned c, uint32_t end)
{
    struct chunk *k = &chunks[c];
    if (end <= k->sealed)
        return;
    hal_code_set_executable(chunk_base(c) + k->sealed, (end - k->sealed) / 4096);
    k->sealed = end;
    T1_COUNT(seals, 1);
    for (struct t1_region *r = k->regions; r; r = r->chunk_next)
        if (!r->sealed && (uint32_t)(r->code + r->code_size - chunk_base(c)) <= end)
            r->sealed = true;
    if (k->used < end) {
#if IA64VMM_TIER1_STATS
        k->padding[end / 4096 - 1] = (uint16_t)(end - k->used);
#endif
        k->used = end;
    }
}

static void free_if_empty(unsigned c);

/* The open chunk takes no more code: seal what it holds. Eviction may have
 * emptied it while open. */
static void close_open(void)
{
    if (open_chunk == NONE)
        return;
    struct chunk *k = &chunks[open_chunk];
    seal_range(open_chunk, page_up(k->used));
    k->state = CHUNK_CLOSED;
    update_span(open_chunk);
    unsigned c = open_chunk;
    open_chunk = NONE;
    free_if_empty(c);
}

static void t1_arena_seal_for_entry_locked(struct t1_region *r)
{
    if (r->sealed)
        return;
    uint32_t end = page_up((uint32_t)(r->code + r->code_size - chunk_base(r->chunk)));
    seal_range(r->chunk, end);
    if (r->chunk == open_chunk && chunks[r->chunk].used >= T1_CHUNK_BYTES)
        close_open();
}

static void link_into(unsigned c, struct t1_region *r)
{
    struct chunk *k = &chunks[c];
    r->chunk = c;
    r->chunk_next = k->regions;
    k->regions = r;
    ++k->live;
    reference_pages(c, r, true);
}

/* An empty closed chunk is writable again and free. */
static void free_if_empty(unsigned c)
{
    struct chunk *k = &chunks[c];
    if (k->live || k->state != CHUNK_CLOSED)
        return;
    if (k->occupied)
        hal_panic("tier1: empty chunk still has code page references");
    if (k->sealed)
        hal_code_set_writable(chunk_base(c), k->sealed / 4096);
    k->used = k->sealed = 0;
    k->regions = NULL;
    k->state = CHUNK_FREE;
    update_span(c);
    ++nfree;
}

static void t1_arena_release_locked(struct t1_region *r)
{
    if (!r->code)
        return;
    struct chunk *k = &chunks[r->chunk];
    struct t1_region **p = &k->regions;
    while (*p && *p != r)
        p = &(*p)->chunk_next;
    if (*p)
        *p = r->chunk_next;
    r->chunk_next = NULL;
    --k->live;
    reference_pages(r->chunk, r, false);
    bytes_used -= (r->code_size + 15) & ~(size_t)15;
    r->code = NULL;
    r->entry = NULL;
    free_if_empty(r->chunk);
}

static void patch(struct t1_region *r)
{
    uintptr_t stub_base = t1_stub.exit_save;
    for (unsigned k = 0; k < r->nfix; ++k) {
        uintptr_t target = stub_base + (intptr_t)r->fix[k].stub_offset;
        int64_t rel = (int64_t)target - (int64_t)((uintptr_t)r->code + r->fix[k].at + 4);
        int32_t r32 = (int32_t)rel;
        if (rel != r32)
            hal_panic("tier1: stub out of rel32 reach");
        memcpy(r->code + r->fix[k].at, &r32, 4);
    }
}

static unsigned take_free(void)
{
    for (unsigned n = 0; n < nchunks; ++n) {
        unsigned c = (hand + n) % nchunks;
        if (chunks[c].state == CHUNK_FREE) {
            chunks[c].state = CHUNK_OPEN;
            chunks[c].used = chunks[c].sealed = 0;
#if IA64VMM_TIER1_STATS
            memset(chunks[c].padding, 0, sizeof(chunks[c].padding));
#endif
            --nfree;
            return c;
        }
    }
    return NONE;
}

/* The next closed chunk at the hand. */
static unsigned victim(void)
{
    for (unsigned n = 0; n < nchunks; ++n) {
        unsigned c = hand;
        hand = (hand + 1) % nchunks;
        if (chunks[c].state == CHUNK_CLOSED)
            return c;
    }
    return NONE;
}

/* Provide an open chunk, or request contiguous pages for epoch reclamation.
 * Only the unregistered executor copies survivors into the reserve. */
static bool recycle(unsigned pages)
{
    /* Executors may hold this code: no move, no eviction here. */
    if (t1_epoch_reclaim()) {
        if (nfree) {
            open_chunk = take_free();
            return true;
        }
        t1_request_code_chunk(pages);
        return false;
    }
    if (t1_any_current())
        hal_panic("tier1: arena recycling inside region code");
    unsigned c = victim();
    if (c == NONE)
        return false;
    unsigned d = take_free();
    if (d == NONE)
        hal_panic("tier1: arena reserve chunk missing");
    open_chunk = d;
    struct chunk *k = &chunks[c], *dk = &chunks[d];
    T1_COUNT(chunk_recycles, 1);
    for (struct t1_region *r = k->regions, *next; r; r = next) {
        next = r->chunk_next;
        if (!r->valid)
            continue; /* retired: reclaimed below */
        if (!r->referenced) {
            t1_region_evict(r);
            continue;
        }
        /* Second chance: copy the code, fix up its stub branches. */
        size_t size = (r->code_size + 15) & ~(size_t)15;
        if (dk->used + size > T1_CHUNK_BYTES)
            hal_panic("tier1: re-placed code exceeds a chunk");
        uint8_t *to = chunk_base(d) + dk->used;
        memcpy(to, r->code, r->code_size);
        struct t1_region **p = &k->regions;
        while (*p != r)
            p = &(*p)->chunk_next;
        *p = r->chunk_next;
        --k->live;
        reference_pages(c, r, false);
        r->code = to;
        r->entry = (t1_entry_fn)(uintptr_t)to;
        r->sealed = false;
        r->referenced = false;
        r->moved = 1;
        patch(r);
        dk->used += (uint32_t)size;
        link_into(d, r);
        T1_COUNT(replacements, 1);
    }
    /* Reclaim evicted/retired records; the emptied victim becomes the next
     * writable reserve, even when all survivors moved. */
    t1_reclaim_passed();
    if (k->live)
        hal_panic("tier1: recycled chunk still holds regions");
    free_if_empty(c);
    if (k->state != CHUNK_FREE)
        hal_panic("tier1: recycled chunk not free");
    return true;
}

/* No compiler owns a closed chunk. Its empty sealed pages contain neither
 * pending code nor code still held by an executor's retirement epoch. */
static bool place_span(struct t1_region *r, const struct t1_emitted *e, unsigned size)
{
    unsigned pages = page_up(size) / 4096;
    if (!span_available(pages))
        return false;
    for (unsigned n = 0; n < nchunks; ++n) {
        unsigned c = span_hand;
        span_hand = (span_hand + 1) % nchunks;
        struct chunk *k = &chunks[c];
        if (k->max_span < pages)
            continue;
        unsigned mask = empty_pages(k), starts = mask;
        for (unsigned p = 1; p < pages; ++p)
            starts &= mask >> p;
        unsigned first = (unsigned)__builtin_ctz(starts);
        uint8_t *to = chunk_base(c) + first * 4096;
        hal_code_set_writable(to, pages);
        r->code = to;
        r->code_size = e->size;
        r->entry = (t1_entry_fn)(uintptr_t)to;
        r->sealed = false;
        r->referenced = false;
        memcpy(to, e->buf, e->size);
        patch(r);
        hal_code_set_executable(to, pages);
        r->sealed = true;
        bytes_used += size;
        link_into(c, r);
        T1_COUNT(seals, 1);
#if IA64VMM_TIER1_STATS
        memset(k->padding + first, 0, pages * sizeof(k->padding[0]));
        k->padding[first + pages - 1] = (uint16_t)(pages * 4096 - size);
        T1_COUNT(span_reuses, 1);
        T1_COUNT(span_reuse_pages, pages);
#endif
        t1_code_chunk_satisfied(pages);
        return true;
    }
    hal_panic("tier1: reusable span count disagrees with chunks");
}

static bool t1_arena_place_locked(struct t1_region *r, const struct t1_emitted *e)
{
    size_t size = (e->size + 15) & ~(size_t)15;
    if (!base || size > T1_CHUNK_BYTES || !e->size)
        return false;
    for (unsigned turn = 0; turn < 2 * nchunks + 2; ++turn) {
        if (open_chunk != NONE && chunks[open_chunk].used + size <= T1_CHUNK_BYTES) {
            struct chunk *k = &chunks[open_chunk];
            r->code = chunk_base(open_chunk) + k->used;
            r->code_size = e->size;
            r->entry = (t1_entry_fn)(uintptr_t)r->code;
            r->sealed = false;
            r->referenced = false;
            k->used += (uint32_t)size;
            bytes_used += size;
            memcpy(r->code, e->buf, e->size);
            patch(r);
            link_into(open_chunk, r);
            t1_code_chunk_satisfied(page_up((uint32_t)size) / 4096);
            return true;
        }
        close_open();
        /* Keep one free chunk as the reserve for re-placement. */
        if (nfree >= 2) {
            open_chunk = take_free();
            continue;
        }
        if (t1_epoch_reclaim() && place_span(r, e, (unsigned)size))
            return true;
        if (!recycle(page_up((uint32_t)size) / 4096))
            return false;
    }
    return false;
}

static bool t1_arena_recycle_one_locked(void)
{
    bool background = t1_background_work();
    if (!t1_epoch_reclaim() && t1_any_current())
        hal_panic("tier1: arena recycling inside region code");
    for (unsigned n = 0; n < (background ? 1u : 2 * nchunks); ++n) {
        unsigned c = victim();
        if (c == NONE)
            return false;
        bool freed = false;
        unsigned budget = background ? 64u : UINT32_MAX;
        for (struct t1_region *r = chunks[c].regions, *next; r && budget--; r = next) {
            next = r->chunk_next;
            if (!r->valid) {
                freed = true;
                continue;
            }
            if (r->referenced)
                r->referenced = false; /* second chance */
            else {
                t1_region_evict(r);
                freed = true;
            }
        }
        if (freed) {
            t1_reclaim_passed();
            return true;
        }
    }
    return false;
}

#ifdef IA64VMM_HOSTED
/* Unit tests place hand-built code without a region record: a whole free
 * chunk, RW, released by t1_arena_free. */
uint8_t *t1_arena_alloc(size_t n)
{
    if (!base || !n || n > T1_CHUNK_BYTES / 4096 || nfree < 2)
        return NULL;
    unsigned c = take_free();
    chunks[c].state = CHUNK_RAW;           /* never a recycling victim */
    chunks[c].sealed = (uint32_t)n * 4096; /* the caller seals; released whole */
    return chunk_base(c);
}

void t1_arena_free(uint8_t *p, size_t n)
{
    unsigned c = (unsigned)((size_t)(p - base - 4096) / T1_CHUNK_BYTES);
    (void)n;
    if (c >= nchunks || chunks[c].state != CHUNK_RAW)
        hal_panic("tier1: t1_arena_free of a non-raw chunk");
    hal_code_set_writable(chunk_base(c), chunks[c].sealed / 4096);
    chunks[c] = (struct chunk){0};
    ++nfree;
}
#endif

uint64_t t1_arena_bytes_used(void)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    uint64_t result = t1_arena_bytes_used_locked();
    if (take)
        code_unlock();
    return result;
}

#if IA64VMM_TIER1_STATS
uint64_t t1_arena_get_stats(struct t1_arena_stats *out)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    memset(out, 0, sizeof(*out));
    for (unsigned c = 0; c < nchunks; ++c) {
        const struct chunk *k = &chunks[c];
        ++out->chunks[k->state];
        if (k->state == CHUNK_FREE || k->state == CHUNK_RAW)
            continue;
        if (k->state == CHUNK_OPEN) {
            out->open_tail += T1_CHUNK_BYTES - k->used;
            out->unsealed_used += k->used - k->sealed;
        } else
            out->closed_tail += T1_CHUNK_BYTES - k->sealed;
        uint32_t occupied[T1_CHUNK_BYTES / 4096] = {0};
        for (const struct t1_region *r = k->regions; r; r = r->chunk_next) {
            uint32_t start = (uint32_t)(r->code - chunk_base(c));
            uint32_t size = (r->code_size + 15u) & ~15u;
            uint32_t end = start + size;
            out->code_bytes[r->retired ? 2 : r->valid ? 1 : 0] += size;
            for (unsigned p = start / 4096; p <= (end - 1) / 4096; ++p) {
                uint32_t lo = start > p * 4096 ? start : p * 4096;
                uint32_t hi = end < (p + 1) * 4096 ? end : (p + 1) * 4096;
                occupied[p] += hi - lo;
            }
        }
        for (unsigned p = 0; p < k->sealed / 4096; ++p)
            if (!occupied[p])
                out->sealed_empty += 4096;
            else {
                out->sealed_code += occupied[p];
                out->sealed_padding += k->padding[p];
                out->sealed_holes += 4096 - occupied[p] - k->padding[p];
            }
    }
    uint64_t result = t1_arena_bytes_used_locked();
    if (take)
        code_unlock();
    return result;
}
#endif

void t1_arena_seal_for_entry(struct t1_region *r)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    t1_arena_seal_for_entry_locked(r);
    if (take)
        code_unlock();
}

void t1_arena_release(struct t1_region *r)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    t1_arena_release_locked(r);
    if (take)
        code_unlock();
}

bool t1_arena_place(struct t1_region *r, const struct t1_emitted *e)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool result = t1_arena_place_locked(r, e);
    if (take)
        code_unlock();
    return result;
}

bool t1_arena_recycle_one(void)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool result = t1_arena_recycle_one_locked();
    if (take)
        code_unlock();
    return result;
}

unsigned t1_arena_free_chunks(void)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    unsigned result = nfree;
    if (take)
        code_unlock();
    return result;
}

bool t1_arena_has_space(unsigned pages)
{
    if (!pages || pages > CHUNK_PAGES)
        return false;
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool result = nfree || (t1_epoch_reclaim() && span_available(pages));
    if (take)
        code_unlock();
    return result;
}

bool t1_arena_place_background(struct t1_region *r, const struct t1_emitted *e, unsigned *chunk)
{
    code_lock();
    unsigned saved = open_chunk;
    open_chunk = *chunk;
    bool result = t1_arena_place_locked(r, e);
    *chunk = open_chunk;
    open_chunk = saved;
    code_unlock();
    return result;
}

void t1_arena_seal_background(unsigned *chunk)
{
    code_lock();
    if (*chunk != NONE) {
        unsigned saved = open_chunk;
        open_chunk = *chunk;
        seal_range(open_chunk, page_up(chunks[open_chunk].used));
        if (chunks[open_chunk].used == T1_CHUNK_BYTES)
            close_open();
        *chunk = open_chunk;
        open_chunk = saved;
    }
    code_unlock();
}

void t1_arena_close_background(unsigned *chunk)
{
    code_lock();
    unsigned saved = open_chunk;
    open_chunk = *chunk;
    unsigned closing = open_chunk;
    close_open();
    if (closing != NONE)
        free_if_empty(closing);
    *chunk = NONE;
    open_chunk = saved;
    code_unlock();
}
