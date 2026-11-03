/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Intel VT-d rev.4.1 §§5.1.3,5.1.5,6.5.2,6.7,8.3,9.9,11.4.9–10. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host/amd64/vtd.h"
#define BASE UINT64_C(0x10000000)
#define TE (1u << 31)
#define QIE (1u << 26)
#define IRE (1u << 25)
static _Alignas(4096) uint8_t pool[64][4096];
static struct amd64_vtd state;
static struct boot_info memory;
static struct pci_device endpoint;
static const struct pci_device *devices[] = {&endpoint};
static uint8_t dmar[96];
static struct fixture {
    unsigned pages, writes, invalidations, waits, flushes, fail;
    uint32_t fault;
    bool bad_physical;
    uint64_t cap[2], ecap[2], root[2], irta[2], queue[2], head[2], tail[2];
    uint64_t visible_high[2][256];
    uint32_t status[2];
    bool revoked;
} f;
static void put(unsigned at, unsigned n, uint64_t v)
{
    for (unsigned i = 0; i < n; ++i)
        dmar[at + i] = v >> (8 * i);
}
static void seal(void)
{
    dmar[9] = 0;
    unsigned sum = 0;
    for (unsigned i = 0; i < sizeof(dmar); ++i)
        sum += dmar[i];
    dmar[9] = -sum;
}
static void *allocate(void *ctx, unsigned count)
{
    (void)ctx;
    assert(count == 1);
    return f.pages < 64 ? pool[f.pages++] : NULL;
}
static bool physical(void *ctx, const void *p, uint64_t *pa)
{
    (void)ctx;
    *pa = f.bad_physical ? 0x200000 : BASE + (uintptr_t)p - (uintptr_t)pool;
    return true;
}
static void *pointer(void *ctx, uint64_t pa)
{
    (void)ctx;
    assert(pa >= BASE && pa - BASE < sizeof(pool));
    return (uint8_t *)pool + pa - BASE;
}
static void flush(void *ctx, const void *p, size_t n)
{
    (void)ctx;
    assert(n && n <= 4096);
    ++f.flushes;
    for (unsigned u = 0; u < 2; ++u)
        if (f.irta[u]) {
            uint64_t *ir = pointer(NULL, f.irta[u] & ~UINT64_C(4095));
            uintptr_t offset = (uintptr_t)p - (uintptr_t)ir;
            if (offset >= 4096)
                continue;
            assert(!(offset & 15) && !(n & 15) && n <= 4096 - offset);
            for (unsigned i = offset / 16; i < (offset + n) / 16; ++i) {
                if (ir[2 * i] & 1)
                    assert(ir[2 * i + 1] == f.visible_high[u][i]);
                f.visible_high[u][i] = ir[2 * i + 1];
            }
        }
}
static uint32_t config(void *ctx, uint16_t bdf, unsigned off, unsigned size)
{
    (void)ctx;
    (void)bdf;
    (void)off;
    (void)size;
    return 0;
}
static uint64_t read_reg(void *ctx, unsigned u, unsigned off, unsigned width)
{
    (void)ctx;
    (void)width;
    assert(u < 2);
    switch (off) {
    case 0:
        return 0x10;
    case 8:
        return f.cap[u];
    case 0x10:
        return f.ecap[u];
    case 0x1c:
        return f.status[u];
    case 0x34:
        return f.fault;
    case 0x28:
        return UINT64_C(1) << 59;
    case 0x108:
        return UINT64_C(1) << 57;
    case 0x80:
        return f.head[u];
    case 0x88:
        return f.tail[u];
    default:
        return 0;
    }
}
static void consume(unsigned u, uint64_t tail)
{
    uint64_t *q = pointer(NULL, f.queue[u]);
    while (f.head[u] != tail) {
        uint64_t lo = q[f.head[u] / 8], hi = q[f.head[u] / 8 + 1];
        if ((lo & 15) == 4) {
            assert(lo == 4 && hi == 0);
            ++f.invalidations;
            if (f.irta[u]) {
                uint64_t *ir = pointer(NULL, f.irta[u] & ~UINT64_C(4095));
                if (u == 0 && !(ir[0x60 * 2] & 1))
                    f.revoked = true;
            }
        } else {
            assert((lo & 15) == 5 && (lo & (1u << 5)) && !(lo & (1u << 4)));
            assert(!(hi & 3));
            ++f.waits;
            if (f.fail != 4)
                *(uint32_t *)pointer(NULL, hi) = lo >> 32;
        }
        f.head[u] = (f.head[u] + 16) & 4095;
    }
}
static void write_reg(void *ctx, unsigned u, unsigned off, uint64_t value, unsigned width)
{
    (void)ctx;
    ++f.writes;
    if (off == 0x18) {
        assert(width == 4);
        if (f.status[u] & TE)
            assert(value & TE);
        if (value & QIE) {
            assert(f.queue[u]);
            if (f.fail != 1)
                f.status[u] |= QIE;
        }
        if (value & (1u << 24)) {
            assert(f.status[u] & QIE);
            assert(f.irta[u]);
            if (f.fail != 2)
                f.status[u] |= 1u << 24;
        }
        if (value & IRE) {
            assert(f.status[u] & (1u << 24));
            assert(f.invalidations && f.waits);
            assert(!(value & (1u << 23)));
            if (f.fail != 3)
                f.status[u] |= IRE;
        }
        if (!(value & (1u << 23)) && f.fail != 5)
            f.status[u] &= ~(1u << 23);
        if (value & TE)
            f.status[u] |= TE;
        if (value & (1u << 30))
            f.status[u] |= 1u << 30;
    } else if (off == 0x20)
        f.root[u] = value;
    else if (off == 0xb8) {
        assert(width == 8 && (value & 15) == 7);
        f.irta[u] = value;
    } else if (off == 0x90) {
        assert(width == 8 && !(value & 4095));
        f.queue[u] = value;
    } else if (off == 0x88) {
        assert(width == 8 && value < 4096 && !(value & 15));
        f.tail[u] = value;
        if (f.status[u] & QIE)
            consume(u, value);
        else
            assert(!value);
    } else if (off == 0x28 || off == 0x108)
        assert(!(f.status[u] & QIE));
    else
        assert(off == 0x38);
}
static const struct amd64_iommu_dma_ops ops = {
    .allocate = allocate,
    .physical = physical,
    .pointer = pointer,
    .flush = flush,
    .config = config,
    .read = read_reg,
    .write = write_reg,
};
static void reset(void)
{
    memset(&f, 0, sizeof(f));
    memset(dmar, 0, sizeof(dmar));
    memset(&memory, 0, sizeof(memory));
    memcpy(dmar, "DMAR", 4);
    put(4, 4, sizeof(dmar));
    dmar[36] = 47;
    dmar[37] = 1;
    put(50, 2, 32);
    put(56, 8, 0xfed90000);
    dmar[64] = 1;
    dmar[65] = 8;
    dmar[70] = 4;
    dmar[72] = 3;
    dmar[73] = 8;
    dmar[76] = 9;
    dmar[78] = 31;
    dmar[79] = 7;
    put(82, 2, 16);
    dmar[84] = 1;
    put(88, 8, 0xfeda0000);
    seal();
    memory.mem_range_count = 2;
    memory.mem[0] = (struct boot_mem_range){0x200000, 4096, BOOT_MEM_USABLE, NULL};
    memory.mem[1] = (struct boot_mem_range){BASE, sizeof(pool), BOOT_MEM_VMM, NULL};
    endpoint = (struct pci_device){.bdf = 0x20, .parent = 0xffff};
    for (unsigned i = 0; i < 2; ++i) {
        f.cap[i] = (UINT64_C(47) << 16) | 0x200;
        f.ecap[i] = 0x101b;
    }
    assert(amd64_vtd_build(dmar, sizeof(dmar), &memory, devices, 1, &ops, NULL, &state));
    assert(amd64_vtd_enable(&state));
}
static void activate(void)
{
    assert(amd64_vtd_irq_init(&state, dmar, sizeof(dmar), false));
    for (unsigned u = 0; u < 2; ++u)
        assert((f.status[u] & (TE | QIE | IRE)) == (TE | QIE | IRE));
}
static void mappings(void)
{
    reset();
    activate();
    uint64_t address, rte = 1u << 16;
    uint32_t data;
    assert(amd64_vtd_irq_msi(&state, 0, 0x20, 0x60, 8, 0xab, &address, &data));
    assert(address == UINT64_C(0xfee00c18) && !data);
    uint64_t *ir = pointer(NULL, f.irta[0] & ~UINT64_C(4095));
    uint64_t *other = pointer(NULL, f.irta[1] & ~UINT64_C(4095));
    for (unsigned i = 0; i < 8; ++i) {
        assert(ir[(0x60 + i) * 2] == ((UINT64_C(0xab) << 40) | ((uint64_t)(0x60 + i) << 16) | 1));
        assert(ir[(0x60 + i) * 2 + 1] == UINT64_C(0x40020));
        assert(!other[(0x60 + i) * 2]);
    }
    assert(amd64_vtd_irq_ioapic(&state, 9, 19, 0x70, 3, true, true, &rte));
    assert(rte == ((UINT64_C(0x70) << 49) | (UINT64_C(1) << 48) | 0x1a070));
    assert(ir[0x70 * 2] == ((UINT64_C(3) << 40) | UINT64_C(0x700011)));
    assert(ir[0x70 * 2 + 1] == UINT64_C(0x400ff));
    f.revoked = false;
    assert(amd64_vtd_irq_msi(&state, 0, 0x20, 0x60, 8, 4, &address, &data));
    assert(f.revoked);
    assert(amd64_vtd_irq_clear(&state, 0x60, 8));
    for (unsigned i = 0; i < 8; ++i)
        assert(!ir[(0x60 + i) * 2] && !ir[(0x60 + i) * 2 + 1]);
    for (unsigned i = 0; i < 150; ++i)
        assert(amd64_vtd_irq_clear(&state, 0x60, 8));
    assert(amd64_vtd_irq_clear(&state, 0x40, 160));
    state.sources[0].bridge = true; /* A PCIe bridge can originate MSI itself. */
    assert(amd64_vtd_irq_msi(&state, 0, 0x20, 0x58, 1, 3, &address, &data));
    assert(ir[0x58 * 2 + 1] == UINT64_C(0x40020));
}
static void rejection(void)
{
    uint64_t a, rte = 0;
    uint32_t d;
    reset();
    activate();
    unsigned writes = f.writes;
    assert(!amd64_vtd_irq_msi(&state, 1, 0x20, 0x60, 1, 0, &a, &d));
    assert(f.writes == writes);
    assert(amd64_vtd_irq_msi(&state, 0, 0x20, 0x60, 1, 0, &a, &d));
    uint64_t *existing = pointer(NULL, f.irta[0] & ~UINT64_C(4095));
    uint64_t saved = existing[0xc0];
    writes = f.writes;
    assert(!amd64_vtd_irq_ioapic(&state, 8, 0, 0x70, 0, false, false, &rte));
    assert(existing[0xc0] == saved && f.writes == writes);
    assert(amd64_vtd_irq_clear(&state, 0x60, 1));
    reset();
    activate();
    assert(!amd64_vtd_irq_msi(&state, 0, 0x28, 0x60, 1, 0, &a, &d));
    reset();
    activate();
    assert(!amd64_vtd_irq_msi(&state, 0, 0x20, 0x61, 8, 0, &a, &d));
    reset();
    activate();
    assert(!amd64_vtd_irq_msi(&state, 0, 0x20, 0x60, 3, 0, &a, &d));
    reset();
    activate();
    assert(!amd64_vtd_irq_msi(&state, 0, 0x20, 0x60, 64, 0, &a, &d));
    reset();
    activate();
    assert(!amd64_vtd_irq_ioapic(&state, 8, 0, 0x70, 0, false, false, &rte));
    reset();
    activate();
    assert(!amd64_vtd_irq_msi(&state, 0, 0x20, 0x60, 1, 256, &a, &d));
    reset();
    f.status[0] &= ~TE;
    writes = f.writes;
    assert(!amd64_vtd_irq_init(&state, dmar, sizeof(dmar), false) && writes == f.writes);
    reset();
    dmar[37] = 0;
    seal();
    writes = f.writes;
    assert(!amd64_vtd_irq_init(&state, dmar, sizeof(dmar), false) && writes == f.writes);
    reset();
    f.ecap[1] &= ~UINT64_C(8);
    state.units[1].ecap = f.ecap[1];
    writes = f.writes;
    assert(!amd64_vtd_irq_init(&state, dmar, sizeof(dmar), false) && writes == f.writes);
    reset();
    dmar[37] = 3;
    seal();
    assert(!amd64_vtd_irq_init(&state, dmar, sizeof(dmar), true));
    reset();
    state.units[1].ecap &= ~UINT64_C(16);
    writes = f.writes;
    assert(!amd64_vtd_irq_init(&state, dmar, sizeof(dmar), true) && f.writes == writes);
    reset();
    dmar[64] = 3;
    dmar[68] = 9;
    seal();
    writes = f.writes;
    assert(!amd64_vtd_irq_init(&state, dmar, sizeof(dmar), false) && f.writes == writes);
    reset();
    dmar[73] = 10;
    seal();
    assert(!amd64_vtd_irq_init(&state, dmar, sizeof(dmar), false));
    reset();
    f.bad_physical = true;
    writes = f.writes;
    assert(!amd64_vtd_irq_init(&state, dmar, sizeof(dmar), false) && f.writes == writes);
    reset();
    f.pages = 64;
    writes = f.writes;
    assert(!amd64_vtd_irq_init(&state, dmar, sizeof(dmar), false) && f.writes == writes);
    reset();
    state.units[0].ecap &= ~UINT64_C(1);
    activate();
    assert(f.flushes);
    assert(amd64_vtd_irq_msi(&state, 0, 0x20, 0x60, 1, 0, &a, &d));
    reset();
    assert(amd64_vtd_irq_init(&state, dmar, sizeof(dmar), true));
    assert(f.irta[0] & 0x800);
    assert(amd64_vtd_irq_msi(&state, 0, 0x20, 0x60, 1, 0x12345678, &a, &d));
    uint64_t *ir = pointer(NULL, f.irta[0] & ~UINT64_C(4095));
    assert(ir[0xc0] >> 32 == 0x12345678);
}
static void failures(void)
{
    for (unsigned stage = 1; stage <= 5; ++stage) {
        reset();
        f.fail = stage;
        f.status[0] |= 1u << 23;
        assert(!amd64_vtd_irq_init(&state, dmar, sizeof(dmar), false));
        assert(f.status[0] & TE);
        assert(state.error);
    }
    reset();
    activate();
    uint64_t a;
    uint32_t d;
    assert(amd64_vtd_irq_msi(&state, 0, 0x20, 0x60, 1, 3, &a, &d));
    f.fail = 4;
    unsigned pages = f.pages;
    assert(!amd64_vtd_irq_clear(&state, 0x60, 1));
    assert(!amd64_vtd_irq_msi(&state, 0, 0x20, 0x60, 1, 4, &a, &d));
    assert(f.pages == pages);
    reset();
    activate();
    f.fail = 4;
    f.fault = 1u << 4;
    assert(!amd64_vtd_irq_clear(&state, 0x60, 1));
}
int main(void)
{
    mappings();
    rejection();
    failures();
    puts("test-vtd-irq: remapping formats, source validation, QI ordering and failures passed");
}
