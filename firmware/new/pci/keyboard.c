/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Publish only the guest-owned i8042; the pinned driver performs keyboard I/O. */
#include "../platform.h"
#include <Protocol/SuperIo.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
static UINT8 keyboard_resources[] = {0x4b, 0x60, 0, 1, 0x4b, 0x64, 0, 1, 0x22, 2, 0, 0x79, 0};
static struct {
    ACPI_HID_DEVICE_PATH acpi;
    EFI_DEVICE_PATH_PROTOCOL end;
} keyboard_path = {
    {{ACPI_DEVICE_PATH, ACPI_DP, {sizeof(ACPI_HID_DEVICE_PATH), 0}}, EISA_PNP_ID(0x303), 0},
    {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0}}};
static EFI_STATUS EFIAPI register_access(const EFI_SIO_PROTOCOL *self, BOOLEAN write, BOOLEAN exit,
                                         UINT8 reg, UINT8 *value)
{
    (void)self;
    (void)write;
    (void)exit;
    (void)reg;
    return value ? EFI_UNSUPPORTED : EFI_INVALID_PARAMETER;
}
static EFI_STATUS EFIAPI get_resources(const EFI_SIO_PROTOCOL *self, ACPI_RESOURCE_HEADER_PTR *list)
{
    (void)self;
    if (!list)
        return EFI_INVALID_PARAMETER;
    list->SmallHeader = (VOID *)keyboard_resources;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI set_resources(const EFI_SIO_PROTOCOL *self, ACPI_RESOURCE_HEADER_PTR list)
{
    (void)self;
    if (!list.SmallHeader)
        return EFI_INVALID_PARAMETER;
    return CompareMem(list.SmallHeader, keyboard_resources, sizeof(keyboard_resources))
               ? EFI_UNSUPPORTED
               : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI modify(const EFI_SIO_PROTOCOL *self,
                                const EFI_SIO_REGISTER_MODIFY *commands, UINTN count)
{
    (void)self;
    (void)count;
    return commands ? EFI_UNSUPPORTED : EFI_INVALID_PARAMETER;
}
static EFI_SIO_PROTOCOL sio = {register_access, get_resources, set_resources, get_resources,
                               modify};
EFI_STATUS EFIAPI VmmKeyboardEntry(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    (void)image;
    (void)system;
    if (!(platform_handoff_hob()->flags & IA64VMM_HF_I8042_ENABLED))
        return EFI_SUCCESS;
    EFI_HANDLE handle = NULL;
    EFI_STATUS status = gBS->InstallMultipleProtocolInterfaces(
        &handle, &gEfiSioProtocolGuid, &sio, &gEfiDevicePathProtocolGuid, &keyboard_path, NULL);
    DebugPrint(DEBUG_INFO, "i8042 guest-owned: %r\n", status);
    return status;
}
