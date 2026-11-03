/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_INTERRUPT_H
#define IA64VMM_IA64_INTERRUPT_H
#include "ia64/semantics.h"
void ia64_deliver_fault(struct vcpu *v, const Ia64Instruction *insn, struct ia64_result result);
void ia64_timer_program(struct vcpu *v);
void ia64_timer_poll(struct vcpu *v);
/* Prove a poll would change no state at the current HAL counter sample. */
bool ia64_timer_poll_noop(const struct vcpu *v);
bool ia64_external_deliver(struct vcpu *v);
/* Non-destructive LSAPIC delivery query, shared by engine dispatchers. */
bool ia64_external_pending(const struct vcpu *v);
/* False whenever ia64_external_pending is: no PAL-based interruption
 * pending, and PSR.i clear or IRR empty. A cheap test for engine loops
 * before the priority comparison. */
static inline bool ia64_external_requested(const struct vcpu *v)
{
    if (v->pal_init_pending | v->pal_mca_pending | v->pal_pmi_pending)
        return true;
    if (!(v->s.psr & IA64_PSR_I))
        return false;
    uint64_t any = 0;
    for (unsigned i = 0; i < 4; ++i)
        any |= __atomic_load_n(&v->s.cr[CR_IRR0 + i], __ATOMIC_ACQUIRE);
    return any != 0;
}
unsigned ia64_ivr_read(struct vcpu *v);
void ia64_eoi(struct vcpu *v);
struct ia64_result ia64_control(struct vcpu *v, const Ia64Instruction *insn);
#endif
