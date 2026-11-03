/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Harness hotness counts backward branches and uncompiled exits separately
 * by (IP, slot). Exits use half hot_threshold; compile failure backs off
 * exponentially. Product per-head holds and demotion are below.
 */
#include "engine/tier1/region.h"
#include "ia64vmm/string.h"

#ifdef IA64VMM_HOSTED
static bool tail_hotness = true;
bool t1_test_set_tail_hotness(bool enabled)
{
    bool old = tail_hotness;
    tail_hotness = enabled;
    return old;
}
#endif

static struct hot_entry *entry_of(struct t1_vcpu *tv, uint64_t ip, unsigned slot, bool tail)
{
    uint64_t h = ((ip >> 4) * UINT64_C(0x9e3779b97f4a7c15)) ^ ((uint64_t)slot << 56);
    struct hot_entry *table = tail ? tv->tail_hot : tv->hot;
    return &table[(h >> 56) % T1_HOT_ENTRIES];
}

void t1_hot_reset(struct t1_vcpu *tv)
{
    memset(tv->hot, 0, sizeof(tv->hot));
    memset(tv->tail_hot, 0, sizeof(tv->tail_hot));
}

static bool note(struct t1_vcpu *tv, unsigned threshold, bool tail)
{
    const struct vcpu *v = tv->v;
    if (!t1_cfg.hot_threshold)
        return false;
    uint64_t ip = v->s.ip;
    unsigned slot = v->s.slot;
    struct hot_entry *e = entry_of(tv, ip, slot, tail);
    if (e->ip != ip || e->slot != slot)
        *e = (struct hot_entry){ip, (uint8_t)slot, 0, 0};
    if (++e->count < threshold + e->backoff)
        return false;
    e->count = 0;
    return true;
}

bool t1_hot_note(struct t1_vcpu *tv, uint64_t before_ip, unsigned before_slot)
{
    const struct vcpu *v = tv->v;
    if (v->s.ip > before_ip || (v->s.ip == before_ip && v->s.slot > before_slot))
        return false;
    return note(tv, t1_cfg.hot_threshold, false);
}

bool t1_hot_trace(struct t1_vcpu *tv)
{
#ifdef IA64VMM_HOSTED
    if (!tail_hotness)
        return false;
#endif
    /* The predecessor is already hot; its tail needs fewer observations
     * than a loop discovered entirely in the lower tier. */
    return note(tv, t1_cfg.hot_threshold / 2 + t1_cfg.hot_threshold % 2, true);
}

void t1_hot_failed(struct t1_vcpu *tv, bool tail)
{
    struct hot_entry *e = entry_of(tv, tv->v->s.ip, tv->v->s.slot, tail);
    if (e->ip != tv->v->s.ip || e->slot != tv->v->s.slot)
        return;
    e->backoff = e->backoff ? (e->backoff < (1u << 20) ? e->backoff * 2 : e->backoff) : 64;
}

/*
 * Product holds/demotion: Tier0 supplies the doubling hotness schedule.
 * A direct-mapped hold keyed by (entry PA, IP, slot) rejects 2^(strikes-1)
 * hot calls before retrying compilation; collisions replace old holds.
 * Compare head-bundle bytes, not page generation: unrelated writes must not
 * cancel a hold.
 *
 * Demote only hotness-compiled regions (r->policy). Profitability uses 256
 * entries and min_insns_per_entry for non-loops; NaT guards use 256 attempts,
 * demoting at the 129th failure. Site exits use 4096 entries, recompiling as
 * a class-M helper at the 17th exit at one site.
 */
#define HOLDS 8192u
#define PROFIT_WINDOW 256u
#define NAT_WINDOW 256u
#define SITE_WINDOW 4096u
#define SITE_EXIT_LIMIT 16u
struct hold {
    gpa_t entry_pa;
    uint64_t ip, low, high;
    uint8_t slot;
    uint8_t strikes;
    bool used;
    uint32_t remaining;
    bool slow_memory;
};
static struct hold holds[HOLDS];

static struct hold *hold_of(gpa_t entry_pa, uint64_t ip, unsigned slot)
{
    uint64_t h = (entry_pa ^ (ip << 1) ^ slot) * UINT64_C(0x9e3779b97f4a7c15);
    return &holds[(h >> 40) % HOLDS];
}

/* The head's bundle (entry_pa: ip is bundle-aligned, the slot separate);
 * false if it cannot be read as code. Both callers run right after a
 * region at the head was entered (demotion after an exit, hot() at
 * Tier0's lookup hit), so the page has a validity record and the bundle is
 * fresh: the read returns the snapshot and changes nothing. If the record
 * had been evicted, the read is a formation-like read (compile.c reads the
 * same way): its allocation may evict another record, which retires that
 * page's dependents, a correct retirement. */
static bool head_bundle(gpa_t entry_pa, uint64_t *low, uint64_t *high)
{
    uint64_t generation;
    return code_source_read_physical(entry_pa & ~UINT64_C(15), low, high, &generation).kind ==
           IA64_STEP_COMPLETE;
}

/* The hold table is read and written under
 * code_lock, which every executor and helper shares. */
bool t1_hold_active(gpa_t entry_pa, uint64_t ip, unsigned slot)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool held = false;
    struct hold *e = hold_of(entry_pa, ip, slot);
    if (e->used && e->entry_pa == entry_pa && e->ip == ip && e->slot == slot) {
        uint64_t low, high;
        if (!head_bundle(entry_pa, &low, &high) || low != e->low || high != e->high)
            e->used = false;
        else if (e->remaining) {
            --e->remaining;
            held = true;
        }
    }
    if (take)
        code_unlock();
    return held;
}

bool t1_head_memory_helpers(gpa_t entry_pa, uint64_t ip, unsigned slot)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    const struct hold *e = hold_of(entry_pa, ip, slot);
    bool slow =
        e->used && e->entry_pa == entry_pa && e->ip == ip && e->slot == slot && e->slow_memory;
    if (take)
        code_unlock();
    return slow;
}

void t1_holds_clear(void)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    memset(holds, 0, sizeof(holds));
    if (take)
        code_unlock();
}

/* code_lock held: one more strike for the head's hold (a new hold for
 * other bytes). */
static void strike(gpa_t entry_pa, uint64_t ip, unsigned slot)
{
    struct hold *e = hold_of(entry_pa, ip, slot);
    uint64_t low = 0, high = 0;
    bool read = head_bundle(entry_pa, &low, &high);
    if (!e->used || e->entry_pa != entry_pa || e->ip != ip || e->slot != slot || e->low != low ||
        e->high != high) {
        if (e->used && (e->entry_pa != entry_pa || e->ip != ip || e->slot != slot))
            T1_COUNT(hold_collisions, 1);
        *e = (struct hold){entry_pa, ip, low, high, (uint8_t)slot, 0, true, 0, false};
    }
    /* An unreadable head holds nothing (t1_hold_active ends such a hold). */
    if (!read)
        e->used = false;
    if (e->strikes < 20)
        ++e->strikes;
    e->remaining = UINT32_C(1) << (e->strikes - 1);
}

/* A compile at a hot head was rejected for a reason that recurs
 * (t1_compile_hot on the executor, a helper's compile of a queued
 * request): the head is held like a demoted one, so a head Tier1 cannot
 * compile is retried ever less often instead of at every hot() call of
 * Tier0's schedule. */
void t1_hold_rejected(gpa_t entry_pa, uint64_t ip, unsigned slot)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    strike(entry_pa, ip, slot);
    if (take)
        code_unlock();
    T1_COUNT(reject_holds, 1);
}

/* One demotion per region: under code_lock, only while r is still valid
 * (another executor's window may have decided first). */
static bool demote(struct t1_region *r, enum t1_demotion why, bool slow_memory)
{
    bool take = !code_lock_held();
    if (take)
        code_lock();
    bool done = r->valid;
    if (done) {
        strike(r->c.entry_pa, r->c.ip, r->c.slot);
        if (slow_memory)
            hold_of(r->c.entry_pa, r->c.ip, r->c.slot)->slow_memory = true;
        T1_COUNT(holds, 1);
        if (why == T1_DEMOTE_PROFIT)
            T1_COUNT(demote_profit, 1);
        else if (why == T1_DEMOTE_NAT)
            T1_COUNT(demote_nat, 1);
        else
            T1_COUNT(demote_site, 1);
        t1_region_demote(r);
    }
    if (take)
        code_unlock();
    return done;
}

/*
 * Shared windows pack generation/counts in one CAS word. The CAS closing
 * a window advances its generation and decides from the old counts. Executors
 * fold local entries/retirement/NaT attempts only into their original generation;
 * drop counts after closure rather than carrying them forward. Closure may be
 * late, never early. Site/helper exits and NaT failures count immediately.
 */
#define PROFIT_RETIRED_MAX 0xfffffu
static uint64_t window_word(uint32_t gen, uint64_t counts)
{
    return (uint64_t)gen << 32 | counts;
}

/* s counts nothing yet, for r's current windows. */
static void window_start(struct t1_window_slot *s, const struct t1_region *r)
{
    *s =
        (struct t1_window_slot){r->link,
                                0,
                                T1_WINDOW_GEN(__atomic_load_n(&r->profit_window, __ATOMIC_SEQ_CST)),
                                T1_WINDOW_GEN(__atomic_load_n(&r->site_window, __ATOMIC_SEQ_CST)),
                                T1_WINDOW_GEN(__atomic_load_n(&r->nat_window, __ATOMIC_SEQ_CST)),
                                0,
                                0};
}

/* Folds s into r (live: the caller has not published quiescence since it
 * resolved r) and starts s again in r's current windows; true: r was
 * demoted. */
static bool fold(struct t1_window_slot *s, struct t1_region *r)
{
    bool unprofitable = false, site_closed = false;
    uint64_t w = __atomic_load_n(&r->profit_window, __ATOMIC_SEQ_CST), next;
    do {
        unprofitable = false;
        if (T1_WINDOW_GEN(w) != s->profit_gen || !s->entries)
            break;
        uint32_t entries = T1_PROFIT_ENTRIES(w) + s->entries;
        uint64_t retired = T1_PROFIT_RETIRED(w) + s->retired;
        if (retired > PROFIT_RETIRED_MAX)
            retired = PROFIT_RETIRED_MAX;
        if (entries >= PROFIT_WINDOW) {
            next = window_word(T1_WINDOW_GEN(w) + 1, 0);
            unprofitable = !r->loop && retired < (uint64_t)t1_cfg.min_insns_per_entry * entries;
        } else
            next = window_word(T1_WINDOW_GEN(w), (uint64_t)entries << 20 | retired);
    } while (!__atomic_compare_exchange_n(&r->profit_window, &w, next, false, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST));
    w = __atomic_load_n(&r->site_window, __ATOMIC_SEQ_CST);
    do {
        site_closed = false;
        if (T1_WINDOW_GEN(w) != s->site_gen || !s->entries)
            break;
        uint32_t entries = (uint32_t)w + s->entries;
        site_closed = entries >= SITE_WINDOW;
        next = site_closed ? window_word(T1_WINDOW_GEN(w) + 1, 0)
                           : window_word(T1_WINDOW_GEN(w), entries);
    } while (!__atomic_compare_exchange_n(&r->site_window, &w, next, false, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST));
    if (site_closed) {
        for (unsigned k = 0; k < T1_MAX_SITES; ++k)
            __atomic_store_n(&r->site_exits[k], 0, __ATOMIC_SEQ_CST);
        for (unsigned k = 0; k < T1_MAX_INSNS; ++k)
            __atomic_store_n(&r->helper_exits[k], 0, __ATOMIC_SEQ_CST);
    }
    w = __atomic_load_n(&r->nat_window, __ATOMIC_SEQ_CST);
    do {
        if (T1_WINDOW_GEN(w) != s->nat_gen || !s->attempts)
            break;
        uint32_t attempts = T1_NAT_ATTEMPTS(w) + s->attempts;
        next = attempts >= NAT_WINDOW
                   ? window_word(T1_WINDOW_GEN(w) + 1, 0)
                   : window_word(T1_WINDOW_GEN(w), (uint64_t)attempts << 16 | T1_NAT_FAILS(w));
    } while (!__atomic_compare_exchange_n(&r->nat_window, &w, next, false, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST));
    window_start(s, r);
    return unprofitable && demote(r, T1_DEMOTE_PROFIT, false);
}

/* r's slot of w: a slot another region holds is folded into that region
 * first (if it is still live) and then counts for r. */
static struct t1_window_slot *window_slot(struct t1_windows *w, struct t1_region *r)
{
    struct t1_window_slot *s = &w->slot[r->id % T1_WINDOW_SLOTS];
    if (s->link == r->link)
        return s;
    struct t1_region *old = s->link ? t1_region_from_link(s->link) : NULL;
    if (old)
        fold(s, old);
    window_start(s, r);
    return s;
}

bool t1_demote_after_exit(struct t1_windows *w, struct t1_region *r, const struct t1_point *P,
                          uint64_t retired)
{
    if (!r->policy || !r->valid)
        return false;
    if (P->kind == T1_PT_SITE && P->site < T1_MAX_SITES &&
        t1_window_inc8(&r->site_exits[P->site]) > SITE_EXIT_LIMIT)
        /* Retain the ordinary backoff once, then retry store-heavy heads
         * with exact memory helpers. Request-local
         * policy keeps worker formation independent of live vCPU state. */
        return demote(r, T1_DEMOTE_SITE, P->access_write != 0);
    /* Value-dependent faults (e.g. reserved FPSR fields) leave at the
     * BEFORE helper point and must obey the same backoff as native sites. */
    if (P->kind == T1_PT_HELPER && P->insn < T1_MAX_INSNS &&
        t1_window_inc8(&r->helper_exits[P->insn]) > SITE_EXIT_LIMIT)
        return demote(r, T1_DEMOTE_SITE, false);
    struct t1_window_slot *s = window_slot(w, r);
    ++s->entries;
    s->retired += retired;
    return s->entries >= T1_WINDOW_FOLD && fold(s, r);
}

/* A failure counts at once, with its attempt, in the current window. */
bool t1_demote_after_nat_guard(struct t1_windows *w, struct t1_region *r, bool failed)
{
    if (!r->policy || !r->valid)
        return false;
    if (!failed) {
        struct t1_window_slot *s = window_slot(w, r);
        return ++s->attempts >= T1_WINDOW_FOLD && fold(s, r);
    }
    uint64_t word = __atomic_load_n(&r->nat_window, __ATOMIC_SEQ_CST), next;
    bool demoted;
    do {
        uint32_t attempts = T1_NAT_ATTEMPTS(word) + 1, fails = T1_NAT_FAILS(word) + 1;
        demoted = fails > NAT_WINDOW / 2;
        next = attempts >= NAT_WINDOW
                   ? window_word(T1_WINDOW_GEN(word) + 1, 0)
                   : window_word(T1_WINDOW_GEN(word), (uint64_t)attempts << 16 | fails);
    } while (!__atomic_compare_exchange_n(&r->nat_window, &word, next, false, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST));
    return demoted && demote(r, T1_DEMOTE_NAT, false);
}
