/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Acpi/Aml/Facts.cpp
 */
/* Routing facts with real bridge swizzling (PCI 3.0 §2.2.6).
 * Link routing reads _CRS, never substitutes _PRS or PCI Interrupt Line;
 * a link whose _STA reports it disabled has no route. */
#include "acpi-irq.h"
#include "ia64vmm/log.h"
static unsigned property(struct acpi_irq *c, unsigned node, const char name[4])
{
    return aml_evaluate(&c->aml, aml_child(&c->aml, node, name), NULL, 0);
}
static unsigned value(struct acpi_irq *c, unsigned id)
{
    for (unsigned i = 0; i < 32; ++i) {
        const struct aml_object *o = aml_object(&c->aml, id);
        if (o->kind != AML_REFERENCE)
            return id;
        id = aml_evaluate(&c->aml, o->first, NULL, 0);
    }
    return 0;
}
static bool number(struct acpi_irq *c, unsigned id, uint64_t *v)
{
    const struct aml_object *o = aml_object(&c->aml, value(c, id));
    if (o->kind != AML_INTEGER)
        return false;
    *v = o->integer;
    return true;
}
static bool root_id(struct acpi_irq *c, unsigned id)
{
    const struct aml_object *o = aml_object(&c->aml, value(c, id));
    if (o->kind == AML_INTEGER)
        return o->integer == 0x030ad041 || o->integer == 0x080ad041;
    if (o->kind == AML_STRING && o->length == 7) {
        const uint8_t *p = o->bytes;
        return p[0] == 'P' && p[1] == 'N' && p[2] == 'P' && p[3] == '0' && p[4] == 'A' &&
               p[5] == '0' && (p[6] == '3' || p[6] == '8');
    }
    if (o->kind == AML_PACKAGE)
        for (unsigned i = 0; i < o->length; ++i) {
            const struct aml_object *item =
                aml_object(&c->aml, value(c, c->aml.elements[o->first + i]));
            if (item->kind == AML_INTEGER &&
                (item->integer == 0x030ad041 || item->integer == 0x080ad041))
                return true;
        }
    return false;
}
static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static bool root_bus(struct acpi_irq *c, unsigned node, unsigned *bus)
{
    uint64_t v;
    if (aml_child(&c->aml, node, "_SEG") && (!number(c, property(c, node, "_SEG"), &v) || v))
        return false;
    if (aml_child(&c->aml, node, "_BBN")) {
        if (!number(c, property(c, node, "_BBN"), &v) || v > 255)
            return false;
        *bus = v;
        return true;
    }
    const struct aml_object *o = aml_object(&c->aml, property(c, node, "_CRS"));
    if (o->kind != AML_BUFFER)
        return false;
    for (unsigned at = 0; at < o->length;) {
        unsigned tag = o->bytes[at++], n;
        if (tag & 128) {
            if (o->length - at < 2)
                return false;
            n = le(o->bytes + at, 2);
            at += 2;
        } else
            n = tag & 7;
        if (n > o->length - at)
            return false;
        unsigned width = tag == 0x88 ? 2 : tag == 0x87 ? 4 : tag == 0x8a ? 8 : 0;
        if (width && n >= 3 + width * 5 && o->bytes[at] == 2) {
            v = le(o->bytes + at + 3 + width, width);
            if (v > 255)
                return false;
            *bus = v;
            return true;
        }
        at += n;
    }
    return false;
}
static bool bus_for(struct acpi_irq *c, unsigned node, unsigned *bus, unsigned depth)
{
    if (!node || depth > 32)
        return false;
    if (root_id(c, property(c, node, "_HID")) || root_id(c, property(c, node, "_CID")))
        return root_bus(c, node, bus);
    unsigned parent = c->aml.nodes[node].parent;
    if (!bus_for(c, parent, bus, depth + 1))
        return false;
    unsigned adr = aml_child(&c->aml, node, "_ADR");
    if (!adr)
        return true; /* Namespace scope between root and bridge. */
    uint64_t v;
    if (!number(c, aml_evaluate(&c->aml, adr, NULL, 0), &v) || (v >> 16) > 31 || (v & 0xffff) > 7)
        return false;
    uint16_t bdf = (*bus << 8) | ((v >> 16) << 3) | (v & 7);
    unsigned header = c->host->read(c->context, bdf, 14, 1) & 0x7f;
    if (header != 1)
        return false;
    unsigned secondary = c->host->read(c->context, bdf, 0x19, 1);
    if (!secondary || secondary > 255 || secondary == *bus)
        return false;
    c->parents[secondary] = bdf;
    *bus = secondary;
    return true;
}
/* ACPI 6.5 §19.6.100: a PCI_Config region addresses the function named by
 * the _ADR of its device, on the bus where that device sits (§6.5.5). */
static void regions(struct acpi_irq *c)
{
    for (unsigned node = 2; node < c->aml.nnodes; ++node) {
        struct aml_node *r = &c->aml.nodes[node];
        if (r->opcode != 0x5b80 || r->flags != 2)
            continue;
        unsigned device = r->parent, adr = aml_child(&c->aml, device, "_ADR"), bus;
        uint64_t v;
        if (!adr || !number(c, aml_evaluate(&c->aml, adr, NULL, 0), &v) || (v >> 16) > 31 ||
            (v & 0xffff) > 7)
            continue;
        bool root =
            root_id(c, property(c, device, "_HID")) || root_id(c, property(c, device, "_CID"));
        if (!(root ? root_bus(c, device, &bus) : bus_for(c, c->aml.nodes[device].parent, &bus, 0)))
            continue;
        r->target = 1 + ((bus << 8) | ((v >> 16) << 3) | (v & 7));
    }
}
bool acpi_irq_prepare(struct acpi_irq *c, const struct pci_host_ops *host, void *context)
{
    c->host = host;
    c->context = context;
    c->nroutes = c->rejected = 0;
    c->prepared = false;
    c->aml.config_read = host->read;
    c->aml.config_context = context;
    for (unsigned i = 0; i < 256; ++i)
        c->parents[i] = 0xffff;
    unsigned pic = aml_child(&c->aml, 1, "_PIC");
    if (pic) {
        unsigned arg = aml_integer(&c->aml, 1);
        aml_evaluate(&c->aml, pic, &arg, 1);
        if (c->aml.error) {
            LOGW("host AML _PIC(1) unsupported; INTx routing unavailable");
            return false;
        }
    }
    regions(c);
    for (unsigned node = 2; node < c->aml.nnodes; ++node) {
        if (!aml_child(&c->aml, node, "_PRT"))
            continue;
        unsigned bus;
        if (!bus_for(c, node, &bus, 0)) {
            ++c->rejected;
            continue;
        }
        unsigned package = property(c, node, "_PRT");
        const struct aml_object *prt = aml_object(&c->aml, package);
        if (prt->kind != AML_PACKAGE) {
            ++c->rejected;
            continue;
        }
        for (unsigned i = 0; i < prt->length; ++i) {
            const struct aml_object *row =
                aml_object(&c->aml, value(c, c->aml.elements[prt->first + i]));
            uint64_t address, pin, index;
            if (row->kind != AML_PACKAGE || row->length != 4 ||
                !number(c, c->aml.elements[row->first], &address) || address > UINT32_MAX ||
                (address >> 16) > 31 || !number(c, c->aml.elements[row->first + 1], &pin) ||
                pin > 3 || !number(c, c->aml.elements[row->first + 3], &index) ||
                index > UINT32_MAX) {
                ++c->rejected;
                continue;
            }
            const struct aml_object *source = aml_object(&c->aml, c->aml.elements[row->first + 2]);
            uint32_t gsi;
            bool low = true, level = true;
            if (source->kind == AML_INTEGER && !source->integer)
                gsi = index;
            else if (source->kind == AML_REFERENCE) {
                /* §6.3.7 _STA bit 1: the link decodes its current resources. */
                unsigned sta = aml_child(&c->aml, source->first, "_STA");
                uint64_t status;
                if (sta &&
                    (!number(c, aml_evaluate(&c->aml, sta, NULL, 0), &status) || !(status & 2))) {
                    ++c->rejected;
                    continue;
                }
                const struct aml_object *crs =
                    aml_object(&c->aml, property(c, source->first, "_CRS"));
                if (!aml_irq_resource(crs, index, &gsi, &low, &level)) {
                    ++c->rejected;
                    continue;
                }
            } else {
                ++c->rejected;
                continue;
            }
            if (c->nroutes == ACPI_IRQ_ROUTES)
                return false;
            c->routes[c->nroutes++] = (struct acpi_irq_route){address, gsi, bus, pin, low, level};
        }
    }
    c->prepared = true;
    LOGI("host AML INTx routes=%u unresolved=%u", c->nroutes, c->rejected);
    return true;
}
bool acpi_irq_resolve(struct acpi_irq *c, uint16_t bdf, unsigned pin, uint32_t *gsi, bool *low,
                      bool *level)
{
    if (!c->prepared || pin < 1 || pin > 4)
        return false;
    --pin;
    for (unsigned depth = 0; depth < 32; ++depth) {
        unsigned bus = bdf >> 8, device = (bdf >> 3) & 31, function = bdf & 7;
        unsigned parent = c->parents[bus];
        for (unsigned i = 0; i < platform_pci_count(); ++i) {
            const struct pci_device *d = platform_pci_device(i);
            if (d->bdf == bdf) {
                parent = d->parent;
                /* PCIe Base 2.1 §2.2.8.1: the implied Device Number
                 * for ARI INTx wire mapping is zero, including swizzling. */
                if (d->ari) {
                    device = 0;
                    function = bdf & 255;
                }
                break;
            }
        }
        const struct acpi_irq_route *match = NULL;
        for (unsigned i = 0; i < c->nroutes; ++i) {
            const struct acpi_irq_route *r = &c->routes[i];
            if (r->bus != bus || (r->address >> 16) != device || r->pin != pin ||
                ((r->address & 0xffff) != 0xffff && (r->address & 0xffff) != function))
                continue;
            if (match && (r->gsi != match->gsi || r->low != match->low || r->level != match->level))
                return false;
            match = r;
        }
        if (match) {
            *gsi = match->gsi;
            *low = match->low;
            *level = match->level;
            return true;
        }
        if (parent == 0xffff || parent == bdf)
            return false;
        pin = (pin + device) & 3;
        bdf = parent;
    }
    return false;
}
/* ISA IRQ to GSI through MADT Interrupt Source Overrides (ACPI 6.5
 * §5.2.12.5, type 2, length 10: bus 0, source, GSI, MPS INTI flags).
 * Without an override the ISA default applies: GSI = IRQ, edge, active
 * high (ACPI 6.5 Table 5.26, "conforms to the bus"). Malformed tables and
 * reserved flag encodings are rejected rather than guessed. */
bool acpi_isa_irq(const uint8_t *madt, unsigned irq, uint32_t *gsi, bool *low, bool *level)
{
    if (!madt || irq > 15 || le(madt + 4, 4) < 44)
        return false;
    unsigned length = le(madt + 4, 4);
    *gsi = irq;
    *low = false;
    *level = false;
    for (unsigned at = 44; at < length;) {
        const uint8_t *p = madt + at;
        if (length - at < 2 || p[1] < 2 || p[1] > length - at)
            return false;
        if (p[0] == 2) {
            if (p[1] != 10)
                return false;
            if (p[2] == 0 && p[3] == irq) {
                unsigned flags = le(p + 8, 2), polarity = flags & 3, trigger = (flags >> 2) & 3;
                if (polarity == 2 || trigger == 2)
                    return false;
                *gsi = le(p + 4, 4);
                *low = polarity == 3;
                *level = trigger == 3;
            }
        }
        at += p[1];
    }
    return true;
}
