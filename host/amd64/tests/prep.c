/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Tests/Boot/Acpi/IvrsTests.cpp
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "boot/ranges.h"
#include "host/amd64/calibration.h"
#include "host/amd64/iommu.h"
#include "platform/platform.h"
#include "ia64vmm/pci.h"

static void put(uint8_t *p, unsigned n, uint64_t v)
{
    for (unsigned i = 0; i < n; ++i)
        p[i] = v >> (8 * i);
}
static void seal(uint8_t *p, unsigned n)
{
    put(p + 4, 4, n);
    p[9] = 0;
    unsigned sum = 0;
    for (unsigned i = 0; i < n; ++i)
        sum += p[i];
    p[9] = -sum;
}
/* IVRS type-10/11/40,
 * duplicate-alternative, truncation, framing, alignment and checksum cases. */
static void tables(void)
{
    uint8_t b[4096] = "IVRS";
    unsigned n = 48;
    struct amd64_iommu_table t;
    for (unsigned i = 0; i < 3; ++i) {
        b[n] = i == 0 ? 0x10 : i == 1 ? 0x11 : 0x40;
        unsigned len = i == 0 ? 24 : 40;
        put(b + n + 2, 2, len);
        put(b + n + 4, 2, 0x30);
        put(b + n + 8, 8, 0xfed80000);
        n += len;
    }
    seal(b, n);
    assert(amd64_iommu_parse(b, n, &t) && t.count == 1 && t.units[0].bdf == 0x30);
    assert(!amd64_iommu_parse(b, n - 1, &t));
    b[n - 1] ^= 1;
    assert(!amd64_iommu_parse(b, n, &t));
    b[n - 1] ^= 1;
    put(b + 50, 2, 0);
    seal(b, n);
    assert(!amd64_iommu_parse(b, n, &t));
    put(b + 50, 2, 24);
    put(b + 56, 8, 0xfed80001);
    seal(b, n);
    assert(!amd64_iommu_parse(b, n, &t));
    /* AMD 48882 §3.2/§5.2: PCSup requires a 512K aperture. Type10
     * derives PCSup from counters; Type11/40 carry its EFR image. */
    memset(b, 0, sizeof(b));
    memcpy(b, "IVRS", 4);
    b[48] = 0x10;
    put(b + 50, 2, 24);
    put(b + 54, 2, 0x40);
    put(b + 56, 8, 0xfed80000);
    put(b + 68, 4, 1u << 13);
    seal(b, 72);
    assert(amd64_iommu_parse(b, 72, &t) && t.units[0].size == 0x80000 &&
           t.units[0].capability == 0x40);
    put(b + 68, 4, 1u << 23);
    seal(b, 72); /* MSI index does not imply counters. */
    assert(amd64_iommu_parse(b, 72, &t) && t.units[0].size == 0x4000);
    b[48] = 0x11;
    put(b + 50, 2, 40);
    put(b + 72, 8, 1u << 9);
    seal(b, 88);
    assert(amd64_iommu_parse(b, 88, &t) && t.units[0].size == 0x80000);
    put(b + 56, 8, 0xfed84000);
    seal(b, 88);
    assert(!amd64_iommu_parse(b, 88, &t));
    memset(b, 0, sizeof(b));
    memcpy(b, "DMAR", 4);
    n = 48;
    for (unsigned i = 0; i < AMD64_IOMMU_MAX_UNITS + 1; ++i) {
        put(b + n + 2, 2, 16);
        put(b + n + 8, 8, 0xfed80000 + i * 0x4000);
        n += 16;
    }
    seal(b, n - 16);
    assert(amd64_iommu_parse(b, n - 16, &t) && t.count == AMD64_IOMMU_MAX_UNITS);
    seal(b, n);
    assert(!amd64_iommu_parse(b, n, &t));
    seal(b, 64);
    assert(amd64_iommu_parse(b, 64, &t) && t.count == 1 && t.units[0].kind == AMD64_VTD);
    b[53] = 1;
    seal(b, 64);
    assert(amd64_iommu_parse(b, 64, &t) && t.units[0].size == 8192);
    b[53] = 0x10;
    seal(b, 64);
    assert(!amd64_iommu_parse(b, 64, &t));
    b[53] = 0;
    put(b + 50, 2, 15);
    seal(b, 64);
    assert(!amd64_iommu_parse(b, 64, &t));
    memset(b, 0, sizeof(b));
    memcpy(b, "IVRS", 4);
    b[48] = 0x12;
    put(b + 50, 2, 24);
    seal(b, 72);
    assert(!amd64_iommu_parse(b, 72, &t));
}
struct registers {
    uint64_t control, status, command, context, iotlb;
    uint32_t pmen;
    unsigned writes, pmen_reads;
    bool stuck;
};
static uint64_t read_register(void *p, unsigned off, unsigned size)
{
    (void)size;
    struct registers *r = p;
    if (off == 0x1c)
        return r->status;
    if (off == 0x18)
        return r->control;
    if (off == 0x10)
        return 0x1000;
    if (off == 0x28)
        return r->context;
    if (off == 0x108)
        return r->iotlb;
    if (off == 0x64) {
        ++r->pmen_reads;
        if (r->pmen == 1 && !r->stuck && r->pmen_reads >= 3)
            r->pmen = 0;
        return r->pmen;
    }
    assert(off == 0x2020);
    return r->status;
}
static void write_register(void *p, unsigned off, uint64_t v, unsigned size)
{
    struct registers *r = p;
    ++r->writes;
    if (off == 0x64) { /* PMEN: EPM written, PRS follows unless stuck */
        assert(size == 4);
        r->pmen = (uint32_t)v;
        if (!r->stuck)
            r->pmen = (r->pmen & ~1u) | (r->pmen >> 31);
        return;
    }
    assert(off == 0x18);
    if (size == 4) {
        r->command = v;
        if (!r->stuck)
            r->status = v;
    } else {
        if (!r->stuck) {
            r->control = v;
            r->status = 0;
        }
    }
}
static void handoff(void)
{
    const struct amd64_iommu_ops ops = {read_register, write_register};
    struct registers r = {.status = 0xc0000000};
    assert(amd64_iommu_disable(AMD64_VTD, &ops, &r));
    assert(r.command == ((0xc0000000 & 0x96ffffffu) & ~0x80000000u) && r.writes == 1);
    /* Protected memory regions left enabled by pre-boot DMA protection. */
    r = (struct registers){.pmen = 0x80000001u};
    assert(amd64_iommu_disable(AMD64_VTD, &ops, &r) && r.pmen == 0 && r.writes == 1);
    r = (struct registers){.pmen = 0x80000001u, .stuck = true};
    assert(!amd64_iommu_disable(AMD64_VTD, &ops, &r));
    r = (struct registers){.status = 0x02000000};
    assert(!amd64_iommu_disable(AMD64_VTD, &ops, &r) && !r.writes); /* IRE */
    r = (struct registers){.status = 0x04000000};
    assert(!amd64_iommu_disable(AMD64_VTD, &ops, &r) && !r.writes); /* firmware QI */
    r = (struct registers){.pmen = 1};
    assert(amd64_iommu_disable(AMD64_VTD, &ops, &r) && r.pmen_reads >= 3 && !r.pmen);
    r = (struct registers){.pmen = 1, .stuck = true};
    assert(!amd64_iommu_disable(AMD64_VTD, &ops, &r) && !r.writes);
    r = (struct registers){.status = 0x88000000};
    assert(!amd64_iommu_disable(AMD64_VTD, &ops, &r) && !r.writes); /* pending WBF */
    r = (struct registers){.status = 0x80000000, .context = UINT64_C(1) << 63};
    assert(!amd64_iommu_disable(AMD64_VTD, &ops, &r) && !r.writes);
    r = (struct registers){.status = 0x80000000, .iotlb = UINT64_C(1) << 63};
    assert(!amd64_iommu_disable(AMD64_VTD, &ops, &r) && !r.writes);
    r = (struct registers){.status = 0x80000000, .stuck = true};
    assert(!amd64_iommu_disable(AMD64_VTD, &ops, &r));
    r = (struct registers){.control = (UINT64_C(1) << 22) | 0x1005, .status = 0x198};
    assert(amd64_iommu_disable(AMD64_AMDVI, &ops, &r));
    assert(r.control == (UINT64_C(1) << 22) && r.writes == 2);
    r = (struct registers){.control = 1, .status = 0x198, .stuck = true};
    assert(!amd64_iommu_disable(AMD64_AMDVI, &ops, &r));
}
struct clock {
    uint64_t tsc;
    uint32_t value, step;
};
static uint64_t clock_tsc(void *p)
{
    return ((struct clock *)p)->tsc;
}
static uint32_t clock_counter(void *p)
{
    struct clock *c = p;
    c->value += c->step;
    c->tsc += (uint64_t)c->step * 1000;
    return c->value;
}
static void calibration(void)
{
    assert(amd64_calibration_hz(125, 24000000, 1) == 3000000000ULL);
    assert(amd64_calibration_hz(150000000, 3579545, 178977) > 2999900000ULL);
    assert(!amd64_calibration_hz(UINT64_MAX, UINT64_MAX, 1));
    assert(!amd64_calibration_hz(1, 1, 0));
    struct clock c = {.value = 0xfffff000, .step = 1000};
    struct amd64_clock_reference r = {clock_counter, clock_tsc, &c};
    assert(amd64_measure_reference(&r, 0xffffff, 3579545) == 3579545000ULL);
    c.step = 0;
    assert(!amd64_measure_reference(&r, 0xffff, 1193182));
    assert(!amd64_measure_reference(&r, 0xffff, UINT64_MAX));
}
static void ranges(void)
{
    static struct boot_info bi;
    static struct boot_mem_range out[BOOT_MAX_MEM_RANGES];
    unsigned n;
    uint64_t reclaimed, kept;
    for (unsigned i = 0; i < BOOT_MAX_MEM_RANGES; ++i)
        bi.mem[i] =
            (struct boot_mem_range){0x100000 + i * 0x10000, 0x1000, BOOT_MEM_RESERVED, NULL};
    bi.mem[0] = (struct boot_mem_range){0x100000, 0x3000, BOOT_MEM_VMM, NULL};
    bi.mem_range_count = BOOT_MAX_MEM_RANGES - 2;
    assert(boot_reclaim_ranges(&bi, out, &n, 0x101000, 0x1000, 0x80000000, &reclaimed, &kept));
    assert(n == BOOT_MAX_MEM_RANGES && reclaimed == 0x2000 && kept == 0x1000);
    bi.mem_range_count = BOOT_MAX_MEM_RANGES - 1;
    assert(!boot_reclaim_ranges(&bi, out, &n, 0x101000, 0x1000, 0x80000000, &reclaimed, &kept));
    assert(bi.mem[0].size == 0x3000 && bi.mem[0].type == BOOT_MEM_VMM); /* input unchanged */
    n = 0;
    for (unsigned i = 0; i < BOOT_MAX_MEM_RANGES; ++i)
        assert(boot_range_append(out, &n, bi.mem[i]));
    assert(!boot_range_append(out, &n, bi.mem[1]) && n == BOOT_MAX_MEM_RANGES);
    assert(
        !boot_range_append(out, &n, (struct boot_mem_range){UINT64_MAX, 1, BOOT_MEM_USABLE, NULL}));
    struct ia64vmm_handoff h = {0};
    for (unsigned i = 0; i < IA64VMM_HANDOFF_MAX_RAM; ++i)
        assert(platform_handoff_ram_append(&h, 0x100000 + i * 0x10000, 0x1000));
    assert(!platform_handoff_ram_append(&h, 0x80000000, 0x1000));
    assert(h.ram_range_count == IA64VMM_HANDOFF_MAX_RAM);
    assert(platform_handoff_ram_append(&h, h.ram[63].base + h.ram[63].size, 0x1000));
    assert(h.ram[63].size == 0x2000); /* coalescing still works at capacity */
}
struct function {
    uint16_t bdf;
    uint32_t id, class_code, buses;
    uint8_t header;
};
static const struct function functions[] = {
    {0x0020, 0x11111234, 0x060400, 0x00412000, 1}, {0x2000, 0x22221234, 0x060400, 0x00414020, 1},
    {0x4000, 0x33331234, 0x060400, 0x00414140, 1}, {0x4100, 0x29228086, 0x010601, 0, 0},
    {0x4108, 0x29228086, 0x010601, 0, 0},          {0x0018, 0x01941033, 0x0c0330, 0, 0},
};
static unsigned writes;
static uint32_t config_read(void *p, uint16_t bdf, unsigned off, unsigned size)
{
    (void)p;
    (void)size;
    for (unsigned i = 0; i < sizeof(functions) / sizeof(*functions); ++i) {
        const struct function *f = &functions[i];
        if (f->bdf != bdf)
            continue;
        if (off == 0)
            return f->id;
        if (off == 8)
            return f->class_code << 8;
        if (off == 12)
            return (uint32_t)f->header << 16;
        if (off == 0x18)
            return f->buses;
        return 0;
    }
    return UINT32_MAX;
}
static void config_write(void *p, uint16_t bdf, unsigned off, uint32_t v, unsigned size)
{
    (void)p;
    (void)bdf;
    (void)off;
    (void)v;
    (void)size;
    ++writes;
}
static void discovery(void)
{
    const struct pci_host_ops ops = {config_read, config_write, NULL};
    platform_pci_set_host(&ops, NULL);
    assert(platform_pci_discover() && platform_pci_count() == 6 && !writes);
    const struct pci_device *d = platform_pci_device(5);
    assert(d->bdf == 0x4108 && d->parent == 0x4000);
    assert(platform_pci_device(3)->parent == 0x2000);
    assert(platform_pci_device(2)->parent == 0x0020);
    platform_pci_list_controllers();
    assert(!writes);
}
int main(void)
{
    tables();
    handoff();
    calibration();
    ranges();
    discovery();
    puts(
        "test-real-prep: IOMMU parsing/disable, gated/wrapping timers, fragmented maps and read-only bridge discovery passed");
}
