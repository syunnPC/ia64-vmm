/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SSE2/FMA3 instructions of Tier1's FP expansions (engine/tier1/fpfast.c,
 * the templates of engine/tier1/rules/fpfast.rules). They use xmm0-xmm3 and
 * R11 only and are emitted outside the T1_X86_OPS table: no isel rule
 * selects them and the allocator never sees an XMM register. t1_x86_decode
 * accepts exactly these encodings through t1_sse_decode.
 * Authority: Intel SDM Vol.2 (MOVQ, POR, PXOR, PCMPEQD, PSLLQ, ADDSx,
 * MULSx, MULPS, VFMADD213Sx/PS) for the encodings.
 */
#ifndef IA64VMM_TIER1_X86SSE_H
#define IA64VMM_TIER1_X86SSE_H
#include "engine/tier1/x86enc.h"

/* The op id t1_x86_decode reports for these encodings; t1_x86_info gives
 * it a row with no operands, implicit registers, flags or attributes. */
#define T1_X86_SSE T1_X86_OP_COUNT

/* The ops, read by engine/tier1/rules/rulelib.py: id, template mnemonic
 * and operand form (XR: xmm, R11; RX: R11, xmm; XX: xmm, xmm; XI: xmm,
 * imm8; XXX: xmm, xmm, xmm, VEX). None changes RFLAGS. */
#define T1_SSE_OPS(X)                                                                              \
    X(MOVQ_XR, "movq", XR)                                                                         \
    X(MOVQ_RX, "movq", RX)                                                                         \
    X(POR, "por", XX)                                                                              \
    X(PXOR, "pxor", XX)                                                                            \
    X(PCMPEQD, "pcmpeqd", XX)                                                                      \
    X(PSLLQ, "psllq", XI)                                                                          \
    X(ADDSD, "addsd", XX)                                                                          \
    X(MULSD, "mulsd", XX)                                                                          \
    X(ADDSS, "addss", XX)                                                                          \
    X(MULSS, "mulss", XX)                                                                          \
    X(MULPS, "mulps", XX)                                                                          \
    X(FMADD213SD, "vfmadd213sd", XXX)                                                              \
    X(FMADD213SS, "vfmadd213ss", XXX)                                                              \
    X(FMADD213PS, "vfmadd213ps", XXX)

enum t1_sse_op {
#define T1_SSE_ENUM(id, name, form) T1_SSE_##id,
    T1_SSE_OPS(T1_SSE_ENUM)
#undef T1_SSE_ENUM
        T1_SSE_OP_COUNT
};
/* op dst, src for the register forms (MOVQ_XR/RX: the xmm and R11). */
void t1_sse_rr(struct t1_x86_buf *b, enum t1_sse_op op, unsigned dst, unsigned src);
/* vfmadd213s{d,s}/ps dst, src2, src3: dst = dst * src2 + src3. */
void t1_sse_fma(struct t1_x86_buf *b, enum t1_sse_op op, unsigned dst, unsigned src2,
                unsigned src3);
/* psllq xmm, count. */
void t1_sse_psllq(struct t1_x86_buf *b, unsigned xmm, unsigned count);
/* movq [R14 + disp32], xmm. */
void t1_sse_store(struct t1_x86_buf *b, unsigned xmm, int32_t disp);
/* Length (and the R14 store's memory operand) of one of the encodings
 * above; false for anything else. */
bool t1_sse_decode(const uint8_t *code, size_t avail, struct t1_x86_decoded *out);
#endif
