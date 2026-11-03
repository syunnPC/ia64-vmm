/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Suite "chain" of build/tier1-test, using tier0+tier1 verification regions.
 * Entry and exit hooks distinguish Tier0-linked and chained entries.
 * Tests cover kicks at chain boundaries, exact icount timer delivery,
 * wall-clock timers and slice limits, fetch remapping, successor NaT guards,
 * CPU/DMA code changes, and epoch reclamation at chain points.
 *
 * Reclamation must leave only the predecessor's link and point in use after
 * its quiescent publication; resolution must not return a retired successor.
 * Runs compare with the stepping reference; wall-clock cases use guest results.
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/engine_tier0t1.h"
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/engines.h"
#include "tests/tier1/programs.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/results.h"
#include "tests/tier1/schedule.h"
#include "tests/tier1/snapshot.h"
#include "engine/tier1/region.h"
#include "engine/tier1/tier1.h"
#include "engine/tier0/tier0.h"
#include "engine/cache/cache.h"
#include "ia64/interrupt.h"
#include "ia64/tlb.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/interrupts.h"

#define BIG_BUDGET UINT64_C(50000000)
#define REGIONS 64u
/* region_chain.S */
#define CHAIN_PASS (T1H_CODE + 0x20)
#define CHAIN_A (T1H_CODE + 0x30)
#define CHAIN_B (T1H_CODE + 0x1000)
#define CHAIN_END (T1H_CODE + 0x1060)
#define CHAIN_ALT (T1H_CODE + 0x1070)
#define CHAIN_PASSES 40u
#define CHAIN_EMPTY UINT64_C(0x106000) /* a page the image leaves empty */

static struct {
    struct vcpu *ref, *v;
    struct t1h_logs ref_logs, logs;
    struct snapshot snap;
    struct run_result reference, actual;
} M;

static struct t1_stats stats_now(void)
{
    struct t1_stats s;
    t1_get_stats(&s);
    return s;
}
#define DELTA(a, b, f) ((b).f - (a).f)

static void open_vcpus(bool icount)
{
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = icount};
    M.ref = vcpu_create(&cfg);
    M.v = vcpu_create(&cfg);
    T1H_CHECK(M.ref && M.v);
    t1h_bind(M.ref, &M.ref_logs);
    t1h_bind(M.v, &M.logs);
    M.ref_logs.stores_on = M.logs.stores_on = false;
}

static void close_vcpus(void)
{
    t1h_unbind(M.ref);
    t1h_unbind(M.v);
    vcpu_destroy(M.ref);
    vcpu_destroy(M.v);
    M.ref = M.v = NULL;
}

/* gather-phys, `passes` passes with the output restarting each pass. */
static const struct guest_program *open_gather(unsigned passes, bool icount, struct guest_params *q)
{
    const struct guest_program *p = guest_program("gather_phys");
    *q = guest_completion_params(p);
    q->passes = passes;
    q->bench = true;
    open_vcpus(icount);
    guest_setup(p, q, M.ref);
    snapshot_take(&M.snap, M.ref);
    return p;
}

/* region_chain.S at its start: A runs a_iterations, B b_iterations per pass;
 * A stores r3 to [r2] in the pass where r9 reaches store_pass (0: never). */
static void open_chain(bool virt, uint64_t store_pass, uint64_t r2, uint64_t r3)
{
    open_vcpus(true);
    t1h_load_image("region_chain.bin");
    struct ia64_entry_regs regs = {.ip = T1H_CODE,
                                   .psr = IA64_PSR_IC | IA64_PSR_BN | (virt ? IA64_PSR_IT : 0),
                                   .bsp = T1H_BSP,
                                   .bspstore = T1H_BSP};
    regs.gr[2] = r2;
    regs.gr[3] = r3;
    regs.gr[8] = 5;
    regs.gr[9] = CHAIN_PASSES;
    regs.gr[10] = store_pass;
    regs.gr[11] = 6;
    regs.gr[20] = 1;
    vcpu_reset(M.ref, &regs);
    M.ref->s.cr[CR_IVA] = T1H_IVT;
    snapshot_take(&M.snap, M.ref);
}

/* ---- observation -------------------------------------------------------- */

struct watch {
    void (*entry)(struct t1_vcpu *tv, uint32_t region);
    void (*exit)(struct t1_vcpu *tv, uint32_t region, bool exit_point);
    uint32_t last_exit;        /* region of the last exit */
    bool chained_out[REGIONS]; /* left through a chain point before */
    uint64_t exits, follows;   /* EXIT exits; those followed at once */
    uint64_t exit_retired, exit_ip;
    unsigned exit_slot;
    bool exit_valid;
};
static struct watch W;

static void watch_entry(struct t1_vcpu *tv, uint32_t region, void *arg)
{
    (void)arg;
    const struct vcpu *v = tv->v;
    if (tv->chained && W.last_exit < REGIONS)
        W.chained_out[W.last_exit] = true;
    if (W.exit_valid && W.exit_retired == v->retired && W.exit_ip == v->s.ip &&
        W.exit_slot == v->s.slot)
        ++W.follows;
    W.exit_valid = false;
    if (W.entry)
        W.entry(tv, region);
}

static void watch_exit(struct t1_vcpu *tv, const struct vcpu *v, const struct t1_restore_record *r,
                       void *arg)
{
    (void)arg;
    if (!r->materialized)
        return;
    const struct t1_region *region = t1_region_get(r->region_id);
    bool exit_point = region && region->point[r->point_id].kind == T1_PT_EXIT;
    W.last_exit = r->region_id;
    W.exit_valid = exit_point;
    W.exit_retired = v->retired;
    W.exit_ip = v->s.ip;
    W.exit_slot = v->s.slot;
    W.exits += exit_point;
    if (W.exit)
        W.exit(tv, r->region_id, exit_point);
}

static bool chained_out(uint32_t region)
{
    return region < REGIONS && W.chained_out[region];
}

/* The engine bound to M.v at M.snap with cold caches, verification regions
 * and the watch installed. */
static void engine_open(struct t1h_engine_vcpu *ev)
{
    t1h_engine_bind(ev, t1h_engine_tier0t1(), M.v);
    tier0_flush();
    t1_flush();
    snapshot_restore(&M.snap, M.v);
    t1h_engine_restored(ev);
    struct t1_compile_options opt = {T1_COMPILE_VERIFY, 0, 0};
    t1_test_set_compile_options(t1h_tier1_vcpu(ev->ctx), &opt);
    t1_test_set_restore_observer(watch_exit, NULL);
    t1_test_set_entry_hook(watch_entry, NULL);
    memset(&W, 0, sizeof(W));
}

static void engine_close(struct t1h_engine_vcpu *ev)
{
    t1_test_set_restore_observer(NULL, NULL);
    t1_test_set_entry_hook(NULL, NULL);
    t1h_engine_release(ev);
}

static struct vcpu_exit run_reference(struct schedule *sched)
{
    snapshot_restore(&M.snap, M.ref);
    struct vcpu_exit e = ref_run(M.ref, BIG_BUDGET, sched);
    T1H_CHECK(e.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END);
    result_take(&M.reference, &e, M.ref);
    return e;
}

static void compare_or_fail(const struct vcpu_exit *e, const char *label)
{
    result_take(&M.actual, e, M.v);
    if (result_compare(&M.reference, &M.actual, false, label)) {
        fprintf(stderr, "chain: %s differs from the reference\n", label);
        T1H_CHECK(!"chained run differs from the reference");
    }
}

/* region_chain: the retired count, from M.snap, at A's header in pass n (from
 * 1), stepping the reference. */
static uint64_t pass_head(unsigned n)
{
    snapshot_restore(&M.snap, M.ref);
    for (unsigned seen = 0;;) {
        if (M.ref->s.ip == CHAIN_PASS && !M.ref->s.slot)
            ++seen;
        if (seen == n && M.ref->s.ip == CHAIN_A && !M.ref->s.slot)
            return M.ref->retired - M.snap.v.retired;
        T1H_CHECK(ref_run(M.ref, 1, NULL).reason == VCPU_EXIT_BUDGET);
    }
}

/* ---- kicks -------------------------------------------------------------- */

static struct {
    bool armed, exited, yielded, entered_after;
    uint32_t region;
    unsigned entries;
    uint64_t exit_retired, exit_ip, yield_retired, yield_ip;
} K;

static void kick_entry(struct t1_vcpu *tv, uint32_t region)
{
    ++K.entries;
    if (K.armed && !K.yielded)
        K.entered_after = true;
    const struct t1_region *r = t1_region_get(region);
    if (!K.armed && K.entries > 32 && r && !r->loop && chained_out(region)) {
        K.armed = true;
        K.region = region;
        vcpu_kick(tv->v);
    }
}

static void kick_exit(struct t1_vcpu *tv, uint32_t region, bool exit_point)
{
    (void)exit_point;
    if (!K.armed || K.exited || region != K.region)
        return;
    K.exited = true;
    K.exit_retired = tv->v->retired;
    K.exit_ip = tv->v->s.ip;
}

static void chain_kicks(void)
{
    struct guest_params q;
    open_gather(60, true, &q);
    run_reference(NULL);
    struct t1h_engine_vcpu ev;
    engine_open(&ev);
    memset(&K, 0, sizeof(K));
    W.entry = kick_entry;
    W.exit = kick_exit;
    struct t1_stats s0 = stats_now();
    unsigned yields = 0;
    struct vcpu_exit e;
    for (;;) {
        e = t1h_tier0t1_slice(ev.ctx, M.v, BIG_BUDGET);
        if (!t1h_kick_yield(&e))
            break;
        if (++yields == 1) {
            K.yielded = true;
            K.yield_retired = M.v->retired;
            K.yield_ip = M.v->s.ip;
        }
    }
    struct t1_stats s1 = stats_now();
    engine_close(&ev);
    compare_or_fail(&e, "chain kicks gather");
    if (!K.armed || !K.exited || yields != 1 || K.entered_after ||
        K.yield_retired != K.exit_retired || K.yield_ip != K.exit_ip)
        fprintf(stderr,
                "chain kicks: armed %d exited %d yields %u entered after %d exit %llu@%#llx yield "
                "%llu@%#llx\n",
                K.armed, K.exited, yields, K.entered_after, (unsigned long long)K.exit_retired,
                (unsigned long long)K.exit_ip, (unsigned long long)K.yield_retired,
                (unsigned long long)K.yield_ip);
    T1H_CHECK(K.armed && K.exited && yields == 1 && !K.entered_after);
    T1H_CHECK(K.yield_retired == K.exit_retired && K.yield_ip == K.exit_ip);
    T1H_CHECK(DELTA(s0, s1, chain_links) > 0 && DELTA(s0, s1, chain_returns[T1_CHAIN_PAIR]) == 1);
    printf(
        "chain: kicks: a kick at the entry of region %u (no checkpoint, chained before) ended the upper entry "
        "at its chain point: Tier0 yielded at its exit boundary (retired %llu), no region entered in between; "
        "%llu chain links; equal to the reference\n",
        K.region, (unsigned long long)K.exit_retired,
        (unsigned long long)DELTA(s0, s1, chain_links));
    close_vcpus();
}

/* ---- timers ------------------------------------------------------------- */

static struct {
    unsigned mode;
    bool armed, delivered;
    uint64_t retired;
} U;

/* SDM Vol.2 Table 5-8: IRR remains pending while masked by PSR.i, TPR
 * or an in-service vector; unmasking needs no second interrupt request. */
static void unmask_pending(struct vcpu *v, unsigned mode)
{
    uint64_t psr = v->s.psr, tpr = v->s.cr[CR_TPR];
    T1H_CHECK((psr & IA64_PSR_I) && !v->irq_kick);
    if (!mode)
        v->s.psr &= ~IA64_PSR_I;
    else if (mode == 1)
        v->s.cr[CR_TPR] |= UINT64_C(1) << 16;
    else
        v->in_service[3] |= UINT64_C(1) << 63;
    T1H_CHECK(vcpu_accept_interrupt(v, 0xef));
    T1H_CHECK(!ia64_external_pending(v) && !v->irq_kick);
    if (!mode)
        v->s.psr = psr;
    else if (mode == 1)
        v->s.cr[CR_TPR] = tpr;
    else
        ia64_eoi(v);
    T1H_CHECK(ia64_external_requested(v) && ia64_external_pending(v) && !v->irq_kick);
}

static void unmask_exit(struct t1_vcpu *tv, uint32_t region, bool exit_point)
{
    if (!U.armed && exit_point && W.exits > 32 && chained_out(region)) {
        U.armed = true;
        U.retired = tv->v->retired;
        unmask_pending(tv->v, U.mode);
    }
}

static void unmask_entry(struct t1_vcpu *tv, uint32_t region)
{
    (void)region;
    if (U.armed && !U.delivered) {
        T1H_CHECK(t1h_bank0(tv->v, 24) == 1 && t1h_bank0(tv->v, 26) == 0xef);
        U.delivered = true;
    }
}

static void chain_unmask(void)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        struct guest_params q;
        open_gather(40, true, &q);
        struct t1h_engine_vcpu ev;
        engine_open(&ev);
        memset(&U, 0, sizeof(U));
        U.mode = mode;
        W.entry = unmask_entry;
        W.exit = unmask_exit;
        struct t1_stats before = stats_now();
        struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
        struct t1_stats after = stats_now();
        engine_close(&ev);
        T1H_CHECK(U.armed && U.delivered &&
                  DELTA(before, after, chain_returns[T1_CHAIN_INTERRUPT]) == 1);
        snapshot_restore(&M.snap, M.ref);
        T1H_CHECK(ref_run(M.ref, U.retired - M.ref->retired, NULL).reason == VCPU_EXIT_BUDGET);
        unmask_pending(M.ref, mode);
        struct vcpu_exit ref = ref_run(M.ref, BIG_BUDGET, NULL);
        result_take(&M.reference, &ref, M.ref);
        compare_or_fail(&e, "pending IRR unmasked without a kick");
        close_vcpus();
    }
    printf(
        "chain: pending IRR: PSR.i, TPR and EOI unmask without a new kick; delivery before successor entry, equal to reference\n");
}

static void chain_timer_icount(void)
{
    struct guest_params q;
    open_gather(40, true, &q);
    struct snapshot base;
    snapshot_init(&base);
    snapshot_take(&base, M.ref);
    run_reference(NULL);
    uint64_t total = M.ref->retired - M.snap.v.retired;
    const uint64_t at[] = {total / 3, total / 2 + 7, 2 * total / 3 + 13};
    uint64_t links = 0, chainable = 0;
    for (unsigned i = 0; i < 3; ++i) {
        M.snap.v = base.v;
        M.snap.v.s.cr[CR_ITV] = 0xef;
        M.snap.v.s.cr[CR_ITM] = ia64_itc_read(&M.snap.v) + at[i];
        ia64_timer_program(&M.snap.v);
        run_reference(NULL);
        T1H_CHECK(t1h_bank0(M.ref, 24) == 1 && t1h_bank0(M.ref, 26) == 0xef &&
                  t1h_bank0(M.ref, 25) == M.snap.v.s.cr[CR_ITM]);
        struct t1h_engine_vcpu ev;
        engine_open(&ev);
        struct t1_stats s0 = stats_now();
        struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
        struct t1_stats s1 = stats_now();
        engine_close(&ev);
        char label[64];
        snprintf(label, sizeof(label), "chain ITM (icount) after %llu", (unsigned long long)at[i]);
        compare_or_fail(&e, label);
        T1H_CHECK(W.follows == DELTA(s0, s1, chainable) &&
                  W.exits == DELTA(s0, s1, exits_by_kind[T1_PT_EXIT]));
        T1H_CHECK(DELTA(s0, s1, chain_links) > 0);
        links += DELTA(s0, s1, chain_links);
        chainable += DELTA(s0, s1, chainable);
    }
    M.snap.v = base.v;
    snapshot_free(&base);
    printf(
        "chain: timers (icount): ITM matches at 3 retired counts inside chains (%llu links) delivered at the "
        "match, equal to the reference; chainable %llu equal to the observed exits followed at once\n",
        (unsigned long long)links, (unsigned long long)chainable);
    close_vcpus();
}

/* upper_start: v->retired at the first region of the current upper entry;
 * upper_max: the most instructions an upper entry retired. */
static struct {
    bool armed, early;
    unsigned after;
    uint64_t upper_start, upper_max;
} T;

static void timer_entry(struct t1_vcpu *tv, uint32_t region)
{
    struct vcpu *v = tv->v;
    if (!tv->chained)
        T.upper_start = v->retired;
    if (T.armed) {
        if (!T.after++ && t1h_bank0(v, 24) != 1)
            T.early = true;
        return;
    }
    const struct t1_region *r = t1_region_get(region);
    if (!r || r->loop || !chained_out(region) || W.exits < 32)
        return;
    /* An ITM equal to the ITC now: the next poll raises the interrupt. */
    T.armed = true;
    v->s.cr[CR_ITV] = 0xef;
    ia64_timer_program(v);
    v->s.cr[CR_ITM] = v->timer_last;
}

static void timer_exit(struct t1_vcpu *tv, uint32_t region, bool exit_point)
{
    (void)region;
    (void)exit_point;
    uint64_t retired = tv->v->retired - T.upper_start;
    if (retired > T.upper_max)
        T.upper_max = retired;
}

/* A slice shorter than the longest upper entries gather-phys makes with the
 * default one, so that it bounds them. */
#define WALLCLOCK_SLICE 256u

static void chain_timer_wallclock(void)
{
    struct guest_params q;
    const struct guest_program *p = open_gather(40, false, &q);
    struct t1h_engine_vcpu ev;
    engine_open(&ev);
    uint64_t slice = t1_cfg.wallclock_slice;
    t1_cfg.wallclock_slice = WALLCLOCK_SLICE;
    memset(&T, 0, sizeof(T));
    W.entry = timer_entry;
    W.exit = timer_exit;
    struct t1_stats s0 = stats_now();
    struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
    struct t1_stats s1 = stats_now();
    t1_cfg.wallclock_slice = slice;
    engine_close(&ev);
    char why[256];
    if (!guest_check(p, &q, M.v, &e, &M.logs, why, sizeof(why))) {
        fprintf(stderr, "chain timers (wall-clock): %s\n", why);
        T1H_CHECK(!"program results");
    }
    if (!T.armed || T.early || !T.after || t1h_bank0(M.v, 24) != 1)
        fprintf(stderr,
                "chain timers (wall-clock): armed %d early %d entries after %u interrupts %llu\n",
                T.armed, T.early, T.after, (unsigned long long)t1h_bank0(M.v, 24));
    T1H_CHECK(T.armed && !T.early && T.after && t1h_bank0(M.v, 24) == 1 &&
              t1h_bank0(M.v, 26) == 0xef);
    T1H_CHECK(DELTA(s0, s1, chain_returns[T1_CHAIN_INTERRUPT]) == 1);
    /* T <= tier1_slice per upper entry. */
    if (T.upper_max > WALLCLOCK_SLICE)
        fprintf(stderr, "chain timers (wall-clock): an upper entry retired %llu (tier1_slice %u)\n",
                (unsigned long long)T.upper_max, WALLCLOCK_SLICE);
    T1H_CHECK(T.upper_max <= WALLCLOCK_SLICE);
    printf(
        "chain: timers (wall-clock): an ITM match made at the entry of a chained region was delivered at its "
        "chain point, before any other region ran; no upper entry retired more than %llu instructions "
        "(tier1_slice %u); program results correct\n",
        (unsigned long long)T.upper_max, WALLCLOCK_SLICE);
    close_vcpus();
}

/* ---- fetch -------------------------------------------------------------- */

static uint64_t code_pte(gpa_t pa)
{
    return pa | 1 | 0x20 | 0x40 | (UINT64_C(3) << 9);
}

static void chain_fetch(void)
{
    open_chain(true, 0, 0, 0);
    struct vcpu *v = M.ref;
    /* Page 0 and the IVT by ITRs, B's page by a TC entry. */
    uint64_t rid = (v->s.rr[0] >> 8) & 0xffffff;
    v->s.tr[0][0] = (struct ia64_tlb_entry){.va = T1H_CODE,
                                            .pte = code_pte(T1H_CODE),
                                            .itir = 12u << 2,
                                            .rid = rid,
                                            .page_shift = 12,
                                            .valid = true};
    v->s.tr[0][1] = (struct ia64_tlb_entry){.va = T1H_IVT,
                                            .pte = code_pte(T1H_IVT),
                                            .itir = 15u << 2,
                                            .rid = rid,
                                            .page_shift = 15,
                                            .valid = true};
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 0);
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 1);
    ia64_tc_insert(v, CHAIN_B, 0, code_pte(CHAIN_B), 12u << 2);
    /* The empty page holds the end marker. */
    uint8_t *ram = t1h_ram();
    for (unsigned k = 0; k < 4096; k += 16)
        memcpy(ram + (CHAIN_EMPTY - T1H_RAM_BASE) + k, ram + (CHAIN_END - T1H_RAM_BASE), 16);
    code_notify_vmm_write(CHAIN_EMPTY, 4096);
    snapshot_take(&M.snap, v);
    /* A's entry in pass 20: B chained after A many times before. */
    uint64_t split = pass_head(20);
    run_reference(NULL);
    snapshot_restore(&M.snap, M.ref);
    T1H_CHECK(ref_run(M.ref, split, NULL).reason == VCPU_EXIT_BUDGET);
    ia64_tc_insert(M.ref, CHAIN_B, 0, code_pte(CHAIN_EMPTY), 12u << 2);
    struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END &&
              M.ref->s.ip == CHAIN_B);
    result_take(&M.reference, &re, M.ref);
    struct t1h_engine_vcpu ev;
    engine_open(&ev);
    struct t1_stats s0 = stats_now();
    struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, split);
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && M.v->s.ip == CHAIN_A);
    struct t1_stats s1 = stats_now();
    ia64_tc_insert(M.v, CHAIN_B, 0, code_pte(CHAIN_EMPTY), 12u << 2);
    e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
    struct t1_stats s2 = stats_now();
    engine_close(&ev);
    compare_or_fail(&e, "chain fetch: B's page remapped");
    T1H_CHECK(DELTA(s0, s1, chain_links) >= 10 &&
              DELTA(s1, s2, chain_returns[T1_CHAIN_FETCH]) == 1);
    printf(
        "chain: fetch (virtual): B's entry page remapped at A's entry in pass 20 after %llu chain links: A's "
        "chain point did not enter B, Tier0 stopped at the remapped page as the reference does\n",
        (unsigned long long)DELTA(s0, s1, chain_links));
    close_vcpus();
}

/* ---- NaT guard ---------------------------------------------------------- */

static void chain_nat(void)
{
    open_chain(false, 0, 0, 0);
    uint64_t split = pass_head(20);
    snapshot_restore(&M.snap, M.ref);
    T1H_CHECK(ref_run(M.ref, split, NULL).reason == VCPU_EXIT_BUDGET);
    M.ref->s.nat[20] = true;
    struct vcpu_exit re = ref_run(M.ref, BIG_BUDGET, NULL);
    T1H_CHECK(re.reason == VCPU_EXIT_DEBUG && M.ref_logs.last_break_imm == T1H_BREAK_END &&
              M.ref->s.nat[14]);
    result_take(&M.reference, &re, M.ref);
    struct t1h_engine_vcpu ev;
    engine_open(&ev);
    struct t1_stats s0 = stats_now();
    struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, split);
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && M.v->s.ip == CHAIN_A);
    struct t1_stats s1 = stats_now();
    M.v->s.nat[20] = true;
    e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
    struct t1_stats s2 = stats_now();
    engine_close(&ev);
    compare_or_fail(&e, "chain NaT: r20 NaT at A's entry");
    T1H_CHECK(DELTA(s0, s1, chain_links) >= 10 && DELTA(s1, s2, chain_returns[T1_CHAIN_NAT]) >= 1);
    T1H_CHECK(DELTA(s1, s2, nat_guard_fails) >= 1);
    printf(
        "chain: NaT: r20 (read by B only) NaT from A's entry in pass 20: %llu chain points left B's guard to "
        "Tier0, %llu Tier0-linked entries declined it; equal to the reference (r14 NaT)\n",
        (unsigned long long)DELTA(s1, s2, chain_returns[T1_CHAIN_NAT]),
        (unsigned long long)DELTA(s1, s2, nat_guard_fails));
    close_vcpus();
}

/* ---- coherence ---------------------------------------------------------- */

static uint64_t image_word(gpa_t pa)
{
    uint64_t w;
    T1H_CHECK(gm_read(pa, &w, 8));
    return w;
}

/* The region last entered at B's header before the change (its link), and
 * whether it was entered after it. */
static struct {
    uint64_t old;
    bool changed, stale, dma;
    uint64_t dma_retired;
    uint32_t a;
} C;

static void coherence_entry(struct t1_vcpu *tv, uint32_t region)
{
    if (tv->v->s.ip != CHAIN_B)
        return;
    uint64_t link = t1_region_link(t1_region_get(region));
    if (C.changed && link == C.old)
        C.stale = true;
    if (!C.changed)
        C.old = link;
}

static void coherence_exit(struct t1_vcpu *tv, uint32_t region, bool exit_point)
{
    struct vcpu *v = tv->v;
    if (!exit_point || v->s.ip != CHAIN_B || !C.dma || C.changed)
        return;
    /* A's exit to B in pass 20 (r9 counts down from CHAIN_PASSES). */
    if (v->s.gr[9] != CHAIN_PASSES - 19)
        return;
    C.a = region;
    C.changed = true;
    C.dma_retired = v->retired;
    uint64_t *p = gm_direct_ptr(CHAIN_B + 8, 8);
    T1H_CHECK(p);
    __atomic_store_n(p, image_word(CHAIN_ALT + 8), __ATOMIC_RELAXED);
    /* The vCPU observes the DMA's completion through a device access, which
     * ends its stay. */
    v->code_entry_generation = 0;
}

/* A's in-region M helper rewrites slot 2 of B's first bundle in pass 20. */
static void chain_coherence_helper(void)
{
    open_chain(false, CHAIN_PASSES - 19, CHAIN_B + 8, 0);
    uint64_t alt = image_word(CHAIN_ALT + 8);
    T1H_CHECK(alt != image_word(CHAIN_B + 8) && image_word(CHAIN_ALT) == image_word(CHAIN_B));
    M.snap.v.s.gr[3] = alt;
    snapshot_restore(&M.snap, M.ref);
    uint64_t head = pass_head(1);
    run_reference(NULL);
    struct t1h_engine_vcpu ev;
    engine_open(&ev);
    memset(&C, 0, sizeof(C));
    W.entry = coherence_entry;
    /* A with exact memory helpers, compiled at its header. */
    struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, head);
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && M.v->s.ip == CHAIN_A);
    struct t1_vcpu *tv = t1h_tier1_vcpu(ev.ctx);
    struct t1_compile_request rq;
    T1H_CHECK(t1_request_make(tv, CHAIN_A, true, &rq));
    rq.slow_memory = true;
    struct t1_compile_report rep;
    T1H_CHECK(t1_compile_hot(tv, &rq, &rep) == T1_COMPILED);
    const struct t1_region *a = t1_region_get(rep.region_id);
    T1H_CHECK(a && a->nhelper >= 1);
    uint64_t a_link = t1_region_link(a);
    struct t1_stats s0 = stats_now();
    /* To A's first entry in pass 20, then on. */
    uint64_t store_at = pass_head(20) - head;
    e = t1h_tier0t1_run(ev.ctx, M.v, store_at);
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && M.v->s.ip == CHAIN_A);
    struct t1_stats s1 = stats_now();
    uint64_t old = C.old;
    const struct t1_region *b = t1_region_from_link(old);
    T1H_CHECK(b && b->valid);
    uint32_t b_id = b->id;
    C.changed = true;
    e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
    struct t1_stats s2 = stats_now();
    engine_close(&ev);
    compare_or_fail(&e, "chain coherence: M helper store into B");
    T1H_CHECK(!C.stale && !t1_region_from_link(old) && t1_region_from_link(a_link));
    T1H_CHECK(DELTA(s0, s1, chain_links) >= 10 &&
              DELTA(s1, s2, chain_returns[T1_CHAIN_SUCCESSOR]) >= 1);
    printf(
        "chain: coherence: A's M helper stored into B's first bundle in pass 20 after %llu chain links: B "
        "(region %u) retired and never entered again, the new code ran; equal to the reference\n",
        (unsigned long long)DELTA(s0, s1, chain_links), b_id);
    close_vcpus();
}

/* The same change by DMA at A's exit to B, under compare-always; the DMA's
 * completion ends the stay. */
static void chain_coherence_dma(void)
{
    open_chain(false, 0, 0, 0);
    struct t1h_engine_vcpu ev;
    engine_open(&ev);
    memset(&C, 0, sizeof(C));
    C.dma = true;
    W.entry = coherence_entry;
    W.exit = coherence_exit;
    struct t1_stats s0 = stats_now();
    struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
    struct t1_stats s1 = stats_now();
    engine_close(&ev);
    T1H_CHECK(C.changed && C.a);
    uint64_t alt = image_word(CHAIN_ALT + 8);
    /* The reference from the image, with the same DMA at the same boundary. */
    struct schedule sched;
    schedule_clear(&sched);
    schedule_add(&sched, (struct injection){.at = C.dma_retired,
                                            .kind = INJ_DMA,
                                            .address = CHAIN_B + 8,
                                            .value = alt,
                                            .width = 8});
    schedule_add(&sched, (struct injection){.at = C.dma_retired, .kind = INJ_STAY_END});
    run_reference(&sched);
    T1H_CHECK(schedule_complete(&sched));
    compare_or_fail(&e, "chain coherence: DMA into B at A's exit");
    T1H_CHECK(!C.stale && DELTA(s0, s1, chain_returns[T1_CHAIN_VALIDATE]) >= 1);
    printf(
        "chain: coherence (compare-always): B's first bundle changed by DMA at A's exit in pass 20: B's "
        "validation at the chain point retired it, it was not entered, the new code ran; equal to the "
        "reference\n");
    close_vcpus();
}

/* ---- lifetime ----------------------------------------------------------- */

enum life_mode { LIFE_R, LIFE_S };
static struct {
    enum life_mode mode;
    bool armed, done, freed;
    uint64_t link, from;
    uint32_t id;
    const struct t1_region *record;
} L;

static void *reclaim_thread(void *arg)
{
    (void)arg;
    t1_reclaim();
    return NULL;
}

static void life_exit(struct t1_vcpu *tv, uint32_t region, bool exit_point)
{
    if (L.armed || !exit_point || !chained_out(region) || W.exits < 32)
        return;
    struct t1_region *r = t1_region_get(region);
    uint64_t from = t1_region_link(r);
    struct t1_region *target = r;
    if (L.mode == LIFE_S) {
        /* The successor this exit's chain point will resolve. */
        target = NULL;
        for (unsigned k = 0; k < T1_SUCCESSORS && !target; ++k)
            if (tv->successors[k].from == from && tv->successors[k].point == tv->x.point)
                target = t1_region_from_link(tv->successors[k].to);
        if (!target)
            return;
    }
    L.armed = true;
    L.link = t1_region_link(target);
    L.from = from;
    L.id = target->id;
    L.record = target;
    t1_region_retire(target);
}

static void life_hook(struct vcpu *v, uint64_t from, bool resolved)
{
    (void)v;
    if (!L.armed || L.done || from != L.from || resolved != (L.mode == LIFE_S))
        return;
    pthread_t thread;
    T1H_CHECK(!pthread_create(&thread, NULL, reclaim_thread, NULL));
    T1H_CHECK(!pthread_join(thread, NULL));
    L.done = true;
    L.freed = t1_region_get(L.id) != L.record;
}

static void chain_lifetime(enum life_mode mode)
{
    struct guest_params q;
    open_gather(40, true, &q);
    run_reference(NULL);
    struct t1h_engine_vcpu ev;
    engine_open(&ev);
    memset(&L, 0, sizeof(L));
    L.mode = mode;
    W.exit = life_exit;
    t1_test_chain_hook = life_hook;
    code_register_vcpu(M.v);
    struct t1_stats s0 = stats_now();
    struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
    struct t1_stats s1 = stats_now();
    code_unregister_vcpu(M.v);
    t1_test_chain_hook = NULL;
    engine_close(&ev);
    compare_or_fail(&e,
                    mode == LIFE_R ? "chain lifetime: R reclaimed" : "chain lifetime: S reclaimed");
    if (!L.armed || !L.done || !L.freed)
        fprintf(stderr, "chain lifetime %s: armed %d reclaimed %d freed %d\n",
                mode == LIFE_R ? "R" : "S", L.armed, L.done, L.freed);
    T1H_CHECK(L.armed && L.done && L.freed && !t1_region_from_link(L.link));
    T1H_CHECK(DELTA(s0, s1, chain_links) > 0);
    if (mode == LIFE_R)
        printf(
            "chain: lifetime: region %u, retired at its EXIT, was reclaimed by another thread right after its "
            "chain point's publication; the chain point went on (%llu chain links); equal to the reference\n",
            L.id, (unsigned long long)DELTA(s0, s1, chain_links));
    else
        printf(
            "chain: lifetime: successor region %u, retired at its predecessor's EXIT, was reclaimed by another "
            "thread right after the chain point resolved its link, which named no region; equal to the "
            "reference\n",
            L.id);
    close_vcpus();
}

static bool pmu_enabled;
static uint64_t pmu_start;
static void pmu_hook(struct vcpu *v, uint64_t from, bool resolved)
{
    (void)from;
    if (resolved || pmu_enabled)
        return;
    pmu_enabled = true;
    pmu_start = v->retired;
    v->s.pmc[4] |= 1;
    v->s.psr |= UINT64_C(1) << 2;
}

static void chain_pmu(void)
{
    struct guest_params q;
    const struct guest_program *p = open_gather(40, true, &q);
    struct t1h_engine_vcpu ev;
    engine_open(&ev);
    /* Ineligible while native code warms; enable at a chain boundary. */
    M.v->s.pmc[4] = (UINT64_C(2) << 24) | 0x800;
    pmu_enabled = false;
    t1_test_chain_hook = pmu_hook;
    struct t1_stats before = stats_now();
    struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
    struct t1_stats after = stats_now();
    t1_test_chain_hook = NULL;
    engine_close(&ev);
    T1H_CHECK(pmu_enabled && M.v->s.pmd[4] == M.v->retired - pmu_start && M.v->s.pmd[4]);
    T1H_CHECK(DELTA(before, after, chain_returns[T1_CHAIN_STATE]) == 1);
    char why[256];
    T1H_CHECK(guest_check(p, &q, M.v, &e, &M.logs, why, sizeof(why)));
    close_vcpus();
}

/* Production flags exercise the native edge, including stable stacked
 * LIVEINs. Peer requests and retirement can arrive without a C callback. */
static struct {
    struct t1_vcpu *tv;
    unsigned action;
    bool stop, fired;
} F;

static void *fast_peer(void *unused)
{
    (void)unused;
    while (__atomic_load_n(&F.tv->fast.completed, __ATOMIC_ACQUIRE) < 32 &&
           !__atomic_load_n(&F.stop, __ATOMIC_ACQUIRE))
        sched_yield();
    if (__atomic_load_n(&F.stop, __ATOMIC_ACQUIRE))
        return NULL;
    if (F.action == 1)
        vcpu_kick(F.tv->v);
    else if (F.action == 2)
        t1_flush();
    else
        T1H_CHECK(vcpu_accept_interrupt(F.tv->v, 0xef));
    F.fired = true;
    return NULL;
}

static void chain_native(void)
{
    struct t1_stats normal_before = {0}, normal_after = {0};
    for (unsigned action = 0; action < 6; ++action) {
        struct guest_params q;
        const struct guest_program *p = open_gather(100, action != 5, &q);
        if (action == 5) {
            M.snap.v.s.cr[CR_ITV] = 0xef;
            M.snap.v.s.cr[CR_ITM] = ia64_itc_read(&M.snap.v) + M.snap.v.cfg.itc_hz * 3600;
            ia64_timer_program(&M.snap.v);
        }
        run_reference(NULL);
        struct t1h_engine_vcpu ev;
        bool old_check = t1h_tier1_set_hostwin_check(action == 4);
        engine_open(&ev);
        t1h_tier1_set_hostwin_check(old_check);
        t1_test_set_restore_observer(NULL, NULL);
        t1_test_set_entry_hook(NULL, NULL);
        struct t1_vcpu *tv = t1h_tier1_vcpu(ev.ctx);
        const struct t1_compile_options opt = {0};
        t1_test_set_compile_options(tv, &opt);
        code_register_vcpu(M.v);
        memset(&F, 0, sizeof(F));
        F.tv = tv;
        F.action = action;
        pthread_t peer;
        if (action && action < 4)
            T1H_CHECK(!pthread_create(&peer, NULL, fast_peer, NULL));
        struct t1_stats before = stats_now();
        struct vcpu_exit e = t1h_tier0t1_run(ev.ctx, M.v, BIG_BUDGET);
        struct t1_stats after = stats_now();
        __atomic_store_n(&F.stop, true, __ATOMIC_RELEASE);
        if (action && action < 4)
            T1H_CHECK(!pthread_join(peer, NULL) && F.fired);
        code_unregister_vcpu(M.v);
        if (action == 4) {
            T1H_CHECK(!DELTA(before, after, chain_native_links));
            T1H_CHECK(DELTA(before, after, entries) == DELTA(normal_before, normal_after, entries));
            T1H_CHECK(DELTA(before, after, tier1_retired) ==
                      DELTA(normal_before, normal_after, tier1_retired));
            T1H_CHECK(DELTA(before, after, chain_links) ==
                      DELTA(normal_before, normal_after, chain_links));
            for (unsigned k = 0; k < 4; ++k)
                T1H_CHECK(DELTA(before, after, exits_by_kind[k]) ==
                          DELTA(normal_before, normal_after, exits_by_kind[k]));
        } else
            T1H_CHECK(DELTA(before, after, chain_native_links) >= 32);
        if (!action) {
            normal_before = before;
            normal_after = after;
        }
        if (action == 3) {
            char why[256];
            T1H_CHECK(t1h_bank0(M.v, 24) == 1 && t1h_bank0(M.v, 26) == 0xef);
            T1H_CHECK(guest_check(p, &q, M.v, &e, &M.logs, why, sizeof(why)));
        } else
            compare_or_fail(&e, "production native edge");
        engine_close(&ev);
        close_vcpus();
    }
    printf(
        "chain: production native edges: counted hops, stacked LIVEINs, peer kick/retirement/IRR, armed host-clock timer, checked fallback and identical accounting, reference results\n");
}

#include "tests/tier1/native_mutation.inc"

void chain_tests(void);
void chain_tests(void)
{
    const char *chain = getenv("T1H_CHAIN");
    if (chain && !strcmp(chain, "off")) {
        printf("chain: not run with T1H_CHAIN=off\n");
        return;
    }
    snapshot_init(&M.snap);
    result_init(&M.reference);
    result_init(&M.actual);
    T1H_CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    chain_kicks();
    chain_unmask();
    chain_timer_icount();
    chain_timer_wallclock();
    chain_nat();
    chain_fetch();
    chain_coherence_helper();
    chain_coherence_dma();
    chain_lifetime(LIFE_R);
    chain_lifetime(LIFE_S);
    chain_pmu();
    chain_native();
    native_mutation_test();
    result_free(&M.reference);
    result_free(&M.actual);
    snapshot_free(&M.snap);
}
