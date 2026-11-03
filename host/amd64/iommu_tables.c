/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Boot/Acpi/{AcpiTableParser,Ivrs}.cpp,
 * Source/Platform/Device/VtdUnit.cpp, Source/Platform/Amd64/Device/AmdViUnit.cpp
 */
#include "host/amd64/iommu.h"
#include "ia64vmm/string.h"

static uint64_t little(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}
/* VT-d §8 (DRHD) / AMD 48882 §5.2 (IVHD): locate remappers for handoff
 * and MMIO mapping. DMA backends interpret scopes and reserved memory. */
bool amd64_iommu_parse(const uint8_t *p, size_t len, struct amd64_iommu_table *out)
{
    struct amd64_iommu_table result = {0};
    if (!p || !out || len < 48 || len > 1048576 || little(p + 4, 4) != len)
        return false;
    bool amd = !memcmp(p, "IVRS", 4);
    if (!amd && memcmp(p, "DMAR", 4))
        return false;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i)
        sum += p[i];
    if (sum)
        return false;
    for (size_t off = 48; off < len;) {
        if (len - off < 4)
            return false;
        const uint8_t *b = p + off;
        unsigned size = little(b + 2, 2), type = amd ? b[0] : little(b, 2);
        if (size < 4 || size > len - off)
            return false;
        bool unit = amd ? type == 0x10 || type == 0x11 || type == 0x40 : type == 0;
        if (unit) {
            unsigned minimum = amd ? (type == 0x10 ? 24 : 40) : 16;
            if (size < minimum)
                return false;
            struct amd64_iommu_unit u = {
                .base = little(b + 8, 8),
                .segment = little(b + (amd ? 16 : 6), 2),
                .size = amd ? (((type == 0x10 ? little(b + 20, 4) & UINT64_C(0x7fe000)
                                              : little(b + 24, 8) & (UINT64_C(1) << 9)))
                                   ? 0x80000
                                   : 0x4000)
                            : 1u << (12 + (b[5] & 15)),
                .bdf = amd ? little(b + 4, 2) : 0,
                .capability = amd ? little(b + 6, 2) : 0,
                .kind = amd ? AMD64_AMDVI : AMD64_VTD,
            };
            if (!u.base || (u.base & 4095) || u.base > (UINT64_C(1) << 50) - u.size ||
                (u.base & (u.size - 1)) || (!amd && (b[5] & 0xf0)))
                return false;
            bool duplicate = false;
            for (unsigned i = 0; i < result.count; ++i) {
                const struct amd64_iommu_unit *old = &result.units[i];
                if (old->base != u.base) {
                    if (u.base < old->base + old->size && old->base < u.base + u.size)
                        return false;
                    continue;
                }
                if (!amd || old->segment != u.segment || old->bdf != u.bdf || old->size != u.size ||
                    old->capability != u.capability)
                    return false;
                duplicate = true; /* alternate IVHD encodings of the same unit */
            }
            if (!duplicate) {
                if (result.count == AMD64_IOMMU_MAX_UNITS)
                    return false;
                result.units[result.count++] = u;
            }
        } else if (amd && (type < 0x20 || type > 0x22)) {
            /* An unknown IVHD format might describe another translating unit. */
            return false;
        }
        off += size;
    }
    *out = result;
    return true;
}

static bool wait_register(const struct amd64_iommu_ops *ops, void *ctx, unsigned off,
                          unsigned width, uint64_t mask, uint64_t expected)
{
    for (unsigned i = 0; i < 1000000; ++i) {
        uint64_t value = ops->read(ctx, off, width);
        if (value == (width == 4 ? UINT32_MAX : UINT64_MAX))
            return false;
        if ((value & mask) == expected)
            return true;
    }
    return false;
}

bool amd64_iommu_disable(enum amd64_iommu_kind kind, const struct amd64_iommu_ops *ops, void *ctx)
{
    if (kind == AMD64_VTD) {
        uint32_t status = ops->read(ctx, 0x1c, 4);
        if (status == UINT32_MAX || (status & (3u << 25)))
            return false;
        /* §6.9: do not issue GCMD over a pending firmware command. An
         * inherited IQ cannot be safely quiesced without its wait descriptor. */
        if (!wait_register(ops, ctx, 0x1c, 4, 1u << 27, 0))
            return false;
        uint32_t pmen = ops->read(ctx, 0x64, 4);
        if (pmen == UINT32_MAX || !wait_register(ops, ctx, 0x64, 4, 1, pmen >> 31))
            return false;
        uint64_t ecap = ops->read(ctx, 0x10, 8);
        unsigned iotlb = (unsigned)((ecap >> 8) & 1023) * 16 + 8;
        if (ecap == UINT64_MAX || iotlb < 0x70 ||
            !wait_register(ops, ctx, 0x28, 8, UINT64_C(1) << 63, 0) ||
            !wait_register(ops, ctx, iotlb, 8, UINT64_C(1) << 63, 0))
            return false;
        status = ops->read(ctx, 0x1c, 4);
        if (status == UINT32_MAX || (status & (3u << 25)))
            return false;
        /* VT-d §11.4.4.1: GCMD reads are undefined. Derive persistent bits
         * from GSTS, clear one-shot fields, clear TE, then await TES=0. */
        if (status & (1u << 31))
            ops->write(ctx, 0x18, (status & 0x96ffffffu) & ~(1u << 31), 4);
        bool off = false;
        for (unsigned i = 0; i < 1000000 && !off; ++i)
            off = !(ops->read(ctx, 0x1c, 4) & (1u << 31));
        if (!off)
            return false;
        /* Pre-boot DMA protection may use protected memory regions instead of
         * (or besides) translation, and PMRs block DMA even with TE clear.
         * VT-d rev.4.1 §11.4.8.1 PMEN_REG (064h): EPM bit 31, PRS bit 0; it reads 0
         * when CAP_REG.PLMR/PHMR are not supported. */
        pmen = ops->read(ctx, 0x64, 4);
        if (pmen == UINT32_MAX)
            return false;
        if (pmen & (1u << 31))
            ops->write(ctx, 0x64, pmen & ~(1u << 31), 4);
        return wait_register(ops, ctx, 0x64, 4, 1, 0);
    }
    /* AMD 48882 §2.4 / Control (0018h): stop translation, stop firmware
     * engines, then wait for CmdBuf/Event/PPR running status to clear.
     * Preserve SMI filters, timeout and reserved bits; no W1C status write. */
    uint64_t control = ops->read(ctx, 0x18, 8);
    if (control == UINT64_MAX)
        return false;
    const uint64_t engines = UINT64_C(0x3f01f) | (UINT64_C(1) << 25) | (UINT64_C(3) << 28) |
                             (UINT64_C(3) << 43) | (UINT64_C(1) << 45) | (UINT64_C(3) << 50) |
                             (UINT64_C(1) << 58) | (UINT64_C(1) << 61);
    ops->write(ctx, 0x18, control & ~UINT64_C(1), 8);
    ops->write(ctx, 0x18, (control & ~UINT64_C(1)) & ~engines, 8);
    for (unsigned i = 0; i < 1000000; ++i)
        if (!(ops->read(ctx, 0x18, 8) & 1) && !(ops->read(ctx, 0x2020, 8) & 0x198))
            return true;
    return false;
}
