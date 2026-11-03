/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The "tier1" provider (provider.h): compiler's Tier1 API on the vCPU
 * contexts of compiler's engine glue (tests/tier1/engine_tier1.h: hosted
 * window instance per vCPU, trap client, code-page hook, t1_init). The
 * provider runs t1_run itself, with verification regions and the
 * observers the driver installed, instead of the engine's run, which
 * replaces the observers on every call.
 */
#include "tests/tier1/provider.h"
#include "tests/tier1/engine_tier1.h"

#define VERIFY_FLAGS (T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG)

static void *create(struct vcpu *v)
{
    void *ctx = t1h_engine_tier1()->create(v);
    struct t1_compile_options opt = {.flags = VERIFY_FLAGS};
    t1_test_set_compile_options(t1h_tier1_vcpu(ctx), &opt);
    return ctx;
}

static void destroy(void *ctx)
{
    t1_test_set_restore_observer(NULL, NULL);
    t1_test_set_store_observer(NULL, NULL);
    t1_test_set_entry_hook(NULL, NULL);
    t1h_engine_tier1()->destroy(ctx);
}

static void restored(void *ctx)
{
    t1_vcpu_state_restored(t1h_tier1_vcpu(ctx));
}

static struct vcpu_exit run(void *ctx, uint64_t budget)
{
    struct t1_compile_options opt = {.flags = VERIFY_FLAGS};
    t1_test_set_compile_options(t1h_tier1_vcpu(ctx), &opt);
    return t1_run(t1h_tier1_vcpu(ctx), budget);
}

static void kick(void *ctx)
{
    t1_kick(t1h_tier1_vcpu(ctx));
}

static enum t1_compile_status compile(void *ctx, const struct t1_compile_options *opt,
                                      struct t1_compile_report *report)
{
    return t1_compile(t1h_tier1_vcpu(ctx), opt, report);
}

static unsigned point_count(void *ctx, uint32_t region)
{
    (void)ctx;
    return t1_point_count(region);
}
static bool point_get(void *ctx, uint32_t region, unsigned index, struct t1_point_info *out)
{
    (void)ctx;
    return t1_point_get(region, index, out);
}

static bool force(void *ctx, uint32_t region, uint32_t point, uint64_t occurrence,
                  enum t1_force_action action, t1_before_resolve_fn before, void *arg)
{
    return t1_test_force(t1h_tier1_vcpu(ctx), region, point, occurrence, action, before, arg);
}

static bool corrupt(void *ctx, uint32_t region, uint32_t point, uint64_t occurrence,
                    uint16_t element, uint64_t mask)
{
    return t1_test_corrupt(t1h_tier1_vcpu(ctx), region, point, occurrence, element, mask);
}

/* The verification hooks are process-wide in tier1.h. */
static void set_restore_observer(void *ctx, t1_restore_fn fn, void *arg)
{
    (void)ctx;
    t1_test_set_restore_observer(fn, arg);
}
static void set_store_observer(void *ctx, t1_store_fn fn, void *arg)
{
    (void)ctx;
    t1_test_set_store_observer(fn, arg);
}
static void set_entry_hook(void *ctx, t1_entry_hook_fn fn, void *arg)
{
    (void)ctx;
    t1_test_set_entry_hook(fn, arg);
}

static unsigned exit_count(void *ctx)
{
    return t1_exit_count(t1h_tier1_vcpu(ctx));
}
static bool exit_get(void *ctx, unsigned index, struct t1_exit_info *out)
{
    return t1_exit_get(t1h_tier1_vcpu(ctx), index, out);
}
static void stats(void *ctx, struct t1_stats *out)
{
    (void)ctx;
    t1_get_stats(out);
}
static struct hostwin_vcpu *hostwin(void *ctx)
{
    return t1h_tier1_hostwin(ctx);
}

static const struct t1h_provider real = {
    .name = "tier1",
    .evidence = true,
    .create = create,
    .destroy = destroy,
    .restored = restored,
    .run = run,
    .kick = kick,
    .compile = compile,
    .point_count = point_count,
    .point_get = point_get,
    .force = force,
    .corrupt = corrupt,
    .set_restore_observer = set_restore_observer,
    .set_store_observer = set_store_observer,
    .set_entry_hook = set_entry_hook,
    .exit_count = exit_count,
    .exit_get = exit_get,
    .stats = stats,
    .hostwin = hostwin,
};

const struct t1h_provider *t1h_provider_real(void)
{
    return &real;
}
