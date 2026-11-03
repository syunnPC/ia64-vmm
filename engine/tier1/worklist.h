/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_TIER1_WORKLIST_H
#define IA64VMM_TIER1_WORKLIST_H
#include "engine/tier1/ir.h"
#include "engine/tier1/memory.h"

/* One queue entry per op, with PARAM positions captured before rewriting.
 * Callers mark an op before appending it, so cycles and duplicate operands
 * never grow the queue beyond the input's nops. Small regions use the
 * inline buffer; larger ones borrow workspace until propagation ends. */
struct t1_worklist {
    uint16_t *op;
    uint16_t *param_pos;
    unsigned count;
    size_t pages;
    uint16_t local[2 * 128];
};
_Static_assert(T1_IR_MAX_OPS <= UINT16_MAX + 1u, "IR worklist op index");

static inline bool t1_worklist_init(struct t1_worklist *w, struct t1_ir *ir)
{
    if (ir->failed)
        return false;
    size_t bytes = ir->nops * (sizeof(*w->op) + sizeof(*w->param_pos));
    w->pages = bytes <= sizeof(w->local) ? 0 : (bytes + 4095) / 4096;
    w->op = w->pages ? t1_ws_alloc(ir->ws, w->pages, 0) : w->local;
    if (!w->op) {
        t1_ir_fail(ir, "no memory for IR worklist");
        return false;
    }
    w->param_pos = w->op + ir->nops;
    w->count = 0;
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        if (ir->block[b].flags & T1_BLK_DEAD)
            continue;
        unsigned pos = 0;
        for (uint32_t o = ir->block[b].first; o != T1_NO_OP && ir->op[o].code == T1_IR_PARAM;
             o = ir->op[o].next)
            w->param_pos[o] = (uint16_t)pos++;
    }
    return true;
}

static inline void t1_worklist_free(struct t1_worklist *w, struct t1_ir *ir)
{
    if (w->pages)
        t1_ws_free(ir->ws, w->op, w->pages);
}
#endif
