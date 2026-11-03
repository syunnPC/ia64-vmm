/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tag -> slot binding, LRU replacement and the bind epoch.
 * A slot is bound to a tag gva >> w
 * only; its shadow pages are derived lazily by hostwin_resolve. Rebinding
 * a slot unmaps its whole 2^w range and increments bind_epoch, the only
 * event that invalidates a (tag, delta) returned earlier.
 */
#include "engine/hostwin/internal.h"

/* mmu.c: implemented(); with IA64_IMPL_VA_MSB = 60 every payload is
 * implemented, but the check follows the macro. */
static bool implemented(uint64_t address)
{
    uint64_t hole = HW_VA_PAYLOAD & ~hw_low_mask(IA64_IMPL_VA_MSB + 1);
    return (address & hole) == ((address & (UINT64_C(1) << IA64_IMPL_VA_MSB)) ? hole : 0);
}

int hw_slot_of_tag(const struct hostwin_vcpu *hw, uint64_t tag)
{
    for (unsigned i = 0; i < hw->slots; ++i)
        if (hw->slot[i].bound && hw->slot[i].tag == tag)
            return (int)i;
    return -1;
}

uint64_t hostwin_bind_epoch(const struct hostwin_vcpu *hw)
{
    return hw->bind_epoch;
}

bool hostwin_window_pa(const struct hostwin_vcpu *hw, uint64_t gva, gpa_t *pa)
{
    int slot = hw_slot_of_tag(hw, gva >> hw->w);
    if (slot < 0)
        return false;
    uintptr_t host = hw_slot_base(hw, (unsigned)slot) + (gva & hw_low_mask(hw->w));
    hw_lock(hw);
    const struct hw_shadow *s = hw_shadow_find(hw, host & HW_PAGE_MASK);
    if (s)
        *pa = s->pa_page | (gva & (HW_PAGE - 1));
    hw_unlock(hw);
    return s != NULL;
}

static enum hostwin_status bind_slot(struct hostwin_vcpu *hw, const struct vcpu *v, uint64_t gva,
                                     struct hostwin_tag *out)
{
    if (v->s.psr & IA64_PSR_DT) {
        if (!implemented(gva))
            return HOSTWIN_SLOW;
    } else if ((gva >> 63) || (gva >> (IA64_IMPL_PA_MSB + 1))) {
        /* Physical: bit 63 is the UC alias (A.6), and pa >= 2^50 is an
         * unimplemented data address; neither is ever direct. */
        return HOSTWIN_SLOW;
    }
    uint64_t tag = gva >> hw->w;
    int found = hw_slot_of_tag(hw, tag);
    unsigned i;
    if (found >= 0) {
        i = (unsigned)found;
    } else {
        i = 0;
        for (unsigned k = 0; k < hw->slots; ++k) {
            if (!hw->slot[k].bound) {
                i = k;
                break;
            }
            if (hw->slot[k].lru < hw->slot[i].lru)
                i = k;
        }
        if (hw->slot[i].bound) {
            /* The old tag's range must hold no mapping once the slot serves
             * another tag: drop (panics if the backend cannot unmap). */
            hw_shadows_drop_slot(hw, i);
            ++hw->bind_epoch;
            ++hw->stats.rebinds;
        }
        hw->slot[i] = (struct hw_slot){.tag = tag, .bound = true};
        ++hw->stats.binds;
    }
    hw->slot[i].lru = ++hw->lru_clock;
    *out =
        (struct hostwin_tag){.tag = tag, .delta = (uint64_t)hw_slot_base(hw, i) - (tag << hw->w)};
    return HOSTWIN_OK;
}

enum hostwin_status hostwin_bind(struct hostwin_vcpu *hw, const struct vcpu *v, uint64_t gva,
                                 struct hostwin_tag *out)
{
    hw_lock(hw);
    enum hostwin_status status = bind_slot(hw, v, gva, out);
    hw_unlock(hw);
    return status;
}
