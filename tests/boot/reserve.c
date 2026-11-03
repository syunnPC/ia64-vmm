/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Limine PROTOCOL.md Memory Map: RAM classes, non-usable overlaps,
 * usable page alignment; a memory hole contributes no installed RAM. */
#include "boot/ranges.h"
#include "ia64vmm/memory_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GIB (UINT64_C(1) << 30)
#define MIB (UINT64_C(1) << 20)
static struct boot_info bi;
static void add(uint64_t base, uint64_t size, enum boot_mem_type type)
{
    assert(boot_range_append(bi.mem, &bi.mem_range_count,
                             (struct boot_mem_range){base, size, type, NULL}));
}
static struct boot_reservation plan(uint64_t request, uint64_t floor,
                                    enum boot_reserve_status expected)
{
    struct boot_reservation p;
    struct boot_info before = bi;
    assert(boot_reserve_plan(&bi, request, floor, &p) == expected);
    assert(!memcmp(&bi, &before, sizeof(bi)));
    if (expected == BOOT_RESERVE_OK) {
        assert(p.size <= p.ram_bytes / 2 && p.size <= p.largest_bytes && p.size >= floor);
        assert(!(p.size & 4095) && !(p.base & 4095));
    }
    return p;
}
int main(void)
{
    const uint64_t choices[] = {2, 3, 4, 6, 8, 12, 16};
    for (unsigned k = 0; k < sizeof(choices) / sizeof(choices[0]); ++k) {
        bi = (struct boot_info){0};
        add(0, 3 * GIB, BOOT_MEM_USABLE);
        add(3 * GIB, GIB, BOOT_MEM_RESERVED);
        add(4 * GIB, 29 * GIB, BOOT_MEM_USABLE);
        struct boot_reservation p = plan(choices[k] * GIB, 512 * MIB, BOOT_RESERVE_OK);
        assert(p.ram_bytes == 32 * GIB && p.half_bytes == 16 * GIB);
        assert(p.size == choices[k] * GIB && p.base == 33 * GIB - p.size);
    }
    bi = (struct boot_info){0};
    add(0, 2 * GIB, BOOT_MEM_USABLE);
    add(2 * GIB, 2 * GIB, BOOT_MEM_RESERVED);
    add(4 * GIB, 4 * GIB, BOOT_MEM_USABLE);
    add(UINT64_C(1) << 45, GIB, BOOT_MEM_FRAMEBUFFER);
    struct boot_reservation p = plan(16 * GIB, 512 * MIB, BOOT_RESERVE_OK);
    assert(p.ram_bytes == 6 * GIB && p.half_bytes == 3 * GIB && p.size == 3 * GIB);
    assert(p.base == 5 * GIB);
    p = plan(128 * MIB, 520 * MIB, BOOT_RESERVE_OK);
    assert(p.size == 520 * MIB);
    /* Non-usable RAM descriptions may overlap; count their union once. */
    bi = (struct boot_info){0};
    add(0, GIB, BOOT_MEM_USABLE);
    add(GIB, GIB, BOOT_MEM_VMM);
    add(GIB + MIB, GIB / 2, BOOT_MEM_ACPI_RECLAIMABLE);
    add(2 * GIB, GIB, BOOT_MEM_ACPI_NVS);
    add(3 * GIB, GIB, BOOT_MEM_BOOTLOADER);
    add(8 * GIB, GIB, BOOT_MEM_BAD);
    p = plan(2 * GIB, 512 * MIB, BOOT_RESERVE_OK);
    assert(p.ram_bytes == 4 * GIB && p.half_bytes == 2 * GIB && p.size == GIB);
    /* Fragmentation caps allocation even when the half-RAM allowance is larger. */
    bi = (struct boot_info){0};
    add(0, 2 * GIB, BOOT_MEM_USABLE);
    add(4 * GIB, 2 * GIB, BOOT_MEM_USABLE);
    add(8 * GIB, 2 * GIB, BOOT_MEM_USABLE);
    p = plan(3 * GIB, 512 * MIB, BOOT_RESERVE_OK);
    assert(p.size == 2 * GIB && p.base == 8 * GIB && p.ram_bytes == 6 * GIB);
    /* A minimum never overrides the hard half-RAM bound. */
    bi = (struct boot_info){0};
    add(0, GIB, BOOT_MEM_USABLE);
    plan(128 * MIB, 520 * MIB, BOOT_RESERVE_TOO_SMALL);
    p = plan(128 * MIB, 512 * MIB, BOOT_RESERVE_OK);
    assert(p.size == 512 * MIB);
    bi = (struct boot_info){0};
    add(0, 512 * MIB + 8192, BOOT_MEM_USABLE);
    p = plan(GIB, 0, BOOT_RESERVE_OK);
    assert(p.size == 256 * MIB + 4096);
    plan(1, 0, BOOT_RESERVE_INVALID);
    plan(GIB, 1, BOOT_RESERVE_INVALID);
    bi.mem[0].base = UINT64_MAX - 4095;
    bi.mem[0].size = 8192;
    plan(GIB, 0, BOOT_RESERVE_INVALID);
    bi = (struct boot_info){0};
    add(0, GIB, BOOT_MEM_USABLE);
    bi.mem_range_count = BOOT_MAX_MEM_RANGES + 1;
    uint64_t base;
    assert(!boot_reserve_range(&bi, MIB, &base));
    plan(GIB, 0, BOOT_RESERVE_INVALID);
    bi = (struct boot_info){0};
    plan(GIB, 0, BOOT_RESERVE_INVALID);
    assert(vmm_tier1_code_bytes(0) == 16 * MIB);
    assert(vmm_tier1_code_bytes(512 * MIB) == 16 * MIB);
    assert(vmm_tier1_code_bytes(GIB) == 32 * MIB);
    assert(vmm_tier1_code_bytes(16 * GIB) == 512 * MIB);
    assert(vmm_tier1_code_bytes(UINT64_MAX) == 512 * MIB);
    puts(
        "boot reserve: half-RAM, large requests, holes, overlapping RAM, fragmentation, minimum and overflow passed");
}
