/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ir_eval vs interp_run: the lifted and
 * optimized IR of a region, executed by the test evaluator inside the machine's
 * minimal dispatcher, must leave the same state as the interpreter, both
 * at the end of the run and in place right after every materialization for
 *   - the guest loops gather, gather-rot, gather-rotp and shladd_xor (frag_loops.S) and a loop
 *     nest, whole, with forced exits at every site and helper point x
 *     occurrences 1-3, with event checkpoints at iterations 1-3 and with
 *     every budget split;
 *   - the NaT counterexample and its variants;
 *   - the frame-rule cases (IR level, identity simplification on);
 *   - 1,000 random programs over the v1 subset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/um_machine.h"
#include "tests/tier1/um_random.h"
#include "tests/tier1/bof.h"
#include "engine/interp/interp.h"

static uint64_t rng = 0x9e3779b97f4a7c15u;
static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}
static unsigned rnd(unsigned n)
{
    return (unsigned)(rnd64() % n);
}

/* ---- forcing hooks: one (point, occurrence) across region entries ---- */

struct arm {
    uint32_t point;
    uint64_t occurrence, seen, iteration, iterations;
    bool fired;
};
static bool force_hook(void *arg, uint32_t point, uint64_t occurrence)
{
    (void)occurrence;
    struct arm *a = arg;
    if (a->fired || point != a->point)
        return false;
    if (++a->seen < a->occurrence)
        return false;
    a->fired = true;
    return true;
}
static bool event_hook(void *arg, uint64_t iteration)
{
    (void)iteration;
    struct arm *a = arg;
    if (a->fired)
        return false;
    if (++a->iterations < a->iteration)
        return false;
    a->fired = true;
    return true;
}

struct totals {
    unsigned cases, captures, forced, entries, exits[4], guard_rejects;
    uint64_t tier1_retired;
};
static struct totals T;

#define MAX_CAPTURES 4096
static struct um_capture *captures;

/* One run of v (restored from snap) with region r and the hooks; compared
 * with the reference. Returns the run for further checks. */
static struct um_run run_case(struct um_region *r, struct vcpu *v, struct vcpu *ref,
                              const struct um_snapshot *snap, uint64_t budget,
                              const struct ir_eval_hooks *hooks, const char *label)
{
    um_snapshot_restore(snap, v);
    struct um_run run = {.hooks = hooks, .captures = captures, .max_captures = MAX_CAPTURES};
    struct vcpu_exit e = um_dispatch(r, v, budget, &run);
    UM_CHECK(run.ncaptures < MAX_CAPTURES);
    if (um_compare(snap, ref, &run, v, &e, budget, label)) {
        fprintf(stderr, "%s: budget %llu, %u entries\n", label, (unsigned long long)budget,
                run.entries);
        UM_CHECK(!"ir_eval differs from interp_run");
    }
    ++T.cases;
    T.captures += run.ncaptures;
    T.forced += run.forced;
    T.entries += run.entries;
    T.guard_rejects += run.guard_rejects;
    T.tier1_retired += run.tier1_retired;
    for (unsigned k = 0; k < 4; ++k)
        T.exits[k] += run.exits[k];
    return run;
}

/* Every forced exit (site and helper points x occurrences 1..3), event
 * checkpoints at iterations 1..3, and budgets 0..split_max. */
static void sweep(struct um_region *r, struct vcpu *v, struct vcpu *ref,
                  const struct um_snapshot *snap, uint64_t budget, unsigned split_max,
                  const char *label)
{
    char name[96];
    struct um_run whole = run_case(r, v, ref, snap, budget, NULL, label);
    UM_CHECK(whole.entries > 0 && whole.tier1_retired > 0);
    for (uint32_t p = 0; p < r->ir->npoints; ++p) {
        const struct t1_ir_point *pt = &r->ir->point[p];
        if (pt->dead || (pt->kind != T1_PT_SITE && pt->kind != T1_PT_HELPER))
            continue;
        for (unsigned k = 1; k <= 3; ++k) {
            struct arm a = {.point = p, .occurrence = k};
            struct ir_eval_hooks h = {force_hook, NULL, &a};
            snprintf(name, sizeof(name), "%s force P%u #%u", label, p, k);
            struct um_run run = run_case(r, v, ref, snap, budget, &h, name);
            UM_CHECK(!a.fired || run.forced == 1);
        }
    }
    for (unsigned k = 1; k <= 3; ++k) {
        struct arm a = {.iteration = k};
        struct ir_eval_hooks h = {NULL, event_hook, &a};
        snprintf(name, sizeof(name), "%s event #%u", label, k);
        run_case(r, v, ref, snap, budget, &h, name);
    }
    for (unsigned b = 0; b <= split_max; ++b) {
        snprintf(name, sizeof(name), "%s budget %u", label, b);
        run_case(r, v, ref, snap, b, NULL, name);
    }
}

/* ---- the guest loops ---- */

enum gather_variant {
    GATHER_PHYS,
    GATHER_ROT,
    GATHER_ROTP,
    GATHER_SIDE_EXIT,
    GATHER_ALIAS63,
    GATHER_VIRT_VRN1,
    GATHER_VIRT_VRN2
};

static void gather(const char *image, enum gather_variant variant)
{
    static const char *names[] = {
        "gather-phys",           "gather-rot",           "gather-rotp",         "gather-side-exit",
        "gather-alias (bit 63)", "gather-alias (VRN 1)", "gather-alias (VRN 2)"};
    /* gather-alias: the same physical code entered through the bit-63
     * physical alias and through two virtual aliases. */
    uint64_t alias = variant == GATHER_ALIAS63     ? UINT64_C(1) << 63
                     : variant == GATHER_VIRT_VRN1 ? UINT64_C(1) << 61
                     : variant == GATHER_VIRT_VRN2 ? UINT64_C(2) << 61
                                                   : 0;
    uint64_t entry = (UM_CODE + (variant == GATHER_ROTP ? 0x200 : 0)) | alias;
    struct vcpu *v = um_vcpu(entry), *ref = um_vcpu(entry);
    um_load_file(UM_CODE, image);
    unsigned n = 40;
    uint64_t index = UM_DATA, table = UM_DATA + 0x2000, out = UM_DATA + 0x4000;
    memset(um_ram(UM_DATA), 0, UM_DATA_SIZE);
    uint64_t t[64];
    for (unsigned i = 0; i < 64; ++i) {
        t[i] = rnd64() & 0xfffff;
        memcpy(um_ram(table + 8 * i), &t[i], 8);
    }
    for (unsigned i = 0; i < n; ++i) {
        uint64_t x = rnd(4) ? 1 + rnd(63) : 0;
        if (variant == GATHER_SIDE_EXIT && i == 30)
            x = 7;
        memcpy(um_ram(index + 8 * i), &x, 8);
    }
    code_notify_vmm_write(UM_DATA, UM_DATA_SIZE);
    if (variant == GATHER_ROT)
        v->s.cfm = um_cfm(8, 8, 8, 3, 0);
    if (variant == GATHER_ROTP)
        v->s.cfm = um_cfm(8, 8, 8, 5, 45);         /* p16..p20 -> p61,p62,p63,p16,p17 */
    v->s.rse_bof = variant == GATHER_ROT ? 90 : 7; /* r32.. wrap the 96-entry ring */
    um_gr(v, 32, index, false);
    um_gr(v, 33, table, false);
    um_gr(v, 34, out, false);
    um_gr(v, 35, 0x80000, false);
    um_gr(v, 36, variant == GATHER_SIDE_EXIT ? t[7] : UINT64_MAX, false);
    um_gr(v, 14, 0, false);
    um_gr(v, 15, 0, false);
    v->s.ar[AR_LC] = n - 1;
    if (variant == GATHER_VIRT_VRN1 || variant == GATHER_VIRT_VRN2)
        um_virtual_code(v);
    /* Predicates the loop writes start as 1, so qp = 0 effects show. */
    for (unsigned p = 6; p <= 20; ++p)
        um_pr(v, p, true);
    um_pr(v, 61, true);
    um_pr(v, 62, true);
    um_pr(v, 63, true);
    struct um_region r;
    char err[200];
    if (um_compile(v, &r, err, sizeof(err)) != T1_COMPILED) {
        fprintf(stderr, "%s: %s\n", names[variant], err);
        UM_CHECK(!"gather compiles");
    }
    /* G: the bases and compare sources r32-r36 (NATCHK, compares) and
     * r16-r18, whose entry NaT reaches the frames of the first iteration's
     * points through the header parameters. */
    UM_CHECK(r.form->loop && r.ir->nsites == 3 && r.guards == 8);
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    sweep(&r, v, ref, &snap, 26 * n + 60, 40, names[variant]);
    um_snapshot_free(&snap);
    um_region_free(&r);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

static void shladd_xor(const char *image)
{
    struct vcpu *v = um_vcpu(UM_CODE + 0x400), *ref = um_vcpu(UM_CODE + 0x400);
    um_load_file(UM_CODE, image);
    um_gr(v, 16, 5, false);
    um_gr(v, 17, 0x1234, false);
    um_gr(v, 18, 0xfeed, false);
    v->s.ar[AR_LC] = 50;
    struct um_region r;
    char err[200];
    if (um_compile(v, &r, err, sizeof(err)) != T1_COMPILED)
        fprintf(stderr, "shladd_xor: %s\n", err);
    UM_CHECK(r.valid);
    UM_CHECK(r.form->loop && r.ir->nsites == 0 && r.guards == 3); /* r16-r18 NaTs reach frames */
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    sweep(&r, v, ref, &snap, 51 * 9 + 20, 30, "shladd_xor");
    um_snapshot_free(&snap);
    um_region_free(&r);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

/* ---- hand-built regions ---- */

#define PROG (UM_CODE + 0x1000) /* hand-built programs */
#define SPIN(at) ((struct ienc_bundle){IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}})

static struct vcpu *program(const struct ienc_bundle *b, unsigned n, struct vcpu **ref)
{
    um_emit(PROG, b, n);
    /* The region's fall-off target spins; formation stops at the
     * unreachable boundary after it, not at older programs' code. */
    struct ienc_bundle tail[] = {SPIN(0),
                                 {IENC_MII, {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}}};
    um_emit(PROG + 16u * n, tail, 2);
    *ref = um_vcpu(PROG);
    return um_vcpu(PROG);
}

static bool compile(struct vcpu *v, struct um_region *r, const char *label)
{
    char err[200];
    if (um_compile(v, r, err, sizeof(err)) == T1_COMPILED)
        return true;
    fprintf(stderr, "%s: %s\n", label, err);
    return false;
}

/* A loop nest: an outer loop counted by r20 with an inner
 * br.cloop loop; loads in both loops, a store in the outer one. r22 is
 * carried around the outer loop and read in the inner one, r20 and r35
 * only in the outer one.
 *  +0x00 { ld8 r22 = [r33], 8 ; adds r20 = -1, r20 ; mov ar.lc = r21 } ;;   head
 *  +0x10 { ld8 r23 = [r34], 8 ; nop.m ; nop.i } ;;                          inner header
 *  +0x20 { nop.m ; add r14 = r14, r23 ; nop.i } ;;
 *  +0x30 { nop.m ; add r15 = r15, r22 ; br.cloop +0x10 } ;;
 *  +0x40 { cmp.eq p7, p6 = r20, r0 ; nop.i ; nop.i } ;;
 *  +0x50 { st8 [r35] = r15, 8 ; nop.i ; (p6) br head } ;; */
#define NEST_BUNDLES 6u
static const struct ienc_bundle *nest_program(void)
{
    static struct ienc_bundle b[NEST_BUNDLES];
    struct ienc_bundle n[NEST_BUNDLES] = {
        {IENC_STOP(IENC_MII),
         {ienc_ld_inc_imm(0, 8, false, 22, 33, 8), ienc_adds(0, 20, -1, 20),
          ienc_mov_to_ar(0, AR_LC, 21)}},
        {IENC_STOP(IENC_MII),
         {ienc_ld_inc_imm(0, 8, false, 23, 34, 8), ienc_nop_i(), ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_alu(0, IENC_ADD, 14, 14, 23), ienc_nop_i()}},
        {IENC_STOP(IENC_MIB),
         {ienc_nop_m(), ienc_alu(0, IENC_ADD, 15, 15, 22), ienc_br_cloop(-0x20)}},
        {IENC_STOP(IENC_MII),
         {ienc_cmp(0, IENC_EQ, false, false, 7, 6, 20, 0), ienc_nop_i(), ienc_nop_i()}},
        {IENC_STOP(IENC_MIB),
         {ienc_st_inc_imm(0, 8, false, 35, 15, 8), ienc_nop_i(), ienc_br_cond(6, -0x50)}},
    };
    memcpy(b, n, sizeof(b));
    return b;
}

/* Every point of the nest, every budget split over two outer passes, with
 * three inner iterations and with one. */
static void nested(void)
{
    for (unsigned inner = 0; inner < 2; ++inner) {
        struct vcpu *ref, *v = program(nest_program(), NEST_BUNDLES, &ref);
        memset(um_ram(UM_DATA), 0, UM_DATA_SIZE);
        for (unsigned i = 0; i < 64; ++i) {
            uint64_t x = rnd64();
            memcpy(um_ram(UM_DATA + 8 * i), &x, 8);
        }
        code_notify_vmm_write(UM_DATA, UM_DATA_SIZE);
        um_gr(v, 20, 3, false);
        um_gr(v, 21, inner ? 0 : 2, false);
        um_gr(v, 33, UM_DATA, false);
        um_gr(v, 34, UM_DATA + 0x100, false);
        um_gr(v, 35, UM_DATA + 0x800, false);
        um_gr(v, 14, 0, false);
        um_gr(v, 15, 0, false);
        struct um_region r;
        UM_CHECK(compile(v, &r, "nest"));
        /* Two headers: the head and the inner one. */
        unsigned backedges = 0;
        uint16_t header = T1_NO_BLOCK;
        for (uint16_t b = 0; b < r.ir->nblocks; ++b) {
            uint32_t t = t1_ir_terminator(r.ir, b);
            if (t == T1_NO_OP || r.ir->op[t].code != T1_IR_BACKEDGE)
                continue;
            ++backedges;
            if (r.ir->block[b].succ[0].target != T1_HEADER_BLOCK)
                header = r.ir->block[b].succ[0].target;
        }
        UM_CHECK(r.form->loop && backedges == 2 && header != T1_NO_BLOCK && r.ir->nsites == 3);
        unsigned pass = 3 + (inner ? 1 : 3) * 9 + 6;
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        sweep(&r, v, ref, &snap, 3 * pass + 10, 2 * pass + 1,
              inner ? "nest, one inner iteration" : "nest");
        um_snapshot_free(&snap);
        um_region_free(&r);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
}

/* NaT counterexample: nat[r20] = 1 at entry,
 * (p8) mov r20 = r21 ;; (p6) st8 [r34] = r20 with p8 = 0, p6 = 1. r20 is
 * in G, the guard rejects the entry, the lower tier raises Register NaT
 * Consumption and no store happens. Variants: p8 = 1 (still rejected,
 * conservative) and an unconditional write of r20 (not in G, the region
 * runs, nat[r20] materializes as 0). */
static void natguard_counterexample(void)
{
    for (unsigned variant = 0; variant < 3; ++variant) {
        unsigned qp = variant == 2 ? 0 : 8;
        struct ienc_bundle b[] = {
            {IENC_STOP(IENC_MII), {ienc_adds(qp, 20, 0, 21), ienc_nop_i(), ienc_nop_i()}},
            {IENC_STOP(IENC_MMI), {ienc_st(6, 8, false, 34, 20), ienc_nop_m(), ienc_nop_i()}},
        };
        struct vcpu *ref, *v = program(b, 2, &ref);
        uint64_t out = UM_DATA + 0x100;
        memset(um_ram(out), 0x5a, 8);
        um_gr(v, 34, out, false);
        um_gr(v, 21, 0x1234, false);
        um_gr(v, 20, 77, true);
        um_pr(v, 8, variant == 1);
        um_pr(v, 6, true);
        struct um_region r;
        UM_CHECK(compile(v, &r, "natguard"));
        bool in_g = (r.ir->nat_guard[0] >> 20) & 1;
        UM_CHECK(in_g == (variant != 2));
        UM_CHECK(((r.ir->nat_guard[0] >> 21) & 1) && ((r.ir->nat_guard[0] >> 34) & 1));
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct um_run run = run_case(&r, v, ref, &snap, 20, NULL, "natguard counterexample");
        if (variant == 2) {
            UM_CHECK(run.entries >= 1 && !v->s.nat[20] && v->s.gr[20] == 0x1234);
            UM_CHECK(!memcmp(um_ram(out), &v->s.gr[20], 8));
        } else {
            UM_CHECK(run.entries == 0 && run.guard_rejects >= 1);
            uint64_t untouched;
            memset(&untouched, 0x5a, 8);
            UM_CHECK(!memcmp(um_ram(out), &untouched, 8) == (variant == 0));
            /* The interpreter delivered Register NaT Consumption (variant 0)
             * or stored nothing because r20 kept its NaT (variant 1 writes
             * r20 = r21 first: no fault, the store happens in the lower tier). */
            if (variant == 0)
                UM_CHECK(v->fault.kind == IA64_FAULT_NAT);
        }
        um_snapshot_free(&snap);
        um_region_free(&r);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
}

/* NaT through a register post-increment: ld8 r10 = [r2], r9 gives the
 * base NaT(r2) | NaT(r9) (ia64/load_store.c increment_nat), so r9 is in G;
 * with nat[r9] = 1 the entry is rejected and the lower tier propagates it. */
static void nat_post_increment(void)
{
    for (unsigned nat9 = 0; nat9 < 2; ++nat9) {
        struct ienc_bundle b[] = {
            {IENC_STOP(IENC_MMI),
             {ienc_ld_inc_reg(0, 8, false, 10, 2, 9), ienc_nop_m(), ienc_nop_i()}}};
        struct vcpu *ref, *v = program(b, 1, &ref);
        um_gr(v, 2, UM_DATA + 0x300, false);
        um_gr(v, 9, 8, nat9);
        struct um_region r;
        UM_CHECK(compile(v, &r, "nat post-increment"));
        UM_CHECK((r.ir->nat_guard[0] >> 9) & 1);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct um_run run = run_case(&r, v, ref, &snap, 10, NULL, "nat post-increment");
        UM_CHECK((run.entries == 0) == nat9 && v->s.nat[2] == nat9);
        um_snapshot_free(&snap);
        um_region_free(&r);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
}

/* ---- frame-rule ---- */

static uint16_t gr_element(const struct vcpu *v, unsigned reg)
{
    return T1_ELEMENT_GR(t1t_element_reg(&v->s, reg)); /* frame-relative */
}

/* The frame entry of element e at point p. */
static struct t1_use *frame_entry(struct t1_ir *ir, uint32_t p, uint16_t e)
{
    const struct t1_ir_point *pt = &ir->point[p];
    for (unsigned k = 0; k < pt->frame_count; ++k)
        if (ir->use[pt->frame_first + k].element == e)
            return &ir->use[pt->frame_first + k];
    return NULL;
}

static uint32_t first_point(const struct t1_ir *ir, unsigned kind)
{
    for (uint32_t p = 0; p < ir->npoints; ++p)
        if (!ir->point[p].dead && ir->point[p].kind == kind)
            return p;
    return T1_NO_POINT;
}

/* adds r21 = 0, r20 ;; adds r20 = 1, r20 ;; tnat.z p6, p7 = r20 ;;
 * adds r20 = 0, r21: with ADD(x, 0) -> x, current(r20) = LIVEIN(r20) = 10
 * at the end, but the helper wrote 11 to the home; r20 is in W, so every
 * frame after it writes 10. */
#define FRAME_RULE_SEQ(q)                                                                          \
    {IENC_STOP(IENC_MII), {ienc_adds(q, 21, 0, 20), ienc_adds(0, 20, 1, 20), ienc_nop_i()}},       \
        {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_tnat(0, 0, 0, 0, 6, 7, 20), ienc_nop_i()}},      \
    {                                                                                              \
        IENC_STOP(IENC_MII),                                                                       \
        {                                                                                          \
            ienc_adds(0, 20, 0, 21), ienc_nop_i(), ienc_nop_i()                                    \
        }                                                                                          \
    }

static void frame_rule_check_frames(const struct um_region *r, const struct vcpu *v,
                                    const char *label)
{
    uint16_t e20 = gr_element(v, 20);
    UM_CHECK(t1_elem_has(r->ir->w_set, e20));
    for (uint32_t p = 0; p < r->ir->npoints; ++p) {
        if (r->ir->point[p].dead)
            continue;
        struct t1_use *u = frame_entry(r->ir, p, e20);
        if (!u || (u->flags & T1_USE_HOME)) {
            fprintf(stderr, "%s: point %u sources r20 (in W) from its home\n", label, p);
            UM_CHECK(!"W element in every frame");
        }
    }
}

static void frame_rule_tests(void)
{
    char name[64];
    /* 1: normal exit after the sequence. 2: exit at a memory point after it. */
    for (unsigned variant = 0; variant < 2; ++variant) {
        struct ienc_bundle b[] = {
            FRAME_RULE_SEQ(0),
            {IENC_STOP(IENC_MMI),
             {variant ? ienc_ld(0, 8, false, 22, 33) : ienc_nop_m(), ienc_nop_m(), ienc_nop_i()}},
        };
        struct vcpu *ref, *v = program(b, 4, &ref);
        um_gr(v, 20, 10, false);
        um_gr(v, 33, UM_DATA + 0x200, false);
        struct um_region r;
        UM_CHECK(compile(v, &r, "frame-rule"));
        frame_rule_check_frames(&r, v, "frame-rule");
        /* The identity simplification made current(r20) = LIVEIN(r20) at
         * the end of the sequence (the exit at the spin, or its checkpoint
         * where the spin is an inner loop): the entry is in the frame only
         * because r20 is in W. */
        uint32_t exit = first_point(r.ir, T1_PT_EXIT);
        if (exit == T1_NO_POINT)
            exit = first_point(r.ir, T1_PT_CHECKPOINT);
        UM_CHECK(exit != T1_NO_POINT);
        const struct t1_use *u = frame_entry(r.ir, exit, gr_element(v, 20));
        UM_CHECK(u && !(u->flags & T1_USE_HOME) &&
                 r.ir->op[T1_VALUE_OP(u->value)].code == T1_IR_LIVEIN);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        snprintf(name, sizeof(name), "frame-rule %s", variant ? "memory point" : "exit");
        struct um_run run = run_case(&r, v, ref, &snap, 30, NULL, name);
        UM_CHECK(run.entries == 1 && v->s.gr[20] == 10 && v->s.gr[21] == 10);
        if (variant) {
            uint32_t site = first_point(r.ir, T1_PT_SITE);
            struct arm a = {.point = site, .occurrence = 1};
            struct ir_eval_hooks h = {force_hook, NULL, &a};
            run = run_case(&r, v, ref, &snap, 30, &h, "frame-rule forced memory point");
            UM_CHECK(run.forced == 1 && run.ncaptures >= 1 && run.captures[0].v.s.gr[20] == 10);
        }
        um_snapshot_free(&snap);
        um_region_free(&r);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    /* 3: a diamond where only one branch runs the helper; the point (a
     * load, then the exit) follows the join. */
    for (unsigned taken = 0; taken < 2; ++taken) {
        struct ienc_bundle b[] = {
            {IENC_STOP(IENC_MIB),
             {ienc_cmp(0, IENC_EQ, false, false, 8, 9, 1, 0), ienc_nop_i(),
              ienc_br_cond(8, 4 * 16)}},
            FRAME_RULE_SEQ(0),
            {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 22, 33), ienc_nop_m(), ienc_nop_i()}},
        };
        struct vcpu *ref, *v = program(b, 5, &ref);
        um_gr(v, 1, taken ? 0 : 1, false);
        um_gr(v, 20, 10, false);
        um_gr(v, 33, UM_DATA + 0x200, false);
        struct um_region r;
        UM_CHECK(compile(v, &r, "frame-rule diamond"));
        frame_rule_check_frames(&r, v, "frame-rule diamond");
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        uint32_t site = first_point(r.ir, T1_PT_SITE);
        struct arm a = {.point = site, .occurrence = 1};
        struct ir_eval_hooks h = {force_hook, NULL, &a};
        struct um_run run = run_case(&r, v, ref, &snap, 30, &h, "frame-rule diamond");
        UM_CHECK(run.forced == 1);
        run_case(&r, v, ref, &snap, 30, NULL, "frame-rule diamond whole");
        um_snapshot_free(&snap);
        um_region_free(&r);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    /* 4: a loop: the helper runs in iteration i, the point (the load at the
     * header) is reached in iteration i + 1 before the helper. */
    {
        struct ienc_bundle b[] = {
            {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 22, 33), ienc_nop_m(), ienc_nop_i()}},
            FRAME_RULE_SEQ(0),
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(-4 * 16)}},
        };
        struct vcpu *ref, *v = program(b, 5, &ref);
        um_gr(v, 20, 10, false);
        um_gr(v, 33, UM_DATA + 0x200, false);
        v->s.ar[AR_LC] = 4;
        struct um_region r;
        UM_CHECK(compile(v, &r, "frame-rule loop"));
        UM_CHECK(r.form->loop);
        frame_rule_check_frames(&r, v, "frame-rule loop");
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        uint32_t site = first_point(r.ir, T1_PT_SITE);
        for (unsigned k = 1; k <= 3; ++k) {
            struct arm a = {.point = site, .occurrence = k};
            struct ir_eval_hooks h = {force_hook, NULL, &a};
            struct um_run run = run_case(&r, v, ref, &snap, 60, &h, "frame-rule loop");
            UM_CHECK(run.forced == 1 && run.captures[0].v.s.gr[20] == 10);
        }
        /* 5: a hand-built frame that sources r20 (in W) from its home at
         * that point: the frame-rule must-analysis cannot prove it -- the helper
         * of the previous iteration wrote the home -- so the verifier
         * rejects it. */
        char err[200];
        struct t1_use *u = frame_entry(r.ir, site, gr_element(v, 20));
        t1_value saved = u->value;
        u->flags |= T1_USE_HOME;
        u->value = T1_VALUE_NONE;
        UM_CHECK(!t1_ir_verify(r.ir, T1_VERIFY_NAT_FOLDED, err, sizeof(err)));
        UM_CHECK(strstr(err, "not proven") != NULL);
        u->flags &= (uint16_t)~T1_USE_HOME;
        u->value = saved;
        UM_CHECK(t1_ir_verify(r.ir, T1_VERIFY_NAT_FOLDED, err, sizeof(err)));
        um_snapshot_free(&snap);
        um_region_free(&r);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    /* 5b: before any helper can have run, the optimizer proves HOME
     * even for an element in W; the independent verifier accepts it. */
    {
        struct ienc_bundle b[] = {
            {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 22, 33), ienc_nop_m(), ienc_nop_i()}},
            FRAME_RULE_SEQ(0),
        };
        struct vcpu *ref, *v = program(b, 4, &ref);
        um_gr(v, 33, UM_DATA + 0x200, false);
        struct um_region r;
        UM_CHECK(compile(v, &r, "frame-rule home"));
        char err[200];
        uint32_t site = first_point(r.ir, T1_PT_SITE);
        struct t1_use *u = frame_entry(r.ir, site, gr_element(v, 20));
        UM_CHECK(u && (u->flags & T1_USE_HOME) && u->value == T1_VALUE_NONE);
        UM_CHECK(t1_ir_verify(r.ir, T1_VERIFY_NAT_FOLDED, err, sizeof(err)));
        um_region_free(&r);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
}

/* ---- random programs ---- */

static void random_programs(unsigned count)
{
    unsigned compiled = 0, entered = 0, nat_programs = 0, nat_rejected = 0, forced_runs = 0;
    unsigned helpers = 0, memory_ops = 0, boundaries = 0;
    char name[64];
    for (unsigned seed = 0; seed < count; ++seed) {
        struct vcpu *v, *ref;
        struct um_random_info info;
        um_random_program(seed, &v, &ref, &info);
        helpers += info.helpers;
        memory_ops += info.memory_ops;
        boundaries += info.boundaries;
        nat_programs += info.nat;
        struct um_region r;
        char err[200];
        enum t1_compile_status st = um_compile(v, &r, err, sizeof(err));
        if (st != T1_COMPILED) {
            if (strncmp(err, "form:", 5)) {
                fprintf(stderr, "random program %u: %s\n", seed, err);
                UM_CHECK(!"random program lifts and optimizes");
            }
            um_region_free(&r);
            vcpu_destroy(v);
            vcpu_destroy(ref);
            continue;
        }
        ++compiled;
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        uint64_t budget = 100 + rnd(300);
        snprintf(name, sizeof(name), "random %u", seed);
        struct um_run run = run_case(&r, v, ref, &snap, budget, NULL, name);
        entered += run.entries > 0;
        nat_rejected += !run.entries && run.guard_rejects;
        /* A forced exit at a random site or helper point and occurrence. */
        uint32_t points[T1_MAX_POINTS], n = 0;
        for (uint32_t p = 0; p < r.ir->npoints; ++p)
            if (!r.ir->point[p].dead &&
                (r.ir->point[p].kind == T1_PT_SITE || r.ir->point[p].kind == T1_PT_HELPER))
                points[n++] = p;
        if (n && run.entries) {
            struct arm a = {.point = points[rnd(n)], .occurrence = 1 + rnd(3)};
            struct ir_eval_hooks h = {force_hook, NULL, &a};
            snprintf(name, sizeof(name), "random %u force P%u #%llu", seed, a.point,
                     (unsigned long long)a.occurrence);
            forced_runs += run_case(&r, v, ref, &snap, budget, &h, name).forced;
        }
        snprintf(name, sizeof(name), "random %u split", seed);
        run_case(&r, v, ref, &snap, rnd((unsigned)budget + 1), NULL, name);
        um_snapshot_free(&snap);
        um_region_free(&r);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    printf("unit_eval: %u random programs (%u memory ops, %u helpers, %u boundaries): %u compiled, "
           "%u entered, %u with NaT bits (%u rejected by G), %u forced exits\n",
           count, memory_ops, helpers, boundaries, compiled, entered, nat_programs, nat_rejected,
           forced_runs);
    UM_CHECK(compiled > count * 3 / 4 && entered > count / 2 && forced_runs > count / 4);
}

void unit_eval_test(const char *image);
void unit_eval_test(const char *image)
{
    um_machine_init();
    captures = calloc(MAX_CAPTURES, sizeof(*captures));
    UM_CHECK(captures != NULL);
    gather(image, GATHER_PHYS);
    gather(image, GATHER_ROT);
    gather(image, GATHER_ROTP);
    gather(image, GATHER_SIDE_EXIT);
    gather(image, GATHER_ALIAS63);
    gather(image, GATHER_VIRT_VRN1);
    gather(image, GATHER_VIRT_VRN2);
    shladd_xor(image);
    nested();
    natguard_counterexample();
    nat_post_increment();
    frame_rule_tests();
    random_programs(1000);
    printf("unit_eval: %u runs equal to interp_run; %u in-place captures (%u forced), %u region "
           "entries, exits exit/ckpt/site/helper %u/%u/%u/%u, %llu instructions by ir_eval\n",
           T.cases, T.captures, T.forced, T.entries, T.exits[0], T.exits[1], T.exits[2], T.exits[3],
           (unsigned long long)T.tier1_retired);
    free(captures);
}
