/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * In-region helpers call ia64_execute on immutable, rebased instructions;
 * qp is evaluated by the helper. Materialize the read set from the point frame
 * (read set ⊆ W), set instruction IP/slot, then reload results on COMPLETE.
 * Other results exit before effect for lower-tier execution; helpers must
 * commit nothing before a fault.
 *
 * Home access uses the entry frame base; t1_point_frame sets the logical frame
 * for ia64_execute. This keeps stacked homes aligned across calls and allocs.
 */
#include "engine/tier1/region.h"
#include "engine/tier1/helpers.h"
#include "ia64/semantics.h"
#include "ia64/rse.h"
#include "ia64/fp.h"
#include "ia64vmm/hal.h"

/*
 * Spill allocs only when ia64_rse_spill_resident confirms every required
 * translation (SDM Vol.2 §6.5); otherwise exit before effect for lower-tier
 * execution, leaving no partial spill. Stores affect only registers below the
 * entry frame: formation keeps delta+sof <= 96, and spilling leaves 96-sof
 * dirty registers below the new frame. Cached stacked elements remain valid;
 * matched returns stay complete. Update entry_dirty so point deltas describe
 * the new partition. BSP stays unchanged; BSPSTORE/ar.rnat remain in homes.
 * If a spill retires this region, exit after completing alloc.
 */
static uint32_t alloc_call(struct t1_exec *x, const struct t1_point *p, Ia64Instruction *copy)
{
    struct vcpu *v = x->v;
    if (!ia64_rse_spill_resident(v, copy->operands.system.immediate & 127)) {
        T1_COUNT_TV(x->tv, spill_exits, 1);
        x->tv->pending = (struct t1_pending){T1V_PATH_HELPER, false, false, false, 0, 0};
        return T1_RESUME_EXIT;
    }
    struct ia64_result res = ia64_execute(v, copy);
    x->entry_dirty = v->s.rse_dirty - p->delta;
    if (res.kind != IA64_STEP_COMPLETE) {
        x->tv->pending = (struct t1_pending){T1V_PATH_HELPER, false, false, false, 0, 0};
        return T1_RESUME_EXIT;
    }
    T1_COUNT_TV(x->tv, spills, 1);
    x->helper_exit = !x->region->valid || hostwin_stop_detail(x->tv->hw) != NULL;
    return T1_RESUME_RETRY;
}

uint32_t t1_helper_call(struct t1_exec *x)
{
    const struct t1_region *r = x->region;
    struct vcpu *v = x->v;
    uint32_t point = x->point;
    uint16_t hi = r->helper_of[point];
    if (hi == UINT16_MAX)
        return T1_RESUME_EXIT;
    const struct t1_helper_info *h = &r->helper[hi];
    const struct t1_point *p = &r->point[point];
    t1_helper_call_note(h->insn.opcode);
#if defined(IA64VMM_HOSTED) || defined(IA64VMM_TIER1_CHECKED)
    /* Checked lockstep: the records so far, before the helper's own
     * accesses reach the capture. */
    if (r->c.flags & T1_COMPILE_UNDO_LOG)
        t1_ck_collect(x->tv);
#endif
    for (unsigned k = 0; k < h->nreads; ++k) {
        const struct t1_frame_entry *f = &r->frame[h->read_frame[k]];
        struct t1_loc loc = t1_frame_loc(f);
        t1_home_write_at(v, x->entry_bof, f->element, t1_loc_value(x, &loc));
    }
    if (r->frames)
        t1_point_frame(x, p, v);
    v->s.ip = p->ip;
    v->s.slot = p->slot;
    Ia64Instruction copy = h->insn;
    const struct t1_helper_row *row = t1_helper_row(copy.opcode);
    /* An FP trap commits the FR, predicates, FPSR and PSR.mfl/mfh before
     * the interruption: undo them for the exit before effect. */
    bool undo = row && row->cls == T1_HC_A;
    unsigned fr = undo ? ia64_fr_index(&v->s, copy.operands.floating.destination & 127) : 0;
    struct ia64_fr old_fr = v->s.fr[fr];
    uint64_t old_pr = v->s.pr, old_fpsr = v->s.ar[AR_FPSR], old_psr = v->s.psr;
    if (copy.opcode == IA64_OP_ALLOC)
        return alloc_call(x, p, &copy);
    struct ia64_result res = ia64_execute(v, &copy);
    if (res.kind != IA64_STEP_COMPLETE) {
        if (undo) {
            v->s.fr[fr] = old_fr;
            v->s.pr = old_pr;
            v->s.ar[AR_FPSR] = old_fpsr;
            v->s.psr = old_psr;
        }
        x->tv->pending = (struct t1_pending){T1V_PATH_HELPER, false, false, false, 0, 0};
        return T1_RESUME_EXIT;
    }
    for (unsigned d = 0; d < 2; ++d) {
        uint64_t value;
        if (!h->wr[d] || h->def_reg[d] == T1_X86_NOREG)
            continue;
        /* wr[] holds elements by construction (lift.c); after the commit an
         * exit "before effect" would execute the instruction twice. */
        if (!t1_home_read_at(v, x->entry_bof, h->wr[d], &value))
            hal_panic("tier1: helper result is not an element");
        x->regs[h->def_reg[d]] = value;
    }
    x->helper_exit = 0;
    if (row) {
        /* The write set formation named (frame-relative elements at the
         * entry's frame base, the instruction's rotation). */
        for (unsigned k = 0; k < h->nwrites; ++k)
            if (!t1_home_read_at(v, x->entry_bof, h->writes[k], &x->helper_out[h->writes[k]]))
                hal_panic("tier1: generalized helper write set");
        if (row->flags & (T1_HROW_STORE | T1_HROW_WALK)) {
            if (row->flags & T1_HROW_WALK)
                hostwin_enter(x->tv->hw, v);
            if (__atomic_load_n(&v->requested_exit, __ATOMIC_ACQUIRE))
                x->helper_notification = point + 1;
            x->helper_exit =
                !r->valid || hostwin_stop_detail(x->tv->hw) != NULL || x->helper_notification;
        }
    }
    return T1_RESUME_RETRY;
}
