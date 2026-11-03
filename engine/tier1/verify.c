/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * IR checker t1_ir_verify():
 * block structure and edges, operand counts and types, SSA dominance per
 * result (operands, edge arguments, frame entries), point kinds, the E_MEM
 * chain, frame completeness, the must-analysis for every home frame
 * entry, and (after natguard) the NaT folding invariant.
 */
#include "engine/tier1/ir.h"
#include "engine/tier1/helpers.h"
#include "engine/tier1/form.h"
#include "ia64vmm/hal.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define HOME_ENTRY ((t1_value)0xfffffffeu) /* home holds LIVEIN(e) */
#define HOME_UNKNOWN ((t1_value)0xfffffffdu)

struct vctx {
    const struct t1_ir *ir;
    char *err;
    size_t size;
    uint16_t idom[T1_MAX_BLOCKS];
    uint64_t dominators[T1_MAX_BLOCKS];
    uint64_t cached_dominators;
    uint32_t pos[T1_IR_MAX_OPS];      /* position within its block */
    uint32_t mem_user[T1_IR_MAX_OPS]; /* first memory op consuming (op, mem result) */
};

_Static_assert(T1_MAX_BLOCKS <= 64, "dominance masks fit one word");

static uint64_t dominance_mask(struct vctx *c, uint16_t b)
{
    uint64_t bit = UINT64_C(1) << b;
    if (!(c->cached_dominators & bit)) {
        /* The dominator routine initializes unused/unreachable entries
         * to NONE; each other parent chain is rooted at ENTRY. Numbering
         * need not put parents before their children. */
        uint64_t mask = UINT64_C(1) << T1_ENTRY_BLOCK;
        for (uint16_t q = b; q != T1_ENTRY_BLOCK; q = c->idom[q])
            mask |= UINT64_C(1) << q;
        c->dominators[b] = mask;
        c->cached_dominators |= bit;
    }
    return c->dominators[b];
}

static bool dominates(struct vctx *c, uint16_t a, uint16_t b)
{
    if (a >= T1_MAX_BLOCKS || b >= T1_MAX_BLOCKS || c->idom[b] == T1_NO_BLOCK)
        return false;
    if (a == T1_ENTRY_BLOCK)
        return true;
    if (c->idom[a] == T1_NO_BLOCK)
        return false;
    if (a == b || c->idom[b] == a)
        return true;
    return (dominance_mask(c, b) >> a) & 1;
}

static bool fail(struct vctx *c, const char *fmt, unsigned a, unsigned b)
{
    vmm_snprintf(c->err, c->size, fmt, a, b);
    return false;
}

static uint8_t element_type(unsigned e)
{
    if (e == T1_ELEMENT_MEM)
        return T1_MEM;
    return e >= 128 && e < T1_ELEMENT_LC ? T1_I1 : T1_I64;
}
static bool is_nat_element(unsigned e)
{
    return e >= 128 && e < 256;
}

static bool live_op(const struct t1_ir *ir, uint32_t o)
{
    return o < ir->nops && !(ir->op[o].flags & T1_OPF_DEAD) && ir->op[o].fwd == T1_VALUE_NONE;
}

/* v is available at the end of block b, or before op `at` (in b) if at != NO_OP. */
static bool available(struct vctx *c, t1_value v, uint16_t b, uint32_t at)
{
    const struct t1_ir *ir = c->ir;
    uint32_t d = T1_VALUE_OP(v);
    if (v == T1_VALUE_NONE || !live_op(ir, d))
        return false;
    if (T1_VALUE_RESULT(v) >= t1_ir_nres(ir, d))
        return false;
    uint16_t db = ir->op[d].block;
    if (db == b)
        return at == T1_NO_OP || c->pos[d] < c->pos[at];
    return dominates(c, db, b);
}

static unsigned nparams(const struct t1_ir *ir, uint16_t b)
{
    unsigned n = 0;
    for (uint32_t o = ir->block[b].first; o != T1_NO_OP && ir->op[o].code == T1_IR_PARAM;
         o = ir->op[o].next)
        ++n;
    return n;
}

static bool check_cfg_bounds(struct vctx *c)
{
    const struct t1_ir *ir = c->ir;
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        const struct t1_block *blk = &ir->block[b];
        if (blk->flags & T1_BLK_DEAD)
            continue;
        if (blk->nsucc > IA64VMM_ARRAY_SIZE(blk->succ))
            return fail(c, "b%u: successor count", b, 0);
        if (blk->npred > IA64VMM_ARRAY_SIZE(blk->pred))
            return fail(c, "b%u: predecessor count", b, 0);
        for (unsigned s = 0; s < blk->nsucc; ++s)
            if (blk->succ[s].target >= ir->nblocks)
                return fail(c, "b%u: edge target %u", b, blk->succ[s].target);
        for (unsigned p = 0; p < blk->npred; ++p)
            if (blk->pred[p] >= ir->nblocks)
                return fail(c, "b%u: predecessor block %u", b, blk->pred[p]);
    }
    return true;
}

static bool check_edges(struct vctx *c, uint16_t b)
{
    const struct t1_ir *ir = c->ir;
    const struct t1_block *blk = &ir->block[b];
    for (unsigned s = 0; s < blk->nsucc; ++s) {
        const struct t1_edge *e = &blk->succ[s];
        const struct t1_block *tb = &ir->block[e->target];
        if (e->target >= ir->nblocks || (tb->flags & T1_BLK_DEAD))
            return fail(c, "b%u: edge %u to a dead block", b, s);
        unsigned seen = 0;
        for (unsigned p = 0; p < tb->npred; ++p)
            seen += tb->pred[p] == b && tb->pred_slot[p] == s;
        if (seen != 1)
            return fail(c, "b%u: edge %u not in the target's predecessors", b, s);
        if (e->nargs != nparams(ir, e->target))
            return fail(c, "b%u: edge to b%u: argument count", b, e->target);
        unsigned k = 0;
        for (uint32_t o = tb->first; o != T1_NO_OP && ir->op[o].code == T1_IR_PARAM;
             o = ir->op[o].next, ++k) {
            const struct t1_use *u = &ir->use[e->args_first + k];
            if (!available(c, u->value, b, T1_NO_OP))
                return fail(c, "b%u: edge argument %u does not dominate", b, k);
            if (t1_ir_value_type(ir, u->value) != ir->op[o].type)
                return fail(c, "b%u: edge argument %u type", b, k);
        }
    }
    for (unsigned p = 0; p < blk->npred; ++p) {
        const struct t1_block *pb = &ir->block[blk->pred[p]];
        if ((pb->flags & T1_BLK_DEAD) || blk->pred_slot[p] >= pb->nsucc ||
            pb->succ[blk->pred_slot[p]].target != b)
            return fail(c, "b%u: stale predecessor b%u", b, blk->pred[p]);
    }
    return true;
}

static bool check_operands(struct vctx *c, uint16_t b, uint32_t o)
{
    const struct t1_ir *ir = c->ir;
    const struct t1_op *op = &ir->op[o];
    const struct t1_op_desc *d = t1_op_desc(op->code);
    unsigned fixed = d->nopnd;
    bool var = fixed && d->opnd_type[fixed - 1] == T1_TYPE_VAR;
    if (var)
        --fixed;
    /* Terminators keep their VAR arguments on the edges. */
    if (d->effect == T1_EFF_TERM)
        var = false;
    if (op->nuse < fixed || (!var && op->nuse != fixed))
        return fail(c, "op %u (%u): operand count", o, op->code);
    if (op->code == T1_IR_GHELPER &&
        (!ir->form || op->imm < 0 || (uint64_t)op->imm >= ir->form->ninsns ||
         (!t1_helper_row(ir->form->insn[op->imm].insn.opcode) &&
          ir->form->insn[op->imm].cls != T1_IC_ALLOC)))
        return fail(c, "op %u: generalized helper instruction", o, 0);
    if (op->code == T1_IR_HREAD) {
        if (op->imm <= 0 || (op->imm == T1_ELEMENT_MEM || op->imm >= T1_N_ELEMENTS) ||
            op->type != element_type(op->imm))
            return fail(c, "op %u: helper snapshot element", o, 0);
        t1_value mv = ir->use[op->use_first].value;
        if (!live_op(ir, T1_VALUE_OP(mv)) || ir->op[T1_VALUE_OP(mv)].code != T1_IR_GHELPER)
            return fail(c, "op %u: helper snapshot producer", o, 0);
        const struct t1_op *producer = &ir->op[T1_VALUE_OP(mv)];
        if (!ir->form || producer->imm < 0 || (uint64_t)producer->imm >= ir->form->ninsns)
            return fail(c, "op %u: helper snapshot instruction", o, 0);
        uint16_t writes[T1_N_ELEMENTS];
        unsigned n = t1_helper_elements(&ir->form->insn[producer->imm], true, writes), k = 0;
        while (k < n && writes[k] != op->imm)
            ++k;
        if (k == n)
            return fail(c, "op %u: undeclared helper snapshot", o, 0);
    }
    for (unsigned i = 0; i < op->nuse; ++i) {
        const struct t1_use *u = &ir->use[op->use_first + i];
        if (!available(c, u->value, b, o))
            return fail(c, "op %u: operand %u does not dominate", o, i);
        uint8_t want = i < fixed ? d->opnd_type[i] : element_type(u->element);
        if (want == T1_TYPE_T)
            want = op->type;
        if (t1_ir_value_type(ir, u->value) != want)
            return fail(c, "op %u: operand %u type", o, i);
        if (i >= fixed &&
            (!u->element || (u->element == T1_ELEMENT_MEM || u->element >= T1_N_ELEMENTS)))
            return fail(c, "op %u: read-set entry %u has no element", o, i);
        if (want == T1_MEM && op->code != T1_IR_PARAM && op->code != T1_IR_HREAD) {
            uint32_t m = T1_VALUE_OP(u->value);
            if (c->mem_user[m] != T1_NO_OP) {
                uint16_t ob = ir->op[c->mem_user[m]].block;
                if (ob == b || dominates(c, ob, b) || dominates(c, b, ob))
                    return fail(c, "op %u: memory chain forks at op %u", o, m);
            }
            c->mem_user[m] = o;
        }
    }
    if (op->code == T1_IR_GHELPER) {
        uint16_t needed[T1_N_ELEMENTS];
        unsigned n = t1_helper_elements(&ir->form->insn[op->imm], false, needed);
        for (unsigned k = 0; k < n; ++k) {
            unsigned j = 1;
            while (j < op->nuse && ir->use[op->use_first + j].element != needed[k])
                ++j;
            if (j == op->nuse)
                return fail(c, "op %u: missing helper input %u", o, needed[k]);
        }
    }
    return true;
}

static bool check_point_op(struct vctx *c, uint16_t b, uint32_t o)
{
    const struct t1_ir *ir = c->ir;
    const struct t1_op *op = &ir->op[o];
    int want = op->code == T1_IR_LOAD || op->code == T1_IR_STORE         ? T1_PT_SITE
               : (op->code == T1_IR_HELPER || op->code == T1_IR_GHELPER) ? T1_PT_HELPER
               : (op->code == T1_IR_EXIT || op->code == T1_IR_HGUARD)    ? T1_PT_EXIT
               : op->code == T1_IR_BACKEDGE                              ? T1_PT_CHECKPOINT
                                                                         : -1;
    if (want < 0)
        return op->point == T1_NO_POINT ? true : fail(c, "op %u: unexpected point", o, 0);
    if (op->point >= ir->npoints)
        return fail(c, "op %u: missing point", o, 0);
    const struct t1_ir_point *p = &ir->point[op->point];
    if (p->dead || p->op != o || p->kind != want || p->block != b)
        return fail(c, "op %u: point %u inconsistent", o, op->point);
    if (op->code == T1_IR_EXIT && (uint32_t)op->imm != op->point)
        return fail(c, "op %u: EXIT immediate", o, 0);
    if (want == T1_PT_SITE &&
        (p->access_size != T1_ACCESS_BYTES(op->imm) ||
         p->access_write != (op->code == T1_IR_STORE) || p->site != T1_ACCESS_SITE(op->imm)))
        return fail(c, "op %u: site point %u access", o, op->point);
    if (p->retired_offset > ir->block[b].charge)
        return fail(c, "point %u: retired offset", op->point, 0);
    return true;
}

static bool check_block(struct vctx *c, uint16_t b, unsigned verify_flags)
{
    const struct t1_ir *ir = c->ir;
    const struct t1_block *blk = &ir->block[b];
    uint32_t prev = T1_NO_OP;
    bool params = true;
    for (uint32_t o = blk->first; o != T1_NO_OP; prev = o, o = ir->op[o].next) {
        const struct t1_op *op = &ir->op[o];
        const struct t1_op_desc *d = t1_op_desc(op->code);
        if (!d)
            return fail(c, "op %u: bad code %u", o, op->code);
        if (op->prev != prev || op->block != b || !live_op(ir, o))
            return fail(c, "op %u: not a live op of b%u", o, b);
        if (op->code == T1_IR_PARAM) {
            if (!params)
                return fail(c, "op %u: PARAM after other ops in b%u", o, b);
            if (op->imm && element_type((unsigned)op->imm) != op->type)
                return fail(c, "op %u: PARAM type", o, 0);
        } else {
            params = false;
        }
        if ((op->code == T1_IR_CONST || op->code == T1_IR_LIVEIN || op->code == T1_IR_NATIN) !=
                (b == T1_ENTRY_BLOCK) &&
            !(b == T1_ENTRY_BLOCK && op->code == T1_IR_JUMP))
            return fail(c, "op %u: only CONST and LIVEIN in the entry block (b%u)", o, b);
        if (op->code == T1_IR_LIVEIN) {
            if (!op->imm || op->imm >= T1_N_ELEMENTS || element_type((unsigned)op->imm) != op->type)
                return fail(c, "op %u: LIVEIN element", o, 0);
            if ((verify_flags & T1_VERIFY_NAT_FOLDED) && is_nat_element((unsigned)op->imm))
                return fail(c, "op %u: LIVEIN of a NaT after folding", o, 0);
        }
        if (op->code == T1_IR_NATIN && (!is_nat_element(op->imm) || op->type != T1_I1))
            return fail(c, "op %u: exact entry NaT element", o, 0);
        if (op->code == T1_IR_CONST && op->type == T1_I1 && (uint64_t)op->imm > 1)
            return fail(c, "op %u: I1 constant", o, 0);
        if (op->code == T1_IR_NATCHK && (verify_flags & T1_VERIFY_NAT_FOLDED))
            return fail(c, "op %u: NATCHK survived folding", o, 0);
        if ((d->effect == T1_EFF_TERM) != (o == blk->last))
            return fail(c, "op %u: terminator position in b%u", o, b);
        if (!check_operands(c, b, o) || !check_point_op(c, b, o))
            return false;
        if (op->code == T1_IR_HELPER) {
            for (unsigned k = 0; k < 2; ++k)
                if (op->wr[k] && (op->wr[k] < 257 || op->wr[k] >= T1_ELEMENT_LC))
                    return fail(c, "op %u: helper write set", o, 0);
            /* Helpers may create NaT (getf, ld.s, ld8.fill). Entry
             * LIVEIN NaTs are still folded under G, and every native NaT
             * consumption must still disappear or end formation. */
        }
    }
    if (blk->last != prev)
        return fail(c, "b%u: list tail", b, 0);
    uint32_t t = t1_ir_terminator(ir, b);
    if (t == T1_NO_OP)
        return fail(c, "b%u: no terminator", b, 0);
    unsigned code = ir->op[t].code;
    unsigned want = code == T1_IR_BRANCH ? 2 : code == T1_IR_EXIT ? 0 : 1;
    if (blk->nsucc != want)
        return fail(c, "b%u: successor count", b, 0);
    if (code == T1_IR_BACKEDGE && !dominates(c, blk->succ[0].target, b))
        return fail(c, "b%u: back-edge target b%u does not dominate it", b, blk->succ[0].target);
    if (b == T1_ENTRY_BLOCK && (code != T1_IR_JUMP || blk->succ[0].target != T1_HEADER_BLOCK))
        return fail(c, "entry block must jump to the header", 0, 0);
    return check_edges(c, b);
}

/*
 * Must-analysis for one element e: the value the home of e holds at
 * every program point (HOME_ENTRY = its entry value LIVEIN(e)), changed
 * only by HELPER read-set write-backs and write sets; merges keep equal
 * values, otherwise HOME_UNKNOWN; loop edges iterate to a fixed point.
 * Returns false if some home frame entry of e is not proven HOME_ENTRY.
 */
static t1_value home_after(const struct t1_ir *ir, uint32_t o, uint16_t e, t1_value h)
{
    const struct t1_op *op = &ir->op[o];
    if (op->code != T1_IR_HELPER && op->code != T1_IR_GHELPER)
        return h;
    if (op->code == T1_IR_GHELPER) {
        uint16_t writes[T1_N_ELEMENTS];
        unsigned n = t1_helper_elements(&ir->form->insn[op->imm], true, writes);
        for (unsigned k = 0; k < n; ++k)
            if (writes[k] == e)
                return HOME_UNKNOWN;
    }
    for (unsigned i = 1; i < op->nuse; ++i) {
        const struct t1_use *u = &ir->use[op->use_first + i];
        if (u->element != e)
            continue;
        const struct t1_op *d = &ir->op[T1_VALUE_OP(u->value)];
        t1_value written = (d->code == T1_IR_LIVEIN || d->code == T1_IR_NATIN) && d->imm == e
                               ? HOME_ENTRY
                               : u->value;
        /* Generalized helpers may bypass write-back (alloc, inline FP).
         * Both the old home and the materialized value must agree. */
        h = op->code == T1_IR_GHELPER && h != written ? HOME_UNKNOWN : written;
    }
    for (unsigned k = 0; k < 2; ++k)
        if (op->wr[k] == e)
            h = T1_VALUE(o, k);
    return h;
}

static bool check_home(struct vctx *c, uint16_t e)
{
    const struct t1_ir *ir = c->ir;
    t1_value in[T1_MAX_BLOCKS], out[T1_MAX_BLOCKS];
    for (unsigned b = 0; b < T1_MAX_BLOCKS; ++b)
        in[b] = out[b] = T1_VALUE_NONE;
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = 0; b < ir->nblocks; ++b) {
            const struct t1_block *blk = &ir->block[b];
            if ((blk->flags & T1_BLK_DEAD) || c->idom[b] == T1_NO_BLOCK)
                continue;
            t1_value h = b == T1_ENTRY_BLOCK ? HOME_ENTRY : T1_VALUE_NONE;
            for (unsigned p = 0; p < blk->npred; ++p) {
                t1_value o = out[blk->pred[p]];
                if (o == T1_VALUE_NONE)
                    continue;
                h = h == T1_VALUE_NONE || h == o ? o : HOME_UNKNOWN;
            }
            in[b] = h;
            for (uint32_t o = blk->first; o != T1_NO_OP && h != T1_VALUE_NONE; o = ir->op[o].next)
                h = home_after(ir, o, e, h);
            if (h != out[b]) {
                out[b] = h;
                changed = true;
            }
        }
    }
    for (unsigned p = 0; p < ir->npoints; ++p) {
        const struct t1_ir_point *pt = &ir->point[p];
        if (pt->dead)
            continue;
        bool home = false;
        for (unsigned k = 0; k < pt->frame_count; ++k) {
            const struct t1_use *u = &ir->use[pt->frame_first + k];
            if (u->element == e && (u->flags & T1_USE_HOME))
                home = true;
        }
        if (!home)
            continue;
        t1_value h = in[pt->block];
        for (uint32_t o = ir->block[pt->block].first; o != pt->op && o != T1_NO_OP;
             o = ir->op[o].next)
            h = home_after(ir, o, e, h);
        if (h != HOME_ENTRY)
            return fail(c, "point %u: home of element %u is not proven", p, e);
    }
    return true;
}

static bool check_points(struct vctx *c, unsigned verify_flags)
{
    (void)verify_flags;
    const struct t1_ir *ir = c->ir;
    uint64_t need_home[T1_ELEMENT_WORDS] = {0};
    for (unsigned p = 0; p < ir->npoints; ++p) {
        const struct t1_ir_point *pt = &ir->point[p];
        if (pt->dead)
            continue;
        if (pt->op >= ir->nops || !live_op(ir, pt->op) || ir->op[pt->op].point != p)
            return fail(c, "point %u: op", p, 0);
        uint64_t seen[T1_ELEMENT_WORDS] = {0};
        for (unsigned k = 0; k < pt->frame_count; ++k) {
            const struct t1_use *u = &ir->use[pt->frame_first + k];
            if (!t1_elem_has(ir->frame_set, u->element) || t1_elem_has(seen, u->element))
                return fail(c, "point %u: frame element %u", p, u->element);
            t1_elem_add(seen, u->element);
            if (u->flags & T1_USE_HOME) {
                if (u->value != T1_VALUE_NONE)
                    return fail(c, "point %u: home entry %u has a value", p, k);
                if (t1_elem_has(ir->w_set, u->element))
                    t1_elem_add(need_home, u->element);
                continue;
            }
            if (!available(c, u->value, pt->block, pt->op))
                return fail(c, "point %u: frame value of element %u does not dominate", p,
                            u->element);
            if (t1_ir_value_type(ir, u->value) != element_type(u->element))
                return fail(c, "point %u: frame type of element %u", p, u->element);
        }
        for (unsigned w = 0; w < T1_ELEMENT_WORDS; ++w)
            if (seen[w] != ir->frame_set[w])
                return fail(c, "point %u: frame incomplete", p, 0);
    }
    for (unsigned w = 0; w < T1_ELEMENT_WORDS; ++w)
        for (uint64_t bits = need_home[w]; bits; bits &= bits - 1) {
            unsigned e = 64 * w + (unsigned)__builtin_ctzll(bits);
            if (e && e < T1_N_ELEMENTS && !check_home(c, (uint16_t)e))
                return false;
        }
    return true;
}

/* Every cycle runs through a BACKEDGE: the other edges are acyclic (the
 * loop forest; max_path follows them). */
static bool check_forward_acyclic(struct vctx *c)
{
    const struct t1_ir *ir = c->ir;
    uint8_t state[T1_MAX_BLOCKS] = {0}; /* 0 new, 1 on the path, 2 done */
    uint16_t stack[T1_MAX_BLOCKS];
    uint8_t next[T1_MAX_BLOCKS];
    for (unsigned root = 0; root < ir->nblocks; ++root) {
        if (state[root] || (ir->block[root].flags & T1_BLK_DEAD))
            continue;
        unsigned sp = 0;
        stack[sp] = (uint16_t)root;
        next[sp++] = 0;
        state[root] = 1;
        while (sp) {
            uint16_t b = stack[sp - 1];
            uint32_t t = t1_ir_terminator(ir, b);
            unsigned n = t != T1_NO_OP && ir->op[t].code == T1_IR_BACKEDGE ? 0 : ir->block[b].nsucc;
            if (next[sp - 1] < n) {
                uint16_t s = ir->block[b].succ[next[sp - 1]++].target;
                if (state[s] == 1)
                    return fail(c, "b%u: cycle without a back-edge through b%u", b, s);
                if (!state[s] && !(ir->block[s].flags & T1_BLK_DEAD)) {
                    state[s] = 1;
                    stack[sp] = s;
                    next[sp++] = 0;
                }
                continue;
            }
            state[b] = 2;
            --sp;
        }
    }
    return true;
}

bool t1_ir_verify(const struct t1_ir *ir, unsigned flags, char *err, size_t err_size)
{
    size_t pages = (sizeof(struct vctx) + 4095) / 4096;
    struct vctx *c = t1_ws_alloc(ir->ws, pages, 0);
    if (!c) {
        vmm_snprintf(err, err_size, "verify: no memory");
        return false;
    }
    c->ir = ir;
    c->err = err;
    c->size = err_size;
    c->cached_dominators = 0;
    err[0] = 0;
    bool ok = true;
    if (ir->failed)
        ok = fail(c, "IR failed", 0, 0);
    else if (ir->nblocks < 2 || ir->nblocks > T1_MAX_BLOCKS || ir->nops > T1_IR_MAX_OPS)
        ok = fail(c, "no region", 0, 0);
    else if (!check_cfg_bounds(c))
        ok = false;
    else if (!t1_ir_dominators(ir, c->idom))
        ok = fail(c, "dominators", 0, 0);
    for (unsigned o = 0; ok && o < ir->nops; ++o)
        c->mem_user[o] = T1_NO_OP;
    for (unsigned b = 0; ok && b < ir->nblocks; ++b) {
        if (ir->block[b].flags & T1_BLK_DEAD)
            continue;
        if (c->idom[b] == T1_NO_BLOCK) {
            ok = fail(c, "b%u: unreachable block", b, 0);
            break;
        }
        uint32_t n = 0;
        for (uint32_t o = ir->block[b].first; o != T1_NO_OP && n <= T1_IR_MAX_OPS;
             o = ir->op[o].next)
            c->pos[o] = n++;
        if (n > T1_IR_MAX_OPS)
            ok = fail(c, "b%u: op list cycle", b, 0);
    }
    if (ok && ir->block[T1_ENTRY_BLOCK].npred)
        ok = fail(c, "entry block has predecessors", 0, 0);
    if (ok)
        ok = check_forward_acyclic(c);
    /* The memory-fork check compares each consumer with the previous one
     * in both dominance directions, so block order does not matter. */
    for (unsigned b = 0; ok && b < ir->nblocks; ++b)
        if (!(ir->block[b].flags & T1_BLK_DEAD))
            ok = check_block(c, (uint16_t)b, flags);
    if (ok)
        ok = check_points(c, flags);
    t1_ws_free(ir->ws, c, pages);
    return ok;
}
