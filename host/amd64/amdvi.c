/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AMD 48882 Rev 3.09: legacy host translation, IVRS §5.2, startup §2.3,
 * command serialization §2.4 and architectural MMIO registers §3.4. */
#include "host/amd64/amdvi.h"
#include "ia64vmm/string.h"
#define PAGE UINT64_C(4096)
#define ADDR UINT64_C(0x000ffffffffff000)
#define PERM (UINT64_C(3) << 61)
#define REQUESTERS 65536u
#define NONE 255u
#define SPECIAL 128u
#define POLLS 1000000u
/* Only firmware SMI filtering, timeout, ordering and reserved bits survive.
 * Other optional translation/routing engines require their own takeover. */
#define ACTIVE                                                                                     \
    (UINT64_C(0x3f01f) | (UINT64_C(1) << 25) | (UINT64_C(3) << 28) | (UINT64_C(7) << 34) |         \
     (UINT64_C(1) << 39) | (UINT64_C(1) << 40) | (UINT64_C(1) << 42) | (UINT64_C(3) << 43) |       \
     (UINT64_C(1) << 45) | (UINT64_C(3) << 50) | (UINT64_C(3) << 52) | (UINT64_C(1) << 55) |       \
     (UINT64_C(1) << 56) | (UINT64_C(1) << 58) | (UINT64_C(7) << 61))
struct route {
    uint16_t alias;
    uint8_t unit, settings;
    uint16_t special;
};
static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static bool fail(struct amd64_amdvi *s, const char *error)
{
    s->error = error;
    return false;
}
static bool overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m)
{
    return n && m && a < b + m && b < a + n;
}
bool amd64_amdvi_prepare(const struct amd64_iommu_unit *unit, const struct amd64_iommu_dma_ops *ops,
                         void *context)
{
    if (!unit || !ops || !ops->config || !ops->config_write || unit->kind != AMD64_AMDVI ||
        unit->segment || unit->capability < 0x40 || unit->capability > 0xe8 ||
        (unit->capability & 3) || (unit->size != 0x4000 && unit->size != 0x80000) || !unit->base ||
        unit->base > (UINT64_C(1) << 52) - unit->size || (unit->base & (unit->size - 1)))
        return false;
    unsigned at = unit->capability;
    uint32_t cap = ops->config(context, unit->bdf, at, 4);
    if ((cap & UINT32_C(0x08ff00ff)) != UINT32_C(0x080b000f))
        return false;
    uint64_t base = ops->config(context, unit->bdf, at + 4, 4) |
                    ((uint64_t)ops->config(context, unit->bdf, at + 8, 4) << 32);
    if (base == UINT64_MAX)
        return false;
    if (!(base & 1)) {
        /* §3.2: write the upper address before the low RW1S Enable bit locks
         * the capability block. The IVRS aperture has been checked by caller. */
        ops->config_write(context, unit->bdf, at + 8, unit->base >> 32, 4);
        ops->config_write(context, unit->bdf, at + 4, (uint32_t)unit->base | 1, 4);
        base = ops->config(context, unit->bdf, at + 4, 4) |
               ((uint64_t)ops->config(context, unit->bdf, at + 8, 4) << 32);
    }
    /* Base Low[13:1] has unspecified read values (§3.2). */
    return (base & ~UINT64_C(0x3ffe)) == (unit->base | 1);
}
static uint64_t allocate(struct amd64_amdvi *s, const struct boot_info *bi, unsigned n,
                         unsigned haw)
{
    void *p = s->ops->allocate(s->context, n);
    uint64_t pa, size = n * PAGE;
    if (!p || !s->ops->physical(s->context, p, &pa) || (pa & (PAGE - 1)) ||
        pa >= (UINT64_C(1) << haw) || size > (UINT64_C(1) << haw) - pa ||
        !amd64_iommu_covered(bi, pa, pa + size, BOOT_MEM_VMM)) {
        fail(s, "AMD-Vi allocation unavailable/outside private addressable RAM");
        return 0;
    }
    /* The host allocator promises physical contiguity. Check every page as
     * well: the device table cannot be represented by scattered pages. */
    for (unsigned i = 1; i < n; ++i) {
        uint64_t next;
        if (!s->ops->physical(s->context, (uint8_t *)p + i * PAGE, &next) ||
            next != pa + i * PAGE) {
            fail(s, "AMD-Vi allocation is not physically contiguous");
            return 0;
        }
    }
    memset(p, 0, size);
    return pa;
}
static bool mark(struct amd64_amdvi *s, struct route *r, unsigned first, unsigned last,
                 unsigned alias, unsigned settings, bool special)
{
    if (last < first || (settings & 8))
        return fail(s, "invalid IVHD range/DTE settings");
    for (unsigned id = first; id <= last; ++id) {
        unsigned target = alias == REQUESTERS ? id : alias;
        if (r[id].unit != NONE) {
            /* A special interrupt source can also lie inside an ALL/range
             * declaration. It must not create another DMA source or alias. */
            if ((special && r[id].unit == SPECIAL) || (!special && r[id].unit != SPECIAL) ||
                r[id].alias != id || target != id)
                return fail(s, "overlapping IVHD device scopes");
            if (!special)
                continue;
        }
        r[id] = (struct route){(uint16_t)target, special ? SPECIAL : 0, (uint8_t)settings, 0};
    }
    return true;
}
static bool scopes(struct amd64_amdvi *s, const uint8_t *q, unsigned size, struct route *r)
{
    memset(r, 255, REQUESTERS * sizeof(*r));
    unsigned header = q[0] == 0x10 ? 24 : 40;
    for (unsigned at = header; at < size;) {
        const uint8_t *p = q + at;
        unsigned type = p[0], bytes = type < 0x40 ? 4 : 8;
        if (type >= 0x80 || size - at < bytes)
            return fail(s, "unsupported/truncated IVHD device entry");
        unsigned first = le(p + 1, 2), last = first, alias = REQUESTERS;
        if (!type) {
            if (le(p, 4))
                return fail(s, "invalid IVHD padding");
            at += 4;
            continue;
        }
        if (type == 0x48) {
            if (first || (p[7] != 1 && p[7] != 2))
                return fail(s, "invalid IVHD special device");
            unsigned id = le(p + 5, 2);
            if (!mark(s, r, id, id, REQUESTERS, p[3], true))
                return false;
            r[id].special = ((unsigned)p[7] << 8) | p[4];
            at += 8;
            continue;
        }
        if (type != 1 && type != 2 && type != 3 && type != 0x42 && type != 0x43 && type != 0x46 &&
            type != 0x47)
            return fail(s, "unsupported IVHD device entry");
        if (type == 1) {
            first = 0;
            last = 65535;
        }
        if (type == 0x42 || type == 0x43) {
            if (p[4] || p[7])
                return fail(s, "invalid IVHD alias reserved fields");
            alias = le(p + 5, 2);
        }
        if ((type == 0x46 || type == 0x47) && (le(p + 4, 4) & UINT32_C(0x7fffffff)))
            return fail(s, "unsupported IVHD extended settings");
        if (type == 3 || type == 0x43 || type == 0x47) {
            if (size - at < bytes + 4 || p[bytes] != 4 || p[bytes + 3])
                return fail(s, "IVHD range lacks immediate end entry");
            last = le(p + bytes + 1, 2);
            bytes += 4;
        }
        if (!mark(s, r, first, last, alias, p[3], false))
            return false;
        at += bytes;
    }
    for (unsigned id = 0; id < REQUESTERS; ++id)
        if (r[id].unit != NONE)
            return true;
    return fail(s, "IVHD contains no device scopes");
}
static bool map(struct amd64_amdvi *s, const struct boot_info *bi, struct amd64_amdvi_unit *u,
                unsigned domain, uint64_t start, uint64_t end, unsigned haw, uint64_t permission,
                bool large)
{
    if (end > (UINT64_C(1) << u->width) || end > (UINT64_C(1) << haw))
        return fail(s, "AMD-Vi DMA range exceeds address width");
    while (start < end) {
        unsigned target = large && !(start & 0x1fffff) && end - start >= 0x200000 ? 2 : 1;
        uint64_t *table = s->ops->pointer(s->context, u->domain[domain]);
        for (unsigned level = u->levels; level > target; --level) {
            uint64_t *entry = &table[(start >> (12 + 9 * (level - 1))) & 511];
            if (!(*entry & 1)) {
                if (!permission) {
                    table = NULL;
                    break;
                }
                uint64_t pa = allocate(s, bi, 1, haw);
                if (!pa)
                    return false;
                *entry = pa | PERM | ((uint64_t)(level - 1) << 9) | 1;
            } else if (!(*entry & 0xe00)) {
                /* Split an identity leaf before applying a 4K IVMD constraint. */
                uint64_t pa = allocate(s, bi, 1, haw);
                if (!pa)
                    return false;
                uint64_t *child = s->ops->pointer(s->context, pa);
                uint64_t base = *entry & ADDR, step = UINT64_C(1) << (12 + 9 * (level - 2));
                for (unsigned j = 0; j < 512; ++j)
                    child[j] = (base + j * step) | (*entry & PERM) | 1;
                *entry = pa | PERM | ((uint64_t)(level - 1) << 9) | 1;
            }
            if (!(*entry & 1)) {
                table = NULL;
                break;
            }
            if (((*entry >> 9) & 7) != level - 1)
                return fail(s, "invalid AMD-Vi page table level");
            table = s->ops->pointer(s->context, *entry & ADDR);
        }
        if (table)
            table[(start >> (12 + 9 * (target - 1))) & 511] =
                permission ? start | permission | 1 : 0;
        start += UINT64_C(1) << (12 + 9 * (target - 1));
    }
    return true;
}
static void flush_tree(struct amd64_amdvi *s, uint64_t pa)
{
    uint64_t *p = s->ops->pointer(s->context, pa);
    for (unsigned i = 0; i < 512; ++i)
        if ((p[i] & 1) && (p[i] & 0xe00))
            flush_tree(s, p[i] & ADDR);
    s->ops->flush(s->context, p, PAGE);
}
static bool safe_range(struct amd64_amdvi *s, const struct pci_device *const *d, unsigned n,
                       uint64_t base, uint64_t size)
{
    if (overlap(base, size, UINT64_C(0xfee00000), 0x100000) ||
        overlap(base, size, UINT64_C(0xfd00000000), UINT64_C(0x100000000)))
        return fail(s, "AMD-Vi DMA range overlaps architectural interrupt/control space");
    for (unsigned u = 0; u < s->count; ++u)
        if (overlap(base, size, s->units[u].base, s->units[u].aperture))
            return fail(s, "AMD-Vi DMA range overlaps remapper registers");
    for (unsigned j = 0; j < n; ++j)
        for (unsigned b = 0; b < PCI_BAR_COUNT; ++b) {
            const struct pci_bar *bar = &d[j]->bars[b];
            if (bar->size > UINT64_MAX - bar->base)
                return fail(s, "PCI BAR extent overflow");
            if (!bar->io && overlap(base, size, bar->base, bar->size))
                return fail(s, "AMD-Vi DMA range overlaps PCI MMIO");
        }
    return true;
}
static unsigned ivmd_domains(const uint8_t *q, unsigned unit, const struct route *r,
                             const struct pci_device *const *d, unsigned n)
{
    unsigned first = q[0] == 0x20 ? 0 : le(q + 4, 2);
    unsigned last = q[0] == 0x20 ? 65535 : q[0] == 0x21 ? first : le(q + 6, 2);
    unsigned domains = 0;
    for (unsigned i = 0; i < n; ++i) {
        unsigned id = d[i]->bdf, alias = r[id].alias;
        if (!d[i]->header && (unit == AMD64_IOMMU_MAX_UNITS || r[id].unit == unit) &&
            ((id >= first && id <= last) || (alias >= first && alias <= last)))
            domains |= 1u << (d[i]->owner == PCI_OWNER_VMM);
    }
    return domains;
}
static bool ivmd(struct amd64_amdvi *s, const uint8_t *p, size_t length, const struct boot_info *bi,
                 const struct route *r, const struct pci_device *const *d, unsigned n, unsigned haw)
{
    for (size_t at = 48; at < length; at += le(p + at + 2, 2)) {
        const uint8_t *q = p + at;
        if (q[0] < 0x20 || q[0] > 0x22)
            continue;
        unsigned size = le(q + 2, 2), flags = q[1];
        uint64_t base = le(q + 16, 8), bytes = le(q + 24, 8);
        if (size != 32 || (flags & 0xf8) || (!(flags & 6) && (flags & 1)) || le(q + 8, 8) ||
            (q[0] == 0x20 && le(q + 4, 4)) || (q[0] == 0x21 && le(q + 6, 2)) ||
            (q[0] == 0x22 && le(q + 6, 2) < le(q + 4, 2)) || !bytes || bytes > UINT64_MAX - base ||
            ((base | bytes) & (PAGE - 1)))
            return fail(s, "unsupported IVMD exclusion/segment/flags or invalid extent");
        if (!safe_range(s, d, n, base, bytes))
            return false;
        unsigned owners = ivmd_domains(q, AMD64_IOMMU_MAX_UNITS, r, d, n);
        if ((flags & 6) && owners == 3)
            return fail(s, "IVMD firmware buffer crosses guest/VMM ownership");
        for (size_t earlier = 48; earlier < at; earlier += le(p + earlier + 2, 2)) {
            const uint8_t *old = p + earlier;
            if (old[0] < 0x20 || old[0] > 0x22 || !(old[1] & 6) || !(flags & 6))
                continue;
            unsigned previous = ivmd_domains(old, AMD64_IOMMU_MAX_UNITS, r, d, n);
            if ((owners | previous) == 3 && overlap(base, bytes, le(old + 16, 8), le(old + 24, 8)))
                return fail(s, "overlapping guest/VMM IVMD firmware buffers");
        }
        for (unsigned u = 0; u < s->count; ++u) {
            unsigned domains = ivmd_domains(q, u, r, d, n);
            for (unsigned domain = 0; domain < 2; ++domain)
                if (domains & (1u << domain)) {
                    /* Shared domains cannot express contradictory per-device
                 * constraints. Refuse these rather than widen permissions. */
                    for (size_t earlier = 48; earlier < at; earlier += le(p + earlier + 2, 2)) {
                        const uint8_t *old = p + earlier;
                        if (old[0] < 0x20 || old[0] > 0x22)
                            continue;
                        if (overlap(base, bytes, le(old + 16, 8), le(old + 24, 8)) &&
                            (ivmd_domains(old, u, r, d, n) & (1u << domain)) &&
                            ((old[1] ^ flags) & 6))
                            return fail(s, "conflicting IVMD permissions in shared DMA domain");
                    }
                    uint64_t end = base + bytes;
                    if ((flags & 6) && !amd64_iommu_covered(bi, base, end, BOOT_MEM_RESERVED) &&
                        !amd64_iommu_covered(bi, base, end,
                                             domain ? BOOT_MEM_VMM : BOOT_MEM_USABLE))
                        return fail(s, "IVMD exposes private/other-owner or unreserved memory");
                    uint64_t permission = ((uint64_t)((flags >> 1) & 3)) << 61;
                    if (!map(s, bi, &s->units[u], domain, base, end, haw, permission, false))
                        return false;
                }
        }
    }
    return true;
}
bool amd64_amdvi_build(const uint8_t *p, size_t length, const struct boot_info *bi,
                       const struct pci_device *const *d, unsigned n,
                       const struct amd64_iommu_dma_ops *ops, void *context, struct amd64_amdvi *s)
{
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->context = context;
    if (!p || !bi || !ops || !ops->allocate || !ops->physical || !ops->pointer || !ops->flush ||
        !ops->config || !ops->read || !ops->write || !d || n > PCI_MAX_DEVICES ||
        bi->mem_range_count > BOOT_MAX_MEM_RANGES || length < 48 || length > 1048576 ||
        memcmp(p, "IVRS", 4) || le(p + 4, 4) != length || !(p[36] & 1) || (p[8] != 1 && p[8] != 2))
        return fail(s, "invalid AMD-Vi input or EFR unavailable");
    struct amd64_iommu_table locations;
    if (!amd64_iommu_parse(p, length, &locations) || !locations.count)
        return fail(s, "invalid/empty IVRS");
    unsigned haw = (le(p + 36, 4) >> 8) & 127;
    s->haw = haw;
    s->memory = bi;
    if (haw != 40 && haw != 48 && haw != 52)
        return fail(s, "invalid IVRS physical address width");
    const char *error = amd64_iommu_memory(bi);
    if (!error)
        error = amd64_iommu_topology(d, n, ops, context);
    if (error)
        return fail(s, error);
    s->count = locations.count;
    const uint8_t *headers[AMD64_IOMMU_MAX_UNITS] = {0};
    for (size_t at = 48; at < length; at += le(p + at + 2, 2)) {
        const uint8_t *q = p + at;
        if (q[0] >= 0x20 && q[0] <= 0x22) {
            if (le(q + 2, 2) != 32)
                return fail(s, "invalid IVMD size");
            continue;
        }
        unsigned u = 0;
        while (u < s->count && locations.units[u].base != le(q + 8, 8))
            ++u;
        if (u == s->count || le(q + 16, 2) || (q[0] == 0x40 && p[8] != 2))
            return fail(s, "unsupported IVHD segment");
        if (!headers[u] || q[0] > headers[u][0])
            headers[u] = q;
    }
    for (unsigned u = 0; u < s->count; ++u) {
        const uint8_t *q = headers[u];
        struct amd64_amdvi_unit *unit = &s->units[u];
        if (!q)
            return fail(s, "missing IVHD unit header");
        unit->base = locations.units[u].base;
        unit->bdf = le(q + 4, 2);
        unit->capability = le(q + 6, 2);
        if (unit->capability < 0x40 || unit->capability > 0xe8 || (unit->capability & 3) ||
            (q[0] != 0x10 && ((le(q + 20, 4) & UINT32_C(0xf0001fff)) || (q[1] & 0xc0))))
            return fail(s, "unsupported IVHD attributes/capability");
        uint32_t cap = ops->config(context, unit->bdf, unit->capability, 4);
        uint64_t base = ops->config(context, unit->bdf, unit->capability + 4, 4) |
                        ((uint64_t)ops->config(context, unit->bdf, unit->capability + 8, 4) << 32);
        uint32_t misc = ops->config(context, unit->bdf, unit->capability + 16, 4);
        unsigned pa = (misc >> 8) & 127, va = (misc >> 15) & 127;
        unit->efr = ops->read(context, u, 0x30, 8);
        unit->control = ops->read(context, u, 0x18, 8);
        uint64_t status = ops->read(context, u, 0x2020, 8);
        if ((cap & UINT32_C(0x08ff00ff)) != UINT32_C(0x080b000f) ||
            (base & ~UINT64_C(0x3ffe)) != (unit->base | 1) || misc == UINT32_MAX ||
            (pa != 40 && pa != 48 && pa != 52) || pa < haw ||
            (va != 32 && va != 40 && va != 48 && va != 64) || unit->efr == UINT64_MAX ||
            ((unit->efr >> 10) & 3) == 3 || !(unit->efr & (UINT64_C(1) << 6)) ||
            unit->control == UINT64_MAX || (unit->control & ACTIVE) || status == UINT64_MAX ||
            (status & 0x198))
            return fail(
                s,
                "AMD-Vi unsupported capability/width, no global invalidation, or active firmware engines");
        if (q[0] != 0x10 && le(q + 24, 8) != unit->efr)
            return fail(s, "IVHD EFR image differs from hardware");
        unit->aperture = unit->efr & (UINT64_C(1) << 9) ? 0x80000 : 0x4000;
        if ((unit->base & (unit->aperture - 1)) ||
            unit->base > (UINT64_C(1) << haw) - unit->aperture)
            return fail(s, "invalid AMD-Vi register aperture");
        for (unsigned previous = 0; previous < u; ++previous)
            if (overlap(unit->base, unit->aperture, s->units[previous].base,
                        s->units[previous].aperture))
                return fail(s, "overlapping AMD-Vi units");
        for (unsigned j = 0; j < bi->mem_range_count; ++j) {
            const struct boot_mem_range *m = &bi->mem[j];
            if (m->type != BOOT_MEM_RESERVED &&
                overlap(unit->base, unit->aperture, m->base, m->size))
                return fail(s, "AMD-Vi registers overlap RAM");
        }
        uint64_t highest = 0;
        for (unsigned j = 0; j < bi->mem_range_count; ++j) {
            const struct boot_mem_range *m = &bi->mem[j];
            if (m->type != BOOT_MEM_USABLE && m->type != BOOT_MEM_VMM)
                continue;
            if (m->base > UINT64_MAX - 4095)
                return fail(s, "DMA RAM alignment overflow");
            if (!safe_range(s, d, n, m->base, m->size))
                return false;
            if (m->size && m->base + m->size > highest)
                highest = m->base + m->size;
        }
        for (size_t at = 48; at < length; at += le(p + at + 2, 2)) {
            const uint8_t *q = p + at;
            if (q[0] < 0x20 || q[0] > 0x22 || !(q[1] & 6))
                continue;
            uint64_t base = le(q + 16, 8), bytes = le(q + 24, 8);
            if (bytes > UINT64_MAX - base)
                return fail(s, "IVMD extent overflow");
            if (base + bytes > highest)
                highest = base + bytes;
        }
        unit->levels = highest <= (UINT64_C(1) << 39) ? 3 : 4;
        unit->width = 12 + 9 * unit->levels;
        if (unit->width > va)
            unit->width = va;
        if (highest > (UINT64_C(1) << unit->width) || highest > (UINT64_C(1) << haw))
            return fail(s, "AMD-Vi address width cannot cover DMA RAM");
        /* Apply the firmware's ordering/tunnel recommendations. Keep optional
         * engines disabled and use coherent page-table walks (DTE SD=0). */
        unit->firmware_control = unit->control;
        unit->control = (unit->control & ~UINT64_C(0xf02)) | ((uint64_t)(q[1] & 6) << 7) |
                        ((uint64_t)(q[1] & 8) << 8) | ((uint64_t)(q[1] & 0x20) << 5) |
                        ((q[1] & 1) << 1);
        unit->table = allocate(s, bi, 512, haw);
        unit->command = allocate(s, bi, 1, haw);
        unit->event = allocate(s, bi, 1, haw);
        unit->completion = allocate(s, bi, 1, haw);
        if (!unit->table || !unit->command || !unit->event || !unit->completion)
            return false;
        for (unsigned domain = 0; domain < 3; ++domain) {
            unit->domain[domain] = allocate(s, bi, 1, haw);
            if (!unit->domain[domain])
                return false;
            if (domain == 2)
                continue;
            for (unsigned j = 0; j < bi->mem_range_count; ++j) {
                const struct boot_mem_range *m = &bi->mem[j];
                if (m->type != (domain ? BOOT_MEM_VMM : BOOT_MEM_USABLE))
                    continue;
                uint64_t start = IA64VMM_ALIGN_UP(m->base, PAGE),
                         end = IA64VMM_ALIGN_DOWN(m->base + m->size, PAGE);
                if (start < end && !map(s, bi, unit, domain, start, end, haw, PERM, true))
                    return false;
            }
        }
        uint64_t *table = ops->pointer(context, unit->table);
        for (unsigned id = 0; id < REQUESTERS; ++id)
            table[id * 4] = unit->domain[2] | (UINT64_C(3) << 9) | 3;
    }
    uint64_t metadata = allocate(s, bi, REQUESTERS * sizeof(struct route) / PAGE, haw);
    uint64_t temporary = allocate(s, bi, REQUESTERS * sizeof(struct route) / PAGE, haw);
    if (!metadata || !temporary)
        return false;
    struct route *routes = ops->pointer(context, metadata),
                 *scratch = ops->pointer(context, temporary);
    memset(routes, 255, REQUESTERS * sizeof(*routes));
    for (unsigned u = 0; u < s->count; ++u) {
        const uint8_t *q = headers[u];
        if (!scopes(s, q, le(q + 2, 2), scratch))
            return false;
        for (unsigned id = 0; id < REQUESTERS; ++id)
            if (scratch[id].unit != NONE) {
                if (routes[id].unit != NONE)
                    return fail(s, "ambiguous IVHD unit coverage");
                routes[id] = scratch[id];
                routes[id].unit |= u;
            }
        for (size_t at = 48; at < length; at += le(p + at + 2, 2)) {
            const uint8_t *old = p + at;
            if (old == q || (old[0] >= 0x20 && old[0] <= 0x22) ||
                le(old + 8, 8) != s->units[u].base)
                continue;
            if (le(old + 4, 4) != le(q + 4, 4) || (old[1] & 0x3f) != (q[1] & 0x3f) ||
                (old[0] != 0x10 && ((le(old + 20, 4) & UINT32_C(0xf0001fff)) ||
                                    le(old + 24, 8) != s->units[u].efr)) ||
                !scopes(s, old, le(old + 2, 2), scratch))
                return fail(s, "inconsistent alternate IVHD headers/scopes");
            for (unsigned id = 0; id < REQUESTERS; ++id) {
                bool present = (routes[id].unit & ~SPECIAL) == u && routes[id].unit != NONE;
                if (present != (scratch[id].unit != NONE) ||
                    (present && (routes[id].alias != scratch[id].alias ||
                                 routes[id].settings != scratch[id].settings ||
                                 (routes[id].unit & SPECIAL) != (scratch[id].unit & SPECIAL) ||
                                 routes[id].special != scratch[id].special)))
                    return fail(s, "alternate IVHD scopes differ");
            }
        }
    }
    for (unsigned id = 0; id < REQUESTERS; ++id)
        if (routes[id].unit != NONE && (routes[id].unit & SPECIAL)) {
            unsigned u = routes[id].unit & ~SPECIAL;
            uint64_t *table = ops->pointer(context, s->units[u].table);
            table[id * 4 + 1] = (uint64_t)((routes[id].settings >> 4) & 3) << 40;
            if ((routes[id].special >> 8) == 1) {
                unsigned handle = routes[id].special & 255;
                if (s->ioapics[handle].present)
                    return fail(s, "ambiguous IVHD IOAPIC handle");
                s->ioapics[handle].present = true;
                s->ioapics[handle].source = s->source_count;
                s->sources[s->source_count++] = (struct amd64_amdvi_irq_source){
                    .bdf = id,
                    .unit = u,
                };
            }
        }
    for (unsigned j = 0; j < n; ++j) {
        unsigned id = d[j]->bdf;
        const struct route *r = &routes[id];
        if (r->unit == NONE || (r->unit & SPECIAL))
            return fail(s, "PCI requester absent from IVHD or collides with special source");
        unsigned alias = r->alias, u = r->unit;
        if (routes[alias].unit != u || routes[alias].alias != alias)
            return fail(s, "IVHD alias target missing, chained or on another unit");
        s->sources[s->source_count++] = (struct amd64_amdvi_irq_source){
            .bdf = id,
            .unit = u,
            .msi = alias == id,
        };
        /* Shared requester IDs cannot authenticate an individual MSI source. */
        for (unsigned other = 0; other < REQUESTERS; ++other)
            if (other != id && routes[other].unit == u && routes[other].alias == id)
                s->sources[s->source_count - 1].msi = false;
        if (d[j]->header)
            continue;
        struct amd64_amdvi_unit *unit = &s->units[u];
        unsigned domain = d[j]->owner == PCI_OWNER_VMM;
        uint64_t *table = ops->pointer(context, unit->table);
        for (unsigned which = 0; which < 2; ++which) {
            unsigned source = which ? alias : id;
            uint64_t *entry = table + source * 4;
            uint64_t value = unit->domain[domain] | ((uint64_t)unit->levels << 9) | PERM | 3;
            uint64_t policy = (domain + 1) | ((uint64_t)((r->settings >> 4) & 3) << 40);
            if ((entry[0] & PERM) && (entry[0] != value || entry[1] != policy))
                return fail(s, "IVHD alias crosses DMA ownership/settings");
            entry[0] = value;
            entry[1] = policy;
        }
    }
    if (!ivmd(s, p, length, bi, routes, d, n, haw))
        return false;
    for (unsigned u = 0; u < s->count; ++u) {
        struct amd64_amdvi_unit *unit = &s->units[u];
        for (unsigned domain = 0; domain < 3; ++domain)
            flush_tree(s, unit->domain[domain]);
        ops->flush(context, ops->pointer(context, unit->table), 512 * PAGE);
        ops->flush(context, ops->pointer(context, unit->command), PAGE);
        ops->flush(context, ops->pointer(context, unit->event), PAGE);
        ops->flush(context, ops->pointer(context, unit->completion), PAGE);
    }
    return true;
}
static bool wait_bits(struct amd64_amdvi *s, unsigned u, unsigned off, uint64_t mask,
                      uint64_t expected)
{
    for (unsigned i = 0; i < POLLS; ++i) {
        uint64_t v = s->ops->read(s->context, u, off, 8);
        if (v == UINT64_MAX)
            return false;
        if ((v & mask) == expected)
            return true;
    }
    return false;
}
static bool events(struct amd64_amdvi *s, unsigned u)
{
    struct amd64_amdvi_unit *unit = &s->units[u];
    uint64_t status = s->ops->read(s->context, u, 0x2020, 8);
    uint64_t head = s->ops->read(s->context, u, 0x2010, 8),
             tail = s->ops->read(s->context, u, 0x2018, 8);
    if (status == UINT64_MAX || (status & 0x19) != 0x18 || ((head | tail) & ~UINT64_C(0xff0)))
        return fail(s, "AMD-Vi command/event engine stopped, overflowed or unreadable");
    while (head != tail) {
        volatile uint64_t *event = s->ops->pointer(s->context, unit->event + head);
        /* Denied DMA may already be in flight at activation. Any table,
         * command or hardware error invalidates the protection claim. */
        if ((event[0] >> 60) != 2)
            return fail(s, "AMD-Vi activation logged a table/command/hardware error");
        head = (head + 16) & 4095;
    }
    if (head != s->ops->read(s->context, u, 0x2010, 8))
        s->ops->write(s->context, u, 0x2010, head, 8);
    return true;
}
static bool complete(struct amd64_amdvi *s, unsigned u, unsigned commands)
{
    struct amd64_amdvi_unit *unit = &s->units[u];
    uint64_t *ring = s->ops->pointer(s->context, unit->command);
    volatile uint64_t *done = s->ops->pointer(s->context, unit->completion);
    unsigned at = unit->tail + commands * 16;
    at &= 4095;
    ring[at / 8] = (UINT64_C(1) << 60) | unit->completion | 5;
    ring[at / 8 + 1] = ++unit->sequence;
    unit->tail = (at + 16) & 4095;
    s->ops->flush(s->context, ring, PAGE);
    s->ops->write(s->context, u, 0x2008, unit->tail, 8);
    bool finished = false;
    for (unsigned i = 0; i < POLLS; ++i) {
        if (*done == unit->sequence) {
            finished = true;
            break;
        }
        if (!(s->ops->read(s->context, u, 0x2020, 8) & 0x10))
            break;
    }
    if (!finished || !wait_bits(s, u, 0x2000, UINT64_MAX, unit->tail))
        return fail(s, "AMD-Vi command completion timed out");
    return events(s, u);
}
bool amd64_amdvi_enable(struct amd64_amdvi *s)
{
    if (!s || s->error || !s->count)
        return false;
    for (unsigned u = 0; u < s->count; ++u) {
        struct amd64_amdvi_unit *unit = &s->units[u];
        uint64_t control = s->ops->read(s->context, u, 0x18, 8);
        if (control != unit->firmware_control || (control & ACTIVE) ||
            !wait_bits(s, u, 0x2020, 0x198, 0))
            return fail(s, "AMD-Vi firmware state changed before activation");
        s->ops->write(s->context, u, 0, unit->table | 511, 8);
        s->ops->write(s->context, u, 8, unit->command | (UINT64_C(8) << 56), 8);
        s->ops->write(s->context, u, 0x10, unit->event | (UINT64_C(8) << 56), 8);
        s->ops->write(s->context, u, 0x2000, 0, 8);
        s->ops->write(s->context, u, 0x2008, 0, 8);
        s->ops->write(s->context, u, 0x2010, 0, 8);
        s->ops->write(s->context, u, 0x2018, 0, 8);
        /* Never retain a firmware translation/access-check bypass. */
        s->ops->write(s->context, u, 0x20, 0, 8);
        s->ops->write(s->context, u, 0x28, 0, 8);
        s->ops->write(s->context, u, 0x2020, 7, 8);
        if (s->ops->read(s->context, u, 0, 8) != (unit->table | 511) ||
            s->ops->read(s->context, u, 8, 8) != (unit->command | (UINT64_C(8) << 56)) ||
            s->ops->read(s->context, u, 0x10, 8) != (unit->event | (UINT64_C(8) << 56)) ||
            s->ops->read(s->context, u, 0x20, 8))
            return fail(s, "AMD-Vi table/ring/exclusion registers did not latch");
        s->ops->write(s->context, u, 0x18, unit->control | 0x1005, 8);
        if (!wait_bits(s, u, 0x18, ACTIVE, (unit->control | 0x1005) & ACTIVE) ||
            !wait_bits(s, u, 0x2020, 0x18, 0x18))
            return fail(s, "AMD-Vi translation/command/event enable failed");
        uint64_t *ring = s->ops->pointer(s->context, unit->command);
        ring[0] = UINT64_C(8) << 60;
        ring[1] = 0;
        if (!complete(s, u, 1))
            return false;
    }
    s->enabled = true;
    return true;
}

static bool irq_fail(struct amd64_amdvi *s, const char *error)
{
    s->irq_failed = true;
    return fail(s, error);
}
static bool irq_range(unsigned first, unsigned count)
{
    return first >= 0x40 && first < 0xe0 && count && count <= 0xe0 - first;
}
/* §§2.4.2, 2.4.5, 2.4.11: first invalidate DTE/IR caches and wait. A second
 * DTE invalidation then matches every source transaction issued using an old
 * IRTE, fencing its writes to the host bridge before LAPIC retirement. */
static bool irq_invalidate(struct amd64_amdvi *s, struct amd64_amdvi_irq_source *source)
{
    struct amd64_amdvi_unit *unit = &s->units[source->unit];
    if (!wait_bits(s, source->unit, 0x2000, UINT64_MAX, unit->tail))
        return irq_fail(s, "AMD-Vi interrupt command queue did not drain");
    uint64_t *ring = s->ops->pointer(s->context, unit->command);
    unsigned at = unit->tail, next = (at + 16) & 4095;
    ring[at / 8] = (UINT64_C(2) << 60) | source->bdf;
    ring[at / 8 + 1] = 0;
    ring[next / 8] = (UINT64_C(5) << 60) | source->bdf;
    ring[next / 8 + 1] = 0;
    if (!complete(s, source->unit, 2)) {
        s->irq_failed = true;
        return false;
    }
    at = unit->tail;
    ring[at / 8] = (UINT64_C(2) << 60) | source->bdf;
    ring[at / 8 + 1] = 0;
    if (!complete(s, source->unit, 1)) {
        s->irq_failed = true;
        return false;
    }
    return true;
}
bool amd64_amdvi_irq_init(struct amd64_amdvi *s, bool x2apic)
{
    if (!s || s->error || !s->enabled || s->irq_ready || s->irq_failed)
        return false;
    s->x2apic = x2apic;
    for (unsigned u = 0; u < s->count; ++u) {
        struct amd64_amdvi_unit *unit = &s->units[u];
        uint64_t control = s->ops->read(s->context, u, 0x18, 8);
        if (control != (unit->control | 0x1005) || (x2apic && (unit->efr & 0x84) != 0x84) ||
            !events(s, u))
            return irq_fail(s, "AMD-Vi interrupt/APIC format unsupported or unit not ready");
    }
    for (unsigned i = 0; i < s->source_count; ++i) {
        s->sources[i].table = allocate(s, s->memory, 1, s->haw);
        if (!s->sources[i].table) {
            s->irq_failed = true;
            return false;
        }
    }
    for (unsigned u = 0; u < s->count; ++u) {
        struct amd64_amdvi_unit *unit = &s->units[u];
        uint64_t *table = s->ops->pointer(s->context, unit->table);
        for (unsigned id = 0; id < REQUESTERS; ++id)
            __atomic_store_n(&table[id * 4 + 2], UINT64_C(33), __ATOMIC_RELEASE);
        s->ops->flush(s->context, table, 512 * PAGE);
        uint64_t *ring = s->ops->pointer(s->context, unit->command);
        ring[unit->tail / 8] = UINT64_C(8) << 60;
        ring[unit->tail / 8 + 1] = 0;
        if (!complete(s, u, 1)) {
            s->irq_failed = true;
            return false;
        }
    }
    /* All DTEs still deny interrupts while the uniform IRTE format changes.
     * §2.2.5.3 requires GAEn with XTEn even with GuestMode=0 throughout. */
    if (x2apic)
        for (unsigned u = 0; u < s->count; ++u) {
            uint64_t control = s->units[u].control | 0x1005 | (UINT64_C(1) << 50) | (1u << 17);
            s->ops->write(s->context, u, 0x18, control, 8);
            if (!wait_bits(s, u, 0x18, UINT64_MAX, control))
                return irq_fail(s, "AMD-Vi extended interrupt format did not latch");
        }
    for (unsigned i = 0; i < s->source_count; ++i) {
        struct amd64_amdvi_irq_source *source = &s->sources[i];
        uint64_t pa = s->units[source->unit].table + (uint64_t)source->bdf * 32;
        uint64_t *entry = s->ops->pointer(s->context, pa);
        /* Table 7: 256 entries, IV=1, IG=1, IntCtl=10, all Pass bits clear. */
        __atomic_store_n(&entry[2], source->table | (UINT64_C(2) << 60) | 49, __ATOMIC_RELEASE);
        s->ops->flush(s->context, s->ops->pointer(s->context, pa & ~(PAGE - 1)), PAGE);
        if (!irq_invalidate(s, source))
            return false;
    }
    s->irq_ready = true;
    return true;
}
static void *irq_entry(struct amd64_amdvi *s, struct amd64_amdvi_irq_source *source, unsigned index)
{
    uint8_t *table = s->ops->pointer(s->context, source->table);
    return table + index * (s->x2apic ? 16 : 4);
}
bool amd64_amdvi_irq_clear(struct amd64_amdvi *s, unsigned first, unsigned count)
{
    if (!s || s->irq_failed || !s->irq_ready)
        return false;
    s->error = NULL;
    if (!irq_range(first, count))
        return fail(s, "invalid AMD-Vi interrupt entry range");
    for (unsigned i = 0; i < s->source_count; ++i) {
        struct amd64_amdvi_irq_source *source = &s->sources[i];
        bool changed = false;
        for (unsigned index = first; index < first + count; ++index) {
            void *entry = irq_entry(s, source, index);
            if (s->x2apic) {
                uint64_t value = *(uint64_t *)entry;
                if (value & 1) {
                    __atomic_store_n((uint64_t *)entry, value & ~UINT64_C(1), __ATOMIC_RELEASE);
                    changed = true;
                }
            } else {
                uint32_t value = *(uint32_t *)entry;
                if (value & 1) {
                    __atomic_store_n((uint32_t *)entry, value & ~1u, __ATOMIC_RELEASE);
                    changed = true;
                }
            }
        }
        if (changed && !irq_invalidate(s, source))
            return false;
        memset(irq_entry(s, source, first), 0, count * (s->x2apic ? 16 : 4));
    }
    return true;
}
static bool irq_publish(struct amd64_amdvi *s, struct amd64_amdvi_irq_source *source,
                        unsigned first, unsigned count, uint32_t apic, bool level)
{
    if (!irq_range(first, count) || (!s->x2apic && apic > 255))
        return fail(s, "invalid AMD-Vi interrupt destination/vector");
    if (!amd64_amdvi_irq_clear(s, first, count))
        return false;
    for (unsigned i = first; i < first + count; ++i) {
        void *entry = irq_entry(s, source, i);
        uint64_t low = (level ? 32 : 0) | ((uint64_t)(apic & 0xffffff) << 8);
        if (s->x2apic) {
            ((uint64_t *)entry)[1] = ((uint64_t)(apic >> 24) << 56) | i;
            __atomic_store_n((uint64_t *)entry, low | 1, __ATOMIC_RELEASE);
        } else {
            __atomic_store_n((uint32_t *)entry, (uint32_t)low | (i << 16) | 1, __ATOMIC_RELEASE);
        }
    }
    return irq_invalidate(s, source);
}
bool amd64_amdvi_irq_msi(struct amd64_amdvi *s, uint16_t segment, uint16_t bdf, unsigned first,
                         unsigned count, uint32_t apic, uint64_t *address, uint32_t *data)
{
    if (!s || s->irq_failed || !s->irq_ready)
        return false;
    s->error = NULL;
    if (segment || !address || !data || !count || count > 32 || (count & (count - 1)) ||
        (first & (count - 1)))
        return fail(s, "invalid AMD-Vi MSI source/message block");
    struct amd64_amdvi_irq_source *source = NULL;
    for (unsigned i = 0; i < s->source_count; ++i)
        if (s->sources[i].bdf == bdf && s->sources[i].msi)
            source = &s->sources[i];
    if (!source)
        return fail(s, "uncovered or aliased AMD-Vi MSI requester");
    if (!irq_publish(s, source, first, count, apic, false))
        return false;
    *address = UINT64_C(0xfee00000);
    *data = first;
    return true;
}
bool amd64_amdvi_irq_ioapic(struct amd64_amdvi *s, unsigned id, unsigned pin, unsigned vector,
                            uint32_t apic, bool low, bool level, uint64_t *rte)
{
    if (!s || s->irq_failed || !s->irq_ready)
        return false;
    s->error = NULL;
    if (id > 255 || pin > 255 || !rte || !s->ioapics[id].present)
        return fail(s, "IOAPIC lacks an exact IVHD special source");
    if (!irq_publish(s, &s->sources[s->ioapics[id].source], vector, 1, apic, level))
        return false;
    *rte = (*rte & (1u << 16)) | vector | (low ? 1u << 13 : 0) | (level ? 1u << 15 : 0);
    return true;
}
