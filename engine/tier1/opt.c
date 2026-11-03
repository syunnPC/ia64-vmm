/* SPDX-License-Identifier: GPL-2.0-or-later */
/* IR folding, predicate simplification, frame rules, DCE and known bits.
 * Constant arithmetic uses proved IR rules, not IA-64 instruction semantics.
 * DCE retains frame pseudo-uses. NaT guards run after predicate folding and
 * DCE so false predicates do not enlarge the guard set. A surviving NATCHK
 * sets boundary_insn so formation ends the region before that instruction.
 */
#include "engine/tier1/ir.h"
#include "engine/tier1/form.h"
#include "engine/tier1/helpers.h"
#include "build/tier1/fold_rules.h"
#include "ia64vmm/hal.h"
#include "engine/tier1/memory.h"
#include "engine/tier1/worklist.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

static bool is_const(const struct t1_ir *ir, t1_value v, uint64_t k)
{
    const struct t1_op *o = &ir->op[T1_VALUE_OP(v)];
    return o->code == T1_IR_CONST && (uint64_t)o->imm == k;
}
static bool any_const(const struct t1_ir *ir, t1_value v)
{
    return ir->op[T1_VALUE_OP(v)].code == T1_IR_CONST;
}
static t1_value opnd(const struct t1_ir *ir, uint32_t o, unsigned i)
{
    t1_value v = ir->use[ir->op[o].use_first + i].value;
    return ir->op[T1_VALUE_OP(v)].fwd != T1_VALUE_NONE && T1_VALUE_RESULT(v) == 0
               ? t1_ir_resolve(ir, v)
               : v;
}

/* Apply proved constant/identity folds before selecting code.
 * Arithmetic lives in the proof table's expression evaluator. Non-leaf
 * replacements still wait for a general IR tree builder. */
struct fold_bind {
    t1_value value[8];
    int64_t imm[8];
    bool seen[8];
};
struct fold_fact {
    uint64_t value;
    bool known, candidate;
};

static bool fold_match(const struct t1_ir *ir, const struct t1_fold_rule *r, unsigned pi,
                       t1_value v, struct fold_bind *b, const struct fold_fact *fact)
{
    const struct t1_op *op = &ir->op[T1_VALUE_OP(v)];
    if (op->fwd != T1_VALUE_NONE && T1_VALUE_RESULT(v) == 0) {
        v = t1_ir_resolve(ir, v);
        op = &ir->op[T1_VALUE_OP(v)];
    }
    bool known = fact && !T1_VALUE_RESULT(v) && fact[T1_VALUE_OP(v)].known;
    unsigned code = known ? T1_IR_CONST : op->code;
    int64_t imm = known ? (int64_t)fact[T1_VALUE_OP(v)].value : op->imm;
    unsigned nuse = known ? 0 : op->nuse;
    const struct t1_pat *p = &r->pat[pi];
    if (p->ir_op == T1_PAT_VALUE) {
        if (p->bind >= 8)
            return false;
        if (b->value[p->bind] != T1_VALUE_NONE)
            return b->value[p->bind] == v;
        b->value[p->bind] = v;
        return true;
    }
    if (code != p->ir_op || T1_VALUE_RESULT(v) != p->result || p->nkids > nuse ||
        imm < p->imm_min || imm > p->imm_max)
        return false;
    if (p->imm_set && (imm < 0 || imm > 31 || !((p->imm_set >> imm) & 1)))
        return false;
    if (p->bind != T1_BINDER_NONE) {
        if (p->bind >= 8 || (b->seen[p->bind] && b->imm[p->bind] != imm))
            return false;
        b->imm[p->bind] = imm;
        b->seen[p->bind] = true;
    }
    for (unsigned k = 0; k < p->nkids; ++k)
        if (!fold_match(ir, r, p->kid[k], ir->use[op->use_first + k].value, b, fact))
            return false;
    return true;
}
static t1_value proved_leaf_fold(struct t1_ir *ir, uint32_t o)
{
    /* Forwarding replaces result zero and removes the whole defining op.
     * Multi-result operations (CLOOP) must keep their other live results. */
    if (t1_op_desc(ir->op[o].code)->nres != 1)
        return T1_VALUE_NONE;
    unsigned count;
    const uint16_t *candidates = t1_fold_leaf_candidates(ir->op[o].code, &count);
    for (unsigned n = 0; n < count; ++n) {
        const struct t1_fold_rule *r = &t1_fold_rules[candidates[n]];
        struct fold_bind b = {0};
        for (unsigned k = 0; k < 8; ++k)
            b.value[k] = T1_VALUE_NONE;
        if (!fold_match(ir, r, 0, T1_VALUE(o, 0), &b, NULL))
            continue;
        bool ok = true;
        for (unsigned k = 0; k < r->ncheck; ++k) {
            int64_t v = t1_iexpr_eval(r->expr, r->check[k].expr, b.imm);
            if (v < r->check[k].min || v > r->check[k].max)
                ok = false;
        }
        if (!ok)
            continue;
        const struct t1_fold_node *res = &r->res[0];
        if (res->ir_op == T1_PAT_VALUE)
            return b.value[res->bind];
        return t1_ir_const(ir, t1_ir_value_type(ir, T1_VALUE(o, 0)),
                           (uint64_t)t1_iexpr_eval(r->expr, T1_IMM_EXPR_INDEX(res->imm), b.imm));
    }
    return T1_VALUE_NONE;
}

/* The value op o simplifies to, or T1_VALUE_NONE. */
static t1_value simplify(struct t1_ir *ir, uint32_t o)
{
    const struct t1_op *op = &ir->op[o];
    t1_value a = op->nuse > 0 ? opnd(ir, o, 0) : T1_VALUE_NONE;
    t1_value b = op->nuse > 1 ? opnd(ir, o, 1) : T1_VALUE_NONE;
    t1_value folded = proved_leaf_fold(ir, o);
    if (folded != T1_VALUE_NONE)
        return folded;
    switch (op->code) {
    case T1_IR_ADD:
    case T1_IR_OR:
    case T1_IR_XOR:
        if (is_const(ir, b, 0))
            return a;
        if (is_const(ir, a, 0))
            return b;
        break;
    case T1_IR_SUB:
    case T1_IR_ANDCM:
    case T1_IR_SHL:
    case T1_IR_SHR:
    case T1_IR_SHRU:
        if (is_const(ir, b, 0))
            return a;
        break;
    case T1_IR_AND:
        if (is_const(ir, b, UINT64_MAX))
            return a;
        if (is_const(ir, a, UINT64_MAX))
            return b;
        break;
    case T1_IR_SHL_IMM:
        if (op->imm == 0)
            return a;
        break;
    case T1_IR_EXTRU:
        if (op->imm == T1_FIELD_IMM(0, 64))
            return a;
        break;
    case T1_IR_NOT1:
        if (any_const(ir, a))
            return t1_ir_const(ir, T1_I1, !ir->op[T1_VALUE_OP(a)].imm);
        if (ir->op[T1_VALUE_OP(a)].code == T1_IR_NOT1)
            return opnd(ir, T1_VALUE_OP(a), 0);
        break;
    case T1_IR_AND1:
        if (is_const(ir, a, 0) || is_const(ir, b, 1) || a == b)
            return a;
        if (is_const(ir, b, 0) || is_const(ir, a, 1))
            return b;
        break;
    case T1_IR_OR1:
        if (is_const(ir, a, 1) || is_const(ir, b, 0) || a == b)
            return a;
        if (is_const(ir, b, 1) || is_const(ir, a, 0))
            return b;
        break;
    case T1_IR_SELECT: {
        t1_value f = opnd(ir, o, 2);
        if (is_const(ir, a, 1) || b == f)
            return b;
        if (is_const(ir, a, 0))
            return f;
        if (op->type == T1_I1 && is_const(ir, b, 1) && is_const(ir, f, 0))
            return a;
        break;
    }
    default:
        break;
    }
    return T1_VALUE_NONE;
}

/*
 * Redundant block parameters (Braun et al., generalized): an optimistic
 * value-identity propagation over all parameters. Each parameter starts at
 * TOP; a parameter whose incoming arguments, seen through parameters with
 * a known value, are all one value V (or itself) becomes V; anything else
 * stands for itself (BOTTOM). This removes trivial parameters and whole
 * cycles of parameters that only pass one outer value around (e.g. a NaT
 * that is CONST 0 on entry and on every back-edge path).
 */
#define CAND_TOP ((t1_value)0xfffffffeu)
#define CAND_BOTTOM ((t1_value)0xfffffffdu)

static t1_value arg_value(const struct t1_ir *ir, const t1_value *cand, t1_value a)
{
    a = t1_ir_resolve(ir, a);
    uint32_t q = T1_VALUE_OP(a);
    if (ir->op[q].code != T1_IR_PARAM)
        return a;
    if (cand[q] == CAND_TOP)
        return CAND_TOP;
    return cand[q] == CAND_BOTTOM ? a : cand[q];
}

static bool fold_params(struct t1_ir *ir)
{
    size_t pages = (ir->nops * sizeof(t1_value) + 4095) / 4096;
    t1_value *cand = t1_ws_alloc(ir->ws, pages ? pages : 1, 0);
    if (!cand) {
        t1_ir_fail(ir, "no memory for folding");
        return false;
    }
    for (unsigned o = 0; o < ir->nops; ++o)
        cand[o] = CAND_TOP;
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = 0; b < ir->nblocks; ++b) {
            const struct t1_block *blk = &ir->block[b];
            if (blk->flags & T1_BLK_DEAD)
                continue;
            unsigned pos = 0;
            for (uint32_t o = blk->first; o != T1_NO_OP && ir->op[o].code == T1_IR_PARAM;
                 o = ir->op[o].next, ++pos) {
                if (ir->op[o].fwd != T1_VALUE_NONE || cand[o] == CAND_BOTTOM)
                    continue;
                t1_value self = T1_VALUE(o, 0), m = CAND_TOP;
                for (unsigned p = 0; p < blk->npred && m != CAND_BOTTOM; ++p) {
                    const struct t1_edge *e = &ir->block[blk->pred[p]].succ[blk->pred_slot[p]];
                    t1_value a = arg_value(ir, cand, ir->use[e->args_first + pos].value);
                    if (a == CAND_TOP || a == self)
                        continue;
                    m = m == CAND_TOP || m == a ? a : CAND_BOTTOM;
                }
                if (m != cand[o]) {
                    cand[o] = m;
                    changed = true;
                }
            }
        }
    }
    bool any = false;
    for (unsigned o = 0; o < ir->nops; ++o)
        if (ir->op[o].code == T1_IR_PARAM && cand[o] != CAND_TOP && cand[o] != CAND_BOTTOM &&
            ir->op[o].fwd == T1_VALUE_NONE && !(ir->op[o].flags & T1_OPF_DEAD)) {
            t1_ir_forward(ir, o, cand[o]);
            any = true;
        }
    t1_ws_free(ir->ws, cand, pages ? pages : 1);
    return any;
}

/*
 * I1 constants through loops: an optimistic propagation over the lattice
 * TOP > {0, 1} > BOTTOM for every I1 result of CONST, PARAM, NOT1, AND1,
 * OR1 and SELECT (all other I1 results are BOTTOM). A loop-carried NaT
 * built as OR1(phi, 0) is 0 on every iteration, which the local rules and
 * the parameter rule cannot see; this is the step that lets the rule
 * "every NaT value folds to 0" hold for loops.
 */
enum { LAT_TOP = 2, LAT_BOTTOM = 3 };

static uint8_t lat_of(const struct t1_ir *ir, const uint8_t *lat, t1_value v)
{
    v = t1_ir_resolve(ir, v);
    return T1_VALUE_RESULT(v) ? LAT_BOTTOM : lat[T1_VALUE_OP(v)];
}

static uint8_t lat_eval(const struct t1_ir *ir, const uint8_t *lat, uint32_t o, unsigned param_pos)
{
    const struct t1_op *op = &ir->op[o];
    if (t1_ir_value_type(ir, T1_VALUE(o, 0)) != T1_I1)
        return LAT_BOTTOM;
    if (op->code == T1_IR_CONST)
        return (uint8_t)(op->imm & 1);
    if (op->code == T1_IR_PARAM) {
        const struct t1_block *blk = &ir->block[op->block];
        uint8_t m = LAT_TOP;
        for (unsigned p = 0; p < blk->npred; ++p) {
            const struct t1_edge *e = &ir->block[blk->pred[p]].succ[blk->pred_slot[p]];
            uint8_t a = lat_of(ir, lat, ir->use[e->args_first + param_pos].value);
            if (a == LAT_TOP)
                continue;
            m = m == LAT_TOP || m == a ? a : LAT_BOTTOM;
        }
        return m;
    }
    uint8_t a = op->nuse > 0 ? lat_of(ir, lat, ir->use[op->use_first].value) : LAT_BOTTOM;
    uint8_t b = op->nuse > 1 ? lat_of(ir, lat, ir->use[op->use_first + 1].value) : LAT_BOTTOM;
    switch (op->code) {
    case T1_IR_NOT1:
        return a <= 1 ? (uint8_t)!a : a;
    case T1_IR_AND1:
        if (a == 0 || b == 0)
            return 0;
        if (a == LAT_TOP || b == LAT_TOP)
            return LAT_TOP;
        return a == 1 && b == 1 ? 1 : LAT_BOTTOM;
    case T1_IR_OR1:
        if (a == 1 || b == 1)
            return 1;
        if (a == LAT_TOP || b == LAT_TOP)
            return LAT_TOP;
        return a == 0 && b == 0 ? 0 : LAT_BOTTOM;
    case T1_IR_SELECT: {
        uint8_t f = lat_of(ir, lat, ir->use[op->use_first + 2].value);
        if (a == LAT_TOP)
            return LAT_TOP;
        if (a <= 1)
            return a ? b : f;
        if (b == LAT_TOP)
            return f;
        if (f == LAT_TOP)
            return b;
        return b == f ? b : LAT_BOTTOM;
    }
    default:
        return LAT_BOTTOM;
    }
}

/* Only these results can change as their inputs reach the lattice. CONST
 * and all unsupported results have their final value at initialization. */
static bool i1_candidate(const struct t1_op *op)
{
    switch (op->code) {
    case T1_IR_NOT1:
    case T1_IR_AND1:
    case T1_IR_OR1:
        return true;
    case T1_IR_PARAM:
    case T1_IR_SELECT:
        return op->type == T1_I1;
    default:
        return false;
    }
}

static bool fold_i1_constants(struct t1_ir *ir)
{
    unsigned count = 0;
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        if (ir->block[b].flags & T1_BLK_DEAD)
            continue;
        for (uint32_t o = ir->block[b].first; o != T1_NO_OP; o = ir->op[o].next)
            if (ir->op[o].fwd == T1_VALUE_NONE && i1_candidate(&ir->op[o]))
                ++count;
    }
    if (!count)
        return false;
    struct item {
        uint16_t op, param_pos;
    };
    size_t bytes = count * sizeof(struct item) + ir->nops;
    size_t pages = (bytes + 4095) / 4096;
    struct item *work = t1_ws_alloc(ir->ws, pages, 0);
    if (!work) {
        t1_ir_fail(ir, "no memory for folding");
        return false;
    }
    uint8_t *lat = (uint8_t *)(work + count);
    memset(lat, LAT_BOTTOM, ir->nops);
    unsigned n = 0;
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        if (ir->block[b].flags & T1_BLK_DEAD)
            continue;
        unsigned pos = 0;
        for (uint32_t o = ir->block[b].first; o != T1_NO_OP; o = ir->op[o].next) {
            const struct t1_op *op = &ir->op[o];
            unsigned here = op->code == T1_IR_PARAM ? pos++ : 0;
            if (op->fwd != T1_VALUE_NONE)
                continue;
            if (op->code == T1_IR_CONST && op->type == T1_I1)
                lat[o] = (uint8_t)(op->imm & 1);
            else if (i1_candidate(op)) {
                work[n++] = (struct item){(uint16_t)o, (uint16_t)here};
                lat[o] = LAT_TOP;
            }
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned k = 0; k < n; ++k) {
            uint32_t o = work[k].op;
            uint8_t l = lat_eval(ir, lat, o, work[k].param_pos);
            if (l != lat[o]) {
                lat[o] = l;
                changed = true;
            }
        }
    }
    bool any = false;
    for (unsigned k = 0; k < n; ++k) {
        uint32_t o = work[k].op;
        if (lat[o] <= 1) {
            t1_ir_forward(ir, o, t1_ir_const(ir, T1_I1, lat[o]));
            any = true;
        }
    }
    t1_ws_free(ir->ws, work, pages);
    return any;
}

/* Constant facts under an explicitly seeded set of loop hypotheses. */
static bool fact_value(const struct t1_ir *ir, const struct fold_fact *fact, t1_value v,
                       uint64_t *value)
{
    v = t1_ir_resolve(ir, v);
    if (T1_VALUE_RESULT(v) || !fact[T1_VALUE_OP(v)].known)
        return false;
    *value = fact[T1_VALUE_OP(v)].value;
    return true;
}

static bool fact_eval(const struct t1_ir *ir, const struct fold_fact *fact, unsigned o,
                      unsigned pos, uint64_t *value)
{
    const struct t1_op *op = &ir->op[o];
    const struct t1_op_desc *desc = t1_op_desc(op->code);
    if (desc->nres != 1 || (desc->effect != T1_EFF_PURE && desc->effect != T1_EFF_PARAM))
        return false;
    if (op->code == T1_IR_PARAM) {
        const struct t1_block *blk = &ir->block[op->block];
        if (!blk->npred)
            return false;
        for (unsigned p = 0; p < blk->npred; ++p) {
            const struct t1_edge *e = &ir->block[blk->pred[p]].succ[blk->pred_slot[p]];
            uint64_t in;
            if (!fact_value(ir, fact, ir->use[e->args_first + pos].value, &in) ||
                (p && in != *value))
                return false;
            *value = in;
        }
        return true;
    }
    uint64_t a = 0, b = 0, c = 0;
    bool ak = op->nuse > 0 && fact_value(ir, fact, opnd(ir, o, 0), &a);
    bool bk = op->nuse > 1 && fact_value(ir, fact, opnd(ir, o, 1), &b);
    bool ck = op->nuse > 2 && fact_value(ir, fact, opnd(ir, o, 2), &c);
    switch (op->code) {
    case T1_IR_SELECT:
        if ((ak && (a ? bk : ck)) || (bk && ck && b == c)) {
            *value = ak ? (a ? b : c) : b;
            return true;
        }
        break;
    case T1_IR_NOT1:
        if (ak) {
            *value = !a;
            return true;
        }
        break;
    case T1_IR_AND1:
        if ((ak && !a) || (bk && !b) || (ak && bk)) {
            *value = a && b;
            return true;
        }
        break;
    case T1_IR_OR1:
        if ((ak && a) || (bk && b) || (ak && bk)) {
            *value = a || b;
            return true;
        }
        break;
    default:
        break;
    }
    /* Arithmetic and NaTVal tests use the same proved leaf rules as
     * ordinary folding; assumptions only substitute constant leaves. */
    unsigned count;
    const uint16_t *rules = t1_fold_leaf_candidates(op->code, &count);
    for (unsigned n = 0; n < count; ++n) {
        const struct t1_fold_rule *r = &t1_fold_rules[rules[n]];
        struct fold_bind bind = {0};
        for (unsigned k = 0; k < 8; ++k)
            bind.value[k] = T1_VALUE_NONE;
        if (!fold_match(ir, r, 0, T1_VALUE(o, 0), &bind, fact))
            continue;
        bool ok = true;
        for (unsigned k = 0; k < r->ncheck; ++k) {
            int64_t v = t1_iexpr_eval(r->expr, r->check[k].expr, bind.imm);
            if (v < r->check[k].min || v > r->check[k].max)
                ok = false;
        }
        if (!ok)
            continue;
        const struct t1_fold_node *res = &r->res[0];
        if (res->ir_op == T1_PAT_VALUE)
            return fact_value(ir, fact, bind.value[res->bind], value);
        *value = (uint64_t)t1_iexpr_eval(r->expr, T1_IMM_EXPR_INDEX(res->imm), bind.imm);
        return true;
    }
    return false;
}

/* A loop hypothesis needs a literal entry seed and another predecessor. */
static bool has_loop_seed(const struct t1_ir *ir)
{
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        const struct t1_block *blk = &ir->block[b];
        if ((blk->flags & T1_BLK_DEAD) || blk->npred < 2)
            continue;
        unsigned pos = 0;
        for (uint32_t o = blk->first; o != T1_NO_OP && ir->op[o].code == T1_IR_PARAM;
             o = ir->op[o].next, ++pos) {
            if (ir->op[o].fwd != T1_VALUE_NONE || ir->op[o].type != T1_I64)
                continue;
            for (unsigned p = 0; p < blk->npred; ++p) {
                const struct t1_edge *e = &ir->block[blk->pred[p]].succ[blk->pred_slot[p]];
                t1_value v = t1_ir_resolve(ir, ir->use[e->args_first + pos].value);
                if (!T1_VALUE_RESULT(v) && any_const(ir, v))
                    return true;
            }
        }
    }
    return false;
}

/* Each hypothesis has a real constant entry seed. All incoming edges
 * must preserve the simultaneous hypotheses; failed hypotheses are removed
 * and every dependent proof is recomputed before any IR is rewritten. */
static bool fold_loop_constants(struct t1_ir *ir)
{
    if (!has_loop_seed(ir))
        return false;
    uint16_t idom[T1_MAX_BLOCKS];
    if (!t1_ir_dominators(ir, idom))
        return false;
    size_t pages = (ir->nops * sizeof(struct fold_fact) + 4095) / 4096;
    struct fold_fact *fact = t1_ws_alloc(ir->ws, pages ? pages : 1, 0);
    if (!fact) {
        t1_ir_fail(ir, "no memory for loop constants");
        return false;
    }
    memset(fact, 0, ir->nops * sizeof(*fact));
    unsigned candidates = 0;
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        const struct t1_block *blk = &ir->block[b];
        if (blk->flags & T1_BLK_DEAD)
            continue;
        unsigned pos = 0;
        for (uint32_t o = blk->first; o != T1_NO_OP && ir->op[o].code == T1_IR_PARAM;
             o = ir->op[o].next, ++pos) {
            if (ir->op[o].fwd != T1_VALUE_NONE || ir->op[o].type != T1_I64)
                continue;
            bool seed = false, feedback = false, ok = true;
            uint64_t value = 0;
            for (unsigned p = 0; p < blk->npred; ++p) {
                uint16_t pred = blk->pred[p];
                if (t1_ir_dominates(idom, (uint16_t)b, pred)) {
                    feedback = true;
                    continue;
                }
                const struct t1_edge *e = &ir->block[pred].succ[blk->pred_slot[p]];
                t1_value v = t1_ir_resolve(ir, ir->use[e->args_first + pos].value);
                const struct t1_op *in = &ir->op[T1_VALUE_OP(v)];
                if (T1_VALUE_RESULT(v) || in->code != T1_IR_CONST ||
                    (seed && value != (uint64_t)in->imm)) {
                    ok = false;
                    break;
                }
                value = (uint64_t)in->imm;
                seed = true;
            }
            if (ok && seed && feedback) {
                fact[o].value = value;
                fact[o].candidate = true;
                ++candidates;
            }
        }
    }
    while (candidates) {
        for (unsigned o = 0; o < ir->nops; ++o) {
            fact[o].known = fact[o].candidate;
            if (ir->op[o].code == T1_IR_CONST) {
                fact[o].known = true;
                fact[o].value = (uint64_t)ir->op[o].imm;
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned b = 0; b < ir->nblocks; ++b) {
                if (ir->block[b].flags & T1_BLK_DEAD)
                    continue;
                unsigned pos = 0;
                for (uint32_t o = ir->block[b].first; o != T1_NO_OP; o = ir->op[o].next) {
                    unsigned here = ir->op[o].code == T1_IR_PARAM ? pos++ : 0;
                    if (fact[o].known || ir->op[o].fwd != T1_VALUE_NONE)
                        continue;
                    uint64_t value = 0;
                    if (fact_eval(ir, fact, o, here, &value)) {
                        fact[o].known = true;
                        fact[o].value = value;
                        changed = true;
                    }
                }
            }
        }
        bool rejected = false;
        for (unsigned b = 0; b < ir->nblocks; ++b) {
            unsigned pos = 0;
            for (uint32_t o = ir->block[b].first; o != T1_NO_OP && ir->op[o].code == T1_IR_PARAM;
                 o = ir->op[o].next, ++pos) {
                uint64_t value = 0;
                if (fact[o].candidate &&
                    (!fact_eval(ir, fact, o, pos, &value) || value != fact[o].value)) {
                    fact[o].candidate = false;
                    --candidates;
                    rejected = true;
                }
            }
        }
        if (!rejected)
            break;
    }
    unsigned nops = ir->nops;
    if (candidates)
        for (unsigned o = 0; o < nops; ++o)
            if (fact[o].candidate)
                t1_ir_forward(ir, o, t1_ir_const(ir, T1_I64, fact[o].value));
    t1_ws_free(ir->ws, fact, pages ? pages : 1);
    return candidates != 0;
}

static bool has_constant_or(const struct t1_ir *ir)
{
    for (unsigned o = 0; o < ir->nops; ++o)
        if (ir->op[o].code == T1_IR_OR && ir->op[o].fwd == T1_VALUE_NONE &&
            !(ir->op[o].flags & T1_OPF_DEAD) &&
            (any_const(ir, opnd(ir, o, 0)) || any_const(ir, opnd(ir, o, 1))))
            return true;
    return false;
}

/* Preserve all unknown bits when an OR only sets bits already known one. */
static bool fold_known_or(struct t1_ir *ir)
{
    if (!has_constant_or(ir))
        return false;
    size_t pages = (ir->nops * (sizeof(struct t1_known) + 1) + 4095) / 4096;
    struct t1_known *kb = t1_ws_alloc(ir->ws, pages ? pages : 1, 0);
    if (!kb) {
        t1_ir_fail(ir, "no memory for known-bit folding");
        return false;
    }
    uint8_t *roots = (uint8_t *)(kb + ir->nops);
    memset(roots, 0, ir->nops);
    for (unsigned o = 0; o < ir->nops; ++o)
        if (ir->op[o].code == T1_IR_OR && ir->op[o].fwd == T1_VALUE_NONE &&
            !(ir->op[o].flags & T1_OPF_DEAD))
            for (unsigned k = 0; k < 2; ++k)
                if (any_const(ir, opnd(ir, o, k ^ 1)))
                    roots[T1_VALUE_OP(opnd(ir, o, k))] = 1;
    if (!t1_known_bits_demand(ir, kb, roots)) {
        t1_ws_free(ir->ws, kb, pages ? pages : 1);
        return false;
    }
    bool changed = false;
    for (unsigned o = 0; o < ir->nops; ++o) {
        if (ir->op[o].code != T1_IR_OR || ir->op[o].fwd != T1_VALUE_NONE ||
            (ir->op[o].flags & T1_OPF_DEAD))
            continue;
        for (unsigned k = 0; k < 2; ++k) {
            t1_value a = opnd(ir, o, k), c = opnd(ir, o, k ^ 1);
            uint64_t mask = (uint64_t)ir->op[T1_VALUE_OP(c)].imm;
            if (any_const(ir, c) && (kb[T1_VALUE_OP(a)].one & mask) == mask) {
                t1_ir_forward(ir, o, a);
                changed = true;
                break;
            }
        }
    }
    t1_ws_free(ir->ws, kb, pages ? pages : 1);
    return changed;
}

/* Unlinks forwarded ops after their uses were rewritten. */
static void cleanup(struct t1_ir *ir)
{
    t1_ir_resolve_all(ir);
    for (unsigned o = 0; o < ir->nops; ++o)
        if (ir->op[o].fwd != T1_VALUE_NONE && !(ir->op[o].flags & T1_OPF_DEAD))
            t1_ir_remove(ir, o);
    t1_ir_compact_params(ir);
}

void t1_opt_fold(struct t1_ir *ir)
{
    for (bool changed = true; changed && !ir->failed;) {
        changed = false;
        changed |= fold_params(ir);
        changed |= fold_i1_constants(ir);
        for (unsigned b = 0; b < ir->nblocks; ++b) {
            if (ir->block[b].flags & T1_BLK_DEAD)
                continue;
            for (uint32_t o = ir->block[b].first, next; o != T1_NO_OP; o = next) {
                next = ir->op[o].next;
                if (ir->op[o].fwd != T1_VALUE_NONE)
                    continue;
                if (ir->op[o].code == T1_IR_NATCHK && is_const(ir, opnd(ir, o, 0), 0)) {
                    t1_ir_remove(ir, o);
                    changed = true;
                    continue;
                }
                t1_value v = simplify(ir, o);
                if (v != T1_VALUE_NONE && v != T1_VALUE(o, 0)) {
                    t1_ir_forward(ir, o, v);
                    changed = true;
                }
            }
        }
        if (!changed && !ir->failed)
            changed = fold_loop_constants(ir);
        if (!changed && !ir->failed)
            changed = fold_known_or(ir);
    }
    cleanup(ir);
}

/* May-clobbered homes only grow: a later LIVEIN read cannot re-establish
 * a home because alloc and inline FP paths can skip materialization. */
static bool clobber_homes(const struct t1_ir *ir, const struct t1_op *op, uint64_t *set)
{
    if (op->code != T1_IR_HELPER && op->code != T1_IR_GHELPER)
        return true;
    for (unsigned k = 1; k < op->nuse; ++k) {
        const struct t1_use *u = &ir->use[op->use_first + k];
        const struct t1_op *d = &ir->op[T1_VALUE_OP(u->value)];
        if ((d->code != T1_IR_LIVEIN && d->code != T1_IR_NATIN) || d->imm != u->element)
            t1_elem_add(set, u->element);
    }
    if (op->code == T1_IR_GHELPER) {
        if (!ir->form || op->imm < 0 || (uint64_t)op->imm >= ir->form->ninsns)
            return false;
        uint16_t writes[T1_N_ELEMENTS];
        unsigned n = t1_helper_elements(&ir->form->insn[op->imm], true, writes);
        for (unsigned k = 0; k < n; ++k)
            t1_elem_add(set, writes[k]);
    } else
        for (unsigned k = 0; k < 2; ++k)
            if (op->wr[k])
                t1_elem_add(set, op->wr[k]);
    return true;
}

static void frame_homes(struct t1_ir *ir, struct t1_ir_point *pt, const uint64_t *clobbered)
{
    if (pt->dead)
        return;
    for (unsigned k = 0; k < pt->frame_count; ++k) {
        struct t1_use *u = &ir->use[pt->frame_first + k];
        if (u->flags & T1_USE_HOME)
            continue;
        const struct t1_op *d = &ir->op[T1_VALUE_OP(u->value)];
        /* Native NaT consumption can still replace this helper input
         * with CONST 0. Wait for natguard before proving a NaT in W. */
        if (!ir->nat_folded && u->element > 128 && u->element < 256 &&
            t1_elem_has(ir->w_set, u->element))
            continue;
        if ((d->code == T1_IR_LIVEIN || d->code == T1_IR_NATIN) && d->imm == u->element &&
            !t1_elem_has(clobbered, u->element)) {
            u->flags |= T1_USE_HOME;
            u->value = T1_VALUE_NONE;
        }
    }
}

void t1_opt_frames(struct t1_ir *ir)
{
    if (ir->failed)
        return;
    bool need_proof = false;
    for (unsigned w = 0; w < T1_ELEMENT_WORDS; ++w)
        need_proof |= ir->w_set[w] != 0;
    struct home_block {
        uint64_t kill[T1_ELEMENT_WORDS], out[T1_ELEMENT_WORDS];
    } *home = NULL;
    size_t pages = (ir->nblocks * sizeof(*home) + 4095) / 4096;
    if (need_proof && pages)
        home = t1_ws_alloc(ir->ws, pages, HAL_PAGE_ZERO);
    bool complete = false;
    if (home) {
        for (unsigned b = 0; b < ir->nblocks; ++b) {
            if (ir->block[b].flags & T1_BLK_DEAD)
                continue;
            for (uint32_t o = ir->block[b].first; o != T1_NO_OP; o = ir->op[o].next)
                if (!clobber_homes(ir, &ir->op[o], home[b].kill))
                    goto done;
            memcpy(home[b].out, home[b].kill, sizeof(home[b].out));
        }
        /* Static kills reach every successor in at most nblocks passes.
         * Do not annotate until loop-carried clobbers have converged. */
        for (unsigned pass = 0; pass < ir->nblocks; ++pass) {
            bool changed = false;
            for (unsigned b = 0; b < ir->nblocks; ++b) {
                const struct t1_block *blk = &ir->block[b];
                if (blk->flags & T1_BLK_DEAD)
                    continue;
                for (unsigned w = 0; w < T1_ELEMENT_WORDS; ++w) {
                    uint64_t out = home[b].kill[w];
                    for (unsigned k = 0; k < blk->npred; ++k)
                        out |= home[blk->pred[k]].out[w];
                    changed |= out != home[b].out[w];
                    home[b].out[w] = out;
                }
            }
            if (!changed) {
                complete = true;
                break;
            }
        }
    }
done:
    if (complete) {
        for (unsigned b = 0; b < ir->nblocks; ++b) {
            const struct t1_block *blk = &ir->block[b];
            if (blk->flags & T1_BLK_DEAD)
                continue;
            uint64_t clobbered[T1_ELEMENT_WORDS] = {0};
            for (unsigned k = 0; k < blk->npred; ++k)
                for (unsigned w = 0; w < T1_ELEMENT_WORDS; ++w)
                    clobbered[w] |= home[blk->pred[k]].out[w];
            for (uint32_t o = blk->first; o != T1_NO_OP; o = ir->op[o].next) {
                const struct t1_op *op = &ir->op[o];
                /* A helper's point precedes its operand write-backs. */
                if (op->point != T1_NO_POINT)
                    frame_homes(ir, &ir->point[op->point], clobbered);
                (void)clobber_homes(ir, op, clobbered);
            }
        }
    } else {
        /* Resource or analysis limits retain the conservative W rule. */
        for (unsigned p = 0; p < ir->npoints; ++p)
            frame_homes(ir, &ir->point[p], ir->w_set);
    }
    if (home)
        t1_ws_free(ir->ws, home, pages);
}

static void swap_successors(struct t1_ir *ir, uint16_t b)
{
    struct t1_block *blk = &ir->block[b];
    for (unsigned s = 0; s < 2; ++s) {
        struct t1_block *tb = &ir->block[blk->succ[s].target];
        for (unsigned p = 0; p < tb->npred; ++p)
            if (tb->pred[p] == b && tb->pred_slot[p] == s)
                tb->pred_slot[p] = (uint8_t)(2 + (1 - s));
    }
    for (unsigned s = 0; s < 2; ++s) {
        struct t1_block *tb = &ir->block[blk->succ[s].target];
        for (unsigned p = 0; p < tb->npred; ++p)
            if (tb->pred[p] == b && tb->pred_slot[p] >= 2)
                tb->pred_slot[p] -= 2;
    }
    struct t1_edge e = blk->succ[0];
    blk->succ[0] = blk->succ[1];
    blk->succ[1] = e;
}

void t1_opt_predicates(struct t1_ir *ir)
{
    bool changed = false;
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        if (ir->block[b].flags & T1_BLK_DEAD)
            continue;
        uint32_t t = t1_ir_terminator(ir, (uint16_t)b);
        if (t == T1_NO_OP || ir->op[t].code != T1_IR_BRANCH)
            continue;
        t1_value c = opnd(ir, t, 0);
        /* BRANCH (NOT1 x) T F -> BRANCH x F T. */
        while (ir->op[T1_VALUE_OP(c)].code == T1_IR_NOT1) {
            c = opnd(ir, T1_VALUE_OP(c), 0);
            ir->use[ir->op[t].use_first].value = c;
            swap_successors(ir, (uint16_t)b);
        }
        if (any_const(ir, c)) {
            t1_ir_branch_to_jump(ir, (uint16_t)b, ir->op[T1_VALUE_OP(c)].imm ? 0 : 1);
            changed = true;
        }
    }
    if (changed)
        t1_ir_remove_unreachable(ir);
}

/* ---- DCE ---- */

static void mark_op(struct t1_ir *ir, struct t1_worklist *w, uint32_t o)
{
    if (ir->op[o].flags & T1_OPF_MARK)
        return;
    ir->op[o].flags |= T1_OPF_MARK;
    w->op[w->count++] = (uint16_t)o;
}

static void use_value(struct t1_ir *ir, struct t1_worklist *w, t1_value v)
{
    mark_op(ir, w, T1_VALUE_OP(t1_ir_resolve(ir, v)));
}

/* Roots: every op that is not PURE or PARAM (memory ops, helpers, fences,
 * NATCHK, terminators). Uses: operands, the incoming arguments of a live
 * PARAM, and the non-home frame entries of a live point (pseudo-uses). */
void t1_opt_dce(struct t1_ir *ir)
{
    struct t1_worklist w;
    if (!t1_worklist_init(&w, ir))
        return;
    for (unsigned o = 0; o < ir->nops; ++o)
        ir->op[o].flags &= (uint8_t)~T1_OPF_MARK;
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        const struct t1_block *blk = &ir->block[b];
        if (blk->flags & T1_BLK_DEAD)
            continue;
        for (uint32_t o = blk->first; o != T1_NO_OP; o = ir->op[o].next)
            if (t1_op_desc(ir->op[o].code)->effect != T1_EFF_PURE && ir->op[o].code != T1_IR_PARAM)
                mark_op(ir, &w, o);
    }
    for (unsigned head = 0; head < w.count; ++head) {
        uint32_t o = w.op[head];
        const struct t1_op *op = &ir->op[o];
        const struct t1_block *blk = &ir->block[op->block];
        for (unsigned i = 0; i < op->nuse; ++i)
            use_value(ir, &w, ir->use[op->use_first + i].value);
        if (op->code == T1_IR_PARAM) {
            for (unsigned p = 0; p < blk->npred; ++p) {
                const struct t1_edge *e = &ir->block[blk->pred[p]].succ[blk->pred_slot[p]];
                use_value(ir, &w, ir->use[e->args_first + w.param_pos[o]].value);
            }
        }
        if (op->point != T1_NO_POINT && !ir->point[op->point].dead) {
            const struct t1_ir_point *pt = &ir->point[op->point];
            for (unsigned k = 0; k < pt->frame_count; ++k)
                if (!(ir->use[pt->frame_first + k].flags & T1_USE_HOME))
                    use_value(ir, &w, ir->use[pt->frame_first + k].value);
        }
    }
    t1_worklist_free(&w, ir);
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        if (ir->block[b].flags & T1_BLK_DEAD)
            continue;
        for (uint32_t o = ir->block[b].first, next; o != T1_NO_OP; o = next) {
            next = ir->op[o].next;
            if (!(ir->op[o].flags & T1_OPF_MARK))
                t1_ir_remove(ir, o);
        }
    }
    for (unsigned o = 0; o < ir->nops; ++o)
        ir->op[o].flags &= (uint8_t)~T1_OPF_MARK;
    t1_ir_compact_params(ir);
}

/* ---- known bits ---- */

static const struct t1_known UNKNOWN = {0, 0};
static const struct t1_known TOP = {UINT64_MAX, UINT64_MAX}; /* not yet reached */

static bool is_top(struct t1_known k)
{
    return k.zero & k.one;
}

static uint64_t low_mask(unsigned len)
{
    return len >= 64 ? UINT64_MAX : (UINT64_C(1) << len) - 1;
}

/* LLVM KnownBits::computeForAddCarry. */
static struct t1_known add_carry(struct t1_known a, struct t1_known b, bool carry)
{
    uint64_t sum_zero = ~a.zero + ~b.zero + (carry ? 1 : 0);
    uint64_t sum_one = a.one + b.one + (carry ? 1 : 0);
    uint64_t carry_zero = ~(sum_zero ^ a.zero ^ b.zero);
    uint64_t carry_one = sum_one ^ a.one ^ b.one;
    uint64_t known = (a.zero | a.one) & (b.zero | b.one) & (carry_zero | carry_one);
    return (struct t1_known){~sum_zero & known, sum_one & known};
}

static struct t1_known sign_extend_known(struct t1_known k, unsigned bits)
{
    uint64_t m = low_mask(bits), sign = UINT64_C(1) << (bits - 1);
    struct t1_known r = {k.zero & m, k.one & m};
    if (k.zero & sign)
        r.zero |= ~m;
    else if (k.one & sign)
        r.one |= ~m;
    return r;
}

/* The operands read by the known-bit transfer function. */
static unsigned known_inputs(unsigned code)
{
    switch (code) {
    case T1_IR_ADD:
    case T1_IR_SUB:
    case T1_IR_AND:
    case T1_IR_OR:
    case T1_IR_XOR:
    case T1_IR_ANDCM:
        return 3;
    case T1_IR_SHL_IMM:
    case T1_IR_EXTR:
    case T1_IR_EXTRU:
    case T1_IR_DEPZ:
    case T1_IR_SXT:
    case T1_IR_ZXT:
        return 1;
    default:
        return 0;
    }
}

static struct t1_known transfer(const struct t1_ir *ir, const struct t1_known *kb, uint32_t o)
{
    const struct t1_op *op = &ir->op[o];
    struct t1_known a = UNKNOWN, b = UNKNOWN;
    if (op->code == T1_IR_CONST)
        return (struct t1_known){~(uint64_t)op->imm, (uint64_t)op->imm};
    if (op->nuse > 0)
        a = kb[T1_VALUE_OP(opnd(ir, o, 0))];
    if (op->nuse > 1)
        b = kb[T1_VALUE_OP(opnd(ir, o, 1))];
    unsigned inputs = known_inputs(op->code);
    if (((inputs & 1) && is_top(a)) || ((inputs & 2) && is_top(b)))
        return TOP;
    unsigned pos = T1_FIELD_POS(op->imm), len = T1_FIELD_LEN(op->imm);
    switch (op->code) {
    case T1_IR_ADD:
        return add_carry(a, b, false);
    case T1_IR_SUB:
        return add_carry(a, (struct t1_known){b.one, b.zero}, true);
    case T1_IR_AND:
        return (struct t1_known){a.zero | b.zero, a.one & b.one};
    case T1_IR_OR:
        return (struct t1_known){a.zero & b.zero, a.one | b.one};
    case T1_IR_XOR:
        return (struct t1_known){(a.zero & b.zero) | (a.one & b.one),
                                 (a.zero & b.one) | (a.one & b.zero)};
    case T1_IR_ANDCM:
        return (struct t1_known){a.zero | b.one, a.one & b.zero};
    case T1_IR_SHL_IMM: {
        unsigned c = (unsigned)op->imm & 63;
        return (struct t1_known){(a.zero << c) | low_mask(c), a.one << c};
    }
    case T1_IR_ZXT: {
        uint64_t m = low_mask((unsigned)op->imm);
        return (struct t1_known){a.zero | ~m, a.one & m};
    }
    case T1_IR_SXT:
        return sign_extend_known(a, (unsigned)op->imm);
    case T1_IR_EXTRU:
    case T1_IR_EXTR: {
        struct t1_known r = {(a.zero >> pos) | ~low_mask(len), (a.one >> pos) & low_mask(len)};
        return op->code == T1_IR_EXTR ? sign_extend_known(r, len) : r;
    }
    case T1_IR_DEPZ: {
        uint64_t field = low_mask(len) << pos;
        return (struct t1_known){~field | ((a.zero << pos) & field), (a.one << pos) & field};
    }
    default:
        return UNKNOWN;
    }
}

static struct t1_known known_eval(const struct t1_ir *ir, const struct t1_known *kb, uint32_t o,
                                  unsigned pos)
{
    const struct t1_op *op = &ir->op[o];
    if (op->code == T1_IR_PARAM) {
        const struct t1_block *blk = &ir->block[op->block];
        struct t1_known k = TOP;
        for (unsigned p = 0; p < blk->npred; ++p) {
            const struct t1_edge *e = &ir->block[blk->pred[p]].succ[blk->pred_slot[p]];
            struct t1_known in =
                kb[T1_VALUE_OP(t1_ir_resolve(ir, ir->use[e->args_first + pos].value))];
            if (is_top(in))
                continue;
            k = is_top(k) ? in : (struct t1_known){k.zero & in.zero, k.one & in.one};
        }
        return k;
    }
    if (op->code == T1_IR_SELECT && op->type == T1_I64) {
        struct t1_known t = kb[T1_VALUE_OP(opnd(ir, o, 1))];
        struct t1_known f = kb[T1_VALUE_OP(opnd(ir, o, 2))];
        return is_top(t) ? f : is_top(f) ? t : (struct t1_known){t.zero & f.zero, t.one & f.one};
    }
    if (t1_op_desc(op->code)->nres && t1_ir_value_type(ir, T1_VALUE(o, 0)) == T1_I64)
        return transfer(ir, kb, o);
    return UNKNOWN;
}

void t1_known_bits(const struct t1_ir *ir, struct t1_known *kb)
{
    /* With only entry and a parameter-free header, live definitions are
     * already in dependency order: entry leaves first, then same-block
     * SSA definitions before their uses. The first pass is a fixed point. */
    uint32_t first = ir->nblocks == 2 ? ir->block[T1_HEADER_BLOCK].first : T1_NO_OP;
    bool single_pass = ir->nblocks == 2 && (first == T1_NO_OP || ir->op[first].code != T1_IR_PARAM);
    for (unsigned o = 0; o < ir->nops; ++o)
        kb[o] = TOP;
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = 0; b < ir->nblocks; ++b) {
            const struct t1_block *blk = &ir->block[b];
            if (blk->flags & T1_BLK_DEAD)
                continue;
            unsigned pos = 0;
            for (uint32_t o = blk->first; o != T1_NO_OP; o = ir->op[o].next) {
                struct t1_known k = known_eval(ir, kb, o, pos);
                if (ir->op[o].code == T1_IR_PARAM)
                    ++pos;
                if (k.zero != kb[o].zero || k.one != kb[o].one) {
                    kb[o] = k;
                    changed = true;
                }
            }
        }
        if (single_pass)
            break;
    }
    for (unsigned o = 0; o < ir->nops; ++o)
        if (is_top(kb[o]))
            kb[o] = UNKNOWN;
}

static void known_add(struct t1_worklist *w, struct t1_known *kb, uint32_t o)
{
    if (kb[o].zero || !kb[o].one)
        return;
    kb[o].zero = 1; /* visited until the dependency closure is complete */
    w->op[w->count++] = (uint16_t)o;
}

bool t1_known_bits_demand(struct t1_ir *ir, struct t1_known *kb, const uint8_t *roots)
{
    struct t1_worklist w;
    if (!t1_worklist_init(&w, ir))
        return false;
    for (unsigned o = 0; o < ir->nops; ++o)
        kb[o] = UNKNOWN;
    /* Match the full analysis's linked domain, including removed PARAMs
     * that have not yet been compacted. */
    for (unsigned b = 0; b < ir->nblocks; ++b)
        if (!(ir->block[b].flags & T1_BLK_DEAD))
            for (uint32_t o = ir->block[b].first; o != T1_NO_OP; o = ir->op[o].next)
                kb[o].one = 1;
    for (unsigned o = 0; o < ir->nops; ++o)
        if (roots[o])
            known_add(&w, kb, o);
    for (unsigned head = 0; head < w.count; ++head) {
        uint32_t o = w.op[head];
        const struct t1_op *op = &ir->op[o];
        if (op->code == T1_IR_PARAM) {
            const struct t1_block *blk = &ir->block[op->block];
            for (unsigned p = 0; p < blk->npred; ++p) {
                const struct t1_edge *e = &ir->block[blk->pred[p]].succ[blk->pred_slot[p]];
                t1_value v = t1_ir_resolve(ir, ir->use[e->args_first + w.param_pos[o]].value);
                known_add(&w, kb, T1_VALUE_OP(v));
            }
        } else if (op->code == T1_IR_SELECT && op->type == T1_I64) {
            known_add(&w, kb, T1_VALUE_OP(opnd(ir, o, 1)));
            known_add(&w, kb, T1_VALUE_OP(opnd(ir, o, 2)));
        } else if (t1_op_desc(op->code)->nres && t1_ir_value_type(ir, T1_VALUE(o, 0)) == T1_I64) {
            unsigned inputs = known_inputs(op->code);
            for (unsigned k = 0; k < 2; ++k)
                if (inputs & (1u << k))
                    known_add(&w, kb, T1_VALUE_OP(opnd(ir, o, k)));
        }
    }
    /* Use the full analysis's block/instruction order and equations. */
    w.count = 0;
    for (unsigned b = 0; b < ir->nblocks; ++b)
        if (!(ir->block[b].flags & T1_BLK_DEAD))
            for (uint32_t o = ir->block[b].first; o != T1_NO_OP; o = ir->op[o].next)
                if (kb[o].zero)
                    w.op[w.count++] = (uint16_t)o;
    /* Unlinked definitions remain TOP during propagation, exactly as
     * in the full analysis; only final output turns residual TOP unknown. */
    for (unsigned o = 0; o < ir->nops; ++o)
        kb[o] = TOP;
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned n = 0; n < w.count; ++n) {
            uint32_t o = w.op[n];
            struct t1_known k =
                known_eval(ir, kb, o, ir->op[o].code == T1_IR_PARAM ? w.param_pos[o] : 0);
            if (k.zero != kb[o].zero || k.one != kb[o].one) {
                kb[o] = k;
                changed = true;
            }
        }
    }
    for (unsigned o = 0; o < ir->nops; ++o)
        if (!roots[o] || is_top(kb[o]))
            kb[o] = UNKNOWN;
    t1_worklist_free(&w, ir);
    return true;
}

/* ---- pipeline ---- */

#ifdef IA64VMM_HOSTED
void (*t1_test_pass_hook)(struct t1_ir *ir, const char *pass);
#endif

static bool check(struct t1_ir *ir, bool verify, unsigned flags, const char *pass, char *err,
                  size_t size)
{
#ifdef IA64VMM_HOSTED
    if (t1_test_pass_hook)
        t1_test_pass_hook(ir, pass);
#endif
    if (ir->failed) {
        if (size) {
            err[0] = 0;
        }
        return false;
    }
    if (!verify)
        return true;
    char inner[160];
    if (t1_ir_verify(ir, flags, inner, sizeof(inner)))
        return true;
    size_t n = strlen(pass);
    if (size) {
        size_t m = n < size - 1 ? n : size - 1;
        memcpy(err, pass, m);
        err[m] = 0;
        size_t k = strlen(inner);
        if (m + 2 + k < size) {
            err[m] = ':';
            err[m + 1] = ' ';
            memcpy(err + m + 2, inner, k + 1);
        }
    }
    return false;
}

bool t1_optimize(struct t1_ir *ir, bool verify, char *err, size_t err_size)
{
    if (!check(ir, verify, 0, "lift", err, err_size))
        return false;
    t1_opt_fold(ir);
    t1_opt_predicates(ir);
    t1_opt_fold(ir);
    if (!check(ir, verify, 0, "fold", err, err_size))
        return false;
    t1_opt_frames(ir);
    if (!check(ir, verify, 0, "frames", err, err_size))
        return false;
    t1_opt_dce(ir);
    if (!check(ir, verify, 0, "dce", err, err_size))
        return false;
    t1_natguard(ir);
    if (ir->boundary_insn != T1_NO_INSN) {
        t1_ir_fail(ir, "NaT consumption check did not fold");
        if (err_size)
            vmm_snprintf(err, err_size, "natguard: NATCHK of instruction %u did not fold",
                         ir->boundary_insn);
        return false;
    }
    if (!check(ir, verify, T1_VERIFY_NAT_FOLDED, "natguard", err, err_size))
        return false;
    t1_opt_predicates(ir);
    t1_opt_fold(ir);
    if (!check(ir, verify, T1_VERIFY_NAT_FOLDED, "predicates", err, err_size))
        return false;
    t1_opt_frames(ir);
    if (!check(ir, verify, T1_VERIFY_NAT_FOLDED, "frames2", err, err_size))
        return false;
    t1_opt_dce(ir);
    return check(ir, verify, T1_VERIFY_NAT_FOLDED, "dce2", err, err_size);
}
