/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tier1 counters (tier1.h struct t1_stats). */
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/log.h"

struct t1_stats t1_stats_slots[T1_STAT_SLOTS];

/* Formation stops by opcode (tier1.h t1_form_stops), [0] after the last
 * instruction, [1] at the head. Compilers increment atomically;
 * live readers load relaxed. */
static uint64_t form_stops[2][IA64_OP_COUNT];

void t1_form_stop_note(unsigned opcode, bool head)
{
    if (opcode < IA64_OP_COUNT)
        __atomic_fetch_add(&form_stops[head][opcode], 1, __ATOMIC_RELAXED);
}

uint64_t t1_form_stops(unsigned opcode, bool head)
{
    return opcode < IA64_OP_COUNT ? __atomic_load_n(&form_stops[head][opcode], __ATOMIC_RELAXED)
                                  : 0;
}

/* Calls formation took, by enum t1_call_note (tier1.h t1_form_calls). */
static uint64_t form_calls[T1_CALL_NOTES];

void t1_form_call_note(enum t1_call_note kind)
{
    if ((unsigned)kind < T1_CALL_NOTES)
        __atomic_fetch_add(&form_calls[kind], 1, __ATOMIC_RELAXED);
}

uint64_t t1_form_calls(enum t1_call_note kind)
{
    return (unsigned)kind < T1_CALL_NOTES ? __atomic_load_n(&form_calls[kind], __ATOMIC_RELAXED)
                                          : 0;
}

/* In-region helper calls by opcode (tier1.h t1_helper_calls): relaxed
 * increments, no locked operation per call. */
static uint64_t helper_calls[IA64_OP_COUNT];

void t1_helper_call_note(unsigned opcode)
{
    if (opcode < IA64_OP_COUNT)
        __atomic_store_n(&helper_calls[opcode],
                         __atomic_load_n(&helper_calls[opcode], __ATOMIC_RELAXED) + 1,
                         __ATOMIC_RELAXED);
}

uint64_t t1_helper_calls(unsigned opcode)
{
    return opcode < IA64_OP_COUNT ? __atomic_load_n(&helper_calls[opcode], __ATOMIC_RELAXED) : 0;
}

/* Distinct rejected heads (tier1.h rejected_heads): a set of 2^16 hashed
 * heads; a collision undercounts. */
static uint64_t rejected_heads[1024];

void t1_rejected_head_note(gpa_t entry_pa, uint64_t ip, unsigned slot)
{
    uint64_t h = ((entry_pa ^ (ip << 1) ^ slot) * UINT64_C(0x9e3779b97f4a7c15)) >> 48;
    uint64_t bit = UINT64_C(1) << (h % 64);
    if (!(__atomic_fetch_or(&rejected_heads[h / 64], bit, __ATOMIC_RELAXED) & bit))
        T1_COUNT(rejected_heads, 1);
}

/* PSR bits by statistic (tier1.h t1_psr_bits). */
static uint64_t psr_bits[2][64];

void t1_psr_note(enum t1_psr_count kind, uint64_t bits)
{
    for (uint64_t b = bits; b; b &= b - 1)
        __atomic_fetch_add(&psr_bits[kind][__builtin_ctzll(b)], 1, __ATOMIC_RELAXED);
}

uint64_t t1_psr_bits(enum t1_psr_count kind, unsigned bit)
{
    return (unsigned)kind < 2 && bit < 64 ? __atomic_load_n(&psr_bits[kind][bit], __ATOMIC_RELAXED)
                                          : 0;
}

void t1_get_stats(struct t1_stats *out)
{
    /* The sum of every thread's slot. Each
     * slot has one writer; this snapshot is relaxed, as Tier0's. */
    uint64_t *dst = (void *)out;
    for (unsigned i = 0; i < sizeof(*out) / sizeof(*dst); ++i)
        dst[i] = 0;
    for (unsigned slot = 0; slot < T1_STAT_SLOTS; ++slot) {
        const uint64_t *src = (const void *)&t1_stats_slots[slot];
        for (unsigned i = 0; i < sizeof(*out) / sizeof(*dst); ++i)
            dst[i] += __atomic_load_n(&src[i], __ATOMIC_RELAXED);
    }
#if IA64VMM_TIER1_STATS
    out->arena_bytes = t1_arena_get_stats(&out->arena);
#else
    out->arena_bytes = t1_arena_bytes_used();
#endif
    struct t1_memory_stats m;
    t1_memory_get_stats(&m);
    out->memory_budget = m.budget_bytes;
    out->memory_reserve = m.reserve_bytes;
    out->memory_bytes = m.used_bytes;
    out->memory_peak = m.peak_bytes;
    out->memory_refusals = m.refusals;
    out->memory_workspace = m.workspace_bytes;
    t1_region_counts(&out->regions, &out->region_capacity);
}

void t1_dump_stats(void)
{
    struct t1_stats total;
    t1_get_stats(&total);
    const struct t1_stats *s = &total;
    LOGI("tier1: compiles=%llu rejects=%llu entries=%llu guard_fails=%llu nat_guard_fails=%llu",
         (unsigned long long)s->compiles, (unsigned long long)s->rejects,
         (unsigned long long)s->entries, (unsigned long long)s->entry_guard_fails,
         (unsigned long long)s->nat_guard_fails);
    LOGI(
        "tier1: exits exit/ckpt/site/helper=%llu/%llu/%llu/%llu lower_steps=%llu lower_retired=%llu "
        "tier1_retired=%llu",
        (unsigned long long)s->exits_by_kind[0], (unsigned long long)s->exits_by_kind[1],
        (unsigned long long)s->exits_by_kind[2], (unsigned long long)s->exits_by_kind[3],
        (unsigned long long)s->lower_steps, (unsigned long long)s->lower_retired,
        (unsigned long long)s->tier1_retired);
    LOGI(
        "tier1: window_misses=%llu (same_region=%llu other_region=%llu) traps=%llu trap_fills=%llu "
        "site_exits=%llu invalidations=%llu reclaimed=%llu arena_bytes=%llu",
        (unsigned long long)s->window_misses, (unsigned long long)s->window_misses_same_region,
        (unsigned long long)s->window_misses_other_region, (unsigned long long)s->traps,
        (unsigned long long)s->trap_fills, (unsigned long long)s->site_exits,
        (unsigned long long)s->invalidations, (unsigned long long)s->reclaimed,
        (unsigned long long)t1_arena_bytes_used());
    /* The product path. */
    LOGI(
        "tier1: upper probes=%llu links=%llu enters=%llu ran=%llu ran_pending=%llu stops=%llu relinks=%llu "
        "declines link/budget/rse/nat/fetch/validate/retired/lower=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
        "lower_refusals=%llu",
        (unsigned long long)s->probes, (unsigned long long)s->probe_links,
        (unsigned long long)s->upper_enters, (unsigned long long)s->upper_ran,
        (unsigned long long)s->upper_ran_pending, (unsigned long long)s->upper_stops,
        (unsigned long long)s->relinks, (unsigned long long)s->decline_link,
        (unsigned long long)s->decline_budget, (unsigned long long)s->decline_rse,
        (unsigned long long)s->decline_nat, (unsigned long long)s->decline_fetch,
        (unsigned long long)s->decline_validate, (unsigned long long)s->decline_retired,
        (unsigned long long)s->decline_lower, (unsigned long long)s->lower_refusals);
    LOGI(
        "tier1: register stack spills %llu, spill exits %llu; calls followed/page/other=%llu/%llu/%llu",
        (unsigned long long)s->spills, (unsigned long long)s->spill_exits,
        (unsigned long long)t1_form_calls(T1_CALL_FOLLOWED),
        (unsigned long long)t1_form_calls(T1_CALL_PAGE),
        (unsigned long long)t1_form_calls(T1_CALL_OTHER));
    LOGI(
        "tier1: hot calls=%llu linked=%llu pending=%llu rejected=%llu deferred=%llu held=%llu holds=%llu "
        "reject_holds=%llu demotions profit/nat/site=%llu/%llu/%llu",
        (unsigned long long)s->hot_calls, (unsigned long long)s->hot_linked,
        (unsigned long long)s->hot_pending, (unsigned long long)s->hot_rejected,
        (unsigned long long)s->hot_deferred, (unsigned long long)s->hot_held,
        (unsigned long long)s->holds, (unsigned long long)s->reject_holds,
        (unsigned long long)s->demote_profit, (unsigned long long)s->demote_nat,
        (unsigned long long)s->demote_site);
    LOGI(
        "tier1: arena seals=%llu chunk_recycles=%llu evictions=%llu replacements=%llu moved_entries=%llu "
        "recompiles=%llu",
        (unsigned long long)s->seals, (unsigned long long)s->chunk_recycles,
        (unsigned long long)s->evictions, (unsigned long long)s->replacements,
        (unsigned long long)s->moved_entries, (unsigned long long)s->recompiles);
    LOGI("tier1: fetch guard of non-entry pages: runs=%llu cache_hits=%llu",
         (unsigned long long)s->fetch_guards, (unsigned long long)s->fetch_cache_hits);
    LOGI(
        "tier1: chain exits=%llu chainable=%llu succ_hits=%llu links=%llu lengths 1/2/4/8/16/32/64/more="
        "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
        (unsigned long long)s->exits_by_kind[T1_PT_EXIT], (unsigned long long)s->chainable,
        (unsigned long long)s->succ_hits, (unsigned long long)s->chain_links,
        (unsigned long long)s->chain_lengths[0], (unsigned long long)s->chain_lengths[1],
        (unsigned long long)s->chain_lengths[2], (unsigned long long)s->chain_lengths[3],
        (unsigned long long)s->chain_lengths[4], (unsigned long long)s->chain_lengths[5],
        (unsigned long long)s->chain_lengths[6], (unsigned long long)s->chain_lengths[7]);
    LOGI(
        "tier1: chain returns pair/break/state/interrupt/successor/contract/fetch/budget/nat/validate/retired="
        "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
        (unsigned long long)s->chain_returns[T1_CHAIN_PAIR],
        (unsigned long long)s->chain_returns[T1_CHAIN_BREAK],
        (unsigned long long)s->chain_returns[T1_CHAIN_STATE],
        (unsigned long long)s->chain_returns[T1_CHAIN_INTERRUPT],
        (unsigned long long)s->chain_returns[T1_CHAIN_SUCCESSOR],
        (unsigned long long)s->chain_returns[T1_CHAIN_CONTRACT],
        (unsigned long long)s->chain_returns[T1_CHAIN_FETCH],
        (unsigned long long)s->chain_returns[T1_CHAIN_BUDGET],
        (unsigned long long)s->chain_returns[T1_CHAIN_NAT],
        (unsigned long long)s->chain_returns[T1_CHAIN_VALIDATE],
        (unsigned long long)s->chain_returns[T1_CHAIN_RETIRED]);
#define E(field)                                                                                   \
    (unsigned long long)s->field[T1_END_LIMIT], (unsigned long long)s->field[T1_END_BRANCH],       \
        (unsigned long long)s->field[T1_END_CALL], (unsigned long long)s->field[T1_END_RET],       \
        (unsigned long long)s->field[T1_END_ALLOC], (unsigned long long)s->field[T1_END_RSE],      \
        (unsigned long long)s->field[T1_END_NAT], (unsigned long long)s->field[T1_END_OTHER]
    LOGI(
        "tier1: exit ends limit/branch/call/ret/alloc/rse/nat/other exits=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
        "returns=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu gaps=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
        E(exits_by_end), E(returns_by_end), E(gap_by_end));
#undef E
#if IA64VMM_TIER1_STATS
    t1_dump_transitions();
#endif
    /* Measured only with the statistics gate on. */
    if (IA64VMM_TIER1_STATS)
        LOGI("tier1: compile_ticks=%llu over %llu compiles and %llu rejects",
             (unsigned long long)s->compile_ticks, (unsigned long long)s->compiles,
             (unsigned long long)s->rejects);
}

#if IA64VMM_TIER1_STATS
/* Each vCPU's 16 most frequent transitions (region
 * id.point -> region id). Stopped executors. */
void t1_dump_transitions(void)
{
    uint64_t mask = (UINT64_C(1) << T1_LINK_ID_BITS) - 1;
    for (unsigned slot = 0; slot < T1_VCPU_SLOTS; ++slot) {
        const struct t1_vcpu *tv = __atomic_load_n(&t1_vcpus[slot], __ATOMIC_ACQUIRE);
        if (!tv)
            continue;
        uint64_t shown[T1_TRANSITIONS / 64] = {0};
        for (unsigned n = 0; n < 16; ++n) {
            unsigned best = T1_TRANSITIONS;
            for (unsigned k = 0; k < T1_TRANSITIONS; ++k)
                if (tv->transitions[k].count && !(shown[k / 64] >> (k % 64) & 1) &&
                    (best == T1_TRANSITIONS ||
                     tv->transitions[k].count > tv->transitions[best].count))
                    best = k;
            if (best == T1_TRANSITIONS)
                break;
            shown[best / 64] |= UINT64_C(1) << (best % 64);
            const struct t1_transition *t = &tv->transitions[best];
            LOGI("tier1: vcpu%u transition r%llu.%u -> r%llu count=%llu", tv->v->cfg.index,
                 (unsigned long long)(t->from & mask), t->point, (unsigned long long)(t->to & mask),
                 (unsigned long long)t->count);
        }
    }
}
#endif
