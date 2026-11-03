/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tier1 AMD64 encoder interface and per-instruction operand/def/flags table.
 *
 * The table T1_X86_OPS below is the single description of every AMD64
 * instruction Tier1 emits. Its consumers are the MIR (mir.h: use, def,
 * clobber of every instruction), the post-allocation verifier
 * (checked against this architectural def table, including implicit
 * RAX/RDX/RCX/flags writes and 32-bit zero extension), the encoder and
 * length decoder (x86enc.c), the rule tools (engine/tier1/rules/rulelib.py
 * parses the rows textually to resolve rule mnemonics) and
 * tests/tier1/rules/flags.py (cross-check of the flag columns against the
 * prover's flag model). Rows use X(...) or XCC(...), with comma-separated operand specs
 * and flag sets written with the T1_XO_ and T1_XF_ names defined here.
 *
 * Authority: Intel SDM Vol.2 (instruction reference) for encodings and
 * Vol.1 Appendix A (EFLAGS cross-reference) for the flag columns.
 * Engine-private: no engine/tier0 header is reused.
 */
#ifndef IA64VMM_TIER1_X86ENC_H
#define IA64VMM_TIER1_X86ENC_H
#include "ia64vmm/types.h"
#include "ia64vmm/host_isa.h"

/* Hardware register numbers (ModRM/REX order). struct hostwin_trap's gpr[]
 * and struct t1_exec's regs[] use the same numbering. */
enum t1_x86_reg {
    T1_RAX,
    T1_RCX,
    T1_RDX,
    T1_RBX,
    T1_RSP,
    T1_RBP,
    T1_RSI,
    T1_RDI,
    T1_R8,
    T1_R9,
    T1_R10,
    T1_R11,
    T1_R12,
    T1_R13,
    T1_R14,
    T1_R15,
    T1_X86_NREGS,
    T1_X86_NOREG = 0xff,
};
#define T1_X86_REGBIT(r) ((uint16_t)(1u << (r)))

/* RFLAGS status bits, at their architectural positions. */
#define T1_XF_NONE 0u
#define T1_XF_CF (1u << 0)
#define T1_XF_PF (1u << 2)
#define T1_XF_AF (1u << 4)
#define T1_XF_ZF (1u << 6)
#define T1_XF_SF (1u << 7)
#define T1_XF_OF (1u << 11)
#define T1_XF_OSZPC (T1_XF_OF | T1_XF_SF | T1_XF_ZF | T1_XF_PF | T1_XF_CF)
#define T1_XF_OSZP (T1_XF_OF | T1_XF_SF | T1_XF_ZF | T1_XF_PF)
#define T1_XF_SZP (T1_XF_SF | T1_XF_ZF | T1_XF_PF)
#define T1_XF_SZPC (T1_XF_SF | T1_XF_ZF | T1_XF_PF | T1_XF_CF)
#define T1_XF_ALL (T1_XF_OSZPC | T1_XF_AF)

/* Condition codes in the encoding order of Jcc/SETcc/CMOVcc (the low nibble
 * of 0F 8x / 0F 9x / 0F 4x). The rule language spells them as suffixes:
 * j<cc>, set<cc>8, cmov<cc>64; z/nz/c/nc are accepted aliases of e/ne/b/ae. */
enum t1_x86_cc {
    T1_CC_O,
    T1_CC_NO,
    T1_CC_B,
    T1_CC_AE,
    T1_CC_E,
    T1_CC_NE,
    T1_CC_BE,
    T1_CC_A,
    T1_CC_S,
    T1_CC_NS,
    T1_CC_P,
    T1_CC_NP,
    T1_CC_L,
    T1_CC_GE,
    T1_CC_LE,
    T1_CC_G,
    T1_CC_COUNT
};
/* RFLAGS bits each condition reads (SDM Vol.1 Table B-1). */
#define T1_X86_CC_READS(cc)                                                                        \
    ((cc) <= T1_CC_NO   ? T1_XF_OF                                                                 \
     : (cc) <= T1_CC_NE ? ((cc) <= T1_CC_AE ? T1_XF_CF : T1_XF_ZF)                                 \
     : (cc) <= T1_CC_A  ? (T1_XF_CF | T1_XF_ZF)                                                    \
     : (cc) <= T1_CC_NS ? T1_XF_SF                                                                 \
     : (cc) <= T1_CC_NP ? T1_XF_PF                                                                 \
     : (cc) <= T1_CC_GE ? (T1_XF_SF | T1_XF_OF)                                                    \
                        : (T1_XF_ZF | T1_XF_SF | T1_XF_OF))

/*
 * Operand specs: <kind><width>_<access>.
 *   R64/R32/R16/R8  register operand of that width (REX-encoded as needed;
 *                   R8 never names AH..BH).
 *   M8..M64         memory operand of that width (base + index*scale + disp32,
 *                   struct t1_x86_mem); MA = address only (lea, no access).
 *   I8/I32/I64      immediate, sign-extended to the operation width, except
 *                   I64 (movabs) and the I32 of mov r32 (zero-extended).
 *   REL             rel32 label operand.
 *   CL              the CL register; the operand must be fixed to RCX.
 * Access: R read, W write, RW read and write.
 * Physical write width semantics: a 64-bit
 * write replaces the register; a 32-bit write zero-extends into bits 63:32;
 * 8- and 16-bit writes preserve the other bits of the 64-bit register.
 * A write-only byte operand does not use the previous vreg definition
 * (t1_mir_effects); the rule prover must allow fresh upper bits if
 * allocation changes the physical register. See rules.h's register model.
 */
enum t1_x86_opnd_spec {
    T1_XO_NONE,
    T1_XO_R64_R,
    T1_XO_R64_W,
    T1_XO_R64_RW,
    T1_XO_R32_R,
    T1_XO_R32_W,
    T1_XO_R32_RW,
    T1_XO_R16_R,
    T1_XO_R8_R,
    T1_XO_R8_W,
    T1_XO_M8_R,
    T1_XO_M16_R,
    T1_XO_M32_R,
    T1_XO_M64_R,
    T1_XO_M8_W,
    T1_XO_M16_W,
    T1_XO_M32_W,
    T1_XO_M64_W,
    T1_XO_M64_RW,
    T1_XO_MA,
    T1_XO_I8,
    T1_XO_I32,
    T1_XO_I64,
    T1_XO_REL,
    T1_XO_CL,
};

/* Attributes. */
#define T1_XA_NONE 0u
/* Flags are written only if the masked count is non-zero (SHL/SHR/SAR):
 * with count 0 every flag keeps its value; OF is defined only for count 1
 * and is listed as undefined (havoc) here. */
#define T1_XA_COUNT0_KEEPS_FLAGS (1u << 0)
#define T1_XA_BRANCH (1u << 1)     /* transfers control to its REL operand */
#define T1_XA_NOFALL (1u << 2)     /* never falls through (jmp, ret) */
#define T1_XA_CALL (1u << 3)       /* SysV call: clobbers T1_X86_CALLER_SAVED and all flags */
#define T1_XA_ZERO_IDIOM (1u << 4) /* xor r32, r32 of one register: defines it without reading */

#define T1_X86_CALLER_SAVED                                                                        \
    (T1_X86_REGBIT(T1_RAX) | T1_X86_REGBIT(T1_RCX) | T1_X86_REGBIT(T1_RDX) |                       \
     T1_X86_REGBIT(T1_RSI) | T1_X86_REGBIT(T1_RDI) | T1_X86_REGBIT(T1_R8) | T1_X86_REGBIT(T1_R9) | \
     T1_X86_REGBIT(T1_R10) | T1_X86_REGBIT(T1_R11))
#define T1_X86_RSP_ONLY T1_X86_REGBIT(T1_RSP)

/*
 * X(ID, "mnemonic", nopnd, op0, op1, implicit_use, implicit_def,
 *   flags_read, flags_def, flags_undef, attributes)
 * A mnemonic may name several rows (e.g. sub64 register and immediate
 * forms); the rule tools pick the row whose operand specs match the
 * operand kinds written (register, immediate, memory, label, cl). The zero
 * idiom ZERO32 is written `xor32 $t, $t`.
 * XCC(ID, "pattern", ...) is a family of 16 rows, one per enum t1_x86_cc,
 * with "<cc>" in the pattern replaced by the condition's name and
 * flags_read = T1_X86_CC_READS(cc); its op id is T1_X86_<ID> + cc.
 * flags_def: written with an architecturally defined value. flags_undef:
 * architecturally undefined afterwards (the prover havocs them). Per
 * AF is treated as undefined after every flag-writing
 * instruction; no rule may read it. Implicit masks are T1_X86_REGBIT sets.
 * Additions are append-only (op ids are table positions).
 */
#define T1_X86_OPS(X, XCC)                                                                         \
    X(MOV64_RR, "mov64", 2, T1_XO_R64_W, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,    \
      T1_XA_NONE)                                                                                  \
    X(MOV32_RR, "mov32", 2, T1_XO_R32_W, T1_XO_R32_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,    \
      T1_XA_NONE)                                                                                  \
    X(MOV64_RI64, "movabs64", 2, T1_XO_R64_W, T1_XO_I64, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE, \
      T1_XA_NONE)                                                                                  \
    X(MOV64_RI32, "mov64", 2, T1_XO_R64_W, T1_XO_I32, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,    \
      T1_XA_NONE)                                                                                  \
    X(MOV32_RI32, "mov32", 2, T1_XO_R32_W, T1_XO_I32, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,    \
      T1_XA_NONE)                                                                                  \
    X(LOAD64, "load64", 2, T1_XO_R64_W, T1_XO_M64_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,     \
      T1_XA_NONE)                                                                                  \
    X(LOAD32, "load32", 2, T1_XO_R32_W, T1_XO_M32_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,     \
      T1_XA_NONE)                                                                                  \
    X(LOAD16ZX, "load16zx", 2, T1_XO_R32_W, T1_XO_M16_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE, \
      T1_XA_NONE)                                                                                  \
    X(LOAD8ZX, "load8zx", 2, T1_XO_R32_W, T1_XO_M8_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,    \
      T1_XA_NONE)                                                                                  \
    X(STORE64, "store64", 2, T1_XO_M64_W, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,   \
      T1_XA_NONE)                                                                                  \
    X(STORE32, "store32", 2, T1_XO_M32_W, T1_XO_R32_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,   \
      T1_XA_NONE)                                                                                  \
    X(STORE16, "store16", 2, T1_XO_M16_W, T1_XO_R16_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,   \
      T1_XA_NONE)                                                                                  \
    X(STORE8, "store8", 2, T1_XO_M8_W, T1_XO_R8_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,       \
      T1_XA_NONE)                                                                                  \
    X(STORE64_I32, "store64", 2, T1_XO_M64_W, T1_XO_I32, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE, \
      T1_XA_NONE)                                                                                  \
    X(STORE32_I32, "store32", 2, T1_XO_M32_W, T1_XO_I32, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE, \
      T1_XA_NONE)                                                                                  \
    X(MOVZX32_8, "movzx32_8", 2, T1_XO_R32_W, T1_XO_R8_R, 0, 0, T1_XF_NONE, T1_XF_NONE,            \
      T1_XF_NONE, T1_XA_NONE)                                                                      \
    X(MOVZX32_16, "movzx32_16", 2, T1_XO_R32_W, T1_XO_R16_R, 0, 0, T1_XF_NONE, T1_XF_NONE,         \
      T1_XF_NONE, T1_XA_NONE)                                                                      \
    X(MOVSX64_8, "movsx64_8", 2, T1_XO_R64_W, T1_XO_R8_R, 0, 0, T1_XF_NONE, T1_XF_NONE,            \
      T1_XF_NONE, T1_XA_NONE)                                                                      \
    X(MOVSX64_16, "movsx64_16", 2, T1_XO_R64_W, T1_XO_R16_R, 0, 0, T1_XF_NONE, T1_XF_NONE,         \
      T1_XF_NONE, T1_XA_NONE)                                                                      \
    X(MOVSX64_32, "movsx64_32", 2, T1_XO_R64_W, T1_XO_R32_R, 0, 0, T1_XF_NONE, T1_XF_NONE,         \
      T1_XF_NONE, T1_XA_NONE)                                                                      \
    X(LEA64, "lea64", 2, T1_XO_R64_W, T1_XO_MA, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,          \
      T1_XA_NONE)                                                                                  \
    X(MULPAIR64, "mulpair64", 2, T1_XO_R64_RW, T1_XO_R64_RW, 0, 0, T1_XF_NONE,                     \
      T1_XF_OF | T1_XF_CF, T1_XF_SF | T1_XF_ZF | T1_XF_PF | T1_XF_AF, T1_XA_NONE)                  \
    X(IMULPAIR64, "imulpair64", 2, T1_XO_R64_RW, T1_XO_R64_RW, 0, 0, T1_XF_NONE,                   \
      T1_XF_OF | T1_XF_CF, T1_XF_SF | T1_XF_ZF | T1_XF_PF | T1_XF_AF, T1_XA_NONE)                  \
    X(ADC64_RI, "adc64", 2, T1_XO_R64_RW, T1_XO_I32, 0, 0, T1_XF_CF, T1_XF_OSZPC, T1_XF_AF,        \
      T1_XA_NONE)                                                                                  \
    X(IMUL64_RR, "imul64", 2, T1_XO_R64_RW, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_OF | T1_XF_CF,    \
      T1_XF_SF | T1_XF_ZF | T1_XF_PF | T1_XF_AF, T1_XA_NONE)                                       \
    X(ADD64_RR, "add64", 2, T1_XO_R64_RW, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,    \
      T1_XA_NONE)                                                                                  \
    X(ADD64_RI, "add64", 2, T1_XO_R64_RW, T1_XO_I32, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,      \
      T1_XA_NONE)                                                                                  \
    X(SUB64_RR, "sub64", 2, T1_XO_R64_RW, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,    \
      T1_XA_NONE)                                                                                  \
    X(SUB64_RI, "sub64", 2, T1_XO_R64_RW, T1_XO_I32, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,      \
      T1_XA_NONE)                                                                                  \
    X(AND64_RR, "and64", 2, T1_XO_R64_RW, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,    \
      T1_XA_NONE)                                                                                  \
    X(AND64_RI, "and64", 2, T1_XO_R64_RW, T1_XO_I32, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,      \
      T1_XA_NONE)                                                                                  \
    X(OR64_RR, "or64", 2, T1_XO_R64_RW, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,      \
      T1_XA_NONE)                                                                                  \
    X(OR64_RI, "or64", 2, T1_XO_R64_RW, T1_XO_I32, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,        \
      T1_XA_NONE)                                                                                  \
    X(OR32_RM, "or32", 2, T1_XO_R32_RW, T1_XO_M32_R, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,      \
      T1_XA_NONE)                                                                                  \
    X(XOR64_RR, "xor64", 2, T1_XO_R64_RW, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,    \
      T1_XA_NONE)                                                                                  \
    X(XOR64_RI, "xor64", 2, T1_XO_R64_RW, T1_XO_I32, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,      \
      T1_XA_NONE)                                                                                  \
    X(ZERO32, "xor32", 1, T1_XO_R32_W, T1_XO_NONE, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,        \
      T1_XA_ZERO_IDIOM)                                                                            \
    X(NOT64, "not64", 1, T1_XO_R64_RW, T1_XO_NONE, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,       \
      T1_XA_NONE)                                                                                  \
    X(NEG64, "neg64", 1, T1_XO_R64_RW, T1_XO_NONE, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,        \
      T1_XA_NONE)                                                                                  \
    X(INC64_R, "inc64", 1, T1_XO_R64_RW, T1_XO_NONE, 0, 0, T1_XF_NONE, T1_XF_OSZP, T1_XF_AF,       \
      T1_XA_NONE)                                                                                  \
    X(INC64_M, "inc64", 1, T1_XO_M64_RW, T1_XO_NONE, 0, 0, T1_XF_NONE, T1_XF_OSZP, T1_XF_AF,       \
      T1_XA_NONE)                                                                                  \
    X(DEC64_M, "dec64", 1, T1_XO_M64_RW, T1_XO_NONE, 0, 0, T1_XF_NONE, T1_XF_OSZP, T1_XF_AF,       \
      T1_XA_NONE)                                                                                  \
    X(CMP64_RR, "cmp64", 2, T1_XO_R64_R, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,     \
      T1_XA_NONE)                                                                                  \
    X(CMP64_RI, "cmp64", 2, T1_XO_R64_R, T1_XO_I32, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,       \
      T1_XA_NONE)                                                                                  \
    X(CMP64_RM, "cmp64", 2, T1_XO_R64_R, T1_XO_M64_R, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,     \
      T1_XA_NONE)                                                                                  \
    X(CMP32_RR, "cmp32", 2, T1_XO_R32_R, T1_XO_R32_R, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,     \
      T1_XA_NONE)                                                                                  \
    X(CMP32_RI, "cmp32", 2, T1_XO_R32_R, T1_XO_I32, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,       \
      T1_XA_NONE)                                                                                  \
    X(TEST64_RR, "test64", 2, T1_XO_R64_R, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,   \
      T1_XA_NONE)                                                                                  \
    X(TEST64_RI, "test64", 2, T1_XO_R64_R, T1_XO_I32, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,     \
      T1_XA_NONE)                                                                                  \
    X(TEST32_RR, "test32", 2, T1_XO_R32_R, T1_XO_R32_R, 0, 0, T1_XF_NONE, T1_XF_OSZPC, T1_XF_AF,   \
      T1_XA_NONE)                                                                                  \
    X(SHL64_RI, "shl64", 2, T1_XO_R64_RW, T1_XO_I8, 0, 0, T1_XF_NONE, T1_XF_SZPC,                  \
      T1_XF_OF | T1_XF_AF, T1_XA_COUNT0_KEEPS_FLAGS)                                               \
    X(SHR64_RI, "shr64", 2, T1_XO_R64_RW, T1_XO_I8, 0, 0, T1_XF_NONE, T1_XF_SZPC,                  \
      T1_XF_OF | T1_XF_AF, T1_XA_COUNT0_KEEPS_FLAGS)                                               \
    X(SAR64_RI, "sar64", 2, T1_XO_R64_RW, T1_XO_I8, 0, 0, T1_XF_NONE, T1_XF_SZPC,                  \
      T1_XF_OF | T1_XF_AF, T1_XA_COUNT0_KEEPS_FLAGS)                                               \
    X(SHL64_RCL, "shl64", 2, T1_XO_R64_RW, T1_XO_CL, 0, 0, T1_XF_NONE, T1_XF_SZPC,                 \
      T1_XF_OF | T1_XF_AF, T1_XA_COUNT0_KEEPS_FLAGS)                                               \
    X(SHR64_RCL, "shr64", 2, T1_XO_R64_RW, T1_XO_CL, 0, 0, T1_XF_NONE, T1_XF_SZPC,                 \
      T1_XF_OF | T1_XF_AF, T1_XA_COUNT0_KEEPS_FLAGS)                                               \
    X(SAR64_RCL, "sar64", 2, T1_XO_R64_RW, T1_XO_CL, 0, 0, T1_XF_NONE, T1_XF_SZPC,                 \
      T1_XF_OF | T1_XF_AF, T1_XA_COUNT0_KEEPS_FLAGS)                                               \
    XCC(CMOV64, "cmov<cc>64", 2, T1_XO_R64_RW, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_NONE,          \
        T1_XA_NONE)                                                                                \
    XCC(SET8, "set<cc>8", 1, T1_XO_R8_W, T1_XO_NONE, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XA_NONE)     \
    XCC(JCC, "j<cc>", 1, T1_XO_REL, T1_XO_NONE, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XA_BRANCH)        \
    X(JMP, "jmp", 1, T1_XO_REL, T1_XO_NONE, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,              \
      T1_XA_BRANCH | T1_XA_NOFALL)                                                                 \
    X(JMP_M, "jmp", 1, T1_XO_M64_R, T1_XO_NONE, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,          \
      T1_XA_NOFALL)                                                                                \
    X(CALL_R, "call", 1, T1_XO_R64_R, T1_XO_NONE, T1_X86_RSP_ONLY, T1_X86_CALLER_SAVED,            \
      T1_XF_NONE, T1_XF_NONE, T1_XF_ALL, T1_XA_CALL)                                               \
    X(RET, "ret", 0, T1_XO_NONE, T1_XO_NONE, T1_X86_RSP_ONLY, T1_X86_RSP_ONLY, T1_XF_NONE,         \
      T1_XF_NONE, T1_XF_NONE, T1_XA_NOFALL)                                                        \
    X(PUSH64_R, "push64", 1, T1_XO_R64_R, T1_XO_NONE, T1_X86_RSP_ONLY, T1_X86_RSP_ONLY,            \
      T1_XF_NONE, T1_XF_NONE, T1_XF_NONE, T1_XA_NONE)                                              \
    X(PUSH64_M, "push64", 1, T1_XO_M64_R, T1_XO_NONE, T1_X86_RSP_ONLY, T1_X86_RSP_ONLY,            \
      T1_XF_NONE, T1_XF_NONE, T1_XF_NONE, T1_XA_NONE)                                              \
    X(POP64_R, "pop64", 1, T1_XO_R64_W, T1_XO_NONE, T1_X86_RSP_ONLY, T1_X86_RSP_ONLY, T1_XF_NONE,  \
      T1_XF_NONE, T1_XF_NONE, T1_XA_NONE)                                                          \
    X(POPFQ, "popfq", 0, T1_XO_NONE, T1_XO_NONE, T1_X86_RSP_ONLY, T1_X86_RSP_ONLY, T1_XF_NONE,     \
      T1_XF_ALL, T1_XF_NONE, T1_XA_NONE)                                                           \
    X(MFENCE, "mfence", 0, T1_XO_NONE, T1_XO_NONE, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,       \
      T1_XA_NONE)                                                                                  \
    X(SHLX64, "shlx64", 2, T1_XO_R64_RW, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,    \
      T1_XA_NONE)                                                                                  \
    X(SHRX64, "shrx64", 2, T1_XO_R64_RW, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,    \
      T1_XA_NONE)                                                                                  \
    X(SARX64, "sarx64", 2, T1_XO_R64_RW, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,    \
      T1_XA_NONE)                                                                                  \
    X(ANDN64, "andn64", 2, T1_XO_R64_RW, T1_XO_R64_R, 0, 0, T1_XF_NONE,                            \
      T1_XF_SF | T1_XF_ZF | T1_XF_CF | T1_XF_OF, T1_XF_PF | T1_XF_AF, T1_XA_NONE)                  \
    X(BEXTR64, "bextr64", 2, T1_XO_R64_RW, T1_XO_R64_R, 0, 0, T1_XF_NONE,                          \
      T1_XF_ZF | T1_XF_CF | T1_XF_OF, T1_XF_SF | T1_XF_PF | T1_XF_AF, T1_XA_NONE)                  \
    X(BLSI64, "blsi64", 2, T1_XO_R64_W, T1_XO_R64_R, 0, 0, T1_XF_NONE,                             \
      T1_XF_SF | T1_XF_ZF | T1_XF_CF | T1_XF_OF, T1_XF_PF | T1_XF_AF, T1_XA_NONE)                  \
    X(BLSMSK64, "blsmsk64", 2, T1_XO_R64_W, T1_XO_R64_R, 0, 0, T1_XF_NONE,                         \
      T1_XF_SF | T1_XF_ZF | T1_XF_CF | T1_XF_OF, T1_XF_PF | T1_XF_AF, T1_XA_NONE)                  \
    X(BLSR64, "blsr64", 2, T1_XO_R64_W, T1_XO_R64_R, 0, 0, T1_XF_NONE,                             \
      T1_XF_SF | T1_XF_ZF | T1_XF_CF | T1_XF_OF, T1_XF_PF | T1_XF_AF, T1_XA_NONE)                  \
    X(LZCNT64, "lzcnt64", 2, T1_XO_R64_W, T1_XO_R64_R, 0, 0, T1_XF_NONE, T1_XF_CF | T1_XF_ZF,      \
      T1_XF_OF | T1_XF_SF | T1_XF_PF | T1_XF_AF, T1_XA_NONE)                                       \
    X(RORX64, "rorx64", 2, T1_XO_R64_RW, T1_XO_I8, 0, 0, T1_XF_NONE, T1_XF_NONE, T1_XF_NONE,       \
      T1_XA_NONE)                                                                                  \
    X(CALL_REL, "call", 1, T1_XO_REL, T1_XO_NONE, T1_X86_RSP_ONLY, T1_X86_CALLER_SAVED,            \
      T1_XF_NONE, T1_XF_NONE, T1_XF_ALL, T1_XA_CALL | T1_XA_BRANCH)                                \
    X(LOCK_INC64_M, "inc64", 1, T1_XO_M64_RW, T1_XO_NONE, 0, 0, T1_XF_NONE, T1_XF_OSZP, T1_XF_AF,  \
      T1_XA_NONE)

/* The optional three-operand forms expose a tied destination/source so
 * the existing two-operand MIR contract describes all reads and writes. */

/* Op ids: table positions; an XCC family occupies 16 consecutive ids. */
enum t1_x86_op {
#define T1_X86_ENUM_X(id, name, n, o0, o1, iu, id_, fr, fd, fu, at) T1_X86_##id,
#define T1_X86_ENUM_XCC(id, name, n, o0, o1, iu, id_, fd, fu, at)                                  \
    T1_X86_##id, T1_X86_##id##_LAST = T1_X86_##id + T1_CC_COUNT - 1,
    T1_X86_OPS(T1_X86_ENUM_X, T1_X86_ENUM_XCC)
#undef T1_X86_ENUM_X
#undef T1_X86_ENUM_XCC
        T1_X86_OP_COUNT
};

struct t1_x86_info {
    const char *name; /* expanded mnemonic, e.g. "jl" */
    uint8_t nopnd;
    uint8_t opnd[2];                     /* enum t1_x86_opnd_spec */
    uint8_t cc;                          /* enum t1_x86_cc, or T1_CC_COUNT if none */
    uint16_t implicit_use, implicit_def; /* T1_X86_REGBIT masks */
    uint16_t flags_read, flags_def, flags_undef;
    uint8_t attr; /* T1_XA_* */
};
/* NULL for an invalid op id. */
const struct t1_x86_info *t1_x86_info(unsigned op);
/* Required scalar capability; zero for every baseline op. */
unsigned t1_x86_required_isa(unsigned op);
/* Shared mapping for the runtime query and constant generated rule masks.
 * The constant-expression form is for side-effect-free opcode expressions. */
#define T1_X86_SCALAR_ISA(X, op)                                                                   \
    X(op, SHLX64, HOST_ISA_BMI2)                                                                   \
    X(op, SHRX64, HOST_ISA_BMI2)                                                                   \
    X(op, SARX64, HOST_ISA_BMI2)                                                                   \
    X(op, RORX64, HOST_ISA_BMI2)                                                                   \
    X(op, ANDN64, HOST_ISA_BMI1)                                                                   \
    X(op, BEXTR64, HOST_ISA_BMI1)                                                                  \
    X(op, BLSI64, HOST_ISA_BMI1)                                                                   \
    X(op, BLSMSK64, HOST_ISA_BMI1)                                                                 \
    X(op, BLSR64, HOST_ISA_BMI1)                                                                   \
    X(op, LZCNT64, HOST_ISA_LZCNT)
#define T1_X86_ISA_TEST(op, id, isa) | ((op) == T1_X86_##id ? (isa) : 0u)
#define T1_X86_REQUIRED_ISA(op) (0u T1_X86_SCALAR_ISA(T1_X86_ISA_TEST, op))

/* base/index are enum t1_x86_reg or T1_X86_NOREG; scale_shift 0..3. */
struct t1_x86_mem {
    uint8_t base, index, scale_shift;
    int32_t disp;
};
/* One concrete instruction: registers already allocated. Register operands
 * hold enum t1_x86_reg in reg (CL operands hold T1_RCX), immediates in imm,
 * REL operands a label id in imm, M/MA operands use mem. */
struct t1_x86_operand {
    uint8_t reg;
    int64_t imm;
};
struct t1_x86_insn {
    uint16_t op;
    struct t1_x86_operand opnd[2];
    struct t1_x86_mem mem;
};
struct t1_x86_buf {
    uint8_t *data;
    size_t size, capacity;
    bool failed;
};

/* Appends the encoding; REL operands are emitted as rel32 0 and their
 * position is returned in *fixup (SIZE_MAX if none). Out-of-range operands
 * or capacity set b->failed. */
void t1_x86_emit(struct t1_x86_buf *b, const struct t1_x86_insn *i, size_t *fixup);
void t1_x86_patch_rel32(struct t1_x86_buf *b, size_t fixup, size_t target);

/* Length decoder for the static checks and for rule_runner: accepts
 * exactly the encodings t1_x86_emit produces and fails on anything else. */
struct t1_x86_decoded {
    uint16_t op; /* enum t1_x86_op */
    uint8_t length;
    bool has_mem;
    struct t1_x86_mem mem;
};
bool t1_x86_decode(const uint8_t *code, size_t avail, struct t1_x86_decoded *out);
#endif
