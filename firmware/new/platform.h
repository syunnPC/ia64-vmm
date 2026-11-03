/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_PLATFORM_H
#define VMM_PLATFORM_H
#include <PiDxe.h>
#include <Library/HobLib.h>
#include <Library/DebugLib.h>
#include <ia64vmm/platform.h>
#define PLATFORM_CONTROL_OFFSET 0x120000u
#define PLATFORM_TIMER_VECTOR 0xe8u
#define PLATFORM_REARM_OFFSET 5664u /* IVT tail's next ITM interval in ITC ticks. */
#define PLATFORM_HANDOFF_GUID                                                                      \
    {0x68a6d2bb, 0x37f5, 0x4113, {0xa0, 0xb2, 0xa4, 0x46, 0x8d, 0xec, 0x0c, 0x41}}
#define PLATFORM_PANIC_NO_HANDOFF 0x4732484fu
static inline VOID platform_service(UINTN offset, UINT64 value)
{
    *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + offset) = value;
}
static inline const struct ia64vmm_handoff *platform_handoff_hob(VOID)
{
    static const EFI_GUID guid = PLATFORM_HANDOFF_GUID;
    VOID *hob = GetFirstGuidHob(&guid);
    if (!hob)
        platform_service(IA64VMM_SVC_PANIC, PLATFORM_PANIC_NO_HANDOFF);
    return GET_GUID_HOB_DATA(hob);
}
static inline UINT64 platform_itc(VOID)
{
    UINT64 value;
    __asm__ volatile("mov %0=ar.itc" : "=r"(value));
    return value;
}
#endif
