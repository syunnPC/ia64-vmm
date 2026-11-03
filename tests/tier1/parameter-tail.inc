/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PARAM insertion order is also incoming-edge argument order. Exercise
 * additions after body/terminator emission and after different removals. */
static uint32_t add_test_param(struct t1_ir *ir, uint8_t type, t1_value arg)
{
    uint32_t p = t1_ir_add_param(ir, 2, type, 0);
    UM_CHECK(p != T1_NO_OP);
    UM_CHECK(t1_ir_edge_add_arg(ir, 1, 0, arg, 0));
    return p;
}

static void parameter_insertion(struct t1_ir *ir)
{
    struct base s = build(ir);
    /* Both public insertion APIs preserve the same prefix order. */
    uint32_t generic = t1_ir_append(ir, 2, T1_IR_PARAM, T1_I64, 0, NULL, 0, T1_NO_INSN);
    UM_CHECK(t1_ir_edge_add_arg(ir, 1, 0, T1_VALUE(s.x, 0), 0));
    uint32_t added = add_test_param(ir, T1_I64, T1_VALUE(s.c8, 0));
    UM_CHECK(ir->block[2].first == generic && ir->op[generic].next == added);
    generic = t1_ir_append(ir, 2, T1_IR_PARAM, T1_I64, 0, NULL, 0, T1_NO_INSN);
    UM_CHECK(t1_ir_edge_add_arg(ir, 1, 0, T1_VALUE(s.x, 0), 0));
    uint32_t last = add_test_param(ir, T1_I64, T1_VALUE(s.c8, 0));
    UM_CHECK(ir->op[added].next == generic && ir->op[generic].next == last);
    expect_ok(ir, 0, "interleaved generic and parameter append");
    s = build(ir);
    t1_value x = T1_VALUE(s.x, 0), c = T1_VALUE(s.c8, 0);
    uint32_t a = add_test_param(ir, T1_I64, x);
    uint32_t b = add_test_param(ir, T1_I1, t1_ir_const(ir, T1_I1, 1));
    uint32_t d = add_test_param(ir, T1_I64, c);
    t1_value args[2] = {T1_VALUE(a, 0), c};
    uint32_t body = t1_ir_append(ir, 2, T1_IR_ADD, 0, 0, args, 2, T1_NO_INSN);
    UM_CHECK(ir->block[2].first == a && ir->op[a].next == b &&
            ir->op[b].next == d && ir->op[d].next == body);
    expect_ok(ir, 0, "parameters before an existing body");

    /* Remove the middle entry and forward the tail. Edge arguments must
     * be compacted in the same order as the remaining parameter list. */
    t1_ir_remove(ir, b); t1_ir_forward(ir, d, c);
    t1_ir_resolve_all(ir); t1_ir_compact_params(ir);
    UM_CHECK(ir->op[a].next == body && ir->block[1].succ[0].nargs == 1);
    uint32_t e = add_test_param(ir, T1_I64, c);
    UM_CHECK(ir->op[a].next == e && ir->op[e].prev == a && ir->op[e].next == body);
    expect_ok(ir, 0, "parameter after tail compaction");

    /* A dead tail remains linked until compaction. Inserting before that
     * compaction must not permute its new incoming argument. */
    t1_ir_remove(ir, e);
    uint32_t f = add_test_param(ir, T1_I64, x);
    UM_CHECK(ir->op[e].next == f && ir->op[f].prev == e);
    t1_ir_compact_params(ir);
    struct t1_edge *edge = &ir->block[1].succ[0];
    UM_CHECK(ir->op[a].next == f && ir->op[f].next == body && edge->nargs == 2);
    UM_CHECK(ir->use[edge->args_first].value == x && ir->use[edge->args_first + 1].value == x);
    expect_ok(ir, 0, "parameter after a still-linked dead tail");

    /* Empty the prefix, then prepend a parameter to the surviving body. */
    t1_ir_forward(ir, a, x); t1_ir_remove(ir, f);
    t1_ir_resolve_all(ir); t1_ir_compact_params(ir);
    UM_CHECK(ir->block[2].first == body && edge->nargs == 0);
    uint32_t g = add_test_param(ir, T1_I64, c);
    UM_CHECK(ir->block[2].first == g && ir->op[g].prev == T1_NO_OP && ir->op[g].next == body);
    expect_ok(ir, 0, "parameter after removing the whole prefix");

    /* Reusing the IR resets every block's insertion position. */
    s = build(ir);
    uint32_t first = T1_NO_OP, previous = T1_NO_OP;
    for (unsigned k = 0; k < 2048; ++k) {
        uint32_t p = add_test_param(ir, T1_I64, T1_VALUE(s.x, 0));
        if (previous == T1_NO_OP) first = p;
        else UM_CHECK(ir->op[previous].next == p && ir->op[p].prev == previous);
        previous = p;
    }
    UM_CHECK(ir->block[2].first == first && ir->op[previous].next == ir->block[2].last);
    expect_ok(ir, 0, "large parameter prefix after IR reset");
}
