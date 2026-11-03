/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tier1 IR: op descriptors from ir_ops.def, builder, CFG queries,
 * forwarding/rewriting, printing. Freestanding.
 */
#include "engine/tier1/ir.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define T1_TY_I64 T1_I64
#define T1_TY_I1 T1_I1
#define T1_TY_MEM T1_MEM
#define T1_TY_T T1_TYPE_T
#define T1_TY_VAR T1_TYPE_VAR

static const struct t1_op_desc op_desc[T1_IR_OP_COUNT] = {
#define T1_IR_OP(name_, imm_, effect_, nopnd_, nres_)                                              \
    [T1_IR_##name_].name = #name_, [T1_IR_##name_].imm = T1_IMM_##imm_,                            \
    [T1_IR_##name_].effect = T1_EFF_##effect_, [T1_IR_##name_].nopnd = nopnd_,                     \
    [T1_IR_##name_].nres = nres_,
#define T1_IR_OPERAND(op_, index_, name_, type_) [T1_IR_##op_].opnd_type[index_] = T1_TY_##type_,
#define T1_IR_RESULT(op_, index_, name_, type_)                                                    \
    [T1_IR_##op_].res_type[index_] = T1_TY_##type_, [T1_IR_##op_].res_name[index_] = #name_,
#include "engine/tier1/ir_ops.def"
};

const struct t1_op_desc *t1_op_desc(unsigned code)
{
    return code < T1_IR_OP_COUNT ? &op_desc[code] : NULL;
}

bool t1_ir_fail(struct t1_ir *ir, const char *reason)
{
    if (!ir->failed) {
        ir->failed = true;
        ir->fail_reason = reason;
    }
    return false;
}

void t1_ir_init(struct t1_ir *ir, const struct t1_form *form)
{
    ir->form = form;
    ir->nops = ir->nblocks = ir->nuses = ir->npoints = ir->nsites = 0;
    ir->failed = false;
    ir->fail_reason = NULL;
    memset(ir->frame_set, 0, sizeof(ir->frame_set));
    memset(ir->w_set, 0, sizeof(ir->w_set));
    ir->nat_guard[0] = ir->nat_guard[1] = 0;
    ir->nat_folded = false;
    ir->boundary_insn = T1_NO_INSN;
    for (unsigned k = 0; k < T1_ENTRY_SLOTS; ++k)
        ir->entry_slot[k] = T1_NO_OP;
}

uint16_t t1_ir_new_block(struct t1_ir *ir, uint8_t flags)
{
    if (ir->nblocks >= T1_MAX_BLOCKS) {
        t1_ir_fail(ir, "block limit");
        return T1_NO_BLOCK;
    }
    uint16_t b = (uint16_t)ir->nblocks++;
    struct t1_block *blk = &ir->block[b];
    memset(blk, 0, sizeof(*blk));
    blk->first = blk->last = T1_NO_OP;
    blk->flags = flags;
    return b;
}

/* The reserved uses are not initialized: the caller writes every field of
 * each before the IR is read or verified. alloc_uses() clears them. */
static uint32_t reserve_uses(struct t1_ir *ir, unsigned n)
{
    if (n > T1_IR_MAX_USES - ir->nuses) {
        t1_ir_fail(ir, "use pool exhausted");
        return UINT32_MAX;
    }
    uint32_t first = ir->nuses;
    ir->nuses += n;
    return first;
}

static uint32_t alloc_uses(struct t1_ir *ir, unsigned n)
{
    uint32_t first = reserve_uses(ir, n);
    if (first == UINT32_MAX)
        return first;
    for (unsigned i = 0; i < n; ++i)
        ir->use[first + i] = (struct t1_use){T1_VALUE_NONE, 0, 0};
    return first;
}

static void link_after(struct t1_ir *ir, uint16_t b, uint32_t after, uint32_t op)
{
    struct t1_block *blk = &ir->block[b];
    struct t1_op *o = &ir->op[op];
    o->block = b;
    o->prev = after;
    o->next = after == T1_NO_OP ? blk->first : ir->op[after].next;
    if (o->prev == T1_NO_OP)
        blk->first = op;
    else
        ir->op[o->prev].next = op;
    if (o->next == T1_NO_OP)
        blk->last = op;
    else
        ir->op[o->next].prev = op;
    /* The generic append API can also append a PARAM to an empty prefix. */
    if (o->code == T1_IR_PARAM)
        blk->param_end = (uint16_t)(op + 1);
}

static uint32_t new_op(struct t1_ir *ir, uint16_t code, uint8_t type, int64_t imm,
                       const t1_value *opnds, unsigned n, uint16_t insn)
{
    if (ir->nops >= T1_IR_MAX_OPS) {
        t1_ir_fail(ir, "op limit");
        return T1_NO_OP;
    }
    uint32_t first = reserve_uses(ir, n);
    if (first == UINT32_MAX)
        return T1_NO_OP;
    uint32_t i = ir->nops++;
    struct t1_op *o = &ir->op[i];
    *o = (struct t1_op){.code = code,
                        .type = type,
                        .imm = imm,
                        .insn = insn,
                        .prev = T1_NO_OP,
                        .next = T1_NO_OP,
                        .use_first = first,
                        .nuse = (uint16_t)n,
                        .use_cap = (uint16_t)n,
                        .point = T1_NO_POINT,
                        .fwd = T1_VALUE_NONE};
    for (unsigned k = 0; k < n; ++k)
        ir->use[first + k] = (struct t1_use){opnds[k], 0, 0};
    return i;
}

uint32_t t1_ir_append(struct t1_ir *ir, uint16_t block, uint16_t code, uint8_t type, int64_t imm,
                      const t1_value *opnds, unsigned n, uint16_t insn)
{
    if (ir->failed)
        return T1_NO_OP;
    uint32_t i = new_op(ir, code, type, imm, opnds, n, insn);
    if (i == T1_NO_OP)
        return i;
    uint32_t term = t1_ir_terminator(ir, block);
    link_after(ir, block, term == T1_NO_OP ? ir->block[block].last : ir->op[term].prev, i);
    return i;
}

/* The live entry-block op (code, type, imm), created first in the entry
 * block (it dominates everything) if there is none. Every such op is made
 * here. Bucket chains keep the index usable up to the IR op limit, without
 * falling back to an entry-block scan when there are many constants.
 * DCE's removed ops and natguard's changed LIVEINs no longer match. */
static t1_value entry_value(struct t1_ir *ir, uint16_t code, uint8_t type, int64_t imm)
{
    if (ir->failed)
        return T1_VALUE_NONE;
    uint64_t key = ((uint64_t)imm * UINT64_C(0x9e3779b97f4a7c15)) ^ ((uint64_t)code << 40) ^
                   ((uint64_t)type << 32);
    unsigned h = (unsigned)(key >> 52) & (T1_ENTRY_SLOTS - 1);
    for (uint32_t o = ir->entry_slot[h]; o != T1_NO_OP; o = ir->op[o].entry_next) {
        const struct t1_op *op = &ir->op[o];
        if (op->code == code && op->type == type && op->imm == imm && !(op->flags & T1_OPF_DEAD))
            return T1_VALUE(o, 0);
    }
    uint32_t i = new_op(ir, code, type, imm, NULL, 0, T1_NO_INSN);
    if (i == T1_NO_OP)
        return T1_VALUE_NONE;
    link_after(ir, T1_ENTRY_BLOCK, T1_NO_OP, i); /* first: dominates everything */
    ir->op[i].entry_next = ir->entry_slot[h];
    ir->entry_slot[h] = i;
    return T1_VALUE(i, 0);
}

t1_value t1_ir_const(struct t1_ir *ir, uint8_t type, uint64_t value)
{
    if (type == T1_I1)
        value &= 1;
    return entry_value(ir, T1_IR_CONST, type, (int64_t)value);
}

t1_value t1_ir_livein(struct t1_ir *ir, uint8_t type, uint16_t element)
{
    return entry_value(ir, T1_IR_LIVEIN, type, element);
}

uint32_t t1_ir_add_param(struct t1_ir *ir, uint16_t block, uint8_t type, uint16_t element)
{
    _Static_assert(T1_IR_MAX_OPS <= UINT16_MAX, "parameter tail stores op + 1");
    if (ir->failed)
        return T1_NO_OP;
    uint32_t i = new_op(ir, T1_IR_PARAM, type, element, NULL, 0, T1_NO_INSN);
    if (i == T1_NO_OP)
        return i;
    struct t1_block *blk = &ir->block[block];
    uint32_t after = blk->param_end ? blk->param_end - 1u : T1_NO_OP;
    link_after(ir, block, after, i);
    return i;
}

/* Grows a use array, extending it in place when it already ends the pool. */
static bool grow(struct t1_ir *ir, uint32_t *first, uint16_t n, uint16_t *cap)
{
    if (n < *cap)
        return true;
    unsigned ncap = *cap ? *cap * 2u : 8u;
    if (ncap > UINT16_MAX)
        return t1_ir_fail(ir, "operand list too long");
    if (*first + *cap == ir->nuses) {
        if (alloc_uses(ir, ncap - *cap) == UINT32_MAX)
            return false;
        *cap = (uint16_t)ncap;
        return true;
    }
    uint32_t nf = alloc_uses(ir, ncap);
    if (nf == UINT32_MAX)
        return false;
    for (unsigned i = 0; i < n; ++i)
        ir->use[nf + i] = ir->use[*first + i];
    *first = nf;
    *cap = (uint16_t)ncap;
    return true;
}

bool t1_ir_add_operand(struct t1_ir *ir, uint32_t op, t1_value v, uint16_t element)
{
    struct t1_op *o = &ir->op[op];
    if (ir->failed || !grow(ir, &o->use_first, o->nuse, &o->use_cap))
        return false;
    ir->use[o->use_first + o->nuse++] = (struct t1_use){v, element, 0};
    return true;
}

static bool add_edge(struct t1_ir *ir, uint16_t b, unsigned slot, uint16_t target)
{
    struct t1_block *tb = &ir->block[target];
    if (tb->npred >= T1_MAX_BLOCKS)
        return t1_ir_fail(ir, "too many predecessors");
    ir->block[b].succ[slot] = (struct t1_edge){target, 0, 0, 0};
    tb->pred[tb->npred] = (uint8_t)b;
    tb->pred_slot[tb->npred++] = (uint8_t)slot;
    return true;
}

static bool set_terminator(struct t1_ir *ir, uint16_t b, uint16_t code, const t1_value *opnds,
                           unsigned n, int64_t imm, uint32_t point, uint16_t insn)
{
    if (ir->failed)
        return false;
    if (t1_ir_terminator(ir, b) != T1_NO_OP)
        return t1_ir_fail(ir, "block already terminated");
    uint32_t i = new_op(ir, code, 0, imm, opnds, n, insn);
    if (i == T1_NO_OP)
        return false;
    link_after(ir, b, ir->block[b].last, i);
    ir->op[i].point = point;
    if (point != T1_NO_POINT)
        ir->point[point].op = i;
    return true;
}

bool t1_ir_set_jump(struct t1_ir *ir, uint16_t block, uint16_t target)
{
    if (!set_terminator(ir, block, T1_IR_JUMP, NULL, 0, 0, T1_NO_POINT, T1_NO_INSN))
        return false;
    ir->block[block].nsucc = 1;
    return add_edge(ir, block, 0, target);
}

bool t1_ir_set_branch(struct t1_ir *ir, uint16_t block, t1_value cond, uint16_t t, uint16_t f,
                      uint16_t insn)
{
    if (!set_terminator(ir, block, T1_IR_BRANCH, &cond, 1, 0, T1_NO_POINT, insn))
        return false;
    ir->block[block].nsucc = 2;
    return add_edge(ir, block, 0, t) && add_edge(ir, block, 1, f);
}

bool t1_ir_set_backedge(struct t1_ir *ir, uint16_t block, uint16_t header, uint32_t point,
                        uint16_t insn)
{
    if (!set_terminator(ir, block, T1_IR_BACKEDGE, NULL, 0, 0, point, insn))
        return false;
    ir->block[block].nsucc = 1;
    return add_edge(ir, block, 0, header);
}

bool t1_ir_set_exit(struct t1_ir *ir, uint16_t block, uint32_t point, uint16_t insn)
{
    ir->block[block].nsucc = 0;
    return set_terminator(ir, block, T1_IR_EXIT, NULL, 0, (int64_t)point, point, insn);
}

bool t1_ir_edge_add_arg(struct t1_ir *ir, uint16_t block, unsigned succ, t1_value v,
                        uint16_t element)
{
    struct t1_edge *e = &ir->block[block].succ[succ];
    if (ir->failed || !grow(ir, &e->args_first, e->nargs, &e->cap))
        return false;
    ir->use[e->args_first + e->nargs++] = (struct t1_use){v, element, 0};
    return true;
}

uint32_t t1_ir_new_point(struct t1_ir *ir, enum t1_point_kind kind, uint64_t ip, unsigned slot,
                         uint16_t insn, uint16_t block, uint16_t retired_offset)
{
    if (ir->failed)
        return T1_NO_POINT;
    if (ir->npoints >= T1_MAX_POINTS) {
        t1_ir_fail(ir, "point limit");
        return T1_NO_POINT;
    }
    uint32_t p = ir->npoints++;
    ir->point[p] = (struct t1_ir_point){.kind = (uint8_t)kind,
                                        .slot = (uint8_t)slot,
                                        .insn = insn,
                                        .block = block,
                                        .retired_offset = retired_offset,
                                        .op = T1_NO_OP,
                                        .ip = ip};
    return p;
}

bool t1_ir_set_frame(struct t1_ir *ir, uint32_t point, const uint16_t *element,
                     const t1_value *value, const uint16_t *flags, unsigned n)
{
    uint32_t first = reserve_uses(ir, n);
    if (first == UINT32_MAX)
        return false;
    for (unsigned i = 0; i < n; ++i)
        ir->use[first + i] = (struct t1_use){value[i], element[i], flags ? flags[i] : 0};
    ir->point[point].frame_first = first;
    ir->point[point].frame_count = n;
    return true;
}

uint8_t t1_ir_value_type(const struct t1_ir *ir, t1_value v)
{
    const struct t1_op *o = &ir->op[T1_VALUE_OP(v)];
    uint8_t t = op_desc[o->code].res_type[T1_VALUE_RESULT(v)];
    return t == T1_TYPE_T ? o->type : t;
}

unsigned t1_ir_nres(const struct t1_ir *ir, uint32_t op)
{
    return op_desc[ir->op[op].code].nres;
}

/* ---- CFG queries ---- */

static unsigned postorder(const struct t1_ir *ir, uint16_t *order, uint16_t *index)
{
    uint8_t state[T1_MAX_BLOCKS] = {0}; /* 0 new, 1 on stack, 2 done */
    uint16_t stack[T1_MAX_BLOCKS], next[T1_MAX_BLOCKS];
    unsigned sp = 0, n = 0;
    stack[sp] = T1_ENTRY_BLOCK;
    next[sp++] = 0;
    state[T1_ENTRY_BLOCK] = 1;
    while (sp) {
        uint16_t b = stack[sp - 1];
        const struct t1_block *blk = &ir->block[b];
        if (next[sp - 1] < blk->nsucc) {
            uint16_t s = blk->succ[next[sp - 1]++].target;
            if (!state[s] && !(ir->block[s].flags & T1_BLK_DEAD)) {
                state[s] = 1;
                stack[sp] = s;
                next[sp++] = 0;
            }
            continue;
        }
        state[b] = 2;
        index[b] = (uint16_t)n;
        order[n++] = b;
        --sp;
    }
    return n;
}

bool t1_ir_dominators(const struct t1_ir *ir, uint16_t idom[T1_MAX_BLOCKS])
{
    uint16_t order[T1_MAX_BLOCKS], index[T1_MAX_BLOCKS];
    for (unsigned b = 0; b < T1_MAX_BLOCKS; ++b) {
        idom[b] = T1_NO_BLOCK;
        index[b] = 0;
    }
    if (!ir->nblocks)
        return false;
    unsigned n = postorder(ir, order, index);
    idom[T1_ENTRY_BLOCK] = T1_ENTRY_BLOCK;
    /* Cooper, Harvey, Kennedy: "A Simple, Fast Dominance Algorithm". */
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned k = n; k-- > 0;) {
            uint16_t b = order[k];
            if (b == T1_ENTRY_BLOCK)
                continue;
            const struct t1_block *blk = &ir->block[b];
            uint16_t nd = T1_NO_BLOCK;
            for (unsigned p = 0; p < blk->npred; ++p) {
                uint16_t q = blk->pred[p];
                if (idom[q] == T1_NO_BLOCK)
                    continue;
                if (nd == T1_NO_BLOCK) {
                    nd = q;
                    continue;
                }
                uint16_t x = q, y = nd;
                while (x != y) {
                    while (index[x] < index[y])
                        x = idom[x];
                    while (index[y] < index[x])
                        y = idom[y];
                }
                nd = x;
            }
            if (nd != T1_NO_BLOCK && idom[b] != nd) {
                idom[b] = nd;
                changed = true;
            }
        }
    }
    return true;
}

bool t1_ir_dominates(const uint16_t idom[T1_MAX_BLOCKS], uint16_t a, uint16_t b)
{
    if (idom[a] == T1_NO_BLOCK || idom[b] == T1_NO_BLOCK)
        return false;
    for (;;) {
        if (a == b)
            return true;
        if (b == T1_ENTRY_BLOCK)
            return false;
        b = idom[b];
    }
}

static unsigned longest(const struct t1_ir *ir, uint16_t b, int32_t *memo)
{
    if (memo[b] >= 0)
        return (unsigned)memo[b];
    const struct t1_block *blk = &ir->block[b];
    unsigned best = 0;
    uint32_t t = t1_ir_terminator(ir, b);
    if (t != T1_NO_OP && ir->op[t].code != T1_IR_BACKEDGE)
        for (unsigned s = 0; s < blk->nsucc; ++s) {
            unsigned l = longest(ir, blk->succ[s].target, memo);
            if (l > best)
                best = l;
        }
    memo[b] = (int32_t)(best + blk->charge);
    return (unsigned)memo[b];
}

unsigned t1_ir_max_path(const struct t1_ir *ir, uint16_t header)
{
    int32_t memo[T1_MAX_BLOCKS];
    for (unsigned b = 0; b < T1_MAX_BLOCKS; ++b)
        memo[b] = -1;
    return header < ir->nblocks ? longest(ir, header, memo) : 0;
}

_Static_assert(T1_MAX_BLOCKS <= 64, "loop bodies are block bitmasks");

void t1_ir_loops(const struct t1_ir *ir, struct t1_ir_loops *out)
{
    for (unsigned b = 0; b < T1_MAX_BLOCKS; ++b) {
        out->body[b] = 0;
        out->header[b] = T1_NO_BLOCK;
        out->depth[b] = 0;
    }
    /* The natural loop of each back-edge s -> h: h and every block that
     * reaches s without passing h (over predecessor lists). */
    for (unsigned s = 0; s < ir->nblocks; ++s) {
        uint32_t t =
            (ir->block[s].flags & T1_BLK_DEAD) ? T1_NO_OP : t1_ir_terminator(ir, (uint16_t)s);
        if (t == T1_NO_OP || ir->op[t].code != T1_IR_BACKEDGE)
            continue;
        uint16_t h = ir->block[s].succ[0].target;
        uint64_t body = out->body[h] | (UINT64_C(1) << h);
        uint16_t stack[T1_MAX_BLOCKS];
        unsigned n = 0;
        if (!(body & (UINT64_C(1) << s))) {
            body |= UINT64_C(1) << s;
            stack[n++] = (uint16_t)s;
        }
        while (n) {
            const struct t1_block *blk = &ir->block[stack[--n]];
            for (unsigned p = 0; p < blk->npred; ++p) {
                uint16_t q = blk->pred[p];
                if (body & (UINT64_C(1) << q))
                    continue;
                body |= UINT64_C(1) << q;
                stack[n++] = q;
            }
        }
        out->body[h] = body;
    }
    /* The loops holding a block are nested: its innermost loop is the one
     * whose body the others contain. */
    for (unsigned h = 0; h < ir->nblocks; ++h) {
        if (!out->body[h])
            continue;
        for (unsigned b = 0; b < ir->nblocks; ++b) {
            if (!(out->body[h] & (UINT64_C(1) << b)))
                continue;
            ++out->depth[b];
            uint16_t cur = out->header[b];
            if (cur == T1_NO_BLOCK || (out->body[h] & out->body[cur]) == out->body[h])
                out->header[b] = (uint16_t)h;
        }
    }
}

/* ---- rewriting ---- */

t1_value t1_ir_resolve(const struct t1_ir *ir, t1_value v)
{
    for (unsigned guard = 0; v != T1_VALUE_NONE && guard < T1_IR_MAX_OPS; ++guard) {
        const struct t1_op *o = &ir->op[T1_VALUE_OP(v)];
        if (o->fwd == T1_VALUE_NONE || T1_VALUE_RESULT(v) != 0)
            return v;
        v = o->fwd;
    }
    return v;
}

void t1_ir_forward(struct t1_ir *ir, uint32_t op, t1_value to)
{
    to = t1_ir_resolve(ir, to);
    if (to != T1_VALUE(op, 0))
        ir->op[op].fwd = to;
}

static void resolve_range(struct t1_ir *ir, uint32_t first, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        t1_value v = ir->use[first + i].value;
        if (v == T1_VALUE_NONE)
            continue;
        t1_value to = ir->op[T1_VALUE_OP(v)].fwd;
        if (to == T1_VALUE_NONE || T1_VALUE_RESULT(v) != 0)
            continue;
        to = t1_ir_resolve(ir, to);
        ir->use[first + i].value = to;
        /* Later uses can skip the same forwarding chain. A nonzero result
         * terminates resolution, so its producer's forwarding is untouched. */
        while (v != to) {
            struct t1_op *op = &ir->op[T1_VALUE_OP(v)];
            t1_value next = op->fwd;
            if (next == to)
                break;
            op->fwd = to;
            v = next;
        }
    }
}

void t1_ir_resolve_all(struct t1_ir *ir)
{
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        struct t1_block *blk = &ir->block[b];
        if (blk->flags & T1_BLK_DEAD)
            continue;
        for (uint32_t o = blk->first; o != T1_NO_OP; o = ir->op[o].next)
            resolve_range(ir, ir->op[o].use_first, ir->op[o].nuse);
        for (unsigned s = 0; s < blk->nsucc; ++s)
            resolve_range(ir, blk->succ[s].args_first, blk->succ[s].nargs);
    }
    for (unsigned p = 0; p < ir->npoints; ++p)
        if (!ir->point[p].dead)
            resolve_range(ir, ir->point[p].frame_first, ir->point[p].frame_count);
}

void t1_ir_remove(struct t1_ir *ir, uint32_t op)
{
    struct t1_op *o = &ir->op[op];
    if (o->flags & T1_OPF_DEAD)
        return;
    o->flags |= T1_OPF_DEAD;
    if (o->code == T1_IR_PARAM)
        return; /* unlinked by t1_ir_compact_params */
    struct t1_block *blk = &ir->block[o->block];
    if (o->prev == T1_NO_OP)
        blk->first = o->next;
    else
        ir->op[o->prev].next = o->next;
    if (o->next == T1_NO_OP)
        blk->last = o->prev;
    else
        ir->op[o->next].prev = o->prev;
    o->prev = o->next = T1_NO_OP;
    if (o->point != T1_NO_POINT)
        ir->point[o->point].dead = 1;
}

static void unlink_param(struct t1_ir *ir, uint32_t op)
{
    struct t1_op *o = &ir->op[op];
    struct t1_block *blk = &ir->block[o->block];
    if (blk->param_end == op + 1)
        blk->param_end = o->prev == T1_NO_OP ? 0 : (uint16_t)(o->prev + 1);
    if (o->prev == T1_NO_OP)
        blk->first = o->next;
    else
        ir->op[o->prev].next = o->next;
    if (o->next == T1_NO_OP)
        blk->last = o->prev;
    else
        ir->op[o->next].prev = o->prev;
    o->prev = o->next = T1_NO_OP;
    o->flags |= T1_OPF_DEAD;
}

/* The edge of predecessor index p of block b. */
static struct t1_edge *in_edge(struct t1_ir *ir, uint16_t b, unsigned p)
{
    return &ir->block[ir->block[b].pred[p]].succ[ir->block[b].pred_slot[p]];
}

void t1_ir_compact_params(struct t1_ir *ir)
{
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        struct t1_block *blk = &ir->block[b];
        if (blk->flags & T1_BLK_DEAD)
            continue;
        unsigned pos = 0, keep = 0;
        for (uint32_t o = blk->first, next; o != T1_NO_OP && ir->op[o].code == T1_IR_PARAM;
             o = next) {
            next = ir->op[o].next;
            bool dead = (ir->op[o].flags & T1_OPF_DEAD) || ir->op[o].fwd != T1_VALUE_NONE;
            if (!dead) {
                for (unsigned p = 0; p < blk->npred; ++p) {
                    struct t1_edge *e = in_edge(ir, (uint16_t)b, p);
                    if (pos < e->nargs)
                        ir->use[e->args_first + keep] = ir->use[e->args_first + pos];
                }
                ++keep;
            } else {
                unlink_param(ir, o);
            }
            ++pos;
        }
        for (unsigned p = 0; p < blk->npred; ++p) {
            struct t1_edge *e = in_edge(ir, (uint16_t)b, p);
            if (e->nargs > keep)
                e->nargs = (uint16_t)keep;
        }
    }
}

static void remove_pred(struct t1_ir *ir, uint16_t target, uint16_t from, unsigned slot)
{
    struct t1_block *tb = &ir->block[target];
    for (unsigned p = 0; p < tb->npred; ++p)
        if (tb->pred[p] == from && tb->pred_slot[p] == slot) {
            for (unsigned q = p + 1; q < tb->npred; ++q) {
                tb->pred[q - 1] = tb->pred[q];
                tb->pred_slot[q - 1] = tb->pred_slot[q];
            }
            --tb->npred;
            return;
        }
}

void t1_ir_kill_block(struct t1_ir *ir, uint16_t b)
{
    struct t1_block *blk = &ir->block[b];
    if (blk->flags & T1_BLK_DEAD)
        return;
    for (unsigned s = 0; s < blk->nsucc; ++s)
        remove_pred(ir, blk->succ[s].target, b, s);
    blk->nsucc = 0;
    for (uint32_t o = blk->first, next; o != T1_NO_OP; o = next) {
        next = ir->op[o].next;
        ir->op[o].flags |= T1_OPF_DEAD;
        ir->op[o].prev = ir->op[o].next = T1_NO_OP;
        if (ir->op[o].point != T1_NO_POINT)
            ir->point[ir->op[o].point].dead = 1;
    }
    blk->first = blk->last = T1_NO_OP;
    blk->param_end = 0;
    blk->flags |= T1_BLK_DEAD;
}

void t1_ir_branch_to_jump(struct t1_ir *ir, uint16_t b, unsigned keep)
{
    struct t1_block *blk = &ir->block[b];
    uint32_t t = t1_ir_terminator(ir, b);
    if (t == T1_NO_OP || ir->op[t].code != T1_IR_BRANCH)
        return;
    remove_pred(ir, blk->succ[1 - keep].target, b, 1 - keep);
    if (keep == 1) {
        struct t1_block *tb = &ir->block[blk->succ[1].target];
        for (unsigned p = 0; p < tb->npred; ++p)
            if (tb->pred[p] == b && tb->pred_slot[p] == 1)
                tb->pred_slot[p] = 0;
        blk->succ[0] = blk->succ[1];
    }
    blk->nsucc = 1;
    ir->op[t].code = T1_IR_JUMP;
    ir->op[t].nuse = 0;
}

void t1_ir_remove_unreachable(struct t1_ir *ir)
{
    uint16_t order[T1_MAX_BLOCKS], index[T1_MAX_BLOCKS];
    bool live[T1_MAX_BLOCKS] = {false};
    unsigned n = postorder(ir, order, index);
    for (unsigned i = 0; i < n; ++i)
        live[order[i]] = true;
    for (unsigned b = 0; b < ir->nblocks; ++b)
        if (!live[b])
            t1_ir_kill_block(ir, (uint16_t)b);
}

/* ---- printing ---- */

static const char *type_name(uint8_t t)
{
    return t == T1_I64 ? "i64" : t == T1_I1 ? "i1" : t == T1_MEM ? "mem" : "?";
}

static void print_value(char *buf, size_t size, const struct t1_ir *ir, t1_value v)
{
    if (v == T1_VALUE_NONE) {
        vmm_snprintf(buf, size, "-");
        return;
    }
    const struct t1_op *o = &ir->op[T1_VALUE_OP(v)];
    if (o->code == T1_IR_CONST)
        vmm_snprintf(buf, size, "#%llx", (unsigned long long)o->imm);
    else if (T1_VALUE_RESULT(v))
        vmm_snprintf(buf, size, "%%%u.%s", (unsigned)T1_VALUE_OP(v),
                     op_desc[o->code].res_name[T1_VALUE_RESULT(v)]);
    else
        vmm_snprintf(buf, size, "%%%u", (unsigned)T1_VALUE_OP(v));
}

static void append(char *line, size_t size, const char *text)
{
    size_t n = strlen(line), m = strlen(text);
    if (n + m >= size)
        m = size - n - 1;
    memcpy(line + n, text, m);
    line[n + m] = 0;
}

static void print_uses(char *line, size_t size, const struct t1_ir *ir, uint32_t first, unsigned n,
                       bool elements)
{
    char v[48], e[24];
    for (unsigned i = 0; i < n; ++i) {
        const struct t1_use *u = &ir->use[first + i];
        if (u->flags & T1_USE_HOME)
            vmm_snprintf(v, sizeof(v), "home");
        else
            print_value(v, sizeof(v), ir, u->value);
        if (elements && u->element) {
            vmm_snprintf(e, sizeof(e), "e%u=", u->element);
            append(line, size, i ? " " : "");
            append(line, size, e);
        } else {
            append(line, size, i ? ", " : "");
        }
        append(line, size, v);
    }
}

void t1_ir_print(const struct t1_ir *ir, t1_ir_out_fn out, void *arg)
{
    char line[1024], tmp[96];
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        const struct t1_block *blk = &ir->block[b];
        if (blk->flags & T1_BLK_DEAD)
            continue;
        vmm_snprintf(line, sizeof(line), "b%u: charge %u%s preds", b, blk->charge,
                     blk->flags & T1_BLK_SYNTHETIC ? " synthetic" : "");
        for (unsigned p = 0; p < blk->npred; ++p) {
            vmm_snprintf(tmp, sizeof(tmp), " b%u", blk->pred[p]);
            append(line, sizeof(line), tmp);
        }
        out(arg, line);
        for (uint32_t o = blk->first; o != T1_NO_OP; o = ir->op[o].next) {
            const struct t1_op *op = &ir->op[o];
            const struct t1_op_desc *d = &op_desc[op->code];
            if (d->nres)
                vmm_snprintf(line, sizeof(line), "  %%%u:%s = %s", o,
                             type_name(t1_ir_value_type(ir, T1_VALUE(o, 0))), d->name);
            else
                vmm_snprintf(line, sizeof(line), "  %s", d->name);
            if (d->imm != T1_IMM_NONE) {
                vmm_snprintf(tmp, sizeof(tmp), " [%llx]", (unsigned long long)op->imm);
                append(line, sizeof(line), tmp);
            }
            append(line, sizeof(line), " ");
            print_uses(line, sizeof(line), ir, op->use_first, op->nuse, op->code == T1_IR_HELPER);
            if (d->effect == T1_EFF_TERM)
                for (unsigned s = 0; s < blk->nsucc; ++s) {
                    vmm_snprintf(tmp, sizeof(tmp), " -> b%u(", blk->succ[s].target);
                    append(line, sizeof(line), tmp);
                    print_uses(line, sizeof(line), ir, blk->succ[s].args_first, blk->succ[s].nargs,
                               false);
                    append(line, sizeof(line), ")");
                }
            if (op->point != T1_NO_POINT) {
                const struct t1_ir_point *p = &ir->point[op->point];
                vmm_snprintf(tmp, sizeof(tmp), " @P%u(k%u ip=%llx.%u off=%u) {", op->point, p->kind,
                             (unsigned long long)p->ip, p->slot, p->retired_offset);
                append(line, sizeof(line), tmp);
                print_uses(line, sizeof(line), ir, p->frame_first, p->frame_count, true);
                append(line, sizeof(line), "}");
            }
            out(arg, line);
        }
    }
}
