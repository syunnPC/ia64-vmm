/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Statistical profile (core/profile.h). Management owns the tables; the
 * profile IPI handler only stores the interrupted RIP in its CPU's
 * mailbox, which management takes at the next tick. */
#include "core/profile.h"
#include "core/helper.h"
#include "core/smp.h"
#include "engine/cache/validity.h"
#include "engine/tier0/internal.h"
#include "engine/tier1/tier1.h"
#include "ia64/semantics.h"
#include "ia64/state.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"
#include "platform/platform.h"
#pragma weak t1_get_stats
#pragma weak t1_form_stops
#pragma weak t1_psr_bits
#pragma weak t1_helper_calls
#pragma weak t1_form_calls
#pragma weak helper_active
#pragma weak helper_get_stats

#define PROFILE_SLOTS 16384u
#define PROFILE_HOST_TOP 160u
#define PROFILE_GUEST_TOP 80u
/* Host keys: a VMM text RIP, or one of these for generated code. */
#define PROFILE_TIER0_CODE UINT64_C(1)
#define PROFILE_OTHER_CODE UINT64_C(2)
#define PROFILE_PARKED UINT64_C(3)
struct profile_entry {
    uint64_t key, count;
};
struct profile_table {
    struct profile_entry e[PROFILE_SLOTS];
    uint64_t total, dropped;
};
static struct profile_table *host_table, *guest_table, *helper_table;
static uint64_t period, interval, next_sample, next_dump, interval_start;
static uint64_t mailbox[256];
static const char *requested;
struct profile_cpu {
    uint64_t retired, exec, idle, timer_irqs, ipis;
};
static struct profile_cpu last[IA64VMM_MAX_CPUS];
static struct tier0_stats tier0_last, tier0_now;
static struct code_stay_stats stay_last;
static struct t1_stats t1_last;
static uint64_t stops_last[2][IA64_OP_COUNT], helper_calls_last[IA64_OP_COUNT],
    calls_last[T1_CALL_NOTES];
static uint64_t psr_bits_last[2][64];
static struct code_validity_stats validity_last;
static struct helper_stats helper_last;
static uint64_t helper_ipis_last;
static struct code_lock_stats lock_last;

/* Tier0 counters of the interval and its most frequent helper opcodes. */
static void dump_tier0(void)
{
    if (!tier0_arena_initialized())
        return;
    tier0_get_stats(&tier0_now);
    const struct tier0_stats *a = &tier0_now, *b = &tier0_last;
#define D(field) (unsigned long long)(a->field - b->field)
    LOGI(
        "profile tier0 executed=%llu translations=%llu hits=%llu lookups=%llu probes=%llu boundary_hits=%llu "
        "boundary_misses=%llu boundary_conflicts=%llu boundary_stale=%llu fetch_checks=%llu fetch_faults=%llu interrupts=%llu "
        "evictions=%llu page_entries=%llu translate_ticks=%llu",
        D(executed_instructions), D(translations), D(hits), D(lookups), D(lookup_probes),
        D(boundary_hits), D(boundary_misses), D(boundary_conflicts), D(boundary_stale),
        D(fetch_checks), D(fetch_faults), D(interrupts), D(evictions), D(cross_page_validations),
        D(translate_ticks));
    uint64_t interp = 0;
    for (unsigned i = 0; i < TIER0_INTERP_REASON_COUNT; ++i)
        interp += a->interpreter_instructions[i] - b->interpreter_instructions[i];
    uint64_t helpers = 0;
    for (unsigned i = 0; i < IA64_OP_COUNT; ++i)
        helpers += a->helpers[i] - b->helpers[i];
    LOGI("profile tier0 interpreter=%llu helpers=%llu", (unsigned long long)interp,
         (unsigned long long)helpers);
    char line[200];
    size_t used = 0;
    for (unsigned k = 0; k < 24; ++k) {
        unsigned best = IA64_OP_COUNT;
        uint64_t count = 0;
        for (unsigned i = 0; i < IA64_OP_COUNT; ++i) {
            uint64_t n = a->helpers[i] - b->helpers[i];
            if (n > count) {
                count = n;
                best = i;
            }
        }
        if (best == IA64_OP_COUNT)
            break;
        used += (size_t)vmm_snprintf(line + used, sizeof(line) - used, " %s:%llu",
                                     ia64_opcode_info(best)->name, (unsigned long long)count);
        tier0_last.helpers[best] = tier0_now.helpers[best];
        if (used > sizeof(line) - 50) {
            LOGI("profile helpers%s", line);
            used = 0;
        }
    }
    if (used)
        LOGI("profile helpers%s", line);
#undef D
    tier0_last = tier0_now;
    struct code_stay_stats st;
    code_stay_get_stats(&st);
    struct code_validity_stats vs;
    code_validity_get_stats(&vs);
    LOGI("profile validator comparisons=%llu mismatches=%llu invalidations=%llu evictions=%llu "
         "snapshot_bytes=%llu",
         (unsigned long long)(vs.comparisons - validity_last.comparisons),
         (unsigned long long)(vs.mismatches - validity_last.mismatches),
         (unsigned long long)(vs.invalidations - validity_last.invalidations),
         (unsigned long long)(vs.evictions - validity_last.evictions),
         (unsigned long long)vs.snapshot_bytes);
    validity_last = vs;
    LOGI(
        "profile stays page=%llu reset=%llu event=%llu epoch=%llu fetch=%llu bundles_compared=%llu mmu=%llu "
        "evicted=%llu",
        (unsigned long long)(st.page - stay_last.page),
        (unsigned long long)(st.reset - stay_last.reset),
        (unsigned long long)(st.event - stay_last.event),
        (unsigned long long)(st.epoch - stay_last.epoch),
        (unsigned long long)(st.fetch - stay_last.fetch),
        (unsigned long long)(st.bundles_compared - stay_last.bundles_compared),
        (unsigned long long)(st.mmu - stay_last.mmu),
        (unsigned long long)(st.evicted - stay_last.evicted));
    stay_last = st;
}

static unsigned long long ms(uint64_t ticks)
{
    return (unsigned long long)(ticks * 1000 / hal_counter_hz());
}

/* The most frequent formation stops of the interval (t1_form_stops). */
static void dump_stops(bool head)
{
    char line[200];
    size_t used = 0;
    for (unsigned k = 0; k < 12; ++k) {
        unsigned best = IA64_OP_COUNT;
        uint64_t count = 0;
        for (unsigned i = 0; i < IA64_OP_COUNT; ++i) {
            uint64_t n = t1_form_stops(i, head) - stops_last[head][i];
            if (n > count) {
                count = n;
                best = i;
            }
        }
        if (best == IA64_OP_COUNT)
            break;
        used += (size_t)vmm_snprintf(line + used, sizeof(line) - used, " %s:%llu",
                                     ia64_opcode_info(best)->name, (unsigned long long)count);
        stops_last[head][best] += count;
        if (used > sizeof(line) - 50) {
            LOGI("profile tier1 stops %s%s", head ? "head" : "end", line);
            used = 0;
        }
    }
    if (used)
        LOGI("profile tier1 stops %s%s", head ? "head" : "end", line);
    for (unsigned i = 0; i < IA64_OP_COUNT; ++i)
        stops_last[head][i] = t1_form_stops(i, head);
}

/* The opcodes of the interval's most frequent in-region helper calls
 * (t1_helper_calls). */
static void dump_helper_calls(void)
{
    char line[200];
    size_t used = 0;
    for (unsigned k = 0; k < 16; ++k) {
        unsigned best = IA64_OP_COUNT;
        uint64_t count = 0;
        for (unsigned i = 0; i < IA64_OP_COUNT; ++i) {
            uint64_t n = t1_helper_calls(i) - helper_calls_last[i];
            if (n > count) {
                count = n;
                best = i;
            }
        }
        if (best == IA64_OP_COUNT)
            break;
        used += (size_t)vmm_snprintf(line + used, sizeof(line) - used, " %s:%llu",
                                     ia64_opcode_info(best)->name, (unsigned long long)count);
        helper_calls_last[best] += count;
        if (used > sizeof(line) - 50) {
            LOGI("profile tier1 helpers%s", line);
            used = 0;
        }
    }
    if (used)
        LOGI("profile tier1 helpers%s", line);
    for (unsigned i = 0; i < IA64_OP_COUNT; ++i)
        helper_calls_last[i] = t1_helper_calls(i);
}

#if IA64VMM_TIER1_STATS
static void native_histogram(const char *name, const uint64_t *now, const uint64_t *last,
                             unsigned n)
{
    char line[256];
    size_t used = 0;
    for (unsigned i = 0; i < n; ++i) {
        uint64_t count = now[i] - last[i];
        if (!count)
            continue;
        used += (size_t)vmm_snprintf(line + used, sizeof(line) - used, " %u:%llu", i,
                                     (unsigned long long)count);
        if (used > sizeof(line) - 40) {
            LOGI("profile tier1 native %s%s", name, line);
            used = 0;
        }
    }
    if (used)
        LOGI("profile tier1 native %s%s", name, line);
}
#endif

/* Tier1 counters of the interval (tier1.h struct t1_stats), the helper
 * queue's (core/helper.h) and, in statistics builds, code_lock's. */
static void dump_tier1(void)
{
    struct t1_stats now;
    t1_get_stats(&now);
    const struct t1_stats *a = &now, *b = &t1_last;
#define D(field) (unsigned long long)(a->field - b->field)
    LOGI(
        "profile tier1 compiles=%llu rejects=%llu deferrals=%llu compile_ms=%llu "
        "rejects request/form/lift/isel/regalloc/emit/frame/publish/cancel=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
        D(compiles), D(rejects), D(memory_deferrals), ms(a->compile_ticks - b->compile_ticks),
        D(rejects_by_stage[0]), D(rejects_by_stage[1]), D(rejects_by_stage[2]),
        D(rejects_by_stage[3]), D(rejects_by_stage[4]), D(rejects_by_stage[5]),
        D(rejects_by_stage[6]), D(rejects_by_stage[7]), D(rejects_by_stage[8]));
    LOGI(
        "profile tier1 entries=%llu retired=%llu exits exit/ckpt/site/helper=%llu/%llu/%llu/%llu enters=%llu "
        "ran=%llu ran_pending=%llu stops=%llu declines link/budget/rse/nat/fetch/validate/retired/lower="
        "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
        D(entries), D(tier1_retired), D(exits_by_kind[0]), D(exits_by_kind[1]), D(exits_by_kind[2]),
        D(exits_by_kind[3]), D(upper_enters), D(upper_ran), D(upper_ran_pending), D(upper_stops),
        D(decline_link), D(decline_budget), D(decline_rse), D(decline_nat), D(decline_fetch),
        D(decline_validate), D(decline_retired), D(decline_lower));
    LOGI(
        "profile tier1 probes=%llu links=%llu relinks=%llu variants=%llu psr_misses=%llu hot=%llu linked=%llu "
        "pending=%llu rejected=%llu deferred=%llu held=%llu unrequestable=%llu refused=%llu lower=%llu "
        "psr_variants=%llu holds=%llu reject_holds=%llu demotions profit/nat/site=%llu/%llu/%llu "
        "head_rejects=%llu rejected_heads=%llu hold_collisions=%llu",
        D(probes), D(probe_links), D(relinks), D(variant_compiles), D(variant_psr_misses),
        D(hot_calls), D(hot_linked), D(hot_pending), D(hot_rejected), D(hot_deferred), D(hot_held),
        D(hot_unrequestable), D(hot_submit_refusals), D(lower_refusals), D(hot_psr_variants),
        D(holds), D(reject_holds), D(demote_profit), D(demote_nat), D(demote_site),
        D(hot_head_rejects), D(rejected_heads), D(hold_collisions));
    LOGI(
        "profile tier1 evictions=%llu replacements=%llu recompiles=%llu invalidations=%llu reclaimed=%llu "
        "seals=%llu chunk_recycles=%llu window_misses=%llu traps=%llu trap_fills=%llu site_exits=%llu "
        "fetch_guards=%llu fetch_hits=%llu sync_waits=%llu refusals=%llu arena_bytes=%llu memory_bytes=%llu",
        D(evictions), D(replacements), D(recompiles), D(invalidations), D(reclaimed), D(seals),
        D(chunk_recycles), D(window_misses), D(traps), D(trap_fills), D(site_exits),
        D(fetch_guards), D(fetch_cache_hits), D(sync_waits), D(memory_refusals),
        (unsigned long long)a->arena_bytes, (unsigned long long)a->memory_bytes);
#if IA64VMM_TIER1_STATS
    const struct t1_arena_stats *arena = &a->arena;
    LOGI("profile tier1 arena chunks free/open/closed/raw=%llu/%llu/%llu/%llu "
         "code_alloc pending/published/retired=%llu/%llu/%llu "
         "open_tail=%llu closed_tail=%llu unsealed_used=%llu "
         "sealed_empty=%llu sealed_code=%llu sealed_padding=%llu sealed_holes=%llu "
         "span_reuses=%llu span_pages=%llu",
         (unsigned long long)arena->chunks[0], (unsigned long long)arena->chunks[1],
         (unsigned long long)arena->chunks[2], (unsigned long long)arena->chunks[3],
         (unsigned long long)arena->code_bytes[0], (unsigned long long)arena->code_bytes[1],
         (unsigned long long)arena->code_bytes[2], (unsigned long long)arena->open_tail,
         (unsigned long long)arena->closed_tail, (unsigned long long)arena->unsealed_used,
         (unsigned long long)arena->sealed_empty, (unsigned long long)arena->sealed_code,
         (unsigned long long)arena->sealed_padding, (unsigned long long)arena->sealed_holes,
         D(span_reuses), D(span_reuse_pages));
#endif
    LOGI(
        "profile tier1 memory deferrals pressure id/chunk/bytes=%llu/%llu/%llu refused id/chunk/bytes="
        "%llu/%llu/%llu hot id/chunk/bytes=%llu/%llu/%llu published_bytes=%llu regions=%llu capacity=%llu "
        "workspace_bytes=%llu",
        D(deferrals_pressure[T1_DEFER_ID]), D(deferrals_pressure[T1_DEFER_CHUNK]),
        D(deferrals_pressure[T1_DEFER_BYTES]), D(deferrals_refused[T1_DEFER_ID]),
        D(deferrals_refused[T1_DEFER_CHUNK]), D(deferrals_refused[T1_DEFER_BYTES]),
        D(hot_deferrals[T1_DEFER_ID]), D(hot_deferrals[T1_DEFER_CHUNK]),
        D(hot_deferrals[T1_DEFER_BYTES]), D(published_bytes), (unsigned long long)a->regions,
        (unsigned long long)a->region_capacity, (unsigned long long)a->memory_workspace);
#define E(field)                                                                                   \
    D(field[T1_END_LIMIT]), D(field[T1_END_BRANCH]), D(field[T1_END_CALL]), D(field[T1_END_RET]),  \
        D(field[T1_END_ALLOC]), D(field[T1_END_RSE]), D(field[T1_END_NAT]), D(field[T1_END_OTHER])
    LOGI(
        "profile tier1 chain links=%llu returns pair/break/state/interrupt/successor/contract/fetch/budget/nat/"
        "validate/retired=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
        D(chain_links), D(chain_returns[T1_CHAIN_PAIR]), D(chain_returns[T1_CHAIN_BREAK]),
        D(chain_returns[T1_CHAIN_STATE]), D(chain_returns[T1_CHAIN_INTERRUPT]),
        D(chain_returns[T1_CHAIN_SUCCESSOR]), D(chain_returns[T1_CHAIN_CONTRACT]),
        D(chain_returns[T1_CHAIN_FETCH]), D(chain_returns[T1_CHAIN_BUDGET]),
        D(chain_returns[T1_CHAIN_NAT]), D(chain_returns[T1_CHAIN_VALIDATE]),
        D(chain_returns[T1_CHAIN_RETIRED]));
    LOGI("profile tier1 native links=%llu batches=%llu plans=%llu reuses=%llu "
         "declines virtual/timer/shape/context=%llu/%llu/%llu/%llu",
         D(chain_native_links), D(chain_native_batches), D(chain_native_plans),
         D(chain_native_reuses), D(chain_native_decline_virtual), D(chain_native_decline_timer),
         D(chain_native_decline_shape), D(chain_native_decline_context));
#if IA64VMM_TIER1_STATS
    LOGI("profile tier1 native topology hits/misses/negative/retained=%llu/%llu/%llu/%llu",
         D(native_topology_hits), D(native_topology_misses), D(native_topology_negative),
         D(native_topology_retained));
    LOGI(
        "profile tier1 native rebuild empty/retirement/mmu/psr/cfm/bind/token/iseq/pta/region/rebase="
        "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu discard learned/failed/rebuild=%llu/%llu/%llu",
        D(native_rebuild[0]), D(native_rebuild[1]), D(native_rebuild[2]), D(native_rebuild[3]),
        D(native_rebuild[4]), D(native_rebuild[5]), D(native_rebuild[6]), D(native_rebuild[7]),
        D(native_rebuild[8]), D(native_rebuild[9]), D(native_rebuild[10]),
        D(native_discard_learned), D(native_discard_failed), D(native_discard_rebuild));
    LOGI(
        "profile tier1 native node_reject capacity/shape/window/fetch/witness/group/successor/contract="
        "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
        D(native_node_reject[0]), D(native_node_reject[1]), D(native_node_reject[2]),
        D(native_node_reject[3]), D(native_node_reject[4]), D(native_node_reject[5]),
        D(native_node_reject[6]), D(native_node_reject[7]));
    native_histogram("build_nodes", a->native_build_nodes, b->native_build_nodes, 9);
    native_histogram("build_edges", a->native_build_edges, b->native_build_edges, 257);
    native_histogram("hops", a->native_hops, b->native_hops, 33);
    for (unsigned graph = 0; graph < 2; ++graph)
        for (unsigned ran = 0; ran < 2; ++ran) {
            LOGI(
                "profile tier1 native stop edges=%s hops=%s "
                "batch/point/edge/event/context/timer/budget/window/source/witness/second/mf/nat/fr/publish="
                "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
                graph ? "multiple" : "one", ran ? "nonzero" : "zero", D(native_stop[graph][ran][0]),
                D(native_stop[graph][ran][1]), D(native_stop[graph][ran][2]),
                D(native_stop[graph][ran][3]), D(native_stop[graph][ran][4]),
                D(native_stop[graph][ran][5]), D(native_stop[graph][ran][6]),
                D(native_stop[graph][ran][7]), D(native_stop[graph][ran][8]),
                D(native_stop[graph][ran][9]), D(native_stop[graph][ran][10]),
                D(native_stop[graph][ran][11]), D(native_stop[graph][ran][12]),
                D(native_stop[graph][ran][13]), D(native_stop[graph][ran][14]));
        }
#endif
    LOGI(
        "profile tier1 ends limit/branch/call/ret/alloc/rse/nat/other exits=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
        "returns=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu gaps=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
        "spills=%llu spill_exits=%llu",
        E(exits_by_end), E(returns_by_end), E(gap_by_end), D(spills), D(spill_exits));
#undef E
#undef D
    t1_last = now;
    if (t1_form_stops) {
        dump_stops(true);
        dump_stops(false);
    }
    if (t1_form_calls) {
        uint64_t n[T1_CALL_NOTES];
        for (unsigned k = 0; k < T1_CALL_NOTES; ++k) {
            n[k] = t1_form_calls((enum t1_call_note)k) - calls_last[k];
            calls_last[k] += n[k];
        }
        LOGI("profile tier1 calls followed/page/other=%llu/%llu/%llu",
             (unsigned long long)n[T1_CALL_FOLLOWED], (unsigned long long)n[T1_CALL_PAGE],
             (unsigned long long)n[T1_CALL_OTHER]);
    }
    if (t1_helper_calls)
        dump_helper_calls();
    for (unsigned kind = 0; t1_psr_bits && kind < 2; ++kind) {
        char line[200];
        size_t used = 0;
        for (unsigned bit = 0; bit < 64; ++bit) {
            uint64_t n = t1_psr_bits((enum t1_psr_count)kind, bit);
            if (n != psr_bits_last[kind][bit] && used < sizeof(line) - 30)
                used += (size_t)vmm_snprintf(line + used, sizeof(line) - used, " %u:%llu", bit,
                                             (unsigned long long)(n - psr_bits_last[kind][bit]));
            psr_bits_last[kind][bit] = n;
        }
        if (used)
            LOGI("profile tier1 psr_bits %s%s", kind == T1_PSR_VARIANT ? "variant" : "lower", line);
    }
    if (helper_get_stats && helper_active && helper_active()) {
        struct helper_stats h;
        helper_get_stats(&h);
        uint64_t ipis = 0;
        for (unsigned i = 0; i < hal_helper_count(); ++i) {
            uint64_t timer, ipi;
            hal_cpu_interrupts(hal_helper_cpu(i), &timer, &ipi);
            ipis += ipi;
        }
        LOGI(
            "profile helper_queue submitted=%llu refused=%llu queued=%llu completed=%llu cancelled=%llu "
            "steps=%llu compile_ms=%llu maintenance_ms=%llu wait_ms=%llu ipis=%llu",
            (unsigned long long)(h.submitted - helper_last.submitted),
            (unsigned long long)(h.refused - helper_last.refused),
            (unsigned long long)(h.queued - helper_last.queued),
            (unsigned long long)(h.completed - helper_last.completed),
            (unsigned long long)(h.cancelled - helper_last.cancelled),
            (unsigned long long)(h.steps - helper_last.steps),
            ms(h.compile_ticks - helper_last.compile_ticks),
            ms(h.maintenance_ticks - helper_last.maintenance_ticks),
            ms(h.wait_ticks - helper_last.wait_ticks),
            (unsigned long long)(ipis - helper_ipis_last));
        helper_last = h;
        helper_ipis_last = ipis;
    }
    if (IA64VMM_TIER0_STATS) {
        struct code_lock_stats l;
        code_lock_get_stats(&l);
        LOGI("profile code_lock acquisitions=%llu wait_ms=%llu",
             (unsigned long long)(l.acquisitions - lock_last.acquisitions),
             ms(l.wait_ticks - lock_last.wait_ticks));
        lock_last = l;
    }
}

void vmm_profile_sample(unsigned cpu, uint64_t rip)
{
    if (cpu < IA64VMM_ARRAY_SIZE(mailbox))
        __atomic_store_n(&mailbox[cpu], rip, __ATOMIC_RELAXED);
}

static bool decimal(const char *value, size_t length, uint64_t limit, uint64_t *out)
{
    uint64_t n = 0;
    if (!length)
        return false;
    for (size_t i = 0; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9')
            return false;
        n = n * 10 + (unsigned)(value[i] - '0');
        if (n > limit)
            return false;
    }
    *out = n;
    return true;
}

bool profile_configure(const char *cmdline)
{
    const char *value;
    size_t length;
    uint64_t hz = 0, sec = 30;
    if (!platform_cmdline_value(cmdline, "profile_hz", &value, &length))
        return true;
    if (!decimal(value, length, 100000, &hz) || !hz)
        return false;
    if (platform_cmdline_value(cmdline, "profile_sec", &value, &length) &&
        (!decimal(value, length, 86400, &sec) || !sec))
        return false;
    size_t pages = (sizeof(struct profile_table) + 4095) / 4096;
    host_table = hal_alloc_pages(pages, HAL_PAGE_ZERO);
    guest_table = hal_alloc_pages(pages, HAL_PAGE_ZERO);
    helper_table = hal_alloc_pages(pages, HAL_PAGE_ZERO);
    if (!host_table || !guest_table || !helper_table)
        return false;
    uint64_t now = hal_counter();
    period = hal_counter_hz() / hz;
    interval = hal_counter_hz() * sec;
    next_sample = now + period;
    next_dump = now + interval;
    interval_start = now;
    LOGI("profile: %llu Hz, interval %llu s, tier0 arena %p+%llx", (unsigned long long)hz,
         (unsigned long long)sec, (void *)tier0_arena.code,
         (unsigned long long)tier0_arena.code_pages * 4096);
    return true;
}

static void table_add(struct profile_table *t, uint64_t key)
{
    ++t->total;
    uint64_t h = (key * UINT64_C(0x9e3779b97f4a7c15)) >> 50;
    for (unsigned n = 0; n < 256; ++n) {
        struct profile_entry *e = &t->e[(h + n) & (PROFILE_SLOTS - 1)];
        if (e->count && e->key == key) {
            ++e->count;
            return;
        }
        if (!e->count) {
            e->key = key;
            e->count = 1;
            return;
        }
    }
    ++t->dropped;
}

static uint64_t host_key(uint64_t rip)
{
    if (hal_text_contains(rip))
        return rip;
    uintptr_t base = (uintptr_t)tier0_arena.code;
    if (base && rip >= base && rip - base < tier0_arena.code_pages * 4096)
        return PROFILE_TIER0_CODE;
    return PROFILE_OTHER_CODE;
}

uint64_t profile_tick(struct vm *vm)
{
    if (!host_table)
        return UINT64_MAX;
    uint64_t now = hal_counter();
    if (now >= next_sample) {
        for (unsigned i = 0; i < vm->count; ++i) {
            struct vm_cpu *c = &vm->cpu[i];
            if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != VM_CPU_RUNNING ||
                c->host_cpu >= IA64VMM_ARRAY_SIZE(mailbox))
                continue;
            uint64_t rip = __atomic_exchange_n(&mailbox[c->host_cpu], 0, __ATOMIC_RELAXED);
            if (rip)
                table_add(host_table, host_key(rip));
            uint64_t ip = __atomic_load_n(&c->v->s.ip, __ATOMIC_RELAXED);
            uint64_t psr = __atomic_load_n(&c->v->s.psr, __ATOMIC_RELAXED);
            bool parked = __atomic_load_n(&c->v->parked, __ATOMIC_RELAXED) != 0;
            table_add(guest_table, (ip & ~UINT64_C(15)) | ((psr >> 32) & 3) |
                                       (psr & IA64_PSR_IT ? 4u : 0u) | (parked ? 8u : 0u));
            /* A parked executor idles in the HAL; an IPI would only wake it. */
            if (parked)
                table_add(host_table, PROFILE_PARKED);
            else
                hal_cpu_sample(c->host_cpu);
        }
        /* Helper CPUs (core/helper.h): an idle helper is sampled in the HAL's idle. */
        for (unsigned i = 0; helper_active && helper_active() && i < hal_helper_count(); ++i) {
            unsigned cpu = hal_helper_cpu(i);
            if (cpu >= IA64VMM_ARRAY_SIZE(mailbox))
                continue;
            uint64_t rip = __atomic_exchange_n(&mailbox[cpu], 0, __ATOMIC_RELAXED);
            if (rip)
                table_add(helper_table, host_key(rip));
            hal_cpu_sample(cpu);
        }
        next_sample += period;
        if (next_sample <= now)
            next_sample = now + period;
    }
    const char *label = __atomic_exchange_n(&requested, NULL, __ATOMIC_ACQ_REL);
    if (label || now >= next_dump)
        profile_dump(label ? label : "interval");
    return next_sample < next_dump ? next_sample : next_dump;
}

static void dump_table(const char *kind, struct profile_table *t, unsigned top)
{
    struct profile_entry best[PROFILE_HOST_TOP];
    unsigned n = 0;
    for (unsigned i = 0; i < PROFILE_SLOTS; ++i) {
        struct profile_entry e = t->e[i];
        if (!e.count || (n == top && e.count <= best[n - 1].count))
            continue;
        unsigned at = n < top ? n++ : n - 1;
        while (at && best[at - 1].count < e.count) {
            best[at] = best[at - 1];
            --at;
        }
        best[at] = e;
    }
    char line[200];
    size_t used = 0;
    for (unsigned i = 0; i < n; ++i) {
        used += (size_t)vmm_snprintf(line + used, sizeof(line) - used, " %llx:%llu",
                                     (unsigned long long)best[i].key,
                                     (unsigned long long)best[i].count);
        if (used > sizeof(line) - 40 || i + 1 == n) {
            LOGI("profile %s%s", kind, line);
            used = 0;
        }
    }
    memset(t, 0, sizeof(*t));
}

void profile_request_dump(const char *label)
{
    if (host_table)
        __atomic_store_n(&requested, label, __ATOMIC_RELEASE);
}

void profile_dump(const char *label)
{
    if (!host_table)
        return;
    struct vm *vm = vm_current();
    uint64_t now = hal_counter(), hz = hal_counter_hz();
    LOGI(
        "profile %s seconds=%llu host_samples=%llu host_dropped=%llu guest_samples=%llu guest_dropped=%llu",
        label, (unsigned long long)((now - interval_start) / hz),
        (unsigned long long)host_table->total, (unsigned long long)host_table->dropped,
        (unsigned long long)guest_table->total, (unsigned long long)guest_table->dropped);
    for (unsigned i = 0; vm && i < vm->count; ++i) {
        struct vm_cpu *c = &vm->cpu[i];
        if (!c->v)
            continue;
        uint64_t retired = __atomic_load_n(&c->v->retired, __ATOMIC_RELAXED);
        uint64_t exec = __atomic_load_n(&c->exec_ticks, __ATOMIC_RELAXED);
        uint64_t idle = __atomic_load_n(&c->idle_ticks, __ATOMIC_RELAXED);
        uint64_t timer_irqs, ipis;
        hal_cpu_interrupts(c->host_cpu, &timer_irqs, &ipis);
        LOGI("profile vCPU%u retired=%llu exec_ms=%llu idle_ms=%llu timer_irqs=%llu ipis=%llu", i,
             (unsigned long long)(retired - last[i].retired),
             (unsigned long long)((exec - last[i].exec) * 1000 / hz),
             (unsigned long long)((idle - last[i].idle) * 1000 / hz),
             (unsigned long long)(timer_irqs - last[i].timer_irqs),
             (unsigned long long)(ipis - last[i].ipis));
        last[i] = (struct profile_cpu){retired, exec, idle, timer_irqs, ipis};
    }
    dump_tier0();
    if (t1_get_stats && vm && vm->cpu[0].v && vm->cpu[0].v->upper)
        dump_tier1();
    dump_table("host", host_table, PROFILE_HOST_TOP);
    dump_table("guest", guest_table, PROFILE_GUEST_TOP);
    if (helper_table->total)
        dump_table("helper", helper_table, PROFILE_HOST_TOP);
    interval_start = now;
    next_dump = now + interval;
}
