/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64_IA32_DECODE_H
#define IA64_IA32_DECODE_H
#include "ia64vmm/ia32_state.h"

enum ia32_decode_status {
    IA32_DECODE_OK,
    IA32_DECODE_INCOMPLETE,
    IA32_DECODE_ILLEGAL,
    IA32_DECODE_UNSUPPORTED,
};
enum ia32_instruction_kind {
    IA32_INSN_NATIVE,
    IA32_INSN_JMPE,
    IA32_INSN_INTERCEPT,
    IA32_INSN_INT,
    IA32_INSN_INT3,
    IA32_INSN_INTO,
    IA32_INSN_SYSTEM_FLAG,
    IA32_INSN_CONTROL_READ,
    IA32_INSN_CPUID,
    IA32_INSN_CLOCK,
    IA32_INSN_IO,
    IA32_INSN_SEGMENT,
    IA32_INSN_FAR,
};
#define IA32_INSN_MEMORY (1u << 0)
#define IA32_INSN_FP (1u << 1)
#define IA32_INSN_MMX (1u << 2)
#define IA32_INSN_SSE (1u << 3)
#define IA32_INSN_WRITE_FLAGS (1u << 4)
#define IA32_INSN_WRITE_SEGMENT (1u << 5)
#define IA32_INSN_REP (1u << 6)
#define IA32_INSN_LOCK (1u << 7)
#define IA32_INSN_STRING (1u << 8)
#define IA32_INSN_BRANCH (1u << 9)
#define IA32_INSN_STACK (1u << 10)
#define IA32_INSN_RMW (1u << 11)

struct ia32_decoded {
    enum ia32_instruction_kind kind;
    unsigned flags;
    uint32_t native_features; /* Required IA32_NATIVE_* host capabilities. */
    uint8_t length, prefix_length, operand_bits, address_bits;
    uint16_t opcode;         /* 0x0f00 for a two-byte opcode; prefixes excluded. */
    uint16_t intercept_code; /* Vol.2 Table 9-1: effective sizes and stripped prefixes. */
    uint64_t iim;            /* Up to eight instruction bytes after stripped prefixes. */
    bool has_modrm, has_sib, operand_memory, overlong;
    uint8_t modrm, sib, segment, vector, access_width;
    enum ia32_access_kind access_kind; /* UNKNOWN/width zero requires policy qualification. */
    uint32_t offset;                   /* Effective memory offset before segmentation. */
    uint32_t operand; /* JMPE immediate or register value; memory contents are never read. */
};

/* Original bounded legacy IA-32 policy decoder for the Itanium System
 * Environment. OK permits inspection of all fields, not automatic native
 * execution: kind, flags and native_features still require policy. ILLEGAL
 * is an architectural instruction intercept; UNSUPPORTED means exact stop
 * because this helper cannot safely classify the encoding. Neither permits
 * host execution.
 * INCOMPLETE requests more bytes, never more than 15. ILLEGAL reports the
 * consumed opcode bytes and prefix summary; overlong selects #GP(0).
 * UNSUPPORTED cannot safely supply an architectural IIM record.
 * Operand/address defaults come from CS.D; repeated size prefixes toggle
 * the default once. The last segment/repeat prefix wins. No memory access,
 * privilege check, fault ordering, or architectural state mutation occurs. */
enum ia32_decode_status ia32_decode(const uint8_t *, size_t, const struct ia32_native_state *,
                                    struct ia32_decoded *);
#endif
