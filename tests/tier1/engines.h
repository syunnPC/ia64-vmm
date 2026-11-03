/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Engine adapters for dispatch and program tests.
 * Interpreter and Tier0 are always present; Tier1 registers through the
 * weak t1h_engine_tier1() accessor. Every suite iterates t1h_engine_at()
 * and compares each adapter with the stepping reference (reference.h).
 */
#ifndef IA64VMM_TESTS_TIER1_ENGINES_H
#define IA64VMM_TESTS_TIER1_ENGINES_H
#include "tests/tier1/schedule.h"

struct t1h_engine {
    const char *name;
    /* Per-vCPU context (Tier1: t1_vcpu_create with the vCPU's hostwin
     * instance); NULL functions mean none is needed. */
    void *(*create)(struct vcpu *v);
    void (*destroy)(void *ctx);
    struct vcpu_exit (*run)(void *ctx, struct vcpu *v, uint64_t budget);
    /* After every snapshot restore of v (Tier1: t1_vcpu_state_restored). */
    void (*restored)(void *ctx, struct vcpu *v);
    /* Asynchronous kick from another thread after vcpu_raise_external
     * (Tier1: t1_kick); NULL for engines that poll at every boundary
     * (interpreter) or region entry (Tier0). */
    void (*kick)(void *ctx);
    /* kick-delivery: retired instructions a raised interrupt may wait before
     * delivery (Tier1: wallclock_slice + max_path). */
    uint64_t kick_bound;
    /* The engine budget contract is exact (t1_run); false
     * would make budget-sweep a finding, not a pass. */
    bool exact_budget;
};

/* Interpreter, Tier0, then Tier1 when linked. */
unsigned t1h_engine_count(void);
const struct t1h_engine *t1h_engine_at(unsigned index);
const struct t1h_engine *t1h_engine_named(const char *name);
const struct t1h_engine *t1h_engine_tier1(void) __attribute__((weak));

/* An engine bound to one vCPU. */
struct t1h_engine_vcpu {
    const struct t1h_engine *e;
    void *ctx;
    struct vcpu *v;
};
void t1h_engine_bind(struct t1h_engine_vcpu *ev, const struct t1h_engine *e, struct vcpu *v);
void t1h_engine_release(struct t1h_engine_vcpu *ev);
struct vcpu_exit t1h_engine_run(struct t1h_engine_vcpu *ev, uint64_t budget);
void t1h_engine_restored(struct t1h_engine_vcpu *ev);
/* Runs `budget` instructions split at the schedule's boundaries, applying
 * each injection between two engine calls (the test-side half of "the same
 * injection schedule"). */
struct vcpu_exit t1h_engine_run_sched(struct t1h_engine_vcpu *ev, uint64_t budget,
                                      struct schedule *sched);
#endif
