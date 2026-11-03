/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_HOB_H
#define VMM_HOB_H
#include <PiDxe.h>
#include <Pi/PiHob.h>
#include <ia64vmm/platform.h>

#define IPL_HOB_OFFSET 0x110000u
/* BSP interruption stack (ivt.S, CONTROL_InterruptStack), right after the
 * control block, inside the BootServicesData reservation of everything
 * below IPL_CORE_OFFSET (hob.c). It holds the timer's C handler, which runs
 * DXE event notifications (dxe/arch.c tick), hence 64 KiB. */
#define IPL_INTERRUPT_STACK_OFFSET 0x122000u
#define IPL_INTERRUPT_STACK_SIZE 0x10000u
#define IPL_CORE_OFFSET 0x200000u
#define IPL_BSP_OFFSET 0x400000u
#define IPL_STACK_OFFSET 0x800000u
#define IPL_STACK_SIZE 0x100000u
#define IPL_FREE_OFFSET 0x1800000u
#define IPL_HOB_CAPACITY 0x10000u
/* Whole IA-64 8 KiB pages: RAM is described to the PI core in these (hob.c). */
#define IPL_RAM_GRANULE 0x2000u

BOOLEAN ipl_validate_handoff(const struct ia64vmm_handoff *handoff, UINTN size, UINTN id);
VOID *ipl_build_hobs(const struct ia64vmm_handoff *handoff, VOID *buffer, UINTN capacity,
                     UINT64 core_base, UINT64 core_size, UINT64 entry, UINT64 fv_base,
                     UINT64 fv_size);
#endif
