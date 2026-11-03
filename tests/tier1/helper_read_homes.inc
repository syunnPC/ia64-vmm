/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Save an entry value, expose a temporary through a helper operand, then
 * restore the entry value. Every exit must recover the original home. */
static void helper_read_homes(void)
{
    unsigned cases = 0;
    for (unsigned kind = 0; kind < 4; ++kind)
    for (unsigned initial = 0; initial < 2; ++initial)
    for (unsigned loop = 0; loop < 2; ++loop)
    for (unsigned checked = 0; checked < 2; ++checked) {
        const uint64_t prog = BOF_PROG + 0x1c00;
        unsigned reg = kind == 1 ? 40 : 8;
        struct ienc_bundle code[] = {
            {IENC_STOP(IENC_MII), {ienc_adds(0, 14, 0, reg), ienc_nop_i(), ienc_nop_i()}},
            {IENC_STOP(IENC_MII), {ienc_adds(0, reg, 1, 0), ienc_nop_i(), ienc_nop_i()}},
            {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_mov_to_ar(0, AR_EC, reg), ienc_nop_i()}},
            {IENC_STOP(IENC_MII), {ienc_adds(0, reg, 0, 14), ienc_nop_i(), ienc_nop_i()}},
            {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 22, 33), ienc_nop_m(), ienc_nop_i()}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(),
                                   loop ? ienc_br_cloop(-5 * 16) : ienc_br_cond(0, 0)}},
            {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
        };
        if (kind == 2) {
            code[0].slot[0] = ienc_cmp(6, IENC_EQ, false, true, 8, 0, 0, 0);
            code[1].slot[0] = ienc_cmp(0, IENC_EQ, false, false,
                                     initial ? 0 : 6, initial ? 6 : 0, 0, 0);
            code[2].slot[1] = ienc_mov_to_ar(6, AR_EC, 15);
            code[3].slot[0] = ienc_cmp(8, IENC_EQ, false, true, 6, 0, 0, 0);
        }
        if (kind == 3) {
            uint64_t fmin = (UINT64_C(0x14) << 27) | (UINT64_C(6) << 20) |
                            (UINT64_C(6) << 13) | (UINT64_C(8) << 6);
            Ia64Instruction in = ia64_decode_insn(IA64_UNIT_F, fmin, 0, 1);
            CHECK(in.valid && in.opcode == IA64_OP_FMIN);
            for (unsigned k = 0; k < 4; ++k)
                code[k] = (struct ienc_bundle){0x0d, /* MFI with stop */
                                               {ienc_nop_m(), 0, ienc_nop_i()}};
            code[0].slot[1] = native_fr_merge(0, 9, 6);
            code[1].slot[1] = native_fr_merge(0, 6, 1);
            code[2].slot[1] = fmin;
            code[3].slot[1] = native_fr_merge(0, 6, 9);
        }
        um_emit(prog, code, sizeof(code) / sizeof(code[0]));
        struct vcpu *v = um_vcpu(prog), *ref = um_vcpu(prog);
        v->s.cfm = um_cfm(16, 16, 0, 0, 0);
        v->s.rse_bof = initial ? 95 : 0;
        um_gr(v, reg, UINT64_C(0x2000000000010000), initial != 0);
        um_gr(v, 15, 1, false);
        um_gr(v, 33, UM_DATA, false);
        um_pr(v, 6, initial != 0);
        v->s.fr[6] = initial ? (struct ia64_fr){0, 0x1fffe, false}
                             : (struct ia64_fr){UINT64_C(0x8000000000000000), 0xffff, true};
        v->s.ar[AR_LC] = 3;
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        unsigned flags = checked ? T1_COMPILE_VERIFY : 0;
        uint32_t id = umt_compile(v, flags, 0, 0, NULL, "helper-read-homes");
        CHECK(id && t1_region_get(id)->nhelper == 1);
        CHECK(!loop || t1_region_get(id)->loop);
        char label[80];
        snprintf(label, sizeof(label), "helper-read-homes kind=%u initial=%u loop=%u checked=%u",
                 kind, initial, loop, checked);
        struct umt_run run;
        umt_case(v, ref, &snap, flags, loop ? 90 : 30, id, NULL, &run, false, label);
        CHECK(run.run.entries);
        ++cases;
        if (checked) {
            struct umt_arm arm = {.point = point_at(id, T1_PT_HELPER, 0), .occurrence = 1,
                                  .action = T1_FORCE_HELPER_EXIT};
            umt_case(v, ref, &snap, flags, loop ? 90 : 30, id, &arm, &run, false, label);
            CHECK(run.forced == 1);
            ++cases;
            static const enum t1_force_action actions[] = {
                T1_FORCE_TRAP_EXIT, T1_FORCE_MISS_EXIT, T1_FORCE_TRAP_RETRY};
            for (unsigned a = 0; a < sizeof(actions) / sizeof(actions[0]); ++a) {
                arm = (struct umt_arm){.point = point_at(id, T1_PT_SITE, 0),
                                       .occurrence = loop ? 2 : 1, .action = actions[a]};
                umt_case(v, ref, &snap, flags, loop ? 90 : 30, id, &arm, &run, false, label);
                CHECK(run.forced == 1);
                ++cases;
            }
        }
        um_snapshot_free(&snap);
        umt_release(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    printf("unit_runtime: helper read homes: %u original GR/NaT, wrapped stacked, PR and FR states; natural, helper and memory exits, loops and retries\n", cases);
}
