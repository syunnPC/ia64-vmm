/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Check fills in SDM priority order using the original gva. Mapping uses
 * page addresses; the lower tier executes accesses or delivers guest faults. */
#include "engine/hostwin/internal.h"
#include "ia64/mmu.h"
#include "engine/cache/cache.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"

static enum hostwin_fill leave(struct hostwin_vcpu *hw, enum hostwin_exit_reason reason,
                               enum hostwin_exit_reason *why)
{
    ++hw->stats.exits[reason];
    *why = reason;
    return HOSTWIN_EXIT;
}

/* Require an entire, host-aligned RAM page. gm_lookup bypasses memory hooks. */
static bool eligible(gpa_t pa, gpa_t pa_page, enum hostwin_exit_reason *reason)
{
    const struct gm_region *r = gm_lookup(pa_page);
    if (r && r->kind == GM_RAM && pa_page >= r->base && pa_page + HW_PAGE - r->base <= r->size &&
        !(((uintptr_t)r->hva + (uintptr_t)(pa_page - r->base)) & (HW_PAGE - 1)))
        return true;
    /* The reason classifies the accessed byte: RAM in a page that is not
     * wholly direct is PARTIAL_PAGE, anything else NOT_RAM. */
    const struct gm_region *at = gm_lookup(pa);
    *reason = at && at->kind == GM_RAM ? HW_EXIT_PARTIAL_PAGE : HW_EXIT_NOT_RAM;
    return false;
}

/* TR and TC are separate arrays; ordering their pointers is undefined
 * (C11 §6.5.8p5), so identify entries by equality. */
static bool entry_source(const struct ia64_state *s, const struct ia64_tlb_entry *e,
                         struct hw_shadow *fields)
{
    for (unsigned i = 0; i < IA64_DTR_COUNT; ++i)
        if (e == &s->tr[1][i]) {
            fields->source = HW_SOURCE_TR;
            fields->index = (uint8_t)i;
            return true;
        }
    for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i)
        if (e == &s->tc[1][i]) {
            fields->source = HW_SOURCE_TC;
            fields->index = (uint8_t)i;
            return true;
        }
    return false;
}

/* The fill decision with the instance lock held: a TRACKED event either
 * protects the alias recorded here or precedes the code check below. */
static enum hostwin_fill resolve(struct hostwin_vcpu *hw, struct vcpu *v, uint64_t gva,
                                 unsigned size, bool write, enum hostwin_exit_reason *why)
{
    /* Checks without side effects first (hostwin.h): the injector, a stale
     * tag, a malformed access. */
    if (hw->inject_left && !--hw->inject_left)
        return leave(hw, HW_EXIT_INJECTED, why);
    int slot = hw_slot_of_tag(hw, gva >> hw->w);
    if (slot < 0)
        return leave(hw, HW_EXIT_STALE_SLOT, why);
    if ((size != 1 && size != 2 && size != 4 && size != 8) || (gva & (size - 1)))
        return leave(hw, HW_EXIT_GUEST_FAULT, why);

    uintptr_t host_page =
        (hw_slot_base(hw, (unsigned)slot) + (gva & hw_low_mask(hw->w))) & HW_PAGE_MASK;
    unsigned cpl = (unsigned)(v->s.psr >> 32) & 3;
    bool virt = (v->s.psr & IA64_PSR_DT) != 0;
    gpa_t pa;
    bool readable = true, writable = true;
    struct hw_shadow fields = {
        .host_page = host_page, .slot = (uint8_t)slot, .source = HW_SOURCE_PHYSICAL};
    if (virt) {
        /* Translate once, including any VHPT walk and TC insertion. */
        struct ia64_translation t;
        ++hw->stats.translate_calls;
        struct ia64_result r = ia64_translate(
            v, gva, IA64_REF_DATA, write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ, cpl, false, &t);
        /* The walker's insertion (and its purges) are logged context
         * changes; the shadow recorded below belongs to v->mmu_seq now. */
        hw_context_update(hw, v);
        if (r.kind != IA64_STEP_COMPLETE || t.deferred)
            return leave(hw, HW_EXIT_GUEST_FAULT, why);
        if (t.ma != IA64_MA_WB)
            return leave(hw, HW_EXIT_ATTRIBUTE, why);
        pa = t.pa;
        const struct ia64_tlb_entry *e = t.entry;
        if (!e)
            hal_panic("hostwin_resolve: completed translation without a D-side entry");
        /* PSR.da grants transient access, never shadow permissions. */
        unsigned lasting = ia64_lasting_rights(&v->s, e, cpl);
        readable = lasting & IA64_ACCESS_READ;
        writable = lasting & IA64_ACCESS_WRITE;
        if (!readable || (write && !writable))
            return leave(hw, HW_EXIT_PERMISSION_SPLIT, why);
        if (!entry_source(&v->s, e, &fields))
            hal_panic("hostwin_resolve: D-side entry outside the TR and TC arrays");
        fields.gva_page = gva & HW_VA_PAYLOAD & HW_PAGE_MASK;
        fields.rid = (uint32_t)((v->s.rr[gva >> 61] >> 8) & 0xffffff);
    } else {
        /* Physical: WB without consulting an attribute (A.6: bit 63 clear
         * is WB). bind refuses bit-63 and >= 2^50 tags; a caller that
         * bypassed it still never gets a shadow. */
        hw_context_update(hw, v);
        if (gva >> 63)
            return leave(hw, HW_EXIT_ATTRIBUTE, why);
        if (gva >> (IA64_IMPL_PA_MSB + 1))
            return leave(hw, HW_EXIT_GUEST_FAULT, why);
        pa = gva;
    }
    gpa_t pa_page = pa & HW_PAGE_MASK;
    enum hostwin_exit_reason reason;
    if (!eligible(pa, pa_page, &reason))
        return leave(hw, reason, why);
    /* The tracked bitmap avoids taking code_lock under the instance lock.
     * Without code validity there is no tracked set. */
    if (!code_validity_active()) {
        hw->stop_detail = "hostwin: window fill without code validity";
        vcpu_request_exit(v, VCPU_EXIT_BAD_ACCESS);
        return leave(hw, HW_EXIT_BACKEND, why);
    }
    bool code = code_page_tracked_fast(pa_page);
    if (write && code)
        return leave(hw, HW_EXIT_CODE_PAGE_STORE, why);
    /* Reclaim before mapping so this page survives until the retry. */
    bool writable_map = writable && !code;
    if (hw->live_count >= HOSTWIN_MAX_SHADOWS && !hw_shadow_find(hw, host_page))
        hw_shadows_drop_all(hw);
    enum hostwin_map_status status = hw->backend->map(hw->ctx, host_page, pa_page, writable_map);
    bool retried = status == HOSTWIN_MAP_EXHAUSTED;
    if (retried) {
        ++hw->stats.backend_failures;
        LOGW("hostwin: vCPU%u shadow pool exhausted; dropping window and retrying once",
             v->cfg.index);
        hw_shadows_drop_all(hw);
        /* A failed walk may have installed empty tables without a shadow
         * record. Remove the entire reservation before retrying, too. */
        if (!hw->backend->unmap(hw->ctx, hw->base, (uint64_t)hw->slots << hw->w))
            hw_panic(hw, "exhaustion cleanup", host_page);
        hw->backend->flush(hw->ctx, HOSTWIN_FLUSH_ALL, true);
        status = hw->backend->map(hw->ctx, host_page, pa_page, writable_map);
    }
    if ((retried && status != HOSTWIN_MAP_OK) || status == HOSTWIN_MAP_WRONG_CPU) {
        ++hw->stats.backend_failures;
        hw->stop_detail = status == HOSTWIN_MAP_WRONG_CPU
                              ? "hostwin: fill on a host CPU other than the window context's"
                          : status == HOSTWIN_MAP_EXHAUSTED
                              ? "hostwin: shadow table pool exhausted after one retry"
                              : "hostwin: map failed after shadow pool exhaustion retry";
        LOGE("hostwin: stopping vCPU%u: %s (host page=%llx)", v->cfg.index, hw->stop_detail,
             (unsigned long long)host_page);
        vcpu_request_exit(v, VCPU_EXIT_BAD_ACCESS);
        return leave(hw, HW_EXIT_BACKEND, why);
    }
    switch (status) {
    case HOSTWIN_MAP_OK:
        break;
    case HOSTWIN_MAP_UNBACKED: {
        /* A failed map may have left an earlier shadow of this page in an
         * unknown state: remove it. */
        struct hw_shadow *old = hw_shadow_find(hw, host_page);
        if (old)
            hw_shadow_drop(hw, old);
        return leave(hw, HW_EXIT_UNBACKED, why);
    }
    default:
        hw_panic(hw, "map", host_page);
    }
    fields.pa_page = pa_page;
    fields.writable = writable_map;
    hw_shadow_record(hw, &fields);
    ++hw->stats.fills;
    if (!writable_map)
        ++hw->stats.fills_ro;
    return HOSTWIN_MAPPED;
}

enum hostwin_fill hostwin_resolve(struct hostwin_vcpu *hw, struct vcpu *v, uint64_t gva,
                                  unsigned size, bool write, enum hostwin_exit_reason *why)
{
    hw_lock(hw);
    enum hostwin_fill fill = resolve(hw, v, gva, size, write, why);
    hw_unlock(hw);
    return fill;
}

#ifdef IA64VMM_HOSTED
void hostwin_test_inject(struct hostwin_vcpu *hw, uint64_t nth)
{
    hw->inject_left = nth;
}
#endif
