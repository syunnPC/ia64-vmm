/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Engines under test; see engines.h. */
#include <string.h>
#include "tests/tier1/engines.h"
#include "engine/interp/interp.h"
#include "engine/tier0/tier0.h"

static struct vcpu_exit interp_engine(void *ctx, struct vcpu *v, uint64_t budget)
{
    (void)ctx;
    return interp_run(v, budget);
}

static struct vcpu_exit tier0_engine(void *ctx, struct vcpu *v, uint64_t budget)
{
    (void)ctx;
    return tier0_run(v, budget);
}

static void tier0_restored(void *ctx, struct vcpu *v)
{
    (void)ctx;
    tier0_vcpu_state_restored(v);
}

static const struct t1h_engine interp = {
    .name = "interp",
    .run = interp_engine,
    .kick_bound = 1,
    .exact_budget = true,
};
/* Tier0 polls interrupts before each region of at most TIER0_MAX_INSNS. */
static const struct t1h_engine tier0 = {
    .name = "tier0",
    .run = tier0_engine,
    .restored = tier0_restored,
    .kick_bound = TIER0_MAX_INSNS,
    .exact_budget = true,
};

/* The product path, tests/tier1/engine_tier0t1.c. */
const struct t1h_engine *t1h_engine_tier0t1(void) __attribute__((weak));

unsigned t1h_engine_count(void)
{
    if (!t1h_engine_tier1 || !t1h_engine_tier1())
        return 2;
    return t1h_engine_tier0t1 && t1h_engine_tier0t1() ? 4 : 3;
}

const struct t1h_engine *t1h_engine_at(unsigned index)
{
    switch (index) {
    case 0:
        return &interp;
    case 1:
        return &tier0;
    case 2:
        return t1h_engine_tier1 ? t1h_engine_tier1() : NULL;
    case 3:
        return t1h_engine_tier0t1 ? t1h_engine_tier0t1() : NULL;
    default:
        return NULL;
    }
}

const struct t1h_engine *t1h_engine_named(const char *name)
{
    for (unsigned i = 0; i < t1h_engine_count(); ++i)
        if (!strcmp(t1h_engine_at(i)->name, name))
            return t1h_engine_at(i);
    return NULL;
}

void t1h_engine_bind(struct t1h_engine_vcpu *ev, const struct t1h_engine *e, struct vcpu *v)
{
    ev->e = e;
    ev->v = v;
    ev->ctx = e->create ? e->create(v) : NULL;
    T1H_CHECK(!e->create || ev->ctx);
}

void t1h_engine_release(struct t1h_engine_vcpu *ev)
{
    if (ev->e && ev->e->destroy)
        ev->e->destroy(ev->ctx);
    *ev = (struct t1h_engine_vcpu){0};
}

struct vcpu_exit t1h_engine_run(struct t1h_engine_vcpu *ev, uint64_t budget)
{
    return ev->e->run(ev->ctx, ev->v, budget);
}

void t1h_engine_restored(struct t1h_engine_vcpu *ev)
{
    if (ev->e->restored)
        ev->e->restored(ev->ctx, ev->v);
}

struct vcpu_exit t1h_engine_run_sched(struct t1h_engine_vcpu *ev, uint64_t budget,
                                      struct schedule *sched)
{
    struct vcpu *v = ev->v;
    schedule_apply_due(sched, v);
    for (;;) {
        uint64_t next = schedule_next(sched, v);
        uint64_t chunk =
            next == UINT64_MAX || next - v->retired > budget ? budget : next - v->retired;
        uint64_t before = v->retired;
        struct vcpu_exit e = t1h_engine_run(ev, chunk);
        T1H_CHECK(v->retired - before <= chunk);
        budget -= v->retired - before;
        if (e.reason != VCPU_EXIT_BUDGET)
            return e;
        schedule_apply_due(sched, v);
        /* A BUDGET return short of the chunk breaks the exact-budget
         * contract; the caller's retired-count check reports it. */
        if (!budget || v->retired - before != chunk)
            return e;
    }
}
