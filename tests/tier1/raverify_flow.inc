/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A later predecessor, or a self-edge, can invalidate a block that has
 * already passed verification. Its frame and register uses must be
 * checked again after the incoming state changes. */
static void rv_flow_jump(struct t1_mir *m, unsigned target)
{
    struct t1_mir_insn *in = t1_mir_append(m, T1_X86_JMP);
    CHECK(in != NULL);
    in->nopnd = 1;
    in->opnd[0] = (struct t1_mir_opnd){.kind = T1_MO_LABEL, .imm = target};
}

static void rv_flow_define(struct t1_mir *m, t1_vreg v, uint8_t reg)
{
    struct t1_mir_insn *in = t1_mir_append(m, T1_MIR_LIVEIN);
    CHECK(in != NULL);
    in->nopnd = 2;
    in->opnd[0] = (struct t1_mir_opnd){T1_MO_VREG, reg, v, 0};
    in->opnd[1] = (struct t1_mir_opnd){.kind = T1_MO_IMM, .imm = T1_ELEMENT_GR(14 + v)};
}

static void rv_flow_use(struct t1_mir *m)
{
    struct t1_mir_insn *in = t1_mir_append(m, T1_X86_CMP64_RI);
    CHECK(in != NULL);
    in->nopnd = 2;
    in->opnd[0] = (struct t1_mir_opnd){T1_MO_VREG, T1_RAX, 0, 0};
    in->opnd[1] = (struct t1_mir_opnd){.kind = T1_MO_IMM, .imm = 0};
}

static void raverify_changed_entry(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    for (unsigned frame = 0; frame < 2; ++frame)
    for (unsigned conflict = 0; conflict < 3; ++conflict) {
        /* B0 branches to B1/B2; B2 reaches B1 after its first visit.
         * conflict 0 preserves RAX, 1 overwrites it on B2, and 2
         * overwrites it on B1 before a self-edge. */
        memset(ir, 0, sizeof(*ir));
        ir->npoints = frame;
        ir->nuses = frame;
        ir->point[0].frame_count = frame;
        struct t1_mir *m = t1_mir_create(ir);
        CHECK(m != NULL);
        CHECK(t1_mir_new_vreg(m, T1_I64, T1_VALUE_NONE) == 0);
        CHECK(t1_mir_new_vreg(m, T1_I64, T1_VALUE_NONE) == 1);
        m->nblocks = 4;
        rv_flow_define(m, 0, T1_RAX);
        rv_flow_use(m);
        m->block[0].term = m->ninsns;
        struct t1_mir_insn *in = t1_mir_append(m, T1_X86_JCC + T1_CC_E);
        CHECK(in != NULL);
        in->nopnd = 1;
        in->opnd[0] = (struct t1_mir_opnd){.kind = T1_MO_LABEL, .imm = 1};
        rv_flow_jump(m, 2);
        m->block[0].count = m->ninsns;
        m->block[0].nsucc = 2;
        m->block[0].succ[0] = 1; m->block[0].succ[1] = 2;

        m->block[1].first = m->ninsns;
        if (frame) {
            m->nframe = 1;
            m->frame[0] = (struct t1_mir_frame){T1_ELEMENT_GR(14), T1_MF_VREG, 0, 0};
            m->frame_loc[0] = (struct t1_loc){.kind = T1_LOC_REG, .reg = T1_RAX};
            m->point[0] = (struct t1_mir_point){m->ninsns, T1_MIR_NONE, 0, 1, T1_PT_CHECKPOINT};
            in = t1_mir_append(m, T1_MIR_CHECKPOINT);
            CHECK(in != NULL);
            in->point = 0;
        } else rv_flow_use(m);
        if (conflict == 2) rv_flow_define(m, 1, T1_RAX);
        m->block[1].term = m->ninsns;
        rv_flow_jump(m, conflict == 2 ? 1 : 3);
        m->block[1].count = m->ninsns - m->block[1].first;
        m->block[1].nsucc = 1;
        m->block[1].succ[0] = conflict == 2 ? 1 : 3;

        m->block[2].first = m->ninsns;
        rv_flow_define(m, 1, conflict == 1 ? T1_RAX : T1_RDX);
        m->block[2].term = m->ninsns;
        rv_flow_jump(m, 1);
        m->block[2].count = m->ninsns - m->block[2].first;
        m->block[2].nsucc = 1; m->block[2].succ[0] = 1;
        m->block[3].first = m->block[3].term = m->ninsns;
        CHECK(t1_mir_append(m, T1_X86_RET) != NULL);
        m->block[3].count = 1;

        char err[200];
        bool ok = t1_raverify(m, err, sizeof(err));
        CHECK(ok == (conflict == 0));
        if (conflict) CHECK(strstr(err, frame ? "frame location" : "used vreg"));
        t1_mir_destroy(m);
    }
    free(ir);
    printf("unit_runtime: raverify revisits changed entries: later predecessors, self-edges, operands and frames\n");
}
