/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_IOMMU_H
#define VMM_AMD64_IOMMU_H
#include "ia64vmm/types.h"
#define AMD64_IOMMU_MAX_UNITS 32u
enum amd64_iommu_kind { AMD64_VTD, AMD64_AMDVI };
struct amd64_iommu_unit {
    uint64_t base;
    uint32_t size;
    uint16_t segment, bdf, capability;
    enum amd64_iommu_kind kind;
};
struct amd64_iommu_table {
    unsigned count;
    struct amd64_iommu_unit units[AMD64_IOMMU_MAX_UNITS];
};
bool amd64_iommu_parse(const uint8_t *table, size_t length, struct amd64_iommu_table *out);
struct amd64_iommu_ops {
    uint64_t (*read)(void *, unsigned, unsigned);
    void (*write)(void *, unsigned, uint64_t, unsigned);
};
/* False means firmware state cannot safely be taken over; guest must not run. */
bool amd64_iommu_disable(enum amd64_iommu_kind kind, const struct amd64_iommu_ops *, void *);
bool amd64_iommu_irq_enabled(void);
bool amd64_iommu_ioapic(unsigned id, unsigned pin, unsigned cpu, unsigned vector, bool low,
                        bool level, uint64_t *rte);
#endif
