/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform.h"
#include "ap.h"
#include "ivt-layout.h"
#include <Library/PlatformBootManagerLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Guid/GlobalVariable.h>
#include <Guid/EventGroup.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/GraphicsOutput.h>
#include <Guid/ConsoleInDevice.h>
#include <Guid/ConsoleOutDevice.h>
#include <Guid/StandardErrorDevice.h>
#include <Protocol/LoadedImage.h>
#include <IndustryStandard/PeImage.h>
static EFI_BOOT_MANAGER_LOAD_OPTION boot_option;
static struct {
    MEMMAP_DEVICE_PATH memory;
    EFI_DEVICE_PATH_PROTOCOL end;
} boot_path;
VOID observe_boot_media(VOID);
static VOID media_boot(VOID);
static VOID input_observe(VOID);
static VOID map_observe(VOID);
static VOID fatal(const CHAR8 *reason, EFI_STATUS status)
{
    DebugPrint(DEBUG_ERROR, "BDS failure: %a: %r\n", reason, status);
    gRT->ResetSystem(EfiResetShutdown, status, 0, NULL);
    for (;;)
        __asm__ volatile("hint @pause");
}
VOID EFIAPI PlatformBootManagerBeforeConsole(VOID)
{
    const struct ia64vmm_handoff *h = platform_handoff_hob();
    /* Vendor-defined firmware revision of this firmware (1.0). */
    gST->FirmwareRevision = 0x10000;
    gST->Hdr.CRC32 = 0;
    gBS->CalculateCrc32(gST, gST->Hdr.HeaderSize, &gST->Hdr.CRC32);
    DebugPrint(DEBUG_INFO, "BDS: handoff kind=%u flags=%x\n", h->boot_image_kind, h->flags);
    /* Applications started from here may wake APs (SAL and PIB),
     * so every CPU's area must already be published (acpi/driver.c). */
    const volatile struct ap_wake_block *ap = (const VOID *)(UINTN)((
        const UINT64 *)(UINTN)(h->fw_data_base + PLATFORM_CONTROL_OFFSET))[CONTROL_ApBlock / 8];
    if (!ap || __atomic_load_n(&ap->count, __ATOMIC_ACQUIRE) != h->cpu_count || !ap->percpu_base ||
        ap->percpu_stride != PERCPU_STRIDE)
        fatal("AP areas not published before BDS", EFI_NOT_READY);
    if (h->boot_image_kind == IA64VMM_BOOT_DISK_IMAGE)
        fatal("disk-image modules unsupported; boot real media", EFI_UNSUPPORTED);
    EfiBootManagerConnectAll();
    UINT16 timeout = h->flags & IA64VMM_HF_NO_BOOT_MENU_WAIT ? 0 : 3;
    EFI_STATUS status = gRT->SetVariable(
        L"Timeout", &gEfiGlobalVariableGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(timeout), &timeout);
    if (EFI_ERROR(status))
        fatal("Timeout variable", status);
    EFI_HANDLE *handles;
    UINTN count;
    status = gBS->LocateHandleBuffer(ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &count,
                                     &handles);
    if (!EFI_ERROR(status)) {
        for (UINTN i = 0; i < count; ++i) {
            EFI_STATUS connected = gBS->ConnectController(handles[i], NULL, NULL, TRUE);
            DebugPrint(DEBUG_INFO, "GOP connect: %r\n", connected);
        }
        FreePool(handles);
    }
    EFI_GUID *protocols[] = {&gEfiSimpleTextOutProtocolGuid, &gEfiSimpleTextInProtocolGuid};
    for (UINTN p = 0; p < 2; ++p) {
        status = gBS->LocateHandleBuffer(ByProtocol, protocols[p], NULL, &count, &handles);
        if (EFI_ERROR(status))
            continue;
        for (UINTN i = 0; i < count; ++i) {
            EFI_DEVICE_PATH_PROTOCOL *path = DevicePathFromHandle(handles[i]);
            if (path) {
                /* These fixed platform consoles are selected unconditionally.
                 * ConSplitter consumes the marker protocols normally installed
                 * by ConPlatformDxe after consulting console variables. */
                static const EFI_GUID out = EFI_CONSOLE_OUT_DEVICE_GUID;
                static const EFI_GUID in = EFI_CONSOLE_IN_DEVICE_GUID;
                static const EFI_GUID err = EFI_STANDARD_ERROR_DEVICE_GUID;
                EFI_GUID *marker = (EFI_GUID *)(p ? &in : &out);
                VOID *unused;
                if (gBS->HandleProtocol(handles[i], marker, &unused) == EFI_UNSUPPORTED) {
                    status = gBS->InstallProtocolInterface(&handles[i], marker,
                                                           EFI_NATIVE_INTERFACE, NULL);
                    if (EFI_ERROR(status))
                        fatal("console marker", status);
                }
                if (!p &&
                    gBS->HandleProtocol(handles[i], (EFI_GUID *)&err, &unused) == EFI_UNSUPPORTED)
                    gBS->InstallProtocolInterface(&handles[i], (EFI_GUID *)&err,
                                                  EFI_NATIVE_INTERFACE, NULL);
                EfiBootManagerUpdateConsoleVariable(p == 0 ? ConOut : ConIn, path, NULL);
                if (!p)
                    EfiBootManagerUpdateConsoleVariable(ErrOut, path, NULL);
                EFI_STATUS connected = gBS->ConnectController(handles[i], NULL, NULL, TRUE);
                DebugPrint(DEBUG_INFO, "console connect p=%u: %r\n", (UINT32)p, connected);
            }
        }
        FreePool(handles);
    }
    if (h->boot_image_kind == IA64VMM_BOOT_NONE)
        return;
    if (h->boot_image_kind != IA64VMM_BOOT_PE_IMAGE || !h->boot_image_size)
        fatal("invalid handoff boot image", EFI_UNSUPPORTED);
    boot_path.memory.Header.Type = HARDWARE_DEVICE_PATH;
    boot_path.memory.Header.SubType = HW_MEMMAP_DP;
    boot_path.memory.Header.Length[0] = sizeof(MEMMAP_DEVICE_PATH);
    boot_path.memory.MemoryType = EfiLoaderCode;
    boot_path.memory.StartingAddress = h->boot_image_base;
    boot_path.memory.EndingAddress = h->boot_image_base + h->boot_image_size - 1;
    SetDevicePathEndNode(&boot_path.end);
    status = EfiBootManagerInitializeLoadOption(&boot_option, 0, LoadOptionTypeBoot,
                                                LOAD_OPTION_ACTIVE, L"IA64 VMM handoff",
                                                (EFI_DEVICE_PATH_PROTOCOL *)&boot_path, NULL, 0);
    if (EFI_ERROR(status))
        fatal("boot option initialization", status);
    /* Nonvolatile variables survive a restart and a VMM run: when BootOrder
     * already names 0000, EDK2's AddLoadOptionVariable returns EFI_ALREADY_STARTED without writing Boot0000, which may hold
     * an earlier boot's option. This boot's option replaces it. */
    status = EfiBootManagerAddLoadOptionVariable(&boot_option, 0);
    if (status == EFI_ALREADY_STARTED)
        status = EfiBootManagerLoadOptionToVariable(&boot_option);
    if (EFI_ERROR(status))
        fatal("Boot0000/BootOrder variables", status);
}
static VOID EFIAPI event_noop(EFI_EVENT event, VOID *context)
{
    (void)event;
    (void)context;
}
VOID EFIAPI PlatformBootManagerAfterConsole(VOID)
{
    const struct ia64vmm_handoff *h = platform_handoff_hob();
    input_observe();
    observe_boot_media();
    map_observe();
    EFI_STATUS status;
    EFI_HANDLE child;
    static const EFI_GUID end_dxe = EFI_END_OF_DXE_EVENT_GROUP_GUID;
    EFI_EVENT event;
    status =
        gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, event_noop, NULL, &end_dxe, &event);
    if (EFI_ERROR(status))
        fatal("EndOfDxe event", status);
    gBS->SignalEvent(event);
    gBS->CloseEvent(event);
    if (h->boot_image_kind == IA64VMM_BOOT_NONE) {
        media_boot();
        return;
    }
    if (h->boot_image_kind != IA64VMM_BOOT_PE_IMAGE)
        return;
    EfiSignalEventReadyToBoot();
    if (!(h->flags & IA64VMM_HF_NO_BOOT_MENU_WAIT)) {
        status = gBS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &event);
        if (EFI_ERROR(status))
            fatal("boot timeout event", status);
        gBS->SetTimer(event, TimerRelative, 30000000);
        UINTN index;
        gBS->WaitForEvent(1, &event, &index);
        gBS->CloseEvent(event);
    }
    UINT16 current = (UINT16)boot_option.OptionNumber;
    status = gRT->SetVariable(L"BootCurrent", &gEfiGlobalVariableGuid,
                              EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                              sizeof(current), &current);
    if (EFI_ERROR(status))
        fatal("BootCurrent variable", status);
    DebugPrint(DEBUG_INFO, "BDS: LoadImage handoff buffer\n");
    status = gBS->LoadImage(FALSE, gImageHandle, (EFI_DEVICE_PATH_PROTOCOL *)&boot_path,
                            (VOID *)(UINTN)h->boot_image_base, (UINTN)h->boot_image_size, &child);
    if (EFI_ERROR(status))
        fatal("LoadImage", status);
    gBS->SetWatchdogTimer(300, 0, 0, NULL);
    DebugPrint(DEBUG_INFO, "BDS: StartImage\n");
    status = gBS->StartImage(child, NULL, NULL);
    gBS->SetWatchdogTimer(0, 0, 0, NULL);
    DebugPrint(DEBUG_INFO, "BDS: boot image returned %r\n", status);
    EFI_HANDLE *graphics;
    UINTN graphics_count;
    if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL,
                                           &graphics_count, &graphics))) {
        for (UINTN i = 0; i < graphics_count; ++i) {
            EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out;
            EFI_STATUS text_status =
                gBS->HandleProtocol(graphics[i], &gEfiSimpleTextOutProtocolGuid, (VOID **)&out);
            DebugPrint(DEBUG_INFO, "GOP text: %r row=%u\n", text_status,
                       EFI_ERROR(text_status) ? 0 : (UINT32)out->Mode->CursorRow);
        }
        FreePool(graphics);
    }
    if (h->flags & IA64VMM_HF_SHUTDOWN_ON_BOOT_RETURN)
        gRT->ResetSystem(EfiResetShutdown, status, 0, NULL);
    gRT->SetVariable(L"BootCurrent", &gEfiGlobalVariableGuid, 0, 0, NULL);
}
VOID EFIAPI PlatformBootManagerWaitCallback(UINT16 remaining)
{
    (void)remaining;
}

/* The pinned bus/partition/FAT drivers expose real devices. BootManagerLib
 * expands whole-disk options to filesystems, including El Torito EFI images,
 * and applies the IPF removable fallback \EFI\BOOT\BOOTIA64.EFI. */
static EFI_INPUT_READ_KEY original_read_key;
static EFI_INPUT_READ_KEY_EX original_read_key_ex;
static EFI_STATUS EFIAPI observed_read_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self, EFI_INPUT_KEY *key)
{
    platform_service(IA64VMM_SVC_CONSOLE_FLUSH, 1);
    EFI_STATUS status = original_read_key(self, key);
    if (!EFI_ERROR(status))
        platform_service(IA64VMM_SVC_INPUT_KEY, ((UINT64)key->ScanCode << 16) | key->UnicodeChar);
    return status;
}
static EFI_STATUS EFIAPI observed_read_key_ex(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *self,
                                              EFI_KEY_DATA *key)
{
    platform_service(IA64VMM_SVC_CONSOLE_FLUSH, 1);
    EFI_STATUS status = original_read_key_ex(self, key);
    if (!EFI_ERROR(status))
        platform_service(IA64VMM_SVC_INPUT_KEY,
                         ((UINT64)key->Key.ScanCode << 16) | key->Key.UnicodeChar);
    return status;
}
/* The MMIO and I/O port descriptors of the EFI map the boot image will see,
 * one line each, so a run's log shows what the OS loader gets. */
static VOID map_observe(VOID)
{
    UINTN size = 0, key, descriptor_size;
    UINT32 version;
    if (gBS->GetMemoryMap(&size, NULL, &key, &descriptor_size, &version) != EFI_BUFFER_TOO_SMALL)
        return;
    size += 8 * descriptor_size;
    EFI_MEMORY_DESCRIPTOR *map = AllocatePool(size);
    if (!map)
        return;
    if (!EFI_ERROR(gBS->GetMemoryMap(&size, map, &key, &descriptor_size, &version)))
        for (UINTN at = 0; at < size; at += descriptor_size) {
            const EFI_MEMORY_DESCRIPTOR *d = (VOID *)((UINT8 *)map + at);
            if (d->Type == EfiMemoryMappedIO || d->Type == EfiMemoryMappedIOPortSpace)
                DebugPrint(DEBUG_INFO, "BDS: map %a %lx pages=%lx attr=%lx\n",
                           d->Type == EfiMemoryMappedIO ? "MMIO" : "port", d->PhysicalStart,
                           d->NumberOfPages, d->Attribute);
        }
    FreePool(map);
}
static VOID input_observe(VOID)
{
    if (!original_read_key && gST->ConIn) {
        original_read_key = gST->ConIn->ReadKeyStroke;
        gST->ConIn->ReadKeyStroke = observed_read_key;
    }
    /* GRUB prefers InputEx. Observe the splitter on ConsoleInHandle, so
     * consumption is recorded once and newline-free menus flush promptly. */
    static EFI_GUID input_ex = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *protocol;
    if (!original_read_key_ex &&
        !EFI_ERROR(gBS->HandleProtocol(gST->ConsoleInHandle, &input_ex, (VOID **)&protocol))) {
        original_read_key_ex = protocol->ReadKeyStrokeEx;
        protocol->ReadKeyStrokeEx = observed_read_key_ex;
    }
}
static VOID media_boot(VOID)
{
    const struct ia64vmm_handoff *h = platform_handoff_hob();
    EfiBootManagerRefreshAllBootOption();
    UINTN count = 0;
    EFI_BOOT_MANAGER_LOAD_OPTION *options =
        EfiBootManagerGetLoadOptions(&count, LoadOptionTypeBoot);
    if (!count)
        fatal("no bootable media", EFI_NOT_FOUND);
    Print(L"\r\nIA-64 VMM boot menu\r\n");
    DebugPrint(DEBUG_INFO, "BDS: %u boot options\n", (UINT32)count);
    for (UINTN i = 0; i < count; ++i) {
        Print(L"  %u: %s\r\n", i + 1, options[i].Description);
        DebugPrint(DEBUG_INFO, "BDS: Boot%04x %s\n", (UINT32)options[i].OptionNumber,
                   options[i].Description);
    }
    UINTN selected = 0;
    if (!(h->flags & IA64VMM_HF_NO_BOOT_MENU_WAIT)) {
        Print(L"Up/Down selects, Enter boots; automatic boot in 3 seconds.\r\n");
        EFI_EVENT timer;
        EFI_STATUS status = gBS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &timer);
        if (EFI_ERROR(status))
            fatal("menu timer", status);
        gBS->SetTimer(timer, TimerRelative, 30000000);
        EFI_EVENT events[2] = {gST->ConIn->WaitForKey, timer};
        for (;;) {
            UINTN index;
            status = gBS->WaitForEvent(2, events, &index);
            if (EFI_ERROR(status) || index == 1)
                break;
            EFI_INPUT_KEY key;
            if (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key)))
                continue;
            /* First input cancels autoboot so browsing never races its deadline. */
            gBS->SetTimer(timer, TimerCancel, 0);
            if (key.UnicodeChar == CHAR_CARRIAGE_RETURN)
                break;
            if (key.ScanCode == SCAN_DOWN)
                selected = (selected + 1) % count;
            if (key.ScanCode == SCAN_UP)
                selected = (selected + count - 1) % count;
            Print(L"Selected %u: %s\r\n", selected + 1, options[selected].Description);
        }
        gBS->CloseEvent(timer);
    }
    for (UINTN tried = 0; tried < count; ++tried) {
        UINTN i = (selected + tried) % count;
        DebugPrint(DEBUG_INFO, "BDS: trying Boot%04x\n", (UINT32)options[i].OptionNumber);
        EfiBootManagerBoot(&options[i]);
        DebugPrint(DEBUG_INFO, "BDS: boot image returned %r\n", options[i].Status);
        if (!EFI_ERROR(options[i].Status) && (h->flags & IA64VMM_HF_SHUTDOWN_ON_BOOT_RETURN))
            gRT->ResetSystem(EfiResetShutdown, options[i].Status, 0, NULL);
    }
    EfiBootManagerFreeLoadOptions(options, count);
}
