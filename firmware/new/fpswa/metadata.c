/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Uefi.h>
/* The driver has no constructors/PCDs; its entry has the EFI image ABI. */
EFI_GUID gEfiEventVirtualAddressChangeGuid = {
    0x13fa7698, 0xc831, 0x49c7, {0x87, 0xea, 0x8f, 0x43, 0xfc, 0xc2, 0x51, 0x96}};
