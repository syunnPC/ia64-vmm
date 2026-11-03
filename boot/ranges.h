/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_BOOT_RANGES_H
#define VMM_BOOT_RANGES_H
#include "ia64vmm/bootinfo.h"
bool boot_range_append(struct boot_mem_range *, unsigned *, struct boot_mem_range);
/* Highest page-aligned usable span. */
bool boot_reserve_range(const struct boot_info *, uint64_t size, uint64_t *base);
struct boot_reservation {
    uint64_t ram_bytes, half_bytes, largest_bytes, base, size;
};
enum boot_reserve_status { BOOT_RESERVE_OK, BOOT_RESERVE_INVALID, BOOT_RESERVE_TOO_SMALL };
/* Requests are bounded by half the known RAM and one contiguous usable span. */
enum boot_reserve_status boot_reserve_plan(const struct boot_info *, uint64_t requested,
                                           uint64_t minimum, struct boot_reservation *);
bool boot_reclaim_ranges(const struct boot_info *, struct boot_mem_range *, unsigned *,
                         uint64_t image, uint64_t image_size, uint64_t arena, uint64_t *reclaimed,
                         uint64_t *kept);
#endif
