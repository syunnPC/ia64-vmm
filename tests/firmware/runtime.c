/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "firmware/new/dxe/runtime.c"

static EFI_TPL current_tpl = TPL_APPLICATION;
static UINT32 saved_high;
static BOOLEAN pending, forbid_boot_services;
static EFI_STATUS variable_status;
VOID *EFIAPI ZeroMem(VOID *dest, UINTN size)
{
    return memset(dest, 0, size);
}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL tpl)
{
    assert(!forbid_boot_services && tpl >= current_tpl);
    EFI_TPL previous = current_tpl;
    current_tpl = tpl;
    return previous;
}
static VOID EFIAPI restore_tpl(EFI_TPL tpl)
{
    assert(!forbid_boot_services);
    current_tpl = tpl;
    if (pending && tpl < TPL_CALLBACK) {
        pending = FALSE;
        current_tpl = TPL_CALLBACK;
        counter_notify(counter_event, NULL);
        current_tpl = tpl;
    }
}
static EFI_STATUS EFIAPI signal_event(EFI_EVENT event)
{
    assert(!forbid_boot_services && event == counter_event);
    pending = TRUE;
    restore_tpl(current_tpl);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI set_variable(CHAR16 *name, EFI_GUID *guid, UINT32 attributes, UINTN size,
                                      VOID *data)
{
    (void)name;
    (void)guid;
    assert(forbid_boot_services || current_tpl <= TPL_NOTIFY);
    assert(size == sizeof(saved_high) && (attributes & EFI_VARIABLE_NON_VOLATILE));
    if (!EFI_ERROR(variable_status))
        memcpy(&saved_high, data, size);
    return variable_status;
}
static EFI_BOOT_SERVICES boots = {
    .RaiseTPL = raise_tpl, .RestoreTPL = restore_tpl, .SignalEvent = signal_event};
static EFI_RUNTIME_SERVICES runtimes = {.SetVariable = set_variable};
EFI_BOOT_SERVICES *gBS = &boots;
EFI_RUNTIME_SERVICES *gRT = &runtimes;

static unsigned publication_errors;
VOID EFIAPI DebugPrint(UINTN level, CONST CHAR8 *format, ...)
{
    (void)level;
    (void)format;
    ++publication_errors;
}
static unsigned map_calls, map_fail;
static struct {
    UINT64 base, size, attributes;
} published[256];
static EFI_STATUS EFIAPI map_attributes(EFI_PHYSICAL_ADDRESS base, UINT64 length, UINT64 attributes)
{
    assert(map_calls < 256);
    published[map_calls].base = base;
    published[map_calls].size = length;
    published[map_calls++].attributes = attributes;
    return map_calls == map_fail ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_DXE_SERVICES dxe = {.SetMemorySpaceAttributes = map_attributes};
EFI_DXE_SERVICES *gDS = &dxe;

static void check_publication(struct ia64vmm_handoff *h)
{
    map_calls = map_fail = 0;
    assert(publish_legacy_memory(h) == EFI_SUCCESS);
    for (UINT64 at = 0xa0000; at < 0x100000; at += 4096) {
        unsigned expected = 1, found = 0;
        for (unsigned i = 0; i < h->ram_range_count; ++i)
            if (at >= h->ram[i].base && at - h->ram[i].base < h->ram[i].size)
                expected = 0;
        for (unsigned i = 0; i < h->pci_mmio_range_count; ++i)
            if (at >= h->pci_mmio[i].base && at - h->pci_mmio[i].base < h->pci_mmio[i].size)
                expected = 0;
        for (unsigned i = 0; i < map_calls; ++i)
            if (at >= published[i].base && at - published[i].base < published[i].size) {
                ++found;
                UINT64 attr = EFI_MEMORY_UC;
                if (h->legacy_version || (at < 0xc0000 && (h->flags & IA64VMM_HF_LEGACY_VGA)))
                    attr |= EFI_MEMORY_RUNTIME;
                assert(published[i].attributes == attr);
            }
        assert(found == expected);
    }
}
static void test_vga_publication(void)
{
    struct ia64vmm_handoff h = {0};
    check_publication(&h);
    assert(map_calls == 2);
    h.flags = IA64VMM_HF_LEGACY_VGA;
    h.vga_base = IA64VMM_VGA_MEMORY_BASE;
    h.vga_size = IA64VMM_VGA_MEMORY_SIZE;
    check_publication(&h);
    map_calls = 0;
    map_fail = 1;
    assert(publish_legacy_memory(&h) == EFI_SUCCESS && map_calls == 2 && publication_errors == 1);
    h.legacy_version = IA64VMM_HANDOFF_LEGACY_VERSION;
    h.legacy_flags = IA64VMM_LEGACY_TEXT_CONSOLE;
    map_calls = 0;
    map_fail = 2;
    assert(publish_legacy_memory(&h) == EFI_DEVICE_ERROR && map_calls == 2);
    check_publication(&h);
    /* Partial overlaps on both sides of the VGA/ROM boundary. */
    h.ram_range_count = 2;
    h.ram[0] = (struct ia64vmm_ram_range){0xa8000, 0x4000};
    h.ram[1] = (struct ia64vmm_ram_range){0xd0000, 0x4000};
    h.pci_mmio_range_count = 2;
    h.pci_mmio[0] = (struct ia64vmm_ram_range){0xb0000, 0x2000};
    h.pci_mmio[1] = (struct ia64vmm_ram_range){0xc8000, 0x2000};
    check_publication(&h);
    h.legacy_version = h.legacy_flags = h.flags = 0;
    check_publication(&h);
    h.ram_range_count = 1;
    h.ram[0] = (struct ia64vmm_ram_range){0, 0x100000};
    check_publication(&h);
    assert(!map_calls);
}

int main(void)
{
    test_vga_publication();
    UINT64 registers[IA64VMM_SERVICE_SIZE / 8] = {0};
    service = registers;
    EFI_TIME time, roundtrip;
    EFI_TIME_CAPABILITIES capabilities;
    assert(get_time(NULL, NULL) == EFI_INVALID_PARAMETER);
    assert(get_time(&time, &capabilities) == EFI_SUCCESS && time.Year == 1970 && time.Month == 1 &&
           time.Day == 1);
    assert(capabilities.Resolution == 1 && time.TimeZone == EFI_UNSPECIFIED_TIMEZONE);
    time.Year = 2000;
    time.Month = 2;
    time.Day = 29;
    time.Hour = 23;
    time.Minute = 59;
    time.Second = 58;
    time.TimeZone = 540;
    time.Daylight = EFI_TIME_ADJUST_DAYLIGHT;
    assert(set_time(&time) == EFI_SUCCESS && registers[IA64VMM_SVC_RTC / 8] == 0);
    assert(get_time(&roundtrip, NULL) == EFI_SUCCESS && roundtrip.Year == 2000 &&
           roundtrip.Month == 2 && roundtrip.Day == 29 && roundtrip.Second == 58 &&
           roundtrip.TimeZone == 540 && roundtrip.Daylight == time.Daylight);
    time.Year = 1900;
    assert(set_time(&time) == EFI_INVALID_PARAMETER);
    time.Year = 1969;
    time.Month = 12;
    time.Day = 31;
    time.TimeZone = EFI_UNSPECIFIED_TIMEZONE;
    assert(set_time(&time) == EFI_SUCCESS && get_time(&roundtrip, NULL) == EFI_SUCCESS &&
           roundtrip.Year == 1969);
    time.Month = 13;
    assert(set_time(&time) == EFI_INVALID_PARAMETER);
    assert(get_wakeup(NULL, NULL, NULL) == EFI_INVALID_PARAMETER &&
           update_capsule(NULL, 1, 0) == EFI_INVALID_PARAMETER);
    monotonic = 7ULL << 32;
    counter_event = (VOID *)(UINTN)1;
    UINT64 count;
    UINT32 high;
    assert(next_count(NULL) == EFI_INVALID_PARAMETER && next_high(NULL) == EFI_INVALID_PARAMETER);
    assert(next_high(&high) == EFI_SUCCESS && high == 8 && saved_high == 8);
    assert(next_count(&count) == EFI_SUCCESS && count == (8ULL << 32) + 1);
    monotonic = (8ULL << 32) | MAX_UINT32;
    assert(next_count(&count) == EFI_SUCCESS && count == (9ULL << 32) && saved_high == 9);
    current_tpl = TPL_HIGH_LEVEL;
    monotonic = (9ULL << 32) | MAX_UINT32;
    assert(next_count(&count) == EFI_SUCCESS && count == (10ULL << 32) && pending &&
           saved_high == 9);
    restore_tpl(TPL_APPLICATION);
    assert(!pending && saved_high == 10);
    variable_status = EFI_OUT_OF_RESOURCES;
    assert(next_high(&high) == EFI_OUT_OF_RESOURCES && next_count(&count) == EFI_OUT_OF_RESOURCES);
    variable_status = EFI_SUCCESS;
    assert(next_high(&high) == EFI_SUCCESS && saved_high == 12);
    exit_boot(NULL, NULL);
    forbid_boot_services = TRUE;
    assert(next_count(&count) == EFI_UNSUPPORTED && next_high(&high) == EFI_SUCCESS && high == 13);
    monotonic = MAX_UINT64;
    assert(next_high(&high) == EFI_DEVICE_ERROR);
    puts(
        "runtime: RTC calendar/offset validation, monotonic rollover TPL, failures and runtime services passed");
    return 0;
}
