/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Runtime tests on generated code: checkpoint and mid-block exits, in-place
 * restoration, window rebinding, register pressure, verifier rejection,
 * instruction selection and static loop-code constraints.
 * Stacked-register tests reuse one compiled region across wrapped BOFs,
 * checking entry images, NaT guards, helper write-back and materialization
 * against physical homes. Frame-fill and guard tests cover every BOF 0..95
 * with random slots and states using bof.h's architectural mapping.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/um_t1.h"
#include "tests/tier1/hostwin_linux.h"
#include "engine/tier1/codegen.h"
#include "engine/tier1/form.h"
#include "engine/tier1/region.h"
#include "ia64vmm/hal.h"
#include "ia64/rse.h"
#include "engine/hostwin/hostwin.h"
#include "ia64vmm/guest_mem.h"
#include "tests/tier1/bof.h"

extern uint16_t t1_test_alloc_pool;

static unsigned checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        UM_CHECK(c);                                                                               \
        ++checks;                                                                                  \
    } while (0)

static uint64_t rng = 0x8badf00dcafef00du;
static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* ---- gather (frag_loops.S) set-up, as unit_codegen.c ---- */

struct gather_loop {
    struct vcpu *v, *ref;
    struct um_snapshot snap;
};

static void gather_open(struct gather_loop *L, const char *image)
{
    L->v = um_vcpu(UM_CODE);
    L->ref = um_vcpu(UM_CODE);
    struct vcpu *v = L->v;
    um_load_file(UM_CODE, image);
    unsigned n = 40;
    uint64_t index = UM_DATA, table = UM_DATA + 0x2000, out = UM_DATA + 0x4000;
    memset(um_ram(UM_DATA), 0, UM_DATA_SIZE);
    for (unsigned i = 0; i < 64; ++i) {
        uint64_t t = rnd64() & 0xfffff;
        memcpy(um_ram(table + 8 * i), &t, 8);
    }
    for (unsigned i = 0; i < n; ++i) {
        uint64_t x = (rnd64() & 3) ? 1 + rnd64() % 63 : 0;
        memcpy(um_ram(index + 8 * i), &x, 8);
    }
    code_notify_vmm_write(UM_DATA, UM_DATA_SIZE);
    v->s.rse_bof = 7;
    um_gr(v, 32, index, false);
    um_gr(v, 33, table, false);
    um_gr(v, 34, out, false);
    um_gr(v, 35, 0x80000, false);
    um_gr(v, 36, UINT64_MAX, false);
    um_gr(v, 14, 0, false);
    um_gr(v, 15, 0, false);
    v->s.ar[AR_LC] = n - 1;
    for (unsigned p = 6; p <= 20; ++p)
        um_pr(v, p, true);
    um_snapshot_take(&L->snap, v);
}

static void gather_close(struct gather_loop *L)
{
    um_snapshot_free(&L->snap);
    umt_release(L->v);
    vcpu_destroy(L->v);
    vcpu_destroy(L->ref);
}

static uint32_t point_at(uint32_t id, unsigned kind, unsigned nth)
{
    for (unsigned k = 0; k < t1_point_count(id); ++k) {
        struct t1_point_info pi;
        UM_CHECK(t1_point_get(id, k, &pi));
        if (pi.kind == kind && !nth--)
            return pi.point_id;
    }
    UM_CHECK(!"point not found");
    return UINT32_MAX;
}

/* ---- pinned-rax ---- */

static void kick_before(struct t1_vcpu *tv, struct vcpu *v, void *arg)
{
    (void)v;
    (void)arg;
    t1_kick(tv);
}

static bool requested_once;
static void request_hook(struct t1_vcpu *tv, uint32_t region, void *arg)
{
    (void)region;
    if (requested_once)
        return;
    requested_once = true;
    vcpu_request_exit(arg, VCPU_EXIT_DEBUG);
    (void)tv;
}
static void hook_before(struct t1_vcpu *tv, struct vcpu *v, void *arg)
{
    (void)tv;
    (void)arg;
    requested_once = false;
    t1_test_set_entry_hook(request_hook, v);
}

static void t_pinned_rax(const char *image)
{
    struct gather_loop L;
    gather_open(&L, image);
    struct t1_compile_report rep;
    um_snapshot_restore(&L.snap, L.v);
    uint16_t r14 = T1_ELEMENT_GR(14);
    uint32_t id = umt_compile(L.v, T1_COMPILE_VERIFY, r14, T1_RAX, &rep, "pinned-rax");
    CHECK(id);
    bool pinned = false;
    for (unsigned k = 0; k < rep.n_loop_carried; ++k)
        if (rep.loop_carried[k] == r14)
            pinned = rep.loop_carried_reg[k] == T1_RAX;
    CHECK(pinned);
    struct umt_run run;
    /* Kick exit at the first checkpoint, then on to the end in the same run. */
    struct umt_arm kick = {.point = UINT32_MAX, .before_run = kick_before};
    umt_case(L.v, L.ref, &L.snap, T1_COMPILE_VERIFY, 26 * 40 + 60, id, &kick, &run, false,
             "pinned-rax kick");
    unsigned ckpt = 0;
    for (unsigned k = 0; k < run.run.ncaptures; ++k)
        ckpt += run.run.captures[k].point == point_at(id, T1_PT_CHECKPOINT, 0);
    CHECK(ckpt >= 1);
    /* requested_exit seen at the checkpoint: the run returns DEBUG there
     * (the interpreter consumes the request) and continues. */
    struct umt_arm req = {.point = UINT32_MAX, .before_run = hook_before, .resume_requested = true};
    umt_case(L.v, L.ref, &L.snap, T1_COMPILE_VERIFY, 26 * 40 + 60, id, &req, &run, false,
             "pinned-rax requested exit");
    t1_test_set_entry_hook(NULL, NULL);
    CHECK(run.resumed == 1);
    printf(
        "unit_runtime: pinned-rax: r14 pinned to rax across kick and requested-exit checkpoint exits\n");
    gather_close(&L);
}

/* ---- wrong-old-value ---- */

static void t_wrong_old_value(const char *image)
{
    struct gather_loop L;
    gather_open(&L, image);
    um_snapshot_restore(&L.snap, L.v);
    uint32_t id = umt_compile(L.v, T1_COMPILE_VERIFY, 0, 0, NULL, "wrong-old-value");
    CHECK(id);
    uint32_t s2 = point_at(id, T1_PT_SITE, 1);
    struct umt_run run;
    struct umt_arm arm = {.point = s2,
                          .occurrence = 2,
                          .action = T1_FORCE_TRAP_EXIT,
                          .corrupt = true,
                          .force_too = true,
                          .cpoint = s2,
                          .coccurrence = 2,
                          .element = T1_ELEMENT_GR(18),
                          .mask = 0x40};
    unsigned d = umt_case(L.v, L.ref, &L.snap, T1_COMPILE_VERIFY, 26 * 40 + 60, id, &arm, &run,
                          true, "wrong-old-value in place");
    CHECK(d > 0 && run.forced == 1);
    arm.final_only = true;
    d = umt_case(L.v, L.ref, &L.snap, T1_COMPILE_VERIFY, 26 * 40 + 60, id, &arm, &run, false,
                 "wrong-old-value end");
    CHECK(d == 0);
    printf(
        "unit_runtime: wrong-old-value: a wrong old r18 at S2 fails in place, the end state is equal\n");
    gather_close(&L);
}

/* ---- mid-block-exit ---- */

#define PROG (UM_CODE + 0x3000)

static void t_mid_block_exit(void)
{
    /* adds r20 ; adds r21 ;; ld8 r22 = [r33] ;; add r23 = r23, r22 ; br.cloop;
     * the break ends formation's lookahead past the loop
     * before code earlier tests left in memory. */
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MII), {ienc_adds(0, 20, 1, 20), ienc_adds(0, 21, 2, 21), ienc_nop_i()}},
        {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 22, 33), ienc_nop_m(), ienc_nop_i()}},
        {IENC_STOP(IENC_MIB),
         {ienc_nop_m(), ienc_alu(0, IENC_ADD, 23, 23, 22), ienc_br_cloop(-2 * 16)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
        {IENC_STOP(IENC_MII), {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}},
    };
    um_emit(PROG, b, 5);
    struct vcpu *v = um_vcpu(PROG), *ref = um_vcpu(PROG);
    um_gr(v, 33, UM_DATA + 0x800, false);
    uint64_t x = 0x1234;
    memcpy(um_ram(UM_DATA + 0x800), &x, 8);
    v->s.ar[AR_LC] = 20;
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    struct t1_compile_report rep;
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "mid-block-exit");
    CHECK(id);
    uint32_t site = point_at(id, T1_PT_SITE, 0);
    struct t1_point_info pi;
    for (unsigned k = 0; k < t1_point_count(id); ++k)
        if (t1_point_get(id, k, &pi) && pi.point_id == site)
            break;
    struct umt_run run;
    for (uint64_t budget = 20; budget <= 60; budget += 7) {
        struct umt_arm arm = {.point = site, .occurrence = 2, .action = T1_FORCE_TRAP_EXIT};
        uint64_t before = snap.v.retired;
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, budget, id, &arm, &run, false, "mid-block-exit");
        CHECK(run.forced == 1 && v->retired - before == budget);
        struct t1_exit_info xi;
        bool found = false;
        for (unsigned k = 0; k < t1_exit_count(umt_tv(v)); ++k)
            if (t1_exit_get(umt_tv(v), k, &xi) && xi.point_id == site && xi.kind == T1_PT_SITE)
                found = xi.lower_tier_step_followed;
        CHECK(found);
    }
    printf(
        "unit_runtime: mid-block-exit: site exits after completed pure instructions continue in the lower "
        "tier, retired == budget\n");
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

/* ---- window-tags ---- */

#define TAGS 9u

static void t_window_tags(void)
{
    static uint8_t *piece[TAGS];
    unsigned w = hostwin_bits(umt_hostwin());
    for (unsigned k = 0; k < TAGS; ++k) {
        if (piece[k])
            continue;
        gpa_t base = (gpa_t)(k + 1) << w;
        piece[k] = hostwin_linux_ram(base, 4096);
        CHECK(piece[k] != NULL);
        CHECK(gm_add_region(&(struct gm_region){
            .base = base, .size = 4096, .kind = GM_RAM, .hva = piece[k], .name = "window-tags"}));
    }
    /* ld8 r16 = [r32], 8 ;; ld8 r17 = [r16] ;; add r14 = r14, r17 ; br.cloop */
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MMI),
         {ienc_ld_inc_imm(0, 8, false, 16, 32, 8), ienc_nop_m(), ienc_nop_i()}},
        {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 17, 16), ienc_nop_m(), ienc_nop_i()}},
        {IENC_STOP(IENC_MIB),
         {ienc_nop_m(), ienc_alu(0, IENC_ADD, 14, 14, 17), ienc_br_cloop(-2 * 16)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
    };
    um_emit(PROG + 0x100, b, 4);
    unsigned n = 4 * TAGS;
    for (unsigned i = 0; i < n; ++i) {
        unsigned k = i % TAGS;
        uint64_t ptr = ((uint64_t)(k + 1) << w) | (uint64_t)(8 * (i / TAGS));
        memcpy(um_ram(UM_DATA + 8 * i), &ptr, 8);
        uint64_t val = rnd64();
        memcpy(piece[k] + 8 * (i / TAGS), &val, 8);
    }
    code_notify_vmm_write(UM_DATA, 8 * n);
    struct vcpu *v = um_vcpu(PROG + 0x100), *ref = um_vcpu(PROG + 0x100);
    um_gr(v, 32, UM_DATA, false);
    v->s.ar[AR_LC] = n - 1;
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "window-tags");
    CHECK(id);
    struct hostwin_stats h0, h1;
    hostwin_get_stats(umt_hostwin(), &h0);
    struct umt_run run;
    umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 3 * n + 10, id, NULL, &run, false, "window-tags");
    hostwin_get_stats(umt_hostwin(), &h1);
    /* TAGS + 1 tags on HOSTWIN_MAX_SLOTS slots: at least the surplus is rebound. */
    CHECK(h1.rebinds - h0.rebinds >= TAGS + 1 - HOSTWIN_MAX_SLOTS && run.nexits >= 1);
    /* Every iteration ran natively except the region's own exits. */
    CHECK(run.nexits < n / 2);
    printf(
        "unit_runtime: window-tags: %u tags + the pointer list's, %u loads each: %llu binds (%llu rebinds), "
        "%llu fills, %u misses retried, %u region exits, states equal\n",
        TAGS, n, (unsigned long long)(h1.binds - h0.binds),
        (unsigned long long)(h1.rebinds - h0.rebinds), (unsigned long long)(h1.fills - h0.fills),
        run.retries, run.nexits);
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

/* ---- spill pressure, raverify at both RIPs ---- */

static void spill_pressure(const char *image)
{
    struct gather_loop L;
    gather_open(&L, image);
    um_snapshot_restore(&L.snap, L.v);
    t1_test_alloc_pool = T1_X86_REGBIT(T1_RAX) | T1_X86_REGBIT(T1_RCX) | T1_X86_REGBIT(T1_RDX) |
                         T1_X86_REGBIT(T1_RBX) | T1_X86_REGBIT(T1_RSI);
    struct t1_compile_report rep;
    uint32_t id = umt_compile(L.v, T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG, T1_ELEMENT_GR(14),
                              T1_RBX, &rep, "spill pressure");
    t1_test_alloc_pool = 0;
    CHECK(id);
    CHECK(rep.loop_value_spills + rep.restore_only_spills > 0);
    unsigned cases = 0;
    struct umt_run run;
    for (unsigned k = 0; k < t1_point_count(id); ++k) {
        struct t1_point_info pi;
        CHECK(t1_point_get(id, k, &pi));
        if (pi.kind != T1_PT_SITE)
            continue;
        static const enum t1_force_action acts[] = {T1_FORCE_TRAP_EXIT, T1_FORCE_MISS_EXIT,
                                                    T1_FORCE_TRAP_RETRY};
        for (unsigned a = 0; a < 3; ++a)
            for (unsigned occ = 1; occ <= 3; ++occ) {
                struct umt_arm arm = {.point = pi.point_id, .occurrence = occ, .action = acts[a]};
                umt_case(L.v, L.ref, &L.snap, T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG,
                         26 * 40 + 60, id, &arm, &run, false, "spill pressure");
                ++cases;
            }
    }
    printf(
        "unit_runtime: spill pressure: gather on 5 registers: %u loop-value and %u restoration-only spills, "
        "%u forced cases equal\n",
        rep.loop_value_spills, rep.restore_only_spills, cases);
    gather_close(&L);
}

/* ---- the internal pipeline ---- */

struct pipeline {
    struct t1_form *form;
    struct t1_ir *ir;
    struct t1_mir *m;
};

static void pipeline(struct vcpu *v, uint32_t flags, struct pipeline *p)
{
    p->form = calloc(1, sizeof(*p->form));
    p->ir = calloc(1, sizeof(*p->ir));
    UM_CHECK(p->form && p->ir);
    const char *reason;
    unsigned fail;
    char err[200];
    UM_CHECK(t1_form(v, flags, T1_MAX_INSNS, p->form, &reason) == T1_COMPILED);
    UM_CHECK(t1_lift(p->form, p->ir, &fail));
    UM_CHECK(t1_optimize(p->ir, true, err, sizeof(err)));
    p->m = t1_mir_create(p->ir);
    UM_CHECK(p->m != NULL);
    p->m->window_bits = 40;
}

static void pipeline_free(struct pipeline *p)
{
    if (p->m)
        t1_mir_destroy(p->m);
    free(p->form);
    free(p->ir);
}

/* SDM Vol.3 br, Operation (cloop): taken iff LC != 0, decrement only
 * when taken. Duplicate the pure CLOOP for separate projections and add
 * an identity use of @taken to force the fallback rules on this decoded
 * branch. Pressure tests depend on split choices; the prover's
 * fresh upper bits cover every allocation of a pure byte definition. */
static void cloop_pressure(void)
{
    struct ienc_bundle b[8];
    for (unsigned k = 0; k < 6; ++k)
        b[k] = (struct ienc_bundle){
            IENC_STOP(IENC_MII),
            {ienc_nop_m(), ienc_alu_imm8(0, IENC_XOR, 12 + 2 * k, 12 + 2 * k, 12 + 2 * k),
             ienc_alu_imm8(0, IENC_XOR, 13 + 2 * k, 13 + 2 * k, 13 + 2 * k)}};
    b[6] = (struct ienc_bundle){IENC_STOP(IENC_MIB),
                                {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(0x1000 - 6 * 16)}};
    b[7] = (struct ienc_bundle){IENC_STOP(IENC_MIB),
                                {ienc_break_m(0x1ffff), ienc_nop_i(), ienc_nop_b()}};
    struct vcpu *v = um_vcpu(UM_CODE);
    um_emit(UM_CODE, b, 8);
    struct pipeline p;
    pipeline(v, 0, &p);
    unsigned branches = 0;
    for (unsigned o = 0; o < p.ir->nops; ++o) {
        struct t1_op *op = &p.ir->op[o];
        if (op->code != T1_IR_BRANCH || (op->flags & T1_OPF_DEAD))
            continue;
        t1_value cond = p.ir->use[op->use_first].value;
        CHECK(p.ir->op[T1_VALUE_OP(cond)].code == T1_IR_CLOOP);
        const struct t1_op *cl = &p.ir->op[T1_VALUE_OP(cond)];
        t1_value lc = p.ir->use[cl->use_first].value;
        uint32_t next = t1_ir_append(p.ir, op->block, T1_IR_CLOOP, 0, 0, &lc, 1, cl->insn);
        CHECK(next != T1_NO_OP);
        t1_value old = T1_VALUE(T1_VALUE_OP(cond), T1_RES_CLOOP_next);
        for (unsigned k = 0; k < p.ir->nuses; ++k)
            if (p.ir->use[k].value == old)
                p.ir->use[k].value = T1_VALUE(next, T1_RES_CLOOP_next);
        t1_value args[2] = {cond, cond};
        uint32_t same = t1_ir_append(p.ir, op->block, T1_IR_OR1, 0, 0, args, 2, op->insn);
        CHECK(same != T1_NO_OP);
        p.ir->use[p.ir->op[o].use_first].value = T1_VALUE(same, 0);
        ++branches;
    }
    char err[200];
    CHECK(branches == 1 && t1_ir_verify(p.ir, 0, err, sizeof(err)));
    p.m->allocatable = T1_X86_REGBIT(T1_RAX) | T1_X86_REGBIT(T1_RCX) | T1_X86_REGBIT(T1_RDX) |
                       T1_X86_REGBIT(T1_RBX) | T1_X86_REGBIT(T1_RSI);
    if (!t1_isel(p.m, 0))
        um_fail(__FILE__, __LINE__, p.m->fail_reason);
    unsigned sets = 0;
    for (unsigned i = 0; i < p.m->ninsns; ++i)
        sets += p.m->insn[i].op == T1_X86_SET8 + T1_CC_NE;
    CHECK(sets == 1); /* cloop_taken, not the fused cloop branch */
    CHECK(t1_regalloc(p.m) && t1_raverify(p.m, err, sizeof(err)));
    CHECK(p.m->total_spills > 0);
    struct t1_emitted e;
    CHECK(t1_emit(p.m, &e, err, sizeof(err)) && !e.nframe_in);
    size_t pages = (e.size + 4095) / 4096;
    uint8_t *code = t1_arena_alloc(pages);
    CHECK(code != NULL);
    t1_emit_place(&e, code);
    hal_code_set_executable(code, pages);
    static struct t1_exec xc;
    const uint64_t counts[] = {0, 1, 2, UINT64_MAX};
    for (unsigned n = 0; n < sizeof(counts) / sizeof(counts[0]); ++n) {
        memset(&xc, 0, sizeof(xc));
        v->s.ar[AR_LC] = counts[n];
        for (unsigned k = 12; k < 24; ++k)
            um_gr(v, k, UINT64_C(0xfedcba9876543200) + k, false);
        t1_entry_fn fn = (t1_entry_fn)(uintptr_t)code;
        CHECK(fn(v, 99, &xc) == T1_RET_POINT && xc.point < p.ir->npoints);
        const struct t1_ir_point *pt = &p.ir->point[xc.point];
        CHECK(pt->ip == UM_CODE + (counts[n] ? 0x1000 : 7 * 16) && pt->slot == 0);
        const struct t1_mir_point *mp = &p.m->point[xc.point];
        unsigned lc = 0, gr = 0;
        for (unsigned k = mp->frame_first; k < mp->frame_first + mp->frame_count; ++k) {
            uint16_t el = p.m->frame[k].element;
            if (el == T1_ELEMENT_LC) {
                CHECK(t1_loc_value(&xc, &p.m->frame_loc[k]) == (counts[n] ? counts[n] - 1 : 0));
                ++lc;
            } else if (el >= T1_ELEMENT_GR(12) && el < T1_ELEMENT_GR(24)) {
                CHECK(t1_loc_value(&xc, &p.m->frame_loc[k]) ==
                      ((UINT64_C(0xfedcba9876543200) + el) ^ el));
                ++gr;
            }
        }
        CHECK(lc == 1 && gr == 12);
    }
    t1_arena_free(code, pages);
    t1_emit_free(&e);
    pipeline_free(&p);
    vcpu_destroy(v);
    printf("unit_runtime: cloop_taken under pressure: LC 0/1/2/max, branch and next LC correct\n");
}

static void raverify_negative(const char *image)
{
    struct gather_loop L;
    gather_open(&L, image);
    um_snapshot_restore(&L.snap, L.v);
    struct pipeline p;
    pipeline(L.v, T1_COMPILE_VERIFY, &p);
    char err[200];
    CHECK(t1_isel(p.m, T1_COMPILE_VERIFY) && t1_regalloc(p.m) &&
          t1_raverify(p.m, err, sizeof(err)));
    /* A frame entry naming a register that does not hold its value. */
    unsigned bad = 0;
    for (unsigned k = 0; k < p.m->nframe && !bad; ++k) {
        struct t1_loc *l = &p.m->frame_loc[k];
        if (p.m->frame[k].kind != T1_MF_VREG || l->kind != T1_LOC_REG)
            continue;
        uint8_t saved = l->reg;
        l->reg = saved == T1_RAX ? T1_RCX : T1_RAX;
        CHECK(!t1_raverify(p.m, err, sizeof(err)) && strstr(err, "frame"));
        l->reg = saved;
        ++bad;
    }
    CHECK(bad && t1_raverify(p.m, err, sizeof(err)));
    /* An operand in the wrong register. */
    bad = 0;
    for (uint32_t i = 0; i < p.m->ninsns && !bad; ++i) {
        struct t1_mir_insn *in = &p.m->insn[i];
        if (in->op != T1_X86_LEA64 && in->op != T1_X86_CMP64_RR)
            continue;
        for (unsigned k = 0; k < in->nopnd && !bad; ++k) {
            if (in->opnd[k].kind != T1_MO_VREG)
                continue;
            uint8_t saved = in->opnd[k].fixed;
            in->opnd[k].fixed = saved == T1_RAX ? T1_RCX : T1_RAX;
            bool ok = t1_raverify(p.m, err, sizeof(err));
            in->opnd[k].fixed = saved;
            if (in->op == T1_X86_CMP64_RR || k > 0) {
                CHECK(!ok);
                ++bad;
            }
        }
    }
    CHECK(bad && t1_raverify(p.m, err, sizeof(err)));
    /* A FORCE whose frame differs from its site's: the two RIPs. */
    bad = 0;
    for (unsigned q = 0; q < p.ir->npoints && !bad; ++q) {
        const struct t1_mir_point *mp = &p.m->point[q];
        if (mp->kind != T1_PT_SITE || mp->force_insn == T1_MIR_NONE)
            continue;
        struct t1_mir_insn *f = &p.m->insn[mp->force_insn];
        for (unsigned k = 0; k < 2; ++k)
            if (f->opnd[k].kind == T1_MO_VREG) {
                uint8_t saved = f->opnd[k].fixed;
                f->opnd[k].fixed = saved == T1_RDX ? T1_RSI : T1_RDX;
                CHECK(!t1_raverify(p.m, err, sizeof(err)));
                f->opnd[k].fixed = saved;
                ++bad;
                break;
            }
    }
    CHECK(bad);
    pipeline_free(&p);
    printf(
        "unit_runtime: raverify rejects a wrong frame location, operand register and FORCE operand\n");
    gather_close(&L);
}

/* ---- isel ---- */

static unsigned count_op(const struct t1_mir *m, unsigned b, uint16_t op)
{
    unsigned n = 0;
    for (uint32_t i = m->block[b].first; i < m->block[b].first + m->block[b].count; ++i)
        n += m->insn[i].op == op;
    return n;
}

/* Rule format 2 in isel (rules.h): a field extract inside the word
 * (EXTRU pos 4 len 8: two shifts by derived counts, relation pos + len <=
 * 63), one that runs to bit 63 (EXTRU pos 40 len 30: one shift) and an
 * immediate add (lea with the CONST as displacement). The values reach an
 * exit frame, so each is selected on its own. */
static void isel_one(uint16_t code, int64_t imm, bool add_const, const uint16_t *want,
                     const unsigned *count, unsigned nwant)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    UM_CHECK(ir != NULL);
    t1_ir_init(ir, NULL);
    t1_elem_add(ir->frame_set, T1_ELEMENT_GR(22));
    for (unsigned b = 0; b < 3; ++b)
        t1_ir_new_block(ir, 0);
    t1_value a = t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(20));
    t1_value opnd[2] = {a, add_const ? t1_ir_const(ir, T1_I64, (uint64_t)imm) : 0};
    uint32_t op = t1_ir_append(ir, 1, code, 0, add_const ? 0 : imm, opnd, add_const ? 2 : 1, 0);
    UM_CHECK(op != T1_NO_OP);
    UM_CHECK(t1_ir_set_jump(ir, 0, 1));
    UM_CHECK(t1_ir_set_jump(ir, 1, 2));
    uint32_t p = t1_ir_new_point(ir, T1_PT_EXIT, UM_CODE, 0, 0, 2, 0);
    uint16_t el = T1_ELEMENT_GR(22);
    t1_value fv = T1_VALUE(op, 0);
    UM_CHECK(t1_ir_set_frame(ir, p, &el, &fv, NULL, 1));
    UM_CHECK(t1_ir_set_exit(ir, 2, p, 0));
    char err[200];
    if (!t1_ir_verify(ir, 0, err, sizeof(err)))
        fprintf(stderr, "isel IR: %s\n", err);
    UM_CHECK(t1_ir_verify(ir, 0, err, sizeof(err)));
    struct t1_mir *m = t1_mir_create(ir);
    UM_CHECK(m && t1_isel(m, 0));
    unsigned total = 0;
    for (unsigned b = 0; b < m->nblocks; ++b)
        for (unsigned k = 0; k < nwant; ++k)
            total += count_op(m, b, want[k]) == count[k];
    CHECK(total >= nwant);
    for (unsigned k = 0; k < nwant; ++k) {
        unsigned n = 0;
        for (unsigned b = 0; b < m->nblocks; ++b)
            n += count_op(m, b, want[k]);
        CHECK(n == count[k]);
    }
    t1_mir_destroy(m);
    free(ir);
}

static void isel_format2(void)
{
    /* extr.u inside the word: shl by 64 - 4 - 8, shr by 64 - 8; no CL sequence. */
    static const uint16_t field[] = {T1_X86_SHL64_RI, T1_X86_SHR64_RI, T1_X86_SHR64_RCL,
                                     T1_X86_SHL64_RCL, T1_X86_BEXTR64};
    bool bmi1 = (hal_host_isa() & HOST_ISA_BMI1) != 0;
    const unsigned field_n[] = {!bmi1, !bmi1, 0, 0, bmi1};
    isel_one(T1_IR_EXTRU, T1_FIELD_IMM(4, 8), false, field, field_n, 5);
    /* shr.u by 40 (a field running to bit 63): one shift. */
    static const unsigned top_n[] = {0, 1, 0, 0, 0};
    isel_one(T1_IR_EXTRU, T1_FIELD_IMM(40, 24), false, field, top_n, 5);
    /* adds 8: lea with the constant as displacement, no tied add. */
    static const uint16_t add[] = {T1_X86_LEA64, T1_X86_ADD64_RI};
    static const unsigned add_n[] = {1, 0};
    isel_one(T1_IR_ADD, 8, true, add, add_n, 2);
}

/* An IR by hand: entry (LIVEINs) -> header: CMP eq (r20, x) -> BRANCH to
 * two exits. frame_use puts the compare into the exits' frames (as element
 * p6), making it a multi-use value. */
static void isel_branch(bool constant, bool frame_use)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    UM_CHECK(ir != NULL);
    t1_ir_init(ir, NULL);
    if (frame_use)
        t1_elem_add(ir->frame_set, T1_ELEMENT_PR(6));
    for (unsigned b = 0; b < 4; ++b)
        t1_ir_new_block(ir, 0);
    t1_value a = t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(20));
    t1_value bv =
        constant ? t1_ir_const(ir, T1_I64, 0) : t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(21));
    t1_value opnd[2] = {a, bv};
    uint32_t cmp = t1_ir_append(ir, 1, T1_IR_CMP, 0, T1_CMP_IMM(T1_CMP_EQ, 64), opnd, 2, 0);
    UM_CHECK(cmp != T1_NO_OP);
    UM_CHECK(t1_ir_set_jump(ir, 0, 1));
    UM_CHECK(t1_ir_set_branch(ir, 1, T1_VALUE(cmp, 0), 2, 3, 0));
    for (unsigned k = 0; k < 2; ++k) {
        uint32_t p = t1_ir_new_point(ir, T1_PT_EXIT, UM_CODE + 16 * k, 0, 0, (uint16_t)(2 + k), 0);
        uint16_t el = T1_ELEMENT_PR(6);
        t1_value fv = T1_VALUE(cmp, 0);
        UM_CHECK(t1_ir_set_frame(ir, p, &el, &fv, NULL, frame_use ? 1 : 0));
        UM_CHECK(t1_ir_set_exit(ir, (uint16_t)(2 + k), p, 0));
    }
    char err[200];
    if (!t1_ir_verify(ir, 0, err, sizeof(err)))
        fprintf(stderr, "isel IR: %s\n", err);
    UM_CHECK(t1_ir_verify(ir, 0, err, sizeof(err)));
    struct t1_mir *m = t1_mir_create(ir);
    UM_CHECK(m && t1_isel(m, 0));
    unsigned header = m->header;
    unsigned jcc = 0;
    for (unsigned cc = 0; cc < T1_CC_COUNT; ++cc)
        jcc += count_op(m, header, (uint16_t)(T1_X86_JCC + cc));
    if (!frame_use) {
        /* Fused: one compare, one jcc, no setcc. Against the constant 0
         * the fused zero test (rule cmp_eq_zero_branch, test r, r) wins:
         * the constant needs no register at all. */
        CHECK(count_op(m, header, constant ? T1_X86_TEST64_RR : T1_X86_CMP64_RR) == 1 && jcc == 1);
        CHECK(count_op(m, header, T1_X86_SET8 + T1_CC_E) == 0);
        if (constant)
            CHECK(count_op(m, header, T1_X86_ZERO32) + count_op(m, header, T1_X86_MOV32_RI32) == 0);
    } else {
        /* Multi-use: the compare value is computed (immediate form for the
         * constant), then the generic branch on it. */
        CHECK(count_op(m, header, T1_X86_SET8 + T1_CC_E) == 1);
        CHECK(count_op(m, header, constant ? T1_X86_CMP64_RI : T1_X86_CMP64_RR) == 1);
        CHECK(count_op(m, header, T1_X86_TEST64_RR) == 1 && jcc == 1);
    }
    t1_mir_destroy(m);
    free(ir);
}

/* CLOOP whose taken result feeds something that is not a BRANCH: no rule
 * covers CLOOP alone, so isel names the instruction to end the region at. */
static void isel_uncovered(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    UM_CHECK(ir != NULL);
    t1_ir_init(ir, NULL);
    t1_elem_add(ir->frame_set, T1_ELEMENT_LC);
    t1_elem_add(ir->frame_set, T1_ELEMENT_PR(6));
    for (unsigned b = 0; b < 3; ++b)
        t1_ir_new_block(ir, 0);
    t1_value lc = t1_ir_livein(ir, T1_I64, T1_ELEMENT_LC);
    uint32_t cl = t1_ir_append(ir, 1, T1_IR_CLOOP, 0, 0, &lc, 1, 5);
    UM_CHECK(cl != T1_NO_OP);
    UM_CHECK(t1_ir_set_jump(ir, 0, 1));
    UM_CHECK(t1_ir_set_jump(ir, 1, 2));
    uint32_t p = t1_ir_new_point(ir, T1_PT_EXIT, UM_CODE, 0, 5, 2, 0);
    uint16_t el[2] = {T1_ELEMENT_LC, T1_ELEMENT_PR(6)};
    t1_value fv[2] = {T1_VALUE(cl, T1_RES_CLOOP_next), T1_VALUE(cl, T1_RES_CLOOP_taken)};
    UM_CHECK(t1_ir_set_frame(ir, p, el, fv, NULL, 2));
    UM_CHECK(t1_ir_set_exit(ir, 2, p, 5));
    struct t1_mir *m = t1_mir_create(ir);
    UM_CHECK(m);
    CHECK(!t1_isel(m, 0) && m->fail_insn == 5);
    t1_mir_destroy(m);
    free(ir);
}

static void isel_tests(void)
{
    isel_branch(false, false);
    isel_branch(true, false);
    isel_branch(false, true);
    isel_branch(true, true);
    isel_uncovered();
    isel_format2();
    printf(
        "unit_runtime: isel: fused compare-and-branch, constant immediates, uncovered op ends the region, "
        "format-2 rules (derived shift counts, relation checks, lea displacement from a binder)\n");
}

static unsigned no_stacked_home_code(const uint8_t *code, size_t size, const char *label);
static unsigned no_stacked_home(uint32_t id, const char *label);

/* ---- emit: parallel copies with cycles, LIVEIN homes, exit stub ---- */

/* Region-shaped code by hand: LIVEINs into fixed registers, one parallel
 * copy holding a register 3-cycle, a slot swap, a register/slot swap and
 * constants, then an exit. Placed in the arena and entered like a region:
 * the exit frame captures the observed registers in x->regs. */
static void emit_pcopy(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    UM_CHECK(ir != NULL);
    t1_ir_init(ir, NULL);
    t1_ir_new_block(ir, 0);
    uint32_t p = t1_ir_new_point(ir, T1_PT_EXIT, UM_CODE, 0, 0, 0, 0);
    UM_CHECK(p != T1_NO_POINT && t1_ir_set_frame(ir, p, NULL, NULL, NULL, 0));
    ir->point[p].frame_count = ir->nuses = 10;
    struct t1_mir *m = t1_mir_create(ir);
    UM_CHECK(m != NULL);
    m->nblocks = 1;
    /* Stacked LIVEINs read the entry image: GR(37) twice (one
     * slot), NaT(41) as one byte, GR(127) (frame offset 95). */
    static const struct {
        uint8_t reg;
        uint16_t element;
    } in[] = {
        {T1_RAX, T1_ELEMENT_GR(20)}, {T1_RCX, T1_ELEMENT_GR(21)},  {T1_RDX, T1_ELEMENT_GR(22)},
        {T1_RBX, T1_ELEMENT_PR(37)}, {T1_RSI, T1_ELEMENT_NAT(23)}, {T1_RDI, T1_ELEMENT_LC},
        {T1_R8, T1_ELEMENT_GR(37)},  {T1_R9, T1_ELEMENT_NAT(41)},  {T1_R10, T1_ELEMENT_GR(127)},
        {T1_R15, T1_ELEMENT_GR(37)},
    };
    m->nframe = m->point[p].frame_count = sizeof(in) / sizeof(in[0]);
    for (unsigned k = 0; k < m->nframe; ++k) {
        m->frame[k] = (struct t1_mir_frame){T1_ELEMENT_GR(1 + k), T1_MF_VREG, k, 0};
        m->frame_loc[k] = (struct t1_loc){.kind = T1_LOC_REG, .reg = in[k].reg};
    }
    for (unsigned k = 0; k < sizeof(in) / sizeof(in[0]); ++k) {
        struct t1_mir_insn *i = t1_mir_append(m, T1_MIR_LIVEIN);
        i->nopnd = 2;
        i->opnd[0] = (struct t1_mir_opnd){T1_MO_VREG, in[k].reg, k, 0};
        i->opnd[1] = (struct t1_mir_opnd){T1_MO_IMM, T1_X86_NOREG, T1_VREG_NONE, in[k].element};
    }
    struct t1_loc R[6], S[4];
    for (unsigned k = 0; k < 6; ++k)
        R[k] = (struct t1_loc){T1_LOC_REG, in[k].reg, 0, 0};
    for (unsigned k = 0; k < 4; ++k)
        S[k] = (struct t1_loc){T1_LOC_SPILL, 0, (uint16_t)k, 0};
    struct {
        struct t1_loc d, s;
    } cp[] = {
        {R[0], R[1]},
        {R[1], R[2]},
        {R[2], R[0]}, /* rax <- rcx <- rdx <- rax */
        {S[1], S[2]},
        {S[2], S[1]}, /* slot swap */
        {R[3], S[3]},
        {S[3], R[3]}, /* rbx <-> slot 3 */
        {S[0], R[0]}, /* old rax to slot 0 */
        {R[4], {T1_LOC_CONST, 0, 0, UINT64_C(0xfedcba9876543210)}},
        {R[5], {T1_LOC_CONST, 0, 0, UINT64_C(0xffffffff)}},
    };
    unsigned nc = sizeof(cp) / sizeof(cp[0]);
    uint32_t first = t1_mir_new_copies(m, nc);
    for (unsigned k = 0; k < nc; ++k) {
        m->copy[first + k].dloc = cp[k].d;
        m->copy[first + k].sloc = cp[k].s;
        m->copy[first + k].dst = m->copy[first + k].src = 0;
    }
    struct t1_mir_insn *pc = t1_mir_append(m, T1_MIR_PCOPY);
    pc->copies_first = first;
    pc->ncopies = nc;
    m->block[0].term = m->ninsns;
    struct t1_mir_insn *x = t1_mir_append(m, T1_MIR_EXIT);
    x->point = p;
    m->point[p].insn = m->ninsns - 1;
    m->block[0].first = 0;
    m->block[0].count = m->ninsns;
    struct t1_emitted e;
    char err[200];
    CHECK(t1_emit(m, &e, err, sizeof(err)));
    CHECK(e.nframe_in == 3 && e.frame_in[0] == T1_ELEMENT_GR(37) &&
          e.frame_in[1] == T1_ELEMENT_NAT(41) && e.frame_in[2] == T1_ELEMENT_GR(127));
    CHECK(no_stacked_home_code(e.buf, e.size, "emit") == 4);
    size_t pages = (e.size + 4095) / 4096;
    uint8_t *code = t1_arena_alloc(pages);
    CHECK(code != NULL);
    t1_emit_place(&e, code);
    hal_code_set_executable(code, pages);
    struct vcpu *v = um_vcpu(UM_CODE);
    for (unsigned k = 20; k <= 22; ++k)
        um_gr(v, k, 0x1000 + k, false);
    um_pr(v, 37, true);
    v->s.nat[ia64_gr_index(&v->s, 23)] = true;
    v->s.ar[AR_LC] = 77;
    static struct t1_exec xc;
    memset(&xc, 0, sizeof(xc));
    xc.spill[1] = 0x111;
    xc.spill[2] = 0x222;
    xc.spill[3] = 0x333;
    xc.frame_in[0] = UINT64_C(0x3737373737373737);
    xc.frame_in[1] = UINT64_C(0xffffffffffffff01); /* a NaT slot: the low byte only */
    xc.frame_in[2] = UINT64_C(0x1270000000000127);
    t1_entry_fn fn = (t1_entry_fn)(uintptr_t)code;
    CHECK(fn(v, 99, &xc) == T1_RET_POINT && xc.point == p && xc.raw_r13 == 99);
    CHECK(xc.regs[T1_R8] == xc.frame_in[0] && xc.regs[T1_R9] == 1 &&
          xc.regs[T1_R10] == xc.frame_in[2] && xc.regs[T1_R15] == xc.frame_in[0]);
    uint64_t g20 = v->s.gr[ia64_gr_index(&v->s, 20)], g21 = v->s.gr[ia64_gr_index(&v->s, 21)];
    uint64_t g22 = v->s.gr[ia64_gr_index(&v->s, 22)];
    CHECK(xc.regs[T1_RAX] == g21 && xc.regs[T1_RCX] == g22 && xc.regs[T1_RDX] == g20);
    CHECK(xc.spill[0] == g20 && xc.spill[1] == 0x222 && xc.spill[2] == 0x111);
    CHECK(xc.regs[T1_RBX] == 0x333 && xc.spill[3] == 1); /* p37 = 1 */
    CHECK(xc.regs[T1_RSI] == UINT64_C(0xfedcba9876543210) && xc.regs[T1_RDI] == 0xffffffff);
    t1_arena_free(code, pages);
    t1_emit_free(&e);
    t1_mir_destroy(m);
    free(ir);
    vcpu_destroy(v);
    printf(
        "unit_runtime: emit: parallel copy with register and slot cycles, LIVEIN homes, stacked LIVEINs from "
        "the entry image (one slot per element, NaT as a byte), exit stub\n");
}

/* ---- static checks on production regions ---- */

static void static_checks(const char *image)
{
    struct gather_loop L;
    gather_open(&L, image);
    for (unsigned prog = 0; prog < 2; ++prog) {
        struct vcpu *v = L.v;
        um_snapshot_restore(&L.snap, v);
        if (prog == 1) { /* shladd_xor */
            v->s.ip = UM_CODE + 0x400;
            v->s.slot = 0;
            um_gr(v, 16, 5, false);
            um_gr(v, 17, 0x1234, false);
            um_gr(v, 18, 0xfeed, false);
            v->s.ar[AR_LC] = 50;
        }
        struct t1_compile_report rep;
        uint32_t id =
            umt_compile(v, 0, 0, 0, &rep, prog ? "shladd_xor production" : "gather production");
        CHECK(id);
        const uint8_t *code;
        size_t size, ls, le;
        CHECK(t1_test_region_code(id, &code, &size, &ls, &le) && ls < le && le <= size);
        unsigned insns = 0;
        for (size_t at = ls; at < le;) {
            struct t1_x86_decoded d;
            CHECK(t1_x86_decode(code + at, le - at, &d));
            CHECK(!(t1_x86_info(d.op)->attr & T1_XA_CALL));
            if (d.has_mem &&
                d.mem.base == T1_R12) /* the checkpoint's requested_exit/irq_kick word */
                CHECK(d.op == T1_X86_LOAD64 && d.mem.disp == (int32_t)T1_VCPU_CKPT);
            at += d.length;
            ++insns;
        }
        CHECK(insns == rep.loop_host_insns);
        CHECK(rep.loop_value_spills == 0);
        /* Nowhere in the region a stacked home: gather's five stacked
         * LIVEINs (r32-r36) come from the entry image. */
        CHECK(no_stacked_home(id, prog ? "shladd_xor production" : "gather production") ==
              (prog ? 0u : 5u));
        printf(
            "unit_runtime: static %s: %u host instructions in the loop, no call, R12 only for requested_exit/irq_kick, "
            "%u loop-value spills (%u restoration-only)\n",
            prog ? "shladd_xor" : "gather", insns, rep.loop_value_spills, rep.restore_only_spills);
    }
    gather_close(&L);
}

/* ---- frame-relative homes ---- */

#define BOF_PROG (UM_CODE + 0x4000)
static const unsigned bof_set[] = {0, 44, 90, 95, 77};
#define BOF_SETS 5u
#define BOF_COMPILED_AT 1u /* bof_set[1] = 44 */

static uint64_t bof_head;
static unsigned bof_head_compiles;
static void bof_on_compile(const struct vcpu *v, enum t1_compile_status st,
                           const struct t1_compile_report *rep, void *arg)
{
    (void)rep;
    (void)arg;
    if (v->s.ip == bof_head && v->s.slot == 0 && st == T1_COMPILED)
        ++bof_head_compiles;
}

/* The stacked physical registers outside v's frame: values and NaTs a home
 * at a wrong frame base would read or overwrite. */
static void poison_outside_frame(struct vcpu *v)
{
    unsigned bof = v->s.rse_bof, sof = (unsigned)(v->s.cfm & 127);
    for (unsigned o = sof; o < IA64_N_STACKED_PHYS; ++o) {
        unsigned at = t1t_phys(bof, 32 + o);
        v->s.gr[at] = UINT64_C(0x5ac0000000000000) | (uint64_t)o << 16 | bof;
        v->s.nat[at] = o % 3 == 1;
    }
}

/* No instruction of the region (body and stubs) addresses a stacked home;
 * returns its entry-image loads. */
static unsigned no_stacked_home_code(const uint8_t *code, size_t size, const char *label)
{
    unsigned image;
    CHECK(umt_no_stacked_home(code, size, label, &image));
    return image;
}

static unsigned no_stacked_home(uint32_t id, const char *label)
{
    const uint8_t *code;
    size_t size, ls, le;
    CHECK(t1_test_region_code(id, &code, &size, &ls, &le));
    return no_stacked_home_code(code, size, label);
}

/* The region's exits of the last run all left `id`, and there was one. */
static void all_exits_from(struct vcpu *v, uint32_t id, const char *label)
{
    unsigned mine = 0, other = 0;
    struct t1_exit_info xi;
    for (unsigned k = 0; k < t1_exit_count(umt_tv(v)); ++k)
        if (t1_exit_get(umt_tv(v), k, &xi)) {
            if (xi.region_id == id)
                ++mine;
            else if (t1_region_get(xi.region_id) && t1_region_get(xi.region_id)->c.ip == bof_head)
                ++other;
        }
    if (!mine || other)
        fprintf(stderr, "%s: %u exits from region %u, %u from another region at the header\n",
                label, mine, id, other);
    CHECK(mine > 0 && other == 0);
}

/*
 * stacked-liveins. Frame sof = sol = 24, sor = 16, rrb.gr = 5:
 *   add r14 = r14, r32 ; add r15 = r15, r39 ;;      r32 -> o 5, r39 -> o 12
 *   add r14 = r14, r47 ; xor r15 = r15, r48 ;;      r47 -> o 4, r48 -> o 16
 *   ld8 r16 = [r34], 8 ; add r14 = r14, r55 ;;      r34 -> o 7, r55 -> o 23
 *   add r15 = r15, r16 ; br.cloop
 */
static void t_stacked_liveins(void)
{
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MII),
         {ienc_alu(0, IENC_ADD, 14, 14, 32), ienc_alu(0, IENC_ADD, 15, 15, 39), ienc_nop_i()}},
        {IENC_STOP(IENC_MII),
         {ienc_alu(0, IENC_ADD, 14, 14, 47), ienc_alu(0, IENC_XOR, 15, 15, 48), ienc_nop_i()}},
        {IENC_STOP(IENC_MMI),
         {ienc_ld_inc_imm(0, 8, false, 16, 34, 8), ienc_nop_m(),
          ienc_alu(0, IENC_ADD, 14, 14, 55)}},
        {IENC_STOP(IENC_MIB),
         {ienc_nop_m(), ienc_alu(0, IENC_ADD, 15, 15, 16), ienc_br_cloop(-3 * 16)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
    };
    um_emit(BOF_PROG, b, 5);
    for (unsigned i = 0; i < 32; ++i) {
        uint64_t x = rnd64();
        memcpy(um_ram(UM_DATA + 0x600 + 8 * i), &x, 8);
    }
    code_notify_vmm_write(UM_DATA + 0x600, 256);
    static const unsigned want_o[] = {5, 12, 4, 16, 7, 23}; /* r32 r39 r47 r48 r34 r55 */
    struct vcpu *v[BOF_SETS], *ref = um_vcpu(BOF_PROG);
    struct um_snapshot snap[BOF_SETS];
    for (unsigned k = 0; k < BOF_SETS; ++k) {
        v[k] = um_vcpu(BOF_PROG);
        v[k]->s.cfm = um_cfm(24, 24, 16, 5, 0);
        v[k]->s.rse_bof = bof_set[k];
        poison_outside_frame(v[k]);
        for (unsigned r = 32; r < 56; ++r)
            um_gr(v[k], r, rnd64(), false);
        um_gr(v[k], 34, UM_DATA + 0x600, false);
        um_gr(v[k], 14, 0, false);
        um_gr(v[k], 15, 0, false);
        v[k]->s.ar[AR_LC] = 10;
        um_snapshot_take(&snap[k], v[k]);
    }
    /* BOF 90, 95, 77: the frame's last register is physically below its first. */
    for (unsigned k = 0; k < BOF_SETS; ++k) {
        CHECK((t1t_phys(bof_set[k], 32 + 23) < t1t_phys(bof_set[k], 32)) == (bof_set[k] + 24 > 96));
        for (unsigned r = 32; r < 56; ++r) /* bof.h agrees with the CPU model's renaming */
            CHECK(ia64_gr_index(&snap[k].v.s, r) ==
                  t1t_phys(bof_set[k], t1t_element_reg(&snap[k].v.s, r)));
    }
    um_snapshot_restore(&snap[BOF_COMPILED_AT], v[0]);
    struct t1_compile_report rep;
    uint32_t id = umt_compile(v[0], T1_COMPILE_VERIFY, 0, 0, &rep, "stacked-liveins");
    CHECK(id);
    const struct t1_region *r = t1_region_get(id);
    CHECK(r && r->nframe_in == 6);
    bool seen[6] = {false};
    for (unsigned k = 0; k < r->nframe_in; ++k) {
        CHECK(!(r->frame_in[k] & T1_FRAME_IN_NAT));
        for (unsigned j = 0; j < 6; ++j)
            seen[j] |= (r->frame_in[k] & 0x7f) == want_o[j];
    }
    for (unsigned j = 0; j < 6; ++j)
        CHECK(seen[j]);
    CHECK(no_stacked_home(id, "stacked-liveins") == 6);
    bof_head = BOF_PROG;
    bof_head_compiles = 0;
    t1_test_set_compile_observer(bof_on_compile, NULL);
    struct umt_run run;
    for (unsigned k = 0; k < BOF_SETS; ++k) {
        char label[64];
        snprintf(label, sizeof(label), "stacked-liveins bof %u", bof_set[k]);
        umt_case(v[0], ref, &snap[k], T1_COMPILE_VERIFY, 11 * 7 + 40, id, NULL, &run, false, label);
        all_exits_from(v[0], id, label);
        /* The image of the (single) entry: slot k = the physical home of its
         * frame offset at this BOF, in the entry state. */
        const struct t1_exec *x = &umt_tv(v[0])->x;
        for (unsigned s = 0; s < r->nframe_in; ++s)
            CHECK(x->frame_in[s] ==
                  snap[k].v.s.gr[t1t_phys(bof_set[k], 32 + (r->frame_in[s] & 0x7f))]);
        /* A forced exit at the site's third occurrence: the frame (r34's
         * post-incremented element among them) materialized at this BOF. */
        struct umt_arm arm = {
            .point = point_at(id, T1_PT_SITE, 0), .occurrence = 3, .action = T1_FORCE_TRAP_EXIT};
        snprintf(label, sizeof(label), "stacked-liveins forced bof %u", bof_set[k]);
        umt_case(v[0], ref, &snap[k], T1_COMPILE_VERIFY, 11 * 7 + 40, id, &arm, &run, false, label);
        CHECK(run.forced == 1);
    }
    t1_test_set_compile_observer(NULL, NULL);
    CHECK(bof_head_compiles == 0);
    printf(
        "unit_runtime: stacked-liveins: 6 stacked LIVEINs of a rotating 24-register frame, one region (compiled at "
        "BOF 44) run at BOF 0/44/90/95/77: equal, image = physical homes, no stacked home in the code\n");
    for (unsigned k = 0; k < BOF_SETS; ++k) {
        um_snapshot_free(&snap[k]);
        vcpu_destroy(v[k]);
    }
    umt_release(v[0]);
    vcpu_destroy(ref);
}

/*
 * stacked-nat-guard. Frame sof = 4: ld8 r16 = [r33] ;; add r14 = r14, r16 ;
 * br.cloop. r33 is in G (NaT consumption). Compiled at BOF 0; at BOF 90 a
 * NaT on r33's home (physical 123) must reject the entry (the lower tier
 * delivers the fault), a NaT on physical r33 (frame offset 7, outside the
 * frame at BOF 90) must not.
 */
static void t_stacked_nat_guard(void)
{
    const uint64_t prog = BOF_PROG + 0x200;
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 16, 33), ienc_nop_m(), ienc_nop_i()}},
        {IENC_STOP(IENC_MIB),
         {ienc_nop_m(), ienc_alu(0, IENC_ADD, 14, 14, 16), ienc_br_cloop(-16)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
    };
    um_emit(prog, b, 3);
    uint64_t x = 0x1234567;
    memcpy(um_ram(UM_DATA + 0x700), &x, 8);
    code_notify_vmm_write(UM_DATA + 0x700, 8);
    struct um_snapshot snap[3];
    for (unsigned k = 0; k < 3; ++k) {
        struct vcpu *s = um_vcpu(prog);
        s->s.cfm = um_cfm(4, 4, 0, 0, 0);
        s->s.rse_bof = k ? 90 : 0;
        um_gr(s, 33, UM_DATA + 0x700, k == 1);
        if (k == 2)
            s->s.nat[33] = true;
        um_gr(s, 14, 0, false);
        s->s.ar[AR_LC] = 5;
        um_snapshot_take(&snap[k], s);
        vcpu_destroy(s);
    }
    CHECK(t1t_phys(90, 33) == 123 && snap[1].v.s.nat[123] && !snap[1].v.s.nat[33] &&
          snap[2].v.s.nat[33]);
    struct vcpu *v = um_vcpu(prog), *ref = um_vcpu(prog);
    um_snapshot_restore(&snap[0], v);
    struct t1_compile_report rep;
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "stacked-nat-guard");
    CHECK(id && rep.nat_guards >= 1 && ((t1_region_get(id)->nat_guard[0] >> 33) & 1));
    bof_head = prog;
    static const char *const what[] = {"BOF 0", "BOF 90, NaT on r33's home",
                                       "BOF 90, NaT on physical r33"};
    struct umt_run run;
    for (unsigned k = 0; k < 3; ++k) {
        char label[96];
        snprintf(label, sizeof(label), "stacked-nat-guard %s", what[k]);
        struct t1_stats before, after;
        t1_get_stats(&before);
        umt_case(v, ref, &snap[k], T1_COMPILE_VERIFY, 40, id, NULL, &run, false, label);
        t1_get_stats(&after);
        if (k == 1) {
            unsigned mine = 0;
            struct t1_exit_info xi;
            for (unsigned e = 0; e < t1_exit_count(umt_tv(v)); ++e)
                if (t1_exit_get(umt_tv(v), e, &xi) && xi.region_id == id)
                    ++mine;
            CHECK(mine == 0 && after.nat_guard_fails > before.nat_guard_fails &&
                  v->fault.kind == IA64_FAULT_NAT);
        } else {
            all_exits_from(v, id, label);
        }
    }
    printf(
        "unit_runtime: stacked-nat-guard: r33's NaT guard reads its home at the entry's BOF (rejects a NaT at "
        "physical 123 at BOF 90, ignores physical r33 outside the frame)\n");
    for (unsigned k = 0; k < 3; ++k)
        um_snapshot_free(&snap[k]);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

/*
 * corrupt-stacked-nat. stacked-liveins's site (ld8 r16 = [r34], 8; r34 -> element 39)
 * forced at its second occurrence at BOF 90 and 77, once plainly and once
 * with t1_test_corrupt flipping NaT(39): the two captures differ exactly in
 * nat[] at r34's physical home, the corruption fails in place.
 */
static void t_corrupt_stacked_nat(void)
{
    struct vcpu *v = um_vcpu(BOF_PROG), *ref = um_vcpu(BOF_PROG);
    for (unsigned k = 2; k < BOF_SETS; k += 2) {
        unsigned bof = bof_set[k];
        struct vcpu *s = um_vcpu(BOF_PROG);
        s->s.cfm = um_cfm(24, 24, 16, 5, 0);
        s->s.rse_bof = bof;
        poison_outside_frame(s);
        for (unsigned r = 32; r < 56; ++r)
            um_gr(s, r, rnd64(), false);
        um_gr(s, 34, UM_DATA + 0x600, false);
        s->s.ar[AR_LC] = 10;
        struct um_snapshot snap;
        um_snapshot_take(&snap, s);
        vcpu_destroy(s);
        um_snapshot_restore(&snap, v);
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "corrupt-stacked-nat");
        CHECK(id);
        uint32_t site = point_at(id, T1_PT_SITE, 0);
        unsigned home = t1t_phys(bof, 39);
        CHECK(home == ia64_gr_index(&snap.v.s, 34));
        struct umt_run run;
        struct umt_arm arm = {.point = site, .occurrence = 2, .action = T1_FORCE_TRAP_EXIT};
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 11 * 7 + 40, id, &arm, &run, false,
                 "corrupt-stacked-nat control");
        static struct vcpu control;
        bool found = false;
        for (unsigned c = 0; c < run.run.ncaptures && !found; ++c)
            if (run.run.captures[c].forced) {
                control = run.run.captures[c].v;
                found = true;
            }
        CHECK(found && !control.s.nat[home]);
        arm.corrupt = arm.force_too = true;
        arm.cpoint = site;
        arm.coccurrence = 2;
        arm.element = T1_ELEMENT_NAT(39);
        arm.mask = 1;
        unsigned d = umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 11 * 7 + 40, id, &arm, &run, true,
                              "corrupt-stacked-nat");
        CHECK(d > 0);
        found = false;
        for (unsigned c = 0; c < run.run.ncaptures && !found; ++c) {
            const struct vcpu *got = &run.run.captures[c].v;
            if (!run.run.captures[c].forced)
                continue;
            found = true;
            for (unsigned i = 0; i < 128; ++i)
                CHECK(got->s.gr[i] == control.s.gr[i] &&
                      got->s.nat[i] == (control.s.nat[i] ^ (i == home)));
        }
        CHECK(found);
        um_snapshot_free(&snap);
    }
    printf(
        "unit_runtime: corrupt-stacked-nat: a corrupted stacked NaT element lands on its physical home at BOF 90 "
        "and 77 and fails in place\n");
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

/*
 * frame-rule-stacked: frame-rule with stacked registers, frame sof = 16:
 *   adds r41 = 0, r40 ; adds r40 = 1, r40 ;; tnat.z p6, p7 = r40 ;;
 *   adds r40 = 0, r41 ;; [ld8 r22 = [r33]]
 * The helper's read set (r40) is written back to r40's home before
 * ia64_execute reads r40 there, so the write-back and the CPU model's
 * renaming must meet at the entry's BOF; the frames restore 10 (r40 in W).
 * r40/r41 are frame offsets 8/9: at BOF 90 and 95 physical 34/35 and
 * 39/40. Variants: exit, memory point (forced site actions and the forced
 * helper exit), loop (helper in iteration i, point in i + 1). One region
 * per variant, compiled at BOF 44, run at BOF 0/44/90/95.
 */
#define FRAME_RULE_STACKED_SEQ(q)                                                                  \
    {IENC_STOP(IENC_MII), {ienc_adds(q, 41, 0, 40), ienc_adds(0, 40, 1, 40), ienc_nop_i()}},       \
        {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_tnat(0, 0, 0, 0, 6, 7, 40), ienc_nop_i()}},      \
    {                                                                                              \
        IENC_STOP(IENC_MII),                                                                       \
        {                                                                                          \
            ienc_adds(0, 40, 0, 41), ienc_nop_i(), ienc_nop_i()                                    \
        }                                                                                          \
    }

static void frame_rule_stacked_states(uint64_t prog, struct um_snapshot *snap, unsigned lc)
{
    for (unsigned k = 0; k < 4; ++k) {
        struct vcpu *s = um_vcpu(prog);
        s->s.cfm = um_cfm(16, 16, 0, 0, 0);
        s->s.rse_bof = bof_set[k];
        poison_outside_frame(s);
        for (unsigned r = 32; r < 48; ++r)
            um_gr(s, r, rnd64(), false);
        um_gr(s, 40, 10, false);
        um_gr(s, 33, UM_DATA + 0x200, false);
        s->s.ar[AR_LC] = lc;
        um_snapshot_take(&snap[k], s);
        vcpu_destroy(s);
    }
}

static void t_frame_rule_stacked(void)
{
    unsigned cases = 0;
    for (unsigned variant = 0; variant < 3; ++variant) {
        const uint64_t prog = BOF_PROG + 0x400 + 0x100 * variant;
        struct ienc_bundle loop[] = {
            {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 22, 33), ienc_nop_m(), ienc_nop_i()}},
            FRAME_RULE_STACKED_SEQ(0),
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(-4 * 16)}},
            {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
        };
        struct ienc_bundle straight[] = {
            FRAME_RULE_STACKED_SEQ(0),
            {IENC_STOP(IENC_MMI),
             {variant ? ienc_ld(0, 8, false, 22, 33) : ienc_nop_m(), ienc_nop_m(), ienc_nop_i()}},
            {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
        };
        if (variant == 2)
            um_emit(prog, loop, 6);
        else
            um_emit(prog, straight, 5);
        struct um_snapshot snap[4];
        frame_rule_stacked_states(prog, snap, 4);
        struct vcpu *v = um_vcpu(prog), *ref = um_vcpu(prog);
        um_snapshot_restore(&snap[BOF_COMPILED_AT], v);
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "frame-rule-stacked");
        CHECK(id);
        const struct t1_region *r = t1_region_get(id);
        CHECK(r->nhelper == 1 && no_stacked_home(id, "frame-rule-stacked") >= 1);
        bof_head = prog;
        bof_head_compiles = 0;
        t1_test_set_compile_observer(bof_on_compile, NULL);
        for (unsigned k = 0; k < 4; ++k) {
            char label[64];
            unsigned r40 = ia64_gr_index(&snap[k].v.s, 40);
            CHECK(r40 == t1t_phys(bof_set[k], 40));
            snprintf(label, sizeof(label), "frame-rule-stacked variant %u bof %u", variant,
                     bof_set[k]);
            struct umt_run run;
            uint64_t budget = variant == 2 ? 60 : 30;
            umt_case(v, ref, &snap[k], T1_COMPILE_VERIFY, budget, id, NULL, &run, false, label);
            all_exits_from(v, id, label);
            if (variant < 2)
                CHECK(v->s.gr[r40] == 10);
            ++cases;
            if (variant >= 1) {
                static const enum t1_force_action acts[] = {T1_FORCE_TRAP_EXIT, T1_FORCE_MISS_EXIT,
                                                            T1_FORCE_TRAP_RETRY};
                for (unsigned a = 0; a < 3; ++a)
                    for (unsigned occ = 1; occ <= (variant == 2 ? 3u : 1u); ++occ) {
                        struct umt_arm arm = {.point = point_at(id, T1_PT_SITE, 0),
                                              .occurrence = occ,
                                              .action = acts[a]};
                        umt_case(v, ref, &snap[k], T1_COMPILE_VERIFY, budget, id, &arm, &run, false,
                                 label);
                        CHECK(run.forced == 1);
                        if (acts[a] != T1_FORCE_TRAP_RETRY)
                            CHECK(run.run.ncaptures >= 1 && run.run.captures[0].v.s.gr[r40] == 10);
                        ++cases;
                    }
                struct umt_arm arm = {.point = point_at(id, T1_PT_HELPER, 0),
                                      .occurrence = 1,
                                      .action = T1_FORCE_HELPER_EXIT};
                umt_case(v, ref, &snap[k], T1_COMPILE_VERIFY, budget, id, &arm, &run, false, label);
                CHECK(run.forced == 1);
                ++cases;
            }
        }
        t1_test_set_compile_observer(NULL, NULL);
        CHECK(bof_head_compiles == 0);
        for (unsigned k = 0; k < 4; ++k)
            um_snapshot_free(&snap[k]);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    printf(
        "unit_runtime: frame-rule-stacked: the helper's stacked read set written back at BOF 0/44/90/95 (one region per "
        "variant), %u cases equal in place and at the end\n",
        cases);
}

/*
 * frame-fill, nat-guard-holds: the two per-entry functions alone, on hand-built region
 * tables (t1_frame_build's encoding: offset | T1_FRAME_IN_NAT, the span;
 * t1_nat_guard_list from a random G), at every BOF 0..95 against the
 * homes bof.h names. The gather path runs when the slots fit below 96 -
 * BOF, the modular one otherwise and with NaT slots; the guard's two paths
 * likewise by G's largest stacked offset.
 */
static void t_frame_fill_nat_guard(void)
{
    static struct t1_region r;
    static struct t1_exec x;
    static struct vcpu v;
    unsigned fills = 0, guards = 0, gather = 0, rejected = 0;
    for (unsigned trial = 0; trial < 64; ++trial) {
        memset(&r, 0, sizeof(r));
        unsigned n = 1 + (unsigned)(rnd64() % 24), maxo = trial < 32 ? 8 + trial : 96;
        bool nats = trial % 3 == 2;
        for (unsigned k = 0; k < n; ++k) {
            unsigned o = (unsigned)(rnd64() % maxo);
            bool nat = nats && (rnd64() & 1);
            r.frame_in[k] = (uint8_t)(o | (nat ? T1_FRAME_IN_NAT : 0));
            if (nat)
                r.frame_in_span = UINT8_MAX;
            else if (r.frame_in_span != UINT8_MAX && o + 1 > r.frame_in_span)
                r.frame_in_span = (uint8_t)(o + 1);
        }
        r.nframe_in = n;
        r.nat_guard[0] = rnd64() & rnd64() & ~UINT64_C(1);
        r.nat_guard[1] = trial < 32 ? 0 : rnd64() & rnd64() & rnd64();
        if (trial < 32)
            r.nat_guard[0] &= (UINT64_C(1) << (32 + maxo / 2)) - 1;
        t1_nat_guard_list(&r);
        for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof) {
            v.s.rse_bof = bof;
            for (unsigned i = 0; i < 128; ++i) {
                v.s.gr[i] = rnd64();
                v.s.nat[i] = (rnd64() % 23) == 0;
            }
            t1_frame_in_fill(&x, &r, &v);
            for (unsigned k = 0; k < n; ++k) {
                unsigned at = t1t_phys(bof, 32 + (r.frame_in[k] & 0x7f));
                uint64_t want = (r.frame_in[k] & T1_FRAME_IN_NAT) ? v.s.nat[at] : v.s.gr[at];
                CHECK(x.frame_in[k] == want);
            }
            gather += bof + r.frame_in_span <= 96;
            ++fills;
            bool holds = true;
            for (unsigned p = 1; p < 128; ++p)
                if (((r.nat_guard[p / 64] >> (p % 64)) & 1) && v.s.nat[t1t_phys(bof, p)])
                    holds = false;
            CHECK(t1_nat_guard_holds(&r, &v) == holds);
            rejected += !holds;
            ++guards;
        }
    }
    CHECK(gather > fills / 4 && gather < fills && rejected > guards / 8 && rejected < guards);
    printf(
        "unit_runtime: frame-fill/nat-guard-holds: %u entry images and %u NaT guards at every BOF equal to the "
        "architectural homes (%u by the gather path, %u guards rejecting)\n",
        fills, guards, gather, rejected);
}

/* SDM Vol.1 register files and Vol.2 register-stack mapping: restoration
 * must preserve every home outside the frame, including predicate bits
 * and the other half of an FR. Exercise arbitrary frame order and every
 * location, with overflowing additions and every register-stack base. */
static void t_materialize_homes(void)
{
    static struct vcpu v, want;
    static struct t1_exec x;
    static struct t1_region r;
    struct t1_frame_entry frame[T1_ELEMENT_LIMIT];
    unsigned n = 0;
    for (unsigned e = 1; e < T1_ELEMENT_LIMIT; ++e) {
        if (e == 128 || e == 256 || e == T1_ELEMENT_MEM || e == 323 || e == 324 || e == 452 ||
            e == 453)
            continue;
        frame[n++] = (struct t1_frame_entry){.element = e};
    }
    for (unsigned i = n; i > 1; --i) {
        unsigned k = rnd64() % i;
        struct t1_frame_entry f = frame[k];
        frame[k] = frame[i - 1];
        frame[i - 1] = f;
    }
    r.frame = frame;
    x.v = &v;
    x.region = &r;
    struct t1_point p = {.frame_count = n, .ip = UM_CODE + 16, .slot = 2, .retired_offset = 7};
    for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof) {
        memset(&v, 0xa5, sizeof(v));
        for (unsigned i = 0; i < 128; ++i)
            v.s.nat[i] = (i & 1) != 0;
        v.s.rse_bof = bof;
        v.s.psr = rnd64();
        v.s.pr = rnd64();
        want = v;
        x.budget_entry = 100;
        x.raw_r13 = 31;
        x.retired_entry = 123;
        x.entry_bof = bof;
        for (unsigned i = 0; i < 16; ++i)
            x.regs[i] = rnd64();
        for (unsigned i = 0; i < T1_SPILL_SLOTS; ++i)
            x.spill[i] = rnd64();
        for (unsigned i = 0; i < n; ++i) {
            struct t1_frame_entry *f = &frame[i];
            unsigned kind = (i + bof) % 6;
            struct t1_loc loc = {kind, i % 16, i % T1_SPILL_SLOTS, rnd64()};
            *f = t1_frame_entry_make(f->element, &loc);
            uint64_t value = f->value;
            if (kind == T1_LOC_HOME)
                continue;
            if (kind == T1_LOC_REG || kind == T1_LOC_REG_PLUS)
                value = x.regs[f->reg];
            if (kind == T1_LOC_SPILL || kind == T1_LOC_SPILL_PLUS)
                value = x.spill[f->slot];
            if (kind == T1_LOC_REG_PLUS || kind == T1_LOC_SPILL_PLUS)
                value += f->value;
            unsigned e = f->element;
            if (e < 128)
                want.s.gr[t1t_phys(bof, e)] = value;
            else if (e < 256)
                want.s.nat[t1t_phys(bof, e - 128)] = (value & 1) != 0;
            else if (e < T1_ELEMENT_LC) {
                uint64_t bit = UINT64_C(1) << (e - 256);
                want.s.pr = (want.s.pr & ~bit) | ((value & 1) ? bit : 0);
            } else if (e == T1_ELEMENT_LC)
                want.s.ar[AR_LC] = value;
            else if (e == T1_ELEMENT_LSB)
                want.last_successful_bundle = value;
            else if (e <= T1_ELEMENT_FR(127))
                want.s.fr[e - 323].significand = value;
            else if (e == T1_ELEMENT_PSR_MF)
                want.s.psr |= value & T1_PSR_MF_BITS;
            else if (e == T1_ELEMENT_PFS)
                want.s.ar[AR_PFS] = value;
            else if (e == T1_ELEMENT_EC)
                want.s.ar[AR_EC] = value;
            else if (e >= T1_ELEMENT_BR(0) && e <= T1_ELEMENT_BR(7))
                want.s.br[e - T1_ELEMENT_BR(0)] = value;
            else {
                want.s.fr[e - 452].exponent = value & 0x1ffff;
                want.s.fr[e - 452].sign = (value >> 17) & 1;
            }
        }
        want.s.ip = p.ip;
        want.s.slot = p.slot;
        want.retired = 123 + (100 - 31) + 7;
        /* The region tables list a point's HOME entries after the others
         * (frame_restore); materialization never reads a HOME entry. */
        unsigned restore = 0;
        for (unsigned i = 0; i < n; ++i)
            if (frame[i].kind != T1_LOC_HOME) {
                struct t1_frame_entry f = frame[i];
                frame[i] = frame[restore];
                frame[restore++] = f;
            }
        p.frame_restore = restore;
        for (unsigned i = restore; i < n; ++i)
            frame[i].reg = 0xff; /* never read */
        t1_materialize(&x, &p);
        CHECK(!memcmp(&v, &want, sizeof(v)));
    }
    printf("unit_runtime: materialization: every location and home at every BOF preserved\n");
}

/* SDM Vol.2 Table 6-2: a call skips an entry RNaT collection even
 * with sol=0; its matched return keeps the following data slot. Before
 * the first taken call, including a false predicate, BSP stays put. */
static void t_collection_entry(void)
{
    const uint64_t base = UM_DATA + 0x1f8;
    for (unsigned pred = 0; pred < 3; ++pred) {
        struct ienc_bundle b[10];
        for (unsigned i = 0; i < 10; ++i)
            b[i] = (struct ienc_bundle){IENC_STOP(IENC_MII),
                                        {ienc_nop_m(), ienc_nop_i(), ienc_nop_i()}};
        b[0].slot[0] = ienc_ld(0, 8, false, 14, 2);
        b[1] = (struct ienc_bundle){
            IENC_STOP(IENC_MIB),
            {ienc_nop_m(), ienc_nop_i(), ienc_br_call(pred ? 6 : 0, 0, 7 * 16)}};
        b[2].slot[0] = ienc_ld(0, 8, false, 15, 2);
        b[3] = (struct ienc_bundle){IENC_STOP(IENC_MIB),
                                    {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(-3 * 16)}};
        b[4] = (struct ienc_bundle){IENC_STOP(IENC_MIB),
                                    {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}};
        b[5].slot[0] = ienc_break_m(1);
        b[8].slot[0] = ienc_ld(0, 8, false, 16, 2);
        b[9] = (struct ienc_bundle){IENC_STOP(IENC_MIB),
                                    {ienc_nop_m(), ienc_nop_i(), ienc_br_ret(0, 0)}};
        um_emit(PROG, b, 10);
        static const unsigned locals[] = {0, 1, 62, 63, 64};
        for (unsigned n = 0; n < sizeof(locals) / sizeof(locals[0]); ++n) {
            unsigned sol = locals[n];
            struct vcpu *v = um_vcpu(PROG), *ref = um_vcpu(PROG);
            v->s.cfm = um_cfm(sol + 1, sol, 0, 0, 0);
            v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = base;
            v->s.ar[AR_RNAT] = 0x1234;
            v->s.ar[AR_LC] = 2;
            um_gr(v, 2, UM_DATA + 0x1000, false);
            um_pr(v, 6, pred == 2);
            for (unsigned i = 32; i < 128; ++i)
                v->s.nat[i] = (i & 1) != 0;
            struct um_snapshot snap;
            um_snapshot_take(&snap, v);
            uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "collection-entry");
            CHECK(id);
            unsigned sites = 0;
            for (unsigned k = 0; k < t1_point_count(id); ++k) {
                struct t1_point_info pi;
                CHECK(t1_point_get(id, k, &pi));
                if (pi.kind != T1_PT_SITE)
                    continue;
                ++sites;
                if (pred == 2 && pi.ip != PROG)
                    continue; /* Taken predicated calls leave the region. */
                for (unsigned occurrence = 1; occurrence <= 2; ++occurrence) {
                    struct umt_arm arm = {.point = pi.point_id,
                                          .occurrence = occurrence,
                                          .action = T1_FORCE_TRAP_EXIT};
                    struct umt_run run;
                    umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 100, id, &arm, &run, false,
                             "collection-entry");
                    CHECK(run.forced == 1);
                    for (unsigned c = 0; c < run.run.ncaptures; ++c) {
                        const struct um_capture *cap = &run.run.captures[c];
                        if (!cap->forced)
                            continue;
                        uint64_t expected = base;
                        if (pred != 1 && (pi.ip != PROG || occurrence > 1)) {
                            expected += 8;
                            if (pi.ip == PROG + 8 * 16)
                                expected += 8 * (sol + sol / 63);
                        }
                        CHECK(cap->v.s.ar[AR_BSP] == expected);
                    }
                }
            }
            CHECK(sites == (pred ? 2u : 3u));
            if (!pred && !sol) {
                snap.v.s.gr[2] = UINT64_C(0x40000000);
                struct umt_run run;
                umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 100, id, NULL, &run, false,
                         "collection-entry fault before call");
                CHECK(run.run.ncaptures && run.exit.reason == VCPU_EXIT_BAD_ACCESS);
                CHECK(v->s.ar[AR_BSP] == base);
            }
            um_snapshot_free(&snap);
            umt_release(v);
            vcpu_destroy(v);
            vcpu_destroy(ref);
        }
    }
    puts(
        "unit_runtime: collection entry: first call, zero/nonzero locals, returns, loop, predicates and forced exits");
}

static unsigned tail_compiles;
static void tail_compiled(const struct vcpu *v, enum t1_compile_status st,
                          const struct t1_compile_report *rep, void *arg)
{
    (void)rep;
    if (st == T1_COMPILED && v->s.ip == *(const uint64_t *)arg && v->s.slot == 0)
        ++tail_compiles;
}

/* A loop's fall-through tail is repeatedly reached without being the
 * target of a backward branch. It must become hot too; execution and
 * every restoration still agree with the instruction reference. The tail
 * returns through a jump bundle before the loop, not by a branch to the
 * loop's head, which would make it part of the head's region. */
static void t_hot_tail(void)
{
    uint64_t tail = PROG + 0x410;
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 16)}},
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(0)}},
        {IENC_STOP(IENC_MII), {ienc_adds(0, 16, 1, 16), ienc_nop_i(), ienc_nop_i()}},
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, -48)}},
        {IENC_STOP(IENC_MII), {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}},
    };
    um_emit(tail - 32, b, 5);
    struct vcpu *v = um_vcpu(tail - 16), *ref = um_vcpu(tail - 16);
    v->s.ar[AR_LC] = 0;
    um_gr(v, 16, 0, false);
    um_pr(v, 6, true);
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "hot tail");
    CHECK(id);
    struct t1_config saved = t1_cfg;
    t1_cfg.hot_threshold = 2;
    tail_compiles = 0;
    t1_test_set_compile_observer(tail_compiled, &tail);
    struct umt_run run;
    umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 1200, id, NULL, &run, false, "hot tail");
    t1_test_set_compile_observer(NULL, NULL);
    CHECK(v->s.gr[16] == 100 && tail_compiles > 0);
    t1_cfg = saved;
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
    printf("unit_runtime: hot fall-through tail equal to the reference\n");
}

static void guarded_tail_compiled(const struct vcpu *v, enum t1_compile_status st,
                                  const struct t1_compile_report *rep, void *arg)
{
    tail_compiled(v, st, rep, arg);
    CHECK(tail_compiles <= 1);
}

/* An observed-return tail can compile while its entry dirty guard fails.
 * Its alloc must then spill in the lower tier, without recompiling the
 * same unchanged trace boundary forever. */
static void t_guarded_hot_tail(void)
{
    uint64_t head = PROG + 0x600, tail = head + 16;
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(0)}},
        {IENC_STOP(IENC_MII), {ienc_alloc(0, 14, 16, 4, 0), ienc_nop_i(), ienc_nop_i()}},
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_ret(0, 2)}},
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
    };
    um_emit(head, b, 4);
    struct vcpu *v = um_vcpu(head), *ref = um_vcpu(head);
    v->s.cfm = 0;
    v->s.rse_dirty = 96;
    v->s.ar[AR_BSPSTORE] = UM_DATA;
    v->s.ar[AR_BSP] = ia64_rse_advance(UM_DATA, 96);
    v->s.ar[AR_PFS] = 8;
    v->s.ar[AR_LC] = 0;
    v->s.br[2] = head + 48;
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "guarded tail predecessor");
    CHECK(id);
    struct t1_config saved = t1_cfg;
    t1_cfg.hot_threshold = 2;
    tail_compiles = 0;
    uint64_t helpers = t1_helper_calls(IA64_OP_ALLOC);
    t1_test_set_compile_observer(guarded_tail_compiled, &tail);
    struct umt_run run;
    umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 9, id, NULL, &run, false, "guarded hot tail");
    t1_test_set_compile_observer(NULL, NULL);
    CHECK(tail_compiles == 1 && run.nexits == 1 && v->retired == snap.v.retired + 9);
    CHECK(v->s.ip == head + 48 && !v->s.slot && v->s.cfm == 8 && v->s.rse_dirty == 80);
    CHECK(v->s.ar[AR_BSP] == snap.v.s.ar[AR_BSP] && v->s.ar[AR_BSPSTORE] == UM_DATA + 128);
    CHECK(t1_helper_calls(IA64_OP_ALLOC) == helpers);
    t1_cfg = saved;
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

/* End a run exactly at the loop's natural exit, then resume at an
 * uncompiled forward tail. A fresh budget does not make a trace head. */
static void t_hot_resume(void)
{
    uint64_t head = PROG + 0x500;
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(0)}},
        {IENC_STOP(IENC_MII), {ienc_adds(0, 16, 1, 16), ienc_nop_i(), ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}},
    };
    um_emit(head, b, 3);
    struct vcpu *v = um_vcpu(head);
    uint32_t id = umt_compile(v, 0, 0, 0, NULL, "budget resume");
    CHECK(id);
    const struct t1_region *r = t1_region_get(id);
    CHECK(r->max_path == 3);
    struct t1_config saved = t1_cfg;
    t1_cfg.hot_threshold = 2;
    uint64_t retired = v->retired;
    struct t1_stats before, exited, after;
    t1_get_stats(&before);
    CHECK(t1_run(umt_tv(v), 3).reason == VCPU_EXIT_BUDGET);
    t1_get_stats(&exited);
    CHECK(exited.entries == before.entries + 1 && v->retired == retired + 3);
    CHECK(v->s.ip == head + 16 && v->s.slot == 0);
    struct t1_vcpu *tv = umt_tv(v);
    CHECK(tv->nexits && tv->exits[tv->nexits - 1].kind == T1_PT_EXIT);
    CHECK(t1_run(tv, 1).reason == VCPU_EXIT_BUDGET);
    t1_get_stats(&after);
    CHECK(v->retired == retired + 4 && v->s.ip == head + 16 && v->s.slot == 1);
    CHECK(after.compiles == exited.compiles && after.entries == exited.entries);
    t1_cfg = saved;
    umt_release(v);
    vcpu_destroy(v);
    printf(
        "unit_runtime: compiled natural exit at budget end; fresh resume does not compile a tail\n");
}

/* The first region exit of the last run: a checkpoint at `point` after
 * `consumed` instructions. */
static bool kick_exit(struct vcpu *v, uint32_t point, uint64_t consumed)
{
    struct t1_exit_info xi;
    return t1_exit_get(umt_tv(v), 0, &xi) && xi.kind == T1_PT_CHECKPOINT && xi.point_id == point &&
           xi.consumed == consumed;
}

/* A region at an inner loop's header continues through the unconditional
 * outer branch into the enclosing loop and closes at a second back-edge to
 * its head, so the head bundle is region code twice (unit_lift has the
 * formation). Every budget end over one outer pass and
 * every forced restoration at the duplicate's site compare equal with the
 * reference in place and at the end. A kick pending at entry ends the run
 * at the first back-edge taken: the inner one after one
 * iteration, or, in the last inner iteration, the duplicate's after the
 * outer code (tests/tier1/kick_mutants.py breaks either). */
static void t_stitched_outer(void)
{
    uint64_t outer = PROG + 0x600, head = outer + 0x20;
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MII),
         {ienc_cmp(0, IENC_EQ, false, false, 6, 7, 20, 0), ienc_adds(0, 20, -1, 20),
          ienc_mov_to_ar(0, AR_LC, 21)}},
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, 0x40)}},
        {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 22, 33), ienc_nop_m(), ienc_nop_i()}},
        {IENC_STOP(IENC_MIB),
         {ienc_nop_m(), ienc_alu(0, IENC_ADD, 14, 14, 22), ienc_br_cloop(-0x10)}},
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, -0x40)}},
        {IENC_STOP(IENC_MII), {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}},
    };
    um_emit(outer, b, 6);
    struct vcpu *v = um_vcpu(head), *ref = um_vcpu(head);
    um_gr(v, 14, 0, false);
    um_gr(v, 20, 30, false);
    um_gr(v, 21, 2, false);
    um_gr(v, 33, UM_DATA + 0x800, false);
    uint64_t x = 0x1234;
    memcpy(um_ram(UM_DATA + 0x800), &x, 8);
    v->s.ar[AR_LC] = 2;
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "stitched outer loop");
    CHECK(id);
    uint32_t dup = UINT32_MAX, inner = UINT32_MAX, outer_ckpt = UINT32_MAX;
    unsigned sites = 0;
    for (unsigned k = 0; k < t1_point_count(id); ++k) {
        struct t1_point_info pi;
        CHECK(t1_point_get(id, k, &pi));
        if (pi.kind == T1_PT_CHECKPOINT && pi.insn_index == 5)
            inner = pi.point_id;
        if (pi.kind == T1_PT_CHECKPOINT && pi.insn_index == 20)
            outer_ckpt = pi.point_id;
        if (pi.kind != T1_PT_SITE)
            continue;
        CHECK(pi.ip == head && pi.slot == 0);
        ++sites;
        if (pi.insn_index == 15)
            dup = pi.point_id;
    }
    CHECK(sites == 2 && dup != UINT32_MAX && inner != UINT32_MAX && outer_ckpt != UINT32_MAX);
    /* One outer pass: three inner iterations of 6 and 9 outer instructions. */
    struct umt_run run;
    unsigned cases = 0;
    for (uint64_t budget = 1; budget <= 3 * 6 + 9 + 3 * 6 + 1; ++budget, ++cases) {
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, budget, id, NULL, &run, false,
                 "stitched outer loop");
        CHECK(run.exit.reason == VCPU_EXIT_BUDGET && v->retired == snap.v.retired + budget);
    }
    static const enum t1_force_action acts[] = {T1_FORCE_TRAP_EXIT, T1_FORCE_MISS_EXIT,
                                                T1_FORCE_TRAP_RETRY};
    for (unsigned a = 0; a < 3; ++a)
        for (unsigned occ = 1; occ <= 3; ++occ, ++cases) {
            struct umt_arm arm = {.point = dup, .occurrence = occ, .action = acts[a]};
            umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 200, id, &arm, &run, false,
                     "stitched outer loop forced");
            CHECK(run.forced == 1);
        }
    struct umt_arm kick = {.point = UINT32_MAX, .before_run = kick_before};
    umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 60, id, &kick, &run, false,
             "stitched outer loop kick");
    CHECK(kick_exit(v, inner, 6));
    snap.v.s.ar[AR_LC] = 0;
    umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 60, id, &kick, &run, false,
             "stitched outer loop kick, last inner");
    CHECK(kick_exit(v, outer_ckpt, 6 + 3 + 6 + 6));
    cases += 2;
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
    printf(
        "unit_runtime: stitched outer loop: the head bundle twice in one region, %u cases (budget ends, "
        "forced restorations at the duplicate's site, kicks at both back-edges) equal in place and at "
        "the end\n",
        cases);
}

/* A loop nest: the region at the outer head holds the
 * inner br.cloop loop as an inner loop with its own checkpoint (resuming at
 * the inner header). Every budget end over two outer passes and every
 * forced restoration at each site compare equal with the reference in place
 * and at the end. A kick pending at entry ends the run at the first
 * back-edge taken: the inner one, or with one inner iteration the outer
 * one. The innermost loop body (loop_start/loop_end) holds the inner site
 * only, and no loop keeps a value it reads in a spill slot
 * (restoration-only values may wait there).
 *  +0x00 { ld8 r22 = [r33], 8 ; adds r20 = -1, r20 ; mov ar.lc = r21 } ;;   head
 *  +0x10 { ld8 r23 = [r34], 8 ; nop.i ; nop.i } ;;                          inner header
 *  +0x20 { nop.m ; add r14 = r14, r23 ; nop.i } ;;
 *  +0x30 { nop.m ; add r15 = r15, r22 ; br.cloop +0x10 } ;;
 *  +0x40 { cmp.eq p7, p6 = r20, r0 ; nop.i ; nop.i } ;;
 *  +0x50 { st8 [r35] = r15, 8 ; nop.i ; (p6) br head } ;;
 *  +0x60 { break.m 1 ; nop.i ; nop.i } ;; */
static void t_nested(void)
{
    uint64_t head = PROG + 0x800;
    struct ienc_bundle b[] = {
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
        {IENC_STOP(IENC_MII), {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}},
    };
    um_emit(head, b, 7);
    struct vcpu *v = um_vcpu(head), *ref = um_vcpu(head);
    for (unsigned i = 0; i < 64; ++i) {
        uint64_t x = rnd64();
        memcpy(um_ram(UM_DATA + 0x1000 + 8 * i), &x, 8);
    }
    code_notify_vmm_write(UM_DATA + 0x1000, 0x200);
    um_gr(v, 14, 0, false);
    um_gr(v, 15, 0, false);
    um_gr(v, 20, 3, false);
    um_gr(v, 21, 2, false);
    um_gr(v, 33, UM_DATA + 0x1000, false);
    um_gr(v, 34, UM_DATA + 0x1100, false);
    um_gr(v, 35, UM_DATA + 0x1800, false);
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    struct t1_compile_report rep;
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "loop nest");
    CHECK(id);
    uint32_t inner = UINT32_MAX, outer = UINT32_MAX, sites[3];
    unsigned nsites = 0;
    for (unsigned k = 0; k < t1_point_count(id); ++k) {
        struct t1_point_info pi;
        CHECK(t1_point_get(id, k, &pi));
        if (pi.kind == T1_PT_CHECKPOINT && pi.insn_index == 11) {
            inner = pi.point_id;
            CHECK(pi.ip == head + 0x10);
        }
        if (pi.kind == T1_PT_CHECKPOINT && pi.insn_index == 17) {
            outer = pi.point_id;
            CHECK(pi.ip == head);
        }
        if (pi.kind == T1_PT_SITE && nsites < 3)
            sites[nsites++] = pi.point_id;
    }
    CHECK(inner != UINT32_MAX && outer != UINT32_MAX && nsites == 3);
    /* One outer pass: 3 + 3 x 9 + 6 instructions. Every budget is retired
     * exactly (the program ends after three passes). */
    unsigned pass = 36, cases = 0;
    struct umt_run run;
    for (uint64_t budget = 1; budget <= 2 * pass + 1; ++budget, ++cases) {
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, budget, id, NULL, &run, false, "loop nest");
        CHECK(run.exit.reason == VCPU_EXIT_BUDGET && v->retired == snap.v.retired + budget);
    }
    static const enum t1_force_action acts[] = {T1_FORCE_TRAP_EXIT, T1_FORCE_MISS_EXIT,
                                                T1_FORCE_TRAP_RETRY};
    for (unsigned k = 0; k < 3; ++k)
        for (unsigned a = 0; a < 3; ++a)
            for (unsigned occ = 1; occ <= 3; ++occ, ++cases) {
                struct umt_arm arm = {.point = sites[k], .occurrence = occ, .action = acts[a]};
                umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 200, id, &arm, &run, false,
                         "loop nest forced");
                CHECK(run.forced == 1);
            }
    struct umt_arm kick = {.point = UINT32_MAX, .before_run = kick_before};
    umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 200, id, &kick, &run, false, "loop nest kick");
    CHECK(kick_exit(v, inner, 3 + 9));
    snap.v.s.gr[21] = 0;
    umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 200, id, &kick, &run, false,
             "loop nest kick, one inner iteration");
    CHECK(kick_exit(v, outer, 3 + 9 + 6));
    cases += 2;
    /* The innermost loop body: its one site, no call; no loop value spilled. */
    const uint8_t *code;
    size_t size, ls, le;
    CHECK(t1_test_region_code(id, &code, &size, &ls, &le) && ls < le && le <= size);
    unsigned insns = 0, tags = 0;
    for (size_t at = ls; at < le;) {
        struct t1_x86_decoded d;
        CHECK(t1_x86_decode(code + at, le - at, &d));
        CHECK(!(t1_x86_info(d.op)->attr & T1_XA_CALL));
        tags += d.has_mem && d.mem.base == T1_R14 && d.mem.index == T1_X86_NOREG &&
                d.op == T1_X86_CMP64_RM && d.mem.disp >= (int32_t)X_SITE(0) &&
                d.mem.disp < (int32_t)X_SITE(T1_SITE_SLOTS);
        at += d.length;
        ++insns;
    }
    CHECK(insns == rep.loop_host_insns && tags == 1 && !rep.loop_value_spills);
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
    printf(
        "unit_runtime: loop nest: inner checkpoint at its header, %u cases (budget ends, forced restorations "
        "at every site, kicks at both back-edges) equal in place and at the end; innermost body %u host "
        "instructions, one site, no loop-value spill (%u restoration-only)\n",
        cases, insns, rep.restore_only_spills);
}

/* Loop and tail traffic must not erase one another's failed-request
 * backoff, even when every bucket sees competing heads. */
static void t_hot_tables(void)
{
    struct vcpu v = {0};
    struct t1_vcpu *tv = calloc(1, sizeof(*tv));
    CHECK(tv);
    tv->v = &v;
    struct t1_config saved = t1_cfg;
    t1_cfg.hot_threshold = 2;
    for (unsigned tail = 0; tail < 2; ++tail) {
        t1_hot_reset(tv);
        v.s.ip = PROG;
        CHECK(tail ? t1_hot_trace(tv) : !t1_hot_note(tv, PROG + 16, 0));
        if (!tail)
            CHECK(t1_hot_note(tv, PROG + 16, 0));
        t1_hot_failed(tv, tail);
        for (unsigned retry = 0; retry < 64; ++retry) {
            for (unsigned i = 1; i <= 512; ++i) {
                v.s.ip = PROG + 16 * i;
                if (tail)
                    (void)t1_hot_note(tv, v.s.ip + 16, 0);
                else
                    (void)t1_hot_trace(tv);
            }
            v.s.ip = PROG;
            CHECK(tail ? !t1_hot_trace(tv) : !t1_hot_note(tv, PROG + 16, 0));
        }
        if (!tail)
            CHECK(!t1_hot_note(tv, PROG + 16, 0));
        CHECK(tail ? t1_hot_trace(tv) : t1_hot_note(tv, PROG + 16, 0));
        t1_hot_failed(tv, tail);
        for (unsigned retry = 0; retry < 128; ++retry)
            CHECK(tail ? !t1_hot_trace(tv) : !t1_hot_note(tv, PROG + 16, 0));
        if (!tail)
            CHECK(!t1_hot_note(tv, PROG + 16, 0));
        CHECK(tail ? t1_hot_trace(tv) : t1_hot_note(tv, PROG + 16, 0));
    }
    t1_hot_reset(tv);
    t1_cfg.hot_threshold = 0;
    for (unsigned i = 0; i < 256; ++i) {
        CHECK(!t1_hot_trace(tv));
        CHECK(!t1_hot_note(tv, PROG + 16, 0));
    }
    t1_cfg = saved;
    free(tv);
    printf(
        "unit_runtime: loop/tail backoff survives competing traffic; exponential retry and zero threshold hold\n");
}

/* ---- one compile_ticks for every reader ---- */

static unsigned observed_compiles;
static uint64_t observed_ticks;
static void on_compile(const struct vcpu *v, enum t1_compile_status st,
                       const struct t1_compile_report *rep, void *arg)
{
    (void)v;
    (void)arg;
    if (st != T1_COMPILED)
        return;
    ++observed_compiles;
    observed_ticks = rep->compile_ticks;
}

/* The caller's report, the observer's and the stored region's agree on the
 * compile time of a successful compile (the region's copy was taken before
 * the measurement ended). */
static void t_compile_ticks(const char *image)
{
    struct gather_loop L;
    gather_open(&L, image);
    um_snapshot_restore(&L.snap, L.v);
    struct t1_compile_report rep;
    observed_compiles = 0;
    t1_test_set_compile_observer(on_compile, NULL);
    uint32_t id = umt_compile(L.v, T1_COMPILE_VERIFY, 0, 0, &rep, "compile ticks");
    t1_test_set_compile_observer(NULL, NULL);
    CHECK(id && observed_compiles == 1);
    const struct t1_region *r = t1_region_get(id);
    CHECK(r != NULL);
    if (r->report.compile_ticks != rep.compile_ticks)
        printf(
            "unit_runtime: compile ticks: region compile_ticks %llu, caller %llu, observer %llu\n",
            (unsigned long long)r->report.compile_ticks, (unsigned long long)rep.compile_ticks,
            (unsigned long long)observed_ticks);
    /* compile_ticks is measured only with the statistics
     * gate on (IA64VMM_TIER1_STATS=1); with it off no clock is read and
     * every reader sees 0. */
    CHECK((IA64VMM_TIER1_STATS ? rep.compile_ticks != 0 : rep.compile_ticks == 0) &&
          observed_ticks == rep.compile_ticks);
    CHECK(r->report.compile_ticks == rep.compile_ticks);
    printf(
        "unit_runtime: compile ticks: compile_ticks %llu in the report, the observer and the region\n",
        (unsigned long long)rep.compile_ticks);
    gather_close(&L);
}

/* Compile outcomes by stage (tier1.h struct t1_stats, t1_form_stops): a
 * head whose first instruction is outside the subset is a formation
 * reject, counted under that stage and under the instruction's opcode as a
 * head stop; a region that ends before such an instruction counts it as a
 * region-end stop; a compile that succeeds changes no reject count. */
static void t_reject_stats(const char *image)
{
    struct ienc_bundle head[] = {
        {IENC_STOP(IENC_MII), {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}}};
    um_emit(PROG, head, 1);
    struct vcpu *v = um_vcpu(PROG);
    struct t1_stats before, after;
    t1_get_stats(&before);
    uint64_t heads = t1_form_stops(IA64_OP_BREAK, true), ends = t1_form_stops(IA64_OP_BREAK, false);
    struct t1_compile_report rep;
    CHECK(t1_compile(umt_tv(v), NULL, &rep) == T1_REJECTED);
    t1_get_stats(&after);
    CHECK(after.rejects == before.rejects + 1 && after.compiles == before.compiles);
    for (unsigned k = 0; k < T1_REJECT_STAGES; ++k)
        CHECK(after.rejects_by_stage[k] == before.rejects_by_stage[k] + (k == T1_REJECT_FORM));
    CHECK(t1_form_stops(IA64_OP_BREAK, true) == heads + 1 &&
          t1_form_stops(IA64_OP_BREAK, false) == ends);
    umt_release(v);
    vcpu_destroy(v);

    struct ienc_bundle tail[] = {
        {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_break_m(1)}}};
    um_emit(PROG, tail, 1);
    v = um_vcpu(PROG);
    t1_get_stats(&before);
    CHECK(umt_compile(v, T1_COMPILE_VERIFY, 0, 0, &rep, "stop stats") != 0);
    t1_get_stats(&after);
    CHECK(after.compiles == before.compiles + 1 && after.rejects == before.rejects);
    CHECK(t1_form_stops(IA64_OP_BREAK, true) == heads + 1 &&
          t1_form_stops(IA64_OP_BREAK, false) == ends + 1);
    umt_release(v);
    vcpu_destroy(v);

    struct gather_loop L;
    gather_open(&L, image);
    t1_get_stats(&before);
    CHECK(umt_compile(L.v, 0, 0, 0, &rep, "gather stats") != 0);
    t1_get_stats(&after);
    CHECK(after.compiles == before.compiles + 1 && after.rejects == before.rejects);
    for (unsigned k = 0; k < T1_REJECT_STAGES; ++k)
        CHECK(after.rejects_by_stage[k] == before.rejects_by_stage[k]);
    gather_close(&L);
    printf(
        "unit_runtime: reject statistics: a head outside the subset is one formation reject and one head "
        "stop of its opcode; a region ending there one end stop; compiled regions no reject\n");
}

/* PSR-only variants (region.h t1_region_psr_difference): a live region at
 * v's ip and slot whose contract differs from v's state only in PSR bits
 * names exactly those bits; a full match, another CFM or other compile
 * flags name none. */
static void t_psr_variants(const char *image)
{
    t1_flush();
    struct gather_loop L;
    gather_open(&L, image);
    struct t1_compile_report rep;
    CHECK(umt_compile(L.v, T1_COMPILE_VERIFY, 0, 0, &rep, "PSR variants") != 0);
    unsigned bits = hostwin_bits(umt_hostwin());
    CHECK(t1_region_psr_difference(L.v, T1_COMPILE_VERIFY, bits) == 0);
    L.v->s.psr ^= IA64_PSR_DFH | IA64_PSR_PK;
    CHECK(t1_region_psr_difference(L.v, T1_COMPILE_VERIFY, bits) == (IA64_PSR_DFH | IA64_PSR_PK));
    L.v->s.psr ^= IA64_PSR_DFH | IA64_PSR_PK | IA64_PSR_DI | IA64_PSR_I;
    /* Bits outside the contract make no difference. */
    CHECK(t1_region_psr_difference(L.v, T1_COMPILE_VERIFY, bits) == 0);
    L.v->s.psr ^= IA64_PSR_DI | IA64_PSR_I | IA64_PSR_DFH | IA64_PSR_PK;
    CHECK(t1_region_psr_difference(L.v, 0, bits) == 0);
    uint64_t cfm = L.v->s.cfm;
    L.v->s.cfm ^= 1;
    CHECK(t1_region_psr_difference(L.v, T1_COMPILE_VERIFY, bits) == 0);
    L.v->s.cfm = cfm;
    L.v->s.ip += 16;
    CHECK(t1_region_psr_difference(L.v, T1_COMPILE_VERIFY, bits) == 0);
    gather_close(&L);
    printf(
        "unit_runtime: PSR-only variants name the differing PSR bits; full matches, other frames, flags "
        "or heads none\n");
}

#include "tests/tier1/reserved.inc"
#include "tests/tier1/helperabi.inc"
#include "tests/tier1/raverify_flow.inc"
#include "tests/tier1/regalloc_reentry.inc"
#include "tests/tier1/raverify_clobbers.inc"
#include "tests/tier1/raverify_initial.inc"
#include "tests/tier1/frame_plan.inc"
#include "tests/tier1/restore_native.inc"
#include "tests/tier1/emit_shared_cold.inc"
#include "tests/tier1/memory_reuse.inc"
#include "tests/tier1/loop_metadata_runtime.inc"
#include "tests/tier1/window_loop.inc"
#include "tests/tier1/native_timer.inc"
#include "tests/tier1/native_handoff.inc"
#include "tests/tier1/emit_relative_calls.inc"
#include "tests/tier1/emit_register_cycles.inc"
#include "tests/tier1/retained_fixups.inc"
#include "tests/tier1/packed_wrap.inc"
#include "tests/tier1/guard_progress.inc"
#include "tests/tier1/sidepath_runtime.inc"
#include "tests/tier1/tail_runtime.inc"
#include "tests/tier1/native_pages.inc"
#include "tests/tier1/native_virtual.inc"
#include "tests/tier1/native_chka.inc"
#include "tests/tier1/native_fr.inc"
#include "tests/tier1/native_gr.inc"
#include "tests/tier1/helper_read_homes.inc"

void unit_runtime_test(const char *image);
void unit_runtime_test(const char *image)
{
    umt_init();
    helper_read_homes();
    sidepath_runtime_test();
    tail_runtime_test();
    t_native_restore();
    t_guard_progress();
    regalloc_reentry();
    t_pinned_rax(image);
    t_wrong_old_value(image);
    t_mid_block_exit();
    t_window_tags();
    spill_pressure(image);
    cloop_pressure();
    raverify_negative(image);
    raverify_changed_entry();
    raverify_clobber_bits();
    raverify_initial_locations();
    isel_tests();
    emit_pcopy();
    emit_shared_cold();
    t_memory_reuse();
    t_loop_metadata();
    t_window_loop();
    native_handoff_test();
    native_pages_test();
    emit_register_cycles(NULL);
    static_checks(image);
    t_compile_ticks(image);
    t_reject_stats(image);
    t_psr_variants(image);
    t_stacked_liveins();
    t_frame_fill_nat_guard();
    t_materialize_homes();
    t_collection_entry();
    t_frame_compact();
    t_frame_plan();
    retained_fixup_offsets();
    t_hot_tail();
    t_guarded_hot_tail();
    t_hot_resume();
    t_stitched_outer();
    t_nested();
    t_hot_tables();
    t_stacked_nat_guard();
    t_corrupt_stacked_nat();
    t_frame_rule_stacked();
    t_reserved();
    helper_wrapper();
    helper_allocation();
    emit_relative_calls();
    t_packed_wrap();
    t_packed_consumers();
    printf("unit_runtime: %u checks passed\n", checks);
}
