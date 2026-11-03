/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Injection schedules; see schedule.h. */
#include <string.h>
#include "tests/tier1/schedule.h"
#include "ia64/tlb.h"
#include "ia64vmm/guest_mem.h"

#define VA_PAYLOAD ((UINT64_C(1) << 61) - 1)
#define PA_LIMIT (UINT64_C(1) << (IA64_IMPL_PA_MSB + 1))

void schedule_clear(struct schedule *s)
{
    s->n = 0;
}

void schedule_add(struct schedule *s, struct injection in)
{
    T1H_CHECK(s->n < SCHEDULE_MAX);
    T1H_CHECK(in.kind != INJ_DMA || in.width == 1 || in.width == 2 || in.width == 4 ||
              in.width == 8);
    in.done = false;
    s->e[s->n++] = in;
}

void schedule_rearm(struct schedule *s)
{
    for (unsigned i = 0; i < s->n; ++i)
        s->e[i].done = false;
}

static void unmap(struct vcpu *v, uint64_t gva)
{
    struct ia64_state *s = &v->s;
    unsigned rid = (s->rr[gva >> 61] >> 8) & 0xffffff;
    /* What a guest ptc.l of the page does to the D-TC (SDM Vol.3 ptc.l),
     * logged as ptc.l logs it (hostwin's completeness check
     * found this purge unlogged); TR entries stay (UNMAP targets pages the
     * guest maps through the TC). */
    for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i) {
        struct ia64_tlb_entry *e = &s->tc[1][i];
        if (e->valid && e->rid == rid &&
            !((e->va ^ (gva & VA_PAYLOAD)) & ~ia64_low_mask(e->page_shift))) {
            e->valid = false;
            ia64_mmu_changed(v, IA64_MMU_TC, 1, i);
        }
    }
    /* Short-format VHPT entry (SDM Vol.2 §4.1.5.3): clear PTE.p so the walker
     * no longer refills the page. */
    if (!(s->cr[CR_PTA] & 1) || !(s->rr[gva >> 61] & 1) || (s->cr[CR_PTA] & 0x100))
        return;
    uint64_t hash = ia64_thash(s, gva);
    const struct ia64_tlb_entry *m = ia64_tlb_find(s, hash, 1);
    if (!m)
        return;
    gpa_t pa = (m->pte & (PA_LIMIT - 4096) & ~ia64_low_mask(m->page_shift)) |
               (hash & ia64_low_mask(m->page_shift));
    uint64_t pte;
    T1H_CHECK(gm_read(pa, &pte, 8));
    if (pte & 1) {
        pte &= ~UINT64_C(1);
        T1H_CHECK(gm_write(pa, &pte, 8));
    }
}

void inject_apply(struct vcpu *v, const struct injection *in)
{
    switch (in->kind) {
    case INJ_RAISE:
        vcpu_raise_external(v, (unsigned)in->address);
        break;
    case INJ_DMA: {
        /* Device DMA into backing RAM: no CPU-write notification, no store
         * record. */
        uint8_t *p = gm_direct_ptr(in->address, in->width);
        T1H_CHECK(p && !((uintptr_t)p & (in->width - 1)));
        switch (in->width) {
        case 1:
            *(volatile uint8_t *)p = (uint8_t)in->value;
            break;
        case 2:
            *(volatile uint16_t *)p = (uint16_t)in->value;
            break;
        case 4:
            *(volatile uint32_t *)p = (uint32_t)in->value;
            break;
        default:
            *(volatile uint64_t *)p = in->value;
            break;
        }
        break;
    }
    case INJ_UNMAP:
        unmap(v, in->address);
        break;
    case INJ_STAY_END:
        v->code_entry_generation = 0;
        break;
    }
}

unsigned schedule_apply_due(struct schedule *s, struct vcpu *v)
{
    unsigned applied = 0;
    if (!s)
        return 0;
    for (unsigned i = 0; i < s->n; ++i) {
        struct injection *in = &s->e[i];
        if (in->done)
            continue;
        T1H_CHECK(in->at != T1H_AT_PENDING);
        if (in->at < v->retired) {
            T1H_CHECK(!"injection boundary passed without applying it");
        }
        if (in->at == v->retired) {
            inject_apply(v, in);
            in->done = true;
            ++applied;
        }
    }
    return applied;
}

uint64_t schedule_next(const struct schedule *s, const struct vcpu *v)
{
    uint64_t next = UINT64_MAX;
    if (!s)
        return next;
    for (unsigned i = 0; i < s->n; ++i)
        if (!s->e[i].done && s->e[i].at > v->retired && s->e[i].at < next)
            next = s->e[i].at;
    return next;
}

bool schedule_complete(const struct schedule *s)
{
    if (!s)
        return true;
    for (unsigned i = 0; i < s->n; ++i)
        if (!s->e[i].done)
            return false;
    return true;
}

void schedule_resolve_pending(struct schedule *s, uint64_t retired)
{
    for (unsigned i = 0; i < s->n; ++i)
        if (s->e[i].at == T1H_AT_PENDING) {
            s->e[i].at = retired;
            s->e[i].done = true;
        }
}

void schedule_before_resolve(struct vcpu *v, void *arg)
{
    struct schedule_hook *hook = arg;
    struct injection in = hook->in;
    in.at = T1H_AT_PENDING;
    inject_apply(v, &in);
    schedule_add(hook->s, in);
    hook->s->e[hook->s->n - 1].done = true;
}

void schedule_apply_now(struct schedule *s, struct vcpu *v, struct injection in)
{
    in.at = v->retired;
    inject_apply(v, &in);
    schedule_add(s, in);
    s->e[s->n - 1].done = true;
}
