/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Vol.3 pp.3:35-3:36, formats M22/M23: chk.a may always fail.
 * Unlike chk.s, its register specifier consumes neither a value nor NaT,
 * and the data form does not check the disabled FP banks. */
static uint64_t chka_insn(unsigned qp, bool clear, bool fp, unsigned reg, int64_t disp)
{
    uint64_t imm = (uint64_t)(disp / 16) & 0x1fffff;
    uint64_t raw = qp | (uint64_t)reg << 6 | (imm & 0xfffff) << 13 |
                   (uint64_t)(4 + clear + 2 * fp) << 33 | (imm >> 20) << 36;
    Ia64Instruction in = ia64_decode_insn(IA64_UNIT_M, raw, 0, 0);
    CHECK(in.valid && in.opcode == (clear ? IA64_OP_CHK_A_CLR : IA64_OP_CHK_A));
    CHECK(in.qp == qp && in.check_fp == fp && in.operands.memory.source == reg &&
          in.operands.memory.immediate == disp);
    return raw;
}

static unsigned chka_predicates(void)
{
    const uint64_t head = UM_CODE + 0xb0000;
    const unsigned regs[] = {0, 34, 127, 0, 1, 6, 90};
    unsigned cases = 0;
    for (unsigned kind = 0; kind < sizeof(regs) / sizeof(regs[0]); ++kind)
        for (unsigned clear = 0; clear < 2; ++clear)
            for (unsigned slot = 0; slot < 2; ++slot) {
                bool fp = kind >= 3;
                uint64_t recovery = clear ? head - 0x1000 : head + 0x1000;
                struct ienc_bundle code[] = {
                    {IENC_STOP(IENC_MMI),
                     {ienc_adds(0, 14, 1, 14), ienc_nop_m(), ienc_adds(0, 15, 1, 15)}},
                    {IENC_STOP(IENC_MII), {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}}};
                code[0].slot[slot] =
                    chka_insn(16, clear, fp, regs[kind], (int64_t)(recovery - head));
                const struct ienc_bundle tail[] = {
                    {IENC_STOP(IENC_MII),
                     {ienc_ld(0, 8, false, 18, 33), ienc_adds(0, 17, 1, 17), ienc_nop_i()}},
                    {IENC_STOP(IENC_MII), {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}}};
                um_emit(head, code, 2);
                um_emit(recovery, tail, 2);
                struct vcpu *v = um_vcpu(head), *ref = um_vcpu(head);
                v->s.cfm = um_cfm(8, 8, 8, 3, 5) | (UINT64_C(7) << 25);
                v->s.rse_bof = 95;
                v->s.psr |= IA64_PSR_DFL | IA64_PSR_DFH;
                if (!fp)
                    um_gr(v, regs[kind], UINT64_MAX, true);
                for (unsigned f = 2; f < 128; ++f)
                    v->s.fr[f] = (struct ia64_fr){0, 0x1fffe, false};
                um_gr(v, 33, UM_DATA + 0x900, false);
                uint64_t value = UINT64_C(0x123456789abcdef0);
                memcpy(um_ram(UM_DATA + 0x900), &value, 8);
                uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "chk.a predicate");
                CHECK(id);
                const struct t1_region *r = t1_region_get(id);
                CHECK(r && !r->nhelper && r->ninsns == 3);
                CHECK(!r->fr_guard[0] && !r->fr_guard[1]);
                if (!fp) {
                    struct ia64_state names = v->s;
                    names.rse_bof = 0;
                    unsigned reg = ia64_gr_index(&names, regs[kind]);
                    CHECK(!(r->nat_guard[reg / 64] & (UINT64_C(1) << (reg % 64))));
                }
                struct um_snapshot snap;
                um_snapshot_take(&snap, v);
                struct umt_run run;
                for (unsigned pred = 0; pred < 2; ++pred) {
                    um_pr(&snap.v, 16, pred);
                    for (unsigned budget = 1; budget <= 9; ++budget, ++cases)
                        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, budget, id, NULL, &run, false,
                                 "chk.a predicate/slot/NaT/disabled FR");
                    CHECK(run.nexits && v->s.gr[15] == !pred && v->s.gr[17] == pred);
                    CHECK(v->s.gr[18] == (pred ? value : 0));
                }
                /* A true check still executes the recovery access. A false
                 * check must not consume the recovery base's NaT or fault. */
                for (unsigned pred = 0; pred < 2; ++pred)
                    for (unsigned fault = 0; fault < 2; ++fault, ++cases) {
                        um_pr(&snap.v, 16, pred);
                        um_gr(&snap.v, 33,
                              fault ? UM_RAM_BASE + UM_RAM_SIZE + 4096 : UM_DATA + 0x900, !fault);
                        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 9, id, NULL, &run, false,
                                 "chk.a recovery load fault");
                    }
                um_snapshot_free(&snap);
                umt_release(v);
                vcpu_destroy(v);
                vcpu_destroy(ref);
            }
    return cases;
}

static uint64_t chka_advanced(unsigned r, unsigned base)
{
    uint64_t raw = ienc_ld(0, 8, false, r, base) | (UINT64_C(8) << 30);
    Ia64Instruction in = ia64_decode_insn(IA64_UNIT_M, raw, 0, 0);
    CHECK(in.valid && in.opcode == IA64_OP_LD8A);
    return raw;
}

static void chka_loop_code(uint64_t head, uint64_t recovery)
{
    uint64_t xma = (UINT64_C(0xe) << 37) | (UINT64_C(1) << 36) | (UINT64_C(7) << 27) |
                   (UINT64_C(6) << 20) | (8 << 6);
    uint64_t getf = (UINT64_C(4) << 37) | (UINT64_C(0xe1) << 27) | (UINT64_C(8) << 13) | (21 << 6);
    Ia64Instruction in = ia64_decode_insn(IA64_UNIT_F, xma, 0, 1);
    CHECK(in.valid && in.opcode == IA64_OP_XMA_L);
    in = ia64_decode_insn(IA64_UNIT_M, getf, 0, 0);
    CHECK(in.valid && in.opcode == IA64_OP_GETF_SIG);
    const struct ienc_bundle code[] = {
        {IENC_STOP(IENC_MMI), {chka_advanced(19, 33), ienc_nop_m(), ienc_adds(0, 14, 1, 14)}},
        {IENC_STOP(IENC_MMI),
         {ienc_st(0, 8, false, 33, 34), chka_insn(0, true, false, 19, recovery - head - 16),
          ienc_adds(0, 15, 7, 15)}},
        {IENC_STOP(IENC_MMI), {chka_advanced(20, 36), ienc_nop_m(), ienc_nop_i()}},
        {IENC_STOP(IENC_MMI),
         {ienc_st(0, 8, false, 36, 37), chka_insn(0, false, false, 20, recovery - head + 16),
          ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {metadata_setf(0, 6, 19), ienc_nop_i(), ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {metadata_setf(0, 7, 20), ienc_nop_i(), ienc_nop_i()}},
        {0x0d, {ienc_nop_m(), xma, ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {getf, ienc_nop_i(), ienc_adds(0, 34, 1, 34)}},
        {IENC_STOP(IENC_MIB),
         {ienc_st(0, 8, false, 38, 21), ienc_adds(0, 37, 1, 37), ienc_br_cloop(-128)}},
        {IENC_STOP(IENC_MII), {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}}};
    um_emit(head, code, sizeof(code) / sizeof(code[0]));
    for (unsigned k = 0; k < 2; ++k) {
        uint64_t at = recovery + 64 * k;
        const struct ienc_bundle tail[] = {
            {IENC_STOP(IENC_MII),
             {ienc_ld(0, 8, false, 19 + k, k ? 36 : 39), ienc_adds(0, 35, 1, 35), ienc_nop_i()}},
            {IENC_STOP(IENC_MIB),
             {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, head + 32 + 32 * k - at - 16)}}};
        um_emit(at, tail, 2);
    }
}

static unsigned chka_recovery_loop(void)
{
    const uint64_t head = UM_CODE + 0xb8150, recovery = UM_CODE + 0xb96f0;
    unsigned cases = 0;
    for (unsigned virt = 0; virt < 2; ++virt) {
        chka_loop_code(head, recovery);
        struct vcpu *v = um_vcpu(head), *ref = um_vcpu(head);
        if (virt) {
            um_virtual_code(v);
            v->s.ip |= UINT64_C(1) << 61;
            native_virtual_map(v, 1, v->s.ip & ~UINT64_C(4095), head & ~UINT64_C(4095), 12);
            native_virtual_map(v, 3, (recovery | UINT64_C(1) << 61) & ~UINT64_C(4095),
                               recovery & ~UINT64_C(4095), 12);
            v->s.tr[1][0] = v->s.tr[0][0];
            v->s.tr[1][0].pte |= UINT64_C(3) << 9;
            ia64_mmu_changed(v, IA64_MMU_TR, 1, 0);
            v->s.psr |= IA64_PSR_DT;
        }
        v->s.ar[AR_LC] = 3;
        um_gr(v, 33, UM_DATA + 0xa00, false);
        um_gr(v, 34, 3, false);
        um_gr(v, 36, UM_DATA + 0xa08, false);
        um_gr(v, 37, 5, false);
        um_gr(v, 38, UM_DATA + 0xa10, false);
        um_gr(v, 39, UM_DATA + 0xa00, false);
        uint64_t initial[3] = {1, 2, 0};
        memcpy(um_ram(UM_DATA + 0xa00), initial, sizeof(initial));
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "chk.a recovery loop");
        CHECK(id);
        const struct t1_region *r = t1_region_get(id);
        CHECK(r && r->loop && r->npages == 2 && r->ninsns == 37 && !r->nhelper);
        CHECK(r->report.loop_host_insns);
        uint64_t link = r->link;
        uint32_t sites[2] = {UINT32_MAX, UINT32_MAX};
        for (unsigned p = 0; p < r->npoints; ++p)
            for (unsigned k = 0; k < 2; ++k)
                if (r->point[p].kind == T1_PT_SITE &&
                    r->point[p].ip == ((recovery + 64 * k) | (virt ? UINT64_C(1) << 61 : 0)))
                    sites[k] = p;
        CHECK(sites[0] != UINT32_MAX && sites[1] != UINT32_MAX);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct umt_run run;
        for (unsigned budget = 1; budget <= 149; ++budget, ++cases)
            umt_case(v, ref, &snap, T1_COMPILE_VERIFY, budget, id, NULL, &run, false,
                     "chk.a stitched exact budget");
        for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof, ++cases) {
            snap.v.s.rse_bof = bof;
            um_gr(&snap.v, 33, UM_DATA + 0xa00, false);
            um_gr(&snap.v, 34, bof + 3, false);
            um_gr(&snap.v, 35, 0, false);
            um_gr(&snap.v, 36, UM_DATA + 0xa08, false);
            um_gr(&snap.v, 37, bof + 5, false);
            um_gr(&snap.v, 38, UM_DATA + 0xa10, false);
            um_gr(&snap.v, 39, UM_DATA + 0xa00, false);
            umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 148, id, NULL, &run, false,
                     "chk.a stitched all BOFs");
            uint64_t output;
            memcpy(&output, um_ram(UM_DATA + 0xa10), 8);
            CHECK(run.nexits && v->s.gr[14] == 4 && !v->s.gr[15]);
            CHECK(ia64_gr_read(&v->s, 35) == 8 && output == (bof + 6u) * (bof + 8u));
        }
        const enum t1_force_action actions[] = {T1_FORCE_TRAP_EXIT, T1_FORCE_MISS_EXIT,
                                                T1_FORCE_TRAP_RETRY};
        for (unsigned k = 0; k < 2; ++k)
            for (unsigned a = 0; a < 3; ++a, ++cases) {
                struct umt_arm arm = {.point = sites[k], .occurrence = 2, .action = actions[a]};
                umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 148, id, &arm, &run, false,
                         "chk.a recovery access exit/retry");
                CHECK(run.forced == 1);
            }
        struct vcpu untimed = snap.v;
        for (unsigned fault = 0; fault < 3; ++fault, ++cases) {
            snap.v = untimed;
            um_gr(&snap.v, 39,
                  fault == 1   ? UM_RAM_BASE + UM_RAM_SIZE + 4096
                  : fault == 2 ? UM_DATA + 0xa01
                               : UM_DATA + 0xa00,
                  !fault);
            umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 148, id, NULL, &run, false,
                     "chk.a stitched recovery real fault");
        }
        for (unsigned deadline = 1; deadline <= 75; ++deadline, ++cases) {
            snap.v = untimed;
            snap.v.s.cr[CR_ITV] = 0xef;
            snap.v.s.cr[CR_ITM] = ia64_itc_read(&snap.v) + deadline;
            ia64_timer_program(&snap.v);
            umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 148, id, NULL, &run, false,
                     "chk.a recovery exact timer");
            CHECK(v->timer_fired);
        }
        if (virt)
            for (unsigned change = 0; change < 4; ++change, ++cases) {
                snap.v = untimed;
                struct ia64_tlb_entry *target = &snap.v.s.tr[0][3];
                if (!change)
                    target->pte = native_virtual_pte(head & ~UINT64_C(4095));
                else if (change == 1)
                    target->pte &= ~(UINT64_C(7) << 9);
                else if (change == 2)
                    target->pte &= ~(UINT64_C(1) << 5);
                else
                    target->valid = false;
                ia64_mmu_changed(&snap.v, IA64_MMU_TR, 0, 3);
                umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 148, id, NULL, &run, false,
                         "chk.a recovery fetch remap/permission/accessed/miss");
            }
        /* A source change in the stitched recovery page retires the whole
         * region. The replacement recovery must execute its changed work. */
        snap.v = untimed;
        um_snapshot_restore(&snap, v);
        const struct ienc_bundle changed = {
            IENC_STOP(IENC_MII),
            {ienc_ld(0, 8, false, 19, 39), ienc_adds(0, 35, 5, 35), ienc_nop_i()}};
        um_emit(recovery, &changed, 1);
        memcpy(snap.ram + recovery - UM_RAM_BASE, um_ram(recovery), 16);
        CHECK(!t1_region_from_link(link));
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 148, id, NULL, &run, false,
                 "chk.a recovery code mutation");
        CHECK(ia64_gr_read(&v->s, 35) == 24);
        ++cases;
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    return cases;
}

static void chka_boundaries(void)
{
    const uint64_t head = UM_CODE + 0xba000;
    struct ienc_bundle code[] = {
        {IENC_STOP(IENC_MMI), {chka_insn(0, true, true, 90, 0x5000), ienc_nop_m(), ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}}};
    um_emit(head, code, 2);
    struct vcpu *v = um_vcpu(head);
    v->s.psr |= IA64_PSR_DFL | IA64_PSR_DFH;
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "chk.a unlisted recovery");
    CHECK(id);
    const struct t1_region *r = t1_region_get(id);
    CHECK(r && r->npages == 1 && !r->nhelper);
    bool exits = false;
    for (unsigned k = 0; k < r->npoints; ++k)
        exits |= r->point[k].kind == T1_PT_EXIT && r->point[k].ip == head + 0x5000 &&
                 r->point[k].slot == 0;
    CHECK(exits);
    struct t1_compile_report report;
    v->s.psr |= IA64_PSR_TB;
    CHECK(t1_compile(umt_tv(v), NULL, &report) == T1_REJECTED);
    v->s.psr &= ~IA64_PSR_TB;
    v->s.rse_missing = 1;
    CHECK(t1_compile(umt_tv(v), NULL, &report) == T1_REJECTED);
    v->s.rse_missing = 0;
    /* A supported instruction does not make a reserved template valid. */
    code[0].tmpl = 0x06;
    CHECK(!ia64_template_info(code[0].tmpl)->defined);
    um_emit(head, code, 2);
    CHECK(t1_compile(umt_tv(v), NULL, &report) == T1_REJECTED);
    umt_release(v);
    vcpu_destroy(v);
}

void native_chka_test(void);
void native_chka_test(void)
{
    umt_init();
    (void)chka_insn(0, false, false, 127, -(INT64_C(1) << 24));
    (void)chka_insn(63, true, true, 127, (INT64_C(1) << 24) - 16);
    unsigned cases = chka_predicates() + chka_recovery_loop();
    chka_boundaries();
    printf("unit_runtime: chk.a: %u exact predicate/register/budget/recovery/timer cases, "
           "two-page physical/virtual loops, every BOF and native integer FR multiply\n",
           cases);
}
