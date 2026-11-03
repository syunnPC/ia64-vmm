/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_TIER0_H
#define IA64VMM_TIER0_H
#include "engine/cache/cache.h"
#ifndef IA64VMM_TIER0_STATS
#define IA64VMM_TIER0_STATS 0
#endif

#define TIER0_ABI_VERSION 2u
#define TIER0_MAX_INSNS 48u
#define TIER0_CODE_DEFAULT_BYTES (UINT64_C(32) * 1024 * 1024)
#define TIER0_CODE_MAX_DEFAULT_BYTES (UINT64_C(1024) * 1024 * 1024)
enum tier0_interpreter_reason {
    TIER0_INTERP_UNSUPPORTED_STATE,
    TIER0_INTERP_DEBUG,
    TIER0_INTERP_SINGLE_INSTRUCTION,
    TIER0_INTERP_OPAQUE_HELPER,
    TIER0_INTERP_CACHE_FAILURE,
    TIER0_INTERP_GLOBAL_PURGE,
    TIER0_INTERP_REASON_COUNT
};
struct tier0_stats {
    _Atomic(uint64_t) translations, hits, invalidations, reclaimed, bytes, checked_regions;
    _Atomic(uint64_t) allocated_bytes, lookup_probes;
    /* Cache/arena totals are available in every build. */
    _Atomic(uint64_t) publications, evictions, arena_bytes, arena_high_water;
    _Atomic(uint64_t) protection_changes, lookups, cross_page_validations;
    /* Boundary cache: boundaries that used an entry, found
     * the entry of their IP and slot empty or keyed otherwise, or found it
     * stale (region retired, instruction-side MMU sequence changed, or entry
     * validation failed). */
    _Atomic(uint64_t) boundary_hits, boundary_misses, boundary_stale;
    /* Boundaries whose set held no entry of the vCPU's IP and slot. */
    _Atomic(uint64_t) boundary_conflicts;
    _Atomic(uint64_t) inlined[IA64_OP_COUNT], helpers[IA64_OP_COUNT], unchecked_instructions;
    /* Retirement totals include cold semantic entries in every build. */
    _Atomic(uint64_t) executed_instructions, virtual_instructions, handler_instructions;
    _Atomic(uint64_t) interpreter_instructions[TIER0_INTERP_REASON_COUNT];
    _Atomic(uint64_t) fetch_checks, fetch_faults, interrupts, checked_boundaries;
    /* Counter ticks of the translation path
     * of lookup misses, from region allocation to its return; measured
     * only with IA64VMM_TIER0_STATS (0 otherwise). */
    _Atomic(uint64_t) translate_ticks;
};
/* Background reclamation: stopped setup/teardown only; NULL is synchronous. */
void tier0_set_reclaim_submit(void (*submit)(void));
void tier0_reclaim_batch(unsigned limit);
struct vcpu_exit tier0_run(struct vcpu *v, uint64_t budget);

/*
 * Upper-tier hooks run at Tier0 lookup hits after fetch/event checks and
 * budget calculation. Register only with stopped executors; NULL disables.
 * Tier0 owns the outer loop and caches opaque (id, generation) links as hints.
 * enter must check liveness and contract fields omitted by Tier0's key
 * (including full CFM, runtime PSR and any frame-base requirement), re-probing
 * on mismatch. probe rejects tracing and incompatible lockstep regions.
 *
 * Per-region entries count unlinked hits; promotion progress and pending
 * holds survive eviction from the executor's bounded link cache. Re-probe when the head counter changes or a
 * pending hold ages by TIER0_UPPER_HOLD_RELEASE sequence changes. Without a
 * link/hold, call hot at threshold * 2^k (zero threshold disables); select the
 * loop threshold after a taken backward branch. PENDING holds the request,
 * REJECTED keeps the schedule, and DEFERRED restarts it. Re-check Tier0 region
 * validity after hot or DECLINED: compilation may retire its source. Re-probe
 * after hot; reset entries whenever a previously nonzero link becomes zero,
 * so promotion backoff starts at link loss rather than region creation.
 *
 * RAN resumes at the loop top with exact retired state. RAN_PENDING leaves an
 * unexecuted instruction at v's IP/slot: a one-shot skip runs it in Tier0 before
 * consulting the upper tier again. STOP returns *out. reclaim runs only outside
 * region code at the loop top while reclaim_due is nonzero.
 */
enum tier0_upper_status {
    TIER0_UPPER_DECLINED,    /* guards failed: run the Tier0 region */
    TIER0_UPPER_RAN,         /* ran to an after-completion point */
    TIER0_UPPER_RAN_PENDING, /* ran to a before-effect point: the instruction
                                 at v's ip/slot runs in Tier0 first */
    TIER0_UPPER_STOP,        /* *out is the vcpu_exit to return */
};
enum tier0_upper_hot {
    TIER0_HOT_LINKED,   /* published synchronously: probe and enter now */
    TIER0_HOT_PENDING,  /* queued: keep running Tier0, upper_hold set */
    TIER0_HOT_REJECTED, /* neither: continue the doubling schedule */
    TIER0_HOT_DEFERRED, /* no upper-tier memory now: restart the schedule */
};
struct tier0_upper_ops {
    /* Link for v's state at entry PA entry_pa; 0 = none. Cheap: one hash
     * lookup and contract compares, no compilation. */
    uint64_t (*probe)(struct vcpu *v, gpa_t entry_pa);
    /* Run the linked region if its guards hold.
     * *link may be replaced (re-probe) or cleared. region_budget >= 1. */
    enum tier0_upper_status (*enter)(struct vcpu *v, uint64_t *link, uint64_t region_budget,
                                     struct vcpu_exit *out);
    /* The head at v's ip/slot (entry PA entry_pa) became hot. Compiles
     * synchronously or queues a compile request (tier1.h). */
    enum tier0_upper_hot (*hot)(struct vcpu *v, gpa_t entry_pa, bool loop_head);
    /* Frees retired upper-tier code; called only outside region code. */
    void (*reclaim)(void);
    /* Changes whenever a new link may exist (a publication) or a queued
     * request finished; never changes otherwise. Read with an acquire load. */
    const uint64_t *sequence;
    uint32_t loop_threshold, trace_threshold; /* defaults 64, 1024 */
    /*
     * Per-head counters change when links may appear or queued requests finish;
     * acquire loads. A stub may use one counter with mask 0.
     */
    const uint64_t *heads;
    uint64_t head_mask;
    /* Nonzero while reclaim() has work. */
    const unsigned *reclaim_due;
    /* PSR bits of states the upper tier never runs. */
    uint64_t lower_psr;
};
/*
 * Queued-request hold: low sequence bits with bit 0 set (never zero).
 * Release after TIER0_UPPER_HOLD_RELEASE changes past hold & ~1.
 */
#define TIER0_UPPER_HOLD(sequence) ((uint32_t)(sequence) | 1u)
#define TIER0_UPPER_HOLD_RELEASE 64u
/* Stopped executors only; NULL disables. Implemented by product dispatch. */
void tier0_set_upper(const struct tier0_upper_ops *ops);
/* The vCPU's state was replaced (snapshot restore) or the vCPU is attached:
 * its boundary-cache and data translation cache entries are forgotten,
 * since a restored MMU sequence can repeat with other TLB contents.
 * Called wherever t1_vcpu_state_restored is, and by
 * engine attach. */
void tier0_vcpu_state_restored(const struct vcpu *v);
/* Executor-local state of v's cfg.index (homes, boundary and data
 * translation caches), from the VMM reserve; false if unavailable. Called by
 * engine attach; Tier0 allocates it at first use otherwise. */
bool tier0_executor_attach(const struct vcpu *v);
#ifdef IA64VMM_HOSTED
/* Hosted builds compare every boundary-cache hit with the full path
 * (default); paired measurements use the cache unchecked or not at all. */
enum tier0_boundary_mode { TIER0_BOUNDARY_CHECKED, TIER0_BOUNDARY_ON, TIER0_BOUNDARY_OFF };
void tier0_test_set_boundary_mode(enum tier0_boundary_mode mode);
#endif

/* Code validity freed its tracked-page bitmaps: forget the executors'
 * cached spans (stopped executors). */
void tier0_code_spans_reset(void);
void tier0_invalidate(gpa_t gpa, uint64_t len);
void tier0_flush(void);
bool tier0_page_has_code(gpa_t page);
/* Shadow writes are private; they must not invalidate the real cache. */
bool tier0_shadow_active(void);
/* Maximum lazy lockstep workspace, rounded to allocator pages. */
uint64_t tier0_check_reserve_bytes(void);
/* Recognise a replay by its own check context, independently of peers. */
bool tier0_is_shadow(const struct vcpu *v);
/* Capture only instruction sources when SMP data accesses use value replay. */
bool tier0_check_code(struct vcpu *v, gpa_t gpa);
struct ia64_bundle *tier0_shadow_bundle(struct vcpu *v);
/* SMP interpreter boundaries replay the exact decoded source selected by
 * the real fetch, even if a peer wrote after its page was captured. */
bool tier0_check_bundle(struct vcpu *v, gpa_t gpa, const struct ia64_bundle *bundle);
#if defined(IA64VMM_TIER0_TEST_HOOKS) && IA64VMM_TIER0_TEST_HOOKS
/* Foundation-only scheduling points; absent from product/bench objects. */
enum tier0_test_stage {
    TIER0_TEST_DEBUG_BEFORE,
    TIER0_TEST_DEBUG_READY,
    TIER0_TEST_HOMES_BEFORE,
    TIER0_TEST_HOMES_READY,
    TIER0_TEST_BOUNDARY_HIT
}; /* a boundary entry matched, before enter() */
typedef void (*tier0_test_hook_fn)(const struct vcpu *, enum tier0_test_stage);
void tier0_set_test_hook(tier0_test_hook_fn hook);
#endif
void tier0_get_stats(struct tier0_stats *out);
void tier0_dump_stats(void);
/* A region entry began a stay or added a page to it; slot: the caller's
 * code_counter_index(). */
void tier0_count_page_entry(unsigned slot);
bool tier0_arena_initialized(void);
/* Stopped executor only. Command line accepts a positive integer MiB. */
bool tier0_configure(uint64_t code_bytes);
/* Code bytes without tier0_code_mib=: a quarter of the VMM reserve (an
 * eighth when Tier1, with its own arena and host-window pool, shares it),
 * whole MiB, within TIER0_CODE_DEFAULT_BYTES..TIER0_CODE_MAX_DEFAULT_BYTES.
 * Region metadata comes on top: per KiB of code a region record (448
 * bytes), its tag and hash links, about 46 % more, so the default arena
 * takes about 37 % of the reserve (18 % with Tier1). */
uint64_t tier0_default_code_bytes(uint64_t reserve_bytes, bool tier1);
bool tier0_configure_cmdline(const char *cmdline);
void tier0_destroy(void);
#ifdef IA64VMM_HOSTED
/* Inspect the owner state and actual mapping through hosted tests. */
bool tier0_test_mapping(const struct vcpu *v, const void **allocation, const void **entry,
                        bool *executable);
#endif
/* Diagnostic entry for testing decoded instructions with the real encoder
 * and W^X publication path, independent of an assembler's canonicalization. */
struct ia64_result tier0_test_instruction(struct vcpu *v, const Ia64Instruction *insn);
#endif
