/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

extern CONST UINT8 gate_driver_pe[];
extern CONST UINT8 gate_driver_pe_end[];
static UINTN value = 42;
static UINTN *volatile pointer = &value;
static UINTN read_value(VOID)
{
    return *pointer;
}
static UINTN (*volatile functions[])(VOID) = {read_value};

EFI_STATUS EFIAPI GateHello(EFI_HANDLE image, EFI_SYSTEM_TABLE *table)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_HANDLE driver = NULL;
    EFI_STATUS status;
    (void)table;
    Print(L"gate EDK2 HelloWorld\r\n");
    if (pointer != &value || functions[0]() != 42)
        return EFI_DEVICE_ERROR;
    status = gBS->HandleProtocol(image, &gEfiLoadedImageProtocolGuid, (VOID **)&loaded);
    if (EFI_ERROR(status))
        return status;
    Print(L"gate pointers=42, loaded=%lx\r\n", (UINTN)loaded->ImageBase);
    status = gBS->LoadImage(FALSE, image, NULL, (VOID *)gate_driver_pe,
                            (UINTN)gate_driver_pe_end - (UINTN)gate_driver_pe, &driver);
    if (EFI_ERROR(status))
        return status;
    status = gBS->StartImage(driver, NULL, NULL);
    if (EFI_ERROR(status))
        return status;
    Print(L"gate app+driver EFI_SUCCESS\r\n");
    return EFI_SUCCESS;
}
