/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ACPI 3.0b §5.2.16.2 (40-byte Memory Affinity Structure) and UEFI
 * §7.2.3: affinity describes RAM, not MMIO, unusable or absent pages. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include "firmware/new/acpi/tables.h"

static struct acpi_image acpi;
static uint8_t facs[64];
int main(void)
{
    struct ia64vmm_handoff h = {
        .cpu_count = 1,
        .ram_range_count = 2,
        /* The 4 KiB edges are not in the EFI map's whole 8 KiB granules. */
        .ram = {{0x1000, 0x20000}, {0x100001000, 0x20000}},
        .ecam_base = IA64VMM_ECAM_BASE,
        .ecam_size = IA64VMM_ECAM_SIZE,
        .ecam_bus_last = 255,
        .iosapic_base = IA64VMM_IOSAPIC_BASE,
        .iosapic_gsi_count = 24,
        .pib_base = IA64VMM_PIB_BASE,
        .acpi_pm_port_base = IA64VMM_ACPI_PM_PORT_BASE,
        .acpi_pm_port_size = IA64VMM_ACPI_PM_PORT_SIZE,
    };
    /* DescriptorSize may exceed sizeof(EFI_MEMORY_DESCRIPTOR); use the
     * UEFI definition as the independent wire-layout authority. */
    struct descriptor {
        EFI_MEMORY_DESCRIPTOR d;
        uint64_t extension;
    } map[] = {
        {.d = {.Type = EfiConventionalMemory, .PhysicalStart = 0x2000, .NumberOfPages = 2}},
        {.d = {.Type = EfiLoaderCode, .PhysicalStart = 0x4000, .NumberOfPages = 2}},
        {.d = {.Type = EfiLoaderData, .PhysicalStart = 0x6000, .NumberOfPages = 2}},
        {.d = {.Type = EfiBootServicesCode, .PhysicalStart = 0x8000, .NumberOfPages = 2}},
        {.d = {.Type = EfiBootServicesData, .PhysicalStart = 0xa000, .NumberOfPages = 2}},
        {.d = {.Type = EfiRuntimeServicesCode, .PhysicalStart = 0xc000, .NumberOfPages = 2}},
        {.d = {.Type = EfiRuntimeServicesData, .PhysicalStart = 0xe000, .NumberOfPages = 2}},
        {.d = {.Type = EfiACPIReclaimMemory, .PhysicalStart = 0x10000, .NumberOfPages = 2}},
        {.d = {.Type = EfiACPIMemoryNVS, .PhysicalStart = 0x12000, .NumberOfPages = 2}},
        {.d = {.Type = EfiPalCode, .PhysicalStart = 0x14000, .NumberOfPages = 2}},
        {.d = {.Type = EfiMemoryMappedIO, .PhysicalStart = 0x16000, .NumberOfPages = 2}},
        {.d = {.Type = EfiMemoryMappedIOPortSpace, .PhysicalStart = 0x18000, .NumberOfPages = 2}},
        {.d = {.Type = EfiReservedMemoryType, .PhysicalStart = 0x1a000, .NumberOfPages = 2}},
        {.d = {.Type = EfiUnusableMemory, .PhysicalStart = 0x1c000, .NumberOfPages = 2}},
        {.d = {.Type = EfiMaxMemoryType, .PhysicalStart = 0x1e000, .NumberOfPages = 2}},
        {.d = {.Type = EfiConventionalMemory, .PhysicalStart = 0x100002000, .NumberOfPages = 30}},
    };
    memset(&acpi, 0xff, sizeof(acpi));
    memset(facs, 0xff, sizeof(facs));
    assert(acpi_build(&h, &acpi, 0x40000000, facs, 0x50000000, map, sizeof(map), sizeof(map[0])));
    const uint8_t *s = acpi.bytes + acpi.srat;
    size_t length = (size_t)table_get(s + 4, 4);
    assert(s[8] == 1 && table_sum(s, length) == 0);
    unsigned entries = 0;
    uint64_t total = 0;
    for (size_t at = 48; at < length; at += s[at + 1]) {
        const uint8_t *m = s + at;
        assert(at + 2 <= length && m[1] >= 2 && m[1] <= length - at);
        if (m[0] != 1)
            continue;
        assert(m[1] == 40 && table_get(m + 2, 4) == 0 && table_get(m + 28, 4) == 1);
        assert(table_get(m + 6, 2) == 0 && table_get(m + 24, 4) == 0 && table_get(m + 32, 8) == 0);
        uint64_t base = table_get(m + 8, 8), size = table_get(m + 16, 8);
        assert(size && size <= UINT64_MAX - base);
        /* The union is exactly [0x2000,0x16000), [0x100002000,0x100020000). */
        assert((base >= 0x2000 && base + size <= 0x16000) ||
               (base >= 0x100002000 && base + size <= 0x100020000));
        for (size_t before = 48; before < at; before += s[before + 1]) {
            const uint8_t *other = s + before;
            if (other[0] != 1)
                continue;
            uint64_t b = table_get(other + 8, 8), n = table_get(other + 16, 8);
            assert(base >= b + n || b >= base + size);
        }
        total += size;
        ++entries;
    }
    assert(entries && total == 0x32000);
    assert(!acpi_build(&h, &acpi, 0x40000000, facs, 0x50000000, NULL, sizeof(map), sizeof(map[0])));
    assert(
        !acpi_build(&h, &acpi, 0x40000000, facs, 0x50000000, map, sizeof(map) - 1, sizeof(map[0])));
    assert(!acpi_build(&h, &acpi, 0x40000000, facs, 0x50000000, map, sizeof(map), 24));
    map[0].d.NumberOfPages = 0;
    assert(!acpi_build(&h, &acpi, 0x40000000, facs, 0x50000000, map, sizeof(map), sizeof(map[0])));
    map[0].d.NumberOfPages = UINT64_MAX;
    assert(!acpi_build(&h, &acpi, 0x40000000, facs, 0x50000000, map, sizeof(map), sizeof(map[0])));
    map[0].d.NumberOfPages = 2;
    map[0].d.PhysicalStart = 1;
    assert(!acpi_build(&h, &acpi, 0x40000000, facs, 0x50000000, map, sizeof(map), sizeof(map[0])));
    map[0].d.PhysicalStart = 0x2000;
    /* UEFI does not require descriptor order; reversing the map must
     * describe exactly the same physical ranges. */
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]) / 2; ++i) {
        struct descriptor tmp = map[i];
        map[i] = map[sizeof(map) / sizeof(map[0]) - 1 - i];
        map[sizeof(map) / sizeof(map[0]) - 1 - i] = tmp;
    }
    assert(acpi_build(&h, &acpi, 0x40000000, facs, 0x50000000, map, sizeof(map), sizeof(map[0])));
    total = 0;
    s = acpi.bytes + acpi.srat;
    for (size_t at = 48; at < table_get(s + 4, 4); at += s[at + 1])
        if (s[at] == 1)
            total += table_get(s + at + 16, 8);
    assert(total == 0x32000);
    /* UEFI 2.7 §7.2.3, Memory Type Usage after ExitBootServices(): loader and boot
     * services storage becomes available; runtime, ACPI NVS and PAL storage
     * must be preserved; ACPI reclaim waits for ACPI enable. All are RAM.
     * Reserved/unusable and I/O space must not be used as system RAM.
     * ACPI 3.0b §5.2.16.2: Enabled=1, Hot Pluggable=0, reserved bits=0. */
    const struct {
        UINT32 type;
        bool affinity;
    } usage[] = {
        {EfiReservedMemoryType, false},
        {EfiLoaderCode, true},
        {EfiLoaderData, true},
        {EfiBootServicesCode, true},
        {EfiBootServicesData, true},
        {EfiRuntimeServicesCode, true},
        {EfiRuntimeServicesData, true},
        {EfiConventionalMemory, true},
        {EfiUnusableMemory, false},
        {EfiACPIReclaimMemory, true},
        {EfiACPIMemoryNVS, true},
        {EfiMemoryMappedIO, false},
        {EfiMemoryMappedIOPortSpace, false},
        {EfiPalCode, true},
        {EfiMaxMemoryType, false},
    };
    for (unsigned i = 0; i < sizeof(usage) / sizeof(usage[0]); ++i) {
        EFI_MEMORY_DESCRIPTOR d[] = {
            {.Type = EfiConventionalMemory, .PhysicalStart = 0, .NumberOfPages = 2},
            {.Type = usage[i].type, .PhysicalStart = 0x2000, .NumberOfPages = 2},
            {.Type = EfiConventionalMemory, .PhysicalStart = 0x4000, .NumberOfPages = 2},
        };
        assert(acpi_build(&h, &acpi, 0x40000000, facs, 0x50000000, d, sizeof(d), sizeof(d[0])));
        s = acpi.bytes + acpi.srat;
        size_t at = 48 + 16; /* one processor affinity */
        assert(s[at] == 1 && table_get(s + at + 8, 8) == 0);
        assert(table_get(s + at + 28, 4) == 1);
        if (usage[i].affinity) {
            assert(table_get(s + at + 16, 8) == 0x6000);
            assert(table_get(s + 4, 4) == at + 40);
        } else {
            assert(table_get(s + at + 16, 8) == 0x2000);
            at += 40;
            assert(s[at] == 1 && table_get(s + at + 8, 8) == 0x4000);
            assert(table_get(s + at + 16, 8) == 0x2000);
            assert(table_get(s + at + 28, 4) == 1);
            assert(table_get(s + 4, 4) == at + 40);
        }
    }
    puts("SRAT: EFI RAM types, trimmed edges, holes, high memory and wire layout passed");
    return 0;
}
