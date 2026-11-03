/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The Tier1 engine of the test harness; see engine_tier1.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/hostwin_linux.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/storelog.h"
#include "engine/tier1/exec.h"
#include "engine/hostwin/hostwin.h"
#include "engine/cache/validity.h"
#include "engine/tier0/tier0.h"
#include "ia64/mmu_log.h"

#define SLICE 16384u

/* Guest RAM must be the backend's memfd so windows map the very pages the
 * interpreter uses; installed before any t1h_init. */
__attribute__((constructor)) static void memfd_ram(void)
{
    t1h_set_ram_allocator(hostwin_linux_ram);
}

struct ctx {
    struct vcpu *v;
    struct hostwin_vcpu *hw;
    struct t1_vcpu *tv;
};

static uint32_t flags = T1_COMPILE_VERIFY;
static struct observer *observer;

void t1h_tier1_set_flags(uint32_t f)
{
    flags = f;
}
void t1h_tier1_set_observer(struct observer *o)
{
    observer = o;
    if (!o)
        t1_test_set_restore_observer(NULL, NULL);
}
struct t1_vcpu *t1h_tier1_vcpu(void *ctx)
{
    return ((struct ctx *)ctx)->tv;
}
struct hostwin_vcpu *t1h_tier1_hostwin(void *ctx)
{
    return ((struct ctx *)ctx)->hw;
}

static void (*lower_observer)(struct vcpu *v, void *arg);
static void *lower_observer_arg;
void t1h_tier1_set_lower_observer(void (*fn)(struct vcpu *v, void *arg), void *arg)
{
    lower_observer = fn;
    lower_observer_arg = arg;
}

/* Tier1's lower tier: the stepping reference (ref_lower), observed. With
 * T1H_LOWER=tier0 (measurements only):
 * tier0_run, called by the dispatcher one instruction at a time as it
 * calls every lower tier; lower-tier stores are then not paired with GVAs,
 * so store-log comparisons do not apply. */
static bool lower_tier0;
static struct vcpu_exit lower(struct vcpu *v, uint64_t budget)
{
    if (lower_observer)
        lower_observer(v, lower_observer_arg);
    return lower_tier0 ? tier0_run(v, budget) : ref_lower(v, budget);
}

/* Test switches of the MMU change log, environment only:
 * T1H_MMU_MUTE=<family> mutes one writer family (ia64/mmu_log.h) for the
 * harness-level mutation runs (D-* and fuzz must then end in the check's
 * panic); T1H_HOSTWIN_CHECK=0 turns the completeness check off for
 * measurements of the product path (tools/bench/t1_perf.py). */
static bool hostwin_check = true;
bool t1h_tier1_set_hostwin_check(bool check)
{
    bool old = hostwin_check;
    hostwin_check = check;
    return old;
}

static void mmu_log_switches(void)
{
    static const char *const families[IA64_MMU_WRITERS] = {
        "tc_insert_purge", "tc_insert", "insert_purge", "itc", "itr", "ptc_l",   "ptc_g", "ptc_ga",
        "ptc_e",           "ptr_tc",    "ptr_tr",       "rr",  "pkr", "pkr_dup", "reset"};
    const char *mute = getenv("T1H_MMU_MUTE"), *check = getenv("T1H_HOSTWIN_CHECK");
    if (mute && *mute) {
        unsigned f = 0;
        while (f < IA64_MMU_WRITERS && (!families[f] || strcmp(families[f], mute)))
            ++f;
        if (f == IA64_MMU_WRITERS) {
            fprintf(stderr, "T1H_MMU_MUTE: unknown writer family %s\n", mute);
            exit(2);
        }
        ia64_mmu_test_mute = UINT32_C(1) << f;
    }
    if (check && !strcmp(check, "0"))
        hostwin_check = false;
}

static void setup(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    mmu_log_switches();
    T1H_CHECK(hostwin_linux_init());
    hostwin_set_trap_client(t1_trap_client);
    code_validity_set_track_hook(hostwin_code_page_event);
    const char *which = getenv("T1H_LOWER"), *chain = getenv("T1H_CHAIN");
    lower_tier0 = which && !strcmp(which, "tier0");
    /* loop/trace thresholds: the `tier0+tier1` engine's hot(), low so that the short test programs exercise it. */
    struct t1_config cfg = {.wallclock_slice = SLICE,
                            .hot_threshold = 2,
                            .lower = lower,
                            .loop_threshold = 2,
                            .trace_threshold = 8,
                            .chain_off = chain && !strcmp(chain, "off")};
    T1H_CHECK(t1_init(&cfg));
}

static void *create(struct vcpu *v)
{
    setup();
    struct ctx *c = calloc(1, sizeof(*c));
    T1H_CHECK(c != NULL);
    struct hostwin_config hc = {40, HOSTWIN_MAX_SLOTS,
                                hostwin_check}; /* with the completeness check */
    c->hw = hostwin_create(&hc, hostwin_linux_backend(), hostwin_linux_ctx());
    if (!c->hw) {
        hc.w = 36;
        c->hw = hostwin_create(&hc, hostwin_linux_backend(), hostwin_linux_ctx());
    }
    T1H_CHECK(c->hw != NULL);
    c->v = v;
    c->tv = t1_vcpu_create(v, c->hw);
    T1H_CHECK(c->tv != NULL);
    return c;
}

static void destroy(void *ctx)
{
    struct ctx *c = ctx;
    t1_vcpu_destroy(c->tv);
    hostwin_destroy(c->hw);
    free(c);
}

static struct vcpu_exit run(void *ctx, struct vcpu *v, uint64_t budget)
{
    struct ctx *c = ctx;
    struct t1h_logs *logs = t1h_logs_of(v);
    bool stores = logs && logs->stores_on;
    struct t1_compile_options opt = {flags | (stores ? T1_COMPILE_STORE_LOG : 0), 0, 0};
    t1_test_set_compile_options(c->tv, &opt);
    t1_test_set_store_observer(stores ? storelog_t1_store_ranged : NULL, stores ? (void *)v : NULL);
    t1_test_set_restore_observer(observer ? observe_t1_restore : NULL, observer);
    return t1_run(c->tv, budget);
}

static void restored(void *ctx, struct vcpu *v)
{
    (void)v;
    t1_vcpu_state_restored(((struct ctx *)ctx)->tv);
}

static void kick(void *ctx)
{
    t1_kick(((struct ctx *)ctx)->tv);
}

static const struct t1h_engine tier1 = {
    .name = "tier1",
    .create = create,
    .destroy = destroy,
    .run = run,
    .restored = restored,
    .kick = kick,
    .kick_bound = SLICE + T1_MAX_INSNS,
    .exact_budget = true,
};

const struct t1h_engine *t1h_engine_tier1(void)
{
    return &tier1;
}
