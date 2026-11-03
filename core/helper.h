/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_CORE_HELPER_H
#define IA64VMM_CORE_HELPER_H
#include "ia64vmm/types.h"
struct t1_compile_request;
enum helper_kind { HELPER_T1_COMPILE, HELPER_RECLAIM, HELPER_REVALIDATE, HELPER_STATS };
bool helper_init(unsigned helpers);
/* Optional acceleration; false keeps synchronous compilation. */
bool helper_start(bool requested, bool icount, bool tier1);
void helper_shutdown(void);
/* Submission copies the request; contention, capacity or an in-flight key
 * declines immediately, leaving the executor's hotness backoff intact. */
bool helper_submit_t1(const struct t1_compile_request *rq, void *arg);
bool helper_post(enum helper_kind kind, uint64_t arg);
bool helper_step(void);
void helper_quiesce(void);
void helper_resume(void);
bool helper_active(void);
#ifndef IA64VMM_TIER1_STATS
#define IA64VMM_TIER1_STATS 0
#endif
/* Steps and their longest duration, completed and cancelled compiles;
 * Tier1 submissions accepted and declined, the queue depth each accepted
 * one found (summed), the step time of compile and of maintenance steps,
 * and the time accepted requests waited in the queue (with
 * IA64VMM_TIER1_STATS only, 0 otherwise). */
struct helper_stats {
    uint64_t steps, max_step_ticks, completed, cancelled;
    uint64_t submitted, refused, queued, compile_ticks, maintenance_ticks, wait_ticks;
};
/* Exact for stopped/quiesced helpers; a relaxed snapshot otherwise. */
void helper_get_stats(struct helper_stats *out);
#endif
