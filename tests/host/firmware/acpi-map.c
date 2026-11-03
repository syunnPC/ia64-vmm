/* SPDX-License-Identifier: GPL-2.0-or-later */
/* UEFI GetMemoryMap returns a versioned, strided snapshot without an
 * address-order guarantee. Adjacent RAM has one ACPI affinity regardless
 * of insertion order or descriptor extensions. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "firmware/new/acpi/driver.c"

static struct descriptor {
    EFI_MEMORY_DESCRIPTOR d;
    UINT64 extension;
} snapshot[1024];
static UINT32 map_version;
static unsigned allocated;
static EFI_STATUS EFIAPI get_map(UINTN *bytes, EFI_MEMORY_DESCRIPTOR *map, UINTN *key,
                                 UINTN *stride, UINT32 *version)
{
    *key = 1;
    *stride = sizeof(snapshot[0]);
    *version = map_version;
    if (!map || *bytes < sizeof(snapshot)) {
        *bytes = sizeof(snapshot);
        return EFI_BUFFER_TOO_SMALL;
    }
    *bytes = sizeof(snapshot);
    memcpy(map, snapshot, sizeof(snapshot));
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI pool(EFI_MEMORY_TYPE type, UINTN bytes, VOID **out)
{
    assert(type == EfiBootServicesData);
    *out = malloc(bytes);
    assert(*out);
    ++allocated;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI release(VOID *p)
{
    free(p);
    --allocated;
    return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES services = {
    .GetMemoryMap = get_map, .AllocatePool = pool, .FreePool = release};
EFI_BOOT_SERVICES *gBS = &services;

int main(void)
{
    struct ia64vmm_handoff h = {
        .cpu_count = 1,
        .ram_range_count = 1,
        .ram = {{0, 1024 * 8192}},
        .ecam_base = IA64VMM_ECAM_BASE,
        .ecam_size = IA64VMM_ECAM_SIZE,
        .ecam_bus_last = 255,
        .iosapic_base = IA64VMM_IOSAPIC_BASE,
        .iosapic_gsi_count = 24,
        .pib_base = IA64VMM_PIB_BASE,
        .acpi_pm_port_base = IA64VMM_ACPI_PM_PORT_BASE,
        .acpi_pm_port_size = IA64VMM_ACPI_PM_PORT_SIZE,
    };
    /* Odd addresses followed by even addresses: no adjacent descriptors
     * in list order, but the physical union is contiguous. */
    for (unsigned i = 0; i < 1024; ++i) {
        snapshot[i].d.Type = EfiConventionalMemory;
        snapshot[i].d.PhysicalStart = (i < 512 ? 2 * i + 1 : 2 * (i - 512)) * 8192;
        snapshot[i].d.NumberOfPages = 2;
        snapshot[i].extension = snapshot[i].d.PhysicalStart ^ 0xabcdef;
    }
    static struct acpi_image acpi;
    _Alignas(64) uint8_t facs[64];
    for (unsigned version = 0; version <= 2; ++version) {
        map_version = version;
        EFI_STATUS status = acpi_table(&h, &acpi, 0x40000000, (UINTN)facs);
        assert(!allocated);
        if (version != EFI_MEMORY_DESCRIPTOR_VERSION) {
            assert(status == EFI_INVALID_PARAMETER);
            continue;
        }
        assert(status == EFI_SUCCESS);
        const uint8_t *s = acpi.bytes + acpi.srat;
        assert(table_get(s + 4, 4) == 48 + 16 + 40 && table_sum(s, 48 + 16 + 40) == 0);
        assert(table_get(s + 48 + 16 + 8, 8) == 0);
        assert(table_get(s + 48 + 16 + 16, 8) == 1024 * 8192);
    }
    struct descriptor sorted[1024];
    memcpy(sorted, snapshot, sizeof(sorted));
    assert(sort_map((UINT8 *)sorted, sizeof(sorted), sizeof(sorted[0])));
    for (unsigned i = 0; i < 1024; ++i) {
        assert(sorted[i].d.PhysicalStart == i * 8192);
        assert(sorted[i].extension == ((i * 8192) ^ 0xabcdef));
    }
    assert(!sort_map((UINT8 *)sorted, sizeof(sorted) - 1, sizeof(sorted[0])));
    assert(!sort_map((UINT8 *)sorted, sizeof(sorted), 0));
    /* Sorting must never change the firmware-owned memory map. */
    assert(snapshot[0].d.PhysicalStart == 8192 && snapshot[512].d.PhysicalStart == 0);
    return 0;
}
