/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_COHERENCE_INLINE_H
#define IA64VMM_COHERENCE_INLINE_H
#include "engine/cache/validity.h"

/* Engine-private fast publication for instruction/region loop tops. The
 * SC epoch load acquires retirement's unlink; a reader that loaded an old
 * epoch blocks reuse until its next boundary. The external API calls
 * this same implementation. No lock or locked RMW belongs on this path. */
extern uint64_t code_retirement_epoch;
static inline void code_quiescent_fast(struct vcpu *v)
{
    uint64_t epoch = __atomic_load_n(&code_retirement_epoch, __ATOMIC_SEQ_CST);
    __atomic_store_n(&v->code_quiescent, epoch, __ATOMIC_RELEASE);
}
#endif
