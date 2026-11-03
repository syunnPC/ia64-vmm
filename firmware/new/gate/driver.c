/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Library/UefiLib.h>

static UINTN value = 73;
static UINTN *volatile pointer = &value;
static UINTN read_value(VOID)
{
    return *pointer;
}
static UINTN (*volatile functions[])(VOID) = {read_value};

EFI_STATUS EFIAPI GateDriver(EFI_HANDLE image, EFI_SYSTEM_TABLE *table)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_STATUS status =
        table->BootServices->HandleProtocol(image, &gEfiLoadedImageProtocolGuid, (VOID **)&loaded);
    if (EFI_ERROR(status))
        return status;
    if ((UINTN)loaded->ImageBase == 0x04000000)
        return EFI_DEVICE_ERROR;
    if (pointer != &value || functions[0]() != 73)
        return EFI_DEVICE_ERROR;
    Print(L"gate DXE driver: entry reached, pointers=73\r\n");
    Print(L"gate DXE driver: preferred=4000000 actual=%lx\r\n", (UINTN)loaded->ImageBase);
    return EFI_SUCCESS;
}
