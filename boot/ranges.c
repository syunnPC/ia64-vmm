/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "boot/ranges.h"

bool boot_reserve_range(const struct boot_info *bi, uint64_t size, uint64_t *base)
{
    uint64_t highest = 0;
    if (!size || size % IA64VMM_PAGE_SIZE || bi->mem_range_count > BOOT_MAX_MEM_RANGES)
        return false;
    for (unsigned i = 0; i < bi->mem_range_count; ++i) {
        const struct boot_mem_range *m = &bi->mem[i];
        if (m->size > UINT64_MAX - m->base)
            return false;
        if (m->type != BOOT_MEM_USABLE)
            continue;
        uint64_t end = m->base + m->size;
        end = IA64VMM_ALIGN_DOWN(end, IA64VMM_PAGE_SIZE);
        if (m->base > UINT64_MAX - (IA64VMM_PAGE_SIZE - 1))
            continue;
        uint64_t start = IA64VMM_ALIGN_UP(m->base, IA64VMM_PAGE_SIZE);
        if (end >= start && end - start >= size && end > highest)
            highest = end;
    }
    if (!highest)
        return false;
    *base = highest - size;
    return true;
}

bool boot_range_append(struct boot_mem_range *ranges, unsigned *count, struct boot_mem_range r)
{
    if (!r.size)
        return true;
    if (*count >= BOOT_MAX_MEM_RANGES || r.size > UINT64_MAX - r.base)
        return false;
    ranges[(*count)++] = r;
    return true;
}
bool boot_reclaim_ranges(const struct boot_info *bi, struct boot_mem_range *out, unsigned *count,
                         uint64_t image, uint64_t size, uint64_t arena, uint64_t *reclaimed,
                         uint64_t *kept)
{
    *count = 0;
    *reclaimed = *kept = 0;
    if (!size || size > UINT64_MAX - image || bi->mem_range_count > BOOT_MAX_MEM_RANGES)
        return false;
    uint64_t image_end = image + size;
    for (unsigned i = 0; i < bi->mem_range_count; ++i) {
        struct boot_mem_range r = bi->mem[i];
        if (r.size > UINT64_MAX - r.base)
            return false;
        uint64_t end = r.base + r.size;
        bool overlaps = r.base < image_end && image < end;
        if (r.type == BOOT_MEM_BOOTLOADER) {
            if (overlaps)
                return false;
            *reclaimed += r.size;
            r.type = BOOT_MEM_USABLE;
        } else if (r.type == BOOT_MEM_VMM && overlaps) {
            uint64_t lo = r.base > image ? r.base : image;
            uint64_t hi = end < image_end ? end : image_end;
            struct boot_mem_range prefix = {r.base, lo - r.base, BOOT_MEM_USABLE, r.hva};
            struct boot_mem_range body = {lo, hi - lo, BOOT_MEM_VMM,
                                          r.hva ? r.hva + (lo - r.base) : NULL};
            if (!boot_range_append(out, count, prefix) || !boot_range_append(out, count, body))
                return false;
            *kept += hi - lo;
            *reclaimed += prefix.size + end - hi;
            r.hva = r.hva ? r.hva + (hi - r.base) : NULL;
            r.base = hi;
            r.size = end - hi;
            r.type = BOOT_MEM_USABLE;
        } else if (r.type == BOOT_MEM_VMM && !(arena >= r.base && arena - r.base < r.size)) {
            *reclaimed += r.size;
            r.type = BOOT_MEM_USABLE;
        }
        if (!boot_range_append(out, count, r))
            return false;
    }
    return *kept == size;
}

static bool known_ram(enum boot_mem_type type)
{
    return type == BOOT_MEM_USABLE || type == BOOT_MEM_ACPI_RECLAIMABLE ||
           type == BOOT_MEM_ACPI_NVS || type == BOOT_MEM_BOOTLOADER || type == BOOT_MEM_VMM;
}

enum boot_reserve_status boot_reserve_plan(const struct boot_info *bi, uint64_t requested,
                                           uint64_t minimum, struct boot_reservation *out)
{
    struct {
        uint64_t base, end;
    } ram[BOOT_MAX_MEM_RANGES];
    unsigned n = 0;
    *out = (struct boot_reservation){0};
    if (!requested || requested % IA64VMM_PAGE_SIZE || minimum % IA64VMM_PAGE_SIZE ||
        !bi->mem_range_count || bi->mem_range_count > BOOT_MAX_MEM_RANGES)
        return BOOT_RESERVE_INVALID;
    for (unsigned i = 0; i < bi->mem_range_count; ++i) {
        const struct boot_mem_range *m = &bi->mem[i];
        if (m->size > UINT64_MAX - m->base)
            return BOOT_RESERVE_INVALID;
        uint64_t end = m->base + m->size;
        if (m->type == BOOT_MEM_USABLE && m->base <= UINT64_MAX - (IA64VMM_PAGE_SIZE - 1)) {
            uint64_t start = IA64VMM_ALIGN_UP(m->base, IA64VMM_PAGE_SIZE);
            uint64_t stop = IA64VMM_ALIGN_DOWN(end, IA64VMM_PAGE_SIZE);
            if (stop > start && stop - start > out->largest_bytes)
                out->largest_bytes = stop - start;
        }
        if (!m->size || !known_ram(m->type))
            continue;
        unsigned j = n++;
        while (j && ram[j - 1].base > m->base) {
            ram[j] = ram[j - 1];
            --j;
        }
        ram[j].base = m->base;
        ram[j].end = end;
    }
    /* Non-usable RAM entries may overlap. Count their union, not the
     * highest physical address or a sum that counts firmware twice. */
    uint64_t end = 0;
    for (unsigned i = 0; i < n; ++i) {
        uint64_t start = ram[i].base > end ? ram[i].base : end;
        if (ram[i].end > start)
            out->ram_bytes += ram[i].end - start;
        if (ram[i].end > end)
            end = ram[i].end;
    }
    out->half_bytes = IA64VMM_ALIGN_DOWN(out->ram_bytes / 2, IA64VMM_PAGE_SIZE);
    uint64_t size = requested > minimum ? requested : minimum;
    if (size > out->half_bytes)
        size = out->half_bytes;
    if (size > out->largest_bytes)
        size = out->largest_bytes;
    if (!size || size < minimum)
        return BOOT_RESERVE_TOO_SMALL;
    if (!boot_reserve_range(bi, size, &out->base))
        return BOOT_RESERVE_INVALID;
    out->size = size;
    return BOOT_RESERVE_OK;
}
