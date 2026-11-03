/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Ia64/Interrupt/Ia64LocalSapicIpiController.cpp
 */
/* Address encoder; SDM Rev 2.3 Vol.2 §5.8.4: byte INTA/XTP, aligned 8-byte IPI stores. */
#include "ia64vmm/interrupts.h"
#include "ia64vmm/log.h"

static uint64_t pib_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    (void)opaque;
    if (off == IA64_PIB_INTA && size == 1)
        return 15; /* No legacy PIC wired. */
    if (off == IA64_PIB_XTP && size == 1)
        return platform_irq_xtp(v);
    return 0;
}
static void pib_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t data, unsigned size)
{
    (void)opaque;
    if (off == IA64_PIB_XTP && size == 1) {
        platform_irq_set_xtp(v, data);
        return;
    }
    if (size != 8 || (off & 15) || off >= 0x100000) {
        LOGW("PIB rejected access off=%llx width=%u", (unsigned long long)off, size);
        return;
    }
    int target = platform_irq_target((off >> 12) & 255, (off >> 4) & 255);
    if (target < 0 || !platform_irq_inject(target, data & 255, (data >> 8) & 7))
        LOGW("PIB rejected message off=%llx data=%llx", (unsigned long long)off,
             (unsigned long long)data);
}
const struct gm_mmio_ops platform_pib_ops = {pib_read, pib_write};
