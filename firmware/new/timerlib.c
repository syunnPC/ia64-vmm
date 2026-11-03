/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ITC frequency contract, platform.h. Delay loops do not call PAL idle;
 * architectural Timer notifications are driven exclusively by ITM/IVT. */
#include <Uefi.h>
#include <Library/TimerLib.h>
#include <ia64vmm/platform.h>
#include "timing.h"
UINT64 EFIAPI GetPerformanceCounter(VOID)
{
    UINT64 value;
    __asm__ volatile("mov %0=ar.itc" : "=r"(value));
    return value;
}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *start, UINT64 *end)
{
    if (start)
        *start = 0;
    if (end)
        *end = MAX_UINT64;
    return IA64VMM_ITC_HZ;
}
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 ticks)
{
    return itc_ticks_to_ns(ticks);
}
UINTN EFIAPI NanoSecondDelay(UINTN ns)
{
    UINT64 ticks = itc_ns_to_ticks(ns);
    UINT64 start = GetPerformanceCounter();
    while (GetPerformanceCounter() - start < ticks)
        __asm__ volatile("hint @pause");
    return ns;
}
UINTN EFIAPI MicroSecondDelay(UINTN us)
{
    while (us > MAX_UINT64 / 1000) {
        NanoSecondDelay(MAX_UINT64 / 1000 * 1000);
        us -= MAX_UINT64 / 1000;
    }
    NanoSecondDelay(us * 1000);
    return us;
}
