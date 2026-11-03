/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FP helper expansions (docs/floating-point.md): the table gen.py
 * writes from engine/tier1/rules/fpfast.rules, every rule conditionally checked by
 * tests/tier1/rules/prove.py, and the one walker that emits a rule (the
 * helper expansion in emit.c and the conformance runner share it).
 *
 * A rule is straight-line code on R11 and xmm0-xmm3 with forward jumps;
 * it reads its inputs through the caller (T1_FPX_IN: R11 = the value) and
 * stores the helper's results to R14-relative slots, then jumps to $DONE,
 * or jumps to $BAIL (the helper call) having stored nothing.
 */
#ifndef IA64VMM_TIER1_FPFAST_H
#define IA64VMM_TIER1_FPFAST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "engine/tier1/rules.h"
#include "engine/tier1/x86sse.h"

enum t1_fpx_kind {
    T1_FPX_X86,
    T1_FPX_SSE,
    T1_FPX_IN,
    T1_FPX_OUT_R,
    T1_FPX_OUT_X,
    T1_FPX_OUT_I,
    T1_FPX_LABEL
};
/* rulelib.EXPAND_INPUTS order: a = FR[f2], b = FR[f3], c = FR[f4], d = FR[f1]. */
enum t1_fpx_input {
    T1_FPX_QP,
    T1_FPX_FPSR,
    T1_FPX_A_SIG,
    T1_FPX_A_META,
    T1_FPX_B_SIG,
    T1_FPX_B_META,
    T1_FPX_C_SIG,
    T1_FPX_C_META,
    T1_FPX_D_SIG,
    T1_FPX_D_META,
    T1_FPX_INPUTS
};
/* rulelib.EXPAND_SLOTS order: FR[f1] significand and metadata, PSR.mfl/mfh. */
enum t1_fpx_slot { T1_FPX_SIG, T1_FPX_META, T1_FPX_MF, T1_FPX_SLOTS };
enum t1_fpx_addend { T1_FPX_ANY, T1_FPX_F0, T1_FPX_REG };
enum t1_fpx_mode { T1_FPX_EXACT, T1_FPX_APPROX };
#define T1_FPX_BAIL 0u /* labels 0 and 1 are the exits */
#define T1_FPX_DONE 1u
#define T1_FPX_NO_EXPR 0xffu
#define T1_FPX_MAX_LABELS 32u

struct t1_fpx_insn {
    uint8_t kind;
    uint16_t op;     /* X86: T1_X86_* (jcc: T1_X86_JCC + cc); SSE: enum t1_sse_op */
    uint8_t a, b, c; /* SSE: xmm operands; IN: the input; OUT: slot (a), xmm (b) */
    uint8_t expr;    /* the immediate's expression, or T1_FPX_NO_EXPR: imm */
    uint8_t label;   /* X86 jcc/jmp target; LABEL: the label defined */
    int64_t imm;
};

struct t1_fpx_rule {
    const char *name;
    uint16_t opcode;   /* Ia64Opcode */
    uint8_t precision; /* Ia64 precision completer: 1 .s, 2 .d, 0 none (the parallel forms: 1) */
    uint8_t addend, fma3, mode, nlabel;
    uint16_t ninsn;
    const struct t1_fpx_insn *insn;
    const struct t1_iexpr *expr;
};
extern const struct t1_fpx_rule t1_fpx_rules[];
extern const unsigned t1_fpx_rule_count;

struct t1_fpx_io {
    struct t1_x86_buf *b;
    /* Emits one x86 instruction; its rel32 fixup offset, else SIZE_MAX. */
    size_t (*x86)(void *ctx, const struct t1_x86_insn *i);
    void (*in)(void *ctx, unsigned input);    /* R11 = the input */
    int32_t (*out)(void *ctx, unsigned slot); /* the slot's displacement from R14 */
    void *ctx;
    int64_t imm[2]; /* #sf, #mf (rulelib.EXPAND_IMMS) */
};

/* The first rule for the instruction under the host's FMA3 and fp_approx,
 * or NULL. The parallel forms match any precision field. */
const struct t1_fpx_rule *t1_fpx_select(unsigned opcode, unsigned precision, bool addend_f0,
                                        bool fma3, bool approx);
/* Emits r. The rel32 fixups of the jumps to $BAIL and $DONE are appended
 * to bail (maxbail entries) and done (maxdone); false on an overflow of a
 * list or the buffer (the caller drops the code). */
bool t1_fpx_emit(const struct t1_fpx_rule *r, const struct t1_fpx_io *io, uint32_t *bail,
                 unsigned *nbail, unsigned maxbail, uint32_t *done, unsigned *ndone,
                 unsigned maxdone);
#endif
