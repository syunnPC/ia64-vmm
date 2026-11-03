/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_TLB_H
#define IA64VMM_IA64_TLB_H
#include "ia64/semantics.h"
struct ia64_result ia64_tlb(struct vcpu *v, const Ia64Instruction *insn);
uint64_t ia64_thash(const struct ia64_state *s, uint64_t address);
uint64_t ia64_ttag(const struct ia64_state *s, uint64_t address);
const struct ia64_tlb_entry *ia64_tlb_find(const struct ia64_state *s, uint64_t address,
                                           unsigned side);
/* Derived indexed lookup, with the same first-TR/first-TC ordering as
 * the state-only associative reference. Replays the per-vCPU MMU log. */
const struct ia64_tlb_entry *ia64_tlb_lookup(struct vcpu *v, uint64_t address, unsigned side);
/* Inserts into the TC of side (purging overlapping entries) and logs every
 * change in v's MMU log. */
void ia64_tc_insert(struct vcpu *v, uint64_t address, unsigned side, uint64_t pte, uint64_t itir);
/* Walker fills check same-stream TR overlaps before inserting. False means
 * a required machine check has no PALE_CHECK and the caller must stop. */
bool ia64_vhpt_insert(struct vcpu *v, uint64_t address, unsigned side, uint64_t pte, uint64_t itir);
unsigned ia64_tlb_permissions(uint64_t pte, unsigned level);
#endif
