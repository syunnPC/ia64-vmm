/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Point, frame and location tables of a compiled region: the struct t1_point of every live point with its frame
 * (struct t1_frame_entry: element and location), the RIP table the
 * trap client searches, the helper table, the homes of the
 * architectural elements in struct vcpu, and the entry image of the stacked LIVEINs with its per-entry fill.
 */
#include "engine/tier1/region.h"
#include "engine/tier1/helpers.h"
#include "ia64vmm/hal.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/string.h"

bool t1_element_home(uint16_t element, uint32_t *offset, unsigned *bit, unsigned *bytes)
{
    *bit = 0;
    *bytes = 8;
    /* A stacked home moves with the frame base: no fixed
     * offset exists, and generated code must not address one. */
    if (t1_element_stacked(element))
        return false;
    if (element >= 1 && element < 128) {
        *offset = (uint32_t)(offsetof(struct vcpu, s.gr) + 8u * element);
    } else if (element >= T1_ELEMENT_NAT(1) && element < T1_ELEMENT_PR(0)) {
        *offset = (uint32_t)(offsetof(struct vcpu, s.nat) + (element - 128u));
        *bytes = 1;
    } else if (element >= T1_ELEMENT_PR(1) && element < T1_ELEMENT_LC) {
        *offset = (uint32_t)offsetof(struct vcpu, s.pr);
        *bit = element - 256u;
    } else if (element == T1_ELEMENT_LC) {
        *offset = (uint32_t)(offsetof(struct vcpu, s.ar) + 8u * AR_LC);
    } else if (element == T1_ELEMENT_LSB) {
        *offset = (uint32_t)offsetof(struct vcpu, last_successful_bundle);
    } else if (element >= T1_ELEMENT_FR(2) && element <= T1_ELEMENT_FR(127)) {
        *offset =
            (uint32_t)(offsetof(struct vcpu, s.fr) + sizeof(struct ia64_fr) * (element - 323u));
    } else if (element >= T1_ELEMENT_FR_META(2) && element <= T1_ELEMENT_FR_META(127)) {
        *offset =
            (uint32_t)(offsetof(struct vcpu, s.fr) + sizeof(struct ia64_fr) * (element - 452u) +
                       offsetof(struct ia64_fr, exponent));
    } else if (element == T1_ELEMENT_PSR_MF) {
        *offset = (uint32_t)offsetof(struct vcpu, s.psr);
    } else if (element == T1_ELEMENT_PFS) {
        *offset = (uint32_t)(offsetof(struct vcpu, s.ar) + 8u * AR_PFS);
    } else if (element == T1_ELEMENT_EC) {
        *offset = (uint32_t)(offsetof(struct vcpu, s.ar) + 8u * AR_EC);
    } else if (element >= T1_ELEMENT_BR(0) && element <= T1_ELEMENT_BR(7)) {
        *offset = (uint32_t)(offsetof(struct vcpu, s.br) + 8u * (element - T1_ELEMENT_BR(0)));
    } else {
        return false;
    }
    return true;
}

/* The restore class of a frame entry's element (struct t1_point plan). */
static unsigned restore_class(uint16_t e)
{
    if (e >= 1 && e < 32)
        return 0;
    if (e >= 32 && e < 128)
        return 1;
    if (e >= T1_ELEMENT_NAT(1) && e < T1_ELEMENT_NAT(32))
        return 2;
    if (e >= T1_ELEMENT_NAT(32) && e < T1_ELEMENT_PR(0))
        return 3;
    if (e >= T1_ELEMENT_PR(1) && e < T1_ELEMENT_LC)
        return 4;
    if (e == T1_ELEMENT_LC)
        return 5;
    if (e == T1_ELEMENT_LSB)
        return 6;
    return 7;
}

static void *carve(uint8_t **p, size_t bytes)
{
    void *r = *p;
    *p += (bytes + 15) & ~(size_t)15;
    return r;
}

static void sort_rips(struct t1_rip *r, unsigned n)
{
    for (unsigned i = 1; i < n; ++i) {
        struct t1_rip x = r[i];
        unsigned j = i;
        while (j && r[j - 1].offset > x.offset) {
            r[j] = r[j - 1];
            --j;
        }
        r[j] = x;
    }
}

/* The entry image: slot k <- the home of the region's k-th
 * stacked LIVEIN at v's frame base. When no slot wraps the ring at this BOF
 * and none is a NaT (frame_in_span, t1_frame_build) it is a gather from one
 * base; otherwise each offset is reduced modulo 96 on its own. */
void t1_frame_in_fill(struct t1_exec *x, const struct t1_region *r, const struct vcpu *v)
{
    unsigned n = r->nframe_in, bof = v->s.rse_bof, span = r->frame_in_span;
    if (!n)
        return;
    const uint8_t *slot = r->frame_in;
    uint64_t *out = x->frame_in;
    if (span <= IA64_N_STACKED_PHYS && bof <= IA64_N_STACKED_PHYS - span) {
        const uint64_t *gr = v->s.gr + 32 + bof;
        for (unsigned k = 0; k < n; ++k)
            out[k] = gr[slot[k]];
        return;
    }
    bof %= IA64_N_STACKED_PHYS; /* rse_bof < 96 (ia64/rse.c); kept exact anyway */
    const uint64_t *gr = v->s.gr + 32;
    const bool *nat = v->s.nat + 32;
    for (unsigned k = 0; k < n; ++k) {
        unsigned q = bof + (slot[k] & ~T1_FRAME_IN_NAT);
        q = q >= IA64_N_STACKED_PHYS ? q - IA64_N_STACKED_PHYS : q;
        out[k] = (slot[k] & T1_FRAME_IN_NAT) ? (uint64_t)nat[q] : gr[q];
    }
}

/* The write set of a helper point's instruction, written back after the
 * call (helper.c): a generalized helper's (with a helpers.c row), none for
 * tnat, whose results are wr[]. */
static unsigned helper_writes(const struct t1_ir *ir, const struct t1_op *op, uint16_t *elements)
{
    const struct t1_insn *ti = &ir->form->insn[op->imm];
    return t1_helper_row(ti->insn.opcode) ? t1_helper_elements(ti, true, elements) : 0;
}

/* nreads: the helpers' read operands, which bound their read_frame
 * entries (uses after the instruction's own, t1_frame_build); nwrites:
 * the elements of their write sets. */
struct table_counts {
    unsigned np, nframe, nsites, nhelpers, nreads, nwrites, ncheck;
};
static struct table_counts table_counts(const struct t1_mir *m)
{
    struct table_counts n = {.np = m->ir->npoints};
    if (m->flags & T1_COMPILE_ACCESS_LOG)
        n.ncheck = m->ir->form->ninsns;
    for (unsigned p = 0; p < n.np; ++p) {
        const struct t1_mir_point *mp = &m->point[p];
        if (mp->insn == T1_MIR_NONE)
            continue;
        n.nframe += mp->frame_count;
        if (mp->kind == T1_PT_SITE)
            ++n.nsites;
        if (mp->kind == T1_PT_HELPER) {
            const struct t1_op *op = &m->ir->op[m->ir->point[p].op];
            uint16_t elements[T1_N_ELEMENTS];
            ++n.nhelpers;
            n.nreads += op->nuse ? op->nuse - 1u : 0u;
            n.nwrites += helper_writes(m->ir, op, elements);
        }
    }
    return n;
}

static size_t table_pages(struct table_counts n, const struct t1_emitted *e)
{
    size_t bytes =
        n.np * sizeof(struct t1_point) + n.nframe * sizeof(struct t1_frame_entry) +
        3 * n.nsites * sizeof(struct t1_rip) + n.nhelpers * sizeof(struct t1_helper_info) +
        n.nreads * sizeof(uint32_t) + n.nwrites * sizeof(uint16_t) + n.np * sizeof(uint16_t) +
        e->nabs * sizeof(struct t1_fixup) + n.ncheck * sizeof(struct t1_check_insn) + 224;
    return (bytes + 4095) / 4096;
}

size_t t1_frame_pages(const struct t1_mir *m, const struct t1_emitted *e)
{
    return table_pages(table_counts(m), e);
}

bool t1_frame_build(struct t1_region *r, const struct t1_mir *m, const struct t1_emitted *e)
{
    const struct t1_ir *ir = m->ir;
    /* r->nat_guard is the IR's G, set before the tables (compile.c). */
    t1_nat_guard_list(r);
    if (e->nframe_in > T1_MAX_FRAME_IN)
        return false;
    r->nframe_in = e->nframe_in;
    r->frame_in_span = 0;
    for (unsigned k = 0; k < e->nframe_in; ++k) {
        uint16_t el = e->frame_in[k];
        if (!t1_element_stacked(el))
            return false;
        bool nat = el >= T1_ELEMENT_NAT(32);
        unsigned o = nat ? el - T1_ELEMENT_NAT(32) : el - T1_ELEMENT_GR(32);
        r->frame_in[k] = (uint8_t)(o | (nat ? T1_FRAME_IN_NAT : 0));
        /* A NaT slot takes the general path of t1_frame_in_fill. */
        if (nat)
            r->frame_in_span = UINT8_MAX;
        else if (o + 1 > r->frame_in_span)
            r->frame_in_span = (uint8_t)(o + 1);
    }
    struct table_counts n = table_counts(m);
    unsigned np = ir->npoints, nframe = n.nframe, nsites = n.nsites, nhelpers = n.nhelpers;
    r->meta_pages = table_pages(n, e);
    uint8_t *p = t1_memory_alloc(r->meta_pages, HAL_PAGE_ZERO);
    if (!p)
        return false;
    r->meta = p;
    r->point = carve(&p, np * sizeof(struct t1_point));
    r->frame = carve(&p, nframe * sizeof(struct t1_frame_entry));
    r->rip = carve(&p, 3 * nsites * sizeof(struct t1_rip));
    r->helper = carve(&p, nhelpers * sizeof(struct t1_helper_info));
    uint32_t *reads = carve(&p, n.nreads * sizeof(uint32_t));
    uint16_t *writes = carve(&p, n.nwrites * sizeof(uint16_t));
    r->helper_of = carve(&p, np * sizeof(uint16_t));
    r->fix = carve(&p, e->nabs * sizeof(struct t1_fixup));
    if (n.ncheck) {
        r->check_insn = carve(&p, n.ncheck * sizeof(struct t1_check_insn));
        for (unsigned k = 0; k < n.ncheck; ++k) {
            const struct t1_insn *ti = &ir->form->insn[k];
            r->check_insn[k].insn = ti->insn;
            r->check_insn[k].steps = (ti->next_slot ? ti->next_slot : 3) - ti->insn.slot;
            r->check_insn[k].memory_base =
                ti->cls == T1_IC_LOAD || ti->cls == T1_IC_STORE ? ti->insn.operands.memory.base : 0;
        }
    }
    uintptr_t stub_base = t1_stub.exit_save;
    for (unsigned k = 0; k < e->nabs; ++k) {
        /* Unsigned subtraction and roundtrip also handle negative offsets
         * without overflowing a signed pointer-sized intermediate. */
        uintptr_t delta = e->abs_target[k] - stub_base;
        int32_t offset = (int32_t)delta;
        if ((uintptr_t)(intptr_t)offset != delta)
            return false;
        r->fix[k] = (struct t1_fixup){e->abs_fix[k], offset};
    }
    r->nfix = e->nabs;
    r->npoints = np;
    r->site_base = e->site_base;
    r->native_entry = e->native_entry;
    r->frame_in_offset = e->frame_in_offset;
    r->nframe = r->nrip = r->nhelper = r->nlive_points = 0;
    r->fast_points = 0;
    for (unsigned q = 0; q < np; ++q) {
        const struct t1_mir_point *mp = &m->point[q];
        const struct t1_ir_point *ip = &ir->point[q];
        struct t1_point *pt = &r->point[q];
        r->helper_of[q] = UINT16_MAX;
        if (mp->insn == T1_MIR_NONE) {
            pt->kind = T1_DEAD_POINT;
            continue;
        }
        ++r->nlive_points;
        const struct t1_emit_point *ep = &e->pt[q];
        pt->kind = ip->kind;
        pt->timing = ip->kind == T1_PT_SITE || ip->kind == T1_PT_HELPER ? T1_BEFORE_EFFECT
                                                                        : T1_AFTER_COMPLETION;
        pt->access_size = ip->access_size;
        pt->access_write = ip->access_write;
        pt->addr_reg = ep->addr_reg;
        pt->end = ip->kind == T1_PT_EXIT ? ip->end : T1_END_LIMIT;
        pt->cfm = ir->form ? ir->form->fs[ip->fs].cfm : 0;
        pt->delta = ir->form ? ir->form->fs[ip->fs].delta : 0;
        pt->rse = ip->fs != 0;
        pt->site = ip->site;
        pt->host_offset = ep->host_offset;
        pt->force_offset = ep->force_offset;
        pt->force_resume = ep->force_resume;
        pt->pre_offset = ep->pre_offset;
        pt->restore_offset = ep->restore_offset;
        pt->ip = ip->ip;
        pt->slot = ip->slot;
        pt->retired_offset = ip->retired_offset;
        pt->insn = ip->insn;
        pt->frame_first = r->nframe;
        pt->frame_count = mp->frame_count;
        /* Entries to restore first, by restore class (the plan; frame
         * classes name disjoint homes, so the order does not change the
         * restored state), then the HOME entries, which
         * materialization skips without reading. */
        pt->pr_mask = 0;
        unsigned count[9] = {0}, next[9];
        for (unsigned k = 0; k < mp->frame_count; ++k) {
            const struct t1_loc *loc = &m->frame_loc[mp->frame_first + k];
            uint16_t element = m->frame[mp->frame_first + k].element;
            unsigned cls = loc->kind == T1_LOC_HOME ? 8 : restore_class(element);
            ++count[cls];
            if (cls == 4)
                pt->pr_mask |= UINT64_C(1) << (element - T1_ELEMENT_PR(0));
        }
        if (q < 32 && pt->kind == T1_PT_EXIT && !pt->retired_offset && !pt->rse && !pt->delta &&
            pt->restore_offset)
            r->fast_points |= UINT32_C(1) << q;
        for (unsigned pass = 0; pass <= 8; ++pass) {
            next[pass] = r->nframe;
            r->nframe += count[pass];
            if (pass < 7)
                pt->plan[pass] = (uint16_t)(r->nframe - pt->frame_first);
            if (pass == 7)
                pt->frame_restore = r->nframe - pt->frame_first;
        }
        /* Stable scatter: retain the original order within each class
         * with two frame traversals instead of one per restore class. */
        for (unsigned k = 0; k < mp->frame_count; ++k) {
            const struct t1_loc *loc = &m->frame_loc[mp->frame_first + k];
            uint16_t element = m->frame[mp->frame_first + k].element;
            unsigned cls = loc->kind == T1_LOC_HOME ? 8 : restore_class(element);
            struct t1_frame_entry *f = &r->frame[next[cls]++];
            *f = t1_frame_entry_make(element, loc);
        }
        if (ip->kind == T1_PT_SITE) {
            r->rip[r->nrip++] = (struct t1_rip){pt->host_offset, q, T1_RIP_ACCESS};
            if (ep->fast_offset)
                r->rip[r->nrip++] = (struct t1_rip){ep->fast_offset, q, T1_RIP_ACCESS};
            if (m->flags & T1_COMPILE_VERIFY)
                r->rip[r->nrip++] = (struct t1_rip){pt->force_offset, q, T1_RIP_FORCE};
            if (pt->pre_offset)
                r->rip[r->nrip++] = (struct t1_rip){pt->pre_offset, q, T1_RIP_PRE};
        }
        if (ip->kind == T1_PT_HELPER) {
            const struct t1_op *op = &ir->op[ip->op];
            struct t1_helper_info *h = &r->helper[r->nhelper];
            r->helper_of[q] = (uint16_t)r->nhelper++;
            h->point = q;
            h->insn = ir->form->insn[op->imm].insn;
            h->nreads = 0;
            h->read_frame = reads;
            reads += op->nuse ? op->nuse - 1u : 0u;
            for (unsigned k = 1; k < op->nuse; ++k) {
                uint16_t el = ir->use[op->use_first + k].element;
                bool found = false;
                for (unsigned f = 0; f < pt->frame_count && !found; ++f)
                    if (r->frame[pt->frame_first + f].element == el) {
                        if (r->frame[pt->frame_first + f].kind == T1_LOC_HOME) {
                            found = true;
                            break;
                        }
                        h->read_frame[h->nreads++] = pt->frame_first + f;
                        found = true;
                    }
                if (!found)
                    return false; /* every cached read has a BEFORE frame entry */
            }
            h->writes = writes;
            h->nwrites = (uint16_t)helper_writes(ir, op, writes);
            writes += h->nwrites;
            h->wr[0] = op->wr[0];
            h->wr[1] = op->wr[1];
            const struct t1_mir_insn *mi = &m->insn[mp->insn];
            for (unsigned d = 0; d < 2; ++d)
                h->def_reg[d] = mi->opnd[d].kind == T1_MO_VREG ? mi->opnd[d].fixed : T1_X86_NOREG;
        }
    }
    sort_rips(r->rip, r->nrip);
    return true;
}
