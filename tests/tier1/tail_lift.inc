/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "tests/tier1/tail_fixture.h"
#include "ia64/mmu.h"
#include "ia64/tlb.h"

static void tail_request_test(void)
{
    const uint64_t physical_head = UM_CODE + 0x7c000, physical_tail = physical_head - 0x1100;
    const uint64_t head = (UINT64_C(2) << 61) + 0x1000, tail = head - 0x1100;
    tail_fixture(physical_head, physical_tail);
    struct vcpu *v = um_vcpu(head);
    tail_virtual_pages(v, head, physical_head, tail, physical_tail);
    const char *reason;
    const struct t1_compile_options opt = {0};
    struct t1_compile_request rq;
    struct ia64_state *names = calloc(1, sizeof(*names));
    static struct vcpu original, before;
    original = *v;
    uint64_t low, high, generation;
    /* Translation capture itself must not read or start tracking a target. */
    UM_CHECK(!code_page_is_tracked(physical_tail));
    UM_CHECK(t1_request_from_vcpu(v, &opt, 0, &rq, &reason));
    UM_CHECK(!code_page_is_tracked(physical_tail));
    UM_CHECK(names && t1_form_request(&rq, names, T1_MAX_INSNS, F, &reason) == T1_COMPILED &&
             F->ninsns == 6 && F->npages == 1 && !code_page_is_tracked(physical_tail));
    UM_CHECK(code_source_read_physical(physical_tail, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
    for (unsigned change = 0; change < 10; ++change) {
        *v = original;
        tail_virtual_change(v, change, tail);
        before = *v;
        UM_CHECK(t1_request_from_vcpu(v, &opt, 0, &rq, &reason));
        UM_CHECK(!memcmp(&v->s, &before.s, sizeof(v->s)) && v->mmu_seq == before.mmu_seq &&
                 v->mmu_iseq == before.mmu_iseq &&
                 !memcmp(v->mmu_log, before.mmu_log, sizeof(v->mmu_log)));
        UM_CHECK(t1_form_request(&rq, names, T1_MAX_INSNS, F, &reason) == T1_COMPILED &&
                 F->ninsns == (change ? 6u : 9u) && F->npages == (change ? 1u : 2u));
        if (!change) {
            UM_CHECK(F->page[1].pa == (physical_tail & ~UINT64_C(4095)) &&
                     F->insn[6].insn.address == tail && F->insn[6].bundle_pa == physical_tail &&
                     F->insn[5].edge == T1_EDGE_INTERNAL && F->insn[5].target_insn == 6);
            UM_CHECK(t1_form_request(&rq, names, 8, F, &reason) == T1_COMPILED &&
                     F->ninsns == 6 && F->npages == 1);
        }
    }
    /* Even a valid VHPT entry must not be walked for a future target. */
    *v = original;
    v->s.psr |= IA64_PSR_DT;
    v->s.rr[tail >> 61] |= 1;
    v->s.cr[CR_PTA] = (UM_DATA + 0x20000) | (15u << 2) | 0x101;
    v->s.tr[1][0] = (struct ia64_tlb_entry){.va = UM_DATA + 0x20000,
        .pte = (UM_DATA + 0x20000) | 1 | (1u << 5) | (1u << 6),
        .itir = 15u << 2, .rid = 0x100, .page_shift = 15, .valid = true};
    uint64_t entry[] = {v->s.tr[0][2].pte, v->s.tr[0][2].itir, ia64_ttag(&v->s, tail), 0};
    um_load(ia64_thash(&v->s, tail), entry, sizeof(entry));
    v->s.tr[0][2].valid = false;
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 2);
    ia64_mmu_changed(v, IA64_MMU_TR, 1, 0);
    ia64_mmu_changed(v, IA64_MMU_RR, 0, tail >> 61);
    before = *v;
    UM_CHECK(t1_request_from_vcpu(v, &opt, 0, &rq, &reason));
    UM_CHECK(!memcmp(&v->s, &before.s, sizeof(v->s)) && v->mmu_seq == before.mmu_seq &&
             v->mmu_iseq == before.mmu_iseq &&
             !memcmp(v->mmu_log, before.mmu_log, sizeof(v->mmu_log)));
    UM_CHECK(t1_form_request(&rq, names, T1_MAX_INSNS, F, &reason) == T1_COMPILED &&
             F->ninsns == 6 && F->npages == 1);
    struct ia64_translation translated;
    UM_CHECK(ia64_translate(v, tail, IA64_REF_FETCH, IA64_ACCESS_EXECUTE, 0, false,
                           &translated).kind == IA64_STEP_COMPLETE &&
             translated.pa == physical_tail && v->mmu_seq > before.mmu_seq);
    /* A resident TC translation is sufficient too. */
    *v = original;
    v->s.tc[0][0] = v->s.tr[0][2];
    v->s.tr[0][2].valid = false;
    ia64_mmu_changed(v, IA64_MMU_TC, 0, 0);
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 2);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 9 && F->npages == 2);
    *v = original;
    UM_CHECK(t1_request_from_vcpu(v, &opt, 0, &rq, &reason));
    /* An obsolete target snapshot cannot be revived by a saved request. */
    code_notify_vmm_write(physical_tail, 16);
    UM_CHECK(!code_page_is_tracked(physical_tail));
    UM_CHECK(t1_form_request(&rq, names, T1_MAX_INSNS, F, &reason) == T1_COMPILED &&
             F->ninsns == 6 && F->npages == 1 && !code_page_is_tracked(physical_tail));
    UM_CHECK(code_source_read_physical(physical_tail, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
    /* A new source branch cannot reuse a different captured target page. */
    struct ienc_bundle changed = {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(),
        ienc_br_cond(0, (int64_t)(tail - head - 16 - 4096))}};
    um_emit(physical_head + 16, &changed, 1);
    UM_CHECK(t1_form_request(&rq, names, T1_MAX_INSNS, F, &reason) == T1_COMPILED &&
             F->ninsns == 6 && F->npages == 1 && F->insn[5].target == tail - 4096);
    /* A captured target still has to satisfy the discriminator shape. */
    tail_fixture(physical_head, physical_tail);
    changed.slot[2] = ienc_br_cond(0, 0);
    um_emit(physical_tail, &changed, 1);
    UM_CHECK(code_source_read_physical(physical_tail, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 1);
    free(names);
    vcpu_destroy(v);
    printf("unit_lift: separate virtual tail mappings: resident rights/key proof, state preservation and bounded snapshots\n");
}

static void tail_lift_test(void)
{
    tail_request_test();
    const uint64_t head = UM_CODE + 0x6a000, tail = head - 0x1100;
    tail_fixture(head, tail);
    struct vcpu *v = um_vcpu(head);
    v->s.cfm = um_cfm(8, 8, 8, 5, 7);
    const char *reason;
    uint64_t low, high, generation;
    UM_CHECK(!code_page_is_tracked(tail));
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 1);
    UM_CHECK(!code_page_is_tracked(tail));
    UM_CHECK(code_source_read_physical(tail, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 9 && F->npages == 2 && !F->loop);
    UM_CHECK(F->insn[2].edge == T1_EDGE_EXIT && F->insn[2].target == head + 0x100);
    UM_CHECK(F->insn[5].edge == T1_EDGE_INTERNAL && F->insn[5].target_insn == 6);
    UM_CHECK(F->insn[6].leader && F->insn[8].edge == T1_EDGE_EXIT &&
             F->insn[8].target == tail - 0x80);
    UM_CHECK(F->end_ip == tail + 16 && !F->end_slot && F->end_class == T1_END_LIMIT);
    UM_CHECK(F->insn[7].p1 == 23 && F->insn[7].p2 == 24 && F->insn[8].qp == 23);
    unsigned fail;
    char err[200];
    UM_CHECK(t1_lift(F, IR, &fail) && t1_ir_verify(IR, 0, err, sizeof(err)));
    unsigned exits = 0;
    for (unsigned p = 0; p < IR->npoints; ++p)
        if (!IR->point[p].dead && IR->point[p].kind == T1_PT_EXIT) {
            UM_CHECK(IR->point[p].ip == head + 0x100 || IR->point[p].ip == tail - 0x80 ||
                     IR->point[p].ip == tail + 16);
            ++exits;
        }
    UM_CHECK(exits == 3);
    UM_CHECK(t1_form(v, 0, 8, F, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 1);
    UM_CHECK(t1_form(v, 0, 9, F, &reason) == T1_COMPILED && F->ninsns == 9 && F->npages == 2);
    v->s.ip |= UINT64_C(1) << 63;
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 9 &&
             F->page[1].pa == (tail & ~UINT64_C(4095)) &&
             F->end_ip == ((tail + 16) | (UINT64_C(1) << 63)));
    v->s.ip = head;
    /* A snapshot invalidated without remaining coverage must not be
     * revived by speculative formation of an unlisted target. */
    code_notify_vmm_write(tail, 16);
    UM_CHECK(code_page_has_snapshot(tail & ~UINT64_C(4095)) && !code_page_is_tracked(tail));
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 1);
    UM_CHECK(!code_page_is_tracked(tail));
    UM_CHECK(code_source_read_physical(tail, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
    um_virtual_code(v);
    uint64_t seq = v->mmu_seq;
    static struct vcpu unchanged;
    unchanged = *v;
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 9 && F->npages == 2 &&
             v->mmu_seq == seq && !memcmp(&v->s, &unchanged.s, sizeof(v->s)));
    UM_CHECK(F->page[1].pa == (tail & ~UINT64_C(4095)));
    /* A captured 4 KiB translation does not prove a preceding page, even
     * when that page has an unrelated cached I-TLB entry and snapshot. */
    struct t1_compile_request rq;
    const struct t1_compile_options opt = {0};
    struct ia64_state *names = calloc(1, sizeof(*names));
    UM_CHECK(names && t1_request_from_vcpu(v, &opt, 0, &rq, &reason));
    for (unsigned p = 0; p < rq.npages; ++p)
        rq.page[p].shift = 12;
    UM_CHECK(t1_form_request(&rq, names, T1_MAX_INSNS, F, &reason) == T1_COMPILED &&
             F->ninsns == 6 && F->npages == 1);
    free(names);
    v->s.psr &= ~IA64_PSR_IT;
    /* A failed probe retains neither partial instructions nor a page. */
    for (unsigned bad = 0; bad < 7; ++bad) {
        tail_fixture(head, tail);
        struct ienc_bundle b = {IENC_STOP(IENC_MIB), {ienc_nop_m(),
            ienc_cmp(10, IENC_LTU, false, false, 16, 17, 34, 35), ienc_br_cond(16, -0x80)}};
        if (bad == 0)
            b.slot[0] = ienc_ld(0, 8, false, 14, 33);
        else if (bad == 1)
            b.slot[0] = ienc_alloc(0, 35, 8, 0, 0);
        else if (bad == 2)
            b.slot[0] = ienc_break_m(0);
        else if (bad == 3)
            b.slot[2] = ienc_br_cond(0, -0x80);
        else if (bad == 4)
            b.slot[2] = ienc_br_cond(16, (int64_t)(head - tail));
        else if (bad == 5)
            b.slot[2] = ienc_br_cond(16, 0);
        else
            b.tmpl = 0x06; /* undefined template */
        um_emit(tail, &b, 1);
        UM_CHECK(code_source_read_physical(tail, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
        UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 1 &&
                 F->insn[5].edge == T1_EDGE_EXIT && F->insn[5].target == tail);
        UM_CHECK(t1_lift(F, IR, &fail) && t1_ir_verify(IR, 0, err, sizeof(err)));
    }
    /* An integer ALU before the terminal branch remains supported. */
    tail_fixture(head, tail);
    struct ienc_bundle alu = {IENC_STOP(IENC_MIB), {ienc_adds(11, 36, 3, 36),
        ienc_cmp(10, IENC_LTU, false, false, 16, 17, 34, 35), ienc_br_cond(16, -0x80)}};
    um_emit(tail, &alu, 1);
    UM_CHECK(code_source_read_physical(tail, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 9 && F->insn[6].cls == T1_IC_ALU);
    /* A prefix that changes its register-stack frame stays at its old
     * physical branch boundary. */
    struct ienc_bundle alloc = {IENC_STOP(IENC_MIB), {ienc_alloc(0, 35, 8, 8, 1),
        ienc_nop_i(), ienc_br_cond(9, 0x100)}};
    um_emit(head, &alloc, 1);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 1);
    struct ienc_bundle helper = {IENC_STOP(IENC_MIB), {ienc_nop_m(),
        ienc_tnat(0, 0, 0, 1, 16, 17, 34), ienc_br_cond(9, 0x100)}};
    um_emit(head, &helper, 1);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 1);
    /* Existing source-page capacity remains a hard bound. */
    for (unsigned p = 0; p < T1_MAX_PAGES; ++p) {
        uint64_t at = head + 4096u * p;
        uint64_t target = p + 1 == T1_MAX_PAGES ? tail : at + 4096;
        struct ienc_bundle page[] = {
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(),
                ienc_br_cond(0, (int64_t)(target - at))}},
            {IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}}
        };
        um_emit(at, page, 2);
    }
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 3 * T1_MAX_PAGES &&
             F->npages == T1_MAX_PAGES);
    /* Falling off a tail at the preceding page's last bundle exits to
     * the original head; its physical adjacency must not create a loop. */
    tail_fixture(head, head - 16);
    UM_CHECK(code_source_read_physical(head - 16, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 9 && F->npages == 2 && !F->loop &&
             F->end_ip == head && F->insn[8].next_ip == head);
    UM_CHECK(t1_lift(F, IR, &fail) && t1_ir_verify(IR, 0, err, sizeof(err)));
    unsigned head_exits = 0;
    for (unsigned p = 0; p < IR->npoints; ++p)
        if (!IR->point[p].dead) {
            UM_CHECK(IR->point[p].kind != T1_PT_CHECKPOINT);
            head_exits += IR->point[p].kind == T1_PT_EXIT && IR->point[p].ip == head;
        }
    UM_CHECK(head_exits == 1 && !count_ops(IR, T1_IR_BACKEDGE));
    vcpu_destroy(v);
    printf("unit_lift: physical branch tail: exact CFG, bounded snapshot admission, aliases and rejection rollback\n");
}
