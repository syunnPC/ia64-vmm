/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PI FVB2, 16 KiB variable FV, 16 KiB FTW workspace, 32 KiB spare.
 * The service window is byte-addressable RAM; emulate NOR erase polarity
 * here so upstream FTW's state-bit transitions retain their semantics. */
#include "../platform.h"
#include <Library/DxeServicesTableLib.h>
#include <Protocol/FirmwareVolumeBlock.h>
#include <Pi/PiFirmwareVolume.h>
#include <Guid/VariableFormat.h>
#include <Guid/SystemNvDataGuid.h>
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
static volatile UINT8 *storage = (VOID *)(UINTN)IA64VMM_NVRAM_BASE;
static volatile UINT64 *commit_register =
    (VOID *)(UINTN)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_NVRAM_COMMIT);
static VOID commit(VOID)
{
    *commit_register = 0;
}
static EFI_EVENT virtual_event;
#define BLOCK_SIZE 4096u
#define BLOCKS 16u
static EFI_STATUS EFIAPI get_attr(const EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *self,
                                  EFI_FVB_ATTRIBUTES_2 *attr)
{
    (void)self;
    if (!attr)
        return EFI_INVALID_PARAMETER;
    *attr = EFI_FVB2_READ_ENABLED_CAP | EFI_FVB2_READ_STATUS | EFI_FVB2_WRITE_ENABLED_CAP |
            EFI_FVB2_WRITE_STATUS | EFI_FVB2_ERASE_POLARITY;
    *attr |= EFI_FVB2_MEMORY_MAPPED;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI set_attr(const EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *self,
                                  EFI_FVB_ATTRIBUTES_2 *attr)
{
    EFI_FVB_ATTRIBUTES_2 current;
    if (!attr)
        return EFI_INVALID_PARAMETER;
    get_attr(self, &current);
    if (*attr != current)
        return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI address(const EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *self,
                                 EFI_PHYSICAL_ADDRESS *base)
{
    (void)self;
    if (!base)
        return EFI_INVALID_PARAMETER;
    /* Like UDK2018 OVMF FvbGetPhysicalAddress, return the converted mapped
     * base at runtime: Variable.c uses it for pointer differences. */
    *base = (UINTN)storage;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI block(const EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *self, EFI_LBA lba,
                               UINTN *size, UINTN *count)
{
    (void)self;
    if (lba >= BLOCKS || !size || !count)
        return EFI_INVALID_PARAMETER;
    *size = BLOCK_SIZE;
    *count = BLOCKS - lba;
    return EFI_SUCCESS;
}
static EFI_STATUS transfer(EFI_LBA lba, UINTN offset, UINTN *length, UINT8 *buffer, BOOLEAN writing)
{
    if (lba >= BLOCKS || offset >= BLOCK_SIZE || !length || (!buffer && *length))
        return EFI_INVALID_PARAMETER;
    EFI_STATUS status = EFI_SUCCESS;
    if (*length > BLOCK_SIZE - offset) {
        *length = BLOCK_SIZE - offset;
        status = EFI_BAD_BUFFER_SIZE;
    }
    UINTN base = (UINTN)lba * BLOCK_SIZE + offset;
    if (writing) {
        /* Reject a 0->1 change before modifying any byte. */
        for (UINTN i = 0; i < *length; ++i)
            if ((storage[base + i] & buffer[i]) != buffer[i])
                return EFI_DEVICE_ERROR;
        for (UINTN i = 0; i < *length; ++i)
            storage[base + i] = buffer[i];
        if (*length)
            commit();
    } else
        for (UINTN i = 0; i < *length; ++i)
            buffer[i] = storage[base + i];
    return status;
}
static EFI_STATUS EFIAPI read_block(const EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *self, EFI_LBA lba,
                                    UINTN offset, UINTN *length, UINT8 *buffer)
{
    (void)self;
    return transfer(lba, offset, length, buffer, FALSE);
}
static EFI_STATUS EFIAPI write_block(const EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *self, EFI_LBA lba,
                                     UINTN offset, UINTN *length, UINT8 *buffer)
{
    (void)self;
    return transfer(lba, offset, length, buffer, TRUE);
}
static EFI_STATUS EFIAPI erase(const EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *self, ...)
{
    (void)self;
    VA_LIST args;
    /* PI requires validation of the whole list before the first erase. */
    VA_START(args, self);
    for (;;) {
        EFI_LBA lba = VA_ARG(args, EFI_LBA);
        if (lba == EFI_LBA_LIST_TERMINATOR)
            break;
        UINTN count = VA_ARG(args, UINTN);
        if (!count || lba >= BLOCKS || count > BLOCKS - lba) {
            VA_END(args);
            return EFI_INVALID_PARAMETER;
        }
    }
    VA_END(args);
    VA_START(args, self);
    for (;;) {
        EFI_LBA lba = VA_ARG(args, EFI_LBA);
        if (lba == EFI_LBA_LIST_TERMINATOR)
            break;
        UINTN count = VA_ARG(args, UINTN);
        for (UINTN i = 0; i < count * BLOCK_SIZE; ++i)
            storage[lba * BLOCK_SIZE + i] = 0xff;
    }
    VA_END(args);
    commit();
    return EFI_SUCCESS;
}
static EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL fvb = {get_attr,   set_attr,    address, block,
                                                 read_block, write_block, erase,   NULL};
static VOID EFIAPI convert(EFI_EVENT event, VOID *context)
{
    (void)event;
    (void)context;
    gRT->ConvertPointer(0, (VOID **)&storage);
    gRT->ConvertPointer(0, (VOID **)&commit_register);
}
EFI_STATUS EFIAPI VmmFvbEntry(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    (void)image;
    (void)system;
    EFI_STATUS map_status = gDS->SetMemorySpaceAttributes(IA64VMM_NVRAM_BASE, IA64VMM_NVRAM_SIZE,
                                                          EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    if (EFI_ERROR(map_status))
        return map_status;
    const struct ia64vmm_handoff *h = platform_handoff_hob();
    if (h->nvram_base != IA64VMM_NVRAM_BASE || h->nvram_size != IA64VMM_NVRAM_SIZE)
        return EFI_UNSUPPORTED;
    /* A fresh VMM store is zeroed. Existing PI bytes must never be reformatted
     * on a checksum/FTW failure; journal recovery belongs to upstream FTW. */
    BOOLEAN blank = TRUE;
    for (UINTN i = 0; i < IA64VMM_NVRAM_SIZE; ++i)
        if (storage[i]) {
            blank = FALSE;
            break;
        }
    if (blank) {
        for (UINTN i = 0; i < IA64VMM_NVRAM_SIZE; ++i)
            storage[i] = 0xff;
        struct {
            EFI_FIRMWARE_VOLUME_HEADER fv;
            EFI_FV_BLOCK_MAP_ENTRY end;
            VARIABLE_STORE_HEADER var;
        } header;
        ZeroMem(&header, sizeof(header));
        header.fv.FileSystemGuid = gEfiSystemNvDataFvGuid;
        /* One memory-mapped FV covers all FVB blocks. Its variable store is
         * only the first 16 KiB; FTW owns the remaining working/spare blocks,
         * as in OVMF InitializeVariableFvHeader (UDK2018). */
        header.fv.FvLength = IA64VMM_NVRAM_SIZE;
        header.fv.Signature = EFI_FVH_SIGNATURE;
        get_attr(&fvb, &header.fv.Attributes);
        header.fv.HeaderLength = sizeof(header.fv) + sizeof(header.end);
        header.fv.Revision = EFI_FVH_REVISION;
        header.fv.BlockMap[0].NumBlocks = BLOCKS;
        header.fv.BlockMap[0].Length = BLOCK_SIZE;
        UINT16 sum = 0;
        for (UINTN i = 0; i < header.fv.HeaderLength / 2; ++i)
            sum += ((UINT16 *)&header)[i];
        header.fv.Checksum = (UINT16)-sum;
        header.var.Signature = gEfiVariableGuid;
        header.var.Size = 0x4000 - header.fv.HeaderLength;
        header.var.Format = VARIABLE_STORE_FORMATTED;
        header.var.State = VARIABLE_STORE_HEALTHY;
        for (UINTN i = 0; i < header.fv.HeaderLength + sizeof(header.var); ++i)
            storage[i] = ((UINT8 *)&header)[i];
        commit();
    }
    EFI_STATUS status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, convert, NULL,
                                           &gEfiEventVirtualAddressChangeGuid, &virtual_event);
    if (EFI_ERROR(status))
        return status;
    EFI_HANDLE handle = NULL;
    status = gBS->InstallProtocolInterface(&handle, &gEfiFirmwareVolumeBlockProtocolGuid,
                                           EFI_NATIVE_INTERFACE, &fvb);
    DebugPrint(DEBUG_INFO, "FVB: 16K variables / 16K working / 32K spare: %r\n", status);
    return status;
}
