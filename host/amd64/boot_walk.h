/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_BOOT_WALK_H
#define VMM_AMD64_BOOT_WALK_H
#include "ia64vmm/types.h"

enum amd64_boot_walk_status {
    AMD64_BOOT_UNTRIED,
    AMD64_BOOT_PRESENT,
    AMD64_BOOT_ABSENT,
    AMD64_BOOT_UNREADABLE,
    AMD64_BOOT_INVALID,
};
struct amd64_boot_walk {
    enum amd64_boot_walk_status status;
    unsigned count;
    uint64_t slot[4], entry[4], physical, page_bytes;
    bool writable;
};
typedef bool (*amd64_boot_read)(void *, uint64_t, uint64_t *);
void amd64_boot_walk(uint64_t cr3, uint64_t hhdm, uint64_t va, amd64_boot_read read, void *ctx,
                     struct amd64_boot_walk *out);
struct amd64_boot_probe {
    bool armed;
    uint64_t rip, address;
};
bool amd64_boot_probe_matches(const struct amd64_boot_probe *, bool early_bsp, uint64_t vector,
                              uint64_t error, uint64_t rip, uint64_t cr2);
#endif
