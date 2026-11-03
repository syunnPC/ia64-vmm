/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Post-allocation verifier: forward dataflow
 * over the allocated MIR assigning each physical location (the 16 GPRs and
 * the spill slots) the vreg it holds, and RFLAGS whether a producer of the
 * same rule or stub still holds them. It fails if
 *   - a use reads a location that does not hold its vreg (this also covers
 *     a value live across an instruction that clobbers its register: the
 *     clobber forgets the register),
 *   - a frame entry names a location not holding its value, at the point's
 *     instruction and, for sites and helpers of verification builds, at its
 *     FORCE instruction as well (both RIPs of the site),
 *   - an operand is in a register outside the pool or the reserved
 *     registers, a precolored vreg is elsewhere than its register, or a
 *     frame names R11-R14,
 *   - a flags read has no producer since the last clobber (so no flag value
 *     is live across a point: every point clobbers RFLAGS).
 * Writes are taken from x86enc's table (operand access, implicit
 * registers, flags) through t1_mir_effects, the same description the
 * allocator used; the pseudo-op expansions are checked against their
 * declared clobbers by emit.c.
 */
#include "engine/tier1/codegen.h"
#include "ia64vmm/hal.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define UNKNOWN UINT32_MAX
#define NLOC (T1_X86_NREGS + T1_SPILL_SLOTS)

struct state {
    t1_vreg loc[NLOC];
    bool flags;
    bool reached;
};

struct rv {
    const struct t1_mir *m;
    struct state *in; /* per block */
    char *err;
    size_t err_size;
    bool failed;
    bool pending[T1_MIR_MAX_BLOCKS]; /* first visit or changed entry state */
};

static bool rv_fail(struct rv *R, uint32_t insn, const char *what, unsigned a, unsigned b)
{
    if (!R->failed && R->err_size)
        vmm_snprintf(R->err, R->err_size, "raverify: insn %u: %s (%u, %u)", insn, what, a, b);
    R->failed = true;
    return false;
}

static unsigned loc_index(const struct t1_loc *l)
{
    if (l->kind == T1_LOC_REG)
        return l->reg;
    if (l->kind == T1_LOC_SPILL)
        return T1_X86_NREGS + l->slot;
    return NLOC;
}

static void meet(struct state *into, const struct state *from)
{
    if (!into->reached) {
        *into = *from;
        into->reached = true;
        into->flags = false;
        return;
    }
    for (unsigned k = 0; k < NLOC; ++k)
        if (into->loc[k] != from->loc[k])
            into->loc[k] = UNKNOWN;
    into->flags = false;
}

static bool reg_ok(const struct t1_mir *m, t1_vreg v, uint8_t reg)
{
    if (reg >= T1_X86_NREGS)
        return false;
    if (v < m->nvregs && m->vreg[v].fixed != T1_X86_NOREG)
        return reg == m->vreg[v].fixed;
    return (m->allocatable & T1_X86_REGBIT(reg)) != 0;
}

static bool check_frame(struct rv *R, const struct state *s, uint32_t i, uint32_t point)
{
    const struct t1_mir *m = R->m;
    const struct t1_mir_point *p = &m->point[point];
    for (unsigned k = 0; k < p->frame_count; ++k) {
        const struct t1_mir_frame *f = &m->frame[p->frame_first + k];
        const struct t1_loc *l = &m->frame_loc[p->frame_first + k];
        switch (f->kind) {
        case T1_MF_HOME:
            if (l->kind != T1_LOC_HOME)
                return rv_fail(R, i, "home entry with a location", f->element, l->kind);
            break;
        case T1_MF_CONST:
            if (l->kind != T1_LOC_CONST || l->value != f->value)
                return rv_fail(R, i, "constant entry", f->element, l->kind);
            break;
        default: {
            unsigned x = loc_index(l);
            if (x >= NLOC)
                return rv_fail(R, i, "frame value without a location", f->element, l->kind);
            if (l->kind == T1_LOC_REG && !(m->allocatable & T1_X86_REGBIT(l->reg)) &&
                m->vreg[f->vreg].fixed != l->reg)
                return rv_fail(R, i, "frame names a reserved register", f->element, l->reg);
            if (s->loc[x] != f->vreg)
                return rv_fail(R, i, "frame location does not hold its value", f->element, f->vreg);
        }
        }
    }
    return true;
}

static bool check_use(struct rv *R, const struct state *s, uint32_t i, t1_vreg v, uint8_t reg)
{
    if (!reg_ok(R->m, v, reg))
        return rv_fail(R, i, "operand register outside the pool", v, reg);
    if (s->loc[reg] != v)
        return rv_fail(R, i, "register does not hold the used vreg", v, reg);
    return true;
}

static void clobber_mask(struct state *s, uint16_t mask)
{
    while (mask) {
        unsigned r = (unsigned)__builtin_ctz((unsigned)mask);
        s->loc[r] = UNKNOWN;
        mask &= mask - 1;
    }
}

static void flow_to(struct rv *R, uint16_t target, const struct state *s, bool *changed)
{
    struct state before = R->in[target];
    meet(&R->in[target], s);
    if (!before.reached || memcmp(before.loc, R->in[target].loc, sizeof(before.loc))) {
        R->pending[target] = true;
        *changed = true;
    }
}

static bool step(struct rv *R, struct state *s, uint32_t i, bool *changed)
{
    const struct t1_mir *m = R->m;
    const struct t1_mir_insn *in = &m->insn[i];
    const struct t1_x86_info *x = t1_x86_info(in->op);
    if (x) {
        struct t1_mir_effects fx;
        t1_mir_effects(in, &fx);
        for (unsigned k = 0; k < fx.nuse; ++k) {
            uint8_t o = fx.use[k];
            if (o == 0x80) {
                if (!check_use(R, s, i, in->mem.base, m->memreg[i].base))
                    return false;
                continue;
            }
            if (o == 0x81) {
                if (!check_use(R, s, i, in->mem.index, m->memreg[i].index))
                    return false;
                continue;
            }
            if (!check_use(R, s, i, in->opnd[o].vreg, in->opnd[o].fixed))
                return false;
            if (fx.fixed[o] != T1_X86_NOREG && in->opnd[o].fixed != fx.fixed[o])
                return rv_fail(R, i, "fixed operand elsewhere", in->opnd[o].vreg,
                               in->opnd[o].fixed);
        }
        if (x->flags_read && !s->flags)
            return rv_fail(R, i, "flags read without a producer", in->op, 0);
        clobber_mask(s, fx.clobber_regs);
        for (unsigned k = 0; k < fx.ndef; ++k) {
            const struct t1_mir_opnd *o = &in->opnd[fx.def[k]];
            if (!reg_ok(m, o->vreg, o->fixed))
                return rv_fail(R, i, "result register outside the pool", o->vreg, o->fixed);
            s->loc[o->fixed] = o->vreg;
        }
        if (x->flags_def || x->flags_undef)
            s->flags = x->flags_def != 0;
        if (x->attr & T1_XA_BRANCH) {
            if (in->opnd[0].kind != T1_MO_LABEL || in->opnd[0].imm >= m->nblocks)
                return rv_fail(R, i, "branch without a block label", in->op, 0);
            flow_to(R, (uint16_t)in->opnd[0].imm, s, changed);
        }
        return true;
    }
    switch (in->op) {
    case T1_MIR_HREAD:
    case T1_MIR_LIVEIN:
        s->flags = false;
        if (!reg_ok(m, in->opnd[0].vreg, in->opnd[0].fixed))
            return rv_fail(R, i, "LIVEIN register", in->opnd[0].vreg, 0);
        s->loc[in->opnd[0].fixed] = in->opnd[0].vreg;
        return true;
    case T1_MIR_PCOPY: {
        t1_vreg val[T1_MIR_MAX_COPIES < 256 ? T1_MIR_MAX_COPIES : 256];
        if (in->ncopies > 256)
            return rv_fail(R, i, "parallel copy too large", in->ncopies, 0);
        for (uint32_t c = 0; c < in->ncopies; ++c) {
            const struct t1_mir_copy *cp = &m->copy[in->copies_first + c];
            val[c] = cp->dst;
            if (cp->sloc.kind == T1_LOC_CONST)
                continue;
            unsigned xs = loc_index(&cp->sloc);
            if (xs >= NLOC)
                return rv_fail(R, i, "copy source location", cp->src, cp->sloc.kind);
            if (cp->sloc.kind == T1_LOC_SPILL && cp->sloc.slot >= T1_SPILL_SCRATCH)
                return rv_fail(R, i, "copy from the scratch slot", cp->src, 0);
            if (s->loc[xs] != cp->src)
                return rv_fail(R, i, "copy source does not hold its vreg", cp->src, xs);
        }
        for (uint32_t c = 0; c < in->ncopies; ++c) {
            const struct t1_mir_copy *cp = &m->copy[in->copies_first + c];
            unsigned xd = loc_index(&cp->dloc);
            if (xd >= NLOC)
                return rv_fail(R, i, "copy destination location", cp->dst, cp->dloc.kind);
            if (cp->dloc.kind == T1_LOC_SPILL && cp->dloc.slot >= T1_SPILL_SCRATCH)
                return rv_fail(R, i, "copy to the scratch slot", cp->dst, 0);
            if (cp->dloc.kind == T1_LOC_REG && !reg_ok(m, cp->dst, cp->dloc.reg))
                return rv_fail(R, i, "copy destination register", cp->dst, cp->dloc.reg);
        }
        for (uint32_t c = 0; c < in->ncopies; ++c) {
            const struct t1_mir_copy *cp = &m->copy[in->copies_first + c];
            s->loc[loc_index(&cp->dloc)] = val[c];
        }
        s->loc[T1_R11] = UNKNOWN;
        return true;
    }
    case T1_MIR_CHARGE:
        s->flags = false;
        return true;
    case T1_MIR_FENCE:
    case T1_MIR_BSPCALL:
    case T1_MIR_BLOCK:
        return true;
    case T1_MIR_HGUARD:
    case T1_MIR_CHECKPOINT:
        if (!check_frame(R, s, i, in->point))
            return false;
        s->flags = false;
        s->loc[T1_R11] = UNKNOWN;
        return true;
    case T1_MIR_EXIT:
        return check_frame(R, s, i, in->point);
    case T1_MIR_FORCE:
    case T1_MIR_SITE:
    case T1_MIR_SLOG: {
        struct t1_mir_effects fx;
        t1_mir_effects(in, &fx);
        for (unsigned k = 0; k < fx.nuse; ++k)
            if (!check_use(R, s, i, in->opnd[fx.use[k]].vreg, in->opnd[fx.use[k]].fixed))
                return false;
        if (in->op != T1_MIR_SLOG && !check_frame(R, s, i, in->point))
            return false;
        s->flags = false;
        s->loc[T1_R11] = UNKNOWN;
        for (unsigned k = 0; k < fx.ndef; ++k) {
            const struct t1_mir_opnd *o = &in->opnd[fx.def[k]];
            if (!reg_ok(m, o->vreg, o->fixed))
                return rv_fail(R, i, "site result register", o->vreg, o->fixed);
            s->loc[o->fixed] = o->vreg;
        }
        return true;
    }
    case T1_MIR_HELPER: {
        /* The exceptional edge uses the capture before any result exists. */
        if (!check_frame(R, s, i, in->point))
            return false;
        struct t1_mir_effects fx;
        t1_mir_effects(in, &fx);
        clobber_mask(s, fx.clobber_regs);
        s->flags = false;
        /* Only the normal edge defines results; other saved GPRs survive. */
        for (unsigned k = 0; k < fx.ndef; ++k) {
            const struct t1_mir_opnd *o = &in->opnd[fx.def[k]];
            if (!reg_ok(m, o->vreg, o->fixed))
                return rv_fail(R, i, "helper result register", o->vreg, o->fixed);
            s->loc[o->fixed] = o->vreg;
        }
        return true;
    }
    default:
        return rv_fail(R, i, "unexpected MIR op", in->op, 0);
    }
}

bool t1_raverify(const struct t1_mir *m, char *err, size_t err_size)
{
    struct rv R = {.m = m, .err = err, .err_size = err_size};
    size_t pages = (m->nblocks * sizeof(struct state) + 4095) / 4096;
    R.in = t1_ws_alloc(m->ir->ws, pages ? pages : 1, HAL_PAGE_ZERO);
    if (!R.in)
        return rv_fail(&R, 0, "no memory", 0, 0);
    /* Unreached entries are copied from their first predecessor by
     * meet(). Only the initial entry needs unknown locations now. */
    for (unsigned k = 0; k < NLOC; ++k)
        R.in[0].loc[k] = UNKNOWN;
    R.in[0].reached = true;
    R.pending[0] = true;
    /* Every site and helper of a verification build is checked at its
     * FORCE instruction too (step() checks the frame of any instruction
     * carrying the point). */
    for (bool changed = true; changed && !R.failed;) {
        changed = false;
        for (unsigned b = 0; b < m->nblocks && !R.failed; ++b) {
            /* Replaying an unchanged entry repeats exactly the same
             * checks and outgoing meets. Clear before stepping so a
             * self-edge can request another visit. */
            if (!R.pending[b])
                continue;
            R.pending[b] = false;
            struct state s = R.in[b];
            const struct t1_mir_block *blk = &m->block[b];
            for (uint32_t i = blk->first; i < blk->first + blk->count && !R.failed; ++i)
                step(&R, &s, i, &changed);
        }
    }
    /* Every live point is reached. */
    for (unsigned p = 0; p < m->ir->npoints && !R.failed; ++p) {
        const struct t1_mir_point *mp = &m->point[p];
        if (mp->insn == T1_MIR_NONE)
            continue;
        if ((m->flags & T1_COMPILE_VERIFY) &&
            (mp->kind == T1_PT_SITE || mp->kind == T1_PT_HELPER) && mp->force_insn == T1_MIR_NONE)
            rv_fail(&R, mp->insn, "verification point without a force counter", p, 0);
    }
    t1_ws_free(m->ir->ws, R.in, pages ? pages : 1);
    return !R.failed;
}
