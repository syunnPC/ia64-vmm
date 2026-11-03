/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Invalidate shadows when their fill context changes. RR, PKR or relevant
 * PSR changes drop all shadows, preserving slot bindings. Changed D-side
 * TR/TC entries drop both shadows sourced from that index and shadows within
 * the new mapping. This covers ia64_tlb_find's TR-before-TC, first-match rule;
 * overlap purges are logged too. Physical shadows do not depend on TLBs.
 *
 * Log replay compares actual values before invalidation, so redundant writes
 * and identical reinsertion do not drop shadows. I-side changes need no action.
 * Check mode verifies log completeness on both sides and compares replay
 * invalidation with the full diff. Overflow/ALL/create/resync restart the
 * check snapshot because their changes cannot be attributed to the log.
 */
#include "engine/hostwin/internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

/*
 * Fill context: dt, pk, cpl. Exclude PSR.da: lasting rights never depend on
 * its one-shot A/D suppression (ia64_lasting_rights).
 */
#define HW_PSR_CONTEXT (IA64_PSR_DT | IA64_PSR_PK | (UINT64_C(3) << 32))

static bool same_entry(const struct ia64_tlb_entry *a, const struct ia64_tlb_entry *b)
{
    /* Field by field (the struct has padding): va, pte, itir, rid,
     * page_shift, valid. */
    return a->va == b->va && a->pte == b->pte && a->itir == b->itir && a->rid == b->rid &&
           a->page_shift == b->page_shift && a->valid == b->valid;
}

static bool same_registers(const struct hw_context *c, const struct ia64_state *s)
{
    for (unsigned i = 0; i < 8; ++i)
        if (c->rr[i] != s->rr[i])
            return false;
    for (unsigned i = 0; i < 16; ++i)
        if (c->pkr[i] != s->pkr[i])
            return false;
    return c->psr == (s->psr & HW_PSR_CONTEXT);
}

static void take(struct hw_context *c, const struct ia64_state *s)
{
    for (unsigned i = 0; i < 8; ++i)
        c->rr[i] = s->rr[i];
    for (unsigned i = 0; i < 16; ++i)
        c->pkr[i] = s->pkr[i];
    c->psr = s->psr & HW_PSR_CONTEXT;
    for (unsigned i = 0; i < IA64_DTR_COUNT; ++i)
        c->tr[i] = s->tr[1][i];
    for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i)
        c->tc[i] = s->tc[1][i];
}

/* What a diff did (or would do): drop everything, or entry_changed for the
 * D-side TRs and TCs in the masks. */
_Static_assert(IA64_DTR_COUNT <= 64, "TR action bitmap covers every storage slot");
struct hw_actions {
    bool all;
    uint64_t tr, tc[(IA64_TC_ENTRIES + 63) / 64];
};

static void drop_everything(struct hostwin_vcpu *hw, const struct ia64_state *s,
                            struct hw_actions *did)
{
    ++hw->stats.context_full_drops;
    hw_shadows_drop_all(hw);
    take(&hw->context, s);
    did->all = true;
}

static void entry_changed(struct hostwin_vcpu *hw, enum hw_source source, unsigned index,
                          struct ia64_tlb_entry *saved, const struct ia64_tlb_entry *now,
                          struct hw_actions *did)
{
    ++hw->stats.entry_diffs;
    hw_shadows_drop_source(hw, source, index);
    if (now->valid)
        hw_shadows_drop_in_entry(hw, now);
    *saved = *now;
    if (source == HW_SOURCE_TR)
        did->tr |= (UINT64_C(1) << index);
    else
        did->tc[index / 64] |= UINT64_C(1) << (index % 64);
}

/* The full diff: every register, then every D-side entry. */
static void hw_context_sync(struct hostwin_vcpu *hw, const struct vcpu *v, struct hw_actions *did)
{
    const struct ia64_state *s = &v->s;
    if (!hw->context_valid) {
        hw_shadows_drop_all(hw);
        take(&hw->context, s);
        hw->context_valid = true;
        did->all = true;
    } else if (!same_registers(&hw->context, s)) {
        drop_everything(hw, s, did);
    } else {
        for (unsigned i = 0; i < IA64_DTR_COUNT; ++i)
            if (!same_entry(&hw->context.tr[i], &s->tr[1][i]))
                entry_changed(hw, HW_SOURCE_TR, i, &hw->context.tr[i], &s->tr[1][i], did);
        for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i)
            if (!same_entry(&hw->context.tc[i], &s->tc[1][i]))
                entry_changed(hw, HW_SOURCE_TC, i, &hw->context.tc[i], &s->tc[1][i], did);
    }
}

/* The diff's actions without performing them (check mode). */
static void diff_actions(const struct hostwin_vcpu *hw, const struct ia64_state *s,
                         struct hw_actions *out)
{
    *out = (struct hw_actions){0};
    if (!same_registers(&hw->context, s)) {
        out->all = true;
        return;
    }
    for (unsigned i = 0; i < IA64_DTR_COUNT; ++i)
        if (!same_entry(&hw->context.tr[i], &s->tr[1][i]))
            out->tr |= (UINT64_C(1) << i);
    for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i)
        if (!same_entry(&hw->context.tc[i], &s->tc[1][i]))
            out->tc[i / 64] |= UINT64_C(1) << (i % 64);
}

static const struct ia64_mmu_change *logged(const struct vcpu *v, uint64_t seq)
{
    const struct ia64_mmu_change *c = &v->mmu_log[seq % IA64_MMU_LOG];
    /* The writers (ia64/mmu_log.h) only produce these; anything else is a
     * corrupted log, never something to replay. */
    unsigned limit = c->kind == IA64_MMU_RR    ? 8
                     : c->kind == IA64_MMU_PKR ? 16
                     : c->kind == IA64_MMU_TR  ? IA64_DTR_COUNT
                                               : IA64_TC_ENTRIES;
    if (c->kind > IA64_MMU_ALL || c->side > 1 || (c->kind != IA64_MMU_ALL && c->index >= limit))
        hal_panic("hostwin: corrupt MMU log entry");
    return c;
}

/* The replay of v->mmu_log[seen .. mmu_seq), at most IA64_MMU_LOG changes
 * and none of them IA64_MMU_ALL: registers and PSR first, as the diff. */
static void replay(struct hostwin_vcpu *hw, const struct vcpu *v, struct hw_actions *did)
{
    const struct ia64_state *s = &v->s;
    bool all = hw->context.psr != (s->psr & HW_PSR_CONTEXT);
    for (uint64_t seq = hw->seen; !all && seq != v->mmu_seq; ++seq) {
        const struct ia64_mmu_change *c = logged(v, seq);
        if (c->kind == IA64_MMU_RR)
            all = hw->context.rr[c->index] != s->rr[c->index];
        else if (c->kind == IA64_MMU_PKR)
            all = hw->context.pkr[c->index] != s->pkr[c->index];
    }
    if (all) {
        drop_everything(hw, s, did);
        return;
    }
    for (uint64_t seq = hw->seen; seq != v->mmu_seq; ++seq) {
        const struct ia64_mmu_change *c = logged(v, seq);
        if (c->side != 1 || (c->kind != IA64_MMU_TR && c->kind != IA64_MMU_TC))
            continue;
        bool tr = c->kind == IA64_MMU_TR;
        struct ia64_tlb_entry *saved = tr ? &hw->context.tr[c->index] : &hw->context.tc[c->index];
        const struct ia64_tlb_entry *now = tr ? &s->tr[1][c->index] : &s->tc[1][c->index];
        if (!same_entry(saved, now))
            entry_changed(hw, tr ? HW_SOURCE_TR : HW_SOURCE_TC, c->index, saved, now, did);
    }
}

/* ---- completeness check (check mode) ---------------------------------- */

static IA64VMM_NORETURN void check_failed(const struct vcpu *v, const struct hostwin_vcpu *hw,
                                          const char *what, unsigned kind, unsigned side,
                                          unsigned index)
{
    static const char *const kinds[] = {"TC", "TR", "RR", "PKR"};
    char element[32], message[256];
    if (kind == IA64_MMU_ALL)
        vmm_snprintf(element, sizeof(element), "RR/PKR/PSR");
    else if (kind == IA64_MMU_RR || kind == IA64_MMU_PKR)
        vmm_snprintf(element, sizeof(element), "%s %u", kinds[kind], index);
    else
        vmm_snprintf(element, sizeof(element), "%s-side %s %u", side ? "D" : "I", kinds[kind],
                     index);
    vmm_snprintf(message, sizeof(message),
                 "hostwin: MMU log check: %s %s (seen %llu, mmu_seq %llu)", element, what,
                 (unsigned long long)hw->seen, (unsigned long long)v->mmu_seq);
    hal_panic(message);
}

static bool in_window(const struct vcpu *v, const struct hostwin_vcpu *hw, unsigned kind,
                      unsigned side, unsigned index)
{
    for (uint64_t seq = hw->seen; seq != v->mmu_seq; ++seq) {
        const struct ia64_mmu_change *c = logged(v, seq);
        if (c->kind == kind && c->index == index &&
            (c->side == side || kind == IA64_MMU_RR || kind == IA64_MMU_PKR))
            return true;
    }
    return false;
}

/* (a): every element that differs from the image has a log entry in
 * [seen, mmu_seq). Called only when the window is complete. */
static void check_coverage(const struct hostwin_vcpu *hw, const struct vcpu *v)
{
    const struct hw_mmu_image *m = hw->image;
    const struct ia64_state *s = &v->s;
    for (unsigned i = 0; i < 8; ++i)
        if (m->rr[i] != s->rr[i] && !in_window(v, hw, IA64_MMU_RR, 0, i))
            check_failed(v, hw, "changed without a log entry", IA64_MMU_RR, 0, i);
    for (unsigned i = 0; i < 16; ++i)
        if (m->pkr[i] != s->pkr[i] && !in_window(v, hw, IA64_MMU_PKR, 0, i))
            check_failed(v, hw, "changed without a log entry", IA64_MMU_PKR, 0, i);
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned i = 0; i < IA64_DTR_COUNT; ++i)
            if (!same_entry(&m->tr[side][i], &s->tr[side][i]) &&
                !in_window(v, hw, IA64_MMU_TR, side, i))
                check_failed(v, hw, "changed without a log entry", IA64_MMU_TR, side, i);
        for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i)
            if (!same_entry(&m->tc[side][i], &s->tc[side][i]) &&
                !in_window(v, hw, IA64_MMU_TC, side, i))
                check_failed(v, hw, "changed without a log entry", IA64_MMU_TC, side, i);
    }
}

static void take_image(struct hostwin_vcpu *hw, const struct ia64_state *s)
{
    struct hw_mmu_image *m = hw->image;
    memcpy(m->rr, s->rr, sizeof(m->rr));
    memcpy(m->pkr, s->pkr, sizeof(m->pkr));
    memcpy(m->tr, s->tr, sizeof(m->tr));
    memcpy(m->tc, s->tc, sizeof(m->tc));
    hw->image_valid = true;
}

/* (b): the replay did what the diff would have done, and left the snapshot
 * equal to v. */
static void check_actions(const struct hostwin_vcpu *hw, const struct vcpu *v,
                          const struct hw_actions *want, const struct hw_actions *did)
{
    if (want->all != did->all)
        check_failed(v, hw,
                     want->all
                         ? "differ: the full diff drops every shadow, the replay did not"
                         : "unchanged: the replay dropped every shadow, the full diff would not",
                     IA64_MMU_ALL, 1, 0);
    if (!want->all) {
        for (unsigned i = 0; i < IA64_DTR_COUNT; ++i)
            if (((want->tr ^ did->tr) >> i) & 1)
                check_failed(v, hw, "acted on differently by the replay and the full diff",
                             IA64_MMU_TR, 1, i);
        for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i)
            if (((want->tc[i / 64] ^ did->tc[i / 64]) >> (i % 64)) & 1)
                check_failed(v, hw, "acted on differently by the replay and the full diff",
                             IA64_MMU_TC, 1, i);
    }
    struct hw_actions left;
    diff_actions(hw, &v->s, &left);
    if (left.all)
        check_failed(v, hw, "left stale in the snapshot after the update", IA64_MMU_ALL, 1, 0);
    if (left.tr)
        check_failed(v, hw, "left stale in the snapshot after the update", IA64_MMU_TR, 1,
                     (unsigned)__builtin_ctzll(left.tr));
    for (unsigned word = 0; word < IA64VMM_ARRAY_SIZE(left.tc); ++word)
        if (left.tc[word])
            check_failed(v, hw, "left stale in the snapshot after the update", IA64_MMU_TC, 1,
                         word * 64 + (unsigned)__builtin_ctzll(left.tc[word]));
}

/* ---- the update --------------------------------------------------------- */

static bool window_has_all(const struct vcpu *v, uint64_t seen)
{
    for (uint64_t seq = seen; seq != v->mmu_seq; ++seq)
        if (logged(v, seq)->kind == IA64_MMU_ALL)
            return true;
    return false;
}

void hw_context_update(struct hostwin_vcpu *hw, const struct vcpu *v)
{
    uint64_t start = IA64VMM_TIER1_STATS ? hal_counter() : 0;
    const struct ia64_state *s = &v->s;
    /* Unsigned distance: a sequence that moved backwards (a restore without
     * hostwin_resync) is far, never replayed. */
    uint64_t distance = v->mmu_seq - hw->seen;
    bool full =
        !hw->context_valid || hw->resync || distance > IA64_MMU_LOG || window_has_all(v, hw->seen);
    struct hw_actions want = {0}, did = {0};
    if (hw->check && !full) {
        ++hw->stats.check_diffs;
        if (hw->image_valid)
            check_coverage(hw, v);
        diff_actions(hw, s, &want);
    }
    if (full) {
        ++hw->stats.context_full_diffs;
        hw_context_sync(hw, v, &did);
    } else if (!distance && hw->context.psr == (s->psr & HW_PSR_CONTEXT)) {
        hw_count_unchanged(hw);
    } else {
        ++hw->stats.context_replays;
        replay(hw, v, &did);
    }
    if (hw->check) {
        if (!full)
            check_actions(hw, v, &want, &did);
        take_image(hw, s);
    }
    hw->seen = v->mmu_seq;
    hw->resync = false;
    hw->force = hw->check;
    if (IA64VMM_TIER1_STATS)
        hw->stats.snapshot_ticks += hal_counter() - start;
}

void hostwin_enter(struct hostwin_vcpu *hw, const struct vcpu *v)
{
    /* Steady state: no logged change, same PSR fields. */
    if (__builtin_expect(v->mmu_seq == hw->seen && !hw->force &&
                             (v->s.psr & HW_PSR_CONTEXT) == hw->context.psr,
                         1)) {
        hw_count_unchanged(hw);
        return;
    }
    hw_lock(hw);
    hw_context_update(hw, v);
    hw_unlock(hw);
}

void hostwin_resync(struct hostwin_vcpu *hw)
{
    hw->resync = true;
    hw->force = true;
    hw->image_valid = false;
}

bool hostwin_can_reenter(const struct hostwin_vcpu *hw)
{
    return !hw->check && !hw->force;
}

void hostwin_note_reentries(struct hostwin_vcpu *hw, uint64_t entries)
{
    __atomic_store_n(&hw->unchanged, __atomic_load_n(&hw->unchanged, __ATOMIC_RELAXED) + entries,
                     __ATOMIC_RELAXED);
}
