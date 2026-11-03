/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Verification hooks require T1_COMPILE_VERIFY (store logs: STORE_LOG).
 * Site/helper force counters decrement once per dynamic occurrence, before
 * the tag check; retries resume after the decrement. Zero forces a poison
 * load for TRAP, an invalid-tag miss for MISS_EXIT, or a helper exit.
 *
 * Counts are per vCPU/point and reset on state restore; arms survive restore
 * and count from arming. Checkpoints count backedges. Restore observers run
 * after materialization for frame exits, and at retry for bind/fill/TRAP_RETRY.
 */
#include "engine/tier1/region.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"

/* Hosted builds only: no verification symbol reaches build/vmm.elf;
 * product builds compile no verification region
 * (t1_test_set_compile_options is hosted-only too). */
#ifdef IA64VMM_HOSTED
static t1_restore_fn restore_observer;
static void *restore_arg;
static t1_store_fn store_observer;
static void *store_arg;
static t1_entry_hook_fn entry_hook;
static void *entry_arg;

static struct t1_occ *occ_table(struct t1_vcpu *tv, const struct t1_region *r, bool create)
{
    for (unsigned k = 0; k < tv->nocc; ++k)
        if (tv->occ[k].region == r->id)
            return &tv->occ[k];
    if (!create)
        return NULL;
    if (tv->nocc >= sizeof(tv->occ) / sizeof(tv->occ[0])) {
        /* Reuse the oldest table. */
        hal_free_pages(tv->occ[0].count, tv->occ[0].pages);
        memmove(&tv->occ[0], &tv->occ[1], (tv->nocc - 1) * sizeof(tv->occ[0]));
        --tv->nocc;
    }
    struct t1_occ *o = &tv->occ[tv->nocc];
    o->pages = (r->npoints * sizeof(uint64_t) + 4095) / 4096;
    o->count = hal_alloc_pages(o->pages ? o->pages : 1, HAL_PAGE_ZERO);
    if (!o->count)
        return NULL;
    o->region = r->id;
    o->iterations = 0;
    ++tv->nocc;
    return o;
}

void t1_vh_reset(struct t1_vcpu *tv)
{
    for (unsigned k = 0; k < tv->nocc; ++k)
        hal_free_pages(tv->occ[k].count, tv->occ[k].pages ? tv->occ[k].pages : 1);
    tv->nocc = 0;
}

struct t1_arm *t1_vh_armed(struct t1_vcpu *tv, const struct t1_region *r, uint32_t point)
{
    for (unsigned k = 0; k < T1_MAX_ARMS; ++k) {
        struct t1_arm *a = &tv->arm[k];
        if (a->active && !a->fired && a->region == r->id && a->point == point)
            return a;
    }
    return NULL;
}

static bool counted(const struct t1_point *p)
{
    return p->kind == T1_PT_SITE || p->kind == T1_PT_HELPER;
}

void t1_vh_entry(struct t1_vcpu *tv, struct t1_region *r)
{
    struct t1_exec *x = &tv->x;
    if (r->c.flags & T1_COMPILE_STORE_LOG)
        x->slog_n = x->slog_total = 0;
    if (!(r->c.flags & T1_COMPILE_VERIFY))
        return;
    x->poison = hostwin_poison_va(tv->hw);
    for (uint32_t k = 0; k < r->npoints; ++k) {
        const struct t1_point *p = &r->point[k];
        uint64_t f = 0;
        if (counted(p)) {
            struct t1_arm *a = t1_vh_armed(tv, r, k);
            if (a) {
                f = a->occurrence;
                if (a->action == T1V_MISS_EXIT)
                    x->poison = (uint64_t)(uintptr_t)&x->poison;
            }
        }
        x->force[k] = f;
        tv->force_initial[k] = f;
    }
}

uint64_t t1_vh_occurrence(struct t1_vcpu *tv, const struct t1_region *r, uint32_t point)
{
    struct t1_occ *o = occ_table(tv, r, true);
    uint64_t base = o ? o->count[point] : 0;
    const struct t1_point *p = &r->point[point];
    if (counted(p))
        return base + (tv->force_initial[point] - tv->x.force[point]);
    return base + 1; /* an exit or checkpoint: this exit */
}

void t1_vh_exit(struct t1_vcpu *tv, struct t1_region *r)
{
    struct t1_exec *x = &tv->x;
    if (!(r->c.flags & T1_COMPILE_VERIFY))
        return;
    struct t1_occ *o = occ_table(tv, r, true);
    uint64_t *c = o ? o->count : NULL;
    uint64_t back_edges = 0;
    for (uint32_t k = 0; k < r->npoints; ++k) {
        const struct t1_point *p = &r->point[k];
        uint64_t delta = tv->force_initial[k] - x->force[k];
        if (p->kind == T1_PT_CHECKPOINT)
            back_edges += delta;
        if (!c)
            continue;
        if (counted(p))
            c[k] += delta;
        else if (k == x->point && p->kind != T1_DEAD_POINT)
            c[k] += 1;
        struct t1_arm *a = counted(p) ? t1_vh_armed(tv, r, k) : NULL;
        if (a)
            a->occurrence = x->force[k];
    }
    if (o)
        o->iterations += 1 + back_edges;
}

void t1_vh_record(struct t1_vcpu *tv, const struct t1_region *r, uint32_t point, bool materialized,
                  uint8_t outcome)
{
    if (!restore_observer || !(r->c.flags & T1_COMPILE_VERIFY))
        return;
    uint64_t back_edges = 0;
    for (uint32_t k = 0; k < r->npoints; ++k)
        if (r->point[k].kind == T1_PT_CHECKPOINT)
            back_edges += tv->force_initial[k] - tv->x.force[k];
    const struct t1_pending *pd = &tv->pending;
    struct t1_occ *o = occ_table(tv, r, true);
    struct t1_restore_record rec = {
        .region_id = r->id,
        .point_id = point,
        .occurrence = t1_vh_occurrence(tv, r, point),
        .iteration = (o ? o->iterations : 0) + 1 + back_edges,
        .host_rip = pd->host_rip,
        .path = (enum t1_restore_path)pd->path,
        .forced = pd->forced,
        .outcome = outcome,
        .materialized = materialized,
        .retired = tv->v->retired,
        .translated = pd->translated,
        .access_gva = pd->gva,
        .access_write = pd->write,
    };
    restore_observer(tv, tv->v, &rec, restore_arg);
}

void t1_vh_corrupt(struct t1_vcpu *tv, const struct t1_region *r, uint32_t point)
{
    for (unsigned k = 0; k < T1_MAX_ARMS; ++k) {
        struct t1_corrupt *c = &tv->corrupt[k];
        if (!c->active || c->region != r->id || c->point != point)
            continue;
        if (t1_vh_occurrence(tv, r, point) != c->occurrence)
            continue;
        /* The home at the region's entry frame base: a stacked element
         * (E_STK) where materialization wrote it. */
        struct vcpu *v = tv->v;
        uint64_t old;
        if (!t1_home_read_at(v, tv->x.entry_bof, c->element, &old))
            continue;
        t1_home_write_at(v, tv->x.entry_bof, c->element, old ^ c->mask);
        c->active = false;
    }
}

bool t1_vh_store_overflow(const struct t1_vcpu *tv)
{
    return tv->x.slog_total > T1_SLOG_ENTRIES;
}

void t1_vh_drain_stores(struct t1_vcpu *tv, const struct t1_region *r)
{
    struct t1_exec *x = &tv->x;
    if (!(r->c.flags & T1_COMPILE_STORE_LOG))
        return;
    uint64_t n = x->slog_total < T1_SLOG_ENTRIES ? x->slog_total : T1_SLOG_ENTRIES;
    uint64_t first = (x->slog_n - n) & T1_SLOG_MASK;
    for (uint64_t k = 0; k < n; ++k) {
        uint64_t i = (first + k) & T1_SLOG_MASK;
        uint32_t point = x->slog_point[i];
        if (point < r->npoints && !r->point[point].access_write)
            continue;
        uint8_t width = point < r->npoints ? r->point[point].access_size : 0;
        uint64_t seq = tv->store_seq++;
        if (store_observer) {
            struct t1_store_record rec = {r->id,
                                          point,
                                          x->slog_addr[i],
                                          x->slog_val[i],
                                          seq,
                                          width,
                                          (r->c.flags & T1_COMPILE_UNDO_LOG) != 0,
                                          x->slog_pre[i]};
            store_observer(&rec, store_arg);
        }
    }
    x->slog_n = 0;
    x->slog_total = 0;
}

void t1_vh_entry_hook(struct t1_vcpu *tv, const struct t1_region *r)
{
    if (entry_hook)
        entry_hook(tv, r->id, entry_arg);
}

bool t1_test_force(struct t1_vcpu *tv, uint32_t region_id, uint32_t point_id, uint64_t occurrence,
                   enum t1_force_action action, t1_before_resolve_fn before, void *arg)
{
    const struct t1_region *r = t1_region_get(region_id);
    if (!r || !(r->c.flags & T1_COMPILE_VERIFY) || point_id >= r->npoints || !occurrence)
        return false;
    const struct t1_point *p = &r->point[point_id];
    bool site = p->kind == T1_PT_SITE, helper = p->kind == T1_PT_HELPER;
    if (helper != (action == T1_FORCE_HELPER_EXIT) || (!site && !helper))
        return false;
    /* x->poison is per entry: a MISS_EXIT and a TRAP arm cannot share a region. */
    for (unsigned k = 0; k < T1_MAX_ARMS; ++k) {
        const struct t1_arm *a = &tv->arm[k];
        if (!a->active || a->fired || a->region != region_id || a->point == point_id)
            continue;
        bool miss_a = a->action == T1V_MISS_EXIT, miss_b = action == T1_FORCE_MISS_EXIT;
        if (a->action != T1V_HELPER_EXIT && action != T1_FORCE_HELPER_EXIT && miss_a != miss_b)
            return false;
    }
    struct t1_arm *slot = NULL;
    for (unsigned k = 0; k < T1_MAX_ARMS && !slot; ++k)
        if (tv->arm[k].active && tv->arm[k].region == region_id && tv->arm[k].point == point_id)
            slot = &tv->arm[k];
    for (unsigned k = 0; k < T1_MAX_ARMS && !slot; ++k)
        if (!tv->arm[k].active || tv->arm[k].fired)
            slot = &tv->arm[k];
    if (!slot)
        return false;
    *slot =
        (struct t1_arm){true, false, region_id, point_id, occurrence, (uint8_t)action, before, arg};
    return true;
}

void t1_test_set_restore_observer(t1_restore_fn fn, void *arg)
{
    restore_observer = fn;
    restore_arg = arg;
}

void t1_test_set_store_observer(t1_store_fn fn, void *arg)
{
    store_observer = fn;
    store_arg = arg;
}

void t1_test_set_entry_hook(t1_entry_hook_fn fn, void *arg)
{
    entry_hook = fn;
    entry_arg = arg;
}

bool t1_test_corrupt(struct t1_vcpu *tv, uint32_t region_id, uint32_t point_id, uint64_t occurrence,
                     uint16_t element, uint64_t mask)
{
    const struct t1_region *r = t1_region_get(region_id);
    if (!r || !(r->c.flags & T1_COMPILE_VERIFY) || point_id >= r->npoints || !element ||
        (element == T1_ELEMENT_MEM || element >= T1_N_ELEMENTS))
        return false;
    for (unsigned k = 0; k < T1_MAX_ARMS; ++k)
        if (!tv->corrupt[k].active) {
            tv->corrupt[k] =
                (struct t1_corrupt){true, region_id, point_id, occurrence, element, mask};
            return true;
        }
    return false;
}
#endif
