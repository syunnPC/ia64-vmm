/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The diagnostic service contract records execution; it never launches a kernel. */
#include "platform.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>
static EFI_GET_MEMORY_MAP get_memory_map;
static EFI_EXIT_BOOT_SERVICES exit_boot_services;
static EFI_IMAGE_LOAD load_image;
static EFI_MEMORY_DESCRIPTOR *final_map;
static UINTN final_size, final_stride, final_key;
static EFI_STATUS EFIAPI observed_map(UINTN *size, EFI_MEMORY_DESCRIPTOR *map, UINTN *key,
                                      UINTN *stride, UINT32 *version)
{
    EFI_STATUS status = get_memory_map(size, map, key, stride, version);
    if (!EFI_ERROR(status)) {
        final_map = map;
        final_size = *size;
        final_stride = *stride;
        final_key = *key;
    }
    return status;
}
static EFI_STATUS EFIAPI observed_load(BOOLEAN policy, EFI_HANDLE parent,
                                       EFI_DEVICE_PATH_PROTOCOL *path, VOID *buffer, UINTN size,
                                       EFI_HANDLE *image)
{
    EFI_STATUS status = load_image(policy, parent, path, buffer, size, image);
    if (!EFI_ERROR(status)) {
        EFI_LOADED_IMAGE_PROTOCOL *loaded;
        if (!EFI_ERROR(
                gBS->HandleProtocol(*image, &gEfiLoadedImageProtocolGuid, (VOID **)&loaded))) {
            platform_service(IA64VMM_SVC_IMAGE_BASE, (UINTN)loaded->ImageBase);
            platform_service(IA64VMM_SVC_IMAGE_SIZE, loaded->ImageSize);
        }
    }
    return status;
}
static EFI_STATUS EFIAPI observed_exit(EFI_HANDLE image, UINTN key)
{
    /* Emit the final ranges before EBS invalidates boot services and the map
     * buffer's lifetime. Diagnostics do not allocate or change the map key. */
    if (final_map && key == final_key && final_stride >= sizeof(*final_map)) {
        for (UINTN offset = 0; offset < final_size; offset += final_stride) {
            EFI_MEMORY_DESCRIPTOR *d = (VOID *)((UINT8 *)final_map + offset);
            if (d->Type == EfiLoaderCode || d->Type == EfiLoaderData) {
                platform_service(IA64VMM_SVC_LOAD_BASE, d->PhysicalStart);
                platform_service(IA64VMM_SVC_LOAD_SIZE, EFI_PAGES_TO_SIZE(d->NumberOfPages));
                platform_service(IA64VMM_SVC_LOAD_TYPE, d->Type);
            }
        }
    }
    EFI_STATUS status = exit_boot_services(image, key);
    if (!EFI_ERROR(status)) {
        platform_service(IA64VMM_SVC_MAP_SIZE, final_size);
        platform_service(IA64VMM_SVC_EXIT_BOOT_SERVICES, key);
    }
    return status;
}
VOID observe_boot_media(VOID)
{
    if (get_memory_map)
        return;
    get_memory_map = gBS->GetMemoryMap;
    exit_boot_services = gBS->ExitBootServices;
    load_image = gBS->LoadImage;
    gBS->GetMemoryMap = observed_map;
    gBS->ExitBootServices = observed_exit;
    gBS->LoadImage = observed_load;
    gBS->Hdr.CRC32 = 0;
    gBS->CalculateCrc32(gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
}
