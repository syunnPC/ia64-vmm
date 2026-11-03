/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_DEBUG_H
#define IA64VMM_IA64_DEBUG_H
#include "ia64/mmu.h"
/* Match after address/permission qualification and before alignment/effects. */
bool ia64_debug_match(const struct ia64_state *s, uint64_t address, unsigned size, unsigned rights,
                      unsigned level);
struct ia64_result ia64_debug_data(struct vcpu *v, uint64_t address, unsigned size, unsigned rights,
                                   unsigned level, bool speculative, bool rse);
struct ia64_result ia64_debug_instruction(struct vcpu *v);
/* Privilege levels at which some pair can match (for engines that
 * skip the per-reference checks when none can): bit pl for an IBR pair with
 * x and plm{pl}, bit 4 + pl for a DBR pair with r or w and plm{pl}. */
#define IA64_DEBUG_ARMED_DATA_SHIFT 4
unsigned ia64_debug_armed(const struct ia64_state *s);
struct ia64_result ia64_completion_trap(struct vcpu *v, const Ia64Instruction *insn,
                                        uint64_t old_psr, bool branch, struct ia64_result result);
#endif
