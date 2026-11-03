/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Instruction-selection table types and shared IR value identity.
 * rules/gen.py reads isel.rules through rulelib.py, emits build/tier1/isel_rules.h (t1_rules[] and t1_rule_count).
 */
#ifndef IA64VMM_TIER1_RULES_H
#define IA64VMM_TIER1_RULES_H
#include "ia64vmm/types.h"

/* A value is (defining op, result index); at most four results. */
typedef uint32_t t1_value; /* (op << 2) | result */
#define T1_VALUE(op, result) (((op) << 2) | (result))
#define T1_VALUE_OP(v) ((v) >> 2)
#define T1_VALUE_RESULT(v) ((v) & 3u)
#define T1_VALUE_NONE UINT32_MAX

enum t1_type { T1_I64, T1_I1, T1_MEM };

enum t1_ir_op {
#define T1_IR_OP(name, imm, effect, nopnd, nres) T1_IR_##name,
#include "engine/tier1/ir_ops.def"
    T1_IR_OP_COUNT
};
/* Result indices, e.g. T1_RES_CLOOP_taken == 1. */
enum t1_ir_result {
#define T1_IR_RESULT(op, index, name, type) T1_RES_##op##_##name = (index),
#include "engine/tier1/ir_ops.def"
};
_Static_assert(T1_IR_OP_COUNT < (1u << 16), "t1_pat.ir_op is 16 bits");

/* CMP's packed immediate (ir_ops.def, layout CMP): rel | width << 8. The
 * rule language spells rel as eq/lt/ltu and width as 32/64. */
enum t1_cmp_rel { T1_CMP_EQ, T1_CMP_LT, T1_CMP_LTU };
#define T1_CMP_IMM(rel, width) ((int64_t)(rel) | ((int64_t)(width) << 8))

/*
 * Rule binders: IN=$x, OUT=outputs, TEMP=temps, IMM=#k;
 * LABEL 0/1 are BRANCH true/false targets.
 *
 * Shared register model (isel, regalloc, prover): each template line has its
 * own uses/defs. Inputs live through their last use, including external uses.
 * Every write-only operand starts a fresh definition; writes narrower than
 * 32 bits have arbitrary upper bits, regardless of earlier clears. Binders
 * are independent unless tied; isel copies still-live tied inputs, including
 * aliases through other binders. Fixed binders occupy their required register
 * at every use. Register I1 values are 0 or 1 across all 64 bits.
 */
enum t1_opnd_kind {
    T1_OP_IN,
    T1_OP_OUT,
    T1_OP_TEMP,
    T1_OP_IMM,
    T1_OP_LABEL,
    T1_OP_MEM /* the template's memory operand (base/index/scale/disp) */
};
/* IMM: index = immediate binder, or T1_BINDER_NONE for the literal imm,
 * or (format 2) T1_IMM_EXPR(k) for a derived immediate (below).
 * MEM: index unused; the operand is struct t1_tmpl's memory fields. */
struct t1_opnd {
    uint8_t kind, index;
    int64_t imm;
}; /* index into the rule's binders */
#define T1_BINDER_NONE 0xffu

/*
 * Pattern nodes in preorder. ir_op is an enum t1_ir_op or T1_PAT_VALUE (a
 * leaf $x matching any value: bind = input binder; a binder used twice
 * matches only the same value). For an op node: result = the result index
 * its parent consumes, kid[] = preorder indices of its operand
 * patterns, bind = immediate binder (#k) or T1_BINDER_NONE, and the node
 * matches only if imm_min <= imm <= imm_max and, when imm_set != 0, imm is
 * in 0..31 with bit imm of imm_set set (the packed immediate of
 * ir_ops.def). The root's result is ignored for BRANCH roots.
 */
#define T1_PAT_VALUE 0xffffu
struct t1_pat {
    uint16_t ir_op;
    uint8_t result;
    uint8_t nkids, kid[3];
    uint8_t bind;
    int64_t imm_min, imm_max;
    uint32_t imm_set;
};
/*
 * One template line. x86op: enum t1_x86_op (x86enc.h). opnd[] follow the
 * op's operand order in T1_X86_OPS, implicit CL operands included (as the
 * binder fixed to rcx). The memory operand, when the op has an M or MA
 * spec: base and index are input/temp binder ids (T1_BINDER_ID) or
 * T1_BINDER_NONE, scale_shift 0..3 or T1_SCALE_FROM_IMM(k) = the shift is
 * immediate binder k (the rule's `when` restricts it to 0..3). Format 2:
 * index may be T1_BINDER_ID(T1_OP_WINDOW, 0) (LOAD/STORE rules), and
 * disp_expr adds a derived displacement.
 */
struct t1_tmpl {
    uint16_t x86op;
    uint8_t nopnd;
    struct t1_opnd opnd[4];
    uint8_t base, index, scale_shift;
    int32_t disp; /* memory operand when used */
    uint8_t disp_expr;
}; /* format 2: T1_TMPL_DISP_NONE or T1_IMM_EXPR(k) */
#define T1_SCALE_FROM_IMM(k) ((uint8_t)(0x80u | (k)))
/* Binder ids in t1_tie, t1_fixed and the memory fields: kind << 5 | index. */
#define T1_BINDER_ID(kind, index) ((uint8_t)(((kind) << 5) | (index)))
/* Format 2 types (defined at the end of this file). */
struct t1_iexpr;
struct t1_icheck;
struct t1_tie {
    uint8_t out, in;
}; /* OUT index, IN index */
struct t1_fixed {
    uint8_t binder, x86reg;
}; /* T1_BINDER_ID, enum t1_x86_reg */
/* t1_rule.flags: the effect of the whole template on
 * RFLAGS. PRESERVED is proved (all status flags unchanged); every other
 * value is a flags clobber for the allocator. DEAD_AFTER: the template ends
 * in a control transfer that consumed the flags it produced. */
enum t1_rule_flags { T1_RULE_PRESERVED, T1_RULE_CLOBBERED, T1_RULE_UNDEFINED, T1_RULE_DEAD_AFTER };
/* out_result[i] defines output binder i: ir_op and result name the IR
 * result, bind is the preorder index of the pattern node producing it. */
struct t1_rule {
    const char *name;
    uint16_t cost;
    uint8_t npat, ntmpl, nout, ntemp, nlabel, ntie, nfixed, flags;
    /* Generated summaries of the immutable pattern/output/template arrays.
     * These occupy the former padding before pat on the AMD64 ABI. */
    uint8_t nin, root_results;
    uint32_t required_isa;
    const struct t1_pat *pat; /* preorder */
    const struct t1_tmpl *tmpl;
    const struct t1_pat *out_result; /* nout IR results defined by the template */
    const struct t1_tie *tie;
    const struct t1_fixed *fixed;
    /* Format 2 (additive; below). */
    uint8_t nexpr, ncheck;
    const struct t1_iexpr *expr;
    const struct t1_icheck *check;
};
extern const struct t1_rule t1_rules[];
extern const unsigned t1_rule_count;

/*
 * Rule format 2 requires consumers to reject newer generated formats;
 * ignoring immediate checks applies rules outside their proved domain.
 *
 * Immediate expression nodes are in evaluation order, referring only to
 * earlier nodes. Arithmetic wraps at 64 bits, shifts use counts modulo 64,
 * and comparisons yield 0/1. imm[k] is the packed immediate bound to #k
 * (ir_ops.def); FIELD still has one binder, with pos/len extracted by
 * expressions. Expressions derive operands, displacements, folds and checks.
 *
 * tests/tier1/rules/prove.py uses these semantics; native conformance checks
 * t1_iexpr_eval against the prover.
 */
#define T1_RULES_FORMAT 2
enum t1_iexpr_op {
    T1_IX_LIT, /* imm */
    T1_IX_IMM, /* imm[a] (a = immediate binder) */
    T1_IX_ADD,
    T1_IX_SUB,
    T1_IX_AND,
    T1_IX_OR,
    T1_IX_XOR, /* v[a] op v[b] */
    T1_IX_SHL,
    T1_IX_SHRU,
    T1_IX_SHR, /* v[a] shifted by v[b] & 63; SHR arithmetic */
    T1_IX_EQ,
    T1_IX_LT,
    T1_IX_LTU, /* v[a] rel v[b] -> 0/1; LT signed, LTU unsigned */
    T1_IX_COUNT
};
struct t1_iexpr {
    uint8_t op, a, b;
    int64_t imm;
};
#define T1_IEXPR_MAX 32u
/* An immediate reference: t1_opnd.index of a T1_OP_IMM operand is an
 * immediate binder k (< 32), T1_BINDER_NONE (the literal t1_opnd.imm) or
 * T1_IMM_EXPR(k) (the value of expression node k). */
#define T1_IMM_EXPR(k) ((uint8_t)(0x40u | (k)))
#define T1_IMM_IS_EXPR(i) (((i) & 0xc0u) == 0x40u)
#define T1_IMM_EXPR_INDEX(i) ((unsigned)(i) & 0x3fu)
/* A relation check: the rule applies only if min <= value(expr) <= max
 * (signed), for every check, in addition to each pattern node's
 * imm_min/imm_max/imm_set. The matcher evaluates them after binding all
 * immediates and before costing. */
struct t1_icheck {
    uint8_t expr;
    int64_t min, max;
};

/* The value of expression node `root` for the bound immediates imm[]. */
static inline int64_t t1_iexpr_eval(const struct t1_iexpr *x, unsigned root, const int64_t *imm)
{
    uint64_t v[T1_IEXPR_MAX];
    const uint64_t sign = UINT64_C(1) << 63;
    if (root >= T1_IEXPR_MAX)
        return 0;
    for (unsigned i = 0; i <= root; ++i) {
        uint64_t a = x[i].a < i ? v[x[i].a] : 0, b = x[i].b < i ? v[x[i].b] : 0;
        switch (x[i].op) {
        case T1_IX_LIT:
            v[i] = (uint64_t)x[i].imm;
            break;
        case T1_IX_IMM:
            v[i] = (uint64_t)imm[x[i].a];
            break;
        case T1_IX_ADD:
            v[i] = a + b;
            break;
        case T1_IX_SUB:
            v[i] = a - b;
            break;
        case T1_IX_AND:
            v[i] = a & b;
            break;
        case T1_IX_OR:
            v[i] = a | b;
            break;
        case T1_IX_XOR:
            v[i] = a ^ b;
            break;
        case T1_IX_SHL:
            v[i] = a << (b & 63);
            break;
        case T1_IX_SHRU:
            v[i] = a >> (b & 63);
            break;
        /* Arithmetic shift without implementation-defined signed shifts. */
        case T1_IX_SHR:
            v[i] = (a & sign) ? ~(~a >> (b & 63)) : a >> (b & 63);
            break;
        case T1_IX_EQ:
            v[i] = a == b;
            break;
        case T1_IX_LT:
            v[i] = (a ^ sign) < (b ^ sign);
            break;
        case T1_IX_LTU:
            v[i] = a < b;
            break;
        default:
            v[i] = 0;
            break;
        }
    }
    return (int64_t)v[root];
}

/* Template memory operands of LOAD/STORE rules (the value side of a memory
 * site): base = the address binder, index =
 * T1_BINDER_ID(T1_OP_WINDOW, 0), the register holding the site's window
 * delta (R11 in the site sequence), scale 1, disp 0. The prover
 * assumes the window invariant: the host word at address + delta is the
 * guest word at address. */
#define T1_OP_WINDOW 6u
/* A template line's displacement: disp plus, when disp_expr is
 * T1_IMM_EXPR(k), the value of expression k (0 = none). The sum fits in
 * 32 bits for every immediate the rule accepts (proved). */
#define T1_TMPL_DISP_NONE 0u

/*
 * Fold tables come from rules/fold.rules into build/tier1/fold_rules.h.
 * Patterns/checks follow isel rules; pat[0].result selects the replaced value.
 * The replacement res[] is preorder: kid[] indexes res, result selects the
 * consumed result (res[0] selects the replacement), and imm is T1_BINDER_NONE
 * or T1_IMM_EXPR(k). T1_PAT_VALUE reuses input binder bind. CONST inherits its
 * use's type, including the replaced type at the root. Every rule proves
 * pattern/result semantic equality over its checked domain.
 */
struct t1_fold_node {
    uint16_t ir_op;
    uint8_t result, nkids, kid[3], bind, imm;
};
struct t1_fold_rule {
    const char *name;
    uint8_t npat, nres, nexpr, ncheck;
    const struct t1_pat *pat;
    const struct t1_fold_node *res;
    const struct t1_iexpr *expr;
    const struct t1_icheck *check;
};
#endif
