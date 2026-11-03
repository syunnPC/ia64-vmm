/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NaT soundness: native consumption folds under the
 * entry guard; exact helper consumption carries an entry snapshot or a
 * helper-produced HREAD. No LIVEIN(E_NAT) survives this pass.
 * t1_nat_guard_holds checks the region's entry NaTs. */
#include "engine/tier1/region.h"
#include "engine/tier1/helpers.h"
#include "engine/tier1/worklist.h"
#include "ia64/fp.h"

/* Only native consumption requires zero. Trace through NaT propagation
 * and loop parameters; helper inputs and restoration preserve exact bits.
 * Iteration avoids recursion over the bounded SSA graph. */
static void need(const struct t1_ir *ir, uint64_t *needed, struct t1_worklist *w, t1_value v)
{
    if (v == T1_VALUE_NONE)
        return;
    unsigned o = T1_VALUE_OP(t1_ir_resolve(ir, v));
    uint64_t bit = UINT64_C(1) << (o % 64);
    if (needed[o / 64] & bit)
        return;
    needed[o / 64] |= bit;
    w->op[w->count++] = (uint16_t)o;
}

static bool wanted(const uint64_t *needed, unsigned o)
{
    return (needed[o / 64] >> (o % 64)) & 1;
}

/* Packed arithmetic lowers natively. Its pure word lowering
 * propagates NaT with the existing I1 operations, so keep those inputs
 * exact too. A real NaT consumer below still traces and guards its inputs;
 * this only exempts conservative native-only folding. Consult the form so
 * this property survives folding, e.g. psub4 rD=rA,rA has a constant value
 * but still propagates rA's exact NaT. This set carries no runtime state.
 * chk.s branches on its GR's NaT bit: read at run time,
 * so an entry with the bit set leaves to the recovery code instead of
 * being declined. */
static void packed_exact_inputs(const struct t1_ir *ir, bool *exact)
{
    if (!ir->form)
        return;
    for (unsigned i = 0; i < ir->form->ninsns; ++i) {
        const struct t1_insn *ti = &ir->form->insn[i];
        if (ti->cls == T1_IC_CHK && !ti->insn.check_fp)
            exact[ti->source] = true;
        if (ti->cls != T1_IC_ALU)
            continue;
        switch (ti->insn.opcode) {
        case IA64_OP_PADD1:
        case IA64_OP_PADD2:
        case IA64_OP_PADD4:
        case IA64_OP_PSUB1:
        case IA64_OP_PSUB2:
        case IA64_OP_PSUB4:
            exact[ti->src1] = exact[ti->src2] = exact[ti->dst] = true;
            break;
        default:
            break;
        }
    }
}

static void native_uses(const struct t1_ir *ir, uint64_t *needed, struct t1_worklist *w,
                        const bool *exact, unsigned first, unsigned n)
{
    for (unsigned k = 0; k < n; ++k) {
        const struct t1_use *u = &ir->use[first + k];
        if (u->value == T1_VALUE_NONE || (u->flags & T1_USE_HOME))
            continue;
        const struct t1_op *d = &ir->op[T1_VALUE_OP(t1_ir_resolve(ir, u->value))];
        if (d->code == T1_IR_LIVEIN && d->imm > 128 && d->imm < 256 && !exact[d->imm - 128])
            need(ir, needed, w, u->value);
    }
}

unsigned t1_natguard(struct t1_ir *ir)
{
    struct t1_worklist w;
    if (!t1_worklist_init(&w, ir))
        return 0;
    uint64_t needed[T1_IR_MAX_OPS / 64] = {0};
    bool exact[128] = {0};
    packed_exact_inputs(ir, exact);
    ir->fr_guard[0] = ir->fr_guard[1] = 0;
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        if (ir->block[b].flags & T1_BLK_DEAD)
            continue;
        for (uint32_t o = ir->block[b].first; o != T1_NO_OP; o = ir->op[o].next) {
            const struct t1_op *op = &ir->op[o];
            if (op->code == T1_IR_HELPER) {
                /* tnat tests the exact bit (SDM Vol.3 Table 2-58). */
                for (unsigned k = 0; k < op->nuse; ++k) {
                    unsigned e = ir->use[op->use_first + k].element;
                    if (e > 128 && e < 256)
                        exact[e - 128] = true;
                }
            }
            if (op->code != T1_IR_GHELPER)
                continue;
            uint16_t elements[T1_N_ELEMENTS];
            for (unsigned w = 0; w < 2; ++w) {
                unsigned n = t1_helper_elements(&ir->form->insn[op->imm], w, elements);
                for (unsigned k = 0; k < n; ++k)
                    if (elements[k] > 128 && elements[k] < 256)
                        exact[elements[k] - 128] = true;
            }
        }
    }
    /* Preserve the conservative folding for native-only elements. */
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        const struct t1_block *blk = &ir->block[b];
        if (blk->flags & T1_BLK_DEAD)
            continue;
        for (uint32_t o = blk->first; o != T1_NO_OP; o = ir->op[o].next)
            native_uses(ir, needed, &w, exact, ir->op[o].use_first, ir->op[o].nuse);
        for (unsigned k = 0; k < blk->nsucc; ++k)
            native_uses(ir, needed, &w, exact, blk->succ[k].args_first, blk->succ[k].nargs);
    }
    for (unsigned p = 0; p < ir->npoints; ++p)
        if (!ir->point[p].dead)
            native_uses(ir, needed, &w, exact, ir->point[p].frame_first, ir->point[p].frame_count);
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        if (ir->block[b].flags & T1_BLK_DEAD)
            continue;
        for (uint32_t o = ir->block[b].first; o != T1_NO_OP; o = ir->op[o].next) {
            const struct t1_op *op = &ir->op[o];
            if (op->code != T1_IR_NATCHK)
                continue;
            for (unsigned k = 0; k < op->nuse; ++k) {
                const struct t1_use *u = &ir->use[op->use_first + k];
                need(ir, needed, &w, u->value);
            }
        }
    }
    for (unsigned head = 0; head < w.count; ++head) {
        unsigned o = w.op[head];
        const struct t1_op *op = &ir->op[o];
        if (op->flags & T1_OPF_DEAD)
            continue;
        if (op->code == T1_IR_HREAD || op->code == T1_IR_GHELPER || op->code == T1_IR_HELPER)
            continue;
        for (unsigned k = 0; k < op->nuse; ++k)
            need(ir, needed, &w, ir->use[op->use_first + k].value);
        if (op->code != T1_IR_PARAM)
            continue;
        const struct t1_block *blk = &ir->block[op->block];
        for (unsigned k = 0; k < blk->npred; ++k) {
            const struct t1_edge *e = &ir->block[blk->pred[k]].succ[blk->pred_slot[k]];
            need(ir, needed, &w, ir->use[e->args_first + w.param_pos[o]].value);
        }
    }
    t1_worklist_free(&w, ir);
    /* FR NaTVal can propagate natively. Guard only when this test feeds
     * consumption that needs zero, like the GR guards below; restoration
     * alone must not repeatedly reject a speculative-load NaTVal. */
    t1_value clear_fr = t1_ir_const(ir, T1_I1, 0);
    for (unsigned o = 0; o < ir->nops; ++o) {
        if (!wanted(needed, o) || ir->op[o].code != T1_IR_FRNATTEST ||
            (ir->op[o].flags & T1_OPF_DEAD))
            continue;
        t1_value s = t1_ir_resolve(ir, ir->use[ir->op[o].use_first].value);
        t1_value m = t1_ir_resolve(ir, ir->use[ir->op[o].use_first + 1].value);
        const struct t1_op *sig = &ir->op[T1_VALUE_OP(s)], *meta = &ir->op[T1_VALUE_OP(m)];
        if (sig->code != T1_IR_LIVEIN || meta->code != T1_IR_LIVEIN ||
            sig->imm < T1_ELEMENT_FR(2) || sig->imm > T1_ELEMENT_FR(127) ||
            meta->imm != sig->imm + 129)
            continue;
        unsigned f = sig->imm - 323u;
        ir->fr_guard[f / 64] |= UINT64_C(1) << (f % 64);
        t1_ir_forward(ir, o, clear_fr);
    }
    t1_ir_resolve_all(ir);
    unsigned n = 0;
    ir->nat_guard[0] = ir->nat_guard[1] = 0;
    t1_value zero = t1_ir_const(ir, T1_I1, 0);
    for (uint32_t o = ir->block[T1_ENTRY_BLOCK].first, next; o != T1_NO_OP; o = next) {
        next = ir->op[o].next;
        const struct t1_op *op = &ir->op[o];
        if (op->code != T1_IR_LIVEIN || op->imm < 129 || op->imm >= 256)
            continue;
        unsigned p = (unsigned)op->imm - 128;
        if (wanted(needed, o)) {
            ir->nat_guard[p / 64] |= UINT64_C(1) << (p % 64);
            ++n;
            t1_ir_forward(ir, o, zero);
        } else {
            /* Keep the exact entry bit for GHELPER inputs, false-qp
             * destinations and frames without weakening LIVEIN checks. */
            ir->op[o].code = T1_IR_NATIN;
        }
    }
    ir->nat_folded = true;
    t1_opt_fold(ir);
    /* Unreachable for the native op set (every NaT value is now CONST 0 and
     * folds), but a surviving check must not be dropped: its instruction
     * becomes a region boundary. */
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        if (ir->block[b].flags & T1_BLK_DEAD)
            continue;
        for (uint32_t o = ir->block[b].first; o != T1_NO_OP; o = ir->op[o].next)
            if (ir->op[o].code == T1_IR_NATCHK && ir->op[o].insn < ir->boundary_insn)
                ir->boundary_insn = ir->op[o].insn;
    }
    return n;
}

/* G as the per-entry check reads it: the static indices, then the stacked
 * offsets (each ascending), and 1 + the largest stacked offset. Built from
 * r->nat_guard by t1_frame_build, after compile.c has set it. */
void t1_nat_guard_list(struct t1_region *r)
{
    unsigned n = 0, span = 0;
    for (unsigned p = 1; p < 32; ++p)
        if ((r->nat_guard[0] >> p) & 1)
            r->guard[n++] = (uint8_t)p;
    r->nguard_static = (uint8_t)n;
    for (unsigned p = 32; p < 128; ++p)
        if ((r->nat_guard[p / 64] >> (p % 64)) & 1) {
            r->guard[n++] = (uint8_t)(p - 32);
            span = p - 31;
        }
    r->nguard = (uint8_t)n;
    r->guard_span = (uint8_t)span;
}

/* Per entry (inside the entry budget): nat[p] of the static elements,
 * nat[32 + (bof + o) mod 96] of the stacked ones; when no stacked offset of
 * G wraps at this BOF (all but about span/96 of the frame bases) one base
 * serves them all. */
bool t1_nat_guard_holds(const struct t1_region *r, const struct vcpu *v)
{
    for (unsigned w = 0; w < 2; ++w) {
        uint64_t g = r->fr_guard[w];
        while (g) {
            unsigned f = (unsigned)__builtin_ctzll(g) + 64 * w;
            if (ia64_fr_is_nat(v->s.fr[f]))
                return false;
            g &= g - 1;
        }
    }
    const bool *nat = v->s.nat;
    const uint8_t *g = r->guard;
    unsigned k = 0, ns = r->nguard_static, n = r->nguard, bad = 0;
    for (; k < ns; ++k)
        bad |= nat[g[k]];
    if (k == n)
        return !bad;
    unsigned bof = v->s.rse_bof;
    if (bof <= IA64_N_STACKED_PHYS - r->guard_span) {
        const bool *stacked = nat + 32 + bof;
        for (; k < n; ++k)
            bad |= stacked[g[k]];
    } else {
        bof %= IA64_N_STACKED_PHYS; /* rse_bof < 96 (ia64/rse.c); kept exact anyway */
        for (; k < n; ++k) {
            unsigned q = bof + g[k];
            bad |= nat[32 + (q >= IA64_N_STACKED_PHYS ? q - IA64_N_STACKED_PHYS : q)];
        }
    }
    return !bad;
}
