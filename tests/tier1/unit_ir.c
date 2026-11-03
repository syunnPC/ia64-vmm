/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unit tests of ir.c (builder, CFG queries, forwarding, parameter
 * compaction, printing) and verify.c (one negative case per check).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/um_machine.h"

/* A hand-built region:
 *   b0: CONST, LIVEIN r14, LIVEIN mem; JUMP b1
 *   b1: y = ADD x, 8; STORE [x] = y (site P0); c = CMP eq y, 0; BRANCH c -> b2, b3
 *   b2: EXIT P1      b3: EXIT P2
 * frame_set = {r14, r15}. */
struct base {
    uint32_t c0, c8, x, m, y, st, cmp;
    uint32_t p0, p1, p2;
};

static void frame(struct t1_ir *ir, uint32_t p, t1_value r14, t1_value r15)
{
    uint16_t e[2] = {T1_ELEMENT_GR(14), T1_ELEMENT_GR(15)};
    t1_value v[2] = {r14, r15};
    UM_CHECK(t1_ir_set_frame(ir, p, e, v, NULL, 2));
}

static struct base build(struct t1_ir *ir)
{
    struct base s;
    t1_ir_init(ir, NULL);
    t1_elem_add(ir->frame_set, T1_ELEMENT_GR(14));
    t1_elem_add(ir->frame_set, T1_ELEMENT_GR(15));
    UM_CHECK(t1_ir_new_block(ir, 0) == T1_ENTRY_BLOCK && t1_ir_new_block(ir, 0) == T1_HEADER_BLOCK);
    uint16_t b2 = t1_ir_new_block(ir, 0), b3 = t1_ir_new_block(ir, 0);
    UM_CHECK(t1_ir_set_jump(ir, T1_ENTRY_BLOCK, T1_HEADER_BLOCK));
    t1_value c0 = t1_ir_const(ir, T1_I64, 0), c8 = t1_ir_const(ir, T1_I64, 8);
    t1_value x = t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(14));
    t1_value r15 = t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(15));
    t1_value m = t1_ir_livein(ir, T1_MEM, T1_ELEMENT_MEM);
    UM_CHECK(t1_ir_const(ir, T1_I64, 8) == c8 && t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(14)) == x);
    s.c0 = T1_VALUE_OP(c0);
    s.c8 = T1_VALUE_OP(c8);
    s.x = T1_VALUE_OP(x);
    s.m = T1_VALUE_OP(m);
    t1_value o[3] = {x, c8};
    s.y = t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_ADD, 0, 0, o, 2, 0);
    s.p0 = t1_ir_new_point(ir, T1_PT_SITE, 0x1000, 0, 0, T1_HEADER_BLOCK, 0);
    frame(ir, s.p0, x, r15);
    o[0] = x;
    o[1] = T1_VALUE(s.y, 0);
    o[2] = m;
    s.st = t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_STORE, 0, T1_ACCESS_IMM(8, 0, 0), o, 3, 0);
    ir->op[s.st].point = s.p0;
    ir->point[s.p0].op = s.st;
    ir->point[s.p0].access_size = 8;
    ir->point[s.p0].access_write = 1;
    ir->nsites = 1;
    o[0] = T1_VALUE(s.y, 0);
    o[1] = c0;
    s.cmp = t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_CMP, 0, T1_CMP_IMM(T1_CMP_EQ, 64), o, 2, 0);
    ir->block[T1_HEADER_BLOCK].charge = 3;
    UM_CHECK(t1_ir_set_branch(ir, T1_HEADER_BLOCK, T1_VALUE(s.cmp, 0), b2, b3, 0));
    s.p1 = t1_ir_new_point(ir, T1_PT_EXIT, 0x2000, 0, 0, b2, 0);
    frame(ir, s.p1, T1_VALUE(s.y, 0), r15);
    UM_CHECK(t1_ir_set_exit(ir, b2, s.p1, 0));
    s.p2 = t1_ir_new_point(ir, T1_PT_EXIT, 0x1010, 0, 0, b3, 0);
    frame(ir, s.p2, x, r15);
    UM_CHECK(t1_ir_set_exit(ir, b3, s.p2, 0));
    UM_CHECK(!ir->failed);
    return s;
}

static char err[256];

static void expect_ok(const struct t1_ir *ir, unsigned flags, const char *what)
{
    if (!t1_ir_verify(ir, flags, err, sizeof(err))) {
        fprintf(stderr, "unit_ir %s: unexpected rejection: %s\n", what, err);
        UM_CHECK(!"verify accepts");
    }
}

static void expect_fail(const struct t1_ir *ir, unsigned flags, const char *substring,
                        const char *what)
{
    if (t1_ir_verify(ir, flags, err, sizeof(err)) || !strstr(err, substring)) {
        fprintf(stderr, "unit_ir %s: expected \"%s\", got \"%s\"\n", what, substring, err);
        UM_CHECK(!"verify rejects");
    }
}

struct sink {
    char text[8192];
    size_t n;
};
static void sink_line(void *arg, const char *line)
{
    struct sink *s = arg;
    size_t m = strlen(line);
    if (s->n + m + 2 < sizeof(s->text)) {
        memcpy(s->text + s->n, line, m);
        s->n += m;
        s->text[s->n++] = '\n';
        s->text[s->n] = 0;
    }
}

static unsigned negatives;

static void forwarding_chains(struct t1_ir *ir)
{
    enum { N = 256 };
    struct base s = build(ir);
    t1_value chain[N], operands[2] = {T1_VALUE(s.x, 0), T1_VALUE(s.c0, 0)};
    for (unsigned k = 0; k < N; ++k) {
        uint32_t op = t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_ADD, 0, 0, operands, 2, 0);
        UM_CHECK(op != T1_NO_OP);
        chain[k] = T1_VALUE(op, 0);
    }
    for (unsigned k = 0; k + 1 < N; ++k)
        t1_ir_forward(ir, T1_VALUE_OP(chain[k]), chain[k + 1]);
    t1_ir_forward(ir, T1_VALUE_OP(chain[N - 1]), operands[0]);
    ir->use[ir->op[s.y].use_first].value = chain[0];
    ir->use[ir->point[s.p2].frame_first].value = chain[N / 2];
    UM_CHECK(t1_ir_add_param(ir, 2, T1_I64, T1_ELEMENT_GR(15)) != T1_NO_OP);
    UM_CHECK(t1_ir_edge_add_arg(ir, 1, 0, chain[N / 4], T1_ELEMENT_GR(15)));
    t1_ir_resolve_all(ir);
    UM_CHECK(ir->use[ir->op[s.y].use_first].value == operands[0]);
    UM_CHECK(ir->use[ir->point[s.p2].frame_first].value == operands[0]);
    UM_CHECK(ir->use[ir->block[1].succ[0].args_first].value == operands[0]);
    for (unsigned k = 0; k < N; ++k) {
        UM_CHECK(t1_ir_resolve(ir, chain[k]) == operands[0]);
        t1_ir_remove(ir, T1_VALUE_OP(chain[k]));
    }
    expect_ok(ir, 0, "forwarding chains across operands, edges and frames");

    /* Result-zero forwarding must not affect a LOAD's memory result. */
    t1_value load_args[2] = {T1_VALUE(s.x, 0), T1_VALUE(s.m, 0)};
    uint32_t load =
        t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_LOAD, 0, T1_ACCESS_IMM(8, 0, 0), load_args, 2, 0);
    uint32_t alias = t1_ir_add_param(ir, 2, T1_MEM, T1_ELEMENT_MEM);
    uint32_t tail = t1_ir_add_param(ir, 2, T1_MEM, T1_ELEMENT_MEM);
    UM_CHECK(load != T1_NO_OP && alias != T1_NO_OP && tail != T1_NO_OP);
    t1_ir_forward(ir, load, load_args[0]);
    t1_ir_forward(ir, alias, T1_VALUE(tail, 0));
    t1_ir_forward(ir, tail, T1_VALUE(load, 1));
    ir->use[ir->op[s.st].use_first + 2].value = T1_VALUE(alias, 0);
    t1_ir_resolve_all(ir);
    UM_CHECK(ir->use[ir->op[s.st].use_first + 2].value == T1_VALUE(load, 1));
    UM_CHECK(t1_ir_resolve(ir, T1_VALUE(load, 0)) == load_args[0]);
    UM_CHECK(t1_ir_resolve(ir, T1_VALUE(load, 1)) == T1_VALUE(load, 1));
}

/* More entry values than the old index could hold: exact identities must
 * survive collisions, removal and re-creation, and LIVEIN -> NATIN. The
 * complete IR may fill after a miss, but a hit still needs no new op. */
static void entry_index(struct t1_ir *ir)
{
    enum { N = 6000 };
    static t1_value value[N];
    build(ir);
    for (unsigned k = 0; k < N; ++k) {
        uint64_t bits = UINT64_C(0x8000000000000000) + (uint64_t)k * 65537;
        value[k] = t1_ir_const(ir, T1_I64, bits);
        UM_CHECK(value[k] != T1_VALUE_NONE);
    }
    unsigned before = ir->nops;
    for (unsigned k = N; k-- > 0;)
        UM_CHECK(t1_ir_const(ir, T1_I64, UINT64_C(0x8000000000000000) + (uint64_t)k * 65537) ==
                 value[k]);
    UM_CHECK(ir->nops == before);
    for (unsigned k = 0; k < N; k += 13) {
        t1_ir_remove(ir, T1_VALUE_OP(value[k]));
        t1_value next = t1_ir_const(ir, T1_I64, UINT64_C(0x8000000000000000) + (uint64_t)k * 65537);
        UM_CHECK(next != value[k] && next != T1_VALUE_NONE);
        UM_CHECK(!(ir->op[T1_VALUE_OP(next)].flags & T1_OPF_DEAD));
        value[k] = next;
    }
    for (unsigned k = 0; k < N; ++k)
        UM_CHECK(t1_ir_const(ir, T1_I64, UINT64_C(0x8000000000000000) + (uint64_t)k * 65537) ==
                 value[k]);
    t1_value zero64 = t1_ir_const(ir, T1_I64, 0), zero1 = t1_ir_const(ir, T1_I1, 0);
    UM_CHECK(zero64 != zero1 && t1_ir_const(ir, T1_I1, 2) == zero1);
    t1_value nat = t1_ir_livein(ir, T1_I1, T1_ELEMENT_NAT(14));
    ir->op[T1_VALUE_OP(nat)].code = T1_IR_NATIN;
    t1_value live = t1_ir_livein(ir, T1_I1, T1_ELEMENT_NAT(14));
    UM_CHECK(live != nat && t1_ir_livein(ir, T1_I1, T1_ELEMENT_NAT(14)) == live);
    expect_ok(ir, 0, "large entry index after removal and NATIN");
    uint64_t fresh = UINT64_C(0x4000000000000000);
    while (ir->nops < T1_IR_MAX_OPS)
        UM_CHECK(t1_ir_const(ir, T1_I64, fresh++) != T1_VALUE_NONE);
    UM_CHECK(t1_ir_const(ir, T1_I64, 0) == zero64 && !ir->failed);
    UM_CHECK(t1_ir_const(ir, T1_I64, fresh) == T1_VALUE_NONE && ir->failed);
    UM_CHECK(!strcmp(ir->fail_reason, "op limit"));
    /* Reinitializing the same storage discards every previous bucket. */
    build(ir);
    expect_ok(ir, 0, "entry index reset");
}

#include "tests/tier1/verify_home_bits.inc"
#include "tests/tier1/parameter-tail.inc"

/* Builders reuse a poisoned use pool. Active operand/frame metadata must
 * be defined, spare grow capacity must retain the NONE sentinel, and a
 * failed reservation must not publish a partial op or frame. */
static void use_storage(struct t1_ir *ir)
{
    for (unsigned poison = 0; poison < 4; ++poison) {
        memset(ir->use, 85 * poison, sizeof(ir->use));
        t1_ir_init(ir, NULL);
        UM_CHECK(t1_ir_new_block(ir, 0) == T1_ENTRY_BLOCK);
        UM_CHECK(t1_ir_new_block(ir, 0) == T1_HEADER_BLOCK);
        t1_value v[3] = {t1_ir_const(ir, T1_I64, 7), t1_ir_const(ir, T1_I64, 11), T1_VALUE_NONE};
        uint32_t op = t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_HELPER, 0, 0, v, 2, 0);
        UM_CHECK(op != T1_NO_OP);
        for (unsigned k = 0; k < 2; ++k) {
            const struct t1_use *u = t1_ir_opnd(ir, op, k);
            UM_CHECK(u->value == v[k] && !u->element && !u->flags);
        }
        for (unsigned k = 0; k < 19; ++k)
            UM_CHECK(t1_ir_add_operand(ir, op, v[k % 2], T1_ELEMENT_GR(14 + k)));
        for (unsigned k = 2; k < ir->op[op].nuse; ++k) {
            const struct t1_use *u = t1_ir_opnd(ir, op, k);
            UM_CHECK(u->value == v[(k - 2) % 2] && u->element == T1_ELEMENT_GR(12 + k) &&
                     !u->flags);
        }
        for (unsigned k = ir->op[op].nuse; k < ir->op[op].use_cap; ++k) {
            const struct t1_use *u = t1_ir_opnd(ir, op, k);
            UM_CHECK(u->value == T1_VALUE_NONE && !u->element && !u->flags);
        }
        uint16_t elements[3] = {T1_ELEMENT_GR(14), T1_ELEMENT_GR(15), T1_ELEMENT_GR(16)};
        uint16_t flags[3] = {0, 0, T1_USE_HOME};
        for (unsigned explicit_flags = 0; explicit_flags < 2; ++explicit_flags) {
            uint32_t p = t1_ir_new_point(ir, T1_PT_EXIT, 0x1000, 0, 0, T1_HEADER_BLOCK, 0);
            unsigned n = explicit_flags ? 3 : 2;
            UM_CHECK(t1_ir_set_frame(ir, p, elements, v, explicit_flags ? flags : NULL, n));
            for (unsigned k = 0; k < n; ++k) {
                const struct t1_use *u = &ir->use[ir->point[p].frame_first + k];
                UM_CHECK(u->value == v[k] && u->element == elements[k] && u->flags == flags[k]);
            }
        }
        ir->nuses = T1_IR_MAX_USES - 2;
        op = t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_ADD, 0, 0, v, 2, 0);
        UM_CHECK(op != T1_NO_OP && ir->nuses == T1_IR_MAX_USES);
        unsigned nops = ir->nops;
        UM_CHECK(t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_ADD, 0, 0, v, 2, 0) == T1_NO_OP);
        UM_CHECK(ir->failed && ir->nops == nops && ir->nuses == T1_IR_MAX_USES &&
                 ir->block[T1_HEADER_BLOCK].last == op &&
                 !strcmp(ir->fail_reason, "use pool exhausted"));
        t1_ir_init(ir, NULL);
        uint32_t p = t1_ir_new_point(ir, T1_PT_EXIT, 0x1000, 0, 0, T1_HEADER_BLOCK, 0);
        uint32_t first = ir->point[p].frame_first, count = ir->point[p].frame_count;
        ir->nuses = T1_IR_MAX_USES - 1;
        UM_CHECK(!t1_ir_set_frame(ir, p, elements, v, flags, 2));
        UM_CHECK(ir->failed && ir->nuses == T1_IR_MAX_USES - 1 &&
                 ir->point[p].frame_first == first && ir->point[p].frame_count == count);
    }
}

static void use_tail_growth(struct t1_ir *ir)
{
    t1_ir_init(ir, NULL);
    UM_CHECK(t1_ir_new_block(ir, 0) == T1_ENTRY_BLOCK);
    UM_CHECK(t1_ir_new_block(ir, 0) == T1_HEADER_BLOCK);
    t1_value v[2] = {t1_ir_const(ir, T1_I64, 7), t1_ir_const(ir, T1_I64, 11)};
    /* A tail list needs only its final capacity, even at the pool limit. */
    ir->nuses = T1_IR_MAX_USES - 16;
    uint32_t op = t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_HELPER, 0, 0, v, 2, 0);
    UM_CHECK(op != T1_NO_OP);
    uint32_t first = ir->op[op].use_first;
    for (unsigned k = 2; k < 16; ++k)
        UM_CHECK(t1_ir_add_operand(ir, op, v[k % 2], T1_ELEMENT_GR(14 + k)));
    UM_CHECK(ir->nuses == T1_IR_MAX_USES && ir->op[op].use_first == first);
    for (unsigned k = 0; k < 16; ++k) {
        const struct t1_use *u = t1_ir_opnd(ir, op, k);
        UM_CHECK(u->value == v[k % 2] && !u->flags);
        UM_CHECK(u->element == (k < 2 ? 0 : T1_ELEMENT_GR(14 + k)));
    }
    UM_CHECK(!t1_ir_add_operand(ir, op, v[0], T1_ELEMENT_GR(30)));
    UM_CHECK(ir->failed && ir->nuses == T1_IR_MAX_USES && ir->op[op].use_first == first &&
             ir->op[op].nuse == 16 && ir->op[op].use_cap == 16);

    /* Interleaved lists must preserve each other's arguments when moved. */
    t1_ir_init(ir, NULL);
    UM_CHECK(t1_ir_new_block(ir, 0) == T1_ENTRY_BLOCK);
    UM_CHECK(t1_ir_new_block(ir, 0) == T1_HEADER_BLOCK);
    v[0] = t1_ir_const(ir, T1_I64, 7);
    v[1] = t1_ir_const(ir, T1_I64, 11);
    UM_CHECK(t1_ir_set_branch(ir, T1_ENTRY_BLOCK, t1_ir_const(ir, T1_I1, 1), T1_HEADER_BLOCK,
                              T1_HEADER_BLOCK, 0));
    for (unsigned k = 0; k < 17; ++k)
        for (unsigned s = 0; s < 2; ++s)
            UM_CHECK(t1_ir_edge_add_arg(ir, T1_ENTRY_BLOCK, s, v[s], T1_ELEMENT_GR(14 + k)));
    for (unsigned s = 0; s < 2; ++s) {
        const struct t1_edge *e = &ir->block[T1_ENTRY_BLOCK].succ[s];
        for (unsigned k = 0; k < 17; ++k) {
            const struct t1_use *u = &ir->use[e->args_first + k];
            UM_CHECK(u->value == v[s] && u->element == T1_ELEMENT_GR(14 + k) && !u->flags);
        }
    }
}

/* Exercise the highest mask bit and dominator trees whose parent order
 * differs from block numbering. The invalid variants must still reject. */
static void deep_dominance(struct t1_ir *ir)
{
    for (unsigned stride = 1; stride <= 17; stride += 16) {
        t1_ir_init(ir, NULL);
        for (unsigned b = 0; b < T1_MAX_BLOCKS; ++b)
            UM_CHECK(t1_ir_new_block(ir, 0) == b);
        t1_elem_add(ir->frame_set, T1_ELEMENT_GR(14));
        t1_elem_add(ir->frame_set, T1_ELEMENT_GR(15));
        t1_value one = t1_ir_const(ir, T1_I64, 1);
        t1_value previous = t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(14));
        uint16_t order[T1_MAX_BLOCKS - 1];
        uint32_t ops[T1_MAX_BLOCKS];
        for (unsigned k = 0; k < T1_MAX_BLOCKS - 1; ++k)
            order[k] = (uint16_t)(1 + k * stride % (T1_MAX_BLOCKS - 1));
        UM_CHECK(t1_ir_set_jump(ir, T1_ENTRY_BLOCK, order[0]));
        for (unsigned k = 0; k < T1_MAX_BLOCKS - 1; ++k) {
            unsigned b = order[k];
            t1_value operands[2] = {previous, one};
            ops[b] = t1_ir_append(ir, (uint16_t)b, T1_IR_ADD, 0, 0, operands, 2, 0);
            UM_CHECK(ops[b] != T1_NO_OP);
            previous = T1_VALUE(ops[b], 0);
            ir->block[b].charge = 1;
            if (k + 1 < T1_MAX_BLOCKS - 1)
                UM_CHECK(t1_ir_set_jump(ir, (uint16_t)b, order[k + 1]));
        }
        uint16_t last = order[T1_MAX_BLOCKS - 2];
        uint32_t p = t1_ir_new_point(ir, T1_PT_EXIT, 0x3000, 0, 0, last, 0);
        frame(ir, p, T1_VALUE(ops[T1_MAX_BLOCKS - 1], 0), T1_VALUE(ops[1], 0));
        UM_CHECK(t1_ir_set_exit(ir, last, p, 0));
        expect_ok(ir, 0, "deep permuted dominator tree");
        struct t1_use *u = &ir->use[ir->op[ops[1]].use_first];
        t1_value saved = u->value;
        u->value = previous;
        expect_fail(ir, 0, "does not dominate", "descendant used by ancestor");
        ++negatives;
        u->value = saved;
        ir->block[T1_ENTRY_BLOCK].npred = 1;
        ir->block[T1_ENTRY_BLOCK].pred[0] = last;
        expect_fail(ir, 0, "entry block has predecessors", "entry predecessor");
        ++negatives;
        ir->block[T1_ENTRY_BLOCK].npred = 0;
        ir->block[order[0]].npred = 0;
        expect_fail(ir, 0, "unreachable block", "missing entry predecessor");
        ++negatives;
    }
}

void unit_ir_test(void);
void unit_ir_test(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    UM_CHECK(ir != NULL);
    forwarding_chains(ir);
    use_storage(ir);
    use_tail_growth(ir);
    deep_dominance(ir);
    entry_index(ir);
    parameter_insertion(ir);
    struct base s = build(ir);
    expect_ok(ir, 0, "base");
    expect_ok(ir, T1_VERIFY_NAT_FOLDED, "base folded");
    UM_CHECK(t1_ir_max_path(ir, T1_HEADER_BLOCK) == 3);
    uint16_t idom[T1_MAX_BLOCKS];
    UM_CHECK(t1_ir_dominators(ir, idom) && idom[1] == 0 && idom[2] == 1 && idom[3] == 1);
    UM_CHECK(t1_ir_dominates(idom, 1, 3) && !t1_ir_dominates(idom, 2, 3));
    struct sink out = {{0}, 0};
    t1_ir_print(ir, sink_line, &out);
    UM_CHECK(strstr(out.text, "BRANCH") && strstr(out.text, "STORE") &&
             strstr(out.text, "-> b2()"));

#define NEG(substring, flags, ...)                                                                 \
    do {                                                                                           \
        s = build(ir);                                                                             \
        __VA_ARGS__;                                                                               \
        expect_fail(ir, flags, substring, #__VA_ARGS__);                                           \
        ++negatives;                                                                               \
    } while (0)
    /* Types and operand counts. */
    NEG("does not dominate", 0, ir->use[ir->op[s.y].use_first].value = T1_VALUE(s.cmp, 0));
    NEG("operand 1 type", 0, ir->use[ir->op[s.y].use_first + 1].value = T1_VALUE(s.m, 0));
    NEG("operand count", 0, ir->op[s.y].nuse = 1);
    /* Dominance: a value of b2 used in b3; a use before its definition. */
    NEG("does not dominate", 0, t1_value o[2] = {T1_VALUE(s.x, 0), T1_VALUE(s.c8, 0)};
        uint32_t a = t1_ir_append(ir, 2, T1_IR_ADD, 0, 0, o, 2, 0);
        ir->use[ir->point[s.p2].frame_first].value = T1_VALUE(a, 0));
    NEG("does not dominate", 0, ir->use[ir->op[s.y].use_first].value = T1_VALUE(s.y, 0));
    /* Forwarded and dead references. */
    NEG("not a live op", 0, ir->op[s.y].fwd = T1_VALUE(s.x, 0));
    NEG("not a live op", 0, ir->op[s.c0].flags |= T1_OPF_DEAD);
    /* Structure: CONST outside the entry block; no terminator; a second
     * terminator; a cycle without a BACKEDGE; a BACKEDGE whose target does
     * not dominate it. */
    NEG("only CONST and LIVEIN", 0, t1_ir_append(ir, 2, T1_IR_CONST, T1_I64, 5, NULL, 0, 0));
    NEG("no terminator", 0, t1_ir_remove(ir, ir->block[3].last));
    NEG("terminator position", 0, t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_JUMP, 0, 0, NULL, 0, 0));
    NEG("cycle without a back-edge", 0, t1_ir_remove(ir, ir->block[3].last);
        t1_ir_set_jump(ir, 3, T1_HEADER_BLOCK));
    NEG("back-edge target", 0, t1_ir_remove(ir, ir->block[3].last);
        uint32_t p = t1_ir_new_point(ir, T1_PT_CHECKPOINT, 0x1000, 0, 0, 3, 0);
        frame(ir, p, T1_VALUE(s.x, 0), T1_VALUE(s.x, 0)); t1_ir_set_backedge(ir, 3, 2, p, 0));
    /* CFG bounds must be checked before dominance and cycle traversal. */
    NEG("successor count", 0, ir->block[1].nsucc = 3);
    NEG("edge target", 0, ir->block[1].succ[0].target = T1_MAX_BLOCKS);
    NEG("predecessor count", 0, ir->block[2].npred = T1_MAX_BLOCKS + 1);
    NEG("predecessor block", 0, ir->block[2].pred[0] = T1_MAX_BLOCKS);
    NEG("no region", 0, ir->nblocks = T1_MAX_BLOCKS + 1);
    /* Edges: an argument without a parameter. */
    NEG("argument count", 0, t1_ir_edge_add_arg(ir, 1, 0, T1_VALUE(s.x, 0), T1_ELEMENT_GR(14)));
    /* Memory chain: two stores consuming one token in a line. */
    NEG("memory chain forks", 0,
        t1_value o[3] = {T1_VALUE(s.x, 0), T1_VALUE(s.x, 0), T1_VALUE(s.m, 0)};
        uint32_t p = t1_ir_new_point(ir, T1_PT_SITE, 0x1000, 0, 0, T1_HEADER_BLOCK, 0);
        frame(ir, p, T1_VALUE(s.x, 0), T1_VALUE(s.x, 0));
        uint32_t st =
            t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_STORE, 0, T1_ACCESS_IMM(8, 0, 1), o, 3, 0);
        ir->op[st].point = p; ir->point[p].op = st; ir->point[p].access_size = 8;
        ir->point[p].access_write = 1; ir->point[p].site = 1);
    /* Points: a site without a point; inconsistent access size; retired
     * offset beyond the block's charge. */
    NEG("missing point", 0, ir->op[s.st].point = T1_NO_POINT);
    NEG("access", 0, ir->point[s.p0].access_size = 4);
    NEG("retired offset", 0, ir->point[s.p0].retired_offset = 9);
    /* Frames: incomplete; a duplicate element; a home entry with a value;
     * a value that does not dominate; a value of the wrong type. */
    NEG("frame incomplete", 0, ir->point[s.p1].frame_count = 1);
    NEG("frame element", 0, ir->use[ir->point[s.p1].frame_first + 1].element = T1_ELEMENT_GR(14));
    NEG("has a value", 0, ir->use[ir->point[s.p1].frame_first].flags = T1_USE_HOME);
    NEG("frame type", 0, ir->use[ir->point[s.p2].frame_first].value = T1_VALUE(s.cmp, 0));
    /* NaT folding (after natguard): NATCHK, LIVEIN of a NaT element. */
    NEG("NATCHK survived folding", T1_VERIFY_NAT_FOLDED, t1_value n = t1_ir_const(ir, T1_I1, 0);
        t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_NATCHK, 0, 0, &n, 1, 0));
    NEG("LIVEIN of a NaT", T1_VERIFY_NAT_FOLDED, t1_ir_livein(ir, T1_I1, T1_ELEMENT_NAT(15)));
    NEG("exact entry NaT element", T1_VERIFY_NAT_FOLDED,
        t1_ir_append(ir, T1_ENTRY_BLOCK, T1_IR_NATIN, T1_I1, T1_ELEMENT_GR(15), NULL, 0, 0));
    /* Exact entry snapshots remain legal; LIVEIN(E_NAT) above does not. */
    s = build(ir);
    t1_ir_append(ir, T1_ENTRY_BLOCK, T1_IR_NATIN, T1_I1, T1_ELEMENT_NAT(15), NULL, 0, 0);
    expect_ok(ir, T1_VERIFY_NAT_FOLDED, "exact entry NaT");
    /* A failed IR is never accepted. */
    NEG("IR failed", 0, t1_ir_fail(ir, "test"));
    /* An inner loop: b3 branches back to itself, in a region whose head
     * heads no loop (a trace). Its max_path is its own path. */
    s = build(ir);
    t1_ir_remove(ir, ir->block[3].last);
    uint32_t ck = t1_ir_new_point(ir, T1_PT_CHECKPOINT, 0x1010, 0, 0, 3, 0);
    frame(ir, ck, T1_VALUE(s.x, 0), T1_VALUE(s.x, 0));
    ir->block[3].charge = 2;
    UM_CHECK(t1_ir_set_backedge(ir, 3, 3, ck, 0));
    expect_ok(ir, 0, "inner self loop");
    UM_CHECK(t1_ir_max_path(ir, 3) == 2 && t1_ir_max_path(ir, T1_HEADER_BLOCK) == 5);
    struct t1_ir_loops loops;
    t1_ir_loops(ir, &loops);
    UM_CHECK(loops.body[3] == UINT64_C(1) << 3 && loops.header[3] == 3 && loops.depth[3] == 1);
    UM_CHECK(!loops.body[T1_HEADER_BLOCK] && loops.header[T1_HEADER_BLOCK] == T1_NO_BLOCK &&
             !loops.depth[T1_HEADER_BLOCK]);

    /* Parameters: add one to b2 with arguments, forward it, compact. */
    s = build(ir);
    uint32_t prm = t1_ir_add_param(ir, 2, T1_I64, T1_ELEMENT_GR(15));
    UM_CHECK(t1_ir_edge_add_arg(ir, 1, 0, T1_VALUE(s.y, 0), T1_ELEMENT_GR(15)));
    ir->use[ir->point[s.p1].frame_first + 1].value = T1_VALUE(prm, 0);
    expect_ok(ir, 0, "param");
    t1_ir_forward(ir, prm, T1_VALUE(s.y, 0));
    expect_fail(ir, 0, "", "forwarded param before resolve");
    t1_ir_resolve_all(ir);
    t1_ir_compact_params(ir);
    UM_CHECK(ir->block[1].succ[0].nargs == 0 &&
             ir->use[ir->point[s.p1].frame_first + 1].value == T1_VALUE(s.y, 0));
    expect_ok(ir, 0, "param compacted");
    /* A constant branch becomes a jump; the unreachable exit dies with
     * its point. */
    t1_ir_branch_to_jump(ir, 1, 1);
    t1_ir_remove(ir, s.cmp);
    t1_ir_remove_unreachable(ir);
    UM_CHECK((ir->block[2].flags & T1_BLK_DEAD) && ir->point[s.p1].dead && ir->block[3].npred == 1);
    expect_ok(ir, 0, "branch folded");
    UM_CHECK(t1_ir_max_path(ir, T1_HEADER_BLOCK) == 3);
    verify_home_bits(ir);
    free(ir);
    printf(
        "unit_ir: builder, queries, forwarding, compaction ok; %u verifier rejections as expected\n",
        negatives);
}
