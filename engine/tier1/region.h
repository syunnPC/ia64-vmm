/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tier1 runtime records, private to engine/tier1: the published region (contract, code, point and
 * frame tables, RIP table, helpers, code-page dependencies), the per-vCPU
 * state (struct t1_vcpu, holding the execution context R14 points to), and
 * the declarations shared by region.c, arena.c, stubs.c, frame.c,
 * compile.c, dispatch.c, materialize.c, trap.c, vhooks.c, hot.c and
 * stats.c.
 *
 * Lifecycle: compiling -> placed RW -> published
 * with dependencies -> retired (unlinked, valid = false) -> reclaimed
 * after executor quiescence. Helpers seal before publication; synchronous
 * regions seal at first entry or chunk closure. Unregistered fixtures keep
 * the execution pin. Only an arena without epoch reclamation (one
 * unregistered executor) moves a live region's code; its id and generation
 * stay.
 */
#ifndef IA64VMM_TIER1_REGION_H
#define IA64VMM_TIER1_REGION_H
#include "engine/tier1/codegen.h"
#include "engine/tier1/form.h"
#include "engine/cache/validity.h"
#include "ia64vmm/hal.h"
#include <stdatomic.h>

/* Clock reads for statistics (compile_ticks) only with the
 * gate on; the same macro gates hostwin's snapshot_ticks (MMU log). */
#ifndef IA64VMM_TIER1_STATS
#define IA64VMM_TIER1_STATS 0
#endif

/* Region records: t1_config.max_regions, default 4096.
 * A link (tier1.h) packs (generation, id) with the id in the low
 * T1_LINK_ID_BITS bits, which bounds the configured capacity. */
_Static_assert(T1_MAX_PAGES <= CODE_STAY_PAGES,
               "a validator stay covers every source page of a region");
#define T1_MAX_REGIONS_DEFAULT 4096u
#define T1_LINK_ID_BITS 20u
_Static_assert(T1_MAX_REGIONS_LIMIT == (1u << T1_LINK_ID_BITS) - 1u, "region ids fit a link");
#define T1_EXIT_LOG 4096u
#define T1_HOT_ENTRIES 256u
#define T1_MAX_ARMS 16u
#define T1_DEAD_POINT 0xffu /* struct t1_point.kind of a removed point */

/* RIP table entry: a site's access, forced trap, or (with
 * T1_COMPILE_UNDO_LOG) the pre-load of a store site, which faults where the
 * store would and resumes at itself after a fill. */
enum t1_rip_kind { T1_RIP_ACCESS, T1_RIP_FORCE, T1_RIP_PRE };
struct t1_rip {
    uint32_t offset, point;
    uint8_t kind;
};

/* An in-region helper: the immutable instruction copy, the frame
 * entries holding its read set (nreads indices into region->frame), the
 * write set of a generalized helper (nwrites elements, t1_helper_elements
 * at compile time: frame-relative), both in the region's
 * tables, and the registers of its results. */
struct t1_helper_info {
    uint32_t point;
    Ia64Instruction insn;
    uint16_t nreads, nwrites;
    uint32_t *read_frame;
    uint16_t *writes;
    uint16_t wr[2];     /* elements of results p1/p2 (0: none) */
    uint8_t def_reg[2]; /* their registers, NOREG if unused */
};

/* rel32 fields of a region's code that reach the stubs (struct t1_emitted
 * abs_fix/abs_target), kept with the region so the arena can re-place its
 * code without recompiling it. The target is stored as
 * a signed offset from t1_stub.exit_save, which shares the stub page with
 * all other target wrappers and stays fixed for the arena's lifetime. */
struct t1_fixup {
    uint32_t at;
    int32_t stub_offset;
};
_Static_assert(sizeof(struct t1_fixup) == 8, "compact retained stub fixup");

struct t1_region {
    uint32_t id;
    uint64_t link;      /* (generation, id), fixed while the record lives */
    _Atomic bool valid; /* published and not retired */
    bool retired;       /* published, then retired: reclaimable */
    bool loop;          /* the region holds its loop (form.loop) */
    /* Calls, returns or allocs change its frame (points carry their frame,
     * struct t1_point delta/cfm). */
    bool frames, calls;
    bool observed_returns;
    uint8_t return_sof;      /* largest observed caller SOF: entry dirty <= 96 - SOF */
    bool policy;             /* compiled by hotness (t1_upper_hot): demotion applies */
    _Atomic bool referenced; /* entered since the arena's clock last passed it */
    _Atomic bool sealed;     /* its code is RX (t1_arena_seal_for_entry) */
    uint8_t moved;           /* re-placed by the arena (1) or not (0) */
    struct t1_contract c;
    /* Eligible replay executions of this region across executors. */
    _Atomic uint64_t check_entries;
    unsigned npages;
    struct t1_page page[T1_MAX_PAGES];
    struct code_dependency dep[T1_MAX_PAGES]; /* entry page first (code_dependencies_enter) */
    bool dep_attached[T1_MAX_PAGES];
    /* G by element register number: bit p < 32 static, p >= 32 the
     * stacked E_STK(p - 32). guard[] is the same set as
     * t1_nat_guard_holds reads it (t1_nat_guard_list): nguard_static static
     * indices, then the stacked offsets up to nguard, guard_span = 1 + the
     * largest stacked offset. */
    uint64_t nat_guard[2];
    uint64_t fr_guard[2]; /* FPI: physical FRs, CFM fixes rotation */
    uint8_t nguard, nguard_static, guard_span;
    uint8_t guard[127];
    /* Entry image: slot k of x->frame_in holds the
     * k-th stacked LIVEIN the code reads, frame offset o in bits 6:0 and
     * T1_FRAME_IN_NAT for a NaT (else the value); t1_frame_in_fill. */
    unsigned nframe_in;
    uint8_t frame_in[T1_MAX_FRAME_IN];
    /* 1 + the largest offset among the value slots (0 if none), UINT8_MAX
     * with a NaT slot: below 96 - BOF the fill is a plain gather. */
    uint8_t frame_in_span;
    unsigned max_path, nsites, ninsns, nblocks;
    /* Ordered replay executes the immutable source selected at compilation. */
    struct t1_check_insn {
        Ia64Instruction insn;
        uint8_t steps, memory_base;
    } *check_insn;
    unsigned site_base; /* first x->site[] slot of its sites (t1_site_base) */
    /* Code: [code, code + code_size) inside arena chunk `chunk`. */
    uint8_t *code;
    size_t code_size;
    unsigned chunk;
    struct t1_region *chunk_next; /* the chunk's regions */
    struct t1_fixup *fix;         /* in the region's tables (meta) */
    unsigned nfix;
    t1_entry_fn entry;
    uint32_t native_entry;    /* code offset; R12=v, R13=budget, R14=x */
    uint32_t fast_points;     /* stable-frame EXITs with native home restoration, points 0..31 */
    uint32_t frame_in_offset; /* (x, v) C-ABI entry-image leaf; 0: C fallback */
    uint32_t loop_start, loop_end;
    /* Points: indexed by point id; removed points have kind T1_DEAD_POINT. */
    unsigned npoints, nlive_points;
    struct t1_point *point;
    struct t1_frame_entry *frame;
    unsigned nframe;
    struct t1_rip *rip;
    unsigned nrip;
    struct t1_helper_info *helper;
    unsigned nhelper;
    uint16_t *helper_of; /* per point: helper index, UINT16_MAX if none */
    /* Statistics. */
    uint64_t entries;
    uint64_t retired_insns; /* instructions retired in it (IA64VMM_TIER1_STATS) */
    struct t1_compile_report report;
    /* Demotion windows of the product path (hot.c):
     * one word each, the window's generation in bits 63..32 and its counts
     * below (T1_WINDOW_*), changed by compare-and-swap; the entry count
     * and variant misses: relaxed hints. */
    uint64_t profit_window; /* entries 31..20 (256), retired 19..0 (saturating) */
    uint64_t site_window;   /* entries 31..0 (4096) */
    uint64_t nat_window;    /* attempts 31..16 (256), failures 15..0 */
    uint8_t helper_exits
        [T1_MAX_INSNS]; /* COV: BEFORE faults per helper instruction in the site window */
    uint8_t site_exits[T1_MAX_SITES]; /* exits per site in that window */
    uint32_t variant_misses;          /* enter: linked here, v in another frame without a region */
    /* Lookup chain (by ip, slot); retired list (reclamation). */
    _Atomic(struct t1_region *) hash_next;
    struct t1_region *retired_next;
    uint64_t retirement; /* shared code epoch: no executor retains this record */
    void *meta;
    size_t meta_pages;
};

/* The entry count and variant misses of
 * shared regions are relaxed hints. Each executor stores the value it read
 * plus one, so an update may be lost, but no access is a data race. */
static inline uint32_t t1_hint_add32(uint32_t *p, uint32_t d)
{
    uint32_t n = __atomic_load_n(p, __ATOMIC_RELAXED) + d;
    __atomic_store_n(p, n, __ATOMIC_RELAXED);
    return n;
}
static inline uint64_t t1_hint_add64(uint64_t *p, uint64_t d)
{
    uint64_t n = __atomic_load_n(p, __ATOMIC_RELAXED) + d;
    __atomic_store_n(p, n, __ATOMIC_RELAXED);
    return n;
}
/* A site or helper point's exits in the site window (hot.c), counted
 * at once by compare-and-swap, saturating. */
static inline unsigned t1_window_inc8(uint8_t *p)
{
    uint8_t n = __atomic_load_n(p, __ATOMIC_SEQ_CST);
    do {
        if (n == UINT8_MAX)
            return n;
    } while (!__atomic_compare_exchange_n(p, &n, (uint8_t)(n + 1), false, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST));
    return n + 1u;
}

/* Verification vocabulary, usable in freestanding builds where tier1.h
 * hides the hosted-only test interface; equal to the public enums. */
enum t1v_path { T1V_PATH_TRAP, T1V_PATH_MISS_STUB, T1V_PATH_EXIT_STUB, T1V_PATH_HELPER };
enum t1v_outcome { T1V_RETRY, T1V_EXIT };
enum t1v_action { T1V_TRAP_RETRY, T1V_TRAP_EXIT, T1V_MISS_EXIT, T1V_HELPER_EXIT };
typedef void (*t1v_before_fn)(struct vcpu *v, void *arg);
#ifdef IA64VMM_HOSTED
_Static_assert((int)T1V_PATH_TRAP == (int)T1_PATH_TRAP &&
                   (int)T1V_PATH_MISS_STUB == (int)T1_PATH_MISS_STUB &&
                   (int)T1V_PATH_EXIT_STUB == (int)T1_PATH_EXIT_STUB &&
                   (int)T1V_PATH_HELPER == (int)T1_PATH_HELPER &&
                   (int)T1V_RETRY == (int)T1_OUTCOME_RETRY &&
                   (int)T1V_EXIT == (int)T1_OUTCOME_EXIT &&
                   (int)T1V_TRAP_RETRY == (int)T1_FORCE_TRAP_RETRY &&
                   (int)T1V_TRAP_EXIT == (int)T1_FORCE_TRAP_EXIT &&
                   (int)T1V_MISS_EXIT == (int)T1_FORCE_MISS_EXIT &&
                   (int)T1V_HELPER_EXIT == (int)T1_FORCE_HELPER_EXIT,
               "verification enums");
#endif

/* Per-vCPU occurrence counters of one region's points (verification). */
struct t1_occ {
    uint32_t region;
    uint64_t *count;
    size_t pages;
    uint64_t iterations;
};

struct t1_arm {
    bool active, fired;
    uint32_t region, point;
    uint64_t occurrence; /* remaining until the armed one, counted from arming */
    uint8_t action;      /* enum t1v_action */
    t1v_before_fn before;
    void *arg;
};
struct t1_corrupt {
    bool active;
    uint32_t region, point;
    uint64_t occurrence;
    uint16_t element;
    uint64_t mask;
};

/* What the trap client / miss handler / helper learned about the exit or
 * retry in progress (vhooks.c builds the restore record from it). */
struct t1_pending {
    uint8_t path; /* enum t1v_path */
    bool forced, translated, write;
    uint64_t gva, host_rip;
};

struct hot_entry {
    uint64_t ip;
    uint8_t slot;
    uint32_t count;
    uint32_t backoff;
};

/* Per-executor demotion counts, direct-mapped by region id. Fold every
 * T1_WINDOW_FOLD entries or on replacement; discard closed-window counts. */
#define T1_WINDOW_SLOTS 16u
#define T1_WINDOW_FOLD 16u
struct t1_window_slot {
    uint64_t link, retired;
    uint32_t profit_gen, site_gen, nat_gen, entries, attempts;
};
struct t1_windows {
    struct t1_window_slot slot[T1_WINDOW_SLOTS];
};
/* The window words' fields (struct t1_region). */
#define T1_WINDOW_GEN(w) ((uint32_t)((w) >> 32))
#define T1_PROFIT_ENTRIES(w) ((uint32_t)((w) >> 20) & 0xfffu)
#define T1_PROFIT_RETIRED(w) ((uint32_t)(w) & 0xfffffu)
#define T1_NAT_ATTEMPTS(w) ((uint32_t)((w) >> 16) & 0xffffu)
#define T1_NAT_FAILS(w) ((uint32_t)(w) & 0xffffu)

#define T1_FETCH_CACHE 16u
#define T1_TRANSITIONS 256u
#define T1_SUCCESSORS 256u

/* A lane never publishes quiescence and completes at most thirty-two edges
 * before returning to C. Cached plans may survive a boundary only behind
 * an unchanged retirement epoch, checked before cached pointee access. */
#define T1_FAST_NODES 8u
#define T1_FAST_POINTS 32u
#define T1_FAST_BATCH 32u
#define T1_FAST_CONNECTIONS 64u
/* Only generation-stamped links survive independently of an active plan.
 * Admission witnesses and host addresses remain in the epoch-pinned nodes. */
struct t1_fast_connections {
    uint64_t from, target[T1_FAST_POINTS];
    uint64_t retirement, psr, cfm;
    uint32_t checked, eligible;
    unsigned cpu_model, hostwin_bits;
    bool icount;
};
struct t1_fast_node;
struct t1_fast_edge {
    struct t1_fast_node *target;
    const struct t1_point *point;
    uintptr_t restore;
};
struct t1_fast_node {
    struct t1_region *region;
    struct t1_window_slot *window; /* NULL for non-policy regions */
    struct code_stay_witness witness;
    uintptr_t entry, frame_in;
    uint32_t max_path, nguard, nfr_guard, outgoing;
    uint64_t nat_guard[2];  /* residual frame-relative identities, retained across BOF changes */
    uint32_t fr_guard[126]; /* displacements of physical FR homes; CFM fixes rotation */
    uint32_t guard[127];    /* byte displacements of NaT homes in this vCPU */
    struct t1_fast_edge edge[T1_FAST_POINTS];
    const struct code_stay_witness *second; /* NULL for one-page nodes */
};
struct t1_fast_record {
    struct t1_fast_node *from, *to;
    uint64_t consumed;
    uint32_t point;
};
struct t1_fast {
    struct t1_fast_node node[T1_FAST_NODES];
    struct t1_fast_record record[T1_FAST_BATCH];
    /* Keep nonentry-page witnesses outside the common node footprint. */
    struct code_stay_witness second[T1_FAST_NODES];
    uint64_t mmu_seq, bind_epoch, psr, cfm, nrecord;
    uint64_t retirement, token, iseq, pta;
    unsigned bof, dirty;
    /* Test/diagnostic progress; one executor writer, read with acquire. */
    _Atomic uint64_t completed;
    unsigned nnodes;
#if IA64VMM_TIER1_STATS
    unsigned diag_edges, diag_stop;
#endif
    /* Topology survives plan rebuilds; keep it after the active plan fields. */
    struct t1_fast_connections connections[T1_FAST_CONNECTIONS];
};

static inline unsigned t1_successor_index(uint64_t from, uint32_t point)
{
    return (unsigned)(((from ^ ((uint64_t)point << 40)) * UINT64_C(0x9e3779b97f4a7c15)) >> 56) %
           T1_SUCCESSORS;
}

struct t1_vcpu {
    struct t1_exec x; /* R14 points here while a region runs */
    struct vcpu *v;
    struct t1_stats *stats; /* T1_COUNT_TV: the current entry's counter slot */
    struct hostwin_vcpu *hw;
    struct t1_compile_options opt; /* implicit compiles (hotness) and lookups */
    struct hot_entry hot[T1_HOT_ENTRIES];
    struct hot_entry tail_hot[T1_HOT_ENTRIES]; /* independent counts and failure backoff */
    struct t1_exit_info exits[T1_EXIT_LOG];
    unsigned nexits;
    /* Verification. */
    uint64_t force_initial[T1_MAX_POINTS]; /* x->force[] at entry */
    struct t1_arm arm[T1_MAX_ARMS];
    struct t1_corrupt corrupt[T1_MAX_ARMS];
    struct t1_occ occ[64];
    unsigned nocc;
    uint64_t store_seq;
    struct t1_pending pending;
    uint8_t trap_kind; /* 0 none, 1 real fault, 2 forced trap, 3 real fault of a pre-load */
    uint64_t trap_rip;
    /* Cached fetch guards, direct-mapped by region id; link 0 is empty.
     * Keys: region generation, I-side MMU sequence and T1_FETCH_PSR. The
     * entry IP's I-TLB PTE supplies its code-fetch context (dispatch.c). */
    struct t1_fetch_ok {
        uint64_t link, mmu_iseq, psr, entry_pte;
    } fetch_ok[T1_FETCH_CACHE];
    /* Replay workspace (checked.c), allocated at the first checked execution. */
    struct t1_check *check;
    bool checking;
    /* Publish running then entry_seq (seq_cst) before the final validity
     * check; clear running after exit. Retirement marks stale_seq under
     * code_lock. Other threads inspect these fields without dereferencing r. */
    _Atomic(const struct t1_region *) running;
    _Atomic uint64_t entry_seq, stale_seq;
    /* Last exit to Tier0. Matching (retired, ip, slot) means no intervening
     * retirement; learn records that entry as the successor of (from, point).
     * end is enum t1_end_class. */
    struct t1_exit_mark {
        uint64_t from, retired, ip;
        uint32_t point;
        unsigned slot;
        uint8_t end;
        bool valid, learn;
    } last_exit;
    /* The successor cache, direct-mapped by (from, point);
     * links name records, so retirement, eviction and reclamation
     * invalidate entries by generation. */
    struct t1_successor {
        uint64_t from, to;
        uint32_t point;
    } successors[T1_SUCCESSORS];
    /* The demotion windows' counts this executor has not folded (hot.c). */
    struct t1_windows windows;
    struct t1_fast fast;
#ifdef IA64VMM_HOSTED
    /* The region being entered was reached at a chain point (tests). */
    bool chained;
#endif
#if IA64VMM_TIER1_STATS
    /* Transitions from (region, EXIT point) to the next region entered at
     * its target, counted (open addressing on from and point; a full table
     * drops new pairs). */
    struct t1_transition {
        uint64_t from, to, count;
        uint32_t point;
    } transitions[T1_TRANSITIONS];
#endif
};

/* ---- globals (region.c, dispatch.c) ---- */
extern struct t1_config t1_cfg;
extern bool t1_ready;
/* Trap context for this host CPU (hosted: thread); NULL outside region code. */
#define T1_CPU_SLOTS 256u
/* t1_any_current also pins unregistered single-executor fixtures (region.c). */
#ifdef IA64VMM_HOSTED
extern _Thread_local struct t1_exec *volatile t1_current_context;
extern _Thread_local bool t1_current_pinned;
extern unsigned t1_current_count;
static inline struct t1_exec *t1_current(void)
{
    return t1_current_context;
}
static inline void t1_set_current(struct t1_exec *x)
{
    struct t1_exec *old = t1_current_context;
    if (x && !old) {
        /* Registered executors already pin code through their epoch. */
        t1_current_pinned = !x->v || !code_vcpu_is_registered(x->v);
        if (t1_current_pinned)
            __atomic_add_fetch(&t1_current_count, 1, __ATOMIC_SEQ_CST);
    }
    t1_current_context = x;
    if (!x && old && t1_current_pinned) {
        __atomic_sub_fetch(&t1_current_count, 1, __ATOMIC_SEQ_CST);
        t1_current_pinned = false;
    }
}
static inline bool t1_any_current(void)
{
    return __atomic_load_n(&t1_current_count, __ATOMIC_SEQ_CST) != 0;
}
#else
extern struct t1_exec *volatile t1_current_context[T1_CPU_SLOTS];
static inline struct t1_exec *t1_current(void)
{
    unsigned cpu = hal_cpu_index();
    return cpu < T1_CPU_SLOTS ? t1_current_context[cpu] : NULL;
}
static inline void t1_set_current(struct t1_exec *x)
{
    unsigned cpu = hal_cpu_index();
    if (cpu >= T1_CPU_SLOTS)
        hal_panic("tier1: executor CPU index out of range");
    t1_current_context[cpu] = x;
}
static inline bool t1_any_current(void)
{
    for (unsigned cpu = 0; cpu < T1_CPU_SLOTS; ++cpu)
        if (t1_current_context[cpu])
            return true;
    return false;
}
#endif
/* Every vCPU's Tier1 state (T1_VCPU_SLOTS, tier1.h), registered and
 * removed with stopped executors (dispatch.c): retirement and sync.i scan
 * the running regions. */
extern struct t1_vcpu *t1_vcpus[T1_VCPU_SLOTS];
/* Code_lock held, r just retired: mark the entries running it and
 * kick every other executor running it. */
void t1_retired_running(const struct t1_region *r);

/* arena.c: the stub page, then 64 KiB chunks. */
#define T1_CHUNK_BYTES 65536u
bool t1_arena_init(uint64_t bytes);
void t1_arena_shutdown(void);
uint8_t *t1_arena_stub_page(void);
uint64_t t1_arena_bytes_used(void);
#if IA64VMM_TIER1_STATS
/* Snapshot occupancy and return arena_bytes under the same lock. */
uint64_t t1_arena_get_stats(struct t1_arena_stats *out);
#endif
/* Places code with its stub fixups in a private RW chunk. Under epoch
 * reclamation, pressure may instead reuse and seal an empty closed span;
 * failure requests space from maintenance. A sole unregistered executor
 * may recycle a chunk, moving survivors. false: no space or oversized code. */
bool t1_arena_place(struct t1_region *r, const struct t1_emitted *e);
/* Private background chunks are never shared with synchronous placement. */
bool t1_arena_place_background(struct t1_region *r, const struct t1_emitted *e, unsigned *chunk);
void t1_arena_seal_background(unsigned *chunk);
void t1_arena_close_background(unsigned *chunk);
/* Seals r's code RX before its first entry: the open part of r's chunk up
 * to the end of r's page (batch sealing). */
void t1_arena_seal_for_entry(struct t1_region *r);
/* Maintenance under code-arena pressure: evicts the cold regions of the
 * next chunk of the clock. */
bool t1_arena_recycle_one(void);
unsigned t1_arena_free_chunks(void);
/* At least this many contiguous pages, in a free chunk or closed span. */
bool t1_arena_has_space(unsigned pages);
/* Unlinks r's code from its chunk (reclamation). */
void t1_arena_release(struct t1_region *r);
#ifdef IA64VMM_HOSTED
/* Unit tests: a raw chunk of up to 16 pages for hand-built code. */
uint8_t *t1_arena_alloc(size_t pages);
void t1_arena_free(uint8_t *p, size_t pages);
#endif

/* stubs.c */
bool t1_stubs_build(uint8_t *page, size_t bytes);
/* Append the native edge stub; fallback is a shared-stub page offset. */
void t1_fast_emit(struct t1_x86_buf *b, size_t fallback);
/* Build or reuse a bounded lane after every entry guard, with no new validation
 * admission. NULL x.fast means the ordinary boundary bridge. */
void t1_fast_prepare(struct t1_vcpu *tv, struct t1_region *r);
#if IA64VMM_TIER1_STATS
void t1_fast_count_exit(struct t1_vcpu *tv);
#endif
/* Read a prior successful fetch guard for r's own entry address. Never
 * translate, walk, or change guest state while planning a future edge. */
bool t1_fetch_context_cached(const struct t1_vcpu *tv, const struct t1_region *r,
                             struct code_fetch *fetch);

/* region.c */
/* t1_init: the record table of `capacity` regions; false: no memory. */
bool t1_regions_init(unsigned capacity);
void t1_regions_shutdown(void);
/* Stopped executors, no record allocated: a table of n records instead;
 * false (the old table kept): records exist, n is out of range or no
 * memory. */
bool t1_regions_resize(unsigned n);
/* The smallest pool footprint of a region (a record page and a table
 * page), which sizes the default table from the pool. */
#define T1_REGION_MIN_BYTES (2u * 4096u)
struct t1_region *t1_region_new(void);
struct t1_region *t1_region_get(uint32_t id);
/* The link of a live or retired-unreclaimed record: (generation, id),
 * never 0 (tier1.h t1_upper_probe). Set when the record is allocated and
 * read without code_lock: its slot is reused only after reclamation. */
static inline uint64_t t1_region_link(const struct t1_region *r)
{
    return r->link;
}
/* The record a link names while it is valid, or NULL: retired, reclaimed
 * or its slot reused (the generation differs). Takes no lock; a returned
 * record stays allocated until the caller's next quiescent publication. */
struct t1_region *t1_region_from_link(uint64_t link);
/* The one contract compare: v's state, the compile flags
 * and the window width against r's contract. */
bool t1_region_matches(const struct t1_region *r, const struct vcpu *v, uint32_t flags,
                       unsigned bits);
/* Same contract, for an immutable exit's target instead of v's current IP. */
bool t1_region_matches_at(const struct t1_region *r, const struct vcpu *v, uint32_t flags,
                          unsigned bits, uint64_t ip, unsigned slot);
/* The next published region after `after` (NULL: the first) whose
 * contract matches v's state and options; the caller checks the entry PA
 * (the fetch guard), since one ip can map to different code. A retired
 * after ends iteration. */
struct t1_region *t1_region_lookup(const struct vcpu *v, const struct t1_compile_options *opt,
                                   unsigned hostwin_bits, struct t1_region *after);
/* Full contract and entry PA probe. For the calling executor's registered,
 * unparked vCPU, takes no lock; retain the result only until that executor's
 * next quiescent publication.
 * Other callers use code_lock and the single-executor lifetime rules. */
struct t1_region *t1_region_probe(const struct vcpu *v, const struct t1_compile_options *opt,
                                  unsigned hostwin_bits, gpa_t entry_pa);
/* Statistics: the PSR bits in which a live region at v's ip and slot
 * differs from v's state while every other contract component matches
 * (0: no region differs in PSR alone). */
uint64_t t1_region_psr_difference(const struct vcpu *v, uint32_t flags, unsigned hostwin_bits);
/* stats.c: the PSR bits of one event of a t1_psr_bits statistic. */
void t1_psr_note(enum t1_psr_count kind, uint64_t bits);
/* stats.c: one in-region helper call of `opcode` (t1_helper_calls). */
void t1_helper_call_note(unsigned opcode);
/* stats.c: a head rejected for good (t1_stats.rejected_heads). */
void t1_rejected_head_note(gpa_t entry_pa, uint64_t ip, unsigned slot);
/* Attaches dependencies and links; false (region discarded) if a page
 * changed meanwhile. */
bool t1_region_publish(struct t1_region *r);
/* Retirement by an invalidation (dependency callback, t1_flush), by the
 * arena's eviction, or by demotion: unlinked, valid =
 * false, reclaimed later; the counters differ. */
void t1_region_retire(struct t1_region *r);
void t1_region_evict(struct t1_region *r);
void t1_region_demote(struct t1_region *r);
void t1_region_discard(struct t1_region *r);
/* Maintenance, only where the caller holds no Tier1
 * pointer: reclaims retired records every registered executor has passed
 * (unregistered single-executor fixtures: outside region code), then
 * evicts cold regions until the bytes awaiting reclamation and the free
 * room cover the outstanding request, the free and awaiting ids a 64th of
 * the table, or a code chunk. */
void t1_reclaim(void);
/* Reclamation alone (allocation refusals). */
void t1_reclaim_passed(void);
/* Retired records or an outstanding request: maintenance has work. */
bool t1_reclaim_pending(void);
/* Maintenance should run now: an outstanding request, an unfinished scan, or retired records,
 * with maintenance posted to helpers T1_RECLAIM_POST of them (a post wakes
 * a helper; reclamation waits for a batch, as Tier0's does). */
#define T1_RECLAIM_POST 64u
bool t1_maintenance_wanted(void);
/* An outstanding request the current room does not cover: compiles defer
 * instead of failing at their allocations. 0: none; otherwise 1 + the
 * enum t1_defer_kind of the request (an id first, then a chunk, then
 * bytes). */
unsigned t1_memory_pressure(void);
/* Records allocated and the table's size (statistics, any thread). */
void t1_region_counts(uint64_t *records, uint64_t *table);
/* A refused region id or contiguous code-page span. Code_lock held. */
void t1_request_region_id(void);
void t1_request_code_chunk(unsigned pages);
void t1_code_chunk_satisfied(unsigned pages);
/* code_lock held: queued work or a caller outside the sole executor. */
bool t1_background_work(void);
/* code_lock held, from setup state: with registered executors or a
 * compile queue, live code stays in place and storage is reused only after
 * the retirement epoch; otherwise the arena may move live code. */
bool t1_epoch_reclaim(void);
/* Second-chance clock over published regions. */
bool t1_region_evict_one(void);
/* Moves on every publication and finished compile request (tier1.h). */
extern uint64_t t1_sequence;
/* The head's counter of tier1.h t1_upper_heads moved: after a publication
 * there, or a finished, cancelled or dropped request for it. */
void t1_head_moved(uint64_t ip, unsigned slot);
/* Maintenance has work (tier1.h t1_upper_reclaim_due): set by whoever
 * records it, cleared by t1_reclaim when none remains. */
void t1_maintenance_due(void);

/* The workspace one compile holds at once: its fixed-capacity arrays and
 * maximal emitter, MIR and allocator rewrite arrays. */
uint64_t t1_compile_workspace_bytes(void);
/* Reserved pipelines: each workspace, context and one maximal region. */
uint64_t t1_compile_reserve_bytes(void);
/* frame.c: region tables from the allocated MIR and the emitted code
 * (offsets, stub fixups); t1_frame_pages: their size, known before the
 * record exists. */
size_t t1_frame_pages(const struct t1_mir *m, const struct t1_emitted *e);
bool t1_frame_build(struct t1_region *r, const struct t1_mir *m, const struct t1_emitted *e);

/* ---- frame-relative stacked elements ---- */
#define T1_FRAME_IN_NAT 0x80u /* struct t1_region.frame_in: a NaT slot */
/* The v->s.gr / v->s.nat index of element register p (1..127) of a GR or
 * NaT element: a static p < 32 is itself; p = 32 + o names the stacked
 * register at frame offset o from frame base bof (a region's entry base) --
 * exactly ia64_gr_index's 32 + (rse_bof + o) % 96 for the rotated offset
 * o; t1_gr_home at v's current base. */
static inline unsigned t1_gr_home_at(unsigned bof, unsigned p)
{
    if (p < 32)
        return p;
    unsigned q = bof % IA64_N_STACKED_PHYS + (p - 32u);
    return 32u + (q >= IA64_N_STACKED_PHYS ? q - IA64_N_STACKED_PHYS : q);
}
static inline unsigned t1_gr_home(const struct ia64_state *s, unsigned p)
{
    return t1_gr_home_at(s->rse_bof, p);
}
/* frame.c: x->frame_in[k] = r's k-th stacked LIVEIN at v's frame base
 * (values as 64 bits, NaTs as 0/1). The dispatcher calls it right before
 * r->entry, after every guard and hook that may change v. */
void t1_frame_in_fill(struct t1_exec *x, const struct t1_region *r, const struct vcpu *v);
/* natguard.c: NaT guard, nat[] of every element of r's G is clear at
 * v's frame base; t1_nat_guard_list derives r->guard[] from r->nat_guard. */
bool t1_nat_guard_holds(const struct t1_region *r, const struct vcpu *v);
void t1_nat_guard_list(struct t1_region *r);

/* compile.c */
enum t1_compile_status t1_compile_at(struct t1_vcpu *tv, const struct t1_compile_options *opt,
                                     struct t1_compile_report *report);
void t1_compile_shutdown(void); /* frees the executor's compile context */
enum t1_compile_status t1_compile_hot(struct t1_vcpu *tv, const struct t1_compile_request *rq,
                                      struct t1_compile_report *report);

/* hot.c: the harness engine's hotness. */
void t1_hot_reset(struct t1_vcpu *tv);
/* After a lower-tier step from (ip, slot): counts a taken backward branch
 * by its target; true when the target became hot. */
bool t1_hot_note(struct t1_vcpu *tv, uint64_t before_ip, unsigned before_slot);
/* An uncompiled natural exit target, never a budget-resume position. */
bool t1_hot_trace(struct t1_vcpu *tv);
void t1_hot_failed(struct t1_vcpu *tv, bool tail);
/* hot.c: per-head holds and the demotion windows of
 * the product path. A head is (entry PA, ip, slot): every Tier0 region at
 * it, whatever its frame. */
bool t1_head_memory_helpers(gpa_t entry_pa, uint64_t ip, unsigned slot);
bool t1_hold_active(gpa_t entry_pa, uint64_t ip, unsigned slot);
void t1_hold_rejected(gpa_t entry_pa, uint64_t ip, unsigned slot);
void t1_holds_clear(void);
enum t1_demotion { T1_DEMOTE_PROFIT, T1_DEMOTE_NAT, T1_DEMOTE_SITE };
/* After an entry of a policy region that retired `retired` instructions
 * and left at point P, counted in w (the executor's); true: r was demoted
 * (retired, head held). */
bool t1_demote_after_exit(struct t1_windows *w, struct t1_region *r, const struct t1_point *P,
                          uint64_t retired);
/* After an entry attempt that reached the NaT guard. */
bool t1_demote_after_nat_guard(struct t1_windows *w, struct t1_region *r, bool failed);

#ifdef IA64VMM_HOSTED
/* region.c: probe after loading a record, publication before the bucket
 * store, retirement after its epoch stamp (with code_lock still held). */
enum t1_region_test_event { T1_REGION_TEST_PROBE, T1_REGION_TEST_PUBLISH, T1_REGION_TEST_RETIRED };
extern void (*t1_test_region_hook)(const struct t1_region *r, unsigned event);
/* Fixed-layout coherence fixtures select loop-only hotness temporarily. */
bool t1_test_set_tail_hotness(bool enabled);
/* dispatch.c: called on the entering thread between the publication of
 * the entry's region and its valid check. */
extern void (*t1_test_publish_hook)(struct vcpu *v, const struct t1_region *r);
/* dispatch.c: called at a chain point after EXIT of the region linked by
 * `from`, right after its quiescent publication (resolved false) and right
 * after the successor's link was resolved (resolved true). */
extern void (*t1_test_chain_hook)(struct vcpu *v, uint64_t from, bool resolved);
/* vhooks.c (verification; hosted builds only) */
void t1_vh_entry(struct t1_vcpu *tv, struct t1_region *r); /* arm x->force[], poison */
void t1_vh_exit(struct t1_vcpu *tv, struct t1_region *r);  /* occurrence counters */
uint64_t t1_vh_occurrence(struct t1_vcpu *tv, const struct t1_region *r, uint32_t point);
struct t1_arm *t1_vh_armed(struct t1_vcpu *tv, const struct t1_region *r, uint32_t point);
void t1_vh_record(struct t1_vcpu *tv, const struct t1_region *r, uint32_t point, bool materialized,
                  uint8_t outcome);
void t1_vh_corrupt(struct t1_vcpu *tv, const struct t1_region *r, uint32_t point);
void t1_vh_drain_stores(struct t1_vcpu *tv, const struct t1_region *r);
void t1_vh_entry_hook(struct t1_vcpu *tv, const struct t1_region *r);
void t1_vh_reset(struct t1_vcpu *tv);
bool t1_vh_store_overflow(const struct t1_vcpu *tv);
#endif
#if defined(IA64VMM_HOSTED) || defined(IA64VMM_TIER1_CHECKED)
/* checked.c: checked lockstep of regions compiled
 * with T1_COMPILE_UNDO_LOG. t1_ck_begin right before the execution (false:
 * no memory for the workspace); t1_ck_collect at every exit to C inside it
 * and at its end, before the store-log ring is drained; t1_ck_finish after
 * materialization with the exit point and retired count: false is a
 * divergence (logged). */
bool t1_ck_begin(struct t1_vcpu *tv, const struct t1_region *r);
void t1_ck_collect(struct t1_vcpu *tv);
bool t1_ck_finish(struct t1_vcpu *tv, const struct t1_point *P, uint64_t consumed);
/* Restore entry hooks after a terminal mapping stop, without replay. */
void t1_ck_cancel(struct t1_vcpu *tv);
void t1_ck_destroy(struct t1_vcpu *tv);
#endif

/* materialize.c: an element's home at v's frame base (stacked GR/NaT
 * through t1_gr_home). t1_home_read: false for an id that is no element.
 * The _at forms place stacked elements at frame base bof instead (a
 * region's entry frame base, struct t1_exec entry_bof). */
void t1_home_write(struct vcpu *v, uint16_t element, uint64_t value);
bool t1_home_read(const struct vcpu *v, uint16_t element, uint64_t *value);
void t1_home_write_at(struct vcpu *v, unsigned bof, uint16_t element, uint64_t value);
bool t1_home_read_at(const struct vcpu *v, unsigned bof, uint16_t element, uint64_t *value);
uint64_t t1_loc_value(const struct t1_exec *x, const struct t1_loc *l);

#ifdef IA64VMM_HOSTED
/* Test diagnostics: when set, compile.c prints the allocated MIR of every
 * region it compiles, and allocations use this register pool instead of
 * T1_ALLOCATABLE (0 = the full pool; spill-pressure tests). */
extern t1_ir_out_fn t1_test_mir_print;
extern uint16_t t1_test_alloc_pool;
/* isel.c: when set, every rule choice is also made by the full scan of
 * the rule table and must agree with the root-indexed one. */
extern bool t1_test_isel_crosscheck;
/* opt.c: called with the IR after each optimizer pass, before its check
 * (a test can break a pass). */
extern void (*t1_test_pass_hook)(struct t1_ir *ir, const char *pass);
#endif

/* stats.c */
/* Every thread counts into its own slot
 * (code_counter_index: host CPU, hosted thread); t1_get_stats sums them. */
#define T1_STAT_SLOTS 256u
extern struct t1_stats t1_stats_slots[T1_STAT_SLOTS];
#define T1_STATS (t1_stats_slots[code_counter_index()])
#if IA64VMM_TIER1_STATS
/* Logs each vCPU's most frequent region transitions. */
void t1_dump_transitions(void);
#endif
/* A slot has one writer; live readers (t1_get_stats) load it relaxed. */
static inline void t1_count_plain(uint64_t *counter, uint64_t n)
{
    __atomic_store_n(counter, __atomic_load_n(counter, __ATOMIC_RELAXED) + n, __ATOMIC_RELAXED);
}

static inline void t1_count_atomic(_Atomic uint64_t *counter, uint64_t n)
{
    atomic_store_explicit(counter, atomic_load_explicit(counter, memory_order_relaxed) + n,
                          memory_order_relaxed);
}

#define T1_COUNT_IN(slot, field, n)                                                                \
    do {                                                                                           \
        struct t1_stats *slot_ = (slot);                                                           \
        _Generic(&slot_->field, _Atomic uint64_t *: t1_count_atomic, default: t1_count_plain)(     \
            &slot_->field, (n));                                                                   \
    } while (0)
#define T1_COUNT(field, n) T1_COUNT_IN(&T1_STATS, field, n)
/* The slot of the executor thread of an upper entry or harness run,
 * looked up once at its start (struct t1_vcpu stats). */
#define T1_COUNT_TV(tv, field, n) T1_COUNT_IN((tv)->stats, field, n)
#endif
