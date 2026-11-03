/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/boot_walk.h"

#define ADDRESS UINT64_C(0x000ffffffffff000)
static bool canonical(uint64_t va)
{
    return va >> 48 == ((va & (UINT64_C(1) << 47)) ? 0xffffu : 0u);
}

/* Intel SDM Vol.3A, 4-level paging. Read the active root without creating
 * mappings; a failed table read is distinct from a non-present entry. */
void amd64_boot_walk(uint64_t cr3, uint64_t hhdm, uint64_t va, amd64_boot_read read, void *ctx,
                     struct amd64_boot_walk *out)
{
    *out = (struct amd64_boot_walk){.status = AMD64_BOOT_INVALID, .writable = true};
    if (!canonical(va))
        return;
    uint64_t table = cr3 & ADDRESS;
    for (unsigned level = 4; level; --level) {
        unsigned at = 4 - level;
        uint64_t offset = ((va >> (12 + 9 * (level - 1))) & 511) * 8;
        if (table > UINT64_MAX - hhdm || offset > UINT64_MAX - hhdm - table)
            return;
        uint64_t slot = hhdm + table + offset;
        if (!canonical(slot) || (slot & 7))
            return;
        out->slot[at] = slot;
        out->count = at + 1;
        if (!read(ctx, slot, &out->entry[at])) {
            out->status = AMD64_BOOT_UNREADABLE;
            return;
        }
        uint64_t entry = out->entry[at];
        if (!(entry & 1)) {
            out->status = AMD64_BOOT_ABSENT;
            return;
        }
        out->writable &= (entry & 2) != 0;
        if (level == 4 && (entry & 128))
            return;
        if (level == 1 || (entry & 128)) {
            uint64_t mask = (UINT64_C(1) << (12 + 9 * (level - 1))) - 1;
            /* Large leaves use bit 12 for PAT, not an address bit. */
            if (level > 1 && (entry & ADDRESS & mask & ~UINT64_C(4096)))
                return;
            out->physical = (entry & ADDRESS & ~mask) | (va & mask);
            out->page_bytes = mask + 1;
            out->status = AMD64_BOOT_PRESENT;
            return;
        }
        table = entry & ADDRESS;
    }
}
bool amd64_boot_probe_matches(const struct amd64_boot_probe *p, bool early_bsp, uint64_t vector,
                              uint64_t error, uint64_t rip, uint64_t cr2)
{
    /* Only a supervisor data-read #PF at this exact aligned load belongs
     * to the probe. Write, user, reserved-bit, fetch and other faults stay fatal. */
    return p->armed && early_bsp && vector == 14 && !(error & ~UINT64_C(1)) && rip == p->rip &&
           cr2 == p->address;
}
