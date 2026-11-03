/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unit tests of form.c (region extent, edges, leaders, the compile-
 * time boundaries, rebasing, contract preconditions) and lift.c (block
 * structure, synthetic blocks and charges, points and frames, the qp = 0
 * forms, helper read sets, lift limits).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/um_machine.h"
#include "engine/cache/validity.h"
#include "ia64/semantics.h"
#include "ia64/fp.h"

/* tier1.h defines the PSR.mf bits numerically (Tier1 does not include
 * ia64/fp.h); they are PSR.mfl and PSR.mfh. */
_Static_assert(T1_PSR_MF_BITS == (IA64_PSR_MFL | IA64_PSR_MFH), "T1_PSR_MF_BITS");

#define PROG (UM_CODE + 0x3000)

static struct t1_form *F;
static struct t1_ir *IR;

static struct vcpu *program_at(uint64_t at, const struct ienc_bundle *b, unsigned n)
{
    um_emit(at, b, n);
    /* The fall-off target spins; formation stops at the unreachable
     * boundary after it, not at older programs' code. */
    struct ienc_bundle tail[] = {{IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
                                 {IENC_MII, {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}}};
    um_emit(at + 16u * n, tail, 2);
    return um_vcpu(at);
}
static struct vcpu *program(const struct ienc_bundle *b, unsigned n)
{
    return program_at(PROG, b, n);
}

static enum t1_compile_status form(struct vcpu *v, const char **reason)
{
    return t1_form(v, 0, T1_MAX_INSNS, F, reason);
}

/* The region of a program whose instruction `at` is a boundary ends just
 * before it, with the given reason. */
static void boundary(uint64_t bad, IA64SlotUnit unit, const char *why)
{
    (void)unit;
    struct ienc_bundle b[] = {{IENC_MII, {ienc_nop_m(), ienc_adds(0, 14, 1, 14), bad}}};
    struct vcpu *v = program(b, 1);
    const char *reason;
    UM_CHECK(form(v, &reason) == T1_COMPILED);
    if (F->ninsns != 2 || F->end_ip != PROG || F->end_slot != 2 || strcmp(F->end_reason, why)) {
        fprintf(stderr, "unit_lift boundary: %u insns, end %llx.%u, \"%s\" (want \"%s\")\n",
                F->ninsns, (unsigned long long)F->end_ip, F->end_slot, F->end_reason, why);
        UM_CHECK(!"boundary");
    }
    vcpu_destroy(v);
}

static unsigned count_ops(const struct t1_ir *ir, unsigned code)
{
    unsigned n = 0;
    for (unsigned b = 0; b < ir->nblocks; ++b) {
        if (ir->block[b].flags & T1_BLK_DEAD)
            continue;
        for (uint32_t o = ir->block[b].first; o != T1_NO_OP; o = ir->op[o].next)
            n += ir->op[o].code == code;
    }
    return n;
}

static void form_tests(const char *image)
{
    const char *reason;
    /* gather (frag_loops.S): 8 bundles, the back-edge last. */
    um_load_file(UM_CODE, image);
    struct vcpu *v = um_vcpu(UM_CODE);
    UM_CHECK(form(v, &reason) == T1_COMPILED);
    UM_CHECK(F->ninsns == 24 && F->loop && F->npages == 1 && F->c.entry_pa == UM_CODE);
    UM_CHECK(F->insn[23].cls == T1_IC_CLOOP && F->insn[23].edge == T1_EDGE_BACK);
    UM_CHECK(F->insn[8].cls == T1_IC_BR && F->insn[8].edge == T1_EDGE_INTERNAL &&
             F->insn[8].target_insn == 21);
    UM_CHECK(F->insn[20].edge == T1_EDGE_EXIT && F->insn[20].target == UM_CODE + 0x600);
    for (unsigned i = 0; i < 24; ++i)
        UM_CHECK(F->insn[i].leader == (i == 0 || i == 9 || i == 21));
    UM_CHECK(F->end_ip == UM_CODE + 0x80 && F->end_slot == 0 &&
             !strcmp(F->end_reason, "back-edge"));
    /* Frame-relative names: r32 is element register 32
     * at any frame base, and the contract and the request key are the same
     * at every BOF (one region per (header, CFM)). */
    {
        static const unsigned bofs[] = {0, 7, 44, 90, 95};
        struct t1_contract c0;
        uint64_t key0 = 0;
        for (unsigned k = 0; k < 5; ++k) {
            v->s.rse_bof = bofs[k];
            UM_CHECK(form(v, &reason) == T1_COMPILED && F->insn[0].base == 32 &&
                     F->insn[0].dst == 16);
            struct t1_compile_request rq;
            const struct t1_compile_options opt = {0};
            UM_CHECK(t1_request_from_vcpu(v, &opt, 40, &rq, &reason));
            if (!k) {
                c0 = F->c;
                key0 = rq.key;
            }
            const struct t1_contract *c = &F->c;
            UM_CHECK(c->ip == c0.ip && c->entry_pa == c0.entry_pa && c->psr == c0.psr &&
                     c->cfm == c0.cfm && c->psr_mask == c0.psr_mask &&
                     c->hostwin_bits == c0.hostwin_bits && c->host_features == c0.host_features &&
                     c->abi == c0.abi && c->flags == c0.flags && c->slot == c0.slot &&
                     c->icount == c0.icount && rq.key == key0);
        }
    }
    /* Rebasing: through the bit-63 alias the addresses are
     * the guest IP, the fetch addresses physical. */
    v->s.ip = UM_CODE | UINT64_C(1) << 63;
    UM_CHECK(form(v, &reason) == T1_COMPILED);
    UM_CHECK(F->c.entry_pa == UM_CODE &&
             F->insn[5].insn.address == ((UM_CODE + 16) | UINT64_C(1) << 63) &&
             F->insn[5].bundle_pa == UM_CODE + 16 &&
             F->end_ip == ((UM_CODE + 0x80) | UINT64_C(1) << 63));
    UM_CHECK(F->insn[23].edge == T1_EDGE_BACK &&
             F->insn[20].target == ((UM_CODE + 0x600) | UINT64_C(1) << 63));
    /* Entry mid-bundle: no back-edge to a slot-0 header is possible. */
    v->s.ip = UM_CODE + 0x70;
    v->s.slot = 2;
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns >= 1 && !F->loop &&
             F->insn[0].edge == T1_EDGE_EXIT);
    /* Virtual fetch without an I-TLB entry; PSR bits for the lower tier. */
    v->s.ip = UM_CODE;
    v->s.slot = 0;
    v->s.psr |= IA64_PSR_IT;
    UM_CHECK(form(v, &reason) == T1_REJECTED && !strcmp(reason, "no I-TLB entry"));
    um_virtual_code(v);
    v->s.ip = UM_CODE | UINT64_C(2) << 61;
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->c.entry_pa == UM_CODE &&
             F->insn[0].insn.address == (UM_CODE | UINT64_C(2) << 61));
    for (unsigned k = 0; k < 9; ++k) {
        static const uint64_t bits[] = {IA64_PSR_DB, IA64_PSR_LP, IA64_PSR_TB,
                                        IA64_PSR_SS, IA64_PSR_ID, IA64_PSR_DA,
                                        IA64_PSR_DD, IA64_PSR_ED, IA64_PSR_IA};
        v->s.psr = IA64_PSR_IC | bits[k];
        v->s.ip = UM_CODE;
        UM_CHECK(form(v, &reason) == T1_REJECTED && !strcmp(reason, "PSR requires the lower tier"));
    }
    /* Validity mode NONE has no snapshots: compilation is refused. */
    v->s.psr = IA64_PSR_IC;
    UM_CHECK(code_validity_configure(CODE_VALIDITY_NONE, CODE_SNAPSHOT_DEFAULT_BYTES));
    UM_CHECK(form(v, &reason) == T1_REJECTED && !strcmp(reason, "no code snapshot"));
    UM_CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    vcpu_destroy(v);

    /* Compile-time boundaries, each as slot 2 after two good ones. */
    boundary(ienc_break_m(0), IA64_UNIT_I, "not in the v1 subset");
    boundary(ienc_alu(0, IENC_ADD, 0, 1, 2), IA64_UNIT_I, "GR destination r0 or outside the frame");
    boundary(ienc_alu(0, IENC_ADD, 40, 1, 2), IA64_UNIT_I,
             "GR destination r0 or outside the frame");
    boundary(ienc_cmp(0, IENC_EQ, false, false, 6, 6, 1, 2), IA64_UNIT_I, "compare with p1 == p2");
    boundary(ienc_tbit(0, 0, 0, 1, 6, 6, 1, 3), IA64_UNIT_I, "compare with p1 == p2");
    /* A false qp does not save a boundary: the lower tier evaluates qp. */
    boundary(ienc_alu(9, IENC_ADD, 0, 1, 2), IA64_UNIT_I, "GR destination r0 or outside the frame");
    struct ienc_bundle mem[] = {
        {IENC_MMI, {ienc_nop_m(), ienc_ld_inc_imm(0, 8, false, 14, 14, 8), ienc_nop_i()}},
        {IENC_MMI, {ienc_nop_m(), ienc_ld_inc_imm(0, 8, false, 14, 0, 8), ienc_nop_i()}},
        {IENC_MMI, {ienc_nop_m(), ienc_st_inc_imm(0, 8, false, 40, 14, 8), ienc_nop_i()}},
        {IENC_MBB, {ienc_nop_m(), ienc_br_cloop(0), ienc_nop_b()}}};
    static const char *mem_why[] = {
        "load post-increment base == destination", "post-increment base r0 or outside the frame",
        "post-increment base r0 or outside the frame", "br.cloop not in slot 2"};
    for (unsigned k = 0; k < 4; ++k) {
        v = program(&mem[k], 1);
        UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 1 &&
                 !strcmp(F->end_reason, mem_why[k]));
        vcpu_destroy(v);
    }
    /* Entry at slot 2 of an MLX bundle is refused; the size limit ends a
     * long straight-line region at 256 instructions. */
    struct ienc_bundle mlx = {IENC_MLX, {ienc_nop_m(), 0, 0}};
    ienc_movl(0, 14, 0x1234, &mlx.slot[1], &mlx.slot[2]);
    v = program(&mlx, 1);
    v->s.slot = 2;
    UM_CHECK(form(v, &reason) == T1_REJECTED && !strcmp(reason, "slot 2 of an MLX bundle"));
    v->s.slot = 0;
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 5 &&
             F->insn[1].insn.opcode == IA64_OP_MOVL && F->insn[1].next_ip == PROG + 16 &&
             F->insn[1].next_slot == 0);
    vcpu_destroy(v);
    static struct ienc_bundle nops[100];
    for (unsigned k = 0; k < 100; ++k)
        nops[k] = (struct ienc_bundle){IENC_MII, {ienc_nop_m(), ienc_nop_i(), ienc_nop_i()}};
    uint64_t near_end = PROG + 0x1000 - 0x200; /* 32 bundles before a page boundary */
    v = program_at(near_end, nops, 100);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == T1_MAX_INSNS &&
             !strcmp(F->end_reason, "size limit") && F->npages == 2 &&
             F->page[1].pa == PROG + 0x1000 && F->insn[96].page == 1 &&
             F->end_ip == near_end + 16 * 85 && F->end_slot == 1);
    vcpu_destroy(v);
    /* Rotation: sof 16, sor 8, rrb.gr 3, rrb.pr 45:
     *   adds r41 = 1, r39 ;; cmp.eq p17, p18 = r32, r40
     * r39 -> o (7 + 3) % 8 = 2, r32 -> o 3, r40/r41 (not rotating) -> 8/9;
     * p17/p18 -> physical 16 + (1 + 45) % 48 = 62, 63. Element registers
     * 34, 41, 35, 40 at every frame base, wrapping or not. */
    struct ienc_bundle rot[] = {
        {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_adds(0, 41, 1, 39), ienc_nop_i()}},
        {IENC_STOP(IENC_MII),
         {ienc_nop_m(), ienc_cmp(0, IENC_EQ, false, false, 17, 18, 32, 40), ienc_nop_i()}}};
    v = program(rot, 2);
    v->s.cfm = um_cfm(16, 16, 8, 3, 45);
    static const unsigned rot_bofs[] = {0, 44, 85, 90, 95};
    for (unsigned k = 0; k < 5; ++k) {
        v->s.rse_bof = rot_bofs[k];
        UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns >= 4);
        const struct t1_insn *add = &F->insn[1], *cmp = &F->insn[4];
        UM_CHECK(add->dst == 41 && add->src2 == 34 && cmp->src1 == 35 && cmp->src2 == 40 &&
                 cmp->p1 == 62 && cmp->p2 == 63);
        /* The physical homes these names denote at this BOF are the CPU
         * model's: 32 + (bof + o) % 96. */
        UM_CHECK(ia64_gr_index(&v->s, 39) == 32 + (rot_bofs[k] + 2) % 96 &&
                 ia64_gr_index(&v->s, 41) == 32 + (rot_bofs[k] + 9) % 96);
    }
    vcpu_destroy(v);
    printf(
        "unit_lift: form: gather extent/edges/leaders, rebasing (bit 63, VRN 2), contract checks, "
        "11 boundary kinds, size and page tracking, frame-relative names (5 BOFs, rotation) ok\n");
}

static void lift_tests(const char *image)
{
    const char *reason;
    unsigned fail;
    char err[200];
    um_load_file(UM_CODE, image);
    struct vcpu *v = um_vcpu(UM_CODE);
    UM_CHECK(form(v, &reason) == T1_COMPILED && t1_lift(F, IR, &fail));
    UM_CHECK(t1_ir_verify(IR, 0, err, sizeof(err)));
    /* Points: 3 sites, 1 checkpoint, the side exit and the loop exit. */
    unsigned kinds[4] = {0};
    for (uint32_t p = 0; p < IR->npoints; ++p)
        if (!IR->point[p].dead)
            ++kinds[IR->point[p].kind];
    UM_CHECK(kinds[T1_PT_SITE] == 3 && kinds[T1_PT_CHECKPOINT] == 1 && kinds[T1_PT_EXIT] == 2 &&
             !kinds[T1_PT_HELPER]);
    /* The predicated store S3: head BRANCH qp, true block (charge 1, the
     * site at retired offset 0), false block (charge 1), join. */
    unsigned synthetic = 0;
    for (unsigned b = 0; b < IR->nblocks; ++b) {
        if (!(IR->block[b].flags & T1_BLK_SYNTHETIC))
            continue;
        ++synthetic;
        uint32_t t = t1_ir_terminator(IR, (uint16_t)b);
        if (IR->block[b].first == t)
            UM_CHECK(IR->block[b].charge == 1); /* the false path */
    }
    UM_CHECK(synthetic == 3);
    for (uint32_t p = 0; p < IR->npoints; ++p)
        if (IR->point[p].kind == T1_PT_SITE && IR->point[p].access_write)
            UM_CHECK(IR->point[p].retired_offset == 0 &&
                     IR->block[IR->point[p].block].charge == 1 &&
                     (IR->block[IR->point[p].block].flags & T1_BLK_SYNTHETIC));
    /* The site S1 is the header's first instruction; S2 follows the
     * (p8) branch: retired offsets 0 in their blocks. Frames cover every
     * written element plus W. */
    UM_CHECK(t1_elem_has(IR->frame_set, T1_ELEMENT_GR(14)) &&
             t1_elem_has(IR->frame_set, T1_ELEMENT_NAT(32)) &&
             t1_elem_has(IR->frame_set, T1_ELEMENT_PR(10)) &&
             t1_elem_has(IR->frame_set, T1_ELEMENT_LC) &&
             t1_elem_has(IR->frame_set, T1_ELEMENT_LSB) &&
             !t1_elem_has(IR->frame_set, T1_ELEMENT_GR(33)) &&
             !t1_elem_has(IR->frame_set, T1_ELEMENT_PR(11)));
    /* (p8) adds -> SELECT; cmp.eq.unc under (p7) -> AND1 with qp. */
    UM_CHECK(count_ops(IR, T1_IR_SELECT) >= 2 && count_ops(IR, T1_IR_NATCHK) == 3 &&
             count_ops(IR, T1_IR_CLOOP) == 1);
    vcpu_destroy(v);

    /* Helpers are unconditional (no branch on their qp); read set order. */
    struct ienc_bundle hb[] = {
        {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_tnat(9, 0, 0, 1, 6, 7, 20), ienc_nop_i()}}};
    v = program(hb, 1);
    UM_CHECK(form(v, &reason) == T1_COMPILED && t1_lift(F, IR, &fail));
    UM_CHECK(count_ops(IR, T1_IR_BRANCH) == 0 && count_ops(IR, T1_IR_HELPER) == 1);
    for (unsigned b = 0; b < IR->nblocks; ++b)
        for (uint32_t o = IR->block[b].first; o != T1_NO_OP; o = IR->op[o].next) {
            const struct t1_op *op = &IR->op[o];
            if (op->code != T1_IR_HELPER)
                continue;
            static const uint16_t want[] = {T1_ELEMENT_PR(9), T1_ELEMENT_GR(20), T1_ELEMENT_NAT(20),
                                            T1_ELEMENT_PR(6), T1_ELEMENT_PR(7)};
            UM_CHECK(op->nuse == 6 && op->wr[0] == T1_ELEMENT_PR(6) &&
                     op->wr[1] == T1_ELEMENT_PR(7));
            for (unsigned k = 0; k < 5; ++k)
                UM_CHECK(IR->use[op->use_first + 1 + k].element == want[k]);
            for (unsigned k = 0; k < 5; ++k)
                UM_CHECK(t1_elem_has(IR->w_set, want[k]));
        }
    vcpu_destroy(v);

    /* A predicated mf sits under a branch-around; br.cloop has no qp. */
    struct ienc_bundle fb[] = {{IENC_STOP(IENC_MII), {ienc_mf(5), ienc_nop_i(), ienc_nop_i()}}};
    v = program(fb, 1);
    UM_CHECK(form(v, &reason) == T1_COMPILED && t1_lift(F, IR, &fail));
    UM_CHECK(count_ops(IR, T1_IR_FENCE) == 1 && count_ops(IR, T1_IR_BRANCH) == 1);
    UM_CHECK(t1_ir_verify(IR, 0, err, sizeof(err)));
    vcpu_destroy(v);

    /* Block limit: 30 predicated stores need 3 blocks each; the lift fails
     * at the store that does not fit, and the region re-formed to end
     * before it lifts. */
    static struct ienc_bundle many[30];
    for (unsigned k = 0; k < 30; ++k)
        many[k] = (struct ienc_bundle){IENC_MII,
                                       {ienc_st(6, 8, false, 2, 14), ienc_nop_i(), ienc_nop_i()}};
    v = program(many, 30);
    UM_CHECK(form(v, &reason) == T1_COMPILED && !t1_lift(F, IR, &fail) &&
             !strcmp(IR->fail_reason, "block limit"));
    UM_CHECK(fail > 0 && fail < F->ninsns && F->insn[fail].cls == T1_IC_STORE);
    UM_CHECK(t1_form(v, 0, fail, F, &reason) == T1_COMPILED && F->ninsns == fail &&
             t1_lift(F, IR, &fail));
    UM_CHECK(t1_ir_verify(IR, 0, err, sizeof(err)));
    vcpu_destroy(v);
    printf(
        "unit_lift: lift: gather points/synthetic blocks/charges/frame set, qp = 0 forms, helper read "
        "sets, predicated mf, block-limit retry ok\n");
}

/* A loop region ends at its farthest back-edge to the
 * head within T1_LOOP_EXTEND instructions past the first one; inner
 * backward branches stay exits; instructions after the cut are lookahead:
 * neither region instructions nor source pages. */
static void extension_tests(void)
{
    const char *reason;
    unsigned fail;
    char err[200];
    /*  +0x00 { nop.m ; adds r14=1,r14 ; (p8) br +0x20 }   forward, internal
     *  +0x10 { nop.m ; nop.i ; (p6) br head }             back-edge 1
     *  +0x20 { nop.m ; adds r15=1,r15 ; (p9) br +0x10 }   backward into a loop with two
     *                                                     entries (+0x10, +0x20): an exit
     *  +0x30 { nop.m ; nop.i ; (p7) br head }             back-edge 2 */
    struct ienc_bundle two[] = {
        {IENC_MIB, {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_br_cond(8, 0x20)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, -0x10)}},
        {IENC_MIB, {ienc_nop_m(), ienc_adds(0, 15, 1, 15), ienc_br_cond(9, -0x10)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(7, -0x30)}}};
    struct vcpu *v = program(two, 4);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->loop && F->ninsns == 12 && F->npages == 1);
    UM_CHECK(F->insn[2].edge == T1_EDGE_INTERNAL && F->insn[2].target_insn == 6);
    UM_CHECK(F->insn[5].edge == T1_EDGE_BACK && F->insn[11].edge == T1_EDGE_BACK);
    UM_CHECK(F->insn[8].edge == T1_EDGE_EXIT && F->insn[8].target == PROG + 0x10);
    UM_CHECK(F->end_ip == PROG + 0x40 && F->end_slot == 0 && !strcmp(F->end_reason, "back-edge"));
    for (unsigned i = 0; i < 12; ++i)
        UM_CHECK(F->insn[i].leader == (i == 0 || i == 3 || i == 6 || i == 9));
    UM_CHECK(t1_lift(F, IR, &fail) && t1_ir_verify(IR, 0, err, sizeof(err)));
    unsigned checkpoints = 0;
    for (uint32_t p = 0; p < IR->npoints; ++p)
        checkpoints += !IR->point[p].dead && IR->point[p].kind == T1_PT_CHECKPOINT;
    UM_CHECK(count_ops(IR, T1_IR_BACKEDGE) == 2 && checkpoints == 2);
    vcpu_destroy(v);

    /* One back-edge: the code after it is lookahead, and a branch to it an exit. */
    struct ienc_bundle one[] = {
        {IENC_MIB, {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_br_cond(8, 0x20)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, -0x10)}},
        {IENC_MII, {ienc_nop_m(), ienc_adds(0, 15, 1, 15), ienc_nop_i()}}};
    v = program(one, 3);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->loop && F->ninsns == 6);
    UM_CHECK(F->insn[2].edge == T1_EDGE_EXIT && F->insn[5].edge == T1_EDGE_BACK);
    UM_CHECK(F->end_ip == PROG + 0x20 && F->end_slot == 0 && !strcmp(F->end_reason, "back-edge"));
    vcpu_destroy(v);
    /* The lookahead reaches the next page: no source page, and while that
     * page is not tracked yet it is not read (no snapshot, no protection). */
    uint64_t near_end = PROG + 0x5000 - 0x20;
    UM_CHECK(!code_page_is_tracked(PROG + 0x5000));
    v = program_at(near_end, one, 3);
    UM_CHECK(!code_page_is_tracked(PROG + 0x5000));
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 1);
    UM_CHECK(!code_page_is_tracked(PROG + 0x5000));
    /* Once code there is tracked, the lookahead reads it; the cut drops it. */
    v->s.ip = PROG + 0x5010;
    UM_CHECK(form(v, &reason) == T1_COMPILED && code_page_is_tracked(PROG + 0x5000));
    v->s.ip = near_end;
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 1);
    vcpu_destroy(v);

    /* A region at an inner header continues through the unconditional
     * outer branch (stitched) into the enclosing loop and closes at the
     * second back-edge to its head: the head bundle is scanned twice.
     *  +0x00 { cmp.eq p6, p7 = r20, r0 ; adds r20 = -1, r20 ; mov ar.lc = r21 } ;;
     *  +0x10 { nop.m ; nop.i ; (p6) br +0x50 } ;;
     *  +0x20 { ld8 r22 = [r33] ; nop.m ; nop.i } ;;          head
     *  +0x30 { nop.m ; add r14 = r14, r22 ; br.cloop +0x20 } ;;
     *  +0x40 { nop.m ; nop.i ; br +0x00 } ;;
     *  +0x50 { break.m 1 ; nop.i ; nop.i } ;; */
    struct ienc_bundle outer[] = {
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
    v = program(outer, 6);
    v->s.ip = PROG + 0x20;
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->loop && F->ninsns == 21 && F->npages == 1);
    for (unsigned i = 0; i < 21; ++i) {
        static const uint16_t at[7] = {0x20, 0x30, 0x40, 0x00, 0x10, 0x20, 0x30};
        UM_CHECK(F->insn[i].insn.address == PROG + at[i / 3]);
        UM_CHECK((F->insn[i].edge == T1_EDGE_BACK) == (i == 5 || i == 20));
        UM_CHECK(F->insn[i].leader == (i == 0 || i == 6 || i == 9 || i == 15));
    }
    UM_CHECK(F->insn[8].edge == T1_EDGE_INTERNAL && F->insn[8].target_insn == 9);
    UM_CHECK(F->insn[11].cls == T1_IC_LC);
    UM_CHECK(F->insn[14].edge == T1_EDGE_EXIT && F->insn[14].target == PROG + 0x50);
    UM_CHECK(F->insn[15].cls == T1_IC_LOAD && F->insn[15].bundle_pa == F->insn[0].bundle_pa);
    UM_CHECK(F->end_ip == PROG + 0x40 && F->end_slot == 0 && !strcmp(F->end_reason, "back-edge"));
    UM_CHECK(t1_lift(F, IR, &fail) && t1_ir_verify(IR, 0, err, sizeof(err)));
    unsigned sites = 0;
    checkpoints = 0;
    for (uint32_t p = 0; p < IR->npoints; ++p) {
        if (IR->point[p].dead)
            continue;
        checkpoints += IR->point[p].kind == T1_PT_CHECKPOINT;
        sites += IR->point[p].kind == T1_PT_SITE;
    }
    UM_CHECK(count_ops(IR, T1_IR_BACKEDGE) == 2 && checkpoints == 2 && sites == 2);
    vcpu_destroy(v);

    /* The extension limit: a second back-edge at T1_LOOP_EXTEND instructions
     * past the first is in the region, one bundle later it is not. */
    static struct ienc_bundle far[T1_MAX_INSNS / 3];
    for (unsigned in = 0; in < 2; ++in) {
        unsigned fill = (T1_LOOP_EXTEND - 3) / 3 + in, n = 0;
        far[n++] =
            (struct ienc_bundle){IENC_MII, {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_nop_i()}};
        far[n++] =
            (struct ienc_bundle){IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, -0x10)}};
        for (unsigned k = 0; k < fill; ++k)
            far[n++] = (struct ienc_bundle){IENC_MII, {ienc_nop_m(), ienc_nop_i(), ienc_nop_i()}};
        far[n] = (struct ienc_bundle){
            IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(7, -(int64_t)(16 * n))}};
        ++n;
        UM_CHECK(8 + 3 * fill <= 5 + T1_LOOP_EXTEND || in);
        v = program(far, n);
        UM_CHECK(form(v, &reason) == T1_COMPILED && F->loop);
        UM_CHECK(F->ninsns == (in ? 6 : 3 * n) && F->insn[F->ninsns - 1].edge == T1_EDGE_BACK);
        vcpu_destroy(v);
    }
    printf("unit_lift: extension: two back-edges to the head, inner backward exit, lookahead "
           "instructions and page excluded, stitched outer loop, T1_LOOP_EXTEND limit ok\n");
}

/* The checkpoint of the BACKEDGE that ends block b. */
static const struct t1_ir_point *backedge_point(uint16_t b)
{
    uint32_t t = t1_ir_terminator(IR, b);
    UM_CHECK(t != T1_NO_OP && IR->op[t].code == T1_IR_BACKEDGE && IR->op[t].point < IR->npoints);
    return &IR->point[IR->op[t].point];
}

/* Forms and lifts the program; every back-edge's checkpoint resumes at its
 * header (slot 0), and the headers' loops have the expected depths (the
 * head's first: 0 for a trace). Returns the number of back-edges. */
static unsigned forest(const struct ienc_bundle *b, unsigned n, unsigned ninsns,
                       const uint8_t *edges, const uint16_t *targets, const unsigned *header_depths,
                       unsigned nheaders)
{
    const char *reason;
    unsigned fail;
    char err[200];
    struct vcpu *v = program(b, n);
    if (form(v, &reason) != T1_COMPILED || F->ninsns != ninsns) {
        fprintf(stderr, "unit_lift forest: %u insns (want %u), %s\n", F->ninsns, ninsns,
                reason ? reason : "-");
        UM_CHECK(!"forest formation");
    }
    for (unsigned i = 0; i < ninsns; ++i) {
        if (F->insn[i].cls != T1_IC_BR && F->insn[i].cls != T1_IC_CLOOP)
            continue;
        if (F->insn[i].edge != edges[i] ||
            (edges[i] == T1_EDGE_BACK && F->insn[i].target_insn != targets[i])) {
            fprintf(stderr, "unit_lift forest: insn %u edge %u target %u (want %u, %u)\n", i,
                    F->insn[i].edge, F->insn[i].target_insn, edges[i], targets[i]);
            UM_CHECK(!"forest edges");
        }
        /* A header starts a block. */
        if (edges[i] == T1_EDGE_BACK)
            UM_CHECK(F->insn[targets[i]].leader);
    }
    UM_CHECK(t1_lift(F, IR, &fail) && t1_ir_verify(IR, 0, err, sizeof(err)));
    struct t1_ir_loops loops;
    t1_ir_loops(IR, &loops);
    unsigned backedges = 0, headers = 0;
    uint64_t seen = 0;
    for (uint16_t k = 0; k < IR->nblocks; ++k) {
        uint32_t t = t1_ir_terminator(IR, k);
        if (t == T1_NO_OP || IR->op[t].code != T1_IR_BACKEDGE)
            continue;
        ++backedges;
        uint16_t h = IR->block[k].succ[0].target;
        const struct t1_ir_point *p = backedge_point(k);
        /* The header block's first instruction is the one the checkpoint resumes at. */
        UM_CHECK(p->kind == T1_PT_CHECKPOINT && p->slot == 0);
        UM_CHECK(loops.body[h] & (UINT64_C(1) << k) && loops.header[h] == h);
        if (!(seen & (UINT64_C(1) << h))) {
            seen |= UINT64_C(1) << h;
            ++headers;
        }
    }
    UM_CHECK(headers == nheaders);
    if (header_depths[0])
        UM_CHECK(loops.depth[T1_HEADER_BLOCK] == header_depths[0]);
    else
        UM_CHECK(!loops.body[T1_HEADER_BLOCK] && loops.header[T1_HEADER_BLOCK] == T1_NO_BLOCK);
    unsigned inner = 0;
    for (uint16_t h = 0; h < IR->nblocks; ++h)
        if (h != T1_HEADER_BLOCK && loops.body[h])
            UM_CHECK(loops.depth[h] == header_depths[1 + inner++]);
    UM_CHECK(inner + (header_depths[0] != 0) == nheaders);
    vcpu_destroy(v);
    return backedges;
}

/* The loop forest: backward branches to scanned bundle
 * starts that their target dominates are back-edges to that header. */
static void forest_tests(void)
{
    enum { X = T1_EDGE_EXIT, I = T1_EDGE_INTERNAL, B = T1_EDGE_BACK };
    /* Nested.
     *  +0x00 { nop.m ; adds r14=1,r14 ; nop.i }       head
     *  +0x10 { nop.m ; adds r15=1,r15 ; nop.i }       inner header
     *  +0x20 { nop.m ; nop.i ; (p9) br +0x10 }        inner back-edge
     *  +0x30 { nop.m ; nop.i ; (p6) br head }          outer back-edge */
    struct ienc_bundle nest[] = {{IENC_MII, {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_nop_i()}},
                                 {IENC_MII, {ienc_nop_m(), ienc_adds(0, 15, 1, 15), ienc_nop_i()}},
                                 {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(9, -0x10)}},
                                 {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, -0x30)}}};
    uint8_t e1[12] = {[8] = B, [11] = B};
    uint16_t t1[12] = {[8] = 3, [11] = 0};
    UM_CHECK(forest(nest, 4, 12, e1, t1, (const unsigned[]){1, 2}, 2) == 2);
    UM_CHECK(F->loop && F->insn[3].leader && !F->insn[6].leader && F->insn[9].leader);
    /* max_path of each header: its longest forward path to a checkpoint or exit. */
    uint16_t inner = T1_NO_BLOCK;
    for (uint16_t k = 0; k < IR->nblocks; ++k) {
        uint32_t t = t1_ir_terminator(IR, k);
        if (t != T1_NO_OP && IR->op[t].code == T1_IR_BACKEDGE &&
            IR->block[k].succ[0].target != T1_HEADER_BLOCK)
            inner = IR->block[k].succ[0].target;
    }
    UM_CHECK(inner != T1_NO_BLOCK);
    UM_CHECK(t1_ir_max_path(IR, T1_HEADER_BLOCK) == 12 && t1_ir_max_path(IR, inner) == 9);
    for (uint16_t k = 0; k < IR->nblocks; ++k) {
        uint32_t t = t1_ir_terminator(IR, k);
        if (t == T1_NO_OP || IR->op[t].code != T1_IR_BACKEDGE)
            continue;
        const struct t1_ir_point *p = backedge_point(k);
        UM_CHECK(p->ip == (IR->block[k].succ[0].target == inner ? PROG + 0x10 : PROG));
    }

    /* Sibling self loops inside the head loop. */
    struct ienc_bundle siblings[] = {
        {IENC_MII, {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_nop_i()}},
        {IENC_MIB, {ienc_nop_m(), ienc_adds(0, 15, 1, 15), ienc_br_cond(8, 0)}},
        {IENC_MIB, {ienc_nop_m(), ienc_adds(0, 16, 1, 16), ienc_br_cond(9, 0)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, -0x30)}}};
    uint8_t e2[12] = {[5] = B, [8] = B, [11] = B};
    uint16_t t2[12] = {[5] = 3, [8] = 6, [11] = 0};
    UM_CHECK(forest(siblings, 4, 12, e2, t2, (const unsigned[]){1, 2, 2}, 3) == 3);

    /* Overlapping branch ranges [+0x10, +0x30] and [+0x20, +0x40] are still
     * nested natural loops: +0x40 reaches the first back-edge through +0x20.
     *  +0x00 head; +0x10 h1; +0x20 h2; +0x30 (p8) br h1; +0x40 (p9) br h2;
     *  +0x50 (p6) br head */
    struct ienc_bundle overlap[] = {
        {IENC_MII, {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_nop_i()}},
        {IENC_MII, {ienc_nop_m(), ienc_adds(0, 15, 1, 15), ienc_nop_i()}},
        {IENC_MII, {ienc_nop_m(), ienc_adds(0, 16, 1, 16), ienc_nop_i()}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(8, -0x20)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(9, -0x20)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, -0x50)}}};
    uint8_t e3[18] = {[11] = B, [14] = B, [17] = B};
    uint16_t t3[18] = {[11] = 3, [14] = 6, [17] = 0};
    UM_CHECK(forest(overlap, 6, 18, e3, t3, (const unsigned[]){1, 2, 3}, 3) == 3);

    /* An inner loop in a trace: the head heads no loop.
     *  +0x00 { nop.m ; adds r14=1,r14 ; nop.i }
     *  +0x10 { nop.m ; adds r15=1,r15 ; (p8) br +0x10 }
     *  +0x20 { break.m 1 ; ... }                        boundary */
    struct ienc_bundle trace[] = {
        {IENC_MII, {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_nop_i()}},
        {IENC_MIB, {ienc_nop_m(), ienc_adds(0, 15, 1, 15), ienc_br_cond(8, 0)}},
        {IENC_MII, {ienc_break_m(1), ienc_nop_i(), ienc_nop_i()}}};
    uint8_t e4[6] = {[5] = B};
    uint16_t t4[6] = {[5] = 3};
    UM_CHECK(forest(trace, 3, 6, e4, t4, (const unsigned[]){0, 1}, 1) == 1);
    UM_CHECK(!F->loop && F->end_ip == PROG + 0x20);

    /* Two entries: +0x20 branches back to +0x10, which +0x00 bypasses into
     * +0x20 (the `two` program of extension_tests): no dominance, an exit.
     * A backward branch in code no path reaches is an exit too.
     *  +0x00 { nop.m ; adds r14=1,r14 ; (p6) br head }
     *  +0x10 { nop.m ; nop.i ; br head }                p0: nothing falls through
     *  +0x20 { nop.m ; adds r15=1,r15 ; (p7) br +0x20 }  unreachable self loop
     *  +0x30 { nop.m ; nop.i ; (p6) br head }            the farthest back-edge */
    struct ienc_bundle dead[] = {
        {IENC_MIB, {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_br_cond(6, 0)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, -0x10)}},
        {IENC_MIB, {ienc_nop_m(), ienc_adds(0, 15, 1, 15), ienc_br_cond(7, 0)}},
        {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, -0x30)}}};
    uint8_t e5[12] = { [2] = B, [5] = B, [8] = X, [11] = B };
    uint16_t t5[12] = {0};
    /* The lifter drops the unreachable code: two back-edges remain. */
    UM_CHECK(forest(dead, 4, 12, e5, t5, (const unsigned[]){1}, 1) == 2);

    /* The outer loop of the stitched program of extension_tests, formed at
     * its own head: the inner br.cloop is a back-edge to +0x20.
     *  +0x00 { cmp.eq p6, p7 = r20, r0 ; adds r20 = -1, r20 ; mov ar.lc = r21 } ;;
     *  +0x10 { nop.m ; nop.i ; (p6) br +0x50 } ;;
     *  +0x20 { ld8 r22 = [r33] ; nop.m ; nop.i } ;;
     *  +0x30 { nop.m ; add r14 = r14, r22 ; br.cloop +0x20 } ;;
     *  +0x40 { nop.m ; nop.i ; br +0x00 } ;;
     *  +0x50 { break.m 1 ; nop.i ; nop.i } ;; */
    struct ienc_bundle outer[] = {
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
    uint8_t e6[15] = { [5] = X, [11] = B, [14] = B };
    uint16_t t6[15] = {[11] = 6, [14] = 0};
    UM_CHECK(forest(outer, 6, 15, e6, t6, (const unsigned[]){1, 2}, 2) == 2);
    UM_CHECK(F->loop && F->insn[5].target == PROG + 0x50);
    printf(
        "unit_lift: loop forest: nested, sibling and overlapping inner loops, an inner loop in a trace, "
        "two-entry and unreachable backward branches as exits, an outer head over its inner loop; "
        "checkpoints resume at their headers ok\n");
}

static void packed_tests(void)
{
    const char *reason;
    unsigned fail;
    char err[200];
    for (unsigned sub = 0; sub < 2; ++sub)
        for (unsigned width = 0; width < 3; ++width)
            for (unsigned sat = 0; sat < (width == 2 ? 1u : 4u); ++sat)
                for (unsigned pred = 0; pred < 2; ++pred) {
                    struct ienc_bundle b[] = {
                        {IENC_STOP(IENC_MII),
                         {ienc_nop_m(),
                          ienc_packed(pred ? 6 : 0, sub, 8u << width, sat, 14, 15, 16),
                          ienc_nop_i()}}};
                    struct vcpu *v = program(b, 1);
                    UM_CHECK(form(v, &reason) == T1_COMPILED);
                    UM_CHECK(F->insn[1].cls == (sat ? T1_IC_GENERAL : T1_IC_ALU));
                    UM_CHECK(t1_lift(F, IR, &fail) && t1_ir_verify(IR, 0, err, sizeof(err)));
                    UM_CHECK(count_ops(IR, T1_IR_GHELPER) == (sat ? 1u : 0u));
                    UM_CHECK(t1_elem_has(IR->frame_set, T1_ELEMENT_GR(14)));
                    UM_CHECK(t1_elem_has(IR->frame_set, T1_ELEMENT_NAT(14)));
                    UM_CHECK(t1_optimize(IR, true, err, sizeof(err)));
                    UM_CHECK(!IR->nat_guard[0] && !IR->nat_guard[1]);
                    vcpu_destroy(v);
                }
    /* Equal 32-bit-lane sources fold the value to zero; its NaT must
     * remain exact even after all arithmetic operations disappear. */
    struct ienc_bundle equal[] = {
        {IENC_STOP(IENC_MII),
         {ienc_packed(0, true, 32, 0, 14, 15, 15), ienc_nop_i(), ienc_nop_i()}}};
    struct vcpu *v = program(equal, 1);
    UM_CHECK(form(v, &reason) == T1_COMPILED && t1_lift(F, IR, &fail));
    UM_CHECK(t1_optimize(IR, true, err, sizeof(err)));
    UM_CHECK(!IR->nat_guard[0] && !IR->nat_guard[1] && count_ops(IR, T1_IR_NATIN));
    vcpu_destroy(v);
    for (unsigned consumer = 0; consumer < 3; ++consumer) {
        struct ienc_bundle b[] = {
            {IENC_STOP(IENC_MII),
             {ienc_packed(0, false, 32, 0, 14, 15, 16), ienc_nop_i(), ienc_nop_i()}},
            {IENC_STOP(IENC_MII),
             {consumer == 0   ? ienc_ld(0, 8, false, 20, 14)
              : consumer == 2 ? ienc_packed(0, false, 8, 1, 20, 14, 17)
                              : ienc_nop_m(),
              consumer == 1 ? ienc_tnat(0, 0, 0, 0, 6, 7, 14) : ienc_nop_i(), ienc_nop_i()}}};
        v = program(b, 2);
        UM_CHECK(form(v, &reason) == T1_COMPILED && t1_lift(F, IR, &fail));
        UM_CHECK(t1_optimize(IR, true, err, sizeof(err)));
        if (consumer == 0)
            UM_CHECK((IR->nat_guard[0] & (UINT64_C(3) << 15)) == (UINT64_C(3) << 15));
        else
            UM_CHECK(!IR->nat_guard[0] && !IR->nat_guard[1] && count_ops(IR, T1_IR_NATIN));
        vcpu_destroy(v);
    }
    boundary(ienc_packed(0, false, 8, 0, 0, 14, 15), IA64_UNIT_I,
             "GR destination r0 or outside the frame");
    boundary(ienc_packed(6, true, 32, 0, 40, 14, 15), IA64_UNIT_I,
             "GR destination r0 or outside the frame");
    printf(
        "unit_lift: packed wrap uses pure IR, saturation stays helper, qp and destination boundaries hold\n");
}

/* alloc at a region's head (SDM Vol.3 alloc): the forms that fault -- a
 * predicate, sof > 96, sol > sof, a rotating region beyond sof, r0 or a
 * stacked destination outside the new frame (Illegal Operation), a change
 * of sor while a rotating base is not zero (Reserved Register/Field) --
 * leave no region (the lower tier raises the fault); the others start one
 * with the alloc. */
static void alloc_tests(void)
{
    static const struct {
        unsigned qp, r1, sof, sol, sor, cur_sor, rrb;
        bool faults;
    } cases[] = {
        {0, 32, 8, 4, 0, 0, 0, false},  {0, 39, 8, 4, 0, 0, 0, false},
        {0, 14, 8, 4, 0, 0, 0, false},  {0, 32, 96, 96, 12, 0, 0, false},
        {0, 32, 16, 8, 1, 8, 3, false}, {6, 32, 8, 4, 0, 0, 0, true},
        {0, 32, 97, 4, 0, 0, 0, true},  {0, 32, 8, 9, 0, 0, 0, true},
        {0, 32, 8, 0, 2, 0, 0, true},   {0, 0, 8, 4, 0, 0, 0, true},
        {0, 40, 8, 4, 0, 0, 0, true},   {0, 32, 16, 8, 2, 8, 3, true},
        {0, 32, 16, 8, 0, 8, 3, true},
    };
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        struct ienc_bundle b[] = {
            {IENC_STOP(IENC_MII),
             {ienc_alloc(cases[k].qp, cases[k].r1, cases[k].sof, cases[k].sol, cases[k].sor),
              ienc_adds(0, 14, 1, 14), ienc_nop_i()}}};
        struct vcpu *v = program(b, 1);
        v->s.cfm = um_cfm(16, 8, cases[k].cur_sor, cases[k].rrb, 0);
        const char *reason;
        enum t1_compile_status st = form(v, &reason);
        bool started =
            st == T1_COMPILED && F->ninsns >= 1 && F->insn[0].insn.opcode == IA64_OP_ALLOC;
        if (started == cases[k].faults) {
            fprintf(stderr, "unit_lift alloc case %u: status %d, %u insns (%s)\n", k, (int)st,
                    st == T1_COMPILED ? F->ninsns : 0, reason ? reason : "");
            UM_CHECK(!"alloc at the head");
        }
        if (cases[k].faults)
            UM_CHECK(st == T1_REJECTED);
        vcpu_destroy(v);
    }
}

/* Calls in formation: a callee that returns is part of
 * the region and the scan goes on in the caller; one that does not return
 * within the region, and a call of the region's own head (recursion, not
 * followed at all), end the region at the call, which leaves to the
 * callee. */
static void call_form_tests(void)
{
    const uint64_t callee = PROG + 0x40;
    for (unsigned indirect = 0; indirect < 2; ++indirect)
        for (unsigned returns = 0; returns < 2; ++returns) {
            struct ienc_bundle b[] = {
                {IENC_STOP(IENC_MIB),
                 {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_br_call(0, 0, 0x40)}},
                {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_adds(0, 16, 1, 16), ienc_nop_i()}},
                {IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}},
                {IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}},
                {IENC_STOP(IENC_MIB),
                 {ienc_nop_m(), ienc_adds(0, 15, 1, 15),
                  returns ? ienc_br_ret(0, 0) : ienc_nop_b()}},
                {IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}},
            };
            if (indirect)
                b[0].slot[2] = ienc_br_call_indirect(0, 0, 0);
            struct vcpu *v = program(b, 6);
            v->s.br[0] = callee | 15;
            const char *reason;
            UM_CHECK(form(v, &reason) == T1_COMPILED);
            if (returns)
                UM_CHECK(F->ninsns > 6 && F->insn[3].insn.address == callee &&
                         F->insn[6].insn.address == PROG + 16 && F->end_class != T1_END_CALL);
            else
                UM_CHECK(F->ninsns == 3 && F->end_ip == callee && F->end_slot == 0 &&
                         F->end_class == T1_END_CALL);
            vcpu_destroy(v);
        }
    struct ienc_bundle r[] = {
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_br_call(0, 0, 0)}}};
    struct vcpu *v = program(r, 1);
    const char *reason;
    uint64_t followed = t1_form_calls(T1_CALL_FOLLOWED);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 3 && F->end_ip == PROG &&
             F->end_class == T1_END_CALL && t1_form_calls(T1_CALL_FOLLOWED) == followed);
    vcpu_destroy(v);
}

static void indirect_request_tests(void)
{
    const uint64_t callee = UM_CODE + 0x7000;
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MIB),
         {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_br_call_indirect(0, 0, 2)}},
        {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_adds(0, 16, 1, 16), ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}},
    };
    struct ienc_bundle leaf = {IENC_STOP(IENC_MIB),
                               {ienc_nop_m(), ienc_adds(0, 15, 1, 15), ienc_br_ret(0, 0)}};
    struct vcpu *v = program(b, 3);
    um_emit(callee, &leaf, 1);
    um_virtual_code(v);
    v->s.tr[0][0].va = PROG;
    v->s.tr[0][0].pte = PROG | 1 | (1u << 5) | (1u << 6) | (1u << 9);
    v->s.tr[0][0].itir = 12 << 2;
    v->s.tr[0][0].page_shift = 12;
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 0);
    v->s.br[2] = callee | 15;
    const char *reason;
    /* A missing target I-TLB entry never walks or applies call effects. */
    uint64_t seq = v->mmu_seq;
    struct t1_compile_request rq, changed;
    const struct t1_compile_options opt = {0};
    UM_CHECK(t1_request_from_vcpu(v, &opt, 0, &rq, &reason) && rq.npages == 1 &&
             rq.pages_end == T1_PAGES_NO_ITLB && v->mmu_seq == seq && v->s.ip == PROG);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 2 && F->end_ip == PROG &&
             F->end_slot == 2 && F->end_class == T1_END_CALL);
    v->s.tr[0][3] = v->s.tr[0][0];
    v->s.tr[0][3].va = callee;
    v->s.tr[0][3].pte = callee | 1 | (1u << 5) | (1u << 6) | (1u << 9);
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 3);
    UM_CHECK(t1_request_from_vcpu(v, &opt, 0, &rq, &reason) && rq.npages == 2 &&
             rq.page[1].va == callee && rq.page[1].pa == callee && rq.br_target[2] == callee);
    /* Observations travel with a copied request; another BR observation
     * changes neither its coalescing key nor the architectural contract. */
    v->s.br[2] = PROG;
    UM_CHECK(t1_request_from_vcpu(v, &opt, 0, &changed, &reason) && changed.key == rq.key &&
             changed.br_target[2] == PROG && rq.br_target[2] == callee);
    struct ia64_state *names = calloc(1, sizeof(*names));
    UM_CHECK(names && t1_form_request(&rq, names, T1_MAX_INSNS, F, &reason) == T1_COMPILED &&
             F->ninsns == 9 && F->insn[2].target == callee && F->insn[3].insn.address == callee &&
             F->insn[6].insn.address == PROG + 16 && F->npages == 2);
    rq.br_observed = 0;
    UM_CHECK(t1_form_request(&rq, names, T1_MAX_INSNS, F, &reason) == T1_COMPILED &&
             F->ninsns == 2 && F->end_ip == PROG && F->end_slot == 2);
    free(names);
    vcpu_destroy(v);
    /* Predicated indirect calls keep the lower-tier boundary, including
     * a currently false predicate: observations do not specialize PRs. */
    b[0].slot[2] = ienc_br_call_indirect(6, 0, 2);
    for (unsigned pred = 0; pred < 2; ++pred) {
        v = program(b, 3);
        v->s.br[2] = callee;
        um_pr(v, 6, pred != 0);
        UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 2 && F->end_slot == 2);
        vcpu_destroy(v);
    }
}

/* SDM Vol.2 Table 6-2: sol=0 leaves BOF/BSP/dirty unchanged, including
 * a caller larger than the current frame. Entry guards exclude output
 * overlap with the dirty partition (§6.5.5). */
static void observed_return_tests(void)
{
    const uint64_t target = UM_CODE + 0x7000;
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_br_ret(0, 2)}},
    };
    struct ienc_bundle caller[] = {
        {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_adds(0, 33, 1, 33), ienc_nop_i()}},
        {IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}},
    };
    struct vcpu *v = program(b, 1);
    um_emit(target, caller, 2);
    v->s.cfm = um_cfm(16, 4, 8, 7, 0);
    v->s.ar[AR_PFS] = um_cfm(8, 0, 8, 3, 0) | (UINT64_C(41) << 52);
    v->s.br[2] = target | 15;
    const char *reason;
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 6 && F->calls &&
             F->insn[2].cls == T1_IC_RET && F->insn[3].insn.address == target &&
             F->fs[F->insn[2].fs_taken].cfm == um_cfm(8, 0, 8, 3, 0) && F->end_fs != 0);
    v->s.rse_dirty = 80;
    v->s.slot = 2;
    struct t1_compile_request rq, changed;
    const struct t1_compile_options opt = {0};
    unsigned stop;
    UM_CHECK(t1_request_from_vcpu(v, &opt, 0, &rq, &reason) && t1_request_head_allowed(&rq, &stop));
    v->s.ar[AR_PFS] = um_cfm(8, 1, 0, 0, 0);
    UM_CHECK(t1_request_from_vcpu(v, &opt, 0, &changed, &reason) && changed.key == rq.key &&
             !t1_request_head_allowed(&changed, &stop) && stop == IA64_OP_BR_RET);
    struct ia64_state *names = calloc(1, sizeof(*names));
    UM_CHECK(names && t1_form_request(&rq, names, T1_MAX_INSNS, F, &reason) == T1_COMPILED &&
             F->insn[0].cls == T1_IC_RET && F->ninsns == 4);
    for (unsigned sof = 17; sof <= 96; sof += 79) {
        v->s.ar[AR_PFS] = um_cfm(sof, 0, 8, 3, 0);
        v->s.rse_dirty = 96 - sof;
        UM_CHECK(t1_request_from_vcpu(v, &opt, 0, &changed, &reason) &&
                 t1_request_head_allowed(&changed, &stop) &&
                 t1_form_request(&changed, names, T1_MAX_INSNS, F, &reason) == T1_COMPILED &&
                 F->insn[0].cls == T1_IC_RET && F->ninsns == 4 &&
                 F->fs[F->insn[0].fs_taken].cfm == um_cfm(sof, 0, 8, 3, 0));
    }
    rq.return_observed = false;
    UM_CHECK(!t1_request_head_allowed(&rq, &stop) &&
             t1_form_request(&rq, names, T1_MAX_INSNS, F, &reason) == T1_REJECTED);
    rq.return_observed = true;
    rq.br_observed = 0;
    UM_CHECK(!t1_request_head_allowed(&rq, &stop));
    free(names);
    v->s.slot = 0;
    static const uint64_t unsupported[] = {
        8 | (1u << 7),           97, 8 | (2u << 14), 8 | (1u << 18), 8 | (UINT64_C(96) << 25),
        8 | (UINT64_C(48) << 32)};
    for (unsigned k = 0; k < sizeof(unsupported) / sizeof(unsupported[0]); ++k) {
        v->s.ar[AR_PFS] = unsupported[k];
        UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 2 && F->end_slot == 2);
    }
    v->s.ar[AR_PFS] = um_cfm(8, 0, 8, 3, 0);
    um_virtual_code(v);
    v->s.tr[0][0].va = PROG;
    v->s.tr[0][0].pte = PROG | 1 | (1u << 5) | (1u << 6) | (1u << 9);
    v->s.tr[0][0].itir = 12 << 2;
    v->s.tr[0][0].page_shift = 12;
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 0);
    uint64_t seq = v->mmu_seq;
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 2 && F->end_slot == 2 &&
             v->mmu_seq == seq);
    v->s.tr[0][3] = v->s.tr[0][0];
    v->s.tr[0][3].va = target;
    v->s.tr[0][3].pte = target | 1 | (1u << 5) | (1u << 6) | (1u << 9);
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 3);
    UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 6 && F->npages == 2);
    b[0].slot[2] = ienc_br_ret(6, 2);
    um_emit(PROG, b, 1);
    for (unsigned pred = 0; pred < 2; ++pred) {
        um_pr(v, 6, pred != 0);
        UM_CHECK(form(v, &reason) == T1_COMPILED && F->ninsns == 2 && F->end_slot == 2);
    }
    vcpu_destroy(v);
}

#include "tests/tier1/sidepath_lift.inc"
#include "tests/tier1/tail_lift.inc"

void unit_lift_test(const char *image);
void unit_lift_test(const char *image)
{
    um_machine_init();
    F = calloc(1, sizeof(*F));
    IR = calloc(1, sizeof(*IR));
    UM_CHECK(F && IR);
    form_tests(image);
    lift_tests(image);
    extension_tests();
    forest_tests();
    packed_tests();
    alloc_tests();
    call_form_tests();
    indirect_request_tests();
    observed_return_tests();
    sidepath_lift_test();
    tail_lift_test();
    free(F);
    free(IR);
}
