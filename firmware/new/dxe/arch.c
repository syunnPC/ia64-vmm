/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Ia64/Time/Ia64LocalTimer.S
 */
/* Timer programming; the shared-RBS IVT front end is in firmware/new/ivt.S (SDM Vol.2 §6.11). */
#include "../platform.h"
#include "../timing.h"
#include <Protocol/Cpu.h>
#include <Protocol/Timer.h>
#include <Protocol/Metronome.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Guid/EventGroup.h>
#include <Guid/IdleLoopEvent.h>
#include <Library/BaseLib.h>
static EFI_CPU_INTERRUPT_HANDLER handlers[256];
static EFI_TIMER_NOTIFY timer_notify;
static UINT64 period, last_tick;
static EFI_EVENT exit_event, idle_event;
static UINT64 pal_entry;
static UINT64 *interrupt_control;
static EFI_STATUS EFIAPI enable(EFI_CPU_ARCH_PROTOCOL *self)
{
    (void)self;
    __asm__ volatile("ssm psr.i;; srlz.d;;" ::: "memory");
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI disable(EFI_CPU_ARCH_PROTOCOL *self)
{
    (void)self;
    __asm__ volatile("rsm psr.i;; srlz.d;;" ::: "memory");
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI state(EFI_CPU_ARCH_PROTOCOL *self, BOOLEAN *enabled)
{
    UINT64 psr;
    (void)self;
    if (!enabled)
        return EFI_INVALID_PARAMETER;
    __asm__ volatile("mov %0=psr" : "=r"(psr));
    *enabled = (psr & (1u << 14)) != 0;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI flush(EFI_CPU_ARCH_PROTOCOL *self, EFI_PHYSICAL_ADDRESS start,
                               UINT64 length, EFI_CPU_FLUSH_TYPE type)
{
    (void)self;
    if (type != EfiCpuFlushTypeWriteBack && type != EfiCpuFlushTypeWriteBackInvalidate)
        return EFI_UNSUPPORTED;
    if (length > MAX_UINT64 - start)
        return EFI_INVALID_PARAMETER;
    UINT64 end = start + length;
    if (length)
        for (UINT64 at = start & ~(UINT64)31; at < end;) {
            __asm__ volatile("fc %0" ::"r"(at) : "memory");
            if (end - at <= 32)
                break;
            at += 32;
        }
    __asm__ volatile("sync.i;; srlz.i;;" ::: "memory");
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI init(EFI_CPU_ARCH_PROTOCOL *self, EFI_CPU_INIT_TYPE type)
{
    (void)self;
    (void)type;
    return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI register_handler(EFI_CPU_ARCH_PROTOCOL *self, EFI_EXCEPTION_TYPE vector,
                                          EFI_CPU_INTERRUPT_HANDLER handler)
{
    (void)self;
    if (vector < 16 || vector > 255)
        return EFI_UNSUPPORTED;
    EFI_TPL tpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    EFI_STATUS status = EFI_SUCCESS;
    if (handler && handlers[vector])
        status = EFI_ALREADY_STARTED;
    else if (!handler && !handlers[vector])
        status = EFI_INVALID_PARAMETER;
    else
        handlers[vector] = handler;
    gBS->RestoreTPL(tpl);
    return status;
}
static EFI_STATUS EFIAPI timer_value(EFI_CPU_ARCH_PROTOCOL *self, UINT32 index, UINT64 *value,
                                     UINT64 *resolution)
{
    (void)self;
    if (index || !value)
        return EFI_INVALID_PARAMETER;
    *value = platform_itc();
    if (resolution)
        *resolution = 2500; /* 400 MHz, picoseconds per tick. */
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI attributes(EFI_CPU_ARCH_PROTOCOL *self, EFI_PHYSICAL_ADDRESS base,
                                    UINT64 length, UINT64 attr)
{
    (void)self;
    if (!length || (base & 4095) || (length & 4095) || length > MAX_UINT64 - base)
        return EFI_INVALID_PARAMETER;
    const struct ia64vmm_handoff *h = platform_handoff_hob();
    /* IA-64 has no MTRRs: a reference's attribute comes from its translation
     * or bit 63 of a physical address. For device space the GCD UC attribute
     * only records what the OS is to use (runtime.c, assigned BAR spans), so
     * there is nothing to change; RAM stays WB. */
    if (attr == EFI_MEMORY_UC) {
        for (UINTN i = 0; i < h->ram_range_count; ++i)
            if (base < h->ram[i].base + h->ram[i].size && h->ram[i].base < base + length)
                return EFI_UNSUPPORTED;
        return EFI_SUCCESS;
    }
    if (attr != EFI_MEMORY_WB)
        return EFI_UNSUPPORTED;
    for (UINTN i = 0; i < h->ram_range_count; ++i)
        if (base >= h->ram[i].base && base - h->ram[i].base < h->ram[i].size &&
            length <= h->ram[i].size - (base - h->ram[i].base))
            return EFI_SUCCESS;
    return EFI_UNSUPPORTED;
}
static EFI_CPU_ARCH_PROTOCOL cpu = {flush,       enable,     disable, state, init, register_handler,
                                    timer_value, attributes, 1,       32};
static VOID arm(UINT64 ticks)
{
    /* Recheck after serialized ITM programming: an overdue match otherwise
     * waits for ITC wrap (SDM Vol.2 §3.3.4.2). */
    UINT64 deadline;
    do {
        deadline = platform_itc() + ticks;
        __asm__ volatile("mov cr.itm=%0;; srlz.d;;" ::"r"(deadline) : "memory");
    } while ((INT64)(deadline - platform_itc()) <= 0);
}
static EFI_STATUS EFIAPI notify(EFI_TIMER_ARCH_PROTOCOL *self, EFI_TIMER_NOTIFY fn)
{
    (void)self;
    if (fn && timer_notify)
        return EFI_ALREADY_STARTED;
    if (!fn && !timer_notify)
        return EFI_INVALID_PARAMETER;
    timer_notify = fn;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI set_period(EFI_TIMER_ARCH_PROTOCOL *self, UINT64 value)
{
    (void)self;
    if (value > MAX_INT64 / 40)
        return EFI_INVALID_PARAMETER;
    value = timer_effective_period(value); /* Minimum supported period: 1 ms. */
    EFI_TPL tpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    UINT64 vector = PLATFORM_TIMER_VECTOR | (1u << 16);
    __asm__ volatile("mov cr.itv=%0;; srlz.d;;" ::"r"(vector) : "memory");
    period = value;
    interrupt_control[PLATFORM_REARM_OFFSET / 8] = value * 40;
    if (value) {
        last_tick = platform_itc();
        arm(value * 40);
        vector = PLATFORM_TIMER_VECTOR;
        __asm__ volatile("mov cr.itv=%0;; srlz.d;;" ::"r"(vector) : "memory");
    }
    gBS->RestoreTPL(tpl);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI get_period(EFI_TIMER_ARCH_PROTOCOL *self, UINT64 *value)
{
    (void)self;
    if (!value)
        return EFI_INVALID_PARAMETER;
    *value = period;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI soft(EFI_TIMER_ARCH_PROTOCOL *self)
{
    (void)self;
    EFI_TPL tpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    if (period && timer_notify)
        timer_notify(period);
    gBS->RestoreTPL(tpl);
    return EFI_SUCCESS;
}
static EFI_TIMER_ARCH_PROTOCOL timer = {notify, set_period, get_period, soft};
static VOID EFIAPI tick(EFI_EXCEPTION_TYPE vector, EFI_SYSTEM_CONTEXT context)
{
    (void)vector;
    (void)context;
    UINT64 now = platform_itc(), elapsed = (now - last_tick) / 40;
    last_tick = now;
    /* IVT rearms after restoring FP/GR state. Rearming here can let a slow
     * restore overrun ITM and starve the interrupted code on re-entry. */
    EFI_TPL tpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    if (period && timer_notify)
        timer_notify(elapsed);
    gBS->RestoreTPL(tpl);
}
static VOID EFIAPI dispatch(EFI_EXCEPTION_TYPE vector, EFI_SYSTEM_CONTEXT context)
{
    if (vector < 16 || vector > 255 || !handlers[vector]) {
        DebugPrint(DEBUG_ERROR, "unhandled external vector %u\n", (UINT32)vector);
        platform_service(IA64VMM_SVC_PANIC, 0x47324952);
        for (;;)
            __asm__ volatile("hint @pause");
    }
    handlers[vector](vector, context);
}
static EFI_STATUS EFIAPI wait_ticks(EFI_METRONOME_ARCH_PROTOCOL *self, UINT32 ticks)
{
    UINT64 start = platform_itc(), duration = (UINT64)ticks * self->TickPeriod * 40;
    while (platform_itc() - start < duration)
        __asm__ volatile("hint @pause");
    return EFI_SUCCESS;
}
static EFI_METRONOME_ARCH_PROTOCOL metronome = {wait_ticks, 10};
static VOID EFIAPI exit_boot(EFI_EVENT event, VOID *context)
{
    (void)event;
    (void)context;
    set_period(&timer, 0);
    disable(&cpu);
}
static VOID EFIAPI idle(EFI_EVENT event, VOID *context)
{
    (void)event;
    (void)context;
    if (period) {
        PAL_CALL_RETURN result = AsmPalCall(pal_entry, 29, 0, 0, 0); /* PAL_HALT_LIGHT. */
        if (result.Status)
            platform_service(IA64VMM_SVC_PANIC, 0x4732504c);
    }
}
EFI_STATUS EFIAPI VmmArchEntry(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    (void)image;
    (void)system;
    const struct ia64vmm_handoff *h = platform_handoff_hob();
    const UINT64 *descriptor = (const VOID *)dispatch;
    UINT64 *control = (VOID *)(UINTN)(h->fw_data_base + PLATFORM_CONTROL_OFFSET);
    interrupt_control = control;
    control[0] = descriptor[0];
    control[1] = descriptor[1];
    pal_entry = control[5656 / 8];
    static const EFI_GUID idle_guid = IDLE_LOOP_EVENT_GUID;
    EFI_STATUS idle_status =
        gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, idle, NULL, &idle_guid, &idle_event);
    if (EFI_ERROR(idle_status))
        return idle_status;
    handlers[PLATFORM_TIMER_VECTOR] = tick;
    EFI_STATUS status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, exit_boot, NULL,
                                           &gEfiEventExitBootServicesGuid, &exit_event);
    if (EFI_ERROR(status))
        return status;
    EFI_HANDLE handle = NULL;
    status = gBS->InstallMultipleProtocolInterfaces(
        &handle, &gEfiCpuArchProtocolGuid, &cpu, &gEfiTimerArchProtocolGuid, &timer,
        &gEfiMetronomeArchProtocolGuid, &metronome, NULL);
    if (!EFI_ERROR(status))
        set_period(&timer, 1000000); /* 100 ms. */
    DebugPrint(DEBUG_INFO, "CPU/ITM/Metronome: %r\n", status);
    return status;
}
