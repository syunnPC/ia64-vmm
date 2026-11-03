/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Isolate firmware text/font handoff from a kernel's console selection. */
#include <efi.h>
#include "../../../include/ia64vmm/platform.h"
static void say(const char *s)
{
    while (*s)
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_CONSOLE) = (unsigned char)*s++;
}
static void hex(UINT64 n)
{
    const char *digits = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        char s[2] = {digits[(n >> shift) & 15], 0};
        say(s);
    }
}
EFI_STATUS EFIAPI _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    UINT64 flags = *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_DEBUG_FLAGS);
    if (!(flags & IA64VMM_HF_LEGACY_VGA))
        return EFI_UNSUPPORTED;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    UINT64 linear = 0;
    if (!EFI_ERROR(st->BootServices->LocateProtocol(&gop_guid, 0, (VOID **)&gop)))
        linear = gop->Mode->FrameBufferBase;
    UINTN size = 65536, key, stride;
    UINT32 version;
    VOID *map;
    EFI_STATUS status = st->BootServices->AllocatePool(EfiLoaderData, size, &map);
    if (EFI_ERROR(status))
        return status;
    for (unsigned retry = 0; retry < 2; ++retry) {
        size = 65536;
        status = st->BootServices->GetMemoryMap(&size, map, &key, &stride, &version);
        if (EFI_ERROR(status))
            return status;
        status = st->BootServices->ExitBootServices(image, key);
        if (!EFI_ERROR(status))
            break;
    }
    if (EFI_ERROR(status))
        return status;
    volatile UINT16 *cells = (void *)(UINTN)0xb8000;
    for (unsigned i = 0; i < 80 * 25; ++i)
        cells[i] = 0x1f20;
    const char *message = "real VGA text/font handoff";
    for (unsigned i = 0; message[i]; ++i)
        cells[i] = 0x1f00 | (unsigned char)message[i];
    say("VGA text probe ready\n");
    if (cells[0] != 0x1f44)
        say("VGA cell readback failed\n");
    if (linear) {
        say("VGA linear text=");
        hex(*(volatile UINT16 *)(UINTN)linear);
        say(" font=");
        hex(*(volatile UINT8 *)(UINTN)(linear + (65 * 32 + 3) * 4 + 2));
        say(" flatfont=");
        hex(*(volatile UINT8 *)(UINTN)(linear + 65 * 32 + 3));
        say(" flattext=");
        hex(*(volatile UINT16 *)(UINTN)(linear + 0x18000));
        say("\n");
    }
    UINT64 start, now;
    __asm__ volatile("mov %0=ar.itc" : "=r"(start));
    do {
        __asm__ volatile("mov %0=ar.itc" : "=r"(now));
    } while (now - start < 30 * IA64VMM_ITC_HZ);
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, 0);
    return EFI_SUCCESS;
}
