/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_PLATFORM_IRQ_INTERNAL_H
#define IA64VMM_PLATFORM_IRQ_INTERNAL_H
#include "ia64vmm/interrupts.h"
/* Opt-in intx_latency=<vector>: host arrival through vCPU 0 to target IVT.
 * Called with IOSAPIC held immediately before successful injection. */
void platform_intx_delivery(struct ia64_iosapic *, unsigned gsi, unsigned cpu, unsigned vector);
void platform_irq_intx_start(unsigned cpu, unsigned vector, uint64_t ticks);
/* Measurement is fixed at VM setup; IRQs skip the counter when disabled. */
bool platform_irq_intx_measuring(void);
#endif
