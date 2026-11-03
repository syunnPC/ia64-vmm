/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_PIIX_IDE_H
#define VMM_PIIX_IDE_H
/* Intel 290550-002 §§2.3.6, 2.3.9, 2.3.10, 2.7.2; 82371AB §5.1.
 * The generic SATA init driver has no parallel ATA timing implementation.
 * Use MWDMA2 with conservative 270 ns controller cycles and PIO0 control
 * I/O. IDENTIFY word 65 may require a cycle longer than the mode minimum. */
static EFI_STATUS vmm_piix_config(EFI_PCI_IO_PROTOCOL *pci, UINT32 cfg[9])
{
    EFI_STATUS status = pci->Pci.Read(pci, EfiPciIoWidthUint32, 0, 9, cfg);
    if (EFI_ERROR(status))
        return status;
    if ((cfg[0] != 0x12308086 && cfg[0] != 0x70108086 && cfg[0] != 0x71118086) ||
        cfg[2] >> 16 != 0x0101)
        return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}
static EFI_STATUS vmm_piix_modes(EFI_PCI_IO_PROTOCOL *pci, EFI_IDENTIFY_DATA *id,
                                 EFI_ATA_COLLECTIVE_MODE *bad, EFI_ATA_COLLECTIVE_MODE *mode)
{
    UINT32 cfg[9];
    EFI_STATUS status = vmm_piix_config(pci, cfg);
    if (EFI_ERROR(status))
        return status;
    mode->PioMode.Valid = TRUE;
    mode->PioMode.Mode = 0;
    mode->UdmaMode.Valid = FALSE;
    mode->MultiWordDmaMode.Valid = FALSE;
    UINT16 *words = (VOID *)id;
    if ((cfg[2] & 0x8000) && (cfg[1] & 5) == 5 && (cfg[8] & 0xffff000fU) == 1 &&
        (cfg[8] & 0xfff0) && (words[49] & 0x100) && (words[63] & 4) && !(words[62] & 0x8000) &&
        (!(words[53] & 2) || words[65] <= 270) &&
        (!bad->MultiWordDmaMode.Valid || bad->MultiWordDmaMode.Mode > 2)) {
        mode->MultiWordDmaMode.Valid = TRUE;
        mode->MultiWordDmaMode.Mode = 2;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS vmm_piix_timing(EFI_PCI_IO_PROTOCOL *pci, UINT8 channel, UINT8 device,
                                  EFI_ATA_COLLECTIVE_MODE *mode)
{
    UINT32 cfg[9];
    EFI_STATUS status = vmm_piix_config(pci, cfg);
    if (EFI_ERROR(status))
        return status;
    if (channel > 1 || device > 1 || !mode)
        return EFI_INVALID_PARAMETER;
    UINT16 timing;
    status = pci->Pci.Read(pci, EfiPciIoWidthUint16, 0x40 + 2 * channel, 1, &timing);
    if (EFI_ERROR(status))
        return status;
    BOOLEAN dma = mode->MultiWordDmaMode.Valid && mode->MultiWordDmaMode.Mode == 2;
    /* Shared ISP/RTC are used only for DMA. DTE keeps PIO compatible on
     * both devices, including a PIO-only peer on the same cable. */
    timing &= ~(0x7300U | (15U << (4 * device)));
    if (dma)
        timing |= 0xbU << (4 * device);
    status = pci->Pci.Write(pci, EfiPciIoWidthUint16, 0x40 + 2 * channel, 1, &timing);
    if (EFI_ERROR(status))
        return status;
    if (cfg[0] == 0x71118086) {
        UINT8 udma;
        status = pci->Pci.Read(pci, EfiPciIoWidthUint8, 0x48, 1, &udma);
        if (EFI_ERROR(status))
            return status;
        udma &= ~(1U << (2 * channel + device));
        status = pci->Pci.Write(pci, EfiPciIoWidthUint8, 0x48, 1, &udma);
        if (EFI_ERROR(status))
            return status;
    }
    if ((cfg[8] & 0xffff000fU) != 1 || !(cfg[8] & 0xfff0) || (cfg[1] & 5) != 5)
        return dma ? EFI_DEVICE_ERROR : EFI_SUCCESS;
    UINT64 port = (cfg[8] & 0xfff0) + 8 * channel + 2;
    UINT8 value = 0;
    status = pci->Io.Read(pci, EfiPciIoWidthUint8, EFI_PCI_IO_PASS_THROUGH_BAR, port, 1, &value);
    if (EFI_ERROR(status))
        return status;
    /* BMIS[6:5] are software-maintained per-device DMA capability bits.
     * Do not acknowledge pending W1C interrupt/error bits while setting them. */
    value = (value & 0x60) & ~(0x20U << device);
    if (dma)
        value |= 0x20U << device;
    status = pci->Io.Write(pci, EfiPciIoWidthUint8, EFI_PCI_IO_PASS_THROUGH_BAR, port, 1, &value);
    if (!EFI_ERROR(status))
        DEBUG((DEBUG_INFO, "PIIX IDE channel %u device %u MWDMA2=%u BAR4=%x command=%x IDETIM=%x\n",
               channel, device, dma, cfg[8], cfg[1] & 0xffff, timing));
    return status;
}
#endif
