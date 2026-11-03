/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_VTD_H
#define VMM_AMD64_VTD_H
#include "ia64vmm/bootinfo.h"
#include "ia64vmm/pci.h"
#include "host/amd64/iommu.h"
#include "host/amd64/iommu_dma.h"

struct amd64_vtd_unit {
    uint64_t base, cap, ecap, root, domain[2];
    uint32_t aperture;
    unsigned levels, width, iotlb;
    uint64_t ir_table, iq, completion;
    unsigned iq_tail;
};
struct amd64_vtd {
    unsigned count;
    struct amd64_vtd_unit units[AMD64_IOMMU_MAX_UNITS];
    const struct amd64_iommu_dma_ops *ops;
    void *context;
    const char *error;
    /* The build-time memory map remains alive through interrupt setup. */
    const struct boot_info *memory;
    unsigned haw, source_count;
    struct {
        uint16_t bdf;
        uint8_t unit, secondary;
        bool bridge;
    } sources[PCI_MAX_DEVICES];
    struct {
        uint16_t sid;
        uint8_t unit;
        bool present;
    } ioapics[256];
    bool irq_ready, irq_failed, x2apic;
};
/* Build immutable identity domains before enabling any unit. Unknown sources
 * have no present context; private allocations never enter the guest domain. */
bool amd64_vtd_build(const uint8_t *, size_t, const struct boot_info *,
                     const struct pci_device *const *, unsigned, const struct amd64_iommu_dma_ops *,
                     void *, struct amd64_vtd *);
bool amd64_vtd_enable(struct amd64_vtd *);
/* Callers serialize access. Failure retains all hardware-visible memory. */
bool amd64_vtd_irq_init(struct amd64_vtd *, const uint8_t *, size_t, bool x2apic);
bool amd64_vtd_irq_msi(struct amd64_vtd *, uint16_t segment, uint16_t bdf, unsigned first,
                       unsigned count, uint32_t apic_id, uint64_t *, uint32_t *);
/* RTE is in/out: preserve its mask bit; set polarity and trigger from arguments. */
bool amd64_vtd_irq_ioapic(struct amd64_vtd *, unsigned madt_id, unsigned pin, unsigned vector,
                          uint32_t apic_id, bool low, bool level, uint64_t *);
bool amd64_vtd_irq_clear(struct amd64_vtd *, unsigned first, unsigned count);
#endif
