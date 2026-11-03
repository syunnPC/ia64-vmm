/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AP side of the SAL boot rendezvous. Only APs
 * run this file, on their own areas: it uses no boot service, no DXE image
 * and none of the BSP's state, which boot services may still be using. */
#include "ap.h"
#include "ivt-layout.h"
#include "unaligned.h"
#include "boot-translation.h"
#include <ia64vmm/platform.h>
struct ap_wake_block ap_wake_block;
void ap_install_translation(void);
void ap_install_translation(void)
{
    struct ipl_boot_translation tr;
    if (ipl_install_boot_translation(&tr))
        return;
    *(volatile uint64_t *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_PANIC) = 0x47334954;
    for (;;)
        __asm__ volatile("hint @pause");
}
/* The BSP's control block dispatches into the DXE CPU driver; an AP has no
 * handler to call and must never enter it. */
void ap_external_interrupt(uint64_t vector, uint64_t *context);
void ap_external_interrupt(uint64_t vector, uint64_t *context)
{
    (void)context;
    *(volatile uint64_t *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_DEBUG) = vector;
    *(volatile uint64_t *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_PANIC) = 0x47334149;
    for (;;)
        __asm__ volatile("hint @pause");
}
/* The AP's own ar.k1 block: ivt.S reaches its state only through ar.k1. */
void sal_cpu_init(void);
void ap_context_init(volatile uint64_t *context);
void ap_context_init(volatile uint64_t *context)
{
    ap_install_translation();
    /* SAL §4.1.1, §5.1, §6.2: this AP's min-state area and SAL PMI entry. */
    sal_cpu_init();
    const uint64_t *external = (const void *)ap_external_interrupt;
    const uint64_t *unaligned = (const void *)unaligned_dispatch;
    for (unsigned i = 0; i < PERCPU_CONTEXT_SIZE / 8; ++i)
        context[i] = 0;
    context[0] = external[0];
    context[1] = external[1];
    context[5688 / 8] = unaligned[0];
    context[5696 / 8] = unaligned[1];
    /* The block is the area base (PERCPU_CONTEXT is 0, ap.S checks). */
    context[CONTROL_InterruptStack / 8] =
        (uint64_t)(uintptr_t)context + PERCPU_INTERRUPT_STACK + PERCPU_INTERRUPT_STACK_SIZE - 16;
}
