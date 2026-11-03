/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TEST_ACPI_MAP_H
#define TEST_ACPI_MAP_H
#include "firmware/new/acpi/tables.h"

/* Table/CPU/console fixtures supply an EFI map with one conventional
 * descriptor per handoff range. The SRAT tests use independent maps. */
static bool test_acpi_build(const struct ia64vmm_handoff *h, struct acpi_image *a, uint64_t base,
                            uint8_t facs[64], uint64_t facs_base)
{
    uint8_t map[IA64VMM_HANDOFF_MAX_RAM][40] = {{0}};
    if (h->ram_range_count > IA64VMM_HANDOFF_MAX_RAM)
        return false;
    for (unsigned i = 0; i < h->ram_range_count; ++i) {
        table_put(map[i], 7, 4); /* UEFI EfiConventionalMemory */
        table_put(map[i] + 8, h->ram[i].base, 8);
        table_put(map[i] + 24, h->ram[i].size / 4096, 8);
    }
    return acpi_build(h, a, base, facs, facs_base, map, h->ram_range_count * 40, 40);
}
#endif
