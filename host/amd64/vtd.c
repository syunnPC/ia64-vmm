/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Intel VT-d architecture rev. 4.1: legacy translation structures (chapter 9),
 * DMAR scopes/RMRR (chapter 8), root replacement/invalidation (chapter 6). */
#include "host/amd64/vtd.h"
#include "ia64vmm/string.h"

#define PAGE UINT64_C(4096)
#define ADDR UINT64_C(0x000ffffffffff000)
#define BUSY (UINT64_C(1) << 63)
#define TE (1u << 31)
#define SRTP (1u << 30)
#define QIE (1u << 26)
#define IRE (1u << 25)
#define POLLS 1000000u

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static bool fail(struct amd64_vtd *s, const char *error)
{
    s->error = error;
    return false;
}
static bool overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m)
{
    return n && m && a < b + m && b < a + n;
}
static int device(const struct pci_device *const *d, unsigned n, uint16_t bdf)
{
    for (unsigned i = 0; i < n; ++i)
        if (d[i]->bdf == bdf)
            return (int)i;
    return -1;
}
static bool descendant(const struct pci_device *const *d, unsigned n, unsigned child,
                       unsigned parent)
{
    for (unsigned depth = 0; depth < n; ++depth) {
        if (child == parent)
            return true;
        int next = device(d, n, d[child]->parent);
        if (next < 0)
            return false;
        child = (unsigned)next;
    }
    return false;
}
/* Scope header is six bytes, followed by one or more (device,function) pairs.
 * IOAPIC/HPET scopes describe interrupt sources, not PCI DMA contexts. */
static bool scope(struct amd64_vtd *s, const uint8_t *p, unsigned len,
                  const struct pci_device *const *d, unsigned n, bool selected[PCI_MAX_DEVICES],
                  bool rmrr, bool all)
{
    for (unsigned at = 0; at < len;) {
        const uint8_t *q = p + at;
        if (len - at < 8 || q[1] < 8 || q[1] > len - at || (q[1] & 1) || q[2] || q[3])
            return fail(s, "malformed DMAR device scope");
        unsigned type = q[0];
        if (type < 1 || type > (rmrr ? 2u : 4u) || (all && type <= 2))
            return fail(s, "unsupported DMAR scope type/INCLUDE_PCI_ALL scope");
        uint16_t bdf = 0;
        unsigned bus = q[5];
        int index = -1;
        for (unsigned path = 6; path < q[1]; path += 2) {
            if (q[path] > 31 || q[path + 1] > 7)
                return fail(s, "invalid DMAR PCI path");
            bdf = (uint16_t)((bus << 8) | (q[path] << 3) | q[path + 1]);
            index = device(d, n, bdf);
            if (path + 2 < q[1]) {
                if (index < 0 || d[index]->header != 1 || !d[index]->secondary)
                    return fail(s, "DMAR path bridge absent");
                bus = d[index]->secondary;
            }
        }
        if (type <= 2) {
            if (index < 0 || (type == 1 && d[index]->header) ||
                (type == 2 && d[index]->header != 1))
                return fail(s, "DMAR endpoint/subtree absent or wrong type");
            for (unsigned j = 0; j < n; ++j)
                if ((type == 1 && j == (unsigned)index) ||
                    (type == 2 && descendant(d, n, j, (unsigned)index)))
                    selected[j] = true;
        }
        at += q[1];
    }
    return true;
}
static uint64_t new_page(struct amd64_vtd *s, const struct boot_info *bi, unsigned haw)
{
    void *p = s->ops->allocate(s->context, 1);
    uint64_t pa;
    if (!p || !s->ops->physical(s->context, p, &pa) || (pa & (PAGE - 1)) ||
        pa >= (UINT64_C(1) << haw) || !amd64_iommu_covered(bi, pa, pa + PAGE, BOOT_MEM_VMM)) {
        fail(s, "VT-d table allocation unavailable/outside private addressable RAM");
        return 0;
    }
    memset(p, 0, PAGE);
    return pa;
}
static bool map(struct amd64_vtd *s, const struct boot_info *bi, struct amd64_vtd_unit *u,
                unsigned domain, uint64_t start, uint64_t end, unsigned haw, bool large)
{
    if (end > (UINT64_C(1) << u->width) || end > (UINT64_C(1) << haw))
        return fail(s, "DMA range exceeds VT-d address width");
    while (start < end) {
        unsigned target = 1;
        if (large && (u->cap & (UINT64_C(1) << 34)) && !(start & 0x1fffff) &&
            end - start >= 0x200000)
            target = 2;
        if (large && (u->cap & (UINT64_C(1) << 35)) && !(start & 0x3fffffff) &&
            end - start >= 0x40000000)
            target = 3;
        uint64_t *table = s->ops->pointer(s->context, u->domain[domain]);
        for (unsigned level = u->levels; level > target; --level) {
            uint64_t *entry = &table[(start >> (12 + 9 * (level - 1))) & 511];
            if (!*entry) {
                uint64_t pa = new_page(s, bi, haw);
                if (!pa)
                    return false;
                *entry = pa | 3;
            }
            if (*entry & 128)
                return fail(s, "overlapping DMA mappings");
            table = s->ops->pointer(s->context, *entry & ADDR);
        }
        uint64_t *entry = &table[(start >> (12 + 9 * (target - 1))) & 511];
        uint64_t value = start | 3 | (target > 1 ? 128 : 0);
        if (*entry && *entry != value)
            return fail(s, "overlapping DMA mappings");
        *entry = value;
        start += UINT64_C(1) << (12 + 9 * (target - 1));
    }
    return true;
}
static void flush_tree(struct amd64_vtd *s, uint64_t pa, unsigned level)
{
    uint64_t *p = s->ops->pointer(s->context, pa);
    if (level > 1)
        for (unsigned i = 0; i < 512; ++i)
            if ((p[i] & 3) && !(p[i] & 128))
                flush_tree(s, p[i] & ADDR, level - 1);
    s->ops->flush(s->context, p, PAGE);
}
bool amd64_vtd_build(const uint8_t *p, size_t length, const struct boot_info *bi,
                     const struct pci_device *const *d, unsigned n,
                     const struct amd64_iommu_dma_ops *ops, void *context, struct amd64_vtd *s)
{
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->context = context;
    if (!p || !bi || !ops || !ops->allocate || !ops->physical || !ops->pointer || !ops->flush ||
        !ops->config || !ops->read || !ops->write || !d || n > PCI_MAX_DEVICES ||
        bi->mem_range_count > BOOT_MAX_MEM_RANGES || length < 48 || length > 1048576 ||
        memcmp(p, "DMAR", 4) || le(p + 4, 4) != length || p[36] < 31 || p[36] > 51)
        return fail(s, "invalid VT-d input/host address width");
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i)
        sum += p[i];
    if (sum)
        return fail(s, "DMAR checksum mismatch");
    unsigned haw = p[36] + 1;
    const char *memory_error = amd64_iommu_memory(bi);
    if (memory_error)
        return fail(s, memory_error);
    s->haw = haw;
    s->memory = bi;
    uint64_t highest = 0;
    for (unsigned i = 0; i < bi->mem_range_count; ++i) {
        const struct boot_mem_range *m = &bi->mem[i];
        if ((m->type == BOOT_MEM_USABLE || m->type == BOOT_MEM_VMM) && m->size &&
            m->base + m->size > highest)
            highest = m->base + m->size;
    }
    const char *error = amd64_iommu_topology(d, n, ops, context);
    if (error)
        return fail(s, error);
    unsigned owners[PCI_MAX_DEVICES];
    for (unsigned i = 0; i < n; ++i)
        owners[i] = AMD64_IOMMU_MAX_UNITS;
    unsigned all = AMD64_IOMMU_MAX_UNITS;
    for (size_t at = 48; at < length;) {
        if (length - at < 4)
            return fail(s, "truncated DMAR structure");
        const uint8_t *q = p + at;
        unsigned type = le(q, 2), size = le(q + 2, 2);
        if (size < 4 || size > length - at)
            return fail(s, "invalid DMAR structure length");
        if (type == 0) {
            if (size < 16 || s->count == AMD64_IOMMU_MAX_UNITS || le(q + 6, 2) || (q[4] & ~1u) ||
                (q[5] & 0xf0))
                return fail(s, "unsupported DRHD/segment");
            unsigned u = s->count++;
            struct amd64_vtd_unit *unit = &s->units[u];
            unit->base = le(q + 8, 8);
            unit->aperture = 1u << (12 + q[5]);
            if (!unit->base || (unit->base & (unit->aperture - 1)) ||
                unit->base > (UINT64_C(1) << haw) - unit->aperture)
                return fail(s, "invalid VT-d register aperture");
            for (unsigned j = 0; j < u; ++j)
                if (overlap(unit->base, unit->aperture, s->units[j].base, s->units[j].aperture))
                    return fail(s, "overlapping VT-d units");
            if (q[4] & 1) {
                if (all != AMD64_IOMMU_MAX_UNITS)
                    return fail(s, "duplicate DRHD INCLUDE_PCI_ALL");
                all = u;
            } else if (all != AMD64_IOMMU_MAX_UNITS)
                return fail(s, "DRHD INCLUDE_PCI_ALL must follow explicit units");
            bool selected[PCI_MAX_DEVICES] = {0};
            if (!scope(s, q + 16, size - 16, d, n, selected, false, q[4] & 1))
                return false;
            for (unsigned j = 0; j < n; ++j)
                if (selected[j]) {
                    if (owners[j] != AMD64_IOMMU_MAX_UNITS)
                        return fail(s, "ambiguous DRHD device coverage");
                    owners[j] = u;
                }
        } else if (type == 1) {
            if (size < 32 || le(q + 4, 2) || le(q + 6, 2))
                return fail(s, "unsupported RMRR/segment");
            uint64_t base = le(q + 8, 8), limit = le(q + 16, 8);
            if ((base & 4095) || limit <= base || limit == UINT64_MAX || ((limit + 1) & 4095))
                return fail(s, "invalid RMRR extent");
            if (limit + 1 > highest)
                highest = limit + 1;
        } else if (type == 2 || type == 3) {
            if (size < (type == 2 ? 8u : 20u))
                return fail(s, "truncated DMAR ATSR/RHSA");
        } else {
            /* Namespace DMA and integrated ATCs need additional policy. */
            return fail(s, "DMAR namespace/ATC/device properties unsupported");
        }
        at += size;
    }
    if (!s->count)
        return fail(s, "DMAR reports no translation units");
    for (unsigned j = 0; j < n; ++j) {
        if (owners[j] == AMD64_IOMMU_MAX_UNITS)
            owners[j] = all;
        if (owners[j] == AMD64_IOMMU_MAX_UNITS)
            return fail(s, "PCI requester not covered by DMAR");
    }
    s->source_count = n;
    for (unsigned j = 0; j < n; ++j) {
        s->sources[j].bdf = d[j]->bdf;
        s->sources[j].unit = owners[j];
        s->sources[j].bridge = d[j]->header == 1;
        s->sources[j].secondary = d[j]->secondary;
    }
    for (unsigned u = 0; u < s->count; ++u) {
        struct amd64_vtd_unit *unit = &s->units[u];
        unsigned version = ops->read(context, u, 0, 4);
        unit->cap = ops->read(context, u, 8, 8);
        unit->ecap = ops->read(context, u, 0x10, 8);
        unsigned status = ops->read(context, u, 0x1c, 4);
        /* Rev.4.1 §11.4.6: major >=6 rejects register invalidations (CAIG/IAIG=0).
         * Those units require the queued-invalidation backend. */
        if (!(version >> 4) || (version >> 4) >= 6 || unit->cap == UINT64_MAX ||
            unit->ecap == UINT64_MAX || status == UINT32_MAX || (status & (TE | IRE | QIE)))
            return fail(
                s,
                "unsupported VT-d version, unreadable unit or firmware translation/QI/remapping still active");
        unit->iotlb = ((unit->ecap >> 8) & 1023) * 16 + 8;
        if (unit->iotlb < 0x70)
            return fail(s, "invalid VT-d IOTLB register offset");
        if (unit->iotlb + 8 > unit->aperture)
            unit->aperture = (uint32_t)IA64VMM_ALIGN_UP(unit->iotlb + 8, PAGE);
        if (unit->base > (UINT64_C(1) << haw) - unit->aperture)
            return fail(s, "VT-d IOTLB aperture exceeds host address width");
        for (unsigned j = 0; j < u; ++j)
            if (overlap(unit->base, unit->aperture, s->units[j].base, s->units[j].aperture))
                return fail(s, "overlapping VT-d register apertures");
        for (unsigned j = 0; j < bi->mem_range_count; ++j)
            if (overlap(unit->base, unit->aperture, bi->mem[j].base, bi->mem[j].size) &&
                bi->mem[j].type != BOOT_MEM_RESERVED)
                return fail(s, "VT-d registers overlap RAM");
        unsigned mgaw = ((unit->cap >> 16) & 63) + 1;
        for (unsigned levels = 3; levels <= 4; ++levels) {
            unsigned width = 12 + 9 * levels;
            if (width > mgaw)
                width = mgaw;
            if ((unit->cap & (UINT64_C(1) << (levels + 6))) && highest <= (UINT64_C(1) << width)) {
                unit->levels = levels;
                unit->width = width;
                break;
            }
        }
        if (!unit->levels || highest > (UINT64_C(1) << haw))
            return fail(s, "VT-d address width cannot cover DMA RAM");
        unit->root = new_page(s, bi, haw);
        for (unsigned domain = 0; domain < 2; ++domain) {
            unit->domain[domain] = new_page(s, bi, haw);
            if (!unit->root || !unit->domain[domain])
                return false;
            for (unsigned j = 0; j < bi->mem_range_count; ++j) {
                const struct boot_mem_range *m = &bi->mem[j];
                if (m->type != (domain ? BOOT_MEM_VMM : BOOT_MEM_USABLE))
                    continue;
                if (m->base > UINT64_MAX - 4095)
                    return fail(s, "DMA RAM alignment overflow");
                uint64_t start = IA64VMM_ALIGN_UP(m->base, PAGE),
                         end = IA64VMM_ALIGN_DOWN(m->base + m->size, PAGE);
                if (start < end && !map(s, bi, unit, domain, start, end, haw, true))
                    return false;
            }
        }
        uint64_t *root = ops->pointer(context, unit->root);
        for (unsigned j = 0; j < n; ++j)
            if (owners[j] == u && !d[j]->header) {
                unsigned bus = d[j]->bdf >> 8, fn = d[j]->bdf & 255;
                if (!root[bus * 2]) {
                    uint64_t pa = new_page(s, bi, haw);
                    if (!pa)
                        return false;
                    root[bus * 2] = pa | 1;
                }
                uint64_t *entries = ops->pointer(context, root[bus * 2] & ADDR);
                unsigned domain = d[j]->owner == PCI_OWNER_VMM;
                entries[fn * 2] = unit->domain[domain] | 1;
                entries[fn * 2 + 1] = ((uint64_t)(domain + 1) << 8) | (unit->levels - 2);
            }
    }
    /* RMRRs add only firmware-reserved pages, to the owning domain on each
     * relevant unit. Never expose the allocator/image or another owner's RMRR. */
    for (size_t at = 48; at < length; at += le(p + at + 2, 2)) {
        const uint8_t *q = p + at;
        if (le(q, 2) != 1)
            continue;
        uint64_t base = le(q + 8, 8), end = le(q + 16, 8) + 1;
        if (!amd64_iommu_covered(bi, base, end, BOOT_MEM_RESERVED))
            return fail(s, "RMRR overlaps RAM or lacks reserved-memory coverage");
        if (overlap(base, end - base, 0xfee00000, 0x100000))
            return fail(s, "RMRR overlaps interrupt aperture");
        for (unsigned j = 0; j < n; ++j)
            for (unsigned b = 0; b < PCI_BAR_COUNT; ++b) {
                const struct pci_bar *bar = &d[j]->bars[b];
                if (bar->size && !bar->io && overlap(base, end - base, bar->base, bar->size))
                    return fail(s, "RMRR overlaps PCI MMIO");
            }
        bool selected[PCI_MAX_DEVICES] = {0};
        unsigned domains = 0;
        if (!scope(s, q + 24, le(q + 2, 2) - 24, d, n, selected, true, false))
            return false;
        for (unsigned j = 0; j < n; ++j)
            if (selected[j] && !d[j]->header)
                domains |= 1u << (d[j]->owner == PCI_OWNER_VMM);
        if (!domains || domains == 3)
            return fail(s, "RMRR scope has no endpoint or crosses guest/VMM ownership");
        for (size_t earlier = 48; earlier < at; earlier += le(p + earlier + 2, 2)) {
            const uint8_t *old = p + earlier;
            if (le(old, 2) != 1)
                continue;
            if (!overlap(base, end - base, le(old + 8, 8), le(old + 16, 8) - le(old + 8, 8) + 1))
                continue;
            bool previous[PCI_MAX_DEVICES] = {0};
            unsigned old_domains = 0;
            if (!scope(s, old + 24, le(old + 2, 2) - 24, d, n, previous, true, false))
                return false;
            for (unsigned j = 0; j < n; ++j)
                if (previous[j] && !d[j]->header)
                    old_domains |= 1u << (d[j]->owner == PCI_OWNER_VMM);
            if (old_domains != domains)
                return fail(s, "overlapping guest/VMM RMRRs");
        }
        for (unsigned u = 0; u < s->count; ++u) {
            struct amd64_vtd_unit *unit = &s->units[u];
            if (overlap(base, end - base, unit->base, unit->aperture))
                return fail(s, "RMRR overlaps VT-d registers");
            bool used = false;
            for (unsigned j = 0; j < n; ++j)
                if (selected[j] && owners[j] == u && !d[j]->header)
                    used = true;
            if (used && !map(s, bi, unit, domains == 2, base, end, haw, false))
                return false;
        }
    }
    for (unsigned u = 0; u < s->count; ++u)
        if (!(s->units[u].ecap & 1)) {
            const struct amd64_vtd_unit *unit = &s->units[u];
            for (unsigned domain = 0; domain < 2; ++domain)
                flush_tree(s, unit->domain[domain], unit->levels);
            uint64_t *root = ops->pointer(context, unit->root);
            for (unsigned j = 0; j < 256; ++j)
                if (root[j * 2])
                    ops->flush(context, ops->pointer(context, root[j * 2] & ADDR), PAGE);
            ops->flush(context, root, PAGE);
        }
    return true;
}
static bool wait_bits(struct amd64_vtd *s, unsigned u, unsigned off, unsigned width, uint64_t mask,
                      uint64_t expected)
{
    for (unsigned i = 0; i < POLLS; ++i) {
        uint64_t value = s->ops->read(s->context, u, off, width);
        if (value == (width == 4 ? UINT32_MAX : UINT64_MAX))
            return false;
        if ((value & mask) == expected)
            return true;
    }
    return false;
}
bool amd64_vtd_enable(struct amd64_vtd *s)
{
    if (!s || s->error || !s->count)
        return false;
    for (unsigned u = 0; u < s->count; ++u) {
        struct amd64_vtd_unit *unit = &s->units[u];
        uint32_t status = s->ops->read(s->context, u, 0x1c, 4);
        if (status == UINT32_MAX || (status & (TE | IRE | QIE)))
            return fail(s, "VT-d state changed before activation");
        if (!wait_bits(s, u, 0x1c, 4, 1u << 27, 0) ||
            !wait_bits(s, u, 0x64, 4, (1u << 31) | 1u, 0) || !wait_bits(s, u, 0x28, 8, BUSY, 0) ||
            !wait_bits(s, u, unit->iotlb, 8, BUSY, 0))
            return fail(s, "VT-d firmware command/PMR still pending before activation");
        uint32_t command = status & 0x96ffffffu;
        uint32_t fault = s->ops->read(s->context, u, 0x38, 4);
        if (fault == UINT32_MAX)
            return fail(s, "VT-d fault-control register unreadable");
        /* The write's CPU fence publishes WB tables before RTADDR/SRTP. */
        s->ops->write(s->context, u, 0x38, fault | (1u << 31), 4);
        /* Invalidation commands implicitly flush RWBF write buffers (§6.8).
         * CPU cache writeback for noncoherent walkers happened after build. */
        s->ops->write(s->context, u, 0x20, unit->root, 8);
        s->ops->write(s->context, u, 0x18, command | SRTP, 4);
        if (!wait_bits(s, u, 0x1c, 4, SRTP, SRTP))
            return fail(s, "VT-d root-pointer command timed out");
        s->ops->write(s->context, u, 0x28, BUSY | (UINT64_C(1) << 61), 8);
        if (!wait_bits(s, u, 0x28, 8, BUSY, 0) ||
            ((s->ops->read(s->context, u, 0x28, 8) >> 59) & 3) != 1)
            return fail(s, "VT-d context invalidation failed");
        uint64_t drain = ((unit->cap >> 54) & 3) << 48;
        uint64_t reserved = s->ops->read(s->context, u, unit->iotlb, 8) & UINT32_MAX;
        s->ops->write(s->context, u, unit->iotlb, BUSY | (UINT64_C(1) << 60) | drain | reserved, 8);
        if (!wait_bits(s, u, unit->iotlb, 8, BUSY, 0) ||
            ((s->ops->read(s->context, u, unit->iotlb, 8) >> 57) & 3) != 1)
            return fail(s, "VT-d IOTLB invalidation failed");
        s->ops->write(s->context, u, 0x18, command | TE, 4);
        if (!wait_bits(s, u, 0x1c, 4, TE, TE))
            return fail(s, "VT-d translation enable timed out");
    }
    return true;
}
