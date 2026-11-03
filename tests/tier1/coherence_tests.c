/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Suite "coherence" of build/tier1-test: bundle-granular coherence on tier1
 * and tier0+tier1, in physical and virtual mode. The stepping reference
 * receives each change at the same boundary.
 *
 * Checks source masks, covered and uncovered CPU/VMM/DMA writes, page reuse,
 * compile-time races, steady-state stays and self-modifying stores. Detected
 * changes must retire exactly the regions covering the changed bundle.
 * Formation lookahead alone must not add a bundle to a region's mask.
 * Unannounced DMA is detected by compare-always, not by epoch-compare.
 * Tier1 entry must validate bundles absent from the enclosing Tier0 region.
 *
 * --suite=coh-bench measures Tier1 per-store cost; it is outside the default run.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tests/tier1/ia64_enc.h"
#include "tests/tier1/engine_tier0t1.h"
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/results.h"
#include "tests/tier1/snapshot.h"
#include "tests/tier1/storelog.h"
#include "engine/tier1/region.h"
#include "engine/tier0/internal.h"
#include "engine/cache/cache.h"
#include "ia64/decoder.h"

#define BIG_BUDGET UINT64_C(50000000)

/* coherence_matrix.S */
#define COH_DRIVER_A (T1H_CODE + 0x60) /* `br .La0`: the change boundary */
#define COH_T (T1H_CODE + 0xfe0)
#define COH_L (T1H_CODE + 0x1000)
#define COH_E0 COH_L
#define COH_U0 (COH_L + 0x800)
#define COH_B0 (COH_L + 0xe00)
#define COH_A0 (COH_L + 0xfd0)
#define COH_A1 (COH_L + 0xfe0)
#define COH_A2 (COH_L + 0xff0)
#define COH_A3 (COH_L + 0x1000)
#define COH_A5 (COH_L + 0x1020)
#define COH_PASSES 4u
#define COH_ITERS 200u
/* coherence_steady.S, coherence_stores.S */
#define SS_INNER (T1H_CODE + 0x40)
#define ST_LOOP (T1H_CODE + 0x20)
#define ST_DATA (T1H_CODE + 0x800)

static unsigned comparisons, cases;

/* ---- the machine ------------------------------------------------------- */

static struct vcpu *ref, *vt;
static struct t1h_logs ref_logs, logs;
static struct snapshot S;
static struct run_result R, R0, ACT;

static void map_code(struct vcpu *v)
{
    /* Code and IVT through ITR0 (VRN 0, a 64 KiB identity page), as
     * tag-keep maps them. */
    for (unsigned r = 0; r < 8; ++r)
        v->s.rr[r] = ((UINT64_C(0x200) + r) << 8) | (12u << 2);
    v->s.tr[0][0] = (struct ia64_tlb_entry){.va = T1H_CODE,
                                            .pte = T1H_CODE | 1 | 0x20 | 0x40 | (UINT64_C(3) << 9),
                                            .itir = 16u << 2,
                                            .rid = 0x200,
                                            .page_shift = 16,
                                            .valid = true};
}

static void machine_open(const char *image, const struct ia64_entry_regs *regs, bool virt)
{
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    ref = vcpu_create(&cfg);
    vt = vcpu_create(&cfg);
    T1H_CHECK(ref && vt);
    t1h_bind(ref, &ref_logs);
    t1h_bind(vt, &logs);
    ref_logs.stores_on = logs.stores_on = false;
    T1H_CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    t1_flush();
    t1h_load_image(image);
    vcpu_reset(ref, regs);
    ref->s.cr[CR_IVA] = T1H_IVT;
    if (virt) {
        map_code(ref);
        ref->s.psr |= IA64_PSR_IT;
    }
    snapshot_take(&S, ref);
}

static void machine_close(void)
{
    t1h_unbind(ref);
    t1h_unbind(vt);
    vcpu_destroy(ref);
    vcpu_destroy(vt);
    ref = vt = NULL;
}

static void compare_or_fail(const char *label)
{
    if (result_compare(&R, &ACT, false, label)) {
        fprintf(stderr, "coherence: %s differs from the reference\n", label);
        T1H_CHECK(!"coherence run differs from the reference");
    }
    ++comparisons;
}

static bool at_end(const struct vcpu_exit *e, const struct t1h_logs *l)
{
    return e->reason == VCPU_EXIT_DEBUG && l->last_break_imm == T1H_BREAK_END;
}

static struct t1_stats t1_now(void)
{
    struct t1_stats s;
    t1_get_stats(&s);
    return s;
}
static struct tier0_stats t0_now(void)
{
    struct tier0_stats s;
    tier0_get_stats(&s);
    return s;
}
static struct code_validity_stats cv_now(void)
{
    struct code_validity_stats s;
    code_validity_get_stats(&s);
    return s;
}
#define DELTA(a, b, f) ((b).f - (a).f)

/* The engines' compile flags: production regions on `tier1` and
 * `tier0+tier1`; `lockstep+tier1` (VER's checked lockstep)
 * compiles with the store and undo logs, and every execution of its
 * regions is checked against the interpreter. */
static bool is_checked(const struct t1h_engine *e)
{
    return e == t1h_engine_checked();
}
static uint32_t engine_flags(const struct t1h_engine *e)
{
    return is_checked(e) ? T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG : 0;
}
static void production_flags(const struct t1h_engine *e, bool on)
{
    uint32_t f = on ? 0 : T1_COMPILE_VERIFY;
    if (e == t1h_engine_tier0t1())
        t1h_tier0t1_set_flags(f);
    else if (e == t1h_engine_tier1())
        t1h_tier1_set_flags(f);
}
/* Under checked lockstep: every Tier1 execution in [s0, s1] checked, none
 * unchecked, no divergence. */
static void checked_clean(const struct t1h_engine *e, const struct t1_stats *s0,
                          const struct t1_stats *s1, const char *label)
{
    if (!is_checked(e))
        return;
    uint64_t entries = s1->entries - s0->entries, checked = s1->checked - s0->checked;
    if (s1->unchecked != s0->unchecked || s1->divergences != s0->divergences ||
        checked != entries) {
        fprintf(stderr,
                "coherence: %s: %llu entries, %llu checked, %llu unchecked, %llu divergences\n",
                label, (unsigned long long)entries, (unsigned long long)checked,
                (unsigned long long)(s1->unchecked - s0->unchecked),
                (unsigned long long)(s1->divergences - s0->divergences));
        T1H_CHECK(!"checked lockstep: an execution unchecked or divergent");
    }
}

static const char *mode_name(enum code_validity_mode m)
{
    return m == CODE_VALIDITY_EPOCH_COMPARE ? "epoch-compare" : "compare-always";
}

/* The reference from S, with `change` applied at `boundary` (retired
 * instructions after S; NULL: no change), to the end. */
typedef void (*change_fn)(void *arg);
static void reference_run(uint64_t boundary, change_fn change, void *arg, struct run_result *out)
{
    snapshot_restore(&S, ref);
    struct vcpu_exit e;
    if (change) {
        e = ref_run(ref, boundary, NULL);
        T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && ref->retired == S.v.retired + boundary);
        change(arg);
        ref->code_entry_generation = 0; /* a slice boundary (coh_case) */
    }
    e = ref_run(ref, BIG_BUDGET, NULL);
    T1H_CHECK(at_end(&e, &ref_logs));
    result_take(out, &e, ref);
}

/* Retired instructions from S to the n-th arrival of the reference at
 * (ip, slot 0). */
static uint64_t reference_arrival(uint64_t ip, unsigned n)
{
    snapshot_restore(&S, ref);
    for (unsigned seen = 0;;) {
        if (ref->s.ip == ip && ref->s.slot == 0 && ++seen == n)
            return ref->retired - S.v.retired;
        struct vcpu_exit e = ref_run(ref, 1, NULL);
        T1H_CHECK(e.reason == VCPU_EXIT_BUDGET);
    }
}

/* ---- regions ------------------------------------------------------------ */

static bool in_mask(const uint64_t mask[4], gpa_t gpa)
{
    unsigned g = (unsigned)(gpa & 4095) >> 4;
    return (mask[g / 64] >> (g % 64)) & 1;
}

/* Does r's dependency on gpa's page hold gpa's bundle? */
static bool region_holds(const struct t1_region *r, gpa_t gpa)
{
    for (unsigned p = 0; p < r->npages; ++p)
        if (r->page[p].pa == (gpa & ~UINT64_C(4095)) && in_mask(r->dep[p].mask, gpa))
            return true;
    return false;
}

static struct t1_region *live_region_at(uint64_t ip)
{
    for (uint32_t id = 1; id <= t1_cfg.max_regions; ++id) {
        struct t1_region *r = t1_region_get(id);
        if (r && r->valid && r->c.ip == ip)
            return r;
    }
    return NULL;
}

/* The bundles [first, last] of one page as a mask. */
static void bundles(uint64_t mask[4], gpa_t first, gpa_t last)
{
    memset(mask, 0, 4 * sizeof(*mask));
    for (gpa_t g = first; g <= last; g += 16) {
        unsigned k = (unsigned)(g & 4095) >> 4;
        mask[k / 64] |= UINT64_C(1) << (k % 64);
    }
}

static bool mask_equal(const uint64_t a[4], const uint64_t b[4])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* Snapshot of every live Tier1 region (by link) and, on the product path,
 * every valid Tier0 region of the given pages (by slot and serial), with
 * whether its mask holds one of the changed bundles. */
#define INVENTORY 512u
struct inventory {
    unsigned n1, n0;
    struct {
        uint64_t link, ip;
        bool holds;
    } t1[INVENTORY];
    struct {
        unsigned slot;
        uint64_t serial, ip;
        bool holds;
    } t0[INVENTORY];
};
static struct inventory inv;

static void inventory_take(const gpa_t *changed, unsigned nchanged)
{
    inv.n1 = inv.n0 = 0;
    for (uint32_t id = 1; id <= t1_cfg.max_regions; ++id) {
        const struct t1_region *r = t1_region_get(id);
        if (!r || !r->valid)
            continue;
        T1H_CHECK(inv.n1 < INVENTORY);
        bool holds = false;
        for (unsigned c = 0; c < nchanged; ++c)
            holds |= region_holds(r, changed[c]);
        inv.t1[inv.n1].link = t1_region_link(r);
        inv.t1[inv.n1].ip = r->c.ip;
        inv.t1[inv.n1++].holds = holds;
    }
    const struct tier0_arena *a = &tier0_arena;
    for (unsigned i = 0; a->regions && i < a->capacity; ++i) {
        const struct tier0_region *r = &a->regions[i];
        if (!r->valid)
            continue;
        bool holds = false, near = false;
        for (unsigned c = 0; c < nchanged; ++c) {
            gpa_t page = changed[c] & ~UINT64_C(4095);
            if ((r->key.gpa & ~UINT64_C(4095)) != page)
                continue;
            near = true;
            holds |= in_mask(r->dependency.mask, changed[c]);
        }
        if (!near)
            continue;
        T1H_CHECK(inv.n0 < INVENTORY);
        inv.t0[inv.n0].slot = i;
        inv.t0[inv.n0].serial = r->serial;
        inv.t0[inv.n0].ip = r->key.ip;
        inv.t0[inv.n0++].holds = holds;
    }
}

static bool t1_alive(unsigned k)
{
    const struct t1_region *r = t1_region_from_link(inv.t1[k].link);
    return r && r->valid;
}

static bool t0_alive(unsigned k)
{
    const struct tier0_region *r = &tier0_arena.regions[inv.t0[k].slot];
    return r->valid && r->serial == inv.t0[k].serial;
}

/* Exactly the regions holding a changed bundle are gone (detected), or
 * every region is alive (not detected). Returns the Tier1 count expected
 * to retire. */
static unsigned inventory_check(bool detected, const char *label)
{
    unsigned holding = 0;
    for (unsigned k = 0; k < inv.n1; ++k) {
        bool expect = detected && inv.t1[k].holds;
        holding += expect;
        if (t1_alive(k) == expect) {
            fprintf(stderr, "coherence: %s: Tier1 region at %#llx %s\n", label,
                    (unsigned long long)inv.t1[k].ip, expect ? "was not retired" : "was retired");
            T1H_CHECK(!"retirement differs from the masks");
        }
    }
    for (unsigned k = 0; k < inv.n0; ++k) {
        bool expect = detected && inv.t0[k].holds;
        if (t0_alive(k) == expect) {
            fprintf(stderr, "coherence: %s: Tier0 region at %#llx %s\n", label,
                    (unsigned long long)inv.t0[k].ip, expect ? "was not retired" : "was retired");
            T1H_CHECK(!"Tier0 retirement differs from the masks");
        }
    }
    return holding;
}

static bool inventory_has(uint64_t ip, bool holds)
{
    for (unsigned k = 0; k < inv.n1; ++k)
        if (inv.t1[k].ip == ip)
            return inv.t1[k].holds == holds;
    return false;
}

/* ---- coherence_matrix.S: the change matrix ----------------------------- */

struct coh_target {
    const char *name;
    gpa_t gpa;
    unsigned r1, r3; /* slot 1: add r1 = r1, r3 */
    char owner;      /* Tier1 region holding it: 'A', 'B', 0 none */
};
static const struct coh_target targets[] = {
    {"A0 (A's header bundle, entry page)", COH_A0, 14, 9, 'A'},
    {"A1 (A, entry page, not in Tier0's header region)", COH_A1, 14, 10, 'A'},
    {"A3 (A, non-entry page)", COH_A3, 15, 10, 'A'},
    {"B0 (B, page L)", COH_B0, 16, 9, 'B'},
    {"U0 (no Tier1 region, page L)", COH_U0, 17, 9, 0},
    {"E0 (no Tier1 region, read by T's formation, page L)", COH_E0, 24, 9, 0},
    {"A5 (no Tier1 region, page L + 1)", COH_A5, 18, 9, 0},
};
#define NTARGETS (sizeof(targets) / sizeof(targets[0]))
/* Every add of pages L and L + 1, for the page-reuse cases. */
static const struct coh_target reuse_adds[] = {
    {"E0", COH_E0, 24, 9, 0},    {"U0", COH_U0, 17, 9, 0},    {"B0", COH_B0, 16, 9, 'B'},
    {"A0", COH_A0, 14, 9, 'A'},  {"A1", COH_A1, 14, 10, 'A'}, {"A2", COH_A2, 15, 9, 'A'},
    {"A3", COH_A3, 15, 10, 'A'}, {"A5", COH_A5, 18, 9, 0},
};

/* The target's bundle with its add reading r11 (= 4) instead. The original
 * is re-encoded first and must equal the image's bytes. */
static void changed_bundle(const struct coh_target *t, uint64_t *low, uint64_t *high)
{
    const uint8_t *src = S.ram + (t->gpa - T1H_RAM_BASE);
    uint64_t lo, hi, l0, h0;
    memcpy(&lo, src, 8);
    memcpy(&hi, src + 8, 8);
    struct ienc_bundle b = {
        (uint8_t)(lo & 31),
        {ia64_bundle_slot(lo, hi, 0), ia64_bundle_slot(lo, hi, 1), ia64_bundle_slot(lo, hi, 2)}};
    T1H_CHECK(b.slot[1] == ienc_alu(0, IENC_ADD, t->r1, t->r1, t->r3));
    ienc_pack(&b, &l0, &h0);
    T1H_CHECK(l0 == lo && h0 == hi);
    b.slot[1] = ienc_alu(0, IENC_ADD, t->r1, t->r1, 11);
    ienc_pack(&b, low, high);
}

enum coh_change { CH_DMA, CH_POLL, CH_VMM, CH_CPU };
static const char *const change_names[] = {"DMA + storage epoch", "DMA without the epoch",
                                           "VMM write", "CPU write"};
#define MAX_CHANGED 8u
struct change {
    enum coh_change kind;
    unsigned n;
    gpa_t gpa[MAX_CHANGED];
    uint64_t low[MAX_CHANGED], high[MAX_CHANGED];
};

static void apply_change(void *arg)
{
    const struct change *c = arg;
    uint8_t *ram = t1h_ram();
    for (unsigned i = 0; i < c->n; ++i) {
        uint8_t *dst = ram + (c->gpa[i] - T1H_RAM_BASE);
        switch (c->kind) {
        case CH_DMA:
        case CH_POLL:
            /* Device DMA: backing RAM, no notification. */
            memcpy(dst, &c->low[i], 8);
            memcpy(dst + 8, &c->high[i], 8);
            break;
        case CH_VMM:
            /* gm_write's order: notify, then write. */
            code_notify_vmm_write(c->gpa[i], 16);
            memcpy(dst, &c->low[i], 8);
            memcpy(dst + 8, &c->high[i], 8);
            break;
        case CH_CPU:
            /* Two st8 as ia64/memory.c performs them: notify, then store. */
            code_notify_cpu_write(c->gpa[i], 8);
            memcpy(dst, &c->low[i], 8);
            code_notify_cpu_write(c->gpa[i] + 8, 8);
            memcpy(dst + 8, &c->high[i], 8);
            break;
        }
    }
    /* The storage device's completion; polling has none. */
    if (c->kind == CH_DMA)
        code_epoch_bump(CODE_DEVICE_STORAGE);
}

static struct ia64_entry_regs coh_regs(void)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[20] = COH_PASSES;
    regs.gr[21] = COH_ITERS - 1;
    return regs;
}

/* Explicit compile of T, then a run to `boundary`. With loop-only
 * hotness, check A, B and T's exact masks; tails can subsume their heads
 * and are checked through the inventory of every published region. */
static void coh_run_to(struct t1h_engine_vcpu *ev, uint64_t boundary, bool tails)
{
    struct t1_vcpu *tv = t1h_tier1_vcpu(ev->ctx);
    uint64_t ip = vt->s.ip;
    vt->s.ip = COH_T;
    struct t1_compile_options opt = {engine_flags(ev->e), 0, 0};
    struct t1_compile_report rep;
    T1H_CHECK(t1_compile(tv, &opt, &rep) == T1_COMPILED && rep.pages == 1);
    vt->s.ip = ip;
    struct vcpu_exit e = t1h_engine_run(ev, boundary);
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && vt->retired == S.v.retired + boundary);
    T1H_CHECK(vt->s.ip == COH_DRIVER_A && vt->s.slot == 0);
    if (tails)
        return;
    const struct t1_region *a = live_region_at(COH_A0), *b = live_region_at(COH_B0),
                           *t = live_region_at(COH_T);
    T1H_CHECK(a && b && t && a->entries && b->entries && t->entries);
    uint64_t m[4];
    T1H_CHECK(a->npages == 2 && a->page[0].pa == COH_L && a->page[1].pa == COH_L + 0x1000);
    bundles(m, COH_A0, COH_A2);
    T1H_CHECK(mask_equal(a->dep[0].mask, m));
    bundles(m, COH_A3, COH_A3 + 0x10);
    T1H_CHECK(mask_equal(a->dep[1].mask, m));
    bundles(m, COH_B0, COH_B0 + 0x10);
    T1H_CHECK(b->npages == 1 && b->page[0].pa == COH_L && mask_equal(b->dep[0].mask, m));
    bundles(m, COH_T, COH_T + 0x10);
    T1H_CHECK(t->npages == 1 && t->page[0].pa == T1H_CODE && mask_equal(t->dep[0].mask, m));
}

struct coh_totals {
    unsigned cases, retired_t1, retired_t0, stale, undetected;
};

/* One change at the boundary of the second pass (the vCPU on page 0),
 * reference first, then the engine: equal results, exactly the holding
 * regions retired, the new code executed (or, for polling completion
 * under epoch-compare, the old code everywhere). */
static void coh_case(const struct t1h_engine *eng, bool virt, enum code_validity_mode mode,
                     const char *what, const struct change *c, uint64_t boundary,
                     struct coh_totals *tot, struct t1_stats *delta, bool tails)
{
    char label[200];
    snprintf(label, sizeof(label), "%s %s %s%s, %s: %s", eng->name, virt ? "virtual" : "physical",
             mode_name(mode), tails ? " tails" : "", change_names[c->kind], what);
    bool detected = c->kind != CH_POLL || mode == CODE_VALIDITY_COMPARE_ALWAYS;
    T1H_CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
    t1_flush();
    reference_run(boundary, apply_change, (void *)c, &R);
    /* The changed code shows in the results exactly when it is detected. */
    bool differs = memcmp(R.v.s.gr, R0.v.s.gr, sizeof(R.v.s.gr)) != 0;
    T1H_CHECK(differs == detected);

    T1H_CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
    t1_flush();
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, eng, vt);
    bool product = eng != t1h_engine_tier1();
    production_flags(eng, true);
    snapshot_restore(&S, vt);
    t1h_engine_restored(&ev);
    coh_run_to(&ev, boundary, tails);
    inventory_take(c->gpa, c->n);
    gpa_t stay_page = vt->code_page;
    struct t1_stats s0 = t1_now();
    struct tier0_stats z0 = t0_now();
    apply_change((void *)c);
    /* The change point is a slice boundary: engine_run's return ends the
     * vCPU's stay there; the harness runs the engine
     * directly, so the test ends it. */
    vt->code_entry_generation = 0;
    /* CPU/VMM notifications retire synchronously; checking before entry
     * prevents entry validation from hiding a missed write notification. */
    if (c->kind == CH_CPU || c->kind == CH_VMM)
        inventory_check(true, label);
    struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
    T1H_CHECK(at_end(&e, &logs));
    struct t1_stats s1 = t1_now();
    struct tier0_stats z1 = t0_now();
    /* From the change to the end of the run only. */
    *delta = (struct t1_stats){.invalidations = DELTA(s0, s1, invalidations),
                               .decline_validate = DELTA(s0, s1, decline_validate),
                               .compiles = DELTA(s0, s1, compiles)};
    result_take(&ACT, &e, vt);
    compare_or_fail(label);
    checked_clean(eng, &s0, &s1, label);
    unsigned holding = inventory_check(detected, label);
    if (getenv("T1H_COH_TRACE"))
        fprintf(stderr,
                "case %s: %u Tier1 regions (%u holding), %u Tier0 regions, %llu invalidations\n",
                label, inv.n1, holding, inv.n0, (unsigned long long)DELTA(s0, s1, invalidations));
    T1H_CHECK(DELTA(s0, s1, invalidations) == holding);
    T1H_CHECK(stay_page == T1H_CODE); /* the vCPU executes page 0 */
    tot->retired_t1 += holding;
    for (unsigned k = 0; k < inv.n0; ++k)
        tot->retired_t0 += detected && inv.t0[k].holds;
    if (!product)
        T1H_CHECK(inv.n0 == 0 && DELTA(z0, z1, translations) == 0);
    if (!detected)
        ++tot->undetected;
    ++tot->cases;
    production_flags(eng, false);
    t1h_engine_release(&ev);
    ++cases;
}

static void coh_matrix(const struct t1h_engine *eng, bool virt, bool tails)
{
    struct ia64_entry_regs regs = coh_regs();
    machine_open("coherence_matrix.bin", &regs, virt);
    reference_run(0, NULL, NULL, &R0);
    T1H_CHECK(R0.v.s.gr[14] == COH_PASSES * COH_ITERS * 3 && R0.v.s.gr[19] == COH_PASSES * 3);
    uint64_t boundary = reference_arrival(COH_DRIVER_A, 2);
    for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS;
         mode <= CODE_VALIDITY_EPOCH_COMPARE; ++mode) {
        struct coh_totals tot = {0};
        for (unsigned t = 0; t < NTARGETS; ++t)
            for (enum coh_change k = CH_DMA; k <= CH_CPU; ++k) {
                /* Polling completion under epoch-compare runs the old code;
                 * checked lockstep's replay fetches the live bytes,
                 * so it reports that execution as a divergence. The
                 * combination is not run on lockstep+tier1. */
                if (is_checked(eng) && k == CH_POLL && mode == CODE_VALIDITY_EPOCH_COMPARE)
                    continue;
                struct change c = {.kind = k, .n = 1, .gpa = {targets[t].gpa}};
                changed_bundle(&targets[t], &c.low[0], &c.high[0]);
                struct t1_stats d;
                coh_case(eng, virt, mode, targets[t].name, &c, boundary, &tot, &d, tails);
                /* Ownership: which regions hold the bundle. */
                bool detected = k != CH_POLL || mode == CODE_VALIDITY_COMPARE_ALWAYS;
                if (!tails) {
                    T1H_CHECK(inventory_has(COH_A0, targets[t].owner == 'A'));
                    T1H_CHECK(inventory_has(COH_B0, targets[t].owner == 'B'));
                    T1H_CHECK(inventory_has(COH_T, false));
                    if (targets[t].owner && detected)
                        T1H_CHECK(d.invalidations >= 1);
                }
                if (!tails && !targets[t].owner && d.invalidations) {
                    for (unsigned z = 0; z < inv.n1; ++z)
                        if (inv.t1[z].holds) {
                            const struct t1_region *h = t1_region_from_link(inv.t1[z].link);
                            fprintf(stderr, "coherence: %s: Tier1 region at %#llx holds it",
                                    targets[t].name, (unsigned long long)inv.t1[z].ip);
                            if (h)
                                for (unsigned p = 0; p < h->npages; ++p)
                                    fprintf(stderr,
                                            " page %#llx mask %016llx %016llx %016llx %016llx",
                                            (unsigned long long)h->page[p].pa,
                                            (unsigned long long)h->dep[p].mask[3],
                                            (unsigned long long)h->dep[p].mask[2],
                                            (unsigned long long)h->dep[p].mask[1],
                                            (unsigned long long)h->dep[p].mask[0]);
                            fprintf(stderr, "\n");
                        }
                    T1H_CHECK(!"a Tier1 region holds a bundle that no region should hold");
                }
                /* Tails hold U0 and A5, which the loop regions leave out.
                 * CPU stores must retire those tails too. */
                if (tails && (targets[t].gpa == COH_U0 || targets[t].gpa == COH_A5)) {
                    bool tail_holds = false;
                    for (unsigned z = 0; z < inv.n1; ++z)
                        tail_holds |= inv.t1[z].holds && inv.t1[z].ip != COH_A0 &&
                                      inv.t1[z].ip != COH_B0 && inv.t1[z].ip != COH_T;
                    T1H_CHECK(tail_holds);
                }
                /* tier1-only-bundle: Tier0's header region does not hold A1; the
                 * Tier1 entry through it found the change. */
                if (eng != t1h_engine_tier1() && mode == CODE_VALIDITY_COMPARE_ALWAYS &&
                    k == CH_DMA && targets[t].gpa == COH_A1) {
                    T1H_CHECK(d.decline_validate >= 1);
                    bool header_alive = false;
                    for (unsigned z = 0; z < inv.n0; ++z)
                        if (inv.t0[z].ip == COH_A0)
                            header_alive = !inv.t0[z].holds && t0_alive(z);
                    T1H_CHECK(header_alive);
                    printf(
                        "coherence: tier1-only-bundle %s %s: DMA into A1 (Tier1 region A only) while the vCPU executed "
                        "page 0; entered through Tier0's header region (not retired): %llu entry declined by "
                        "validation, A retired, equal to the reference\n",
                        eng->name, virt ? "virtual" : "physical",
                        (unsigned long long)d.decline_validate);
                }
            }
        /* Page reuse: L and L + 1, then L + 1 alone, rewritten. */
        for (unsigned pages = 2; pages >= 1; --pages) {
            struct change c = {.kind = CH_DMA};
            for (unsigned i = 0; i < sizeof(reuse_adds) / sizeof(reuse_adds[0]); ++i) {
                if (pages == 1 && reuse_adds[i].gpa < COH_L + 0x1000)
                    continue;
                c.gpa[c.n] = reuse_adds[i].gpa;
                changed_bundle(&reuse_adds[i], &c.low[c.n], &c.high[c.n]);
                ++c.n;
            }
            struct t1_stats d;
            coh_case(eng, virt, mode, pages == 2 ? "page reuse, L and L + 1" : "page reuse, L + 1",
                     &c, boundary, &tot, &d, tails);
            if (!tails)
                T1H_CHECK(inventory_has(COH_A0, true) && inventory_has(COH_B0, pages == 2) &&
                          inventory_has(COH_T, false));
            else
                T1H_CHECK(d.invalidations > 0);
        }
        printf(
            "coherence: %s %s %s %s: %u changes (including page reuse), equal to the reference; %u Tier1 and %u "
            "Tier0 regions retired, each holding a changed bundle; %u undetected by design (polling completion)\n",
            eng->name, virt ? "virtual" : "physical", mode_name(mode), tails ? "tails" : "loops",
            tot.cases, tot.retired_t1, tot.retired_t0, tot.undetected);
    }
    T1H_CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    machine_close();
}

/* ---- race: DMA during a Tier1 compile (CODE_TEST_SEAL) ------------------- */

static struct {
    gpa_t page;         /* Tier1's SEAL check of this page ... */
    uint64_t compiling; /* ... while it publishes the region at this ip */
    bool fired, bump;
    struct change c;
    uint64_t at;    /* vt->retired at the DMA */
    unsigned seals; /* Tier1 SEAL checks of the page in that compile */
    bool counting;
    uint64_t compiled_ip; /* the compile the DMA raced */
} race;

/* The Tier1 region being published: placed in the arena (code set), not
 * linked (valid) and not retired: compile.c publish(), between
 * t1_arena_place and t1_region_publish. Tier0's chunk seal (arena.c) calls
 * the SEAL hook too; it never has such a region. */
static const struct t1_region *in_publication(void)
{
    for (uint32_t id = 1; id <= t1_cfg.max_regions; ++id) {
        const struct t1_region *r = t1_region_get(id);
        if (r && r->code && !r->valid && !r->retired)
            return r;
    }
    return NULL;
}

static void race_hook(gpa_t page, enum code_test_stage stage)
{
    if (stage != CODE_TEST_SEAL || page != race.page)
        return;
    const struct t1_region *r = in_publication();
    if (!r || r->c.ip != race.compiling)
        return;
    if (race.counting) {
        ++race.seals;
        return;
    }
    if (race.fired)
        return;
    race.fired = race.counting = true;
    race.seals = 1;
    race.at = vt->retired;
    struct change c = race.c;
    c.kind = CH_POLL;
    apply_change(&c);
    if (race.bump)
        code_epoch_bump(CODE_DEVICE_STORAGE);
}

static void race_compiled(const struct vcpu *v, enum t1_compile_status st,
                          const struct t1_compile_report *rep, void *arg)
{
    (void)v;
    (void)arg;
    const struct t1_region *r = st == T1_COMPILED ? t1_region_get(rep->region_id) : NULL;
    if (race.counting) {
        race.counting = false;
        race.compiled_ip = r ? r->c.ip : 0;
    }
}

static void race_apply(void *arg)
{
    struct change c = *(const struct change *)arg;
    c.kind = race.bump ? CH_DMA : CH_POLL;
    apply_change(&c);
}

/* The DMA hits `target` while the region at `compiling` is being published
 * (its SEAL check of `page`); retry: whether that publication failed. */
static void race_case(const struct t1h_engine *eng, enum code_validity_mode mode,
                      uint64_t compiling, gpa_t page, const struct coh_target *target, bool retry,
                      const char *what)
{
    char label[200];
    snprintf(label, sizeof(label), "%s %s race: %s", eng->name, mode_name(mode), what);
    T1H_CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
    t1_flush();
    memset(&race, 0, sizeof(race));
    race.page = page;
    race.compiling = compiling;
    race.bump = mode == CODE_VALIDITY_EPOCH_COMPARE;
    race.c.n = 1;
    race.c.gpa[0] = target->gpa;
    changed_bundle(target, &race.c.low[0], &race.c.high[0]);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, eng, vt);
    production_flags(eng, true);
    snapshot_restore(&S, vt);
    t1h_engine_restored(&ev);
    struct t1_stats s0 = t1_now();
    code_validity_set_test_hook(race_hook);
    t1_test_set_compile_observer(race_compiled, NULL);
    struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
    code_validity_set_test_hook(NULL);
    t1_test_set_compile_observer(NULL, NULL);
    struct t1_stats s1 = t1_now();
    T1H_CHECK(at_end(&e, &logs));
    result_take(&ACT, &e, vt);
    T1H_CHECK(race.fired && race.compiled_ip == compiling);
    if ((race.seals > 1) != retry) {
        fprintf(stderr, "coherence: %s: %u SEAL checks of the page in the raced compile\n", label,
                race.seals);
        T1H_CHECK(!"race: publication retry differs");
    }
    reference_run(race.at - S.v.retired, race_apply, &race.c, &R);
    T1H_CHECK(memcmp(R.v.s.gr, R0.v.s.gr, sizeof(R.v.s.gr)) != 0);
    checked_clean(eng, &s0, &s1, label);
    compare_or_fail(label);
    production_flags(eng, false);
    t1h_engine_release(&ev);
    printf(
        "coherence: %s: DMA at retired %llu, %s; %llu compiles, %llu invalidations; equal to the reference\n",
        label, (unsigned long long)(race.at - S.v.retired),
        retry ? "publication retried" : "no retry", (unsigned long long)DELTA(s0, s1, compiles),
        (unsigned long long)DELTA(s0, s1, invalidations));
    ++cases;
}

static void coh_races(const struct t1h_engine *eng)
{
    struct ia64_entry_regs regs = coh_regs();
    machine_open("coherence_matrix.bin", &regs, false);
    reference_run(0, NULL, NULL, &R0);
    const struct coh_target *a1 = &targets[1], *a3 = &targets[2], *u0 = &targets[4],
                            *a5 = &targets[6];
    for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS;
         mode <= CODE_VALIDITY_EPOCH_COMPARE; ++mode) {
        bool epoch = mode == CODE_VALIDITY_EPOCH_COMPARE;
        race_case(eng, mode, COH_A0, COH_L, a1, true, "A's publication, A1 (its entry page)");
        race_case(eng, mode, COH_A0, COH_L + 0x1000, a3, true,
                  "A's publication, A3 (its non-entry page)");
        /* A5 follows A's back-edge: A's formation reads it as lookahead,
         * so it is fresh but no source; epoch-compare
         * then retries as for A1 below. */
        race_case(eng, mode, COH_A0, COH_L + 0x1000, a5, epoch,
                  "A's publication, A5 (lookahead, no source)");
        race_case(eng, mode, COH_B0, COH_L, u0, false, "B's publication, U0 (not read yet)");
        /* A1 is fresh (A's source) but not B's. epoch-compare compares
         * every fresh bundle of the page at a new epoch, so the change
         * retires A and moves the page's generation: B's publication then
         * retries once (validity.c invalidate_mask). */
        race_case(eng, mode, COH_B0, COH_L, a1, epoch, "B's publication, A1 (A's bundle, not B's)");
    }
    T1H_CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    machine_close();
}

/* ---- stay: steady state on one page (coherence_steady.S) -------------------------- */

/* The translation event: ITR0 rewritten with the same PA and other
 * access rights (RX instead of RWX), through the MMU log. */
static void retranslate(void *arg)
{
    struct vcpu *v = arg;
    v->s.tr[0][0].pte = (v->s.tr[0][0].pte & ~(UINT64_C(7) << 9)) | (UINT64_C(1) << 9);
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 0);
}

static void stay_case(const struct t1h_engine *eng, bool virt)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[20] = 120;
    regs.gr[21] = 3;
    machine_open("coherence_steady.bin", &regs, virt);
    /* Warm up: 20 passes (compiles, first entries), then 80 measured. */
    uint64_t pass = reference_arrival(T1H_CODE + 0x10, 2) - reference_arrival(T1H_CODE + 0x10, 1);
    uint64_t warm = reference_arrival(T1H_CODE + 0x10, 21);
    if (virt)
        reference_run(warm + 80 * pass, retranslate, ref, &R);
    else
        reference_run(0, NULL, NULL, &R);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, eng, vt);
    production_flags(eng, true);
    snapshot_restore(&S, vt);
    t1h_engine_restored(&ev);
    struct vcpu_exit e = t1h_engine_run(&ev, warm);
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET);
    const struct t1_region *r = live_region_at(SS_INNER);
    T1H_CHECK(r && r->entries);
    uint64_t entries0 = r->entries;
    struct t1_stats s0 = t1_now();
    struct tier0_stats z0 = t0_now();
    struct code_validity_stats c0 = cv_now();
    e = t1h_engine_run(&ev, 80 * pass);
    struct code_validity_stats c1 = cv_now();
    struct tier0_stats z1 = t0_now();
    struct t1_stats s1 = t1_now();
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && live_region_at(SS_INNER) == r);
    uint64_t entries = r->entries - entries0;
    char label[96];
    snprintf(label, sizeof(label), "stay %s %s", eng->name, virt ? "virtual" : "physical");
    if (c1.comparisons != c0.comparisons ||
        z1.cross_page_validations != z0.cross_page_validations || entries != 80) {
        fprintf(stderr, "coherence: %s: %llu entries, %llu validator comparisons, %llu new stays\n",
                label, (unsigned long long)entries,
                (unsigned long long)(c1.comparisons - c0.comparisons),
                (unsigned long long)(z1.cross_page_validations - z0.cross_page_validations));
        T1H_CHECK(!"steady-state entries compared bundles or began stays");
    }
    T1H_CHECK(DELTA(s0, s1, decline_validate) == 0 && DELTA(s0, s1, compiles) == 0);
    checked_clean(eng, &s0, &s1, label);
    uint64_t compared = 0, stays = 0;
    if (virt) {
        /* A change of the page's fetch translation ends
         * the stay; the next region entry compares its bundles again. */
        retranslate(vt);
        c0 = cv_now();
        z0 = t0_now();
        entries0 = r->entries;
        e = t1h_engine_run(&ev, pass);
        c1 = cv_now();
        z1 = t0_now();
        T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && live_region_at(SS_INNER) == r &&
                  r->entries == entries0 + 1);
        compared = c1.comparisons - c0.comparisons;
        stays = z1.cross_page_validations - z0.cross_page_validations;
        if (!compared || !stays) {
            fprintf(
                stderr,
                "coherence: %s: after the fetch translation changed: %llu comparisons, %llu new stays\n",
                label, (unsigned long long)compared, (unsigned long long)stays);
            T1H_CHECK(!"a changed fetch translation did not make the page leave the stay");
        }
    }
    e = t1h_engine_run(&ev, BIG_BUDGET);
    T1H_CHECK(at_end(&e, &logs));
    result_take(&ACT, &e, vt);
    compare_or_fail(label);
    printf(
        "coherence: %s: 80 passes, 80 entries of the inner loop's region, 0 validator comparisons, 0 new "
        "stays (the Tier1 entry continues the stay of the lookup)",
        label);
    if (virt)
        printf(
            "; ITR0 rewritten (same PA, RX): the next pass began %llu stay(s) with %llu comparison(s)",
            (unsigned long long)stays, (unsigned long long)compared);
    printf("; equal to the reference\n");
    production_flags(eng, false);
    t1h_engine_release(&ev);
    machine_close();
    ++cases;
}

/* ---- stores to the code page (coherence_stores.S) -------------------------------- */

static struct ia64_entry_regs store_regs(uint64_t address, uint64_t value, uint64_t iterations)
{
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE, .psr = IA64_PSR_IC | IA64_PSR_BN, .bsp = T1H_BSP, .bspstore = T1H_BSP};
    regs.gr[2] = address;
    regs.gr[3] = value;
    regs.gr[8] = iterations - 1;
    return regs;
}

/* The loop bundle's high word with slot 2 = add r14 = r14, r10. */
static uint64_t smc_high_word(void)
{
    const uint8_t *src = S.ram + (ST_LOOP - T1H_RAM_BASE);
    uint64_t lo, hi, l1, h1;
    memcpy(&lo, src, 8);
    memcpy(&hi, src + 8, 8);
    struct ienc_bundle b = {
        (uint8_t)(lo & 31),
        {ia64_bundle_slot(lo, hi, 0), ia64_bundle_slot(lo, hi, 1), ia64_bundle_slot(lo, hi, 2)}};
    T1H_CHECK(b.slot[2] == ienc_alu(0, IENC_ADD, 14, 14, 9));
    b.slot[2] = ienc_alu(0, IENC_ADD, 14, 14, 10);
    ienc_pack(&b, &l1, &h1);
    T1H_CHECK(l1 == lo);
    return h1;
}

/* explicit: the loop's region compiled at its header before the run (no
 * demotion applies), else by hotness. */
static void store_case(const struct t1h_engine *eng, bool smc, bool explicit_region)
{
    const uint64_t n = 300;
    struct ia64_entry_regs regs = store_regs(ST_DATA, 0x5a5a, n);
    machine_open("coherence_stores.bin", &regs, false);
    if (smc) {
        ref->s.gr[2] = ST_LOOP + 8;
        ref->s.gr[3] = smc_high_word();
        snapshot_take(&S, ref);
    }
    reference_run(0, NULL, NULL, &R);
    T1H_CHECK(R.v.s.gr[14] == (smc ? 2 * n : n));
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, eng, vt);
    production_flags(eng, true);
    snapshot_restore(&S, vt);
    t1h_engine_restored(&ev);
    /* To the loop header (the second bundle executed: 3 + 3 instructions
     * from _start, for every engine). */
    struct vcpu_exit e = t1h_engine_run(&ev, 6);
    T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && vt->s.ip == ST_LOOP);
    if (explicit_region) {
        struct t1_compile_options opt = {engine_flags(eng), 0, 0};
        struct t1_compile_report rep;
        T1H_CHECK(t1_compile(t1h_tier1_vcpu(ev.ctx), &opt, &rep) == T1_COMPILED);
    }
    struct t1_stats s0 = t1_now();
    struct tier0_stats z0 = t0_now();
    struct code_validity_stats c0 = cv_now();
    e = t1h_engine_run(&ev, BIG_BUDGET);
    struct t1_stats s1 = t1_now();
    struct tier0_stats z1 = t0_now();
    struct code_validity_stats c1 = cv_now();
    T1H_CHECK(at_end(&e, &logs));
    result_take(&ACT, &e, vt);
    char label[128];
    snprintf(label, sizeof(label), "stores %s %s %s", eng->name,
             smc ? "into its own bundle" : "outside its bundles",
             explicit_region ? "explicit" : "by hotness");
    checked_clean(eng, &s0, &s1, label);
    compare_or_fail(label);
    if (!smc) {
        /* Every store traps (the alias of the code page is read-only) and
         * leaves at its site; Tier0 (or the lower tier) stores it, and the
         * bundle filter retires nothing. */
        /* Tier0 counts every retirement but evictions: under lockstep the
         * end marker (break.i with the harness's break hook installed) is
         * refused by tier0_check_safe, and that empty translation is
         * abandoned and counted once (tier0.c translate). */
        uint64_t end_marker = is_checked(eng);
        if (DELTA(s0, s1, invalidations) || DELTA(z0, z1, invalidations) != end_marker ||
            c1.invalidations != c0.invalidations)
            fprintf(
                stderr,
                "coherence: %s: %llu Tier1, %llu Tier0, %llu validity invalidations, %llu mismatches, "
                "%llu Tier1 entries, %llu checked\n",
                label, (unsigned long long)DELTA(s0, s1, invalidations),
                (unsigned long long)DELTA(z0, z1, invalidations),
                (unsigned long long)(c1.invalidations - c0.invalidations),
                (unsigned long long)(c1.mismatches - c0.mismatches),
                (unsigned long long)DELTA(s0, s1, entries),
                (unsigned long long)DELTA(s0, s1, checked));
        T1H_CHECK(DELTA(s0, s1, invalidations) == 0 && DELTA(z0, z1, invalidations) == end_marker);
        T1H_CHECK(c1.invalidations == c0.invalidations);
        T1H_CHECK(DELTA(s0, s1, exits_by_kind[T1_PT_SITE]) >= 1 && DELTA(s0, s1, traps) >= 1);
        if (explicit_region) {
            T1H_CHECK(DELTA(s0, s1, compiles) == 0);
            T1H_CHECK(DELTA(s0, s1, exits_by_kind[T1_PT_SITE]) >= n - 2);
        }
    } else {
        /* Each store rewrites the region's own bundle: it is retired. */
        T1H_CHECK(DELTA(s0, s1, invalidations) >= 1 && c1.invalidations > c0.invalidations);
    }
    printf(
        "coherence: %s: %llu iterations, %llu Tier1 entries, %llu site exits, %llu traps, %llu compiles, %llu "
        "Tier1 and %llu Tier0 invalidations, %llu demotions, %llu hot() calls held; equal to the reference\n",
        label, (unsigned long long)n, (unsigned long long)DELTA(s0, s1, entries),
        (unsigned long long)DELTA(s0, s1, exits_by_kind[T1_PT_SITE]),
        (unsigned long long)DELTA(s0, s1, traps), (unsigned long long)DELTA(s0, s1, compiles),
        (unsigned long long)DELTA(s0, s1, invalidations),
        (unsigned long long)DELTA(z0, z1, invalidations),
        (unsigned long long)DELTA(s0, s1, demotions), (unsigned long long)DELTA(s0, s1, hot_held));
    production_flags(eng, false);
    t1h_engine_release(&ev);
    machine_close();
    ++cases;
}

/* ---- holds after a site demotion (hot.c) ---------------------------------- */

/* coherence_stores.S by hotness on tier0+tier1 until its region is demoted for its
 * site exits (its stores to the code page): the head is held. A store to
 * bytes no region holds keeps the hold -- it belongs to the head's code,
 * not to the page's generation, which every such store moves -- and a
 * change of the head's bundle ends it. */
static void hold_case(bool change_head)
{
    struct ia64_entry_regs regs = store_regs(ST_DATA, 0x5a5a, 300);
    machine_open("coherence_stores.bin", &regs, false);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), vt);
    t1h_tier0t1_set_flags(0);
    snapshot_restore(&S, vt);
    t1h_engine_restored(&ev);
    uint64_t demoted = t1_now().demote_site;
    unsigned iterations = 0;
    while (t1_now().demote_site == demoted) {
        T1H_CHECK(t1h_engine_run(&ev, 6).reason == VCPU_EXIT_BUDGET && ++iterations < 300);
    }
    if (change_head) {
        uint64_t high = smc_high_word();
        code_notify_vmm_write(ST_LOOP + 8, 8);
        memcpy(t1h_ram() + (ST_LOOP + 8 - T1H_RAM_BASE), &high, 8);
        T1H_CHECK(!t1_hold_active(ST_LOOP, ST_LOOP, 0));
    } else {
        uint64_t x = 0x1234;
        code_notify_cpu_write(ST_DATA, 8);
        memcpy(t1h_ram() + (ST_DATA - T1H_RAM_BASE), &x, 8);
        T1H_CHECK(t1_hold_active(ST_LOOP, ST_LOOP, 0));
    }
    printf("coherence: hold after a site demotion (iteration %u): a %s %s the hold\n", iterations,
           change_head ? "VMM write changing the head's bundle"
                       : "CPU store to the code page outside every region",
           change_head ? "ends" : "keeps");
    t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
    machine_close();
    ++cases;
}

/* ---- LZ in the shared layout ---- */

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

#define CSUITE_STACK UINT64_C(0x1f0000)
#define CSUITE_LZ 5u

/* Stores of the reference to pages that held code at the time of the
 * store : count and byte range. */
struct lz_stores {
    uint64_t n, lo, hi;
};

static unsigned store_bytes(Ia64Opcode op)
{
    switch (op) {
    case IA64_OP_ST1:
    case IA64_OP_ST1REL:
        return 1;
    case IA64_OP_ST2:
    case IA64_OP_ST2REL:
        return 2;
    case IA64_OP_ST4:
    case IA64_OP_ST4REL:
        return 4;
    default:
        return 8;
    }
}

static void lz_open(const char *image, uint64_t iters)
{
    struct ia64_entry_regs regs = {.ip = T1H_CODE,
                                   .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN,
                                   .bsp = T1H_BSP,
                                   .bspstore = T1H_BSP};
    regs.gr[2] = CSUITE_LZ;
    regs.gr[3] = iters;
    regs.gr[12] = CSUITE_STACK;
    machine_open(image, &regs, false);
}

/* The stepping reference to the end, counting its stores to code pages
 * (physical mode: the address is the base register). */
static void lz_reference(struct lz_stores *st)
{
    snapshot_restore(&S, ref);
    *st = (struct lz_stores){0, UINT64_MAX, 0};
    struct vcpu_exit e;
    for (;;) {
        Ia64Instruction in;
        if (storelog_decode(ref, ref->s.ip, ref->s.slot, &in) &&
            (unsigned)in.opcode < IA64_OP_COUNT && t1_opcode_class(in.opcode) == T1_IC_STORE &&
            ((ref->s.pr >> ia64_pr_index(&ref->s, in.qp)) & 1)) {
            gpa_t a = ia64_gr_read(&ref->s, in.operands.memory.base) & ~(UINT64_C(1) << 63);
            if (code_page_has_code(a & ~UINT64_C(4095))) {
                ++st->n;
                if (a < st->lo)
                    st->lo = a;
                if (a + store_bytes(in.opcode) - 1 > st->hi)
                    st->hi = a + store_bytes(in.opcode) - 1;
            }
        }
        e = ref_run(ref, 1, NULL);
        if (e.reason != VCPU_EXIT_BUDGET)
            break;
    }
    T1H_CHECK(at_end(&e, &ref_logs) && ref->s.gr[8]);
    result_take(&R, &e, ref);
}

struct lz_run {
    uint64_t ns, retired;
    struct t1_stats t1;
    struct tier0_stats t0;
    struct code_validity_stats cv;
    struct code_cache_stats cc;
};

/* One cold run of `eng` from S (production regions, compare-always): the
 * deltas of every counter over the run, and its result equal to R. */
static void lz_engine(const struct t1h_engine *eng, struct lz_run *out, const char *label)
{
    T1H_CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    t1_flush();
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, eng, vt);
    bool product = eng == t1h_engine_tier0t1(), tier1 = eng == t1h_engine_tier1();
    if (product)
        t1h_tier0t1_set_flags(0);
    if (tier1)
        t1h_tier1_set_flags(0);
    snapshot_restore(&S, vt);
    t1h_engine_restored(&ev);
    struct t1_stats s0 = t1_now();
    struct tier0_stats z0 = t0_now();
    struct code_validity_stats c0 = cv_now();
    struct code_cache_stats d0;
    code_cache_get_stats(&d0);
    uint64_t t0 = now_ns();
    struct vcpu_exit e = t1h_engine_run(&ev, UINT64_C(1) << 40);
    out->ns = now_ns() - t0;
    struct t1_stats s1 = t1_now();
    struct tier0_stats z1 = t0_now();
    struct code_validity_stats c1 = cv_now();
    struct code_cache_stats d1;
    code_cache_get_stats(&d1);
    T1H_CHECK(at_end(&e, &logs));
    out->retired = vt->retired - S.v.retired;
    out->t1 = (struct t1_stats){
        .compiles = DELTA(s0, s1, compiles),
        .invalidations = DELTA(s0, s1, invalidations),
        .entries = DELTA(s0, s1, entries),
        .demotions = DELTA(s0, s1, demotions),
        .demote_site = DELTA(s0, s1, demote_site),
        .holds = DELTA(s0, s1, holds),
        .traps = DELTA(s0, s1, traps),
        .tier1_retired = DELTA(s0, s1, tier1_retired),
        .exits_by_kind = {[T1_PT_SITE] = DELTA(s0, s1, exits_by_kind[T1_PT_SITE])},
        .hot_held = DELTA(s0, s1, hot_held),
        .decline_validate = DELTA(s0, s1, decline_validate)};
    out->t0 = (struct tier0_stats){.translations = DELTA(z0, z1, translations),
                                   .invalidations = DELTA(z0, z1, invalidations)};
    out->cv = (struct code_validity_stats){.invalidations = c1.invalidations - c0.invalidations,
                                           .mismatches = c1.mismatches - c0.mismatches,
                                           .comparisons = c1.comparisons - c0.comparisons};
    out->cc =
        (struct code_cache_stats){.invalidated_pages = d1.invalidated_pages - d0.invalidated_pages,
                                  .decodes = d1.decodes - d0.decodes};
    result_take(&ACT, &e, vt);
    compare_or_fail(label);
    if (product)
        t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    if (tier1)
        t1h_tier1_set_flags(T1_COMPILE_VERIFY);
    t1h_engine_release(&ev);
}

static void lz_print(const char *layout, const char *engine, uint64_t iters, const struct lz_run *x)
{
    printf(
        "coh-lz layout %s engine %s iters %llu retired %llu ns %llu tier1_retired %llu entries %llu compiles %llu "
        "t1_invalidations %llu demotions %llu demote_site %llu holds %llu hot_held %llu site_exits %llu traps %llu "
        "decline_validate %llu tier0_translations %llu tier0_invalidations %llu validity_invalidations %llu "
        "mismatches %llu comparisons %llu decoded_invalidated_pages %llu decodes %llu\n",
        layout, engine, (unsigned long long)iters, (unsigned long long)x->retired,
        (unsigned long long)x->ns, (unsigned long long)x->t1.tier1_retired,
        (unsigned long long)x->t1.entries, (unsigned long long)x->t1.compiles,
        (unsigned long long)x->t1.invalidations, (unsigned long long)x->t1.demotions,
        (unsigned long long)x->t1.demote_site, (unsigned long long)x->t1.holds,
        (unsigned long long)x->t1.hot_held, (unsigned long long)x->t1.exits_by_kind[T1_PT_SITE],
        (unsigned long long)x->t1.traps, (unsigned long long)x->t1.decline_validate,
        (unsigned long long)x->t0.translations, (unsigned long long)x->t0.invalidations,
        (unsigned long long)x->cv.invalidations, (unsigned long long)x->cv.mismatches,
        (unsigned long long)x->cv.comparisons, (unsigned long long)x->cc.invalidated_pages,
        (unsigned long long)x->cc.decodes);
}

/* Target for bundle coherence: on the shared layout, the data
 * stores to the code page invalidate nothing -- no validity hit, no Tier0
 * or Tier1 retirement, no decoded bundle -- on every engine, and the
 * results equal the reference. Returns the per-engine runs. */
static void lz_layout(const char *image, const char *layout, uint64_t iters, bool print,
                      struct lz_run runs[3], struct lz_stores *st)
{
    lz_open(image, iters);
    lz_reference(st);
    if (print)
        printf("coh-lz layout %s reference retired %llu code_page_stores %llu bytes %#llx-%#llx\n",
               layout, (unsigned long long)(R.v.retired - S.v.retired), (unsigned long long)st->n,
               (unsigned long long)(st->n ? st->lo : 0), (unsigned long long)st->hi);
    const struct t1h_engine *engines[] = {t1h_engine_named("tier0"), t1h_engine_tier1(),
                                          t1h_engine_tier0t1()};
    for (unsigned i = 0; i < 3; ++i) {
        char label[96];
        snprintf(label, sizeof(label), "LZ %s layout on %s", layout, engines[i]->name);
        lz_engine(engines[i], &runs[i], label);
        if (print)
            lz_print(layout, engines[i]->name, iters, &runs[i]);
        T1H_CHECK(runs[i].cv.invalidations == 0 && runs[i].cv.mismatches == 0);
        T1H_CHECK(runs[i].t0.invalidations == 0 && runs[i].t1.invalidations == 0);
        T1H_CHECK(runs[i].cc.invalidated_pages == 0);
    }
    machine_close();
}

static void lz_shared(void)
{
    struct lz_run shared[3], separate[3];
    struct lz_stores st_shared, st_separate;
    /* The shared flat image ends at initialized text/data; its BSS follows.
     * Compiler versions and flags can move that boundary. */
    FILE *image = fopen(t1h_image("csuite-shared.bin"), "rb");
    T1H_CHECK(image && fseek(image, 0, SEEK_END) == 0);
    long bytes = ftell(image);
    T1H_CHECK(bytes > 0 && (uint64_t)bytes < T1H_RAM_SIZE && fclose(image) == 0);
    uint64_t data = T1H_CODE + (uint64_t)bytes;
    uint64_t end = (data + 4095) & ~UINT64_C(4095);
    lz_layout("csuite-shared.bin", "shared", 1, false, shared, &st_shared);
    lz_layout("csuite.bin", "separate", 1, false, separate, &st_separate);
    /* The shared layout stores to its code page; the separate one never. */
    T1H_CHECK(st_shared.n > 0 && st_shared.lo >= data && st_shared.hi < end);
    T1H_CHECK(st_separate.n == 0);
    /* No retirement, so the v1 engine (no demotion) compiles the same
     * regions in both layouts. */
    T1H_CHECK(shared[1].t1.compiles == separate[1].t1.compiles);
    /* The product path demotes regions whose stores to the code page exit
     * at their sites and compiles a held head again
     * on the hold's schedule. Formation past helpers also exposes the
     * continuation of a trapping store as a new Tier0 lookup head: bound
     * extra compiles by demotions plus those additional translations. */
    uint64_t new_heads = shared[2].t0.translations > separate[2].t0.translations
                             ? shared[2].t0.translations - separate[2].t0.translations
                             : 0;
    T1H_CHECK(shared[2].t1.compiles <=
              separate[2].t1.compiles + shared[2].t1.demote_site + new_heads);
    printf(
        "coherence: LZ shared layout (1 iteration, %llu instructions): %llu stores to the code page at "
        "%#llx-%#llx; tier0, tier1, tier0+tier1: 0 validity invalidations, 0 Tier0 / Tier1 / decoded-cache "
        "retirements; Tier1 compiles shared/separate: tier1 %llu/%llu, tier0+tier1 %llu/%llu (%llu site "
        "demotions); equal to the reference\n",
        (unsigned long long)shared[0].retired, (unsigned long long)st_shared.n,
        (unsigned long long)st_shared.lo, (unsigned long long)st_shared.hi,
        (unsigned long long)shared[1].t1.compiles, (unsigned long long)separate[1].t1.compiles,
        (unsigned long long)shared[2].t1.compiles, (unsigned long long)separate[2].t1.compiles,
        (unsigned long long)shared[2].t1.demote_site);
    ++cases;
}

/* tier1-test --suite=coh-lz [T1H_COH_LZ_ITERS=n]: the measurement on both
 * layouts (default: 5 iterations, about 19.9 M instructions). */
void coherence_lz(void);
void coherence_lz(void)
{
    snapshot_init(&S);
    result_init(&R);
    result_init(&ACT);
    const char *env = getenv("T1H_COH_LZ_ITERS");
    uint64_t iters = env ? strtoull(env, NULL, 0) : 5;
    struct lz_run runs[3];
    struct lz_stores st;
    lz_layout("csuite-shared.bin", "shared", iters, true, runs, &st);
    lz_layout("csuite.bin", "separate", iters, true, runs, &st);
    result_free(&R);
    result_free(&ACT);
    snapshot_free(&S);
}

/* ---- entry points --------------------------------------------------------- */

void coherence_tests(void);
void coherence_tests(void)
{
    snapshot_init(&S);
    result_init(&R);
    result_init(&R0);
    result_init(&ACT);
    /* lockstep+tier1: Coherence paths under checked lockstep. */
    const struct t1h_engine *engines[] = {t1h_engine_tier1(), t1h_engine_tier0t1(),
                                          t1h_engine_checked()};
    /* T1H_COH_ENGINE=tier1 | tier0+tier1 | lockstep+tier1: one engine only. */
    const char *only = getenv("T1H_COH_ENGINE");
    /* Preserve the fixed loop layout checks, then run every change and
     * page-reuse case against the inventory with tail formation enabled. */
    bool tails = t1_test_set_tail_hotness(false);
    for (unsigned i = 0; i < 3; ++i) {
        if (only && strcmp(only, engines[i]->name))
            continue;
        for (unsigned virt = 0; virt < 2; ++virt) {
            coh_matrix(engines[i], virt, false);
            stay_case(engines[i], virt);
        }
        coh_races(engines[i]);
        store_case(engines[i], false, true);
        store_case(engines[i], false, false);
        store_case(engines[i], true, true);
    }
    t1_test_set_tail_hotness(tails);
    if (!only || !strcmp(only, "tier1")) {
        t1_test_set_tail_hotness(true);
        for (unsigned virt = 0; virt < 2; ++virt)
            coh_matrix(t1h_engine_tier1(), virt, true);
        t1_test_set_tail_hotness(tails);
    }
    if (!only) {
        hold_case(false);
        hold_case(true);
        lz_shared();
    }
    printf("coherence: %u cases, %u comparisons with the reference\n", cases, comparisons);
    result_free(&R);
    result_free(&R0);
    result_free(&ACT);
    snapshot_free(&S);
}

/* The per-store cost of a Tier1 window store to a page
 * that holds code, outside every dependency's bundles, against the same
 * loop storing to a data page. The region is compiled explicitly (no
 * demotion), so every store is a trap, a site exit, materialization, the
 * store by Tier0 (or the lower tier) with the bundle filter, and the next
 * entry with its validation. Production regions, no store log, no MMU-log
 * completeness check when T1H_HOSTWIN_CHECK=0. */

void coherence_bench(void);
void coherence_bench(void)
{
    snapshot_init(&S);
    result_init(&R);
    const char *env = getenv("T1H_COH_BENCH_N"); /* stores per run (default 200000) */
    const uint64_t n = env ? strtoull(env, NULL, 0) : 200000;
    const struct t1h_engine *engines[] = {t1h_engine_tier1(), t1h_engine_tier0t1()};
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned code_page = 0; code_page < 2; ++code_page) {
            struct ia64_entry_regs regs = store_regs(code_page ? ST_DATA : T1H_OUT, 0x5a5a, n);
            machine_open("coherence_stores.bin", &regs, false);
            struct t1h_engine_vcpu ev;
            t1h_engine_bind(&ev, engines[i], vt);
            production_flags(engines[i], true);
            snapshot_restore(&S, vt);
            t1h_engine_restored(&ev);
            struct vcpu_exit e = t1h_engine_run(&ev, 6);
            T1H_CHECK(e.reason == VCPU_EXIT_BUDGET && vt->s.ip == ST_LOOP);
            struct t1_compile_options opt = {0};
            struct t1_compile_report rep;
            T1H_CHECK(t1_compile(t1h_tier1_vcpu(ev.ctx), &opt, &rep) == T1_COMPILED);
            struct t1_stats s0 = t1_now();
            struct tier0_stats z0 = t0_now();
            struct code_validity_stats c0 = cv_now();
            uint64_t t0 = now_ns();
            e = t1h_engine_run(&ev, BIG_BUDGET);
            uint64_t ns = now_ns() - t0;
            struct t1_stats s1 = t1_now();
            struct tier0_stats z1 = t0_now();
            struct code_validity_stats c1 = cv_now();
            T1H_CHECK(at_end(&e, &logs) && vt->s.gr[14] == n);
            printf(
                "coh-store engine %s target %s stores %llu ns %llu ns_per_store %.1f entries %llu site_exits %llu "
                "traps %llu compiles %llu t1_invalidations %llu t0_invalidations %llu validity_invalidations %llu "
                "comparisons %llu\n",
                engines[i]->name, code_page ? "code-page" : "data-page", (unsigned long long)n,
                (unsigned long long)ns, (double)ns / (double)n,
                (unsigned long long)DELTA(s0, s1, entries),
                (unsigned long long)DELTA(s0, s1, exits_by_kind[T1_PT_SITE]),
                (unsigned long long)DELTA(s0, s1, traps),
                (unsigned long long)DELTA(s0, s1, compiles),
                (unsigned long long)DELTA(s0, s1, invalidations),
                (unsigned long long)DELTA(z0, z1, invalidations),
                (unsigned long long)(c1.invalidations - c0.invalidations),
                (unsigned long long)(c1.comparisons - c0.comparisons));
            production_flags(engines[i], false);
            t1h_engine_release(&ev);
            machine_close();
        }
    result_free(&R);
    snapshot_free(&S);
}
