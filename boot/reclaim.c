/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/amd64.h"
#include "boot/ranges.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

/* Every CPU has left Limine. Retain the exact ELF span and high arena;
 * build the split map separately so capacity failures never publish a prefix. */
void amd64_reclaim(struct boot_info *bi)
{
    struct boot_mem_range ranges[BOOT_MAX_MEM_RANGES] = {0};
    unsigned count;
    uint64_t reclaimed, kept;
    LOGI("BootInfo ranges before reclaim=%u/%u", bi->mem_range_count, BOOT_MAX_MEM_RANGES);
    if (!boot_reclaim_ranges(bi, ranges, &count, amd64_image_pa,
                             (uintptr_t)__image_end - (uintptr_t)__image_start, amd64_memory_root(),
                             &reclaimed, &kept))
        hal_panic(
            "reclaim split failed: BootInfo capacity exceeded, invalid range or incomplete image reservation");
    memcpy(bi->mem, ranges, count * sizeof(*ranges));
    bi->mem_range_count = count;
    uint64_t guest = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (ranges[i].type != BOOT_MEM_USABLE)
            continue;
        uint64_t lo = IA64VMM_ALIGN_UP(ranges[i].base, 4096);
        uint64_t hi = IA64VMM_ALIGN_DOWN(ranges[i].base + ranges[i].size, 4096);
        if (hi > lo)
            guest += hi - lo;
    }
    LOGI(
        "relocation image retained VMM-owned %016llx..%016llx; reclaimed=%llu KiB guest RAM=%llu KiB",
        (unsigned long long)amd64_image_pa, (unsigned long long)(amd64_image_pa + kept),
        (unsigned long long)(reclaimed / 1024), (unsigned long long)(guest / 1024));
    LOGI("BootInfo ranges after reclaim=%u/%u", count, BOOT_MAX_MEM_RANGES);
}
