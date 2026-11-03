/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Window misses bind into a temporary. If the bind epoch changed, invalidate
 * all site tags before publishing the current tag last; no bind occurs before
 * the retried access. hostwin_resolve never rebinds.
 *
 * The trap client is async-signal-safe and touches only the faulting CPU/thread's
 * execution context. Accept faults only during region execution at a recorded
 * access/force RIP; otherwise chain to the previous handler. Capture GPRs,
 * RFLAGS and resume RIP, then redirect to trap_entry for normal-context resolve.
 * Force resumes after its counter; an undo-log pre-load resolves with the
 * store's direction/width and resumes at the pre-load before the store.
 */
#include "engine/tier1/region.h"
#include "engine/hostwin/hostwin.h"

#ifdef IA64VMM_HOSTED
_Thread_local struct t1_exec *volatile t1_current_context;
_Thread_local bool t1_current_pinned;
unsigned t1_current_count;
#else
struct t1_exec *volatile t1_current_context[T1_CPU_SLOTS];
#endif

bool t1_trap_client(struct hostwin_trap *t)
{
    struct t1_exec *x = t1_current();
    if (!x || !x->region)
        return false;
    const struct t1_region *r = x->region;
    uintptr_t code = (uintptr_t)r->code;
    if (t->rip < code || t->rip >= code + r->code_size)
        return false;
    uint32_t off = (uint32_t)(t->rip - code);
    unsigned lo = 0, hi = r->nrip;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (r->rip[mid].offset < off)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= r->nrip || r->rip[lo].offset != off)
        return false;
    const struct t1_rip *e = &r->rip[lo];
    const struct t1_point *p = &r->point[e->point];
    for (unsigned k = 0; k < 16; ++k)
        x->regs[k] = t->gpr[k];
    x->rflags = t->rflags;
    x->point = e->point;
    x->rip = code + (e->kind == T1_RIP_FORCE ? p->force_resume : e->offset);
    x->tv->trap_kind = e->kind == T1_RIP_FORCE ? 2 : e->kind == T1_RIP_PRE ? 3 : 1;
    x->tv->trap_rip = t->rip;
    t->rip = t1_stub.trap_entry;
    return true;
}

/* Clear retry metadata so the next exit, checkpoint or site does not inherit
 * its path, forced flag or address. */
static uint32_t resume(struct t1_vcpu *tv)
{
    tv->pending = (struct t1_pending){T1V_PATH_EXIT_STUB, false, false, false, 0, 0};
    return T1_RESUME_RETRY;
}

uint32_t t1_trap_resolve(struct t1_exec *x)
{
    struct t1_vcpu *tv = x->tv;
    const struct t1_region *r = x->region;
    const struct t1_point *p = &r->point[x->point];
    uint8_t kind = tv->trap_kind;
    tv->trap_kind = 0;
    T1_COUNT(traps, 1);
    if (kind == 3)
        T1_COUNT(undo_pre_traps, 1);
    tv->pending =
        (struct t1_pending){T1V_PATH_TRAP, kind == 2, false, p->access_write != 0, 0, tv->trap_rip};
    if (kind == 2) {
#ifdef IA64VMM_HOSTED
        /* A forced trap follows the armed action without hostwin_resolve,
         * so a forced exit has no translation side effect. */
        struct t1_arm *a = t1_vh_armed(tv, r, x->point);
        if (!a || (a->action != T1V_TRAP_RETRY && a->action != T1V_TRAP_EXIT)) {
            /* Not armed for a trap (cannot happen): exit, the lower tier
             * executes the access. */
            return T1_RESUME_EXIT;
        }
        a->fired = true;
        if (a->action == T1V_TRAP_RETRY) {
            t1_vh_record(tv, r, x->point, false, T1V_RETRY);
            return resume(tv);
        }
        if (a->before)
            a->before(x->v, a->arg);
#endif
        /* Product builds compile no verification region, so no forced
         * trap exists; exit if one ever arrives. */
        return T1_RESUME_EXIT;
    }
    uint64_t gva = x->regs[p->addr_reg];
#if defined(IA64VMM_HOSTED) || defined(IA64VMM_TIER1_CHECKED)
    /* Checked lockstep: the records so far, before resolve can change a
     * shadow. */
    if (r->c.flags & T1_COMPILE_UNDO_LOG)
        t1_ck_collect(tv);
#endif
    enum hostwin_exit_reason why = HW_EXIT_GUEST_FAULT;
    enum hostwin_fill fill =
        hostwin_resolve(x->hw, x->v, gva, p->access_size, p->access_write != 0, &why);
    tv->pending.gva = gva;
    if (fill == HOSTWIN_MAPPED) {
        T1_COUNT(trap_fills, 1);
        tv->pending.translated = (x->v->s.psr & IA64_PSR_DT) != 0;
#ifdef IA64VMM_HOSTED
        t1_vh_record(tv, r, x->point, false, T1V_RETRY);
#endif
        return resume(tv);
    }
    /* hostwin.h: the access check ran iff PSR.dt = 1 and why is not one of
     * the checks made before it (injection, stale slot). */
    tv->pending.translated =
        (x->v->s.psr & IA64_PSR_DT) && why != HW_EXIT_INJECTED && why != HW_EXIT_STALE_SLOT;
    T1_COUNT(site_exits, 1);
    return T1_RESUME_EXIT;
}

unsigned t1_site_base(unsigned nsites, uint32_t flags)
{
    static unsigned next;
    if ((flags & T1_COMPILE_VERIFY) || !nsites || nsites > T1_SITE_SLOTS)
        return 0;
    unsigned old = __atomic_load_n(&next, __ATOMIC_RELAXED), base;
    do
        base = old + nsites <= T1_SITE_SLOTS ? old : 0;
    while (!__atomic_compare_exchange_n(&next, &old, base + nsites, false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED));
    return base;
}

uint32_t t1_window_miss(struct t1_exec *x)
{
    struct t1_vcpu *tv = x->tv;
    const struct t1_region *r = x->region;
    const struct t1_point *p = &r->point[x->point];
    uint64_t gva = x->regs[p->addr_reg];
    T1_COUNT(window_misses, 1);
    unsigned slot = r->site_base + p->site;
    /* A valid tag in the slot: another window, published by this region or
     * by another region whose range shares the slot. */
    if (x->site[slot].tag != HOSTWIN_TAG_INVALID) {
        if (x->site_owner[slot] == r->id)
            T1_COUNT(window_misses_same_region, 1);
        else
            T1_COUNT(window_misses_other_region, 1);
    }
    tv->pending =
        (struct t1_pending){T1V_PATH_MISS_STUB, false, false, p->access_write != 0, gva, 0};
#ifdef IA64VMM_HOSTED
    if (r->c.flags & T1_COMPILE_VERIFY) {
        struct t1_arm *a = t1_vh_armed(tv, r, x->point);
        if (a && a->action == T1V_MISS_EXIT && !a->fired && x->force[x->point] == 0) {
            a->fired = true;
            tv->pending.forced = true;
            if (a->before)
                a->before(x->v, a->arg);
            return T1_RESUME_EXIT;
        }
    }
#endif
#if defined(IA64VMM_HOSTED) || defined(IA64VMM_TIER1_CHECKED)
    /* Checked lockstep: the records so far, before a bind can drop the
     * shadows of a slot. */
    if (r->c.flags & T1_COMPILE_UNDO_LOG)
        t1_ck_collect(tv);
#endif
    /* Unaligned: the lower tier raises Unaligned Data Reference. */
    if (gva & (uint64_t)(p->access_size - 1)) {
        T1_COUNT(site_exits, 1);
        return T1_RESUME_EXIT;
    }
    struct hostwin_tag tmp;
    if (hostwin_bind(x->hw, x->v, gva, &tmp) == HOSTWIN_SLOW) {
        T1_COUNT(site_exits, 1);
        return T1_RESUME_EXIT;
    }
    uint64_t e = hostwin_bind_epoch(x->hw);
    if (e != x->observed_epoch)
        for (unsigned k = 0; k < T1_SITE_SLOTS; ++k)
            x->site[k].tag = HOSTWIN_TAG_INVALID;
    x->observed_epoch = e;
    x->site[slot] = tmp; /* the current site last */
    x->site_owner[slot] = r->id;
#ifdef IA64VMM_HOSTED
    t1_vh_record(tv, r, x->point, false, T1V_RETRY);
#endif
    return resume(tv);
}
