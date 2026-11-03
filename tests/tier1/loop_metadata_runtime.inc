/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Vol.3 setf.sig/xma.l, with recovery before and after every write.
 * Instruction fields are cross-checked by the decoder; interp_run is the
 * architectural oracle, including NaTVal, predication and PSR.MF. */
static uint64_t metadata_setf(unsigned qp, unsigned f, unsigned r)
{
    uint64_t raw = (UINT64_C(6) << 37) | (UINT64_C(0xe1) << 27) |
                   ((uint64_t)r << 13) | (f << 6) | qp;
    Ia64Instruction in = ia64_decode_insn(IA64_UNIT_M, raw, 0, 0);
    CHECK(in.valid && in.opcode == IA64_OP_SETF_SIG && in.qp == qp);
    CHECK(in.operands.floating.destination == f && in.operands.floating.source1 == r);
    return raw;
}

static void t_loop_metadata(void)
{
    const uint64_t prog = UM_CODE + 0x6800;
    const uint64_t xma = (UINT64_C(0xe) << 37) | (UINT64_C(1) << 36) |
                         (UINT64_C(10) << 27) | (UINT64_C(9) << 20) | (6 << 13) | (6 << 6) | 6;
    Ia64Instruction in = ia64_decode_insn(IA64_UNIT_F, xma, 0, 1);
    CHECK(in.valid && in.opcode == IA64_OP_XMA_L && in.qp == 6);
    CHECK(in.operands.floating.destination == 6 && in.operands.floating.source1 == 6 &&
          in.operands.floating.source2 == 9 && in.operands.floating.auxiliary1 == 10);
    unsigned runs = 0;
    for (unsigned mode = 0; mode < 3; ++mode) {
        struct ienc_bundle code[] = {
            {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 19, 32), ienc_nop_m(), ienc_nop_i()}},
            {IENC_STOP(IENC_MII), {metadata_setf(mode == 1 ? 8 : 0, 6, mode == 1 ? 41 : 0),
                                    ienc_nop_i(), ienc_nop_i()}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(7, 7 * 16)}},
            {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 17, 33), ienc_nop_m(), ienc_nop_i()}},
            {IENC_STOP(IENC_MII), {metadata_setf(0, 9, 17), ienc_nop_i(), ienc_nop_i()}},
            {IENC_STOP(IENC_MII), {metadata_setf(0, 10, 18), ienc_nop_i(), ienc_nop_i()}},
            {0x0d, {ienc_nop_m(), xma, ienc_nop_i()}}, /* MFI, end stop */
            {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 20, 34), ienc_nop_m(), ienc_nop_i()}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(-5 * 16)}},
            {IENC_STOP(IENC_MII), {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}},
        };
        if (mode == 2) {
            uint64_t raw = UINT64_C(0xc6c0000000) | (UINT64_C(34) << 20) | (6 << 6);
            in = ia64_decode_insn(IA64_UNIT_M, raw, 0, 0);
            CHECK(in.valid && in.opcode == IA64_OP_LDF_FILL);
            code[3].slot[0] = raw;
        }
        um_emit(prog, code, sizeof(code) / sizeof(code[0]));
        struct vcpu *v = um_vcpu(prog), *ref = um_vcpu(prog);
        v->s.cfm = um_cfm(64, 64, 0, 0, 0);
        um_pr(v, 6, true);
        um_pr(v, 7, false);
        um_pr(v, 8, true);
        v->s.ar[AR_LC] = 4;
        uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "loop metadata");
        CHECK(id);
        const struct t1_region *r = t1_region_get(id);
        CHECK(r->report.loop_host_insns && r->nhelper == (mode == 2));
        struct ia64_state initial = v->s;
        for (unsigned bof = 0; bof < 96; ++bof) {
            v->s = initial;
            v->s.ip = prog;
            v->s.slot = 0;
            v->retired = 0;
            v->s.rse_bof = bof;
            v->s.ar[AR_LC] = bof % 5;
            v->s.psr = IA64_PSR_IC | ((bof & 1) ? IA64_PSR_MFH : 0);
            um_pr(v, 6, bof & 2);
            um_pr(v, 7, bof % 11 == 0);
            um_pr(v, 8, bof & 4);
            um_gr(v, 32, UM_DATA, false);
            um_gr(v, 33, UM_DATA + 16, false);
            um_gr(v, 34, UM_DATA + 32, false);
            um_gr(v, 41, UINT64_MAX - bof, (bof & 8) && bof != 14 && bof != 30);
            um_gr(v, 17, UINT64_MAX - bof, false);
            um_gr(v, 18, UINT64_C(0x8000000000000001) + bof, false);
            v->s.fr[6] = (struct ia64_fr){bof & 16 ? 0 : UINT64_MAX,
                                         bof & 16 ? 0x1fffe : 0x1003e, false};
            v->s.fr[9] = (struct ia64_fr){rnd64(), rnd64() & 0x1ffff, (bool)(bof & 1)};
            v->s.fr[10] = (struct ia64_fr){rnd64(), rnd64() & 0x1ffff, (bool)(bof & 2)};
            uint64_t memory[] = {rnd64(), 0, UINT64_MAX - bof, 0, 0, 0x1fffe};
            memcpy(um_ram(UM_DATA), memory, sizeof(memory));
            code_notify_vmm_write(UM_DATA, sizeof(memory));
            struct um_snapshot snap;
            um_snapshot_take(&snap, v);
            struct umt_run run;
            umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 150, id, NULL, &run, false,
                     "loop metadata state");
            ++runs;
            if (bof == 14 || bof == 30) {
                for (unsigned q = 0; q < r->npoints; ++q) {
                    unsigned kind = r->point[q].kind;
                    if (kind != T1_PT_SITE && kind != T1_PT_HELPER)
                        continue;
                    unsigned actions = kind == T1_PT_HELPER ? 1 : 3;
                    for (unsigned action = 0; action < actions; ++action) {
                        const enum t1_force_action acts[] = {T1_FORCE_TRAP_EXIT,
                            T1_FORCE_MISS_EXIT, T1_FORCE_TRAP_RETRY};
                        struct umt_arm arm = {.point = q, .occurrence = bof == 14 ? 1 : 2,
                            .action = kind == T1_PT_HELPER ? T1_FORCE_HELPER_EXIT : acts[action]};
                        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 150, id, &arm, &run, false,
                                 "loop metadata forced recovery");
                        /* A prefix-only site is reached just once. */
                        if (arm.occurrence == 1)
                            CHECK(run.forced == 1);
                        ++runs;
                    }
                }
                struct umt_arm arm = {.point = UINT32_MAX, .before_run = kick_before};
                umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 150, id, &arm, &run, false,
                         "loop metadata checkpoint");
                ++runs;
            }
            um_snapshot_free(&snap);
        }
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    printf("unit_runtime: loop metadata: %u exact state/recovery cases, 96 BOFs, NaTVal, predication and helper writes\n", runs);
}
