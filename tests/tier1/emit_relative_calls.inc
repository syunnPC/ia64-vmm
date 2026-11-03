/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Layout-only MIR exercises the shared absolute-fixup limit. Reusing a
 * point makes the boundary reachable without relaxing the IR point cap;
 * these artificial bodies are never executed. Existing runtime/codegen
 * suites execute real helper/miss wrappers and their preservation ABI. */
static void emit_relative_case(unsigned calls, unsigned count, unsigned kind, bool calls_last)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    t1_ir_init(ir, NULL);
    t1_ir_new_block(ir, 0);
    uint32_t hp = t1_ir_new_point(ir, T1_PT_HELPER, UM_CODE, 0, 0, 0, 0);
    uint32_t xp = t1_ir_new_point(ir, T1_PT_EXIT, UM_CODE, 0, 0, 0, 0);
    CHECK(hp != T1_NO_POINT && xp != T1_NO_POINT);
    /* The emitter reserves cold-code bytes per IR point. Repeated layout
     * checkpoints below reuse one point, unlike real MIR, so retain a
     * full legal point table to avoid testing output capacity first. */
    if (kind == T1_MIR_CHECKPOINT)
        while (ir->npoints < T1_MAX_POINTS)
            CHECK(t1_ir_new_point(ir, T1_PT_EXIT, UM_CODE, 0, 0, 0, 0) != T1_NO_POINT);
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m != NULL);
    m->nblocks = 1;
    m->flags = T1_COMPILE_VERIFY;
    for (unsigned phase = 0; phase < 2; ++phase) {
        if (!!phase == calls_last) {
            for (unsigned k = 0; k < calls; ++k) {
                struct t1_mir_insn *in = t1_mir_append(m, T1_MIR_HELPER);
                CHECK(in != NULL);
                in->point = hp;
            }
        } else {
            for (unsigned k = 0; k < count; ++k) {
                struct t1_mir_insn *in = t1_mir_append(m, (uint16_t)kind);
                CHECK(in != NULL);
                in->point = kind == T1_MIR_FORCE ? hp : xp;
                in->opnd[0].imm = 7;
            }
        }
    }
    m->block[0].term = m->ninsns;
    struct t1_mir_insn *exit = t1_mir_append(m, T1_MIR_EXIT);
    CHECK(exit != NULL);
    exit->point = xp;
    m->block[0].count = m->ninsns;
    unsigned mandatory = count + 1;
    unsigned relative = mandatory >= 1024 ? 0 : calls < 1024 - mandatory ? calls : 1024 - mandatory;
    struct t1_emitted e;
    char err[160] = {0};
    bool ok = t1_emit(m, &e, err, sizeof(err));
    if (ok != (mandatory <= 1024))
        fprintf(stderr, "relative calls: calls=%u count=%u kind=%u last=%u: %s\n",
                calls, count, kind, calls_last, err);
    CHECK(ok == (mandatory <= 1024));
    if (!ok) {
        CHECK(!e.buf && !e.pt && strstr(err, "stub fixup limit") != NULL);
    } else {
        CHECK(e.nabs == mandatory + relative);
        unsigned direct = 0, indirect = 0, calls_fixed = 0;
        for (size_t at = 0; at < e.size;) {
            struct t1_x86_decoded d;
            CHECK(t1_x86_decode(e.buf + at, e.size - at, &d));
            if (d.op == T1_X86_CALL_REL) { CHECK(d.length == 5); ++direct; }
            if (d.op == T1_X86_CALL_R) ++indirect;
            at += d.length;
        }
        CHECK(direct == relative && indirect == calls - relative);
        for (unsigned k = 0; k < e.nabs; ++k) {
            uint32_t at = e.abs_fix[k];
            CHECK(at && at + 4 <= e.size);
            if (e.buf[at - 1] == 0xe8) {
                CHECK(e.abs_target[k] == t1_stub.helper);
                ++calls_fixed;
            } else CHECK(e.buf[at - 1] == 0xe9 && e.abs_target[k] == t1_stub.exit_nosave);
        }
        CHECK(calls_fixed == relative);
        size_t pages = (e.size + 4095) / 4096;
        uint8_t *code[2] = {t1_arena_alloc(pages), t1_arena_alloc(pages)};
        CHECK(code[0] && code[1] && code[0] != code[1]);
        for (unsigned c = 0; c < 2; ++c) {
            t1_emit_place(&e, code[c]);
            for (unsigned k = 0; k < e.nabs; ++k) {
                int32_t delta;
                memcpy(&delta, code[c] + e.abs_fix[k], 4);
                uintptr_t target = (uintptr_t)(code[c] + e.abs_fix[k] + 4) + (intptr_t)delta;
                CHECK(target == e.abs_target[k]);
            }
            t1_arena_free(code[c], pages);
        }
        t1_emit_free(&e);
    }
    t1_mir_destroy(m);
    free(ir);
}

/* A raw local call followed by JMP must never take the JCC inversion or
 * fallthrough-elimination path, even when its callee is the next block. */
static void emit_relative_local_call(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    t1_ir_init(ir, NULL);
    for (unsigned k = 0; k < 3; ++k) t1_ir_new_block(ir, 0);
    uint32_t xp = t1_ir_new_point(ir, T1_PT_EXIT, UM_CODE, 0, 0, 0, 0);
    CHECK(xp != T1_NO_POINT);
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m != NULL);
    m->nblocks = 3;
    struct t1_mir_insn *in = t1_mir_append(m, T1_X86_CALL_REL);
    CHECK(in != NULL);
    in->opnd[0] = (struct t1_mir_opnd){T1_MO_LABEL, T1_X86_NOREG, T1_VREG_NONE, 1};
    in = t1_mir_append(m, T1_X86_JMP);
    CHECK(in != NULL);
    in->opnd[0] = (struct t1_mir_opnd){T1_MO_LABEL, T1_X86_NOREG, T1_VREG_NONE, 2};
    CHECK(t1_mir_append(m, T1_X86_RET) != NULL);
    in = t1_mir_append(m, T1_MIR_EXIT);
    CHECK(in != NULL);
    in->point = xp;
    m->block[0].count = 2;
    m->block[1].first = 2; m->block[1].count = 1;
    m->block[2].first = 3; m->block[2].count = 1;
    struct t1_emitted e;
    char err[160];
    CHECK(t1_emit(m, &e, err, sizeof(err)));
    unsigned calls = 0;
    for (size_t at = 0; at < e.size;) {
        struct t1_x86_decoded d;
        CHECK(t1_x86_decode(e.buf + at, e.size - at, &d));
        if (d.op == T1_X86_CALL_REL) {
            int32_t delta;
            memcpy(&delta, e.buf + at + 1, 4);
            CHECK(d.length == 5 && delta == 5 && at + 10 < e.size);
            CHECK(e.buf[at + 5] == 0xe9 && e.buf[at + 10] == 0xc3);
            ++calls;
        }
        at += d.length;
    }
    CHECK(calls == 1 && e.nabs == 1);
    t1_emit_free(&e);
    t1_mir_destroy(m);
    free(ir);
}

static void emit_relative_calls(void)
{
    static const struct { unsigned calls, count, kind; } cases[] = {
        {0, 0, T1_MIR_EXIT}, {2, 0, T1_MIR_EXIT}, {1025, 0, T1_MIR_EXIT},
        {2, 1022, T1_MIR_EXIT}, {2, 1023, T1_MIR_EXIT}, {2, 1024, T1_MIR_EXIT},
        {2, 1022, T1_MIR_CHECKPOINT}, {2, 1023, T1_MIR_CHECKPOINT}, {2, 1024, T1_MIR_CHECKPOINT},
        {2, 1022, T1_MIR_HGUARD}, {2, 1023, T1_MIR_HGUARD},
        {2, 1022, T1_MIR_FORCE}, {2, 1023, T1_MIR_FORCE},
    };
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k)
        for (unsigned last = 0; last < 2; ++last)
            emit_relative_case(cases[k].calls, cases[k].count, cases[k].kind, !!last);
    emit_relative_local_call();
    printf("unit_runtime: relative wrapper calls: 26 layouts, relocation at two addresses, "
           "all mandatory fixup kinds at the 1024-entry limit and indirect fallback; local call retained\n");
}
