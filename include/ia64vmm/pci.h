/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_PCI_H
#define IA64VMM_PCI_H
#include "ia64vmm/types.h"

#define PCI_MAX_DEVICES 256u
#define PCI_BAR_COUNT 7u /* Six ordinary BARs and the expansion ROM. */
/* Pure address helpers shared by the host backend and hosted tests. ECAM base
 * denotes the first bus's physical address, after the MCFG start-bus offset. */
struct pci_address {
    uint16_t segment;
    uint8_t bus, device, function;
};
struct pci_ecam_region {
    uint64_t base;
    uint16_t segment;
    uint8_t first, last;
};
bool pci_ecam_address(const struct pci_ecam_region *, struct pci_address, unsigned offset,
                      uint64_t *address);
uint32_t pci_cf8_address(struct pci_address, unsigned offset);
enum pci_owner { PCI_OWNER_GUEST, PCI_OWNER_VMM };
struct pci_bar {
    uint64_t base, size;
    uint32_t original[2], mask[2];
    uint16_t offset;
    bool io, wide, rom, prefetch, probe[2], rom_enabled;
    uint8_t *hva;
};
/* DeviceRecord. Bus/window/reset topology remains the host's;
 * parent is the upstream bridge's BDF, or 0xffff for a root function. */
struct pci_device {
    uint16_t bdf, vendor, device, parent;
    uint32_t class_code;
    enum pci_owner owner;
    const char *reservation;
    uint8_t header, irq_line, irq_pin, secondary, subordinate;
    uint32_t bridge_registers[9]; /* 0x18..0x38, host-assigned windows. */
    struct pci_bar bars[PCI_BAR_COUNT];
    uint8_t msi_offset, msi_length, msix_offset;
    uint8_t irq_config[256]; /* MSI/MSI-X requests; never host destinations. */
    uint64_t irq_writes, rejected_writes;
    uint16_t command;
    bool storage;
    /* The immediate upstream Root/Downstream Port has ARI Forwarding
     * enabled: the low eight RID bits identify functions of one device. */
    bool ari;
    /* Interrupt mediation state; allocated before guest execution. */
    struct pci_irq_state *interrupts;
    /* Restart: the command register and MSI/MSI-X
     * capability shadow when the first guest boot started, and whether the
     * guest set Bus Master Enable during the current guest boot. */
    uint16_t boot_command;
    bool guest_bus_master;
    uint8_t boot_irq_config[256];
};
static inline uint16_t pci_device_group(const struct pci_device *d)
{
    return d->bdf & (d->ari ? 0xff00u : 0xfff8u);
}
/* Single management CPU, before vCPUs start. The host supplies real config
 * space and UC mappings; hosted execution leaves this backend unset. */
struct pci_host_ops {
    uint32_t (*read)(void *, uint16_t bdf, unsigned offset, unsigned size);
    void (*write)(void *, uint16_t bdf, unsigned offset, uint32_t value, unsigned size);
    uint8_t *(*map)(void *, uint64_t base, uint64_t size);
};
void platform_pci_set_host(const struct pci_host_ops *ops, void *opaque);
void platform_pci_root_buses(unsigned first, unsigned last);
/* Copy the firmware-verified boot controller before inventory. Segment zero
 * EHCI/xHCI, NVMe and AHCI are supported. Inventory checks its BDF and class;
 * unset/vmm_media=auto reserves it. set_host clears the registration. */
bool platform_pci_boot_media(uint16_t segment, uint16_t bdf, uint32_t class_code);
/* The boot medium is served by a non-PCI backend. Set before inventory. */
void platform_pci_external_media(bool);
bool platform_pci_inventory(const char *cmdline);
/* Read-only controller discovery: no BAR probes, command or MSI writes. */
bool platform_pci_discover(void);
void platform_pci_list_controllers(void);
unsigned platform_pci_count(void);
const struct pci_device *platform_pci_device(unsigned index);
uint64_t platform_pci_read(uint64_t offset, unsigned size);
void platform_pci_write(uint64_t offset, uint64_t value, unsigned size);
struct platform_state;
struct ia64vmm_handoff;
bool platform_pci_map(struct platform_state *s);
void platform_pci_cleanup(void);
bool platform_pci_handoff(struct ia64vmm_handoff *h);
/* The same real UC mapper is used for fixed VGA resources as assigned BARs. */
uint8_t *platform_pci_map_physical(uint64_t base, uint64_t size);
/* Management CPU before the guest. The expansion ROM snapshot of a guest-owned function, taken once
 * and served to the guest later by platform_pci_map; NULL without a ROM. */
const uint8_t *platform_pci_rom(uint16_t bdf, uint64_t *size);
/* Enable I/O+memory decode on the function,
 * VGA Enable plus I/O+memory forwarding on every bridge above it, VGA
 * Enable cleared on every other bridge. Writes host config space. */
bool platform_pci_route_vga(uint16_t bdf);
/* Raw host configuration read for VMM diagnostics (never guest-reachable). */
uint32_t platform_pci_host_read(uint16_t bdf, unsigned offset, unsigned size);
/* Raw host configuration write for the PCI function reset (platform/pci_reset.c),
 * management CPU before the guest; never guest-reachable. */
void platform_pci_host_write(uint16_t bdf, unsigned offset, uint32_t value, unsigned size);
/* GPU reset: reset the function `bdf` the
 * way a machine without firmware drivers sees it at POST. Secondary Bus
 * Reset on its upstream bridge when only that device's functions are below
 * it, else Function Level Reset when the function advertises it, else
 * refused (nothing written). Every function reset is saved first and
 * restored after (ReBAR, BARs, ROM BAR, PCIe control, MSI/MSI-X, command
 * last). FAILED: a reset was asserted and a function did not come back as
 * saved, so the hardware no longer matches the inventory. */
enum pci_reset_result { PCI_RESET_DONE, PCI_RESET_REFUSED, PCI_RESET_FAILED };
enum pci_reset_result platform_pci_reset_function(uint16_t bdf);
/* Restart of the assigned functions, management
 * CPU with every executor stopped. Quiesce clears Bus Master Enable, sets
 * INTx Disable and disables MSI/MSI-X in host configuration space on each
 * guest-owned endpoint in scope, then reads its command register back.
 * reset_functions applies the restart to the functions (false: the run
 * ends). platform_pci_restart returns the mediation state of every
 * guest-owned function to its first-boot values, the command register
 * with Bus Master Enable clear except for an untouched display. */
void platform_pci_quiesce(void);
bool platform_pci_reset_functions(void);
/* The configuration reset_functions restores: every guest-owned function's
 * header, capabilities and listed extended capabilities as the host left
 * them, taken once when the first guest boot starts. */
void platform_pci_restart_snapshot(void);
void platform_pci_restart(void);
/* A display function the guest never gave Bus Master Enable during the
 * boot: no OS driver ran, so restart leaves it as it is. */
bool platform_pci_restart_untouched(const struct pci_device *);
#endif
