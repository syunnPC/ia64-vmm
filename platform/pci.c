/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Kernel/Device/{PciEcamAddress.hpp,PciEndpointBarDecoder.cpp,PciBarProbe.cpp,PciFunctionCatalog.cpp},
 * Source/Platform/Device/PortPciConfigurationSpace.cpp
 */
#include "ia64vmm/pci.h"
#include "ia64vmm/interrupts.h"
#include "platform/platform.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/log.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"
#include "platform/storage.h"
#include "platform/ahci.h"
#include "platform/i8042.h"

static const struct pci_host_ops *host;
static void *host_context;
static struct pci_device devices[PCI_MAX_DEVICES];
static uint16_t lookup[65536]; /* Index + 1; zero means absent. */
static unsigned count;
static unsigned bus_first, bus_last = 255;
static bool inventoried, discovered, external_media;
static bool boot_media_valid;
static uint16_t boot_media_bdf;
static uint32_t boot_media_class;
static uint64_t rom_bases[PCI_MAX_DEVICES];
static unsigned rom_regions;
static struct boot_framebuffer boot_fb;
void platform_pci_boot_framebuffer(const struct boot_framebuffer *fb)
{
    boot_fb = *fb;
}
uint8_t *platform_pci_map_physical(uint64_t base, uint64_t size)
{
    return host && host->map ? host->map(host_context, base, size) : NULL;
}

/* Address calculations follow UEFI PI 1.7 Vol.5 §7.3.1 and PCI Local Bus 3.0 §6.1. */
bool pci_ecam_address(const struct pci_ecam_region *r, struct pci_address a, unsigned off,
                      uint64_t *address)
{
    if ((r->base & 0xfffff) || r->first > r->last)
        return false;
    uint64_t last = (((uint64_t)r->last - r->first + 1) << 20) - 1;
    if (r->base > UINT64_MAX - last || a.device > 31 || a.function > 7 || a.segment != r->segment ||
        a.bus < r->first || a.bus > r->last || off > 0xffc || (off & 3))
        return false;
    uint64_t delta = ((uint64_t)(a.bus - r->first) << 20) | ((uint64_t)a.device << 15) |
                     ((uint64_t)a.function << 12) | off;
    if (r->base > UINT64_MAX - delta)
        return false;
    *address = r->base + delta;
    return true;
}
uint32_t pci_cf8_address(struct pci_address a, unsigned off)
{
    if (a.segment || a.device > 31 || a.function > 7 || off >= 256 || (off & 3))
        return 0;
    return 0x80000000u | ((uint32_t)a.bus << 16) | ((uint32_t)a.device << 11) |
           ((uint32_t)a.function << 8) | off;
}

void platform_pci_set_host(const struct pci_host_ops *ops, void *opaque)
{
    host = ops;
    host_context = opaque;
    count = 0;
    inventoried = discovered = external_media = false;
    rom_regions = 0;
    bus_first = 0;
    bus_last = 255;
    boot_media_valid = false;
    boot_media_bdf = 0;
    boot_media_class = 0;
    memset(&boot_fb, 0, sizeof(boot_fb));
    memset(lookup, 0, sizeof(lookup));
}
void platform_pci_root_buses(unsigned first, unsigned last)
{
    if (first <= last && last <= 255) {
        bus_first = first;
        bus_last = last;
    }
}
bool platform_pci_boot_media(uint16_t segment, uint16_t bdf, uint32_t class_code)
{
    if (inventoried)
        return false;
    boot_media_valid = false;
    if (segment || (class_code != 0x0c0320 && class_code != 0x0c0330 && class_code != 0x010802 &&
                    class_code != 0x010601))
        return false;
    boot_media_bdf = bdf;
    boot_media_class = class_code;
    boot_media_valid = true;
    return true;
}
void platform_pci_external_media(bool enabled)
{
    external_media = enabled;
}
unsigned platform_pci_count(void)
{
    return count;
}
const struct pci_device *platform_pci_device(unsigned i)
{
    return i < count ? &devices[i] : NULL;
}
static uint32_t read_cfg(struct pci_device *d, unsigned off, unsigned size)
{
    return host->read(host_context, d->bdf, off, size);
}
static void write_cfg(struct pci_device *d, unsigned off, uint32_t value, unsigned size)
{
    host->write(host_context, d->bdf, off, value, size);
}
static int hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static bool media_bdf(const char *line, uint16_t *bdf)
{
    const char *p;
    size_t n;
    if (!platform_cmdline_value(line, "vmm_media", &p, &n) || n != 7 || p[2] != ':' ||
        p[5] != '.' || hex(p[0]) < 0 || hex(p[1]) < 0 || hex(p[3]) < 0 || hex(p[4]) < 0 ||
        hex(p[6]) < 0)
        return false;
    unsigned bus = hex(p[0]) * 16 + hex(p[1]);
    unsigned dev = hex(p[3]) * 16 + hex(p[4]), fn = hex(p[6]);
    if (dev > 31 || fn > 7)
        return false;
    *bdf = (bus << 8) | (dev << 3) | fn;
    return true;
}
static void size_bar(struct pci_device *d, unsigned slot, unsigned off, bool rom, unsigned last)
{
    struct pci_bar *b = &d->bars[slot];
    b->offset = off;
    b->rom = rom;
    b->original[0] = read_cfg(d, off, 4);
    b->rom_enabled = rom && (b->original[0] & 1);
    b->io = !rom && (b->original[0] & 1);
    bool wide = !rom && !b->io && (b->original[0] & 6) == 4;
    /* Reject reserved
     * types and truncated pairs instead of treating them as 32-bit memory. */
    if (!rom && ((b->io && (b->original[0] & 2)) || (!b->io && (b->original[0] & 6) && !wide) ||
                 (wide && slot + 1 >= last)))
        return;
    b->wide = wide;
    b->prefetch = !rom && !b->io && (b->original[0] & 8);
    if (b->wide)
        b->original[1] = read_cfg(d, off + 4, 4);
    /* PCI Local Bus §6.2.5: decode is disabled for the entire 64-bit probe.
     * Probing happens once before guest execution, then every value is restored. */
    write_cfg(d, off, rom ? 0xfffffffeu : UINT32_MAX, 4);
    if (b->wide)
        write_cfg(d, off + 4, UINT32_MAX, 4);
    b->mask[0] = read_cfg(d, off, 4);
    if (b->wide)
        b->mask[1] = read_cfg(d, off + 4, 4);
    write_cfg(d, off, b->original[0], 4);
    if (b->wide)
        write_cfg(d, off + 4, b->original[1], 4);
    uint32_t bits = rom ? ~0x7ffu : b->io ? ~3u : ~15u;
    uint64_t mask = b->mask[0] & bits;
    /* PCI Local Bus §6.2.5.1: an I/O BAR of a device that decodes only 16
     * address bits reads 0 in bits 31:16 (an IGD BAR4: 0000ffc1 for
     * 64 ports); its size comes from the low 16 bits. */
    if (b->io && !(b->mask[0] >> 16))
        mask |= 0xffff0000u;
    b->base = b->original[0] & bits;
    if (b->wide) {
        mask |= (uint64_t)b->mask[1] << 32;
        b->base |= (uint64_t)b->original[1] << 32;
        b->size = 0 - mask;
    } else
        b->size = mask ? (uint64_t)(~(uint32_t)mask) + 1 : 0;
    /* Extent checks: power-of-two sizing, assignment alignment, and end-address overflow.
     * This pre-guest inventory quiesces BME too and extends the port to I/O,
     * bridge and ROM BARs. Word command writes preserve W1C status bits. */
    if ((!rom && !b->base) || !b->size || (b->size & (b->size - 1)) ||
        b->size < (rom     ? 2048u
                   : b->io ? 4u
                           : 16u) ||
        (b->base & (b->size - 1)) || b->base > UINT64_MAX - b->size)
        b->size = 0;
    if (rom)
        b->mask[0] &= ~1u;
}
static void capabilities(struct pci_device *d)
{
    if (!(read_cfg(d, 6, 2) & 0x10))
        return;
    bool seen[256] = {0};
    unsigned p = read_cfg(d, 0x34, 1) & ~3u;
    while (p >= 0x40 && p <= 0xfc && !seen[p]) {
        seen[p] = true;
        unsigned id = read_cfg(d, p, 1), ctrl = read_cfg(d, p + 2, 2);
        unsigned n = id == 5      ? ((ctrl & 0x80) ? 14 : 10) + ((ctrl & 0x100) ? 10 : 0)
                     : id == 0x11 ? 12
                                  : 0;
        if (n && p + n <= 256) {
            for (unsigned i = 0; i < n; ++i)
                d->irq_config[p + i] = read_cfg(d, p + i, 1);
            if (id == 5) {
                d->msi_offset = p;
                d->msi_length = n;
            } else
                d->msix_offset = p;
            if (d->owner == PCI_OWNER_GUEST) {
                ctrl = id == 5 ? ctrl & ~1u : (ctrl & ~0x8000u) | 0x4000u;
                d->irq_config[p + 2] = ctrl;
                d->irq_config[p + 3] = ctrl >> 8;
                write_cfg(d, p + 2, ctrl, 2);
            }
        }
        p = read_cfg(d, p + 1, 1) & ~3u;
    }
}
/* PCIe Base 2.1 §§6.13, 7.8.16, 7.8.17: forwarding changes only the
 * immediate secondary bus of a Root Port or Switch Downstream Port.
 * Leave firmware's live routing mode intact. */
static bool ari_forwarding(struct pci_device *d, bool *enabled)
{
    *enabled = false;
    if (d->header != 1 || !(read_cfg(d, 6, 2) & 0x10))
        return true;
    bool seen[256] = {0};
    for (unsigned p = read_cfg(d, 0x34, 1) & ~3u; p;) {
        if (p < 0x40 || p > 0xfc || seen[p])
            return false;
        seen[p] = true;
        if (read_cfg(d, p, 1) == 0x10) {
            unsigned cap = read_cfg(d, p + 2, 2), type = (cap >> 4) & 15;
            if ((type == 4 || type == 6) && (cap & 15) >= 2) {
                if (p > 0xd4)
                    return false;
                *enabled = (read_cfg(d, p + 0x28, 2) & 0x20) != 0;
                if (*enabled && !(read_cfg(d, p + 0x24, 4) & 0x20))
                    return false;
            }
        }
        p = read_cfg(d, p + 1, 1) & ~3u;
    }
    return true;
}
bool platform_pci_discover(void)
{
    if (!host || discovered)
        return true;
    count = 0;
    memset(lookup, 0, sizeof(lookup));
    bool ari_bus[256] = {0};
    /* Bridge bus numbers ascend. On a bus with ARI forwarding, scan all
     * eight RID function bits independently of Header Type.MF (ARI does
     * not require that bit). Ordinary buses keep the conventional walk.
     * The ECAM/RID encoding itself is identical in either mode. */
    for (unsigned bus = bus_first; bus <= bus_last; ++bus)
        for (unsigned dev = 0; dev < 32; ++dev) {
            unsigned functions = ari_bus[bus] ? 8 : 1;
            for (unsigned fn = 0; fn < functions; ++fn) {
                uint16_t bdf = (bus << 8) | (dev << 3) | fn;
                uint32_t id = host->read(host_context, bdf, 0, 4);
                if ((id & 0xffff) == 0xffff || !(id & 0xffff))
                    continue;
                if (count == PCI_MAX_DEVICES) {
                    LOGE("PCI device table full");
                    return false;
                }
                struct pci_device *d = &devices[count++];
                memset(d, 0, sizeof(*d));
                lookup[bdf] = count;
                d->bdf = bdf;
                d->ari = ari_bus[bus];
                d->vendor = id;
                d->device = id >> 16;
                d->parent = 0xffff;
                d->class_code = read_cfg(d, 8, 4) >> 8;
                d->storage = (d->class_code >> 16) == 1;
                unsigned header = (read_cfg(d, 0x0c, 4) >> 16) & 255;
                if (!fn && (header & 0x80))
                    functions = 8;
                d->header = header & 0x7f;
                d->irq_line = read_cfg(d, 0x3c, 1);
                d->irq_pin = read_cfg(d, 0x3d, 1);
                if (d->header == 1) {
                    for (unsigned i = 0; i < 9; ++i)
                        d->bridge_registers[i] = read_cfg(d, 0x18 + i * 4, 4);
                    d->secondary = d->bridge_registers[0] >> 8;
                    d->subordinate = d->bridge_registers[0] >> 16;
                    bool enabled;
                    if (!ari_forwarding(d, &enabled) ||
                        (enabled && (d->secondary <= bus || d->secondary > d->subordinate))) {
                        LOGE("Invalid PCIe ARI forwarding configuration at %04x", bdf);
                        return false;
                    }
                    if (enabled)
                        ari_bus[d->secondary] = true;
                }
            }
        }
    for (unsigned i = 0; i < count; ++i) {
        struct pci_device *d = &devices[i];
        for (unsigned j = 0; j < count; ++j) {
            struct pci_device *bridge = &devices[j];
            unsigned bus = d->bdf >> 8;
            if (bridge != d && bridge->header == 1 && bridge->secondary &&
                (bridge->bdf >> 8) < bridge->secondary && bus >= bridge->secondary &&
                bus <= bridge->subordinate &&
                (d->parent == 0xffff ||
                 bridge->secondary > devices[lookup[d->parent] - 1].secondary))
                d->parent = bridge->bdf;
        }
    }
    discovered = true;
    LOGI("PCI discovery functions=%u/%u", count, PCI_MAX_DEVICES);
    return true;
}
void platform_pci_list_controllers(void)
{
    LOGI("Storage/USB controllers: auto reserves the firmware boot-source controller");
    for (unsigned i = 0; i < count; ++i) {
        const struct pci_device *d = &devices[i];
        if (!d->storage && d->class_code >> 8 != 0x0c03)
            continue;
        LOGI("controller %02x:%02x.%u %04x:%04x class=%06x", d->bdf >> 8, (d->bdf >> 3) & 31,
             d->bdf & 7, d->vendor, d->device, d->class_code);
        uint16_t parent = d->parent;
        for (unsigned depth = 0; parent != 0xffff; ++depth) {
            if (depth == count || !lookup[parent]) {
                LOGE("invalid/cyclic PCI bridge ancestry");
                break;
            }
            const struct pci_device *bridge = &devices[lookup[parent] - 1];
            LOGI("  via %02x:%02x.%u buses=%02x..%02x", parent >> 8, (parent >> 3) & 31, parent & 7,
                 bridge->secondary, bridge->subordinate);
            parent = bridge->parent;
        }
        if (d->parent == 0xffff)
            LOGI("  root bus %02x", d->bdf >> 8);
    }
    LOGI(
        "Unset/vmm_media=auto uses the verified boot source; guest media needs another controller");
}
bool platform_pci_inventory(const char *cmdline)
{
    if (!host || inventoried)
        return true;
    uint16_t media = 0;
    bool automatic = false;
    if (!external_media) {
        const char *p;
        size_t n;
        automatic = !platform_cmdline_value(cmdline, "vmm_media", &p, &n) ||
                    (n == 4 && !memcmp(p, "auto", 4));
        if (automatic) {
            if (!boot_media_valid) {
                LOGE("PCI auto media requires a verified firmware boot-source controller");
                return false;
            }
            media = boot_media_bdf;
        } else if (!media_bdf(cmdline, &media)) {
            LOGE("Invalid vmm_media: expected auto or BB:DD.F");
            return false;
        }
    }
    if (!platform_pci_discover())
        return false;
    if (automatic &&
        (!lookup[media] || devices[lookup[media] - 1].class_code != boot_media_class)) {
        LOGE("Firmware boot-source BDF/class does not match PCI discovery");
        return false;
    }
    if (!external_media &&
        (!lookup[media] || !(devices[lookup[media] - 1].storage ||
                             devices[lookup[media] - 1].class_code >> 8 == 0x0c03))) {
        LOGE("vmm_media BDF absent or not a storage/USB controller");
        return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        struct pci_device *d = &devices[i];
        unsigned cls = d->class_code >> 8;
        if (!external_media && d->bdf == media)
            d->reservation = "vmm-media";
        else if (cls == 0x0600 || cls == 0x0800 || cls == 0x0806)
            d->reservation = "host-system";
        d->owner = d->reservation ? PCI_OWNER_VMM : PCI_OWNER_GUEST;
        d->command = read_cfg(d, 4, 2);
        unsigned bars = d->header == 0 ? 6 : d->header == 1 ? 2 : 0;
        if (bars) {
            write_cfg(d, 4, d->command & ~7u, 2);
            for (unsigned b = 0; b < bars; ++b) {
                size_bar(d, b, 0x10 + b * 4, false, bars);
                if (d->bars[b].wide)
                    ++b;
            }
            size_bar(d, 6, d->header == 0 ? 0x30 : 0x38, true, 7);
            write_cfg(d, 4, d->command, 2);
        }
        capabilities(d);
        LOGI("PCI %02x:%02x.%u %04x:%04x class=%06x owner=%s reason=%s parent=%04x IRQ=%u/%u",
             d->bdf >> 8, (d->bdf >> 3) & 31, d->bdf & 7, d->vendor, d->device, d->class_code,
             d->owner == PCI_OWNER_GUEST ? "guest" : "vmm",
             d->reservation ? d->reservation : "default", d->parent, d->irq_line, d->irq_pin);
        for (unsigned b = 0; b < PCI_BAR_COUNT; ++b)
            if (d->bars[b].size)
                LOGI("  BAR%u %s base=%llx size=%llx", b,
                     d->bars[b].rom  ? "ROM"
                     : d->bars[b].io ? "IO"
                                     : "MMIO",
                     (unsigned long long)d->bars[b].base, (unsigned long long)d->bars[b].size);
        if (d->header == 1)
            LOGI("  bridge buses=%u..%u IO=%08x MEM=%08x PREF=%08x", d->secondary, d->subordinate,
                 d->bridge_registers[1], d->bridge_registers[2], d->bridge_registers[3]);
    }
    for (unsigned i = 0; i < count; ++i)
        if (devices[i].owner == PCI_OWNER_GUEST)
            for (unsigned b = 0; b < PCI_BAR_COUNT; ++b) {
                const struct pci_bar *guest = &devices[i].bars[b];
                if (!guest->size || guest->io)
                    continue;
                for (unsigned j = 0; j < count; ++j)
                    if (devices[j].owner == PCI_OWNER_VMM)
                        for (unsigned k = 0; k < PCI_BAR_COUNT; ++k) {
                            const struct pci_bar *reserved = &devices[j].bars[k];
                            if (!reserved->size || reserved->io || (guest->rom && reserved->rom))
                                continue;
                            if (guest->base < reserved->base + reserved->size &&
                                reserved->base < guest->base + guest->size) {
                                LOGE("guest BAR overlaps a reserved controller");
                                return false;
                            }
                        }
            }
    inventoried = true;
    LOGI(
        "PCI: ownership established; DMA protection follows inventory, MSI/INTx use VMM-owned host vectors");
    return true;
}
static struct pci_device *decode(uint64_t off, unsigned size, unsigned *reg)
{
    if (!host || off >= IA64VMM_ECAM_SIZE || (size != 1 && size != 2 && size != 4) ||
        (off & (size - 1)) || (off & 0xfff) + size > 4096)
        return NULL;
    *reg = off & 0xfff;
    unsigned index = lookup[off >> 12];
    if (!index || devices[index - 1].owner != PCI_OWNER_GUEST)
        return NULL;
    return &devices[index - 1];
}
static struct pci_bar *bar_at(struct pci_device *d, unsigned reg, unsigned *part)
{
    for (unsigned i = 0; i < PCI_BAR_COUNT; ++i) {
        struct pci_bar *b = &d->bars[i];
        if (b->offset && reg >= b->offset && reg < b->offset + (b->wide ? 8u : 4u)) {
            *part = (reg - b->offset) / 4;
            return b;
        }
    }
    return NULL;
}
static bool irq_at(struct pci_device *d, unsigned reg, unsigned size)
{
    return (d->msi_offset && reg >= d->msi_offset && reg + size <= d->msi_offset + d->msi_length) ||
           (d->msix_offset && reg >= d->msix_offset && reg + size <= d->msix_offset + 12u);
}
/* PCI Local Bus 3.0 §6.2.1: a multi-function device implements function 0,
 * and an enumerator skips functions 1-7 of a device whose function 0 reads
 * absent. AMD platforms put their PCIe root ports at function 1 and up
 * behind a dummy host bridge at function 0, which the reservation table
 * keeps as host-system. Hiding function 0 would hide all sibling root ports
 * and their downstream guest devices from the enumerator. Such a function 0
 * with a guest-owned sibling therefore reads as its own identity, read
 * only: vendor/device, revision/class, header type with the multi-function
 * bit, subsystem IDs; command, status, BARs, capabilities and interrupt
 * registers read 0 and writes are dropped. A reserved function without
 * guest-owned siblings stays absent. */
static struct pci_device *guest_sibling(uint16_t bdf)
{
    for (unsigned fn = 1; fn < 8; ++fn) {
        unsigned sibling = lookup[(bdf & ~7u) | fn];
        if (sibling && devices[sibling - 1].owner == PCI_OWNER_GUEST)
            return &devices[sibling - 1];
    }
    return NULL;
}
static struct pci_device *presence_function0(uint64_t off, unsigned size, unsigned *reg)
{
    if (!host || off >= IA64VMM_ECAM_SIZE || (size != 1 && size != 2 && size != 4) ||
        (off & (size - 1)) || (off & 0xfff) + size > 4096)
        return NULL;
    uint16_t bdf = (uint16_t)(off >> 12);
    unsigned index = lookup[bdf];
    if ((bdf & 7) || (index && devices[index - 1].owner == PCI_OWNER_GUEST))
        return NULL;
    struct pci_device *sibling = guest_sibling(bdf);
    if (sibling && (index || sibling->ari)) {
        *reg = off & 0xfff;
        return index ? &devices[index - 1] : sibling;
    }
    return NULL;
}
static uint64_t presence_read(struct pci_device *d, uint16_t bdf, unsigned reg, unsigned size)
{
    /* An ARI function may have no conventional Function 0 at the start
     * of its eight-RID slice. Supply an inert unclassified presence
     * function so the guest's conventional enumerator reaches it. */
    bool synthetic = d->bdf != bdf;
    uint32_t dword = 0;
    switch (reg & ~3u) {
    case 0x00:
        dword = read_cfg(d, reg & ~3u, 4);
        break;
    case 0x08:
        dword = synthetic ? 0xff000000u : read_cfg(d, 8, 4);
        break;
    case 0x0c:
        dword = (synthetic ? 0 : read_cfg(d, 0x0c, 4) & 0x007f0000u) | 0x00800000u;
        break; /* header type only */
    case 0x2c:
        if (!synthetic && d->header == 0)
            dword = read_cfg(d, 0x2c, 4);
        break;
    default:
        break;
    }
    uint64_t mask = size >= 4 ? UINT32_MAX : (UINT64_C(1) << (size * 8)) - 1;
    return (dword >> ((reg & 3) * 8)) & mask;
}
static unsigned pci_lock;
void platform_pci_lock(void)
{
    platform_lock(&pci_lock);
}
void platform_pci_unlock(void)
{
    platform_unlock(&pci_lock);
}
static uint64_t platform_pci_read_unlocked(uint64_t off, unsigned size)
{
    if (platform_ahci_present() && (off >> 12) == PLATFORM_STORAGE_BDF)
        return platform_ahci_cfg_read((unsigned)(off & 0xfff), size);
    unsigned reg = 0, part = 0;
    struct pci_device *d = decode(off, size, &reg);
    uint64_t mask = size == 8   ? UINT64_MAX
                    : size <= 4 ? (UINT64_C(1) << (size * 8)) - 1
                                : UINT64_MAX;
    if (!d) {
        struct pci_device *f0 = presence_function0(off, size, &reg);
        return f0 ? presence_read(f0, off >> 12, reg, size) : mask;
    }
    /* No extended capabilities (ATS/PRI/PASID/reset controls) in this implementation. */
    if (reg >= 256)
        return 0;
    /* Keep the host's ARI routing and expose the same RID bits through
     * the guest's conventional device/function interpretation. */
    if (!(d->bdf & 7) && reg <= 0x0e && reg + size > 0x0e && guest_sibling(d->bdf))
        return read_cfg(d, reg, size) | (UINT64_C(0x80) << ((0x0e - reg) * 8));
    struct pci_bar *b = bar_at(d, reg, &part);
    if (b) {
        uint32_t value = b->original[part];
        if (b->rom)
            value = (value & ~1u) | b->rom_enabled;
        return ((b->probe[part] ? b->mask[part] : value) >> ((reg & 3) * 8)) & mask;
    }
    if (reg == 4 && size == 4)
        return d->command | ((uint32_t)read_cfg(d, 6, 2) << 16);
    if (reg >= 4 && reg + size <= 6)
        return (d->command >> ((reg - 4) * 8)) & mask;
    if (reg >= 0x3c && reg + size <= 0x40) {
        uint32_t value =
            (read_cfg(d, 0x3c, 4) & 0xffff0000u) | d->irq_line | ((uint32_t)d->irq_pin << 8);
        return (value >> ((reg - 0x3c) * 8)) & mask;
    }
    if (irq_at(d, reg, size)) {
        platform_msi_sync_pending(d);
        uint32_t value = 0;
        for (unsigned i = 0; i < size; ++i)
            value |= (uint32_t)d->irq_config[reg + i] << (i * 8);
        return value;
    }
    return read_cfg(d, reg, size);
}
uint64_t platform_pci_read(uint64_t off, unsigned size)
{
    platform_pci_lock();
    uint64_t result = platform_pci_read_unlocked(off, size);
    platform_pci_unlock();
    return result;
}
/* Intel IDE functions keep their channel timing block at 0x40-0x4b:
 * PIIX/PIIX3/PIIX4 (82371FB/SB §2.3.10, 82371AB §5.1.10: IDETIM) and
 * the ICH/PCH SATA controllers in IDE mode,
 * D31:F2/F5 IDE_TIM, SIDETIM, SDMA_CNT, SDMA_TIM; IDE_TIM bit 15 is IDE
 * Decode Enable in both. These registers configure only this function's
 * own channels, whose compatibility ports or BARs the guest already owns;
 * platform firmware enables decode (as a PC BIOS does) and drivers program
 * timings. intelide reports a channel with decode disabled as absent. */
static bool intel_ide_timing(const struct pci_device *d, unsigned reg, unsigned size)
{
    return d->vendor == 0x8086 && d->class_code >> 8 == 0x0101 && reg >= 0x40 && reg + size <= 0x4c;
}
static void platform_pci_write_unlocked(uint64_t off, uint64_t value, unsigned size)
{
    if (platform_ahci_present() && (off >> 12) == PLATFORM_STORAGE_BDF) {
        platform_ahci_cfg_write((unsigned)(off & 0xfff), value, size);
        return;
    }
    unsigned reg = 0, part = 0;
    struct pci_device *d = decode(off, size, &reg);
    if (!d)
        return;
    struct pci_bar *b = bar_at(d, reg, &part);
    if (b) {
        if (size == 4 &&
            ((uint32_t)value == UINT32_MAX || (b->rom && (uint32_t)value == 0xfffffffeu))) {
            b->probe[part] = true;
            return;
        }
        b->probe[part] = false;
        uint32_t mask = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1;
        unsigned shift = (reg & 3) * 8;
        uint32_t current = b->original[part];
        if (b->rom)
            current = (current & ~1u) | b->rom_enabled;
        uint32_t next = (current & ~(mask << shift)) | (((uint32_t)value & mask) << shift);
        if (b->rom && (next & ~1u) == (b->original[0] & ~1u)) {
            bool enable = next & 1;
            for (unsigned i = 0; enable && i < count; ++i) {
                const struct pci_bar *other = &devices[i].bars[6];
                if (other != b && devices[i].owner == PCI_OWNER_GUEST && other->rom_enabled &&
                    other->size && b->base < other->base + other->size &&
                    other->base < b->base + b->size) {
                    ++d->rejected_writes;
                    return;
                }
            }
            b->rom_enabled = enable;
            return;
        }
        if (next == b->original[part])
            return;
        /* Keep the host assignment and type flags. ROM decode is a software
         * selection among snapshots: no guest write changes host ROM decode. */
    } else if ((reg >= 4 && reg + size <= 6) || (reg == 4 && size == 4)) {
        if (d->header == 1) {
            ++d->rejected_writes;
            return;
        }
        unsigned shift = (reg - 4) * 8;
        unsigned mask = size >= 2 ? 0xffff : 0xff;
        uint16_t next = (d->command & ~(mask << shift)) | ((value & mask) << shift);
        d->command = (d->command & ~0x407u) | (next & 0x407u);
        if (d->command & 4)
            d->guest_bus_master = true;
        write_cfg(d, 4, d->command | 0x400u, 2);
        platform_intx_config_changed(d);
        return;
    } else if (irq_at(d, reg, size)) {
        unsigned start =
            d->msi_offset && reg >= d->msi_offset && reg < d->msi_offset + d->msi_length
                ? d->msi_offset
                : d->msix_offset;
        if ((reg < start + 2 && !(reg == start && size == 4)) ||
            (start == d->msix_offset && reg + size > start + 4)) {
            ++d->rejected_writes;
            return;
        }
        /* Pending bits are read-only; data padding is reserved. */
        unsigned msi_data = start + ((d->irq_config[start + 2] & 0x80) ? 12 : 8);
        if (start == d->msi_offset && reg >= msi_data + 8) {
            ++d->rejected_writes;
            return;
        }
        uint16_t control = d->irq_config[start + 2] | (d->irq_config[start + 3] << 8);
        for (unsigned i = 0; i < size; ++i)
            if (reg + i >= start + 2)
                d->irq_config[reg + i] = value >> (i * 8);
        uint16_t requested = d->irq_config[start + 2] | (d->irq_config[start + 3] << 8);
        uint16_t writable = start == d->msix_offset ? 0xc000u : 0x71u;
        control = (control & ~writable) | (requested & writable);
        d->irq_config[start + 2] = control;
        d->irq_config[start + 3] = control >> 8;
        ++d->irq_writes;
        platform_msi_config_changed(d);
        return;
    } else if (intel_ide_timing(d, reg, size)) {
        write_cfg(d, reg, (uint32_t)value, size);
        return;
    }
    /* IDs, bridge buses/windows/reset, PM, PCIe reset, vendor controls and
     * unknown capability writes are immutable until their mediation exists. */
    ++d->rejected_writes;
}
void platform_pci_write(uint64_t off, uint64_t value, unsigned size)
{
    platform_pci_lock();
    platform_pci_write_unlocked(off, value, size);
    platform_pci_unlock();
}

static uint64_t rom_read_unlocked(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    (void)v;
    uint64_t at = *(const uint64_t *)opaque + off;
    for (unsigned i = 0; i < count; ++i) {
        const struct pci_bar *b = &devices[i].bars[6];
        if (devices[i].owner != PCI_OWNER_GUEST || !b->rom_enabled || !b->hva || at < b->base ||
            at - b->base >= b->size || size > b->size - (at - b->base))
            continue;
        uint64_t value = 0;
        for (unsigned j = 0; j < size; ++j)
            value |= (uint64_t)b->hva[at - b->base + j] << (j * 8);
        return value;
    }
    return size == 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
}
static uint64_t rom_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    platform_pci_lock();
    uint64_t result = rom_read_unlocked(opaque, v, off, size);
    platform_pci_unlock();
    return result;
}
static void rom_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    (void)opaque;
    (void)v;
    (void)off;
    (void)value;
    (void)size;
}
static const struct gm_mmio_ops rom_ops = {rom_read, rom_write};
/* PCI-to-PCI Bridge Architecture §3.2.5: ROM cycles are non-prefetchable,
 * but a borrowed prefetchable BAR already establishes a forwarded memory
 * aperture. Require containment and memory decode on every ancestor. */
static bool rom_forwarded(const struct pci_device *d, uint64_t base, uint64_t size)
{
    uint64_t end = base + size - 1;
    uint16_t parent = d->parent;
    for (unsigned depth = 0; parent != 0xffff; ++depth) {
        if (depth == count || !lookup[parent])
            return false;
        const struct pci_device *p = &devices[lookup[parent] - 1];
        if (!(p->command & 2))
            return false;
        uint32_t mem = p->bridge_registers[2], pref = p->bridge_registers[3];
        uint64_t lo = (mem & 0xfff0u) << 16;
        uint64_t hi = (mem & 0xfff00000u) | 0xfffffu;
        bool inside = lo <= hi && base >= lo && end <= hi;
        lo = (uint64_t)(pref & 0xfff0u) << 16;
        hi = (pref & 0xfff00000u) | 0xfffffu;
        if ((pref & 0xf) == 1 && ((pref >> 16) & 0xf) == 1) {
            lo |= (uint64_t)p->bridge_registers[4] << 32;
            hi |= (uint64_t)p->bridge_registers[5] << 32;
        } else if ((pref & 0xf) || ((pref >> 16) & 0xf))
            return false;
        if (!inside && !(lo <= hi && base >= lo && end <= hi))
            return false;
        parent = p->parent;
    }
    return true;
}
static bool framebuffer_bar(const struct pci_bar *b)
{
    return boot_fb.format != BOOT_FB_NONE && boot_fb.size && boot_fb.base < b->base + b->size &&
           (boot_fb.base >= b->base || b->base - boot_fb.base < boot_fb.size);
}
/* Map and allocate before quiescing. This API runs only on the management
 * CPU before guests; AP log sinks drain only on that CPU. No allocation or
 * logging occurs with decode changed. Maskable IRQs stay off until all
 * registers are back; SMM/NMI firmware remains outside the VMM's control. */
static bool snapshot_rom_at(struct pci_device *d, struct pci_bar *b, const struct pci_bar *borrow)
{
    uint64_t base = borrow ? borrow->base : b->base;
    uint8_t *source = host->map(host_context, base, b->size);
    if (!source)
        return false;
    b->hva = hal_alloc_pages((b->size + 4095) / 4096, 0);
    if (!b->hva)
        return false;
    uint32_t saved[PCI_MAX_DEVICES];
    for (unsigned i = 0; i < count; ++i)
        if (devices[i].bars[6].rom && devices[i].bars[6].mask[0])
            saved[i] = read_cfg(&devices[i], devices[i].bars[6].offset, 4);
    uint16_t command = read_cfg(d, 4, 2);
    bool fb = borrow && framebuffer_bar(borrow), fb_enabled = vmm_log_fb_enabled();
    if (fb)
        vmm_log_fb_enable(false);
    uint64_t flags = hal_irq_save(), start = hal_counter();
    for (unsigned i = 0; i < count; ++i) {
        const struct pci_bar *r = &devices[i].bars[6];
        if (r->rom && r->mask[0])
            write_cfg(&devices[i], r->offset, saved[i] & ~1u, 4);
    }
    write_cfg(d, 4, command & ~7u, 2);
    if (borrow) {
        write_cfg(d, borrow->offset, 0, 4);
        if (borrow->wide)
            write_cfg(d, borrow->offset + 4, 0, 4);
    }
    write_cfg(d, b->offset, (uint32_t)base | 1u, 4);
    write_cfg(d, 4, (command & ~7u) | 2u, 2);
    for (uint64_t i = 0; i < b->size; i += 4)
        *(uint32_t *)(b->hva + i) = *(volatile uint32_t *)(source + i);
    write_cfg(d, 4, command & ~7u, 2);
    write_cfg(d, b->offset, saved[d - devices], 4);
    if (borrow) {
        write_cfg(d, borrow->offset, borrow->original[0], 4);
        if (borrow->wide)
            write_cfg(d, borrow->offset + 4, borrow->original[1], 4);
    }
    for (unsigned i = 0; i < count; ++i) {
        const struct pci_bar *r = &devices[i].bars[6];
        if (r->rom && r->mask[0] && &devices[i] != d)
            write_cfg(&devices[i], r->offset, saved[i], 4);
    }
    write_cfg(d, 4, command, 2);
    uint64_t elapsed = hal_counter() - start;
    hal_irq_restore(flags);
    if (fb)
        vmm_log_fb_enable(fb_enabled);
    uint64_t hz = hal_counter_hz();
    LOGI("PCI %02x:%02x.%u: ROM decode window ticks=%llu hz=%llu us=%llu framebuffer_quiesced=%u",
         d->bdf >> 8, (d->bdf >> 3) & 31, d->bdf & 7, (unsigned long long)elapsed,
         (unsigned long long)hz,
         (unsigned long long)(hz ? elapsed / (hz / 1000000 ? hz / 1000000 : 1) : 0), fb);
    return true;
}
static bool snapshot_rom(struct pci_device *d, struct pci_bar *b)
{
    return snapshot_rom_at(d, b, NULL);
}
static bool rom_unassigned(const struct pci_bar *b);
static bool map_bar(struct pci_device *d, struct pci_bar *b)
{
    if (b->rom) {
        /* The VBIOS container may have taken the snapshot before POST: one copy serves both. */
        uint64_t size;
        if (!platform_pci_rom(d->bdf, &size)) {
            /* No usable aperture means no guest ROM, rather than flash. */
            return !b->size;
        }
        /* Unassigned size masks must not map boot flash or stretch _CRS. */
        if (rom_unassigned(b))
            return true;
    } else
        b->hva = host->map(host_context, b->base, b->size);
    if (!b->hva)
        return false;
    uint64_t at = b->base, end = b->base + b->size;
    while (at < end) {
        uint64_t irq_end = end;
        void *irq_context = NULL;
        const struct gm_mmio_ops *irq_ops =
            b->rom ? NULL : platform_msi_bar_region(d, b, at, &irq_end, &irq_context);
        if (irq_end <= at)
            return false;
        const struct gm_region *existing = gm_lookup(at);
        if (existing) {
            if (irq_ops || (b->rom ? existing->ops != &rom_ops : existing->kind != GM_DIRECT_MMIO))
                return false;
            uint64_t limit = existing->base + existing->size;
            at = irq_end < limit ? irq_end : limit;
            continue;
        }
        uint64_t next = irq_end;
        for (unsigned i = 0; i < gm_region_count(); ++i) {
            const struct gm_region *r = gm_region_at(i);
            if (r->base > at && r->base < next)
                next = r->base;
        }
        void *opaque = irq_context;
        if (next <= at)
            return false;
        if (b->rom) {
            if (rom_regions == PCI_MAX_DEVICES)
                return false;
            rom_bases[rom_regions] = at;
            opaque = &rom_bases[rom_regions++];
        }
        struct gm_region r = {at,
                              next - at,
                              (b->rom || irq_ops) ? GM_EMULATED : GM_DIRECT_MMIO,
                              b->rom    ? "pci-rom"
                              : irq_ops ? "pci-msix"
                                        : "pci-bar",
                              (b->rom || irq_ops) ? NULL : b->hva + at - b->base,
                              b->rom ? &rom_ops : irq_ops,
                              opaque};
        if (!gm_add_region(&r))
            return false;
        at = next;
    }
    return true;
}
/* EDK2 PciBusDxe reads every option ROM at a temporary address and then
 * disables it with RomDecode(FALSE), which writes FFFFFFFEh: the register
 * then reads back its own size mask, an address that is not the card's
 * (on QEMU and on PCs it is boot flash). */
static bool rom_unassigned(const struct pci_bar *b)
{
    return !b->base || !((b->original[0] ^ b->mask[0]) & ~0x7ffu);
}
static unsigned rom_word(const uint8_t *p)
{
    return p[0] | ((unsigned)p[1] << 8);
}
/* Validate PCI Firmware §5.1 headers and lengths. As in EDK2's
 * LoadOpRomImage, the ROM aperture also bounds a complete image chain. */
static const char *rom_invalid(const struct pci_bar *b)
{
    uint64_t at = 0;
    while (at < b->size) {
        const uint8_t *p = b->hva + at;
        uint64_t left = b->size - at;
        if (left < 0x1c || p[0] != 0x55 || p[1] != 0xaa)
            return "missing 55AA signature";
        unsigned off = rom_word(p + 0x18);
        if (off < 0x1c || off > left || left - off < 0x18 || memcmp(p + off, "PCIR", 4))
            return "missing or out-of-bounds PCIR";
        const uint8_t *pcir = p + off;
        uint64_t length = (uint64_t)rom_word(pcir + 0x10) * 512;
        unsigned structure = rom_word(pcir + 0x0a);
        if (!length || length > left || off > length || structure < 0x18 ||
            structure > length - off)
            return "invalid PCIR lengths";
        if ((pcir[0x15] & 0x80) || length == left)
            return NULL;
        at += length;
    }
    return "PCIR chain has no last image";
}
const uint8_t *platform_pci_rom(uint16_t bdf, uint64_t *size)
{
    if (!host || !host->map || !lookup[bdf])
        return NULL;
    struct pci_device *d = &devices[lookup[bdf] - 1];
    struct pci_bar *b = &d->bars[6];
    if (d->owner != PCI_OWNER_GUEST || !b->rom || !b->size)
        return NULL;
    if (!b->hva) {
        bool assigned = !rom_unassigned(b) && rom_forwarded(d, b->base, b->size);
        const struct pci_bar *borrow = NULL, *fb_borrow = NULL;
        if (!assigned && (d->command & 2))
            for (unsigned i = 0; i < 6; ++i) {
                const struct pci_bar *c = &d->bars[i];
                if (!c->size || c->io || c->size < b->size ||
                    c->base + c->size > UINT64_C(0x100000000) ||
                    !rom_forwarded(d, c->base, c->size))
                    continue;
                const struct pci_bar **candidate = framebuffer_bar(c) ? &fb_borrow : &borrow;
                if (!*candidate || c->size < (*candidate)->size)
                    *candidate = c;
            }
        if (!assigned && !borrow)
            borrow = fb_borrow;
        if (!assigned && !borrow) {
            LOGE(
                "PCI %02x:%02x.%u: no ROM: unassigned/outside bridge windows; no enabled forwarded memory BAR",
                bdf >> 8, (bdf >> 3) & 31, bdf & 7);
            b->size = 0;
            return NULL;
        }
        if (borrow)
            LOGI(
                "PCI %02x:%02x.%u: ROM BAR unassigned/outside windows (reads %08x); read at BAR%u's %llx",
                bdf >> 8, (bdf >> 3) & 31, bdf & 7, b->original[0], (unsigned)(borrow - d->bars),
                (unsigned long long)borrow->base);
        if (!(assigned ? snapshot_rom(d, b) : snapshot_rom_at(d, b, borrow)))
            return NULL;
        const char *invalid = rom_invalid(b);
        if (invalid) {
            LOGE("PCI %02x:%02x.%u: no ROM: %s", bdf >> 8, (bdf >> 3) & 31, bdf & 7, invalid);
            hal_free_pages(b->hva, (b->size + 4095) / 4096);
            b->hva = NULL;
            b->size = 0;
            return NULL;
        }
    }
    LOGI("PCI %02x:%02x.%u: ROM snapshot signature=%02x%02x size=%llx", bdf >> 8, (bdf >> 3) & 31,
         bdf & 7, b->hva[0], b->hva[1], (unsigned long long)b->size);
    *size = b->size;
    return b->hva;
}
uint32_t platform_pci_host_read(uint16_t bdf, unsigned off, unsigned size)
{
    return host ? host->read(host_context, bdf, off, size) : UINT32_MAX;
}
void platform_pci_host_write(uint16_t bdf, unsigned off, uint32_t value, unsigned size)
{
    if (host)
        host->write(host_context, bdf, off, value, size);
}
/* Linux drivers/pci/pci.c
 * pci_set_vga_state: decode on the device, PCI_BRIDGE_CTL_VGA on every
 * upstream bridge; vgaarb clears it on the bridges of other paths so one
 * path decodes the legacy ranges. PCI-to-PCI Bridge Architecture 1.2: a
 * bridge forwards no I/O or memory cycle while its own command register
 * disables that space, so the ancestors also get I/O+memory enable. */
bool platform_pci_route_vga(uint16_t bdf)
{
    if (!host || !lookup[bdf])
        return false;
    struct pci_device *card = &devices[lookup[bdf] - 1];
    bool above[PCI_MAX_DEVICES] = {false};
    unsigned depth = 0;
    for (uint16_t parent = card->parent; parent != 0xffff; ++depth) {
        if (depth == count || !lookup[parent]) {
            LOGE("VGA route: invalid bridge ancestry");
            return false;
        }
        above[lookup[parent] - 1] = true;
        parent = devices[lookup[parent] - 1].parent;
    }
    for (unsigned i = 0; i < count; ++i) {
        struct pci_device *d = &devices[i];
        if (d->header != 1)
            continue;
        uint16_t control = read_cfg(d, 0x3e, 2);
        uint16_t next = above[i] ? control | 8u : control & ~8u;
        if (next != control) {
            write_cfg(d, 0x3e, next, 2);
            LOGI("VGA route: bridge %02x:%02x.%u VGA Enable %u -> %u", d->bdf >> 8,
                 (d->bdf >> 3) & 31, d->bdf & 7, (unsigned)(control >> 3) & 1,
                 (unsigned)(next >> 3) & 1);
        }
        if (!above[i])
            continue;
        uint16_t command = read_cfg(d, 4, 2);
        if ((command & 3) != 3) {
            write_cfg(d, 4, command | 3u, 2);
            LOGI("VGA route: bridge %02x:%02x.%u command %04x -> %04x", d->bdf >> 8,
                 (d->bdf >> 3) & 31, d->bdf & 7, command, command | 3u);
        }
        d->command |= 3;
    }
    uint16_t command = read_cfg(card, 4, 2);
    if ((command & 3) != 3) {
        write_cfg(card, 4, command | 3u, 2);
        LOGI("VGA route: %02x:%02x.%u command %04x -> %04x", bdf >> 8, (bdf >> 3) & 31, bdf & 7,
             command, command | 3u);
    }
    card->command |= 3;
    LOGI("VGA route: %02x:%02x.%u decodes the legacy VGA ranges through %u bridge(s)", bdf >> 8,
         (bdf >> 3) & 31, bdf & 7, depth);
    return true;
}
bool platform_pci_map(struct platform_state *s)
{
    for (unsigned i = 0; i < count; ++i) {
        struct pci_device *d = &devices[i];
        if (d->owner != PCI_OWNER_GUEST)
            continue;
        if (d->irq_pin || d->msi_offset || d->msix_offset)
            write_cfg(d, 4, d->command | 0x400u, 2);
        if (!platform_msi_prepare(d, host, host_context))
            return false;
        if (!platform_intx_prepare(d, host, host_context, &s->iosapic))
            return false;
        for (unsigned j = 0; j < PCI_BAR_COUNT; ++j) {
            struct pci_bar *b = &d->bars[j];
            if (!b->size)
                continue;
            if (b->io) {
                if (b->base + b->size > 65536 || !platform_allow_ports(s, b->base, b->size))
                    return false;
            } else {
                if (!map_bar(d, b))
                    return false;
            }
        }
        if (d->class_code >> 8 == 0x0601 && !platform_i8042_configured()) {
            /* The LPC's keyboard is guest-owned legacy hardware,
             * including its ISA keyboard/auxiliary IRQs 1 and 12. */
            if (!platform_allow_ports(s, 0x60, 1) || !platform_allow_ports(s, 0x64, 1))
                return false;
            if (!platform_intx_prepare_isa(1, &s->iosapic, false) ||
                !platform_intx_prepare_isa(12, &s->iosapic, false))
                return false;
            LOGI("guest i8042 ports 0060/0064");
        }
        if (d->class_code >> 8 == 0x0101) {
            /* PCI IDE 1.0 §2: a compatibility channel decodes the ISA task
             * files and raises ISA IRQ14 (primary) or IRQ15 (secondary). */
            unsigned prog = d->class_code & 255;
            if (!(prog & 1) &&
                (!platform_allow_ports(s, 0x1f0, 8) || !platform_allow_ports(s, 0x3f6, 1) ||
                 !platform_intx_prepare_isa(14, &s->iosapic, true)))
                return false;
            if (!(prog & 4) &&
                (!platform_allow_ports(s, 0x170, 8) || !platform_allow_ports(s, 0x376, 1) ||
                 !platform_intx_prepare_isa(15, &s->iosapic, true)))
                return false;
        }
    }
    /* The values restart returns to. */
    for (unsigned i = 0; i < count; ++i) {
        devices[i].boot_command = devices[i].command;
        memcpy(devices[i].boot_irq_config, devices[i].irq_config, sizeof(devices[i].irq_config));
    }
    platform_pci_restart_snapshot();
    /* A shared legacy port decode must not give access to a reserved BAR. */
    for (unsigned i = 0; i < count; ++i)
        if (devices[i].owner == PCI_OWNER_VMM)
            for (unsigned j = 0; j < PCI_BAR_COUNT; ++j) {
                const struct pci_bar *b = &devices[i].bars[j];
                if (!b->io || b->base + b->size > 65536)
                    continue;
                for (unsigned p = b->base; p < b->base + b->size; ++p)
                    s->allowed_ports[p / 8] &= ~(1u << (p & 7));
            }
    for (unsigned i = 0; i < count; ++i) {
        const struct pci_device *d = &devices[i];
        if (d->owner != PCI_OWNER_VMM || d->class_code >> 8 != 0x0101)
            continue;
        const unsigned bases[] = {0x1f0, 0x3f6, 0x170, 0x376};
        const unsigned lengths[] = {8, 1, 8, 1};
        for (unsigned j = 0; j < 4; ++j) {
            if (d->class_code & (j < 2 ? 1u : 4u))
                continue;
            for (unsigned p = bases[j]; p < bases[j] + lengths[j]; ++p)
                s->allowed_ports[p / 8] &= ~(1u << (p & 7));
        }
    }
    return true;
}
static void extend(uint64_t *lo, uint64_t *size, uint64_t base, uint64_t length)
{
    uint64_t end = base + length;
    if (*size && *lo + *size > end)
        end = *lo + *size;
    if (!*size || base < *lo)
        *lo = base;
    *size = end - *lo;
}
static bool mmio_range(struct ia64vmm_handoff *h, uint64_t base, uint64_t size)
{
    uint64_t end = IA64VMM_ALIGN_UP(base + size, 4096);
    base = IA64VMM_ALIGN_DOWN(base, 4096);
    unsigned at = 0, n = h->pci_mmio_range_count;
    while (at < n && h->pci_mmio[at].base + h->pci_mmio[at].size < base)
        ++at;
    unsigned after = at;
    while (after < n && h->pci_mmio[after].base <= end) {
        if (h->pci_mmio[after].base < base)
            base = h->pci_mmio[after].base;
        uint64_t other_end = h->pci_mmio[after].base + h->pci_mmio[after].size;
        if (other_end > end)
            end = other_end;
        ++after;
    }
    if (at == after && n == IA64VMM_HANDOFF_MAX_PCI_MMIO)
        return false;
    memmove(&h->pci_mmio[at + 1], &h->pci_mmio[after], (n - after) * sizeof(h->pci_mmio[0]));
    h->pci_mmio[at] = (struct ia64vmm_ram_range){base, end - base};
    h->pci_mmio_range_count = n - (after - at) + 1;
    return true;
}
bool platform_pci_handoff(struct ia64vmm_handoff *h)
{
    uint64_t mem32 = 0, size32 = 0, mem64 = 0, size64 = 0, io = 0, iosize = 0;
    h->pci_mmio_range_count = 0;
    for (unsigned i = 0; i < count; ++i)
        if (devices[i].owner == PCI_OWNER_GUEST)
            for (unsigned j = 0; j < PCI_BAR_COUNT; ++j) {
                const struct pci_bar *b = &devices[i].bars[j];
                if (!b->size || (b->rom && rom_unassigned(b)))
                    continue;
                if (b->io)
                    extend(&io, &iosize, b->base, b->size);
                else {
                    if (b->base + b->size > UINT64_MAX - 4095 || !mmio_range(h, b->base, b->size))
                        return false;
                    if (b->base + b->size <= 0x100000000ULL)
                        extend(&mem32, &size32, b->base, b->size);
                    else
                        extend(&mem64, &size64, b->base, b->size);
                }
            }
    /* PCI0 _CRS must contain its children's legacy I/O resources,
     * including PS/2 ports and compatibility-mode IDE channels. */
    for (unsigned i = 0; i < count; ++i) {
        const struct pci_device *d = &devices[i];
        if (d->owner != PCI_OWNER_GUEST)
            continue;
        if (d->class_code >> 8 == 0x0601) {
            extend(&io, &iosize, 0x60, 1);
            extend(&io, &iosize, 0x64, 1);
        }
        if (d->class_code >> 8 != 0x0101)
            continue;
        if (!(d->class_code & 1)) {
            extend(&io, &iosize, 0x1f0, 8);
            extend(&io, &iosize, 0x3f6, 1);
        }
        if (!(d->class_code & 4)) {
            extend(&io, &iosize, 0x170, 8);
            extend(&io, &iosize, 0x376, 1);
        }
    }
    if (platform_i8042_configured())
        extend(&io, &iosize, 0x60, 5);
    if (platform_storage_configured()) {
        if (!mmio_range(h, PLATFORM_STORAGE_BASE, PLATFORM_AHCI_SIZE))
            return false;
        extend(&mem32, &size32, PLATFORM_STORAGE_BASE, PLATFORM_AHCI_SIZE);
    }
    h->ecam_bus_first = platform_storage_configured() ? 0 : bus_first;
    h->ecam_bus_last = bus_last;
    h->pci_mmio_base = mem32;
    h->pci_mmio_size = size32;
    h->pci_mmio64_base = mem64;
    h->pci_mmio64_size = size64;
    h->pci_io_base = io;
    h->pci_io_size = iosize;
    if (!platform_intx_handoff(h))
        return false;
    if (platform_storage_configured()) {
        if (h->irq_route_count == IA64VMM_HANDOFF_MAX_IRQ_ROUTES)
            return false;
        h->irq_routes[h->irq_route_count++] = (struct ia64vmm_irq_route){
            .bdf = PLATFORM_STORAGE_BDF, .parent = 0xffff, .pin = 0, .gsi = PLATFORM_STORAGE_GSI};
    }
    return true;
}

void platform_pci_restart(void)
{
    for (unsigned i = 0; i < count; ++i) {
        struct pci_device *d = &devices[i];
        if (d->owner != PCI_OWNER_GUEST)
            continue;
        memcpy(d->irq_config, d->boot_irq_config, sizeof(d->irq_config));
        for (unsigned j = 0; j < PCI_BAR_COUNT; ++j) {
            struct pci_bar *b = &d->bars[j];
            b->probe[0] = b->probe[1] = false;
            if (b->rom)
                b->rom_enabled = b->original[0] & 1;
        }
        /* Bridges keep their command register: guest writes never reach it.
         * An LPC/ISA bridge and an untouched display return to the first
         * boot's value; other functions also lose Bus Master Enable. */
        if (d->header == 0) {
            bool keep = platform_pci_restart_untouched(d) || d->class_code >> 16 == 0x06;
            d->command = keep ? d->boot_command : d->boot_command & ~4u;
            write_cfg(d, 4,
                      d->irq_pin || d->msi_offset || d->msix_offset ? d->command | 0x400u
                                                                    : d->command,
                      2);
        }
        d->guest_bus_master = false;
        platform_msi_restart(d);
    }
    platform_intx_restart();
}
void platform_pci_cleanup(void)
{
    platform_intx_cleanup();
    for (unsigned i = 0; i < count; ++i)
        platform_msi_cleanup(&devices[i]);
    for (unsigned i = 0; i < count; ++i) {
        struct pci_bar *b = &devices[i].bars[6];
        if (b->rom && b->hva) {
            hal_free_pages(b->hva, (b->size + 4095) / 4096);
            b->hva = NULL;
        }
    }
}
