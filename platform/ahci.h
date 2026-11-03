/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_PLATFORM_AHCI_H
#define IA64VMM_PLATFORM_AHCI_H
#include "ia64vmm/block.h"
#include "ia64vmm/guest_mem.h"

#define PLATFORM_AHCI_SIZE 0x2000u
#define PLATFORM_AHCI_MAX_PORTS 32u
/* Lifecycle calls run with guest executors stopped. The caller reserves a
 * free PCI function, registers this MMIO BAR, and supplies its IRQ route.
 * notify wakes the management CPU when guest work arrives. */
bool platform_ahci_init(const struct platform_block *, unsigned count, uint64_t base,
                        unsigned irq_line, void (*irq)(void *, bool), void *irq_context,
                        void (*notify)(void *), void *notify_context);
void platform_ahci_cleanup(void);
void platform_ahci_reset(void);
bool platform_ahci_present(void);
uint64_t platform_ahci_cfg_read(unsigned offset, unsigned size);
void platform_ahci_cfg_write(unsigned offset, uint64_t value, unsigned size);
extern const struct gm_mmio_ops platform_ahci_ops;
/* Management CPU only; runs pending commands serially without holding the
 * device lock across backend I/O. Also call while stopping executors. */
void platform_ahci_poll(void);
#endif
