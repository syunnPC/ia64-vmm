/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ACPI 2.0 §17: bounded AML emission.
 * The PCI resource encodings follow the reference platform's table facts;
 * no vendor namespace, inferred interrupt wiring or BAR assignment is used.
 */
#include "tables.h"
struct aml {
    uint8_t *p;
    size_t n, capacity;
    bool failed;
};
static void byte(struct aml *a, uint8_t value)
{
    if (a->n == a->capacity)
        a->failed = true;
    else
        a->p[a->n++] = value;
}
static void number(struct aml *a, uint64_t value)
{
    if (value <= 1) {
        byte(a, (uint8_t)value);
        return;
    }
    /* Ported from firmware/fw-acpi-aml.c:fw_aml_write_integer. Keep the
     * narrowest constant; revision 2 still permits full QWord constants. */
    unsigned width = value <= 0xff ? 1 : value <= 0xffff ? 2 : value <= UINT32_MAX ? 4 : 8;
    byte(a, width == 1 ? 0x0a : width == 2 ? 0x0b : width == 4 ? 0x0c : 0x0e);
    for (unsigned i = 0; i < width; ++i)
        byte(a, (uint8_t)(value >> (8 * i)));
}
static void name(struct aml *a, const char *s)
{
    for (unsigned i = 0; i < 4; ++i)
        byte(a, (uint8_t)s[i]);
}
static size_t begin(struct aml *a, unsigned op)
{
    if (op > 255)
        byte(a, (uint8_t)(op >> 8));
    byte(a, (uint8_t)op);
    size_t start = a->n;
    for (unsigned i = 0; i < 4; ++i)
        byte(a, 0);
    return start;
}
static void end(struct aml *a, size_t start)
{
    /* Ported from firmware/fw-acpi-aml.c:fw_aml_package_end_kind.
     * ACPI 2.0 §17.2.3: the length includes its own encoded bytes. Closing
     * inner packages first leaves every enclosing start offset intact. */
    if (a->failed)
        return;
    size_t body = a->n - start - 4;
    static const size_t maximum[] = {0x3f, 0xfff, 0xfffff, 0xfffffff};
    unsigned width;
    for (width = 1; width <= 4; ++width)
        if (body <= maximum[width - 1] - width)
            break;
    if (width > 4) {
        a->failed = true;
        return;
    }
    for (size_t i = 0; i < body; ++i)
        a->p[start + width + i] = a->p[start + 4 + i];
    a->n -= 4 - width;
    size_t n = body + width;
    a->p[start] = width == 1 ? (uint8_t)n : (uint8_t)(((width - 1) << 6) | (n & 15));
    for (unsigned i = 1; i < width; ++i)
        a->p[start + i] = (uint8_t)(n >> (4 + (i - 1) * 8));
}
static void named(struct aml *a, const char *s, uint64_t value)
{
    byte(a, 8);
    name(a, s);
    number(a, value);
}
static void eisa(struct aml *a, const char *s, uint32_t value)
{
    byte(a, 8);
    name(a, s);
    byte(a, 0x0c);
    for (unsigned i = 0; i < 4; ++i)
        byte(a, (uint8_t)(value >> (8 * i)));
}
static void qword(struct aml *a, unsigned type, unsigned flags, uint64_t base, uint64_t length,
                  uint64_t translation)
{
    if (!length)
        return;
    byte(a, 0x8a);
    byte(a, 43);
    byte(a, 0);
    byte(a, (uint8_t)type);
    byte(a, 0x0c);
    byte(a, (uint8_t)flags);
    uint64_t fields[] = {0, base, base + length - 1, translation, length};
    for (unsigned i = 0; i < 5; ++i)
        for (unsigned j = 0; j < 8; ++j)
            byte(a, (uint8_t)(fields[i] >> (8 * j)));
}
static bool range(uint64_t base, uint64_t size)
{
    return !size || base <= UINT64_MAX - (size - 1);
}
static void ps2(struct aml *a, bool mouse)
{
    /* Resources from firmware/ssdt-platform-devices.asl's PS2K/PS2M:
     * ACPI 2.0 IO (16-bit decode) and IRQNoFlags (edge, high, exclusive).
     * The handoff flag also controls the FADT's 8042-present boot flag. */
    size_t device = begin(a, 0x5b82);
    name(a, mouse ? "PS2M" : "PS2K");
    eisa(a, "_HID", mouse ? 0x130fd041 : 0x0303d041);
    named(a, "_STA", 15);
    byte(a, 8);
    name(a, "_CRS");
    size_t crs = begin(a, 0x11);
    number(a, mouse ? 5 : 21);
    if (!mouse)
        for (unsigned port = 0x60; port <= 0x64; port += 4) {
            byte(a, 0x47);
            byte(a, 1);
            byte(a, port);
            byte(a, 0);
            byte(a, port);
            byte(a, 0);
            byte(a, 1);
            byte(a, 1);
        }
    byte(a, 0x22);
    byte(a, mouse ? 0 : 2);
    byte(a, mouse ? 0x10 : 0);
    byte(a, 0x79);
    byte(a, 0);
    end(a, crs);
    end(a, device);
}
static bool prt(struct aml *a, const struct ia64vmm_handoff *h, unsigned bus, unsigned depth)
{
    if (!h->irq_version)
        return true;
    if (h->irq_version != IA64VMM_HANDOFF_IRQ_VERSION ||
        h->irq_route_count > IA64VMM_HANDOFF_MAX_IRQ_ROUTES ||
        h->irq_bridge_count > IA64VMM_HANDOFF_MAX_IRQ_ROUTES || depth > 32)
        return false;
    unsigned count = 0;
    for (unsigned i = 0; i < h->irq_route_count; ++i)
        if ((h->irq_routes[i].bdf >> 8) == bus)
            ++count;
    if (count > 255)
        return false;
    byte(a, 8);
    name(a, "_PRT");
    size_t outer = begin(a, 0x12);
    byte(a, count);
    for (unsigned i = 0; i < h->irq_route_count; ++i) {
        const struct ia64vmm_irq_route *r = &h->irq_routes[i];
        if ((r->bdf >> 8) != bus)
            continue;
        if (r->pin > 3 || r->gsi < h->iosapic_gsi_base ||
            r->gsi - h->iosapic_gsi_base >= h->iosapic_gsi_count)
            return false;
        size_t row = begin(a, 0x12);
        byte(a, 4);
        number(a, ((uint64_t)((r->bdf >> 3) & 31) << 16) | 0xffff);
        number(a, r->pin);
        number(a, 0);
        number(a, r->gsi);
        end(a, row);
    }
    end(a, outer);
    for (unsigned i = 0; i < h->irq_bridge_count; ++i) {
        const struct ia64vmm_irq_bridge *b = &h->irq_bridges[i];
        if ((b->bdf >> 8) != bus)
            continue;
        static const char hex[] = "0123456789ABCDEF";
        char id[4] = {'B', hex[(b->bdf >> 8) & 15], hex[(b->bdf >> 4) & 15], hex[b->bdf & 15]};
        size_t bridge = begin(a, 0x5b82);
        name(a, id);
        named(a, "_ADR", ((uint64_t)((b->bdf >> 3) & 31) << 16) | (b->bdf & 7));
        if (!b->secondary || b->secondary == bus || !prt(a, h, b->secondary, depth + 1))
            return false;
        end(a, bridge);
    }
    return !a->failed;
}
bool acpi_build_dsdt(const struct ia64vmm_handoff *h, uint8_t *buffer, size_t capacity,
                     size_t *length)
{
    if (length)
        *length = 0;
    if (!h || !buffer || !length || !h->cpu_count || h->cpu_count > TABLE_MAX_CPUS ||
        h->ecam_bus_first > h->ecam_bus_last || h->ecam_bus_last > 255 ||
        !range(h->pci_mmio_base, h->pci_mmio_size) ||
        (h->pci_mmio_size && (h->pci_mmio_base >= UINT64_C(0x100000000) ||
                              h->pci_mmio_size > UINT64_C(0x100000000) - h->pci_mmio_base)) ||
        !range(h->pci_mmio64_base, h->pci_mmio64_size) ||
        (h->pci_mmio64_size && h->pci_mmio64_base < UINT64_C(0x100000000)) ||
        h->pci_io_base > 65535 || h->pci_io_size > 65536 - h->pci_io_base ||
        (h->pci_io_size && (h->legacy_io_size < IA64VMM_LEGACY_IO_SIZE ||
                            !range(h->legacy_io_base, h->legacy_io_size))))
        return false;
    struct aml a = {buffer, 0, capacity, false};
    /* The captured PM1 control uses SLP_TYP=0 for shutdown. */
    byte(&a, 8);
    name(&a, "_S5_");
    size_t s5 = begin(&a, 0x12);
    byte(&a, 4);
    number(&a, 0);
    number(&a, 0);
    number(&a, 0);
    number(&a, 0);
    end(&a, s5);
    size_t scope = begin(&a, 0x10);
    byte(&a, '\\');
    name(&a, "_SB_");
    for (unsigned i = 0; i < h->cpu_count; ++i) {
        static const char hex[] = "0123456789ABCDEF";
        size_t cpu = begin(&a, 0x5b83);
        char id[4] = {'C', 'P', hex[i >> 4], hex[i & 15]};
        name(&a, id);
        byte(&a, (uint8_t)i);
        for (unsigned j = 0; j < 5; ++j)
            byte(&a, 0); /* No P_BLK or C states. */
        end(&a, cpu);
    }
    size_t pci = begin(&a, 0x5b82);
    name(&a, "PCI0");
    eisa(&a, "_HID", 0x080ad041);
    eisa(&a, "_CID", 0x030ad041);
    named(&a, "_SEG", 0);
    named(&a, "_BBN", h->ecam_bus_first);
    named(&a, "_UID", 0);
    named(&a, "_STA", 15);
    if (!prt(&a, h, h->ecam_bus_first, 0))
        return false;
    if (!h->irq_version) {
        byte(&a, 8);
        name(&a, "_PRT");
        size_t empty = begin(&a, 0x12);
        byte(&a, 0);
        end(&a, empty);
    }
    byte(&a, 8);
    name(&a, "_CRS");
    size_t crs = begin(&a, 0x11);
    /* An assigned legacy VGA also uses the root's A0000-FFFFF decode.
     * Include that parent window so child resources fit within _CRS. */
    const bool vga = (h->flags & IA64VMM_HF_LEGACY_VGA) != 0;
    size_t resource_size =
        16 + 2 +
        46 * ((h->pci_mmio_size != 0) + (h->pci_mmio64_size != 0) + (h->pci_io_size != 0) + vga);
    number(&a, resource_size);
    byte(&a, 0x88);
    byte(&a, 13);
    byte(&a, 0);
    byte(&a, 2);
    byte(&a, 0x0c);
    byte(&a, 0);
    uint16_t bus[] = {0, (uint16_t)h->ecam_bus_first, (uint16_t)h->ecam_bus_last, 0,
                      (uint16_t)(h->ecam_bus_last - h->ecam_bus_first + 1)};
    for (unsigned i = 0; i < 5; ++i) {
        byte(&a, (uint8_t)bus[i]);
        byte(&a, (uint8_t)(bus[i] >> 8));
    }
    if (vga)
        qword(&a, 0, 1, IA64VMM_VGA_MEMORY_BASE, IA64VMM_VGA_MEMORY_SIZE + IA64VMM_LEGACY_AREA_SIZE,
              0);
    qword(&a, 0, 1, h->pci_mmio_base, h->pci_mmio_size, 0);
    qword(&a, 0, 1, h->pci_mmio64_base, h->pci_mmio64_size, 0);
    qword(&a, 1, 0x33, h->pci_io_base, h->pci_io_size, h->legacy_io_base);
    byte(&a, 0x79);
    byte(&a, 0);
    end(&a, crs);
    if (h->flags & IA64VMM_HF_I8042_ENABLED) {
        ps2(&a, false);
        if (!(h->flags & IA64VMM_HF_I8042_KEYBOARD_ONLY))
            ps2(&a, true);
    }
    end(&a, pci);
    end(&a, scope);
    if (a.failed)
        return false;
    *length = a.n;
    return true;
}
