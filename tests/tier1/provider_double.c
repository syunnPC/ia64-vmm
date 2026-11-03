/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Interpreter-stepping double of the Tier1 verification API (provider.h).
 * It tests restoration-driver plumbing and supplies no Tier1 restoration evidence.
 * A region spans the header to its br.cloop; point 0 is the checkpoint,
 * followed by a SITE per integer memory instruction in address order.
 * Count occurrences only when the instruction's predicate is true.
 *
 * TRAP_RETRY records a retry without materialization, then executes the access.
 * TRAP_EXIT/MISS_EXIT run the before callback and any armed corruption, then
 * record materialized state before the instruction, which the lower tier executes.
 * Region entry invokes the entry hook. A kick or insufficient budget for one
 * loop pass at the header produces a materialized CHECKPOINT record.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/provider.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/storelog.h"
#include "tests/tier1/bof.h"

#define DBL_REGIONS 4u
#define DBL_POINTS 64u
#define DBL_ARMS 16u
#define DBL_EXITS 256u
#define DBL_MAX_SCAN 256u

struct dbl_region {
    uint64_t header_ip, end_ip; /* [header bundle, back-edge bundle + 16) */
    unsigned header_slot, npoints, insns;
    struct t1_point_info point[DBL_POINTS];
    uint64_t iterations;
};
struct dbl_arm {
    bool active, is_corrupt;
    uint32_t region, point;
    uint64_t occurrence, seen;
    enum t1_force_action action;
    t1_before_resolve_fn before;
    void *arg;
    uint16_t element;
    uint64_t mask;
};
struct dbl {
    struct vcpu *v;
    struct dbl_region region[DBL_REGIONS];
    unsigned nregions;
    struct dbl_arm arm[DBL_ARMS];
    bool kicked, entered;
    int inside; /* region the vCPU is in, or -1 */
    t1_restore_fn restore;
    void *restore_arg;
    t1_entry_hook_fn entry;
    void *entry_arg;
    struct t1_exit_info exits[DBL_EXITS];
    unsigned nexits;
    struct t1_stats stats;
};

static void *create(struct vcpu *v)
{
    struct dbl *d = calloc(1, sizeof(*d));
    T1H_CHECK(d != NULL);
    d->v = v;
    d->inside = -1;
    return d;
}

static void destroy(void *ctx)
{
    free(ctx);
}

static void restored(void *ctx)
{
    struct dbl *d = ctx;
    d->kicked = false;
    d->entered = false;
    d->inside = -1;
}

static void kick(void *ctx)
{
    __atomic_store_n(&((struct dbl *)ctx)->kicked, true, __ATOMIC_RELEASE);
}

static unsigned load_width(Ia64Opcode op)
{
    switch (op) {
    case IA64_OP_LD1:
        return 1;
    case IA64_OP_LD2:
        return 2;
    case IA64_OP_LD4:
        return 4;
    case IA64_OP_LD8:
        return 8;
    default:
        return 0;
    }
}

static enum t1_compile_status compile(void *ctx, const struct t1_compile_options *opt,
                                      struct t1_compile_report *report)
{
    (void)opt;
    struct dbl *d = ctx;
    struct vcpu *v = d->v;
    memset(report, 0, sizeof(*report));
    for (unsigned i = 0; i < d->nregions; ++i)
        if (d->region[i].header_ip == v->s.ip && d->region[i].header_slot == v->s.slot) {
            report->region_id = i;
            report->points = d->region[i].npoints;
            return T1_COMPILED;
        }
    if (d->nregions == DBL_REGIONS)
        return T1_NO_MEMORY;
    struct dbl_region *r = &d->region[d->nregions];
    memset(r, 0, sizeof(*r));
    r->header_ip = v->s.ip;
    r->header_slot = v->s.slot;
    r->point[r->npoints++] = (struct t1_point_info){
        .point_id = 0, .kind = T1_PT_CHECKPOINT, .ip = v->s.ip, .slot = v->s.slot};
    uint64_t ip = v->s.ip;
    unsigned slot = v->s.slot;
    for (unsigned n = 0; n < DBL_MAX_SCAN; ++n) {
        Ia64Instruction in;
        if (storelog_decode(v, ip, slot, &in)) {
            unsigned w = load_width(in.opcode), sw = storelog_store_width(in.opcode);
            if ((w || sw) && in.opcode != IA64_OP_ST8SPILL) {
                if (r->npoints == DBL_POINTS)
                    return T1_REJECTED;
                r->point[r->npoints] = (struct t1_point_info){.point_id = r->npoints,
                                                              .kind = T1_PT_SITE,
                                                              .ip = ip,
                                                              .slot = slot,
                                                              .insn_index = n,
                                                              .access_size = w ? w : sw,
                                                              .access_write = sw != 0};
                ++r->npoints;
            }
            if (in.opcode == IA64_OP_BR_CLOOP &&
                ip + (uint64_t)in.operands.branch.displacement == r->header_ip) {
                r->end_ip = ip + 16;
                r->insns = n + 1;
                report->region_id = d->nregions++;
                report->insns = r->insns;
                report->sites = r->npoints - 1;
                report->points = r->npoints;
                return T1_COMPILED;
            }
        }
        if (++slot == 3) {
            slot = 0;
            ip += 16;
        }
    }
    report->reject_reason = "double: no back-edge to the header within 256 slots";
    return T1_REJECTED;
}

static unsigned point_count(void *ctx, uint32_t region)
{
    struct dbl *d = ctx;
    return region < d->nregions ? d->region[region].npoints : 0;
}

static bool point_get(void *ctx, uint32_t region, unsigned index, struct t1_point_info *out)
{
    struct dbl *d = ctx;
    if (region >= d->nregions || index >= d->region[region].npoints)
        return false;
    *out = d->region[region].point[index];
    return true;
}

static struct dbl_arm *new_arm(struct dbl *d)
{
    for (unsigned i = 0; i < DBL_ARMS; ++i)
        if (!d->arm[i].active)
            return &d->arm[i];
    return NULL;
}

static bool force(void *ctx, uint32_t region, uint32_t point, uint64_t occurrence,
                  enum t1_force_action action, t1_before_resolve_fn before, void *arg)
{
    struct dbl *d = ctx;
    if (region >= d->nregions || point >= d->region[region].npoints || !occurrence ||
        d->region[region].point[point].kind != T1_PT_SITE || action == T1_FORCE_HELPER_EXIT)
        return false;
    struct dbl_arm *a = new_arm(d);
    if (!a)
        return false;
    *a = (struct dbl_arm){.active = true,
                          .region = region,
                          .point = point,
                          .occurrence = occurrence,
                          .action = action,
                          .before = before,
                          .arg = arg};
    return true;
}

static bool corrupt(void *ctx, uint32_t region, uint32_t point, uint64_t occurrence,
                    uint16_t element, uint64_t mask)
{
    struct dbl *d = ctx;
    if (region >= d->nregions || point >= d->region[region].npoints || !occurrence)
        return false;
    struct dbl_arm *a = new_arm(d);
    if (!a)
        return false;
    *a = (struct dbl_arm){.active = true,
                          .is_corrupt = true,
                          .region = region,
                          .point = point,
                          .occurrence = occurrence,
                          .element = element,
                          .mask = mask};
    return true;
}

static void set_restore_observer(void *ctx, t1_restore_fn fn, void *arg)
{
    struct dbl *d = ctx;
    d->restore = fn;
    d->restore_arg = arg;
}
static void set_store_observer(void *ctx, t1_store_fn fn, void *arg)
{
    (void)ctx;
    (void)fn;
    (void)arg; /* every store of the double is a lower-tier store */
}
static void set_entry_hook(void *ctx, t1_entry_hook_fn fn, void *arg)
{
    struct dbl *d = ctx;
    d->entry = fn;
    d->entry_arg = arg;
}
static unsigned exit_count(void *ctx)
{
    return ((struct dbl *)ctx)->nexits;
}
static bool exit_get(void *ctx, unsigned index, struct t1_exit_info *out)
{
    struct dbl *d = ctx;
    if (index >= d->nexits)
        return false;
    *out = d->exits[index];
    return true;
}
static void stats(void *ctx, struct t1_stats *out)
{
    *out = ((struct dbl *)ctx)->stats;
}
static struct hostwin_vcpu *hostwin(void *ctx)
{
    (void)ctx;
    return NULL;
}

/* Element homes; a stacked GR/NaT element is frame-relative:
 * its home at v's frame base. */
static void corrupt_home(struct vcpu *v, uint16_t element, uint64_t mask)
{
    if (element >= 1 && element < 128)
        v->s.gr[t1t_phys(v->s.rse_bof, element)] ^= mask;
    else if (element > 128 && element < 256)
        v->s.nat[t1t_phys(v->s.rse_bof, element - 128u)] ^= (uint8_t)(mask & 1);
    else if (element > 256 && element < 320)
        v->s.pr ^= (mask & 1) << (element - 256);
    else if (element == T1_ELEMENT_LC)
        v->s.ar[AR_LC] ^= mask;
    else if (element == T1_ELEMENT_LSB)
        v->last_successful_bundle ^= mask;
}

static void record_exit(struct dbl *d, uint32_t region, uint32_t point, enum t1_point_kind kind)
{
    if (d->nexits < DBL_EXITS)
        d->exits[d->nexits++] = (struct t1_exit_info){region, point, kind, 0, true};
    ++d->stats.exits_by_kind[kind];
    d->entered = false;
}

static void emit(struct dbl *d, const struct t1_restore_record *r)
{
    if (d->restore)
        d->restore(NULL, d->v, r, d->restore_arg);
}

/* The action of one armed occurrence (a reached SITE). */
static void act(struct dbl *d, struct dbl_arm *a, unsigned region, const struct t1_point_info *p)
{
    struct vcpu *v = d->v;
    struct t1_restore_record r = {.region_id = region,
                                  .point_id = p->point_id,
                                  .occurrence = a->occurrence,
                                  .iteration = d->region[region].iterations,
                                  .host_rip = 0,
                                  .forced = true,
                                  .access_gva = 0,
                                  .access_write = p->access_write};
    if (a->action == T1_FORCE_TRAP_RETRY) {
        r.path = T1_PATH_TRAP;
        r.outcome = T1_OUTCOME_RETRY;
        emit(d, &r);
        return;
    }
    r.path = a->action == T1_FORCE_MISS_EXIT ? T1_PATH_MISS_STUB : T1_PATH_TRAP;
    r.outcome = T1_OUTCOME_EXIT;
    if (a->before)
        a->before(v, a->arg);
    /* wrong-old-value corruption of this very materialization. */
    for (unsigned i = 0; i < DBL_ARMS; ++i) {
        struct dbl_arm *c = &d->arm[i];
        if (c->active && c->is_corrupt && c->region == region && c->point == p->point_id &&
            c->seen == c->occurrence) {
            corrupt_home(v, c->element, c->mask);
            c->active = false;
        }
    }
    r.materialized = true;
    r.retired = v->retired;
    ++d->stats.site_exits;
    emit(d, &r);
    record_exit(d, region, p->point_id, T1_PT_SITE);
}

static bool qp_true(const struct vcpu *v, const struct t1_point_info *p)
{
    Ia64Instruction in;
    return storelog_decode(v, p->ip, p->slot, &in) && ia64_pr_read(&v->s, in.qp);
}

static struct vcpu_exit run(void *ctx, uint64_t budget)
{
    struct dbl *d = ctx;
    struct vcpu *v = d->v;
    d->nexits = 0;
    d->entered = false;
    uint64_t left = budget;
    while (left) {
        int now = -1;
        for (unsigned i = 0; i < d->nregions; ++i)
            if (v->s.ip >= d->region[i].header_ip && v->s.ip < d->region[i].end_ip)
                now = (int)i;
        if (now != d->inside)
            d->entered = false; /* left the loop range: next arrival enters */
        d->inside = now;
        for (unsigned i = 0; i < d->nregions; ++i) {
            struct dbl_region *r = &d->region[i];
            if (v->s.ip == r->header_ip && v->s.slot == r->header_slot) {
                if (!d->entered) {
                    d->entered = true;
                    ++d->stats.entries;
                    if (d->entry)
                        d->entry(NULL, i, d->entry_arg);
                }
                ++r->iterations;
                bool kicked = __atomic_exchange_n(&d->kicked, false, __ATOMIC_ACQ_REL);
                if (kicked || left < r->insns) {
                    struct t1_restore_record c = {.region_id = i,
                                                  .point_id = 0,
                                                  .occurrence = r->iterations,
                                                  .iteration = r->iterations,
                                                  .path = T1_PATH_EXIT_STUB,
                                                  .outcome = T1_OUTCOME_EXIT,
                                                  .materialized = true,
                                                  .retired = v->retired};
                    emit(d, &c);
                    record_exit(d, i, 0, T1_PT_CHECKPOINT);
                }
            }
            for (unsigned k = 1; k < r->npoints; ++k) {
                const struct t1_point_info *p = &r->point[k];
                if (p->ip != v->s.ip || p->slot != v->s.slot || !qp_true(v, p))
                    continue;
                for (unsigned j = 0; j < DBL_ARMS; ++j) {
                    struct dbl_arm *a = &d->arm[j];
                    if (a->active && a->region == i && a->point == k)
                        ++a->seen;
                }
                for (unsigned j = 0; j < DBL_ARMS; ++j) {
                    struct dbl_arm *a = &d->arm[j];
                    if (a->active && !a->is_corrupt && a->region == i && a->point == k &&
                        a->seen == a->occurrence) {
                        a->active = false;
                        act(d, a, i, p);
                    }
                }
            }
        }
        uint64_t before = v->retired;
        struct vcpu_exit e = ref_run(v, 1, NULL);
        d->stats.lower_retired += v->retired - before;
        ++d->stats.lower_steps;
        if (e.reason != VCPU_EXIT_BUDGET)
            return e;
        --left;
    }
    return (struct vcpu_exit){.reason = VCPU_EXIT_BUDGET, .ip = v->s.ip, .slot = v->s.slot};
}

static const struct t1h_provider dbl = {
    .name = "double",
    .evidence = false,
    .create = create,
    .destroy = destroy,
    .restored = restored,
    .run = run,
    .kick = kick,
    .compile = compile,
    .point_count = point_count,
    .point_get = point_get,
    .force = force,
    .corrupt = corrupt,
    .set_restore_observer = set_restore_observer,
    .set_store_observer = set_store_observer,
    .set_entry_hook = set_entry_hook,
    .exit_count = exit_count,
    .exit_get = exit_get,
    .stats = stats,
    .hostwin = hostwin,
};

const struct t1h_provider *t1h_provider_double(void)
{
    return &dbl;
}

bool t1h_run_to_loop_header(struct vcpu *v, uint64_t limit, uint64_t *header_ip)
{
    uint64_t header = UINT64_MAX;
    for (uint64_t n = 0; n < limit; ++n) {
        if (header != UINT64_MAX && v->s.ip == header && v->s.slot == 0) {
            *header_ip = header;
            return true;
        }
        Ia64Instruction in;
        if (header == UINT64_MAX && storelog_decode(v, v->s.ip, v->s.slot, &in) &&
            in.opcode == IA64_OP_BR_CLOOP && in.operands.branch.displacement < 0)
            header = v->s.ip + (uint64_t)in.operands.branch.displacement;
        if (ref_run(v, 1, NULL).reason != VCPU_EXIT_BUDGET)
            return false;
    }
    return false;
}
