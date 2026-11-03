/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Providers of the Tier1 verification API for the restoration drivers
 * The drivers call Tier1 only through this table, so
 * the same driver runs against
 *   "tier1"  the Tier1 compiler (provider_real.c) through its engine glue
 *            (engine_tier1.h): the restoration evidence, mandatory;
 *   "double" a plumbing test only: an interpreter-stepping double
 *            (provider_double.c) that
 *            reports SITE points for the loads and stores of a loop and
 *            emits restore records at the armed occurrences with the vCPU
 *            exactly at the instruction boundary: no evidence about
 *            Tier1 at all -- it exercises the drivers, schedules, in-place
 *            captures, store-log merging and the coverage ledger. Its output lines say "(plumbing double)".
 */
#ifndef IA64VMM_TESTS_TIER1_PROVIDER_H
#define IA64VMM_TESTS_TIER1_PROVIDER_H
#include "engine/tier1/tier1.h"
#include "engine/tier1/exec.h"
#include "tests/tier1/harness.h"

struct t1h_provider {
    const char *name;
    bool evidence; /* true: the Tier1 compiler */
    void *(*create)(struct vcpu *v);
    void (*destroy)(void *ctx);
    void (*restored)(void *ctx);
    struct vcpu_exit (*run)(void *ctx, uint64_t budget);
    void (*kick)(void *ctx);
    /* Compiles the region whose header is v's ip/slot (t1_compile). */
    enum t1_compile_status (*compile)(void *ctx, const struct t1_compile_options *opt,
                                      struct t1_compile_report *report);
    unsigned (*point_count)(void *ctx, uint32_t region);
    bool (*point_get)(void *ctx, uint32_t region, unsigned index, struct t1_point_info *out);
    bool (*force)(void *ctx, uint32_t region, uint32_t point, uint64_t occurrence,
                  enum t1_force_action action, t1_before_resolve_fn before, void *arg);
    bool (*corrupt)(void *ctx, uint32_t region, uint32_t point, uint64_t occurrence,
                    uint16_t element, uint64_t mask);
    void (*set_restore_observer)(void *ctx, t1_restore_fn fn, void *arg);
    void (*set_store_observer)(void *ctx, t1_store_fn fn, void *arg);
    void (*set_entry_hook)(void *ctx, t1_entry_hook_fn fn, void *arg);
    unsigned (*exit_count)(void *ctx);
    bool (*exit_get)(void *ctx, unsigned index, struct t1_exit_info *out);
    void (*stats)(void *ctx, struct t1_stats *out);
    /* The vCPU's window instance (NULL for the double). */
    struct hostwin_vcpu *(*hostwin)(void *ctx);
};

/* Tier1 on the engine glue's vCPU contexts (t1_init with the
 * stepping reference as the lower tier, so lower-tier stores are paired).
 * Every run requires verification regions
 * (T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG through
 * t1_test_set_compile_options): regions are keyed by their flags, so the
 * explicitly compiled region is the one entered, and hot compiles of other
 * headers are verification regions too. */
const struct t1h_provider *t1h_provider_real(void);
/* The interpreter-stepping double (always available). */
const struct t1h_provider *t1h_provider_double(void);

/* Steps v (stepping reference, no schedule) to the next arrival at a loop
 * header -- the target of the first backward br.cloop it executes -- and
 * stops there, before the header instruction. False if the program ends
 * first. */
bool t1h_run_to_loop_header(struct vcpu *v, uint64_t limit, uint64_t *header_ip);
#endif
