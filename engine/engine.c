/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/interp/interp.h"
#include "engine/tier0/tier0.h" /* Tier0 dispatch; interruption delivery stays in interp. */
#include "engine/cache/cache.h"
#include "engine/tier1/tier1.h"
#include "engine/upper.h"
#include "ia64vmm/log.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/memory_policy.h"
#include "ia64vmm/string.h"
#include "ia64/ia32/run.h"

/* mk/tier1.mk passes it to every object; a build without it times nothing
 * in Tier1. */
#ifndef IA64VMM_TIER1_STATS
#define IA64VMM_TIER1_STATS 0
#endif

static vcpu_break_hook_fn break_hook;

/* Small test binaries may omit the native backend and dispatcher.
 * Never turn a missing native runner into a successful yield or guest #UD. */
__attribute__((weak)) const struct ia32_backend_ops *hal_ia32_backend(void)
{
    return NULL;
}
__attribute__((weak)) bool hal_ia32_backing_hpa(const void *ptr, hpa_t *out)
{
    (void)ptr;
    (void)out;
    return false;
}
__attribute__((weak)) const struct engine_ia32_ops *engine_ia32_glue(void)
{
    return NULL;
}

static struct vcpu_position isa_position(const struct vcpu *v)
{
    bool native = (v->s.psr & IA64_PSR_IS) != 0;
    /* Before native state is imported, v->s.ip is already the linear byte
     * address. An EIP is not fabricated from an unconverted CSD. */
    return (struct vcpu_position){.isa = native ? VCPU_ISA_IA32 : VCPU_ISA_IA64,
                                  .ip = native ? (uint32_t)v->s.ip : v->s.ip,
                                  .slot = native ? 0 : v->s.slot};
}

__attribute__((weak)) void vcpu_dump_isa(const struct vcpu *v)
{
    struct vcpu_position p = isa_position(v);
    LOGE("ISA=%s IP=%016llx slot=%u native snapshot unavailable",
         p.isa == VCPU_ISA_IA32 ? "IA32" : "IA64", (unsigned long long)p.ip, p.slot);
    if (p.isa == VCPU_ISA_IA64) {
        vcpu_dump(v);
        return;
    }
    /* The legacy dump fetches/decodes a bundle. Without an IA-32 engine to
     * supply a native dump, log source storage without fetching any guest bytes. */
    const struct ia64_state *s = &v->s;
    LOGI("IA32 source PSR=%016llx CFM=%016llx PR=%016llx retired=%llu RSE=%u/%u/%u",
         (unsigned long long)s->psr, (unsigned long long)s->cfm, (unsigned long long)s->pr,
         (unsigned long long)v->retired, s->rse_bof, s->rse_dirty, s->rse_missing);
    for (unsigned i = 0; i < 128; ++i) {
        if (s->gr[i] || s->nat[i])
            LOGI(" raw-r%u=%016llx NaT=%u", i, (unsigned long long)s->gr[i], s->nat[i]);
        if (s->fr[i].significand || s->fr[i].exponent || s->fr[i].sign)
            LOGI(" raw-f%u=%u:%05x:%016llx", i, s->fr[i].sign, s->fr[i].exponent,
                 (unsigned long long)s->fr[i].significand);
        LOGI(" ar%u=%016llx cr%u=%016llx", i, (unsigned long long)s->ar[i], i,
             (unsigned long long)s->cr[i]);
    }
    for (unsigned i = 0; i < 8; ++i)
        LOGI(" b%u=%016llx rr%u=%016llx ibr%u=%016llx dbr%u=%016llx", i,
             (unsigned long long)s->br[i], i, (unsigned long long)s->rr[i], i,
             (unsigned long long)s->ibr[i], i, (unsigned long long)s->dbr[i]);
    for (unsigned i = 0; i < 16; ++i)
        LOGI(" bank-r%u=%016llx NaT=%u pkr%u=%016llx", i + 16, (unsigned long long)s->bank_gr[i],
             s->bank_nat[i], i, (unsigned long long)s->pkr[i]);
}

__attribute__((weak)) bool vcpu_snapshot_isa(const struct vcpu *v, struct vcpu_isa_snapshot *out)
{
    (void)v;
    (void)out;
    return false;
}

__attribute__((weak)) struct vcpu_run_exit engine_run_bounded(struct vcpu *v,
                                                              const struct vcpu_run_budget *budget)
{
    (void)budget;
    struct vcpu_run_exit out = {.stop = VCPU_RUN_EXIT,
                                .reason = VCPU_EXIT_UNIMPLEMENTED,
                                .position = isa_position(v),
                                .info = v->s.psr,
                                .detail = "bounded ISA dispatcher not built"};
    vcpu_dump_isa(v);
    return out;
}

void engine_set_break_hook(vcpu_break_hook_fn hook)
{
    /* A checked region may contain an ordinary break only without an
     * opaque callback. Changing that premise invalidates its translation. */
    if (hook != break_hook)
        code_cache_flush();
    break_hook = hook;
}
bool ia64_break_hook_installed(void)
{
    return break_hook != NULL;
}
bool ia64_break_hook(struct vcpu *v, uint64_t imm, uint64_t ip)
{
    return break_hook && break_hook(v, imm, ip);
}

/*
 * engine=tier1: Tier0 is the
 * outer loop and Tier1 its upper tier. This file composes Tier0's hook from
 * Tier1's glue table and is the only place that registers it, so no other
 * engine ever runs Tier1 code -- except engine=lockstep
 * with tier1=on, which runs it checked.
 *
 * Tier1 lives in engine/tier1 and engine/hostwin, which the per-module
 * test binaries that link this file do not link. For them this weak
 * default stands in; a binary that links engine/tier1, including the
 * product, gets its
 * definition (dispatch.c). Without Tier1, engine=tier1 is a setup error.
 */
__attribute__((weak)) const struct t1_engine_glue *engine_tier1_glue(void)
{
    return NULL;
}

_Static_assert((int)T1_UPPER_DECLINED == (int)TIER0_UPPER_DECLINED &&
                   (int)T1_UPPER_RAN == (int)TIER0_UPPER_RAN &&
                   (int)T1_UPPER_RAN_PENDING == (int)TIER0_UPPER_RAN_PENDING &&
                   (int)T1_UPPER_STOP == (int)TIER0_UPPER_STOP,
               "upper-tier statuses (tier0.h, tier1.h)");
_Static_assert((int)T1_HOT_LINKED == (int)TIER0_HOT_LINKED &&
                   (int)T1_HOT_PENDING == (int)TIER0_HOT_PENDING &&
                   (int)T1_HOT_REJECTED == (int)TIER0_HOT_REJECTED &&
                   (int)T1_HOT_DEFERRED == (int)TIER0_HOT_DEFERRED,
               "hot statuses (tier0.h, tier1.h)");

static const struct t1_engine_glue *t1;
static bool tier1_configured, tier1_lockstep;
uint64_t engine_tier1_reserve_bytes(void)
{
    return tier1_configured && t1->reserve_bytes ? t1->reserve_bytes() : 0;
}

void engine_tier1_memory_budget(uint64_t free_bytes, uint64_t snapshot_bytes,
                                uint64_t reserve_bytes)
{
    if (!tier1_configured || !t1->memory_budget)
        return;
    uint64_t lazy = code_cache_reserve_bytes();
    uint64_t unavailable = snapshot_bytes > UINT64_MAX - lazy ? UINT64_MAX : snapshot_bytes + lazy;
    t1->memory_budget(free_bytes, unavailable, reserve_bytes);
}

/* enter and hot return Tier1's enums (equal values, asserted above), so
 * Tier0 calls them through these wrappers; probe and reclaim have Tier0's
 * types and are Tier1's functions themselves (one indirect call per Tier0
 * loop iteration for reclaim, not two). */
static enum tier0_upper_status upper_enter(struct vcpu *v, uint64_t *link, uint64_t region_budget,
                                           struct vcpu_exit *out)
{
    return (enum tier0_upper_status)t1->enter(v, link, region_budget, out);
}
static enum tier0_upper_hot upper_hot(struct vcpu *v, gpa_t entry_pa, bool loop_head)
{
    return (enum tier0_upper_hot)t1->hot(v, entry_pa, loop_head);
}
static struct tier0_upper_ops tier1_ops = {.enter = upper_enter, .hot = upper_hot};

const struct tier0_upper_ops *engine_tier1_ops(void)
{
    if (!t1)
        t1 = engine_tier1_glue();
    if (!t1)
        return NULL;
    const struct t1_config *c = t1->config();
    tier1_ops.probe = t1->probe;
    tier1_ops.reclaim = t1->reclaim;
    tier1_ops.sequence = t1->sequence();
    tier1_ops.heads = t1->heads();
    tier1_ops.head_mask = T1_HEAD_SEQUENCES - 1u;
    tier1_ops.reclaim_due = t1->reclaim_due();
    tier1_ops.lower_psr = IA64_PSR_BE | IA64_PSR_IS | T1_PSR_LOWER_TIER;
    tier1_ops.loop_threshold = c->loop_threshold;
    tier1_ops.trace_threshold = c->trace_threshold;
    return &tier1_ops;
}

/* A positive decimal `key=` value at most `max`; *value untouched when the
 * key is absent. false: present but invalid. */
static bool option(const char *cmdline, const char *key, uint64_t max, uint64_t *value)
{
    size_t n = strlen(key);
    for (const char *at = cmdline; *at;) {
        while (*at == ' ' || *at == '\t')
            ++at;
        const char *end = at;
        while (*end && *end != ' ' && *end != '\t')
            ++end;
        if ((size_t)(end - at) > n && !strncmp(at, key, n) && at[n] == '=') {
            uint64_t x = 0;
            const char *p = at + n + 1;
            if (p == end)
                return false;
            for (; p < end; ++p) {
                if (*p < '0' || *p > '9' || x > max)
                    return false;
                x = x * 10 + (unsigned)(*p - '0');
            }
            if (x > max)
                return false;
            *value = x;
            return true;
        }
        at = end;
    }
    return true;
}

/* The `key=` value as text, or NULL when the key is absent; *length is
 * 0 for an empty value. */
static const char *option_text(const char *cmdline, const char *key, size_t *length)
{
    size_t n = strlen(key);
    for (const char *at = cmdline; *at;) {
        while (*at == ' ' || *at == '\t')
            ++at;
        const char *end = at;
        while (*end && *end != ' ' && *end != '\t')
            ++end;
        if ((size_t)(end - at) > n && !strncmp(at, key, n) && at[n] == '=') {
            *length = (size_t)(end - at) - n - 1;
            return at + n + 1;
        }
        at = end;
    }
    return NULL;
}

bool engine_tier1_configure(const char *cmdline, enum code_validity_mode validity)
{
    /* Tier1 requires code validation. */
    if (validity == CODE_VALIDITY_NONE) {
        LOGE("engine=tier1 requires code validation: code_validity=none is a setup error");
        return false;
    }
    /* tier1_code_mib= (reserve-derived default, 16..512), tier1_hot= (the loop threshold, default
     * 64; trace heads wait 16 times longer, default 1024), tier1_slice=
     * (wall-clock slice), tier1_verify=passes|final
     * (IR verification after every optimizer pass, the default, or of the
     * optimized IR only), tier1_chain=on|off (region chaining; default
     * on), tier1_regions= (region records; default: sized with the
     * pool), tier1_min_insns= (the profitability demotion's instructions
     * per entry), hostwin_check= (the MMU log completeness check; unit
     * tests only). */
    struct hal_memory_stats memory;
    uint64_t mib =
        vmm_tier1_code_bytes(hal_memory_get_stats(&memory) ? memory.total_bytes : 0) >> 20;
    uint64_t hot = 64, slice = 16384, check = 0, check_every = 0, check_after = 0;
    uint64_t min_insns = T1_MIN_INSNS_DEFAULT;
    uint64_t regions = UINT64_MAX; /* absent */
    if (!option(cmdline, "tier1_check_every", UINT32_MAX, &check_every) ||
        !option(cmdline, "tier1_check_after", UINT64_C(1) << 60, &check_after) ||
        (check_after && !check_every)) {
        LOGE("invalid Tier1 replay sampling options");
        return false;
    }
#if !defined(IA64VMM_HOSTED) && !defined(IA64VMM_TIER1_CHECKED)
    if (check_every) {
        LOGE("tier1_check_every requires a TIER1_CHECKED=1 build");
        return false;
    }
#endif
    if (!option(cmdline, "tier1_code_mib", 1024, &mib) || !mib) {
        LOGE("invalid tier1_code_mib (expected 1..1024)");
        return false;
    }
    if (!option(cmdline, "tier1_hot", UINT32_MAX / 16, &hot) || !hot) {
        LOGE("invalid tier1_hot (expected a positive loop threshold)");
        return false;
    }
    if (!option(cmdline, "tier1_slice", UINT64_C(1) << 32, &slice) || !slice) {
        LOGE("invalid tier1_slice (expected a positive instruction count)");
        return false;
    }
    if (!option(cmdline, "tier1_regions", T1_MAX_REGIONS_LIMIT, &regions) || !regions) {
        LOGE("invalid tier1_regions (expected 1..%u)", T1_MAX_REGIONS_LIMIT);
        return false;
    }
    if (!option(cmdline, "tier1_min_insns", T1_MAX_INSNS, &min_insns) || !min_insns) {
        LOGE("invalid tier1_min_insns (expected 1..%u)", T1_MAX_INSNS);
        return false;
    }
    if (!option(cmdline, "hostwin_check", 1, &check)) {
        LOGE("invalid hostwin_check (expected 0 or 1)");
        return false;
    }
    size_t verify_length = 0, chain_length = 0;
    const char *verify = option_text(cmdline, "tier1_verify", &verify_length);
    const char *chain = option_text(cmdline, "tier1_chain", &chain_length);
    bool chain_off = chain && chain_length == 3 && !strncmp(chain, "off", 3);
    if (chain && !chain_off && !(chain_length == 2 && !strncmp(chain, "on", 2))) {
        LOGE("invalid tier1_chain (expected on or off)");
        return false;
    }
    bool verify_final = verify && verify_length == 5 && !strncmp(verify, "final", 5);
    if (verify && !verify_final && !(verify_length == 6 && !strncmp(verify, "passes", 6))) {
        LOGE("invalid tier1_verify (expected passes or final)");
        return false;
    }
#ifndef IA64VMM_HOSTED
    if (check) {
        LOGE("hostwin_check=1 is not available in vmm.elf");
        return false;
    }
#endif
    t1 = engine_tier1_glue();
    if (!t1) {
        LOGE("engine=tier1: Tier1 is not linked into this binary");
        return false;
    }
    struct t1_config cfg = {.code_bytes = mib << 20,
                            .wallclock_slice = slice,
                            .loop_threshold = (uint32_t)hot,
                            .trace_threshold = (uint32_t)hot * 16,
                            .min_insns_per_entry = (uint32_t)min_insns,
                            .max_regions = regions == UINT64_MAX ? 0 : (uint32_t)regions,
                            .verify_final = verify_final,
                            .chain_off = chain_off,
                            .check_every = check_every,
                            .check_after = check_after};
    const char *why = NULL;
    if (!t1->setup(&cfg, check != 0, &why)) {
        LOGE("engine=tier1: %s", why ? why : "setup failed");
        return false;
    }
    const struct tier0_upper_ops *ops = engine_tier1_ops();
    tier0_set_upper(ops);
    tier1_configured = true;
    LOGI(
        "engine tier1: Tier0 outer loop, Tier1 upper tier; tier1_code_mib=%llu loop_threshold=%u "
        "trace_threshold=%u tier1_slice=%llu tier1_verify=%s tier1_chain=%s tier1_regions=%u%s tier1_min_insns=%u "
        "hostwin_check=%u",
        (unsigned long long)mib, ops->loop_threshold, ops->trace_threshold,
        (unsigned long long)slice, verify_final ? "final" : "passes", chain_off ? "off" : "on",
        t1->config()->max_regions, regions == UINT64_MAX ? " (sized with the pool)" : "",
        t1->config()->min_insns_per_entry, (unsigned)check);
    if (check_every)
        LOGI("tier1 replay: every=%llu after=%llu retired instructions",
             (unsigned long long)check_every, (unsigned long long)check_after);
    if (check_every)
        LOGW("tier1 replay coverage: store preloads/logging, 256-instruction budget, "
             "memory-helper boundaries; normal generated code is not checked");
    return true;
}

bool engine_tier1_lockstep_configure(const char *cmdline, enum code_validity_mode validity)
{
#if !defined(IA64VMM_HOSTED) && !defined(IA64VMM_TIER1_CHECKED)
    (void)cmdline;
    (void)validity;
    LOGE("engine=lockstep tier1=on requires a TIER1_CHECKED=1 build");
    return false;
#else
    if (!engine_tier1_configure(cmdline, validity))
        return false;
    tier1_lockstep = true;
    LOGW("tier1 replay coverage: store preloads/logging, 256-instruction budget, "
         "memory-helper boundaries; normal generated code is not checked");
    LOGI(
        "engine lockstep tier1=on: Tier0 regions checked by Tier0's lockstep, every Tier1 execution by "
        "Tier1's checker");
    return true;
#endif
}

bool engine_vcpu_attach(struct vcpu *v)
{
    if (!code_cache_vcpu_init(v))
        return false;
    if (v->cfg.engine != ENGINE_INTERP && !tier0_executor_attach(v)) {
        LOGE("Tier0 executor state unavailable for vCPU%u", v->cfg.index);
        return false;
    }
    tier0_vcpu_state_restored(v);
    if (v->cfg.engine != ENGINE_TIER1 && !(v->cfg.engine == ENGINE_LOCKSTEP && tier1_lockstep)) {
        code_register_vcpu(v);
        return true;
    }
    if (!tier1_configured)
        return false;
    struct t1_vcpu *tv = t1->vcpu_create(v);
    if (!tv) {
        LOGE("engine=tier1: no window instance for vCPU%u", v->cfg.index);
        return false;
    }
    v->upper = tv;
    code_register_vcpu(v);
    return true;
}

void engine_vcpu_detach(struct vcpu *v)
{
    const struct engine_ia32_ops *native = engine_ia32_glue();
    if (native)
        native->detach(v);
    code_unregister_vcpu(v);
    if (!v->upper)
        return;
    t1->vcpu_destroy(v->upper);
    v->upper = NULL;
}

void engine_tier1_destroy(void)
{
    if (!tier1_configured)
        return;
    tier0_set_upper(NULL);
    t1->teardown();
    tier1_configured = tier1_lockstep = false;
}

/* One line for every engine (engine=tier0 runs
 * show compiles=0, the proof that no other engine runs Tier1 code). */
void engine_dump_stats(void)
{
    const struct t1_engine_glue *g = t1 ? t1 : engine_tier1_glue();
    struct t1_stats s = {0};
    if (g)
        g->stats(&s);
    LOGI(
        "tier1 linked=%u compiles=%llu rejects=%llu entries=%llu tier1_retired=%llu probes=%llu links=%llu "
        "enters=%llu ran=%llu ran_pending=%llu declines=%llu hot=%llu held=%llu demotions=%llu "
        "evictions=%llu replacements=%llu recompiles=%llu invalidations=%llu site_exits=%llu",
        g != NULL, (unsigned long long)s.compiles, (unsigned long long)s.rejects,
        (unsigned long long)s.entries, (unsigned long long)s.tier1_retired,
        (unsigned long long)s.probes, (unsigned long long)s.probe_links,
        (unsigned long long)s.upper_enters, (unsigned long long)s.upper_ran,
        (unsigned long long)s.upper_ran_pending,
        (unsigned long long)(s.decline_link + s.decline_budget + s.decline_rse + s.decline_nat +
                             s.decline_fetch + s.decline_validate + s.decline_retired +
                             s.decline_lower),
        (unsigned long long)s.hot_calls, (unsigned long long)s.hot_held,
        (unsigned long long)s.demotions, (unsigned long long)s.evictions,
        (unsigned long long)s.replacements, (unsigned long long)s.recompiles,
        (unsigned long long)s.invalidations, (unsigned long long)s.site_exits);
    /* Region chaining: regions entered at chain points,
     * chainable EXIT exits, successor-cache hits and the chain-point
     * returns by row. */
    LOGI(
        "tier1 chain links=%llu chainable=%llu successor_hits=%llu returns pair/break/state/interrupt/successor/"
        "contract/fetch/budget/nat/validate/retired=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
        (unsigned long long)s.chain_links, (unsigned long long)s.chainable,
        (unsigned long long)s.succ_hits, (unsigned long long)s.chain_returns[T1_CHAIN_PAIR],
        (unsigned long long)s.chain_returns[T1_CHAIN_BREAK],
        (unsigned long long)s.chain_returns[T1_CHAIN_STATE],
        (unsigned long long)s.chain_returns[T1_CHAIN_INTERRUPT],
        (unsigned long long)s.chain_returns[T1_CHAIN_SUCCESSOR],
        (unsigned long long)s.chain_returns[T1_CHAIN_CONTRACT],
        (unsigned long long)s.chain_returns[T1_CHAIN_FETCH],
        (unsigned long long)s.chain_returns[T1_CHAIN_BUDGET],
        (unsigned long long)s.chain_returns[T1_CHAIN_NAT],
        (unsigned long long)s.chain_returns[T1_CHAIN_VALIDATE],
        (unsigned long long)s.chain_returns[T1_CHAIN_RETIRED]);
    /* Deferrals by kind (tier1.h enum t1_defer_kind): before compiling,
     * refused during the compile, and hot() calls before a request. */
    LOGI(
        "tier1 memory budget_bytes=%llu reserve_bytes=%llu used_bytes=%llu peak_bytes=%llu refusals=%llu "
        "deferrals=%llu pressure id/chunk/bytes=%llu/%llu/%llu refused id/chunk/bytes=%llu/%llu/%llu "
        "hot id/chunk/bytes=%llu/%llu/%llu regions=%llu capacity=%llu",
        (unsigned long long)s.memory_budget, (unsigned long long)s.memory_reserve,
        (unsigned long long)s.memory_bytes, (unsigned long long)s.memory_peak,
        (unsigned long long)s.memory_refusals, (unsigned long long)s.memory_deferrals,
        (unsigned long long)s.deferrals_pressure[T1_DEFER_ID],
        (unsigned long long)s.deferrals_pressure[T1_DEFER_CHUNK],
        (unsigned long long)s.deferrals_pressure[T1_DEFER_BYTES],
        (unsigned long long)s.deferrals_refused[T1_DEFER_ID],
        (unsigned long long)s.deferrals_refused[T1_DEFER_CHUNK],
        (unsigned long long)s.deferrals_refused[T1_DEFER_BYTES],
        (unsigned long long)s.hot_deferrals[T1_DEFER_ID],
        (unsigned long long)s.hot_deferrals[T1_DEFER_CHUNK],
        (unsigned long long)s.hot_deferrals[T1_DEFER_BYTES], (unsigned long long)s.regions,
        (unsigned long long)s.region_capacity);
    /* The host windows of the attached vCPUs: with guest RAM the windows
     * cannot alias, Tier1 still runs through unbacked site exits.
     * tests/hosted/tier1stats.py fails a product run on those exits. */
    /* Checked lockstep: with engine=lockstep tier1=on,
     * checked must equal entries and unchecked be 0 (tier1stats.py). */
    LOGI("tier1 lockstep checked=%llu checked_retired=%llu unchecked=%llu unchecked_retired=%llu "
         "divergences=%llu captures=%llu undo_pre_traps=%llu",
         (unsigned long long)s.checked, (unsigned long long)s.checked_retired,
         (unsigned long long)s.unchecked, (unsigned long long)s.unchecked_retired,
         (unsigned long long)s.divergences, (unsigned long long)s.check_captures,
         (unsigned long long)s.undo_pre_traps);
    struct t1_window_totals w = {0};
    if (g && g->windows)
        g->windows(&w);
    LOGI("hostwin instances=%llu fills=%llu fills_ro=%llu live=%llu unbacked=%llu not_ram=%llu "
         "backend_failures=%llu",
         (unsigned long long)w.instances, (unsigned long long)w.fills,
         (unsigned long long)w.fills_ro, (unsigned long long)w.live, (unsigned long long)w.unbacked,
         (unsigned long long)w.not_ram, (unsigned long long)w.backend_failures);
}

/* include/ia64vmm/engine.h: Tier0's lookup-miss
 * translations plus Tier1 compilation inside engine_run. Background
 * compilation has separate accounting and is not part of executor time. Each part is timed only behind its statistics gate;
 * the total is reported only when every engine that translated in this run
 * was timed: TIER0_STATS (Tier0 translates under every engine but interp,
 * and without it nothing is timed), and TIER1_STATS once Tier1 compiled. */
bool engine_compile_ticks(uint64_t *ticks)
{
    const struct t1_engine_glue *g = t1 ? t1 : engine_tier1_glue();
    struct t1_stats s = {0};
    if (g)
        g->stats(&s);
    if (!IA64VMM_TIER0_STATS || ((s.compiles || s.rejects) && !IA64VMM_TIER1_STATS))
        return false;
    struct tier0_stats t0;
    tier0_get_stats(&t0);
    uint64_t t1_ticks =
        g && g->executor_compile_ticks ? g->executor_compile_ticks() : s.compile_ticks;
    *ticks = t0.translate_ticks + t1_ticks;
    return true;
}

static void dump_stats(const struct vcpu *v)
{
    if (v->cfg.engine == ENGINE_INTERP)
        return;
    tier0_dump_stats();
    engine_dump_stats();
}

struct vcpu_exit engine_run_ia64(struct vcpu *v, uint64_t budget)
{
    code_quiescent(v);
    struct vcpu_exit out;
    /* ENGINE_TIER1 is Tier0 with the upper tier registered. */
    if (v->cfg.engine == ENGINE_INTERP)
        out = interp_run(v, budget);
    else
        out = tier0_run(v, budget);
    /* A code-validity stay lasts at most one call. */
    v->code_entry_generation = 0;
    code_quiescent(v);
    if (out.reason == VCPU_EXIT_UNIMPLEMENTED || out.reason == VCPU_EXIT_BAD_ACCESS) {
        vcpu_dump_isa(v);
        dump_stats(v);
    } else if (out.reason != VCPU_EXIT_BUDGET) {
        ia64_trace_dump(v);
        if (out.reason != VCPU_EXIT_HALT)
            dump_stats(v);
    }
    return out;
}

struct vcpu_exit engine_run(struct vcpu *v, uint64_t budget)
{
    if (!(v->s.psr & IA64_PSR_IS) || !engine_ia32_glue())
        return engine_run_ia64(v, budget);
    uint64_t now = hal_counter(), ticks = hal_counter_hz() / 1000;
    if (!ticks)
        ticks = 1;
    struct vcpu_run_budget b = {
        .ia64_instructions = budget,
        .native = {.deadline = now < UINT64_MAX - ticks ? now + ticks : UINT64_MAX - 1}};
    struct vcpu_run_exit e = engine_run_bounded(v, &b);
    return (struct vcpu_exit){.reason = e.stop == VCPU_RUN_YIELD ? VCPU_EXIT_BUDGET : e.reason,
                              .ip = e.position.ip,
                              .slot = e.position.slot,
                              .info = e.info,
                              .detail = e.detail};
}
