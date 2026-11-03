/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_ACPI_IRQ_H
#define VMM_ACPI_IRQ_H
#include "aml.h"
#include "ia64vmm/pci.h"
#define ACPI_IRQ_ROUTES 4096u
struct acpi_irq_route {
    uint32_t address, gsi;
    uint8_t bus, pin;
    bool low, level;
};
struct acpi_irq {
    struct aml_context aml;
    const struct pci_host_ops *host;
    void *context;
    struct acpi_irq_route routes[ACPI_IRQ_ROUTES];
    uint16_t parents[256];
    unsigned nroutes, rejected;
    bool prepared;
};
bool acpi_irq_prepare(struct acpi_irq *, const struct pci_host_ops *, void *);
bool acpi_irq_resolve(struct acpi_irq *, uint16_t bdf, unsigned pin, uint32_t *gsi, bool *low,
                      bool *level);
/* Host MADT ISA IRQ override lookup (identity/edge/high by default). */
bool acpi_isa_irq(const uint8_t *madt, unsigned irq, uint32_t *gsi, bool *low, bool *level);
#endif
