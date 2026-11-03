/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded native edges. Active plans contain validated, epoch-pinned
 * regions. Stable-frame exits restore homes, perform all admission guards,
 * publish the successor, and refresh its entry image. Armed host-clock
 * timers use a HAL counter proof; policy folds and exit observations
 * remain at the ordinary boundary. */
#include "engine/tier1/region.h"
#include "ia64/interrupt.h"
#include "ia64/fp.h"
#include "ia64/pmu.h"
#include "ia64/fault-count.h"
#include "ia64vmm/string.h"

#if IA64VMM_TIER1_STATS
#define FAST_DECLINE(tv, kind) T1_COUNT_TV(tv, chain_native_decline_##kind, 1)
#define FAST_COUNT(tv, field) T1_COUNT_TV(tv, field, 1)
#else
#define FAST_DECLINE(tv, kind) ((void)0)
#define FAST_COUNT(tv, field) ((void)0)
#endif

static struct t1_fast_node *fast_find(struct t1_fast *f, const struct t1_region *r)
{
    for (unsigned k = 0; k < f->nnodes; ++k)
        if (f->node[k].region == r)
            return &f->node[k];
    return NULL;
}

static void fast_gr_homes(struct t1_fast_node *n, unsigned bof)
{
    n->nguard = 0;
    for (unsigned w = 0; w < 2; ++w) {
        uint64_t guarded = n->nat_guard[w];
        while (guarded) {
            unsigned gr = 64 * w + (unsigned)__builtin_ctzll(guarded);
            n->guard[n->nguard++] =
                (uint32_t)(offsetof(struct vcpu, s.nat) + t1_gr_home_at(bof, gr));
            guarded &= guarded - 1;
        }
    }
}

static struct t1_fast_node *fast_node(struct t1_vcpu *tv, struct t1_region *r, bool shape_proved)
{
    struct t1_fast *f = &tv->fast;
    struct vcpu *v = tv->v;
    struct t1_fast_node *found = fast_find(f, r);
    if (found)
        return found;
    if (f->nnodes == T1_FAST_NODES || !r->valid || !r->sealed ||
        (!shape_proved &&
         (r->c.flags || r->nhelper || !r->npages || r->npages > CODE_STAY_WITNESS_PAGES ||
          r->npoints > T1_FAST_POINTS || (r->nframe_in && !r->frame_in_offset))) ||
        (r->observed_returns && v->s.rse_dirty > IA64_N_STACKED_PHYS - r->return_sof)) {
        FAST_COUNT(tv, native_node_reject[f->nnodes == T1_FAST_NODES ? T1_NATIVE_NODE_CAPACITY
                                                                     : T1_NATIVE_NODE_SHAPE]);
        return NULL;
    }
    struct t1_window_slot *window = r->policy ? &tv->windows.slot[r->id % T1_WINDOW_SLOTS] : NULL;
    if (window && window->link != r->link) {
        FAST_COUNT(tv, native_node_reject[T1_NATIVE_NODE_WINDOW]);
        return NULL;
    }
    struct t1_fast_node *n = &f->node[f->nnodes];
    struct code_fetch fetch;
    if (!t1_fetch_context_cached(tv, r, &fetch)) {
        FAST_COUNT(tv, native_node_reject[T1_NATIVE_NODE_FETCH]);
        return NULL;
    }
    n->second = NULL;
    if (r->npages == 1) {
        if (!code_stay_witness_take(v, &r->dep[0], &fetch, &n->witness)) {
            FAST_COUNT(tv, native_node_reject[T1_NATIVE_NODE_WITNESS]);
            return NULL;
        }
    } else {
        struct code_stay_witness_group group;
        if (!code_stay_witness_group_take(v, r->dep, r->npages, &fetch, &group)) {
            FAST_COUNT(tv, native_node_reject[T1_NATIVE_NODE_GROUP]);
            return NULL;
        }
        n->witness = group.page[0];
        f->second[f->nnodes] = group.page[1];
        n->second = &f->second[f->nnodes];
    }
    n->region = r;
    n->window = window;
    n->entry = (uintptr_t)r->code + r->native_entry;
    n->frame_in = r->nframe_in ? (uintptr_t)r->code + r->frame_in_offset : 0;
    n->max_path = r->max_path;
    n->nat_guard[0] = r->nat_guard[0];
    n->nat_guard[1] = r->nat_guard[1];
    fast_gr_homes(n, tv->x.entry_bof);
    n->nfr_guard = 0;
    for (unsigned w = 0; w < 2; ++w) {
        uint64_t guarded = r->fr_guard[w];
        while (guarded) {
            unsigned fr = 64 * w + (unsigned)__builtin_ctzll(guarded);
            n->fr_guard[n->nfr_guard++] =
                (uint32_t)(offsetof(struct vcpu, s.fr) + fr * sizeof(struct ia64_fr));
            guarded &= guarded - 1;
        }
    }
    n->outgoing = 0;
    memset(n->edge, 0, sizeof(n->edge));
    ++f->nnodes;
    return n;
}

/* Called only after lifetime/context equality. CFM fixes logical guard
 * indices; BOF changes only their physical NaT homes. Every target must
 * also accept the new dirty partition before the plan can be activated. */
static bool fast_rebase(struct t1_vcpu *tv)
{
    struct t1_fast *f = &tv->fast;
    if (f->dirty != tv->x.entry_dirty)
        for (unsigned k = 0; k < f->nnodes; ++k) {
            const struct t1_region *r = f->node[k].region;
            if (r->observed_returns && tv->x.entry_dirty > IA64_N_STACKED_PHYS - r->return_sof)
                return false;
        }
    if (f->bof != tv->x.entry_bof)
        for (unsigned k = 0; k < f->nnodes; ++k)
            fast_gr_homes(&f->node[k], tv->x.entry_bof);
    f->bof = tv->x.entry_bof;
    f->dirty = tv->x.entry_dirty;
    return true;
}

/* Region shape and entry contracts are immutable for a link generation.
 * Retain their connections when the direct-mapped successor hint collides.
 * Sealing, window ownership, dirty partitions, fetch proofs and witnesses are tested
 * only by activation; a failure there never changes this topology. */
static struct t1_fast_connections *fast_connections(struct t1_vcpu *tv,
                                                    const struct t1_region *from, unsigned bits,
                                                    uint64_t retirement)
{
    if (!from->link || !from->fast_points)
        return NULL;
    struct t1_fast_connections *c = &tv->fast.connections[from->id % T1_FAST_CONNECTIONS];
    const struct vcpu *v = tv->v;
    bool same = c->from == from->link;
    if (!same)
        *c = (struct t1_fast_connections){.from = from->link};
    same = same && c->retirement == retirement && c->psr == v->s.psr && c->cfm == v->s.cfm &&
           c->cpu_model == (unsigned)v->cfg.cpu_model && c->hostwin_bits == bits &&
           c->icount == v->cfg.itc_icount;
    if (same)
        FAST_COUNT(tv, native_topology_hits);
    else {
        FAST_COUNT(tv, native_topology_misses);
        c->checked = c->eligible = 0;
        c->retirement = retirement;
        c->psr = v->s.psr;
        c->cfm = v->s.cfm;
        c->cpu_model = (unsigned)v->cfg.cpu_model;
        c->hostwin_bits = bits;
        c->icount = v->cfg.itc_icount;
    }
    uint32_t points = from->fast_points;
    while (points) {
        unsigned p = (unsigned)__builtin_ctz(points);
        uint32_t bit = UINT32_C(1) << p;
        points &= points - 1;
        const struct t1_successor *e = &tv->successors[t1_successor_index(from->link, p)];
        if (e->from == from->link && e->point == p) {
            if (c->target[p] != e->to) {
                c->target[p] = e->to;
                c->checked &= ~bit;
                c->eligible &= ~bit;
            }
        } else if (c->target[p])
            FAST_COUNT(tv, native_topology_retained);
        if (c->checked & bit)
            continue;
        c->checked |= bit;
        struct t1_region *to = t1_region_from_link(c->target[p]);
        const struct t1_point *point = &from->point[p];
        if (!c->target[p])
            FAST_COUNT(tv, native_node_reject[T1_NATIVE_NODE_SUCCESSOR]);
        else if (!to || !t1_region_matches_at(to, v, 0, bits, point->ip, point->slot) ||
                 (!(v->s.psr & IA64_PSR_IT) &&
                  to->c.entry_pa != (point->ip & ~(UINT64_C(1) << 63))))
            FAST_COUNT(tv, native_node_reject[T1_NATIVE_NODE_CONTRACT]);
        else if (to->c.flags || to->nhelper || !to->npages ||
                 to->npages > CODE_STAY_WITNESS_PAGES || to->npoints > T1_FAST_POINTS ||
                 (to->nframe_in && !to->frame_in_offset))
            FAST_COUNT(tv, native_node_reject[T1_NATIVE_NODE_SHAPE]);
        else
            c->eligible |= bit;
    }
    return c;
}

static struct t1_region *fast_target(struct t1_vcpu *tv, const struct t1_fast_node *n, unsigned p,
                                     unsigned bits, const struct t1_fast_connections *connections)
{
    /* The connection checked the complete contract and immutable shape
     * under this prepare's context. Preparation runs no guest instruction
     * or helper that can change those inputs. Resolve the generation again:
     * retirement may invalidate the target while witnesses are collected. */
    if (connections) {
        if (!(connections->eligible & (UINT32_C(1) << p)))
            return NULL;
        struct t1_region *to = t1_region_from_link(connections->target[p]);
        if (!to)
            FAST_COUNT(tv, native_node_reject[T1_NATIVE_NODE_CONTRACT]);
        return to;
    }
    const struct t1_region *from = n->region;
    const struct t1_successor *e = &tv->successors[t1_successor_index(from->link, p)];
    if (e->from != from->link || e->point != p) {
        FAST_COUNT(tv, native_node_reject[T1_NATIVE_NODE_SUCCESSOR]);
        return NULL;
    }
    struct t1_region *to = t1_region_from_link(e->to);
    const struct t1_point *point = &from->point[p];
    if (!to || !t1_region_matches_at(to, tv->v, 0, bits, point->ip, point->slot) ||
        (!(tv->v->s.psr & IA64_PSR_IT) && to->c.entry_pa != (point->ip & ~(UINT64_C(1) << 63)))) {
        FAST_COUNT(tv, native_node_reject[T1_NATIVE_NODE_CONTRACT]);
        return NULL;
    }
    return to;
}

static void fast_connect(struct t1_fast_node *n, unsigned p, struct t1_fast_node *target)
{
    const struct t1_point *point = &n->region->point[p];
    n->edge[p] =
        (struct t1_fast_edge){target, point, (uintptr_t)n->region->code + point->restore_offset};
    n->outgoing |= UINT32_C(1) << p;
}

/* Original entry guards anchor cycles at every C entry. Restoration kills
 * that proof for a written NaT unless its stored low bit is constant zero.
 * A target must satisfy the residual requirements of every incoming edge. */
static void fast_gr_guards(struct t1_fast *f, unsigned bof)
{
    uint64_t needed[T1_FAST_NODES][2] = {{0}};
    unsigned incoming = 0;
    for (unsigned k = 0; k < f->nnodes; ++k) {
        const struct t1_fast_node *n = &f->node[k];
        uint32_t edges = n->outgoing;
        while (edges) {
            unsigned p = (unsigned)__builtin_ctz(edges);
            edges &= edges - 1;
            const struct t1_fast_edge *e = &n->edge[p];
            if (!(e->target->region->nat_guard[0] | e->target->region->nat_guard[1]))
                continue;
            uint64_t zero[2] = {n->region->nat_guard[0], n->region->nat_guard[1]};
            for (unsigned j = 0; j < e->point->frame_restore; ++j) {
                const struct t1_frame_entry *a = &n->region->frame[e->point->frame_first + j];
                if (a->element < T1_ELEMENT_NAT(1) || a->element >= T1_ELEMENT_PR(0))
                    continue;
                unsigned gr = a->element - T1_ELEMENT_NAT(0);
                uint64_t bit = UINT64_C(1) << (gr % 64);
                zero[gr / 64] &= ~bit;
                if (a->kind == T1_LOC_CONST && !(a->value & 1))
                    zero[gr / 64] |= bit;
            }
            unsigned target = (unsigned)(e->target - f->node);
            incoming |= 1u << target;
            for (unsigned w = 0; w < 2; ++w)
                needed[target][w] |= e->target->region->nat_guard[w] & ~zero[w];
        }
    }
    for (unsigned k = 0; k < f->nnodes; ++k) {
        if (!(incoming & (1u << k)))
            continue;
        struct t1_fast_node *n = &f->node[k];
        n->nat_guard[0] = needed[k][0];
        n->nat_guard[1] = needed[k][1];
        fast_gr_homes(n, bof);
    }
}

/* Helper-free bodies change FR homes only through restoration. Reuse
 * admission's FR proof only when all incoming edges preserve both components.
 * Use original guard sets even for cycles; C entry checks the full set. */
static void fast_fr_guards(struct t1_fast *f)
{
    uint64_t guarded = 0;
    for (unsigned k = 0; k < f->nnodes; ++k)
        guarded |= f->node[k].region->fr_guard[0] | f->node[k].region->fr_guard[1];
    if (!guarded)
        return;
    uint64_t needed[T1_FAST_NODES][2] = {{0}};
    unsigned incoming = 0;
    for (unsigned k = 0; k < f->nnodes; ++k) {
        const struct t1_fast_node *n = &f->node[k];
        uint32_t edges = n->outgoing;
        while (edges) {
            unsigned p = (unsigned)__builtin_ctz(edges);
            edges &= edges - 1;
            const struct t1_fast_edge *e = &n->edge[p];
            if (!(e->target->region->fr_guard[0] | e->target->region->fr_guard[1]))
                continue;
            uint64_t preserved[2] = {n->region->fr_guard[0], n->region->fr_guard[1]};
            for (unsigned j = 0; j < e->point->frame_restore; ++j) {
                unsigned el = n->region->frame[e->point->frame_first + j].element, fr;
                if (el >= T1_ELEMENT_FR(2) && el <= T1_ELEMENT_FR(127))
                    fr = el - T1_ELEMENT_FR(0);
                else if (el >= T1_ELEMENT_FR_META(2) && el <= T1_ELEMENT_FR_META(127))
                    fr = el - T1_ELEMENT_FR_META(0);
                else
                    continue;
                preserved[fr / 64] &= ~(UINT64_C(1) << (fr % 64));
            }
            unsigned target = (unsigned)(e->target - f->node);
            incoming |= 1u << target;
            for (unsigned w = 0; w < 2; ++w)
                needed[target][w] |= e->target->region->fr_guard[w] & ~preserved[w];
        }
    }
    for (unsigned k = 0; k < f->nnodes; ++k) {
        if (!(incoming & (1u << k)))
            continue;
        struct t1_fast_node *n = &f->node[k];
        n->nfr_guard = 0;
        for (unsigned w = 0; w < 2; ++w)
            while (needed[k][w]) {
                unsigned fr = 64 * w + (unsigned)__builtin_ctzll(needed[k][w]);
                n->fr_guard[n->nfr_guard++] =
                    (uint32_t)(offsetof(struct vcpu, s.fr) + fr * sizeof(struct ia64_fr));
                needed[k][w] &= needed[k][w] - 1;
            }
    }
}

void t1_fast_prepare(struct t1_vcpu *tv, struct t1_region *r)
{
    struct t1_fast *f = &tv->fast;
    struct vcpu *v = tv->v;
    tv->x.fast = NULL;
    if (!r->fast_points || r->nhelper || !r->npages || r->npages > CODE_STAY_WITNESS_PAGES ||
        r->npoints > T1_FAST_POINTS || (r->nframe_in && !r->frame_in_offset) || t1_cfg.chain_off ||
        tv->opt.flags || !code_vcpu_is_registered(v) || !hostwin_can_reenter(tv->hw) ||
        ia64_pmu_active(v) || ia64_break_ips || v->trace_count ||
        v->cfg.engine == ENGINE_LOCKSTEP || v->s.rse_cfle || v->s.rse_missing) {
#if IA64VMM_TIER1_STATS
        if (!r->fast_points || r->nhelper || !r->npages || r->npages > CODE_STAY_WITNESS_PAGES ||
            r->npoints > T1_FAST_POINTS || (r->nframe_in && !r->frame_in_offset))
            FAST_DECLINE(tv, shape);
        else
            FAST_DECLINE(tv, context);
#endif
        return;
    }
#ifdef IA64VMM_HOSTED
    if (t1_test_publish_hook || t1_test_chain_hook || t1_test_region_hook) {
        FAST_DECLINE(tv, context);
        return;
    }
#endif
    /* Sample before dereferencing cached pointers or building a plan.
     * Later retirement is newer than our pin; earlier retirement rejects
     * reuse before dereference. Registration changes also advance the epoch. */
    uint64_t retirement = code_epoch_sample();
#if IA64VMM_TIER1_STATS
    bool rebase_failed = false;
#endif
    if (f->retirement == retirement && f->mmu_seq == v->mmu_seq && f->psr == v->s.psr &&
        f->cfm == v->s.cfm && f->bind_epoch == tv->x.observed_epoch &&
        f->token == v->code_entry_generation && f->iseq == v->mmu_iseq &&
        f->pta == v->s.cr[CR_PTA]) {
        for (unsigned k = 0; k < f->nnodes; ++k)
            if (f->node[k].region == r) {
                if (!fast_rebase(tv)) {
#if IA64VMM_TIER1_STATS
                    rebase_failed = true;
#endif
                    break;
                }
                if (f->node[k].outgoing)
                    tv->x.fast = &f->node[k];
                else
                    FAST_DECLINE(tv, context);
                T1_COUNT_TV(tv, chain_native_reuses, 1);
                return;
            }
    }
    unsigned bits = hostwin_bits(tv->hw);
    const struct t1_fast_connections *root = fast_connections(tv, r, bits, retirement);
    if (root && !root->eligible) {
        FAST_COUNT(tv, native_topology_negative);
        FAST_DECLINE(tv, context);
        return;
    }
#if IA64VMM_TIER1_STATS
    if (!f->nnodes)
        FAST_COUNT(tv, native_rebuild[T1_NATIVE_REBUILD_EMPTY]);
    else {
        FAST_COUNT(tv, native_discard_rebuild);
        bool changed = false;
#define KEY(field, value, reason)                                                                  \
    do {                                                                                           \
        if (f->field != (value)) {                                                                 \
            FAST_COUNT(tv, native_rebuild[T1_NATIVE_REBUILD_##reason]);                            \
            changed = true;                                                                        \
        }                                                                                          \
    } while (0)
        KEY(retirement, retirement, RETIREMENT);
        KEY(mmu_seq, v->mmu_seq, MMU);
        KEY(psr, v->s.psr, PSR);
        KEY(cfm, v->s.cfm, CFM);
        KEY(bind_epoch, tv->x.observed_epoch, BIND);
        KEY(token, v->code_entry_generation, TOKEN);
        KEY(iseq, v->mmu_iseq, ISEQ);
        KEY(pta, v->s.cr[CR_PTA], PTA);
#undef KEY
        if (!changed)
            FAST_COUNT(tv, native_rebuild[rebase_failed ? T1_NATIVE_REBUILD_REBASE
                                                        : T1_NATIVE_REBUILD_REGION]);
    }
#endif
    f->nnodes = 0;
    struct t1_fast_node *first = fast_node(tv, r, false);
    if (!first) {
        FAST_COUNT(tv, native_build_nodes[0]);
        FAST_COUNT(tv, native_build_edges[0]);
        FAST_DECLINE(tv, context);
        return;
    }
    unsigned edges = 0, scanned = 0;
    uint32_t deferred[T1_FAST_NODES] = {0};
    for (;;) {
        /* Preserve the one-page graph before a cold two-page successor
         * can consume its capacity. The initial node is never displaced. */
        for (; scanned < f->nnodes; ++scanned) {
            struct t1_fast_node *n = &f->node[scanned];
            const struct t1_fast_connections *connections =
                scanned ? fast_connections(tv, n->region, bits, retirement) : root;
            for (unsigned p = 0; p < n->region->npoints; ++p) {
                if (!(n->region->fast_points & (UINT32_C(1) << p)))
                    continue;
                struct t1_region *to = fast_target(tv, n, p, bits, connections);
                if (!to)
                    continue;
                struct t1_fast_node *target = fast_find(f, to);
                if (!target && to->npages == 2) {
                    deferred[scanned] |= UINT32_C(1) << p;
                    continue;
                }
                if (!target)
                    target = fast_node(tv, to, connections != NULL);
                if (!target)
                    continue;
                fast_connect(n, p, target);
                ++edges;
            }
        }
        bool added = false;
        for (unsigned k = 0; k < f->nnodes && !added; ++k) {
            struct t1_fast_node *n = &f->node[k];
            uint32_t pending = deferred[k];
            const struct t1_fast_connections *connections =
                pending ? fast_connections(tv, n->region, bits, retirement) : NULL;
            while (pending) {
                unsigned p = (unsigned)__builtin_ctz(pending);
                pending &= pending - 1;
                struct t1_region *to = fast_target(tv, n, p, bits, connections);
                struct t1_fast_node *target = to ? fast_find(f, to) : NULL;
                if (!target && f->nnodes == T1_FAST_NODES) {
                    FAST_COUNT(tv, native_node_reject[T1_NATIVE_NODE_CAPACITY]);
                    continue;
                }
                deferred[k] &= ~(UINT32_C(1) << p);
                if (!target && to) {
                    target = fast_node(tv, to, connections != NULL);
                    added = target != NULL;
                }
                if (target) {
                    fast_connect(n, p, target);
                    ++edges;
                }
                /* Expand this node's one-page descendants before the
                 * next two-page candidate. At most eight nodes are added. */
                if (added)
                    break;
            }
        }
        if (!added)
            break;
    }
#if IA64VMM_TIER1_STATS
    _Static_assert(T1_FAST_NODES == 8 && T1_FAST_POINTS == 32 && T1_FAST_BATCH == 32,
                   "native diagnostic histogram bounds");
    FAST_COUNT(tv, native_build_nodes[f->nnodes]);
    FAST_COUNT(tv, native_build_edges[edges]);
    f->diag_edges = edges;
#endif
    if (edges) {
        fast_fr_guards(f);
        fast_gr_guards(f, tv->x.entry_bof);
        f->retirement = retirement;
        f->bof = tv->x.entry_bof;
        f->dirty = tv->x.entry_dirty;
        f->token = v->code_entry_generation;
        f->iseq = v->mmu_iseq;
        f->mmu_seq = v->mmu_seq;
        f->bind_epoch = tv->x.observed_epoch;
        f->psr = v->s.psr;
        f->pta = v->s.cr[CR_PTA];
        f->cfm = v->s.cfm;
        tv->x.fast = first;
        T1_COUNT_TV(tv, chain_native_plans, 1);
    } else {
        f->nnodes = 0;
        FAST_DECLINE(tv, context);
    }
}

#if IA64VMM_TIER1_STATS
void t1_fast_count_exit(struct t1_vcpu *tv)
{
    const struct t1_fast *f = &tv->fast;
    FAST_COUNT(tv, native_hops[f->nrecord]);
    FAST_COUNT(tv, native_stop[f->diag_edges > 1][f->nrecord != 0][f->diag_stop]);
}
#endif

/* R15: source node; RBP: its edge; RBX: target node. These survive the
 * generated restore/entry-image leaf calls. R12/R13/R14 keep the native ABI. */
#define FO(member) ((int32_t)offsetof(struct t1_vcpu, fast.member))
#define NO(member) ((int32_t)offsetof(struct t1_fast_node, member))
#define WO(member) ((int32_t)offsetof(struct code_stay_witness, member))
#define VO(member) ((int32_t)offsetof(struct vcpu, member))
#define XO(member) ((int32_t)offsetof(struct t1_exec, member))

static struct t1_x86_insn fi(uint16_t op)
{
    struct t1_x86_insn i = {0};
    i.op = op;
    i.opnd[0].reg = i.opnd[1].reg = T1_X86_NOREG;
    i.mem.base = i.mem.index = T1_X86_NOREG;
    return i;
}
static size_t fe(struct t1_x86_buf *b, struct t1_x86_insn i)
{
    size_t fix = SIZE_MAX;
    t1_x86_emit(b, &i, &fix);
    return fix;
}
static void fr(struct t1_x86_buf *b, uint16_t op, unsigned d, unsigned s)
{
    struct t1_x86_insn i = fi(op);
    i.opnd[0].reg = (uint8_t)d;
    i.opnd[1].reg = (uint8_t)s;
    fe(b, i);
}
static void fc(struct t1_x86_buf *b, uint16_t op, unsigned r, int64_t value)
{
    struct t1_x86_insn i = fi(op);
    i.opnd[0].reg = (uint8_t)r;
    i.opnd[1].imm = value;
    fe(b, i);
}
static void fm(struct t1_x86_buf *b, uint16_t op, unsigned r, unsigned base, int32_t offset)
{
    struct t1_x86_insn i = fi(op);
    i.mem = (struct t1_x86_mem){(uint8_t)base, T1_X86_NOREG, 0, offset};
    i.opnd[0].reg = (uint8_t)r;
    i.opnd[1].reg = (uint8_t)r;
    fe(b, i);
}
static void fmi(struct t1_x86_buf *b, unsigned base, int32_t offset, int32_t value)
{
    struct t1_x86_insn i = fi(T1_X86_STORE64_I32);
    i.mem = (struct t1_x86_mem){(uint8_t)base, T1_X86_NOREG, 0, offset};
    i.opnd[1].imm = value;
    fe(b, i);
}
#if IA64VMM_TIER1_STATS
static void fstop(struct t1_x86_buf *b, enum t1_native_stop reason)
{
    struct t1_x86_insn i = fi(T1_X86_STORE32_I32);
    i.mem = (struct t1_x86_mem){T1_R14, T1_X86_NOREG, 0, FO(diag_stop)};
    i.opnd[1].imm = reason;
    fe(b, i);
}
#define FAST_STOP(b, kind) fstop(b, T1_NATIVE_STOP_##kind)
#else
#define FAST_STOP(b, kind) ((void)0)
#endif
static size_t fj(struct t1_x86_buf *b, unsigned cc)
{
    return fe(b, fi((uint16_t)(T1_X86_JCC + cc)));
}
static void fb(struct t1_x86_buf *b, unsigned cc, size_t to)
{
    t1_x86_patch_rel32(b, fj(b, cc), to);
}
static void ftest(struct t1_x86_buf *b, unsigned r, size_t fail)
{
    fr(b, T1_X86_TEST64_RR, r, r);
    fb(b, T1_CC_NE, fail);
}
static void fcmp(struct t1_x86_buf *b, unsigned base, int32_t at, int32_t expected, size_t fail)
{
    fm(b, T1_X86_LOAD64, T1_RAX, base, at);
    fm(b, T1_X86_LOAD64, T1_RCX, T1_R14, expected);
    fr(b, T1_X86_CMP64_RR, T1_RAX, T1_RCX);
    fb(b, T1_CC_NE, fail);
}
static void fwitness(struct t1_x86_buf *b, unsigned base, int32_t pointer, int32_t expected,
                     size_t fail)
{
    fm(b, T1_X86_LOAD64, T1_RAX, base, pointer);
    fm(b, T1_X86_LOAD64, T1_RAX, T1_RAX, 0);
    fm(b, T1_X86_LOAD64, T1_RCX, base, expected);
    fr(b, T1_X86_CMP64_RR, T1_RAX, T1_RCX);
    fb(b, T1_CC_NE, fail);
}
static void fpage_guard(struct t1_x86_buf *b, unsigned base, int32_t at, size_t fail)
{
    fm(b, T1_X86_LOAD64, T1_RAX, base, at + WO(owner));
    fr(b, T1_X86_CMP64_RR, T1_RAX, T1_R12);
    fb(b, T1_CC_NE, fail);
    fm(b, T1_X86_LOAD64, T1_RAX, T1_R12, VO(code_entry_generation));
    fm(b, T1_X86_LOAD64, T1_RCX, base, at + WO(token));
    fr(b, T1_X86_CMP64_RR, T1_RAX, T1_RCX);
    fb(b, T1_CC_NE, fail);
    fm(b, T1_X86_LOAD64, T1_RAX, T1_R12, VO(mmu_iseq));
    fm(b, T1_X86_LOAD64, T1_RCX, base, at + WO(iseq));
    fr(b, T1_X86_CMP64_RR, T1_RAX, T1_RCX);
    fb(b, T1_CC_NE, fail);
    fwitness(b, base, at + WO(page_word), at + WO(page), fail);
    fm(b, T1_X86_LOAD64, T1_RAX, base, at + WO(ready_word));
    fm(b, T1_X86_LOAD8ZX, T1_RAX, T1_RAX, 0);
    fr(b, T1_X86_TEST64_RR, T1_RAX, T1_RAX);
    fb(b, T1_CC_E, fail);
    fwitness(b, base, at + WO(epoch_word), at + WO(epoch), fail);
    fwitness(b, base, at + WO(generation_word), at + WO(generation), fail);
    fwitness(b, base, at + WO(admission_word), at + WO(admission), fail);
}
static void fentered(struct t1_x86_buf *b, unsigned base, int32_t at)
{
    fm(b, T1_X86_LOAD64, T1_RAX, base, at + WO(clock));
    fm(b, T1_X86_LOAD64, T1_RCX, T1_RAX, 0);
    fc(b, T1_X86_ADD64_RI, T1_RCX, 1);
    fm(b, T1_X86_STORE64, T1_RCX, T1_RAX, 0);
    fm(b, T1_X86_LOAD64, T1_RAX, base, at + WO(entered));
    fm(b, T1_X86_STORE64, T1_RCX, T1_RAX, 0);
}
static void ftouch(struct t1_x86_buf *b, unsigned base, int32_t at)
{
    fm(b, T1_X86_LOAD64, T1_RAX, base, at + WO(entry_tick));
    fm(b, T1_X86_LOAD64, T1_RCX, T1_RAX, 0);
    fr(b, T1_X86_TEST64_RR, T1_RCX, T1_RCX);
    size_t touched = fj(b, T1_CC_NE);
    fmi(b, T1_RAX, 0, 1);
    t1_x86_patch_rel32(b, touched, b->size);
}
static void fwindow(struct t1_x86_buf *b, unsigned node, bool entry, size_t fail)
{
    fm(b, T1_X86_LOAD64, T1_RAX, node, NO(window));
    fr(b, T1_X86_TEST64_RR, T1_RAX, T1_RAX);
    size_t none = fj(b, T1_CC_E);
    fm(b, T1_X86_LOAD64, T1_RDX, node, NO(region));
    fm(b, T1_X86_LOAD64, T1_RDX, T1_RDX, offsetof(struct t1_region, link));
    fm(b, T1_X86_LOAD64, T1_RCX, T1_RAX, offsetof(struct t1_window_slot, link));
    fr(b, T1_X86_CMP64_RR, T1_RCX, T1_RDX);
    fb(b, T1_CC_NE, fail);
    fm(b, T1_X86_LOAD32, T1_RCX, T1_RAX,
       entry ? offsetof(struct t1_window_slot, attempts)
             : offsetof(struct t1_window_slot, entries));
    fc(b, T1_X86_CMP32_RI, T1_RCX, T1_WINDOW_FOLD - 1);
    fb(b, T1_CC_AE, fail);
    t1_x86_patch_rel32(b, none, b->size);
}

void t1_fast_emit(struct t1_x86_buf *b, size_t fallback)
{
    _Static_assert(sizeof(struct t1_fast_record) == 32, "native record stride");
    _Static_assert(sizeof(struct t1_fast_edge) == 24, "native edge stride");
    fm(b, T1_X86_LOAD64, T1_R15, T1_R14, XO(fast));
    FAST_STOP(b, BATCH);
    fm(b, T1_X86_LOAD64, T1_RAX, T1_R14, FO(nrecord));
    fc(b, T1_X86_CMP64_RI, T1_RAX, T1_FAST_BATCH);
    fb(b, T1_CC_AE, fallback);
    FAST_STOP(b, POINT);
    fm(b, T1_X86_LOAD32, T1_RAX, T1_R14, XO(point));
    fc(b, T1_X86_CMP32_RI, T1_RAX, T1_FAST_POINTS);
    fb(b, T1_CC_AE, fallback);
    struct t1_x86_insn lea = fi(T1_X86_LEA64);
    lea.opnd[0].reg = T1_RAX;
    lea.mem = (struct t1_x86_mem){T1_RAX, T1_RAX, 1, 0};
    fe(b, lea);
    lea.opnd[0].reg = T1_RBP;
    lea.mem = (struct t1_x86_mem){T1_R15, T1_RAX, 3, NO(edge)};
    fe(b, lea);
    FAST_STOP(b, EDGE);
    fm(b, T1_X86_LOAD64, T1_RBX, T1_RBP, offsetof(struct t1_fast_edge, target));
    fr(b, T1_X86_TEST64_RR, T1_RBX, T1_RBX);
    fb(b, T1_CC_E, fallback);

    /* Conservative interrupt rejection includes masked IRR and all PAL
     * requests; the bridge performs priority selection and delivery. */
    FAST_STOP(b, EVENT);
    fm(b, T1_X86_LOAD64, T1_RAX, T1_R12, T1_VCPU_CKPT);
    for (unsigned k = 0; k < 4; ++k) {
        fm(b, T1_X86_LOAD64, T1_RCX, T1_R12, VO(s.cr) + 8 * (CR_IRR0 + k));
        fr(b, T1_X86_OR64_RR, T1_RAX, T1_RCX);
    }
    fm(b, T1_X86_LOAD16ZX, T1_RCX, T1_R12, VO(pal_pmi_pending));
    fr(b, T1_X86_OR64_RR, T1_RAX, T1_RCX);
    fm(b, T1_X86_LOAD16ZX, T1_RCX, T1_R12, VO(pal_mca_pending));
    fr(b, T1_X86_OR64_RR, T1_RAX, T1_RCX);
    fm(b, T1_X86_LOAD8ZX, T1_RCX, T1_R12, VO(pal_init_pending));
    fr(b, T1_X86_OR64_RR, T1_RAX, T1_RCX);
    ftest(b, T1_RAX, fallback);
    fm(b, T1_X86_LOAD32, T1_RAX, T1_R14, XO(bsp_called));
    fm(b, T1_X86_LOAD32, T1_RCX, T1_R14, XO(helper_exit));
    fr(b, T1_X86_OR64_RR, T1_RAX, T1_RCX);
    fm(b, T1_X86_LOAD32, T1_RCX, T1_R14, XO(helper_notification));
    fr(b, T1_X86_OR64_RR, T1_RAX, T1_RCX);
    ftest(b, T1_RAX, fallback);
    FAST_STOP(b, CONTEXT);
    fcmp(b, T1_R12, VO(mmu_seq), FO(mmu_seq), fallback);
    fcmp(b, T1_R12, VO(s.cr) + 8 * CR_PTA, FO(pta), fallback);
    fcmp(b, T1_R12, VO(s.psr), FO(psr), fallback);
    fcmp(b, T1_R12, VO(s.cfm), FO(cfm), fallback);
    fcmp(b, T1_R14, XO(observed_epoch), FO(bind_epoch), fallback);
    FAST_STOP(b, TIMER);
    fm(b, T1_X86_LOAD8ZX, T1_RAX, T1_R12, VO(timer_armed));
    fr(b, T1_X86_TEST64_RR, T1_RAX, T1_RAX);
    size_t no_timer = fj(b, T1_CC_E);
    fm(b, T1_X86_LOAD8ZX, T1_RAX, T1_R12, VO(timer_fired));
    fr(b, T1_X86_TEST64_RR, T1_RAX, T1_RAX);
    size_t fired_timer = fj(b, T1_CC_NE);
    /* The HAL defines this clock's domain. The proof changes no state;
     * its failure leaves all retirement and delivery to the bridge.
     * RSP is 16-byte aligned and source/edge/target are callee saves. */
    fr(b, T1_X86_MOV64_RR, T1_RDI, T1_R12);
    fc(b, T1_X86_MOV64_RI64, T1_RAX, (int64_t)(uintptr_t)&ia64_timer_poll_noop);
    fr(b, T1_X86_CALL_R, T1_RAX, T1_RAX);
    fc(b, T1_X86_AND64_RI, T1_RAX, 255); /* SysV bool result is AL. */
    fb(b, T1_CC_E, fallback);
    t1_x86_patch_rel32(b, no_timer, b->size);
    t1_x86_patch_rel32(b, fired_timer, b->size);
    FAST_STOP(b, BUDGET);
    fm(b, T1_X86_LOAD64, T1_RAX, T1_R14, XO(budget_entry));
    fr(b, T1_X86_CMP64_RR, T1_RAX, T1_R13);
    fb(b, T1_CC_BE, fallback); /* zero retirement is RAN_PENDING */
    fm(b, T1_X86_LOAD32, T1_RAX, T1_RBX, NO(max_path));
    fr(b, T1_X86_CMP64_RR, T1_R13, T1_RAX);
    fb(b, T1_CC_B, fallback);
    FAST_STOP(b, WINDOW);
    fwindow(b, T1_R15, false, fallback);
    fwindow(b, T1_RBX, true, fallback);
    FAST_STOP(b, SOURCE);
    fm(b, T1_X86_LOAD64, T1_RAX, T1_R15, NO(region));
    fm(b, T1_X86_LOAD8ZX, T1_RAX, T1_RAX, offsetof(struct t1_region, valid));
    fr(b, T1_X86_TEST64_RR, T1_RAX, T1_RAX);
    fb(b, T1_CC_E, fallback);

    /* The witness was constructed for this unchanged, already checked fetch
     * context. No admission or comparison is omitted on a failed witness. */
    FAST_STOP(b, WITNESS);
    fpage_guard(b, T1_RBX, NO(witness), fallback);
    fm(b, T1_X86_LOAD64, T1_R11, T1_RBX, NO(second));
    fr(b, T1_X86_TEST64_RR, T1_R11, T1_R11);
    size_t one_page = fj(b, T1_CC_E);
    FAST_STOP(b, SECOND);
    fpage_guard(b, T1_R11, 0, fallback);
    t1_x86_patch_rel32(b, one_page, b->size);

    /* Home restoration is idempotent. Every later failed guard leaves
     * source capture and entry metadata intact for the ordinary bridge. */
    fr(b, T1_X86_MOV64_RR, T1_RDI, T1_R14);
    fr(b, T1_X86_MOV64_RR, T1_RSI, T1_R12);
    fm(b, T1_X86_LOAD64, T1_RAX, T1_RBP, offsetof(struct t1_fast_edge, restore));
    fr(b, T1_X86_CALL_R, T1_RAX, T1_RAX);
    /* MF restoration is an idempotent OR. A newly set bit changes the
     * contract and must return to C; an already set bit keeps the plan. */
    FAST_STOP(b, MF);
    fcmp(b, T1_R12, VO(s.psr), FO(psr), fallback);
    fc(b, T1_X86_MOV32_RI32, T1_RCX, 0);
    FAST_STOP(b, NAT);
    fm(b, T1_X86_LOAD32, T1_RDX, T1_RBX, NO(nguard));
    fr(b, T1_X86_TEST64_RR, T1_RDX, T1_RDX);
    size_t no_guard = fj(b, T1_CC_E);
    size_t guard_loop = b->size;
    struct t1_x86_insn load = fi(T1_X86_LOAD32);
    load.opnd[0].reg = T1_RAX;
    load.mem = (struct t1_x86_mem){T1_RBX, T1_RCX, 2, NO(guard)};
    fe(b, load);
    load.op = T1_X86_LOAD8ZX;
    load.mem = (struct t1_x86_mem){T1_R12, T1_RAX, 0, 0};
    fe(b, load);
    ftest(b, T1_RAX, fallback);
    fc(b, T1_X86_ADD64_RI, T1_RCX, 1);
    fr(b, T1_X86_CMP64_RR, T1_RCX, T1_RDX);
    fb(b, T1_CC_B, guard_loop);
    t1_x86_patch_rel32(b, no_guard, b->size);

    /* Same NaTVal predicate as ia64_fr_is_nat: sign 0, exponent 0x1fffe,
     * significand 0. Test restored homes before publication or accounting. */
    fc(b, T1_X86_MOV32_RI32, T1_RCX, 0);
    FAST_STOP(b, FR);
    fm(b, T1_X86_LOAD32, T1_RDX, T1_RBX, NO(nfr_guard));
    fr(b, T1_X86_TEST64_RR, T1_RDX, T1_RDX);
    size_t no_fr_guard = fj(b, T1_CC_E);
    size_t fr_guard_loop = b->size;
    load.op = T1_X86_LOAD32;
    load.opnd[0].reg = T1_RAX;
    load.mem = (struct t1_x86_mem){T1_RBX, T1_RCX, 2, NO(fr_guard)};
    fe(b, load);
    load.opnd[0].reg = T1_R11;
    load.mem = (struct t1_x86_mem){T1_R12, T1_RAX, 0, offsetof(struct ia64_fr, exponent)};
    fe(b, load);
    fc(b, T1_X86_CMP32_RI, T1_R11, IA64_FP_NAT_EXP);
    size_t not_nat_exp = fj(b, T1_CC_NE);
    load.op = T1_X86_LOAD64;
    load.mem.disp = offsetof(struct ia64_fr, significand);
    fe(b, load);
    fr(b, T1_X86_TEST64_RR, T1_R11, T1_R11);
    size_t not_nat_sig = fj(b, T1_CC_NE);
    load.op = T1_X86_LOAD8ZX;
    load.mem.disp = offsetof(struct ia64_fr, sign);
    fe(b, load);
    fr(b, T1_X86_TEST64_RR, T1_R11, T1_R11);
    fb(b, T1_CC_E, fallback);
    t1_x86_patch_rel32(b, not_nat_exp, b->size);
    t1_x86_patch_rel32(b, not_nat_sig, b->size);
    fc(b, T1_X86_ADD64_RI, T1_RCX, 1);
    fr(b, T1_X86_CMP64_RR, T1_RCX, T1_RDX);
    fb(b, T1_CC_B, fr_guard_loop);
    t1_x86_patch_rel32(b, no_fr_guard, b->size);

    /* Intel SDM Vol.3A: locked operations have a total order and prevent
     * preceding/following loads and stores from crossing them. The sole
     * writer advances the SC entry sequence after publishing running. */
    _Static_assert(offsetof(struct t1_vcpu, entry_seq) % 8 == 0, "aligned locked sequence");
    FAST_STOP(b, PUBLISH);
    fm(b, T1_X86_LOAD64, T1_RAX, T1_RBX, NO(region));
    fm(b, T1_X86_STORE64, T1_RAX, T1_R14, offsetof(struct t1_vcpu, running));
    fm(b, T1_X86_LOCK_INC64_M, 0, T1_R14, offsetof(struct t1_vcpu, entry_seq));
    fm(b, T1_X86_LOAD8ZX, T1_RCX, T1_RAX, offsetof(struct t1_region, valid));
    fr(b, T1_X86_TEST64_RR, T1_RCX, T1_RCX);
    size_t retired = fj(b, T1_CC_E);

    /* Commit starts here: no fallback may repeat this accounting. */
    fc(b, T1_X86_MOV32_RI32, T1_RCX, 1);
    fm(b, T1_X86_STORE8, T1_RCX, T1_RAX, offsetof(struct t1_region, referenced));
    fentered(b, T1_RBX, NO(witness));
    fm(b, T1_X86_LOAD64, T1_R11, T1_RBX, NO(second));
    fr(b, T1_X86_TEST64_RR, T1_R11, T1_R11);
    one_page = fj(b, T1_CC_E);
    /* Match group entry: advance every admission before touching pages.
     * Aliased dependencies still advance the shared clock twice. */
    fentered(b, T1_R11, 0);
    ftouch(b, T1_RBX, NO(witness));
    ftouch(b, T1_R11, 0);
    size_t recency_done = fe(b, fi(T1_X86_JMP));
    t1_x86_patch_rel32(b, one_page, b->size);
    ftouch(b, T1_RBX, NO(witness));
    t1_x86_patch_rel32(b, recency_done, b->size);
    fm(b, T1_X86_LOAD64, T1_RAX, T1_R14, XO(budget_entry));
    fr(b, T1_X86_SUB64_RR, T1_RAX, T1_R13);
    fm(b, T1_X86_LOAD64, T1_RCX, T1_R14, FO(nrecord));
    fc(b, T1_X86_SHL64_RI, T1_RCX, 5);
    lea.opnd[0].reg = T1_R10;
    lea.mem = (struct t1_x86_mem){T1_R14, T1_RCX, 0, FO(record)};
    fe(b, lea);
    fm(b, T1_X86_STORE64, T1_R15, T1_R10, offsetof(struct t1_fast_record, from));
    fm(b, T1_X86_STORE64, T1_RBX, T1_R10, offsetof(struct t1_fast_record, to));
    fm(b, T1_X86_STORE64, T1_RAX, T1_R10, offsetof(struct t1_fast_record, consumed));
    fm(b, T1_X86_LOAD32, T1_RCX, T1_R14, XO(point));
    fm(b, T1_X86_STORE32, T1_RCX, T1_R10, offsetof(struct t1_fast_record, point));
    fm(b, T1_X86_INC64_M, 0, T1_R14, FO(nrecord));

    fm(b, T1_X86_LOAD64, T1_RDX, T1_R15, NO(window));
    fr(b, T1_X86_TEST64_RR, T1_RDX, T1_RDX);
    size_t source_no_policy = fj(b, T1_CC_E);
    fm(b, T1_X86_LOAD32, T1_RCX, T1_RDX, offsetof(struct t1_window_slot, entries));
    fc(b, T1_X86_ADD64_RI, T1_RCX, 1);
    fm(b, T1_X86_STORE32, T1_RCX, T1_RDX, offsetof(struct t1_window_slot, entries));
    fm(b, T1_X86_LOAD64, T1_RCX, T1_RDX, offsetof(struct t1_window_slot, retired));
    fr(b, T1_X86_ADD64_RR, T1_RCX, T1_RAX);
    fm(b, T1_X86_STORE64, T1_RCX, T1_RDX, offsetof(struct t1_window_slot, retired));
    t1_x86_patch_rel32(b, source_no_policy, b->size);
    fm(b, T1_X86_LOAD64, T1_RDX, T1_RBX, NO(window));
    fr(b, T1_X86_TEST64_RR, T1_RDX, T1_RDX);
    size_t target_no_policy = fj(b, T1_CC_E);
    fm(b, T1_X86_LOAD32, T1_RCX, T1_RDX, offsetof(struct t1_window_slot, attempts));
    fc(b, T1_X86_ADD64_RI, T1_RCX, 1);
    fm(b, T1_X86_STORE32, T1_RCX, T1_RDX, offsetof(struct t1_window_slot, attempts));
    t1_x86_patch_rel32(b, target_no_policy, b->size);
    fm(b, T1_X86_LOAD64, T1_RCX, T1_R14, XO(retired_entry));
    fr(b, T1_X86_ADD64_RR, T1_RCX, T1_RAX);
    fm(b, T1_X86_STORE64, T1_RCX, T1_R12, VO(retired));
    fm(b, T1_X86_STORE64, T1_RCX, T1_R14, XO(retired_entry));
    fm(b, T1_X86_STORE64, T1_R13, T1_R14, XO(budget_entry));
    fm(b, T1_X86_LOAD64, T1_RDX, T1_RBP, offsetof(struct t1_fast_edge, point));
    fm(b, T1_X86_LOAD64, T1_RCX, T1_RDX, offsetof(struct t1_point, ip));
    fm(b, T1_X86_STORE64, T1_RCX, T1_R12, VO(s.ip));
    fm(b, T1_X86_LOAD8ZX, T1_RCX, T1_RDX, offsetof(struct t1_point, slot));
    fm(b, T1_X86_STORE32, T1_RCX, T1_R12, VO(s.slot));
    fm(b, T1_X86_LOAD64, T1_RAX, T1_RBX, NO(region));
    fm(b, T1_X86_STORE64, T1_RAX, T1_R14, XO(region));
    fm(b, T1_X86_STORE64, T1_RBX, T1_R14, XO(fast));
    fc(b, T1_X86_MOV32_RI32, T1_RCX, 0);
    fm(b, T1_X86_STORE32, T1_RCX, T1_R14, XO(point));
    fm(b, T1_X86_LOAD64, T1_RAX, T1_RBX, NO(frame_in));
    fr(b, T1_X86_TEST64_RR, T1_RAX, T1_RAX);
    size_t no_frame_in = fj(b, T1_CC_E);
    fr(b, T1_X86_MOV64_RR, T1_RDI, T1_R14);
    fr(b, T1_X86_MOV64_RR, T1_RSI, T1_R12);
    fr(b, T1_X86_CALL_R, T1_RAX, T1_RAX);
    t1_x86_patch_rel32(b, no_frame_in, b->size);
    fm(b, T1_X86_INC64_M, 0, T1_R14, FO(completed));
    fm(b, T1_X86_JMP_M, 0, T1_RBX, NO(entry));

    t1_x86_patch_rel32(b, retired, b->size);
    fmi(b, T1_R14, offsetof(struct t1_vcpu, running), 0);
    size_t back = fe(b, fi(T1_X86_JMP));
    t1_x86_patch_rel32(b, back, fallback);
}
