/* SPDX-License-Identifier: GPL-2.0-or-later */
/* UEFI 2.11: Loaded Image DeviceHandle, LocateDevicePath's closest match,
 * PCI I/O GetLocation, and LoadImage with a full file device path. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <efi.h>
#include "ia64vmm/bootmedia.h"

EFI_STATUS EFIAPI efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE *);

static EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID media_guid = BOOT_MEDIA_GUID;
static EFI_BOOT_SERVICES services;
static EFI_SYSTEM_TABLE system_table;
static EFI_LOADED_IMAGE loaded;
static EFI_PCI_IO_PROTOCOL pci;
static unsigned char volume_path[256], pci_path[128], child_path[512], allocation[4096];
static size_t volume_bytes, pci_bytes, matched;
static unsigned reads, locations, loads, starts, installs, frees;
static unsigned segment, bus, device, function, class_code;
static EFI_STATUS locate_status, location_status, config_status, load_status, start_status;
static EFI_STATUS pages_status, pool_status, install_status;
static void *installed;
static EFI_HANDLE image = (EFI_HANDLE)1, volume = (EFI_HANDLE)2, controller = (EFI_HANDLE)3,
                  child = (EFI_HANDLE)4;

static int guid_is(const EFI_GUID *a, const EFI_GUID *b)
{
    return !memcmp(a, b, sizeof(*a));
}
static size_t node(unsigned char *p, unsigned type, unsigned subtype, unsigned bytes)
{
    memset(p, 0, bytes);
    p[0] = type;
    p[1] = subtype;
    p[2] = bytes;
    p[3] = bytes >> 8;
    return bytes;
}
static void fixture(int with_pci)
{
    memset(volume_path, 0, sizeof(volume_path));
    volume_bytes = node(volume_path, ACPI_DEVICE_PATH, ACPI_DP, 12);
    if (with_pci)
        volume_bytes += node(volume_path + volume_bytes, HARDWARE_DEVICE_PATH, HW_PCI_DP, 6);
    pci_bytes = volume_bytes;
    memcpy(pci_path, volume_path, pci_bytes);
    node(pci_path + pci_bytes, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, 4);
    matched = pci_bytes;
    volume_bytes += node(volume_path + volume_bytes, MESSAGING_DEVICE_PATH,
                         with_pci ? MSG_USB_DP : MSG_VENDOR_DP, 20);
    volume_bytes += node(volume_path + volume_bytes, MEDIA_DEVICE_PATH, MEDIA_HARDDRIVE_DP, 42);
    volume_bytes +=
        node(volume_path + volume_bytes, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, 4);
    reads = locations = loads = starts = installs = frees = 0;
    installed = NULL;
    memset(allocation, 0xa5, sizeof(allocation));
    segment = 0;
    bus = 4;
    device = 2;
    function = 1;
    class_code = 0x0c0330;
    locate_status = location_status = config_status = load_status = start_status = EFI_SUCCESS;
    pages_status = pool_status = install_status = EFI_SUCCESS;
    system_table.NumberOfTableEntries = 0;
    system_table.ConfigurationTable = NULL;
}
static EFI_STATUS EFIAPI protocol(EFI_HANDLE handle, EFI_GUID *guid, void **out)
{
    if (handle == image && guid_is(guid, &loaded_guid))
        *out = &loaded;
    else if (handle == volume && guid_is(guid, &path_guid))
        *out = volume_path;
    else if (handle == controller && guid_is(guid, &path_guid))
        *out = pci_path;
    else if (handle == controller && guid_is(guid, &pci_guid))
        *out = &pci;
    else
        return EFI_NOT_FOUND;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI locate(EFI_GUID *guid, EFI_DEVICE_PATH **path, EFI_HANDLE *out)
{
    assert(guid_is(guid, &pci_guid));
    assert((void *)*path == volume_path);
    if (locate_status != EFI_SUCCESS)
        return locate_status;
    *path = (EFI_DEVICE_PATH *)(volume_path + matched);
    *out = controller;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI get_location(EFI_PCI_IO_PROTOCOL *self, UINTN *seg, UINTN *b, UINTN *d,
                                      UINTN *f)
{
    assert(self == &pci);
    ++locations;
    *seg = segment;
    *b = bus;
    *d = device;
    *f = function;
    return location_status;
}
static EFI_STATUS EFIAPI config_read(EFI_PCI_IO_PROTOCOL *self, EFI_PCI_IO_PROTOCOL_WIDTH width,
                                     UINT32 offset, UINTN count, void *out)
{
    assert(self == &pci && width == EfiPciIoWidthUint32 && offset == 8 && count == 1);
    ++reads;
    *(UINT32 *)out = class_code << 8 | 0x12;
    return config_status;
}
static EFI_STATUS EFIAPI allocate(EFI_ALLOCATE_TYPE type, EFI_MEMORY_TYPE memory, UINTN pages,
                                  EFI_PHYSICAL_ADDRESS *out)
{
    assert(type == AllocateAnyPages && memory == EfiACPIReclaimMemory && pages == 1);
    if (pages_status != EFI_SUCCESS)
        return pages_status;
    *out = (UINTN)allocation;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_pages(EFI_PHYSICAL_ADDRESS address, UINTN pages)
{
    assert(address == (UINTN)allocation && pages == 1);
    ++frees;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI allocate_pool(EFI_MEMORY_TYPE memory, UINTN bytes, void **out)
{
    assert(memory == EfiLoaderData && bytes < sizeof(child_path));
    *out = child_path;
    return pool_status;
}
static EFI_STATUS EFIAPI free_pool(void *p)
{
    assert(p == child_path);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI install(EFI_GUID *guid, void *table)
{
    assert(guid_is(guid, &media_guid));
    ++installs;
    if (install_status != EFI_SUCCESS)
        return install_status;
    installed = table;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI load(BOOLEAN policy, EFI_HANDLE parent, EFI_DEVICE_PATH *path,
                              void *source, UINTN size, EFI_HANDLE *out)
{
    assert(!policy && parent == image && !source && !size && installed == allocation);
    assert(!memcmp(path, volume_path, volume_bytes - 4));
    const EFI_DEVICE_PATH *file = (void *)((unsigned char *)path + volume_bytes - 4);
    static const UINT16 expected[] = {'\\', 'E', 'F', 'I', '\\', 'v', 'm', 'm', '\\', 'l',
                                      'i',  'm', 'i', 'n', 'e',  '.', 'e', 'f', 'i',  0};
    assert(file->Type == MEDIA_DEVICE_PATH && file->SubType == MEDIA_FILEPATH_DP);
    assert(DevicePathNodeLength(file) == 4 + sizeof(expected));
    assert(!memcmp((unsigned char *)file + 4, expected, sizeof(expected)));
    assert(IsDevicePathEnd(NextDevicePathNode(file)));
    ++loads;
    *out = child;
    return load_status;
}
static EFI_STATUS EFIAPI start(EFI_HANDLE handle, UINTN *size, CHAR16 **data)
{
    assert(handle == child && !size && !data && installed == allocation);
    ++starts;
    return start_status;
}
static EFI_STATUS EFIAPI unload(EFI_HANDLE handle)
{
    assert(handle == child);
    return EFI_SUCCESS;
}
static const struct boot_media *record(void)
{
    return (void *)allocation;
}
static void run(void)
{
    assert(efi_main(image, &system_table) == start_status);
    assert(loads == 1 && starts == 1 && installs == 2 && !installed && frees == 1);
    assert(record()->magic == BOOT_MEDIA_MAGIC && record()->version == BOOT_MEDIA_VERSION);
    assert(record()->size == sizeof(struct boot_media) && !record()->reserved);
}
int main(void)
{
    services.HandleProtocol = protocol;
    services.LocateDevicePath = locate;
    services.AllocatePages = allocate;
    services.FreePages = free_pages;
    services.AllocatePool = allocate_pool;
    services.FreePool = free_pool;
    services.InstallConfigurationTable = install;
    services.LoadImage = load;
    services.StartImage = start;
    services.UnloadImage = unload;
    system_table.BootServices = &services;
    loaded.DeviceHandle = volume;
    pci.GetLocation = get_location;
    pci.Pci.Read = config_read;

    const unsigned supported[] = {0x0c0320, 0x0c0330, 0x010802, 0x010601};
    for (unsigned i = 0; i < sizeof(supported) / sizeof(*supported); ++i) {
        fixture(1);
        class_code = supported[i];
        run();
        assert(record()->status == BOOT_MEDIA_PCI && record()->segment == 0);
        assert(record()->bdf == 0x0411 && record()->class_code == class_code && reads == 1 &&
               locations == 1);
    }
    fixture(0);
    run();
    assert(record()->status == BOOT_MEDIA_NONE && !reads && !locations);
    fixture(1);
    class_code = 0x010000;
    run();
    assert(record()->status == BOOT_MEDIA_UNSUPPORTED);
    fixture(1);
    class_code = 0x060400;
    run();
    assert(record()->status == BOOT_MEDIA_INVALID);
    fixture(1);
    locate_status = EFI_NOT_FOUND;
    run();
    assert(record()->status == BOOT_MEDIA_INVALID);
    fixture(1);
    location_status = EFI_DEVICE_ERROR;
    run();
    assert(record()->status == BOOT_MEDIA_INVALID);
    fixture(1);
    config_status = EFI_DEVICE_ERROR;
    run();
    assert(record()->status == BOOT_MEDIA_INVALID);
    fixture(1);
    segment = 1;
    run();
    assert(record()->status == BOOT_MEDIA_PCI && record()->segment == 1);
    fixture(1);
    bus = 256;
    run();
    assert(record()->status == BOOT_MEDIA_INVALID);

    /* A PCI bridge prefix cannot identify the endpoint below it. */
    fixture(1);
    memmove(volume_path + matched + 6, volume_path + matched, volume_bytes - matched);
    node(volume_path + matched, HARDWARE_DEVICE_PATH, HW_PCI_DP, 6);
    volume_bytes += 6;
    run();
    assert(record()->status == BOOT_MEDIA_INVALID && !reads && !locations);
    fixture(1);
    matched++;
    run();
    assert(record()->status == BOOT_MEDIA_INVALID);
    fixture(1);
    pci_path[5] ^= 1;
    run();
    assert(record()->status == BOOT_MEDIA_INVALID);

    fixture(1);
    volume_path[2] = 0;
    assert(efi_main(image, &system_table) == EFI_COMPROMISED_DATA && !loads && !starts);
    fixture(1);
    volume_path[volume_bytes - 3] = END_INSTANCE_DEVICE_PATH_SUBTYPE;
    assert(efi_main(image, &system_table) == EFI_COMPROMISED_DATA && !loads && !starts);
    fixture(1);
    load_status = EFI_NOT_FOUND;
    assert(efi_main(image, &system_table) == EFI_NOT_FOUND && !starts && !installed && frees == 1);
    fixture(1);
    start_status = EFI_ABORTED;
    run();
    fixture(1);
    pages_status = EFI_OUT_OF_RESOURCES;
    assert(efi_main(image, &system_table) == EFI_OUT_OF_RESOURCES && !installs && !loads && !frees);
    fixture(1);
    pool_status = EFI_OUT_OF_RESOURCES;
    assert(efi_main(image, &system_table) == EFI_OUT_OF_RESOURCES && installs == 2 && !installed &&
           !loads && frees == 1);
    fixture(1);
    install_status = EFI_OUT_OF_RESOURCES;
    assert(efi_main(image, &system_table) == EFI_OUT_OF_RESOURCES && installs == 1 && !loads &&
           frees == 1);
    fixture(1);
    EFI_CONFIGURATION_TABLE old = {BOOT_MEDIA_GUID, (void *)5};
    system_table.ConfigurationTable = &old;
    system_table.NumberOfTableEntries = 1;
    assert(efi_main(image, &system_table) == EFI_SUCCESS && installed == old.VendorTable &&
           frees == 1);
    puts("bootmedia UEFI mocks: passed");
    return 0;
}
