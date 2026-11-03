/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AMD 48882 Rev 3.09, Tables 7/8, 15-18, 34/35/41, 87-110 and §2.3.
 * Fixtures and the independent table walker encode the architectural formats. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host/amd64/amdvi.h"
#define POOL_BASE UINT64_C(0x10000000)
#define POOL_PAGES 8192u
static _Alignas(4096) uint8_t pool[POOL_PAGES][4096];
static struct boot_info bi;
static struct pci_device devices[4];
static const struct pci_device *list[4];
static uint8_t ivrs[1024];
static unsigned length;
static struct amd64_amdvi state;
struct fixture {
    unsigned pages, writes, flushes, bad, commands, pci_writes, va, pa;
    uint64_t control, status, efr, table, command, event, head, tail, ehead, etail, exclusion,
        pci_base;
    bool timeout, illegal, stuck, bad_physical, scattered, latch, unreadable, acs;
    unsigned dev_invalidations, irq_invalidations, drained, completed_irq_epoch, pending_epoch,
        waits, fail_wait;
    bool pending_interrupt;
};
static struct fixture f, second;
static void put(uint8_t *p, unsigned n, uint64_t v)
{
    for (unsigned i = 0; i < n; ++i)
        p[i] = v >> (8 * i);
}
static void seal(void)
{
    put(ivrs + 4, 4, length);
    ivrs[9] = 0;
    unsigned sum = 0;
    for (unsigned i = 0; i < length; ++i)
        sum += ivrs[i];
    ivrs[9] = -sum;
}
static void *allocate(void *p, unsigned n)
{
    struct fixture *r = p;
    if (!n || n > POOL_PAGES - r->pages)
        return NULL;
    void *v = pool[r->pages];
    r->pages += n;
    return v;
}
static bool physical(void *p, const void *v, uint64_t *pa)
{
    struct fixture *r = p;
    *pa = r->bad_physical ? UINT64_C(1) << 48 : POOL_BASE + (uintptr_t)v - (uintptr_t)pool;
    if (r->scattered && (uintptr_t)v != (uintptr_t)pool)
        *pa += 4096;
    return true;
}
static void *pointer(void *p, uint64_t pa)
{
    (void)p;
    assert(pa >= POOL_BASE && pa - POOL_BASE < sizeof(pool));
    return (uint8_t *)pool + pa - POOL_BASE;
}
static void flush(void *p, const void *v, size_t n)
{
    (void)v;
    assert(n && !(n & 4095));
    ((struct fixture *)p)->flushes += n / 4096;
}
static uint32_t config(void *p, uint16_t bdf, unsigned off, unsigned width)
{
    (void)p;
    (void)width;
    if (f.acs && (bdf == 0x18 || bdf == 0x20)) {
        if (off == 6)
            return 0x10;
        if (off == 0x34)
            return 0x60;
        if (off == 0x60)
            return 0x10;
        if (off == 0x62)
            return 2;
        if (off == 0x100)
            return 0x1000d;
        return 0;
    }
    if (bdf == 0x28 && devices[2].header == 1) {
        if (off == 6)
            return 0x10;
        if (off == 0x34)
            return 0x60;
        if (off == 0x60)
            return 0x10;
        if (off == 0x62)
            return 0x42; /* PCIe v2 root port. */
        return 0;
    }
    if (bdf == 2 || bdf == 3) {
        if (off == 0x40)
            return f.bad == 2 ? 0x0803000f : 0x080b000f; /* EFRSup, CapRev=1, CapType=3 */
        if (off == 0x44)
            return bdf == 2 ? (uint32_t)f.pci_base : 0xfeda0001;
        if (off == 0x48)
            return bdf == 2 ? f.pci_base >> 32 : 0;
        if (off == 0x50)
            return (f.pa << 8) | (f.va << 15);
    }
    if (f.bad == 1) {
        if (off == 6)
            return 0x10;
        if (off == 0x34)
            return 0x60;
        if (off == 0x60)
            return 0x10;
        if (off == 0x62)
            return 2;
    }
    if (off == 0x100 && f.bad == 1)
        return 0x1000f;
    if (off == 0x106 && f.bad == 1)
        return 0x8000;
    return 0;
}
static void config_write(void *p, uint16_t bdf, unsigned off, uint32_t value, unsigned width)
{
    struct fixture *r = p;
    assert(bdf == 2 && width == 4);
    ++r->pci_writes;
    if (r->pci_base & 1)
        return;
    if (off == 0x48)
        r->pci_base = (r->pci_base & UINT32_MAX) | ((uint64_t)value << 32);
    else {
        assert(off == 0x44);
        if (!r->latch)
            r->pci_base = (r->pci_base & ~UINT64_C(0xffffffff)) | value;
    }
}
static uint64_t read_reg(void *p, unsigned u, unsigned off, unsigned width)
{
    (void)width;
    struct fixture *r = u ? &second : p;
    assert(u < 2);
    switch (off) {
    case 0:
        return r->latch ? 0 : r->table;
    case 8:
        return r->command;
    case 0x10:
        return r->event;
    case 0x18:
        return r->unreadable ? UINT64_MAX : r->control;
    case 0x20:
        return r->exclusion;
    case 0x30:
        return r->efr;
    case 0x2000:
        return r->head;
    case 0x2008:
        return r->tail;
    case 0x2010:
        return r->ehead;
    case 0x2018:
        return r->etail;
    case 0x2020:
        return r->status;
    default:
        return 0;
    }
}
static void execute(struct fixture *r)
{
    while (r->head != r->tail) {
        uint64_t *cmd = pointer(r, (r->command & UINT64_C(0x000ffffffffff000)) + r->head);
        unsigned op = cmd[0] >> 60;
        ++r->commands;
        if (op == 1) {
            assert(!(cmd[0] & UINT64_C(0x0ff0000000000000)));
            assert((cmd[0] & 7) == 5); /* store + flush, no interrupt */
            ++r->waits;
            if (!r->timeout && r->waits != r->fail_wait) {
                r->completed_irq_epoch = r->irq_invalidations;
                *(uint64_t *)pointer(r, cmd[0] & UINT64_C(0x000ffffffffffff8)) = cmd[1];
            }
        } else if (op == 2 || op == 5) {
            assert(!(cmd[0] & UINT64_C(0x0fffffffffff0000)) && !cmd[1]);
            if (op == 2) {
                ++r->dev_invalidations;
                /* Model stale interrupt translation until IR invalidation completes. */
                if (r->completed_irq_epoch > r->pending_epoch) {
                    r->pending_interrupt = false;
                    ++r->drained;
                }
            } else
                ++r->irq_invalidations;
        } else {
            assert(op == 8 && cmd[0] == UINT64_C(8) << 60 && !cmd[1]);
        }
        r->head = (r->head + 16) & 4095;
    }
    if (r->illegal) {
        uint64_t *event = pointer(r, r->event & UINT64_C(0x000ffffffffff000));
        event[0] = UINT64_C(5) << 60;
        r->etail = 16;
        r->status |= 2;
    }
}
static void write_reg(void *p, unsigned u, unsigned off, uint64_t v, unsigned width)
{
    struct fixture *r = u ? &second : p;
    assert(u < 2 && width == 8);
    ++r->writes;
    switch (off) {
    case 0:
        assert(!(r->control & 0x1005));
        r->table = v;
        break;
    case 8:
        assert(!(r->control & 0x1005));
        r->command = v;
        break;
    case 0x10:
        assert(!(r->control & 0x1005));
        r->event = v;
        break;
    case 0x18:
        r->control = v;
        if (!r->stuck)
            r->status = (v & 1) ? 0x18 : 0;
        break;
    case 0x20:
        assert(!(r->control & 0x1005));
        r->exclusion = v;
        break;
    case 0x28:
        assert(!(r->control & 0x1005) && !v);
        break;
    case 0x2000:
        assert(!(r->control & 0x1005));
        r->head = v;
        break;
    case 0x2008:
        r->tail = v;
        if (r->control & 1)
            execute(r);
        break;
    case 0x2010:
        r->ehead = v;
        break;
    case 0x2018:
        assert(!(r->control & 0x1005));
        r->etail = v;
        break;
    case 0x2020:
        r->status &= ~v;
        break;
    default:
        assert(0);
    }
}
static const struct amd64_iommu_dma_ops ops = {allocate, physical, pointer,   flush,
                                               config,   read_reg, write_reg, config_write};
static void reset(void)
{
    memset(&f, 0, sizeof(f));
    memset(&bi, 0, sizeof(bi));
    memset(devices, 0, sizeof(devices));
    memset(ivrs, 0, sizeof(ivrs));
    f.efr = 1u << 6;
    f.va = f.pa = 48;
    f.pci_base = 0xfed80001;
    second = f;
    memcpy(ivrs, "IVRS", 4);
    ivrs[8] = 2;
    put(ivrs + 36, 4, 1 | (48 << 8));
    length = 76;
    ivrs[48] = 0x10;
    ivrs[49] = 0x21;
    put(ivrs + 50, 2, 28);
    put(ivrs + 52, 2, 2);
    put(ivrs + 54, 2, 0x40);
    put(ivrs + 56, 8, 0xfed80000);
    ivrs[72] = 1;
    bi.mem_range_count = 4;
    bi.mem[0] = (struct boot_mem_range){0x1003, 0x2ff9, BOOT_MEM_USABLE, NULL};
    bi.mem[1] = (struct boot_mem_range){0x200000, 0x400000, BOOT_MEM_USABLE, NULL};
    bi.mem[2] = (struct boot_mem_range){POOL_BASE, sizeof(pool), BOOT_MEM_VMM, NULL};
    bi.mem[3] = (struct boot_mem_range){0x700000, 0x2000, BOOT_MEM_RESERVED, NULL};
    devices[0] = (struct pci_device){.bdf = 0x20, .parent = 0xffff, .owner = PCI_OWNER_GUEST};
    devices[1] = (struct pci_device){.bdf = 0x18, .parent = 0xffff, .owner = PCI_OWNER_VMM};
    for (unsigned i = 0; i < 4; ++i)
        list[i] = &devices[i];
    seal();
}
static bool build(unsigned n)
{
    return amd64_amdvi_build(ivrs, length, &bi, list, n, &ops, &f, &state);
}
static bool walk_unit(unsigned u, uint16_t sid, uint64_t address, bool write, unsigned did)
{
    uint64_t *dte = pointer(&f, state.units[u].table + (uint64_t)sid * 32);
    assert((dte[0] & 3) == 3); /* V=0 passes through; TV=0 has different fault semantics. */
    assert((dte[1] & 0xffff) == did && !dte[2] && !dte[3]);
    if (!(dte[0] & (UINT64_C(1) << (write ? 62 : 61))))
        return false;
    unsigned level = (dte[0] >> 9) & 7;
    if (address >> (12 + level * 9))
        return false;
    uint64_t *table = pointer(&f, dte[0] & UINT64_C(0x000ffffffffff000));
    while (level) {
        unsigned shift = 12 + 9 * (level - 1);
        uint64_t pte = table[(address >> shift) & 511];
        if (!(pte & 1) || !(pte & (UINT64_C(1) << (write ? 62 : 61))))
            return false;
        assert(!(pte & UINT64_C(0x9ff00000000001fe)));
        unsigned next = (pte >> 9) & 7;
        if (!next) {
            assert(((pte & UINT64_C(0x000ffffffffff000)) |
                    (address & ((UINT64_C(1) << shift) - 1))) == address);
            return true;
        }
        assert(next == level - 1);
        level = next;
        table = pointer(&f, pte & UINT64_C(0x000ffffffffff000));
    }
    return false;
}
static bool walk(uint16_t sid, uint64_t address, bool write, unsigned did)
{
    return walk_unit(0, sid, address, write, did);
}
static void entry(unsigned at, unsigned type, unsigned a, unsigned b)
{
    ivrs[at] = type;
    put(ivrs + at + 1, 2, a);
    if (type >= 0x40)
        put(ivrs + at + 5, 2, b);
}
static void prepare(void)
{
    const struct amd64_iommu_unit unit = {
        .base = 0xfed80000, .size = 0x4000, .bdf = 2, .capability = 0x40, .kind = AMD64_AMDVI};
    reset();
    assert(amd64_amdvi_prepare(&unit, &ops, &f) && !f.pci_writes);
    f.pci_base = 0;
    assert(amd64_amdvi_prepare(&unit, &ops, &f));
    assert(f.pci_base == 0xfed80001 && f.pci_writes == 2 && !f.writes);
    reset();
    f.pci_base = 0xfeda0001;
    assert(!amd64_amdvi_prepare(&unit, &ops, &f) && !f.pci_writes);
    reset();
    f.pci_base = 0;
    f.latch = true;
    assert(!amd64_amdvi_prepare(&unit, &ops, &f));
    reset();
    f.bad = 2;
    assert(!amd64_amdvi_prepare(&unit, &ops, &f) && !f.pci_writes);
    reset();
    f.pci_base |= 0x3ffe;
    assert(amd64_amdvi_prepare(&unit, &ops, &f) && !f.pci_writes && build(2));
}
static void mapping(void)
{
    reset();
    assert(build(2) && !f.writes && f.flushes);
    assert(walk(0x20, 0x2000, true, 1) && walk(0x20, 0x5fffff, false, 1));
    assert(!walk(0x20, 0x1003, false, 1) && !walk(0x20, 0x3000, true, 1));
    assert(!walk(0x20, POOL_BASE, true, 1) && !walk(0x28, 0x2000, true, 0));
    assert(walk(0x18, POOL_BASE, true, 2) && !walk(0x18, 0x2000, false, 2));
    for (unsigned id = 0; id < 65536; ++id) {
        uint64_t *dte = pointer(&f, state.units[0].table + (uint64_t)id * 32);
        assert((dte[0] & 3) == 3);
    }
    assert(amd64_amdvi_enable(&state));
    assert((f.control & 0x1005) == 0x1005 && !(f.control & 0x18) && !f.exclusion);
    assert(f.commands == 2); /* INVALIDATE_IOMMU_ALL includes cached DTEs (§2.4.8). */
    reset();
    f.control = 0x20;
    ivrs[49] = 0x2f;
    seal();
    assert(build(2));
    assert(state.units[0].control == 0xf22 && amd64_amdvi_enable(&state));
    reset();
    f.va = 32;
    assert(build(2) && state.units[0].width == 32);
    reset();
    f.va = 39;
    assert(!build(2));
    reset();
    f.pa = 47;
    assert(!build(2));
    reset();
    bi.mem[1].base = UINT64_C(1) << 40;
    assert(build(2) && state.units[0].levels == 4);
    assert(walk(0x20, UINT64_C(1) << 40, true, 1));
    reset();
    f.bad_physical = true;
    assert(!build(2) && !f.writes);
    reset();
    f.pages = POOL_PAGES;
    assert(!build(2));
    reset();
    f.scattered = true;
    assert(!build(2));
    reset();
    bi.mem[0].size = UINT64_MAX;
    assert(!build(2));
    reset();
    bi.mem[1].base = POOL_BASE;
    assert(!build(2));
    reset();
    f.bad = 1;
    assert(!build(2));
    reset();
    f.efr = 0;
    assert(!build(2)); /* bounded takeover requires global invalidation */
    reset();
    f.unreadable = true;
    assert(!build(2));
    reset();
    f.control = UINT64_C(1) << 40;
    assert(!build(2)); /* active MARC can change routing */
}
static void scopes(void)
{
    reset();
    length = 84;
    put(ivrs + 50, 2, 36);
    entry(72, 2, 0x18, 0);
    entry(76, 0x42, 0x20, 0x18);
    seal();
    assert(!build(2)); /* aliases cannot cross ownership */
    f.acs = true;
    assert(!build(2) && strstr(state.error, "IVHD alias crosses"));
    devices[1].owner = PCI_OWNER_GUEST;
    assert(build(2));
    assert(walk(0x18, 0x2000, true, 1) && walk(0x20, 0x2000, true, 1));
    reset();
    length = 80;
    put(ivrs + 50, 2, 32);
    entry(72, 3, 0x18, 0);
    entry(76, 4, 0x20, 0);
    seal();
    assert(build(2));
    entry(76, 4, 0x17, 0);
    seal();
    assert(!build(2));
    entry(76, 2, 0x20, 0);
    seal();
    assert(!build(2)); /* range end must immediately follow */
    reset();
    length = 84;
    put(ivrs + 50, 2, 36);
    entry(76, 0x48, 0, 0xa0);
    ivrs[83] = 1;
    ivrs[79] = 0x20;
    seal();
    assert(build(2));
    assert(!walk(0xa0, 0x2000, true, 0));
    assert(((uint64_t *)pointer(&f, state.units[0].table))[0xa0 * 4 + 1] == UINT64_C(2) << 40);
    ivrs[83] = 3;
    seal();
    assert(!build(2));
    /* IVHD special sources may precede the encompassing ordinary scope. */
    reset();
    length = 84;
    put(ivrs + 50, 2, 36);
    entry(72, 0x48, 0, 0xa0);
    ivrs[75] = 0x20;
    ivrs[79] = 1;
    entry(80, 1, 0, 0);
    seal();
    assert(build(2));
    assert(!walk(0xa0, 0x2000, true, 0));
    assert(((uint64_t *)pointer(&f, state.units[0].table))[0xa0 * 4 + 1] == UINT64_C(2) << 40);
    length = 88;
    put(ivrs + 50, 2, 40);
    entry(80, 0x42, 0xa0, 0xb0);
    seal();
    assert(!build(2) && strstr(state.error, "overlapping"));
    reset();
    length = 120;
    memcpy(ivrs + 76, ivrs + 48, 24);
    ivrs[76] = 0x11;
    put(ivrs + 78, 2, 44);
    put(ivrs + 100, 8, f.efr);
    ivrs[116] = 1;
    seal();
    assert(build(2) && state.count == 1);
    put(ivrs + 96, 4, 1);
    seal();
    assert(!build(2)); /* HATDis */
    reset();
    length = 88;
    put(ivrs + 50, 2, 40);
    entry(72, 2, 0x18, 0);
    entry(76, 0x43, 0x20, 0x18);
    entry(84, 4, 0x27, 0);
    devices[1].owner = PCI_OWNER_GUEST;
    seal();
    assert(build(2));
    assert(walk(0x20, 0x2000, true, 1));
    reset();
    length = 84;
    put(ivrs + 50, 2, 36);
    entry(72, 0x46, 0x20, 0);
    put(ivrs + 76, 4, UINT32_C(0x80000000));
    entry(80, 2, 0x18, 0);
    seal();
    assert(build(2));
    ivrs[76] = 1;
    seal();
    assert(!build(2));
    reset();
    ivrs[72] = 2;
    put(ivrs + 73, 2, 0x20);
    seal();
    assert(!build(2)); /* uncovered media */
    reset();
    length = 84;
    put(ivrs + 50, 2, 36);
    entry(76, 0x42, 0x20, 0x28);
    seal();
    assert(!build(2)); /* ALL + alias overlap is forbidden by IVHD */
    reset();
    length = 112;
    put(ivrs + 50, 2, 32);
    entry(72, 3, 0x20, 0);
    entry(76, 4, 0x27, 0);
    memcpy(ivrs + 80, ivrs + 48, 32);
    put(ivrs + 84, 2, 3);
    put(ivrs + 88, 8, 0xfeda0000);
    entry(104, 3, 0x18, 0);
    entry(108, 4, 0x1f, 0);
    seal();
    assert(build(2) && state.count == 2);
    assert(walk_unit(0, 0x20, 0x2000, true, 1) && !walk_unit(0, 0x18, POOL_BASE, true, 0));
    assert(walk_unit(1, 0x18, POOL_BASE, true, 2) && !walk_unit(1, 0x20, 0x2000, true, 0));
    entry(104, 3, 0x20, 0);
    entry(108, 4, 0x27, 0);
    seal();
    assert(!build(2));
    reset();
    length = 92;
    put(ivrs + 50, 2, 44);
    entry(76, 0x48, 0, 0xa0);
    ivrs[80] = 4;
    ivrs[83] = 1;
    entry(84, 0x48, 0, 0xb0);
    ivrs[88] = 4;
    ivrs[91] = 1;
    seal();
    assert(!build(2)); /* Two requester IDs cannot name the same IOAPIC. */
    put(ivrs + 89, 2, 0xa0);
    ivrs[88] = 5;
    seal();
    assert(!build(2));
}
static void ivmd(unsigned flags, uint64_t start, uint64_t size)
{
    ivrs[76] = 0x21;
    ivrs[77] = flags;
    put(ivrs + 78, 2, 32);
    put(ivrs + 80, 2, 0x20);
    put(ivrs + 92, 8, start);
    put(ivrs + 100, 8, size);
    length = 108;
    seal();
}
static void reserved(void)
{
    reset();
    ivmd(3, 0x700000, 4096);
    assert(build(2));
    assert(walk(0x20, 0x700000, false, 1) && !walk(0x20, 0x700000, true, 1));
    assert(!walk(0x18, 0x700000, false, 2));
    reset();
    ivmd(7, POOL_BASE, 4096);
    assert(!build(2));
    reset();
    ivmd(8, 0x700000, 4096);
    assert(!build(2)); /* no exclusion bypass */
    reset();
    ivmd(0, 0x201000, 4096);
    assert(build(2));
    assert(!walk(0x20, 0x201000, false, 1) && walk(0x20, 0x202000, true, 1));
    reset();
    ivmd(3, 0x700000, 4096);
    devices[0].bars[0] = (struct pci_bar){.base = 0x700000, .size = 4096};
    assert(!build(2));
    reset();
    ivmd(3, 0x700001, 4096);
    assert(!build(2));
    reset();
    ivmd(5, 0x700000, 4096);
    assert(build(2));
    assert(walk(0x20, 0x700000, true, 1) && !walk(0x20, 0x700000, false, 1));
    reset();
    ivmd(0, 0x800000, 4096);
    assert(build(2));
    assert(walk(0x20, 0x2000, true, 1) && !walk(0x20, 0x800000, true, 1));
    reset();
    ivmd(3, 0x700000, 4096);
    memcpy(ivrs + 108, ivrs + 76, 32);
    ivrs[109] = 7;
    length = 140;
    seal();
    assert(!build(2));
    reset();
    ivmd(7, 0x700000, 4096);
    ivrs[76] = 0x20;
    put(ivrs + 80, 2, 0);
    seal();
    assert(!build(2)); /* firmware buffers cannot join guest and trusted media */
    reset();
    ivmd(3, 0x700000, 4096);
    memcpy(ivrs + 108, ivrs + 76, 32);
    put(ivrs + 112, 2, 0x18);
    length = 140;
    seal();
    assert(!build(2));
    reset();
    ivmd(7, 0xfee00000, 4096);
    assert(!build(2));
    reset();
    ivmd(7, UINT64_C(0xfd00000000), 4096);
    assert(!build(2));
    reset();
    bi.mem[4] = (struct boot_mem_range){UINT64_C(1) << 39, 4096, BOOT_MEM_RESERVED, NULL};
    bi.mem_range_count = 5;
    ivmd(3, UINT64_C(1) << 39, 4096);
    assert(build(2) && state.units[0].levels == 4);
    assert(walk(0x20, UINT64_C(1) << 39, false, 1) && !walk(0x20, UINT64_C(1) << 39, true, 1));
}
static void sequencing(void)
{
    reset();
    assert(build(2));
    f.control = 1;
    assert(!amd64_amdvi_enable(&state) && !f.writes);
    reset();
    assert(build(2));
    f.latch = true;
    assert(!amd64_amdvi_enable(&state) && !f.control);
    reset();
    assert(build(2));
    f.stuck = true;
    assert(!amd64_amdvi_enable(&state));
    reset();
    assert(build(2));
    f.timeout = true;
    assert(!amd64_amdvi_enable(&state));
    reset();
    assert(build(2));
    f.illegal = true;
    assert(!amd64_amdvi_enable(&state));
}

/* Tables 7/20/22, Figure 19: decode independently from the programming API. */
static bool interrupt_unit(unsigned unit, uint16_t sid, unsigned index, unsigned *vector,
                           uint32_t *destination)
{
    uint64_t *dte = pointer(&f, state.units[unit].table + (uint64_t)sid * 32);
    assert(dte[2] & 1); /* IV=0 would pass through. */
    unsigned ctl = dte[2] >> 60 & 3, length = dte[2] >> 1 & 15;
    if (!ctl)
        return false;
    assert(ctl == 2 && length <= 11 && !(dte[2] & UINT64_C(0xcfc0000000000000)));
    if (index >= (1u << length))
        return false;
    bool wide = f.control & (UINT64_C(1) << 17);
    uint8_t *table = pointer(&f, dte[2] & UINT64_C(0x000fffffffffffc0));
    if (wide) {
        uint64_t *e = (uint64_t *)table + index * 2;
        if (!(e[0] & 1))
            return false;
        assert(!(e[0] & UINT64_C(0xffffffff000000de)));
        assert(!(e[1] & UINT64_C(0x00ffffffffffff00)));
        *vector = e[1] & 255;
        *destination = (uint32_t)(e[0] >> 8) | (uint32_t)(e[1] >> 56) << 24;
    } else {
        uint32_t e = ((uint32_t *)table)[index];
        if (!(e & 1))
            return false;
        assert(!(e & UINT32_C(0xff0000de)));
        *vector = e >> 16 & 255;
        *destination = e >> 8 & 255;
    }
    return true;
}
static bool interrupt(uint16_t sid, unsigned index, unsigned *vector, uint32_t *destination)
{
    return interrupt_unit(0, sid, index, vector, destination);
}
static void interrupts(void)
{
    uint64_t address, rte;
    uint32_t data, destination;
    unsigned vector;
    reset();
    assert(build(2) && amd64_amdvi_enable(&state));
    assert(amd64_amdvi_irq_init(&state, false));
    uint64_t *device_table = pointer(&f, state.units[0].table);
    for (unsigned id = 0; id < 65536; ++id)
        assert(device_table[id * 4 + 2] & 1);
    assert(!interrupt(0x20, 0x40, &vector, &destination));
    assert(amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 4, 7, &address, &data));
    assert(address == 0xfee00000 && data == 0x40);
    for (unsigned i = 0; i < 4; ++i) {
        assert(interrupt(0x20, data + i, &vector, &destination));
        assert(vector == 0x40 + i && destination == 7);
        assert(!interrupt(0x18, data + i, &vector, &destination));
        assert(!interrupt(0x28, data + i, &vector, &destination));
    }
    uint64_t saved_dma[2];
    memcpy(saved_dma, pointer(&f, state.units[0].table + 0x20 * 32), sizeof(saved_dma));
    f.pending_interrupt = true;
    f.pending_epoch = f.completed_irq_epoch;
    assert(amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 4, 9, &address, &data));
    assert(!f.pending_interrupt && f.dev_invalidations && f.irq_invalidations && f.drained);
    assert(interrupt(0x20, 0x40, &vector, &destination) && destination == 9);
    assert(amd64_amdvi_irq_clear(&state, 0x40, 4));
    assert(!interrupt(0x20, 0x40, &vector, &destination));
    assert(!memcmp(saved_dma, pointer(&f, state.units[0].table + 0x20 * 32), sizeof(saved_dma)));
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x28, 0x40, 1, 0, &address, &data));
    assert(!amd64_amdvi_irq_msi(&state, 1, 0x20, 0x40, 1, 0, &address, &data));
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x20, 0x41, 4, 0, &address, &data));
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 3, 0, &address, &data));
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 0, 0, &address, &data));
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 64, 0, &address, &data));
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x20, 0xe0, 1, 0, &address, &data));
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x20, 0x20, 1, 0, &address, &data));
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 1, 256, &address, &data));
    assert(!amd64_amdvi_irq_ioapic(&state, 4, 0, 0x50, 0, false, false, &rte));
    for (unsigned i = 0; i < 90; ++i) {
        assert(amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 1, 1, &address, &data));
        assert(amd64_amdvi_irq_clear(&state, 0x40, 1));
    }
    assert(amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 1, 1, &address, &data));
    f.timeout = true;
    address = 123;
    data = 123;
    assert(!amd64_amdvi_irq_clear(&state, 0x40, 1) && state.irq_failed);
    f.timeout = false;
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 1, 1, &address, &data));
    assert(address == 123 && data == 123);

    reset();
    assert(build(2) && amd64_amdvi_enable(&state));
    unsigned writes = f.writes;
    assert(!amd64_amdvi_irq_init(&state, true));
    assert(f.writes == writes && !state.irq_ready);
    assert(!((uint64_t *)pointer(&f, state.units[0].table))[0x20 * 4 + 2]);
    reset();
    f.efr |= (1u << 7) | 4;
    assert(build(2) && amd64_amdvi_enable(&state) && amd64_amdvi_irq_init(&state, true));
    assert((f.control & ((UINT64_C(1) << 50) | (1u << 17))) == ((UINT64_C(1) << 50) | (1u << 17)));
    assert(amd64_amdvi_irq_msi(&state, 0, 0x20, 0x60, 1, 0xfedcba98, &address, &data));
    assert(interrupt(0x20, 0x60, &vector, &destination) && destination == 0xfedcba98 &&
           vector == 0x60);
    assert(amd64_amdvi_irq_clear(&state, 0x60, 1));
    assert(!interrupt(0x20, 0x60, &vector, &destination));

    reset();
    length = 84;
    put(ivrs + 50, 2, 36);
    entry(76, 0x48, 0, 0xa0);
    ivrs[80] = 4;
    ivrs[83] = 1;
    seal();
    assert(build(2) && amd64_amdvi_enable(&state) && amd64_amdvi_irq_init(&state, false));
    rte = 1u << 16;
    assert(amd64_amdvi_irq_ioapic(&state, 4, 23, 0x50, 8, true, true, &rte));
    assert(rte == ((UINT64_C(1) << 16) | (1u << 15) | (1u << 13) | 0x50));
    assert(interrupt(0xa0, 0x50, &vector, &destination) && vector == 0x50 && destination == 8);
    uint64_t *dte = pointer(&f, state.units[0].table + 0xa0 * 32);
    assert(((uint32_t *)pointer(&f, dte[2] & UINT64_C(0x000fffffffffffc0)))[0x50] & 32);
    assert(!interrupt(0x20, 0x50, &vector, &destination));
    assert(amd64_amdvi_irq_clear(&state, 0x50, 1));
    assert(!interrupt(0xa0, 0x50, &vector, &destination));
    reset();
    length = 84;
    put(ivrs + 50, 2, 36);
    entry(76, 0x48, 0, 0xa0);
    ivrs[80] = 4;
    ivrs[83] = 2;
    seal();
    assert(build(2) && amd64_amdvi_enable(&state) && amd64_amdvi_irq_init(&state, false));
    assert(!amd64_amdvi_irq_ioapic(&state, 4, 0, 0x50, 8, false, false, &rte));
    assert(!interrupt(0xa0, 0x50, &vector, &destination));

    reset();
    length = 84;
    put(ivrs + 50, 2, 36);
    entry(72, 2, 0x18, 0);
    entry(76, 0x42, 0x20, 0x18);
    devices[1].owner = PCI_OWNER_GUEST;
    seal();
    assert(build(2) && amd64_amdvi_enable(&state) && amd64_amdvi_irq_init(&state, false));
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 1, 0, &address, &data));
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x18, 0x40, 1, 0, &address, &data));
    reset();
    length = 112;
    put(ivrs + 50, 2, 32);
    entry(72, 3, 0x20, 0);
    entry(76, 4, 0x27, 0);
    memcpy(ivrs + 80, ivrs + 48, 32);
    put(ivrs + 84, 2, 3);
    put(ivrs + 88, 8, 0xfeda0000);
    entry(104, 3, 0x18, 0);
    entry(108, 4, 0x1f, 0);
    seal();
    assert(build(2) && amd64_amdvi_enable(&state) && amd64_amdvi_irq_init(&state, false));
    assert(amd64_amdvi_irq_msi(&state, 0, 0x18, 0x70, 1, 3, &address, &data));
    assert(interrupt_unit(1, 0x18, 0x70, &vector, &destination) && destination == 3);
    assert(!interrupt_unit(0, 0x18, 0x70, &vector, &destination));
    assert(!interrupt_unit(1, 0x20, 0x70, &vector, &destination));
    assert(amd64_amdvi_irq_clear(&state, 0x70, 1));
    assert(!interrupt_unit(1, 0x18, 0x70, &vector, &destination));

    reset();
    assert(build(2) && amd64_amdvi_enable(&state));
    f.pages = POOL_PAGES;
    assert(!amd64_amdvi_irq_init(&state, false) && state.irq_failed && !state.irq_ready);
    reset();
    assert(build(2) && amd64_amdvi_enable(&state));
    f.timeout = true;
    assert(!amd64_amdvi_irq_init(&state, false) && state.irq_failed && !state.irq_ready);
    reset();
    assert(build(2) && amd64_amdvi_enable(&state) && amd64_amdvi_irq_init(&state, false));
    f.timeout = true;
    address = 123;
    data = 456;
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 1, 1, &address, &data) && state.irq_failed);
    assert(address == 123 && data == 456);
    reset();
    assert(build(2) && amd64_amdvi_enable(&state) && amd64_amdvi_irq_init(&state, false));
    f.illegal = true;
    assert(!amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 1, 1, &address, &data) && state.irq_failed);
    reset();
    assert(build(2) && amd64_amdvi_enable(&state) && amd64_amdvi_irq_init(&state, false));
    assert(amd64_amdvi_irq_msi(&state, 0, 0x20, 0x40, 1, 1, &address, &data));
    f.fail_wait = f.waits + 2;
    assert(!amd64_amdvi_irq_clear(&state, 0x40, 1) && state.irq_failed);
    assert(f.waits == f.fail_wait);
}

/* Tables 7/9 select interrupt translation by DeviceID independently of DMA
 * permissions and PCI header type; Table 20 supplies the remapped vector. */
static void bridge_interrupts(void)
{
    for (unsigned wide = 0; wide < 2; ++wide) {
        reset();
        if (wide)
            f.efr |= (1u << 7) | 4;
        devices[2] = (struct pci_device){
            .bdf = 0x28,
            .parent = 0xffff,
            .header = 1,
            .secondary = 1,
            .subordinate = 1,
            .owner = PCI_OWNER_GUEST,
        };
        assert(build(3));
        assert(!walk(0x28, 0x2000, false, 0) && !walk(0x28, POOL_BASE, true, 0));
        uint64_t *dte = pointer(&f, state.units[0].table + 0x28 * 32), dma[2];
        memcpy(dma, dte, sizeof(dma));
        assert(amd64_amdvi_enable(&state) && amd64_amdvi_irq_init(&state, wide));
        uint64_t address;
        uint32_t data, destination;
        unsigned vector;
        assert(amd64_amdvi_irq_msi(&state, 0, 0x28, 0x80, 1, 7, &address, &data));
        assert(address == 0xfee00000 && data == 0x80);
        assert(interrupt(0x28, data, &vector, &destination) && vector == 0x80 && destination == 7);
        assert(!interrupt(0x20, data, &vector, &destination));
        assert(!memcmp(dma, dte, sizeof(dma)) && !(dte[0] & (UINT64_C(3) << 61)));
        assert(amd64_amdvi_irq_clear(&state, 0x80, 1));
        assert(!interrupt(0x28, 0x80, &vector, &destination));
        assert(!memcmp(dma, dte, sizeof(dma)));
    }
}
int main(void)
{
    prepare();
    mapping();
    scopes();
    reserved();
    sequencing();
    interrupts();
    bridge_interrupts();
    puts(
        "test-amdvi: denied defaults, DMA permissions, aliases/scopes/IVMD, MSI/IOAPIC remapping and draining passed");
}
