/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The deterministic reference: interp_run stepped one
 * instruction at a time, so a run can stop at any exact retired count and
 * the store log can pair every lower-tier store with its GVA (storelog.h).
 *
 * interp_run(v, 1) retires exactly one instruction per BUDGET return; it may
 * first deliver an external interrupt or a fault (no retirement) and then
 * retire the handler's first instruction (A.8). So stepping never skips a
 * boundary, and ref_run(v, k) equals interp_run(v, k) in state and exit.
 */
#ifndef IA64VMM_TESTS_TIER1_REFERENCE_H
#define IA64VMM_TESTS_TIER1_REFERENCE_H
#include "tests/tier1/schedule.h"

/* Steps up to `budget` instructions, applying due injections at every
 * boundary (sched may be NULL). Returns the first non-BUDGET exit, else
 * {VCPU_EXIT_BUDGET, ip, slot, 0, NULL} as interp_run does. */
struct vcpu_exit ref_run(struct vcpu *v, uint64_t budget, struct schedule *sched);
/* ref_run without a schedule, with the signature of t1_config.lower: the
 * lower tier of Tier1 runs at so its stores are paired too. One
 * difference: as the lower tier of the v1 engine (t1_run, the outer loop),
 * a step may consume the vCPU's kick word instead of retiring (t1_kick sets irq_kick; interp_run takes it when cfg.engine is
 * ENGINE_INTERP); that step returns VCPU_EXIT_BUDGET with nothing retired
 * and nothing changed, and t1_run accounts by the retired delta. */
struct vcpu_exit ref_lower(struct vcpu *v, uint64_t budget);
/* Steps until v->retired == target. False (with *exit set) if an exit other
 * than BUDGET intervenes first. */
bool ref_run_to(struct vcpu *v, uint64_t target, struct schedule *sched, struct vcpu_exit *exit);
#endif
