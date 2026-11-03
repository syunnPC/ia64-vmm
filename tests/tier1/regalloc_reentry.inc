/* SPDX-License-Identifier: GPL-2.0-or-later */
static void reentry_use(struct t1_mir *m, t1_vreg v)
{
    struct t1_mir_insn *in = t1_mir_append(m, T1_X86_CMP64_RI);
    CHECK(in != NULL);
    in->nopnd = 2;
    in->opnd[0] = (struct t1_mir_opnd){T1_MO_VREG, T1_X86_NOREG, v, 0};
    in->opnd[1] = (struct t1_mir_opnd){.kind = T1_MO_IMM, .imm = 0};
}

static void regalloc_reentry(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m != NULL);
    m->allocatable = T1_X86_REGBIT(T1_RAX) | T1_X86_REGBIT(T1_RCX);
    m->nblocks = 4;
    for (unsigned v = 0; v < 4; ++v)
        CHECK(t1_mir_new_vreg(m, T1_I64, T1_VALUE_NONE) == v);

    rv_flow_define(m, 0, T1_X86_NOREG);
    rv_flow_define(m, 1, T1_X86_NOREG);
    reentry_use(m, 1);
    m->block[0].term = m->ninsns;
    struct t1_mir_insn *in = t1_mir_append(m, T1_X86_JCC + T1_CC_E);
    CHECK(in != NULL);
    in->nopnd = 1;
    in->opnd[0] = (struct t1_mir_opnd){.kind = T1_MO_LABEL, .imm = 1};
    rv_flow_jump(m, 2);
    m->block[0].count = m->ninsns;
    m->block[0].nsucc = 2;
    m->block[0].succ[0] = 1; m->block[0].succ[1] = 2;

    /* v0 has a hole on B1. v2 borrows its register, then pressure
     * evicts v1. Reloading v1 evicts both v2 and the inactive v0. */
    m->block[1].first = m->ninsns;
    rv_flow_define(m, 2, T1_X86_NOREG);
    rv_flow_define(m, 3, T1_X86_NOREG);
    reentry_use(m, 2);
    reentry_use(m, 1);
    reentry_use(m, 3);
    reentry_use(m, 2);
    m->block[1].term = m->ninsns;
    rv_flow_jump(m, 3);
    m->block[1].count = m->ninsns - m->block[1].first;
    m->block[1].nsucc = 1; m->block[1].succ[0] = 3;

    m->block[2].first = m->ninsns;
    reentry_use(m, 0);
    reentry_use(m, 1);
    m->block[2].term = m->ninsns;
    rv_flow_jump(m, 3);
    m->block[2].count = m->ninsns - m->block[2].first;
    m->block[2].nsucc = 1; m->block[2].succ[0] = 3;
    m->block[3].first = m->block[3].term = m->ninsns;
    CHECK(t1_mir_append(m, T1_X86_RET) != NULL);
    m->block[3].count = 1;

    CHECK(t1_regalloc(m));
    char err[200];
    CHECK(t1_raverify(m, err, sizeof(err)));
    CHECK(m->total_spills > 0);
    t1_mir_destroy(m);
    free(ir);
    printf("unit_runtime: regalloc re-entry: immediate use after a lifetime hole under pressure\n");
}
