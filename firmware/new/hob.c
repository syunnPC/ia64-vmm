/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PI 1.6 Vol.3 HOB producer; no guessed memory across handoff holes. */
#include "hob.h"
#include "legacy-handoff.h"
#include "legacy-window.h"

static const EFI_GUID stack_guid = {
    0x4ed4bf27, 0x4092, 0x42e9, {0x80, 0x7d, 0x52, 0x7b, 0x1d, 0, 0xc9, 0xbd}};
static const EFI_GUID bsp_guid = {
    0x564b33cd, 0xc92a, 0x4593, {0x90, 0xbf, 0x24, 0x73, 0xe4, 0x3c, 0x63, 0x22}};
static const EFI_GUID module_guid = {
    0xf8e21975, 0x899, 0x4f58, {0xa4, 0xbe, 0x55, 0x25, 0xa9, 0xc6, 0xd7, 0x7a}};
static const EFI_GUID core_guid = {
    0xd6a2cb7f, 0x6a18, 0x4e2f, {0xb4, 0x3b, 0x99, 0x20, 0xa7, 0x33, 0x70, 0x0a}};
static const EFI_GUID handoff_guid = {
    0x68a6d2bb, 0x37f5, 0x4113, {0xa0, 0xb2, 0xa4, 0x46, 0x8d, 0xec, 0x0c, 0x41}};

static void zero(VOID *buffer, UINTN size)
{
    UINT8 *p = buffer;
    while (size--)
        *p++ = 0;
}

static BOOLEAN contains(const struct ia64vmm_handoff *h, UINT64 base, UINT64 size)
{
    if (!size || size > MAX_UINT64 - base)
        return FALSE;
    for (UINTN i = 0; i < h->ram_range_count; ++i) {
        if (base < h->ram[i].base)
            return FALSE;
        if (base - h->ram[i].base >= h->ram[i].size)
            continue;
        UINT64 available = h->ram[i].size - (base - h->ram[i].base);
        UINT64 consumed = size < available ? size : available;
        base += consumed;
        size -= consumed;
        if (!size)
            return TRUE;
    }
    return FALSE;
}

/* Describe RAM to the PI core in whole 8 KiB granules so a page cannot
 * span a host hole. This also preserves the alignment UEFI 2.11 §2.3.3
 * requires for IA-64 runtime and ACPI descriptors when RAM is retyped.
 * A sliver below 8 KiB at a hole stays undescribed. *i advances past one
 * maximal run of contiguous ranges; FALSE when no whole granule remains. */
static BOOLEAN ram_granules(const struct ia64vmm_handoff *h, UINTN *i, UINT64 *start, UINT64 *end)
{
    UINT64 base = h->ram[*i].base, limit = base + h->ram[*i].size;
    while (++*i < h->ram_range_count && h->ram[*i].base == limit)
        limit += h->ram[*i].size;
    *start = (base + IPL_RAM_GRANULE - 1) & ~(UINT64)(IPL_RAM_GRANULE - 1);
    *end = limit & ~(UINT64)(IPL_RAM_GRANULE - 1);
    return *start < *end;
}

/* Whether [base, base + size) lies in RAM as the resource HOBs describe it. */
static BOOLEAN described(const struct ia64vmm_handoff *h, UINT64 base, UINT64 size)
{
    UINT64 start, end;
    if (!size || size > MAX_UINT64 - base)
        return FALSE;
    for (UINTN i = 0; i < h->ram_range_count;)
        if (ram_granules(h, &i, &start, &end) && base >= start && base + size <= end)
            return TRUE;
    return FALSE;
}

BOOLEAN ipl_validate_handoff(const struct ia64vmm_handoff *h, UINTN size, UINTN id)
{
    if (!h || id != IA64VMM_PLATFORM_ID || size < IA64VMM_HANDOFF_BASE_SIZE ||
        h->magic != IA64VMM_HANDOFF_MAGIC || h->version != IA64VMM_HANDOFF_VERSION ||
        h->size < IA64VMM_HANDOFF_BASE_SIZE || h->size > size || !ia64vmm_handoff_cpu_valid(h) ||
        !h->ram_range_count || h->ram_range_count > IA64VMM_HANDOFF_MAX_RAM ||
        h->service_base != IA64VMM_SERVICE_BASE || h->service_size != IA64VMM_SERVICE_SIZE ||
        h->fw_data_base % 0x10000 || h->fw_data_size < 0x2000000 || h->itc_hz != IA64VMM_ITC_HZ ||
        !h->cpu_count || h->cpu_count > IA64VMM_MAX_CPUS || h->firmware_base % 4096 ||
        (h->boot_image_size && h->boot_image_base % 4096) || legacy_handoff_check(h))
        return FALSE;
    UINT64 end = 0;
    for (UINTN i = 0; i < h->ram_range_count; ++i) {
        UINT64 base = h->ram[i].base, length = h->ram[i].size;
        if (base < end || base % 4096 || !length || length % 4096 || length > MAX_UINT64 - base ||
            base >= IA64VMM_WINDOW_BASE || length > IA64VMM_WINDOW_BASE - base)
            return FALSE;
        end = base + length;
    }
    if (!contains(h, h->fw_data_base, h->fw_data_size) ||
        !contains(h, h->firmware_base, h->firmware_size) ||
        (h->boot_image_size && !contains(h, h->boot_image_base, h->boot_image_size)))
        return FALSE;
    UINT64 image_end = h->firmware_base + h->firmware_size;
    if (h->firmware_base < h->fw_data_base + h->fw_data_size && h->fw_data_base < image_end)
        return FALSE;
    if (h->boot_image_size && ((h->boot_image_base < h->fw_data_base + h->fw_data_size &&
                                h->fw_data_base < h->boot_image_base + h->boot_image_size) ||
                               (h->boot_image_base < image_end &&
                                h->firmware_base < h->boot_image_base + h->boot_image_size)))
        return FALSE;
    return TRUE;
}

struct builder {
    UINT8 *next;
    UINTN remaining;
};
static VOID *append(struct builder *b, UINT16 kind, UINTN size)
{
    size = (size + 7) & ~(UINTN)7;
    if (size > b->remaining || size > MAX_UINT16)
        return NULL;
    EFI_HOB_GENERIC_HEADER *h = (VOID *)b->next;
    zero(h, size);
    h->HobType = kind;
    h->HobLength = (UINT16)size;
    b->next += size;
    b->remaining -= size;
    return h;
}

static BOOLEAN allocation(struct builder *b, const EFI_GUID *name, UINT64 base, UINT64 length,
                          EFI_MEMORY_TYPE type)
{
    if (!length)
        return TRUE;
    EFI_HOB_MEMORY_ALLOCATION *h = append(b, EFI_HOB_TYPE_MEMORY_ALLOCATION, sizeof(*h));
    if (!h)
        return FALSE;
    if (name)
        h->AllocDescriptor.Name = *name;
    h->AllocDescriptor.MemoryBaseAddress = base;
    h->AllocDescriptor.MemoryLength = (length + 4095) & ~(UINT64)4095;
    h->AllocDescriptor.MemoryType = type;
    return TRUE;
}

VOID *ipl_build_hobs(const struct ia64vmm_handoff *h, VOID *buffer, UINTN capacity,
                     UINT64 core_base, UINT64 core_size, UINT64 entry, UINT64 fv_base,
                     UINT64 fv_size)
{
    UINT64 data = h->fw_data_base;
    if ((UINTN)buffer % 8 || !ipl_validate_handoff(h, sizeof(*h), IA64VMM_PLATFORM_ID) ||
        core_base != data + IPL_CORE_OFFSET || core_size < 16 ||
        core_size > IPL_BSP_OFFSET - IPL_CORE_OFFSET || entry < core_base ||
        entry > core_base + core_size - 16 || !fv_size || fv_base < h->firmware_base ||
        fv_base - h->firmware_base > h->firmware_size ||
        fv_size > h->firmware_size - (fv_base - h->firmware_base))
        return NULL;
    /* Every allocation below must lie in described RAM, or the PI core
     * would hand it to the OS as free or lose it from the map. The VMM
     * places fw_data on 64 KiB and the boot image on 8 KiB granules. */
    if (!described(h, h->firmware_base, h->firmware_size) || !described(h, data, h->fw_data_size) ||
        (h->boot_image_size &&
         !described(h, h->boot_image_base, (h->boot_image_size + 4095) & ~(UINT64)4095)))
        return NULL;
    struct builder b = {buffer, capacity};
    EFI_HOB_HANDOFF_INFO_TABLE *phit = append(&b, EFI_HOB_TYPE_HANDOFF, sizeof(*phit));
    if (!phit)
        return NULL;
    phit->Version = EFI_HOB_HANDOFF_TABLE_VERSION;
    phit->BootMode = BOOT_WITH_FULL_CONFIGURATION;
    phit->EfiMemoryBottom = data;
    phit->EfiMemoryTop = data + h->fw_data_size;
    phit->EfiFreeMemoryBottom = data + IPL_FREE_OFFSET;
    phit->EfiFreeMemoryTop = phit->EfiMemoryTop;
    for (UINTN i = 0; i < h->ram_range_count;) {
        UINT64 start, end;
        if (!ram_granules(h, &i, &start, &end))
            continue;
        EFI_HOB_RESOURCE_DESCRIPTOR *r = append(&b, EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, sizeof(*r));
        if (!r)
            return NULL;
        r->ResourceType = EFI_RESOURCE_SYSTEM_MEMORY;
        r->ResourceAttribute = EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                               EFI_RESOURCE_ATTRIBUTE_TESTED |
                               EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE;
        r->PhysicalStart = start;
        r->ResourceLength = end - start;
    }
    const UINT64 mmio_base[] = {h->service_base, h->nvram_base, h->fb_base, h->uart_base};
    const UINT64 mmio_size[] = {h->service_size, h->nvram_size, h->fb_size,
                                h->flags & IA64VMM_HF_UART_PRESENT ? IA64VMM_UART_RUNTIME_SIZE : 0};
    for (UINTN i = 0; i < 4; ++i) {
        if (!mmio_size[i])
            continue;
        BOOLEAN covered = FALSE;
        if (i == 2) {
            if (h->pci_mmio_range_count > IA64VMM_HANDOFF_MAX_PCI_MMIO)
                return NULL;
            for (UINTN j = 0; j < h->pci_mmio_range_count; ++j)
                if (h->fb_base >= h->pci_mmio[j].base && h->fb_size <= h->pci_mmio[j].size &&
                    h->fb_base - h->pci_mmio[j].base <= h->pci_mmio[j].size - h->fb_size)
                    covered = TRUE;
        }
        if (covered)
            continue;
        EFI_HOB_RESOURCE_DESCRIPTOR *r = append(&b, EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, sizeof(*r));
        if (!r)
            return NULL;
        r->ResourceType = EFI_RESOURCE_MEMORY_MAPPED_IO;
        r->ResourceAttribute = EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                               EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE;
        r->PhysicalStart = mmio_base[i];
        r->ResourceLength = (mmio_size[i] + 4095) & ~(UINT64)4095;
    }
    /* Assigned BAR spans, not aperture holes, belong in the EFI map. P1's
     * page-rounded list is already merged; avoid duplicating inherited GOP. */
    if (h->pci_mmio_range_count > IA64VMM_HANDOFF_MAX_PCI_MMIO)
        return NULL;
    for (UINTN i = 0; i < h->pci_mmio_range_count; ++i) {
        UINT64 base = h->pci_mmio[i].base, size = h->pci_mmio[i].size;
        if (!size || base % 4096 || size % 4096 || size > MAX_UINT64 - base)
            return NULL;
        EFI_HOB_RESOURCE_DESCRIPTOR *r = append(&b, EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, sizeof(*r));
        if (!r)
            return NULL;
        r->ResourceType = EFI_RESOURCE_MEMORY_MAPPED_IO;
        r->ResourceAttribute = EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                               EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE;
        r->PhysicalStart = base;
        r->ResourceLength = size;
    }
    /* As in firmware.c:efi_init_vmm_memory_map, the PCI legacy aperture is
     * device space even without POST or an assigned VGA (master abort).
     * Under legacy VGA it holds the read-only image and stub instead. */
    const UINT64 window_base[] = {h->ecam_base, h->legacy_io_base};
    const UINT64 window_size[] = {h->ecam_size, h->legacy_io_size};
    for (UINTN i = 0; i < 2; ++i) {
        if (!window_size[i])
            continue;
        EFI_HOB_RESOURCE_DESCRIPTOR *r = append(&b, EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, sizeof(*r));
        if (!r)
            return NULL;
        r->ResourceType = EFI_RESOURCE_MEMORY_MAPPED_IO;
        r->ResourceAttribute = EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                               EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE;
        r->PhysicalStart = window_base[i];
        r->ResourceLength = window_size[i];
    }
    for (uint64_t cursor = IA64VMM_VGA_MEMORY_BASE; cursor < 0x100000;) {
        uint64_t base, size;
        /* Keep the VGA/ROM boundary for their different runtime policy. */
        uint64_t limit = cursor < IA64VMM_LEGACY_AREA_BASE ? IA64VMM_LEGACY_AREA_BASE : 0x100000;
        if (!legacy_window_next(h, &cursor, limit, &base, &size))
            continue;
        EFI_HOB_RESOURCE_DESCRIPTOR *r = append(&b, EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, sizeof(*r));
        if (!r)
            return NULL;
        r->ResourceType = EFI_RESOURCE_MEMORY_MAPPED_IO;
        r->ResourceAttribute = EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                               EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE;
        r->PhysicalStart = base;
        r->ResourceLength = size;
    }
    /* Reserve every producer-owned byte, with disjoint named stack/BSP ranges. */
    /* The linker places the independent, copyable PAL block first. */
    if (h->firmware_size < 32768 || !allocation(&b, NULL, h->firmware_base, 32768, EfiPalCode) ||
        !allocation(&b, NULL, h->firmware_base + 32768, h->firmware_size - 32768,
                    EfiRuntimeServicesCode) ||
        !allocation(&b, NULL, h->boot_image_base, h->boot_image_size, EfiLoaderData) ||
        !allocation(&b, NULL, data, IPL_CORE_OFFSET, EfiBootServicesData))
        return NULL;
    /* Reserve page 0 even without an IVT copy, as the reference does. */
    if (described(h, 0, 4096) && !allocation(&b, NULL, 0, 4096, EfiReservedMemoryType))
        return NULL;
    EFI_HOB_MEMORY_ALLOCATION_MODULE *module =
        append(&b, EFI_HOB_TYPE_MEMORY_ALLOCATION, sizeof(*module));
    if (!module)
        return NULL;
    module->MemoryAllocationHeader.Name = module_guid;
    module->MemoryAllocationHeader.MemoryBaseAddress = core_base;
    module->MemoryAllocationHeader.MemoryLength = (core_size + 4095) & ~(UINT64)4095;
    module->MemoryAllocationHeader.MemoryType = EfiBootServicesCode;
    module->ModuleName = core_guid;
    module->EntryPoint = entry;
    UINT64 core_end = core_base + module->MemoryAllocationHeader.MemoryLength;
    if (!allocation(&b, NULL, core_end, data + IPL_BSP_OFFSET - core_end, EfiBootServicesData) ||
        !allocation(&b, &bsp_guid, data + IPL_BSP_OFFSET, IPL_STACK_SIZE, EfiBootServicesData) ||
        !allocation(&b, NULL, data + IPL_BSP_OFFSET + IPL_STACK_SIZE,
                    IPL_STACK_OFFSET - IPL_BSP_OFFSET - IPL_STACK_SIZE, EfiBootServicesData) ||
        !allocation(&b, &stack_guid, data + IPL_STACK_OFFSET, IPL_STACK_SIZE,
                    EfiBootServicesData) ||
        !allocation(&b, NULL, data + IPL_STACK_OFFSET + IPL_STACK_SIZE,
                    IPL_FREE_OFFSET - IPL_STACK_OFFSET - IPL_STACK_SIZE, EfiBootServicesData))
        return NULL;
    EFI_HOB_CPU *cpu = append(&b, EFI_HOB_TYPE_CPU, sizeof(*cpu));
    EFI_HOB_FIRMWARE_VOLUME *fv = append(&b, EFI_HOB_TYPE_FV, sizeof(*fv));
    EFI_HOB_GUID_TYPE *private =
        append(&b, EFI_HOB_TYPE_GUID_EXTENSION, sizeof(*private) + sizeof(*h));
    if (!cpu || !fv || !private)
        return NULL;
    cpu->SizeOfMemorySpace = IA64VMM_PHYS_ADDR_BITS;
    cpu->SizeOfIoSpace = 16;
    fv->BaseAddress = fv_base;
    fv->Length = fv_size;
    private->Name = handoff_guid;
    UINT8 *copy = (VOID *)(private + 1);
    ia64vmm_handoff_copy((struct ia64vmm_handoff *)copy, h);
    phit->EfiEndOfHobList = (UINTN)b.next;
    if (!append(&b, EFI_HOB_TYPE_END_OF_HOB_LIST, sizeof(EFI_HOB_GENERIC_HEADER)))
        return NULL;
    return buffer;
}
