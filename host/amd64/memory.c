/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/amd64.h"
#include "host/amd64/boot_diag.h"
#include "host/amd64/smp.h"
#include "engine/hostwin/hostwin.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define PAGE UINT64_C(4096)
#define PTE_P UINT64_C(1)
#define PTE_W UINT64_C(2)
#define PTE_PWT UINT64_C(8)
#define PTE_PAT_4K UINT64_C(128)
#define PTE_PAT_LARGE UINT64_C(4096)
#define PTE_PAT_SELECT (UINT64_C(1) << 62)
#define PTE_PCD UINT64_C(16)
#define PTE_PS UINT64_C(128)
#define PTE_G UINT64_C(256)
#define PTE_NX (UINT64_C(1) << 63)
#define PTE_ADDR UINT64_C(0x000ffffffffff000)
#define TABLE_COUNT 2048
static uint64_t (*tables)[512];
static unsigned allocator_lock;
static unsigned tables_used, table_capacity = TABLE_COUNT;
static unsigned reservation_tables(uint64_t size)
{
    /* Large code arenas need both code and HHDM aliases in 4 KiB pages. */
    /* Boot keeps a 2 MiB crash ring outside this allocator capacity. */
    return size >= (UINT64_C(2) << 30) - (2u << 20) ? 2 * TABLE_COUNT : TABLE_COUNT;
}
static bool live, pages_1g;
extern bool amd64_fb_wc; /* Set by CPU PAT setup before building mappings. */
uint64_t amd64_hhdm, amd64_image_pa, amd64_image_va;
struct allocation {
    uint32_t span;
    uint8_t kind;
};
static struct allocation *allocations;
static uint64_t arena_base;
static size_t arena_pages, metadata_pages, first_free;
static size_t allocated_pages, peak_pages;

static uint64_t *new_table(void)
{
    if (tables_used == table_capacity)
        hal_panic("host page table pool exhausted");
    uint64_t *p = tables[tables_used++];
    memset(p, 0, PAGE);
    return p;
}
static uint64_t *table_pointer(uint64_t entry)
{
    uint64_t pa = entry & PTE_ADDR;
    return (uint64_t *)(uintptr_t)(amd64_hhdm + pa);
}
static uint64_t *descend(uint64_t *table, unsigned index, unsigned level)
{
    uint64_t old = table[index];
    if (!(old & PTE_P)) {
        uint64_t *next = new_table();
        __atomic_store_n(&table[index], ((uintptr_t)next - amd64_hhdm) | PTE_P | PTE_W,
                         __ATOMIC_RELEASE);
        return next;
    }
    if (old & PTE_PS) {
        if (level != 3 && level != 2)
            hal_panic("invalid large-page level");
        uint64_t *next = new_table();
        uint64_t step = UINT64_C(1) << (12 + 9 * (level - 2));
        /* Bit 12 is PAT in large leaves, but a physical address bit in
         * 4 KiB leaves. Transfer it to bit 7 when splitting a 2 MiB leaf. */
        uint64_t base = old & PTE_ADDR & ~PTE_PAT_LARGE;
        uint64_t attrs = old & ~PTE_ADDR & ~PTE_PS;
        if (old & PTE_PAT_LARGE)
            attrs |= level == 3 ? PTE_PAT_LARGE : PTE_PAT_4K;
        if (level == 3)
            attrs |= PTE_PS;
        for (unsigned i = 0; i < 512; ++i)
            next[i] = (base + i * step) | attrs;
        __atomic_store_n(&table[index], ((uintptr_t)next - amd64_hhdm) | PTE_P | PTE_W,
                         __ATOMIC_RELEASE);
    }
    return table_pointer(table[index]);
}
static uint64_t *leaf(uint64_t va, unsigned target)
{
    uint64_t *table = tables[0];
    for (unsigned level = 4; level > target; --level)
        table = descend(table, (va >> (12 + 9 * (level - 1))) & 511, level);
    return &table[(va >> (12 + 9 * (target - 1))) & 511];
}
static void map_page(uint64_t va, uint64_t pa, unsigned level, uint64_t attrs)
{
    uint64_t *pte = leaf(va, level);
    /* Shared boundary pages may already have a smaller mapping. Preserve
     * that table rather than abandoning a previously protected subpage. */
    if (level > 1 && (*pte & PTE_P) && !(*pte & PTE_PS)) {
        uint64_t step = UINT64_C(1) << (12 + 9 * (level - 2));
        for (unsigned i = 0; i < 512; ++i)
            map_page(va + i * step, pa + i * step, level - 1, attrs);
        return;
    }
    /* Carry a logical PAT selector through recursive mapping; its hardware
     * bit differs between 4 KiB and 2 MiB/1 GiB leaves (SDM Vol.3A 12.12). */
    bool pat = (attrs & PTE_PAT_SELECT) != 0;
    attrs &= ~PTE_PAT_SELECT;
    if (pat)
        attrs |= level == 1 ? PTE_PAT_4K : PTE_PAT_LARGE;
    if (va >> 63)
        attrs |= PTE_G;
    bool was_present = (*pte & PTE_P) != 0;
    __atomic_store_n(pte, pa | attrs | PTE_P | (level > 1 ? PTE_PS : 0), __ATOMIC_RELEASE);
    /* A not-present entry is never cached; only replaced mappings need
     * local and remote invalidation. */
    if (live && was_present) {
        amd64_invlpg(va);
        amd64_tlb_note(va);
    }
}
static void map_range(uint64_t va, uint64_t pa, uint64_t size, uint64_t attrs, bool large)
{
    if (!size)
        return;
    if (size > UINT64_MAX - pa || size > UINT64_MAX - va || pa + size > PTE_ADDR ||
        ((va ^ pa) & (PAGE - 1)))
        hal_panic("host mapping address out of range");
    uint64_t end = IA64VMM_ALIGN_UP(pa + size, PAGE);
    uint64_t offset = pa & (PAGE - 1);
    pa -= offset;
    va -= offset;
    while (pa < end) {
        unsigned level = 1;
        uint64_t step = PAGE;
        if (large && !((pa | va) & ((UINT64_C(1) << 21) - 1)) && end - pa >= (UINT64_C(1) << 21)) {
            level = 2;
            step = UINT64_C(1) << 21;
            if (pages_1g && !((pa | va) & ((UINT64_C(1) << 30) - 1)) &&
                end - pa >= (UINT64_C(1) << 30)) {
                level = 3;
                step = UINT64_C(1) << 30;
            }
        }
        map_page(va, pa, level, attrs);
        pa += step;
        va += step;
    }
}
static void kernel_segment(char *start, char *end, uint64_t attrs)
{
    uint64_t pa = amd64_image_phys(start);
    map_range((uintptr_t)start, pa, end - start, attrs, false);
    /* Even a non-executable alias must not permit writes to RX/RO pages. */
    map_range(amd64_hhdm + pa, pa, end - start, (attrs & PTE_W) | PTE_NX, false);
}
void amd64_memory_init(const struct boot_info *bi, uint64_t base, uint64_t size)
{
    uint32_t a, b, c, d;
    amd64_cpuid(0x80000001, 0, &a, &b, &c, &d);
    pages_1g = (d & (1u << 26)) != 0;
    arena_base = base;
    arena_pages = size / PAGE;
    metadata_pages = IA64VMM_ALIGN_UP(arena_pages * sizeof(*allocations), PAGE) / PAGE;
    if (metadata_pages >= arena_pages)
        hal_panic("VMM reservation too small for allocator");
    allocations = (struct allocation *)(uintptr_t)(amd64_hhdm + base);
    amd64_boot_diag_metadata((uintptr_t)allocations, metadata_pages * PAGE);
    memset(allocations, 0, metadata_pages * PAGE);
    amd64_boot_diag_written();
    for (size_t i = 0; i < metadata_pages; ++i)
        allocations[i].kind = 1;
    allocated_pages = peak_pages = first_free = metadata_pages;
    table_capacity = reservation_tables(size);
    tables = hal_alloc_pages(table_capacity, HAL_PAGE_ZERO);
    if (!tables)
        hal_panic("VMM reservation too small for page table pool");
    new_table();
    /* Every CPU root shares stable upper-half PDPT pointers, even
     * for code-cache and MMIO mappings first created after AP startup. */
    for (unsigned i = 256; i < 512; ++i)
        descend(tables[0], i, 4);
    uint64_t fb_attrs = PTE_PCD;
    if (amd64_fb_wc)
        fb_attrs |= PTE_PWT | PTE_PAT_SELECT;
    for (unsigned i = 0; i < bi->mem_range_count; ++i) {
        const struct boot_mem_range *m = &bi->mem[i];
        if (m->size > UINT64_MAX - m->base || m->base + m->size >= AMD64_CODE_BASE - amd64_hhdm)
            hal_panic("physical map overlaps the code-cache window");
        uint64_t attrs = PTE_W | PTE_NX;
        if (m->type == BOOT_MEM_FRAMEBUFFER)
            attrs |= fb_attrs;
        else if (m->type == BOOT_MEM_RESERVED || m->type == BOOT_MEM_BAD)
            attrs |= PTE_PCD;
        map_range(amd64_hhdm + m->base, m->base, m->size, attrs, true);
    }
    if (bi->fb.hva)
        map_range(amd64_hhdm + bi->fb.base, bi->fb.base, bi->fb.size, PTE_W | PTE_NX | fb_attrs,
                  true);
    kernel_segment(__text_start, __text_end, 0);
    kernel_segment(__rodata_start, __rodata_end, PTE_NX);
    kernel_segment(__data_start, __image_end, PTE_W | PTE_NX);
    /* Discard any global translations inherited from the bootloader too. */
    uint64_t cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    __asm__ volatile("mov %0,%%cr4; mov %1,%%cr3; mov %2,%%cr4" ::"r"(cr4 & ~UINT64_C(128)),
                     "r"((uintptr_t)tables[0] - amd64_hhdm), "r"(cr4)
                     : "memory");
    live = true;
    LOGI("own CR3=%016llx tables=%u HHDM=%016llx large-pages=%s framebuffer=%s CR0.WP=1 NXE=1",
         (unsigned long long)(uintptr_t)tables[0] - amd64_hhdm, tables_used,
         (unsigned long long)amd64_hhdm, pages_1g ? "1GiB/2MiB" : "2MiB",
         amd64_fb_wc ? "WC (PAT[7])" : "UC- (PAT unavailable)");
}

static void *allocate(size_t npages, unsigned flags)
{
    if (!allocations || !npages || npages > arena_pages - metadata_pages)
        return NULL;
    /* All pages below first_free are occupied. Freeing any earlier run
     * lowers it, preserving first-fit order without rescanning that prefix. */
    while (first_free < arena_pages && allocations[first_free].kind)
        ++first_free;
    size_t run = 0;
    for (size_t i = first_free; i < arena_pages; ++i) {
        run = allocations[i].kind ? 0 : run + 1;
        if (run != npages)
            continue;
        size_t start = i + 1 - npages;
        uint64_t pa = arena_base + start * PAGE;
        uint64_t va = (flags & HAL_PAGE_EXEC) ? AMD64_CODE_BASE + start * PAGE : amd64_hhdm + pa;
        for (size_t j = start; j <= i; ++j) {
            allocations[j].kind = flags & HAL_PAGE_EXEC ? 2 : 1;
            allocations[j].span = j == start ? (uint32_t)npages : 0;
        }
        if (start == first_free)
            first_free = i + 1;
        if (flags & HAL_PAGE_EXEC)
            map_range(va, pa, npages * PAGE, PTE_W | PTE_NX, false);
        void *p = (void *)(uintptr_t)va;
        if (flags & HAL_PAGE_ZERO)
            memset(p, 0, npages * PAGE);
        allocated_pages += npages;
        if (allocated_pages > peak_pages)
            peak_pages = allocated_pages;
        return p;
    }
    return NULL;
}
static size_t allocation_index(void *p, size_t npages, bool *code, bool whole)
{
    uint64_t va = (uintptr_t)p;
    *code = va >= AMD64_CODE_BASE && va - AMD64_CODE_BASE < arena_pages * PAGE;
    uint64_t pa = *code ? arena_base + va - AMD64_CODE_BASE : va - amd64_hhdm;
    if (!allocations || !npages || (pa & (PAGE - 1)) || pa < arena_base ||
        pa - arena_base >= arena_pages * PAGE)
        hal_panic("invalid VMM allocation pointer");
    size_t index = (pa - arena_base) / PAGE;
    if (index < metadata_pages || npages > arena_pages - index ||
        (whole && npages != allocations[index].span))
        hal_panic("invalid VMM allocation length");
    for (size_t i = 0; i < npages; ++i)
        if ((*code ? allocations[index + i].kind < 2 : allocations[index + i].kind != 1) ||
            (i && allocations[index + i].span))
            hal_panic("VMM allocation range crosses allocation boundary");
    return index;
}
static void code_protect(void *p, size_t npages, bool executable)
{
    bool code;
    size_t start = allocation_index(p, npages, &code, false);
    if (!code)
        hal_panic("W^X requested on non-code pages");
    for (size_t j = 0; j < npages; ++j) {
        uint64_t pa = arena_base + (start + j) * PAGE;
        /* Remove writing from both aliases before enabling execution. On
         * the reverse transition remove execution before allowing writes. */
        if (executable) {
            map_page(amd64_hhdm + pa, pa, 1, PTE_NX);
            map_page(AMD64_CODE_BASE + (start + j) * PAGE, pa, 1, 0);
        } else {
            map_page(AMD64_CODE_BASE + (start + j) * PAGE, pa, 1, PTE_W | PTE_NX);
            map_page(amd64_hhdm + pa, pa, 1, PTE_W | PTE_NX);
        }
        allocations[start + j].kind = executable ? 3 : 2;
    }
}
static void free_pages(void *p, size_t npages)
{
    if (!p)
        return;
    bool code;
    size_t start = allocation_index(p, npages, &code, true);
    if (code) {
        code_protect(p, npages, false);
        for (size_t j = 0; j < npages; ++j) {
            uint64_t va = (uintptr_t)p + j * PAGE;
            *leaf(va, 1) = 0;
            amd64_invlpg(va);
            amd64_tlb_note(va);
        }
    }
    memset(allocations + start, 0, npages * sizeof(*allocations));
    allocated_pages -= npages;
    if (start < first_free)
        first_free = start;
}
/* Interrupt handlers never take this lock. A CPU spinning for it must
 * keep servicing IPIs: the owner may be waiting for its TLB invalidation. */
static void lock_allocator(void)
{
    while (__atomic_exchange_n(&allocator_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static void unlock_allocator(bool mapping_changed)
{
    if (mapping_changed && live)
        amd64_tlb_shootdown();
    __atomic_store_n(&allocator_lock, 0, __ATOMIC_RELEASE);
}
void *hal_alloc_pages(size_t npages, unsigned flags)
{
    lock_allocator();
    void *p = allocate(npages, flags);
    unlock_allocator(p && (flags & HAL_PAGE_EXEC));
    return p;
}
bool hal_memory_get_stats(struct hal_memory_stats *out)
{
    lock_allocator();
    bool ready = allocations != NULL;
    if (ready)
        *out = (struct hal_memory_stats){arena_pages * PAGE, allocated_pages * PAGE,
                                         peak_pages * PAGE};
    unlock_allocator(false);
    return ready;
}
void hal_free_pages(void *p, size_t npages)
{
    lock_allocator();
    bool code = (uintptr_t)p >= AMD64_CODE_BASE;
    free_pages(p, npages);
    unlock_allocator(p && code);
}
void hal_code_set_writable(void *p, size_t npages)
{
    lock_allocator();
    code_protect(p, npages, false);
    unlock_allocator(true);
}
void hal_code_set_executable(void *p, size_t npages)
{
    lock_allocator();
    code_protect(p, npages, true);
    unlock_allocator(true);
}
uint64_t amd64_memory_root(void)
{
    return (uintptr_t)tables[0] - amd64_hhdm;
}
void amd64_map_apic(uint64_t pa)
{
    /* PAT[3] UC, not the weaker UC- used for generic firmware holes. */
    lock_allocator();
    map_range(amd64_hhdm + pa, pa, PAGE, PTE_W | PTE_NX | PTE_PCD | PTE_PWT, false);
    unlock_allocator(true);
}

static uint64_t mapping(uint64_t va, uint64_t *pa)
{
    if (!live || (va >> 48) != ((va & (UINT64_C(1) << 47)) ? 0xffffu : 0u))
        return 0;
    uint64_t *table = tables[0];
    for (unsigned level = 4; level; --level) {
        uint64_t entry =
            __atomic_load_n(&table[(va >> (12 + 9 * (level - 1))) & 511], __ATOMIC_ACQUIRE);
        if (!(entry & PTE_P))
            return 0;
        if (level == 1 || (entry & PTE_PS)) {
            uint64_t mask = (UINT64_C(1) << (12 + 9 * (level - 1))) - 1;
            if (pa)
                *pa = ((entry & PTE_ADDR) & ~mask) | (va & mask);
            return entry;
        }
        table = table_pointer(entry);
    }
    return 0;
}
static uint64_t mapped_entry(uint64_t va)
{
    return mapping(va, NULL);
}
bool amd64_memory_physical(uint64_t va, uint64_t *pa)
{
    return (mapping(va, pa) & PTE_P) != 0;
}
bool amd64_readable(uint64_t va, size_t size)
{
    if (!size || size - 1 > UINT64_MAX - va)
        return false;
    uint64_t end = va + size - 1;
    for (;;) {
        if (!(mapped_entry(va) & PTE_P))
            return false;
        uint64_t next = IA64VMM_ALIGN_DOWN(va, PAGE) + PAGE;
        if (!next || next > end)
            return true;
        va = next;
    }
}
struct remote_code_check {
    uint8_t *code;
    bool execute;
    int result;
};
static void remote_code(void *arg)
{
    struct remote_code_check *test = arg;
    test->result =
        test->execute ? ((int (*)(void))(uintptr_t)test->code)() : *(volatile uint8_t *)test->code;
}
static int check_remote_code(uint8_t *code, bool execute)
{
    struct remote_code_check test = {.code = code, .execute = execute};
    if (!hal_cpu_start(1, remote_code, &test))
        hal_panic("cannot dispatch remote code selftest");
    uint64_t until = hal_counter() + hal_counter_hz() * 3;
    while (hal_cpu_busy(1)) {
        if (hal_counter() >= until)
            hal_panic("remote code selftest timed out");
        hal_cpu_poll();
        if (hal_cpu_busy(1))
            hal_idle_until(until);
    }
    return test.result;
}
struct remote_busy {
    uint8_t *code;
    unsigned phase;
    int result;
};
static void remote_busy(void *arg)
{
    struct remote_busy *test = arg;
    (void)*(volatile uint8_t *)test->code; /* cache the W/NX translation */
    __atomic_store_n(&test->phase, 1, __ATOMIC_RELEASE);
    /* Stay active (interrupts on, not lazy): publication must use the IPI. */
    while (__atomic_load_n(&test->phase, __ATOMIC_ACQUIRE) != 2)
        __asm__ volatile("pause");
    test->result = ((int (*)(void))(uintptr_t)test->code)();
}
static void wait_until(const unsigned *flag, unsigned value, const char *what)
{
    uint64_t until = hal_counter() + hal_counter_hz() * 3;
    while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) != value) {
        if (hal_counter() >= until)
            hal_panic(what);
        __asm__ volatile("pause");
    }
}
#define GLOBAL_TEST_PAGES 70u
struct global_check {
    uint8_t *code;
    unsigned phase, failed;
    bool pge_off;
};
static void remote_global(void *arg)
{
    struct global_check *test = arg;
    uint64_t cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    if (test->pge_off)
        __asm__ volatile("mov %0,%%cr4" ::"r"(cr4 & ~UINT64_C(128)) : "memory");
    for (unsigned i = 0; i < GLOBAL_TEST_PAGES; ++i)
        (void)*(volatile uint8_t *)(test->code + i * PAGE);
    __atomic_store_n(&test->phase, 1, __ATOMIC_RELEASE);
    wait_until(&test->phase, 2, "global shootdown selftest publisher timed out");
    for (unsigned i = 0; i < GLOBAL_TEST_PAGES; ++i)
        if (((int (*)(void))(uintptr_t)(test->code + i * PAGE))() != 42)
            test->failed = 1;
    __asm__ volatile("mov %0,%%cr4" ::"r"(cr4) : "memory");
}
void amd64_memory_selftest(void)
{
    struct hal_memory_stats before, during, after;
    if (!hal_memory_get_stats(&before))
        hal_panic("reserve accounting unavailable");
    void *accounting = hal_alloc_pages(2, 0);
    if (!accounting || !hal_memory_get_stats(&during) || during.total_bytes != before.total_bytes ||
        during.used_bytes != before.used_bytes + 2 * PAGE ||
        during.peak_bytes < during.used_bytes || during.peak_bytes < before.peak_bytes)
        hal_panic("reserve allocation accounting failed");
    hal_free_pages(accounting, 2);
    if (!hal_memory_get_stats(&after) || after.used_bytes != before.used_bytes ||
        after.peak_bytes != during.peak_bytes)
        hal_panic("reserve free accounting failed");
    uint8_t *p = hal_alloc_pages(2, HAL_PAGE_EXEC | HAL_PAGE_ZERO);
    if (!p)
        hal_panic("code allocation selftest exhausted reservation");
    for (unsigned i = 0; i < 8192; ++i)
        if (p[i])
            hal_panic("HAL_PAGE_ZERO failed");
    static const uint8_t code[] = {0xb8, 0x2a, 0, 0, 0, 0xc3};
    static const uint8_t code2[] = {0xb8, 0x2c, 0, 0, 0, 0xc3};
    memcpy(p, code, sizeof(code));
    memcpy(p + PAGE, code2, sizeof(code2));
    /* IPI path: CPU1 caches the writable/NX translation and stays active
     * while the page becomes RX; a missed invalidation faults on execute. */
    struct remote_busy busy = {.code = p};
    if (!hal_cpu_start(1, remote_busy, &busy))
        hal_panic("cannot dispatch busy remote selftest");
    wait_until(&busy.phase, 1, "busy remote selftest readiness timed out");
    hal_code_set_executable(p, 1);
    __atomic_store_n(&busy.phase, 2, __ATOMIC_RELEASE);
    wait_until(&amd64_cpus[1].busy, 0, "busy remote selftest timed out");
    if (busy.result != 42)
        hal_panic("IPI-path remote publication selftest failed");
    if (!(mapped_entry((uintptr_t)p + PAGE) & PTE_W) ||
        !(mapped_entry((uintptr_t)p + PAGE) & PTE_NX))
        hal_panic("W^X subrange changed adjacent page");
    /* Lazy path: CPU1 caches the W/NX translation, then idles; the
     * shootdown skips it and its wakeup must flush before executing. */
    if (check_remote_code(p + PAGE, false) != 0xb8)
        hal_panic("remote code read failed");
    wait_until(&amd64_cpus[1].tlb_lazy, 1, "remote CPU did not become lazy");
    hal_code_set_executable(p + PAGE, 1);
    if (check_remote_code(p + PAGE, true) != 44)
        hal_panic("lazy-path remote publication selftest failed");
    uint64_t pa = arena_base + (uintptr_t)p - AMD64_CODE_BASE;
    if ((mapped_entry((uintptr_t)p) & (PTE_W | PTE_NX)) ||
        (mapped_entry(amd64_hhdm + pa) & PTE_W) || !(mapped_entry(amd64_hhdm + pa) & PTE_NX))
        hal_panic("W^X alias selftest failed");
    if (((int (*)(void))(uintptr_t)p)() != 42 || check_remote_code(p, true) != 42)
        hal_panic("local/remote code execution selftest failed");
    hal_code_set_writable(p, 2);
    p[1] = 43;
    hal_code_set_executable(p, 2);
    if (((int (*)(void))(uintptr_t)p)() != 43 || check_remote_code(p, true) != 43)
        hal_panic("local/remote code reprotection selftest failed");
    hal_free_pages(p, 2);
    if (amd64_readable((uintptr_t)p, 1))
        hal_panic("freed code alias still mapped");
    uint8_t *q = hal_alloc_pages(2, HAL_PAGE_ZERO);
    if (!q || (uintptr_t)q - amd64_hhdm != pa)
        hal_panic("page allocator reuse failed");
    hal_free_pages(q, 2);
    /* Overflow the 64-address batch with cached GLOBAL W/NX aliases.
     * A plain CR3 reload here would retain them and fault on execution. */
    /* Both global and nonglobal cached NX leaves must be invalidated. */
    for (unsigned pge_off = 0; pge_off < 2; ++pge_off) {
        uint8_t *many = hal_alloc_pages(GLOBAL_TEST_PAGES, HAL_PAGE_EXEC | HAL_PAGE_ZERO);
        if (!many)
            hal_panic("global shootdown selftest allocation failed");
        for (unsigned i = 0; i < GLOBAL_TEST_PAGES; ++i)
            memcpy(many + i * PAGE, code, sizeof(code));
        struct global_check global = {.code = many, .pge_off = pge_off != 0};
        if (!hal_cpu_start(1, remote_global, &global))
            hal_panic("cannot dispatch global shootdown selftest");
        wait_until(&global.phase, 1, "global shootdown selftest readiness timed out");
        hal_code_set_executable(many, GLOBAL_TEST_PAGES);
        __atomic_store_n(&global.phase, 2, __ATOMIC_RELEASE);
        wait_until(&amd64_cpus[1].busy, 0, "global shootdown selftest execution timed out");
        if (global.failed)
            hal_panic("global shootdown selftest result failed");
        hal_free_pages(many, GLOBAL_TEST_PAGES);
    }
    LOGI(
        "host memory selftest passed: allocation, W^X aliases, IPI/lazy execution, reuse, global batch overflow with PGE on/off");
}

/* Host mappings for PCI config/BARs may be missing from the UEFI memory map.
 * PAT[3] (PCD|PWT) is UC; all accesses retain their device width and order. */
uint8_t *amd64_map_mmio(uint64_t base, uint64_t size)
{
    if (!size || base > UINT64_MAX - size || base + size >= AMD64_CODE_BASE - amd64_hhdm)
        return NULL;
    /* Page tables live in the reservation: map under the allocator
     * lock; map_page invalidates locally and the unlock shoots down peers. */
    lock_allocator();
    map_range(amd64_hhdm + base, base, size, PTE_W | PTE_NX | PTE_PCD | PTE_PWT, true);
    unlock_allocator(true);
    return (uint8_t *)(uintptr_t)(amd64_hhdm + base);
}

/* Lower-half tables have a separate budget and lifetime from permanent
 * VMM tables. Detached tables stay retired until a synchronous target TLB
 * acknowledgement; reusing them earlier can race a cached paging walk. */
/* Sparse 4 KiB pages can each need a PT, PD and PDPT. The pool covers one
 * full window; each root holds at most its share (hostwin_amd64.c), so one
 * root's exhaustion never depends on another's tables. */
#ifdef HWB_TEST_SHADOW_TABLES
#define SHADOW_TABLES HWB_TEST_SHADOW_TABLES
_Static_assert(SHADOW_TABLES > 0, "test shadow pool must have tables");
#else
#define SHADOW_TABLES (3u * HOSTWIN_MAX_SHADOWS)
#endif
#define SHADOW_NONE UINT32_MAX
static uint64_t (*shadow_tables)[512];
static unsigned shadow_next[SHADOW_TABLES], shadow_free = SHADOW_NONE;
static unsigned shadow_lock, shadow_live;
static void shadow_pool_lock(void)
{
    amd64_window_lock(&shadow_lock, UINT32_MAX, "shadow pool lock");
}
bool amd64_shadow_init(void)
{
    if (shadow_tables)
        return true; /* stopped executors only */
    shadow_tables = hal_alloc_pages(SHADOW_TABLES, HAL_PAGE_ZERO);
    if (!shadow_tables)
        return false;
    for (unsigned i = 0; i < SHADOW_TABLES; ++i)
        shadow_next[i] = i + 1;
    shadow_next[SHADOW_TABLES - 1] = SHADOW_NONE;
    shadow_free = 0;
    LOGI("hostwin shadow-table pool=%u pages (%u MiB); VMM table pool=%u pages", SHADOW_TABLES,
         SHADOW_TABLES / 256, TABLE_COUNT);
    return true;
}
uint64_t amd64_memory_private_root(void)
{
    uint64_t *root = hal_alloc_pages(1, HAL_PAGE_ZERO);
    if (!root)
        hal_panic("cannot allocate private host root");
    memcpy(root + 256, tables[0] + 256, 256 * sizeof(*root));
    return (uintptr_t)root - amd64_hhdm;
}
unsigned amd64_shadow_capacity(void)
{
    return SHADOW_TABLES;
}
static uint64_t *shadow_new(struct amd64_shadow_root *root)
{
    if (root->tables >= root->share)
        return NULL;
    shadow_pool_lock();
    unsigned index = shadow_free;
    if (index != SHADOW_NONE) {
        shadow_free = shadow_next[index];
        __atomic_fetch_add(&shadow_live, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&shadow_lock, 0, __ATOMIC_RELEASE);
    if (index == SHADOW_NONE)
        return NULL;
    ++root->tables;
    memset(shadow_tables[index], 0, PAGE);
    return shadow_tables[index];
}
uint64_t *amd64_shadow_leaf(struct amd64_shadow_root *root, uint64_t va, bool create)
{
    if (va >= (UINT64_C(1) << 47) || (va & (PAGE - 1)))
        return NULL;
    uint64_t *table = table_pointer(root->root);
    for (unsigned level = 4; level > 1; --level) {
        unsigned index = (va >> (12 + 9 * (level - 1))) & 511;
        uint64_t entry = __atomic_load_n(&table[index], __ATOMIC_ACQUIRE);
        if (!(entry & PTE_P)) {
            if (!create)
                return NULL;
            uint64_t *next = shadow_new(root);
            if (!next)
                return NULL;
            entry = ((uintptr_t)next - amd64_hhdm) | PTE_P | PTE_W;
            __atomic_store_n(&table[index], entry, __ATOMIC_RELEASE);
        }
        table = table_pointer(entry);
    }
    return &table[(va >> 12) & 511];
}
static bool shadow_empty(uint64_t *table)
{
    for (unsigned i = 0; i < 512; ++i)
        if (__atomic_load_n(&table[i], __ATOMIC_RELAXED) & PTE_P)
            return false;
    return true;
}
static void shadow_remove(struct amd64_shadow_root *root, uint64_t *table, unsigned level,
                          uint64_t origin, uint64_t lo, uint64_t hi)
{
    uint64_t step = UINT64_C(1) << (12 + 9 * (level - 1));
    unsigned first = lo > origin ? (lo - origin) / step : 0;
    unsigned last = hi < origin + step * 512 ? (hi - origin - 1) / step : 511;
    for (unsigned i = first; i <= last; ++i) {
        uint64_t entry = __atomic_load_n(&table[i], __ATOMIC_ACQUIRE);
        if (!(entry & PTE_P))
            continue;
        if (level == 1) {
            __atomic_store_n(&table[i], 0, __ATOMIC_RELEASE);
            continue;
        }
        uint64_t *child = table_pointer(entry);
        shadow_remove(root, child, level - 1, origin + i * step, lo, hi);
        if (shadow_empty(child)) {
            __atomic_store_n(&table[i], 0, __ATOMIC_RELEASE);
            unsigned at = ((uintptr_t)child - (uintptr_t)shadow_tables) / PAGE;
            shadow_next[at] = root->retired;
            root->retired = at;
        }
    }
}
void amd64_shadow_unmap(struct amd64_shadow_root *root, uint64_t va, uint64_t bytes)
{
    shadow_remove(root, table_pointer(root->root), 4, 0, va, va + bytes);
}
void amd64_shadow_reap(struct amd64_shadow_root *root)
{
    /* Caller holds its root lock, after a synchronous window flush. */
    shadow_pool_lock();
    while (root->retired != SHADOW_NONE) {
        unsigned at = root->retired;
        root->retired = shadow_next[at];
        shadow_next[at] = shadow_free;
        shadow_free = at;
        --root->tables;
        __atomic_fetch_sub(&shadow_live, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&shadow_lock, 0, __ATOMIC_RELEASE);
}
unsigned amd64_shadow_used(void)
{
    return __atomic_load_n(&shadow_live, __ATOMIC_RELAXED);
}
