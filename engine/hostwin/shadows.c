/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shadow-page records and their reverse maps:
 * by host page (fills, upgrades, test drops), by PA page (code-page
 * events), and by D-side TLB entry (selective invalidation).
 *
 * Backend failures (hosted unmap/protect need a VMA when
 * they carve a page out of a larger mapping, so they can fail with ENOMEM
 * at vm.max_map_count) are handled by removing more, never by keeping an
 * alias: protect_ro fails -> unmap the page; unmap of a page fails ->
 * unmap the whole slot (whole-range replacement only splits VMAs at its
 * ends); that fails -> hal_panic with the statistics. The per-instance cap
 * HOSTWIN_MAX_SHADOWS bounds the VMAs a reservation can need.
 */
#include "engine/hostwin/internal.h"
#include "ia64vmm/hal.h"

static unsigned host_bucket(uintptr_t host_page)
{
    return (host_page >> 12) & (HW_HASH_BUCKETS - 1);
}
static unsigned pa_bucket(gpa_t pa_page)
{
    return ((pa_page >> 12) ^ (pa_page >> 24)) & (HW_HASH_BUCKETS - 1);
}

#define LINK(s, head, field)                                                                       \
    do {                                                                                           \
        (s)->field##_next = *(head);                                                               \
        (s)->field##_prev = (head);                                                                \
        if (*(head))                                                                               \
            (*(head))->field##_prev = &(s)->field##_next;                                          \
        *(head) = (s);                                                                             \
    } while (0)
#define UNLINK(s, field)                                                                           \
    do {                                                                                           \
        *(s)->field##_prev = (s)->field##_next;                                                    \
        if ((s)->field##_next)                                                                     \
            (s)->field##_next->field##_prev = (s)->field##_prev;                                   \
        (s)->field##_next = NULL;                                                                  \
        (s)->field##_prev = NULL;                                                                  \
    } while (0)

bool hw_shadows_init(struct hostwin_vcpu *hw)
{
    size_t bytes = (size_t)HOSTWIN_MAX_SHADOWS * sizeof(struct hw_shadow);
    hw->pool_pages = (bytes + HW_PAGE - 1) / HW_PAGE;
    hw->pool = hal_alloc_pages(hw->pool_pages, HAL_PAGE_ZERO);
    if (!hw->pool)
        return false;
    hw->free = NULL;
    for (unsigned i = HOSTWIN_MAX_SHADOWS; i-- > 0;) {
        hw->pool[i].live_next = hw->free;
        hw->free = &hw->pool[i];
    }
    return true;
}

void hw_shadows_fini(struct hostwin_vcpu *hw)
{
    hal_free_pages(hw->pool, hw->pool_pages);
    hw->pool = hw->free = hw->live = NULL;
    hw->live_count = 0;
}

static struct hw_shadow **source_head(struct hostwin_vcpu *hw, unsigned source, unsigned index)
{
    if (source == HW_SOURCE_TR)
        return &hw->by_tr[index];
    if (source == HW_SOURCE_TC)
        return &hw->by_tc[index];
    return NULL;
}

struct hw_shadow *hw_shadow_find(const struct hostwin_vcpu *hw, uintptr_t host_page)
{
    for (struct hw_shadow *s = hw->host_hash[host_bucket(host_page)]; s; s = s->host_next)
        if (s->host_page == host_page)
            return s;
    return NULL;
}

/* Removes the record only; the caller has removed or replaced the mapping. */
static void forget(struct hostwin_vcpu *hw, struct hw_shadow *s)
{
    struct hw_shadow **h = &hw->host_hash[host_bucket(s->host_page)];
    while (*h != s)
        h = &(*h)->host_next;
    *h = s->host_next;
    UNLINK(s, pa);
    if (s->src_prev)
        UNLINK(s, src);
    UNLINK(s, live);
    --hw->slot[s->slot].shadows;
    --hw->live_count;
    ++hw->stats.shadow_drops;
    s->live_next = hw->free;
    hw->free = s;
}

void hw_shadow_record(struct hostwin_vcpu *hw, const struct hw_shadow *fields)
{
    struct hw_shadow *old = hw_shadow_find(hw, fields->host_page);
    if (old) {
        /* map() replaced the old mapping in place (write-fault upgrade). */
        forget(hw, old);
        --hw->stats.shadow_drops;
    }
    struct hw_shadow *s = hw->free;
    if (!s)
        hal_panic("hostwin: shadow pool exhausted below the cap");
    hw->free = s->live_next;
    *s = *fields;
    s->src_next = NULL;
    s->src_prev = NULL;
    s->host_next = hw->host_hash[host_bucket(s->host_page)];
    hw->host_hash[host_bucket(s->host_page)] = s;
    LINK(s, &hw->pa_hash[pa_bucket(s->pa_page)], pa);
    struct hw_shadow **head = source_head(hw, s->source, s->index);
    if (head)
        LINK(s, head, src);
    LINK(s, &hw->live, live);
    ++hw->slot[s->slot].shadows;
    ++hw->live_count;
}

/* Unmaps the whole slot range and forgets its records; the binding stays. */
void hw_shadows_drop_slot(struct hostwin_vcpu *hw, unsigned slot)
{
    if (!hw->slot[slot].shadows)
        return;
    uintptr_t base = hw_slot_base(hw, slot);
    if (!hw->backend->unmap(hw->ctx, base, (uint64_t)1 << hw->w))
        hw_panic(hw, "unmap of a whole slot", base);
    hw->backend->flush(hw->ctx, HOSTWIN_FLUSH_ALL, false);
    for (struct hw_shadow *s = hw->live, *next; s; s = next) {
        next = s->live_next;
        if (s->slot == slot)
            forget(hw, s);
    }
}

bool hw_shadow_drop(struct hostwin_vcpu *hw, struct hw_shadow *s)
{
    if (hw->backend->unmap(hw->ctx, s->host_page, HW_PAGE)) {
        hw->backend->flush(hw->ctx, s->host_page, false);
        forget(hw, s);
        return true;
    }
    ++hw->stats.backend_failures;
    hw_shadows_drop_slot(hw, s->slot);
    return false;
}

void hw_shadows_drop_all(struct hostwin_vcpu *hw)
{
    for (unsigned i = 0; i < hw->slots; ++i)
        hw_shadows_drop_slot(hw, i);
}

void hw_shadows_drop_source(struct hostwin_vcpu *hw, enum hw_source source, unsigned index)
{
    struct hw_shadow **head = source_head(hw, source, index);
    /* A drop may fall back to a whole slot, removing other list members
     * too, so restart from the head after every drop. */
    while (head && *head)
        hw_shadow_drop(hw, *head);
}

/* tlb.c: matches(E, S.gva_page, S.rid, 12) -- E.valid, same RID, and equal
 * above max(E.page_shift, 12) in the payload form. */
static bool lies_in(const struct hw_shadow *s, const struct ia64_tlb_entry *e)
{
    unsigned shift = e->page_shift > 12 ? e->page_shift : 12;
    return e->valid && e->rid == s->rid && !((e->va ^ s->gva_page) & ~hw_low_mask(shift));
}

void hw_shadows_drop_in_entry(struct hostwin_vcpu *hw, const struct ia64_tlb_entry *entry)
{
    struct hw_shadow *s = hw->live;
    while (s) {
        struct hw_shadow *next = s->live_next;
        if (s->source != HW_SOURCE_PHYSICAL && lies_in(s, entry) && !hw_shadow_drop(hw, s))
            next = hw->live; /* a whole slot went: restart */
        s = next;
    }
}

void hw_shadows_protect_pa(struct hostwin_vcpu *hw, gpa_t pa_page)
{
    struct hw_shadow *s = hw->pa_hash[pa_bucket(pa_page)];
    while (s) {
        struct hw_shadow *next = s->pa_next;
        if (s->pa_page == pa_page && s->writable) {
            if (hw->backend->protect_ro(hw->ctx, s->host_page)) {
                s->writable = false;
                hw->backend->flush(hw->ctx, s->host_page, true);
            } else {
                ++hw->stats.backend_failures;
                /* Removing the alias is as good as protecting it. */
                if (!hw_shadow_drop(hw, s))
                    next = hw->pa_hash[pa_bucket(pa_page)];
                hw->backend->flush(hw->ctx, HOSTWIN_FLUSH_ALL, true);
            }
        }
        s = next;
    }
}

void hostwin_invalidate(struct hostwin_vcpu *hw)
{
    hw_lock(hw);
    hw_shadows_drop_all(hw);
    hw_unlock(hw);
}

#ifdef IA64VMM_HOSTED
void hostwin_test_drop(struct hostwin_vcpu *hw, uint64_t gva)
{
    int slot = hw_slot_of_tag(hw, gva >> hw->w);
    if (slot < 0)
        return;
    uintptr_t host_page =
        (hw_slot_base(hw, (unsigned)slot) + (gva & hw_low_mask(hw->w))) & HW_PAGE_MASK;
    hw_lock(hw);
    struct hw_shadow *s = hw_shadow_find(hw, host_page);
    if (s)
        hw_shadow_drop(hw, s);
    hw_unlock(hw);
}
#endif
