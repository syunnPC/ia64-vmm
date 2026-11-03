/* SPDX-License-Identifier: GPL-2.0-or-later */
/* alloc may finish without materializing its PFS input. A later read of
 * LIVEIN(PFS) therefore cannot prove that an earlier clobber was undone. */
static void verify_home_bypass(struct t1_ir *ir)
{
    struct t1_form *f = calloc(1, sizeof(*f));
    UM_CHECK(f != NULL);
    f->ninsns = 1;
    f->insn[0].cls = T1_IC_ALLOC;
    f->insn[0].dst = 12;
    t1_ir_init(ir, f);
    uint16_t e = T1_ELEMENT_PFS;
    t1_elem_add(ir->frame_set, e);
    t1_elem_add(ir->w_set, e);
    UM_CHECK(t1_ir_new_block(ir, 0) == T1_ENTRY_BLOCK);
    UM_CHECK(t1_ir_new_block(ir, 0) == T1_HEADER_BLOCK);
    UM_CHECK(t1_ir_set_jump(ir, T1_ENTRY_BLOCK, T1_HEADER_BLOCK));
    t1_value saved = t1_ir_livein(ir, T1_I64, e);
    t1_value dirty = t1_ir_const(ir, T1_I64, 1);
    t1_value mem = t1_ir_livein(ir, T1_MEM, T1_ELEMENT_MEM);
    for (unsigned k = 0; k < 2; ++k) {
        uint32_t p = t1_ir_new_point(ir, T1_PT_HELPER, 0x1000, 0, 0, T1_HEADER_BLOCK, 0);
        UM_CHECK(t1_ir_set_frame(ir, p, &e, &saved, NULL, 1));
        uint32_t o = t1_ir_append(ir, T1_HEADER_BLOCK, k ? T1_IR_GHELPER : T1_IR_HELPER,
                                  0, 0, &mem, 1, 0);
        UM_CHECK(o != T1_NO_OP && t1_ir_add_operand(ir, o, k ? saved : dirty, e));
        ir->op[o].point = p;
        ir->point[p].op = o;
        mem = T1_VALUE(o, T1_RES_HELPER_mem);
    }
    uint32_t p = t1_ir_new_point(ir, T1_PT_EXIT, 0x1010, 0, 0, T1_HEADER_BLOCK, 0);
    t1_value none = T1_VALUE_NONE;
    uint16_t home = T1_USE_HOME;
    UM_CHECK(t1_ir_set_frame(ir, p, &e, &none, &home, 1));
    UM_CHECK(t1_ir_set_exit(ir, T1_HEADER_BLOCK, p, 0));
    expect_fail(ir, 0, "home of element", "HOME cannot depend on skipped materialization");
    ++negatives;
    free(f);
}

/* Exercise a nonempty HOME proof set at word boundaries, with multiple
 * bits in a word and in the final partial word. A helper writes back each
 * element's entry value in the valid case and a different value in the
 * invalid case; the later HOME frame must then be rejected. */
static void verify_home_bits(struct t1_ir *ir)
{
    static const uint16_t element[] = {1, 63, 64, 127, 129, 191, 192,
        255, 257, 319, 320, 383, 384, 447, 448, 511, 512, 579};
    enum { N = sizeof(element) / sizeof(element[0]) };
    uint16_t flags[N];
    t1_value none[N], value[N];
    for (unsigned invalid = 0; invalid <= N; ++invalid) {
        t1_ir_init(ir, NULL);
        UM_CHECK(t1_ir_new_block(ir, 0) == T1_ENTRY_BLOCK);
        UM_CHECK(t1_ir_new_block(ir, 0) == T1_HEADER_BLOCK);
        UM_CHECK(t1_ir_set_jump(ir, T1_ENTRY_BLOCK, T1_HEADER_BLOCK));
        t1_value mem = t1_ir_livein(ir, T1_MEM, T1_ELEMENT_MEM);
        for (unsigned k = 0; k < N; ++k) {
            uint16_t e = element[k];
            t1_elem_add(ir->frame_set, e);
            t1_elem_add(ir->w_set, e);
            uint8_t type = e >= 128 && e < T1_ELEMENT_LC ? T1_I1 : T1_I64;
            value[k] = invalid == k + 1 ? t1_ir_const(ir, type, 1) : t1_ir_livein(ir, type, e);
            none[k] = T1_VALUE_NONE;
            flags[k] = T1_USE_HOME;
        }
        uint32_t pt = t1_ir_new_point(ir, T1_PT_HELPER, 0x1000, 0, 0, T1_HEADER_BLOCK, 0);
        UM_CHECK(t1_ir_set_frame(ir, pt, element, none, flags, N));
        uint32_t helper = t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_HELPER, 0, 0, &mem, 1, 0);
        UM_CHECK(helper != T1_NO_OP);
        for (unsigned k = 0; k < N; ++k) UM_CHECK(t1_ir_add_operand(ir, helper, value[k], element[k]));
        ir->op[helper].point = pt;
        ir->point[pt].op = helper;
        pt = t1_ir_new_point(ir, T1_PT_EXIT, 0x1010, 0, 0, T1_HEADER_BLOCK, 0);
        UM_CHECK(t1_ir_set_frame(ir, pt, element, none, flags, N));
        UM_CHECK(t1_ir_set_exit(ir, T1_HEADER_BLOCK, pt, 0));
        if (!invalid) expect_ok(ir, 0, "HOME proof bitset");
        else {
            char wanted[80];
            snprintf(wanted, sizeof(wanted), "point 1: home of element %u is not proven", element[invalid - 1]);
            expect_fail(ir, 0, wanted, "HOME bit invalidated by helper write-back");
            ++negatives;
        }
    }
    verify_home_bypass(ir);
}
