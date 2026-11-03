/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform/platform.h"
#include "ia64vmm/pci.h"
#include "ia64vmm/log.h"
#include "ia64vmm/hal.h"

static unsigned vga_lock;
static bool assigned;
static bool dispi;
static uint16_t kind;
static const struct pci_device *by_bdf(uint16_t bdf)
{
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *d = platform_pci_device(i);
        if (d->bdf == bdf)
            return d;
    }
    return NULL;
}
static bool routed(const struct pci_device *d)
{
    /* PCI Local Bus 3.0 §6.2.3: endpoint I/O+memory decode, and bridge
     * control VGA Enable on every ancestor, are real routing evidence. */
    if ((d->command & 3) != 3)
        return false;
    for (unsigned depth = 0; d->parent != 0xffff; ++depth) {
        if (depth == platform_pci_count())
            return false;
        d = by_bdf(d->parent);
        if (!d || d->owner != PCI_OWNER_GUEST || d->header != 1 || (d->command & 3) != 3 ||
            !(platform_pci_read(((uint64_t)d->bdf << 12) + 0x3e, 2) & 8))
            return false;
    }
    return true;
}
bool platform_vga_has_dispi(const struct pci_device *d)
{
    if (!d || d->owner != PCI_OWNER_GUEST || d->class_code != 0x030000)
        return false;
    /* QEMU standard-vga and VirtualBox VGA document fixed DISPI ports.
     * VMSVGA shares VMware's PCI identity, so its optional extension must
     * identify itself. Preserve the selector and never write display data. */
    if ((d->vendor == 0x1234 && d->device == 0x1111) ||
        (d->vendor == 0x80ee && d->device == 0xbeef))
        return true;
    if (d->vendor != 0x15ad || d->device != 0x0405 || !routed(d))
        return false;
    uint64_t flags = hal_irq_save();
    uint16_t saved = (uint16_t)hal_port_in(0x1ce, 2), id = 0xffff;
    if (saved != 0xffff) {
        hal_port_out(0x1ce, 0, 2);
        id = (uint16_t)hal_port_in(0x1cf, 2);
        hal_port_out(0x1ce, saved, 2);
    }
    hal_irq_restore(flags);
    return (id >= 0xb0c0 && id <= 0xb0c4) || (id >= 0xbe00 && id <= 0xbe03);
}
/* Linux include/drm/intel/pciids.h INTEL_SNB_IDS: Sandy Bridge (gen6) IGD. */
static bool intel_gen6(const struct pci_device *d)
{
    static const uint16_t ids[] = {0x0102, 0x0106, 0x010a, 0x0112, 0x0116, 0x0122, 0x0126};
    if (d->vendor != 0x8086 || d->bdf != (2u << 3) || d->parent != 0xffff)
        return false;
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
        if (d->device == ids[i])
            return true;
    return false;
}
/* The IGD is a root-bus endpoint; the host bridge decodes legacy VGA for it
 * unless GGC.IVD (bit 1) is set. Device 2 mirrors GGC read-only at 0x50
 * (Linux include/drm/intel/i915_drm.h SNB_GMCH_CTRL, INTEL_GMCH_VGA_DISABLE;
 * intel_vga.c intel_vga_decode_is_enabled). A GOP may leave the endpoint's
 * I/O decode off; i915 intel_vga_get() enables it by hand, as the firmware
 * does through the guest-owned command register. Memory decode carries the
 * inherited framebuffer and must already be on. */
static bool intel_gen6_routed(const struct pci_device *d, uint16_t *ggc)
{
    if (!intel_gen6(d))
        return false;
    *ggc = (uint16_t)platform_pci_read(((uint64_t)d->bdf << 12) + 0x50, 2);
    return (d->command & 2) && !(*ggc & 2);
}
/* VMX: the same rule for the
 * host-image path of display=vbios, which only reads GGC. */
bool platform_vga_intel_gen6(const struct pci_device *d)
{
    return intel_gen6(d);
}
bool platform_vga_intel_gen6_decodes(const struct pci_device *d, uint16_t *ggc)
{
    return intel_gen6_routed(d, ggc);
}
static bool platform_vga_handoff_unlocked(const struct boot_framebuffer *fb,
                                          struct ia64vmm_handoff *h)
{
    assigned = dispi = false;
    kind = IA64VMM_VGA_GENERIC;
    h->flags &= ~IA64VMM_HF_LEGACY_VGA;
    const struct pci_device *selected = NULL;
    for (unsigned i = 0; fb->size && i < platform_pci_count(); ++i) {
        const struct pci_device *d = platform_pci_device(i);
        uint16_t ggc = 0;
        if (d->owner != PCI_OWNER_GUEST || d->class_code != 0x030000)
            continue;
        if (!routed(d) && !intel_gen6_routed(d, &ggc)) {
            if (intel_gen6(d))
                LOGI("Intel gen6 IGD %04x:%04x command=%04x GGC=%04x: no legacy VGA decode",
                     d->vendor, d->device, d->command, ggc);
            continue;
        }
        for (unsigned b = 0; b < PCI_BAR_COUNT; ++b) {
            const struct pci_bar *bar = &d->bars[b];
            if (bar->io || bar->rom || !bar->size || fb->base < bar->base || fb->size > bar->size ||
                fb->base - bar->base > bar->size - fb->size)
                continue;
            if (selected && selected != d) {
                LOGE("ambiguous inherited GOP VGA device");
                return false;
            }
            selected = d;
        }
    }
    if (selected) {
        dispi = platform_vga_has_dispi(selected);
        if (selected->vendor == 0x1234 && selected->device == 0x1111)
            kind = IA64VMM_VGA_BOCHS;
        if (selected->vendor == 0x15ad && selected->device == 0x0405)
            kind = IA64VMM_VGA_VMWARE;
        /* GTTMMADR (BAR0) holds the display registers up to PCH_LVDS 0xe1180
         * (Intel SNB PRM Vol 1 Part 1 §6.1.2, Vol 3 Part 3 §3.7.1). */
        const struct pci_bar *mmio = &selected->bars[0];
        if (intel_gen6(selected)) {
            if (!mmio->io && !mmio->rom && mmio->size >= 0x100000)
                kind = IA64VMM_VGA_INTEL_GEN6;
            else
                LOGE("Intel gen6 IGD without a usable GTTMMADR BAR0 (size %llx)",
                     (unsigned long long)mmio->size);
        }
        h->flags |= IA64VMM_HF_LEGACY_VGA;
        h->vga_base = IA64VMM_VGA_MEMORY_BASE;
        h->vga_size = IA64VMM_VGA_MEMORY_SIZE;
        h->vga_bdf = selected->bdf;
        h->vga_vendor = selected->vendor;
        h->vga_device = selected->device;
        h->vga_kind = kind;
        /* Bochs BAR2 (flat VBE), VMware BAR0 (I/O index), Intel BAR0 (MMIO). */
        unsigned control = kind == IA64VMM_VGA_BOCHS ? 2 : 0;
        if (kind != IA64VMM_VGA_GENERIC) {
            h->vga_control_base = selected->bars[control].base;
            h->vga_control_size = selected->bars[control].size;
        }
        assigned = true;
        LOGI(
            "legacy VGA assigned %02x:%02x.%u %04x:%04x memory=a0000..bffff kind=%u command=%04x control=%llx/%llx dispi=%u",
            selected->bdf >> 8, (selected->bdf >> 3) & 31, selected->bdf & 7, selected->vendor,
            selected->device, kind, selected->command, (unsigned long long)h->vga_control_base,
            (unsigned long long)h->vga_control_size, dispi);
    } else
        LOGI("inherited GOP has no verified legacy VGA decode");
    platform_debug_display_policy(h);
    return true;
}
bool platform_vga_handoff(const struct boot_framebuffer *fb, struct ia64vmm_handoff *h)
{
    platform_lock(&vga_lock);
    bool result = platform_vga_handoff_unlocked(fb, h);
    platform_unlock(&vga_lock);
    return result;
}
static bool platform_vga_map_unlocked(struct platform_state *s)
{
    if (!assigned)
        return true;
    uint8_t *memory = platform_pci_map_physical(IA64VMM_VGA_MEMORY_BASE, IA64VMM_VGA_MEMORY_SIZE);
    const struct gm_region region = {IA64VMM_VGA_MEMORY_BASE,
                                     IA64VMM_VGA_MEMORY_SIZE,
                                     GM_DIRECT_MMIO,
                                     "assigned-legacy-vga",
                                     memory,
                                     NULL,
                                     NULL};
    if (!memory || !gm_add_region(&region) || !platform_allow_ports(s, 0x3b0, 0x30))
        return false;
    /* Allow the byte footprint of word I/O at the real x86 index/data
     * ports (1ce/1cf). The fork's independent 1d0 alias is not assumed. */
    if (dispi && !platform_allow_ports(s, 0x1ce, 3))
        return false;
    return true;
}
bool platform_vga_map(struct platform_state *s)
{
    platform_lock(&vga_lock);
    bool result = platform_vga_map_unlocked(s);
    platform_unlock(&vga_lock);
    return result;
}
