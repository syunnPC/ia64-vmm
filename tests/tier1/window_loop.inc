/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Two independent load streams, with exact counted-loop retirement. */
#include "engine/tier1/window_loop.h"
#include "engine/tier1/memory.h"
static uint64_t window_loop_rip[2];
static unsigned window_loop_faults[2];
static bool window_loop_trap(struct hostwin_trap *trap)
{
    for (unsigned k = 0; k < 2; ++k)
        window_loop_faults[k] += trap->rip == window_loop_rip[k];
    return t1_trap_client(trap);
}

struct window_loop_arm {
    const struct t1_region *region;
    uint64_t address[2];
    unsigned admit, inject;
    bool mismatch, stale, kick;
};
static void window_loop_before(struct t1_vcpu *tv, struct vcpu *v, void *arg)
{
    const struct window_loop_arm *a = arg;
    hostwin_enter(tv->hw, v);
    for (unsigned k = 0; k < a->admit; ++k) {
        struct hostwin_tag tag;
        CHECK(hostwin_bind(tv->hw, v, a->address[k], &tag) == HOSTWIN_OK);
        tv->x.site[a->region->site_base + k] = tag;
    }
    if (a->mismatch)
        tv->x.site[a->region->site_base + 1].tag ^= 1;
    tv->x.observed_epoch = hostwin_bind_epoch(tv->hw) - a->stale;
    if (a->kick)
        t1_kick(tv);
    hostwin_test_drop(tv->hw, a->address[0]);
    hostwin_test_drop(tv->hw, a->address[0] + 4096);
    hostwin_test_drop(tv->hw, a->address[1]);
    hostwin_test_inject(tv->hw, a->inject);
    memset(window_loop_faults, 0, sizeof(window_loop_faults));
    hostwin_set_trap_client(window_loop_trap);
}

static void window_loop_plan_tests(struct vcpu *v)
{
    struct pipeline p;
    pipeline(v, 0, &p);
    CHECK(t1_isel(p.m, 0) && t1_regalloc(p.m));
    char error[200];
    CHECK(t1_raverify(p.m, error, sizeof(error)));
    struct t1_window_loop plan, rejected;
    CHECK(t1_window_loop_plan(p.m, &plan));
    /* Admission does not specialize unknown trip counts, reversed or
     * misaligned recurrences, effectful loops, or spilled entry addresses. */
    uint32_t flags[] = {T1_COMPILE_VERIFY, T1_COMPILE_STORE_LOG, T1_COMPILE_UNDO_LOG};
    for (unsigned k = 0; k < sizeof(flags) / sizeof(flags[0]); ++k) {
        p.m->flags = flags[k];
        CHECK(!t1_window_loop_plan(p.m, &rejected));
    }
    p.m->flags = 0;
    unsigned constants = 0;
    for (unsigned o = 0; o < p.ir->nops; ++o) {
        struct t1_op *op = &p.ir->op[o];
        if (op->code != T1_IR_CONST || (op->imm != 31 && op->imm != 256))
            continue;
        int64_t saved = op->imm;
        const int64_t invalid[] = {-1, INT64_MAX, 64, 7};
        for (unsigned k = 0; k < sizeof(invalid) / sizeof(invalid[0]); ++k) {
            /* LC=7 is bounded, stride=64 is aligned: both remain valid. */
            if ((saved == 31 && invalid[k] == 7) || (saved == 256 && invalid[k] == 64))
                continue;
            op->imm = invalid[k];
            CHECK(!t1_window_loop_plan(p.m, &rejected));
        }
        op->imm = saved;
        unsigned saved_code = op->code;
        op->code = T1_IR_LIVEIN;
        CHECK(!t1_window_loop_plan(p.m, &rejected));
        op->code = saved_code;
        ++constants;
    }
    CHECK(constants == 2);
    for (unsigned k = 0; k < 2; ++k) {
        unsigned i = p.m->point[plan.access[k].point].insn;
        t1_vreg a = p.m->insn[i].opnd[1].vreg;
        struct t1_loc saved = p.m->vreg[a].header_loc;
        p.m->vreg[a].header_loc = (struct t1_loc){T1_LOC_SPILL, 0, 0, 0};
        CHECK(!t1_window_loop_plan(p.m, &rejected));
        p.m->vreg[a].header_loc = saved;
        unsigned op = p.m->insn[i].op;
        p.m->insn[i].op = T1_MIR_HELPER;
        CHECK(!t1_window_loop_plan(p.m, &rejected));
        p.m->insn[i].op = op;
    }
    uint32_t first = p.m->block[plan.header].first;
    struct t1_mir_insn saved = p.m->insn[first];
    p.m->insn[first].op = T1_X86_JCC + T1_CC_E;
    CHECK(!t1_window_loop_plan(p.m, &rejected));
    p.m->insn[first] = saved;
    CHECK(t1_window_loop_plan(p.m, &rejected));
    unsigned after_site = p.m->point[plan.access[0].point].insn + 1;
    saved = p.m->insn[after_site];
    p.m->insn[after_site].op = T1_X86_JCC + T1_CC_E;
    CHECK(!t1_window_loop_plan(p.m, &rejected));
    p.m->insn[after_site] = saved;
    CHECK(t1_window_loop_plan(p.m, &rejected));
    /* Refusing optional output/state allocations falls back to ordinary
     * emission with unique access RIPs and no partially published clone. */
    for (unsigned nth = 1; nth <= 4; ++nth) {
        struct t1_emitted out;
        unsigned prior = t1_site_base(1, 0);
        unsigned expected = prior + 1 + p.ir->nsites <= T1_SITE_SLOTS ? prior + 1 : 0;
        t1_memory_test_refuse(nth);
        CHECK(t1_emit(p.m, &out, error, sizeof(error)));
        CHECK(!p.ir->failed && !p.m->failed);
        CHECK(out.site_base == expected);
        CHECK(t1_site_base(1, 0) == (expected + p.ir->nsites) % T1_SITE_SLOTS);
        t1_memory_test_refuse(0);
        for (unsigned k = 0; k < 2; ++k) {
            CHECK(out.pt[plan.access[k].point].host_offset);
            CHECK(!out.pt[plan.access[k].point].fast_offset);
        }
        t1_emit_free(&out);
    }
    pipeline_free(&p);
}

static void window_loop_access_rips(uint32_t id)
{
    const struct t1_region *r = t1_region_get(id);
    for (unsigned k = 0; k < 2; ++k) {
        unsigned point = point_at(id, T1_PT_SITE, k), found = 0;
        for (unsigned q = 0; q < r->nrip; ++q)
            if (r->rip[q].point == point && r->rip[q].kind == T1_RIP_ACCESS &&
                r->rip[q].offset != r->point[point].host_offset) {
                window_loop_rip[k] = (uintptr_t)r->code + r->rip[q].offset;
                ++found;
            }
        CHECK(found == 1);
    }
}

static void t_window_loop(void)
{
    const uint64_t prog = UM_CODE + 0x7000;
    struct ienc_bundle code[] = {
        {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_adds(0, 14, 31, 0), ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_mov_to_ar(0, AR_LC, 14), ienc_nop_i()}},
        {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 17, 32),
                                ienc_ld_inc_imm(0, 8, false, 18, 33, 8), ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {ienc_adds(0, 32, 256, 32),
                                ienc_alu(0, IENC_ADD, 20, 17, 18), ienc_nop_i()}},
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_alu(0, IENC_ADD, 21, 21, 20),
                                ienc_br_cloop(-2 * 16)}},
        {IENC_STOP(IENC_MII), {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}},
    };
    um_emit(prog, code, sizeof(code) / sizeof(code[0]));
    struct vcpu *v = um_vcpu(prog), *ref = um_vcpu(prog);
    v->s.cfm = um_cfm(64, 64, 0, 0, 0);
    v->s.rse_bof = 95;
    uint64_t address[] = {UM_DATA + 0x20000, UM_DATA + 0x28000};
    um_gr(v, 32, address[0], false);
    um_gr(v, 33, address[1], false);
    um_gr(v, 21, UINT64_MAX, false);
    for (unsigned k = 0; k < 1024; ++k) {
        uint64_t x = rnd64(), y = rnd64();
        memcpy(um_ram(address[0] + 8 * k), &x, 8);
        memcpy(um_ram(address[1] + 8 * k), &y, 8);
    }
    code_notify_vmm_write(address[0], 8192);
    code_notify_vmm_write(address[1], 8192);
    window_loop_plan_tests(v);
    uint32_t id = umt_compile(v, 0, 0, 0, NULL, "window loop");
    CHECK(id);
    const struct t1_region *r = t1_region_get(id);
    CHECK(r->nsites == 2 && !r->nhelper);
    window_loop_access_rips(id);
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    for (unsigned test = 0; test < 4; ++test) {
        struct window_loop_arm data = {.region = r, .address = {address[0], address[1]},
            .admit = test ? 2 : 0, .inject = test > 1 ? test - 1 : 0};
        struct umt_arm arm = {.point = UINT32_MAX, .before_run = window_loop_before,
                              .run_arg = &data};
        struct umt_run run;
        umt_case(v, ref, &snap, 0, 400, id, &arm, &run, false, "window loop recovery");
        hostwin_set_trap_client(t1_trap_client);
        hostwin_test_inject(umt_hostwin(), 0);
        if (!test)
            CHECK(!window_loop_faults[0] && !window_loop_faults[1]);
        else if (test == 1)
            CHECK(window_loop_faults[0] >= 2 && window_loop_faults[1] >= 1);
        else
            CHECK(window_loop_faults[test - 2] >= 1);
    }
    /* Move the actual compiled region bytes within rel32 range, patching
     * only retained external fixups as the arena does. Local clone branches
     * and both RIP records remain offsets. Retry must stay in the moved copy. */
    struct t1_region *moved = (struct t1_region *)r;
    uint8_t *original = moved->code;
    t1_entry_fn original_entry = moved->entry;
    size_t pages = (r->code_size + 4095) / 4096;
    uint8_t *copy = t1_arena_alloc(pages);
    CHECK(copy && copy != original);
    memcpy(copy, original, r->code_size);
    for (unsigned k = 0; k < r->nfix; ++k) {
        uintptr_t target = t1_stub.exit_save + (intptr_t)r->fix[k].stub_offset;
        int64_t delta = (int64_t)target - (int64_t)((uintptr_t)copy + r->fix[k].at + 4);
        int32_t rel = (int32_t)delta;
        CHECK(delta == rel);
        memcpy(copy + r->fix[k].at, &rel, 4);
    }
    hal_code_set_executable(copy, pages);
    moved->code = copy;
    moved->entry = (t1_entry_fn)(uintptr_t)copy;
    window_loop_access_rips(id);
    for (unsigned inject = 0; inject < 3; ++inject) {
        struct window_loop_arm data = {.region = r,
            .address = {address[0], address[1]}, .admit = 2, .inject = inject};
        struct umt_arm arm = {.point = UINT32_MAX, .before_run = window_loop_before,
                              .run_arg = &data};
        struct umt_run run;
        umt_case(v, ref, &snap, 0, 400, id, &arm, &run, false, "window loop relocated");
        hostwin_set_trap_client(t1_trap_client);
        hostwin_test_inject(umt_hostwin(), 0);
        CHECK(window_loop_faults[0] >= 1);
        if (!inject)
            CHECK(window_loop_faults[0] >= 2 && window_loop_faults[1] >= 1);
    }
    moved->code = original;
    moved->entry = original_entry;
    window_loop_access_rips(id);
    t1_arena_free(copy, pages);
    struct ia64_state initial = snap.v.s;
    um_snapshot_free(&snap);
    unsigned cases = 7;
    /* Retirement cuts through either version, every stacked-register BOF,
     * admission failures, NaT rejection and checkpoint notification. */
    for (unsigned test = 0; test < 110; ++test) {
        v->s = initial;
        v->retired = 0;
        v->s.rse_bof = test % 96;
        uint64_t a = address[0], b = address[1];
        if (test == 96)
            ++a;
        if (test == 97)
            ++b;
        um_gr(v, 32, a, test == 98);
        um_gr(v, 33, b, test == 99);
        um_snapshot_take(&snap, v);
        struct window_loop_arm data = {.region = r, .address = {a, b}, .admit = 2,
            .mismatch = test == 100, .stale = test == 101, .kick = test == 102};
        if (test == 103)
            data.admit = 1;
        struct umt_arm arm = {.point = UINT32_MAX, .before_run = window_loop_before,
                              .run_arg = &data};
        struct umt_run run;
        const unsigned budgets[] = {1, 6, 17, 40, 100, 285, 295, 400};
        unsigned budget = test < 96 ? budgets[test % 8] : 400;
        umt_case(v, ref, &snap, 0, budget, id, &arm, &run, false, "window loop cuts");
        hostwin_set_trap_client(t1_trap_client);
        if (test >= 96 && test <= 101)
            CHECK(!window_loop_faults[0] && !window_loop_faults[1]);
        um_snapshot_free(&snap);
        ++cases;
    }
    /* Endpoints that cross a window or wrap modulo 2^64 must select the
     * original loop. The translations are valid on both sides, so skipping
     * a later access or fault cannot hide behind an invalid first address. */
    for (unsigned test = 0; test < 3; ++test) {
        v->s = initial;
        v->retired = 0;
        v->s.psr |= IA64_PSR_DT;
        uint64_t end = test == 2 ? 0 : UINT64_C(1) << hostwin_bits(umt_hostwin());
        uint64_t a = end - (test == 1 ? 0x4000 : 0x1000), b = end - 0x8000;
        v->s.rr[0] = v->s.rr[7] = (UINT64_C(0x321) << 8) | (12 << 2);
        uint64_t va[] = {a, a + 4096, b};
        uint64_t pa[] = {address[0], address[0] + 4096, address[1]};
        for (unsigned k = 0; k < 3; ++k)
            v->s.tr[1][k] = (struct ia64_tlb_entry){.va = va[k],
                .pte = pa[k] | 1 | (1u << 5) | (1u << 6), .itir = 12 << 2,
                .rid = 0x321, .page_shift = 12, .valid = true};
        ia64_mmu_changed(v, IA64_MMU_TR, 1, 0);
        um_gr(v, 32, a, false);
        um_gr(v, 33, b, false);
        id = umt_compile(v, 0, 0, 0, NULL, "window loop virtual");
        CHECK(id);
        r = t1_region_get(id);
        window_loop_access_rips(id);
        um_snapshot_take(&snap, v);
        struct window_loop_arm data = {.region = r, .address = {a, b}, .admit = 2};
        struct umt_arm arm = {.point = UINT32_MAX, .before_run = window_loop_before,
                              .run_arg = &data};
        struct umt_run run;
        umt_case(v, ref, &snap, 0, 400, id, &arm, &run, false, "window loop endpoints");
        hostwin_set_trap_client(t1_trap_client);
        if (test != 1)
            CHECK(!window_loop_faults[0] && !window_loop_faults[1]);
        else
            CHECK(window_loop_faults[0] >= 2 && window_loop_faults[1] >= 1);
        um_snapshot_free(&snap);
        ++cases;
    }
    /* The supported integer widths retain their full access, including
     * the one-iteration LC=0 case and the largest admitted trip count. */
    for (unsigned bytes = 1; bytes <= 8; bytes *= 2)
        for (unsigned lc = 0; lc <= 63; lc += 63) {
            code[0].slot[1] = ienc_adds(0, 14, (int)lc, 0);
            code[2].slot[0] = ienc_ld(0, bytes, false, 17, 32);
            code[2].slot[1] = ienc_ld_inc_imm(0, bytes, false, 18, 33, 8);
            um_emit(prog, code, sizeof(code) / sizeof(code[0]));
            v->s = initial;
            v->retired = 0;
            id = umt_compile(v, 0, 0, 0, NULL, "window loop widths");
            CHECK(id);
            r = t1_region_get(id);
            window_loop_access_rips(id);
            um_snapshot_take(&snap, v);
            struct window_loop_arm data = {.region = r,
                .address = {address[0], address[1]}, .admit = 2};
            struct umt_arm arm = {.point = UINT32_MAX, .before_run = window_loop_before,
                                  .run_arg = &data};
            struct umt_run run;
            umt_case(v, ref, &snap, 0, 700, id, &arm, &run, false, "window loop widths");
            hostwin_set_trap_client(t1_trap_client);
            CHECK(window_loop_faults[0] >= 1 && window_loop_faults[1] >= 1);
            um_snapshot_free(&snap);
            ++cases;
        }
    code[0].slot[1] = ienc_adds(0, 14, 31, 0);
    /* A cached window grants no guest permission. Keep the first load's
     * completed value and the second store's exact before-effect fault.
     * With the first translation absent, no later store may run early. */
    code[2].slot[1] = ienc_st_inc_imm(0, 8, false, 33, 18, 8);
    um_emit(prog, code, sizeof(code) / sizeof(code[0]));
    for (unsigned test = 0; test < 2; ++test) {
        v->s = initial;
        v->retired = 0;
        v->s.psr |= IA64_PSR_DT;
        v->s.rr[0] = (UINT64_C(0x321) << 8) | (12 << 2);
        for (unsigned k = 0; k < 2; ++k)
            v->s.tr[1][k] = (struct ia64_tlb_entry){.va = address[k],
                .pte = address[k] | (test && !k ? 0 : 1) | (1u << 5) | (1u << 6),
                .itir = 12 << 2, .rid = 0x321, .page_shift = 12, .valid = true};
        ia64_mmu_changed(v, IA64_MMU_TR, 1, 0);
        um_gr(v, 18, UINT64_C(0x0123456789abcdef), false);
        id = umt_compile(v, 0, 0, 0, NULL, "window loop permission");
        CHECK(id);
        r = t1_region_get(id);
        window_loop_access_rips(id);
        um_snapshot_take(&snap, v);
        struct window_loop_arm data = {.region = r,
            .address = {address[0], address[1]}, .admit = 2};
        struct umt_arm arm = {.point = UINT32_MAX, .before_run = window_loop_before,
                              .run_arg = &data};
        struct umt_run run;
        umt_case(v, ref, &snap, 0, 400, id, &arm, &run, false, "window loop permission order");
        hostwin_set_trap_client(t1_trap_client);
        CHECK(window_loop_faults[0] >= 1);
        CHECK(test ? !window_loop_faults[1] : window_loop_faults[1] >= 1);
        um_snapshot_free(&snap);
        ++cases;
    }
    printf("unit_runtime: window loop: %u exact admission, retry, cut and 96-BOF cases; bounded planner and allocation fallback\n", cases);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}
