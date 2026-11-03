/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

extern CONST UINT8 gate_hello_pe[], gate_hello_pe_end[];

/* Reuse Hello's generated application entry/constructor metadata. */
EFI_STATUS EFIAPI GateHello(EFI_HANDLE image, EFI_SYSTEM_TABLE *table)
{
    EFI_PHYSICAL_ADDRESS reserved = 0x04000000;
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_HANDLE child;
    EFI_STATUS status;
    (void)table;
    status = gBS->AllocatePages(AllocateAddress, EfiLoaderData, 0x100, &reserved);
    if (EFI_ERROR(status))
        return status;
    status = gBS->LoadImage(FALSE, image, NULL, (VOID *)gate_hello_pe,
                            (UINTN)gate_hello_pe_end - (UINTN)gate_hello_pe, &child);
    if (EFI_ERROR(status))
        return status;
    status = gBS->HandleProtocol(child, &gEfiLoadedImageProtocolGuid, (VOID **)&loaded);
    if (EFI_ERROR(status))
        return status;
    if ((UINTN)loaded->ImageBase == 0x04000000)
        return EFI_DEVICE_ERROR;
    Print(L"gate forced relocation: preferred=4000000 actual=%lx\r\n", (UINTN)loaded->ImageBase);
    status = gBS->StartImage(child, NULL, NULL);
    if (EFI_ERROR(status))
        return status;
    Print(L"gate relocated app EFI_SUCCESS\r\n");
    return EFI_SUCCESS;
}
