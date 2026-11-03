/* SPDX-License-Identifier: GPL-2.0-or-later */
/* HOME needs the original value on every path, before the point's helper.
 * Architectural halves and predicate bits have independent homes. */
static uint32_t home_helper(uint16_t b, t1_value mem, const uint16_t *e,
                            const t1_value *saved, const t1_value *read, unsigned n)
{
    uint32_t p = t1_ir_new_point(IR, T1_PT_HELPER, 0x5000, 0, 0, b, 0);
    UM_CHECK(t1_ir_set_frame(IR, p, e, saved, NULL, n));
    uint32_t o = t1_ir_append(IR, b, T1_IR_HELPER, 0, 0, &mem, 1, 0);
    UM_CHECK(o != T1_NO_OP);
    for (unsigned k = 0; k < n; ++k)
        UM_CHECK(t1_ir_add_operand(IR, o, read[k], e[k]));
    IR->op[o].point = p;
    IR->point[p].op = o;
    return o;
}

static void frame_home_flow(void)
{
    static const uint16_t e[] = {T1_ELEMENT_GR(8), T1_ELEMENT_NAT(8), T1_ELEMENT_PR(6),
        T1_ELEMENT_PR(7), T1_ELEMENT_FR(6), T1_ELEMENT_FR_META(6), T1_ELEMENT_FR_META(127)};
    enum { N = sizeof(e) / sizeof(e[0]) };
    /* Straight paths, a join numbered before its predecessors, and a loop
     * whose clobber reaches a header point on the following iteration. */
    for (unsigned shape = 0; shape < 3; ++shape)
    for (unsigned changed = 0; changed < N + 1; ++changed) {
        start(e, N);
        t1_value saved[N], read[N];
        for (unsigned k = 0; k < N; ++k) {
            uint8_t type = e[k] >= 128 && e[k] < T1_ELEMENT_LC ? T1_I1 : T1_I64;
            t1_elem_add(IR->w_set, e[k]);
            saved[k] = t1_ir_livein(IR, type, e[k]);
            read[k] = changed == k ? K(type, 1) : saved[k];
        }
        uint16_t h = T1_HEADER_BLOCK, join = h, a = h, b = h;
        t1_value mem = t1_ir_livein(IR, T1_MEM, T1_ELEMENT_MEM);
        uint32_t mp = T1_NO_OP;
        if (shape) {
            join = t1_ir_new_block(IR, 0);
            a = t1_ir_new_block(IR, 0);
            b = t1_ir_new_block(IR, 0);
            UM_CHECK(t1_ir_set_branch(IR, h, saved[2], a, shape == 1 ? b : join, 0));
        }
        if (shape == 2) {
            mp = t1_ir_add_param(IR, h, T1_MEM, T1_ELEMENT_MEM);
            UM_CHECK(t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, mem, T1_ELEMENT_MEM));
            mem = T1_VALUE(mp, 0);
        }
        uint32_t helper = home_helper(a, mem, e, saved, read, N);
        /* The extra case is an actual predicate output, even when all
         * materialized inputs still equal their original values. */
        if (changed == N)
            IR->op[helper].wr[0] = e[3];
        if (shape == 1) {
            home_helper(b, mem, e, saved, saved, N);
            UM_CHECK(t1_ir_set_jump(IR, a, join) && t1_ir_set_jump(IR, b, join));
        } else if (shape == 2) {
            UM_CHECK(t1_ir_set_jump(IR, a, b));
            uint32_t p = t1_ir_new_point(IR, T1_PT_CHECKPOINT, 0x5000, 0, 0, b, 0);
            UM_CHECK(t1_ir_set_frame(IR, p, e, saved, NULL, N));
            UM_CHECK(t1_ir_set_backedge(IR, b, h, p, 0));
            UM_CHECK(t1_ir_edge_add_arg(IR, b, 0, T1_VALUE(helper, T1_RES_HELPER_mem),
                                      T1_ELEMENT_MEM));
        }
        uint32_t p = exit_with(join, e, saved, N);
        verify(0, "home flow before");
        t1_opt_frames(IR);
        verify(0, "home flow after");
        for (unsigned k = 0; k < N; ++k) {
            bool dirty = k == (changed == N ? 3 : changed);
            UM_CHECK(!!(entry(p, k)->flags & T1_USE_HOME) == (!dirty && k != 1));
            UM_CHECK(!!(entry(IR->op[helper].point, k)->flags & T1_USE_HOME) ==
                     !(k == 1 || (shape == 2 && dirty)));
        }
        t1_opt_frames(IR);
        verify(0, "home flow repeated");
        t1_natguard(IR);
        t1_opt_frames(IR);
        verify(T1_VERIFY_NAT_FOLDED, "home flow exact NaT");
        for (unsigned k = 0; k < N; ++k)
            UM_CHECK(!!(entry(p, k)->flags & T1_USE_HOME) ==
                     (k != (changed == N ? 3 : changed)));
    }
}

/* A false qp may still materialize the other helper operands. A later
 * read of the saved value cannot repair the proof: fast helpers can skip
 * materialization (alloc is one such path). */
static void frame_home_bypass(void)
{
    uint16_t e[] = {T1_ELEMENT_PFS, T1_ELEMENT_PR(6)};
    start(e, 2);
    for (unsigned k = 0; k < 2; ++k)
        t1_elem_add(IR->w_set, e[k]);
    t1_value saved[] = {t1_ir_livein(IR, T1_I64, e[0]), t1_ir_livein(IR, T1_I1, e[1])};
    t1_value read[] = {K(T1_I64, 1), K(T1_I1, 0)};
    t1_value mem = t1_ir_livein(IR, T1_MEM, T1_ELEMENT_MEM);
    uint32_t first = home_helper(T1_HEADER_BLOCK, mem, e, saved, read, 2);
    mem = T1_VALUE(first, T1_RES_HELPER_mem);
    uint32_t last = home_helper(T1_HEADER_BLOCK, mem, e, saved, saved, 2);
    struct t1_form *f = calloc(1, sizeof(*f));
    UM_CHECK(f != NULL);
    f->ninsns = 1;
    f->insn[0].cls = T1_IC_ALLOC;
    f->insn[0].dst = 12;
    IR->form = f;
    IR->op[last].code = T1_IR_GHELPER;
    uint32_t p = exit_with(T1_HEADER_BLOCK, e, saved, 2);
    verify(0, "home bypass before");
    t1_opt_frames(IR);
    verify(0, "home bypass after");
    for (unsigned k = 0; k < 2; ++k) {
        UM_CHECK(entry(IR->op[first].point, k)->flags & T1_USE_HOME);
        UM_CHECK(!(entry(IR->op[last].point, k)->flags & T1_USE_HOME));
        UM_CHECK(!(entry(p, k)->flags & T1_USE_HOME));
    }
    free(f);
}

static void frame_home_no_memory(void)
{
    uint16_t e[] = {T1_ELEMENT_GR(8), T1_ELEMENT_GR(9)};
    start(e, 2);
    t1_elem_add(IR->w_set, e[0]);
    t1_value saved[] = {t1_ir_livein(IR, T1_I64, e[0]), t1_ir_livein(IR, T1_I64, e[1])};
    uint32_t p = exit_with(T1_HEADER_BLOCK, e, saved, 2);
    struct t1_memory_stats before, after;
    t1_memory_get_stats(&before);
    t1_memory_test_refuse(1);
    t1_opt_frames(IR);
    t1_memory_test_refuse(0);
    t1_memory_get_stats(&after);
    UM_CHECK(!IR->failed && before.workspace_bytes == after.workspace_bytes &&
             before.used_bytes == after.used_bytes);
    UM_CHECK(!(entry(p, 0)->flags & T1_USE_HOME) && entry(p, 0)->value == saved[0]);
    UM_CHECK(entry(p, 1)->flags & T1_USE_HOME);
    verify(0, "home allocation failure");
}

/* A native consumer guards the entry NaT and folds a helper operand to
 * zero. Keep its recovery explicit; HOME never relies on that guard. */
static void frame_home_guarded_nat(void)
{
    uint16_t e = T1_ELEMENT_NAT(34);
    start(&e, 1);
    t1_elem_add(IR->w_set, e);
    t1_value saved = t1_ir_livein(IR, T1_I1, e);
    t1_value mem = t1_ir_livein(IR, T1_MEM, T1_ELEMENT_MEM);
    op(T1_HEADER_BLOCK, T1_IR_NATCHK, 0, 0, saved, 0, 0, 1);
    home_helper(T1_HEADER_BLOCK, mem, &e, &saved, &saved, 1);
    uint32_t p = exit_with(T1_HEADER_BLOCK, &e, &saved, 1);
    t1_opt_frames(IR);
    UM_CHECK(!(entry(p, 0)->flags & T1_USE_HOME));
    UM_CHECK(t1_natguard(IR) == 1);
    verify(T1_VERIFY_NAT_FOLDED, "home guarded NaT");
    t1_opt_frames(IR);
    UM_CHECK(!(entry(p, 0)->flags & T1_USE_HOME));
    UM_CHECK(code_of(entry(p, 0)->value) == T1_IR_CONST);
    verify(T1_VERIFY_NAT_FOLDED, "home guarded NaT after frames");
}

/* Reverse numbering makes one clobber advance only one edge per pass. */
static void frame_home_long_path(void)
{
    uint16_t e = T1_ELEMENT_GR(8);
    start(&e, 1);
    t1_elem_add(IR->w_set, e);
    for (unsigned b = 2; b < T1_MAX_BLOCKS; ++b)
        UM_CHECK(t1_ir_new_block(IR, 0) == b);
    UM_CHECK(t1_ir_set_jump(IR, T1_HEADER_BLOCK, T1_MAX_BLOCKS - 1));
    t1_value saved = t1_ir_livein(IR, T1_I64, e), read = K(T1_I64, 1);
    t1_value mem = t1_ir_livein(IR, T1_MEM, T1_ELEMENT_MEM);
    home_helper(T1_MAX_BLOCKS - 1, mem, &e, &saved, &read, 1);
    for (unsigned b = T1_MAX_BLOCKS - 1; b > 2; --b)
        UM_CHECK(t1_ir_set_jump(IR, b, b - 1));
    uint32_t p = exit_with(2, &e, &saved, 1);
    verify(0, "home longest path before");
    t1_opt_frames(IR);
    UM_CHECK(!(entry(p, 0)->flags & T1_USE_HOME));
    verify(0, "home longest path after");
}
