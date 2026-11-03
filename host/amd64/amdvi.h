/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_AMDVI_H
#define VMM_AMD64_AMDVI_H
#include "host/amd64/iommu.h"
#include "host/amd64/iommu_dma.h"
struct amd64_amdvi_unit {
    uint64_t base, efr, control, firmware_control, table, domain[3], command, event, completion;
    unsigned levels, width, aperture, tail, sequence;
    uint16_t bdf, capability;
};
struct amd64_amdvi_irq_source {
    uint64_t table;
    uint16_t bdf;
    uint8_t unit;
    bool msi;
};
struct amd64_amdvi {
    unsigned count;
    struct amd64_amdvi_unit units[AMD64_IOMMU_MAX_UNITS];
    const struct amd64_iommu_dma_ops *ops;
    void *context;
    const char *error;
    const struct boot_info *memory;
    unsigned haw, source_count;
    struct amd64_amdvi_irq_source sources[PCI_MAX_DEVICES + 256];
    struct {
        bool present;
        unsigned source;
    } ioapics[256];
    bool enabled, irq_ready, irq_failed, x2apic;
};
/* Enable/lock the firmware-declared PCI MMIO aperture before MMIO access. */
bool amd64_amdvi_prepare(const struct amd64_iommu_unit *, const struct amd64_iommu_dma_ops *,
                         void *);
bool amd64_amdvi_build(const uint8_t *, size_t, const struct boot_info *,
                       const struct pci_device *const *, unsigned,
                       const struct amd64_iommu_dma_ops *, void *, struct amd64_amdvi *);
bool amd64_amdvi_enable(struct amd64_amdvi *);
bool amd64_amdvi_irq_init(struct amd64_amdvi *, bool x2apic);
bool amd64_amdvi_irq_clear(struct amd64_amdvi *, unsigned first, unsigned count);
bool amd64_amdvi_irq_msi(struct amd64_amdvi *, uint16_t segment, uint16_t bdf, unsigned first,
                         unsigned count, uint32_t apic_id, uint64_t *, uint32_t *);
bool amd64_amdvi_irq_ioapic(struct amd64_amdvi *, unsigned id, unsigned pin, unsigned vector,
                            uint32_t apic_id, bool low, bool level, uint64_t *);
#endif
