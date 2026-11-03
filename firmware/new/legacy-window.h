/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_LEGACY_WINDOW_H
#define VMM_LEGACY_WINDOW_H
#include <ia64vmm/platform.h>
#include <stdbool.h>

/* PI resource HOBs and GCD attributes must cover the same unclaimed pages.
 * RAM and assigned BARs keep their own descriptors, including partial low
 * overlaps. Inputs are page-aligned, validated handoff ranges. */
static inline bool legacy_window_next(const struct ia64vmm_handoff *h, uint64_t *cursor,
                                      uint64_t limit, uint64_t *base, uint64_t *size)
{
    while (*cursor < limit) {
        uint64_t at = *cursor, next = limit, covered = at;
        for (unsigned kind = 0; kind < 2; ++kind) {
            unsigned count = kind ? h->pci_mmio_range_count : h->ram_range_count;
            const struct ia64vmm_ram_range *ranges = kind ? h->pci_mmio : h->ram;
            for (unsigned i = 0; i < count; ++i) {
                uint64_t start = ranges[i].base, length = ranges[i].size;
                if (start > at && start < next)
                    next = start;
                if (start <= at && at - start < length) {
                    uint64_t remaining = length - (at - start);
                    uint64_t end = remaining < limit - at ? at + remaining : limit;
                    if (end > covered)
                        covered = end;
                }
            }
        }
        if (covered > at) {
            *cursor = covered;
            continue;
        }
        *base = at;
        *size = next - at;
        *cursor = next;
        return true;
    }
    return false;
}
#endif
