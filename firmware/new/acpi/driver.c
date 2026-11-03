/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../platform.h"
#include "tables.h"
#include "../smbios/smbios.h"
#include "../sal/sal.h"
#include "../ivt-layout.h"
#include <Guid/Acpi.h>
#include <Guid/SmBios.h>
#include <Guid/SalSystemTable.h>
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
extern struct sal_state sal_runtime;
extern struct sal_call_result sal_entry(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                        uint64_t, uint64_t);
extern VOID EFIAPI sal_virtual(EFI_EVENT, VOID *);
static EFI_EVENT virtual_event;
static const EFI_GUID hcdp_guid = {
    0xf951938d, 0x620b, 0x42ef, {0x82, 0x79, 0xa8, 0x4b, 0x79, 0x61, 0x78, 0x98}};

static EFI_STATUS sal_table(const struct ia64vmm_handoff *h, volatile struct ap_wake_block *ap)
{
    EFI_PHYSICAL_ADDRESS base;
    EFI_STATUS status = gBS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 4, &base);
    if (EFI_ERROR(status))
        return status;
    UINTN bytes = 0, key, stride;
    UINT32 version;
    status = gBS->GetMemoryMap(&bytes, NULL, &key, &stride, &version);
    if (status != EFI_BUFFER_TOO_SMALL)
        goto failed;
    bytes += 4096;
    VOID *map;
    status = gBS->AllocatePool(EfiBootServicesData, bytes, &map);
    if (EFI_ERROR(status))
        goto failed;
    status = gBS->GetMemoryMap(&bytes, map, &key, &stride, &version);
    if (EFI_ERROR(status)) {
        gBS->FreePool(map);
        goto failed;
    }
    struct sal_memory memory[128];
    size_t count = 0;
    for (UINTN at = 0; at < bytes; at += stride) {
        const EFI_MEMORY_DESCRIPTOR *d = (VOID *)((UINT8 *)map + at);
        if (d->Type == EfiMemoryMappedIO || d->Type == EfiMemoryMappedIOPortSpace ||
            d->Type == EfiReservedMemoryType || d->Type == EfiUnusableMemory)
            continue;
        if (count == 128 || d->NumberOfPages > UINT32_MAX) {
            status = EFI_OUT_OF_RESOURCES;
            break;
        }
        struct sal_memory *m = memory + count++;
        m->base = d->PhysicalStart;
        m->size = d->NumberOfPages * 4096;
        m->attribute = 0;
        switch (d->Type) {
        case EfiPalCode:
            m->usage = 1;
            break;
        case EfiLoaderCode:
        case EfiBootServicesCode:
            m->usage = 2;
            break;
        case EfiLoaderData:
        case EfiBootServicesData:
            m->usage = 3;
            break;
        case EfiRuntimeServicesCode:
            m->usage = 4;
            break;
        case EfiRuntimeServicesData:
            m->usage = 5;
            break;
        case EfiACPIReclaimMemory:
            m->usage = 8;
            break;
        case EfiACPIMemoryNVS:
            m->usage = 9;
            break;
        default:
            m->usage = 0;
            break;
        }
    }
    gBS->FreePool(map);
    if (EFI_ERROR(status))
        goto failed;
    const UINT64 *descriptor = (const VOID *)sal_entry;
    const UINT64 *control = (const VOID *)(UINTN)(h->fw_data_base + PLATFORM_CONTROL_OFFSET);
    const struct sal_tr tr = {control[CONTROL_BootItrAddress / 8], control[CONTROL_BootItrItir / 8],
                              0, 0};
    if (!sal_build((VOID *)(UINTN)base, base, h->firmware_base, descriptor[0], descriptor[1],
                   h->cpu_count, memory, count, &tr, 1)) {
        status = EFI_INVALID_PARAMETER;
        goto failed;
    }
    sal_init(&sal_runtime, h, h->firmware_base);
    sal_runtime.ap = ap;
    volatile struct sal_mc *mc = (VOID *)(UINTN)control[CONTROL_SalMc / 8];
    if (mc) {
        mc->pal_proc = h->firmware_base;
        mc->sal_proc = descriptor[0];
        mc->sal_gp = descriptor[1];
    }
    sal_runtime.mc = mc;
    status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, sal_virtual, NULL,
                                &gEfiEventVirtualAddressChangeGuid, &virtual_event);
    if (EFI_ERROR(status))
        goto failed;
    status = gBS->InstallConfigurationTable(&gEfiSalSystemTableGuid, (VOID *)(UINTN)base);
    if (EFI_ERROR(status)) {
        gBS->CloseEvent(virtual_event);
        goto failed;
    }
    DEBUG((DEBUG_INFO, "SAL: table=%lx PAL=%lx SAL=%lx GP=%lx memory=%u\n", base, h->firmware_base,
           descriptor[0], descriptor[1], (UINT32)count));
    return EFI_SUCCESS;
failed:
    gBS->FreePages(base, 4);
    return status;
}

/* IPF memory descriptors are 8 KiB granular (UEFI 2.7 §7.2.3). */
static EFI_STATUS allocate(EFI_MEMORY_TYPE type, UINTN bytes, EFI_PHYSICAL_ADDRESS *base)
{
    UINTN pages = ((bytes + 8191) & ~(UINTN)8191) / EFI_PAGE_SIZE;
    *base = 0xffffffff;
    return gBS->AllocatePages(AllocateMaxAddress, type, pages, base);
}
/* Per-CPU areas exist before BDS starts any application, since an
 * EFI application may wake APs through SAL and the PIB while boot services
 * are active. Area 0 is the BSP's slot;
 * the BSP keeps its control block. */
static EFI_STATUS ap_areas(const struct ia64vmm_handoff *h, volatile struct ap_wake_block **ap)
{
    const UINT64 *control = (const VOID *)(UINTN)(h->fw_data_base + PLATFORM_CONTROL_OFFSET);
    *ap = (VOID *)(UINTN)control[CONTROL_ApBlock / 8];
    if (!*ap)
        return EFI_DEVICE_ERROR;
    EFI_PHYSICAL_ADDRESS base;
    EFI_STATUS status =
        allocate(EfiRuntimeServicesData, (UINTN)h->cpu_count * PERCPU_STRIDE, &base);
    if (EFI_ERROR(status))
        return status;
    ap_publish(*ap, base, h->cpu_count);
    DEBUG((DEBUG_INFO, "AP: %u areas at %lx stride %x\n", h->cpu_count, base, PERCPU_STRIDE));
    return EFI_SUCCESS;
}
/* Sort the GetMemoryMap snapshot, including descriptor extensions. The EFI
 * map itself has no ordering guarantee; adjacent RAM must merge before the
 * fixed-size ACPI image is allocated. */
static bool sort_map(UINT8 *map, UINTN bytes, UINTN stride)
{
    if (stride < sizeof(EFI_MEMORY_DESCRIPTOR) || bytes % stride)
        return false;
    for (UINTN at = stride; at < bytes; at += stride) {
        for (UINTN pos = at;
             pos && table_get(map + pos + 8, 8) < table_get(map + pos - stride + 8, 8);
             pos -= stride) {
            for (UINTN i = 0; i < stride; ++i) {
                UINT8 tmp = map[pos + i];
                map[pos + i] = map[pos - stride + i];
                map[pos - stride + i] = tmp;
            }
        }
    }
    return true;
}
/* Take the map after allocating the tables. Later loader/boot-services
 * allocations only repartition RAM; they do not change its affinity. */
static EFI_STATUS acpi_table(const struct ia64vmm_handoff *h, struct acpi_image *acpi,
                             EFI_PHYSICAL_ADDRESS reclaim, EFI_PHYSICAL_ADDRESS nvs)
{
    UINTN bytes = 0, key, stride;
    UINT32 version;
    EFI_STATUS status = gBS->GetMemoryMap(&bytes, NULL, &key, &stride, &version);
    if (status != EFI_BUFFER_TOO_SMALL)
        return status;
    bytes += 4096;
    VOID *map;
    status = gBS->AllocatePool(EfiBootServicesData, bytes, &map);
    if (EFI_ERROR(status))
        return status;
    status = gBS->GetMemoryMap(&bytes, map, &key, &stride, &version);
    if (!EFI_ERROR(status) &&
        (version != EFI_MEMORY_DESCRIPTOR_VERSION || !sort_map(map, bytes, stride) ||
         !acpi_build(h, acpi, reclaim, (VOID *)(UINTN)nvs, nvs, map, bytes, stride)))
        status = EFI_INVALID_PARAMETER;
    gBS->FreePool(map);
    return status;
}
EFI_STATUS EFIAPI VmmTablesEntry(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    (void)image;
    (void)system;
    EFI_PHYSICAL_ADDRESS reclaim = 0, nvs = 0, smbios = 0;
    volatile struct ap_wake_block *ap;
    EFI_STATUS status = ap_areas(platform_handoff_hob(), &ap);
    if (EFI_ERROR(status))
        return status;
    status = allocate(EfiACPIReclaimMemory, sizeof(struct acpi_image), &reclaim);
    if (EFI_ERROR(status))
        return status;
    status = allocate(EfiACPIMemoryNVS, 64, &nvs);
    if (EFI_ERROR(status))
        goto free_reclaim;
    status = allocate(EfiRuntimeServicesData, sizeof(struct smbios_image), &smbios);
    if (EFI_ERROR(status))
        goto free_nvs;
    struct acpi_image *acpi = (VOID *)(UINTN)reclaim;
    struct smbios_image *smb = (VOID *)(UINTN)smbios;
    const struct ia64vmm_handoff *h = platform_handoff_hob();
    /* SAL PCI configuration remains callable after SetVirtualAddressMap,
     * so ECAM needs a runtime MMIO descriptor. Reuse any existing MMIO HOB. */
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR gcd;
    status = gDS->GetMemorySpaceDescriptor(h->ecam_base, &gcd);
    if (EFI_ERROR(status))
        goto free_smbios;
    if (gcd.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
        status = gDS->AddMemorySpace(EfiGcdMemoryTypeMemoryMappedIo, h->ecam_base, h->ecam_size,
                                     EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    } else if (gcd.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo ||
               gcd.BaseAddress > h->ecam_base ||
               gcd.Length - (h->ecam_base - gcd.BaseAddress) < h->ecam_size) {
        status = EFI_ACCESS_DENIED;
    } else if ((gcd.Capabilities & (EFI_MEMORY_UC | EFI_MEMORY_RUNTIME)) !=
               (EFI_MEMORY_UC | EFI_MEMORY_RUNTIME)) {
        status = gDS->SetMemorySpaceCapabilities(
            gcd.BaseAddress, gcd.Length, gcd.Capabilities | EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    }
    if (EFI_ERROR(status))
        goto free_smbios;
    status = gDS->SetMemorySpaceAttributes(h->ecam_base, h->ecam_size,
                                           EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    if (EFI_ERROR(status))
        goto free_smbios;
    status = acpi_table(h, acpi, reclaim, nvs);
    if (EFI_ERROR(status))
        goto free_smbios;
    if (!smbios_build(h, smb, smbios + OFFSET_OF(struct smbios_image, bytes))) {
        status = EFI_INVALID_PARAMETER;
        goto free_smbios;
    }
    status = gBS->InstallConfigurationTable(&gEfiAcpi20TableGuid, acpi->bytes + acpi->rsdp);
    if (EFI_ERROR(status))
        goto free_smbios;
    status = gBS->InstallConfigurationTable(&gEfiSmbiosTableGuid, smb->entry);
    if (EFI_ERROR(status)) {
        gBS->InstallConfigurationTable(&gEfiAcpi20TableGuid, NULL);
        goto free_smbios;
    }
    /* PCDP consumers use both XSDT and the EFI configuration GUID. Publish
     * the same reclaim-memory bytes rather than a separate console copy. */
    if (acpi->hcdp != SIZE_MAX) {
        status = gBS->InstallConfigurationTable((EFI_GUID *)&hcdp_guid, acpi->bytes + acpi->hcdp);
        if (EFI_ERROR(status))
            goto remove_tables;
    }
    status = sal_table(h, ap);
    if (EFI_ERROR(status)) {
        if (acpi->hcdp != SIZE_MAX)
            gBS->InstallConfigurationTable((EFI_GUID *)&hcdp_guid, NULL);
        goto remove_tables;
    }
    DEBUG((DEBUG_INFO, "tables: ACPI=%lx FACS=%lx SMBIOS=%lx CPUs=%u\n", reclaim + acpi->rsdp, nvs,
           smbios, h->cpu_count));
    return EFI_SUCCESS;
remove_tables:
    gBS->InstallConfigurationTable(&gEfiAcpi20TableGuid, NULL);
    gBS->InstallConfigurationTable(&gEfiSmbiosTableGuid, NULL);
free_smbios:
    gBS->FreePages(smbios, ((sizeof(struct smbios_image) + 8191) & ~(UINTN)8191) / EFI_PAGE_SIZE);
free_nvs:
    gBS->FreePages(nvs, 8192 / EFI_PAGE_SIZE);
free_reclaim:
    gBS->FreePages(reclaim, ((sizeof(struct acpi_image) + 8191) & ~(UINTN)8191) / EFI_PAGE_SIZE);
    return status;
}
