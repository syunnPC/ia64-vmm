/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The `tier0+tier1` harness engine; see engine_tier0t1.h. */
#include <string.h>
#include "tests/tier1/engine_tier0t1.h"
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/storelog.h"
#include "tests/tier1/provider.h"
#include "engine/tier0/tier0.h"
#include "engine/tier1/region.h"
#include "engine/tier1/tier1.h"
#include "engine/upper.h"
#include "ia64vmm/hal.h"

static uint32_t flags = T1_COMPILE_VERIFY;
static struct observer *observer;
static uint64_t kick_yields;

void t1h_tier0t1_set_flags(uint32_t f)
{
    flags = f;
}
uint32_t t1h_tier0t1_flags(void)
{
    return flags;
}
void t1h_tier0t1_set_observer(struct observer *o)
{
    observer = o;
    if (!o)
        t1_test_set_restore_observer(NULL, NULL);
}
uint64_t t1h_tier0t1_kick_yields(void)
{
    return kick_yields;
}

/* The Tier1 engine glue's per-vCPU context (window instance, struct
 * t1_vcpu) and process set-up (t1_init, trap client) serve both engines. */
static void *create(struct vcpu *v)
{
    return t1h_engine_tier1()->create(v);
}

static void destroy(void *ctx)
{
    t1h_engine_tier1()->destroy(ctx);
}

bool t1h_kick_yield(const struct vcpu_exit *e)
{
    return e->reason == VCPU_EXIT_BUDGET && e->detail &&
           !strcmp(e->detail, "interrupt mailbox kick");
}

static struct vcpu_exit slice(void *ctx, struct vcpu *v, uint64_t budget, enum engine_kind as)
{
    struct t1_vcpu *tv = t1h_tier1_vcpu(ctx);
    const struct tier0_upper_ops *ops = engine_tier1_ops();
    T1H_CHECK(ops != NULL);
    enum engine_kind kind = v->cfg.engine;
    void *upper = v->upper;
    v->cfg.engine = as;
    v->upper = tv;
    tier0_set_upper(ops);
    struct vcpu_exit out = tier0_run(v, budget);
    tier0_set_upper(NULL);
    v->cfg.engine = kind;
    v->upper = upper;
    if (t1h_kick_yield(&out))
        ++kick_yields;
    return out;
}

struct vcpu_exit t1h_tier0t1_slice(void *ctx, struct vcpu *v, uint64_t budget)
{
    return slice(ctx, v, budget, ENGINE_TIER1);
}

struct vcpu_exit t1h_tier0t1_run(void *ctx, struct vcpu *v, uint64_t budget)
{
    uint64_t start = v->retired;
    struct vcpu_exit out;
    /* The interrupt-kick yield (tier0.c): the executor calls again. */
    do
        out = t1h_tier0t1_slice(ctx, v, budget - (v->retired - start));
    while (t1h_kick_yield(&out) && v->retired - start < budget);
    return out;
}

static struct vcpu_exit run(void *ctx, struct vcpu *v, uint64_t budget)
{
    struct t1_vcpu *tv = t1h_tier1_vcpu(ctx);
    struct t1h_logs *logs = t1h_logs_of(v);
    bool stores = logs && logs->stores_on;
    struct t1_compile_options opt = {flags | (stores ? T1_COMPILE_STORE_LOG : 0), 0, 0};
    t1_test_set_compile_options(tv, &opt);
    t1_test_set_store_observer(stores ? storelog_t1_store_ranged : NULL, stores ? (void *)v : NULL);
    t1_test_set_restore_observer(observer ? observe_t1_restore : NULL, observer);
    return t1h_tier0t1_run(ctx, v, budget);
}

static void restored(void *ctx, struct vcpu *v)
{
    tier0_vcpu_state_restored(v);
    t1_vcpu_state_restored(t1h_tier1_vcpu(ctx));
}

static void kick(void *ctx)
{
    t1_kick(t1h_tier1_vcpu(ctx));
}

/* kick-delivery: a kick leaves a Tier1 region at its next checkpoint, within its
 * max_path <= T1_MAX_INSNS instructions, and a Tier0
 * region within TIER0_MAX_INSNS; Tier0's loop top then yields and the next
 * run delivers. */
static const struct t1h_engine tier0t1 = {
    .name = "tier0+tier1",
    .create = create,
    .destroy = destroy,
    .run = run,
    .restored = restored,
    .kick = kick,
    .kick_bound = T1_MAX_INSNS,
    .exact_budget = true,
};
_Static_assert(T1_MAX_INSNS >= TIER0_MAX_INSNS, "kick bound covers Tier0 regions");

const struct t1h_engine *t1h_engine_tier0t1(void)
{
    return &tier0t1;
}

/* ---- lockstep+tier1 ---- */

struct vcpu_exit t1h_checked_slice(void *ctx, struct vcpu *v, uint64_t budget)
{
    return slice(ctx, v, budget, ENGINE_LOCKSTEP);
}

void t1h_checked_release(struct vcpu *v)
{
    if (!v->check_workspace)
        return;
    hal_free_pages(v->check_workspace, v->check_workspace_pages);
    v->check_workspace = NULL;
    v->check_workspace_pages = 0;
}

struct vcpu_exit t1h_checked_run(void *ctx, struct vcpu *v, uint64_t budget)
{
    uint64_t start = v->retired;
    struct vcpu_exit out;
    do
        out = slice(ctx, v, budget - (v->retired - start), ENGINE_LOCKSTEP);
    while (t1h_kick_yield(&out) && v->retired - start < budget);
    t1h_checked_release(v);
    return out;
}

static struct vcpu_exit checked_run(void *ctx, struct vcpu *v, uint64_t budget)
{
    struct t1_vcpu *tv = t1h_tier1_vcpu(ctx);
    struct t1h_logs *logs = t1h_logs_of(v);
    bool stores = logs && logs->stores_on;
    struct t1_compile_options opt = {
        T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG | (flags & T1_COMPILE_ACCESS_LOG), 0, 0};
    t1_test_set_compile_options(tv, &opt);
    t1_test_set_store_observer(stores ? storelog_t1_store_ranged : NULL, stores ? (void *)v : NULL);
    t1_test_set_restore_observer(NULL, NULL);
    return t1h_checked_run(ctx, v, budget);
}

static const struct t1h_engine checked = {
    .name = "lockstep+tier1",
    .create = create,
    .destroy = destroy,
    .run = checked_run,
    .restored = restored,
    .kick = kick,
    .kick_bound = T1_MAX_INSNS,
    .exact_budget = true,
};

const struct t1h_engine *t1h_engine_checked(void)
{
    return &checked;
}

/* ---- the restoration provider (provider.h) ---- */

#define VERIFY_FLAGS (T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG)

static void *pv_create(struct vcpu *v)
{
    void *ctx = create(v);
    struct t1_compile_options opt = {.flags = VERIFY_FLAGS};
    t1_test_set_compile_options(t1h_tier1_vcpu(ctx), &opt);
    return ctx;
}

static void pv_destroy(void *ctx)
{
    t1_test_set_restore_observer(NULL, NULL);
    t1_test_set_store_observer(NULL, NULL);
    t1_test_set_entry_hook(NULL, NULL);
    destroy(ctx);
}

static void pv_restored(void *ctx)
{
    tier0_vcpu_state_restored(t1h_tier1_vcpu(ctx)->v);
    t1_vcpu_state_restored(t1h_tier1_vcpu(ctx));
}

/* The driver's observers stay as it set them (process-wide in tier1.h). */
static struct vcpu_exit pv_run(void *ctx, uint64_t budget)
{
    struct t1_vcpu *tv = t1h_tier1_vcpu(ctx);
    struct t1_compile_options opt = {.flags = VERIFY_FLAGS};
    t1_test_set_compile_options(tv, &opt);
    return t1h_tier0t1_run(ctx, tv->v, budget);
}

static enum t1_compile_status pv_compile(void *ctx, const struct t1_compile_options *opt,
                                         struct t1_compile_report *report)
{
    return t1_compile(t1h_tier1_vcpu(ctx), opt, report);
}
static unsigned pv_point_count(void *ctx, uint32_t region)
{
    (void)ctx;
    return t1_point_count(region);
}
static bool pv_point_get(void *ctx, uint32_t region, unsigned index, struct t1_point_info *out)
{
    (void)ctx;
    return t1_point_get(region, index, out);
}
static bool pv_force(void *ctx, uint32_t region, uint32_t point, uint64_t occurrence,
                     enum t1_force_action action, t1_before_resolve_fn before, void *arg)
{
    return t1_test_force(t1h_tier1_vcpu(ctx), region, point, occurrence, action, before, arg);
}
static bool pv_corrupt(void *ctx, uint32_t region, uint32_t point, uint64_t occurrence,
                       uint16_t element, uint64_t mask)
{
    return t1_test_corrupt(t1h_tier1_vcpu(ctx), region, point, occurrence, element, mask);
}
static void pv_set_restore_observer(void *ctx, t1_restore_fn fn, void *arg)
{
    (void)ctx;
    t1_test_set_restore_observer(fn, arg);
}
/* The drivers pass storelog_t1_store with their logs: the product path
 * needs the range filter (storelog_t1_store_ranged) with the vCPU. */
static void pv_set_store_observer(void *ctx, t1_store_fn fn, void *arg)
{
    (void)arg;
    t1_test_set_store_observer(fn ? storelog_t1_store_ranged : NULL,
                               fn ? t1h_tier1_vcpu(ctx)->v : NULL);
}
static void pv_set_entry_hook(void *ctx, t1_entry_hook_fn fn, void *arg)
{
    (void)ctx;
    t1_test_set_entry_hook(fn, arg);
}
static unsigned pv_exit_count(void *ctx)
{
    return t1_exit_count(t1h_tier1_vcpu(ctx));
}
static bool pv_exit_get(void *ctx, unsigned index, struct t1_exit_info *out)
{
    return t1_exit_get(t1h_tier1_vcpu(ctx), index, out);
}
static void pv_stats(void *ctx, struct t1_stats *out)
{
    (void)ctx;
    t1_get_stats(out);
}
static struct hostwin_vcpu *pv_hostwin(void *ctx)
{
    return t1h_tier1_hostwin(ctx);
}

static const struct t1h_provider provider = {
    .name = "tier0+tier1",
    .evidence = true,
    .create = pv_create,
    .destroy = pv_destroy,
    .restored = pv_restored,
    .run = pv_run,
    .kick = kick,
    .compile = pv_compile,
    .point_count = pv_point_count,
    .point_get = pv_point_get,
    .force = pv_force,
    .corrupt = pv_corrupt,
    .set_restore_observer = pv_set_restore_observer,
    .set_store_observer = pv_set_store_observer,
    .set_entry_hook = pv_set_entry_hook,
    .exit_count = pv_exit_count,
    .exit_get = pv_exit_get,
    .stats = pv_stats,
    .hostwin = pv_hostwin,
};

const struct t1h_provider *t1h_provider_tier0t1(void)
{
    return &provider;
}
