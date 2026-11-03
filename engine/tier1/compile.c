/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Compile: form -> lift -> optimize -> isel -> regalloc -> raverify -> emit
 * -> frames -> publish. Late boundaries truncate before the source instruction
 * and retry. Verify allocated MIR always; verify IR per pass or at final IR,
 * according to configuration.
 *
 * Compile immutable requests using a private context. Stage storage comes
 * from its retained workspace or pool allocations; records/tables use pool
 * metadata. Pool refusal defers with T1_NO_MEMORY without shrinking/rejecting
 * the request; release temporary allocations. An outstanding memory request
 * defers new work before allocating.
 *
 * Publication validates exact source-bundle masks before writing code and
 * before attaching dependencies. Changed source yields T1_RETRY (up to eight
 * attempts); later changes retire the region through its dependencies.
 * See publish() and arena.c for sealing and visibility.
 */
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"
#include "engine/hostwin/hostwin.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

static size_t pages_for(size_t bytes)
{
    return (bytes + 4095) / 4096;
}

#ifdef IA64VMM_HOSTED
t1_ir_out_fn t1_test_mir_print;
uint16_t t1_test_alloc_pool;
static t1_compile_fn compile_observer;
static void *compile_observer_arg;
void t1_test_set_compile_observer(t1_compile_fn fn, void *arg)
{
    compile_observer = fn;
    compile_observer_arg = arg;
}
#endif

struct work {
    struct t1_form *form;
    struct t1_ir *ir;
    struct t1_mir *m;
    struct t1_emitted e;
    char err[200];
};

static void reject(struct t1_compile_report *rep, const char *why, uint64_t ip, unsigned slot)
{
    if (!rep)
        return;
    rep->reject_reason = why;
    rep->reject_ip = ip;
    rep->reject_slot = slot;
}

/* Each worker retains its request and intermediate pipeline state across
 * steps, and its workspace block across compiles. */
struct t1_compile_ctx {
    struct ia64_state *names;
    bool busy, background, on_executor;
    unsigned open_chunk, npending;
    uint64_t batch_started;
    struct t1_region *pending[8];
    unsigned stage, max, shrinks, attempts;
    uint64_t started;
    struct t1_compile_request rq;
    struct t1_compile_report report;
    struct work work;
    struct t1_workspace ws;
    char reason[160]; /* stage-qualified rejection reason */
    /* T1_NO_MEMORY: what was short and whether an outstanding request
     * deferred the compile before it ran (tier1.h deferrals_*). */
    enum t1_defer_kind defer;
    bool defer_pressure;
    struct t1_region *region;
    uint64_t link;
    enum t1_compile_status status;
};
static size_t names_pages(void)
{
    return pages_for(sizeof(struct ia64_state));
}

static const char *stage_reason(struct t1_compile_ctx *ctx, const char *stage, const char *why)
{
    vmm_snprintf(ctx->reason, sizeof(ctx->reason), "%s: %s", stage, why ? why : "?");
    return ctx->reason;
}

static uint64_t page_sum(const size_t *sizes, unsigned n)
{
    uint64_t bytes = 0;
    for (unsigned i = 0; i < n; ++i)
        bytes += pages_for(sizes[i]) * 4096;
    return bytes;
}

uint64_t t1_compile_workspace_bytes(void)
{
    /* alloc_work, mir_create, the emitter and the allocator's rewrite at
     * their maxima. The other stage arrays (lifter, folding, isel, allocator
     * intervals, verifiers) scale with the region and are freed before the
     * emitter allocates: its maxima stand for them. A compile beyond the
     * bound takes the rest from the pool's free room. */
    const size_t sizes[] = {
        sizeof(struct t1_form),
        sizeof(struct t1_ir),
        sizeof(struct t1_mir),
        T1_MIR_MAX_INSNS * sizeof(struct t1_mir_insn),
        T1_MIR_MAX_INSNS * sizeof(struct t1_mir_memreg),
        T1_MIR_MAX_VREGS * sizeof(struct t1_mir_vreg),
        T1_MIR_MAX_COPIES * sizeof(struct t1_mir_copy),
        T1_IR_MAX_USES * sizeof(struct t1_mir_frame),
        T1_IR_MAX_USES * sizeof(struct t1_loc),
        T1_MAX_POINTS * sizeof(struct t1_mir_point),
        T1_MIR_MAX_INSNS * 48u + T1_MAX_POINTS * 64u + 8192u,
        T1_MAX_POINTS * sizeof(struct t1_emit_point),
        t1_emit_state_bytes(),
        T1_MIR_MAX_BLOCKS * (sizeof(uint32_t) + sizeof(uint16_t)),
        T1_MIR_MAX_INSNS * sizeof(struct t1_mir_insn),
        (T1_MIR_MAX_INSNS + 1) * sizeof(uint32_t),
    };
    return page_sum(sizes, IA64VMM_ARRAY_SIZE(sizes));
}

uint64_t t1_compile_reserve_bytes(void)
{
    /* Room for the bounded background pipelines, including each context
     * and one maximal region. */
    const size_t sizes[] = {
        sizeof(struct t1_compile_ctx),
        sizeof(struct ia64_state),
        sizeof(struct t1_region),
        1024u * sizeof(struct t1_fixup),
        T1_MAX_POINTS * sizeof(struct t1_point) + T1_IR_MAX_USES * sizeof(struct t1_frame_entry) +
            3u * T1_MAX_POINTS * sizeof(struct t1_rip) +
            T1_MAX_POINTS * sizeof(struct t1_helper_info) + T1_IR_MAX_USES * sizeof(uint32_t) +
            T1_MAX_POINTS * T1_N_ELEMENTS * sizeof(uint16_t) + T1_MAX_POINTS * sizeof(uint16_t) +
            224u,
    };
    return T1_COMPILE_SLOTS *
           (t1_compile_workspace_bytes() + page_sum(sizes, IA64VMM_ARRAY_SIZE(sizes)));
}

struct t1_compile_ctx *t1_compile_ctx_create(void)
{
    struct t1_compile_ctx *ctx = t1_memory_alloc(pages_for(sizeof(*ctx)), HAL_PAGE_ZERO);
    if (!ctx)
        return NULL;
    ctx->names = t1_memory_alloc(names_pages(), HAL_PAGE_ZERO);
    if (!ctx->names) {
        t1_memory_free(ctx, pages_for(sizeof(*ctx)));
        return NULL;
    }
    ctx->busy = false;
    ctx->open_chunk = UINT32_MAX;
    return ctx;
}

void t1_compile_ctx_destroy(struct t1_compile_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->busy)
        hal_panic("tier1: compile context destroyed while compiling");
    t1_compile_cancel(ctx);
    t1_ws_release(&ctx->ws);
    t1_memory_free(ctx->names, names_pages());
    t1_memory_free(ctx, pages_for(sizeof(*ctx)));
}

/* The sole synchronous executor's context: with more than one vCPU every
 * compile is submitted. */
static struct t1_compile_ctx *executor_ctx;
/* Background pipeline count, or T1_COMPILE_SLOTS + 1 for an exclusive
 * synchronous compile. Helpers retain their workspace slots separately. */
static unsigned compiling;
static uint64_t executor_ticks;

uint64_t t1_executor_compile_ticks(void)
{
    return __atomic_load_n(&executor_ticks, __ATOMIC_RELAXED);
}

void t1_compile_shutdown(void)
{
    t1_compile_ctx_destroy(executor_ctx);
    executor_ctx = NULL;
}

static void fill_report(struct t1_compile_report *rep, const struct t1_region *r,
                        const struct t1_mir *m, const struct t1_emitted *e)
{
    rep->region_id = r->id;
    rep->insns = r->ninsns;
    rep->blocks = r->nblocks;
    rep->pages = r->npages;
    rep->sites = r->nsites;
    rep->points = r->nlive_points;
    rep->loop_host_insns = e->loop_host_insns;
    rep->loop_value_spills = m->loop_value_spills;
    rep->restore_only_spills = m->restore_only_spills;
    rep->nat_guards = 0;
    for (unsigned w = 0; w < 2; ++w)
        for (uint64_t g = r->nat_guard[w]; g; g &= g - 1)
            ++rep->nat_guards; /* no libgcc popcount */
    rep->code_bytes = e->size;
    rep->n_loop_carried = 0;
    /* Loop-carried values: main values (with uses in the loop) first. */
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned v = 0; v < m->nvregs && rep->n_loop_carried < 16; ++v) {
            const struct t1_mir_vreg *vr = &m->vreg[v];
            if (!(vr->flags & T1_VR_LOOP_PARAM) || !vr->element)
                continue;
            if (((vr->flags & T1_VR_REAL_USE) != 0) != (pass == 0))
                continue;
            rep->loop_carried[rep->n_loop_carried] = vr->element;
            rep->loop_carried_reg[rep->n_loop_carried++] =
                vr->header_loc.kind == T1_LOC_REG ? vr->header_loc.reg : T1_X86_NOREG;
        }
}

/* The IR is written as it grows (t1_ir_init and the builders of ir.c) and
 * is not cleared. */
static bool alloc_work(struct t1_compile_ctx *ctx)
{
    struct work *w = &ctx->work;
    memset(w, 0, sizeof(*w));
    t1_ws_reset(&ctx->ws);
    w->form = t1_ws_alloc(&ctx->ws, pages_for(sizeof(*w->form)), HAL_PAGE_ZERO);
    w->ir = t1_ws_alloc(&ctx->ws, pages_for(sizeof(*w->ir)), 0);
    if (w->ir)
        w->ir->ws = &ctx->ws;
    return w->form && w->ir;
}

static void free_work(struct t1_compile_ctx *ctx)
{
    struct work *w = &ctx->work;
    t1_emit_free(&w->e);
    if (w->m)
        t1_mir_destroy(w->m);
    t1_ws_free(&ctx->ws, w->ir, pages_for(sizeof(*w->ir)));
    t1_ws_free(&ctx->ws, w->form, pages_for(sizeof(*w->form)));
    t1_ws_reset(&ctx->ws);
    memset(w, 0, sizeof(*w));
}

void t1_compile_trim(struct t1_compile_ctx *ctx)
{
    if (ctx->busy)
        hal_panic("tier1: compile workspace trimmed while compiling");
    t1_ws_release(&ctx->ws);
}

/*
 * Return the source-page count and exact bundle masks for region instructions.
 * Exclude lookahead and trailing pages with no instruction: the lower tier
 * fetches the exit instruction itself. Every retained mask must be nonempty;
 * code_dependency_attach would widen an empty mask to the whole page.
 */
static unsigned source_masks(struct t1_region *r, const struct t1_form *f)
{
    unsigned pages = 0;
    for (unsigned p = 0; p < T1_MAX_PAGES; ++p)
        memset(r->dep[p].mask, 0, sizeof(r->dep[p].mask));
    for (unsigned i = 0; i < f->ninsns; ++i) {
        const struct t1_insn *ti = &f->insn[i];
        if (ti->page >= f->npages || (ti->bundle_pa & ~UINT64_C(4095)) != f->page[ti->page].pa)
            hal_panic("tier1: instruction outside its source page");
        unsigned granule = (unsigned)(ti->bundle_pa & 4095) >> 4;
        r->dep[ti->page].mask[granule / 64] |= UINT64_C(1) << (granule % 64);
        if (ti->page + 1u > pages)
            pages = ti->page + 1u;
    }
    return pages;
}

/*
 * Place code; T1_RETRY if a masked source bundle changed. Background code
 * stays private until batch seal/publication; synchronous code is published
 * RW and sealed on first entry (arena.c). Validate masks before writing and
 * immediately before attaching dependencies (CODE_TEST_SEAL); thereafter
 * invalidation and entry validation cover changes. Unmasked bytes may change.
 */
static enum t1_compile_status publish(struct t1_compile_ctx *ctx, struct t1_region *r,
                                      const struct t1_emitted *e, const char **why)
{
    *why = "page changed during publication";
    for (unsigned p = 0; p < r->npages; ++p) {
#ifdef IA64VMM_HOSTED
        code_validity_test_hook(r->page[p].pa, CODE_TEST_PUBLISH);
#endif
        if (!code_dependency_publish(NULL, r->page[p].pa, r->page[p].generation, &r->dep[p]))
            return T1_RETRY;
    }
    bool placed =
        ctx->background ? t1_arena_place_background(r, e, &ctx->open_chunk) : t1_arena_place(r, e);
    if (!placed) {
        *why = "code arena full";
        return T1_NO_MEMORY;
    }
    for (unsigned p = 0; p < r->npages; ++p) {
#ifdef IA64VMM_HOSTED
        code_validity_test_hook(r->page[p].pa, CODE_TEST_SEAL);
#endif
        if (!code_dependency_publish(NULL, r->page[p].pa, r->page[p].generation, &r->dep[p])) {
            *why = "page changed during publication";
            return T1_RETRY;
        }
    }
    if (ctx->background) {
        if (!ctx->npending)
            ctx->batch_started = hal_counter();
        ctx->pending[ctx->npending++] = r;
        return T1_COMPILED;
    }
    return t1_region_publish(r) ? T1_COMPILED : T1_RETRY;
}

void t1_compile_flush(struct t1_compile_ctx *ctx)
{
    if (!ctx->npending)
        return;
    t1_arena_seal_background(&ctx->open_chunk);
    for (unsigned i = 0; i < ctx->npending; ++i) {
        struct t1_region *r = ctx->pending[i];
#ifdef IA64VMM_HOSTED
        for (unsigned p = 0; p < r->npages; ++p)
            code_validity_test_hook(r->page[p].pa, CODE_TEST_SEAL);
#endif
        if (!t1_region_publish(r)) {
            t1_head_moved(r->c.ip, r->c.slot);
            t1_region_discard(r);
        }
    }
    ctx->npending = 0;
    __atomic_add_fetch(&t1_sequence, 1, __ATOMIC_RELEASE);
}

enum {
    COMPILE_FORM,
    COMPILE_LIFT,
    COMPILE_ISEL,
    COMPILE_RA,
    COMPILE_EMIT,
    COMPILE_FRAME,
    COMPILE_PUBLISH,
    COMPILE_CANCELLED
};
/* A rejected compile counts under the stage it ended in (tier1.h). */
_Static_assert(T1_REJECT_FORM + COMPILE_CANCELLED == T1_REJECT_CANCELLED &&
                   T1_REJECT_FORM + COMPILE_RA == T1_REJECT_REGALLOC,
               "compile stages and enum t1_reject_stage");

static bool finish(struct t1_compile_ctx *ctx, enum t1_compile_status status,
                   struct t1_compile_report *rep)
{
    uint64_t link = status == T1_COMPILED ? ctx->link : 0;
    if (ctx->region && status != T1_COMPILED)
        t1_region_discard(ctx->region);
    free_work(ctx);
    ctx->report.compile_ticks = IA64VMM_TIER1_STATS ? hal_counter() - ctx->started : 0;
    if (link) {
        /* A background region stays private to this context until its batch
         * is flushed. A published one may already have been retired and
         * reclaimed: resolve its generation under code_lock. */
        code_lock();
        struct t1_region *r = ctx->background ? ctx->region : t1_region_from_link(link);
        if (r)
            atomic_store_explicit(&r->report.compile_ticks, ctx->report.compile_ticks,
                                  memory_order_relaxed);
        code_unlock();
    }
    ctx->region = NULL;
    T1_COUNT(compile_ticks, ctx->report.compile_ticks);
    if (IA64VMM_TIER1_STATS && ctx->on_executor)
        __atomic_fetch_add(&executor_ticks, ctx->report.compile_ticks, __ATOMIC_RELAXED);
    if (status == T1_COMPILED)
        T1_COUNT(compiles, 1);
    else if (status == T1_NO_MEMORY) {
        T1_COUNT(memory_deferrals, 1);
        if (ctx->defer_pressure)
            T1_COUNT(deferrals_pressure[ctx->defer], 1);
        else
            T1_COUNT(deferrals_refused[ctx->defer], 1);
    } else {
        T1_COUNT(rejects, 1);
        T1_COUNT(rejects_by_stage[T1_REJECT_FORM + ctx->stage], 1);
        if (status == T1_REJECTED && !ctx->report.reject_transient) {
            t1_rejected_head_note(ctx->rq.entry_pa, ctx->rq.ip, ctx->rq.slot);
            /* A queued request's reject recurs at its head like the
             * executor's (t1_compile_hot): a strike. */
            if (ctx->background)
                t1_hold_rejected(ctx->rq.entry_pa, ctx->rq.ip, ctx->rq.slot);
        }
    }
    ctx->status = status;
    if (rep)
        *rep = ctx->report;
    /* Completion releases pending heads even when publication was refused:
     * the head's counter moves here unless a publication moves it. */
    if (status != T1_COMPILED)
        t1_head_moved(ctx->rq.ip, ctx->rq.slot);
    if (!ctx->background || status != T1_COMPILED)
        __atomic_add_fetch(&t1_sequence, 1, __ATOMIC_RELEASE);
    if (ctx->background &&
        (ctx->npending == IA64VMM_ARRAY_SIZE(ctx->pending) ||
         (ctx->npending && hal_counter() - ctx->batch_started >= hal_counter_hz() / 1000)))
        t1_compile_flush(ctx);
    ctx->busy = false;
    if (ctx->background)
        __atomic_fetch_sub(&compiling, 1, __ATOMIC_RELEASE);
    else
        __atomic_store_n(&compiling, 0, __ATOMIC_RELEASE);
    return false;
}

bool t1_compile_begin(struct t1_compile_ctx *ctx, const struct t1_compile_request *rq,
                      bool background)
{
    if (!t1_ready || ctx->busy)
        return false;
    unsigned expected = background ? __atomic_load_n(&compiling, __ATOMIC_RELAXED) : 0;
    if (background) {
        do {
            if (expected >= T1_COMPILE_SLOTS)
                return false;
        } while (!__atomic_compare_exchange_n(&compiling, &expected, expected + 1, false,
                                              __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    } else if (!__atomic_compare_exchange_n(&compiling, &expected, T1_COMPILE_SLOTS + 1, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return false;
    ctx->busy = true;
    ctx->background = background;
    ctx->on_executor = !background && hal_cpu_index() == hal_vcpu_cpu(rq->vcpu);
    ctx->rq = *rq;
    memset(&ctx->report, 0, sizeof(ctx->report));
    ctx->region = NULL;
    ctx->stage = COMPILE_FORM;
    ctx->max = T1_MAX_INSNS;
    ctx->shrinks = ctx->attempts = 0;
    ctx->defer = T1_DEFER_BYTES;
    ctx->defer_pressure = false;
    ctx->started = IA64VMM_TIER1_STATS ? hal_counter() : 0;
    return true;
}

static bool shrink(struct t1_compile_ctx *ctx, unsigned max)
{
    if (++ctx->shrinks >= 64)
        return false;
    free_work(ctx);
    ctx->max = max;
    ctx->stage = COMPILE_FORM;
    return true;
}

static bool refused_since(uint64_t refusals)
{
    return t1_memory_refusals_here() != refusals;
}

bool t1_compile_step(struct t1_compile_ctx *ctx, struct t1_compile_report *rep)
{
    if (!ctx->busy)
        hal_panic("tier1: stepping an idle compile context");
    if (ctx->npending && hal_counter() - ctx->batch_started >= hal_counter_hz() / 1000)
        t1_compile_flush(ctx);
    struct work *w = &ctx->work;
    const struct t1_compile_request *rq = &ctx->rq;
    struct t1_compile_report *report = &ctx->report;
    enum t1_compile_status status = T1_REJECTED;
    const char *why = NULL;
    /* A step runs on one CPU: its refusals are this stage's. */
    uint64_t refusals = t1_memory_refusals_here();
    switch (ctx->stage) {
    case COMPILE_FORM: {
        unsigned pressure = t1_memory_pressure();
        if (pressure) {
            why = "memory pressure";
            status = T1_NO_MEMORY;
            ctx->defer = (enum t1_defer_kind)(pressure - 1);
            ctx->defer_pressure = true;
            break;
        }
        /* Without a block every stage array comes from the pool. */
        t1_ws_acquire(&ctx->ws);
        if (!alloc_work(ctx)) {
            why = "no memory";
            status = T1_NO_MEMORY;
            break;
        }
        status = t1_form_request(rq, ctx->names, ctx->max, w->form, &why);
        if (status == T1_REJECTED)
            report->reject_transient = w->form->transient;
        if (status != T1_COMPILED)
            break;
        w->form->c.hostwin_bits = rq->hostwin_bits;
        ctx->stage = COMPILE_LIFT;
        return true;
    }
    case COMPILE_LIFT: {
        unsigned fail_insn = w->form->ninsns;
        if (!t1_lift(w->form, w->ir, &fail_insn)) {
            if (!refused_since(refusals) && fail_insn && fail_insn < w->form->ninsns &&
                shrink(ctx, fail_insn))
                return true;
            why = w->ir->fail_reason ? w->ir->fail_reason : "lift failed";
            break;
        }
        if (!t1_optimize(w->ir, !t1_cfg.verify_final, w->err, sizeof(w->err))) {
            uint16_t boundary = w->ir->boundary_insn;
            if (!refused_since(refusals) && boundary != T1_NO_INSN && boundary > 0 &&
                shrink(ctx, boundary))
                return true;
            why = boundary == 0
                      ? "first instruction consumes a NaT"
                      : stage_reason(ctx, "optimize", w->err[0] ? w->err : w->ir->fail_reason);
            break;
        }
        if (t1_cfg.verify_final &&
            !t1_ir_verify(w->ir, T1_VERIFY_NAT_FOLDED, w->err, sizeof(w->err))) {
            why = stage_reason(ctx, "verify", w->err);
            break;
        }
        ctx->stage = COMPILE_ISEL;
        return true;
    }
    case COMPILE_ISEL:
        w->m = t1_mir_create(w->ir);
        if (!w->m) {
            why = "no memory";
            status = T1_NO_MEMORY;
            break;
        }
        w->m->window_bits = rq->hostwin_bits;
        w->m->pin_element = rq->opt.pin_element;
        w->m->pin_reg = rq->opt.pin_element ? rq->opt.pin_reg : T1_X86_NOREG;
#ifdef IA64VMM_HOSTED
        if (t1_test_alloc_pool)
            w->m->allocatable = t1_test_alloc_pool & T1_ALLOCATABLE;
#endif
        if (!t1_isel(w->m, rq->opt.flags)) {
            uint16_t boundary = w->m->fail_insn;
            if (!refused_since(refusals) && boundary != T1_NO_INSN && boundary > 0 &&
                boundary < w->form->ninsns && shrink(ctx, boundary))
                return true;
            why = stage_reason(ctx, "isel", w->m->fail_reason);
            break;
        }
        ctx->stage = COMPILE_RA;
        return true;
    case COMPILE_RA:
        if (!t1_regalloc(w->m)) {
            if (!refused_since(refusals) && w->m->capacity_failure && w->form->ninsns > 2 &&
                shrink(ctx, w->form->ninsns / 2))
                return true;
            why = stage_reason(ctx, "regalloc", w->m->fail_reason);
            break;
        }
#ifdef IA64VMM_HOSTED
        if (t1_test_mir_print)
            t1_mir_print(w->m, true, t1_test_mir_print, NULL);
#endif
        if (!t1_raverify(w->m, w->err, sizeof(w->err))) {
            why = stage_reason(ctx, "raverify", w->err);
            break;
        }
        ctx->stage = COMPILE_EMIT;
        return true;
    case COMPILE_EMIT:
        if (!t1_emit(w->m, &w->e, w->err, sizeof(w->err))) {
            why = stage_reason(ctx, "emit", w->err);
            break;
        }
        ctx->stage = COMPILE_FRAME;
        return true;
    case COMPILE_FRAME: {
        /* The record and its tables as one request: a deferral asks for
         * their total, so the retry finds room for all of them. */
        uint64_t need = (pages_for(sizeof(struct t1_region)) + t1_frame_pages(w->m, &w->e)) * 4096;
        if (t1_memory_headroom() < need) {
            t1_memory_refuse(need);
            why = "no memory for region tables";
            status = T1_NO_MEMORY;
            break;
        }
        struct t1_region *r = t1_region_new();
        if (!r) {
            why = "no region record";
            status = T1_NO_MEMORY;
            /* A full table records an id request; the record's pages a
             * refusal. */
            if (!refused_since(refusals))
                ctx->defer = T1_DEFER_ID;
            break;
        }
        ctx->region = r;
        r->c = w->form->c;
        memcpy(r->page, w->form->page, sizeof(r->page));
        r->npages = source_masks(r, w->form);
        memcpy(r->nat_guard, w->ir->nat_guard, sizeof(r->nat_guard));
        memcpy(r->fr_guard, w->ir->fr_guard, sizeof(r->fr_guard));
        r->max_path = t1_ir_max_path(w->ir, T1_HEADER_BLOCK);
        r->loop = w->form->loop;
        r->frames = w->form->nfs > 1;
        r->calls = w->form->calls;
        for (unsigned i = 0; i < w->form->ninsns; ++i) {
            const struct t1_insn *ti = &w->form->insn[i];
            if (ti->cls != T1_IC_RET || w->form->fs[ti->fs].call)
                continue;
            unsigned sof = w->form->fs[ti->fs_taken].cfm & 127;
            r->observed_returns = true;
            if (sof > r->return_sof)
                r->return_sof = (uint8_t)sof;
        }
        r->policy = ctx->background;
        r->nsites = w->ir->nsites;
        r->ninsns = w->form->ninsns;
        r->nblocks = w->m->nblocks;
        r->loop_start = w->e.loop_start;
        r->loop_end = w->e.loop_end;
        if (!t1_frame_build(r, w->m, &w->e)) {
            why = "frame tables";
            break;
        }
        fill_report(report, r, w->m, &w->e);
        report->end_reason = w->form->end_reason;
        report->end_ip = w->form->end_ip;
        report->end_slot = w->form->end_slot;
        r->report = *report;
        ctx->link = t1_region_link(r);
        ctx->stage = COMPILE_PUBLISH;
        return true;
    }
    case COMPILE_PUBLISH:
        status = publish(ctx, ctx->region, &w->e, &why);
        if (status == T1_NO_MEMORY && !refused_since(refusals))
            ctx->defer = T1_DEFER_CHUNK;
        break;
    default:
        hal_panic("tier1: invalid compile stage");
    }
    if (status == T1_REJECTED && refused_since(refusals))
        status = T1_NO_MEMORY;
    if (status == T1_RETRY && ++ctx->attempts < 8) {
        if (ctx->region)
            t1_region_discard(ctx->region);
        ctx->region = NULL;
        free_work(ctx);
        ctx->max = T1_MAX_INSNS;
        ctx->shrinks = 0;
        ctx->stage = COMPILE_FORM;
        return true;
    }
    if (why)
        reject(report, why, rq->ip, rq->slot);
    return finish(ctx, status, rep);
}

void t1_compile_dropped(const struct t1_compile_request *rq)
{
    t1_head_moved(rq->ip, rq->slot);
    __atomic_add_fetch(&t1_sequence, 1, __ATOMIC_RELEASE);
}

void t1_compile_cancel(struct t1_compile_ctx *ctx)
{
    if (!ctx)
        return;
    for (unsigned i = 0; i < ctx->npending; ++i) {
        t1_head_moved(ctx->pending[i]->c.ip, ctx->pending[i]->c.slot);
        t1_region_discard(ctx->pending[i]);
    }
    if (ctx->npending) {
        ctx->npending = 0;
        __atomic_add_fetch(&t1_sequence, 1, __ATOMIC_RELEASE);
    }
    if (ctx->busy) {
        reject(&ctx->report, "compile cancelled", ctx->rq.ip, ctx->rq.slot);
        ctx->stage = COMPILE_CANCELLED;
        finish(ctx, T1_RETRY, NULL);
    }
    if (ctx->open_chunk != UINT32_MAX)
        t1_arena_close_background(&ctx->open_chunk);
}

enum t1_compile_status t1_compile_request(struct t1_compile_ctx *ctx,
                                          const struct t1_compile_request *rq,
                                          struct t1_compile_report *rep)
{
    struct t1_compile_report local;
    if (!rep)
        rep = &local;
    memset(rep, 0, sizeof(*rep));
    if (!t1_ready) {
        reject(rep, "tier1 not initialized", rq->ip, rq->slot);
        return T1_REJECTED;
    }
    if (!t1_compile_begin(ctx, rq, false))
        return T1_RETRY;
    while (t1_compile_step(ctx, rep)) {
    }
    return ctx->status;
}

bool t1_request_make(struct t1_vcpu *tv, gpa_t entry_pa, bool loop_head,
                     struct t1_compile_request *rq)
{
    const char *reason;
    if (!t1_request_from_vcpu(tv->v, &tv->opt, hostwin_bits(tv->hw), rq, &reason))
        return false;
    rq->loop_head = loop_head;
    /* Tier0's fetch translation of the same state gave the same PA. */
    return rq->entry_pa == entry_pa;
}

/* The product's synchronous compile at a hot head (t1_upper_hot,
 * t1_config.submit == NULL): the request on the executor's context. */
enum t1_compile_status t1_compile_hot(struct t1_vcpu *tv, const struct t1_compile_request *rq,
                                      struct t1_compile_report *rep)
{
    enum t1_compile_status st;
    memset(rep, 0, sizeof(*rep));
    if (!executor_ctx)
        executor_ctx = t1_compile_ctx_create();
    if (!executor_ctx) {
        reject(rep, "no memory", rq->ip, rq->slot);
        T1_COUNT(memory_deferrals, 1);
        T1_COUNT(deferrals_refused[T1_DEFER_BYTES], 1);
        st = T1_NO_MEMORY;
    } else {
        st = t1_compile_request(executor_ctx, rq, rep);
    }
    /* Formation, lift, optimizer, isel, allocation, emission and frame
     * rejects recur at the head; a transient formation reject does not. */
    if (st == T1_REJECTED && !rep->reject_transient)
        t1_hold_rejected(rq->entry_pa, rq->ip, rq->slot);
#ifdef IA64VMM_HOSTED
    if (compile_observer)
        compile_observer(tv->v, st, rep, compile_observer_arg);
#else
    (void)tv;
#endif
    return st;
}

enum t1_compile_status t1_compile_at(struct t1_vcpu *tv, const struct t1_compile_options *opt,
                                     struct t1_compile_report *rep)
{
    struct vcpu *v = tv->v;
    struct t1_compile_report local;
    if (!rep)
        rep = &local;
    memset(rep, 0, sizeof(*rep));
    if (!t1_ready) {
        reject(rep, "tier1 not initialized", v->s.ip, v->s.slot);
        return T1_REJECTED;
    }
    enum t1_compile_status st;
    struct t1_compile_request rq;
    const char *reason;
    if (!t1_request_from_vcpu(v, opt, hostwin_bits(tv->hw), &rq, &reason)) {
        reject(rep, reason, v->s.ip, v->s.slot);
        T1_COUNT(rejects, 1);
        T1_COUNT(rejects_by_stage[T1_REJECT_REQUEST], 1);
        st = T1_REJECTED;
    } else {
        if (!executor_ctx)
            executor_ctx = t1_compile_ctx_create();
        if (!executor_ctx) {
            reject(rep, "no memory", v->s.ip, v->s.slot);
            T1_COUNT(memory_deferrals, 1);
            T1_COUNT(deferrals_refused[T1_DEFER_BYTES], 1);
            st = T1_NO_MEMORY;
        } else {
            st = t1_compile_request(executor_ctx, &rq, rep);
        }
    }
#ifdef IA64VMM_HOSTED
    if (compile_observer)
        compile_observer(v, st, rep, compile_observer_arg);
#endif
    return st;
}
