/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Apply identical external events to the test run and reference at boundary R:
 * after R instructions retire, before another executes. Comparison at R sees
 * the injection.
 *   RAISE     vcpu_raise_external(v, vector)
 *   DMA       RAM write without code notification or a store-log record
 *   UNMAP     purge D-TC entries for the GVA and clear its present VHPT PTE
 *   STAY_END  end the vCPU's code-validity stay
 *
 * DMA/UNMAP from before-resolve or entry hooks use T1H_AT_PENDING because
 * t1_materialize has not yet updated retired. The next materialized restore
 * record fixes their boundary (observe.c).
 */
#ifndef IA64VMM_TESTS_TIER1_SCHEDULE_H
#define IA64VMM_TESTS_TIER1_SCHEDULE_H
#include "tests/tier1/harness.h"

#define T1H_AT_PENDING UINT64_MAX
#define SCHEDULE_MAX 64u

enum injection_kind { INJ_RAISE, INJ_DMA, INJ_UNMAP, INJ_STAY_END };
struct injection {
    uint64_t at; /* retired count */
    enum injection_kind kind;
    uint64_t address, value; /* DMA: GPA, value; UNMAP: GVA; RAISE: vector */
    unsigned width;          /* DMA: 1, 2, 4 or 8 */
    bool done;
};
struct schedule {
    struct injection e[SCHEDULE_MAX];
    unsigned n;
};

void schedule_clear(struct schedule *s);
void schedule_add(struct schedule *s, struct injection in);
/* Marks every entry not done (to drive another run with the same events). */
void schedule_rearm(struct schedule *s);
/* Applies, in insertion order, every entry not done whose `at` equals
 * v->retired; an entry whose boundary was passed is a test error. Returns
 * the number applied. */
unsigned schedule_apply_due(struct schedule *s, struct vcpu *v);
/* The smallest `at` > v->retired of an entry not done, or UINT64_MAX. */
uint64_t schedule_next(const struct schedule *s, const struct vcpu *v);
/* True if every entry was applied. */
bool schedule_complete(const struct schedule *s);
/* Fixes the boundary of every T1H_AT_PENDING entry (already applied by the
 * run under test) to `retired`. */
void schedule_resolve_pending(struct schedule *s, uint64_t retired);

/* One injection, applied now. */
void inject_apply(struct vcpu *v, const struct injection *in);

/* adapters for Tier1's verification hooks (engine/tier1/tier1.h).
 * Run-side half; the reference replays the recorded entries. */
struct schedule_hook {
    struct schedule *s;
    struct injection in;
};
/* A t1_before_resolve_fn (arg: struct schedule_hook): applies hook->in now
 * and records it with T1H_AT_PENDING, resolved by the next materialized
 * capture (observe_capture), because the hook runs before t1_materialize
 * sets v->retired. */
void schedule_before_resolve(struct vcpu *v, void *arg);
/* Applies `in` now and records it at v->retired (e.g. from Tier1's entry
 * hook, where v->retired is exact: guest-fault-restore). */
void schedule_apply_now(struct schedule *s, struct vcpu *v, struct injection in);
#endif
