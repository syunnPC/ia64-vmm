/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Materialize non-HOME entries from the captured registers and spills, then
 * restore the point's frame, resume target and retired count. Stacked homes
 * are relative to the region's entry BOF. Fault state is unchanged. */
#include "engine/tier1/region.h"
#include "ia64/rse.h"

uint64_t t1_loc_value(const struct t1_exec *x, const struct t1_loc *l)
{
    switch (l->kind) {
    case T1_LOC_CONST:
        return l->value;
    case T1_LOC_REG:
        return x->regs[l->reg & 15];
    case T1_LOC_SPILL:
        return x->spill[l->slot % T1_SPILL_SLOTS];
    case T1_LOC_REG_PLUS:
        return x->regs[l->reg & 15] + l->value;
    case T1_LOC_SPILL_PLUS:
        return x->spill[l->slot % T1_SPILL_SLOTS] + l->value;
    default:
        return 0;
    }
}

void t1_home_write(struct vcpu *v, uint16_t e, uint64_t value)
{
    t1_home_write_at(v, v->s.rse_bof, e, value);
}

void t1_home_write_at(struct vcpu *v, unsigned bof, uint16_t e, uint64_t value)
{
    if (e >= 1 && e < 128)
        v->s.gr[t1_gr_home_at(bof, e)] = value;
    else if (e >= T1_ELEMENT_NAT(1) && e < T1_ELEMENT_PR(0))
        v->s.nat[t1_gr_home_at(bof, e - 128u)] = value & 1;
    else if (e >= T1_ELEMENT_PR(1) && e < T1_ELEMENT_LC)
        v->s.pr = (v->s.pr & ~(UINT64_C(1) << (e - 256u))) | ((value & 1) << (e - 256u));
    else if (e == T1_ELEMENT_LC)
        v->s.ar[AR_LC] = value;
    else if (e == T1_ELEMENT_LSB)
        v->last_successful_bundle = value;
    else if (e >= T1_ELEMENT_FR(2) && e <= T1_ELEMENT_FR(127))
        v->s.fr[e - 323u].significand = value;
    else if (e >= T1_ELEMENT_FR_META(2) && e <= T1_ELEMENT_FR_META(127)) {
        v->s.fr[e - 452u].exponent = value & 0x1ffff;
        v->s.fr[e - 452u].sign = (value >> 17) & 1;
    } else if (e == T1_ELEMENT_PSR_MF)
        v->s.psr |= value & T1_PSR_MF_BITS;
    else if (e == T1_ELEMENT_PFS)
        v->s.ar[AR_PFS] = value;
    else if (e == T1_ELEMENT_EC)
        v->s.ar[AR_EC] = value;
    else if (e >= T1_ELEMENT_BR(0) && e <= T1_ELEMENT_BR(7))
        v->s.br[e - T1_ELEMENT_BR(0)] = value;
}

bool t1_home_read(const struct vcpu *v, uint16_t e, uint64_t *value)
{
    return t1_home_read_at(v, v->s.rse_bof, e, value);
}

bool t1_home_read_at(const struct vcpu *v, unsigned bof, uint16_t e, uint64_t *value)
{
    if (e >= 1 && e < 128)
        *value = v->s.gr[t1_gr_home_at(bof, e)];
    else if (e >= T1_ELEMENT_NAT(1) && e < T1_ELEMENT_PR(0))
        *value = v->s.nat[t1_gr_home_at(bof, e - 128u)];
    else if (e >= T1_ELEMENT_PR(1) && e < T1_ELEMENT_LC)
        *value = (v->s.pr >> (e - 256u)) & 1;
    else if (e == T1_ELEMENT_LC)
        *value = v->s.ar[AR_LC];
    else if (e == T1_ELEMENT_LSB)
        *value = v->last_successful_bundle;
    else if (e >= T1_ELEMENT_FR(2) && e <= T1_ELEMENT_FR(127))
        *value = v->s.fr[e - 323u].significand;
    else if (e >= T1_ELEMENT_FR_META(2) && e <= T1_ELEMENT_FR_META(127))
        *value = v->s.fr[e - 452u].exponent | ((uint64_t)v->s.fr[e - 452u].sign << 17);
    else if (e == T1_ELEMENT_PSR_MF)
        *value = v->s.psr & T1_PSR_MF_BITS;
    else if (e == T1_ELEMENT_PFS)
        *value = v->s.ar[AR_PFS];
    else if (e == T1_ELEMENT_EC)
        *value = v->s.ar[AR_EC];
    else if (e >= T1_ELEMENT_BR(0) && e <= T1_ELEMENT_BR(7))
        *value = v->s.br[e - T1_ELEMENT_BR(0)];
    else
        return false;
    return true;
}

/* In-region calls preserve frames above the entry's and matched returns
 * release them again, so the frame of a point is the entry's advanced by
 * delta registers: BOF by delta, the dirty partition by delta above
 * x->entry_dirty (which an alloc that spills in the region lowers,
 * helper.c), BSP by delta slots with the RNaT collections skipped
 * (ia64_rse_advance). A taken call skips an entry RNaT slot even with
 * zero locals; that step survives matched returns and loop backedges. */
void t1_point_frame(const struct t1_exec *x, const struct t1_point *p, struct vcpu *v)
{
    v->s.cfm = p->rse ? p->cfm : x->entry_cfm;
    v->s.rse_bof = (x->entry_bof + p->delta) % IA64_N_STACKED_PHYS;
    v->s.rse_dirty = x->entry_dirty + p->delta;
    v->s.ar[AR_BSP] =
        p->delta || x->bsp_called ? ia64_rse_advance(x->entry_bsp, p->delta) : x->entry_bsp;
}

static inline uint64_t value_of(const struct t1_exec *x, const struct t1_frame_entry *l)
{
    if (l->kind == T1_LOC_REG)
        return x->regs[l->reg & 15];
    if (l->kind == T1_LOC_SPILL)
        return x->spill[l->slot % T1_SPILL_SLOTS];
    if (l->kind == T1_LOC_CONST)
        return l->value;
    struct t1_loc loc = t1_frame_loc(l);
    return t1_loc_value(x, &loc);
}

static inline uint64_t nat_value_of(const struct t1_exec *x, const struct t1_frame_entry *l)
{
    if (l->kind == T1_LOC_CONST)
        return l->value;
    if (l->kind == T1_LOC_REG)
        return x->regs[l->reg & 15];
    if (l->kind == T1_LOC_SPILL)
        return x->spill[l->slot % T1_SPILL_SLOTS];
    struct t1_loc loc = t1_frame_loc(l);
    return t1_loc_value(x, &loc);
}

/* A bounded native leaf restores hot points; the table fallback visits
 * each class of homes in its own loop, merging predicates once. */
void t1_materialize(struct t1_exec *x, const struct t1_point *p)
{
    const struct t1_region *r = x->region;
    struct vcpu *v = x->v;
    unsigned bof = x->entry_bof;
    /* The frame's HOME entries follow its first frame_restore entries:
     * their homes already hold the values. */
    const struct t1_frame_entry *base = r->frame + p->frame_first, *f = base, *end;
    if (p->restore_offset) {
        typedef void (*restore_fn)(struct t1_exec *, struct vcpu *);
        restore_fn restore = (restore_fn)(uintptr_t)(r->code + p->restore_offset);
        restore(x, v);
    } else {
        for (end = base + p->plan[0]; f != end; ++f)
            v->s.gr[f->element] = value_of(x, f);
        for (f = end, end = base + p->plan[1]; f != end; ++f) {
            unsigned q = f->element + bof;
            v->s.gr[q >= 128 ? q - IA64_N_STACKED_PHYS : q] = value_of(x, f);
        }
        for (f = end, end = base + p->plan[2]; f != end; ++f)
            v->s.nat[f->element - 128u] = nat_value_of(x, f) & 1;
        for (f = end, end = base + p->plan[3]; f != end; ++f) {
            unsigned q = f->element - 128u + bof;
            v->s.nat[q >= 128 ? q - IA64_N_STACKED_PHYS : q] = nat_value_of(x, f) & 1;
        }
        uint64_t pr = 0;
        for (f = end, end = base + p->plan[4]; f != end; ++f)
            pr |= (value_of(x, f) & 1) << (f->element - T1_ELEMENT_PR(0));
        if (p->pr_mask)
            v->s.pr = (v->s.pr & ~p->pr_mask) | pr;
        for (f = end, end = base + p->plan[5]; f != end; ++f)
            v->s.ar[AR_LC] = value_of(x, f);
        for (f = end, end = base + p->plan[6]; f != end; ++f)
            v->last_successful_bundle = value_of(x, f);
        for (f = end, end = base + p->frame_restore; f != end; ++f)
            t1_home_write_at(v, bof, f->element, value_of(x, f));
    }
#ifdef IA64VMM_HOSTED
    /* Every restored home holds its entry's value (PSR_MF: its bits). */
    for (f = base, end = base + p->frame_restore; f != end; ++f) {
        uint64_t want = value_of(x, f), home;
        unsigned e = f->element;
        if (e >= T1_ELEMENT_NAT(1) && e < T1_ELEMENT_LC)
            want &= 1;
        else if (e >= T1_ELEMENT_FR_META(2) && e <= T1_ELEMENT_FR_META(127))
            want &= 0x3ffff;
        if (!t1_home_read_at(v, bof, (uint16_t)e, &home) ||
            (e == T1_ELEMENT_PSR_MF ? (home & want & T1_PSR_MF_BITS) != (want & T1_PSR_MF_BITS)
                                    : home != want))
            hal_panic("tier1: restore plan left a home without its value");
    }
#endif
    if (r->frames)
        t1_point_frame(x, p, v);
    v->s.ip = p->ip;
    v->s.slot = p->slot;
    v->retired = x->retired_entry + t1_point_consumed(x, p);
}
