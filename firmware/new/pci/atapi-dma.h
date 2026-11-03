/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PRD construction adapted from qemu-system-ia64 roms/ia64-firmware/
 * firmware.c:ide_bmdma_prepare_prdt (GPL-2.0-or-later).
 * Intel 290550-002 §§2.7, 3.5.3; ACS-3 §7.18.4. */
#ifndef VMM_ATAPI_DMA_H
#define VMM_ATAPI_DMA_H
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/TimerLib.h>

static EFI_STATUS vmm_dma_io(EFI_PCI_IO_PROTOCOL *pci, BOOLEAN write,
                             EFI_PCI_IO_PROTOCOL_WIDTH width, UINT16 port, VOID *value)
{
    return (write ? pci->Io.Write : pci->Io.Read)(pci, width, EFI_PCI_IO_PASS_THROUGH_BAR, port, 1,
                                                  value);
}
static EFI_STATUS vmm_atapi_dma(EFI_PCI_IO_PROTOCOL *pci, EFI_IDE_REGISTERS *r, UINT8 device,
                                EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *packet)
{
    UINT32 cfg[9];
    UINT8 *c = (VOID *)cfg, *cdb = packet->Cdb;
    UINT32 length = packet->InTransferLength;
    /* READ(10/12) has a fixed data length. Commands such as INQUIRY can
     * complete short; BMIDE provides no residual count, so keep those PIO. */
    if (!cdb ||
        !((cdb[0] == 0x28 && packet->CdbLength == 10) ||
          (cdb[0] == 0xa8 && packet->CdbLength == 12)) ||
        packet->DataDirection != EFI_EXT_SCSI_DATA_DIRECTION_READ || !packet->InDataBuffer ||
        !length || (length & 1) || ((UINTN)packet->InDataBuffer & 1) || length > 0x1000000 ||
        device > 1)
        return EFI_UNSUPPORTED;
    UINT32 blocks = cdb[0] == 0x28 ? ((UINT32)cdb[7] << 8) | cdb[8]
                                   : ((UINT32)cdb[6] << 24) | ((UINT32)cdb[7] << 16) |
                                         ((UINT32)cdb[8] << 8) | cdb[9];
    if (length % 2048 || blocks != length / 2048)
        return EFI_UNSUPPORTED;
    EFI_STATUS status = pci->Pci.Read(pci, EfiPciIoWidthUint32, 0, 9, cfg);
    if (EFI_ERROR(status))
        return EFI_UNSUPPORTED;
    if ((cfg[0] != 0x12308086 && cfg[0] != 0x70108086 && cfg[0] != 0x71118086) || c[11] != 1 ||
        c[10] != 1 || !(c[9] & 0x80) || (c[4] & 5) != 5 || (cfg[8] & 0xffff000fU) != 1 ||
        !(cfg[8] & 0xfff0) ||
        (r->BusMasterBaseAddr != (cfg[8] & 0xfff0) &&
         r->BusMasterBaseAddr != (cfg[8] & 0xfff0) + 8))
        return EFI_UNSUPPORTED;
    UINT16 bm = r->BusMasterBaseAddr;
    UINT8 value = 0;
    status = vmm_dma_io(pci, FALSE, EfiPciIoWidthUint8, bm + 2, &value);
    if (EFI_ERROR(status))
        return EFI_UNSUPPORTED;
    if (value & 1)
        return EFI_UNSUPPORTED;
    if (!(value & (0x20U << device)))
        return EFI_UNSUPPORTED;

    VOID *table = NULL, *table_map = NULL, *data_map = NULL;
    BOOLEAN table_mapped = FALSE, data_mapped = FALSE, prepared = FALSE;
    EFI_PHYSICAL_ADDRESS table_bus = 0, data_bus = 0;
    UINTN pages = 17, bytes = EFI_PAGES_TO_SIZE(pages), entries = 0;
    status = pci->AllocateBuffer(pci, AllocateAnyPages, EfiBootServicesData, pages, &table, 0);
    if (EFI_ERROR(status))
        return EFI_UNSUPPORTED;
    status = pci->Map(pci, EfiPciIoOperationBusMasterCommonBuffer, table, &bytes, &table_bus,
                      &table_map);
    if (EFI_ERROR(status))
        goto done;
    table_mapped = TRUE;
    if (bytes != EFI_PAGES_TO_SIZE(pages) || table_bus > MAX_UINT32 ||
        bytes - 1 > MAX_UINT32 - table_bus || (table_bus & 3)) {
        status = EFI_UNSUPPORTED;
        goto done;
    }
    bytes = length;
    status = pci->Map(pci, EfiPciIoOperationBusMasterWrite, packet->InDataBuffer, &bytes, &data_bus,
                      &data_map);
    if (EFI_ERROR(status))
        goto done;
    data_mapped = TRUE;
    if (bytes != length || (data_bus & 1) || data_bus > MAX_UINT32 ||
        bytes - 1 > MAX_UINT32 - data_bus) {
        status = EFI_UNSUPPORTED;
        goto done;
    }
    UINTN offset = (UINTN)(-table_bus) & 0xffff;
    EFI_ATA_DMA_PRD *prd = (VOID *)((UINT8 *)table + offset);
    UINT32 table_address = (UINT32)(table_bus + offset), remaining = length;
    while (remaining) {
        UINT32 chunk = 0x10000U - ((UINT32)data_bus & 0xffffU);
        if (chunk > remaining)
            chunk = remaining;
        prd[entries].RegionBaseAddr = (UINT32)data_bus;
        prd[entries].ByteCount = (UINT16)chunk;
        prd[entries].EndOfTable = 0;
        data_bus += chunk;
        remaining -= chunk;
        ++entries;
    }
    prd[entries - 1].EndOfTable = 0x8000;
    MemoryFence();
#define DMA_IO(write, width, port, ptr)                                                            \
    do {                                                                                           \
        status = vmm_dma_io(pci, write, width, port, ptr);                                         \
        if (EFI_ERROR(status))                                                                     \
            goto done;                                                                             \
    } while (0)
    prepared = TRUE;
    value = 8;
    DMA_IO(TRUE, EfiPciIoWidthUint8, bm, &value);
    DMA_IO(FALSE, EfiPciIoWidthUint8, bm + 2, &value);
    value = (value & 0x60) | 6;
    DMA_IO(TRUE, EfiPciIoWidthUint8, bm + 2, &value);
    DMA_IO(TRUE, EfiPciIoWidthUint32, bm + 4, &table_address);
    value = 0xe0 | (device << 4);
    DMA_IO(TRUE, EfiPciIoWidthUint8, r->Head, &value);
    /* ATA/ATAPI-6 device selection: allow at least 400 ns to settle. */
    MicroSecondDelay(1);
    value = 0;
    DMA_IO(TRUE, EfiPciIoWidthUint8, r->AltOrDev, &value);
    /* Wait for the selected device before writing the command block. */
    UINT64 started = GetPerformanceCounter();
    for (;;) {
        DMA_IO(FALSE, EfiPciIoWidthUint8, r->AltOrDev, &value);
        if (!(value & 0x88))
            break;
        if (packet->Timeout &&
            GetTimeInNanoSecond(GetPerformanceCounter() - started) / 100 >= packet->Timeout) {
            status = EFI_TIMEOUT;
            goto done;
        }
        MicroSecondDelay(10);
    }
    value = 1;
    DMA_IO(TRUE, EfiPciIoWidthUint8, r->ErrOrFeature, &value);
    value = 0;
    DMA_IO(TRUE, EfiPciIoWidthUint8, r->SectorCount, &value);
    value = 0xfe;
    DMA_IO(TRUE, EfiPciIoWidthUint8, r->CylinderLsb, &value);
    value = 0xff;
    DMA_IO(TRUE, EfiPciIoWidthUint8, r->CylinderMsb, &value);
    value = 0xa0;
    DMA_IO(TRUE, EfiPciIoWidthUint8, r->CmdOrStatus, &value);
    MicroSecondDelay(1);
    for (;;) {
        DMA_IO(FALSE, EfiPciIoWidthUint8, r->AltOrDev, &value);
        if (!(value & 0x80)) {
            if (value & 0x21) {
                status = EFI_DEVICE_ERROR;
                goto done;
            }
            if (value & 8)
                break;
        }
        if (packet->Timeout &&
            GetTimeInNanoSecond(GetPerformanceCounter() - started) / 100 >= packet->Timeout) {
            status = EFI_TIMEOUT;
            goto done;
        }
        MicroSecondDelay(10);
    }
    /* ATA/ATAPI-6 §9.8: acknowledge a device's packet-ready interrupt
     * before transferring the CDB, so it cannot masquerade as DMA completion. */
    DMA_IO(FALSE, EfiPciIoWidthUint8, r->CmdOrStatus, &value);
    if ((value & 0xa9) != 8) {
        status = EFI_DEVICE_ERROR;
        goto done;
    }
    DMA_IO(FALSE, EfiPciIoWidthUint8, bm + 2, &value);
    value = (value & 0x60) | 6;
    DMA_IO(TRUE, EfiPciIoWidthUint8, bm + 2, &value);
    /* The command packet is always PIO, even for a DMA data phase. */
    UINT16 command[6];
    ZeroMem(command, sizeof(command));
    CopyMem(command, cdb, packet->CdbLength);
    for (UINTN i = 0; i < 6; ++i)
        DMA_IO(TRUE, EfiPciIoWidthUint16, r->Data, &command[i]);
    value = 9;
    DMA_IO(TRUE, EfiPciIoWidthUint8, bm, &value);
    for (;;) {
        DMA_IO(FALSE, EfiPciIoWidthUint8, bm + 2, &value);
        if (value & 2) {
            status = EFI_DEVICE_ERROR;
            break;
        }
        if (value & 4) {
            status = (value & 1) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
            break;
        }
        /* PIIX BMIS: inactive without an interrupt means PRD exhaustion. */
        if (!(value & 1)) {
            status = EFI_DEVICE_ERROR;
            break;
        }
        if (packet->Timeout &&
            GetTimeInNanoSecond(GetPerformanceCounter() - started) / 100 >= packet->Timeout) {
            status = EFI_TIMEOUT;
            break;
        }
        MicroSecondDelay(10);
    }
done:
    if (prepared) {
        /* Stop before acknowledging the device or releasing any DMA mapping,
         * including on timeout and errors. Keep both DMA-capable bits. */
        EFI_STATUS result;
        value = 8;
        result = vmm_dma_io(pci, TRUE, EfiPciIoWidthUint8, bm, &value);
        if (EFI_ERROR(result))
            status = result;
        result = vmm_dma_io(pci, FALSE, EfiPciIoWidthUint8, r->CmdOrStatus, &value);
        if (EFI_ERROR(result))
            status = result;
        else if (!EFI_ERROR(status) && (value & 0xa9))
            status = EFI_DEVICE_ERROR;
        vmm_dma_io(pci, FALSE, EfiPciIoWidthUint8, bm + 2, &value);
        value = (value & 0x60) | 6;
        vmm_dma_io(pci, TRUE, EfiPciIoWidthUint8, bm + 2, &value);
        value = 2;
        vmm_dma_io(pci, TRUE, EfiPciIoWidthUint8, r->AltOrDev, &value);
        result = pci->Flush(pci);
        if (EFI_ERROR(result))
            status = result;
        MemoryFence();
    }
    if (prepared && status == EFI_UNSUPPORTED)
        status = EFI_DEVICE_ERROR;
    if (!prepared)
        status = EFI_UNSUPPORTED;
    if (data_mapped)
        pci->Unmap(pci, data_map);
    if (table_mapped)
        pci->Unmap(pci, table_map);
    pci->FreeBuffer(pci, pages, table);
#undef DMA_IO
    static BOOLEAN reported;
    if (!EFI_ERROR(status) && !reported) {
        DEBUG((DEBUG_INFO, "ATAPI DMA READ active: bytes=%u PRDs=%u\n", length, (UINT32)entries));
        reported = TRUE;
    }
    return status;
}
#endif
