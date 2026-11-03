/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Linear scan with splitting, lifetime holes, hints, fixed ranges and edge
 * resolution, following Wimmer & Mössenböck, "Optimized Interval Splitting in
 * a Linear Scan Register Allocator" (VEE 2005). t1_mir_effects defines MIR
 * uses, definitions and clobbers.
 *
 * Instruction i has boundary B=4i+1, input use U=4i+2, clobber C=4i+3 and
 * output D=4i+4. Ranges are half-open: last-use inputs end after U; outputs
 * start at D. Computations require registers; frames and PCOPY accept spills.
 *
 * Register-bearing children start at boundaries, where resolution inserts
 * moves. Mid-instruction eviction creates a spill child, stored at the prior
 * boundary while the parent still holds the value. Never split inside a
 * terminator sequence or between FORCE and its site/helper: both captures
 * and every outgoing edge must see the same register state.
 */
#include "engine/tier1/codegen.h"
#include "ia64vmm/hal.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

/* ---- t1_mir_effects (mir.h) ------------------------------------------- */

static bool reg_spec_read(uint8_t s)
{
    return s == T1_XO_R64_R || s == T1_XO_R64_RW || s == T1_XO_R32_R || s == T1_XO_R32_RW ||
           s == T1_XO_R16_R || s == T1_XO_R8_R || s == T1_XO_CL;
}
static bool reg_spec_write(uint8_t s)
{
    return s == T1_XO_R64_W || s == T1_XO_R64_RW || s == T1_XO_R32_W || s == T1_XO_R32_RW ||
           s == T1_XO_R8_W;
}
static bool mem_spec(uint8_t s)
{
    return (s >= T1_XO_M8_R && s <= T1_XO_M64_RW) || s == T1_XO_MA;
}

static void add_use(struct t1_mir_effects *e, uint8_t k)
{
    if (e->nuse < sizeof(e->use))
        e->use[e->nuse++] = k;
}
static void add_def(struct t1_mir_effects *e, uint8_t k)
{
    if (e->ndef < sizeof(e->def))
        e->def[e->ndef++] = k;
}

void t1_mir_effects(const struct t1_mir_insn *insn, struct t1_mir_effects *out)
{
    memset(out, 0, sizeof(*out));
    for (unsigned k = 0; k < T1_MIR_MAX_OPNDS; ++k)
        out->fixed[k] = T1_X86_NOREG;
    unsigned op = insn->op;
    const struct t1_x86_info *x = t1_x86_info(op);
    if (x) {
        for (unsigned k = 0; k < x->nopnd && k < 2; ++k) {
            uint8_t s = x->opnd[k];
            const struct t1_mir_opnd *o = &insn->opnd[k];
            if (mem_spec(s)) {
                if (insn->mem.base_kind == T1_MO_VREG)
                    add_use(out, 0x80);
                if (insn->mem.index_kind == T1_MO_VREG)
                    add_use(out, 0x81);
                continue;
            }
            if (o->kind != T1_MO_VREG)
                continue;
            if (reg_spec_read(s))
                add_use(out, (uint8_t)k);
            if (reg_spec_write(s))
                add_def(out, (uint8_t)k);
            if (s == T1_XO_CL)
                out->fixed[k] = T1_RCX;
        }
        out->clobber_regs = x->implicit_def;
        out->flags_read = x->flags_read;
        out->flags_clobber = (uint16_t)(x->flags_def | x->flags_undef);
        out->terminator = (x->attr & T1_XA_NOFALL) != 0;
        out->call = (x->attr & T1_XA_CALL) != 0;
        return;
    }
    switch (op) {
    case T1_MIR_HREAD:
        add_def(out, 0);
        break;
    case T1_MIR_HGUARD:
        out->clobber_regs = T1_R11_ONLY;
        out->flags_clobber = T1_XF_ALL;
        out->point = true;
        break;
    case T1_MIR_LIVEIN:
        add_def(out, 0);
        /* A predicate home is a bit of v->s.pr: shr/and. */
        out->flags_clobber = T1_XF_ALL;
        break;
    case T1_MIR_SPILL:
        add_use(out, 0);
        break;
    case T1_MIR_RELOAD:
        add_def(out, 0);
        break;
    case T1_MIR_CHARGE:
        out->flags_clobber = T1_XF_ALL;
        break;
    case T1_MIR_CHECKPOINT:
        out->clobber_regs = T1_R11_ONLY;
        out->flags_clobber = T1_XF_ALL;
        out->point = true;
        break;
    case T1_MIR_EXIT:
        out->point = true;
        out->terminator = true;
        break;
    case T1_MIR_SITE:
        if ((insn->opnd[2].imm & T1_SITE_STORE) && insn->opnd[0].kind == T1_MO_VREG)
            add_use(out, 0);
        else if (insn->opnd[0].kind == T1_MO_VREG)
            add_def(out, 0);
        if (insn->opnd[1].kind == T1_MO_VREG)
            add_use(out, 1);
        out->clobber_regs = T1_R11_ONLY;
        out->flags_clobber = T1_XF_ALL;
        out->point = true;
        break;
    case T1_MIR_FORCE:
        for (unsigned k = 0; k < 2; ++k)
            if (insn->opnd[k].kind == T1_MO_VREG)
                add_use(out, (uint8_t)k);
        out->clobber_regs = T1_R11_ONLY;
        out->flags_clobber = T1_XF_ALL;
        out->point = true;
        break;
    case T1_MIR_SLOG:
        for (unsigned k = 0; k < 2; ++k)
            if (insn->opnd[k].kind == T1_MO_VREG)
                add_use(out, (uint8_t)k);
        out->clobber_regs = T1_R11_ONLY;
        out->flags_clobber = T1_XF_ALL;
        break;
    case T1_MIR_HELPER:
        for (unsigned k = 0; k < 2; ++k)
            if (insn->opnd[k].kind == T1_MO_VREG)
                add_def(out, (uint8_t)k);
        out->clobber_regs = T1_WRAPPER_CLOBBERS;
        out->flags_clobber = T1_XF_ALL;
        out->point = true;
        out->call = true;
        break;
    case T1_MIR_PCOPY:
        out->clobber_regs = T1_R11_ONLY;
        break;
    default:
        break;
    }
}

/* ---- positions ---------------------------------------------------------- */

#define POS_B(i) (4u * (uint32_t)(i) + 1u)
#define POS_U(i) (4u * (uint32_t)(i) + 2u)
#define POS_C(i) (4u * (uint32_t)(i) + 3u)
#define POS_D(i) (4u * (uint32_t)(i) + 4u)
#define POS_INSN(p) (((p) - 1u) / 4u)
#define POS_INF UINT32_MAX
#define NONE UINT32_MAX

/* ANY: any location (frames, copies of restoration-only values). REG: an
 * instruction operand, needs a register. PREFER: wants a register but
 * accepts a slot (loop-end markers, copies of loop values): it ranks the
 * interval when choosing what to evict, but never forces a reload. */
enum { USE_ANY = 0, USE_REG = 1, USE_PREFER = 2 };
struct range {
    uint32_t from, to;
};
struct usepos {
    uint32_t pos;
    uint8_t kind;
};

struct interval {
    t1_vreg vreg;
    uint8_t reg;           /* assigned register, or NOREG */
    bool spilled;          /* lives in the vreg's spill slot */
    bool fixed;            /* precolored: never split, never in unhandled */
    uint32_t r_first, r_n; /* ranges */
    uint32_t u_first, u_n; /* use positions */
    uint32_t next;         /* next child of the same vreg (by start) */
};

/* A move inserted at boundary `at`. Phases order the groups sharing one
 * boundary: values arriving over the incoming edge (block start), split
 * moves into children starting at the boundary (reloads, register moves),
 * stores of children evicted inside the next instruction (their parent's
 * register may itself be loaded by the split phase), then values leaving
 * over the outgoing edge (before the terminator); each group is one
 * parallel copy. */
enum { PHASE_IN, PHASE_SPLIT, PHASE_SPILL, PHASE_OUT };
struct move {
    uint32_t at;
    uint8_t phase;
    t1_vreg vreg;
    struct t1_loc dst, src;
    bool needed_later;
};

struct ra {
    struct t1_mir *m;
    unsigned nwords; /* bitset words per block */
    uint64_t *live_in, *live_out, *gen, *kill;
    struct interval *iv; /* [0, nvregs): base intervals; then children */
    unsigned niv, iv_cap;
    struct range *rg;
    unsigned nrg, rg_cap;
    struct usepos *up;
    unsigned nup, up_cap;
    /* Fixed ranges per physical register (clobbers, precolored vregs). */
    struct range *fx[T1_X86_NREGS];
    unsigned nfx[T1_X86_NREGS], fx_cap;
    uint32_t *unhandled;
    unsigned nun, un_cap;
    uint32_t *active, *inactive;
    unsigned nact, ninact;
    int16_t *slot; /* per vreg */
    struct move *mv;
    unsigned nmv, mv_cap;
    uint8_t *no_split;    /* per instruction: B(i) is not a split position */
    uint8_t *block_first; /* per instruction: first of a block */
    unsigned cur_block;   /* block being visited */
    uint32_t ninsns;
    void *mem;
    size_t mem_pages;
    bool failed;
    const char *why;
};

static bool fail(struct ra *A, const char *why)
{
    if (!A->failed) {
        A->failed = true;
        A->why = why;
    }
    return false;
}

static void *carve(uint8_t **p, size_t bytes)
{
    void *r = *p;
    *p += (bytes + 15) & ~(size_t)15;
    return r;
}

/* ---- intervals ---------------------------------------------------------- */

static struct range *ranges(struct ra *A, const struct interval *it)
{
    return &A->rg[it->r_first];
}
static uint32_t iv_start(struct ra *A, const struct interval *it)
{
    return it->r_n ? A->rg[it->r_first].from : POS_INF;
}
static uint32_t iv_end(struct ra *A, const struct interval *it)
{
    return it->r_n ? A->rg[it->r_first + it->r_n - 1].to : 0;
}

static bool covers(struct ra *A, const struct interval *it, uint32_t pos)
{
    const struct range *r = ranges(A, it);
    for (unsigned k = 0; k < it->r_n; ++k) {
        if (pos < r[k].from)
            return false;
        if (pos < r[k].to)
            return true;
    }
    return false;
}

/* First position >= from where both range lists cover, or POS_INF. */
static uint32_t intersect(const struct range *a, unsigned na, const struct range *b, unsigned nb,
                          uint32_t from)
{
    unsigned i = 0, j = 0;
    while (i < na && j < nb) {
        uint32_t lo = a[i].from > b[j].from ? a[i].from : b[j].from;
        uint32_t hi = a[i].to < b[j].to ? a[i].to : b[j].to;
        if (lo < from)
            lo = from;
        if (lo < hi)
            return lo;
        if (a[i].to < b[j].to)
            ++i;
        else
            ++j;
    }
    return POS_INF;
}

static uint32_t next_reg_use(struct ra *A, const struct interval *it, uint32_t from)
{
    for (unsigned k = 0; k < it->u_n; ++k) {
        const struct usepos *u = &A->up[it->u_first + k];
        if (u->pos >= from && u->kind == USE_REG)
            return u->pos;
    }
    return POS_INF;
}

/* REG or PREFER: how soon the interval wants a register. */
static uint32_t next_wanted(struct ra *A, const struct interval *it, uint32_t from)
{
    for (unsigned k = 0; k < it->u_n; ++k) {
        const struct usepos *u = &A->up[it->u_first + k];
        if (u->pos >= from && u->kind != USE_ANY)
            return u->pos;
    }
    return POS_INF;
}

static struct interval *new_interval(struct ra *A)
{
    if (A->niv >= A->iv_cap) {
        fail(A, "interval limit");
        return NULL;
    }
    struct interval *it = &A->iv[A->niv++];
    memset(it, 0, sizeof(*it));
    it->reg = T1_X86_NOREG;
    it->next = NONE;
    it->vreg = T1_VREG_NONE;
    return it;
}

/* Splits it at pos (start(it) < pos < end(it)): the child takes the ranges
 * and uses at or after pos. Returns the child's index, or NONE. */
static uint32_t split(struct ra *A, uint32_t idx, uint32_t pos)
{
    struct interval *it = &A->iv[idx];
    if (it->fixed || pos <= iv_start(A, it) || pos >= iv_end(A, it)) {
        fail(A, "invalid split");
        return NONE;
    }
    struct interval *c = new_interval(A);
    if (!c)
        return NONE;
    it = &A->iv[idx];
    uint32_t cidx = (uint32_t)(c - A->iv);
    c->vreg = it->vreg;
    struct range *r = ranges(A, it);
    unsigned k = 0;
    while (k < it->r_n && r[k].to <= pos)
        ++k;
    bool cut = r[k].from < pos;
    unsigned cn = it->r_n - k;
    if (A->nrg + cn > A->rg_cap) {
        fail(A, "range pool");
        return NONE;
    }
    c->r_first = A->nrg;
    c->r_n = cn;
    for (unsigned q = 0; q < cn; ++q)
        A->rg[A->nrg++] = r[k + q];
    if (cut) {
        A->rg[c->r_first].from = pos;
        r[k].to = pos;
        it->r_n = k + 1;
    } else
        it->r_n = k;
    unsigned u = 0;
    while (u < it->u_n && A->up[it->u_first + u].pos < pos)
        ++u;
    c->u_first = it->u_first + u;
    c->u_n = it->u_n - u;
    it->u_n = u;
    c->next = it->next;
    it->next = cidx;
    return cidx;
}

/* ---- liveness ----------------------------------------------------------- */

static bool bit(const uint64_t *s, unsigned i)
{
    return (s[i / 64] >> (i % 64)) & 1;
}
static void set_bit(uint64_t *s, unsigned i)
{
    s[i / 64] |= UINT64_C(1) << (i % 64);
}

static unsigned opnd_vreg_of(const struct t1_mir_insn *in, uint8_t k, t1_vreg *v)
{
    if (k == 0x80) {
        *v = in->mem.base;
        return in->mem.base_kind == T1_MO_VREG;
    }
    if (k == 0x81) {
        *v = in->mem.index;
        return in->mem.index_kind == T1_MO_VREG;
    }
    *v = in->opnd[k].vreg;
    return in->opnd[k].kind == T1_MO_VREG;
}

/* Calls fn for every vreg use of instruction i (operands, PCOPY sources,
 * frame entries) and every def, in the order the instruction performs them. */
struct visit {
    void (*use)(struct ra *A, uint32_t i, t1_vreg v, uint8_t kind, void *arg);
    void (*def)(struct ra *A, uint32_t i, t1_vreg v, uint8_t kind, bool also_used, void *arg);
    void *arg;
};

static void frame_uses(struct ra *A, uint32_t i, const struct visit *f)
{
    const struct t1_mir *m = A->m;
    const struct t1_mir_insn *in = &m->insn[i];
    if (in->point == UINT32_MAX)
        return;
    const struct t1_mir_point *p = &m->point[in->point];
    for (unsigned k = 0; k < p->frame_count; ++k) {
        const struct t1_mir_frame *fr = &m->frame[p->frame_first + k];
        if (fr->kind == T1_MF_VREG)
            f->use(A, i, fr->vreg, USE_ANY, f->arg);
    }
}

static void visit_insn(struct ra *A, uint32_t i, const struct visit *f,
                       const struct t1_mir_effects *fx, bool uses_phase)
{
    const struct t1_mir *m = A->m;
    const struct t1_mir_insn *in = &m->insn[i];
    if (in->op == T1_MIR_PCOPY) {
        /* Copies of loop values want registers on both sides
         * (loop-carried residency); restoration-only values may stay in
         * slots. */
        for (uint32_t c = in->copies_first; c < in->copies_first + in->ncopies; ++c) {
            const struct t1_mir_copy *cp = &m->copy[c];
            bool loop_value =
                A->m->block[A->cur_block].loop && (m->vreg[cp->dst].flags & T1_VR_REAL_USE);
            uint8_t kind = loop_value ? USE_PREFER : USE_ANY;
            if (uses_phase) {
                if (cp->src != T1_VREG_NONE)
                    f->use(A, i, cp->src, kind, f->arg);
            } else
                f->def(A, i, cp->dst, kind, false, f->arg);
        }
        return;
    }
    if (uses_phase) {
        for (unsigned k = 0; k < fx->nuse; ++k) {
            t1_vreg v;
            if (opnd_vreg_of(in, fx->use[k], &v))
                f->use(A, i, v, USE_REG, f->arg);
        }
        frame_uses(A, i, f);
    } else {
        for (unsigned k = 0; k < fx->ndef; ++k) {
            t1_vreg v;
            if (!opnd_vreg_of(in, fx->def[k], &v))
                continue;
            bool used = false;
            for (unsigned q = 0; q < fx->nuse; ++q) {
                t1_vreg w;
                if (opnd_vreg_of(in, fx->use[q], &w) && w == v)
                    used = true;
            }
            f->def(A, i, v, USE_REG, used, f->arg);
        }
    }
}

struct gk {
    uint64_t *gen, *kill;
};
static void lv_use(struct ra *A, uint32_t i, t1_vreg v, uint8_t kind, void *arg)
{
    (void)i;
    (void)kind;
    struct gk *g = arg;
    if (v < A->m->nvregs && !bit(g->kill, v))
        set_bit(g->gen, v);
}
static void lv_def(struct ra *A, uint32_t i, t1_vreg v, uint8_t kind, bool also_used, void *arg)
{
    (void)i;
    (void)kind;
    (void)also_used;
    struct gk *g = arg;
    if (v < A->m->nvregs)
        set_bit(g->kill, v);
}

static void liveness(struct ra *A)
{
    struct t1_mir *m = A->m;
    unsigned W = A->nwords;
    for (unsigned b = 0; b < m->nblocks; ++b) {
        struct gk g = {&A->gen[b * W], &A->kill[b * W]};
        struct visit f = {lv_use, lv_def, &g};
        A->cur_block = b;
        for (uint32_t i = m->block[b].first; i < m->block[b].first + m->block[b].count; ++i) {
            struct t1_mir_effects fx;
            t1_mir_effects(&m->insn[i], &fx);
            visit_insn(A, i, &f, &fx, true);
            visit_insn(A, i, &f, &fx, false);
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = m->nblocks; b-- > 0;) {
            uint64_t *out = &A->live_out[b * W], *in = &A->live_in[b * W];
            const uint64_t *gen = &A->gen[b * W], *kill = &A->kill[b * W];
            for (unsigned w = 0; w < W; ++w) {
                uint64_t o = 0;
                for (unsigned s = 0; s < m->block[b].nsucc; ++s)
                    o |= A->live_in[m->block[b].succ[s] * W + w];
                uint64_t n = gen[w] | (o & ~kill[w]);
                if (o != out[w] || n != in[w]) {
                    out[w] = o;
                    in[w] = n;
                    changed = true;
                }
            }
        }
    }
}

/* ---- building intervals ------------------------------------------------- */

struct builder {
    uint32_t *head;  /* per vreg: index of the lowest range node */
    uint32_t *rnext; /* per range node: next (higher) node */
    struct range *node;
    unsigned nnode, node_cap;
    uint32_t *uhead; /* per vreg: list of use nodes (reverse order) */
    uint32_t *unext;
    struct usepos *unode;
    unsigned nunode, unode_cap;
    uint32_t block_from;
};

static void b_add_range(struct ra *A, struct builder *B, t1_vreg v, uint32_t from, uint32_t to)
{
    uint32_t h = B->head[v];
    if (h != NONE && B->node[h].from <= to) {
        if (from < B->node[h].from)
            B->node[h].from = from;
        if (to > B->node[h].to)
            B->node[h].to = to;
        return;
    }
    if (B->nnode >= B->node_cap) {
        fail(A, "range pool");
        return;
    }
    uint32_t n = B->nnode++;
    B->node[n] = (struct range){from, to};
    B->rnext[n] = h;
    B->head[v] = n;
}

static void b_set_from(struct ra *A, struct builder *B, t1_vreg v, uint32_t pos)
{
    uint32_t h = B->head[v];
    if (h != NONE && B->node[h].from <= pos && pos < B->node[h].to) {
        B->node[h].from = pos;
        return;
    }
    /* A dead definition. */
    b_add_range(A, B, v, pos, pos + 1);
}

static void b_add_use(struct ra *A, struct builder *B, t1_vreg v, uint32_t pos, uint8_t kind)
{
    if (B->nunode >= B->unode_cap) {
        fail(A, "use pool");
        return;
    }
    uint32_t n = B->nunode++;
    B->unode[n] = (struct usepos){pos, kind};
    B->unext[n] = B->uhead[v];
    B->uhead[v] = n;
}

static void bd_use(struct ra *A, uint32_t i, t1_vreg v, uint8_t kind, void *arg)
{
    struct builder *B = arg;
    if (v >= A->m->nvregs)
        return;
    b_add_range(A, B, v, B->block_from, POS_U(i) + 1);
    b_add_use(A, B, v, POS_U(i), kind);
}
static void bd_def(struct ra *A, uint32_t i, t1_vreg v, uint8_t kind, bool also_used, void *arg)
{
    struct builder *B = arg;
    if (v >= A->m->nvregs)
        return;
    if (also_used)
        b_add_range(A, B, v, B->block_from, POS_D(i) + 1);
    else
        b_set_from(A, B, v, POS_D(i));
    b_add_use(A, B, v, POS_D(i), kind);
}

static void add_fixed(struct ra *A, unsigned reg, uint32_t from, uint32_t to)
{
    if (A->nfx[reg] >= A->fx_cap) {
        fail(A, "fixed range pool");
        return;
    }
    A->fx[reg][A->nfx[reg]++] = (struct range){from, to};
}

static int cmp_range(const struct range *a, const struct range *b)
{
    return a->from < b->from ? -1 : a->from > b->from;
}

/* Sorts by start and merges overlapping or touching ranges; returns the
 * new count. */
static unsigned sort_ranges(struct range *r, unsigned n)
{
    for (unsigned i = 1; i < n; ++i) {
        struct range x = r[i];
        unsigned j = i;
        while (j && cmp_range(&r[j - 1], &x) > 0) {
            r[j] = r[j - 1];
            --j;
        }
        r[j] = x;
    }
    unsigned out = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (out && r[out - 1].to >= r[i].from) {
            if (r[i].to > r[out - 1].to)
                r[out - 1].to = r[i].to;
        } else {
            r[out++] = r[i];
        }
    }
    return out;
}

static bool build(struct ra *A)
{
    struct t1_mir *m = A->m;
    unsigned W = A->nwords;
    struct builder B;
    unsigned nv = m->nvregs;
    B.node_cap = A->rg_cap;
    B.unode_cap = A->up_cap;
    size_t bytes = (size_t)nv * 2 * sizeof(uint32_t) +
                   (size_t)B.node_cap * (sizeof(struct range) + sizeof(uint32_t)) +
                   (size_t)B.unode_cap * (sizeof(struct usepos) + sizeof(uint32_t)) + 256;
    size_t pages = (bytes + 4095) / 4096;
    uint8_t *p = t1_ws_alloc(m->ir->ws, pages, 0);
    if (!p)
        return fail(A, "no memory for intervals");
    uint8_t *q = p;
    B.head = carve(&q, nv * sizeof(uint32_t));
    B.uhead = carve(&q, nv * sizeof(uint32_t));
    B.node = carve(&q, B.node_cap * sizeof(struct range));
    B.rnext = carve(&q, B.node_cap * sizeof(uint32_t));
    B.unode = carve(&q, B.unode_cap * sizeof(struct usepos));
    B.unext = carve(&q, B.unode_cap * sizeof(uint32_t));
    B.nnode = B.nunode = 0;
    for (unsigned v = 0; v < nv; ++v)
        B.head[v] = B.uhead[v] = NONE;

    struct visit f = {bd_use, bd_def, &B};
    for (unsigned b = m->nblocks; b-- > 0 && !A->failed;) {
        const struct t1_mir_block *blk = &m->block[b];
        uint32_t from = POS_B(blk->first), to = POS_B(blk->first + blk->count);
        B.block_from = from;
        A->cur_block = b;
        const uint64_t *out = &A->live_out[b * W];
        /* A back-edge block: values carried around its loop that some
         * instruction of a loop reads get a register use at the loop end
         * (after Wimmer & Mössenböck), so that a value read at the top of
         * the loop does not look unused from the bottom of it; values only
         * frames read (restoration-only) get none and may wait in
         * their slots. Each loop of a forest gets its own. */
        bool back_edge = blk->back_edge;
        /* Padding bits stay zero in liveness. Visit set bits in the
         * same ascending vreg order without scanning dead vregs. */
        for (unsigned w = 0; w < W; ++w)
            for (uint64_t bits = out[w]; bits; bits &= bits - 1) {
                unsigned v = 64 * w + (unsigned)__builtin_ctzll(bits);
                b_add_range(A, &B, (t1_vreg)v, from, to);
                if (back_edge && (m->vreg[v].flags & T1_VR_REAL_USE) && blk->count)
                    b_add_use(A, &B, (t1_vreg)v, POS_D(blk->first + blk->count - 1), USE_PREFER);
            }
        for (uint32_t i = blk->first + blk->count; i-- > blk->first;) {
            struct t1_mir_effects fx;
            t1_mir_effects(&m->insn[i], &fx);
            visit_insn(A, i, &f, &fx, false);
            visit_insn(A, i, &f, &fx, true);
            uint16_t clob = fx.clobber_regs & m->allocatable;
            for (unsigned r = 0; r < T1_X86_NREGS; ++r)
                if (clob & T1_X86_REGBIT(r))
                    add_fixed(A, r, POS_C(i), POS_C(i) + 1);
        }
    }
    /* Flatten into the interval pools (ascending). */
    for (unsigned v = 0; v < nv && !A->failed; ++v) {
        struct interval *it = &A->iv[v];
        memset(it, 0, sizeof(*it));
        it->vreg = (t1_vreg)v;
        it->reg = T1_X86_NOREG;
        it->next = NONE;
        it->r_first = A->nrg;
        for (uint32_t n = B.head[v]; n != NONE; n = B.rnext[n]) {
            if (A->nrg >= A->rg_cap) {
                fail(A, "range pool");
                break;
            }
            struct range r = B.node[n];
            if (it->r_n && A->rg[A->nrg - 1].to >= r.from) {
                if (r.to > A->rg[A->nrg - 1].to)
                    A->rg[A->nrg - 1].to = r.to;
                continue;
            }
            A->rg[A->nrg++] = r;
            ++it->r_n;
        }
        /* Uses were prepended while walking backwards, so the list runs
         * in increasing position order. */
        unsigned cnt = 0;
        for (uint32_t n = B.uhead[v]; n != NONE; n = B.unext[n])
            ++cnt;
        if (A->nup + cnt > A->up_cap) {
            fail(A, "use pool");
            break;
        }
        it->u_first = A->nup;
        it->u_n = cnt;
        for (uint32_t n = B.uhead[v]; n != NONE; n = B.unext[n])
            A->up[A->nup++] = B.unode[n];
        for (unsigned k = 1; k < cnt; ++k)
            if (A->up[it->u_first + k - 1].pos > A->up[it->u_first + k].pos) {
                fail(A, "unsorted uses");
                break;
            }
        /* Uses at one position: keep REG over ANY. */
        uint8_t fixed = m->vreg[v].fixed;
        if (fixed != T1_X86_NOREG && it->r_n) {
            it->fixed = true;
            it->reg = fixed;
            for (unsigned q = 0; q < it->r_n; ++q)
                add_fixed(A, fixed, A->rg[it->r_first + q].from, A->rg[it->r_first + q].to);
        }
    }
    A->niv = nv;
    t1_ws_free(m->ir->ws, p, pages);
    for (unsigned r = 0; r < T1_X86_NREGS; ++r)
        A->nfx[r] = sort_ranges(A->fx[r], A->nfx[r]);
    return !A->failed;
}

/* ---- split positions ---------------------------------------------------- */

static bool boundary_ok(struct ra *A, uint32_t i)
{
    return i >= A->ninsns || !A->no_split[i];
}

/* The latest boundary B(j) with lo < B(j) <= hi that may hold a move, or
 * NONE. */
static uint32_t boundary_at_or_before(struct ra *A, uint32_t lo, uint32_t hi)
{
    if (hi < 1)
        return NONE;
    uint32_t j = POS_INSN(hi);
    if (POS_B(j) > hi) {
        if (!j)
            return NONE;
        --j;
    }
    for (;; --j) {
        uint32_t b = POS_B(j);
        if (b <= lo)
            return NONE;
        if (boundary_ok(A, j))
            return b;
        if (!j)
            return NONE;
    }
}

/* ---- the linear scan ---------------------------------------------------- */

static void unhandled_add(struct ra *A, uint32_t idx)
{
    if (A->nun >= A->un_cap) {
        fail(A, "unhandled list");
        return;
    }
    /* Kept sorted by decreasing start: pop from the end. */
    uint32_t s = iv_start(A, &A->iv[idx]);
    unsigned k = A->nun++;
    while (k && iv_start(A, &A->iv[A->unhandled[k - 1]]) < s) {
        A->unhandled[k] = A->unhandled[k - 1];
        --k;
    }
    A->unhandled[k] = idx;
}

static bool unhandled_before(struct ra *A, uint32_t a, uint32_t b)
{
    uint32_t sa = iv_start(A, &A->iv[a]), sb = iv_start(A, &A->iv[b]);
    return sa > sb || (sa == sb && a < b);
}

static void unhandled_init(struct ra *A)
{
    if (A->m->nvregs <= 32) {
        for (unsigned v = 0; v < A->m->nvregs; ++v)
            if (A->iv[v].r_n && !A->iv[v].fixed)
                unhandled_add(A, v);
        return;
    }
    for (unsigned v = 0; v < A->m->nvregs; ++v) {
        if (!A->iv[v].r_n || A->iv[v].fixed)
            continue;
        if (A->nun >= A->un_cap) {
            fail(A, "unhandled list");
            return;
        }
        A->unhandled[A->nun++] = v;
    }
    unsigned n = A->nun, ordered = 1;
    while (ordered < n && unhandled_before(A, A->unhandled[ordered - 1], A->unhandled[ordered]))
        ++ordered;
    if (ordered >= n)
        return;
    /* Match stable insertion of ascending vregs: decreasing start, then
     * increasing vreg. The active list is empty until the scan starts. */
    uint32_t *src = A->unhandled, *dst = A->active;
    for (unsigned width = 1; width < n; width *= 2) {
        for (unsigned first = 0; first < n; first += 2 * width) {
            unsigned mid = first + width < n ? first + width : n;
            unsigned end = first + 2 * width < n ? first + 2 * width : n;
            unsigned left = first, right = mid;
            for (unsigned k = first; k < end; ++k)
                dst[k] = left < mid && (right == end || unhandled_before(A, src[left], src[right]))
                             ? src[left++]
                             : src[right++];
        }
        uint32_t *swap = src;
        src = dst;
        dst = swap;
    }
    if (src != A->unhandled)
        memcpy(A->unhandled, src, n * sizeof(*src));
}

static uint32_t fixed_intersect(struct ra *A, unsigned reg, const struct interval *it)
{
    return intersect(A->fx[reg], A->nfx[reg], ranges(A, it), it->r_n, iv_start(A, it));
}

static uint8_t hint_reg(struct ra *A, const struct interval *cur)
{
    t1_vreg h = A->m->vreg[cur->vreg].hint;
    if (h == T1_VREG_NONE || h >= A->m->nvregs)
        return T1_X86_NOREG;
    uint32_t s = iv_start(A, cur);
    uint8_t any = T1_X86_NOREG;
    for (uint32_t c = h; c != NONE; c = A->iv[c].next) {
        const struct interval *it = &A->iv[c];
        if (it->reg == T1_X86_NOREG)
            continue;
        if (covers(A, it, s - 1) || covers(A, it, s) || iv_end(A, it) == s)
            return it->reg;
        if (any == T1_X86_NOREG)
            any = it->reg;
    }
    return any;
}

static const uint8_t reg_order[] = {T1_RAX, T1_RCX, T1_RDX, T1_RSI, T1_RDI, T1_R8,
                                    T1_R9,  T1_R10, T1_RBX, T1_RBP, T1_R15};

/* The register with the largest key; ties go to the hint, then to the
 * first in reg_order (callee-saved last: values crossing helper calls find
 * them because the call clobbers make the others short). */
static uint8_t pick(const uint32_t *key, uint8_t hint)
{
    uint8_t best = T1_X86_NOREG;
    for (unsigned k = 0; k < sizeof(reg_order); ++k)
        if (best == T1_X86_NOREG || key[reg_order[k]] > key[best])
            best = reg_order[k];
    if (hint != T1_X86_NOREG && best != T1_X86_NOREG && key[hint] == key[best])
        best = hint;
    return best;
}

static bool try_free(struct ra *A, uint32_t idx)
{
    struct interval *cur = &A->iv[idx];
    uint32_t free_until[T1_X86_NREGS];
    uint16_t pool = A->m->allocatable;
    for (unsigned r = 0; r < T1_X86_NREGS; ++r)
        free_until[r] = (pool & T1_X86_REGBIT(r)) ? POS_INF : 0;
    for (unsigned k = 0; k < A->nact; ++k)
        free_until[A->iv[A->active[k]].reg] = 0;
    for (unsigned k = 0; k < A->ninact; ++k) {
        const struct interval *it = &A->iv[A->inactive[k]];
        uint32_t x = intersect(ranges(A, it), it->r_n, ranges(A, cur), cur->r_n, iv_start(A, cur));
        if (x < free_until[it->reg])
            free_until[it->reg] = x;
    }
    for (unsigned r = 0; r < T1_X86_NREGS; ++r) {
        if (!free_until[r])
            continue;
        uint32_t x = fixed_intersect(A, r, cur);
        if (x < free_until[r])
            free_until[r] = x;
    }
    uint32_t start = iv_start(A, cur), end = iv_end(A, cur);
    uint8_t h = hint_reg(A, cur);
    if (h != T1_X86_NOREG && !(pool & T1_X86_REGBIT(h)))
        h = T1_X86_NOREG;
    uint8_t best = h != T1_X86_NOREG && free_until[h] >= end ? h : pick(free_until, h);
    if (best == T1_X86_NOREG || free_until[best] <= start)
        return false;
    if (free_until[best] >= end) {
        cur->reg = best;
        return true;
    }
    /* Only its definition needs the register and it cannot keep it to its
     * end: store it right after the definition instead of later. */
    if (next_wanted(A, cur, start + 1) == POS_INF) {
        uint32_t after = boundary_at_or_before(A, start, POS_B(POS_INSN(start) + 1));
        if (after != NONE && after < free_until[best] && after < end) {
            uint32_t c = split(A, idx, after);
            if (c == NONE)
                return false;
            A->iv[idx].reg = best;
            A->iv[c].spilled = true;
            return true;
        }
    }
    /* Partly free: keep it up to a boundary before it is taken; the rest
     * starts there and is allocated again. */
    uint32_t pos = boundary_at_or_before(A, start, free_until[best]);
    if (pos == NONE)
        return false;
    uint32_t c = split(A, idx, pos);
    if (c == NONE)
        return false;
    A->iv[idx].reg = best;
    unhandled_add(A, c);
    return true;
}

/* Diagnostics for allocation defects (never expected; the compile then
 * rejects the region with the reason). */
static void dump_vreg(struct ra *A, t1_vreg v)
{
    for (uint32_t c = v; c != NONE; c = A->iv[c].next) {
        const struct interval *it = &A->iv[c];
        LOGE("  child %u: [%u, %u) reg %u spilled %d", c, iv_start(A, it), iv_end(A, it), it->reg,
             it->spilled);
        for (unsigned k = 0; k < it->u_n; ++k)
            LOGE("    use %u kind %u", A->up[it->u_first + k].pos, A->up[it->u_first + k].kind);
    }
}

static void no_reload(struct ra *A, uint32_t start, uint32_t use, t1_vreg v)
{
    LOGE("regalloc: no reload position for v%u between %u and %u", v, start, use);
    dump_vreg(A, v);
    fail(A, "no reload position");
}

static void remove_at(uint32_t *list, unsigned *n, unsigned k)
{
    list[k] = list[--*n];
}

/* The evicted part of interval idx from pos on: in its slot until its next
 * register use, and from a boundary before that use back in unhandled. A
 * part that needs a register at its first boundary or definition goes to
 * unhandled directly. */
static void spill_from(struct ra *A, uint32_t idx, uint32_t pos)
{
    struct interval *it = &A->iv[idx];
    if (pos >= iv_end(A, it))
        return;
    uint32_t c = idx;
    if (pos > iv_start(A, it)) {
        c = split(A, idx, pos);
        if (c == NONE)
            return;
    }
    struct interval *ch = &A->iv[c];
    ch->reg = T1_X86_NOREG;
    uint32_t start = iv_start(A, ch);
    uint32_t u = next_reg_use(A, ch, start);
    uint32_t b = u == POS_INF ? NONE : boundary_at_or_before(A, start - 1, u);
    if (u == start || b == start) {
        if (c == idx) {
            fail(A, "eviction at an immediate register use");
            return;
        }
        unhandled_add(A, c);
        return;
    }
    ch->spilled = true;
    if (u != POS_INF) {
        if (b == NONE) {
            no_reload(A, start, u, ch->vreg);
            return;
        }
        uint32_t g = split(A, c, b);
        if (g != NONE)
            unhandled_add(A, g);
    }
}

static bool alloc_blocked(struct ra *A, uint32_t idx)
{
    struct interval *cur = &A->iv[idx];
    uint32_t use_pos[T1_X86_NREGS], block_pos[T1_X86_NREGS];
    uint16_t pool = A->m->allocatable;
    uint32_t start = iv_start(A, cur);
    for (unsigned r = 0; r < T1_X86_NREGS; ++r)
        use_pos[r] = block_pos[r] = (pool & T1_X86_REGBIT(r)) ? POS_INF : 0;
    for (unsigned k = 0; k < A->nact; ++k) {
        const struct interval *it = &A->iv[A->active[k]];
        uint32_t u = next_wanted(A, it, start);
        if (u < use_pos[it->reg])
            use_pos[it->reg] = u;
    }
    for (unsigned k = 0; k < A->ninact; ++k) {
        const struct interval *it = &A->iv[A->inactive[k]];
        if (intersect(ranges(A, it), it->r_n, ranges(A, cur), cur->r_n, start) == POS_INF)
            continue;
        uint32_t u = next_wanted(A, it, start);
        if (u < use_pos[it->reg])
            use_pos[it->reg] = u;
    }
    for (unsigned r = 0; r < T1_X86_NREGS; ++r) {
        if (!use_pos[r])
            continue;
        uint32_t x = fixed_intersect(A, r, cur);
        if (x == start) {
            use_pos[r] = block_pos[r] = 0;
            continue;
        }
        if (x < block_pos[r])
            block_pos[r] = x;
        if (x < use_pos[r])
            use_pos[r] = x;
    }
    uint8_t best = pick(use_pos, hint_reg(A, cur));
    uint32_t wanted = next_wanted(A, cur, start), first = next_reg_use(A, cur, start);
    if (best == T1_X86_NOREG || wanted == POS_INF || use_pos[best] < wanted ||
        (use_pos[best] == wanted && first != wanted)) {
        /* Every register is wanted before cur wants one: cur waits in its
         * slot until its first register use (if any). */
        if (first == start)
            return fail(A, "register pressure at a definition");
        cur->spilled = true;
        if (first != POS_INF) {
            uint32_t b = boundary_at_or_before(A, start, first);
            if (b == NONE) {
                no_reload(A, start, first, cur->vreg);
                return false;
            }
            uint32_t c = split(A, idx, b);
            if (c == NONE)
                return false;
            unhandled_add(A, c);
        }
        return true;
    }
    if (use_pos[best] <= start)
        return fail(A, "register pressure");
    cur->reg = best;
    if (block_pos[best] < iv_end(A, cur)) {
        uint32_t b = boundary_at_or_before(A, start, block_pos[best]);
        if (b == NONE)
            return fail(A, "no split before a fixed register use");
        uint32_t c = split(A, idx, b);
        if (c == NONE)
            return false;
        unhandled_add(A, c);
    }
    /* Evict the other holders of best where they meet cur. */
    for (unsigned k = 0; k < A->nact;) {
        uint32_t o = A->active[k];
        if (A->iv[o].reg != best) {
            ++k;
            continue;
        }
        remove_at(A->active, &A->nact, k);
        spill_from(A, o, start);
    }
    for (unsigned k = 0; k < A->ninact;) {
        uint32_t o = A->inactive[k];
        const struct interval *it = &A->iv[o];
        if (it->reg != best) {
            ++k;
            continue;
        }
        uint32_t x =
            intersect(ranges(A, it), it->r_n, ranges(A, &A->iv[idx]), A->iv[idx].r_n, start);
        if (x == POS_INF) {
            ++k;
            continue;
        }
        /* The part before x lies in cur's holes and keeps the register. */
        spill_from(A, o, x);
        if (iv_start(A, &A->iv[o]) >= x || iv_end(A, &A->iv[o]) <= start)
            remove_at(A->inactive, &A->ninact, k);
        else
            ++k;
    }
    return !A->failed;
}

static bool linear_scan(struct ra *A)
{
    unhandled_init(A);
    while (A->nun && !A->failed) {
        uint32_t idx = A->unhandled[--A->nun];
        uint32_t pos = iv_start(A, &A->iv[idx]);
        for (unsigned k = 0; k < A->nact;) {
            const struct interval *it = &A->iv[A->active[k]];
            if (iv_end(A, it) <= pos)
                remove_at(A->active, &A->nact, k);
            else if (!covers(A, it, pos)) {
                A->inactive[A->ninact++] = A->active[k];
                remove_at(A->active, &A->nact, k);
            } else
                ++k;
        }
        for (unsigned k = 0; k < A->ninact;) {
            const struct interval *it = &A->iv[A->inactive[k]];
            if (iv_end(A, it) <= pos)
                remove_at(A->inactive, &A->ninact, k);
            else if (covers(A, it, pos)) {
                A->active[A->nact++] = A->inactive[k];
                remove_at(A->inactive, &A->ninact, k);
            } else
                ++k;
        }
        /* No instruction needs it in a register (frames and copies of
         * restoration-only values): it lives in its slot, which costs no
         * move at all. */
        if (next_wanted(A, &A->iv[idx], pos) == POS_INF) {
            A->iv[idx].spilled = true;
            continue;
        }
        if (!try_free(A, idx) && !alloc_blocked(A, idx))
            break;
        const struct interval *cur = &A->iv[idx];
        if (cur->reg != T1_X86_NOREG) {
            if (covers(A, cur, pos))
                A->active[A->nact++] = idx;
            else
                A->inactive[A->ninact++] = idx;
        }
    }
    return !A->failed;
}

/* ---- locations ---------------------------------------------------------- */

static bool block_start(const struct ra *A, uint32_t pos);

static struct t1_loc loc_of_iv(struct ra *A, const struct interval *it)
{
    struct t1_loc l = {0};
    if (it->reg != T1_X86_NOREG) {
        l.kind = T1_LOC_REG;
        l.reg = it->reg;
    } else {
        l.kind = T1_LOC_SPILL;
        l.slot = (uint16_t)A->slot[it->vreg];
    }
    return l;
}

static const struct interval *child_at(struct ra *A, t1_vreg v, uint32_t pos)
{
    for (uint32_t c = v; c != NONE; c = A->iv[c].next)
        if (covers(A, &A->iv[c], pos))
            return &A->iv[c];
    return NULL;
}

static bool loc_at(struct ra *A, t1_vreg v, uint32_t pos, struct t1_loc *out)
{
    const struct interval *it = v < A->m->nvregs ? child_at(A, v, pos) : NULL;
    if (!it)
        return false;
    *out = loc_of_iv(A, it);
    return true;
}

static bool same_loc(const struct t1_loc *a, const struct t1_loc *b)
{
    if (a->kind != b->kind)
        return false;
    if (a->kind == T1_LOC_REG)
        return a->reg == b->reg;
    if (a->kind == T1_LOC_SPILL)
        return a->slot == b->slot;
    return a->value == b->value;
}

/* The slot of a vreg holds its value over its stack children, from the
 * boundary where the store into the child happens (for a child evicted
 * inside an instruction, the boundary before it). */
/* The stack-resident ranges of child x, extended to where its slot is
 * written: the boundary before a child that starts inside an instruction,
 * and, for every range starting at a block start, the end of each
 * single-successor predecessor from its terminator on (the outgoing edge
 * moves run there). Returns the number of ranges written to out. */
static unsigned slot_ranges(struct ra *A, const struct interval *x, bool stored_before,
                            struct range *out, unsigned cap)
{
    const struct t1_mir *m = A->m;
    unsigned n = 0;
    for (unsigned k = 0; k < x->r_n && n < cap; ++k) {
        struct range r = A->rg[x->r_first + k];
        if (k == 0 && stored_before)
            r.from = POS_B(POS_INSN(r.from));
        out[n++] = r;
        if (!block_start(A, A->rg[x->r_first + k].from))
            continue;
        uint32_t first = POS_INSN(A->rg[x->r_first + k].from);
        for (unsigned b = 0; b < m->nblocks && n < cap; ++b) {
            const struct t1_mir_block *p = &m->block[b];
            if (p->nsucc != 1 || !p->count || m->block[p->succ[0]].first != first)
                continue;
            out[n++] = (struct range){POS_B(p->term), POS_B(p->first + p->count)};
        }
    }
    sort_ranges(out, n);
    return n;
}

/* A child evicted inside an instruction is stored at the boundary before
 * it; a slot written by a definition (a parallel copy into the slot) is
 * written at the definition itself. */
static bool stored_before(struct ra *A, t1_vreg v, const struct interval *x)
{
    uint32_t s = iv_start(A, x);
    if (s == POS_B(POS_INSN(s)))
        return false;
    for (uint32_t c = v; c != NONE; c = A->iv[c].next)
        if (A->iv[c].next != NONE && &A->iv[A->iv[c].next] == x)
            return iv_end(A, &A->iv[c]) == s;
    return false;
}

static bool slot_ranges_overlap(struct ra *A, t1_vreg vx, const struct interval *x, t1_vreg vy,
                                const struct interval *y)
{
    struct range a[64], b[64];
    unsigned na = slot_ranges(A, x, stored_before(A, vx, x), a, 64);
    unsigned nb = slot_ranges(A, y, stored_before(A, vy, y), b, 64);
    if (na == 64 || nb == 64) /* too fragmented to bother: assume they meet */
        return true;
    return intersect(a, na, b, nb, 0) != POS_INF;
}

static bool vregs_overlap(struct ra *A, t1_vreg a, t1_vreg b)
{
    for (uint32_t x = a; x != NONE; x = A->iv[x].next) {
        if (!A->iv[x].spilled || !A->iv[x].r_n)
            continue;
        for (uint32_t y = b; y != NONE; y = A->iv[y].next)
            if (A->iv[y].spilled && A->iv[y].r_n &&
                slot_ranges_overlap(A, a, &A->iv[x], b, &A->iv[y]))
                return true;
    }
    return false;
}

/* Spill slots: one per vreg with a stack child; two vregs share a slot
 * only if their stack children (with their stores) never overlap. A stack
 * child is always entered through a store or a direct definition into the
 * slot (resolution moves on edges included), so the slot is needed only
 * there. */
static uint32_t find(uint32_t *parent, uint32_t x)
{
    while (parent[x] != x)
        x = parent[x] = parent[parent[x]];
    return x;
}

static bool slot_free_for(struct ra *A, uint32_t head, const uint32_t *next, t1_vreg v)
{
    for (uint32_t w = head; w != NONE; w = next[w])
        if (vregs_overlap(A, v, (t1_vreg)w))
            return false;
    return true;
}

static bool assign_slots(struct ra *A)
{
    struct t1_mir *m = A->m;
    unsigned nslots = 0;
    /* Copy partners (edge copies, tie copies) prefer one slot, so a copy
     * between two of them in memory is no move at all. */
    uint32_t *parent = A->unhandled; /* free after the scan */
    for (unsigned v = 0; v < m->nvregs; ++v)
        parent[v] = v;
    for (uint32_t i = 0; i < m->ninsns; ++i) {
        const struct t1_mir_insn *in = &m->insn[i];
        if (in->op != T1_MIR_PCOPY)
            continue;
        for (uint32_t c = in->copies_first; c < in->copies_first + in->ncopies; ++c) {
            const struct t1_mir_copy *cp = &m->copy[c];
            if (cp->src != T1_VREG_NONE)
                parent[find(parent, cp->dst)] = find(parent, cp->src);
        }
    }
    /* These lists are dead after linear_scan. Copy-group members are
     * linked in ascending vreg order, preserving the old preference. */
    uint32_t *group_next = A->active, *group_first = A->inactive;
    for (unsigned v = 0; v < m->nvregs; ++v) {
        parent[v] = find(parent, v);
        group_first[v] = NONE;
        A->slot[v] = -1;
    }
    for (unsigned v = m->nvregs; v-- > 0;) {
        uint32_t group = parent[v];
        group_next[v] = group_first[group];
        group_first[group] = v;
    }
    for (unsigned v = 0; v < m->nvregs; ++v)
        parent[v] = group_first[parent[v]];
    /* Every assigned vreg is also linked from its chosen slot. */
    uint32_t *slot_next = A->inactive, slot_head[T1_SPILL_SCRATCH];
    for (unsigned s = 0; s < T1_SPILL_SCRATCH; ++s)
        slot_head[s] = NONE;
    for (unsigned v = 0; v < m->nvregs; ++v) {
        bool spilled = false;
        for (uint32_t c = v; c != NONE; c = A->iv[c].next)
            spilled |= A->iv[c].spilled;
        if (!spilled)
            continue;
        int16_t chosen = -1;
        for (uint32_t w = parent[v]; w < v && chosen < 0; w = group_next[w])
            if (A->slot[w] >= 0 && slot_free_for(A, slot_head[A->slot[w]], slot_next, (t1_vreg)v))
                chosen = A->slot[w];
        /* A fresh slot before sharing one with an unrelated value, so that
         * later members of a copy group still find their group's slot. */
        if (chosen < 0 && nslots < T1_SPILL_SCRATCH)
            chosen = (int16_t)nslots++;
        for (unsigned s = 0; s < nslots && chosen < 0; ++s)
            if (slot_free_for(A, slot_head[s], slot_next, (t1_vreg)v))
                chosen = (int16_t)s;
        if (chosen < 0) {
            A->m->capacity_failure = true;
            return fail(A, "spill slots exhausted");
        }
        A->slot[v] = chosen;
        slot_next[v] = slot_head[chosen];
        slot_head[chosen] = v;
    }
    m->spill_slots = nslots;
    return true;
}

/* ---- resolution --------------------------------------------------------- */

/* Whether some instruction (or a loop end) needs v in a register after pos:
 * then a store of v is a spill of a value still to be computed with, not
 * of a restoration-only value. */
static bool reg_use_after(struct ra *A, t1_vreg v, uint32_t pos)
{
    for (uint32_t c = v; c != NONE; c = A->iv[c].next)
        if (next_wanted(A, &A->iv[c], pos + 1) != POS_INF)
            return true;
    return false;
}

/* home_from: where the destination becomes the value's location. */
static void add_move(struct ra *A, uint32_t at, uint8_t phase, t1_vreg v, struct t1_loc dst,
                     struct t1_loc src, uint32_t home_from)
{
    if (same_loc(&dst, &src))
        return;
    if (A->nmv >= A->mv_cap) {
        fail(A, "move list");
        return;
    }
    bool later = src.kind == T1_LOC_SPILL ||
                 (dst.kind == T1_LOC_SPILL && reg_use_after(A, v, home_from - 1));
    A->mv[A->nmv++] = (struct move){at, phase, v, dst, src, later};
}

/* Spill statistics, inside the loop only: a move touching a slot
 * is a loop-value spill if the value is still needed in a register later
 * (a reload, or a store that a reload will follow), else restoration-only. */
static void count_move(struct ra *A, bool loop, const struct t1_loc *d, const struct t1_loc *s,
                       bool later)
{
    struct t1_mir *m = A->m;
    if (same_loc(d, s) || (d->kind != T1_LOC_SPILL && s->kind != T1_LOC_SPILL))
        return;
    ++m->total_spills;
    if (!loop)
        return;
    if (later)
        ++m->loop_value_spills;
    else
        ++m->restore_only_spills;
}

static bool block_start(const struct ra *A, uint32_t pos)
{
    uint32_t i = POS_INSN(pos);
    return pos == POS_B(i) && i < A->ninsns && A->block_first[i];
}

static void resolve_splits(struct ra *A)
{
    struct t1_mir *m = A->m;
    for (unsigned v = 0; v < m->nvregs; ++v) {
        for (uint32_t p = v; p != NONE && A->iv[p].next != NONE; p = A->iv[p].next) {
            const struct interval *a = &A->iv[p], *c = &A->iv[a->next];
            if (!a->r_n || !c->r_n)
                continue;
            uint32_t s = iv_start(A, c);
            if (iv_end(A, a) != s || block_start(A, s))
                continue;
            /* The move goes to the boundary before the instruction holding
             * s; a child starting inside an instruction is a stack child. */
            uint32_t at = POS_B(POS_INSN(s));
            if (at != s && !c->spilled) {
                fail(A, "register child inside an instruction");
                return;
            }
            add_move(A, at, at != s ? PHASE_SPILL : PHASE_SPLIT, (t1_vreg)v, loc_of_iv(A, c),
                     loc_of_iv(A, a), s);
        }
    }
}

static void resolve_edges(struct ra *A)
{
    struct t1_mir *m = A->m;
    unsigned W = A->nwords;
    for (unsigned b = 0; b < m->nblocks; ++b) {
        const struct t1_mir_block *blk = &m->block[b];
        if (!blk->count)
            continue;
        uint32_t end_pos = POS_B(blk->first + blk->count) - 1;
        for (unsigned s = 0; s < blk->nsucc; ++s) {
            const struct t1_mir_block *sb = &m->block[blk->succ[s]];
            uint32_t at;
            uint8_t phase;
            if (blk->nsucc == 1) {
                at = POS_B(blk->term);
                phase = PHASE_OUT;
            } else {
                at = POS_B(sb->first);
                phase = PHASE_IN;
            } /* edge blocks: one predecessor */
            const uint64_t *in = &A->live_in[blk->succ[s] * W];
            for (unsigned w = 0; w < W; ++w)
                for (uint64_t bits = in[w]; bits; bits &= bits - 1) {
                    unsigned v = 64 * w + (unsigned)__builtin_ctzll(bits);
                    struct t1_loc from, to;
                    if (!loc_at(A, (t1_vreg)v, end_pos, &from) ||
                        !loc_at(A, (t1_vreg)v, POS_B(sb->first), &to)) {
                        fail(A, "edge value without a location");
                        return;
                    }
                    add_move(A, at, phase, (t1_vreg)v, to, from, POS_B(sb->first));
                }
        }
    }
}

/* ---- rewriting ---------------------------------------------------------- */

static bool alloc_operand(struct ra *A, struct t1_mir_opnd *o, uint32_t pos)
{
    struct t1_loc l;
    if (!loc_at(A, o->vreg, pos, &l))
        return fail(A, "operand without a location");
    if (l.kind != T1_LOC_REG) {
        LOGE("regalloc: v%u is in slot %u at %u", o->vreg, l.slot, pos);
        dump_vreg(A, o->vreg);
        return fail(A, "register operand in memory");
    }
    o->fixed = l.reg;
    return true;
}

static int cmp_move(const struct move *a, const struct move *b)
{
    if (a->at != b->at)
        return a->at < b->at ? -1 : 1;
    return a->phase < b->phase ? -1 : a->phase > b->phase;
}

static void sort_moves(struct move *mv, unsigned n)
{
    for (unsigned i = 1; i < n; ++i) {
        struct move x = mv[i];
        unsigned j = i;
        while (j && cmp_move(&mv[j - 1], &x) > 0) {
            mv[j] = mv[j - 1];
            --j;
        }
        mv[j] = x;
    }
}

static struct t1_mir_insn *emit_moves(struct ra *A, unsigned *k, uint32_t at,
                                      struct t1_mir_insn *out, unsigned *n)
{
    struct t1_mir *m = A->m;
    while (*k < A->nmv && A->mv[*k].at == at) {
        unsigned first = *k;
        uint8_t phase = A->mv[first].phase;
        while (*k < A->nmv && A->mv[*k].at == at && A->mv[*k].phase == phase)
            ++*k;
        uint32_t c = t1_mir_new_copies(m, *k - first);
        if (c == T1_MIR_NONE) {
            fail(A, "copy limit");
            return NULL;
        }
        for (unsigned q = first; q < *k; ++q) {
            struct t1_mir_copy *cp = &m->copy[c + q - first];
            cp->dst = cp->src = A->mv[q].vreg;
            cp->dloc = A->mv[q].dst;
            cp->sloc = A->mv[q].src;
            count_move(A, A->m->block[A->cur_block].loop, &cp->dloc, &cp->sloc,
                       A->mv[q].needed_later);
        }
        struct t1_mir_insn *mi = &out[(*n)++];
        memset(mi, 0, sizeof(*mi));
        mi->op = T1_MIR_PCOPY;
        mi->tie_def = mi->tie_use = 0xff;
        mi->cc = T1_CC_COUNT;
        mi->point = UINT32_MAX;
        mi->copies_first = c;
        mi->ncopies = *k - first;
    }
    return out;
}

static void rewrite_insn(struct ra *A, struct t1_mir_insn *mi, uint32_t i, uint32_t ni)
{
    struct t1_mir *m = A->m;
    if (mi->op == T1_MIR_PCOPY) {
        for (uint32_t c = mi->copies_first; c < mi->copies_first + mi->ncopies; ++c) {
            struct t1_mir_copy *cp = &m->copy[c];
            if (!loc_at(A, cp->dst, POS_D(i), &cp->dloc)) {
                fail(A, "copy destination");
                return;
            }
            if (cp->src == T1_VREG_NONE)
                cp->sloc = (struct t1_loc){T1_LOC_CONST, 0, 0, cp->src_imm};
            else if (!loc_at(A, cp->src, POS_U(i), &cp->sloc)) {
                fail(A, "copy source");
                return;
            }
            bool later = cp->sloc.kind == T1_LOC_SPILL ||
                         (cp->dloc.kind == T1_LOC_SPILL && reg_use_after(A, cp->dst, POS_D(i)));
            count_move(A, m->block[A->cur_block].loop, &cp->dloc, &cp->sloc, later);
        }
    } else {
        struct t1_mir_effects fx;
        t1_mir_effects(mi, &fx);
        for (unsigned q = 0; q < fx.nuse; ++q)
            if (fx.use[q] < T1_MIR_MAX_OPNDS)
                alloc_operand(A, &mi->opnd[fx.use[q]], POS_U(i));
        for (unsigned q = 0; q < fx.ndef; ++q)
            alloc_operand(A, &mi->opnd[fx.def[q]], POS_D(i));
        m->memreg[ni].base = m->memreg[ni].index = T1_X86_NOREG;
        for (unsigned q = 0; q < 2; ++q) {
            uint8_t kind = q ? mi->mem.index_kind : mi->mem.base_kind;
            if (kind != T1_MO_VREG)
                continue;
            struct t1_mir_opnd tmp = {T1_MO_VREG, T1_X86_NOREG, q ? mi->mem.index : mi->mem.base,
                                      0};
            if (!alloc_operand(A, &tmp, POS_U(i)))
                return;
            if (q)
                m->memreg[ni].index = tmp.fixed;
            else
                m->memreg[ni].base = tmp.fixed;
        }
    }
    if (mi->point != UINT32_MAX && m->point[mi->point].insn == i) {
        const struct t1_mir_point *p = &m->point[mi->point];
        for (unsigned q = 0; q < p->frame_count; ++q) {
            const struct t1_mir_frame *f = &m->frame[p->frame_first + q];
            struct t1_loc *l = &m->frame_loc[p->frame_first + q];
            if (f->kind == T1_MF_HOME)
                *l = (struct t1_loc){T1_LOC_HOME, 0, 0, 0};
            else if (f->kind == T1_MF_CONST)
                *l = (struct t1_loc){T1_LOC_CONST, 0, 0, f->value};
            else if (!loc_at(A, f->vreg, POS_U(i), l))
                fail(A, "frame value without a location");
        }
    }
}

static bool rewrite(struct ra *A)
{
    struct t1_mir *m = A->m;
    sort_moves(A->mv, A->nmv);
    if (m->ninsns + A->nmv + 1 > m->insn_cap)
        return fail(A, "MIR instruction limit after allocation");
    /* Every old instruction survives and each nonempty move group adds
     * at most one instruction per move. The checked bound is enough for
     * this temporary; the MIR's permanent instruction capacity is larger. */
    size_t bytes = (size_t)(m->ninsns + A->nmv + 1) * sizeof(struct t1_mir_insn);
    size_t pages = (bytes + 4095) / 4096;
    struct t1_mir_insn *out = t1_ws_alloc(m->ir->ws, pages, 0);
    size_t npages = ((m->ninsns + 1) * sizeof(uint32_t) + 4095) / 4096;
    uint32_t *newpos = t1_ws_alloc(m->ir->ws, npages, 0);
    if (!out || !newpos) {
        t1_ws_free(m->ir->ws, newpos, npages);
        t1_ws_free(m->ir->ws, out, pages);
        return fail(A, "no memory");
    }
    unsigned n = 0, k = 0;
    m->loop_value_spills = m->restore_only_spills = m->total_spills = 0;
    for (unsigned b = 0; b < m->nblocks; ++b) {
        struct t1_mir_block *blk = &m->block[b];
        uint32_t first = n, term = n;
        A->cur_block = b;
        for (uint32_t i = blk->first; i < blk->first + blk->count && !A->failed; ++i) {
            if (k < A->nmv && A->mv[k].at < POS_B(i)) {
                fail(A, "move outside its block");
                break;
            }
            emit_moves(A, &k, POS_B(i), out, &n);
            if (i == blk->term)
                term = n;
            newpos[i] = n;
            out[n] = m->insn[i];
            rewrite_insn(A, &out[n], i, n);
            ++n;
        }
        if (blk->term >= blk->first + blk->count)
            term = n;
        blk->first = first;
        blk->count = n - first;
        blk->term = term;
    }
    if (k != A->nmv)
        fail(A, "unplaced moves");
    for (unsigned p = 0; p < m->ir->npoints; ++p) {
        if (m->point[p].insn != T1_MIR_NONE)
            m->point[p].insn = newpos[m->point[p].insn];
        if (m->point[p].force_insn != T1_MIR_NONE)
            m->point[p].force_insn = newpos[m->point[p].force_insn];
    }
    memcpy(m->insn, out, (size_t)n * sizeof(struct t1_mir_insn));
    m->ninsns = n;
    t1_ws_free(m->ir->ws, newpos, npages);
    t1_ws_free(m->ir->ws, out, pages);
    return !A->failed;
}

/* ---- driver -------------------------------------------------------------- */

/* Boundaries that never hold a move: inside a terminator sequence (after
 * its first instruction; moves before it run in phases: split moves, then
 * the outgoing edge's) and between a FORCE and the site or helper of its
 * point. */
static void mark_no_split(struct ra *A)
{
    const struct t1_mir *m = A->m;
    for (unsigned b = 0; b < m->nblocks; ++b) {
        const struct t1_mir_block *blk = &m->block[b];
        if (blk->count)
            A->block_first[blk->first] = 1;
        for (uint32_t i = blk->term + 1; i < blk->first + blk->count; ++i)
            A->no_split[i] = 1;
    }
    for (uint32_t i = 1; i < m->ninsns; ++i) {
        const struct t1_mir_insn *in = &m->insn[i], *prev = &m->insn[i - 1];
        if ((in->op == T1_MIR_SITE || in->op == T1_MIR_HELPER) && prev->op == T1_MIR_FORCE &&
            prev->point == in->point)
            A->no_split[i] = 1;
    }
}

static bool allocate(struct ra *A, struct t1_mir *m)
{
    A->m = m;
    A->ninsns = m->ninsns;
    A->nwords = (m->nvregs + 63) / 64;
    if (!A->nwords)
        A->nwords = 1;
    A->iv_cap = m->nvregs * 4 + 1024;
    A->rg_cap = m->nvregs * 8 + m->nblocks * 64 + 4096;
    A->up_cap = m->ninsns * 8 + m->nframe + 4096;
    A->fx_cap = m->ninsns + m->nvregs * 4 + 64;
    A->un_cap = A->iv_cap;
    A->mv_cap = m->nvregs * 4 + m->nblocks * 64 + 1024;
    size_t W = A->nwords * m->nblocks;
    size_t bytes = 4 * W * sizeof(uint64_t) + A->iv_cap * sizeof(struct interval) +
                   A->rg_cap * sizeof(struct range) + A->up_cap * sizeof(struct usepos) +
                   T1_X86_NREGS * A->fx_cap * sizeof(struct range) +
                   3 * A->un_cap * sizeof(uint32_t) + m->nvregs * sizeof(int16_t) +
                   A->mv_cap * sizeof(struct move) + 2 * (m->ninsns + 16) + 64 * 32;
    A->mem_pages = (bytes + 4095) / 4096;
    uint8_t *p = t1_ws_alloc(m->ir->ws, A->mem_pages, 0);
    if (!p)
        return t1_mir_fail(m, "no memory for allocation");
    A->mem = p;
    A->live_in = carve(&p, W * sizeof(uint64_t));
    A->live_out = carve(&p, W * sizeof(uint64_t));
    A->gen = carve(&p, W * sizeof(uint64_t));
    A->kill = carve(&p, W * sizeof(uint64_t));
    A->iv = carve(&p, A->iv_cap * sizeof(struct interval));
    A->rg = carve(&p, A->rg_cap * sizeof(struct range));
    A->up = carve(&p, A->up_cap * sizeof(struct usepos));
    for (unsigned r = 0; r < T1_X86_NREGS; ++r)
        A->fx[r] = carve(&p, A->fx_cap * sizeof(struct range));
    A->unhandled = carve(&p, A->un_cap * sizeof(uint32_t));
    A->active = carve(&p, A->un_cap * sizeof(uint32_t));
    A->inactive = carve(&p, A->un_cap * sizeof(uint32_t));
    A->slot = carve(&p, m->nvregs * sizeof(int16_t));
    A->mv = carve(&p, A->mv_cap * sizeof(struct move));
    A->no_split = carve(&p, m->ninsns + 16);
    A->block_first = carve(&p, m->ninsns + 16);
    /* The interval, range, use, list and move pools are written before
     * their counts expose them. Only liveness and instruction markers
     * need an empty initial state; do not clear the unused pool capacity. */
    memset(A->live_in, 0, W * sizeof(*A->live_in));
    memset(A->live_out, 0, W * sizeof(*A->live_out));
    memset(A->gen, 0, W * sizeof(*A->gen));
    memset(A->kill, 0, W * sizeof(*A->kill));
    memset(A->no_split, 0, m->ninsns + 16);
    memset(A->block_first, 0, m->ninsns + 16);
    mark_no_split(A);
    liveness(A);
    bool ok = build(A) && linear_scan(A) && assign_slots(A);
    if (ok) {
        resolve_splits(A);
        resolve_edges(A);
        ok = !A->failed;
    }
    if (ok)
        for (unsigned v = 0; v < m->nvregs; ++v) {
            if (!(m->vreg[v].flags & T1_VR_LOOP_PARAM))
                continue;
            /* At the start of its own header's block. */
            uint16_t header = m->ir->op[T1_VALUE_OP(m->vreg[v].value)].block;
            unsigned mb = 0;
            while (mb < m->nblocks && m->block[mb].ir_block != header)
                ++mb;
            if (mb == m->nblocks ||
                !loc_at(A, (t1_vreg)v, POS_B(m->block[mb].first), &m->vreg[v].header_loc))
                m->vreg[v].header_loc = (struct t1_loc){0xff, T1_X86_NOREG, 0, 0};
        }
    if (ok)
        ok = rewrite(A);
    t1_ws_free(m->ir->ws, A->mem, A->mem_pages);
    if (!ok)
        return t1_mir_fail(m, A->why ? A->why : "register allocation failed");
    return true;
}

bool t1_regalloc(struct t1_mir *m)
{
    size_t pages = (sizeof(struct ra) + 4095) / 4096;
    struct ra *A = t1_ws_alloc(m->ir->ws, pages, HAL_PAGE_ZERO);
    if (!A)
        return t1_mir_fail(m, "no memory for allocation");
    bool ok = allocate(A, m);
    t1_ws_free(m->ir->ws, A, pages);
    return ok;
}
