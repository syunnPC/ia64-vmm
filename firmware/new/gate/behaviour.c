/* SPDX-License-Identifier: GPL-2.0-or-later */
/* UEFI behaviour probe. Runs separately from the unmodified hello golden. */
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/GraphicsOutput.h>
#include <ia64vmm/platform.h>
#include "runtime-alias.h"
static const EFI_GUID guid = {
    0xe25690e8, 0x4d1e, 0x4a20, {0xad, 0xb5, 0x47, 0x32, 0x54, 0x45, 0x53, 0x54}};
static UINTN notifications;
extern VOID gate_enable_translation(VOID);
static VOID text(const CHAR8 *s)
{
    while (*s)
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_CONSOLE) = (UINT8)*s++;
}
static VOID check(BOOLEAN condition, const CHAR8 *reason)
{
    if (!condition) {
        text("UEFI behaviour FAIL: ");
        text(reason);
        text("\n");
        gRT->ResetSystem(EfiResetShutdown, EFI_ABORTED, 0, NULL);
        for (;;)
            __asm__ volatile("hint @pause");
    }
}
static UINT64 counter(VOID)
{
    UINT64 value;
    __asm__ volatile("mov %0=ar.itc" : "=r"(value));
    return value;
}
static VOID EFIAPI notify(EFI_EVENT event, VOID *context)
{
    (void)event;
    (void)context;
    ++notifications;
}
/* Reuse the frozen Gate application entry/constructor recipe. */
EFI_STATUS EFIAPI GateHello(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    (void)system;
    /* SAL hand-off supplies valid region-register page sizes for the
     * ITIR.ps delivered on an Alternate TLB miss (sal-handoff.h). */
    for (UINT64 region = 0; region < 8; ++region) {
        UINT64 rr;
        __asm__ volatile("mov %0=rr[%1]" : "=r"(rr) : "r"(region << 61));
        check(rr == (0x100030 | region << 8),
              "SAL hand-off region register (RID 0x1000+n, 4 KiB, walker off)");
    }
    UINT64 pta;
    __asm__ volatile("mov %0=cr.pta" : "=r"(pta));
    check(pta == 0x3c, "SAL hand-off PTA 0x3c");
    EFI_INPUT_KEY key_data;
    check(gST->ConIn && gST->ConIn->WaitForKey &&
              gST->ConIn->ReadKeyStroke(gST->ConIn, &key_data) == EFI_NOT_READY,
          "empty ConIn");
    static const EFI_GUID input_ex_guid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *input_ex;
    check(gBS->HandleProtocol(gST->ConsoleInHandle, (EFI_GUID *)&input_ex_guid,
                              (VOID **)&input_ex) == EFI_SUCCESS,
          "ConSplitter InputEx");
    EFI_KEY_DATA extended_key;
    check(input_ex->WaitForKeyEx &&
              input_ex->ReadKeyStrokeEx(input_ex, &extended_key) == EFI_NOT_READY,
          "empty ConInEx");
    EFI_TIME time;
    check(gRT->GetTime(&time, NULL) == EFI_SUCCESS && time.Year >= 2020 && time.Month >= 1 &&
              time.Month <= 12,
          "GetTime");
    UINT32 attrs =
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
    UINT64 value = 0x4732564152544553ULL, got = 0;
    UINTN size = sizeof(got);
    UINT32 returned_attrs = 0;
    check(gRT->SetVariable(L"BehaviourRoundTrip", (EFI_GUID *)&guid, attrs, sizeof(value),
                           &value) == EFI_SUCCESS,
          "SetVariable NV");
    check(gRT->GetVariable(L"BehaviourRoundTrip", (EFI_GUID *)&guid, &returned_attrs, &size,
                           &got) == EFI_SUCCESS &&
              value == got && returned_attrs == attrs && size == sizeof(value),
          "GetVariable round trip");
    EFI_STATUS status = gRT->SetVariable(L"BehaviourAuth", (EFI_GUID *)&guid,
                                         attrs | EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS,
                                         sizeof(value), &value);
    /* UDK2018 Variable.c rejects authenticated attributes with INVALID_PARAMETER
     * when AuthSupport is false, before parsing any authentication header. */
    check(status == EFI_INVALID_PARAMETER, "UDK2018 authenticated-attribute rejection");
    UINT64 maximum, remaining, max_size;
    check(gRT->QueryVariableInfo(attrs, &maximum, &remaining, &max_size) == EFI_SUCCESS &&
              remaining > 1024 && max_size >= 512,
          "QueryVariableInfo");
    UINT8 payload[512];
    for (UINTN generation = 0; generation < 64; ++generation) {
        SetMem(payload, sizeof(payload), (UINT8)generation);
        check(gRT->SetVariable(L"BehaviourReclaim", (EFI_GUID *)&guid, attrs, sizeof(payload),
                               payload) == EFI_SUCCESS,
              "variable reclaim/FTW update");
    }
    size = sizeof(payload);
    check(gRT->GetVariable(L"BehaviourReclaim", (EFI_GUID *)&guid, NULL, &size, payload) ==
                  EFI_SUCCESS &&
              payload[0] == 63 && payload[511] == 63,
          "variable reclaim round trip");
    check(gRT->SetVariable(L"BehaviourReclaim", (EFI_GUID *)&guid, 0, 0, NULL) == EFI_SUCCESS,
          "variable deletion");
    EFI_EVENT periodic, deadline;
    check(gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, notify, NULL, &periodic) ==
              EFI_SUCCESS,
          "periodic event");
    check(gBS->SetTimer(periodic, TimerPeriodic, 1000000) == EFI_SUCCESS, "periodic timer");
    UINT64 start = counter();
    check(gBS->Stall(300000) == EFI_SUCCESS && counter() - start >= IA64VMM_ITC_HZ * 3 / 10,
          "Stall minimum accuracy");
    check(notifications >= 2, "ITM timer notifications");
    check(gBS->SetTimer(periodic, TimerCancel, 0) == EFI_SUCCESS, "timer cancel");
    check(gBS->CloseEvent(periodic) == EFI_SUCCESS, "timer close");
    check(gBS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &deadline) == EFI_SUCCESS, "wait event");
    check(gBS->SetTimer(deadline, TimerRelative, 500000) == EFI_SUCCESS, "relative timer");
    UINTN index = 99;
    check(gBS->WaitForEvent(1, &deadline, &index) == EFI_SUCCESS && index == 0,
          "WaitForEvent PAL idle");
    check(gBS->CloseEvent(deadline) == EFI_SUCCESS, "relative timer close");
    text(
        "UEFI behaviour: RTC, variables, authenticated rejection, ITM events, Stall, PAL idle passed\n");
    UINTN map_size = 0, key, descriptor_size;
    UINT32 version;
    check(gBS->GetMemoryMap(&map_size, NULL, &key, &descriptor_size, &version) ==
              EFI_BUFFER_TOO_SMALL,
          "memory map size");
    map_size += 16 * descriptor_size;
    EFI_MEMORY_DESCRIPTOR *map;
    check(gBS->AllocatePool(EfiLoaderData, map_size, (VOID **)&map) == EFI_SUCCESS,
          "memory map allocation");
    UINTN capacity = map_size;
    check(gBS->GetMemoryMap(&map_size, map, &key, &descriptor_size, &version) == EFI_SUCCESS,
          "memory map");
    EFI_PHYSICAL_ADDRESS page;
    check(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &page) == EFI_SUCCESS,
          "invalidate map key");
    check(gBS->ExitBootServices(image, key) == EFI_INVALID_PARAMETER, "stale map key rejection");
    map_size = capacity;
    check(gBS->GetMemoryMap(&map_size, map, &key, &descriptor_size, &version) == EFI_SUCCESS,
          "second memory map");
    /* IA-64 exposes one memory-mapped I/O port translation window. */
    UINTN port_space = 0;
    for (UINTN at = 0; at < map_size; at += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (VOID *)((UINT8 *)map + at);
        if (d->Type != EfiMemoryMappedIOPortSpace)
            continue;
        ++port_space;
        check(d->PhysicalStart == IA64VMM_LEGACY_IO_BASE &&
                  d->NumberOfPages * 4096 == IA64VMM_LEGACY_IO_SIZE &&
                  d->Attribute == (EFI_MEMORY_UC | EFI_MEMORY_RUNTIME),
              "I/O port space descriptor");
    }
    check(port_space == 1, "one EfiMemoryMappedIOPortSpace descriptor");
    /* The GOP framebuffer (outside any BAR here) as a UC EfiMemoryMappedIO
     * descriptor without EFI_MEMORY_RUNTIME, as the reference lists it. */
    static const EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    check(gBS->LocateProtocol((EFI_GUID *)&gop_guid, NULL, (VOID **)&gop) == EFI_SUCCESS, "GOP");
    UINT64 fb = gop->Mode->FrameBufferBase, fb_end = fb + gop->Mode->FrameBufferSize;
    BOOLEAN fb_listed = FALSE;
    for (UINTN at = 0; at < map_size; at += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (VOID *)((UINT8 *)map + at);
        if (d->Type == EfiMemoryMappedIO && d->Attribute == EFI_MEMORY_UC &&
            d->PhysicalStart <= fb && fb_end <= d->PhysicalStart + d->NumberOfPages * 4096)
            fb_listed = TRUE;
    }
    check(fb_listed, "framebuffer as UC EfiMemoryMappedIO");
    UINT64 pinned[6], pinned_bytes[6];
    UINTN pinned_shift[6], pinned_count = 0, high_count = 0;
    for (UINTN at = 0; at < map_size; at += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (VOID *)((UINT8 *)map + at);
        if (!(d->Attribute & EFI_MEMORY_RUNTIME) || !(d->Attribute & EFI_MEMORY_WB))
            continue;
        UINT64 end = d->PhysicalStart + d->NumberOfPages * 4096;
        for (UINT64 address = d->PhysicalStart; address < end;) {
            BOOLEAN already = FALSE;
            for (UINTN j = 0; j < pinned_count; ++j)
                if (pinned[j] <= address && address < pinned[j] + pinned_bytes[j]) {
                    address = pinned[j] + pinned_bytes[j];
                    already = TRUE;
                    break;
                }
            if (already)
                continue;
            UINTN shift = alias_shift(address, map, map_size, descriptor_size);
            check(shift != 0, "runtime WB descriptor overlaps MMIO");
            UINT64 bytes = 1ULL << shift, base = address & ~(bytes - 1);
            check(pinned_count < 6, "runtime aliases exceed loader TR capacity");
            pinned[pinned_count] = base;
            pinned_bytes[pinned_count] = bytes;
            pinned_shift[pinned_count++] = shift;
            if (base >= 0x40000000ULL)
                ++high_count;
            address = base + bytes;
        }
    }
    UINT64 stack, backing;
    __asm__ volatile("mov %0=r12; mov %1=ar.bsp" : "=r"(stack), "=r"(backing));
    UINTN stack_shift = alias_shift(stack, map, map_size, descriptor_size);
    check(stack_shift != 0, "loader stack overlaps MMIO");
    UINT64 stack_base = stack & ~((1ULL << stack_shift) - 1);
    check(backing >= stack_base && backing < stack_base + (1ULL << stack_shift),
          "loader stack/backing TR span");
    check(gBS->ExitBootServices(image, key) == EFI_SUCCESS, "ExitBootServices retry");
    text("UEFI behaviour: ExitBootServices succeeded\n");
    for (UINTN at = 0; at < map_size; at += descriptor_size) {
        EFI_MEMORY_DESCRIPTOR *d = (VOID *)((UINT8 *)map + at);
        d->VirtualStart = (d->Attribute & EFI_MEMORY_RUNTIME)
                              ? d->PhysicalStart + 0xe000000000000000ULL
                              : d->PhysicalStart;
    }
    /* Act as a loader: install an independent region-7 RID and pinned
     * aliases for low RAM and the service/NVRAM window. Firmware's IVA
     * supplies only region-zero identity refills, by design. */
    UINT64 region = 0xe000000000000000ULL, rr = (7 << 8) | (12 << 2), itir = 30 << 2, pte = 0x661,
           tr = 0;
    UINT64 iva;
    __asm__ volatile("mov %0=cr.iva" : "=r"(iva));
    __asm__ volatile("rsm psr.ic;; srlz.d;;" ::: "memory");
    __asm__ volatile("mov rr[%0]=%1;; srlz.d;;" ::"r"(region), "r"(rr) : "memory");
    region = 0xe000000000000000ULL + (IA64VMM_SERVICE_BASE & ~0xffffffULL);
    itir = 24 << 2;
    pte = (IA64VMM_SERVICE_BASE & ~0xffffffULL) | 0x671;
    tr = 1;
    __asm__ volatile("mov cr.ifa=%0; mov cr.itir=%1;; itr.d dtr[%2]=%3;; srlz.d;;" ::"r"(region),
                     "r"(itir), "r"(tr), "r"(pte)
                     : "memory");
    /* Pin only device-free WB spans; TR1 is UC service/NVRAM, TR2 holds
     * the identity IVT and stack. Runtime allocations may be above 1 GiB. */
    for (UINTN j = 0; j < pinned_count; ++j) {
        tr = j ? j + 2 : 0;
        region = 0xe000000000000000ULL + pinned[j];
        itir = pinned_shift[j] << 2;
        pte = pinned[j] | 0x661;
        __asm__ volatile(
            "mov cr.ifa=%0; mov cr.itir=%1;; itr.i itr[%2]=%3;; srlz.i;; itr.d dtr[%2]=%3;; srlz.d;;" ::
                "r"(region),
            "r"(itir), "r"(tr), "r"(pte)
            : "memory");
    }
    /* Interruption entry preserves translation enable bits. Pin the IVT and
     * adjacent refill code before the first translated fetch can miss. Pin
     * low RAM for the test loader's RBS/stack; app code exercises region-zero refills. */
    region = iva & ~0x3ffffULL;
    itir = 18 << 2;
    pte = region | 0x661;
    tr = 2;
    __asm__ volatile("mov cr.ifa=%0; mov cr.itir=%1;; itr.i itr[%2]=%3;; srlz.i;;" ::"r"(region),
                     "r"(itir), "r"(tr), "r"(pte)
                     : "memory");
    region = stack_base;
    itir = stack_shift << 2;
    pte = stack_base | 0x661;
    __asm__ volatile("mov cr.ifa=%0; mov cr.itir=%1;; itr.d dtr[%2]=%3;; srlz.d;;" ::"r"(region),
                     "r"(itir), "r"(tr), "r"(pte)
                     : "memory");
    __asm__ volatile("ssm psr.ic;; srlz.d;;" ::: "memory");
    text("UEFI behaviour: high runtime aliases pinned=");
    CHAR8 digits[21];
    UINTN used = 0;
    do {
        digits[used++] = (CHAR8)('0' + high_count % 10);
        high_count /= 10;
    } while (high_count);
    while (used) {
        CHAR8 one[2] = {digits[--used], 0};
        text(one);
    }
    text("\n");
    text("UEFI behaviour: calling SetVirtualAddressMap\n");
    check(gRT->SetVirtualAddressMap(map_size, descriptor_size, version, map) == EFI_SUCCESS,
          "SetVirtualAddressMap region 7");
    text("UEFI behaviour: virtual relocation succeeded\n");
    gate_enable_translation();
    text("UEFI behaviour: loader translation enabled\n");
    check(gRT->GetTime(&time, NULL) == EFI_SUCCESS, "GetTime after virtual map");
    got = 0;
    size = sizeof(got);
    check(gRT->GetVariable(L"BehaviourRoundTrip", (EFI_GUID *)&guid, NULL, &size, &got) ==
                  EFI_SUCCESS &&
              got == value,
          "GetVariable after virtual map");
    value++;
    check(gRT->SetVariable(L"BehaviourRoundTrip", (EFI_GUID *)&guid, attrs, sizeof(value),
                           &value) == EFI_SUCCESS,
          "SetVariable through relocated FVB");
    got = 0;
    size = sizeof(got);
    check(gRT->GetVariable(L"BehaviourRoundTrip", (EFI_GUID *)&guid, NULL, &size, &got) ==
                  EFI_SUCCESS &&
              got == value,
          "GetVariable after virtual write");
    text(
        "UEFI behaviour: map key, ExitBootServices retry, nonidentity virtual map and runtime calls passed\n");
    gRT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    return EFI_ABORTED;
}
