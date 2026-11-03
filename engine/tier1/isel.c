/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Select MIR by minimum-cost tree cover over generated, proved rules;
 * table order breaks ties. Cut values with multiple consumers or uses outside
 * their tree (including frames, edges, sites and helpers). Constants match by
 * value and materialize only when needed as operands.
 *
 * Lower ties and fixed-register constraints to copies/precolored vregs before
 * allocation. An absorbed multi-result node must expose every used result.
 * Format-2 immediate checks must hold; LOAD/STORE rules belong to memory sites,
 * not pure-value covers. Uncovered pure operations end the region before the
 * source instruction (fail_insn).
 */
#include "engine/tier1/codegen.h"
#include "build/tier1/isel_rules.h"
#include "engine/tier1/form.h"
#include "ia64vmm/hal.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define INF_COST UINT32_MAX
#define MAX_BINDERS 8u
_Static_assert(T1_RULES_FORMAT == 2, "isel implements rule format 2 (rules.h)");

/* Per value (op << 2 | result) facts. */
#define VF_NEEDS_VREG 1u  /* used by an edge, a frame, an access, a helper */
#define VF_OTHER_BLOCK 2u /* used outside its defining block */
#define VF_PRODUCED 4u    /* defined as an extra output of another cover */
#define VF_MEMO 8u        /* cover memo valid */

struct bind {
    t1_value in[MAX_BINDERS];
    int64_t imm[MAX_BINDERS];
    bool imm_set[MAX_BINDERS];
};

struct isel {
    struct t1_mir *m;
    const struct t1_ir *ir;
    uint32_t flags;
    unsigned host_isa;
    unsigned nvalues;
    uint16_t *uses;                        /* per value */
    uint8_t *vflags;                       /* per value */
    t1_vreg *vreg_of;                      /* per value */
    uint32_t *memo_cost;                   /* per value */
    uint16_t *memo_rule;                   /* per value */
    struct t1_known *kb;                   /* per op */
    uint16_t mblock[T1_MAX_BLOCKS];        /* IR block -> MIR block */
    uint16_t edge_block[T1_MAX_BLOCKS][2]; /* BRANCH succ s -> MIR edge block */
    uint16_t order[T1_MAX_BLOCKS];
    unsigned norder;
    bool in_loop[T1_MAX_BLOCKS];
    struct t1_ir_loops loops;
    uint16_t cur_block; /* IR block being selected */
    size_t pages;
};

static t1_value resolve(const struct isel *S, t1_value v)
{
    return t1_ir_resolve(S->ir, v);
}
static const struct t1_op *def_op(const struct isel *S, t1_value v)
{
    return &S->ir->op[T1_VALUE_OP(v)];
}
static bool is_const(const struct isel *S, t1_value v)
{
    return def_op(S, v)->code == T1_IR_CONST;
}
static t1_value operand(const struct isel *S, uint32_t o, unsigned i)
{
    return resolve(S, t1_ir_opnd(S->ir, o, i)->value);
}

/* ---- linear block order ------------------------------------------------ */

static uint16_t forward_succ(const struct t1_ir *ir, uint16_t b, unsigned s)
{
    uint32_t t = t1_ir_terminator(ir, b);
    if (t != T1_NO_OP && ir->op[t].code == T1_IR_BACKEDGE)
        return T1_NO_BLOCK;
    return s < ir->block[b].nsucc ? ir->block[b].succ[s].target : T1_NO_BLOCK;
}

static void rpo_visit(const struct t1_ir *ir, uint16_t b, bool *seen, uint16_t *post, unsigned *n)
{
    seen[b] = true;
    for (unsigned s = 0; s < 2; ++s) {
        uint16_t t = forward_succ(ir, b, s);
        if (t != T1_NO_BLOCK && !seen[t] && !(ir->block[t].flags & T1_BLK_DEAD))
            rpo_visit(ir, t, seen, post, n);
    }
    post[(*n)++] = b;
}

/* Places the blocks of `region` in reverse postorder; reaching the header
 * of a loop other than `own` places that loop's blocks at once. */
static void place(struct isel *S, const uint16_t *post, unsigned n, uint64_t region, uint16_t own,
                  uint64_t *placed)
{
    for (unsigned k = n; k-- > 0;) {
        uint16_t b = post[k];
        uint64_t bit = UINT64_C(1) << b;
        if (!(region & bit) || (*placed & bit))
            continue;
        if (S->loops.body[b] && b != own) {
            place(S, post, n, S->loops.body[b], b, placed);
            continue;
        }
        *placed |= bit;
        S->order[S->norder++] = b;
    }
}

/* The entry block, then the rest in reverse postorder with every loop body
 * contiguous: a loop's blocks are placed at
 * once when its header, the first of them in reverse postorder, is
 * reached, inner loops likewise; blocks after a loop's exits follow it. */
static void linear_order(struct isel *S)
{
    const struct t1_ir *ir = S->ir;
    bool seen[T1_MAX_BLOCKS] = {0};
    uint16_t post[T1_MAX_BLOCKS];
    unsigned n = 0;
    rpo_visit(ir, T1_ENTRY_BLOCK, seen, post, &n);
    t1_ir_loops(ir, &S->loops);
    uint64_t live = 0;
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        S->in_loop[b] = seen[b] && S->loops.depth[b];
        if (seen[b])
            live |= UINT64_C(1) << b;
    }
    uint64_t placed = UINT64_C(1) << T1_ENTRY_BLOCK;
    S->norder = 0;
    S->order[S->norder++] = T1_ENTRY_BLOCK;
    place(S, post, n, live, T1_NO_BLOCK, &placed);
}

/* The deepest loop holding both blocks (T1_NO_BLOCK if none). */
static uint16_t common_loop(const struct isel *S, uint16_t a, uint16_t b)
{
    uint16_t best = T1_NO_BLOCK;
    for (unsigned h = 0; h < S->ir->nblocks; ++h) {
        uint64_t body = S->loops.body[h];
        if (((body >> a) & 1) && ((body >> b) & 1) &&
            (best == T1_NO_BLOCK || S->loops.depth[h] > S->loops.depth[best]))
            best = (uint16_t)h;
    }
    return best;
}

/* ---- use analysis ------------------------------------------------------ */

static unsigned vidx(t1_value v)
{
    return T1_VALUE_OP(v) * 4u + T1_VALUE_RESULT(v);
}

static void count_use(struct isel *S, t1_value v, uint16_t at_block, bool needs_vreg)
{
    if (v == T1_VALUE_NONE)
        return;
    v = resolve(S, v);
    if (t1_ir_value_type(S->ir, v) == T1_MEM)
        return;
    unsigned i = vidx(v);
    if (S->uses[i] < UINT16_MAX)
        ++S->uses[i];
    if (needs_vreg)
        S->vflags[i] |= VF_NEEDS_VREG;
    if (def_op(S, v)->block != at_block)
        S->vflags[i] |= VF_OTHER_BLOCK;
}

static void analyse_uses(struct isel *S)
{
    const struct t1_ir *ir = S->ir;
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        const struct t1_block *blk = &ir->block[b];
        if (blk->flags & T1_BLK_DEAD)
            continue;
        for (uint32_t o = blk->first; o != T1_NO_OP; o = ir->op[o].next) {
            const struct t1_op *op = &ir->op[o];
            uint8_t eff = t1_op_desc(op->code)->effect;
            bool rule_consumer = eff == T1_EFF_PURE || op->code == T1_IR_BRANCH;
            for (unsigned k = 0; k < op->nuse; ++k) {
                t1_value v = t1_ir_opnd(ir, o, k)->value;
                if (v == T1_VALUE_NONE || t1_ir_value_type(ir, resolve(S, v)) == T1_MEM)
                    continue;
                count_use(S, v, (uint16_t)b, !rule_consumer);
            }
            if (op->point != T1_NO_POINT && !ir->point[op->point].dead) {
                const struct t1_ir_point *pt = &ir->point[op->point];
                for (unsigned k = 0; k < pt->frame_count; ++k) {
                    const struct t1_use *u = &ir->use[pt->frame_first + k];
                    if (!(u->flags & T1_USE_HOME))
                        count_use(S, u->value, (uint16_t)b, true);
                }
            }
        }
        for (unsigned s = 0; s < blk->nsucc; ++s)
            for (unsigned k = 0; k < blk->succ[s].nargs; ++k)
                count_use(S, ir->use[blk->succ[s].args_first + k].value, T1_NO_BLOCK, true);
    }
}

/* A value with a register of its own (cut points). */
static bool is_root(const struct isel *S, t1_value v)
{
    const struct t1_op *d = def_op(S, v);
    if (d->code == T1_IR_CONST)
        return false;
    if (t1_op_desc(d->code)->effect != T1_EFF_PURE || d->code == T1_IR_LIVEIN ||
        d->code == T1_IR_NATIN || d->code == T1_IR_HREAD)
        return true;
    unsigned i = vidx(v);
    return S->uses[i] != 1 || (S->vflags[i] & (VF_NEEDS_VREG | VF_OTHER_BLOCK));
}

/* ---- matching ---------------------------------------------------------- */

static uint32_t cover_cost(struct isel *S, t1_value v);
static uint32_t const_cost(struct isel *S, t1_value v);

static bool imm_ok(const struct t1_pat *p, int64_t imm)
{
    if (imm < p->imm_min || imm > p->imm_max)
        return false;
    if (p->imm_set && (imm < 0 || imm > 31 || !((p->imm_set >> imm) & 1)))
        return false;
    return true;
}

/* Interior node at preorder index pi: v must be consumed only here, or be
 * a constant; the op's other used results must be outputs of the rule. */
static bool absorbable(const struct isel *S, const struct t1_rule *r, unsigned pi, t1_value v)
{
    const struct t1_op *d = def_op(S, v);
    if (d->code == T1_IR_CONST)
        return true;
    if (is_root(S, v) || d->block != S->cur_block)
        return false;
    unsigned nres = t1_op_desc(d->code)->nres;
    for (unsigned res = 0; res < nres; ++res) {
        if (res == T1_VALUE_RESULT(v) || !S->uses[T1_VALUE_OP(v) * 4u + res])
            continue;
        bool out = false;
        for (unsigned j = 0; j < r->nout; ++j)
            if (r->out_result[j].bind == pi && r->out_result[j].result == res)
                out = true;
        if (!out)
            return false;
    }
    return true;
}

static bool match(const struct isel *S, const struct t1_rule *r, unsigned pi, t1_value v, bool top,
                  struct bind *B)
{
    const struct t1_pat *p = &r->pat[pi];
    if (p->ir_op == T1_PAT_VALUE) {
        if (p->bind >= MAX_BINDERS)
            return false;
        if (B->in[p->bind] != T1_VALUE_NONE)
            return B->in[p->bind] == v;
        B->in[p->bind] = v;
        return true;
    }
    const struct t1_op *d = def_op(S, v);
    if (d->code != p->ir_op)
        return false;
    if (!top) {
        if (T1_VALUE_RESULT(v) != p->result)
            return false;
        if (!absorbable(S, r, pi, v))
            return false;
    }
    if (!imm_ok(p, d->imm))
        return false;
    if (p->bind != T1_BINDER_NONE) {
        if (p->bind >= MAX_BINDERS)
            return false;
        if (B->imm_set[p->bind] && B->imm[p->bind] != d->imm)
            return false;
        B->imm[p->bind] = d->imm;
        B->imm_set[p->bind] = true;
    }
    if (p->nkids > d->nuse)
        return false;
    for (unsigned k = 0; k < p->nkids; ++k)
        if (!match(S, r, p->kid[k], operand(S, T1_VALUE_OP(v), k), false, B))
            return false;
    return true;
}

static void bind_init(struct bind *B)
{
    for (unsigned i = 0; i < MAX_BINDERS; ++i) {
        B->in[i] = T1_VALUE_NONE;
        B->imm_set[i] = false;
        B->imm[i] = 0;
    }
}

static unsigned in_binders(const struct t1_rule *r)
{
    unsigned n = 0;
    for (unsigned i = 0; i < r->npat; ++i)
        if (r->pat[i].ir_op == T1_PAT_VALUE && r->pat[i].bind + 1u > n)
            n = r->pat[i].bind + 1u;
    return n;
}

/* Cost of the tree at value v (or the BRANCH terminator op, top = op's
 * value with result 0 standing for the op) under rule r, or INF. */
static uint32_t rule_cost(struct isel *S, const struct t1_rule *r, t1_value v, bool branch,
                          struct bind *B)
{
    bind_init(B);
    if (branch != (r->pat[0].ir_op == T1_IR_BRANCH))
        return INF_COST;
    if (!branch) {
        /* The root value must be an output of the rule. */
        if (!(r->root_results & (1u << T1_VALUE_RESULT(v))))
            return INF_COST;
        /* Other used results of the root op too. */
        const struct t1_op *d = def_op(S, v);
        for (unsigned res = 0; res < t1_op_desc(d->code)->nres; ++res) {
            if (res == T1_VALUE_RESULT(v) || !S->uses[T1_VALUE_OP(v) * 4u + res])
                continue;
            if (!(r->root_results & (1u << res)))
                return INF_COST;
        }
    }
    if (!match(S, r, 0, v, true, B))
        return INF_COST;
    if (r->required_isa & ~S->host_isa)
        return INF_COST;
    /* Format 2: relation checks over the bound immediates. */
    for (unsigned k = 0; k < r->ncheck; ++k) {
        int64_t x = t1_iexpr_eval(r->expr, r->check[k].expr, B->imm);
        if (x < r->check[k].min || x > r->check[k].max)
            return INF_COST;
    }
    uint64_t cost = r->cost;
    unsigned n = r->nin;
    for (unsigned i = 0; i < n; ++i) {
        t1_value w = B->in[i];
        if (w == T1_VALUE_NONE)
            return INF_COST;
        uint32_t c;
        if (is_const(S, w))
            c = const_cost(S, w);
        else if (is_root(S, w))
            c = 0;
        else
            c = cover_cost(S, w);
        if (c == INF_COST)
            return INF_COST;
        cost += c;
    }
    return cost >= INF_COST ? INF_COST : (uint32_t)cost;
}

static uint32_t best_rule_scan(struct isel *S, t1_value v, bool branch, unsigned *rule)
{
    uint32_t best = INF_COST;
    struct bind B;
    for (unsigned i = 0; i < t1_rule_count; ++i) {
        uint16_t root = t1_rules[i].pat[0].ir_op;
        if (root == T1_IR_LOAD || root == T1_IR_STORE)
            continue; /* site accesses, not covers */
        /* Reconstruct summaries for the full-scan oracle, independently of
         * the generated metadata used by the indexed selector. */
        struct t1_rule reference = t1_rules[i];
        reference.nin = (uint8_t)in_binders(&reference);
        reference.root_results = 0;
        for (unsigned j = 0; j < reference.nout; ++j)
            if (reference.out_result[j].bind == 0)
                reference.root_results |= (uint8_t)(1u << reference.out_result[j].result);
        reference.required_isa = 0;
        for (unsigned t = 0; t < reference.ntmpl; ++t)
            reference.required_isa |= t1_x86_required_isa(reference.tmpl[t].x86op);
        uint32_t c = rule_cost(S, &reference, v, branch, &B);
        if (c < best) {
            best = c;
            *rule = i;
        }
    }
    return best;
}

#ifdef IA64VMM_HOSTED
bool t1_test_isel_crosscheck;

const uint16_t *t1_test_isel_candidates(unsigned op, unsigned *count);
const uint16_t *t1_test_isel_candidates(unsigned op, unsigned *count)
{
    return t1_isel_candidates(op, count);
}
#endif

static uint32_t best_rule(struct isel *S, t1_value v, bool branch, unsigned *rule)
{
    unsigned op = def_op(S, v)->code;
    if (op >= T1_IR_OP_COUNT)
        return best_rule_scan(S, v, branch, rule);
    unsigned count;
    const uint16_t *candidates = t1_isel_candidates(op, &count);
    uint32_t best = INF_COST;
    struct bind B;
    for (unsigned k = 0; k < count; ++k) {
        uint32_t c = rule_cost(S, &t1_rules[candidates[k]], v, branch, &B);
        if (c < best) {
            best = c;
            *rule = candidates[k];
        }
    }
#ifdef IA64VMM_HOSTED
    if (t1_test_isel_crosscheck) {
        unsigned scan_rule = 0;
        uint32_t scan = best_rule_scan(S, v, branch, &scan_rule);
        if (scan != best || (best != INF_COST && scan_rule != *rule))
            hal_panic("tier1: indexed rule choice differs from the full scan");
    }
#endif
    return best;
}

static uint32_t const_cost(struct isel *S, t1_value v)
{
    unsigned i = vidx(v);
    if (!(S->vflags[i] & VF_MEMO)) {
        unsigned r = 0;
        S->memo_cost[i] = best_rule(S, v, false, &r);
        S->memo_rule[i] = (uint16_t)r;
        S->vflags[i] |= VF_MEMO;
    }
    return S->memo_cost[i];
}

static uint32_t cover_cost(struct isel *S, t1_value v)
{
    return const_cost(S, v); /* the same memo: best rule defining v */
}

/* ---- MIR emission ------------------------------------------------------ */

static struct t1_mir_insn *add(struct isel *S, uint16_t op)
{
    return t1_mir_append(S->m, op);
}

static void opnd_vreg(struct t1_mir_insn *i, unsigned k, t1_vreg v)
{
    i->opnd[k].kind = T1_MO_VREG;
    i->opnd[k].vreg = v;
    i->opnd[k].fixed = T1_X86_NOREG;
    if (k + 1u > i->nopnd)
        i->nopnd = (uint8_t)(k + 1u);
}
static void opnd_imm(struct t1_mir_insn *i, unsigned k, int64_t imm)
{
    i->opnd[k].kind = T1_MO_IMM;
    i->opnd[k].imm = imm;
    if (k + 1u > i->nopnd)
        i->nopnd = (uint8_t)(k + 1u);
}
static void opnd_label(struct t1_mir_insn *i, unsigned k, uint16_t block)
{
    i->opnd[k].kind = T1_MO_LABEL;
    i->opnd[k].imm = block;
    if (k + 1u > i->nopnd)
        i->nopnd = (uint8_t)(k + 1u);
}
static void opnd_none(struct t1_mir_insn *i, unsigned k)
{
    i->opnd[k].kind = T1_MO_NONE;
    if (k + 1u > i->nopnd)
        i->nopnd = (uint8_t)(k + 1u);
}

static t1_vreg new_vreg(struct isel *S, t1_value v)
{
    uint8_t type = v == T1_VALUE_NONE ? T1_I64 : t1_ir_value_type(S->ir, v);
    return t1_mir_new_vreg(S->m, type, v);
}

static t1_vreg copy_of(struct isel *S, t1_vreg src, uint8_t fixed)
{
    t1_vreg c = t1_mir_new_vreg(S->m, S->m->vreg[src].type, T1_VALUE_NONE);
    if (c == T1_VREG_NONE)
        return c;
    S->m->vreg[c].fixed = fixed;
    S->m->vreg[c].flags |= T1_VR_COPY;
    S->m->vreg[c].hint = src;
    struct t1_mir_insn *i = add(S, T1_X86_MOV64_RR);
    if (!i)
        return T1_VREG_NONE;
    opnd_vreg(i, 0, c);
    opnd_vreg(i, 1, src);
    return c;
}

static bool emit_cover(struct isel *S, t1_value v, bool branch, unsigned rule, uint16_t label_t,
                       uint16_t label_f);

/* The vreg holding value v at this point of the block; covers a pending
 * subtree or materializes a constant as needed. */
static t1_vreg value_vreg(struct isel *S, t1_value v)
{
    v = resolve(S, v);
    unsigned i = vidx(v);
    if (S->vreg_of[i] != T1_VREG_NONE && !is_const(S, v))
        return S->vreg_of[i];
    if (is_const(S, v)) {
        uint32_t c = const_cost(S, v);
        if (c == INF_COST) {
            t1_mir_fail(S->m, "no rule for a constant");
            return T1_VREG_NONE;
        }
        /* A fresh vreg per use: rematerialized, never a cut. */
        S->vreg_of[i] = T1_VREG_NONE;
        if (!emit_cover(S, v, false, S->memo_rule[i], 0, 0))
            return T1_VREG_NONE;
        t1_vreg r = S->vreg_of[i];
        S->vreg_of[i] = T1_VREG_NONE;
        return r;
    }
    if (is_root(S, v)) {
        t1_mir_fail(S->m, "use of a root before its definition");
        return T1_VREG_NONE;
    }
    if (cover_cost(S, v) == INF_COST) {
        S->m->fail_insn = def_op(S, v)->insn;
        t1_mir_fail(S->m, "no rule covers an operation");
        return T1_VREG_NONE;
    }
    if (!emit_cover(S, v, false, S->memo_rule[i], 0, 0))
        return T1_VREG_NONE;
    return S->vreg_of[i];
}

/* Instantiates rule `rule` at v (or the BRANCH op T1_VALUE_OP(v) with the
 * edge labels) into MIR. */
static bool emit_cover(struct isel *S, t1_value v, bool branch, unsigned rule, uint16_t label_t,
                       uint16_t label_f)
{
    const struct t1_rule *r = &t1_rules[rule];
    struct t1_mir *m = S->m;
    struct bind B;
    bind_init(&B);
    if (branch != (r->pat[0].ir_op == T1_IR_BRANCH) || !match(S, r, 0, v, true, &B))
        return t1_mir_fail(m, "isel: rule no longer matches");
    t1_vreg in[MAX_BINDERS], out[MAX_BINDERS], temp[MAX_BINDERS];
    unsigned nin = r->nin;
    for (unsigned k = 0; k < nin; ++k) {
        in[k] = value_vreg(S, B.in[k]);
        if (in[k] == T1_VREG_NONE)
            return false;
    }
    /* Outputs: the IR results of the pattern nodes named by out_result. */
    for (unsigned j = 0; j < r->nout; ++j) {
        const struct t1_pat *o = &r->out_result[j];
        /* Walk the pattern to find the value of node o->bind. */
        t1_value nodev = v;
        if (o->bind != 0) {
            /* Find the IR value bound to pattern node o->bind by matching
             * structure again: parents in preorder. */
            t1_value vals[32];
            for (unsigned q = 0; q < 32; ++q)
                vals[q] = T1_VALUE_NONE;
            vals[0] = v;
            for (unsigned q = 0; q < r->npat && q < 32; ++q) {
                const struct t1_pat *p = &r->pat[q];
                if (p->ir_op == T1_PAT_VALUE || vals[q] == T1_VALUE_NONE)
                    continue;
                for (unsigned k = 0; k < p->nkids; ++k)
                    if (p->kid[k] < 32)
                        vals[p->kid[k]] = operand(S, T1_VALUE_OP(vals[q]), k);
            }
            if (o->bind >= 32 || vals[o->bind] == T1_VALUE_NONE)
                return t1_mir_fail(m, "isel: output node");
            nodev = vals[o->bind];
        }
        t1_value ov = T1_VALUE(T1_VALUE_OP(nodev), o->result);
        out[j] = new_vreg(S, ov);
        if (out[j] == T1_VREG_NONE)
            return false;
        if (!is_const(S, ov)) {
            if (S->vreg_of[vidx(ov)] != T1_VREG_NONE && !is_const(S, ov))
                return t1_mir_fail(m, "isel: value defined twice");
            if (ov != v)
                S->vflags[vidx(ov)] |= VF_PRODUCED;
        }
        S->vreg_of[vidx(ov)] = out[j];
    }
    for (unsigned k = 0; k < r->ntemp; ++k) {
        temp[k] = new_vreg(S, T1_VALUE_NONE);
        if (temp[k] == T1_VREG_NONE)
            return false;
    }
    /* Fixed registers: inputs through a precolored copy, temps
     * precolored. */
    for (unsigned k = 0; k < r->nfixed; ++k) {
        unsigned kind = r->fixed[k].binder >> 5, idx = r->fixed[k].binder & 31;
        uint8_t reg = r->fixed[k].x86reg;
        if (kind == T1_OP_IN && idx < nin) {
            in[idx] = copy_of(S, in[idx], reg);
            if (in[idx] == T1_VREG_NONE)
                return false;
        } else if (kind == T1_OP_TEMP && idx < r->ntemp) {
            m->vreg[temp[idx]].fixed = reg;
        } else {
            return t1_mir_fail(m, "isel: unsupported fixed binder");
        }
    }
    /* Ties (rules.h): tied binders are one register in the proved
     * template. The output starts as a copy of its input, and every
     * template reference to the input names the output; the input's own
     * vreg keeps the value for its other uses (the allocator coalesces the
     * copy when the input dies there). */
    for (unsigned k = 0; k < r->ntie; ++k) {
        unsigned o = r->tie[k].out, i = r->tie[k].in;
        if (o >= r->nout || i >= nin)
            return t1_mir_fail(m, "isel: bad tie");
        struct t1_mir_insn *mv = add(S, T1_X86_MOV64_RR);
        if (!mv)
            return false;
        opnd_vreg(mv, 0, out[o]);
        opnd_vreg(mv, 1, in[i]);
        m->vreg[out[o]].hint = in[i];
        in[i] = out[o];
    }
    for (unsigned t = 0; t < r->ntmpl; ++t) {
        const struct t1_tmpl *tl = &r->tmpl[t];
        struct t1_mir_insn *mi = add(S, tl->x86op);
        if (!mi)
            return false;
        for (unsigned k = 0; k < tl->nopnd; ++k) {
            const struct t1_opnd *op = &tl->opnd[k];
            switch (op->kind) {
            case T1_OP_IN:
                opnd_vreg(mi, k, in[op->index]);
                break;
            case T1_OP_OUT:
                opnd_vreg(mi, k, out[op->index]);
                break;
            case T1_OP_TEMP:
                opnd_vreg(mi, k, temp[op->index]);
                break;
            case T1_OP_IMM:
                if (op->index == T1_BINDER_NONE)
                    opnd_imm(mi, k, op->imm);
                else if (T1_IMM_IS_EXPR(op->index) && T1_IMM_EXPR_INDEX(op->index) < r->nexpr)
                    opnd_imm(mi, k, t1_iexpr_eval(r->expr, T1_IMM_EXPR_INDEX(op->index), B.imm));
                else if (op->index < MAX_BINDERS && B.imm_set[op->index])
                    opnd_imm(mi, k, B.imm[op->index]);
                else
                    return t1_mir_fail(m, "isel: unbound immediate");
                break;
            case T1_OP_LABEL:
                opnd_label(mi, k, op->index == 0 ? label_t : label_f);
                break;
            case T1_OP_MEM: {
                mi->opnd[k].kind = T1_MO_MEM;
                if (k + 1u > mi->nopnd)
                    mi->nopnd = (uint8_t)(k + 1u);
                uint8_t ids[2] = {tl->base, tl->index};
                for (unsigned q = 0; q < 2; ++q) {
                    uint8_t id = ids[q], kind = id >> 5, idx = id & 31;
                    t1_vreg vr = T1_VREG_NONE;
                    if (id != T1_BINDER_NONE) {
                        if (kind == T1_OP_IN && idx < nin)
                            vr = in[idx];
                        else if (kind == T1_OP_TEMP && idx < r->ntemp)
                            vr = temp[idx];
                        else if (kind == T1_OP_OUT && idx < r->nout)
                            vr = out[idx];
                        else
                            return t1_mir_fail(m, "isel: bad memory binder");
                    }
                    if (q == 0) {
                        mi->mem.base_kind = vr == T1_VREG_NONE ? T1_MO_NONE : T1_MO_VREG;
                        mi->mem.base = vr;
                    } else {
                        mi->mem.index_kind = vr == T1_VREG_NONE ? T1_MO_NONE : T1_MO_VREG;
                        mi->mem.index = vr;
                    }
                }
                uint8_t sc = tl->scale_shift;
                if (sc & 0x80) {
                    unsigned b = sc & 0x7f;
                    if (b >= MAX_BINDERS || !B.imm_set[b] || B.imm[b] < 0 || B.imm[b] > 3)
                        return t1_mir_fail(m, "isel: scale binder");
                    sc = (uint8_t)B.imm[b];
                }
                mi->mem.scale_shift = sc;
                int64_t disp = tl->disp;
                if (tl->disp_expr != T1_TMPL_DISP_NONE) {
                    if (!T1_IMM_IS_EXPR(tl->disp_expr) ||
                        T1_IMM_EXPR_INDEX(tl->disp_expr) >= r->nexpr)
                        return t1_mir_fail(m, "isel: displacement expression");
                    disp += t1_iexpr_eval(r->expr, T1_IMM_EXPR_INDEX(tl->disp_expr), B.imm);
                }
                if (disp < INT32_MIN || disp > INT32_MAX)
                    return t1_mir_fail(m, "isel: disp32");
                mi->mem.disp = (int32_t)disp;
                break;
            }
            default:
                return t1_mir_fail(m, "isel: operand kind");
            }
        }
    }
    return true;
}

/* ---- blocks ------------------------------------------------------------ */

/* A MIR block in the innermost loop `header` (T1_NO_BLOCK: none). */
static void begin_block(struct isel *S, uint16_t mb, uint16_t ir_block, uint16_t header)
{
    struct t1_mir_block *blk = &S->m->block[mb];
    blk->first = S->m->ninsns;
    blk->count = 0;
    blk->term = T1_MIR_NONE;
    blk->ir_block = ir_block;
    blk->loop = header != T1_NO_BLOCK;
    blk->back_edge = false;
    blk->depth = header != T1_NO_BLOCK ? S->loops.depth[header] : 0;
    blk->loop_header = header;
    blk->nsucc = 0;
}

static void end_block(struct isel *S, uint16_t mb)
{
    struct t1_mir_block *blk = &S->m->block[mb];
    blk->count = S->m->ninsns - blk->first;
    if (blk->term == T1_MIR_NONE)
        blk->term = blk->first + blk->count;
}

static void mark_term(struct isel *S, uint16_t mb)
{
    S->m->block[mb].term = S->m->ninsns;
}

/* The parallel copy of an edge's arguments into the target's parameters. */
static bool edge_copy(struct isel *S, uint16_t ir_block, unsigned succ)
{
    const struct t1_ir *ir = S->ir;
    const struct t1_edge *e = &ir->block[ir_block].succ[succ];
    if (!e->nargs)
        return true;
    /* The memory token (E_MEM) needs no register: it orders, it holds no value. */
    unsigned nval = 0, k = 0;
    for (uint32_t q = ir->block[e->target].first; q != T1_NO_OP && ir->op[q].code == T1_IR_PARAM;
         q = ir->op[q].next)
        nval += ir->op[q].type != T1_MEM;
    if (!nval)
        return true;
    uint32_t first = t1_mir_new_copies(S->m, nval), n = 0;
    if (first == T1_MIR_NONE)
        return false;
    for (uint32_t q = ir->block[e->target].first; q != T1_NO_OP && ir->op[q].code == T1_IR_PARAM;
         q = ir->op[q].next, ++k) {
        if (k >= e->nargs)
            return t1_mir_fail(S->m, "isel: edge arity");
        if (ir->op[q].type == T1_MEM)
            continue;
        struct t1_mir_copy *c = &S->m->copy[first + n++];
        c->dst = S->vreg_of[vidx(T1_VALUE(q, 0))];
        t1_value a = resolve(S, ir->use[e->args_first + k].value);
        if (is_const(S, a)) {
            c->src = T1_VREG_NONE;
            c->src_imm = (uint64_t)def_op(S, a)->imm;
        } else {
            c->src = S->vreg_of[vidx(a)];
            if (c->src == T1_VREG_NONE) {
                LOGE("isel: edge B%u.%u argument %u: op %u (%s) result %u in block %u", ir_block,
                     succ, k, T1_VALUE_OP(a), t1_op_desc(def_op(S, a)->code)->name,
                     T1_VALUE_RESULT(a), def_op(S, a)->block);
                return t1_mir_fail(S->m, "isel: edge argument without vreg");
            }
            if (S->m->vreg[c->dst].hint == T1_VREG_NONE)
                S->m->vreg[c->dst].hint = c->src;
            if (S->m->vreg[c->src].hint == T1_VREG_NONE)
                S->m->vreg[c->src].hint = c->dst;
        }
        if (c->dst == T1_VREG_NONE)
            return t1_mir_fail(S->m, "isel: parameter without vreg");
    }
    if (k != e->nargs)
        return t1_mir_fail(S->m, "isel: edge arity");
    struct t1_mir_insn *i = add(S, T1_MIR_PCOPY);
    if (!i)
        return false;
    i->copies_first = first;
    i->ncopies = nval;
    return true;
}

static bool jump_to(struct isel *S, uint16_t mb, uint16_t target)
{
    mark_term(S, mb);
    struct t1_mir_insn *j = add(S, T1_X86_JMP);
    if (!j)
        return false;
    opnd_label(j, 0, target);
    S->m->block[mb].nsucc = 1;
    S->m->block[mb].succ[0] = target;
    return true;
}

static void frame_of(struct isel *S, uint32_t point)
{
    const struct t1_ir *ir = S->ir;
    const struct t1_ir_point *pt = &ir->point[point];
    struct t1_mir *m = S->m;
    struct t1_mir_point *mp = &m->point[point];
    if (m->nframe + pt->frame_count > m->frame_cap) {
        t1_mir_fail(m, "frame pool");
        return;
    }
    mp->frame_first = m->nframe;
    mp->frame_count = pt->frame_count;
    for (unsigned k = 0; k < pt->frame_count; ++k) {
        const struct t1_use *u = &ir->use[pt->frame_first + k];
        struct t1_mir_frame *f = &m->frame[m->nframe++];
        f->element = u->element;
        f->vreg = T1_VREG_NONE;
        f->value = 0;
        if (u->flags & T1_USE_HOME) {
            f->kind = T1_MF_HOME;
            continue;
        }
        t1_value v = resolve(S, u->value);
        if (is_const(S, v)) {
            f->kind = T1_MF_CONST;
            f->value = (uint64_t)def_op(S, v)->imm;
            continue;
        }
        f->kind = T1_MF_VREG;
        f->vreg = S->vreg_of[vidx(v)];
        if (f->vreg == T1_VREG_NONE)
            t1_mir_fail(m, "isel: frame value without vreg");
    }
}

static void attach_point(struct isel *S, struct t1_mir_insn *i, uint32_t point, bool force)
{
    i->point = point;
    if (force) {
        S->m->point[point].force_insn = (uint32_t)(i - S->m->insn);
        return;
    }
    S->m->point[point].insn = (uint32_t)(i - S->m->insn);
    frame_of(S, point);
}

static bool select_access(struct isel *S, uint32_t o)
{
    const struct t1_ir *ir = S->ir;
    const struct t1_op *op = &ir->op[o];
    bool store = op->code == T1_IR_STORE;
    unsigned bytes = T1_ACCESS_BYTES(op->imm), site = T1_ACCESS_SITE(op->imm);
    t1_value av = operand(S, o, 0);
    t1_vreg addr = value_vreg(S, av), data = T1_VREG_NONE;
    if (addr == T1_VREG_NONE)
        return false;
    if (store) {
        data = value_vreg(S, operand(S, o, 1));
        if (data == T1_VREG_NONE)
            return false;
    }
    if (S->flags & T1_COMPILE_VERIFY) {
        struct t1_mir_insn *f = add(S, T1_MIR_FORCE);
        if (!f)
            return false;
        opnd_vreg(f, 0, addr);
        if (store)
            opnd_vreg(f, 1, data);
        else
            opnd_none(f, 1);
        attach_point(S, f, op->point, true);
    }
    if (!store) {
        data = new_vreg(S, T1_VALUE(o, T1_RES_LOAD_value));
        if (data == T1_VREG_NONE)
            return false;
        S->vreg_of[vidx(T1_VALUE(o, T1_RES_LOAD_value))] = data;
    }
    unsigned flags = store ? T1_SITE_STORE : T1_SITE_LOAD;
    /* The alignment test goes only where known bits prove it. */
    const struct t1_known *k = &S->kb[T1_VALUE_OP(av)];
    if (bytes == 1 || (T1_VALUE_RESULT(av) == 0 && (k->zero & (bytes - 1)) == bytes - 1))
        flags |= T1_SITE_ALIGN_PROVEN;
    if (store && (S->flags & T1_COMPILE_STORE_LOG))
        flags |= T1_SITE_STORE_LOG;
    struct t1_mir_insn *s = add(S, T1_MIR_SITE);
    if (!s)
        return false;
    opnd_vreg(s, 0, data);
    opnd_vreg(s, 1, addr);
    opnd_imm(s, 2, (int64_t)(flags | bytes << 8 | site << 16));
    attach_point(S, s, op->point, false);
    if ((store || (S->flags & T1_COMPILE_ACCESS_LOG)) && (S->flags & T1_COMPILE_STORE_LOG)) {
        struct t1_mir_insn *l = add(S, T1_MIR_SLOG);
        if (!l)
            return false;
        opnd_vreg(l, 0, addr);
        opnd_vreg(l, 1, data);
        opnd_imm(l, 2, op->point);
    }
    return true;
}

static bool select_helper(struct isel *S, uint32_t o)
{
    const struct t1_op *op = &S->ir->op[o];
    if (S->flags & T1_COMPILE_VERIFY) {
        struct t1_mir_insn *f = add(S, T1_MIR_FORCE);
        if (!f)
            return false;
        attach_point(S, f, op->point, true);
    }
    struct t1_mir_insn *h = add(S, T1_MIR_HELPER);
    if (!h)
        return false;
    /* An fma-family helper gets the inline host FP path (emit.c); not in
     * checked compiles, whose helper stub collects the capture first. */
    if (op->code == T1_IR_GHELPER && !(S->flags & T1_COMPILE_UNDO_LOG) &&
        t1_fp_fast_opcode(S->ir->form->insn[op->imm].insn.opcode))
        opnd_imm(h, 3, 1);
    for (unsigned res = 0; op->code == T1_IR_HELPER && res < 2; ++res) {
        t1_value v = T1_VALUE(o, res);
        if (!op->wr[res] || !S->uses[vidx(v)]) {
            opnd_none(h, res);
            continue;
        }
        t1_vreg d = new_vreg(S, v);
        if (d == T1_VREG_NONE)
            return false;
        S->vreg_of[vidx(v)] = d;
        h = &S->m->insn[S->m->ninsns - 1];
        opnd_vreg(h, res, d);
    }
    attach_point(S, h, op->point, false);
    return true;
}

static bool select_terminator(struct isel *S, uint16_t b, uint16_t mb, uint32_t t)
{
    const struct t1_ir *ir = S->ir;
    struct t1_mir *m = S->m;
    const struct t1_op *op = &ir->op[t];
    if (ir->block[b].charge) {
        struct t1_mir_insn *c = add(S, T1_MIR_CHARGE);
        if (!c)
            return false;
        opnd_imm(c, 0, ir->block[b].charge);
    }
    switch (op->code) {
    case T1_IR_JUMP: {
        if (!edge_copy(S, b, 0))
            return false;
        return jump_to(S, mb, S->mblock[ir->block[b].succ[0].target]);
    }
    case T1_IR_BACKEDGE: {
        /* The budget a path from this loop's header needs. */
        uint16_t header = ir->block[b].succ[0].target;
        struct t1_mir_insn *c = add(S, T1_MIR_CHECKPOINT);
        if (!c)
            return false;
        opnd_imm(c, 0, t1_ir_max_path(ir, header));
        attach_point(S, c, op->point, false);
        if (!edge_copy(S, b, 0))
            return false;
        m->block[mb].back_edge = true;
        return jump_to(S, mb, S->mblock[header]);
    }
    case T1_IR_EXIT: {
        mark_term(S, mb);
        struct t1_mir_insn *x = add(S, T1_MIR_EXIT);
        if (!x)
            return false;
        attach_point(S, x, op->point, false);
        m->block[mb].nsucc = 0;
        return true;
    }
    case T1_IR_BRANCH: {
        unsigned rule = 0;
        t1_value top = T1_VALUE(t, 0);
        uint32_t cost = best_rule(S, top, true, &rule);
        if (cost == INF_COST) {
            m->fail_insn = op->insn;
            return t1_mir_fail(m, "no rule covers a branch");
        }
        /* emit_cover emits the subtrees, constants and copies first and
         * the template last: the terminator sequence is the rule's ntmpl
         * last instructions. */
        uint32_t before = m->ninsns;
        if (!emit_cover(S, top, true, rule, S->edge_block[b][0], S->edge_block[b][1]))
            return false;
        m->block[mb].term = m->ninsns - t1_rules[rule].ntmpl;
        if (m->block[mb].term < before)
            return t1_mir_fail(m, "isel: terminator sequence");
        m->block[mb].nsucc = 2;
        m->block[mb].succ[0] = S->edge_block[b][0];
        m->block[mb].succ[1] = S->edge_block[b][1];
        return true;
    }
    default:
        return t1_mir_fail(m, "isel: unknown terminator");
    }
}

static bool select_block(struct isel *S, uint16_t b)
{
    const struct t1_ir *ir = S->ir;
    struct t1_mir *m = S->m;
    uint16_t mb = S->mblock[b];
    S->cur_block = b;
    begin_block(S, mb, b, S->in_loop[b] ? S->loops.header[b] : T1_NO_BLOCK);
    uint32_t term = t1_ir_terminator(ir, b);
    if (term == T1_NO_OP)
        return t1_mir_fail(m, "isel: block without terminator");
    /* The terminator's BRANCH cover may absorb values defined just before
     * it (CLOOP): decide it first so its extra outputs are known. */
    if (ir->op[term].code == T1_IR_BRANCH) {
        unsigned rule = 0;
        if (best_rule(S, T1_VALUE(term, 0), true, &rule) != INF_COST) {
            struct bind B;
            bind_init(&B);
            const struct t1_rule *r = &t1_rules[rule];
            if (match(S, r, 0, T1_VALUE(term, 0), true, &B) && r->nout) {
                t1_value vals[32];
                for (unsigned q = 0; q < 32; ++q)
                    vals[q] = T1_VALUE_NONE;
                vals[0] = T1_VALUE(term, 0);
                for (unsigned q = 0; q < r->npat && q < 32; ++q) {
                    const struct t1_pat *p = &r->pat[q];
                    if (p->ir_op == T1_PAT_VALUE || vals[q] == T1_VALUE_NONE)
                        continue;
                    for (unsigned k = 0; k < p->nkids; ++k)
                        if (p->kid[k] < 32)
                            vals[p->kid[k]] = operand(S, T1_VALUE_OP(vals[q]), k);
                }
                for (unsigned j = 0; j < r->nout; ++j)
                    if (r->out_result[j].bind < 32 && vals[r->out_result[j].bind] != T1_VALUE_NONE)
                        S->vflags[vidx(T1_VALUE(T1_VALUE_OP(vals[r->out_result[j].bind]),
                                                r->out_result[j].result))] |= VF_PRODUCED;
            }
        }
    }
    for (uint32_t o = ir->block[b].first; o != T1_NO_OP && !m->failed; o = ir->op[o].next) {
        const struct t1_op *op = &ir->op[o];
        if (o == term)
            break;
        switch (op->code) {
        case T1_IR_PARAM:
        case T1_IR_CONST:
            break;
        case T1_IR_LIVEIN:
        case T1_IR_NATIN:
        case T1_IR_HREAD: {
            t1_value v = T1_VALUE(o, 0);
            if (!S->uses[vidx(v)])
                break;
            t1_vreg d = new_vreg(S, v);
            if (d == T1_VREG_NONE)
                return false;
            S->vreg_of[vidx(v)] = d;
            struct t1_mir_insn *i = add(S, op->code == T1_IR_HREAD ? T1_MIR_HREAD : T1_MIR_LIVEIN);
            if (!i)
                return false;
            opnd_vreg(i, 0, d);
            opnd_imm(i, 1, op->imm);
            break;
        }
        case T1_IR_LOAD:
        case T1_IR_STORE:
            if (!select_access(S, o))
                return false;
            break;
        case T1_IR_FENCE:
            if (!add(S, T1_MIR_FENCE))
                return false;
            break;
        case T1_IR_BSPCALL:
            if (!add(S, T1_MIR_BSPCALL))
                return false;
            break;
        case T1_IR_HGUARD: {
            struct t1_mir_insn *h = add(S, T1_MIR_HGUARD);
            if (!h)
                return false;
            attach_point(S, h, op->point, false);
            break;
        }
        case T1_IR_GHELPER:
        case T1_IR_HELPER:
            if (!select_helper(S, o))
                return false;
            break;
        case T1_IR_NATCHK:
            m->fail_insn = op->insn;
            return t1_mir_fail(m, "isel: NATCHK survived");
        default: {
            if (t1_op_desc(op->code)->effect != T1_EFF_PURE)
                return t1_mir_fail(m, "isel: unexpected op");
            unsigned nres = t1_op_desc(op->code)->nres;
            for (unsigned res = 0; res < nres; ++res) {
                t1_value v = T1_VALUE(o, res);
                unsigned i = vidx(v);
                if (!S->uses[i] || !is_root(S, v) || (S->vflags[i] & VF_PRODUCED))
                    continue;
                if (cover_cost(S, v) == INF_COST) {
                    m->fail_insn = op->insn;
                    return t1_mir_fail(m, "no rule covers an operation");
                }
                if (!emit_cover(S, v, false, S->memo_rule[i], 0, 0))
                    return false;
            }
            break;
        }
        }
    }
    if (m->failed)
        return false;
    if (!select_terminator(S, b, mb, term))
        return false;
    end_block(S, mb);
    /* Edge blocks of a BRANCH: the parallel copy, then the target. */
    if (ir->op[term].code == T1_IR_BRANCH) {
        for (unsigned s = 0; s < 2; ++s) {
            uint16_t eb = S->edge_block[b][s];
            uint16_t target = ir->block[b].succ[s].target;
            begin_block(S, eb, T1_NO_BLOCK, common_loop(S, b, target));
            if (!edge_copy(S, b, s) || !jump_to(S, eb, S->mblock[target]))
                return false;
            end_block(S, eb);
        }
    }
    return true;
}

static bool setup(struct isel *S, struct t1_mir *m, uint32_t flags)
{
    const struct t1_ir *ir = m->ir;
    S->m = m;
    S->ir = ir;
    S->flags = flags;
    S->host_isa = hal_host_isa();
    S->nvalues = ir->nops * 4u;
    size_t per =
        sizeof(uint16_t) + sizeof(uint8_t) + sizeof(t1_vreg) + sizeof(uint32_t) + sizeof(uint16_t);
    size_t bytes = (size_t)S->nvalues * per + (size_t)ir->nops * sizeof(struct t1_known) + 64;
    S->pages = (bytes + 4095) / 4096;
    uint8_t *p = t1_ws_alloc(ir->ws, S->pages ? S->pages : 1, HAL_PAGE_ZERO);
    if (!p)
        return t1_mir_fail(m, "no memory for isel");
    S->kb = (struct t1_known *)p;
    p += (size_t)ir->nops * sizeof(struct t1_known);
    S->memo_cost = (uint32_t *)p;
    p += (size_t)S->nvalues * sizeof(uint32_t);
    S->vreg_of = (t1_vreg *)p;
    p += (size_t)S->nvalues * sizeof(t1_vreg);
    S->uses = (uint16_t *)p;
    p += (size_t)S->nvalues * sizeof(uint16_t);
    S->memo_rule = (uint16_t *)p;
    p += (size_t)S->nvalues * sizeof(uint16_t);
    S->vflags = p;
    for (unsigned i = 0; i < S->nvalues; ++i)
        S->vreg_of[i] = T1_VREG_NONE;
    t1_known_bits(ir, S->kb);
    return true;
}

static bool select_mir(struct isel *S, struct t1_mir *m, uint32_t flags)
{
    const struct t1_ir *ir = m->ir;
    m->flags = flags;
    if (!setup(S, m, flags))
        return false;
    analyse_uses(S);
    linear_order(S);
    /* MIR block ids: each IR block, followed by its two edge blocks when it
     * ends in BRANCH. */
    unsigned nb = 0;
    for (unsigned k = 0; k < S->norder; ++k) {
        uint16_t b = S->order[k];
        S->mblock[b] = (uint16_t)nb++;
        uint32_t t = t1_ir_terminator(ir, b);
        if (t != T1_NO_OP && ir->op[t].code == T1_IR_BRANCH) {
            S->edge_block[b][0] = (uint16_t)nb++;
            S->edge_block[b][1] = (uint16_t)nb++;
        }
    }
    if (nb > T1_MIR_MAX_BLOCKS) {
        t1_ws_free(ir->ws, S->kb, S->pages ? S->pages : 1);
        return t1_mir_fail(m, "MIR block limit");
    }
    m->nblocks = nb;
    m->header = S->mblock[T1_HEADER_BLOCK];
    /* Block parameters get their vregs up front: edges into a block may be
     * selected before it. */
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        if (ir->block[b].flags & T1_BLK_DEAD)
            continue;
        for (uint32_t q = ir->block[b].first; q != T1_NO_OP && ir->op[q].code == T1_IR_PARAM;
             q = ir->op[q].next) {
            if (ir->op[q].type == T1_MEM)
                continue;
            t1_vreg v = new_vreg(S, T1_VALUE(q, 0));
            if (v == T1_VREG_NONE)
                break;
            m->vreg[v].flags |= T1_VR_PARAM;
            m->vreg[v].element = (uint16_t)ir->op[q].imm;
            if (S->loops.body[b])
                m->vreg[v].flags |= T1_VR_LOOP_PARAM;
            if (m->pin_element && ir->op[q].imm == m->pin_element && b == T1_HEADER_BLOCK)
                m->vreg[v].fixed = m->pin_reg;
            S->vreg_of[vidx(T1_VALUE(q, 0))] = v;
        }
    }
    bool ok = !m->failed;
    for (unsigned k = 0; k < S->norder && ok; ++k)
        ok = select_block(S, S->order[k]);
    /* Real uses inside the loop (loop_value_spills classification). */
    if (ok)
        for (unsigned b = 0; b < m->nblocks; ++b) {
            if (!m->block[b].loop)
                continue;
            for (uint32_t i = m->block[b].first; i < m->block[b].first + m->block[b].count; ++i) {
                const struct t1_mir_insn *in = &m->insn[i];
                struct t1_mir_effects fx;
                t1_mir_effects(in, &fx);
                for (unsigned u = 0; u < fx.nuse; ++u) {
                    uint8_t k = fx.use[u];
                    t1_vreg v = k == 0x80   ? in->mem.base
                                : k == 0x81 ? in->mem.index
                                            : in->opnd[k].vreg;
                    if ((k == 0x80 && in->mem.base_kind != T1_MO_VREG) ||
                        (k == 0x81 && in->mem.index_kind != T1_MO_VREG))
                        continue;
                    if (v != T1_VREG_NONE && v < m->nvregs)
                        m->vreg[v].flags |= T1_VR_REAL_USE;
                }
            }
        }
    /* A value copied into a loop value on an edge of the loop is one too
     * (its register flows on): propagate backwards through the copies. */
    for (bool changed = ok; changed;) {
        changed = false;
        for (unsigned b = 0; b < m->nblocks; ++b) {
            if (!m->block[b].loop)
                continue;
            for (uint32_t i = m->block[b].first; i < m->block[b].first + m->block[b].count; ++i) {
                const struct t1_mir_insn *in = &m->insn[i];
                if (in->op != T1_MIR_PCOPY)
                    continue;
                for (uint32_t c = in->copies_first; c < in->copies_first + in->ncopies; ++c) {
                    const struct t1_mir_copy *cp = &m->copy[c];
                    if (cp->src == T1_VREG_NONE || !(m->vreg[cp->dst].flags & T1_VR_REAL_USE) ||
                        (m->vreg[cp->src].flags & T1_VR_REAL_USE))
                        continue;
                    m->vreg[cp->src].flags |= T1_VR_REAL_USE;
                    changed = true;
                }
            }
        }
    }
    t1_ws_free(ir->ws, S->kb, S->pages ? S->pages : 1);
    return ok && !m->failed;
}

bool t1_isel(struct t1_mir *m, uint32_t flags)
{
    if ((flags & T1_COMPILE_UNDO_LOG) && !(flags & T1_COMPILE_STORE_LOG))
        return t1_mir_fail(m, "undo log without the store log");
    if ((flags & T1_COMPILE_ACCESS_LOG) && !(flags & T1_COMPILE_UNDO_LOG))
        return t1_mir_fail(m, "access log without checked replay");
    size_t pages = (sizeof(struct isel) + 4095) / 4096;
    struct isel *S = t1_ws_alloc(m->ir->ws, pages, HAL_PAGE_ZERO);
    if (!S)
        return t1_mir_fail(m, "no memory for isel");
    bool ok = select_mir(S, m, flags);
    t1_ws_free(m->ir->ws, S, pages);
    return ok;
}
