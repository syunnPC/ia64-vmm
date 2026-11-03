/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The shared wrapper preserves all allocatable GPRs except declared
 * results. Exceptional completion retains the capture from before the
 * call, including the old values in the result registers. */
static const uint8_t helper_regs[] = {
    T1_RAX, T1_RCX, T1_RDX, T1_RBX, T1_RBP, T1_RSI,
    T1_RDI, T1_R8, T1_R9, T1_R10, T1_R15
};

static struct t1_mir *helper_mir(struct t1_ir *ir, unsigned n, unsigned results,
                                unsigned first, bool assigned)
{
    memset(ir, 0, sizeof(*ir));
    ir->npoints = 2;
    ir->nuses = 2 * n;
    ir->point[0].kind = T1_PT_HELPER;
    ir->point[1].kind = T1_PT_EXIT;
    ir->point[0].frame_count = ir->point[1].frame_count = n;
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m != NULL);
    m->flags = T1_COMPILE_VERIFY;
    m->nblocks = 1;
    m->nframe = 2 * n;
    for (unsigned k = 0; k < n; ++k) {
        t1_vreg v = t1_mir_new_vreg(m, T1_I64, T1_VALUE_NONE);
        struct t1_mir_insn *in = t1_mir_append(m, T1_MIR_LIVEIN);
        CHECK(in != NULL && v == k);
        in->nopnd = 2;
        in->opnd[0] = (struct t1_mir_opnd){T1_MO_VREG, assigned ? helper_regs[k] : T1_X86_NOREG, v, 0};
        in->opnd[1] = (struct t1_mir_opnd){.kind = T1_MO_IMM, .imm = T1_ELEMENT_GR(12 + k)};
        m->frame[k] = (struct t1_mir_frame){T1_ELEMENT_GR(12 + k), T1_MF_VREG, v, 0};
        m->frame[n + k] = m->frame[k];
        m->frame_loc[k] = m->frame_loc[n + k] = (struct t1_loc){.kind = T1_LOC_REG, .reg = helper_regs[k]};
    }
    struct t1_mir_insn *in = t1_mir_append(m, T1_MIR_FORCE);
    CHECK(in != NULL);
    in->point = 0;
    m->point[0] = (struct t1_mir_point){m->ninsns, m->ninsns - 1, 0, n, T1_PT_HELPER};
    in = t1_mir_append(m, T1_MIR_HELPER);
    CHECK(in != NULL);
    in->point = 0;
    in->nopnd = results;
    for (unsigned d = 0; d < results; ++d) {
        unsigned k = (first + d) % n;
        t1_vreg v = t1_mir_new_vreg(m, T1_I1, T1_VALUE_NONE);
        in->opnd[d] = (struct t1_mir_opnd){T1_MO_VREG, assigned ? helper_regs[k] : T1_X86_NOREG, v, 0};
        m->frame[n + k].vreg = v;
    }
    /* Arithmetic consumers and recovery-only values both cross the call. */
    in = t1_mir_append(m, T1_X86_CMP64_RR);
    CHECK(in != NULL);
    in->nopnd = 2;
    for (unsigned k = 0; k < 2; ++k)
        in->opnd[k] = (struct t1_mir_opnd){T1_MO_VREG, assigned ? helper_regs[k] : T1_X86_NOREG,
                                         m->frame[n + k].vreg, 0};
    m->block[0].term = m->ninsns;
    m->point[1] = (struct t1_mir_point){m->ninsns, T1_MIR_NONE, n, n, T1_PT_EXIT};
    in = t1_mir_append(m, T1_MIR_EXIT);
    CHECK(in != NULL);
    in->point = 1;
    m->block[0].count = m->ninsns;
    return m;
}

static void helper_wrapper(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    struct vcpu *v = um_vcpu(UM_CODE);
    CHECK(ir != NULL);
    static struct t1_exec x;
    struct t1_vcpu *tv = umt_tv(v);
    /* Rotate the output pair through every saved register. The remaining
     * nine values must survive both the C call and the wrapper's return. */
    for (unsigned allocation = 0; allocation < 3; ++allocation)
    for (unsigned first = 0; first < sizeof(helper_regs); ++first) {
        struct t1_mir *m = helper_mir(ir, sizeof(helper_regs), 2, first, allocation == 0);
        struct t1_emitted e;
        char err[200];
        if (allocation) {
            m->vreg[0].fixed = T1_RAX;
            if (allocation == 2)
                m->allocatable = T1_X86_REGBIT(T1_RAX) | T1_X86_REGBIT(T1_RCX) |
                                 T1_X86_REGBIT(T1_RDX) | T1_X86_REGBIT(T1_RBX) | T1_X86_REGBIT(T1_RSI);
            CHECK(t1_regalloc(m));
            CHECK(t1_raverify(m, err, sizeof(err)));
            CHECK(allocation == 2 ? m->total_spills > 0 : m->total_spills == 0);
        }
        CHECK(t1_emit(m, &e, err, sizeof(err)));
        size_t pages = (e.size + 4095) / 4096;
        uint8_t *code = t1_arena_alloc(pages);
        CHECK(code != NULL);
        t1_emit_place(&e, code);
        hal_code_set_executable(code, pages);
        struct t1_helper_info h = {
            .insn = {.opcode = IA64_OP_TNAT_Z, .valid = true,
                     .operands.integer = {.source2 = 20, .predicate1 = 6, .predicate2 = 7}},
            .wr = {T1_ELEMENT_PR(6), T1_ELEMENT_PR(7)},
            .def_reg = {m->insn[m->point[0].insn].opnd[0].fixed,
                        m->insn[m->point[0].insn].opnd[1].fixed}
        };
        struct t1_point points[2] = {{.ip = UM_CODE, .slot = 1}, {.ip = UM_CODE + 16}};
        uint16_t helper_of[2] = {0, UINT16_MAX};
        struct t1_region r = {.helper = &h, .helper_of = helper_of, .point = points};
        for (unsigned mode = 0; mode < 4; ++mode) {
            memset(&x, 0, sizeof(x));
            x.v = v; x.tv = tv; x.region = &r;
            for (unsigned k = 0; k < sizeof(helper_regs); ++k)
                um_gr(v, 12 + k, UINT64_C(0xfedcba9876543200) + k, false);
            um_gr(v, 20, UINT64_C(0xfedcba9876543208), mode == 1);
            h.insn.opcode = mode == 2 ? IA64_OP_ILLEGAL : IA64_OP_TNAT_Z;
            x.force[0] = mode == 3 ? 1 : 0;
            CHECK(((t1_entry_fn)(uintptr_t)code)(v, 99, &x) == T1_RET_POINT);
            CHECK(x.point == (mode >= 2 ? 0u : 1u) && x.raw_r13 == 99);
            for (unsigned k = 0; k < sizeof(helper_regs); ++k) {
                uint64_t expected = UINT64_C(0xfedcba9876543200) + k;
                if (mode < 2 && k == first) expected = mode == 0;
                if (mode < 2 && k == (first + 1) % sizeof(helper_regs)) expected = mode == 1;
                unsigned frame = (mode >= 2 ? 0 : sizeof(helper_regs)) + k;
                CHECK(t1_loc_value(&x, &m->frame_loc[frame]) == expected);
            }
            CHECK(x.v == v && x.region == &r);
        }
        t1_arena_free(code, pages);
        t1_emit_free(&e);
        t1_mir_destroy(m);
    }
    free(ir);
    umt_release(v);
    vcpu_destroy(v);
    printf("unit_runtime: helper wrapper: every GPR preserved, output pairs, NaT, exceptional and forced capture, allocation and spill pressure\n");
}

static void helper_allocation(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    char err[200];
    for (unsigned results = 0; results <= 2; ++results) {
        struct t1_mir *m = helper_mir(ir, 8, results, 6, false);
        /* A fixed caller-saved value is also live across the helper. */
        m->vreg[0].fixed = T1_RAX;
        CHECK(t1_regalloc(m));
        CHECK(t1_raverify(m, err, sizeof(err)));
        CHECK(m->total_spills == 0);
        unsigned h = m->point[0].insn;
        struct t1_mir_insn *in = &m->insn[h];
        struct t1_mir_effects fx;
        t1_mir_effects(in, &fx);
        CHECK(fx.ndef == results && fx.point && fx.call && fx.flags_clobber == T1_XF_ALL);
        CHECK(!(fx.clobber_regs & T1_ALLOCATABLE));
        /* Results overwrite their assigned locations on the normal edge. */
        if (results) {
            uint8_t save = in->opnd[0].fixed;
            in->opnd[0].fixed = T1_RAX;
            CHECK(!t1_raverify(m, err, sizeof(err)));
            in->opnd[0].fixed = save;
        }
        /* The exceptional frame must still name the old value. */
        struct t1_loc save = m->frame_loc[0];
        m->frame_loc[0].reg = T1_R11;
        CHECK(!t1_raverify(m, err, sizeof(err)));
        m->frame_loc[0] = save;
        /* A flags consumer immediately after a helper has no producer. */
        struct t1_mir_insn saved = m->insn[h + 1];
        m->insn[h + 1].op = T1_X86_SET8 + T1_CC_E;
        CHECK(!t1_raverify(m, err, sizeof(err)) && strstr(err, "flags"));
        m->insn[h + 1] = saved;
        CHECK(t1_raverify(m, err, sizeof(err)));
        t1_mir_destroy(m);
    }
    free(ir);
    printf("unit_runtime: helper allocation: fixed, live-through and recovery values, results, flags, no spills\n");
}
