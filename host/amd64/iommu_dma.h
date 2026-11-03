/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_IOMMU_DMA_H
#define VMM_AMD64_IOMMU_DMA_H
#include "ia64vmm/bootinfo.h"
#include "ia64vmm/pci.h"
struct amd64_iommu_dma_ops {
    void *(*allocate)(void *, unsigned); /* Contiguous private pages; retained. */
    bool (*physical)(void *, const void *, uint64_t *);
    void *(*pointer)(void *, uint64_t);
    void (*flush)(void *, const void *, size_t);
    uint32_t (*config)(void *, uint16_t, unsigned, unsigned);
    uint64_t (*read)(void *, unsigned, unsigned, unsigned);
    /* Each MMIO write completes with a full CPU memory fence. */
    void (*write)(void *, unsigned, unsigned, uint64_t, unsigned);
    void (*config_write)(void *, uint16_t, unsigned, uint32_t, unsigned);
};
const char *amd64_iommu_topology(const struct pci_device *const *, unsigned,
                                 const struct amd64_iommu_dma_ops *, void *);
bool amd64_iommu_covered(const struct boot_info *, uint64_t, uint64_t, enum boot_mem_type);
const char *amd64_iommu_memory(const struct boot_info *);
#endif
