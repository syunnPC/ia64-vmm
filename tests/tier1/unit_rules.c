/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unit test of the generated rule table against rules.h: every entry gen.py emits is well formed, and the
 * spike rules carry the constraints the rule text states.
 *
 * engine/tier1/isel.c is the translation unit that instantiates
 * build/tier1/isel_rules.h; this file uses the extern declarations of
 * rules.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/tier1/rules.h"
#include "engine/tier1/x86enc.h"
#include "build/tier1/fold_rules.h"

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static const struct t1_rule *rule(const char *name)
{
    for (unsigned i = 0; i < t1_rule_count; ++i)
        if (!strcmp(t1_rules[i].name, name))
            return &t1_rules[i];
    fprintf(stderr, "unit_rules: rule %s missing\n", name);
    exit(1);
}

static unsigned count_of(const struct t1_rule *r, unsigned kind)
{
    unsigned in = 0;
    for (unsigned i = 0; i < r->npat; ++i)
        if (r->pat[i].ir_op == T1_PAT_VALUE && r->pat[i].bind + 1u > in)
            in = r->pat[i].bind + 1u;
    switch (kind) {
    case T1_OP_IN:
        return in;
    case T1_OP_OUT:
        return r->nout;
    case T1_OP_TEMP:
        return r->ntemp;
    case T1_OP_LABEL:
        return r->nlabel;
    default:
        return 32;
    }
}

static void well_formed(const struct t1_rule *r)
{
    CHECK(r->npat >= 1 && r->ntmpl >= 1 && r->flags <= T1_RULE_DEAD_AFTER);
    CHECK(r->nin == count_of(r, T1_OP_IN));
    unsigned results = 0, required = 0;
    for (unsigned i = 0; i < r->nout; ++i)
        if (r->out_result[i].bind == 0)
            results |= 1u << r->out_result[i].result;
    for (unsigned i = 0; i < r->ntmpl; ++i)
        required |= t1_x86_required_isa(r->tmpl[i].x86op);
    CHECK(r->root_results == results && r->required_isa == required);
    for (unsigned i = 0; i < r->npat; ++i) {
        const struct t1_pat *p = &r->pat[i];
        CHECK(p->ir_op == T1_PAT_VALUE || p->ir_op < T1_IR_OP_COUNT);
        CHECK(p->nkids <= 3 && p->imm_min <= p->imm_max);
        for (unsigned k = 0; k < p->nkids; ++k)
            CHECK(p->kid[k] > i && p->kid[k] < r->npat); /* preorder */
    }
    bool branch = r->pat[0].ir_op == T1_IR_BRANCH;
    CHECK(branch ? r->nlabel == 2 : r->nlabel == 0);
    for (unsigned t = 0; t < r->ntmpl; ++t) {
        const struct t1_tmpl *m = &r->tmpl[t];
        CHECK(m->x86op < T1_X86_OP_COUNT && m->nopnd <= 4);
        for (unsigned o = 0; o < m->nopnd; ++o) {
            const struct t1_opnd *op = &m->opnd[o];
            CHECK(op->kind <= T1_OP_MEM);
            if (op->kind != T1_OP_IMM && op->kind != T1_OP_MEM)
                CHECK(op->index < count_of(r, op->kind));
        }
    }
    for (unsigned i = 0; i < r->nout; ++i)
        CHECK(r->out_result[i].bind < r->npat &&
              r->pat[r->out_result[i].bind].ir_op == r->out_result[i].ir_op);
    for (unsigned i = 0; i < r->ntie; ++i)
        CHECK(r->tie[i].out < r->nout && r->tie[i].in < count_of(r, T1_OP_IN));
    for (unsigned i = 0; i < r->nfixed; ++i)
        CHECK(r->fixed[i].x86reg < T1_X86_NREGS && r->fixed[i].x86reg != T1_RSP &&
              (r->fixed[i].binder >> 5) <= T1_OP_TEMP);
}

void unit_rules_test(void);
void unit_rules_test(void)
{
    CHECK(t1_rule_count >= 6); /* the spike rules and later ones */
    for (unsigned i = 0; i < t1_rule_count; ++i)
        well_formed(&t1_rules[i]);

    /* The fold index must be exactly the old full scan's eligible rules,
     * in table order, including empty buckets and multi-result roots.
     * This checks future rule additions without duplicating the index. */
    unsigned indexed = 0;
    for (unsigned op = 0; op <= T1_IR_OP_COUNT; ++op) {
        unsigned count, at = 0;
        const uint16_t *candidates = t1_fold_leaf_candidates(op, &count);
        for (unsigned i = 0; i < t1_fold_rule_count; ++i) {
            const struct t1_fold_rule *f = &t1_fold_rules[i];
            if (f->pat[0].ir_op != op || f->nres != 1 ||
                (f->res[0].ir_op != T1_IR_CONST && f->res[0].ir_op != T1_PAT_VALUE))
                continue;
            CHECK(at < count && candidates[at++] == i);
        }
        CHECK(at == count);
        indexed += count;
    }
    CHECK(indexed > 0 && indexed <= t1_fold_rule_count);

    const struct t1_rule *r = rule("cmp_lt_branch");
    CHECK(r->pat[1].ir_op == T1_IR_CMP && r->pat[1].imm_min == T1_CMP_IMM(T1_CMP_LT, 64));
    CHECK(r->tmpl[1].x86op == T1_X86_JCC + T1_CC_L && r->tmpl[1].opnd[0].kind == T1_OP_LABEL &&
          r->tmpl[1].opnd[0].index == 0 && r->flags == T1_RULE_DEAD_AFTER);

    r = rule("cloop"); /* multi-output: BRANCH consumes @taken, $n defines @next */
    CHECK(r->pat[1].ir_op == T1_IR_CLOOP && r->pat[1].result == T1_RES_CLOOP_taken);
    CHECK(r->nout == 1 && r->out_result[0].result == T1_RES_CLOOP_next &&
          r->out_result[0].bind == 1);
    CHECK(r->ntie == 1 && r->tie[0].out == 0 && r->tie[0].in == 0);

    r = rule("shladd_lea"); /* scale from the immediate binder #c in {1, 2, 3} */
    CHECK(r->pat[1].ir_op == T1_IR_SHL_IMM && r->pat[1].imm_set == 0xe && r->pat[1].bind == 0);
    CHECK(r->tmpl[0].x86op == T1_X86_LEA64 && r->tmpl[0].scale_shift == T1_SCALE_FROM_IMM(0) &&
          r->tmpl[0].opnd[1].kind == T1_OP_MEM && r->flags == T1_RULE_PRESERVED);

    r = rule("shladd4"); /* count 4: no lea scale 16, a temp */
    CHECK(r->pat[1].imm_set == 0x10 && r->ntemp == 1 && r->ntmpl == 2);
    CHECK(r->tmpl[0].base == T1_BINDER_NONE && r->tmpl[0].scale_shift == 3 &&
          r->tmpl[1].index == T1_BINDER_ID(T1_OP_TEMP, 0) && r->tmpl[1].scale_shift == 1);

    r = rule("cmp4_ltu");
    CHECK(r->pat[0].imm_min == T1_CMP_IMM(T1_CMP_LTU, 32));
    CHECK(r->tmpl[1].x86op == T1_X86_SET8 + T1_CC_B && r->tmpl[2].x86op == T1_X86_MOVZX32_8);

    r = rule("shl_var"); /* tie, fixed rcx, temp, zero idiom */
    CHECK(r->nfixed == 1 && r->fixed[0].binder == T1_BINDER_ID(T1_OP_IN, 1) &&
          r->fixed[0].x86reg == T1_RCX);
    CHECK(r->tmpl[0].x86op == T1_X86_ZERO32 && r->tmpl[1].x86op == T1_X86_SHL64_RCL &&
          r->tmpl[1].opnd[1].kind == T1_OP_IN && r->tmpl[1].opnd[1].index == 1 &&
          r->tmpl[3].x86op == T1_X86_CMOV64 + T1_CC_A);

    /* Compare the generated candidate membership and tie-breaking order
     * with the original full scan, including empty and invalid roots. */
    extern const uint16_t *t1_test_isel_candidates(unsigned op, unsigned *count);
    for (unsigned op = 0; op <= T1_IR_OP_COUNT; ++op) {
        unsigned count, at = 0;
        const uint16_t *candidates = t1_test_isel_candidates(op, &count);
        for (unsigned i = 0; op < T1_IR_OP_COUNT && i < t1_rule_count; ++i) {
            unsigned root = t1_rules[i].pat[0].ir_op;
            if (root == T1_IR_LOAD || root == T1_IR_STORE || (root != op && root != T1_PAT_VALUE))
                continue;
            CHECK(at < count && candidates[at++] == i);
        }
        CHECK(at == count);
    }
    printf("unit_rules: %u generated rules well formed\n", t1_rule_count);
}
