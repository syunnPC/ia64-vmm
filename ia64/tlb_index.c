/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/tlb.h"
#include "ia64/profile.h"
#include "ia64vmm/string.h"
#include "ia64vmm/hal.h"

#define NONE UINT16_MAX
_Static_assert(IA64_TLB_NODES <= UINT8_MAX, "page-size population fits its counter");
static unsigned hash(uint64_t va, unsigned rid, unsigned shift)
{
    uint64_t key = (va >> shift) ^ ((uint64_t)rid << 17) ^ shift;
    key ^= key >> 32;
    key *= UINT64_C(0x9e3779b97f4a7c15);
    return (unsigned)(key >> 56);
}
static const struct ia64_tlb_entry *entry(const struct vcpu *v, unsigned side, unsigned node)
{
    return node < IA64_ITR_COUNT ? &v->s.tr[side][node] : &v->s.tc[side][node - IA64_ITR_COUNT];
}
static void update(struct vcpu *v, unsigned side, unsigned node)
{
    struct ia64_tlb_index *m = &v->tlb_index;
    unsigned old = m->bucket[side][node];
    if (old != NONE) {
        uint16_t *link = &m->head[side][old];
        while (*link != node)
            link = &m->next[side][*link];
        *link = m->next[side][node];
        unsigned ps = m->shift[side][node];
        if (!--m->count[side][ps])
            m->shifts[side] &= ~(UINT64_C(1) << ps);
        m->bucket[side][node] = NONE;
    }
    const struct ia64_tlb_entry *e = entry(v, side, node);
    if (!e->valid)
        return;
    unsigned bucket = hash(e->va, e->rid, e->page_shift);
    m->next[side][node] = m->head[side][bucket];
    m->head[side][bucket] = node;
    m->bucket[side][node] = bucket;
    m->shift[side][node] = e->page_shift;
    ++m->count[side][e->page_shift];
    m->shifts[side] |= UINT64_C(1) << e->page_shift;
}
static void rebuild(struct vcpu *v)
{
    struct ia64_tlb_index *m = &v->tlb_index;
    memset(m, 0, sizeof(*m));
    memset(m->head, 0xff, sizeof(m->head));
    memset(m->bucket, 0xff, sizeof(m->bucket));
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned i = 0; i < ia64_model_tr_count(v->cfg.cpu_model, side); ++i)
            update(v, side, i);
        for (unsigned i = 0; i < ia64_model_tc_count(v->cfg.cpu_model, side); ++i)
            update(v, side, IA64_ITR_COUNT + i);
    }
    m->seq = v->mmu_seq;
    m->ready = true;
}
const struct ia64_tlb_entry *ia64_tlb_lookup(struct vcpu *v, uint64_t address, unsigned side)
{
    if (v->s.cpu_model != v->cfg.cpu_model)
        hal_panic("TLB CPU model differs from immutable configuration");
    struct ia64_tlb_index *m = &v->tlb_index;
    /* Lost history (including backward restore) is never replayed. RR/PKR
     * do not change entry locations; every query uses the current RID. */
    if (!m->ready || v->mmu_seq - m->seq > IA64_MMU_LOG)
        rebuild(v);
    else {
        for (uint64_t seq = m->seq; seq != v->mmu_seq; ++seq) {
            const struct ia64_mmu_change *c = &v->mmu_log[seq % IA64_MMU_LOG];
            if (c->kind == IA64_MMU_ALL) {
                rebuild(v);
                break;
            }
            if (c->kind == IA64_MMU_TC || c->kind == IA64_MMU_TR)
                update(v, c->side, c->index + (c->kind == IA64_MMU_TC ? IA64_ITR_COUNT : 0));
        }
        m->seq = v->mmu_seq;
    }
    unsigned rid = (v->s.rr[address >> 61] >> 8) & 0xffffff;
    uint64_t payload = address & ia64_low_mask(61), shifts = m->shifts[side];
    unsigned best = IA64_TLB_NODES;
    while (shifts) {
        unsigned ps = (unsigned)__builtin_ctzll(shifts);
        shifts &= shifts - 1;
        unsigned bucket = hash(payload & ~ia64_low_mask(ps), rid, ps);
        for (unsigned node = m->head[side][bucket]; node != NONE; node = m->next[side][node]) {
            const struct ia64_tlb_entry *e = entry(v, side, node);
            if (node < best && e->valid && e->page_shift == ps && e->rid == rid &&
                !((e->va ^ payload) & ~ia64_low_mask(ps)))
                best = node;
        }
    }
    /* Lowest node preserves associative ordering even across page sizes:
     * every TR precedes every TC, then increasing architectural slot. */
    return best == IA64_TLB_NODES ? NULL : entry(v, side, best);
}
