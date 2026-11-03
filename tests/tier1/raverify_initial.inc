/* SPDX-License-Identifier: GPL-2.0-or-later */
/* An undefined value is unknown both at entry and after the first edge.
 * In particular zero-filled storage must never stand for virtual register
 * zero. Check every physical register and every ordinary spill slot. */
static void raverify_initial_locations(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    ir->npoints = 1;
    ir->nuses = 1;
    ir->point[0].frame_count = 1;
    for (unsigned late = 0; late < 2; ++late)
    for (unsigned loc = 0; loc < T1_X86_NREGS + T1_SPILL_SCRATCH; ++loc) {
        struct t1_mir *m = t1_mir_create(ir);
        CHECK(m != NULL);
        CHECK(t1_mir_new_vreg(m, T1_I64, T1_VALUE_NONE) == 0);
        if (loc < T1_X86_NREGS) m->vreg[0].fixed = (uint8_t)loc;
        m->nblocks = 2;
        if (late) {
            rv_flow_jump(m, 1);
            m->block[0].count = m->ninsns;
            m->block[1].first = m->ninsns;
        }
        m->frame[0] = (struct t1_mir_frame){T1_ELEMENT_GR(14), T1_MF_VREG, 0, 0};
        m->frame_loc[0] = loc < T1_X86_NREGS ?
            (struct t1_loc){.kind = T1_LOC_REG, .reg = (uint8_t)loc} :
            (struct t1_loc){.kind = T1_LOC_SPILL, .slot = (uint16_t)(loc - T1_X86_NREGS)};
        m->nframe = 1;
        m->point[0] = (struct t1_mir_point){m->ninsns, T1_MIR_NONE, 0, 1, T1_PT_CHECKPOINT};
        struct t1_mir_insn *in = t1_mir_append(m, T1_MIR_CHECKPOINT);
        CHECK(in != NULL);
        in->point = 0;
        m->block[late].count = m->ninsns - m->block[late].first;
        char err[200];
        CHECK(!t1_raverify(m, err, sizeof(err)));
        CHECK(strstr(err, "frame location does not hold its value"));
        t1_mir_destroy(m);
    }
    free(ir);
    printf("unit_runtime: raverify initial locations: undefined vreg zero rejected at entry and first edge\n");
}
