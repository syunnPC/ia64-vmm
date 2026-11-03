/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the vendor enumeration function with ATA command/protocol fakes.
 * A failed optional DMA mode must leave a registered PIO device. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "AtaAtapiPassThru.h"

static unsigned selected, registered, timings, pools, fault;
static BOOLEAN disk, allow_dma, dma_set, timing_dma;
static EFI_IDENTIFY_DATA registered_id;
static EFI_ATA_COLLECTIVE_MODE registered_modes;

VOID EFIAPI FreePool(VOID *p)
{
    assert(pools);
    --pools;
    free(p);
}
UINTN EFIAPI MicroSecondDelay(UINTN us)
{
    return us;
}
BOOLEAN EFIAPI DebugPrintEnabled(VOID)
{
    return FALSE;
}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN l)
{
    (void)l;
    return FALSE;
}
VOID EFIAPI DebugPrint(UINTN l, CONST CHAR8 *f, ...)
{
    (void)l;
    (void)f;
}
BOOLEAN EFIAPI ReportProgressCodeEnabled(VOID)
{
    return FALSE;
}
BOOLEAN EFIAPI ReportErrorCodeEnabled(VOID)
{
    return FALSE;
}
BOOLEAN EFIAPI ReportDebugCodeEnabled(VOID)
{
    return FALSE;
}
EFI_STATUS EFIAPI ReportStatusCode(EFI_STATUS_CODE_TYPE t, EFI_STATUS_CODE_VALUE v)
{
    (void)t;
    (void)v;
    return EFI_SUCCESS;
}
static VOID IdeWritePortB(EFI_PCI_IO_PROTOCOL *p, UINT16 port, UINT8 value)
{
    (void)p;
    if (port == 0x1f6)
        selected = (value >> 4) & 1;
}
static UINT8 IdeReadPortB(EFI_PCI_IO_PROTOCOL *p, UINT16 port)
{
    (void)p;
    if (selected)
        return 0;
    if (port == 0x1f2 || port == 0x1f3)
        return 1;
    if (port == 0x1f4)
        return disk ? 0 : 0x14;
    if (port == 0x1f5)
        return disk ? 0 : 0xeb;
    abort();
}
static EFI_STATUS WaitForBSYClear(EFI_PCI_IO_PROTOCOL *p, EFI_IDE_REGISTERS *r, UINT64 t)
{
    (void)p;
    (void)r;
    (void)t;
    return EFI_SUCCESS;
}
EFI_STATUS EFIAPI AtaIdentify(ATA_ATAPI_PASS_THRU_INSTANCE *i, UINT8 c, UINT8 d,
                              EFI_IDENTIFY_DATA *id, EFI_ATA_STATUS_BLOCK *s)
{
    (void)i;
    (void)c;
    (void)d;
    (void)s;
    memset(id, 0, sizeof(*id));
    id->AtaData.field_validity = 6;
    id->AtaData.ultra_dma_mode = 0x3f;
    /* ATA IDENTIFY word 255: A5h signature and an 8-bit zero sum. */
    id->AtaData.integrity_word = 0xa5;
    UINT8 sum = 0;
    for (unsigned at = 0; at < sizeof(*id); ++at)
        sum += ((UINT8 *)id)[at];
    id->AtaData.integrity_word |= (UINT16)(UINT8)-sum << 8;
    return EFI_SUCCESS;
}
EFI_STATUS EFIAPI AtaIdentifyPacket(ATA_ATAPI_PASS_THRU_INSTANCE *i, UINT8 c, UINT8 d,
                                    EFI_IDENTIFY_DATA *id, EFI_ATA_STATUS_BLOCK *s)
{
    return AtaIdentify(i, c, d, id, s);
}
static EFI_STATUS SetDeviceTransferMode(ATA_ATAPI_PASS_THRU_INSTANCE *i, UINT8 c, UINT8 d,
                                        EFI_ATA_TRANSFER_MODE *m, EFI_ATA_STATUS_BLOCK *s)
{
    (void)i;
    (void)c;
    (void)d;
    (void)s;
    if (m->ModeCategory == EFI_ATA_MODE_MDMA) {
        assert(m->ModeNumber == 2);
        dma_set = TRUE;
        if (fault == 1)
            return EFI_DEVICE_ERROR;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS SetDriveParameters(ATA_ATAPI_PASS_THRU_INSTANCE *i, UINT8 c, UINT8 d,
                                     EFI_ATA_DRIVE_PARMS *m, EFI_ATA_STATUS_BLOCK *s)
{
    (void)i;
    (void)c;
    (void)d;
    (void)m;
    (void)s;
    return EFI_SUCCESS;
}
static VOID IdeAtaSmartSupport(ATA_ATAPI_PASS_THRU_INSTANCE *i, UINT8 c, UINT8 d,
                               EFI_IDENTIFY_DATA *id, EFI_ATA_STATUS_BLOCK *s)
{
    (void)i;
    (void)c;
    (void)d;
    (void)id;
    (void)s;
    abort();
}
static EFI_STATUS EFIAPI submit(EFI_IDE_CONTROLLER_INIT_PROTOCOL *p, UINT8 c, UINT8 d,
                                EFI_IDENTIFY_DATA *id)
{
    (void)p;
    (void)c;
    (void)d;
    (void)id;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI calculate(EFI_IDE_CONTROLLER_INIT_PROTOCOL *p, UINT8 c, UINT8 d,
                                   EFI_ATA_COLLECTIVE_MODE **out)
{
    (void)p;
    (void)c;
    (void)d;
    *out = calloc(1, sizeof(**out));
    assert(*out);
    ++pools;
    (*out)->PioMode.Valid = TRUE;
    (*out)->MultiWordDmaMode.Valid = allow_dma;
    (*out)->MultiWordDmaMode.Mode = 2;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI timing(EFI_IDE_CONTROLLER_INIT_PROTOCOL *p, UINT8 c, UINT8 d,
                                EFI_ATA_COLLECTIVE_MODE *mode)
{
    (void)p;
    (void)c;
    (void)d;
    ++timings;
    registered_modes = *mode;
    timing_dma = mode->MultiWordDmaMode.Valid;
    if (fault >= 2 && timings == 1)
        return EFI_DEVICE_ERROR;
    if (fault == 3)
        return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
}
EFI_STATUS EFIAPI CreateNewDeviceInfo(ATA_ATAPI_PASS_THRU_INSTANCE *i, UINT16 c, UINT16 d,
                                      EFI_ATA_DEVICE_TYPE t, EFI_IDENTIFY_DATA *id)
{
    (void)i;
    assert(c == 0 && d == 0);
    assert(t == (disk ? EfiIdeHarddisk : EfiIdeCdrom));
    ++registered;
    registered_id = *id;
    return EFI_SUCCESS;
}
/* The test driver extracts this function from the patched build copy. */
#include "enumeration.inc"

int main(void)
{
    for (unsigned is_disk = 0; is_disk < 2; ++is_disk) {
        for (unsigned kind = 0; kind < 5; ++kind) {
            disk = is_disk;
            allow_dma = kind != 4;
            fault = kind == 4 ? 0 : kind;
            registered = timings = pools = 0;
            dma_set = timing_dma = FALSE;
            EFI_IDE_CONTROLLER_INIT_PROTOCOL init = {
                .SubmitData = submit, .CalculateMode = calculate, .SetTiming = timing};
            ATA_ATAPI_PASS_THRU_INSTANCE instance = {.IdeControllerInit = &init};
            instance.IdeRegisters[0] = (EFI_IDE_REGISTERS){.Head = 0x1f6,
                                                           .CmdOrStatus = 0x1f7,
                                                           .SectorCount = 0x1f2,
                                                           .SectorNumber = 0x1f3,
                                                           .CylinderLsb = 0x1f4,
                                                           .CylinderMsb = 0x1f5};
            assert(DetectAndConfigIdeDevice(&instance, 0) == EFI_SUCCESS);
            assert(registered == 1 && pools == 0);
            assert(timings == (fault >= 2 ? 2 : 1));
            assert(dma_set == allow_dma);
            assert(timing_dma == (kind == 0));
            assert(!registered_modes.UdmaMode.Valid);
            UINT8 sum = 0;
            for (unsigned at = 0; at < sizeof(registered_id); ++at)
                sum += ((UINT8 *)&registered_id)[at];
            assert(sum == 0);
            /* AtaBus uses word 53 bit 2 and word 88 supported bits to
             * choose READ DMA, even when UDMA was not selected. */
            BOOLEAN ata_dma = (registered_id.AtaData.field_validity & 4) &&
                              (registered_id.AtaData.ultra_dma_mode & 0x7f);
            if (disk)
                assert(ata_dma == (kind == 0));
        }
    }
    return 0;
}
