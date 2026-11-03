/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Intel 290550-002 §§2.3, 2.7, 3.5.3; ACS-3 §7.18; UEFI PCI I/O Map.
 * The fixture models W1C status and a device consuming physical PRDs. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#undef NULL
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Protocol/PciIo.h>
#include <Protocol/ScsiPassThruExt.h>
#include "third_party/edk2-ipf/MdeModulePkg/Bus/Ata/AtaAtapiPassThru/IdeMode.h"
#include "atapi-dma.h"
#include <Protocol/IdeControllerInit.h>
#include "piix-ide.h"
#ifdef VMM_TEST_OVERLAY
#include "SataController.h"
#endif

static UINT8 config[256], bm[16], task[16];
static UINT32 prdt;
static EFI_PHYSICAL_ADDRESS mapped_table;
static VOID *host_table;
static unsigned maps, unmaps, allocations, frees, commands, words, starts;
static unsigned failure, outcome;
static BOOLEAN running, selection_delay, command_delay, packet_irq, packet_ack;
static UINT64 ticks;
static UINT8 input[0x21000];
static EFI_PCI_IO_PROTOCOL pci;
static EFI_IDE_REGISTERS regs = {.Data = 0x1f0,
                                 .ErrOrFeature = 0x1f1,
                                 .SectorCount = 0x1f2,
                                 .CylinderLsb = 0x1f4,
                                 .CylinderMsb = 0x1f5,
                                 .Head = 0x1f6,
                                 .CmdOrStatus = 0x1f7,
                                 .AltOrDev = 0x3f6,
                                 .BusMasterBaseAddr = 0xc000};
VOID EFIAPI MemoryFence(VOID)
{
    __sync_synchronize();
}
UINT64 EFIAPI DivU64x32(UINT64 a, UINT32 b)
{
    return a / b;
}
UINT64 EFIAPI GetPerformanceCounter(VOID)
{
    return ticks += 100;
}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *s, UINT64 *e)
{
    (void)s;
    (void)e;
    return 10000000;
}
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 t)
{
    return t * 100;
}
UINTN EFIAPI MicroSecondDelay(UINTN us)
{
    if (us >= 1)
        selection_delay = command_delay = TRUE;
    ticks += us * 10;
    return us;
}
VOID *EFIAPI CopyMem(VOID *d, const VOID *s, UINTN n)
{
    return memcpy(d, s, n);
}
VOID *EFIAPI AllocateZeroPool(UINTN n)
{
    return calloc(1, n);
}
VOID EFIAPI FreePool(VOID *p)
{
    free(p);
}
BOOLEAN EFIAPI DebugAssertEnabled(VOID)
{
    return FALSE;
}
VOID EFIAPI DebugAssert(CONST CHAR8 *f, UINTN l, CONST CHAR8 *d)
{
    (void)f;
    (void)l;
    (void)d;
    abort();
}
VOID *EFIAPI ZeroMem(VOID *d, UINTN n)
{
    return memset(d, 0, n);
}
BOOLEAN EFIAPI DebugPrintEnabled(VOID)
{
    return TRUE;
}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN level)
{
    (void)level;
    return TRUE;
}
VOID EFIAPI DebugPrint(UINTN level, const CHAR8 *fmt, ...)
{
    (void)level;
    (void)fmt;
}
static EFI_STATUS EFIAPI cfg_read(EFI_PCI_IO_PROTOCOL *p, EFI_PCI_IO_PROTOCOL_WIDTH w, UINT32 off,
                                  UINTN n, VOID *buf)
{
    (void)p;
    if (failure == 8)
        return EFI_DEVICE_ERROR;
    memcpy(buf, config + off, n * ((UINTN)1 << w));
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI cfg_write(EFI_PCI_IO_PROTOCOL *p, EFI_PCI_IO_PROTOCOL_WIDTH w, UINT32 off,
                                   UINTN n, VOID *buf)
{
    (void)p;
    memcpy(config + off, buf, n * ((UINTN)1 << w));
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI allocate(EFI_PCI_IO_PROTOCOL *p, EFI_ALLOCATE_TYPE t, EFI_MEMORY_TYPE m,
                                  UINTN pages, VOID **buf, UINT64 a)
{
    (void)p;
    (void)t;
    (void)m;
    (void)a;
    if (failure == 1)
        return EFI_OUT_OF_RESOURCES;
    *buf = mmap(0, pages * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT,
                -1, 0);
    assert(*buf != MAP_FAILED);
    ++allocations;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI release(EFI_PCI_IO_PROTOCOL *p, UINTN pages, VOID *buf)
{
    (void)p;
    assert(!running);
    assert(!munmap(buf, pages * 4096));
    ++frees;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mapping(EFI_PCI_IO_PROTOCOL *p, EFI_PCI_IO_PROTOCOL_OPERATION op,
                                 VOID *host, UINTN *n, EFI_PHYSICAL_ADDRESS *bus, VOID **handle)
{
    (void)p;
    if ((failure == 2 && op == EfiPciIoOperationBusMasterCommonBuffer) ||
        (failure == 3 && op == EfiPciIoOperationBusMasterWrite))
        return EFI_OUT_OF_RESOURCES;
    ++maps;
    *handle = (VOID *)(UINTN)maps;
    if (op == EfiPciIoOperationBusMasterCommonBuffer) {
        *bus = failure == 7 ? (0x30000000U | (((UINTN)host + 0x2000) & 0xffff)) : (UINTN)host;
        mapped_table = *bus;
        host_table = host;
    } else {
        assert(op == EfiPciIoOperationBusMasterWrite);
        *bus = 0x1fffe;
    }
    if (failure == 4 && op == EfiPciIoOperationBusMasterWrite)
        *n -= 2;
    if (failure == 5 && op == EfiPciIoOperationBusMasterWrite)
        *bus = 0xfffffff0;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI unmapping(EFI_PCI_IO_PROTOCOL *p, VOID *handle)
{
    (void)p;
    assert(!running);
    assert(handle);
    ++unmaps;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI io_read(EFI_PCI_IO_PROTOCOL *p, EFI_PCI_IO_PROTOCOL_WIDTH w, UINT8 bar,
                                 UINT64 off, UINTN n, VOID *buf)
{
    (void)p;
    assert(bar == EFI_PCI_IO_PASS_THROUGH_BAR && w == EfiPciIoWidthUint8 && n == 1);
    UINT8 v = 0;
    if (failure == 9 && off == 0xc002)
        return EFI_DEVICE_ERROR;
    if (off == 0x3f6 && commands && words < 6)
        assert(command_delay);
    if (off == 0x1f7 && commands && words < 6)
        packet_ack = TRUE;
    if (off >= 0xc000 && off < 0xc010)
        v = bm[off - 0xc000];
    else if (off == 0x1f7 || off == 0x3f6)
        v = (commands && words < 6) ? 8 : 0x40;
    else
        assert(0);
    if ((off == 0xc002 || off == 0xc00a) && running) {
        if (outcome == 1)
            v = 0x21; /* active, no completion interrupt */
        else if (outcome == 4)
            v = 0x25; /* completion before the full fixed-length read */
        else if (outcome == 2)
            v = 0x26; /* PCI bus error */
        else if (outcome == 5)
            v = 0x20; /* PRDs exhausted without device completion */
        else
            v = 0x24;
    }
    if ((off == 0x1f7 || off == 0x3f6) && outcome == 3 && words == 6)
        v = 0x41;
    *(UINT8 *)buf = v;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI io_write(EFI_PCI_IO_PROTOCOL *p, EFI_PCI_IO_PROTOCOL_WIDTH w, UINT8 bar,
                                  UINT64 off, UINTN n, VOID *buf)
{
    (void)p;
    assert(bar == EFI_PCI_IO_PASS_THROUGH_BAR && n == 1);
    if (off >= 0xc000 && off < 0xc010) {
        unsigned r = off - 0xc000;
        if (r % 8 == 4) {
            assert(w == EfiPciIoWidthUint32 && !running);
            prdt = *(UINT32 *)buf;
            return EFI_SUCCESS;
        }
        assert(w == EfiPciIoWidthUint8);
        UINT8 v = *(UINT8 *)buf;
        if (r % 8 == 2) {
            bm[r] = (v & 0x60) | (bm[r] & 0x9f & ~(v & 6));
            return EFI_SUCCESS;
        }
        assert(r % 8 == 0);
        if (v & 1) {
            assert(commands == 1 && words == 6 && task[1] == 1 && (v & 8));
            assert(!(bm[r + 2] & 6));
            assert((prdt & 0xffff) == 0);
            UINT32 *d = (VOID *)((UINT8 *)host_table + (prdt - mapped_table));
            UINT32 address = 0x1fffe, total = 0;
            for (unsigned i = 0; i < 5; ++i) {
                assert(d[2 * i] == address);
                UINT32 len = d[2 * i + 1] & 0xffff;
                if (!len)
                    len = 0x10000;
                assert(!(d[2 * i + 1] & 0x7fff0000));
                assert((address & 0xffff) + len <= 0x10000);
                total += len;
                address += len;
                if (d[2 * i + 1] & 0x80000000U)
                    break;
            }
            assert(total == sizeof(input));
            ++starts;
            running = TRUE;
        } else
            running = FALSE;
        bm[r] = v;
        return EFI_SUCCESS;
    }
    if (off == 0x1f0) {
        assert(w == EfiPciIoWidthUint16 && commands == 1);
        if (packet_irq)
            assert(packet_ack && !(bm[regs.BusMasterBaseAddr - 0xc000 + 2] & 4));
        ++words;
        return EFI_SUCCESS;
    }
    assert(w == EfiPciIoWidthUint8);
    if (off == 0x1f7) {
        assert(*(UINT8 *)buf == 0xa0);
        assert(task[4] == 0xfe && task[5] == 0xff);
        ++commands;
        command_delay = FALSE;
        if (packet_irq)
            bm[regs.BusMasterBaseAddr - 0xc000 + 2] |= 4;
        return failure == 6 ? EFI_UNSUPPORTED : EFI_SUCCESS;
    }
    if (off == 0x3f6)
        return EFI_SUCCESS;
    if (off == 0x1f6)
        selection_delay = FALSE;
    if (off == 0x1f1)
        assert(selection_delay);
    assert(off >= 0x1f0 && off < 0x1f8);
    task[off - 0x1f0] = *(UINT8 *)buf;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI flush(EFI_PCI_IO_PROTOCOL *p)
{
    (void)p;
    assert(!running);
    return EFI_SUCCESS;
}
static void reset(void)
{
    memset(config, 0, sizeof(config));
    memset(bm, 0, sizeof(bm));
    memset(task, 0, sizeof(task));
    UINT32 id = 0x70108086, bar = 0xc001;
    memcpy(config, &id, 4);
    memcpy(config + 0x20, &bar, 4);
    config[4] = 5;
    config[9] = 0x80;
    config[10] = 1;
    config[11] = 1;
    bm[2] = 0x66;
    bm[10] = 0x66;
    maps = unmaps = allocations = frees = commands = words = starts = 0;
    running = packet_irq = packet_ack = FALSE;
    failure = outcome = 0;
    ticks = 0;
    pci = (EFI_PCI_IO_PROTOCOL){.Pci = {cfg_read, cfg_write},
                                .Io = {io_read, io_write},
                                .Map = mapping,
                                .Unmap = unmapping,
                                .AllocateBuffer = allocate,
                                .FreeBuffer = release,
                                .Flush = flush};
}
static void controller_modes(void)
{
    EFI_IDENTIFY_DATA id = {0};
    UINT16 *words = (VOID *)&id;
    words[49] = 0x100;
    words[53] = 6;
    words[63] = 7;
    words[65] = 180;
    words[88] = 0x3f;
    EFI_ATA_COLLECTIVE_MODE bad = {0}, mode = {0};
    reset();
    assert(vmm_piix_modes(&pci, &id, &bad, &mode) == EFI_SUCCESS);
    assert(mode.PioMode.Valid && mode.PioMode.Mode == 0);
    assert(!mode.UdmaMode.Valid && mode.MultiWordDmaMode.Valid && mode.MultiWordDmaMode.Mode == 2);
    for (unsigned ch = 0; ch < 2; ++ch)
        for (unsigned dev = 0; dev < 2; ++dev) {
            reset();
            config[0x40 + 2 * ch] = 0xff;
            config[0x41 + 2 * ch] = 0xff;
            assert(vmm_piix_timing(&pci, ch, dev, &mode) == EFI_SUCCESS);
            UINT16 timing;
            memcpy(&timing, config + 0x40 + 2 * ch, 2);
            assert((timing & 0x7300) == 0); /* shared ISP=5 clocks, RTC=4 clocks (270 ns) */
            assert(((timing >> (dev * 4)) & 15) == 0xb); /* DMA-only fast timing, PIO compatible */
            assert(bm[2 + 8 * ch] & (0x20 << dev));
            assert((bm[2 + 8 * ch] & 6) == 6); /* SetTiming must not acknowledge W1C bits. */
            mode.MultiWordDmaMode.Valid = FALSE;
            assert(vmm_piix_timing(&pci, ch, dev, &mode) == EFI_SUCCESS);
            memcpy(&timing, config + 0x40 + 2 * ch, 2);
            assert(!((timing >> (dev * 4)) & 15));
            assert(!(bm[2 + 8 * ch] & (0x20 << dev)));
            mode.MultiWordDmaMode.Valid = TRUE;
        }
    for (unsigned kind = 0; kind < 6; ++kind) {
        reset();
        words[49] = 0x100;
        words[63] = 7;
        words[65] = 180;
        bad.MultiWordDmaMode.Valid = FALSE;
        if (kind == 0)
            config[9] = 0;
        if (kind == 1)
            config[4] = 1;
        if (kind == 2)
            words[49] = 0;
        if (kind == 3)
            words[63] = 3;
        if (kind == 4)
            words[65] = 300;
        if (kind == 5) {
            bad.MultiWordDmaMode.Valid = TRUE;
            bad.MultiWordDmaMode.Mode = 2;
        }
        assert(vmm_piix_modes(&pci, &id, &bad, &mode) == EFI_SUCCESS);
        assert(!mode.MultiWordDmaMode.Valid && !mode.UdmaMode.Valid && mode.PioMode.Mode == 0);
    }
    reset();
    config[0] = 0;
    memset(&mode, 0xa5, sizeof(mode));
    EFI_ATA_COLLECTIVE_MODE before = mode;
    assert(vmm_piix_modes(&pci, &id, &bad, &mode) == EFI_UNSUPPORTED);
    assert(!memcmp(&before, &mode, sizeof(mode)));
}
int main(void)
{
    controller_modes();
#ifdef VMM_TEST_OVERLAY
    /* Exercise the real controller-init protocol and packet dispatcher. */
    reset();
    EFI_IDENTIFY_DATA ids[4] = {0};
    BOOLEAN valid[4] = {0};
    EFI_ATA_COLLECTIVE_MODE bad[4] = {0};
    EFI_SATA_CONTROLLER_PRIVATE_DATA init = {.Signature = SATA_CONTROLLER_SIGNATURE,
                                             .PciIo = &pci,
                                             .DeviceCount = 2,
                                             .IdentifyData = ids,
                                             .IdentifyValid = valid,
                                             .DisqualifiedModes = bad};
    init.IdeInit.ChannelCount = 2;
    UINT16 *w = (VOID *)&ids[0];
    w[49] = 0x100;
    w[53] = 6;
    w[63] = 7;
    w[65] = 120;
    w[88] = 0x3f;
    assert(IdeInitSubmitData(&init.IdeInit, 0, 0, &ids[0]) == EFI_SUCCESS);
    EFI_ATA_COLLECTIVE_MODE *selected = NULL;
    assert(IdeInitCalculateMode(&init.IdeInit, 0, 0, &selected) == EFI_SUCCESS);
    assert(selected->MultiWordDmaMode.Valid && !selected->UdmaMode.Valid);
    assert(IdeInitSetTiming(&init.IdeInit, 0, 0, selected) == EFI_SUCCESS);
    free(selected);
    config[10] = 6; /* AHCI keeps the generic driver's timing policy. */
    init.IdeInit.ChannelCount = 32;
    init.DeviceCount = 15;
    assert(IdeInitSetTiming(&init.IdeInit, 12, 7, &bad[0]) == EFI_SUCCESS);
#endif
    UINT8 cdb[12] = {0x28, 0, 0, 0, 0, 0, 0, 0, 66};
    EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET packet = {.Cdb = cdb,
                                                         .CdbLength = 10,
                                                         .DataDirection =
                                                             EFI_EXT_SCSI_DATA_DIRECTION_READ,
                                                         .InDataBuffer = input,
                                                         .InTransferLength = sizeof(input),
                                                         .Timeout = 1000};
    for (unsigned channel = 0; channel < 2; ++channel)
        for (unsigned dev = 0; dev < 2; ++dev) {
            reset();
            regs.BusMasterBaseAddr = 0xc000 + 8 * channel;
#ifdef VMM_TEST_OVERLAY
            assert(AtaPacketCommandExecute(&pci, &regs, channel, dev, &packet) == EFI_SUCCESS);
#else
            assert(vmm_atapi_dma(&pci, &regs, dev, &packet) == EFI_SUCCESS);
#endif
            assert(starts == 1 && commands == 1 && maps == 2 && unmaps == maps &&
                   frees == allocations);
            assert(!running && !(bm[8 * channel + 2] & 6) && (bm[8 * channel + 2] & 0x60) == 0x60);
        }
    regs.BusMasterBaseAddr = 0xc000;
    reset();
    bm[2] = 0x21;
    assert(vmm_atapi_dma(&pci, &regs, 0, &packet) == EFI_UNSUPPORTED);
    assert(!commands && !allocations && !starts && bm[2] == 0x21);
    reset();
    packet_irq = TRUE;
    assert(vmm_atapi_dma(&pci, &regs, 0, &packet) == EFI_SUCCESS);
    assert(packet_ack && starts == 1 && maps == unmaps && allocations == frees);
    reset();
    failure = 7; /* A common-buffer mapping need not preserve low 16 bits. */
    assert(vmm_atapi_dma(&pci, &regs, 0, &packet) == EFI_SUCCESS);
    assert(starts == 1 && maps == unmaps && allocations == frees);
    for (unsigned kind = 1; kind <= 9; ++kind) {
        if (kind == 6 || kind == 7)
            continue;
        reset();
        failure = kind;
        assert(vmm_atapi_dma(&pci, &regs, 0, &packet) == EFI_UNSUPPORTED);
        assert(!commands && maps == unmaps && allocations == frees);
    }
    reset();
    failure = 6;
    assert(vmm_atapi_dma(&pci, &regs, 0, &packet) == EFI_DEVICE_ERROR);
    assert(commands == 1 && !starts && !running && maps == unmaps && allocations == frees);
    for (unsigned kind = 1; kind <= 5; ++kind) {
        reset();
        outcome = kind;
        assert(vmm_atapi_dma(&pci, &regs, 0, &packet) ==
               (kind == 1 ? EFI_TIMEOUT : EFI_DEVICE_ERROR));
        assert(starts == 1 && !running && maps == unmaps && allocations == frees);
    }
    /* Both READ forms must agree with the byte buffer, including zero,
     * oversized counts and READ(12) counts that overflow a 32-bit product. */
    for (unsigned read12 = 0; read12 < 2; ++read12) {
        for (unsigned kind = 0; kind < 5; ++kind) {
            reset();
            memset(cdb, 0, sizeof(cdb));
            cdb[0] = read12 ? 0xa8 : 0x28;
            packet.CdbLength = read12 ? 12 : 10;
            UINT32 blocks = kind == 0   ? 66
                            : kind == 1 ? 65
                            : kind == 2 ? 67
                            : kind == 3 ? 0
                                        : 0x80000042U;
            if (read12) {
                cdb[6] = (UINT8)(blocks >> 24);
                cdb[7] = (UINT8)(blocks >> 16);
                cdb[8] = (UINT8)(blocks >> 8);
                cdb[9] = (UINT8)blocks;
            } else {
                cdb[7] = (UINT8)(blocks >> 8);
                cdb[8] = (UINT8)blocks;
            }
            if (!read12 && kind == 4)
                continue;
            assert(vmm_atapi_dma(&pci, &regs, 0, &packet) ==
                   (kind ? EFI_UNSUPPORTED : EFI_SUCCESS));
            assert(kind ? (!commands && !allocations) : starts == 1);
        }
    }
    reset();
    outcome = 5;
    packet.Timeout = 0;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;
    cdb[8] = 66;
    packet.CdbLength = 10;
    assert(vmm_atapi_dma(&pci, &regs, 0, &packet) == EFI_DEVICE_ERROR);
    assert(starts == 1 && !running && maps == unmaps && allocations == frees);
    /* No command, allocation, or BM writes for unsupported capability/state. */
    for (unsigned kind = 0; kind < 7; ++kind) {
        reset();
        if (kind == 0)
            config[9] = 0;
        if (kind == 1)
            config[4] = 1;
        if (kind == 2)
            config[4] = 4;
        if (kind == 3)
            memset(config + 0x20, 0, 4);
        if (kind == 4)
            bm[2] = 0x40;
        if (kind == 5)
            config[0] = 0;
        if (kind == 6)
            cdb[0] = 0x12; /* INQUIRY may return a short response. */
        assert(vmm_atapi_dma(&pci, &regs, 0, &packet) == EFI_UNSUPPORTED);
        assert(!commands && !allocations && !starts);
    }
    return 0;
}
