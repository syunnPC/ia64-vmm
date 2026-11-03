/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Writers of the MMU change log.
 *
 * Every write of a TR or TC entry (either side), of an RR or of a PKR
 * appends one change with ia64_mmu_changed (state.h) right after the write.
 * The call sites go through ia64_mmu_logged, which names the writer family:
 * hosted builds can mute one family (ia64_mmu_test_mute), so the mutation
 * test can show that the completeness check (hostwin_config.check) reports
 * the writes that family leaves unlogged. Bare metal compiles the plain
 * call. A purge logs only a valid -> invalid transition: purging an invalid
 * entry writes nothing (its other fields are left as they were).
 */
#ifndef IA64VMM_IA64_MMU_LOG_H
#define IA64VMM_IA64_MMU_LOG_H
#include "ia64/state.h"

/* One family per log call site; tests/tier1/mmu_log_test.c drives each. */
enum ia64_mmu_writer {
    IA64_MMU_W_TC_INSERT_PURGE, /* ia64_tc_insert (VHPT walker): overlapping TC entries */
    IA64_MMU_W_TC_INSERT,       /* ia64_tc_insert: the inserted entry */
    IA64_MMU_W_INSERT_PURGE,    /* itc/itr: overlapping TC entries */
    IA64_MMU_W_ITC,             /* itc.i, itc.d: the inserted entry */
    IA64_MMU_W_ITR,             /* itr.i, itr.d: the inserted entry */
    IA64_MMU_W_PTC_L,
    IA64_MMU_W_PTC_G,
    IA64_MMU_W_PTC_GA,
    IA64_MMU_W_PTC_E,
    IA64_MMU_W_PTR_TC,       /* ptr.i, ptr.d: TC entries */
    IA64_MMU_W_PTR_TR,       /* ptr.i, ptr.d: TR entries */
    IA64_MMU_W_RR,           /* mov rr[] = */
    IA64_MMU_W_PKR,          /* mov pkr[] = : the written register */
    IA64_MMU_W_PKR_DUP,      /* mov pkr[] = : others with the same key lose v */
    IA64_MMU_W_RESET,        /* vcpu_reset: IA64_MMU_ALL */
    IA64_MMU_W_PTC_G_REMOTE, /* SMP target: range purge and ring overrun */
    IA64_MMU_WRITERS
};
_Static_assert(IA64_MMU_WRITERS <= 32, "ia64_mmu_test_mute is a 32-bit mask");

#ifdef IA64VMM_HOSTED
/* Hosted-only test switch (ia64/mmu_log.c): bit w set drops the log calls
 * of writer family w. Zero except in the mutation tests; no product option
 * sets it. */
extern uint32_t ia64_mmu_test_mute;
#endif

static inline void ia64_mmu_logged(struct vcpu *v, enum ia64_mmu_writer writer,
                                   enum ia64_mmu_change_kind kind, unsigned side, unsigned index)
{
#ifdef IA64VMM_HOSTED
    if (__builtin_expect((ia64_mmu_test_mute >> writer) & 1, 0))
        return;
#else
    (void)writer;
#endif
    ia64_mmu_changed(v, kind, side, index);
}
#endif
