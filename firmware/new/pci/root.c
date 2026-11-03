/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Kernel/Device/PciEcamAddress.hpp
 */
/* UEFI 2.7 §14.2: preserve the host's assignments. */
#include "../platform.h"
#include <Protocol/PciRootBridgeIo.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <IndustryStandard/Acpi.h>
#include <Protocol/DevicePath.h>

static const struct ia64vmm_handoff *handoff;
static EFI_HANDLE root_handle;
static struct {
    ACPI_HID_DEVICE_PATH acpi;
    EFI_DEVICE_PATH_PROTOCOL end;
} root_path = {
    {{ACPI_DEVICE_PATH, ACPI_DP, {sizeof(ACPI_HID_DEVICE_PATH), 0}}, EISA_PNP_ID(0x0a03), 0},
    {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0}}};
static struct {
    EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR range[4];
    EFI_ACPI_END_TAG_DESCRIPTOR end;
} resources;
static UINTN range_count;
static UINT64 attributes;
#define ROOT_ATTRIBUTES                                                                            \
    (EFI_PCI_ATTRIBUTE_ISA_IO | EFI_PCI_ATTRIBUTE_ISA_MOTHERBOARD_IO |                             \
     EFI_PCI_ATTRIBUTE_IDE_PRIMARY_IO | EFI_PCI_ATTRIBUTE_IDE_SECONDARY_IO |                       \
     EFI_PCI_ATTRIBUTE_VGA_IO | EFI_PCI_ATTRIBUTE_VGA_MEMORY | EFI_PCI_ATTRIBUTE_VGA_PALETTE_IO |  \
     EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE)

static BOOLEAN inside(UINT64 base, UINT64 size, UINT64 address, UINT64 bytes)
{
    return size && address >= base && bytes <= size && address - base <= size - bytes;
}
static BOOLEAN ram(UINT64 address, UINT64 bytes)
{
    if (!bytes || bytes > MAX_UINT64 - address)
        return FALSE;
    for (UINTN i = 0; i < handoff->ram_range_count; ++i) {
        UINT64 base = handoff->ram[i].base, size = handoff->ram[i].size;
        if (address < base)
            return FALSE;
        if (address - base >= size)
            continue;
        UINT64 part = size - (address - base);
        if (bytes <= part)
            return TRUE;
        address += part;
        bytes -= part;
    }
    return FALSE;
}
static UINT64 read_register(UINTN size, UINT64 address)
{
    MemoryFence();
    UINT64 value;
    switch (size) {
    case 1:
        value = *(volatile UINT8 *)(UINTN)address;
        break;
    case 2:
        value = *(volatile UINT16 *)(UINTN)address;
        break;
    case 4:
        value = *(volatile UINT32 *)(UINTN)address;
        break;
    default:
        value = *(volatile UINT64 *)(UINTN)address;
        break;
    }
    MemoryFence();
    return value;
}
static VOID write_register(UINTN size, UINT64 address, UINT64 value)
{
    MemoryFence();
    switch (size) {
    case 1:
        *(volatile UINT8 *)(UINTN)address = (UINT8)value;
        break;
    case 2:
        *(volatile UINT16 *)(UINTN)address = (UINT16)value;
        break;
    case 4:
        *(volatile UINT32 *)(UINTN)address = (UINT32)value;
        break;
    default:
        *(volatile UINT64 *)(UINTN)address = value;
        break;
    }
    MemoryFence();
}
/* ECAM validation.
 * The VMM's segment-zero window starts at bus zero, even for a narrower root.
 * PCI Express Base Specification §7.2.2; extended offset takes precedence. */
static EFI_STATUS access_registers(UINTN space, BOOLEAN write,
                                   EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width, UINT64 address,
                                   UINTN count, VOID *buffer)
{
    if ((UINT32)width >= EfiPciWidthMaximum || !buffer)
        return EFI_INVALID_PARAMETER;
    UINTN size = (UINTN)1 << (width & 3), mode = width >> 2;
    if ((UINTN)buffer % size || (space != 2 && address % size))
        return EFI_INVALID_PARAMETER;
    if (space != 0 && size == 8)
        return EFI_INVALID_PARAMETER;
    if (!count)
        return EFI_SUCCESS;
    UINTN stride = mode == 1 ? 0 : size, buffer_stride = mode == 2 ? 0 : size;
    if (stride && count > MAX_UINT64 / stride)
        return EFI_INVALID_PARAMETER;
    if (buffer_stride && count > (MAX_UINTN - (UINTN)buffer) / buffer_stride)
        return EFI_INVALID_PARAMETER;
    UINT64 span = stride ? count * stride : size;
    if (space == 2) {
        UINT32 bus = (UINT32)(address >> 24) & 255, dev = (UINT32)(address >> 16) & 255;
        UINT32 fun = (UINT32)(address >> 8) & 255;
        UINT64 reg = address >> 32;
        if (!reg)
            reg = address & 255;
        if (bus < handoff->ecam_bus_first || bus > handoff->ecam_bus_last || dev > 31 || fun > 7 ||
            reg % size || !inside(0, 4096, reg, span))
            return EFI_INVALID_PARAMETER;
        UINT64 offset = ((UINT64)bus << 20) | ((UINT64)dev << 15) | ((UINT64)fun << 12) | reg;
        if (!inside(0, handoff->ecam_size, offset, span))
            return EFI_INVALID_PARAMETER;
        address = handoff->ecam_base + offset;
    } else if (space == 1) {
        /* Legacy IDE/VGA ports can fall outside the assigned I/O BAR aperture.
         * The VMM allow-list is authoritative for each individual port. */
        if (!inside(0, 0x10000, address, span))
            return EFI_INVALID_PARAMETER;
    } else if (!inside(handoff->pci_mmio_base, handoff->pci_mmio_size, address, span) &&
               !inside(handoff->pci_mmio64_base, handoff->pci_mmio64_size, address, span) &&
               !inside(handoff->fb_base, handoff->fb_size, address, span) &&
               !inside(0xa0000, 0x20000, address, span))
        return EFI_INVALID_PARAMETER;
    UINT8 *p = buffer;
    for (UINTN i = 0; i < count; ++i) {
        UINT64 target = address;
        if (space == 1)
            target = handoff->legacy_io_base + ((address >> 2) << 12 | (address & 0xfff));
        UINT64 value = 0;
        if (write) {
            CopyMem(&value, p, size);
            write_register(size, target, value);
        } else {
            value = read_register(size, target);
            CopyMem(p, &value, size);
        }
        address += stride;
        p += buffer_stride;
    }
    return EFI_SUCCESS;
}
#define ACCESS(name, space, write)                                                                 \
    static EFI_STATUS EFIAPI name(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,                           \
                                  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width, UINT64 address,     \
                                  UINTN count, VOID *buffer)                                       \
    {                                                                                              \
        (void)self;                                                                                \
        return access_registers(space, write, width, address, count, buffer);                      \
    }
ACCESS(mem_read, 0, FALSE)
ACCESS(mem_write, 0, TRUE)
ACCESS(io_read, 1, FALSE)
ACCESS(io_write, 1, TRUE)
ACCESS(pci_read, 2, FALSE)
ACCESS(pci_write, 2, TRUE)

static EFI_STATUS poll(UINTN space, EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width, UINT64 address,
                       UINT64 mask, UINT64 value, UINT64 delay, UINT64 *result)
{
    if (!result || width > EfiPciWidthUint64)
        return EFI_INVALID_PARAMETER;
    UINT64 start = GetPerformanceCounter();
    UINT64 frequency = GetPerformanceCounterProperties(NULL, NULL);
    UINT64 ticks =
        delay > MAX_UINT64 / (frequency / 10000000) ? MAX_UINT64 : delay * (frequency / 10000000);
    do {
        *result = 0;
        EFI_STATUS status = access_registers(space, FALSE, width, address, 1, result);
        if (EFI_ERROR(status))
            return status;
        if ((*result & mask) == value)
            return EFI_SUCCESS;
        if (!delay)
            break;
    } while (GetPerformanceCounter() - start < ticks);
    return EFI_TIMEOUT;
}
static EFI_STATUS EFIAPI poll_mem(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
                                  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w, UINT64 a, UINT64 m,
                                  UINT64 v, UINT64 d, UINT64 *r)
{
    (void)self;
    return poll(0, w, a, m, v, d, r);
}
static EFI_STATUS EFIAPI poll_io(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
                                 EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w, UINT64 a, UINT64 m,
                                 UINT64 v, UINT64 d, UINT64 *r)
{
    (void)self;
    return poll(1, w, a, m, v, d, r);
}
static EFI_STATUS EFIAPI copy_mem(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
                                  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width, UINT64 dest,
                                  UINT64 source, UINTN count)
{
    (void)self;
    if (width > EfiPciWidthUint64)
        return EFI_INVALID_PARAMETER;
    UINTN size = (UINTN)1 << width;
    if (count > MAX_UINT64 / size)
        return EFI_INVALID_PARAMETER;
    UINT64 bytes = count * size;
    /* Validate both sides before writing, including overlapping backwards copies. */
    UINT64 probe;
    EFI_STATUS status = access_registers(0, FALSE, width, source, 0, &probe);
    if (EFI_ERROR(status))
        return status;
    if (source % size || dest % size ||
        (!inside(handoff->pci_mmio_base, handoff->pci_mmio_size, source, bytes) &&
         !inside(handoff->pci_mmio64_base, handoff->pci_mmio64_size, source, bytes)) ||
        (!inside(handoff->pci_mmio_base, handoff->pci_mmio_size, dest, bytes) &&
         !inside(handoff->pci_mmio64_base, handoff->pci_mmio64_size, dest, bytes)))
        return EFI_INVALID_PARAMETER;
    BOOLEAN backwards = dest > source && dest - source < bytes;
    for (UINTN i = 0; i < count; ++i) {
        UINT64 offset = (backwards ? count - 1 - i : i) * size;
        write_register(size, dest + offset, read_register(size, source + offset));
    }
    return EFI_SUCCESS;
}
struct dma_map {
    UINT64 bounce;
    UINTN pages, bytes;
    VOID *host;
    BOOLEAN copy_back;
};
static EFI_STATUS EFIAPI map(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self,
                             EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_OPERATION operation, VOID *host,
                             UINTN *bytes, EFI_PHYSICAL_ADDRESS *device, VOID **mapping)
{
    (void)self;
    if (operation >= EfiPciOperationMaximum || !host || !bytes || !device || !mapping ||
        !ram((UINTN)host, *bytes))
        return EFI_INVALID_PARAMETER;
    *mapping = NULL;
    UINT64 address = (UINTN)host;
    if (operation >= EfiPciOperationBusMasterRead64 || inside(0, 0x100000000ULL, address, *bytes)) {
        *device = address;
        MemoryFence();
        return EFI_SUCCESS;
    }
    /* UEFI §14.2.4: a 32-bit common buffer cannot be bounced. Read/write
     * bounce buffers also live in identity RAM; no IOVA remapping is implied. */
    if (operation == EfiPciOperationBusMasterCommonBuffer)
        return EFI_UNSUPPORTED;
    if (*bytes > MAX_UINTN - 4095)
        return EFI_INVALID_PARAMETER;
    struct dma_map *m = AllocatePool(sizeof(*m));
    if (!m)
        return EFI_OUT_OF_RESOURCES;
    m->pages = EFI_SIZE_TO_PAGES(*bytes);
    m->bounce = MAX_UINT32;
    EFI_STATUS status =
        gBS->AllocatePages(AllocateMaxAddress, EfiBootServicesData, m->pages, &m->bounce);
    if (EFI_ERROR(status)) {
        FreePool(m);
        return status;
    }
    m->bytes = *bytes;
    m->host = host;
    m->copy_back = operation == EfiPciOperationBusMasterWrite;
    if (!m->copy_back)
        CopyMem((VOID *)(UINTN)m->bounce, host, *bytes);
    *device = m->bounce;
    *mapping = m;
    MemoryFence();
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI unmap(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self, VOID *mapping)
{
    (void)self;
    MemoryFence();
    if (!mapping)
        return EFI_SUCCESS;
    struct dma_map *m = mapping;
    if (m->copy_back)
        CopyMem(m->host, (VOID *)(UINTN)m->bounce, m->bytes);
    EFI_STATUS status = gBS->FreePages(m->bounce, m->pages);
    FreePool(m);
    return status;
}
static EFI_STATUS EFIAPI allocate(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self, EFI_ALLOCATE_TYPE type,
                                  EFI_MEMORY_TYPE memory, UINTN pages, VOID **host, UINT64 attr)
{
    (void)self;
    (void)type;
    if (!host || !pages || (memory != EfiBootServicesData && memory != EfiRuntimeServicesData))
        return EFI_INVALID_PARAMETER;
    if (attr & EFI_PCI_ATTRIBUTE_INVALID_FOR_ALLOCATE_BUFFER)
        return EFI_UNSUPPORTED;
    EFI_PHYSICAL_ADDRESS address =
        attr & EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE ? MAX_UINT64 : MAX_UINT32;
    EFI_STATUS status = gBS->AllocatePages(AllocateMaxAddress, memory, pages, &address);
    if (!EFI_ERROR(status))
        *host = (VOID *)(UINTN)address;
    return status;
}
static EFI_STATUS EFIAPI free_buffer(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self, UINTN pages, VOID *host)
{
    (void)self;
    return gBS->FreePages((UINTN)host, pages);
}
static EFI_STATUS EFIAPI flush(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self)
{
    (void)self;
    MemoryFence();
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI get_attributes(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self, UINT64 *supported,
                                        UINT64 *current)
{
    (void)self;
    if (!supported && !current)
        return EFI_INVALID_PARAMETER;
    if (supported)
        *supported = ROOT_ATTRIBUTES;
    if (current)
        *current = attributes;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI set_attributes(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self, UINT64 attr,
                                        UINT64 *base, UINT64 *length)
{
    (void)self;
    (void)base;
    (void)length;
    if (attr & ~ROOT_ATTRIBUTES)
        return EFI_UNSUPPORTED;
    attributes = attr;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI configuration(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *self, VOID **data)
{
    (void)self;
    if (!data)
        return EFI_INVALID_PARAMETER;
    *data = &resources;
    return EFI_SUCCESS;
}
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL root = {NULL,
                                               poll_mem,
                                               poll_io,
                                               {mem_read, mem_write},
                                               {io_read, io_write},
                                               {pci_read, pci_write},
                                               copy_mem,
                                               map,
                                               unmap,
                                               allocate,
                                               free_buffer,
                                               flush,
                                               get_attributes,
                                               set_attributes,
                                               configuration,
                                               0};
static EFI_STATUS EFIAPI notify(EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *self,
                                EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PHASE phase)
{
    (void)self;
    (void)phase;
    /* Full enumeration would reassign buses/BARs. A wrong PCD fails clearly. */
    return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI next_root(EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *self,
                                   EFI_HANDLE *handle)
{
    (void)self;
    if (!handle)
        return EFI_INVALID_PARAMETER;
    if (*handle == root_handle)
        return EFI_NOT_FOUND;
    if (*handle)
        return EFI_INVALID_PARAMETER;
    *handle = root_handle;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI host_attributes(EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *self,
                                         EFI_HANDLE handle, UINT64 *attr)
{
    (void)self;
    if (handle != root_handle || !attr)
        return EFI_INVALID_PARAMETER;
    *attr = EFI_PCI_HOST_BRIDGE_COMBINE_MEM_PMEM | EFI_PCI_HOST_BRIDGE_MEM64_DECODE;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI bus_start(EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *self,
                                   EFI_HANDLE handle, VOID **data)
{
    (void)self;
    if (handle != root_handle || !data)
        return EFI_INVALID_PARAMETER;
    UINTN size = sizeof(resources.range[0]) + sizeof(resources.end);
    UINT8 *copy = AllocatePool(size);
    if (!copy)
        return EFI_OUT_OF_RESOURCES;
    CopyMem(copy, &resources.range[0], sizeof(resources.range[0]));
    CopyMem(copy + sizeof(resources.range[0]), &resources.end, sizeof(resources.end));
    *data = copy;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI immutable(EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *self,
                                   EFI_HANDLE handle, VOID *data)
{
    (void)self;
    (void)data;
    return handle == root_handle ? EFI_UNSUPPORTED : EFI_INVALID_PARAMETER;
}
static EFI_STATUS EFIAPI proposed(EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *self,
                                  EFI_HANDLE handle, VOID **data)
{
    (void)self;
    (void)data;
    return handle == root_handle ? EFI_UNSUPPORTED : EFI_INVALID_PARAMETER;
}
static EFI_STATUS EFIAPI preprocess(EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *self,
                                    EFI_HANDLE handle,
                                    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI_ADDRESS address,
                                    EFI_PCI_CONTROLLER_RESOURCE_ALLOCATION_PHASE phase)
{
    (void)self;
    if (handle != root_handle)
        return EFI_INVALID_PARAMETER;
    if (phase != EfiPciBeforeResourceCollection)
        return EFI_SUCCESS;
    UINT64 bdf = EFI_PCI_ADDRESS(address.Bus, address.Device, address.Function, 0);
    UINT32 identity = 0, class_revision = 0;
    EFI_STATUS status = pci_read(&root, EfiPciWidthUint32, bdf, 1, &identity);
    if (EFI_ERROR(status))
        return EFI_SUCCESS;
    /* Intel 82371FB/SB datasheet §2.3.10 and 82371AB §5.1.10: IDETIM
     * bit 15 enables each channel's task-file decode; Intel 9 Series PCH
     * §13.1.22 applies it to both compatibility and native mode. Ported
     * from firmware.c:ide_enable_intel_decode with its vendor/class test.
     * EDK2 PciPciDeviceInfoCollector calls this before BAR probing, then
     * GatherDeviceInfo disables PCI command decoding for resource collection.
     * Set the channel gates even with unassigned native BARs; the PCI bus
     * driver enables I/O later. Preserve all timings and assignments. */
    if ((identity & 0xffffU) != 0x8086U)
        return EFI_SUCCESS;
    status = pci_read(&root, EfiPciWidthUint32, bdf + 8, 1, &class_revision);
    if (EFI_ERROR(status))
        return EFI_SUCCESS;
    if (class_revision >> 16 != 0x0101U)
        return EFI_SUCCESS;
    for (UINTN channel = 0; channel < 2; ++channel) {
        UINT64 reg = bdf + 0x40 + 2 * channel;
        UINT16 timing = 0, readback = 0;
        status = pci_read(&root, EfiPciWidthUint16, reg, 1, &timing);
        UINT16 enabled = timing | 0x8000U;
        if (!EFI_ERROR(status))
            status = root.Pci.Write(&root, EfiPciWidthUint16, reg, 1, &enabled);
        if (!EFI_ERROR(status))
            status = pci_read(&root, EfiPciWidthUint16, reg, 1, &readback);
        if (EFI_ERROR(status) || readback != enabled) {
            /* PI §10.8.11: EFI_DEVICE_ERROR suppresses enumeration.
             * Optional chipset setup must not hide the other channel. */
            DebugPrint(DEBUG_ERROR,
                       "IDE %02x:%02x.%u channel %u IDETIM failed: %r expected %04x read %04x\n",
                       address.Bus, address.Device, address.Function, (UINT32)channel, status,
                       enabled, readback);
            continue;
        }
        DebugPrint(DEBUG_INFO, "IDE %02x:%02x.%u channel %u IDETIM %04x -> %04x\n", address.Bus,
                   address.Device, address.Function, (UINT32)channel, timing, readback);
    }
    return EFI_SUCCESS;
}
static EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL host = {
    notify, next_root, host_attributes, bus_start, immutable, immutable, proposed, preprocess};
static VOID add_range(UINT8 type, UINT64 bits, UINT64 base, UINT64 size)
{
    if (!size)
        return;
    EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d = &resources.range[range_count++];
    d->Desc = ACPI_ADDRESS_SPACE_DESCRIPTOR;
    d->Len = sizeof(*d) - 3;
    d->ResType = type;
    d->AddrSpaceGranularity = bits;
    d->AddrRangeMin = base;
    d->AddrRangeMax = base + size - 1;
    d->AddrLen = size;
}
EFI_STATUS EFIAPI VmmPciEntry(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    (void)image;
    (void)system;
    handoff = platform_handoff_hob();
    if (handoff->ecam_base != IA64VMM_ECAM_BASE || handoff->ecam_size != IA64VMM_ECAM_SIZE ||
        handoff->ecam_bus_first > handoff->ecam_bus_last || handoff->ecam_bus_last > 255 ||
        handoff->legacy_io_base != IA64VMM_LEGACY_IO_BASE)
        return EFI_UNSUPPORTED;
    add_range(ACPI_ADDRESS_SPACE_TYPE_BUS, 0, handoff->ecam_bus_first,
              handoff->ecam_bus_last - handoff->ecam_bus_first + 1);
    add_range(ACPI_ADDRESS_SPACE_TYPE_IO, 0, handoff->pci_io_base, handoff->pci_io_size);
    add_range(ACPI_ADDRESS_SPACE_TYPE_MEM, 32, handoff->pci_mmio_base, handoff->pci_mmio_size);
    add_range(ACPI_ADDRESS_SPACE_TYPE_MEM, 64, handoff->pci_mmio64_base, handoff->pci_mmio64_size);
    /* The end tag immediately follows the populated descriptors. */
    EFI_ACPI_END_TAG_DESCRIPTOR *end = (VOID *)&resources.range[range_count];
    end->Desc = ACPI_END_TAG_DESCRIPTOR;
    end->Checksum = 0;
    resources.end = *end;
    EFI_STATUS status = gBS->InstallMultipleProtocolInterfaces(
        &root.ParentHandle, &gEfiPciHostBridgeResourceAllocationProtocolGuid, &host, NULL);
    if (EFI_ERROR(status))
        return status;
    status = gBS->InstallMultipleProtocolInterfaces(&root_handle, &gEfiPciRootBridgeIoProtocolGuid,
                                                    &root, &gEfiDevicePathProtocolGuid, &root_path,
                                                    NULL);
    DebugPrint(DEBUG_INFO, "PCI root: buses %u-%u MMIO32 %lx/%lx MMIO64 %lx/%lx IO %lx/%lx: %r\n",
               handoff->ecam_bus_first, handoff->ecam_bus_last, handoff->pci_mmio_base,
               handoff->pci_mmio_size, handoff->pci_mmio64_base, handoff->pci_mmio64_size,
               handoff->pci_io_base, handoff->pci_io_size, status);
    return status;
}
