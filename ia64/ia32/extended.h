/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64_IA32_EXTENDED_H
#define IA64_IA32_EXTENDED_H
#include "ia64/semantics.h"
#include "ia64/ia32/decode.h"

/* ENTER, x87 control/environments, FX state and masked byte stores. Successful
 * execution advances EIP and clears RF. Every write is preflighted before
 * any memory or register effect; false handled leaves the instruction alone. */
struct ia64_result ia32_extended(struct vcpu *, struct ia32_native_state *,
                                 const struct ia32_decoded *, const uint8_t *, bool *handled,
                                 struct ia32_interruption *);
/* Admission only: no operand read/write, EIP change or FP state mutation. */
struct ia64_result ia32_extended_check(struct vcpu *, const struct ia32_native_state *,
                                       const struct ia32_decoded *, struct ia32_interruption *);
#endif
