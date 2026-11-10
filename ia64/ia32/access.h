/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64_IA32_ACCESS_H
#define IA64_IA32_ACCESS_H
#include "ia64/ia32/run.h"
/* Prepare has no device or RAM effects. Alignment is the conditional #AC
 * boundary; use one when an instruction has its own alignment rule. */
struct ia64_result ia32_operand_prepare(struct vcpu *, const struct ia32_native_state *,
                                        unsigned segment, uint32_t offset, unsigned size,
                                        bool write, unsigned alignment, struct ia32_interruption *);
/* Revalidate the complete range before its first access. Call prepare first
 * for instruction-specific alignment and for every operand of a multi-access
 * instruction before committing writes. Maximum size is 512 bytes. */
struct ia64_result ia32_operand_bytes(struct vcpu *, const struct ia32_native_state *,
                                      unsigned segment, uint32_t offset, unsigned size, bool write,
                                      uint8_t *, struct ia32_interruption *);
#endif
