/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The harness engine `tier0+tier1`: the
 * product path. tier0_run with Tier1 registered as its upper tier through
 * engine/engine.c's own composition (engine_tier1_ops), the vCPU set to
 * ENGINE_TIER1 with v->upper = its struct t1_vcpu, on the Tier1 engine
 * glue's vCPU contexts (engine_tier1.h: hosted window instance with the
 * MMU-log completeness check, trap client, code-page hook, t1_init with the
 * harness thresholds). The hook is registered only for the duration of each
 * run, so the plain `tier0` engine never has it.
 *
 * A run returns when the budget is consumed or at a non-budget exit. Tier0
 * yields early with VCPU_EXIT_BUDGET when it consumes an interrupt kick
 * (tier0.c loop top); the engine then calls tier0_run again, as the
 * product executor does (core/vm.c), so the exact-budget contract holds at
 * the engine interface. Every other short budget return is passed on (and
 * fails the caller's retired-count check).
 */
#ifndef IA64VMM_TESTS_TIER1_ENGINE_TIER0T1_H
#define IA64VMM_TESTS_TIER1_ENGINE_TIER0T1_H
#include "tests/tier1/engines.h"
#include "tests/tier1/observe.h"

const struct t1h_engine *t1h_engine_tier0t1(void);
/* Compile flags of the regions the engine's hot() compiles and links
 * (default T1_COMPILE_VERIFY; T1_COMPILE_STORE_LOG is added while the
 * vCPU's store log is on), as t1h_tier1_set_flags for `tier1`. */
void t1h_tier0t1_set_flags(uint32_t flags);
uint32_t t1h_tier0t1_flags(void);
/* The in-place observer of the engine's restore records (NULL: none). */
void t1h_tier0t1_set_observer(struct observer *o);
/* Kick yields absorbed by the engine's runs since the process started. */
uint64_t t1h_tier0t1_kick_yields(void);
/* One run as the engine's `run` does it, with the hook and the vCPU's
 * engine kind in place, but with no observer change: for drivers that set
 * their own observers (the restoration provider). */
struct vcpu_exit t1h_tier0t1_run(void *ctx, struct vcpu *v, uint64_t budget);
/* One tier0_run with the hook in place, returning also on a kick yield
 * (VCPU_EXIT_BUDGET short of the budget, detail "interrupt mailbox kick"):
 * for tests that model the executor's drain between engine runs. */
struct vcpu_exit t1h_tier0t1_slice(void *ctx, struct vcpu *v, uint64_t budget);
bool t1h_kick_yield(const struct vcpu_exit *e);

/* (checked execution) The engine `lockstep+tier1`: the same
 * product path with the vCPU set to ENGINE_LOCKSTEP and its regions
 * compiled with T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG, as
 * `engine=lockstep tier1=on` ran vmm-hosted: Tier0
 * regions checked by Tier0's lockstep, every Tier1 execution by Tier1's
 * checker (engine/tier1/checked.c). Not in the engine list of the other
 * suites (t1h_engine_at); suite checked (ver_tests.c) runs it. Tier0's
 * lockstep workspace is released after every run (snapshot_restore
 * refuses a vCPU that owns one). */
const struct t1h_engine *t1h_engine_checked(void);
struct vcpu_exit t1h_checked_run(void *ctx, struct vcpu *v, uint64_t budget);
/* One tier0_run of that engine, returning also on a kick yield (as
 * t1h_tier0t1_slice); the caller releases Tier0's lockstep workspace with
 * t1h_checked_release before the next snapshot restore. */
struct vcpu_exit t1h_checked_slice(void *ctx, struct vcpu *v, uint64_t budget);
void t1h_checked_release(struct vcpu *v);

struct t1h_provider;
/* The restoration provider (provider.h) of the product path: the "tier1"
 * provider's verification API with its runs on `tier0+tier1`, so every
 * restoration driver's region is entered from Tier0's lookups. */
const struct t1h_provider *t1h_provider_tier0t1(void);
#endif
