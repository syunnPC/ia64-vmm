/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA32_MEMORY_H
#define IA64VMM_IA32_MEMORY_H
#include "ia64/memory.h"
#include "ia64vmm/ia32.h"

/* Vol.2 10.6.3: two instruction and eight data translations may coexist. */
#define IA32_MEMORY_PAGES 10u
struct ia32_memory_page {
    struct ia32_mapping mapping;
    gpa_t gpa;
    struct ia64_tlb_entry source;
    unsigned side, index;
    bool tr, indirect;
};
struct ia32_authorization {
    struct ia32_memory_page page[IA32_MEMORY_PAGES];
    unsigned count;
    struct ia32_access access[10];
    unsigned ranges;
    uint64_t mmu_seq, psr, rr, pta;
};
/* The access must be classified from original instruction bytes, including
 * first linear byte, full width, and RMW rights. No guest physical access is
 * issued. Failure publishes no pages; a translation fault retains its MMU
 * record and names the first failing byte in access.linear. Token != GPA.
 * A native caller must establish pre-effect/partial-effect ordering before
 * authorizing any data reference; raw NPF metadata is insufficient. */
struct ia64_result ia32_memory_authorize(struct vcpu *, struct ia32_access *,
                                         struct ia32_authorization *);
/* Ranges are in architecturally required fault order. This checks all
 * references before publishing any leaf, retaining the complete two-fetch /
 * eight-data working set across walker fills. */
struct ia64_result ia32_memory_authorize_ranges(struct vcpu *, struct ia32_access *,
                                                unsigned ranges, struct ia32_authorization *);
/* No walks, faults, physical accesses or raw source pointers. */
bool ia32_memory_current(const struct vcpu *, const struct ia32_authorization *);
/* Fetch exactly bytes requested by an incremental instruction decoder.
 * Stops at the first untranslated/unowned byte; never reads a device. */
struct ia64_result ia32_memory_fetch(struct vcpu *, ia32_linear_t, uint8_t *out, unsigned bytes);
#endif
