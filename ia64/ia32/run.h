/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64_IA32_RUN_H
#define IA64_IA32_RUN_H
#include "ia64/semantics.h"
#include "ia64vmm/ia32_state.h"
#include "ia64/ia32/decode.h"
struct ia64_result ia32_enter(struct vcpu *, enum ia32_entry_kind, uint64_t target);
void ia32_deliver(struct vcpu *, const struct ia32_interruption *);
struct vcpu_exit engine_run_ia64(struct vcpu *, uint64_t budget);
struct ia64_result ia32_exception(const struct ia32_native_state *, unsigned vector, unsigned code,
                                  struct ia32_interruption *);
struct ia64_result ia32_operand(struct vcpu *, const struct ia32_native_state *, unsigned segment,
                                uint32_t offset, unsigned size, bool write, uint64_t *value,
                                struct ia32_interruption *);
void ia32_note_operand(struct vcpu *, uint32_t address, unsigned size, bool write);
unsigned ia32_operand_breakpoints(const struct vcpu *);
struct ia64_result ia32_assist(struct vcpu *, struct ia32_native_state *,
                               const struct ia32_decoded *, const uint8_t *, bool *handled,
                               unsigned *loaded, struct ia32_interruption *);
const struct ia32_segment *ia32_segment(const struct ia32_native_state *, unsigned);
struct ia64_result ia32_io(struct vcpu *, struct ia32_native_state *, const struct ia32_decoded *,
                           const uint8_t *, struct ia32_interruption *);
#endif
