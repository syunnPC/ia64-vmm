/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Protocol-level boundary, sparse-port and DMA tests of the actual driver. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#undef NULL
#include "firmware/new/pci/root.c"
VOID *EFIAPI CopyMem(VOID *dest, const VOID *source, UINTN size)
{
    return memcpy(dest, source, size);
}
VOID *EFIAPI AllocatePool(UINTN size)
{
    return malloc(size);
}
VOID EFIAPI FreePool(VOID *p)
{
    free(p);
}
VOID EFIAPI MemoryFence(VOID)
{
    __sync_synchronize();
}
static UINTN decode_errors, decode_writes;
VOID EFIAPI DebugPrint(UINTN level, const CHAR8 *format, ...)
{
    (void)format;
    if (level == DEBUG_ERROR)
        ++decode_errors;
}
UINT64 EFIAPI LShiftU64(UINT64 value, UINTN shift)
{
    return value << shift;
}
static UINT64 clock_ticks;
UINT64 EFIAPI GetPerformanceCounter(VOID)
{
    return clock_ticks += 400;
}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *start, UINT64 *end)
{
    (void)start;
    (void)end;
    return 400000000;
}
static EFI_STATUS EFIAPI alloc_pages(EFI_ALLOCATE_TYPE type, EFI_MEMORY_TYPE memory, UINTN pages,
                                     EFI_PHYSICAL_ADDRESS *address)
{
    assert(type == AllocateMaxAddress && memory == EfiBootServicesData && *address == MAX_UINT32);
    VOID *p = mmap(0, pages * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT,
                   -1, 0);
    if (p == MAP_FAILED)
        return EFI_OUT_OF_RESOURCES;
    *address = (UINTN)p;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_pages(EFI_PHYSICAL_ADDRESS address, UINTN pages)
{
    assert(!munmap((VOID *)(UINTN)address, pages * 4096));
    return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES boots = {.AllocatePages = alloc_pages, .FreePages = free_pages};
EFI_BOOT_SERVICES *gBS = &boots;
/* A controller whose primary IDE enable bit is read-only zero. */
static EFI_STATUS EFIAPI readonly_primary(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
                                          EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,
                                          UINT64 address, UINTN count, VOID *buffer)
{
    ++decode_writes;
    if ((address & 255) == 0x40) {
        assert(width == EfiPciWidthUint16 && count == 1);
        UINT16 value = *(UINT16 *)buffer & ~0x8000U;
        return pci_write(self, width, address, count, &value);
    }
    return pci_write(self, width, address, count, buffer);
}
int main(void)
{
    struct ia64vmm_handoff h = {0};
    handoff = &h;
    _Alignas(8) UINT64 regs[8] = {1, 2, 3, 4}, buffer[8] = {0};
    h.pci_mmio64_base = (UINTN)regs;
    h.pci_mmio64_size = sizeof(regs);
    assert(mem_read(&root, EfiPciWidthUint64, (UINTN)regs, 4, buffer) == EFI_SUCCESS &&
           buffer[3] == 4);
    assert(mem_read(&root, EfiPciWidthFifoUint64, (UINTN)regs, 4, buffer) == EFI_SUCCESS &&
           buffer[3] == 1);
    assert(mem_read(&root, EfiPciWidthFillUint64, (UINTN)regs, 4, buffer) == EFI_SUCCESS &&
           buffer[0] == 4);
    assert(mem_read(&root, EfiPciWidthUint64, (UINTN)regs, 9, buffer) == EFI_INVALID_PARAMETER);
    assert(mem_read(&root, EfiPciWidthUint64, (UINTN)regs + 1, 1, buffer) == EFI_INVALID_PARAMETER);
    assert(mem_read(&root, EfiPciWidthUint64, (UINTN)regs, MAX_UINTN, buffer) ==
           EFI_INVALID_PARAMETER);
    assert(mem_read(&root, EfiPciWidthMaximum, (UINTN)regs, 1, buffer) == EFI_INVALID_PARAMETER);
    assert(copy_mem(&root, EfiPciWidthUint64, (UINTN)(regs + 1), (UINTN)regs, 3) == EFI_SUCCESS);
    assert(regs[1] == 1 && regs[2] == 2 && regs[3] == 3);
    UINT64 value;
    assert(poll_mem(&root, EfiPciWidthUint64, (UINTN)regs, ~0ULL, 1, 0, &value) == EFI_SUCCESS);
    assert(poll_mem(&root, EfiPciWidthUint64, (UINTN)regs, ~0ULL, 8, 10, &value) == EFI_TIMEOUT);
    VOID *ecam = mmap(0, 4 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(ecam != MAP_FAILED);
    h.ecam_base = (UINTN)ecam;
    h.ecam_size = 4 << 20;
    h.ecam_bus_first = 3;
    h.ecam_bus_last = 3;
    UINT64 address = EFI_PCI_ADDRESS(3, 31, 7, 0xffc);
    UINT32 config = 0x12345678, actual = 0;
    assert(pci_write(&root, EfiPciWidthUint32, address, 1, &config) == EFI_SUCCESS);
    assert(pci_read(&root, EfiPciWidthUint32, address, 1, &actual) == EFI_SUCCESS &&
           actual == config);
    /* ExtendedRegister replaces Register, including its alignment. */
    assert(pci_read(&root, EfiPciWidthUint32, address | 1, 1, &actual) == EFI_SUCCESS &&
           actual == config);
    assert(*(UINT32 *)((UINT8 *)ecam + (4 << 20) - 4) == config);
    /* Intel 82371FB/SB datasheet §2.3.10: IDETIM bit 15 enables decode
     * independently for each channel. Firmware must leave timing and BAR
     * assignments intact. PCI IDE 1.0 §2 fixes compatibility-mode ports. */
    UINT8 *ide = (UINT8 *)ecam + (3 << 20) + (1 << 15) + (1 << 12);
    UINT32 identity = 0x70108086, class_revision = 0x01018000;
    UINT16 primary = 0x1234, secondary = 0x4321;
    memcpy(ide, &identity, 4);
    memcpy(ide + 8, &class_revision, 4);
    memcpy(ide + 0x40, &primary, 2);
    memcpy(ide + 0x42, &secondary, 2);
    memset(ide + 0x10, 0xa5, 20);
    UINT8 before[256];
    memcpy(before, ide, sizeof(before));
    root_handle = (EFI_HANDLE)(UINTN)1;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI_ADDRESS bdf = {.Bus = 3, .Device = 1, .Function = 1};
    assert(preprocess(&host, root_handle, bdf, EfiPciBeforeChildBusEnumeration) == EFI_SUCCESS);
    assert(!memcmp(before, ide, sizeof(before)));
    assert(preprocess(&host, root_handle, bdf, EfiPciBeforeResourceCollection) == EFI_SUCCESS);
    before[0x41] |= 0x80;
    before[0x43] |= 0x80;
    assert(!memcmp(before, ide, sizeof(before)));
    assert(preprocess(&host, root_handle, bdf, EfiPciBeforeResourceCollection) == EFI_SUCCESS);
    assert(!memcmp(before, ide, sizeof(before)));
    /* Another vendor's IDE timing registers are not Intel IDETIM. */
    identity = 0x70101234;
    memcpy(ide, &identity, 4);
    ide[0x41] = 0x12;
    ide[0x43] = 0x43;
    memcpy(before, ide, sizeof(before));
    assert(preprocess(&host, root_handle, bdf, EfiPciBeforeResourceCollection) == EFI_SUCCESS);
    assert(!memcmp(before, ide, sizeof(before)));
    assert(preprocess(&host, NULL, bdf, EfiPciBeforeResourceCollection) == EFI_INVALID_PARAMETER);
    /* Reference vendor/class selection includes ICH/PCH IDE functions and
     * does not require a known device ID. AHCI/RAID/non-storage are excluded. */
    static const UINT32 intel[] = {0x12308086, 0x70108086, 0x71118086,
                                   0x29208086, 0x8c008086, 0x12348086};
    static const UINT32 other_classes[] = {0x01060100, 0x01040000, 0x02000000};
    for (UINTN i = 0; i < sizeof(intel) / sizeof(intel[0]); ++i) {
        memcpy(ide, &intel[i], 4);
        for (UINTN j = 0; j < sizeof(other_classes) / sizeof(other_classes[0]); ++j) {
            class_revision = other_classes[j];
            memcpy(ide + 8, &class_revision, 4);
            memcpy(before, ide, sizeof(before));
            assert(preprocess(&host, root_handle, bdf, EfiPciBeforeResourceCollection) ==
                   EFI_SUCCESS);
            assert(!memcmp(before, ide, sizeof(before)));
        }
        /* PCH datasheet §13.1.22: bit 15 gates legacy and native decode.
         * PI §10.8.11 runs this hook before BAR probing; EDK2 subsequently
         * disables PCI command decoding while collecting resources. Native
         * BARs may still be unassigned, so they cannot gate initialization. */
        static const UINT8 interfaces[] = {0x80, 0x8a, 0x8f, 0x81, 0x84};
        for (UINTN j = 0; j < sizeof(interfaces); ++j) {
            class_revision = 0x01010000U | (UINT32)interfaces[j] << 8;
            memcpy(ide + 8, &class_revision, 4);
            memset(ide + 4, 0, 2);
            memset(ide + 0x10, 0, 16);
            memcpy(ide + 0x40, &primary, 2);
            memcpy(ide + 0x42, &secondary, 2);
            memcpy(before, ide, sizeof(before));
            before[0x41] |= 0x80;
            before[0x43] |= 0x80;
            assert(preprocess(&host, root_handle, bdf, EfiPciBeforeResourceCollection) ==
                   EFI_SUCCESS);
            assert(!memcmp(before, ide, sizeof(before)));
            assert(preprocess(&host, root_handle, bdf, EfiPciBeforeResourceCollection) ==
                   EFI_SUCCESS);
            assert(!memcmp(before, ide, sizeof(before)));
        }
    }
    /* PI §10.8.11: EFI_DEVICE_ERROR would suppress device enumeration.
     * A failed primary readback must be logged and leave secondary usable. */
    memcpy(ide + 0x40, &primary, 2);
    memcpy(ide + 0x42, &secondary, 2);
    memcpy(before, ide, sizeof(before));
    before[0x43] |= 0x80;
    root.Pci.Write = readonly_primary;
    assert(preprocess(&host, root_handle, bdf, EfiPciBeforeResourceCollection) == EFI_SUCCESS);
    assert(!memcmp(before, ide, sizeof(before)));
    assert(decode_writes == 2 && decode_errors == 1);
    root.Pci.Write = pci_write;
    assert(pci_read(&root, EfiPciWidthUint32, address, 2, &actual) == EFI_INVALID_PARAMETER);
    assert(pci_read(&root, EfiPciWidthUint32, EFI_PCI_ADDRESS(2, 0, 0, 0), 1, &actual) ==
           EFI_INVALID_PARAMETER);
    assert(pci_read(&root, EfiPciWidthUint32, EFI_PCI_ADDRESS(3, 32, 0, 0), 1, &actual) ==
           EFI_INVALID_PARAMETER);
    assert(pci_read(&root, EfiPciWidthUint64, address, 1, &value) == EFI_INVALID_PARAMETER);
    VOID *ports = mmap(0, 1 << 26, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(ports != MAP_FAILED);
    h.legacy_io_base = (UINTN)ports;
    UINT8 byte = 0xa5, bytes[5] = {1, 2, 3, 4, 5};
    assert(io_write(&root, EfiPciWidthUint8, 0x60, 1, &byte) == EFI_SUCCESS);
    assert(*(UINT8 *)((UINT8 *)ports + (0x18 << 12) + 0x60) == byte);
    assert(io_write(&root, EfiPciWidthUint8, 0x60, 5, bytes) == EFI_SUCCESS);
    assert(*(UINT8 *)((UINT8 *)ports + (0x19 << 12) + 0x64) == 5);
    assert(io_read(&root, EfiPciWidthUint16, 0xffff, 1, &actual) == EFI_INVALID_PARAMETER);
    h.ram_range_count = 1;
    h.ram[0] = (struct ia64vmm_ram_range){(UINTN)regs, sizeof(regs)};
    UINTN size = sizeof(regs);
    EFI_PHYSICAL_ADDRESS device;
    VOID *mapping;
    assert(map(&root, EfiPciOperationBusMasterRead64, regs, &size, &device, &mapping) ==
           EFI_SUCCESS);
    assert(device == (UINTN)regs && !mapping);
    assert(map(&root, EfiPciOperationBusMasterCommonBuffer, regs, &size, &device, &mapping) ==
           EFI_UNSUPPORTED);
    assert(map(&root, EfiPciOperationBusMasterRead, regs, &size, &device, &mapping) ==
               EFI_SUCCESS &&
           mapping);
    assert(device <= MAX_UINT32 && !memcmp(regs, (VOID *)(UINTN)device, size));
    assert(unmap(&root, mapping) == EFI_SUCCESS);
    assert(map(&root, EfiPciOperationBusMasterWrite, regs, &size, &device, &mapping) ==
           EFI_SUCCESS);
    *(UINT64 *)(UINTN)device = 77;
    assert(unmap(&root, mapping) == EFI_SUCCESS && regs[0] == 77);
    size = sizeof(regs) + 1;
    assert(map(&root, EfiPciOperationBusMasterRead64, regs, &size, &device, &mapping) ==
           EFI_INVALID_PARAMETER);
    h.ram_range_count = 2;
    h.ram[0] = (struct ia64vmm_ram_range){0x1000, 0x1000};
    h.ram[1] = (struct ia64vmm_ram_range){0x3000, 0x1000};
    assert(!ram(0x1800, 0x1800));
    h.ram[1].base = 0x2000;
    assert(ram(0x1800, 0x1800));
    VOID *common;
    assert(allocate(&root, AllocateAnyPages, EfiBootServicesData, 1, &common, 0) == EFI_SUCCESS);
    assert((UINTN)common <= MAX_UINT32 && free_buffer(&root, 1, common) == EFI_SUCCESS);
    assert(allocate(&root, AllocateAnyPages, EfiBootServicesData, 1, &common,
                    EFI_PCI_ATTRIBUTE_MEMORY_DISABLE) == EFI_UNSUPPORTED);
    assert(notify(&host, EfiPciHostBridgeBeginEnumeration) == EFI_UNSUPPORTED);
    assert(unmap(&root, NULL) == EFI_SUCCESS);
    munmap(ecam, 4 << 20);
    munmap(ports, 1 << 26);
    return 0;
}
