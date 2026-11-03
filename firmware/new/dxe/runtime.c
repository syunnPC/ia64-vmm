/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../platform.h"
#include "../legacy-window.h"
#include <Library/DxeServicesTableLib.h>
#include <Protocol/Reset.h>
#include <Protocol/RealTimeClock.h>
#include <Protocol/MonotonicCounter.h>
#include <Protocol/Capsule.h>
#include <Protocol/WatchdogTimer.h>
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
static volatile UINT64 *service = (VOID *)(UINTN)IA64VMM_SERVICE_BASE;
static INT64 rtc_offset;
static UINT64 monotonic;
static INT16 timezone = EFI_UNSPECIFIED_TIMEZONE;
static UINT8 daylight;
static EFI_EVENT virtual_event, watchdog_event;
static EFI_EVENT counter_event, exit_event;
static BOOLEAN at_runtime;
static EFI_STATUS counter_status;
static VOID *watchdog_registration;
static EFI_WATCHDOG_TIMER_ARCH_PROTOCOL *watchdog;
static EFI_WATCHDOG_TIMER_SET_TIMER_PERIOD watchdog_set;
static const EFI_GUID counter_guid = {
    0x54f438fe, 0x2b16, 0x4d6c, {0xa3, 0x0b, 0x9b, 0x1c, 0x44, 0xf2, 0xb4, 0x59}};
static VOID EFIAPI reset(EFI_RESET_TYPE type, EFI_STATUS status, UINTN size, VOID *data)
{
    (void)size;
    (void)data;
    service[IA64VMM_SVC_RESET_STATUS / 8] = status;
    service[IA64VMM_SVC_RESET / 8] = type;
    for (;;)
        CpuPause();
}
static BOOLEAN leap(UINTN year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
static UINTN month_days(UINTN year, UINTN month)
{
    static const UINT8 lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return lengths[month - 1] + (month == 2 && leap(year));
}
static EFI_STATUS EFIAPI get_time(EFI_TIME *time, EFI_TIME_CAPABILITIES *caps)
{
    if (!time)
        return EFI_INVALID_PARAMETER;
    INT64 seconds = (INT64)service[IA64VMM_SVC_RTC / 8] + rtc_offset;
    /* Calendar bounds are UEFI 2.7 §8.3; the VMM supplies UTC Unix seconds. */
    if (seconds < -2208988800LL || seconds > 253402300799LL)
        return EFI_DEVICE_ERROR;
    INT64 days = seconds / 86400, remainder = seconds % 86400;
    if (remainder < 0) {
        remainder += 86400;
        --days;
    }
    UINTN year = 1970;
    while (days < 0) {
        --year;
        days += 365 + leap(year);
    }
    while (days >= (INT64)(365 + leap(year))) {
        days -= 365 + leap(year);
        ++year;
    }
    UINTN month = 1;
    while (days >= (INT64)month_days(year, month)) {
        days -= month_days(year, month);
        ++month;
    }
    ZeroMem(time, sizeof(*time));
    time->Year = year;
    time->Month = month;
    time->Day = days + 1;
    time->Hour = remainder / 3600;
    time->Minute = remainder / 60 % 60;
    time->Second = remainder % 60;
    time->TimeZone = timezone;
    time->Daylight = daylight;
    if (caps) {
        caps->Resolution = 1;
        caps->Accuracy = 50000000;
        caps->SetsToZero = FALSE;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI set_time(EFI_TIME *time)
{
    if (!time || time->Year < 1900 || time->Year > 9999 || time->Month < 1 || time->Month > 12 ||
        time->Day < 1 || time->Day > month_days(time->Year, time->Month) || time->Hour > 23 ||
        time->Minute > 59 || time->Second > 59 || time->Nanosecond > 999999999 ||
        (time->TimeZone != EFI_UNSPECIFIED_TIMEZONE &&
         (time->TimeZone < -1440 || time->TimeZone > 1440)) ||
        (time->Daylight & ~3u))
        return EFI_INVALID_PARAMETER;
    INT64 days = 0;
    if (time->Year >= 1970)
        for (UINTN y = 1970; y < time->Year; ++y)
            days += 365 + leap(y);
    else
        for (UINTN y = time->Year; y < 1970; ++y)
            days -= 365 + leap(y);
    for (UINTN m = 1; m < time->Month; ++m)
        days += month_days(time->Year, m);
    INT64 seconds =
        (days + time->Day - 1) * 86400 + time->Hour * 3600 + time->Minute * 60 + time->Second;
    /* Software clock adjustment lasts this firmware instance. The VMM's host
     * RTC and checkpoint format do not retain SetTime/timezone/daylight. */
    rtc_offset = seconds - (INT64)service[IA64VMM_SVC_RTC / 8];
    timezone = time->TimeZone;
    daylight = time->Daylight;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI get_wakeup(BOOLEAN *enabled, BOOLEAN *pending, EFI_TIME *time)
{
    if (!enabled || !pending || !time)
        return EFI_INVALID_PARAMETER;
    return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI set_wakeup(BOOLEAN enabled, EFI_TIME *time)
{
    (void)enabled;
    (void)time;
    return EFI_UNSUPPORTED;
}
static EFI_STATUS persist_counter(VOID)
{
    UINT32 high = (UINT32)(monotonic >> 32);
    counter_status = gRT->SetVariable(L"VmmMonotonicHigh", (EFI_GUID *)&counter_guid,
                                      EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                          EFI_VARIABLE_RUNTIME_ACCESS,
                                      sizeof(high), &high);
    return counter_status;
}
static EFI_STATUS EFIAPI next_high(UINT32 *value)
{
    if (!value)
        return EFI_INVALID_PARAMETER;
    EFI_TPL tpl = at_runtime ? TPL_APPLICATION : gBS->RaiseTPL(TPL_HIGH_LEVEL);
    UINT32 high = (UINT32)(monotonic >> 32);
    if (high == MAX_UINT32) {
        if (!at_runtime)
            gBS->RestoreTPL(tpl);
        return EFI_DEVICE_ERROR;
    }
    ++high;
    monotonic = (UINT64)high << 32;
    *value = high;
    if (!at_runtime)
        gBS->RestoreTPL(tpl);
    return persist_counter();
}
static VOID EFIAPI counter_notify(EFI_EVENT event, VOID *context)
{
    (void)event;
    (void)context;
    persist_counter();
}
static VOID EFIAPI exit_boot(EFI_EVENT event, VOID *context)
{
    (void)event;
    (void)context;
    at_runtime = TRUE;
}
static EFI_STATUS EFIAPI next_count(UINT64 *value)
{
    if (!value)
        return EFI_INVALID_PARAMETER;
    if (at_runtime)
        return EFI_UNSUPPORTED;
    EFI_TPL tpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    EFI_STATUS status = monotonic == MAX_UINT64 ? EFI_DEVICE_ERROR : counter_status;
    if (!EFI_ERROR(status))
        *value = ++monotonic;
    BOOLEAN overflow = !EFI_ERROR(status) && (UINT32)monotonic == 0;
    gBS->RestoreTPL(tpl);
    /* As in UDK2018 MonotonicCounterRuntimeDxe, defer NV writes to a
     * TPL_CALLBACK event: VariableRuntimeDxe cannot acquire its TPL_NOTIFY
     * lock while this service holds TPL_HIGH_LEVEL. */
    if (overflow)
        gBS->SignalEvent(counter_event);
    return EFI_ERROR(status) ? status : counter_status;
}
static EFI_STATUS EFIAPI update_capsule(EFI_CAPSULE_HEADER **capsules, UINTN count,
                                        EFI_PHYSICAL_ADDRESS gather)
{
    (void)gather;
    if (!capsules || !count)
        return EFI_INVALID_PARAMETER;
    return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI query_capsule(EFI_CAPSULE_HEADER **capsules, UINTN count, UINT64 *size,
                                       EFI_RESET_TYPE *type)
{
    if (!capsules || !count || !size || !type)
        return EFI_INVALID_PARAMETER;
    return EFI_UNSUPPORTED;
}
static VOID EFIAPI convert(EFI_EVENT event, VOID *context)
{
    (void)event;
    (void)context;
    gRT->ConvertPointer(0, (VOID **)&service);
}
static EFI_STATUS EFIAPI report_watchdog(EFI_WATCHDOG_TIMER_ARCH_PROTOCOL *self, UINT64 duration)
{
    EFI_STATUS status = watchdog_set(self, duration);
    if (!EFI_ERROR(status))
        service[IA64VMM_SVC_WATCHDOG / 8] = duration / 10000000;
    return status;
}
static VOID EFIAPI bind_watchdog(EFI_EVENT event, VOID *context)
{
    (void)event;
    (void)context;
    if (watchdog)
        return;
    if (!EFI_ERROR(
            gBS->LocateProtocol(&gEfiWatchdogTimerArchProtocolGuid, NULL, (VOID **)&watchdog))) {
        watchdog_set = watchdog->SetTimerPeriod;
        watchdog->SetTimerPeriod = report_watchdog;
    }
}
/* HOB capabilities alone do not publish MMIO in GetMemoryMap. The PI
 * overlay reports active UC without RUNTIME on the default path. Assigned
 * VGA and legacy VGA keep the UC|RUNTIME policy. */
static EFI_STATUS publish_legacy_memory(const struct ia64vmm_handoff *h)
{
    BOOLEAN legacy = h->legacy_version == IA64VMM_HANDOFF_LEGACY_VERSION &&
                     (h->legacy_flags & IA64VMM_LEGACY_TEXT_CONSOLE);
    for (uint64_t cursor = IA64VMM_VGA_MEMORY_BASE; cursor < 0x100000;) {
        uint64_t base, size;
        uint64_t limit = cursor < IA64VMM_LEGACY_AREA_BASE ? IA64VMM_LEGACY_AREA_BASE : 0x100000;
        if (!legacy_window_next(h, &cursor, limit, &base, &size))
            continue;
        UINT64 attributes = EFI_MEMORY_UC;
        if (legacy || (base < IA64VMM_LEGACY_AREA_BASE && (h->flags & IA64VMM_HF_LEGACY_VGA)))
            attributes |= EFI_MEMORY_RUNTIME;
        EFI_STATUS status = gDS->SetMemorySpaceAttributes(base, size, attributes);
        if (EFI_ERROR(status)) {
            DebugPrint(DEBUG_ERROR, "legacy UC publication %lx+%lx failed: %r\n", base, size,
                       status);
            /* An unclaimed default window must not disable RTC/reset/etc.
             * legacy VGA still requires its ROM/stub runtime mapping. */
            if (legacy)
                return status;
        }
    }
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI VmmRuntimeEntry(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    (void)image;
    (void)system;
    EFI_STATUS map_status = gDS->SetMemorySpaceAttributes(
        IA64VMM_SERVICE_BASE, IA64VMM_SERVICE_SIZE, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    if (EFI_ERROR(map_status))
        return map_status;
    const struct ia64vmm_handoff *h = platform_handoff_hob();
    map_status = publish_legacy_memory(h);
    if (EFI_ERROR(map_status)) {
        DebugPrint(DEBUG_ERROR, "VGA/legacy memory attributes: %r\n", map_status);
        return map_status; /* Explicit refusal: mandatory UC publication failed. */
    }
    /* Publish IA-64's I/O port translation window as
     * EfiMemoryMappedIOPortSpace. The PI core selects that type for runtime
     * MMIO with its private EFI_MEMORY_PORT_IO attribute (DxeMain.h,
     * Page.c); Gcd.c gives MMIO entries this capability. */
    const UINT64 port_io = 0x4000000000000000ULL;
    map_status = gDS->SetMemorySpaceAttributes(h->legacy_io_base, h->legacy_io_size,
                                               EFI_MEMORY_UC | EFI_MEMORY_RUNTIME | port_io);
    if (EFI_ERROR(map_status)) {
        DebugPrint(DEBUG_ERROR, "I/O port space %lx: %r\n", h->legacy_io_base, map_status);
        return map_status;
    }
    if (h->flags & IA64VMM_HF_UART_PRESENT) {
        map_status = gDS->SetMemorySpaceAttributes(h->uart_base, IA64VMM_UART_RUNTIME_SIZE,
                                                   EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
        if (EFI_ERROR(map_status))
            return map_status;
    }
    /* Publish assigned PCI BARs and any separate framebuffer as UC
     * EfiMemoryMappedIO. These device ranges are not firmware runtime
     * memory. The gcd-uc-mmio.patch overlay preserves their descriptors. */
    BOOLEAN fb_in_bar = FALSE;
    for (UINTN i = 0; i < h->pci_mmio_range_count && i < IA64VMM_HANDOFF_MAX_PCI_MMIO; ++i) {
        UINT64 base = h->pci_mmio[i].base, length = h->pci_mmio[i].size;
        map_status = gDS->SetMemorySpaceAttributes(base, length, EFI_MEMORY_UC);
        if (EFI_ERROR(map_status)) {
            DebugPrint(DEBUG_ERROR, "PCI MMIO %lx+%lx: %r\n", base, length, map_status);
            return map_status;
        }
        if (h->fb_base >= base && h->fb_size <= length && h->fb_base - base <= length - h->fb_size)
            fb_in_bar = TRUE;
    }
    /* The same range hob.c gives the framebuffer outside the BAR spans. */
    if (h->fb_size && !fb_in_bar) {
        map_status = gDS->SetMemorySpaceAttributes(h->fb_base, (h->fb_size + 4095) & ~(UINT64)4095,
                                                   EFI_MEMORY_UC);
        if (EFI_ERROR(map_status)) {
            DebugPrint(DEBUG_ERROR, "framebuffer %lx: %r\n", h->fb_base, map_status);
            return map_status;
        }
    }
    UINT32 high = 0, attr;
    UINTN size = sizeof(high);
    EFI_STATUS status =
        gRT->GetVariable(L"VmmMonotonicHigh", (EFI_GUID *)&counter_guid, &attr, &size, &high);
    if (status != EFI_NOT_FOUND && EFI_ERROR(status))
        return status;
    monotonic = (UINT64)high << 32;
    status = next_high(&high);
    if (EFI_ERROR(status))
        return status;
    status =
        gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, counter_notify, NULL, &counter_event);
    if (EFI_ERROR(status))
        return status;
    static const EFI_GUID exit_guid = EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
    status =
        gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, exit_boot, NULL, &exit_guid, &exit_event);
    if (EFI_ERROR(status))
        return status;
    status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, convert, NULL,
                                &gEfiEventVirtualAddressChangeGuid, &virtual_event);
    if (EFI_ERROR(status))
        return status;
    status =
        gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, bind_watchdog, NULL, &watchdog_event);
    if (EFI_ERROR(status))
        return status;
    status = gBS->RegisterProtocolNotify(&gEfiWatchdogTimerArchProtocolGuid, watchdog_event,
                                         &watchdog_registration);
    if (EFI_ERROR(status))
        return status;
    bind_watchdog(NULL, NULL);
    gRT->ResetSystem = reset;
    gRT->GetTime = get_time;
    gRT->SetTime = set_time;
    gRT->GetWakeupTime = get_wakeup;
    gRT->SetWakeupTime = set_wakeup;
    gRT->GetNextHighMonotonicCount = next_high;
    gBS->GetNextMonotonicCount = next_count;
    gRT->UpdateCapsule = update_capsule;
    gRT->QueryCapsuleCapabilities = query_capsule;
    EFI_HANDLE handle = NULL;
    status = gBS->InstallMultipleProtocolInterfaces(
        &handle, &gEfiResetArchProtocolGuid, NULL, &gEfiRealTimeClockArchProtocolGuid, NULL,
        &gEfiMonotonicCounterArchProtocolGuid, NULL, &gEfiCapsuleArchProtocolGuid, NULL, NULL);
    DebugPrint(DEBUG_INFO, "runtime Reset/RTC/Monotonic/Capsule: %r\n", status);
    return status;
}
