/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Region entry/return handling for the t1_run harness and Tier0 upper hooks.
 * Enter only when the remaining budget, timer/slice limits and store-log room
 * cover max_path; v->retired must remain exact on every exit.
 *
 * On return, drain the store log, materialize the frame, apply wrong-old-value
 * corruption and call the restore observer, in that order, before lower-tier
 * execution or event delivery. A before-effect exit executes its instruction
 * exactly once in the lower tier: a t1_run step or Tier0's RAN_PENDING skip.
 */
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"
#include "engine/hostwin/hostwin.h"
#include "engine/interp/interp.h"
#include "ia64/pmu.h"
#include "engine/cache/validity.h"
#include "engine/cache/coherence-inline.h"
#include "ia64/fault-count.h"
#include "ia64/interrupt.h"
#include "ia64/mmu.h"
#include "ia64/tlb.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

struct t1_config t1_cfg;
bool t1_ready;
#ifdef IA64VMM_HOSTED
static uint32_t init_loop_threshold, init_trace_threshold; /* t1_test_set_thresholds */
#endif

bool t1_init(const struct t1_config *cfg)
{
    if (t1_ready)
        return true;
    t1_cfg = cfg ? *cfg : (struct t1_config){0};
    if (!t1_cfg.code_bytes)
        t1_cfg.code_bytes = UINT64_C(8) << 20;
    if (!t1_cfg.wallclock_slice)
        t1_cfg.wallclock_slice = 16384;
    if (!t1_cfg.lower)
        t1_cfg.lower = interp_run;
    /* Product-path defaults. */
    if (!t1_cfg.loop_threshold)
        t1_cfg.loop_threshold = 64;
    if (!t1_cfg.trace_threshold)
        t1_cfg.trace_threshold = 1024;
    if (!t1_cfg.min_insns_per_entry)
        t1_cfg.min_insns_per_entry = T1_MIN_INSNS_DEFAULT;
    if (!t1_cfg.max_regions)
        t1_cfg.max_regions = T1_MAX_REGIONS_DEFAULT;
    if (t1_cfg.code_bytes % (UINT64_C(1) << 20) || t1_cfg.code_bytes > (UINT64_C(1024) << 20))
        return false;
    if (t1_cfg.max_regions > T1_MAX_REGIONS_LIMIT)
        return false;
    t1_memory_reset();
    if (!t1_regions_init(t1_cfg.max_regions))
        return false;
    if (!t1_arena_init(t1_cfg.code_bytes)) {
        t1_regions_shutdown();
        return false;
    }
    uint8_t *stubs = t1_arena_stub_page();
    if (!t1_stubs_build(stubs, 4096)) {
        t1_arena_shutdown();
        t1_regions_shutdown();
        return false;
    }
    hal_code_set_executable(stubs, 1);
    t1_holds_clear();
#ifdef IA64VMM_HOSTED
    init_loop_threshold = t1_cfg.loop_threshold;
    init_trace_threshold = t1_cfg.trace_threshold;
#endif
    t1_ready = true;
    return true;
}

void t1_shutdown(void)
{
    if (!t1_ready)
        return;
    t1_flush();
    t1_compile_shutdown();
    t1_arena_shutdown();
    t1_regions_shutdown();
    t1_ready = false;
}

struct t1_vcpu *t1_vcpus[T1_VCPU_SLOTS];

struct t1_vcpu *t1_vcpu_create(struct vcpu *v, struct hostwin_vcpu *hw)
{
    if (!v || !hw)
        return NULL;
    unsigned slot = 0;
    while (slot < T1_VCPU_SLOTS && t1_vcpus[slot])
        ++slot;
    if (slot == T1_VCPU_SLOTS)
        return NULL;
    struct t1_vcpu *tv = hal_alloc_pages((sizeof(*tv) + 4095) / 4096, HAL_PAGE_ZERO);
    if (!tv)
        return NULL;
    memset(tv, 0, sizeof(*tv));
    tv->v = v;
    tv->hw = hw;
    tv->x.v = v;
    tv->x.tv = tv;
    tv->x.hw = hw;
    tv->stale_seq = UINT64_MAX;
    for (unsigned k = 0; k < T1_SITE_SLOTS; ++k)
        tv->x.site[k].tag = HOSTWIN_TAG_INVALID;
    __atomic_store_n(&t1_vcpus[slot], tv, __ATOMIC_RELEASE);
    return tv;
}

void t1_vcpu_destroy(struct t1_vcpu *tv)
{
    if (!tv)
        return;
    for (unsigned slot = 0; slot < T1_VCPU_SLOTS; ++slot)
        if (t1_vcpus[slot] == tv)
            __atomic_store_n(&t1_vcpus[slot], NULL, __ATOMIC_RELEASE);
#ifdef IA64VMM_HOSTED
    t1_vh_reset(tv);
#endif
#if defined(IA64VMM_HOSTED) || defined(IA64VMM_TIER1_CHECKED)
    t1_ck_destroy(tv);
#endif
    hal_free_pages(tv, (sizeof(*tv) + 4095) / 4096);
}

#ifdef IA64VMM_HOSTED
void (*t1_test_publish_hook)(struct vcpu *v, const struct t1_region *r);
void (*t1_test_chain_hook)(struct vcpu *v, uint64_t from, bool resolved);
#endif

/* The entry publishes r and its count,
 * then checks valid; a retirement stores valid, then reads the count, r and
 * the count (t1_retired_running). The count is the entry's one sequentially
 * consistent store, after r (release): either the entry's valid check sees
 * the retirement, or the retirement's first read of the count follows that
 * store, synchronizes with it and so finds r. false: retired, nothing
 * published. */
static bool publish_running(struct t1_vcpu *tv, const struct t1_region *r)
{
    atomic_store_explicit(&tv->running, r, memory_order_release);
    atomic_store_explicit(&tv->entry_seq,
                          atomic_load_explicit(&tv->entry_seq, memory_order_relaxed) + 1,
                          memory_order_seq_cst);
#ifdef IA64VMM_HOSTED
    if (t1_test_publish_hook)
        t1_test_publish_hook(tv->v, r);
#endif
    if (r->valid)
        return true;
    atomic_store_explicit(&tv->running, NULL, memory_order_release);
    return false;
}

static void clear_running(struct t1_vcpu *tv)
{
    atomic_store_explicit(&tv->running, NULL, memory_order_release);
}

/* The entry count is read before and after the running region: an entry
 * that began in between published after this retirement and finds it by
 * its own valid check, so only an entry that ran r throughout both
 * reads is marked and kicked. */
void t1_retired_running(const struct t1_region *r)
{
    const struct t1_exec *self = t1_current();
    for (unsigned slot = 0; slot < T1_VCPU_SLOTS; ++slot) {
        struct t1_vcpu *tv = __atomic_load_n(&t1_vcpus[slot], __ATOMIC_ACQUIRE);
        if (!tv)
            continue;
        uint64_t seq = atomic_load_explicit(&tv->entry_seq, memory_order_seq_cst);
        if (atomic_load_explicit(&tv->running, memory_order_seq_cst) != r ||
            atomic_load_explicit(&tv->entry_seq, memory_order_seq_cst) != seq)
            continue;
        atomic_store_explicit(&tv->stale_seq, seq, memory_order_seq_cst);
        if (!self || self->tv != tv)
            vcpu_kick(tv->v);
    }
}

#ifdef IA64VMM_HOSTED
bool t1_test_running(const struct vcpu *v)
{
    for (unsigned slot = 0; slot < T1_VCPU_SLOTS; ++slot) {
        struct t1_vcpu *tv = __atomic_load_n(&t1_vcpus[slot], __ATOMIC_ACQUIRE);
        if (tv && tv->v == v && atomic_load_explicit(&tv->running, memory_order_seq_cst))
            return true;
    }
    return false;
}
#endif

/* The entries of other vCPUs whose mark
 * equals their count, i.e. that run a region retired before the sample.
 * Each was kicked by that retirement and leaves at its next checkpoint. */
unsigned t1_sync_i_sample(const struct vcpu *v, struct t1_sync_wait *w)
{
    w->n = 0;
    for (unsigned slot = 0; slot < T1_VCPU_SLOTS; ++slot) {
        struct t1_vcpu *tv = __atomic_load_n(&t1_vcpus[slot], __ATOMIC_ACQUIRE);
        if (!tv || tv->v == v)
            continue;
        uint64_t s = atomic_load_explicit(&tv->entry_seq, memory_order_seq_cst);
        const struct t1_region *r = atomic_load_explicit(&tv->running, memory_order_seq_cst);
        if (!r || atomic_load_explicit(&tv->stale_seq, memory_order_seq_cst) != s)
            continue;
        w->held[w->n].tv = tv;
        w->held[w->n].region = r;
        w->held[w->n++].seq = s;
    }
    if (w->n)
        T1_COUNT(sync_waits, 1);
    return w->n;
}

bool t1_sync_i_held(const struct t1_sync_wait *w, unsigned i)
{
    const struct t1_vcpu *tv = w->held[i].tv;
    return atomic_load_explicit(&tv->running, memory_order_acquire) == w->held[i].region &&
           atomic_load_explicit(&tv->entry_seq, memory_order_acquire) == w->held[i].seq;
}

struct vcpu *t1_sync_i_vcpu(const struct t1_sync_wait *w, unsigned i)
{
    return w->held[i].tv->v;
}

void t1_vcpu_state_restored(struct t1_vcpu *tv)
{
    tv->x.fast = NULL;
    tv->fast.nnodes = 0;
    memset(tv->fast.connections, 0, sizeof(tv->fast.connections));
    __atomic_store_n(&tv->v->irq_kick, 0, __ATOMIC_RELEASE);
    t1_hot_reset(tv);
    for (unsigned k = 0; k < T1_SITE_SLOTS; ++k)
        tv->x.site[k].tag = HOSTWIN_TAG_INVALID;
    tv->x.observed_epoch = 0;
    /* The restored mmu_seq may repeat a value seen with other contents. */
    hostwin_resync(tv->hw);
    memset(tv->fetch_ok, 0, sizeof(tv->fetch_ok));
    /* The mark compares v's retired count, IP and slot, which a restore
     * may repeat. */
    tv->last_exit.valid = false;
    memset(tv->successors, 0, sizeof(tv->successors));
#ifdef IA64VMM_HOSTED
    t1_vh_reset(tv);
#endif
}

/* The kick word is the vCPU's irq_kick which the event checkpoint
 * reads together with requested_exit (exec.h T1_VCPU_CKPT). */
void t1_kick(struct t1_vcpu *tv)
{
    vcpu_kick(tv->v);
}

unsigned t1_exit_count(const struct t1_vcpu *tv)
{
    return tv->nexits;
}

bool t1_exit_get(const struct t1_vcpu *tv, unsigned index, struct t1_exit_info *out)
{
    if (index >= tv->nexits || index >= T1_EXIT_LOG)
        return false;
    *out = tv->exits[index];
    return true;
}

enum t1_compile_status t1_compile(struct t1_vcpu *tv, const struct t1_compile_options *opt,
                                  struct t1_compile_report *report)
{
    static const struct t1_compile_options none;
    return t1_compile_at(tv, opt ? opt : &none, report);
}

#ifdef IA64VMM_HOSTED
void t1_test_set_compile_options(struct t1_vcpu *tv, const struct t1_compile_options *opt)
{
    tv->opt = opt ? *opt : (struct t1_compile_options){0};
}

void t1_test_set_thresholds(uint32_t loop_threshold, uint32_t trace_threshold)
{
    t1_cfg.loop_threshold = loop_threshold ? loop_threshold : init_loop_threshold;
    t1_cfg.trace_threshold = trace_threshold ? trace_threshold : init_trace_threshold;
}
#endif

/*
 * Decline entry, lookup and compilation while CFLE or rse_missing indicates
 * an incomplete frame (SDM Vol.2 §6.8; ia64/rse.c). Lower-tier fetch resumes
 * mandatory loads, with their interrupt/deferred-trap checks. An interruption
 * can clear CFLE while leaving rse_missing until cover. In-region calls,
 * matched returns and allocs keep frames complete, so entry checks suffice.
 */
static bool rse_incomplete(const struct vcpu *v)
{
    return v->s.rse_cfle || v->s.rse_missing;
}

/* Unknown-frame returns have zero locals: only outputs can overlap the
 * entry dirty partition (SDM Vol.2 §6.5.5). Alloc may reduce that partition;
 * matched calls/returns restore it, so the entry bound suffices. */
static bool return_guard(const struct vcpu *v, const struct t1_region *r)
{
    return !r->observed_returns || v->s.rse_dirty <= IA64_N_STACKED_PHYS - r->return_sof;
}

/* Entry preconditions (the mirror of Tier0's needs_interpreter). */
static bool needs_lower(const struct vcpu *v)
{
    if (ia64_pmu_active(v))
        return true;
    if ((v->s.psr & (IA64_PSR_BE | IA64_PSR_IS | T1_PSR_LOWER_TIER)) || v->s.slot > 2)
        return true;
    if (rse_incomplete(v))
        return true;
    if (__atomic_load_n(&v->requested_exit, __ATOMIC_ACQUIRE) || ia64_external_pending(v))
        return true;
    uint64_t deadline = vcpu_next_timer_deadline(v);
    return deadline != UINT64_MAX && deadline <= hal_counter();
}

/* Guard: the fetch translation of the source pages from `first`
 * on, without a walk. Never with CFLE set: the fetch translation would
 * perform the mandatory RSE loads (callers check rse_incomplete first).
 * Every caller checks every page, the entry page against the entry PA. */
static bool fetch_guard(struct vcpu *v, const struct t1_region *r, unsigned first)
{
    if (rse_incomplete(v))
        return false;
    if (!(v->s.psr & IA64_PSR_IT))
        return true;
    unsigned cpl = (unsigned)((v->s.psr >> 32) & 3);
    for (unsigned p = first; p < r->npages; ++p) {
        uint64_t ip = p ? r->page[p].ip_base : v->s.ip;
        if (!ia64_tlb_lookup(v, ip, 0))
            return false;
        struct ia64_translation t;
        struct ia64_result res =
            ia64_translate(v, ip, IA64_REF_FETCH, IA64_ACCESS_EXECUTE, cpl, false, &t);
        if (res.kind != IA64_STEP_COMPLETE)
            return false;
        gpa_t want = p ? r->page[p].pa : r->c.entry_pa;
        if ((p ? (t.pa & ~UINT64_C(4095)) : t.pa) != want)
            return false;
    }
    return true;
}

/*
 * Cache I-TLB-hit fetch guards (no walks) by region id/generation,
 * mmu_iseq and T1_FETCH_PSR. I-side TR/TC, RR and PKR writers advance
 * mmu_iseq; D-side-only changes cannot affect these hits. Sample before
 * guarding. Live links retain their pages; slot reuse changes generation.
 * Both callers decline CFLE/rse_missing first. Tier0 lookup establishes
 * entry PA; chain points must certify it. Physical mode needs no guard.
 * State restore clears the cache because sequences may repeat.
 */
#define T1_FETCH_PSR (IA64_PSR_IT | IA64_PSR_IC | IA64_PSR_PK | IA64_PSR_IA | (UINT64_C(3) << 32))
/* Cache the entry PTE for entry_fetch too: fixed-address I-TLB lookup
 * reads only I-side TR/TC and RR[ip >> 61], so it is stable with mmu_iseq
 * while the link keeps r's ip fixed. fetch_guards / fetch_cache_hits count
 * only multi-page regions, where the cache avoids guarding other pages. */
static bool fetch_guard_cached(struct t1_vcpu *tv, struct vcpu *v, const struct t1_region *r,
                               uint64_t *entry_pte)
{
    *entry_pte = 0;
    if (!(v->s.psr & IA64_PSR_IT))
        return true;
    uint64_t link = t1_region_link(r), seq = v->mmu_iseq, psr = v->s.psr & T1_FETCH_PSR;
    struct t1_fetch_ok *e = &tv->fetch_ok[r->id % T1_FETCH_CACHE];
    if (e->link == link && e->mmu_iseq == seq && e->psr == psr) {
        if (r->npages > 1)
            T1_COUNT_TV(tv, fetch_cache_hits, 1);
        *entry_pte = e->entry_pte;
        return true;
    }
    /* At a Tier0 boundary its fetch translation found or inserted (a walk)
     * the entry: a miss is not reachable there. At a chain point a miss
     * returns to Tier0, which walks. */
    const struct ia64_tlb_entry *t = ia64_tlb_lookup(v, v->s.ip, 0);
    if (!t)
        return false;
    uint64_t pte = t->pte;
    if (r->npages > 1)
        T1_COUNT_TV(tv, fetch_guards, 1);
    if (!fetch_guard(v, r, 0))
        return false;
    *e = (struct t1_fetch_ok){link, seq, psr, pte};
    *entry_pte = pte;
    return true;
}

/*
 * Match Tier0's code_fetch context (key PSR, RR, PTA and entry I-TLB PTE;
 * zero PTE in physical mode). Runtime PSR bits are ignored by validity stays.
 * Matching keeps the stay across tier transitions without rechecking bundles.
 */
static struct code_fetch entry_fetch(const struct vcpu *v, uint64_t pte)
{
    bool virt = (v->s.psr & IA64_PSR_IT) != 0;
    return (struct code_fetch){.psr = v->s.psr,
                               .rr = virt ? v->s.rr[v->s.ip >> 61] : 0,
                               .pta = virt ? v->s.cr[CR_PTA] : 0,
                               .pte = virt ? pte : 0};
}

bool t1_fetch_context_cached(const struct t1_vcpu *tv, const struct t1_region *r,
                             struct code_fetch *fetch)
{
    const struct vcpu *v = tv->v;
    *fetch = (struct code_fetch){.psr = v->s.psr};
    if (!(v->s.psr & IA64_PSR_IT))
        return true;
    const struct t1_fetch_ok *e = &tv->fetch_ok[r->id % T1_FETCH_CACHE];
    if (e->link != r->link || e->mmu_iseq != v->mmu_iseq || e->psr != (v->s.psr & T1_FETCH_PSR))
        return false;
    fetch->rr = v->s.rr[r->c.ip >> 61];
    fetch->pta = v->s.cr[CR_PTA];
    fetch->pte = e->entry_pte;
    return true;
}

/*
 * Continue v's validation stay across all source pages and compare source
 * bundles not yet checked in that stay. A mismatch retires overlapping
 * dependents, including r. Defensively retire r on any failure so entry cannot
 * decline forever even if the failing validation did not retire it.
 */
static bool validate_entry(struct vcpu *v, struct t1_region *r, const struct code_fetch *fetch)
{
    bool ok = code_dependencies_enter(v, r->dep, r->npages, fetch);
    if (!ok && r->valid)
        t1_region_retire(r);
    return ok;
}

/* The region budget of an entry: the caller's, limited by the wall-clock
 * slice (slice_budget is the budget T a chain shares) and, in store-log builds, by the ring (ring_budget: at most one
 * store per retired instruction, so the ring never overflows between two
 * drains). */
static uint64_t slice_budget(const struct vcpu *v, uint64_t budget)
{
    return !v->cfg.itc_icount && budget > t1_cfg.wallclock_slice ? t1_cfg.wallclock_slice : budget;
}
static uint64_t ring_budget(const struct t1_region *r, uint64_t budget)
{
    return (r->c.flags & T1_COMPILE_STORE_LOG) && budget > T1_SLOG_ENTRIES ? T1_SLOG_ENTRIES
                                                                           : budget;
}
static uint64_t entry_budget(const struct vcpu *v, const struct t1_region *r, uint64_t budget)
{
    return ring_budget(r, slice_budget(v, budget));
}

static void log_exit(struct t1_vcpu *tv, const struct t1_region *r, uint32_t point,
                     uint64_t consumed)
{
    if (tv->nexits < T1_EXIT_LOG)
        tv->exits[tv->nexits] = (struct t1_exit_info){
            r->id, point, (enum t1_point_kind)r->point[point].kind, consumed, false};
    ++tv->nexits;
}

/* The region to enter at v's state: contract match and the fetch
 * translation of every source page. */
static struct t1_region *find(struct t1_vcpu *tv)
{
    struct vcpu *v = tv->v;
    unsigned bits = hostwin_bits(tv->hw);
    for (struct t1_region *r = t1_region_lookup(v, &tv->opt, bits, NULL); r;
         r = t1_region_lookup(v, &tv->opt, bits, r))
        if (return_guard(v, r) && fetch_guard(v, r, 0))
            return r;
    return NULL;
}

/* One lower-tier step, accounted by the retired delta. */
static bool lower_step(struct t1_vcpu *tv, uint64_t *budget, struct vcpu_exit *out)
{
    struct vcpu *v = tv->v;
    uint64_t before = v->retired, ip = v->s.ip;
    unsigned slot = v->s.slot;
    *out = t1_cfg.lower(v, 1);
    uint64_t done = v->retired - before;
    *budget -= done < *budget ? done : *budget;
    T1_COUNT(lower_steps, 1);
    T1_COUNT(lower_retired, done);
    if (out->reason != VCPU_EXIT_BUDGET)
        return false;
    /* After a br.ret or rfi the frame may be incomplete: no lookup (its fetch
     * guard would perform the mandatory loads) and no compile here. */
    if (t1_hot_note(tv, ip, slot) && !rse_incomplete(v) && !find(tv)) {
        /* A deferral (T1_NO_MEMORY) adds no backoff. */
        enum t1_compile_status st = t1_compile_at(tv, &tv->opt, NULL);
        if (st != T1_COMPILED && st != T1_NO_MEMORY)
            t1_hot_failed(tv, false);
    }
    return true;
}

/*
 * Prepare r's entry context. Cached site tags are valid for
 * x->observed_epoch: preserve them across entries only while the bind epoch
 * is unchanged, otherwise invalidate all regions' tags. t1_window_miss uses
 * the same rule. Each region has a separate site range. Verification regions
 * reset their sites on entry for deterministic MISS_EXIT occurrence counts.
 */
static void prepare_entry(struct t1_vcpu *tv, struct t1_region *r, uint64_t region_budget)
{
    struct t1_exec *x = &tv->x;
    struct vcpu *v = tv->v;
    x->region = r;
    x->budget_entry = region_budget;
    x->retired_entry = v->retired;
    x->entry_bof = v->s.rse_bof % IA64_N_STACKED_PHYS;
    x->entry_dirty = v->s.rse_dirty;
    x->bsp_called = 0;
    x->helper_exit = 0;
    x->entry_bsp = v->s.ar[AR_BSP];
    x->entry_cfm = v->s.cfm;
    x->point = 0;
    x->helper_notification = 0;
    uint64_t epoch = hostwin_bind_epoch(tv->hw);
    if (x->observed_epoch != epoch)
        for (unsigned k = 0; k < T1_SITE_SLOTS; ++k)
            x->site[k].tag = HOSTWIN_TAG_INVALID;
    else if (r->c.flags & T1_COMPILE_VERIFY)
        for (unsigned k = 0; k < r->nsites && r->site_base + k < T1_SITE_SLOTS; ++k)
            x->site[r->site_base + k].tag = HOSTWIN_TAG_INVALID;
    x->observed_epoch = epoch;
    tv->pending = (struct t1_pending){T1V_PATH_EXIT_STUB, false, false, false, 0, 0};
#ifdef IA64VMM_HOSTED
    t1_vh_entry(tv, r);
#elif defined(IA64VMM_TIER1_CHECKED)
    if (r->c.flags & T1_COMPILE_STORE_LOG)
        x->slog_n = x->slog_total = 0;
#endif
    if (!r->sealed)
        t1_arena_seal_for_entry(r);
    /* A recycling hint, independent of the guards that permit execution;
     * stored only when clear. */
    if (!atomic_load_explicit(&r->referenced, memory_order_relaxed))
        atomic_store_explicit(&r->referenced, true, memory_order_relaxed);
    t1_hint_add64(&r->entries, 1);
    T1_COUNT_TV(tv, entries, 1);
    T1_COUNT_TV(tv, moved_entries, r->moved);
    /* Entry image of the stacked LIVEINs at v's frame base,
     * after every guard and hook that may change v. */
    t1_frame_in_fill(x, r, v);
}

static uint32_t enter(struct t1_vcpu *tv, struct t1_region *r, uint64_t region_budget)
{
    struct t1_exec *x = &tv->x;
    prepare_entry(tv, r, region_budget);
    if (x->handoff)
        t1_fast_prepare(tv, r);
    else
        x->fast = NULL;
    t1_set_current(x);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    r->entry(tv->v, region_budget, x);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    t1_set_current(NULL);
    clear_running(tv);
    return x->point;
}

/* Process a captured exit through the restore observer;
 * *consumed: the point's retired count. */
static const struct t1_point *finish_execute(struct t1_vcpu *tv, struct t1_region *r,
                                             uint64_t *consumed)
{
    struct t1_exec *x = &tv->x;
    uint32_t point = x->point;
    if (point >= r->npoints || r->point[point].kind == T1_DEAD_POINT)
        hal_panic("tier1: generated code returned an invalid point");
    const struct t1_point *P = &r->point[point];
    *consumed = t1_point_consumed(x, P);
    T1_COUNT_TV(tv, exits_by_kind[P->kind & 3], 1);
    if (P->kind == T1_PT_EXIT)
        T1_COUNT_TV(tv, exits_by_end[P->end], 1);
#if defined(IA64VMM_HOSTED) || defined(IA64VMM_TIER1_CHECKED)
    if (r->c.flags & T1_COMPILE_UNDO_LOG)
        t1_ck_collect(tv);
#endif
#ifdef IA64VMM_HOSTED
    bool verify = (r->c.flags & T1_COMPILE_VERIFY) != 0;
    if (r->c.flags & T1_COMPILE_STORE_LOG) {
        if (t1_vh_store_overflow(tv))
            hal_panic("tier1: store log ring overflow");
        t1_vh_drain_stores(tv, r);
    }
    if (verify && P->kind == T1_PT_HELPER && tv->pending.path == T1V_PATH_EXIT_STUB) {
        /* Only the forced helper exit leaves a helper point through the
         * exit stub; its before callback runs first. */
        struct t1_arm *a = t1_vh_armed(tv, r, point);
        tv->pending = (struct t1_pending){T1V_PATH_HELPER, true, false, false, 0, 0};
        if (a) {
            a->fired = true;
            if (a->before)
                a->before(tv->v, a->arg);
        }
    }
#endif
    t1_materialize(x, P);
#ifdef IA64VMM_HOSTED
    if (verify) {
        t1_vh_corrupt(tv, r, point);
        t1_vh_record(tv, r, point, true, T1V_EXIT);
        t1_vh_exit(tv, r);
    }
#endif
    T1_COUNT_TV(tv, tier1_retired, *consumed);
    if (IA64VMM_TIER1_STATS)
        t1_hint_add64(&r->retired_insns, *consumed);
    log_exit(tv, r, point, *consumed);
    return P;
}

static const struct t1_point *execute(struct t1_vcpu *tv, struct t1_region *r,
                                      uint64_t region_budget, uint64_t *consumed)
{
    enter(tv, r, region_budget);
    return finish_execute(tv, r, consumed);
}

/* Notifications finish the instruction, but name its original site in
 * the exit record, as interp_run and Tier0's inline_requested do. */
static bool helper_notification(struct t1_vcpu *tv, struct vcpu_exit *out)
{
    const struct t1_exec *x = &tv->x;
    if (!x->helper_notification)
        return false;
    unsigned requested = __atomic_exchange_n(&tv->v->requested_exit, 0, __ATOMIC_ACQ_REL);
    if (!requested)
        return false;
    const struct t1_point *p = &x->region->point[x->helper_notification - 1];
    *out = (struct vcpu_exit){requested - 1, p->ip, p->slot, 0, "requested exit"};
    return true;
}

static struct vcpu_exit run(struct t1_vcpu *tv, uint64_t budget)
{
    struct vcpu *v = tv->v;
    tv->stats = &T1_STATS;
    struct vcpu_exit out;
    bool trace_head = false;
    while (budget) {
        t1_reclaim();
        ia64_timer_poll(v);
        if (needs_lower(v)) {
            trace_head = false;
            if (!lower_step(tv, &budget, &out))
                return out;
            continue;
        }
        struct t1_region *r = find(tv);
        if (!r) {
            /* Back-edge hotness alone misses tails reached only after a
             * region returns. Count those boundaries before stepping, so
             * frequently executed tails can stay in generated code too. */
            if (trace_head && t1_hot_trace(tv)) {
                enum t1_compile_status st = t1_trace_head_allowed(v, &tv->opt)
                                                ? t1_compile_at(tv, &tv->opt, NULL)
                                                : T1_REJECTED;
                if (st == T1_COMPILED) {
                    trace_head = false;
                    continue;
                }
                if (st != T1_NO_MEMORY)
                    t1_hot_failed(tv, true);
            }
            trace_head = false;
            if (!lower_step(tv, &budget, &out))
                return out;
            continue;
        }
        trace_head = false;
        uint64_t region_budget = budget;
        if (v->cfg.itc_icount && v->timer_armed && !v->timer_fired) {
            uint64_t delta = v->s.cr[CR_ITM] - ia64_itc_read_unhooked(v);
            if (delta && delta < region_budget)
                region_budget = delta;
        }
        region_budget = entry_budget(v, r, region_budget);
        /* Guards, budget last. */
        if (region_budget < r->max_path) {
            T1_COUNT(entry_guard_fails, 1);
            if (!lower_step(tv, &budget, &out))
                return out;
            continue;
        }
        if (!t1_nat_guard_holds(r, v)) {
            T1_COUNT(nat_guard_fails, 1);
            if (!lower_step(tv, &budget, &out))
                return out;
            continue;
        }
        /* find's fetch guard translated the entry page without a
         * walk, so its I-TLB entry exists; the harness engine looks it up
         * at every entry (Tier1 is its own outer loop: no Tier0 lookup
         * began the stay here, the lower tier's per-bundle entries do not
         * compare a fetch context). */
        const struct ia64_tlb_entry *it =
            (v->s.psr & IA64_PSR_IT) ? ia64_tlb_lookup(v, v->s.ip, 0) : NULL;
        const struct code_fetch fetch = entry_fetch(v, it ? it->pte : 0);
        if (((v->s.psr & IA64_PSR_IT) && !it) || !validate_entry(v, r, &fetch)) {
            /* A changed bundle retired the region; look up again (a region
             * still valid here cannot be entered: step instead). */
            T1_COUNT(entry_guard_fails, 1);
            if (r->valid) {
                if (!lower_step(tv, &budget, &out))
                    return out;
            }
            continue;
        }
#ifdef IA64VMM_HOSTED
        if (r->c.flags & T1_COMPILE_VERIFY)
            t1_vh_entry_hook(tv, r);
#endif
        hostwin_enter(tv->hw, v);
        if (!publish_running(tv, r))
            continue;
        uint64_t consumed;
        const struct t1_point *P = execute(tv, r, region_budget, &consumed);
        if (helper_notification(tv, &out))
            return out;
        if (hostwin_stop_detail(tv->hw))
            return (struct vcpu_exit){VCPU_EXIT_BAD_ACCESS, v->s.ip, v->s.slot, 0,
                                      hostwin_stop_detail(tv->hw)};
        budget -= consumed < budget ? consumed : budget;
        trace_head = P->kind == T1_PT_EXIT;
        /* The harness engine is its own outer loop: it consumes the kick
         * the checkpoint left at (in the product, Tier0's loop top does). */
        if (P->kind == T1_PT_CHECKPOINT)
            __atomic_exchange_n(&v->irq_kick, 0, __ATOMIC_ACQ_REL);
        /* An EXIT guard can fail before the region's first retirement. */
        if ((P->kind == T1_PT_SITE || P->kind == T1_PT_HELPER ||
             (P->kind == T1_PT_EXIT && !consumed)) &&
            budget) {
            if (tv->nexits <= T1_EXIT_LOG)
                tv->exits[tv->nexits - 1].lower_tier_step_followed = true;
            uint64_t before = v->retired;
            out = t1_cfg.lower(v, 1);
            uint64_t done = v->retired - before;
            budget -= done < budget ? done : budget;
            T1_COUNT(lower_steps, 1);
            T1_COUNT(lower_retired, done);
            if (out.reason != VCPU_EXIT_BUDGET)
                return out;
        }
    }
    return (struct vcpu_exit){VCPU_EXIT_BUDGET, v->s.ip, v->s.slot, 0, NULL};
}

struct vcpu_exit t1_run(struct t1_vcpu *tv, uint64_t budget)
{
    tv->nexits = 0;
    struct vcpu_exit out = run(tv, budget);
    /* As tier0_run: a non-budget exit starts a new page-entry interval. */
    if (out.reason != VCPU_EXIT_BUDGET)
        tv->v->code_entry_generation = 0;
    return out;
}

/* ---- the product path: Tier1 as Tier0's upper tier (tier0.h) ------------------------------------------------ */

/* Region `to` runs right after EXIT point `point` of the
 * region linked by `from`, with no instruction retired in between. The
 * transition table is kept with the statistics gate on. */
static inline void count_chainable(struct t1_vcpu *tv, uint64_t from, uint32_t point, uint64_t to)
{
    T1_COUNT_TV(tv, chainable, 1);
#if IA64VMM_TIER1_STATS
    unsigned h =
        (unsigned)(((from ^ ((uint64_t)point << 40)) * UINT64_C(0x9e3779b97f4a7c15)) >> 56);
    for (unsigned k = 0; k < T1_TRANSITIONS; ++k) {
        struct t1_transition *t = &tv->transitions[(h + k) % T1_TRANSITIONS];
        if (!t->count) {
            *t = (struct t1_transition){from, to, 1, point};
            return;
        }
        if (t->from == from && t->point == point && t->to == to) {
            ++t->count;
            return;
        }
    }
#else
    (void)tv;
    (void)from;
    (void)point;
    (void)to;
#endif
}

/* An upper entry that ran n >= 1 regions: bucket 0 for 1, k for
 * 2^(k-1) < n <= 2^k, the last one for more. Statistics gate on. */
static inline void count_length(struct t1_vcpu *tv, uint64_t n)
{
    if (!IA64VMM_TIER1_STATS)
        return;
    unsigned k = n <= 1 ? 0 : 64u - (unsigned)__builtin_clzll(n - 1);
    T1_COUNT_TV(tv, chain_lengths[k < T1_CHAIN_LENGTHS ? k : T1_CHAIN_LENGTHS - 1], 1);
}

/* The live region matching v's state at entry_pa (the contract compare of
 * region.c and the entry PA), or NULL. */
static struct t1_region *probe(struct t1_vcpu *tv, const struct vcpu *v, gpa_t entry_pa)
{
    return t1_region_probe(v, &tv->opt, hostwin_bits(tv->hw), entry_pa);
}

/* Tier0's key has `tracing` and `checking`: a region is linked only while
 * tracing is off (the entry contract requires tracing == false), and under
 * ENGINE_LOCKSTEP only to regions compiled for checking:
 * the vCPU's compile options then carry T1_COMPILE_UNDO_LOG (set when
 * Tier1 attaches to a lockstep vCPU, glue_vcpu_create), which the contract
 * compares, so probes find and hot() compiles checked regions only. */
static bool linkable(const struct vcpu *v)
{
    if (v->trace_count)
        return false;
    if (v->cfg.engine != ENGINE_LOCKSTEP)
        return true;
#if defined(IA64VMM_HOSTED) || defined(IA64VMM_TIER1_CHECKED)
    const struct t1_vcpu *tv = v->upper;
    return tv && (tv->opt.flags & T1_COMPILE_UNDO_LOG);
#else
    return false;
#endif
}

/* Restricted PSR states and active PMU counting use the lower tier.
 * This also prevents native links, compilation and successor chaining. */
static bool lower_tier_psr(const struct vcpu *v)
{
    uint64_t bits = v->s.psr & (IA64_PSR_BE | IA64_PSR_IS | T1_PSR_LOWER_TIER);
    if (bits)
        t1_psr_note(T1_PSR_LOWER, bits);
    return bits != 0 || ia64_pmu_active(v);
}

/* The region for v's state at the hot head entry_pa, compiled now on the
 * executor (t1_config.submit == NULL: LINKED or REJECTED) or queued
 * (PENDING); a held head is REJECTED; memory pressure or a compile refused
 * memory is DEFERRED. Before a submission, a head whose
 * first instruction formation rejects is REJECTED without a request and
 * without a strike. Regions
 * compiled here are policy regions: the demotions apply (hot.c). Shared by
 * hot() and enter's variant compile. */
static enum t1_hot_status compile_head(struct t1_vcpu *tv, struct vcpu *v, gpa_t entry_pa,
                                       bool loop_head)
{
    if (t1_hold_active(entry_pa, v->s.ip, v->s.slot)) {
        T1_COUNT(hot_held, 1);
        return T1_HOT_REJECTED;
    }
    unsigned pressure = t1_memory_pressure();
    if (pressure) {
        T1_COUNT(hot_deferrals[pressure - 1], 1);
        return T1_HOT_DEFERRED;
    }
    struct t1_compile_request rq;
    if (!t1_request_make(tv, entry_pa, loop_head, &rq)) {
        T1_COUNT(hot_unrequestable, 1);
        return T1_HOT_REJECTED;
    }
    rq.slow_memory = t1_head_memory_helpers(entry_pa, v->s.ip, v->s.slot);
    if (rq.slow_memory)
        rq.key ^= UINT64_C(0x9e3779b97f4a7c15);
    if (!rq.key)
        rq.key = 1;
    if (t1_cfg.submit) {
        unsigned stop;
        if (!t1_request_head_allowed(&rq, &stop)) {
            T1_COUNT(hot_head_rejects, 1);
            t1_form_stop_note(stop, true);
            t1_rejected_head_note(entry_pa, v->s.ip, v->s.slot);
            return T1_HOT_REJECTED;
        }
        /* The helper queue; a refused submission stays in the backoff. */
        if (t1_cfg.submit(&rq, t1_cfg.submit_arg))
            return T1_HOT_PENDING;
        T1_COUNT(hot_submit_refusals, 1);
        return T1_HOT_REJECTED;
    }
    struct t1_compile_report rep;
    enum t1_compile_status st = t1_compile_hot(tv, &rq, &rep);
    if (st == T1_NO_MEMORY)
        return T1_HOT_DEFERRED;
    if (st != T1_COMPILED)
        return T1_HOT_REJECTED;
    struct t1_region *r = t1_region_get(rep.region_id);
    if (r)
        r->policy = true;
    return T1_HOT_LINKED;
}

uint64_t t1_upper_probe(struct vcpu *v, gpa_t entry_pa)
{
    struct t1_vcpu *tv = v->upper;
    T1_COUNT(probes, 1);
    if (!tv || !linkable(v))
        return 0;
    if (lower_tier_psr(v)) {
        T1_COUNT(lower_refusals, 1);
        return 0;
    }
    struct t1_region *r = probe(tv, v, entry_pa);
    if (!r)
        return 0;
    T1_COUNT(probe_links, 1);
    return t1_region_link(r);
}

/* An upper entry returns to Tier0 after EXIT point
 * `point` of the region linked by `from`. */
static inline void mark_exit(struct t1_vcpu *tv, uint64_t from, uint32_t point, uint8_t end,
                             bool learn)
{
    struct t1_exit_mark *m = &tv->last_exit;
    m->from = from;
    m->retired = tv->v->retired;
    m->ip = tv->v->s.ip;
    m->point = point;
    m->slot = tv->v->s.slot;
    m->end = end;
    m->valid = true;
    m->learn = learn;
}

/* Checked replay starts before entry setup and finishes after the exit
 * observer. Each native successor receives its own sampling decision. */
static bool begin_region(struct t1_vcpu *tv, struct t1_region *r, bool *checking,
                         struct vcpu_exit *out)
{
#if defined(IA64VMM_HOSTED) || defined(IA64VMM_TIER1_CHECKED)
    struct vcpu *v = tv->v;
    /* Sampling is per region: a vCPU-wide period could miss one member of
     * an alternating sequence forever. Threshold-ineligible entries do not
     * advance the counter. Lockstep always replays every execution. */
    bool checked = (r->c.flags & T1_COMPILE_UNDO_LOG) != 0;
    if (checked && v->cfg.engine != ENGINE_LOCKSTEP) {
        uint64_t every = t1_cfg.check_every ? t1_cfg.check_every : 1;
        checked =
            v->retired >= t1_cfg.check_after &&
            atomic_fetch_add_explicit(&r->check_entries, 1, memory_order_relaxed) % every == 0;
    }
    if (checked && !t1_ck_begin(tv, r)) {
        clear_running(tv);
        *out = (struct vcpu_exit){VCPU_EXIT_DEBUG, v->s.ip, v->s.slot, 0,
                                  "tier1 lockstep allocation failed"};
        T1_COUNT_TV(tv, upper_stops, 1);
        return false;
    }
#else
    (void)tv;
    (void)r;
    (void)out;
    bool checked = false;
#endif
    *checking = checked;
    return true;
}

static bool finish_region(struct t1_vcpu *tv, struct t1_region *r, const struct t1_point *P,
                          uint64_t consumed, bool checked, bool *demoted, struct vcpu_exit *out)
{
    struct vcpu *v = tv->v;
#if defined(IA64VMM_HOSTED) || defined(IA64VMM_TIER1_CHECKED)
    if (checked && hostwin_stop_detail(tv->hw))
        t1_ck_cancel(tv);
    else if (checked && !t1_ck_finish(tv, P, consumed)) {
        *out =
            (struct vcpu_exit){VCPU_EXIT_DEBUG, v->s.ip, v->s.slot, 0, "tier1 lockstep divergence"};
        T1_COUNT_TV(tv, upper_stops, 1);
        return false;
    }
#endif
    if (hostwin_stop_detail(tv->hw)) {
        *out = (struct vcpu_exit){VCPU_EXIT_BAD_ACCESS, v->s.ip, v->s.slot, 0,
                                  hostwin_stop_detail(tv->hw)};
        T1_COUNT_TV(tv, upper_stops, 1);
        return false;
    }
    if (helper_notification(tv, out)) {
        T1_COUNT_TV(tv, upper_stops, 1);
        return false;
    }
    if ((v->cfg.engine == ENGINE_LOCKSTEP || (r->c.flags & T1_COMPILE_UNDO_LOG)) && !checked) {
        T1_COUNT_TV(tv, unchecked, 1);
        T1_COUNT_TV(tv, unchecked_retired, consumed);
    }
    *demoted = t1_demote_after_exit(&tv->windows, r, P, consumed);
    return true;
}

/* The chain point after EXIT point `point` of the region
 * linked by `from`, whose exit is fully processed and whose quiescence is
 * published; `left`: the starting boundary's budget T less the
 * instructions retired since. The region to enter next, past every guard
 * and published, with its region budget in *budget; or NULL and the
 * row that failed. Consumes nothing (requests, kicks, interrupts), walks no
 * VHPT and compiles nothing: those stay with Tier0. */
static struct t1_region *chain_next(struct t1_vcpu *tv, uint64_t from, uint32_t point,
                                    uint64_t left, uint64_t *budget, enum t1_chain_return *why)
{
    struct vcpu *v = tv->v;
    if (__atomic_load_n(&v->requested_exit, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&v->irq_kick, __ATOMIC_ACQUIRE)) {
        *why = T1_CHAIN_PAIR;
        return NULL;
    }
    if (ia64_break_ips) {
        *why = T1_CHAIN_BREAK;
        return NULL;
    }
    if (lower_tier_psr(v) || rse_incomplete(v) || !linkable(v)) {
        *why = T1_CHAIN_STATE;
        return NULL;
    }
    /* Tier0's loop top: only the poll can make an interrupt pending here. */
    if (v->timer_armed && !v->timer_fired)
        ia64_timer_poll(v);
    if (ia64_external_requested(v) && ia64_external_pending(v)) {
        *why = T1_CHAIN_INTERRUPT;
        return NULL;
    }
    const struct t1_successor *e = &tv->successors[t1_successor_index(from, point)];
    struct t1_region *s = e->from == from && e->point == point ? t1_region_from_link(e->to) : NULL;
#ifdef IA64VMM_HOSTED
    if (t1_test_chain_hook)
        t1_test_chain_hook(v, from, true);
#endif
    if (!s) {
        *why = T1_CHAIN_SUCCESSOR;
        return NULL;
    }
    T1_COUNT_TV(tv, succ_hits, 1);
    if (!t1_region_matches(s, v, tv->opt.flags, hostwin_bits(tv->hw))) {
        *why = T1_CHAIN_CONTRACT;
        return NULL;
    }
    if (!return_guard(v, s)) {
        *why = T1_CHAIN_STATE;
        return NULL;
    }
    /* Physical mode: the entry PA is the IP without bit 63; virtual: the
     * fetch-guard cache, whose fills check every page from the entry page. */
    uint64_t pte = 0;
    if ((v->s.psr & IA64_PSR_IT) ? !fetch_guard_cached(tv, v, s, &pte)
                                 : s->c.entry_pa != (v->s.ip & ~(UINT64_C(1) << 63))) {
        *why = T1_CHAIN_FETCH;
        return NULL;
    }
    *budget = ring_budget(s, left);
    if (*budget < s->max_path) {
        *why = T1_CHAIN_BUDGET;
        return NULL;
    }
    /* A failure is left to Tier0's next entry, which counts it once. */
    if (!t1_nat_guard_holds(s, v)) {
        *why = T1_CHAIN_NAT;
        return NULL;
    }
    t1_demote_after_nat_guard(&tv->windows, s, false);
    const struct code_fetch fetch = entry_fetch(v, pte);
    if (!validate_entry(v, s, &fetch)) {
        *why = T1_CHAIN_VALIDATE;
        return NULL;
    }
#ifdef IA64VMM_HOSTED
    tv->chained = true;
    if (s->c.flags & T1_COMPILE_VERIFY)
        t1_vh_entry_hook(tv, s);
#endif
    hostwin_enter(tv->hw, v);
    if (!publish_running(tv, s)) {
        *why = T1_CHAIN_RETIRED;
        return NULL;
    }
    return s;
}

/* One outer SysV entry owns this state and the shared stub's save frame.
 * A handoff runs from the arena's permanent stub page, so quiescence can
 * reclaim both predecessor metadata and code before a successor is chosen. */
struct native_chain {
    uint64_t total, used, regions;
    uint64_t *link;
    struct vcpu_exit *out;
    enum t1_upper_status status;
    bool checked;
};

/* Native edges update architecture and the local policy windows at their
 * commit. Their diagnostic counts/logs are drained in order before the
 * next ordinary exit and before any pointer can pass quiescence. */
static void fast_flush(struct t1_vcpu *tv, struct native_chain *c)
{
    struct t1_fast *f = &tv->fast;
#if IA64VMM_TIER1_STATS
    if (tv->x.fast)
        t1_fast_count_exit(tv);
#endif
    /* A failed first edge may have lost a stay/admission without retiring
     * any code. Rebuild on the next opportunity instead of retrying it. */
    if (tv->x.fast && !f->nrecord && tv->x.point < T1_FAST_POINTS &&
        tv->x.fast->edge[tv->x.point].target) {
#if IA64VMM_TIER1_STATS
        if (f->nnodes)
            T1_COUNT_TV(tv, native_discard_failed, 1);
#endif
        f->nnodes = 0;
    }
    tv->x.fast = NULL;
    if (f->nrecord) {
        T1_COUNT_TV(tv, chain_native_batches, 1);
        hostwin_note_reentries(tv->hw, f->nrecord);
    }
    for (unsigned k = 0; k < f->nrecord; ++k) {
        const struct t1_fast_record *e = &f->record[k];
        struct t1_region *r = e->from->region, *s = e->to->region;
        const struct t1_point *p = &r->point[e->point];
        T1_COUNT_TV(tv, exits_by_kind[T1_PT_EXIT], 1);
        T1_COUNT_TV(tv, exits_by_end[p->end], 1);
        T1_COUNT_TV(tv, tier1_retired, e->consumed);
        if (IA64VMM_TIER1_STATS)
            t1_hint_add64(&r->retired_insns, e->consumed);
        log_exit(tv, r, e->point, e->consumed);
        T1_COUNT_TV(tv, succ_hits, 1);
        T1_COUNT_TV(tv, chain_links, 1);
        T1_COUNT_TV(tv, chain_native_links, 1);
        count_chainable(tv, r->link, e->point, s->link);
        t1_hint_add64(&s->entries, 1);
        T1_COUNT_TV(tv, entries, 1);
        T1_COUNT_TV(tv, moved_entries, s->moved);
        ++c->regions;
        c->used += e->consumed;
    }
#ifdef IA64VMM_HOSTED
    if (f->nrecord)
        tv->chained = true;
#endif
    f->nrecord = 0;
}

static uintptr_t native_handoff(struct t1_exec *x)
{
    struct t1_vcpu *tv = x->tv;
    struct vcpu *v = tv->v;
    struct native_chain *c = x->handoff_arg;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    t1_set_current(NULL);
    clear_running(tv);
    fast_flush(tv, c);
    struct t1_region *r = (struct t1_region *)x->region;
    uint64_t consumed;
    const struct t1_point *P = finish_execute(tv, r, &consumed);
    bool demoted;
    if (!finish_region(tv, r, P, consumed, c->checked, &demoted, c->out)) {
        c->status = T1_UPPER_STOP;
        return 0;
    }
    if (demoted && c->regions == 1)
        *c->link = 0;
    c->used += consumed;
    if (P->kind == T1_PT_EXIT && consumed && !t1_cfg.chain_off) {
        uint64_t from = t1_region_link(r);
        uint32_t point = x->point;
        uint8_t end = P->end;
        /* No predecessor pointer is dereferenced after this publication. */
        code_quiescent_fast(v);
#ifdef IA64VMM_HOSTED
        if (t1_test_chain_hook)
            t1_test_chain_hook(v, from, false);
#endif
        enum t1_chain_return why;
        uint64_t next_budget;
        struct t1_region *s = chain_next(tv, from, point, c->total - c->used, &next_budget, &why);
        if (!s) {
            T1_COUNT_TV(tv, chain_returns[why], 1);
            mark_exit(tv, from, point, end, why == T1_CHAIN_SUCCESSOR || why == T1_CHAIN_CONTRACT);
            T1_COUNT_TV(tv, upper_ran, 1);
            c->status = T1_UPPER_RAN;
            return 0;
        }
        T1_COUNT_TV(tv, chain_links, 1);
        count_chainable(tv, from, point, t1_region_link(s));
        ++c->regions;
        if (!begin_region(tv, s, &c->checked, c->out)) {
            c->status = T1_UPPER_STOP;
            return 0;
        }
        prepare_entry(tv, s, next_budget);
        t1_fast_prepare(tv, s);
        uintptr_t target = (uintptr_t)s->code + s->native_entry;
        t1_set_current(x);
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        return target;
    }
    if (P->kind == T1_PT_SITE || P->kind == T1_PT_HELPER || (P->kind == T1_PT_EXIT && !consumed)) {
        if (tv->nexits <= T1_EXIT_LOG)
            tv->exits[tv->nexits - 1].lower_tier_step_followed = true;
        T1_COUNT_TV(tv, upper_ran_pending, 1);
        c->status = T1_UPPER_RAN_PENDING;
        return 0;
    }
    if (P->kind == T1_PT_EXIT)
        mark_exit(tv, t1_region_link(r), x->point, P->end, false);
    T1_COUNT_TV(tv, upper_ran, 1);
    c->status = T1_UPPER_RAN;
    return 0;
}

enum t1_upper_status t1_upper_enter(struct vcpu *v, uint64_t *link, uint64_t region_budget,
                                    struct vcpu_exit *out)
{
    struct t1_vcpu *tv = v->upper;
    tv->stats = &T1_STATS;
    T1_COUNT_TV(tv, upper_enters, 1);
    /* An entry right after an EXIT return: Tier0 found
     * the exit target with nothing retired in between. */
    const struct t1_exit_mark *mark = &tv->last_exit;
    bool follows = mark->valid && mark->retired == v->retired && mark->ip == v->s.ip &&
                   mark->slot == v->s.slot;
    if (mark->valid) {
        T1_COUNT_TV(tv, returns_by_end[mark->end], 1);
        T1_COUNT_TV(tv, gap_by_end[mark->end], v->retired - mark->retired);
    }
    tv->last_exit.valid = false;
    /* Backend resource/placement failures stop after materialization,
     * before the lower tier can execute the faulting access. */
    /* A lower-tier PSR state at a linked head: the link stays (it is right
     * once the state leaves the lower tier). */
    if (lower_tier_psr(v)) {
        T1_COUNT_TV(tv, decline_lower, 1);
        return T1_UPPER_DECLINED;
    }
    /* The link is a hint: liveness and generation
     * (t1_region_from_link), then every contract component, of which
     * Tier0's key does not determine the CFM rotation bases (the frame base
     * is no contract component), before anything runs. On a mismatch,
     * re-probe at the region's entry
     * PA (the Tier0 region holding the link has that PA in its key). */
    struct t1_region *r = t1_region_from_link(*link);
    if (!r || !r->valid) {
        *link = 0;
        T1_COUNT_TV(tv, decline_link, 1);
        return T1_UPPER_DECLINED;
    }
    if (!t1_region_matches(r, v, tv->opt.flags, hostwin_bits(tv->hw))) {
        /*
         * Re-probe for this frame; retain the existing link if no variant matches.
         * Compile missing variants on their own doubling schedule: alternating
         * frames can otherwise leave one frame with only even shared entry counts
         * and never trigger Tier0 hotness (programs/rotating_links.S, suite upper).
         */
        gpa_t pa = r->c.entry_pa;
        bool loop = r->loop;
        struct t1_region *m = linkable(v) ? probe(tv, v, pa) : NULL;
        uint32_t misses = m ? 0 : t1_hint_add32(&r->variant_misses, 1);
        if (!m && linkable(v) && !(misses & (misses - 1))) {
            T1_COUNT_TV(tv, variant_compiles, 1);
            /* r may be evicted by the compile: not used after it. */
            if (compile_head(tv, v, pa, loop) == T1_HOT_LINKED)
                m = probe(tv, v, pa);
        }
        if (!m) {
            uint64_t psr =
                linkable(v) ? t1_region_psr_difference(v, tv->opt.flags, hostwin_bits(tv->hw)) : 0;
            if (psr) {
                T1_COUNT_TV(tv, variant_psr_misses, 1);
                t1_psr_note(T1_PSR_VARIANT, psr);
            }
            T1_COUNT_TV(tv, decline_link, 1);
            return T1_UPPER_DECLINED;
        }
        *link = t1_region_link(m);
        r = m;
        T1_COUNT_TV(tv, relinks, 1);
    }
    /* The "checked" guards. rse_cfle is clear: Tier0's fetch
     * translation ran ia64_rse_resume at this boundary. */
    if (rse_incomplete(v) || !return_guard(v, r)) {
        T1_COUNT_TV(tv, entry_guard_fails, 1);
        T1_COUNT_TV(tv, decline_rse, 1);
        return T1_UPPER_DECLINED;
    }
    /* T: the budget the whole upper entry shares. */
    uint64_t T = slice_budget(v, region_budget), budget = ring_budget(r, T);
    if (budget < r->max_path) {
        T1_COUNT_TV(tv, entry_guard_fails, 1);
        T1_COUNT_TV(tv, decline_budget, 1);
        return T1_UPPER_DECLINED;
    }
    bool nat_ok = t1_nat_guard_holds(r, v);
    if (t1_demote_after_nat_guard(&tv->windows, r, !nat_ok))
        *link = 0;
    if (!nat_ok) {
        T1_COUNT_TV(tv, nat_guard_fails, 1);
        T1_COUNT_TV(tv, decline_nat, 1);
        return T1_UPPER_DECLINED;
    }
    /* Fetch guard, cached per vCPU, and the entry page's
     * I-TLB PTE for the fetch context. */
    uint64_t entry_pte;
    if (!fetch_guard_cached(tv, v, r, &entry_pte)) {
        T1_COUNT_TV(tv, entry_guard_fails, 1);
        T1_COUNT_TV(tv, decline_fetch, 1);
        return T1_UPPER_DECLINED;
    }
    /* Code validation (COH): r's own bundles. Tier0's
     * lookup validated only its region's bundles, in the stay this entry
     * continues. */
    const struct code_fetch fetch = entry_fetch(v, entry_pte);
    if (!validate_entry(v, r, &fetch)) {
        T1_COUNT_TV(tv, entry_guard_fails, 1);
        T1_COUNT_TV(tv, decline_validate, 1);
        return T1_UPPER_DECLINED;
    }
#ifdef IA64VMM_HOSTED
    tv->chained = false;
    if (r->c.flags & T1_COMPILE_VERIFY)
        t1_vh_entry_hook(tv, r);
#endif
    hostwin_enter(tv->hw, v);
    /* A validation (snapshot eviction), the hooks or another thread may have
     * retired it. */
    if (!publish_running(tv, r)) {
        T1_COUNT_TV(tv, decline_retired, 1);
        return T1_UPPER_DECLINED;
    }
    if (follows) {
        uint64_t to = t1_region_link(r);
        count_chainable(tv, mark->from, mark->point, to);
        /* Tier0 found the successor the chain point lacked
         * with every duty discharged. */
        if (mark->learn) {
            tv->successors[t1_successor_index(mark->from, mark->point)] =
                (struct t1_successor){mark->from, to, mark->point};
#if IA64VMM_TIER1_STATS
            if (tv->fast.nnodes)
                T1_COUNT_TV(tv, native_discard_learned, 1);
#endif
            tv->fast.nnodes = 0;
        }
    }
    struct native_chain chain = {.total = T, .regions = 1, .link = link, .out = out};
    if (!begin_region(tv, r, &chain.checked, out)) {
        count_length(tv, 1);
        return T1_UPPER_STOP;
    }
    tv->x.handoff_arg = &chain;
    tv->x.handoff = native_handoff;
    enter(tv, r, budget);
    tv->x.handoff = NULL;
    tv->x.handoff_arg = NULL;
    count_length(tv, chain.regions);
    return chain.status;
}

enum t1_hot_status t1_upper_hot(struct vcpu *v, gpa_t entry_pa, bool loop_head)
{
    struct t1_vcpu *tv = v->upper;
    T1_COUNT(hot_calls, 1);
    enum t1_hot_status st;
    if (!tv || !linkable(v))
        st = T1_HOT_REJECTED;
    else if (lower_tier_psr(v)) {
        T1_COUNT(lower_refusals, 1);
        st = T1_HOT_REJECTED;
    }
    /* hot() is called only for heads without a link, but a region for v's
     * state may exist already (another frame of the same head linked
     * first): never compile it again. */
    else if (probe(tv, v, entry_pa))
        st = T1_HOT_LINKED;
    else {
        uint64_t psr = t1_region_psr_difference(v, tv->opt.flags, hostwin_bits(tv->hw));
        if (psr) {
            T1_COUNT(hot_psr_variants, 1);
            t1_psr_note(T1_PSR_VARIANT, psr);
        }
        st = compile_head(tv, v, entry_pa, loop_head);
    }
    if (st == T1_HOT_LINKED)
        T1_COUNT(hot_linked, 1);
    else if (st == T1_HOT_PENDING)
        T1_COUNT(hot_pending, 1);
    else if (st == T1_HOT_DEFERRED)
        T1_COUNT(hot_deferred, 1);
    else
        T1_COUNT(hot_rejected, 1);
    return st;
}

void t1_upper_reclaim(void)
{
    if (t1_cfg.submit_reclaim) {
        if (t1_maintenance_wanted())
            t1_cfg.submit_reclaim();
    } else
        t1_reclaim();
}

const uint64_t *t1_upper_sequence(void)
{
    return &t1_sequence;
}

/* ---- engine=tier1 in the product (tier1.h struct t1_engine_glue) ---- */

static const struct hostwin_backend *glue_backend;
static void *glue_backend_ctx;
static bool glue_check;
/* No tier1_regions=: the table is sized with the pool (glue_memory_budget). */
static bool glue_regions_auto;

static bool glue_setup(const struct t1_config *cfg, bool hostwin_check, const char **why)
{
    void *ctx = NULL;
    const struct hostwin_backend *backend = hostwin_host_backend(&ctx);
    /* Never Tier1 without windows. */
    if (!backend) {
        *why = "the host has no window backend (hostwin_host_backend)";
        return false;
    }
    if (!t1_init(cfg)) {
        *why = "t1_init failed (tier1_code_mib, max_regions or memory)";
        return false;
    }
    glue_backend = backend;
    glue_backend_ctx = ctx;
    glue_check = hostwin_check;
    glue_regions_auto = !cfg->max_regions;
    hostwin_set_trap_client(t1_trap_client);
    code_validity_set_track_hook(hostwin_code_page_event);
    return true;
}

static void glue_teardown(void)
{
    code_validity_set_track_hook(NULL);
    hostwin_set_trap_client(NULL);
    t1_shutdown();
    glue_backend = NULL;
    glue_backend_ctx = NULL;
}

/* The window instances glue_vcpu_create made and glue_vcpu_destroy has
 * not destroyed (stopped executors only), for glue_windows: one per vCPU,
 * at most IA64VMM_MAX_CPUS (ia64vmm/platform.h). */
#define GLUE_INSTANCES 256u
static struct hostwin_vcpu *glue_instances[GLUE_INSTANCES];

/* Window width, one for the VM: regions match on it, so vCPUs of
 * different widths could not share them. The first instance reserves 40
 * bits, 36 if that is refused; every later instance takes the same width,
 * and a vCPU whose reservation is refused gets none (engine_vcpu_attach
 * fails setup naming it). Where the backend has private roots, each
 * instance lies in its vCPU's host CPU context, and all of them reuse one
 * range. */
static unsigned glue_width;
static struct t1_vcpu *glue_vcpu_create(struct vcpu *v)
{
    unsigned at = 0, live = 0;
    for (unsigned k = 0; k < GLUE_INSTANCES; ++k)
        live += glue_instances[k] != NULL;
    while (at < GLUE_INSTANCES && glue_instances[at])
        ++at;
    if (at == GLUE_INSTANCES)
        return NULL;
    unsigned cpu = hal_vcpu_cpu(v->cfg.index);
    void *ctx = glue_backend->cpu_context ? glue_backend->cpu_context(glue_backend_ctx, cpu)
                                          : glue_backend_ctx;
    struct hostwin_config hc = {live ? glue_width : 40, HOSTWIN_MAX_SLOTS, glue_check};
    struct hostwin_vcpu *hw = hostwin_create(&hc, glue_backend, ctx);
    if (!hw && !live) {
        hc.w = 36;
        hw = hostwin_create(&hc, glue_backend, ctx);
    }
    if (!hw)
        return NULL;
    glue_width = hc.w;
    struct t1_vcpu *tv = t1_vcpu_create(v, hw);
    if (!tv) {
        hostwin_destroy(hw);
        return NULL;
    }
    glue_instances[at] = hw;
#if defined(IA64VMM_HOSTED) || defined(IA64VMM_TIER1_CHECKED)
    /* Diagnostic replay uses instrumented regions even on unsampled runs:
     * store preloads/logging, a 256-instruction budget, and boundaries at
     * memory helpers. It does not check the normal generated code. */
    if (v->cfg.engine == ENGINE_LOCKSTEP || t1_cfg.check_every) {
        tv->opt.flags = T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG;
        if (hal_vcpu_count() > 1)
            tv->opt.flags |= T1_COMPILE_ACCESS_LOG;
    }
#endif
    return tv;
}

static void glue_vcpu_destroy(struct t1_vcpu *tv)
{
    if (!tv)
        return;
    struct hostwin_vcpu *hw = tv->hw;
    for (unsigned k = 0; k < GLUE_INSTANCES; ++k)
        if (glue_instances[k] == hw)
            glue_instances[k] = NULL;
    t1_vcpu_destroy(tv);
    hostwin_destroy(hw);
}

static void glue_windows(struct t1_window_totals *out)
{
    *out = (struct t1_window_totals){0};
    for (unsigned k = 0; k < GLUE_INSTANCES; ++k) {
        if (!glue_instances[k])
            continue;
        struct hostwin_stats s;
        hostwin_get_stats(glue_instances[k], &s);
        ++out->instances;
        out->fills += s.fills;
        out->fills_ro += s.fills_ro;
        out->live += s.live_mappings;
        out->unbacked += s.exits[HW_EXIT_UNBACKED];
        out->not_ram += s.exits[HW_EXIT_NOT_RAM];
        out->backend_failures += s.backend_failures;
    }
}

static void glue_background(t1_submit_fn submit, void (*reclaim)(void))
{
    t1_cfg.submit = submit;
    t1_cfg.submit_arg = NULL;
    t1_cfg.submit_reclaim = reclaim;
}

static const struct t1_config *glue_config(void)
{
    return &t1_cfg;
}

/* Without tier1_regions= the table holds as many records
 * as the pool's allowance could hold at T1_REGION_MIN_BYTES each, at least
 * the default, so the pool and not the table limits the regions; then the
 * cap is set, counting the table. */
static void glue_memory_budget(uint64_t free_bytes, uint64_t unavailable, uint64_t reserve_bytes)
{
    struct t1_memory_stats before, after;
    t1_memory_get_stats(&before);
    uint64_t n = t1_memory_allowance(free_bytes, unavailable, reserve_bytes) / T1_REGION_MIN_BYTES;
    if (n > T1_MAX_REGIONS_LIMIT)
        n = T1_MAX_REGIONS_LIMIT;
    if (glue_regions_auto && n > t1_cfg.max_regions && t1_regions_resize((unsigned)n))
        t1_cfg.max_regions = (uint32_t)n;
    t1_memory_get_stats(&after);
    uint64_t growth =
        after.used_bytes > before.used_bytes ? after.used_bytes - before.used_bytes : 0;
    free_bytes = free_bytes > growth ? free_bytes - growth : 0;
    t1_memory_configure(free_bytes, unavailable, reserve_bytes);
}

static const struct t1_engine_glue glue = {
    .setup = glue_setup,
    .teardown = glue_teardown,
    .config = glue_config,
    .reserve_bytes = t1_compile_reserve_bytes,
    .memory_budget = glue_memory_budget,
    .background = glue_background,
    .executor_compile_ticks = t1_executor_compile_ticks,
    .vcpu_create = glue_vcpu_create,
    .vcpu_destroy = glue_vcpu_destroy,
    .probe = t1_upper_probe,
    .enter = t1_upper_enter,
    .hot = t1_upper_hot,
    .reclaim = t1_upper_reclaim,
    .sequence = t1_upper_sequence,
    .heads = t1_upper_heads,
    .reclaim_due = t1_upper_reclaim_due,
    .stats = t1_get_stats,
    .windows = glue_windows,
};

const struct t1_engine_glue *engine_tier1_glue(void)
{
    return &glue;
}
