/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/amd64.h"
#include "ia64vmm/pci.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"

#if defined(IA64VMM_AMD64_HOST_ONLY)
/* The explicitly selected host-only smoke image has no platform module. */
void amd64_pci_init(const struct boot_info *bi)
{
    (void)bi;
}
#else
struct ecam_range {
    struct pci_ecam_region region;
    uint8_t *hva;
};
static struct ecam_range ranges[16];
static unsigned range_count;
static const struct boot_info *layout;
static uint64_t little(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}
static uint8_t *map(void *opaque, uint64_t base, uint64_t size)
{
    (void)opaque;
    if (!size || base > UINT64_MAX - size || base + size > (UINT64_C(1) << 50))
        return NULL;
    for (unsigned i = 0; i < layout->mem_range_count; ++i) {
        const struct boot_mem_range *r = &layout->mem[i];
        if (r->type != BOOT_MEM_RESERVED && r->type != BOOT_MEM_FRAMEBUFFER &&
            base < r->base + r->size && r->base < base + size) {
            LOGE("PCI MMIO overlaps host RAM/reservation at %llx", (unsigned long long)base);
            return NULL;
        }
    }
    return amd64_map_mmio(base, size);
}
static volatile uint8_t *ecam(uint16_t bdf, unsigned off)
{
    struct pci_address a = {0, bdf >> 8, (bdf >> 3) & 31, bdf & 7};
    uint64_t physical;
    for (unsigned i = 0; i < range_count; ++i)
        if (pci_ecam_address(&ranges[i].region, a, off & ~3u, &physical))
            return ranges[i].hva + (physical - ranges[i].region.base) + (off & 3);
    return NULL;
}
static uint32_t read(void *opaque, uint16_t bdf, unsigned off, unsigned size)
{
    (void)opaque;
    volatile uint8_t *p = ecam(bdf, off);
    if (p) {
        if (size == 1)
            return *p;
        if (size == 2)
            return *(volatile uint16_t *)p;
        return *(volatile uint32_t *)p;
    }
    if (off >= 256)
        return UINT32_MAX;
    struct pci_address a = {0, bdf >> 8, (bdf >> 3) & 31, bdf & 7};
    hal_port_out(0xcf8, pci_cf8_address(a, off & ~3u), 4);
    return hal_port_in(0xcfc + (off & 3), size);
}
static void write(void *opaque, uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    (void)opaque;
    volatile uint8_t *p = ecam(bdf, off);
    if (p) {
        if (size == 1)
            *p = value;
        else if (size == 2)
            *(volatile uint16_t *)p = value;
        else
            *(volatile uint32_t *)p = value;
    } else if (off < 256) {
        struct pci_address a = {0, bdf >> 8, (bdf >> 3) & 31, bdf & 7};
        hal_port_out(0xcf8, pci_cf8_address(a, off & ~3u), 4);
        hal_port_out(0xcfc + (off & 3), value, size);
    }
}
void amd64_pci_init(const struct boot_info *bi)
{
    static const struct pci_host_ops ops = {read, write, map};
    layout = bi;
    range_count = 0;
    const uint8_t *mcfg = amd64_acpi_table(bi, "MCFG");
    if (mcfg) {
        unsigned length = little(mcfg + 4, 4);
        for (unsigned off = 44; off + 16 <= length; off += 16) {
            const uint8_t *p = mcfg + off;
            if (little(p + 8, 2) || p[10] > p[11])
                continue;
            uint64_t base = little(p, 8), delta = (uint64_t)p[10] << 20;
            if ((base & 0xfffff) || base > UINT64_MAX - delta || range_count == 16)
                hal_panic("invalid/too many host MCFG allocations");
            for (unsigned i = 0; i < range_count; ++i)
                if (p[10] <= ranges[i].region.last && ranges[i].region.first <= p[11])
                    hal_panic("overlapping host MCFG bus ranges");
            uint8_t *hva = map(NULL, base + delta, ((uint64_t)p[11] - p[10] + 1) << 20);
            if (!hva)
                hal_panic("cannot map host MCFG");
            ranges[range_count++] = (struct ecam_range){{base + delta, 0, p[10], p[11]}, hva};
            LOGI("host PCI ECAM segment=0 base=%llx buses=%u..%u", (unsigned long long)base, p[10],
                 p[11]);
        }
    }
    if (!range_count)
        LOGI("host PCI config: CF8/CFC fallback");
    platform_pci_set_host(&ops, NULL);
    amd64_intx_init(bi, &ops, NULL);
    if (range_count) {
        unsigned first = 255, last = 0;
        for (unsigned i = 0; i < range_count; ++i) {
            if (ranges[i].region.first < first)
                first = ranges[i].region.first;
            if (ranges[i].region.last > last)
                last = ranges[i].region.last;
        }
        platform_pci_root_buses(first, last);
    }
}
#endif
