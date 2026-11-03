/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <efi.h>
#include "ia64vmm/bootmedia.h"

#define PATH_LIMIT 4096u
static EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID media_guid = BOOT_MEDIA_GUID;
static const CHAR16 child_name[] = {'\\', 'E', 'F', 'I', '\\', 'v', 'm', 'm', '\\', 'l',
                                    'i',  'm', 'i', 'n', 'e',  '.', 'e', 'f', 'i',  0};

static void copy_bytes(void *out, const void *in, UINTN count)
{
    UINT8 *d = out;
    const UINT8 *s = in;
    while (count--)
        *d++ = *s++;
}
static BOOLEAN equal_bytes(const void *a, const void *b, UINTN count)
{
    const UINT8 *p = a, *q = b;
    while (count--)
        if (*p++ != *q++)
            return FALSE;
    return TRUE;
}

/* A single complete path; LocateDevicePath only resolves its first instance. */
static UINTN path_bytes(const EFI_DEVICE_PATH *path, UINTN *last_pci)
{
    if (last_pci)
        *last_pci = 0;
    if (!path)
        return 0;
    for (UINTN at = 0; at <= PATH_LIMIT - sizeof(*path);) {
        const EFI_DEVICE_PATH *p = (const void *)((const UINT8 *)path + at);
        UINTN bytes = DevicePathNodeLength(p);
        if (bytes < sizeof(*p) || bytes > PATH_LIMIT - at || (p->Type & EFI_DP_TYPE_UNPACKED))
            return 0;
        if (p->Type == END_DEVICE_PATH_TYPE)
            return p->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE && bytes == sizeof(*p) ? at + bytes
                                                                                       : 0;
        if (p->Type == HARDWARE_DEVICE_PATH && p->SubType == HW_PCI_DP) {
            if (bytes != sizeof(PCI_DEVICE_PATH))
                return 0;
            if (last_pci)
                *last_pci = at + bytes;
        }
        at += bytes;
    }
    return 0;
}

static void identify(EFI_BOOT_SERVICES *bs, EFI_DEVICE_PATH *path, UINTN bytes,
                     struct boot_media *media)
{
    UINTN last_pci;
    (void)path_bytes(path, &last_pci);
    if (!last_pci) {
        media->status = BOOT_MEDIA_NONE;
        return;
    }
    media->status = BOOT_MEDIA_INVALID;
    EFI_DEVICE_PATH *remaining = path, *controller_path;
    EFI_HANDLE controller;
    EFI_PCI_IO_PROTOCOL *pci;
    if (bs->LocateDevicePath(&pci_guid, &remaining, &controller) != EFI_SUCCESS)
        return;
    UINTN start = (UINTN)path, rest = (UINTN)remaining;
    if (rest < start || rest - start > bytes - sizeof(*path) || rest - start != last_pci)
        return;
    if (bs->HandleProtocol(controller, &path_guid, (void **)&controller_path) != EFI_SUCCESS)
        return;
    UINTN controller_last, controller_bytes = path_bytes(controller_path, &controller_last);
    if (controller_last != last_pci || controller_bytes != last_pci + sizeof(*path) ||
        !equal_bytes(controller_path, path, last_pci))
        return;
    if (bs->HandleProtocol(controller, &pci_guid, (void **)&pci) != EFI_SUCCESS)
        return;
    UINTN segment, bus, device, function;
    UINT32 revision_class;
    if (pci->GetLocation(pci, &segment, &bus, &device, &function) != EFI_SUCCESS ||
        segment > UINT16_MAX || bus > 255 || device > 31 || function > 7 ||
        pci->Pci.Read(pci, EfiPciIoWidthUint32, 8, 1, &revision_class) != EFI_SUCCESS)
        return;
    UINT32 class_code = revision_class >> 8;
    if (class_code >> 16 == 6 || class_code == 0xffffff)
        return;
    media->segment = segment;
    media->bdf = bus << 8 | device << 3 | function;
    media->class_code = class_code;
    media->status = class_code == 0x0c0320 || class_code == 0x0c0330 || class_code == 0x010802 ||
                            class_code == 0x010601
                        ? BOOT_MEDIA_PCI
                        : BOOT_MEDIA_UNSUPPORTED;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    EFI_BOOT_SERVICES *bs = system->BootServices;
    EFI_LOADED_IMAGE *loaded;
    EFI_DEVICE_PATH *volume_path;
    EFI_STATUS status = bs->HandleProtocol(image, &loaded_guid, (void **)&loaded);
    if (status != EFI_SUCCESS)
        return status;
    status = bs->HandleProtocol(loaded->DeviceHandle, &path_guid, (void **)&volume_path);
    if (status != EFI_SUCCESS)
        return status;
    UINTN bytes = path_bytes(volume_path, NULL);
    if (!bytes)
        return EFI_COMPROMISED_DATA;

    EFI_PHYSICAL_ADDRESS address;
    status = bs->AllocatePages(AllocateAnyPages, EfiACPIReclaimMemory, 1, &address);
    if (status != EFI_SUCCESS)
        return status;
    struct boot_media *media = (void *)(UINTN)address;
    *media = (struct boot_media){
        .magic = BOOT_MEDIA_MAGIC, .version = BOOT_MEDIA_VERSION, .size = sizeof(*media)};
    identify(bs, volume_path, bytes, media);

    void *previous = NULL;
    for (UINTN i = 0; i < system->NumberOfTableEntries; ++i)
        if (equal_bytes(&system->ConfigurationTable[i].VendorGuid, &media_guid, sizeof(media_guid)))
            previous = system->ConfigurationTable[i].VendorTable;
    status = bs->InstallConfigurationTable(&media_guid, media);
    if (status != EFI_SUCCESS) {
        (void)bs->FreePages(address, 1);
        return status;
    }

    EFI_DEVICE_PATH *child_path = NULL;
    UINTN prefix = bytes - sizeof(*volume_path),
          file_bytes = sizeof(EFI_DEVICE_PATH) + sizeof(child_name);
    status = bs->AllocatePool(EfiLoaderData, prefix + file_bytes + sizeof(*volume_path),
                              (void **)&child_path);
    if (status == EFI_SUCCESS) {
        copy_bytes(child_path, volume_path, prefix);
        EFI_DEVICE_PATH *file = (void *)((UINT8 *)child_path + prefix);
        file->Type = MEDIA_DEVICE_PATH;
        file->SubType = MEDIA_FILEPATH_DP;
        SetDevicePathNodeLength(file, file_bytes);
        copy_bytes((UINT8 *)file + sizeof(*file), child_name, sizeof(child_name));
        EFI_DEVICE_PATH *end = (void *)((UINT8 *)file + file_bytes);
        SetDevicePathEndNode(end);
        EFI_HANDLE child;
        status = bs->LoadImage(FALSE, image, child_path, NULL, 0, &child);
        (void)bs->FreePool(child_path);
        if (status == EFI_SUCCESS)
            status = bs->StartImage(child, NULL, NULL);
    }
    if (bs->InstallConfigurationTable(&media_guid, previous) == EFI_SUCCESS)
        (void)bs->FreePages(address, 1);
    return status;
}
