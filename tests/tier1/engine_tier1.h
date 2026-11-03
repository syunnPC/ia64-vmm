/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Tier1 engine of the test harness (engines.h):
 * t1_run behind the t1h_engine interface, with C's reference as the lower
 * tier (ref_lower: lower-tier stores are paired with their GVA), the
 * hosted window backend, the trap client, the code-page tracking hook, and
 * the harness observers (store log always; the in-place restore observer
 * when a test installs one).
 */
#ifndef IA64VMM_TESTS_TIER1_ENGINE_TIER1_H
#define IA64VMM_TESTS_TIER1_ENGINE_TIER1_H
#include "tests/tier1/engines.h"
#include "tests/tier1/observe.h"

/* Compile flags of the regions the engine compiles (by hotness) and
 * enters. Default T1_COMPILE_VERIFY; T1_COMPILE_STORE_LOG is added while
 * the vCPU's store log is on, so native stores are logged too. */
void t1h_tier1_set_flags(uint32_t flags);
/* Configure future window instances; returns the previous setting. */
bool t1h_tier1_set_hostwin_check(bool check);
/* The observer observe_t1_restore feeds (NULL: none, the default). */
void t1h_tier1_set_observer(struct observer *o);
/* The Tier1 vCPU and window instance behind an engine context. */
struct t1_vcpu *t1h_tier1_vcpu(void *ctx);
struct hostwin_vcpu *t1h_tier1_hostwin(void *ctx);
/* Called before every lower-tier step of every Tier1 vCPU (the
 * vCPU at the instruction about to be stepped); NULL: none, the default.
 * Diagnostics (lower-tier opcode statistics, tests/bench/jobdump.c). */
void t1h_tier1_set_lower_observer(void (*fn)(struct vcpu *v, void *arg), void *arg);
#endif
