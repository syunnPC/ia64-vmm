/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "tests/tier1/sidepath_fixture.h"

static void sidepath_lift_test(void)
{
    const uint64_t head = UM_CODE + 0x70000, side = head + 0x1100;
    sidepath_fixture(head, side);
    struct vcpu *v = um_vcpu(head);
    const char *reason;
    uint64_t low, high, generation;
    /* An untracked side page is not made executable code by speculation. */
    UM_CHECK(!code_page_is_tracked(side));
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 1);
    UM_CHECK(!code_page_is_tracked(side));
    UM_CHECK(code_source_read_physical(side, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 15 && F->npages == 2);
    UM_CHECK(F->insn[2].edge == T1_EDGE_INTERNAL && F->insn[2].target_insn == 6);
    UM_CHECK(F->insn[5].edge == T1_EDGE_BACK && F->insn[14].edge == T1_EDGE_BACK);
    UM_CHECK(F->end_ip == head + 32 && F->end_slot == 0);
    UM_CHECK(F->insn[6].insn.address == side && F->insn[6].leader);
    unsigned fail;
    char err[200];
    UM_CHECK(t1_lift(F, IR, &fail) && t1_ir_verify(IR, 0, err, sizeof(err)));
    unsigned checkpoints = 0, main_exit = 0, side_exit = 0;
    for (unsigned p = 0; p < IR->npoints; ++p) {
        const struct t1_ir_point *pt = &IR->point[p];
        if (pt->dead)
            continue;
        checkpoints += pt->kind == T1_PT_CHECKPOINT && pt->ip == head;
        main_exit += pt->kind == T1_PT_EXIT && pt->ip == head + 32;
        side_exit += pt->kind == T1_PT_EXIT && pt->ip == side + 48;
    }
    UM_CHECK(checkpoints == 2 && main_exit == 1 && side_exit == 1);
    /* The overall instruction budget cannot be expanded by a side path. */
    UM_CHECK(t1_form(v, 0, 14, F, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 1);
    UM_CHECK(t1_form(v, 0, 15, F, &reason) == T1_COMPILED && F->ninsns == 15 && F->npages == 2);
    /* A requested I-side translation is necessary even for a tracked page. */
    struct t1_compile_request rq;
    const struct t1_compile_options opt = {0};
    struct ia64_state *names = calloc(1, sizeof(*names));
    UM_CHECK(names && t1_request_from_vcpu(v, &opt, 0, &rq, &reason));
    rq.npages = 1;
    UM_CHECK(t1_form_request(&rq, names, T1_MAX_INSNS, F, &reason) == T1_COMPILED &&
             F->ninsns == 6 && F->npages == 1);
    free(names);
    /* Reject every path that does not close at its first transfer. An
     * already read extra page must not become a region dependency. */
    const uint64_t bad[] = {ienc_break_m(1), ienc_alloc(0, 35, 8, 0, 0),
                           ienc_br_call(0, 1, (int64_t)(head - side - 32)),
                           ienc_br_cond(8, 16)};
    for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); ++k) {
        sidepath_fixture(head, side);
        struct ienc_bundle reject = k < 2
            ? (struct ienc_bundle){IENC_MII, {bad[k], ienc_nop_i(), ienc_nop_i()}}
            : (struct ienc_bundle){IENC_MIB, {ienc_nop_m(), ienc_nop_i(), bad[k]}};
        um_emit(side + 32, &reject, 1);
        UM_CHECK(code_source_read_physical(side, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
        UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 1);
        UM_CHECK(F->insn[2].edge == T1_EDGE_EXIT && F->end_ip == head + 32);
        UM_CHECK(t1_lift(F, IR, &fail) && t1_ir_verify(IR, 0, err, sizeof(err)));
    }
    /* Twelve decoded instructions fit; a transfer after the bound does
     * not keep any of its speculative instructions or page dependencies. */
    for (unsigned bundles = 4; bundles <= 5; ++bundles) {
        sidepath_fixture(head, side);
        struct ienc_bundle wide[5];
        for (unsigned b = 0; b < bundles; ++b)
            wide[b] = (struct ienc_bundle){IENC_MII,
                {ienc_nop_m(), ienc_adds(0, 16, 1, 16), ienc_nop_i()}};
        wide[bundles - 1] = (struct ienc_bundle){IENC_MIB,
            {ienc_nop_m(), ienc_nop_i(),
             ienc_br_cond(8, (int64_t)(head - side - 16 * (bundles - 1)))}};
        um_emit(side, wide, bundles);
        UM_CHECK(code_source_read_physical(side, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
        UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == (bundles == 4 ? 18 : 6) &&
                 F->npages == (bundles == 4 ? 2 : 1));
    }
    vcpu_destroy(v);
    printf("unit_lift: bounded side path, distinct fallthroughs, new head backedge, "
           "instruction/page bounds and rejection rollback\n");
}
