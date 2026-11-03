/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/vtd.h"
#include "ia64vmm/string.h"

#define PAGE 4096u
#define POLLS 1000000u
#define TE (1u << 31)
#define QIE (1u << 26)
#define IRE (1u << 25)
#define SIRTP (1u << 24)
#define CFI (1u << 23)

static bool fail(struct amd64_vtd *s, const char *error)
{
    s->error = error;
    s->irq_failed = true;
    return false;
}
static bool reject(struct amd64_vtd *s, const char *error)
{
    s->error = error;
    return false;
}
static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}
static bool wait_bits(struct amd64_vtd *s, unsigned u, unsigned off, unsigned width, uint64_t mask,
                      uint64_t expected)
{
    for (unsigned i = 0; i < POLLS; ++i) {
        uint64_t v = s->ops->read(s->context, u, off, width);
        if (v == (width == 4 ? UINT32_MAX : UINT64_MAX))
            return false;
        if ((v & mask) == expected)
            return true;
    }
    return false;
}
static uint64_t allocate(struct amd64_vtd *s)
{
    void *p = s->ops->allocate(s->context, 1);
    uint64_t pa;
    if (!p || !s->ops->physical(s->context, p, &pa) || (pa & (PAGE - 1)) ||
        pa >= (UINT64_C(1) << s->haw)) {
        fail(s, "VT-d interrupt table allocation unavailable/unaddressable");
        return 0;
    }
    if (!amd64_iommu_covered(s->memory, pa, pa + PAGE, BOOT_MEM_VMM)) {
        fail(s, "VT-d interrupt table allocation outside private RAM");
        return 0;
    }
    memset(p, 0, PAGE);
    return pa;
}
static int source(struct amd64_vtd *s, uint16_t bdf)
{
    for (unsigned i = 0; i < s->source_count; ++i)
        if (s->sources[i].bdf == bdf)
            return (int)i;
    return -1;
}
static bool scopes(struct amd64_vtd *s, const uint8_t *p, size_t length)
{
    if (!p || length < 48 || length > 1048576 || memcmp(p, "DMAR", 4) || le(p + 4, 4) != length ||
        !(p[37] & 1) || (s->x2apic && (p[37] & 2)))
        return fail(s, "DMAR does not permit interrupt remapping/APIC mode");
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i)
        sum += p[i];
    if (sum)
        return fail(s, "DMAR interrupt table checksum mismatch");
    unsigned unit = 0;
    for (size_t at = 48; at < length;) {
        const uint8_t *q = p + at;
        if (length - at < 4 || le(q + 2, 2) < 4 || le(q + 2, 2) > length - at)
            return fail(s, "malformed DMAR interrupt structure");
        unsigned size = le(q + 2, 2);
        if (!le(q, 2)) {
            if (size < 16 || unit >= s->count || le(q + 6, 2) ||
                le(q + 8, 8) != s->units[unit].base)
                return fail(s, "DMAR interrupt unit differs from DMA unit");
            for (unsigned off = 16; off < size;) {
                const uint8_t *v = q + off;
                if (size - off < 8 || v[1] < 8 || (v[1] & 1) || v[1] > size - off || v[2] || v[3] ||
                    v[0] < 1 || v[0] > 4)
                    return fail(s, "malformed DMAR interrupt source scope");
                if (v[0] == 3) {
                    unsigned bus = v[5];
                    uint16_t sid = 0;
                    for (unsigned path = 6; path < v[1]; path += 2) {
                        if (v[path] > 31 || v[path + 1] > 7)
                            return fail(s, "invalid DMAR interrupt PCI path");
                        sid = (uint16_t)((bus << 8) | (v[path] << 3) | v[path + 1]);
                        if (path + 2 < v[1]) {
                            int i = source(s, sid);
                            if (i < 0 || !s->sources[i].bridge || !s->sources[i].secondary)
                                return fail(s, "DMAR interrupt path bridge absent");
                            bus = s->sources[i].secondary;
                        }
                    }
                    unsigned id = v[4];
                    if (s->ioapics[id].present)
                        return fail(s, "ambiguous DMAR IOAPIC scope");
                    s->ioapics[id].present = true;
                    s->ioapics[id].sid = sid;
                    s->ioapics[id].unit = unit;
                }
                off += v[1];
            }
            ++unit;
        }
        at += size;
    }
    return unit == s->count || fail(s, "DMAR interrupt unit count differs from DMA unit count");
}
/* Rev.4.1 §§6.5.2.7–8: global IEC invalidation followed by a status-write wait. */
static bool invalidate(struct amd64_vtd *s, unsigned u)
{
    struct amd64_vtd_unit *unit = &s->units[u];
    uint64_t *queue = s->ops->pointer(s->context, unit->iq);
    volatile uint32_t *done = s->ops->pointer(s->context, unit->completion);
    unsigned tail = unit->iq_tail, wait = (tail + 16) & (PAGE - 1), next = (tail + 32) & (PAGE - 1);
    /* Every preceding submission completed before these ring slots are reused. */
    if (!wait_bits(s, u, 0x80, 8, UINT64_MAX, tail))
        return fail(s, "VT-d invalidation queue failed to drain");
    *done = 0;
    queue[tail / 8] = 4;
    queue[tail / 8 + 1] = 0;
    queue[wait / 8] = (UINT64_C(1) << 32) | (1u << 6) | (1u << 5) | 5;
    queue[wait / 8 + 1] = unit->completion;
    unit->iq_tail = next;
    s->ops->write(s->context, u, 0x88, next, 8);
    for (unsigned i = 0; i < POLLS; ++i) {
        if (*done == 1)
            return true;
        uint32_t fault = s->ops->read(s->context, u, 0x34, 4);
        if (fault & ((1u << 4) | (1u << 5) | (1u << 6)))
            return fail(s, "VT-d interrupt invalidation queue fault");
    }
    return fail(s, "VT-d interrupt invalidation completion timed out");
}
bool amd64_vtd_irq_init(struct amd64_vtd *s, const uint8_t *dmar, size_t length, bool x2apic)
{
    if (!s || s->error || !s->count || !s->memory || s->irq_ready)
        return false;
    s->x2apic = x2apic;
    if (!scopes(s, dmar, length))
        return false;
    for (unsigned u = 0; u < s->count; ++u) {
        struct amd64_vtd_unit *unit = &s->units[u];
        uint32_t status = s->ops->read(s->context, u, 0x1c, 4);
        if ((unit->ecap & 10) != 10 || (x2apic && !(unit->ecap & 16)) || status == UINT32_MAX ||
            (status & (TE | QIE | IRE)) != TE || !wait_bits(s, u, 0x28, 8, UINT64_C(1) << 63, 0) ||
            !wait_bits(s, u, unit->iotlb, 8, UINT64_C(1) << 63, 0))
            return fail(s, "VT-d interrupt remapping/QI unsupported or unit not ready");
    }
    for (unsigned u = 0; u < s->count; ++u) {
        struct amd64_vtd_unit *unit = &s->units[u];
        unit->ir_table = allocate(s);
        unit->iq = allocate(s);
        unit->completion = allocate(s);
        if (!unit->ir_table || !unit->iq || !unit->completion)
            return false;
        if (!(unit->ecap & 1))
            s->ops->flush(s->context, s->ops->pointer(s->context, unit->ir_table), PAGE);
    }
    for (unsigned u = 0; u < s->count; ++u) {
        struct amd64_vtd_unit *unit = &s->units[u];
        uint32_t command = TE;
        s->ops->write(s->context, u, 0x88, 0, 8);
        s->ops->write(s->context, u, 0x90, unit->iq, 8);
        command |= QIE;
        s->ops->write(s->context, u, 0x18, command, 4);
        if (!wait_bits(s, u, 0x1c, 4, QIE, QIE))
            return fail(s, "VT-d queued invalidation enable timed out");
        s->ops->write(s->context, u, 0xb8, unit->ir_table | 7 | (x2apic ? 0x800 : 0), 8);
        s->ops->write(s->context, u, 0x18, command | SIRTP, 4);
        if (!wait_bits(s, u, 0x1c, 4, SIRTP, SIRTP))
            return fail(s, "VT-d interrupt table pointer timed out");
        if (!invalidate(s, u))
            return false;
        s->ops->write(s->context, u, 0x18, command, 4);
        if (!wait_bits(s, u, 0x1c, 4, CFI, 0))
            return fail(s, "VT-d compatibility interrupts remain enabled");
        s->ops->write(s->context, u, 0x18, command | IRE, 4);
        if (!wait_bits(s, u, 0x1c, 4, IRE, IRE))
            return fail(s, "VT-d interrupt remapping enable timed out");
    }
    s->irq_ready = true;
    return true;
}
static bool range(unsigned first, unsigned count)
{
    return first >= 0x40 && first < 0xe0 && count && count <= 0xe0 - first;
}
static void flush_entries(struct amd64_vtd *s, unsigned u, unsigned first, unsigned count)
{
    if (!(s->units[u].ecap & 1))
        s->ops->flush(s->context,
                      (uint64_t *)s->ops->pointer(s->context, s->units[u].ir_table) + first * 2,
                      count * 16);
}
bool amd64_vtd_irq_clear(struct amd64_vtd *s, unsigned first, unsigned count)
{
    if (!s || s->irq_failed || !s->irq_ready)
        return false;
    s->error = NULL;
    if (!range(first, count))
        return reject(s, "invalid VT-d interrupt entry range");
    for (unsigned u = 0; u < s->count; ++u) {
        uint64_t *ir = s->ops->pointer(s->context, s->units[u].ir_table);
        for (unsigned i = first; i < first + count; ++i)
            ir[i * 2] &= ~UINT64_C(1);
        flush_entries(s, u, first, count);
        if (!invalidate(s, u))
            return false;
        memset(ir + first * 2, 0, count * 16);
        flush_entries(s, u, first, count);
    }
    return true;
}
static bool publish(struct amd64_vtd *s, unsigned u, uint16_t sid, unsigned first, unsigned count,
                    uint32_t apic, bool level)
{
    if ((!s->x2apic && apic > 255) || !range(first, count))
        return reject(s, "invalid VT-d interrupt destination/vector");
    if (!amd64_vtd_irq_clear(s, first, count))
        return false;
    uint64_t *ir = s->ops->pointer(s->context, s->units[u].ir_table);
    uint64_t destination = (uint64_t)apic << (s->x2apic ? 32 : 40);
    for (unsigned i = first; i < first + count; ++i) {
        ir[i * 2 + 1] = (UINT64_C(1) << 18) | sid; /* SVT=01, SQ=00. */
        ir[i * 2] = destination | ((uint64_t)i << 16) | (level ? 16 : 0);
    }
    /* Publish source validation before Present, including noncoherent walkers. */
    flush_entries(s, u, first, count);
    for (unsigned i = first; i < first + count; ++i)
        __atomic_store_n(&ir[i * 2], ir[i * 2] | 1, __ATOMIC_RELEASE);
    flush_entries(s, u, first, count);
    return invalidate(s, u);
}
bool amd64_vtd_irq_msi(struct amd64_vtd *s, uint16_t segment, uint16_t bdf, unsigned first,
                       unsigned count, uint32_t apic_id, uint64_t *address, uint32_t *data)
{
    if (!s || s->irq_failed || !s->irq_ready)
        return false;
    s->error = NULL;
    int i = source(s, bdf);
    if (segment || i < 0 || !address || !data || !count || count > 32 || (count & (count - 1)) ||
        (first & (count - 1)))
        return reject(s, "invalid/uncovered VT-d MSI source or message block");
    if (!publish(s, s->sources[i].unit, bdf, first, count, apic_id, false))
        return false;
    *address = UINT64_C(0xfee00018) | ((uint64_t)(first & 0x7fff) << 5) | ((first & 0x8000) >> 13);
    *data = 0;
    return true;
}
bool amd64_vtd_irq_ioapic(struct amd64_vtd *s, unsigned madt_id, unsigned pin, unsigned vector,
                          uint32_t apic_id, bool low, bool level, uint64_t *rte)
{
    if (!s || s->irq_failed || !s->irq_ready)
        return false;
    s->error = NULL;
    if (madt_id > 255 || pin > 255 || !rte || !s->ioapics[madt_id].present)
        return reject(s, "IOAPIC lacks an exact DMAR interrupt source scope");
    if (!publish(s, s->ioapics[madt_id].unit, s->ioapics[madt_id].sid, vector, 1, apic_id, level))
        return false;
    *rte = (*rte & (1u << 16)) | ((uint64_t)(vector & 0x7fff) << 49) | (UINT64_C(1) << 48) |
           ((vector & 0x8000) >> 4) | vector | (low ? 1u << 13 : 0) | (level ? 1u << 15 : 0);
    return true;
}
