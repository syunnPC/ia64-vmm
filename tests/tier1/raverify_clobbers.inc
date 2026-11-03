/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The symbolic verifier must invalidate every bit of a declared clobber,
 * retain the other physical locations, and accept a subsequent definition.
 * Fixed operands observe locations without prescribing which registers may
 * appear in a frame. This MIR is checked only, never emitted or executed. */
static void raverify_clobber_bits(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    ir->npoints = 1;
    for (unsigned kind = 0; kind < 3; ++kind)
    for (unsigned reg = 0; reg < T1_X86_NREGS; ++reg)
    for (unsigned restore = 0; restore < 2; ++restore) {
        struct t1_mir *m = t1_mir_create(ir);
        CHECK(m != NULL);
        CHECK(t1_mir_new_vreg(m, T1_I64, T1_VALUE_NONE) == 0);
        CHECK(t1_mir_new_vreg(m, T1_I64, T1_VALUE_NONE) == 1);
        uint8_t fnreg = reg == T1_RBX ? T1_RBP : T1_RBX;
        m->vreg[0].fixed = (uint8_t)reg;
        m->vreg[1].fixed = fnreg;
        m->nblocks = 1;
        rv_flow_define(m, 0, (uint8_t)reg);
        rv_flow_define(m, 1, fnreg);
        struct t1_mir_insn *in;
        uint16_t mask = 0;
        if (!kind) {
            in = t1_mir_append(m, T1_X86_CMP64_RI);
            CHECK(in != NULL);
            in->nopnd = 2;
            in->opnd[0] = (struct t1_mir_opnd){T1_MO_VREG, (uint8_t)reg, 0, 0};
            in->opnd[1] = (struct t1_mir_opnd){.kind = T1_MO_IMM, .imm = 0};
        } else if (kind == 1) {
            in = t1_mir_append(m, T1_X86_CALL_R);
            CHECK(in != NULL);
            in->nopnd = 1;
            in->opnd[0] = (struct t1_mir_opnd){T1_MO_VREG, fnreg, 1, 0};
            mask = T1_X86_CALLER_SAVED;
        } else {
            m->point[0] = (struct t1_mir_point){m->ninsns, T1_MIR_NONE, 0, 0, T1_PT_HELPER};
            in = t1_mir_append(m, T1_MIR_HELPER);
            CHECK(in != NULL);
            in->point = 0;
            mask = T1_WRAPPER_CLOBBERS;
        }
        if (restore) rv_flow_define(m, 0, (uint8_t)reg);
        in = t1_mir_append(m, T1_X86_CMP64_RI);
        CHECK(in != NULL);
        in->nopnd = 2;
        in->opnd[0] = (struct t1_mir_opnd){T1_MO_VREG, (uint8_t)reg, 0, 0};
        in->opnd[1] = (struct t1_mir_opnd){.kind = T1_MO_IMM, .imm = 0};
        m->block[0].count = m->ninsns;
        char err[200];
        bool ok = t1_raverify(m, err, sizeof(err));
        CHECK(ok == (restore || !(mask & T1_X86_REGBIT(reg))));
        if (!ok) CHECK(strstr(err, "register does not hold the used vreg"));
        t1_mir_destroy(m);
    }
    free(ir);
    printf("unit_runtime: raverify clobbers: empty, helper and call masks across all 16 locations\n");
}
