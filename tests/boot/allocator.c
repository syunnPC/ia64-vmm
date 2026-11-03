/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production allocator without installing host page tables.
 * Non-executable, non-zeroed allocations only touch its metadata. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "host/amd64/memory.c"

void hal_panic(const char *message)
{
    fputs(message, stderr);
    abort();
}
void amd64_tlb_note(uint64_t address)
{
    (void)address;
    abort();
}
void amd64_tlb_shootdown(void)
{
    abort();
}
static void first_fit(void)
{
    enum { PREFIX = 1u << 20, WINDOW = 128, SLOTS = 32 };
    struct {
        void *p;
        unsigned first, n;
    } slots[SLOTS] = {0};
    bool occupied[WINDOW] = {0};
    arena_base = UINT64_C(32) << 30;
    amd64_hhdm = UINT64_C(1) << 40;
    arena_pages = PREFIX + WINDOW;
    metadata_pages = (arena_pages * sizeof(*allocations) + PAGE - 1) / PAGE;
    allocations = calloc(arena_pages, sizeof(*allocations));
    assert(allocations);
    for (unsigned i = 0; i < PREFIX; ++i)
        allocations[i].kind = 1;
    allocated_pages = peak_pages = PREFIX;
    first_free = metadata_pages;
    uint32_t random = 1;
    for (unsigned step = 0; step < 2000; ++step) {
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        unsigned slot = (random >> 8) % SLOTS;
        if (slots[slot].p) {
            hal_free_pages(slots[slot].p, slots[slot].n);
            for (unsigned j = 0; j < slots[slot].n; ++j)
                occupied[slots[slot].first + j] = false;
            slots[slot].p = NULL;
        } else {
            unsigned n = 1 + ((random >> 16) % 12), expected = WINDOW;
            for (unsigned i = 0; i + n <= WINDOW; ++i) {
                unsigned j = 0;
                while (j < n && !occupied[i + j])
                    ++j;
                if (j == n) {
                    expected = i;
                    break;
                }
            }
            void *p = hal_alloc_pages(n, 0);
            assert((p != NULL) == (expected != WINDOW));
            if (p) {
                assert((uintptr_t)p ==
                       amd64_hhdm + arena_base + ((uint64_t)PREFIX + expected) * PAGE);
                slots[slot].p = p;
                slots[slot].first = expected;
                slots[slot].n = n;
                for (unsigned j = 0; j < n; ++j)
                    occupied[expected + j] = true;
            }
        }
        unsigned used = 0, earliest = WINDOW;
        for (unsigned i = 0; i < WINDOW; ++i) {
            used += occupied[i];
            if (!occupied[i] && earliest == WINDOW)
                earliest = i;
        }
        struct hal_memory_stats m;
        assert(hal_memory_get_stats(&m) && m.used_bytes == ((uint64_t)PREFIX + used) * PAGE);
        assert(first_free <= PREFIX + earliest);
    }
    free(allocations);
    allocations = NULL;
    puts("allocator: first-fit/reference agreement through 2000 fragmented operations above 4 GiB");
}
static void table_budget(void)
{
    const uint64_t gib = UINT64_C(1) << 30;
    assert(reservation_tables(gib) == 2048);
    assert(reservation_tables(2 * gib - (2u << 20) - PAGE) == 2048);
    assert(reservation_tables(2 * gib - (2u << 20)) == 4096);
    assert(reservation_tables(2 * gib) == 4096);
    table_capacity = reservation_tables(16 * gib);
    tables = aligned_alloc(PAGE, table_capacity * sizeof(*tables));
    assert(tables);
    memset(tables, 0, table_capacity * sizeof(*tables));
    tables_used = 0;
    pages_1g = live = false;
    amd64_hhdm = UINT64_C(0xffff800000000000);
    new_table();
    for (unsigned i = 256; i < 512; ++i)
        descend(tables[0], i, 4);
    /* 32 GiB host, both aliases of 1.5 GiB code, 16 MiB kernel, and
     * sparse MMIO pages in 512 distinct 2 MiB ranges. No 1 GiB pages. */
    map_range(amd64_hhdm, 0, 32 * gib, PTE_W | PTE_NX, true);
    uint64_t code = gib + gib / 2, physical = 16 * gib + (40u << 20);
    map_range(AMD64_CODE_BASE, physical, code, PTE_NX, false);
    map_range(amd64_hhdm + physical, physical, code, PTE_NX, false);
    map_range(UINT64_C(0xffffffff80000000), 16u << 20, 16u << 20, 0, false);
    for (unsigned i = 0; i < 512; ++i) {
        uint64_t mmio = 64 * gib + ((uint64_t)i << 21);
        map_range(amd64_hhdm + mmio, mmio, PAGE, PTE_W | PTE_NX | PTE_PCD, false);
    }
    assert(tables_used > 2048 && tables_used < table_capacity);
    free(tables);
    tables = NULL;
    puts("page tables: large code aliases and sparse MMIO fit without 1 GiB pages");
}
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "tables")) {
        table_budget();
        return 0;
    }
    const uint64_t gib = UINT64_C(1) << 30;
    const unsigned sizes[] = {2, 3, 4, 6, 8, 12, 16};
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
        arena_base = 32 * gib;
        amd64_hhdm = UINT64_C(1) << 40;
        arena_pages = sizes[k] * gib / PAGE;
        metadata_pages = (arena_pages * sizeof(*allocations) + PAGE - 1) / PAGE;
        allocations = calloc(arena_pages, sizeof(*allocations));
        assert(allocations);
        for (size_t i = 0; i < metadata_pages; ++i)
            allocations[i].kind = 1;
        allocated_pages = peak_pages = first_free = metadata_pages;
        size_t n = arena_pages - metadata_pages;
        void *p = hal_alloc_pages(n, 0);
        assert((uintptr_t)p == amd64_hhdm + arena_base + metadata_pages * PAGE);
        assert(allocations[metadata_pages].span == n);
        assert(first_free == arena_pages);
        assert(!hal_alloc_pages(1, 0));
        struct hal_memory_stats m;
        assert(hal_memory_get_stats(&m) && m.total_bytes == sizes[k] * gib);
        assert(m.used_bytes == m.total_bytes && m.peak_bytes == m.total_bytes);
        hal_free_pages(p, n);
        assert(first_free == metadata_pages);
        assert(hal_memory_get_stats(&m) && m.used_bytes == metadata_pages * PAGE);
        if (sizes[k] == 16) {
            void *a = hal_alloc_pages(6 * gib / PAGE, 0);
            void *b = hal_alloc_pages(8 * gib / PAGE, 0);
            assert(a && b && (uintptr_t)b - (uintptr_t)a == 6 * gib);
            assert(!hal_alloc_pages(3 * gib / PAGE, 0));
            hal_free_pages(a, 6 * gib / PAGE);
            assert(!hal_alloc_pages(7 * gib / PAGE, 0));
            a = hal_alloc_pages(4 * gib / PAGE, 0);
            assert(a && (uintptr_t)a == amd64_hhdm + arena_base + metadata_pages * PAGE);
            hal_free_pages(a, 4 * gib / PAGE);
            hal_free_pages(b, 8 * gib / PAGE);
            assert(hal_memory_get_stats(&m) && m.used_bytes == metadata_pages * PAGE);
        }
        free(allocations);
        allocations = NULL;
    }
    first_fit();
    table_budget();
    puts("allocator: 2..16 GiB capacity, >4 GiB spans/offsets/accounting and fragmentation passed");
}
