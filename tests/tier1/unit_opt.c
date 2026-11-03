/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unit tests of opt.c (folding: identities, I1 algebra, redundant and
 * cyclic parameters, I1 constants through loops; the frame rule;
 * predicate branches; DCE with frame pseudo-uses; known bits) and
 * natguard.c (G and NaT folding).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/um_machine.h"
#include "engine/tier1/memory.h"

static struct t1_ir *IR;
static char err[256];
static void metadata_known_demand(struct t1_ir *ir);

static t1_value op(uint16_t block, uint16_t code, uint8_t type, int64_t imm, t1_value a, t1_value b,
                   t1_value c, unsigned n)
{
    t1_value o[3] = {a, b, c};
    uint32_t i = t1_ir_append(IR, block, code, type, imm, o, n, 0);
    UM_CHECK(i != T1_NO_OP);
    return T1_VALUE(i, 0);
}
static t1_value K(uint8_t type, uint64_t v)
{
    return t1_ir_const(IR, type, v);
}

/* Entry and header blocks; the frame covers the given elements. */
static void start(const uint16_t *elements, unsigned n)
{
    t1_ir_init(IR, NULL);
    for (unsigned k = 0; k < n; ++k)
        t1_elem_add(IR->frame_set, elements[k]);
    UM_CHECK(t1_ir_new_block(IR, 0) == T1_ENTRY_BLOCK && t1_ir_new_block(IR, 0) == T1_HEADER_BLOCK);
    UM_CHECK(t1_ir_set_jump(IR, T1_ENTRY_BLOCK, T1_HEADER_BLOCK));
}
static uint32_t exit_with(uint16_t block, const uint16_t *e, const t1_value *v, unsigned n)
{
    uint32_t p = t1_ir_new_point(IR, T1_PT_EXIT, 0x5000, 0, 0, block, 0);
    UM_CHECK(t1_ir_set_frame(IR, p, e, v, NULL, n) && t1_ir_set_exit(IR, block, p, 0));
    return p;
}
static const struct t1_use *entry(uint32_t p, unsigned k)
{
    return &IR->use[IR->point[p].frame_first + k];
}
static unsigned code_of(t1_value v)
{
    return IR->op[T1_VALUE_OP(v)].code;
}
static void verify(unsigned flags, const char *what)
{
    if (!t1_ir_verify(IR, flags, err, sizeof(err))) {
        fprintf(stderr, "unit_opt %s: %s\n", what, err);
        UM_CHECK(!"verify");
    }
}

static void identities(void)
{
    uint16_t e[2] = {T1_ELEMENT_GR(14), T1_ELEMENT_PR(6)};
    start(e, 2);
    uint16_t h = T1_HEADER_BLOCK;
    t1_value x = t1_ir_livein(IR, T1_I64, e[0]), p = t1_ir_livein(IR, T1_I1, e[1]);
    t1_value a = op(h, T1_IR_ADD, 0, 0, x, K(T1_I64, 0), 0, 2);
    a = op(h, T1_IR_OR, 0, 0, K(T1_I64, 0), a, 0, 2);
    a = op(h, T1_IR_AND, 0, 0, a, K(T1_I64, UINT64_MAX), 0, 2);
    a = op(h, T1_IR_SUB, 0, 0, a, K(T1_I64, 0), 0, 2);
    a = op(h, T1_IR_XOR, 0, 0, a, K(T1_I64, 0), 0, 2);
    a = op(h, T1_IR_SHL_IMM, 0, 0, a, 0, 0, 1);
    a = op(h, T1_IR_ANDCM, 0, 0, a, K(T1_I64, 0), 0, 2);
    a = op(h, T1_IR_EXTRU, 0, T1_FIELD_IMM(0, 64), a, 0, 0, 1);
    t1_value q = op(h, T1_IR_NOT1, 0, 0, op(h, T1_IR_NOT1, 0, 0, p, 0, 0, 1), 0, 0, 1);
    q = op(h, T1_IR_AND1, 0, 0, q, K(T1_I1, 1), 0, 2);
    q = op(h, T1_IR_OR1, 0, 0, K(T1_I1, 0), q, 0, 2);
    q = op(h, T1_IR_SELECT, T1_I1, 0, q, K(T1_I1, 1), K(T1_I1, 0), 3);
    q = op(h, T1_IR_SELECT, T1_I1, 0, K(T1_I1, 1), q, K(T1_I1, 0), 3);
    q = op(h, T1_IR_SELECT, T1_I1, 0, p, q, q, 3);
    t1_value not_folded = op(h, T1_IR_ADD, 0, 0, x, K(T1_I64, 1), 0, 2);
    t1_value v[2] = {a, q};
    uint32_t pt = exit_with(h, e, v, 2);
    verify(0, "identities before");
    t1_opt_fold(IR);
    verify(0, "identities folded");
    UM_CHECK(entry(pt, 0)->value == x && entry(pt, 1)->value == p);
    UM_CHECK(
        !(IR->op[T1_VALUE_OP(not_folded)].flags & T1_OPF_DEAD)); /* x + 1: no arithmetic in C */
    /* Frame rule: both are LIVEIN of their own element and not in W. */
    t1_opt_frames(IR);
    UM_CHECK((entry(pt, 0)->flags & T1_USE_HOME) && (entry(pt, 1)->flags & T1_USE_HOME));
    t1_opt_dce(IR);
    verify(0, "identities dce");
    /* Only the terminator stays: the LIVEINs had home uses only. */
    UM_CHECK(IR->block[h].first == IR->block[h].last &&
             IR->block[T1_ENTRY_BLOCK].first == IR->block[T1_ENTRY_BLOCK].last);
}

/* Same operands, including equality exposed by forwarding, are enough
 * to simplify I64 values. Removing the value computation must preserve
 * the independent NaT use and its required entry guard. */
static void same_operands(void)
{
    static const uint16_t codes[] = {T1_IR_SUB, T1_IR_XOR, T1_IR_ANDCM, T1_IR_AND, T1_IR_OR};
    uint16_t e[2] = {T1_ELEMENT_GR(20), T1_ELEMENT_NAT(20)};
    for (unsigned k = 0; k < sizeof(codes) / sizeof(codes[0]); ++k) {
        start(e, 2);
        t1_value x = t1_ir_livein(IR, T1_I64, T1_ELEMENT_GR(14));
        t1_value y = t1_ir_livein(IR, T1_I64, T1_ELEMENT_GR(15));
        t1_value nat = t1_ir_livein(IR, T1_I1, T1_ELEMENT_NAT(14));
        t1_value alias = op(T1_HEADER_BLOCK, T1_IR_ADD, 0, 0, x, K(T1_I64, 0), 0, 2);
        t1_value tail = op(T1_HEADER_BLOCK, T1_IR_ADD, 0, 0, alias, K(T1_I64, 0), 0, 2);
        t1_value same = op(T1_HEADER_BLOCK, codes[k], 0, 0, x, alias, 0, 2);
        t1_value different = op(T1_HEADER_BLOCK, codes[k], 0, 0, x, y, 0, 2);
        t1_value v[2] = {same, op(T1_HEADER_BLOCK, T1_IR_OR1, 0, 0, nat, nat, 0, 2)};
        uint32_t pt = exit_with(T1_HEADER_BLOCK, e, v, 2);
        verify(0, "same operands before");
        /* Retain two hops for the arithmetic consumer's pattern match. */
        t1_ir_forward(IR, T1_VALUE_OP(alias), tail);
        t1_ir_forward(IR, T1_VALUE_OP(tail), x);
        UM_CHECK(IR->op[T1_VALUE_OP(alias)].fwd == tail);
        metadata_known_demand(IR);
        t1_opt_fold(IR);
        verify(0, "same operands folded");
        if (k < 3) {
            UM_CHECK(code_of(entry(pt, 0)->value) == T1_IR_CONST);
            UM_CHECK(IR->op[T1_VALUE_OP(entry(pt, 0)->value)].imm == 0);
        } else
            UM_CHECK(entry(pt, 0)->value == x);
        UM_CHECK(IR->op[T1_VALUE_OP(same)].flags & T1_OPF_DEAD);
        UM_CHECK(!(IR->op[T1_VALUE_OP(different)].flags & T1_OPF_DEAD));
        UM_CHECK(entry(pt, 1)->value == nat);
        t1_opt_dce(IR);
        verify(0, "same operands dce");
        UM_CHECK(!(IR->op[T1_VALUE_OP(nat)].flags & T1_OPF_DEAD));
        if (k < 3)
            UM_CHECK(IR->op[T1_VALUE_OP(x)].flags & T1_OPF_DEAD);
        UM_CHECK(t1_natguard(IR) == 1 && IR->nat_guard[0] == (UINT64_C(1) << 14));
        verify(T1_VERIFY_NAT_FOLDED, "same operands NaT guard");
    }
}

/* A leaf rewrite cannot remove CLOOP while its second result is live. */
static void cloop_two_results(void)
{
    uint16_t e[2] = {T1_ELEMENT_LC, T1_ELEMENT_PR(6)};
    start(e, 2);
    t1_value lc = op(T1_HEADER_BLOCK, T1_IR_CLOOP, 0, 0, K(T1_I64, 0), 0, 0, 1);
    t1_value v[2] = {lc, T1_VALUE(T1_VALUE_OP(lc), 1)};
    uint32_t p = exit_with(T1_HEADER_BLOCK, e, v, 2);
    metadata_known_demand(IR);
    t1_opt_fold(IR);
    verify(0, "CLOOP both results");
    UM_CHECK(!(IR->op[T1_VALUE_OP(lc)].flags & T1_OPF_DEAD));
    UM_CHECK(entry(p, 1)->value == v[1]);
}

/* Only matching physical FR live-ins may be guarded. A derived metadata
 * value must retain an exact native NaTVal test. */
static void fr_guard(void)
{
    uint16_t e[1] = {T1_ELEMENT_NAT(14)};
    start(e, 1);
    t1_value sig = t1_ir_livein(IR, T1_I64, T1_ELEMENT_FR(67));
    t1_value meta = t1_ir_livein(IR, T1_I64, T1_ELEMENT_FR_META(67));
    t1_value test = op(T1_HEADER_BLOCK, T1_IR_FRNATTEST, 0, 0, sig, meta, 0, 2);
    uint32_t p = exit_with(T1_HEADER_BLOCK, e, &test, 1);
    t1_natguard(IR);
    UM_CHECK(!IR->fr_guard[0] && !IR->fr_guard[1]);
    UM_CHECK(code_of(entry(p, 0)->value) == T1_IR_FRNATTEST);
    verify(T1_VERIFY_NAT_FOLDED, "FR propagation");
    /* Consumption, unlike restoration, benefits from the entry guard. */
    start(e, 1);
    sig = t1_ir_livein(IR, T1_I64, T1_ELEMENT_FR(67));
    meta = t1_ir_livein(IR, T1_I64, T1_ELEMENT_FR_META(67));
    test = op(T1_HEADER_BLOCK, T1_IR_FRNATTEST, 0, 0, sig, meta, 0, 2);
    op(T1_HEADER_BLOCK, T1_IR_NATCHK, 0, 0, test, 0, 0, 1);
    p = exit_with(T1_HEADER_BLOCK, e, &test, 1);
    t1_natguard(IR);
    UM_CHECK(IR->fr_guard[0] == 0 && IR->fr_guard[1] == 8);
    UM_CHECK(code_of(entry(p, 0)->value) == T1_IR_CONST &&
             IR->op[T1_VALUE_OP(entry(p, 0)->value)].imm == 0);
    verify(T1_VERIFY_NAT_FOLDED, "FR guard");
    start(e, 1);
    sig = t1_ir_livein(IR, T1_I64, T1_ELEMENT_FR(67));
    meta = op(T1_HEADER_BLOCK, T1_IR_AND, 0, 0, t1_ir_livein(IR, T1_I64, T1_ELEMENT_FR_META(67)),
              K(T1_I64, 0x3ffff), 0, 2);
    test = op(T1_HEADER_BLOCK, T1_IR_FRNATTEST, 0, 0, sig, meta, 0, 2);
    p = exit_with(T1_HEADER_BLOCK, e, &test, 1);
    t1_natguard(IR);
    UM_CHECK(!IR->fr_guard[0] && !IR->fr_guard[1]);
    UM_CHECK(code_of(entry(p, 0)->value) == T1_IR_FRNATTEST);
}

/* W alone does not clobber an entry home before any helper executes. */
static void frame_rule_w(void)
{
    uint16_t e[1] = {T1_ELEMENT_GR(20)};
    start(e, 1);
    t1_elem_add(IR->w_set, e[0]);
    t1_value x = t1_ir_livein(IR, T1_I64, e[0]);
    uint32_t pt = exit_with(T1_HEADER_BLOCK, e, &x, 1);
    t1_opt_frames(IR);
    UM_CHECK(entry(pt, 0)->flags & T1_USE_HOME);
    t1_opt_dce(IR);
    verify(0, "frame rule W");
    UM_CHECK(IR->op[T1_VALUE_OP(x)].flags & T1_OPF_DEAD);
}

/* CONST and LIVEIN are deduplicated, but a removed op is never returned:
 * after DCE removed an unused entry CONST and LIVEIN, asking for the same
 * value again yields a new live op in the entry block (which later passes
 * and the verifier accept), and that op is deduplicated in turn. */
static bool in_entry_block(t1_value v)
{
    for (uint32_t o = IR->block[T1_ENTRY_BLOCK].first; o != T1_NO_OP; o = IR->op[o].next)
        if (o == T1_VALUE_OP(v))
            return true;
    return false;
}
static void entry_values_after_dce(void)
{
    uint16_t e[2] = {T1_ELEMENT_GR(14), T1_ELEMENT_GR(15)};
    start(e, 1); /* the frame covers GR14 */
    t1_value x = t1_ir_livein(IR, T1_I64, e[0]);
    t1_value k = K(T1_I64, 0x1234), y = t1_ir_livein(IR, T1_I64, e[1]);
    UM_CHECK(K(T1_I64, 0x1234) == k && t1_ir_livein(IR, T1_I64, e[1]) == y); /* deduplicated */
    UM_CHECK(K(T1_I1, 0x1234) != k); /* the type is part of the key */
    uint16_t b = t1_ir_new_block(IR, 0);
    UM_CHECK(t1_ir_set_jump(IR, T1_HEADER_BLOCK, b));
    exit_with(b, e, &x, 1);
    t1_opt_dce(IR);
    verify(0, "entry values after DCE");
    UM_CHECK((IR->op[T1_VALUE_OP(k)].flags & T1_OPF_DEAD) &&
             (IR->op[T1_VALUE_OP(y)].flags & T1_OPF_DEAD));
    t1_value k2 = K(T1_I64, 0x1234), y2 = t1_ir_livein(IR, T1_I64, e[1]);
    UM_CHECK(k2 != k && y2 != y);
    UM_CHECK(!(IR->op[T1_VALUE_OP(k2)].flags & T1_OPF_DEAD) && in_entry_block(k2));
    UM_CHECK(!(IR->op[T1_VALUE_OP(y2)].flags & T1_OPF_DEAD) && in_entry_block(y2));
    UM_CHECK(K(T1_I64, 0x1234) == k2 && t1_ir_livein(IR, T1_I64, e[1]) == y2);
    op(T1_HEADER_BLOCK, T1_IR_ADD, 0, 0, k2, y2, 0,
       2); /* uses of the new ops: the IR stays valid */
    verify(0, "re-created entry values");
}

/* Effects remain roots even with unused results. Each site's frame keeps
 * its pre-effect state, independently of the eventual exit's frame. */
static void dce_effects(void)
{
    uint16_t e = T1_ELEMENT_GR(14), h = T1_HEADER_BLOCK;
    start(&e, 1);
    t1_value x = t1_ir_livein(IR, T1_I64, e), m = t1_ir_livein(IR, T1_MEM, T1_ELEMENT_MEM);
    t1_value before = op(h, T1_IR_ADD, 0, 0, x, K(T1_I64, 1), 0, 2);
    t1_value addr = op(h, T1_IR_ADD, 0, 0, x, K(T1_I64, 8), 0, 2);
    uint32_t load = T1_VALUE_OP(op(h, T1_IR_LOAD, 0, T1_ACCESS_IMM(8, 0, 0), addr, m, 0, 2));
    uint32_t lp = t1_ir_new_point(IR, T1_PT_SITE, 0x5000, 0, 0, h, 0);
    UM_CHECK(t1_ir_set_frame(IR, lp, &e, &before, NULL, 1));
    IR->op[load].point = lp;
    IR->point[lp].op = load;
    IR->point[lp].access_size = 8;
    m = op(h, T1_IR_FENCE, 0, 0, T1_VALUE(load, 1), 0, 0, 1);
    t1_value value = op(h, T1_IR_XOR, 0, 0, x, K(T1_I64, 7), 0, 2);
    uint32_t store = T1_VALUE_OP(op(h, T1_IR_STORE, 0, T1_ACCESS_IMM(8, 0, 1), addr, value, m, 3));
    uint32_t sp = t1_ir_new_point(IR, T1_PT_SITE, 0x5000, 0, 0, h, 0);
    UM_CHECK(t1_ir_set_frame(IR, sp, &e, &before, NULL, 1));
    IR->op[store].point = sp;
    IR->point[sp].op = store;
    IR->point[sp].access_size = 8;
    IR->point[sp].access_write = 1;
    IR->point[sp].site = 1;
    t1_value unused = op(h, T1_IR_ADD, 0, 0, x, K(T1_I64, 3), 0, 2);
    exit_with(h, &e, &x, 1);
    verify(0, "DCE effects before");
    t1_opt_dce(IR);
    verify(0, "DCE effects after");
    UM_CHECK(!(IR->op[load].flags & T1_OPF_DEAD) && !(IR->op[store].flags & T1_OPF_DEAD));
    UM_CHECK(!(IR->op[T1_VALUE_OP(m)].flags & T1_OPF_DEAD));
    UM_CHECK(!(IR->op[T1_VALUE_OP(before)].flags & T1_OPF_DEAD));
    UM_CHECK(!(IR->op[T1_VALUE_OP(value)].flags & T1_OPF_DEAD));
    UM_CHECK(IR->op[T1_VALUE_OP(unused)].flags & T1_OPF_DEAD);
}

/* The final frame alone keeps a deep dependency chain, including duplicate
 * operands. Exercise the IR capacity, rather than the guest region limit. */
static void dce_chain(void)
{
    uint16_t e = T1_ELEMENT_GR(14);
    start(&e, 1);
    t1_value v = t1_ir_livein(IR, T1_I64, e);
    unsigned first = IR->nops;
    for (unsigned k = 0; k < T1_IR_MAX_OPS - 3; ++k)
        v = op(T1_HEADER_BLOCK, T1_IR_ADD, 0, 0, v, v, 0, 2);
    exit_with(T1_HEADER_BLOCK, &e, &v, 1);
    UM_CHECK(IR->nops == T1_IR_MAX_OPS);
    t1_opt_dce(IR);
    verify(0, "DCE capacity chain");
    for (unsigned o = first; o < IR->nops; ++o)
        UM_CHECK(!(IR->op[o].flags & (T1_OPF_DEAD | T1_OPF_MARK)));
}

/* Demand must reach a native FR NaTVal test through a full-capacity graph.
 * Pre-existing zero lets folding finish without extending the full IR. */
static void nat_chain(void)
{
    uint16_t e = T1_ELEMENT_NAT(14);
    start(&e, 1);
    K(T1_I1, 0);
    t1_value v =
        op(T1_HEADER_BLOCK, T1_IR_FRNATTEST, 0, 0, t1_ir_livein(IR, T1_I64, T1_ELEMENT_FR(67)),
           t1_ir_livein(IR, T1_I64, T1_ELEMENT_FR_META(67)), 0, 2);
    for (unsigned k = 0; k < T1_IR_MAX_OPS - 7; ++k)
        v = op(T1_HEADER_BLOCK, T1_IR_OR1, 0, 0, v, v, 0, 2);
    op(T1_HEADER_BLOCK, T1_IR_NATCHK, 0, 0, v, 0, 0, 1);
    uint32_t pt = exit_with(T1_HEADER_BLOCK, &e, &v, 1);
    UM_CHECK(IR->nops == T1_IR_MAX_OPS);
    UM_CHECK(t1_natguard(IR) == 0 && !IR->fr_guard[0] && IR->fr_guard[1] == 8);
    UM_CHECK(code_of(entry(pt, 0)->value) == T1_IR_CONST && IR->boundary_insn == T1_NO_INSN);
    verify(T1_VERIFY_NAT_FOLDED, "NaT capacity chain");
}

/* A live cycle through many anonymous PARAMs must retain every incoming
 * argument. The first, disjoint self-cycle is unused and must be compacted.
 * FR NaTVal demand follows the live cycle; a restoration-only FR stays exact. */
static void param_demand(void)
{
    enum { N = 256 };
    for (unsigned nat = 0; nat < 2; ++nat) {
        uint16_t e[2] = {T1_ELEMENT_GR(14), T1_ELEMENT_GR(15)};
        if (nat) {
            e[0] = T1_ELEMENT_NAT(14);
            e[1] = T1_ELEMENT_NAT(15);
        }
        start(e, 2);
        uint16_t h = T1_HEADER_BLOCK, back = t1_ir_new_block(IR, 0), out = t1_ir_new_block(IR, 0);
        uint8_t type = nat ? T1_I1 : T1_I64;
        t1_value source = t1_ir_livein(IR, T1_I64, T1_ELEMENT_GR(14)), restore = source;
        if (nat) {
            source = op(h, T1_IR_FRNATTEST, 0, 0, t1_ir_livein(IR, T1_I64, T1_ELEMENT_FR(67)),
                        t1_ir_livein(IR, T1_I64, T1_ELEMENT_FR_META(67)), 0, 2);
            restore = op(h, T1_IR_FRNATTEST, 0, 0, t1_ir_livein(IR, T1_I64, T1_ELEMENT_FR(68)),
                         t1_ir_livein(IR, T1_I64, T1_ELEMENT_FR_META(68)), 0, 2);
        }
        /* Entry arguments must dominate the header. Place the FR tests in
         * a predecessor of a separate loop header below. */
        uint16_t loop = t1_ir_new_block(IR, 0);
        UM_CHECK(t1_ir_set_jump(IR, h, loop));
        uint32_t unused = t1_ir_add_param(IR, loop, type, 0), param[N];
        UM_CHECK(t1_ir_edge_add_arg(IR, h, 0, K(type, 0), 0));
        for (unsigned k = 0; k < N; ++k) {
            param[k] = t1_ir_add_param(IR, loop, type, 0);
            UM_CHECK(param[k] != T1_NO_OP && t1_ir_edge_add_arg(IR, h, 0, source, 0));
        }
        t1_value last = T1_VALUE(param[N - 1], 0);
        if (nat)
            op(loop, T1_IR_NATCHK, 0, 0, last, 0, 0, 1);
        UM_CHECK(
            t1_ir_set_branch(IR, loop, t1_ir_livein(IR, T1_I1, T1_ELEMENT_PR(6)), back, out, 0));
        uint32_t ck = t1_ir_new_point(IR, T1_PT_CHECKPOINT, 0x5000, 0, 0, back, 0);
        t1_value frame[2] = {restore, last};
        UM_CHECK(t1_ir_set_frame(IR, ck, e, frame, NULL, 2) &&
                 t1_ir_set_backedge(IR, back, loop, ck, 0));
        UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, T1_VALUE(unused, 0), 0));
        for (unsigned k = 0; k < N; ++k)
            UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, T1_VALUE(param[(k + 1) % N], 0), 0));
        uint32_t pt = exit_with(out, e, frame, 2);
        verify(0, "parameter demand before");
        t1_opt_dce(IR);
        verify(0, "parameter demand DCE");
        UM_CHECK(IR->op[unused].flags & T1_OPF_DEAD);
        UM_CHECK(IR->block[h].succ[0].nargs == N && IR->block[back].succ[0].nargs == N);
        for (unsigned k = 0; k < N; ++k)
            UM_CHECK(!(IR->op[param[k]].flags & T1_OPF_DEAD));
        if (nat) {
            UM_CHECK(t1_natguard(IR) == 0);
            UM_CHECK(!IR->fr_guard[0] && IR->fr_guard[1] == 8);
            UM_CHECK(code_of(entry(pt, 0)->value) == T1_IR_FRNATTEST);
            UM_CHECK(code_of(entry(pt, 1)->value) == T1_IR_CONST);
            UM_CHECK(IR->boundary_insn == T1_NO_INSN);
            verify(T1_VERIFY_NAT_FOLDED, "parameter demand NaT");
        }
    }
}

/* A refused analysis allocation must stop compilation without removing
 * operations or folding entry state, and must leave no workspace held. */
static void demand_no_memory(void)
{
    for (unsigned nat = 0; nat < 2; ++nat) {
        uint16_t e = T1_ELEMENT_NAT(14);
        start(&e, 1);
        t1_value v = t1_ir_livein(IR, T1_I1, e);
        for (unsigned k = 0; k < 256; ++k)
            v = op(T1_HEADER_BLOCK, T1_IR_OR1, 0, 0, v, v, 0, 2);
        exit_with(T1_HEADER_BLOCK, &e, &v, 1);
        struct t1_memory_stats before, after;
        t1_memory_get_stats(&before);
        t1_memory_test_refuse(1);
        if (nat)
            UM_CHECK(t1_natguard(IR) == 0);
        else
            t1_opt_dce(IR);
        t1_memory_test_refuse(0);
        t1_memory_get_stats(&after);
        UM_CHECK(IR->failed && !IR->nat_folded && !IR->nat_guard[0] && !IR->nat_guard[1]);
        UM_CHECK(after.workspace_bytes == before.workspace_bytes &&
                 after.used_bytes == before.used_bytes);
        for (unsigned o = 0; o < IR->nops; ++o)
            UM_CHECK(!(IR->op[o].flags & (T1_OPF_DEAD | T1_OPF_MARK)) &&
                     IR->op[o].fwd == T1_VALUE_NONE);
    }
}

/* A loop-carried NaT: header param n = PARAM(0 from entry, OR1(n, 0) and
 * SELECT(c, n, 0) around the loop). Neither the trivial-parameter rule
 * nor local folding sees it is 0; the I1 constant propagation does. Also
 * a cycle of two I64 parameters passing one outer value around. */
static void loop_constants(void)
{
    uint16_t e[3] = {T1_ELEMENT_NAT(14), T1_ELEMENT_GR(14), T1_ELEMENT_LC};
    start(e, 3);
    uint16_t h = T1_HEADER_BLOCK, body = t1_ir_new_block(IR, 0), latch = t1_ir_new_block(IR, 0);
    uint16_t back = t1_ir_new_block(IR, 0), out = t1_ir_new_block(IR, 0);
    uint32_t n = t1_ir_add_param(IR, h, T1_I1, e[0]);
    uint32_t g = t1_ir_add_param(IR, h, T1_I64, e[1]);
    uint32_t lc = t1_ir_add_param(IR, h, T1_I64, e[2]);
    t1_value x = t1_ir_livein(IR, T1_I64, e[1]), l0 = t1_ir_livein(IR, T1_I64, e[2]);
    UM_CHECK(t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, K(T1_I1, 0), e[0]) &&
             t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, x, e[1]) &&
             t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, l0, e[2]));
    UM_CHECK(t1_ir_set_jump(IR, h, body));
    uint32_t g2 = t1_ir_add_param(IR, body, T1_I64, e[1]);
    UM_CHECK(t1_ir_edge_add_arg(IR, h, 0, T1_VALUE(g, 0), e[1]));
    t1_value n1 = op(body, T1_IR_OR1, 0, 0, T1_VALUE(n, 0), K(T1_I1, 0), 0, 2);
    t1_value cl = op(body, T1_IR_CLOOP, 0, 0, T1_VALUE(lc, 0), 0, 0, 1);
    t1_value n2 =
        op(body, T1_IR_SELECT, T1_I1, 0, T1_VALUE(T1_VALUE_OP(cl), 1), n1, K(T1_I1, 0), 3);
    UM_CHECK(t1_ir_set_branch(IR, body, T1_VALUE(T1_VALUE_OP(cl), 1), latch, out, 0));
    UM_CHECK(t1_ir_set_jump(IR, latch, back));
    uint32_t ck = t1_ir_new_point(IR, T1_PT_CHECKPOINT, 0x5000, 0, 0, back, 0);
    t1_value fv[3] = {n2, T1_VALUE(g2, 0), cl};
    UM_CHECK(t1_ir_set_frame(IR, ck, e, fv, NULL, 3) &&
             t1_ir_set_backedge(IR, back, T1_HEADER_BLOCK, ck, 0));
    UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, n2, e[0]) &&
             t1_ir_edge_add_arg(IR, back, 0, T1_VALUE(g2, 0), e[1]) &&
             t1_ir_edge_add_arg(IR, back, 0, cl, e[2]));
    uint32_t pt = exit_with(out, e, fv, 3);
    verify(0, "loop constants built");
    t1_opt_fold(IR);
    verify(0, "loop constants folded");
    UM_CHECK(code_of(entry(pt, 0)->value) == T1_IR_CONST &&
             IR->op[T1_VALUE_OP(entry(pt, 0)->value)].imm == 0);
    UM_CHECK(entry(pt, 1)->value == x); /* the parameter cycle */
    UM_CHECK(code_of(entry(pt, 2)->value) == T1_IR_CLOOP);
}

/* Sparse I1 loops among unrelated I64 values: parameter positions count
 * every type, and an alternating Boolean must not become its entry value.
 * CLOOP's second result is an unknown I1 source, not its I64 first result. */
static void sparse_i1_loop(void)
{
    for (unsigned alternating = 0; alternating < 2; ++alternating) {
        uint16_t e[2] = {T1_ELEMENT_PR(6), T1_ELEMENT_LC};
        start(e, 2);
        uint16_t h = T1_HEADER_BLOCK, back = t1_ir_new_block(IR, 0), out = t1_ir_new_block(IR, 0);
        uint32_t lc = t1_ir_add_param(IR, h, T1_I64, e[1]);
        uint32_t p = t1_ir_add_param(IR, h, T1_I1, e[0]);
        t1_value x = t1_ir_livein(IR, T1_I64, T1_ELEMENT_GR(14));
        UM_CHECK(t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, t1_ir_livein(IR, T1_I64, e[1]), e[1]));
        UM_CHECK(t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, K(T1_I1, 0), e[0]));
        for (unsigned k = 0; k < 2000; ++k)
            op(h, T1_IR_ADD, 0, 0, x, K(T1_I64, k + 1), 0, 2);
        t1_value next = alternating ? op(h, T1_IR_NOT1, 0, 0, T1_VALUE(p, 0), 0, 0, 1)
                                    : op(h, T1_IR_OR1, 0, 0, T1_VALUE(p, 0), K(T1_I1, 0), 0, 2);
        t1_value cl = op(h, T1_IR_CLOOP, 0, 0, T1_VALUE(lc, 0), 0, 0, 1);
        t1_value taken = T1_VALUE(T1_VALUE_OP(cl), 1);
        next = op(h, T1_IR_SELECT, T1_I1, 0, taken, next, T1_VALUE(p, 0), 3);
        UM_CHECK(t1_ir_set_branch(IR, h, taken, back, out, 0));
        t1_value frame[2] = {next, cl};
        uint32_t ck = t1_ir_new_point(IR, T1_PT_CHECKPOINT, 0x5000, 0, 0, back, 0);
        UM_CHECK(t1_ir_set_frame(IR, ck, e, frame, NULL, 2));
        UM_CHECK(t1_ir_set_backedge(IR, back, h, ck, 0));
        UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, cl, e[1]));
        UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, next, e[0]));
        uint32_t pt = exit_with(out, e, frame, 2);
        verify(0, "sparse I1 loop before");
        t1_opt_fold(IR);
        verify(0, "sparse I1 loop folded");
        if (!alternating) {
            UM_CHECK(code_of(entry(pt, 0)->value) == T1_IR_CONST);
            UM_CHECK(IR->op[T1_VALUE_OP(entry(pt, 0)->value)].imm == 0);
        } else {
            UM_CHECK(code_of(entry(pt, 0)->value) == T1_IR_SELECT);
            UM_CHECK(!(IR->op[p].flags & T1_OPF_DEAD));
        }
        UM_CHECK(code_of(entry(pt, 1)->value) == T1_IR_CLOOP);
    }
}

/* BRANCH (NOT1 c) swaps its targets; BRANCH (CONST 1) becomes a JUMP and
 * the dead side goes with its point. */
static void predicates(void)
{
    uint16_t e[1] = {T1_ELEMENT_PR(6)};
    start(e, 1);
    uint16_t h = T1_HEADER_BLOCK, a = t1_ir_new_block(IR, 0), b = t1_ir_new_block(IR, 0);
    t1_value c = t1_ir_livein(IR, T1_I1, e[0]);
    UM_CHECK(t1_ir_set_branch(IR, h, op(h, T1_IR_NOT1, 0, 0, c, 0, 0, 1), a, b, 0));
    uint32_t pa = exit_with(a, e, &c, 1), pb = exit_with(b, e, &c, 1);
    t1_opt_predicates(IR);
    verify(0, "predicates swap");
    UM_CHECK(IR->block[h].succ[0].target == b && IR->block[h].succ[1].target == a &&
             IR->use[IR->op[t1_ir_terminator(IR, h)].use_first].value == c);
    IR->use[IR->op[t1_ir_terminator(IR, h)].use_first].value = K(T1_I1, 1);
    t1_opt_predicates(IR);
    t1_opt_fold(IR);
    t1_opt_dce(IR);
    verify(0, "predicates constant");
    UM_CHECK((IR->block[a].flags & T1_BLK_DEAD) && IR->point[pa].dead && !IR->point[pb].dead &&
             IR->op[t1_ir_terminator(IR, h)].code == T1_IR_JUMP);
}

/* Native-only elements retain conservative entry folding. */
static void natguard(void)
{
    uint16_t e[3] = {T1_ELEMENT_NAT(20), T1_ELEMENT_NAT(21), T1_ELEMENT_GR(22)};
    start(e, 3);
    uint16_t h = T1_HEADER_BLOCK;
    t1_value n20 = t1_ir_livein(IR, T1_I1, e[0]), n21 = t1_ir_livein(IR, T1_I1, e[1]);
    t1_value n23 = t1_ir_livein(IR, T1_I1, T1_ELEMENT_NAT(23));
    t1_value p = t1_ir_livein(IR, T1_I1, T1_ELEMENT_PR(5));
    t1_value g = t1_ir_livein(IR, T1_I64, e[2]);
    t1_value merged = op(h, T1_IR_SELECT, T1_I1, 0, p, n21, n20, 3); /* (p5) mov r20 = r21 */
    op(h, T1_IR_NATCHK, 0, 0, op(h, T1_IR_OR1, 0, 0, n23, K(T1_I1, 0), 0, 2), 0, 0, 1);
    t1_value v[3] = {merged, n21, g};
    uint32_t pt = exit_with(h, e, v, 3);
    t1_opt_frames(IR); /* NaT(21) and r22 are LIVEIN of themselves: home */
    t1_opt_dce(IR);
    verify(0, "natguard before");
    UM_CHECK(t1_natguard(IR) == 3);
    UM_CHECK(IR->nat_guard[0] == ((UINT64_C(1) << 20) | (UINT64_C(1) << 21) | (UINT64_C(1) << 23)));
    verify(T1_VERIFY_NAT_FOLDED, "natguard folded");
    UM_CHECK(code_of(entry(pt, 0)->value) == T1_IR_CONST && (entry(pt, 1)->flags & T1_USE_HOME));
    UM_CHECK(t1_natguard(IR) == 0 && IR->nat_guard[0] == 0); /* idempotent */
    UM_CHECK(IR->boundary_insn == T1_NO_INSN);
    /* A check that cannot fold (here of a predicate, which no v1 lifting
     * produces) is reported as a region boundary at its instruction. */
    t1_value o = t1_ir_livein(IR, T1_I1, T1_ELEMENT_PR(5));
    uint32_t chk = t1_ir_append(IR, h, T1_IR_NATCHK, 0, 0, &o, 1, 7);
    UM_CHECK(chk != T1_NO_OP);
    t1_natguard(IR);
    UM_CHECK(IR->boundary_insn == 7);
}

static void known_bits(void)
{
    uint16_t e[2] = {T1_ELEMENT_GR(14), T1_ELEMENT_GR(15)};
    start(e, 2);
    uint16_t h = T1_HEADER_BLOCK, body = t1_ir_new_block(IR, 0), back = t1_ir_new_block(IR, 0);
    uint16_t out = t1_ir_new_block(IR, 0);
    t1_value x = t1_ir_livein(IR, T1_I64, e[0]);
    /* A pointer starting at an aligned constant and stepping by 8. */
    uint32_t ptr = t1_ir_add_param(IR, h, T1_I64, e[1]);
    uint32_t raw = t1_ir_add_param(IR, h, T1_I64, e[0]);
    UM_CHECK(t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, K(T1_I64, 0x10000), e[1]) &&
             t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, x, e[0]));
    UM_CHECK(t1_ir_set_jump(IR, h, body));
    t1_value next = op(body, T1_IR_ADD, 0, 0, T1_VALUE(ptr, 0), K(T1_I64, 8), 0, 2);
    t1_value nraw = op(body, T1_IR_ADD, 0, 0, T1_VALUE(raw, 0), K(T1_I64, 8), 0, 2);
    t1_value sh = op(body, T1_IR_SHL_IMM, 0, 3, x, 0, 0, 1);
    t1_value z = op(body, T1_IR_ZXT, 0, 16, x, 0, 0, 1);
    t1_value sx =
        op(body, T1_IR_SXT, 0, 8, op(body, T1_IR_AND, 0, 0, x, K(T1_I64, 0x7f), 0, 2), 0, 0, 1);
    t1_value ex = op(body, T1_IR_EXTRU, 0, T1_FIELD_IMM(4, 8), x, 0, 0, 1);
    t1_value dz = op(body, T1_IR_DEPZ, 0, T1_FIELD_IMM(8, 4), x, 0, 0, 1);
    t1_value sum = op(body, T1_IR_ADD, 0, 0, sh, K(T1_I64, 4), 0, 2); /* low 3 bits 100 */
    t1_value sub = op(body, T1_IR_SUB, 0, 0, K(T1_I64, 0x100), K(T1_I64, 1), 0, 2);
    t1_value cond = op(body, T1_IR_CMP, 0, T1_CMP_IMM(T1_CMP_EQ, 64), next, x, 0, 2);
    UM_CHECK(t1_ir_set_branch(IR, body, cond, out, back, 0));
    uint32_t ck = t1_ir_new_point(IR, T1_PT_CHECKPOINT, 0x5000, 0, 0, back, 0);
    t1_value fv[2] = {nraw, next};
    UM_CHECK(t1_ir_set_frame(IR, ck, e, fv, NULL, 2) &&
             t1_ir_set_backedge(IR, back, T1_HEADER_BLOCK, ck, 0));
    UM_CHECK(t1_ir_edge_add_arg(IR, back, 0, next, e[1]) &&
             t1_ir_edge_add_arg(IR, back, 0, nraw, e[0]));
    t1_value ev[2] = {nraw, next};
    exit_with(out, e, ev, 2);
    verify(0, "known bits");
    struct t1_known *kb = calloc(IR->nops, sizeof(*kb));
    UM_CHECK(kb != NULL);
    t1_known_bits(IR, kb);
#define KB(v) kb[T1_VALUE_OP(v)]
    UM_CHECK(KB(x).zero == 0 && KB(x).one == 0); /* live-ins unknown */
    UM_CHECK((KB(T1_VALUE(ptr, 0)).zero & 7) == 7 &&
             (KB(next).zero & 7) == 7);             /* loop phi aligned */
    UM_CHECK((KB(T1_VALUE(raw, 0)).zero & 7) == 0); /* phi of a live-in */
    UM_CHECK(KB(sh).zero == 7 && KB(sh).one == 0);
    UM_CHECK((KB(sum).zero & 7) == 3 && (KB(sum).one & 7) == 4);
    UM_CHECK(KB(z).zero == ~UINT64_C(0xffff) && KB(sx).zero == ~UINT64_C(0x7f));
    UM_CHECK(KB(ex).zero == ~UINT64_C(0xff) && KB(dz).zero == ~UINT64_C(0xf00));
    UM_CHECK(KB(sub).one == 0xff && KB(sub).zero == ~UINT64_C(0xff)); /* carry-aware */
    free(kb);
    metadata_known_demand(IR);
}

/* A two-block graph can still need a fixed point: the header parameter
 * is 0 on entry and 0xff after its backedge. Its low bits are unknown. */
static void known_bits_loop(void)
{
    uint16_t e = T1_ELEMENT_GR(20), h = T1_HEADER_BLOCK;
    start(&e, 1);
    uint32_t p = t1_ir_add_param(IR, h, T1_I64, e);
    UM_CHECK(t1_ir_edge_add_arg(IR, T1_ENTRY_BLOCK, 0, K(T1_I64, 0), e));
    t1_value value = op(h, T1_IR_OR, 0, 0, T1_VALUE(p, 0), K(T1_I64, 0xff), 0, 2);
    uint32_t pt = t1_ir_new_point(IR, T1_PT_CHECKPOINT, 0x5000, 0, 0, h, 0);
    UM_CHECK(t1_ir_set_frame(IR, pt, &e, &value, NULL, 1));
    UM_CHECK(t1_ir_set_backedge(IR, h, h, pt, 0));
    UM_CHECK(t1_ir_edge_add_arg(IR, h, 0, value, e));
    verify(0, "two-block known-bits loop");
    struct t1_known *kb = calloc(IR->nops, sizeof(*kb));
    UM_CHECK(kb != NULL);
    t1_known_bits(IR, kb);
    UM_CHECK(kb[p].zero == ~UINT64_C(0xff) && !kb[p].one);
    UM_CHECK(kb[T1_VALUE_OP(value)].zero == ~UINT64_C(0xff) && kb[T1_VALUE_OP(value)].one == 0xff);
    free(kb);
    metadata_known_demand(IR);
}

/* The whole pipeline on gather and shladd_xor (frag_loops.S), with the verifier after
 * every pass. */
static void pipeline(const char *image)
{
    um_load_file(UM_CODE, image);
    for (unsigned k = 0; k < 2; ++k) {
        struct vcpu *v = um_vcpu(UM_CODE + (k ? 0x400 : 0));
        struct um_region r;
        UM_CHECK(um_compile(v, &r, err, sizeof(err)) == T1_COMPILED);
        UM_CHECK(r.ir->nat_folded && t1_ir_verify(r.ir, T1_VERIFY_NAT_FOLDED, err, sizeof(err)));
        um_region_free(&r);
        vcpu_destroy(v);
    }
}

/* An absorbing constant removes the other data operand, but its NaT
 * remains demanded even when no data use of the live-in survives. */
static void absorbing_constants(void)
{
    static const struct {
        uint16_t code;
        bool left;
        uint64_t constant, result;
    } cases[] = {
        {T1_IR_AND, false, 0, 0},
        {T1_IR_AND, true, 0, 0},
        {T1_IR_OR, false, UINT64_MAX, UINT64_MAX},
        {T1_IR_OR, true, UINT64_MAX, UINT64_MAX},
        {T1_IR_ANDCM, true, 0, 0},
        {T1_IR_ANDCM, false, UINT64_MAX, 0},
        {T1_IR_SHL, true, 0, 0},
        {T1_IR_SHR, true, 0, 0},
        {T1_IR_SHRU, true, 0, 0},
    };
    uint16_t e[2] = {T1_ELEMENT_GR(20), T1_ELEMENT_NAT(20)};
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        start(e, 2);
        t1_value x = t1_ir_livein(IR, T1_I64, T1_ELEMENT_GR(14));
        t1_value nat = t1_ir_livein(IR, T1_I1, T1_ELEMENT_NAT(14));
        t1_value c = K(T1_I64, cases[k].constant);
        t1_value value = op(T1_HEADER_BLOCK, cases[k].code, 0, 0, cases[k].left ? c : x,
                            cases[k].left ? x : c, 0, 2);
        c = K(T1_I64, cases[k].constant ^ 1);
        t1_value different = op(T1_HEADER_BLOCK, cases[k].code, 0, 0, cases[k].left ? c : x,
                                cases[k].left ? x : c, 0, 2);
        t1_value v[2] = {value, op(T1_HEADER_BLOCK, T1_IR_OR1, 0, 0, nat, K(T1_I1, 0), 0, 2)};
        uint32_t pt = exit_with(T1_HEADER_BLOCK, e, v, 2);
        verify(0, "absorbing constant before");
        t1_opt_fold(IR);
        verify(0, "absorbing constant folded");
        UM_CHECK(code_of(entry(pt, 0)->value) == T1_IR_CONST);
        UM_CHECK((uint64_t)IR->op[T1_VALUE_OP(entry(pt, 0)->value)].imm == cases[k].result);
        UM_CHECK(IR->op[T1_VALUE_OP(value)].flags & T1_OPF_DEAD);
        UM_CHECK(!(IR->op[T1_VALUE_OP(different)].flags & T1_OPF_DEAD));
        UM_CHECK(entry(pt, 1)->value == nat);
        t1_opt_dce(IR);
        verify(0, "absorbing constant dce");
        UM_CHECK(IR->op[T1_VALUE_OP(x)].flags & T1_OPF_DEAD);
        UM_CHECK(!(IR->op[T1_VALUE_OP(nat)].flags & T1_OPF_DEAD));
        UM_CHECK(t1_natguard(IR) == 1 && IR->nat_guard[0] == (UINT64_C(1) << 14));
        verify(T1_VERIFY_NAT_FOLDED, "absorbing constant NaT guard");
    }
}

/* Removing a load's last data use must retain the faulting access and
 * the values demanded by its pre-instruction restoration frame. */
static void absorbing_constant_load(void)
{
    uint16_t e = T1_ELEMENT_GR(14), h = T1_HEADER_BLOCK;
    start(&e, 1);
    t1_value x = t1_ir_livein(IR, T1_I64, e), m = t1_ir_livein(IR, T1_MEM, T1_ELEMENT_MEM);
    t1_value before = op(h, T1_IR_ADD, 0, 0, x, K(T1_I64, 1), 0, 2);
    t1_value addr = op(h, T1_IR_ADD, 0, 0, x, K(T1_I64, 8), 0, 2);
    t1_value load = op(h, T1_IR_LOAD, 0, T1_ACCESS_IMM(8, 0, 0), addr, m, 0, 2);
    uint32_t lp = t1_ir_new_point(IR, T1_PT_SITE, 0x5000, 0, 0, h, 0);
    UM_CHECK(t1_ir_set_frame(IR, lp, &e, &before, NULL, 1));
    IR->op[T1_VALUE_OP(load)].point = lp;
    IR->point[lp].op = T1_VALUE_OP(load);
    IR->point[lp].access_size = 8;
    t1_value value = op(h, T1_IR_AND, 0, 0, load, K(T1_I64, 0), 0, 2);
    uint32_t pt = exit_with(h, &e, &value, 1);
    verify(0, "absorbing constant load before");
    t1_opt_fold(IR);
    t1_opt_dce(IR);
    verify(0, "absorbing constant load after");
    UM_CHECK(IR->op[T1_VALUE_OP(value)].flags & T1_OPF_DEAD);
    UM_CHECK(code_of(entry(pt, 0)->value) == T1_IR_CONST);
    UM_CHECK(!(IR->op[T1_VALUE_OP(load)].flags & T1_OPF_DEAD) && !IR->point[lp].dead);
    UM_CHECK(entry(lp, 0)->value == before && !(IR->op[T1_VALUE_OP(before)].flags & T1_OPF_DEAD));
}

#include "tests/tier1/loop_metadata.inc"
#include "tests/tier1/frame_homes.inc"

void unit_opt_test(const char *image);
void unit_opt_test(const char *image)
{
    um_machine_init();
    IR = calloc(1, sizeof(*IR));
    UM_CHECK(IR != NULL);
    identities();
    same_operands();
    cloop_two_results();
    fr_guard();
    frame_rule_w();
    frame_home_flow();
    frame_home_bypass();
    frame_home_no_memory();
    frame_home_guarded_nat();
    frame_home_long_path();
    entry_values_after_dce();
    dce_effects();
    absorbing_constants();
    absorbing_constant_load();
    dce_chain();
    nat_chain();
    param_demand();
    demand_no_memory();
    loop_constants();
    sparse_i1_loop();
    predicates();
    natguard();
    known_bits();
    known_bits_loop();
    loop_metadata();
    pipeline(image);
    free(IR);
    printf(
        "unit_opt: identities, I1 algebra, frame rule (home / W), entry values after DCE, loop and "
        "parameter-cycle constants, predicate branches, DCE, natguard G, known bits, gather/shladd_xor pipeline ok\n");
}
