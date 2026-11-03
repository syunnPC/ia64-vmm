/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tier1 compiler/runtime interface, linked
 * into the product and compiler tests (mk/tier1.mk).
 * Engine setup registers hostwin_code_page_event with code_validity_set_track_hook,
 * installs the trap backend and calls t1_vcpu_state_restored after every
 * snapshot restore.
 */
#ifndef IA64VMM_TIER1_H
#define IA64VMM_TIER1_H
#include "ia64/state.h"
struct hostwin_vcpu;
#define T1_ABI_VERSION 3u

/* Region limits. */
#define T1_MAX_INSNS 256u
#define T1_MAX_BLOCKS 64u
#define T1_MAX_PAGES 4u
#define T1_MAX_SITES 64u
#define T1_MAX_POINTS 512u

/*
 * Dispatcher preconditions: entries with any of these PSR bits go to
 * the lower tier (debug modes db, lp, tb, ss and the single-instruction
 * bits id, da, dd, ed, ia), as must PSR.be = 1 and PSR.is = 1. Defined
 * through the ia64/state.h macros, whose positions ia64/state.h pins to SDM
 * Vol.2 Table 3-2; the assertion pins the set itself (bits 24, 25, 26, 40,
 * 37, 38, 39, 43, 45).
 */
#define T1_PSR_LOWER_TIER                                                                          \
    (IA64_PSR_DEBUG_MODES | IA64_PSR_ID | IA64_PSR_DA | IA64_PSR_DD | IA64_PSR_ED | IA64_PSR_IA)
_Static_assert(T1_PSR_LOWER_TIER ==
                   ((UINT64_C(1) << 24) | (UINT64_C(1) << 25) | (UINT64_C(1) << 26) |
                    (UINT64_C(1) << 40) | (UINT64_C(1) << 37) | (UINT64_C(1) << 38) |
                    (UINT64_C(1) << 39) | (UINT64_C(1) << 43) | (UINT64_C(1) << 45)),
               "Tier1 lower-tier PSR set");

/*
 * Architectural element ids (struct t1_frame_entry.element,
 * t1_compile_options.pin_element, t1_compile_report.loop_carried[]),
 * resolved from the CFM of the instruction's frame (the contract's, or one
 * a call or alloc of the region made): GR/NaT p = ia64_gr_index(&names, r)
 * and PR q = ia64_pr_index(&names, p), where names is that frame with
 * rse_bof = its base above the entry's (form.h struct t1_fstate). 0 is not
 * an element (r0 and p0 are constants).
 *
 * Static GRs (p < 32) are physical. A stacked
 * GR is frame-relative: T1_ELEMENT_GR(32 + o) and T1_ELEMENT_NAT(32 + o)
 * name the stacked register at frame offset o from the entry's frame base
 * (0..95, after rotation by rrb.gr; E_STK(o)), whose home is v->s.gr /
 * v->s.nat[32 + (rse_bof + o) % 96] at that base. The contract holds no
 * BOF; the runtime maps E_STK through the entry's v->s.rse_bof (entry
 * image, NaT guard, materialization, helper write-back and the
 * verification hooks, struct t1_exec entry_bof).
 */
#define T1_ELEMENT_GR(p) ((uint16_t)(p))           /* p = 1..127: v->s.gr[p] */
#define T1_ELEMENT_NAT(p) ((uint16_t)(128u + (p))) /* p = 1..127: v->s.nat[p] */
#define T1_ELEMENT_PR(q) ((uint16_t)(256u + (q)))  /* q = 1..63: bit q of v->s.pr */
#define T1_ELEMENT_LC ((uint16_t)320u)             /* v->s.ar[AR_LC] */
#define T1_ELEMENT_LSB ((uint16_t)321u)            /* v->last_successful_bundle */
#define T1_ELEMENT_MEM ((uint16_t)322u)            /* guest memory (never in a frame) */
/* FR f = 2..127 (E_FR): the
 * physical FR ia64_fr_index(&entry_state, f) (rrb.fr is a contract
 * constant), home v->s.fr[]. FPI caches its significand separately
 * from the exact exponent/sign companion (FR_META below). Integer results
 * write {sig, 0x1003e, 0}; predicated old values keep their own tag. */
#define T1_ELEMENT_FR(f) ((uint16_t)(323u + (f))) /* 325..450 */
/* PSR.mfl/mfh: set by an in-region FR write and
 * materialized as `psr |= T1_PSR_MF_BITS & written` (an OR, never a copy). */
#define T1_ELEMENT_PSR_MF ((uint16_t)451u)
/* Element ids are below this bound; ir.h's T1_N_ELEMENTS (element bitsets)
 * grows to it with the first FR or PSR_MF element user (helper coverage). */
/* FPI: exact exponent/sign companion, including old predicated values.
 * FR(f) now caches only the significand; META caches exp | sign << 17. */
#define T1_ELEMENT_FR_META(f) ((uint16_t)(452u + (f))) /* 454..579 */
/* ar.pfs, ar.ec and b0-b7: written by calls and returns inside a region
 * (homes v->s.ar[AR_PFS], v->s.ar[AR_EC], v->s.br[k]). */
#define T1_ELEMENT_PFS ((uint16_t)580u)
#define T1_ELEMENT_EC ((uint16_t)581u)
#define T1_ELEMENT_BR(k) ((uint16_t)(582u + (k))) /* k = 0..7 */
#define T1_ELEMENT_LIMIT 590u

/*
 * PSR.mfl and PSR.mfh (bits 4 and 5, SDM Vol.2 Table
 * 3-2; ia64/fp.h IA64_PSR_MFL | IA64_PSR_MFH, asserted in
 * tests/tier1/unit_lift.c). A contract compares the entry PSR under its
 * psr_mask (form.h T1_CONTRACT_PSR_MASK), which excludes them: every point
 * after an FR write materializes the bit it set.
 */
#define T1_PSR_MF_BITS ((UINT64_C(1) << 4) | (UINT64_C(1) << 5))

struct t1_compile_request;
/* Where the product path sends compile requests. Called on the executor; copies *rq (the caller's
 * copy dies on return) and never blocks. false: not queued (queue full, or a
 * request with the same key is in flight); the head stays in its backoff. */
typedef bool (*t1_submit_fn)(const struct t1_compile_request *rq, void *arg);

/* t1_config.min_insns_per_entry by default. */
#define T1_MIN_INSNS_DEFAULT 16u
/* t1_config.max_regions at most: a link holds the region id in 20 bits. */
#define T1_MAX_REGIONS_LIMIT ((1u << 20) - 1u)
struct t1_config {
    uint64_t code_bytes;      /* arena, multiple of 1 MiB, default 8 MiB (the product
                                     passes tier1_code_mib=, reserve-derived default 16..512 MiB) */
    uint64_t wallclock_slice; /* default 16384 instructions */
    /* Harness engine only (t1_run): */
    unsigned hot_threshold;                                     /* 0 = only explicit t1_compile */
    struct vcpu_exit (*lower)(struct vcpu *v, uint64_t budget); /* default interp_run */
    /* Product path (Tier0 outer loop):
     * 0 selects the default. */
    uint32_t loop_threshold;      /* Tier0 entries before hot() for loop heads: 64 */
    uint32_t trace_threshold;     /* ... for other heads: 1024 */
    uint32_t min_insns_per_entry; /* P_min of the profitability demotion (tier1_min_insns=):
                                     T1_MIN_INSNS_DEFAULT */
    uint32_t max_regions;         /* region records: 4096 (tier1_regions=; the product
                                     sizes the table from the pool) */
    /* NULL: hot() compiles synchronously on the executor.
     * Otherwise hot() builds the request and submits it; a helper
     * compiles it with t1_compile_request (the helper queue). */
    t1_submit_fn submit;
    void *submit_arg;
    /* Diagnostic replay sampling; zero keeps existing compile options.
     * ENGINE_LOCKSTEP always checks every execution. */
    uint64_t check_every, check_after;
    void (*submit_reclaim)(void); /* optional helper scheduling, stopped setup only */
    /* tier1_verify=final: t1_ir_verify checks only the optimized IR instead
     * of the IR after every pass (passes, the default); raverify always
     * runs. */
    bool verify_final;
    /* tier1_chain=off: every upper entry runs one region. */
    bool chain_off;
};
struct t1_vcpu;
bool t1_init(const struct t1_config *cfg); /* stopped executors only */
void t1_shutdown(void);
struct t1_vcpu *t1_vcpu_create(struct vcpu *v, struct hostwin_vcpu *hw);
void t1_vcpu_destroy(struct t1_vcpu *tv);
/* The caller replaced *v (snapshot restore): clears the kick word,
 * hot counters, site caches and observed epoch; the next entry re-diffs the
 * MMU context. */
void t1_vcpu_state_restored(struct t1_vcpu *tv);
/* Exact budget. Non-budget exits clear v->code_entry_generation as
 * tier0_run does. */
struct vcpu_exit t1_run(struct t1_vcpu *tv, uint64_t budget);

enum t1_point_kind { T1_PT_EXIT, T1_PT_CHECKPOINT, T1_PT_SITE, T1_PT_HELPER };
struct t1_exit_info {
    uint32_t region_id, point_id;
    enum t1_point_kind kind;
    uint64_t consumed;
    bool lower_tier_step_followed;
};
/* Region exits of the most recent t1_run, oldest first. */
unsigned t1_exit_count(const struct t1_vcpu *tv);
bool t1_exit_get(const struct t1_vcpu *tv, unsigned index, struct t1_exit_info *out);

#define T1_COMPILE_VERIFY 1u    /* forced-fault counters, restore records, raverify */
#define T1_COMPILE_STORE_LOG 2u /* completed-store log */
/* With STORE_LOG: every
 * native store's record also holds the bytes it replaced (the undo log), and
 * eligible executions are replayed against the interpreter (checked.c,
 * tests and diagnostic builds). Lockstep checks every execution; the
 * diagnostic Tier1 engine can sample per region. The contract compares
 * the flags, so instrumented and normal regions never replace each other. */
#define T1_COMPILE_UNDO_LOG 4u
/* With STORE_LOG | UNDO_LOG: log native loads and replay ordered data accesses. */
#define T1_COMPILE_ACCESS_LOG 8u
struct t1_compile_options {
    uint32_t flags;
    uint16_t pin_element; /* 0 = none; else element id pinned ... */
    uint8_t pin_reg;      /* ... to this AMD64 register (tests only) */
};
enum t1_compile_status { T1_COMPILED, T1_REJECTED, T1_RETRY, T1_NO_MEMORY };
struct t1_compile_report {
    uint32_t region_id;
    const char *reject_reason;
    uint64_t reject_ip;
    unsigned reject_slot;
    bool reject_transient; /* formation could not read the source or found no snapshot */
    unsigned insns, blocks, pages, sites, points, loop_host_insns;
    unsigned loop_value_spills, restore_only_spills, nat_guards;
    size_t code_bytes;
    _Atomic uint64_t compile_ticks;
    uint16_t loop_carried[16];
    uint8_t loop_carried_reg[16];
    unsigned n_loop_carried;
    /* Compiled regions: why formation ended the region and the
     * first instruction not in it (the back-edge's fall-through, or the
     * boundary instruction). Diagnostic; NULL/0 for rejects. */
    const char *end_reason;
    uint64_t end_ip;
    unsigned end_slot;
};
/* Compile the region whose header is v's current ip/slot.  */
enum t1_compile_status t1_compile(struct t1_vcpu *tv, const struct t1_compile_options *opt,
                                  struct t1_compile_report *report);
/* Any thread or signal handler.  The kick word is the
 * vCPU's irq_kick: t1_kick(tv) is vcpu_kick(v), and the event checkpoint
 * reads it together with requested_exit (exec.h T1_VCPU_CKPT). */
void t1_kick(struct t1_vcpu *tv);
void t1_flush(void); /* stopped executors only */

/* ---- compile requests ----------------------------------------
 *
 * The product path never compiles from a live struct vcpu. At a hot head
 * the executor makes an immutable, self-contained request; formation and
 * compilation read only that request (and guest code through the shared
 * snapshots, code_source_read_physical); publication re-checks every source
 * generation it read and announces the new region only through
 * t1_upper_sequence(). Synchronous (t1_config.submit == NULL): the executor
 * compiles the request inside hot(). Asynchronous: a helper does,
 * on its own compile context, while the executor keeps running Tier0. The
 * two differ only in which thread calls t1_compile_request.
 *
 * Soundness never rests on the request being current: a region built from
 * stale inputs is declined at publication (changed bytes: generations) or
 * fails the entry checks (contract match in the probe, entry PA, fetch
 * guard of the other pages, NaT guard, bundle validation), so it is never
 * executed.
 */
/* One source page as the executor translated it when making the request:
 * an I-TLB hit (ia64_tlb_find) whose fetch translation completed, never a
 * VHPT walk; physical mode: pa = va without bit 63, pte = itir = rr = 0. */
struct t1_compile_page {
    uint64_t va;            /* page-aligned executing VA (bit 63 alias kept) */
    gpa_t pa;               /* page-aligned PA */
    uint64_t pte, itir, rr; /* the TLB entry and RR[va >> 61] of the translation */
    unsigned shift;         /* that entry's page size (log2; 12 in physical mode) */
    /* code_source_generation(pa) then (0: no snapshot yet). Informational:
     * a request whose page moved on may be dropped before compiling;
     * publication checks the generations formation read, not these. */
    uint64_t generation;
};
/* Why a request lists no further page (diagnostic: the end_reason of a
 * region that reaches it). */
enum t1_request_pages_end { T1_PAGES_LIMIT, T1_PAGES_NO_ITLB, T1_PAGES_FETCH_FAULT };
struct t1_compile_request {
    /* Coalescing key: architectural entry (entry PA, ip, slot, contract
     * psr/cfm, flags); observed targets do not distinguish requests. Never 0. */
    uint64_t key;
    /* The entry contract (form.h struct t1_contract). psr: v's PSR under
     * the contract mask; cfm: complete (sof, sol, sor, rrb.*), all
     * formation needs to name registers (stacked names are frame offsets,
     * No frame base). */
    uint64_t ip, psr, cfm;
    gpa_t entry_pa; /* page[0].pa + (ip & 4095) */
    uint8_t slot;
    unsigned cpu_model;    /* CPU model, immutable, part of the compile/entry contract. */
    bool icount;           /* v->cfg.itc_icount */
    bool slow_memory;      /* COV: retry a head with exact memory helpers after site demotion */
    bool loop_head;        /* the hot() argument (diagnostic, demotion policy) */
    unsigned hostwin_bits; /* the vCPU's window width (contract) */
    struct t1_compile_options opt; /* flags (incl. checked lockstep) and test pins */
    /* Observed BR targets, bundle-aligned. These are guarded at each
     * indirect call/observed return, not part of the entry contract/key. */
    uint64_t br_target[8];
    uint8_t br_observed; /* bit b: br_target[b] was captured */
    uint64_t return_cfm; /* observed PFS.pfm, guarded at unknown-frame returns */
    bool return_observed;
    /* The entry page and the pages after it in VA order, while each is an
     * I-TLB hit with a completed fetch translation, then the pages of
     * direct/observed calls and observed returns on the head's path, up to
     * T1_MAX_PAGES in all (form.c request_targets). Formation scans from
     * the header; a page it needs that is not listed ends the region there,
     * except the page of a call target that a listed page's translation
     * also covers (form.c derive_page). */
    unsigned npages;
    struct t1_compile_page page[T1_MAX_PAGES];
    /* One ordinary branch target's resident fetch translation, captured
     * without reading its code. Only branch_tail may use it: it does not
     * authorize the main scan to follow or snapshot this page. */
    struct t1_compile_page tail_page;
    bool tail_observed;
    uint8_t pages_end; /* enum t1_request_pages_end: why page[npages] is missing */
    uint64_t mmu_seq;  /* v->mmu_seq when the request was made */
    unsigned vcpu;     /* v->cfg.index */
};
/* Executor, at a Tier0 lookup point of v (the head is v's ip/slot): fills
 * *rq from v's state. false: v's state cannot be a region entry (lower-tier
 * PSR, slot, incomplete RSE frame, untranslatable entry page). */
bool t1_request_make(struct t1_vcpu *tv, gpa_t entry_pa, bool loop_head,
                     struct t1_compile_request *rq);
/* Background workers retain at most this many compile workspaces.
 * The synchronous compiler remains exclusive. */
#define T1_COMPILE_SLOTS 2u

/* Compile state: the request, the pipeline between steps and the workspace
 * block every stage carves its arrays from, taken at the
 * context's first compile. One per thread that compiles: each executor
 * (synchronous mode) and each helper. trim returns the block to the pool
 * (the context is idle); destroy returns everything. */
struct t1_compile_ctx;
struct t1_compile_ctx *t1_compile_ctx_create(void);
void t1_compile_ctx_destroy(struct t1_compile_ctx *ctx);
void t1_compile_trim(struct t1_compile_ctx *ctx);
/* Forms, compiles and publishes the region rq describes, on ctx, on the
 * calling thread, which need not be an executor. Reads no struct vcpu.
 * T1_COMPILED: published, the sequence moved; T1_RETRY: a source changed
 * (the caller may request again); T1_REJECTED: report says why;
 * T1_NO_MEMORY: deferred, the pool refused memory or a request is
 * outstanding. A synchronous compile excludes all other compiles. */
enum t1_compile_status t1_compile_request(struct t1_compile_ctx *ctx,
                                          const struct t1_compile_request *rq,
                                          struct t1_compile_report *report);

/* Cooperative compile pipeline for helpers and the management fallback.
 * begin copies rq and reserves one of T1_COMPILE_SLOTS background compiles,
 * or the exclusive synchronous compiler; false means busy.
 * Each step runs one pipeline stage, true while another stage remains.
 * Background completion stages private code; flush seals and links the batch.
 * cancel releases unpublished work and announces completion to hot heads.
 * The context must stay on one worker until completion or cancellation. */
bool t1_compile_begin(struct t1_compile_ctx *ctx, const struct t1_compile_request *rq,
                      bool background);
bool t1_compile_step(struct t1_compile_ctx *ctx, struct t1_compile_report *report);
void t1_compile_cancel(struct t1_compile_ctx *ctx);
/* Seal and revalidate the worker's staged regions before linking them. */
void t1_compile_flush(struct t1_compile_ctx *ctx);
uint64_t t1_executor_compile_ticks(void); /* excludes helper/management work */

/* ---- product path: the upper tier of Tier0 -----------
 *
 * Tier1 includes no engine/tier0 header, so it cannot
 * build Tier0's struct tier0_upper_ops itself: engine/engine.c fills one
 * from these functions (engine=tier1 only) and asserts that the enums below
 * equal tier0.h's. The per-vCPU struct t1_vcpu is v->upper, set by
 * engine/engine.c. Semantics: engine/tier0/tier0.h.
 */
enum t1_upper_status { T1_UPPER_DECLINED, T1_UPPER_RAN, T1_UPPER_RAN_PENDING, T1_UPPER_STOP };
enum t1_hot_status { T1_HOT_LINKED, T1_HOT_PENDING, T1_HOT_REJECTED, T1_HOT_DEFERRED };
/* Link (opaque (region id, generation), never 0) of the region whose
 * contract matches v's state at entry_pa, or 0. */
uint64_t t1_upper_probe(struct vcpu *v, gpa_t entry_pa);
/* Checks the link against v (liveness, generation, the contract components
 * Tier0's key does not determine), runs the remaining entry guards and the
 * region, materializes. May replace *link (re-probe) or clear it. */
enum t1_upper_status t1_upper_enter(struct vcpu *v, uint64_t *link, uint64_t region_budget,
                                    struct vcpu_exit *out);
/* t1_request_make, then t1_compile_request on the executor's context
 * (submit == NULL: LINKED or REJECTED) or t1_config.submit (PENDING or
 * REJECTED; REJECTED without a submission for a head formation rejects at
 * its first instruction). LINKED also when a matching region already
 * exists. Heads the
 * upper tier holds (profitability or NaT-guard demotion)
 * return REJECTED without compiling. DEFERRED: memory pressure, nothing
 * compiled or queued. */
enum t1_hot_status t1_upper_hot(struct vcpu *v, gpa_t entry_pa, bool loop_head);
/* Maintenance (t1_reclaim), or its request to the helpers for an
 * outstanding request or a batch of retired records; outside region code
 * only. */
void t1_upper_reclaim(void);
/* v executes sync.i. The sample holds the
 * entries of other vCPUs that run a region retired before it, read without
 * a lock and without dereferencing the regions; ia64_smp_sync_i
 * (core/smp.c) waits until each has ended. T1_VCPU_SLOTS bounds the vCPUs
 * with Tier1 state. */
#define T1_VCPU_SLOTS 256u
struct t1_sync_wait {
    unsigned n;
    struct {
        const struct t1_vcpu *tv;
        const void *region;
        uint64_t seq;
    } held[T1_VCPU_SLOTS];
};
unsigned t1_sync_i_sample(const struct vcpu *v, struct t1_sync_wait *w);
bool t1_sync_i_held(const struct t1_sync_wait *w, unsigned i); /* entry i has not ended */
struct vcpu *t1_sync_i_vcpu(const struct t1_sync_wait *w, unsigned i);
/* Moves on every publication and every finished request (published or
 * not); stable otherwise. Valid after t1_init. */
const uint64_t *t1_upper_sequence(void);
/* Per-head counters: the counter of the head at
 * ip (with its bit-63 alias) and slot is t1_upper_heads()[((ip >> 2) |
 * slot) & (T1_HEAD_SEQUENCES - 1)]. It moves after every publication of a
 * region at that head and every finished, cancelled or dropped compile
 * request for it; heads sharing a counter see each other's moves. */
#define T1_HEAD_SEQUENCES 16384u
const uint64_t *t1_upper_heads(void);
/* Nonzero while maintenance has work (t1_upper_reclaim). */
const unsigned *t1_upper_reclaim_due(void);
/* A queued request dropped without a compile: completes it as a finished
 * one does (its head's counter and the sequence move). */
void t1_compile_dropped(const struct t1_compile_request *rq);

/* ---- engine=tier1 in the product --------
 *
 * engine/engine.c reaches Tier1 only through this table: it composes
 * Tier0's struct tier0_upper_ops from it and sets up Tier1 for
 * engine=tier1. Binaries that do not link
 * engine/tier1 (the per-module test binaries) get engine.c's weak default,
 * which returns NULL: engine=tier1 is then a setup error. The product
 * links Tier1 and the bare-metal host-window backend; compiler tests link
 * the Linux test backend.
 * The function's name is outside Tier1's namespace for the weak default. */
struct t1_stats;
/* struct hostwin_stats totals, so that engine.c
 * needs no hostwin header: instances counted; fills (shadow pages mapped,
 * read-only ones among them), live shadow pages now, exits by reason
 * (unbacked: the backend could not alias a RAM page; not_ram: no
 * GM_RAM page there), backend operations that failed. */
struct t1_window_totals {
    uint64_t instances, fills, fills_ro, live, unbacked, not_ram, backend_failures;
};
struct t1_engine_glue {
    /* t1_init(cfg), the host's window backend (hostwin_host_backend: NULL
     * fails), its trap client and the code-page hook; false: *why says why.
     * hostwin_check: the MMU-log completeness check of every instance. */
    bool (*setup)(const struct t1_config *cfg, bool hostwin_check, const char **why);
    void (*teardown)(void);
    const struct t1_config *(*config)(void); /* after t1_init: with its defaults */
    /* v's window instance and struct t1_vcpu (the caller sets v->upper). */
    struct t1_vcpu *(*vcpu_create)(struct vcpu *v);
    void (*vcpu_destroy)(struct t1_vcpu *tv);
    /* The product API above. */
    uint64_t (*probe)(struct vcpu *v, gpa_t entry_pa);
    enum t1_upper_status (*enter)(struct vcpu *v, uint64_t *link, uint64_t region_budget,
                                  struct vcpu_exit *out);
    enum t1_hot_status (*hot)(struct vcpu *v, gpa_t entry_pa, bool loop_head);
    void (*reclaim)(void);
    const uint64_t *(*sequence)(void);
    const uint64_t *(*heads)(void);
    const unsigned *(*reclaim_due)(void);
    void (*stats)(struct t1_stats *out);
    /* The host windows of the vCPUs this table
     * created and has not destroyed, summed (hostwin_get_stats of each),
     * for the line engine.c prints next to the tier1 line: a product run
     * whose windows do not alias guest RAM shows unbacked exits;
     * tests/hosted/tier1stats.py fails on them. */
    void (*windows)(struct t1_window_totals *out);
    /* Remaining lazy compiler workspace, reserved before sizing snapshots. */
    uint64_t (*reserve_bytes)(void);
    /* Stopped setup, after snapshot sizing; free excludes fixed allocations. */
    void (*memory_budget)(uint64_t free_bytes, uint64_t unavailable, uint64_t reserve_bytes);
    /* Stopped executors: enable/reset background work after helper startup. */
    void (*background)(t1_submit_fn submit, void (*reclaim)(void));
    uint64_t (*executor_compile_ticks)(void); /* milestone time inside engine_run */
};
const struct t1_engine_glue *engine_tier1_glue(void);

/* Point enumeration. */
struct t1_point_info {
    uint32_t point_id;
    enum t1_point_kind kind;
    uint64_t ip;
    unsigned slot;
    unsigned insn_index, access_size;
    bool access_write;
    unsigned end; /* EXIT: enum t1_end_class */
};
unsigned t1_point_count(uint32_t region_id);
bool t1_point_get(uint32_t region_id, unsigned index, struct t1_point_info *out);

#ifdef IA64VMM_HOSTED
/* Verification hooks; effective only in regions compiled with T1_COMPILE_VERIFY. */
enum t1_force_action {
    T1_FORCE_TRAP_RETRY,
    T1_FORCE_TRAP_EXIT,
    T1_FORCE_MISS_EXIT,
    T1_FORCE_HELPER_EXIT
};
/* Runs for every EXIT action (trap, miss, helper) before
 * t1_materialize, without any hostwin_resolve; e.g. deterministic data DMA. */
typedef void (*t1_before_resolve_fn)(struct vcpu *v, void *arg);
bool t1_test_force(struct t1_vcpu *tv, uint32_t region_id, uint32_t point_id, uint64_t occurrence,
                   enum t1_force_action action, t1_before_resolve_fn before, void *arg);
enum t1_restore_path { T1_PATH_TRAP, T1_PATH_MISS_STUB, T1_PATH_EXIT_STUB, T1_PATH_HELPER };
enum t1_restore_outcome { T1_OUTCOME_RETRY, T1_OUTCOME_EXIT }; /* t1_restore_record.outcome */
struct t1_restore_record {
    uint32_t region_id, point_id;
    uint64_t occurrence, iteration, host_rip;
    enum t1_restore_path path;
    bool forced;
    uint8_t outcome;
    bool materialized;
    uint64_t retired;
    bool translated;
    uint64_t access_gva;
    bool access_write;
};
/* Called on the executor thread. For records with materialized:
 * immediately after t1_materialize() and before any lower-tier step,
 * interruption delivery, timer poll or region entry; otherwise at the retry.
 * May read *v, guest memory (gm_read), the device log and the store log;
 * must not write any of them and must not call t1_* / hostwin_* functions
 * except t1_get_stats and hostwin_get_stats. The harness captures the state
 * (vCPU by value, per-page RAM hashes, log lengths) and compares it with the
 * reference at the same retired count. */
typedef void (*t1_restore_fn)(struct t1_vcpu *tv, const struct vcpu *v,
                              const struct t1_restore_record *r, void *arg);
void t1_test_set_restore_observer(t1_restore_fn fn, void *arg);
/* gva: effective (pre-increment) address; value: the register value (the
 * comparator truncates to width); region/point/seq: diagnostics only. */
struct t1_store_record {
    uint32_t region_id, point_id;
    uint64_t gva, value, seq;
    uint8_t width;
    /* T1_COMPILE_UNDO_LOG regions: the bytes the store
                          * replaced, zero-extended */
    bool has_pre;
    uint64_t pre;
};
typedef void (*t1_store_fn)(const struct t1_store_record *r, void *arg);
void t1_test_set_store_observer(t1_store_fn fn, void *arg);
/* Called after the MMU-independent entry guards and before
 * hostwin_enter(), so changes it makes are seen by the MMU diff (guest-fault-restore). */
typedef void (*t1_entry_hook_fn)(struct t1_vcpu *tv, uint32_t region_id, void *arg);
void t1_test_set_entry_hook(t1_entry_hook_fn fn, void *arg);
/* wrong-old-value: when (region, point, occurrence) is materialized, XOR
 * the home of `element` (T1_ELEMENT_*) with `mask` after t1_materialize and
 * before the restore observer runs. */
bool t1_test_corrupt(struct t1_vcpu *tv, uint32_t region_id, uint32_t point_id, uint64_t occurrence,
                     uint16_t element, uint64_t mask);
/* Options for the compiles t1_run triggers by hotness and the
 * flags its region lookup requires (verification and production regions
 * never share a key); default all zero. */
void t1_test_set_compile_options(struct t1_vcpu *tv, const struct t1_compile_options *opt);
/* The emitted code of a region and the byte range of its loop
 * body, for the static checks (decode with x86enc's length decoder). */
bool t1_test_region_code(uint32_t region_id, const uint8_t **code, size_t *size, size_t *loop_start,
                         size_t *loop_end);
/* Called after every compile attempt, explicit or by
 * hotness, with v at the region header and the attempt's report.
 * Diagnostics only (region statistics); must not call t1_* functions. */
typedef void (*t1_compile_fn)(const struct vcpu *v, enum t1_compile_status st,
                              const struct t1_compile_report *rep, void *arg);
void t1_test_set_compile_observer(t1_compile_fn fn, void *arg);
/* The hotness thresholds of the product path
 * (t1_config.loop_threshold / trace_threshold) after t1_init, for harness
 * runs that measure the production configuration; 0 restores the value
 * t1_init set. They reach Tier0 when the upper tier is next registered
 * (engine_tier1_ops). */
void t1_test_set_thresholds(uint32_t loop_threshold, uint32_t trace_threshold);
#endif
#if defined(IA64VMM_HOSTED) || defined(IA64VMM_TIER1_CHECKED)
/* Mutation tests of checked lockstep
 * (engine/tier1/checked.c):
 * the next checked execution that has more than `index` store records
 * (any checked execution for T1_MUTATE_STATE) is altered once, right
 * before the checker uses what is altered; each must be reported as a
 * divergence. STORE_VALUE: record index's written value; PRE_IMAGE: its
 * pre-image (seen only if the replay reads the location before it stores
 * there); DROP_RECORD: the record removed; GPA: its address on another
 * page; STATE: r1 of the vCPU after the execution (a wrong Tier1 result);
 * CAPTURE_UNDO: the C side's page capture without undoing the records
 * made before it. LOAD_VALUE and APPEND_RECORD alter ordered access replay. */
enum t1_check_mutation {
    T1_MUTATE_NONE,
    T1_MUTATE_STORE_VALUE,
    T1_MUTATE_PRE_IMAGE,
    T1_MUTATE_DROP_RECORD,
    T1_MUTATE_GPA,
    T1_MUTATE_STATE,
    T1_MUTATE_CAPTURE_UNDO,
    T1_MUTATE_LOAD_VALUE,
    T1_MUTATE_APPEND_RECORD
};
#ifdef IA64VMM_HOSTED
void t1_test_check_mutate(enum t1_check_mutation m, unsigned index);
/* true while the executor is inside the
 * checker (replay and comparison of a checked execution): for tests
 * that act from another thread. */
bool t1_test_checking(void);
/* true while v executes a Tier1 region (its running region), read from
 * any thread. */
bool t1_test_running(const struct vcpu *v);
#endif
#endif

#define T1_CHAIN_LENGTHS 8u
/* The row at which a chain point returned to Tier0. */
enum t1_chain_return {
    T1_CHAIN_PAIR,
    T1_CHAIN_BREAK,
    T1_CHAIN_STATE,
    T1_CHAIN_INTERRUPT,
    T1_CHAIN_SUCCESSOR,
    T1_CHAIN_CONTRACT,
    T1_CHAIN_FETCH,
    T1_CHAIN_BUDGET,
    T1_CHAIN_NAT,
    T1_CHAIN_VALIDATE,
    T1_CHAIN_RETIRED,
    T1_CHAIN_RETURNS
};
/* Why a region left at an EXIT point. Where formation stopped (form.c
 * t1_end_class): the region's shape (size, page or loop limits; formation
 * stopped at no instruction) or the instruction formation could not take:
 * br.call, br.ret, alloc, another register-stack instruction (cover,
 * flushrs, loadrs, moves of ar.rsc, ar.bsp, ar.bspstore, ar.rnat), chk.s
 * or any other; a call or return formation did not follow counts as
 * br.call or br.ret. Elsewhere: a taken branch, a call or a return whose
 * target the region does not hold (or a return whose BR or ar.pfs differs
 * from its call's: it leaves before effect), a chk.s that found a NaT. */
enum t1_end_class {
    T1_END_LIMIT,
    T1_END_BRANCH,
    T1_END_CALL,
    T1_END_RET,
    T1_END_ALLOC,
    T1_END_RSE,
    T1_END_NAT,
    T1_END_OTHER,
    T1_END_CLASSES
};
#if IA64VMM_TIER1_STATS
/* The chunk partition excludes the stub page and the allocation's trailing
 * space smaller than a chunk. Code allocations include 16-byte alignment.
 * Placed pending/published/retired records all count until arena release. */
struct t1_arena_stats {
    uint64_t chunks[4];     /* free, open, closed, raw */
    uint64_t code_bytes[3]; /* pending, published, retired */
    /* Untouched tails and the allocated RW prefix above the sealed range. */
    uint64_t open_tail, closed_tail, unsealed_used;
    /* Sealed pages with no remaining allocation; remaining pages split
     * into code allocations, original page-rounding gaps and released
     * allocation holes. Empty pages include any padding they contain. */
    uint64_t sealed_empty, sealed_code, sealed_padding, sealed_holes;
};
#endif
#if IA64VMM_TIER1_STATS
enum t1_native_rebuild {
    T1_NATIVE_REBUILD_EMPTY,
    T1_NATIVE_REBUILD_RETIREMENT,
    T1_NATIVE_REBUILD_MMU,
    T1_NATIVE_REBUILD_PSR,
    T1_NATIVE_REBUILD_CFM,
    T1_NATIVE_REBUILD_BIND,
    T1_NATIVE_REBUILD_TOKEN,
    T1_NATIVE_REBUILD_ISEQ,
    T1_NATIVE_REBUILD_PTA,
    T1_NATIVE_REBUILD_REGION,
    T1_NATIVE_REBUILD_REBASE,
    T1_NATIVE_REBUILDS
};
enum t1_native_node_reject {
    T1_NATIVE_NODE_CAPACITY,
    T1_NATIVE_NODE_SHAPE,
    T1_NATIVE_NODE_WINDOW,
    T1_NATIVE_NODE_FETCH,
    T1_NATIVE_NODE_WITNESS,
    T1_NATIVE_NODE_GROUP,
    T1_NATIVE_NODE_SUCCESSOR,
    T1_NATIVE_NODE_CONTRACT,
    T1_NATIVE_NODE_REJECTS
};
enum t1_native_stop {
    T1_NATIVE_STOP_BATCH,
    T1_NATIVE_STOP_POINT,
    T1_NATIVE_STOP_EDGE,
    T1_NATIVE_STOP_EVENT,
    T1_NATIVE_STOP_CONTEXT,
    T1_NATIVE_STOP_TIMER,
    T1_NATIVE_STOP_BUDGET,
    T1_NATIVE_STOP_WINDOW,
    T1_NATIVE_STOP_SOURCE,
    T1_NATIVE_STOP_WITNESS,
    T1_NATIVE_STOP_SECOND,
    T1_NATIVE_STOP_MF,
    T1_NATIVE_STOP_NAT,
    T1_NATIVE_STOP_FR,
    T1_NATIVE_STOP_PUBLISH,
    T1_NATIVE_STOPS
};
#endif
struct t1_stats {
    _Atomic uint64_t compiles, rejects;
    uint64_t entries, entry_guard_fails, nat_guard_fails, exits_by_kind[4];
    uint64_t lower_steps, lower_retired, tier1_retired, window_misses, traps, trap_fills;
    uint64_t site_exits;
    _Atomic uint64_t invalidations, reclaimed;
    uint64_t arena_bytes;
#if IA64VMM_TIER1_STATS
    struct t1_arena_stats arena;
    uint64_t span_reuses, span_reuse_pages;
#endif
    /* The Tier1 pool, separate from the fixed code arena:
     * cap, workspace reservation, use, peak, refused allocations; compile
     * requests deferred for memory (never counted as rejects). */
    uint64_t memory_budget, memory_reserve, memory_bytes, memory_peak, memory_refusals;
    _Atomic uint64_t memory_deferrals;
    /* Deferred compiles by what was short (enum t1_defer_kind): deferred
     * before compiling, for an outstanding request of that kind (pressure),
     * or at an allocation of their own (refused); the sum of both is
     * memory_deferrals. hot() calls deferred for an outstanding request
     * before making one (synchronous mode adds the compiles it deferred to
     * hot_deferred). */
    uint64_t deferrals_pressure[3], deferrals_refused[3], hot_deferrals[3];
    /* Records allocated (live and awaiting reclamation) and the table's
     * size; pool bytes of the records and tables published; the pool's
     * workspace bytes (part of memory_bytes). */
    uint64_t regions, region_capacity, published_bytes, memory_workspace;
    /* The product path:
     * probes and links made; enter calls and their outcomes (a decline per
     * failed guard: dead or mismatched link, budget, rse_missing, NaT guard,
     * fetch guard of a non-entry page, code validation, region retired by
     * the guards, a lower-tier PSR state such as PSR.db); hot() calls and
     * their outcomes (deferred: memory pressure, the head's hotness count
     * restarts); probes and hot() calls refused for a lower-tier PSR
     * state; holds and demotions;
     * the arena's seals, chunk recycles, evictions, re-placements and
     * recompiles of evicted contracts (a lower bound). */
    uint64_t probes, probe_links, upper_enters, upper_ran, upper_ran_pending, upper_stops;
    uint64_t decline_link, decline_budget, decline_rse, decline_nat, decline_fetch;
    uint64_t decline_validate, decline_retired, decline_lower;
    uint64_t hot_calls, hot_linked, hot_pending, hot_rejected, hot_deferred, hot_held,
        lower_refusals;
    uint64_t holds, demotions, demote_profit, demote_nat, demote_site;
    _Atomic uint64_t seals, chunk_recycles, evictions, replacements, recompiles;
    /* enter calls whose link named another frame (rotation bases) and
     * whose re-probe found the region for v's (relinks); variant compiles
     * enter started for a frame without a region; entries into re-placed
     * code. */
    uint64_t relinks, variant_compiles, moved_entries;
    /* The fetch guard of non-entry pages at enter: runs
     * of the guard (cache misses in virtual mode) and cache hits. */
    uint64_t fetch_guards, fetch_cache_hits;
    /* The compile_ticks of every compile request
     * (published or not), summed: Tier1's part of the milestone lines'
     * translation time (engine_compile_ticks); 0 unless
     * IA64VMM_TIER1_STATS. */
    _Atomic uint64_t compile_ticks;
    /* Checked lockstep:
     * host faults taken at the undo log's pre-load of a store site (then
     * resolved as the store's own access); Tier1 executions checked
     * against the interpreter and their retired instructions; Tier1
     * executions under ENGINE_LOCKSTEP that were not checked (a region
     * without the undo log; never expected) and theirs; divergences. */
    uint64_t undo_pre_traps, checked, checked_retired, unchecked, unchecked_retired, divergences;
    /* RAM pages captured by the C side of checked executions (walks of
     * hostwin_resolve; class M helpers). */
    uint64_t check_captures;
    /* Window misses at a site whose tag slot held a valid tag published by
     * the same region or by another region (the slot index is shared). */
    uint64_t window_misses_same_region, window_misses_other_region;
    /* Holds taken for a head whose executor compile at hot() was rejected. */
    uint64_t reject_holds;
    /* sync.i instructions that waited for executors running a retired
     * region. */
    uint64_t sync_waits;
    /* Region chaining: EXIT exits whose next boundary
     * entered Tier1 at the exit target with no instruction retired in
     * between, and upper entries by the number of regions they ran:
     * 1, 2, 3-4, 5-8, ..., 65 and more. Chain points: successor-cache
     * hits, regions entered (links) and returns to Tier0 by row. */
    uint64_t chainable, chain_lengths[T1_CHAIN_LENGTHS];
    uint64_t succ_hits, chain_links, chain_returns[T1_CHAIN_RETURNS];
    uint64_t chain_native_links, chain_native_batches;
    uint64_t chain_native_plans, chain_native_reuses;
    /* Statistics builds: failed preparations, classified in this order;
     * context includes unavailable witnesses and successor edges. */
    uint64_t chain_native_decline_virtual, chain_native_decline_timer;
    uint64_t chain_native_decline_shape, chain_native_decline_context;
#if IA64VMM_TIER1_STATS
    /* Empty is exclusive; otherwise every mismatched key counts. Region and
     * rebase count only after all keys match. Builds include rejected graphs. */
    uint64_t native_rebuild[T1_NATIVE_REBUILDS], native_node_reject[T1_NATIVE_NODE_REJECTS];
    uint64_t native_discard_learned, native_discard_failed, native_discard_rebuild;
    uint64_t native_topology_hits, native_topology_misses, native_topology_negative;
    uint64_t native_topology_retained;
    uint64_t native_build_nodes[9], native_build_edges[257], native_hops[33];
    /* [one/multiple graph edges][zero/nonzero completed hops][failed guard]. */
    uint64_t native_stop[2][2][T1_NATIVE_STOPS];
#endif
    /* Rejected compile requests by the stage that rejected them (enum
     * t1_reject_stage; their sum is `rejects`). */
    uint64_t rejects_by_stage[9];
    /* hot() calls refused before compiling: no compile request could be
     * made for v's state (t1_request_from_vcpu), and the helper queue
     * declined the request (busy, full, in flight). */
    uint64_t hot_unrequestable, hot_submit_refusals;
    /* Contracts that differ from v's state in PSR bits alone (t1_psr_bits
     * T1_PSR_VARIANT): enter calls whose linked head has no region for v's
     * state but one for other PSR bits, and hot() calls of heads with such
     * a region. */
    uint64_t variant_psr_misses, hot_psr_variants;
    /* hot() calls refused before a submission by the head pre-check
     * (formation would reject at the head); heads rejected for good (the
     * pre-check, non-transient compile rejects), each counted at its first
     * reject in a set of 2^16 hashed heads; holds that replaced another
     * head's hold in the direct-mapped hold table. */
    uint64_t hot_head_rejects, rejected_heads, hold_collisions;
    /* EXIT exits by the class of their target instruction, chained or not;
     * upper entries that returned to Tier0 at such an exit (product path),
     * and the instructions retired from that return to the next upper
     * entry. */
    uint64_t exits_by_end[T1_END_CLASSES], returns_by_end[T1_END_CLASSES],
        gap_by_end[T1_END_CLASSES];
    /* In-region allocs that spill (helper.c): completed in the region
     * (spills), and left before effect because a page of their mandatory
     * stores had no resident translation, so that the lower tier executes
     * the alloc and takes the fault (spill_exits). */
    uint64_t spills, spill_exits;
};
/* What a deferred compile lacked: a region id, a code
 * chunk or pool bytes. */
enum t1_defer_kind { T1_DEFER_ID, T1_DEFER_CHUNK, T1_DEFER_BYTES, T1_DEFER_KINDS };
_Static_assert(T1_DEFER_KINDS == sizeof(((struct t1_stats *)0)->hot_deferrals) / sizeof(uint64_t),
               "t1_stats deferral kinds");
/* request: t1_compile's request from the vCPU state; then the compile
 * pipeline's stages (lift includes the optimizer and its verification,
 * regalloc includes raverify; publish: retries exhausted); cancelled:
 * t1_compile_cancel of a compile in progress. */
enum t1_reject_stage {
    T1_REJECT_REQUEST,
    T1_REJECT_FORM,
    T1_REJECT_LIFT,
    T1_REJECT_ISEL,
    T1_REJECT_REGALLOC,
    T1_REJECT_EMIT,
    T1_REJECT_FRAME,
    T1_REJECT_PUBLISH,
    T1_REJECT_CANCELLED,
    T1_REJECT_STAGES
};
_Static_assert(T1_REJECT_STAGES ==
                   sizeof(((struct t1_stats *)0)->rejects_by_stage) / sizeof(uint64_t),
               "t1_stats.rejects_by_stage");
/* Formation stops at an instruction it cannot take (form.c classify):
 * the count of such stops at `opcode`, either at the head (head: the
 * request is rejected) or after the region's last instruction. */
uint64_t t1_form_stops(unsigned opcode, bool head);
/* Calls the formation scan met: followed into their
 * callee (a callee it then did not return from is cut afterwards), left at
 * the callee because its page is not one of the request's, or left for
 * another reason (a predicate, the call depth, recursion). */
enum t1_call_note { T1_CALL_FOLLOWED, T1_CALL_PAGE, T1_CALL_OTHER, T1_CALL_NOTES };
uint64_t t1_form_calls(enum t1_call_note kind);
/* In-region helper calls (helper.c t1_helper_call) by the opcode of the
 * helper's instruction; concurrent vCPUs may lose an update. */
uint64_t t1_helper_calls(unsigned opcode);
/* PSR bits behind two statistics, by PSR bit (0..63): the PSR-only
 * differences counted in variant_psr_misses and hot_psr_variants, and the
 * lower-tier PSR states counted in decline_lower and lower_refusals
 * (T1_PSR_LOWER_TIER, PSR.be, PSR.is). */
enum t1_psr_count { T1_PSR_VARIANT, T1_PSR_LOWER };
uint64_t t1_psr_bits(enum t1_psr_count kind, unsigned bit);
void t1_get_stats(struct t1_stats *out);
void t1_dump_stats(void);
#endif
