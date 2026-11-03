/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_VBIOS_TRACE_H
#define VMM_VBIOS_TRACE_H
#include "platform/platform.h"
#include "ia64vmm/pci.h"
bool vbios_guest_trace_init(const char *cmdline);
bool vbios_guest_trace_enabled(void);
void vbios_guest_trace_card(const struct pci_device *card);
bool vbios_guest_trace_port(struct vcpu *v, uint16_t port, unsigned size, bool write,
                            uint32_t value);
/* Port-80 diagnostic IN after a header allocates the next packet sequence. */
bool vbios_guest_trace_sequence(struct vcpu *v, uint16_t port, unsigned size, uint32_t *value);
void vbios_guest_trace_summary(void);
#endif
