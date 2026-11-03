/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_RSE_H
#define IA64VMM_IA64_RSE_H
#include "ia64/semantics.h"
uint64_t ia64_rse_advance(uint64_t address, unsigned count);
struct ia64_result ia64_rse(struct vcpu *v, const Ia64Instruction *insn);
struct ia64_result ia64_rse_call(struct vcpu *v, const Ia64Instruction *insn, uint64_t target);
struct ia64_result ia64_rse_return(struct vcpu *v, const Ia64Instruction *insn, uint64_t target);
struct ia64_result ia64_rse_restore(struct vcpu *v, uint64_t cfm, unsigned preserved);
/* An alloc of a frame of sof registers at v's state either needs no
 * mandatory store (SDM Vol.2 §6.5) or every page its stores touch is
 * translated by a resident TLB entry (or physical mode) with write rights
 * at RSC.pl and is WB RAM, and no data breakpoint or PSR.da/dd applies:
 * ia64_rse then executes that alloc without an interruption or exit.
 * false says nothing about the alloc. Changes no architectural state. */
bool ia64_rse_spill_resident(struct vcpu *v, unsigned sof);
/* Shared pre-fetch mandatory fills, before instruction translation/debug. */
struct ia64_result ia64_rse_resume(struct vcpu *v);
#endif
