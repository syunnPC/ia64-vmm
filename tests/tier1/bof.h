/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Frame-base helpers of the Tier1 tests (frame-relative homes),
 * written from the architecture (SDM Vol.1 §4.1.1, Vol.2 §6: a stacked GR
 * names the physical register at its frame offset from BOF, modulo the 96
 * stacked registers) rather than from engine/tier1, so that a test that uses
 * them checks the engine instead of repeating it.
 *
 *   t1t_element_reg  the element register number Tier1 gives a logical GR:
 *                    static r < 32 itself, stacked r -> 32 + o, o the frame
 *                    offset after rotation by CFM.rrb.gr (no frame base)
 *   t1t_phys         the physical gr[]/nat[] index of element register p at
 *                    frame base bof
 *   t1t_rebase_bof   moves a state's frame base, rotating the 96 stacked
 *                    physical registers with it
 */
#ifndef IA64VMM_TESTS_TIER1_BOF_H
#define IA64VMM_TESTS_TIER1_BOF_H
#include "ia64/state.h"

static inline unsigned t1t_element_reg(const struct ia64_state *s, unsigned reg)
{
    if (reg < 32)
        return reg;
    unsigned o = reg - 32, sor = 8u * (unsigned)((s->cfm >> 14) & 15);
    if (o < sor)
        o = (o + (unsigned)((s->cfm >> 18) & 127)) % sor;
    return 32 + o;
}

static inline unsigned t1t_phys(unsigned bof, unsigned p)
{
    return p < 32 ? p : 32 + (bof + (p - 32)) % IA64_N_STACKED_PHYS;
}

/*
 * Every RSE computation of the CPU model is relative to rse_bof modulo 96
 * (ia64/rse.c physical(), ia64_gr_index): rse_dirty, rse_missing and CFLE
 * count registers from BOF, the backing store is addressed by BSP/BSPSTORE.
 * Rotating gr/nat[32..127] by (bof - rse_bof) and setting rse_bof = bof is
 * therefore the same architectural state with another physical numbering --
 * the state a guest reaches when the same code runs at that call depth. A
 * reference run from the result is the reference at that BOF.
 */
static inline void t1t_rebase_bof(struct ia64_state *s, unsigned bof)
{
    uint64_t gr[IA64_N_STACKED_PHYS];
    bool nat[IA64_N_STACKED_PHYS];
    unsigned from = s->rse_bof % IA64_N_STACKED_PHYS;
    bof %= IA64_N_STACKED_PHYS;
    for (unsigned o = 0; o < IA64_N_STACKED_PHYS; ++o) {
        gr[o] = s->gr[32 + (from + o) % IA64_N_STACKED_PHYS];
        nat[o] = s->nat[32 + (from + o) % IA64_N_STACKED_PHYS];
    }
    for (unsigned o = 0; o < IA64_N_STACKED_PHYS; ++o) {
        s->gr[32 + (bof + o) % IA64_N_STACKED_PHYS] = gr[o];
        s->nat[32 + (bof + o) % IA64_N_STACKED_PHYS] = nat[o];
    }
    s->rse_bof = bof;
}
#endif
