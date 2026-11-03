/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The deterministic stepping reference; see reference.h. */
#include <string.h>
#include "tests/tier1/reference.h"
#include "tests/tier1/storelog.h"
#include "engine/interp/interp.h"

struct vcpu_exit ref_run(struct vcpu *v, uint64_t budget, struct schedule *sched)
{
    schedule_apply_due(sched, v);
    while (budget) {
        uint64_t before = v->retired;
        storelog_predict(v);
        struct vcpu_exit e = interp_run(v, 1);
        storelog_step_done(v);
        if (e.reason != VCPU_EXIT_BUDGET)
            return e;
        T1H_CHECK(v->retired == before + 1);
        --budget;
        schedule_apply_due(sched, v);
    }
    return (struct vcpu_exit){VCPU_EXIT_BUDGET, v->s.ip, v->s.slot, 0, NULL};
}

struct vcpu_exit ref_lower(struct vcpu *v, uint64_t budget)
{
    while (budget) {
        uint64_t before = v->retired;
        storelog_predict(v);
        struct vcpu_exit e = interp_run(v, 1);
        storelog_step_done(v);
        if (e.reason != VCPU_EXIT_BUDGET)
            return e;
        /* interp_run's kick exit (engine/interp/interp.c) happens at its loop
         * top, before any state change; nothing else returns BUDGET from a
         * one-instruction step without retiring it. */
        if (v->retired == before && e.detail && !strcmp(e.detail, "interrupt mailbox kick"))
            return e;
        T1H_CHECK(v->retired == before + 1);
        --budget;
    }
    return (struct vcpu_exit){VCPU_EXIT_BUDGET, v->s.ip, v->s.slot, 0, NULL};
}

bool ref_run_to(struct vcpu *v, uint64_t target, struct schedule *sched, struct vcpu_exit *exit)
{
    T1H_CHECK(v->retired <= target);
    *exit = ref_run(v, target - v->retired, sched);
    return exit->reason == VCPU_EXIT_BUDGET && v->retired == target;
}
