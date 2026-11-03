/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * engine/hostwin private definitions. Shared by
 * registry.c, slots.c, context.c, resolve.c, shadows.c and trap.c only.
 */
#ifndef IA64VMM_HOSTWIN_INTERNAL_H
#define IA64VMM_HOSTWIN_INTERNAL_H
#include "engine/hostwin/hostwin.h"
#include "ia64vmm/hal.h"

#define HW_PAGE 4096u
#define HW_PAGE_MASK (~(uint64_t)(HW_PAGE - 1))
/* tlb.c's VA_PAYLOAD: TLB entries keep va in this VRN-stripped form (A.6). */
#define HW_VA_PAYLOAD ((UINT64_C(1) << 61) - 1)
#define HW_HASH_BUCKETS 4096u
#define HW_MAX_INSTANCES 64u
/* The statistics gate (make TIER1_STATS, mk/tier1.mk):
 * clock reads only in statistics builds. */
#ifndef IA64VMM_TIER1_STATS
#define IA64VMM_TIER1_STATS 0
#endif

/* Where a shadow's translation came from; TR and TC indices are
 * distinct namespaces, both D-side. */
enum hw_source { HW_SOURCE_PHYSICAL, HW_SOURCE_TR, HW_SOURCE_TC };

struct hw_shadow {
    uintptr_t host_page;
    gpa_t pa_page;
    uint64_t gva_page; /* gva & VA_PAYLOAD & ~4095, as tlb.c stores .va */
    uint32_t rid;      /* RR[gva >> 61].rid at fill time */
    uint8_t source, index, slot;
    bool writable;
    struct hw_shadow *host_next;              /* host_hash chain */
    struct hw_shadow *pa_next, **pa_prev;     /* pa_hash chain (code events) */
    struct hw_shadow *src_next, **src_prev;   /* per-(source, index) list */
    struct hw_shadow *live_next, **live_prev; /* every live shadow; free list */
};

/* The MMU context a shadow depends on: {rr, pkr, PSR fields, D-side
 * TR and TC}. tc_next is not part of it. */
struct hw_context {
    uint64_t rr[8], pkr[16], psr;
    struct ia64_tlb_entry tr[IA64_DTR_COUNT], tc[IA64_TC_ENTRIES];
};
/* Completeness check only (hostwin_config.check): every
 * logged element of the MMU state, both sides, as at the last update. */
_Static_assert(IA64_ITR_COUNT == IA64_DTR_COUNT, "one TR count for both sides");
struct hw_mmu_image {
    uint64_t rr[8], pkr[16];
    struct ia64_tlb_entry tr[2][IA64_DTR_COUNT], tc[2][IA64_TC_ENTRIES];
};

struct hw_slot {
    uint64_t tag, lru;
    uint32_t shadows;
    bool bound;
};

struct hostwin_vcpu {
    /* Slots, shadow records and the MMU
     * snapshot change only under this spinlock, taken by the owner's entry
     * points and by hostwin_code_page_event on any thread, which also
     * counts its statistics under it. The no-change fast path of
     * hostwin_enter reads owner-written fields only. contended counts
     * acquisitions that had to spin; unchanged is the owner's
     * context_unchanged count, also kept outside the lock. */
    unsigned lock;
    uint64_t contended, unchanged;
    const struct hostwin_backend *backend;
    void *ctx;
    unsigned w, slots;
    uintptr_t base, poison;
    struct hw_slot slot[HOSTWIN_MAX_SLOTS];
    uint64_t lru_clock, bind_epoch;
    struct hw_context context;
    bool context_valid;
    /* MMU log: v->mmu_seq at the last update; resync: the
     * next update is the full diff; force: the next hostwin_enter takes the
     * update path even without a change (resync, or the check mode). */
    uint64_t seen;
    bool check, resync, force;
    /* Check mode: the full image and whether it is comparable (not before
     * the first update or across a resync). */
    struct hw_mmu_image *image;
    size_t image_pages;
    bool image_valid;
    struct hw_shadow *pool, *free, *live;
    size_t pool_pages, self_pages;
    uint64_t live_count;
    struct hw_shadow *host_hash[HW_HASH_BUCKETS], *pa_hash[HW_HASH_BUCKETS];
    struct hw_shadow *by_tr[IA64_DTR_COUNT], *by_tc[IA64_TC_ENTRIES];
    uint64_t inject_left; /* 0 = disarmed; 1 = the next resolve exits */
    const char *stop_detail;
    struct hostwin_stats stats;
};

/* Order: code_lock, instance lock, backend window lock. Spinning leaves
 * interrupts enabled, so a holder's synchronous flush is acknowledged; a
 * waiter with interrupts disabled could never acknowledge one. */
static inline void hw_lock(const struct hostwin_vcpu *hw)
{
    struct hostwin_vcpu *w = (struct hostwin_vcpu *)hw;
    if (!__atomic_exchange_n(&w->lock, 1, __ATOMIC_ACQUIRE))
        return;
#ifndef IA64VMM_HOSTED
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    if (!(flags & 0x200))
        hal_panic("hostwin: instance lock contended with interrupts disabled");
#endif
    __atomic_fetch_add(&w->contended, 1, __ATOMIC_RELAXED);
    do {
        while (__atomic_load_n(&w->lock, __ATOMIC_RELAXED))
            __asm__ volatile("pause");
    } while (__atomic_exchange_n(&w->lock, 1, __ATOMIC_ACQUIRE));
}
static inline void hw_unlock(const struct hostwin_vcpu *hw)
{
    __atomic_store_n(&((struct hostwin_vcpu *)hw)->lock, 0, __ATOMIC_RELEASE);
}

/* context_unchanged: written by the owner only, read by hostwin_get_stats. */
static inline void hw_count_unchanged(struct hostwin_vcpu *hw)
{
    __atomic_store_n(&hw->unchanged, __atomic_load_n(&hw->unchanged, __ATOMIC_RELAXED) + 1,
                     __ATOMIC_RELAXED);
}

static inline uint64_t hw_low_mask(unsigned bits)
{
    return bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
}
static inline uintptr_t hw_slot_base(const struct hostwin_vcpu *hw, unsigned slot)
{
    return hw->base + ((uintptr_t)slot << hw->w);
}

/* registry.c */
IA64VMM_NORETURN void hw_panic(const struct hostwin_vcpu *hw, const char *what,
                               uintptr_t host_page);

/* slots.c */
int hw_slot_of_tag(const struct hostwin_vcpu *hw, uint64_t tag);

/* Everything below runs with the instance lock held. */

/* context.c: the update of hostwin_enter after a change (log replay or full
 * diff, then the check), shared with hostwin_resolve. */
void hw_context_update(struct hostwin_vcpu *hw, const struct vcpu *v);

/* shadows.c */
bool hw_shadows_init(struct hostwin_vcpu *hw);
void hw_shadows_fini(struct hostwin_vcpu *hw);
struct hw_shadow *hw_shadow_find(const struct hostwin_vcpu *hw, uintptr_t host_page);
/* Records a shadow just mapped by backend.map (replacing a record for the
 * same host page, whose mapping map() overwrote). */
void hw_shadow_record(struct hostwin_vcpu *hw, const struct hw_shadow *fields);
/* true: exactly s was removed; false: the backend could not unmap the
 * page and the whole slot was dropped instead. */
bool hw_shadow_drop(struct hostwin_vcpu *hw, struct hw_shadow *s);
void hw_shadows_drop_slot(struct hostwin_vcpu *hw, unsigned slot);
void hw_shadows_drop_all(struct hostwin_vcpu *hw);
void hw_shadows_drop_source(struct hostwin_vcpu *hw, enum hw_source source, unsigned index);
void hw_shadows_drop_in_entry(struct hostwin_vcpu *hw, const struct ia64_tlb_entry *entry);
/* Every writable shadow of pa_page becomes read-only; flushed with sync. */
void hw_shadows_protect_pa(struct hostwin_vcpu *hw, gpa_t pa_page);
#endif
