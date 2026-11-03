/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_CODE_CACHE_H
#define IA64VMM_CODE_CACHE_H
#include "ia64/memory.h"
#include "engine/cache/validity.h"

struct ia64_bundle {
    uint64_t low, high;
    uint8_t template_code;
    bool long_instruction;
    Ia64Instruction insn[3];
};
struct code_cache_stats {
    uint64_t hits, decodes, invalidated_pages;
};
/* Owner-only bundle storage, remote validity changes under code_lock.
 * Destroy only after the owning executor has stopped. */
bool code_cache_vcpu_init(struct vcpu *v);
/* Worst-case remaining decoded pages and lockstep storage of all owners. */
uint64_t code_cache_reserve_bytes(void);
void code_cache_vcpu_destroy(struct vcpu *v);
struct ia64_result code_get_bundle(struct vcpu *v, gpa_t gpa, const struct ia64_bundle **bundle);
void ia64_decode_bundle(uint64_t low, uint64_t high, gpa_t gpa, struct ia64_bundle *bundle);
void code_cache_get_stats(struct code_cache_stats *stats);
void code_cache_flush(void);
/* Tier0 WB stores must use the notifying helper for any tracked code page. */
bool code_page_has_code(gpa_t page);
#endif
