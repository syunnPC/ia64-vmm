/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Machine IR: AMD64 instructions on virtual registers with explicit use,
 * def, clobber, tie and fixed constraints.
 * Produced by isel (rule templates, rules.h) and by the lowering of
 * points and stubs; consumed by the linear-scan allocator, the
 * post-allocation verifier (raverify) and the emitter.
 *
 * Everything that executes inside a region body is MIR before
 * allocation -- checkpoints, exit and miss stubs, forced-fault hooks,
 * helper calls, store-log sequences, parallel copies. Their fixed
 * expansions use only the reserved R11, RFLAGS, the pinned R12/R13/R14 and
 * memory relative to R12/R14; the pseudo-op table below states, for
 * each, what the allocator must assume. Only the prologue (before any
 * value is defined) and the shared epilogue and runtime stubs (after the
 * frame has been saved to struct t1_exec) are outside the allocator's
 * view.
 */
#ifndef IA64VMM_TIER1_MIR_H
#define IA64VMM_TIER1_MIR_H
#include "engine/tier1/x86enc.h"
#include "engine/tier1/rules.h"

/* Host register usage. */
#define T1_REG_VCPU T1_R12    /* struct vcpu *, pinned */
#define T1_REG_BUDGET T1_R13  /* raw budget counter, pinned; block charges */
#define T1_REG_EXEC T1_R14    /* struct t1_exec *, pinned */
#define T1_REG_SCRATCH T1_R11 /* reserved scratch: never allocated, never in a frame */
#define T1_ALLOC_CALLER_SAVED                                                                      \
    (T1_X86_REGBIT(T1_RAX) | T1_X86_REGBIT(T1_RCX) | T1_X86_REGBIT(T1_RDX) |                       \
     T1_X86_REGBIT(T1_RSI) | T1_X86_REGBIT(T1_RDI) | T1_X86_REGBIT(T1_R8) | T1_X86_REGBIT(T1_R9) | \
     T1_X86_REGBIT(T1_R10))
#define T1_ALLOC_CALLEE_SAVED                                                                      \
    (T1_X86_REGBIT(T1_RBX) | T1_X86_REGBIT(T1_RBP) | T1_X86_REGBIT(T1_R15))
/* The 11 allocatable registers; the exit/checkpoint/miss stubs save
 * exactly these to x->regs. */
#define T1_ALLOCATABLE (T1_ALLOC_CALLER_SAVED | T1_ALLOC_CALLEE_SAVED)
_Static_assert((T1_ALLOCATABLE &
                (T1_X86_REGBIT(T1_RSP) | T1_X86_REGBIT(T1_R11) | T1_X86_REGBIT(T1_R12) |
                 T1_X86_REGBIT(T1_R13) | T1_X86_REGBIT(T1_R14))) == 0,
               "pinned and reserved registers");

typedef uint32_t t1_vreg;
#define T1_VREG_NONE UINT32_MAX

/*
 * Operands. VREG: an allocatable virtual register; fixed = the physical
 * register it must occupy at this instruction (CL operands, pinning) or
 * T1_X86_NOREG. PREG: one of the pinned/reserved registers by number
 * (R11..R14), never allocated. IMM: imm. LABEL: imm = block id. MEM: the
 * instruction's memory operand (struct t1_mir_mem).
 */
enum t1_mir_opnd_kind { T1_MO_NONE, T1_MO_VREG, T1_MO_PREG, T1_MO_IMM, T1_MO_LABEL, T1_MO_MEM };
struct t1_mir_opnd {
    uint8_t kind, fixed;
    t1_vreg vreg;
    int64_t imm;
};
/* base/index: a VREG or a PREG (kind fields as above); index kind NONE if
 * absent. */
struct t1_mir_mem {
    uint8_t base_kind, index_kind, scale_shift;
    t1_vreg base, index;
    int32_t disp;
};

/*
 * Pseudo-ops for points and stubs. Columns:
 *   uses/defs          the virtual registers named by the operands, as
 *                      described per row (all VREG uses are read before
 *                      any def of the same instruction)
 *   clobber_regs       physical registers destroyed (never hold a value
 *                      live across the instruction)
 *   clobber_flags      RFLAGS bits destroyed
 *   point              whether the instruction carries a point: its frame
 *                      entries are pseudo-uses at the instruction;
 *                      a flag value is never live across it
 *
 * X(NAME, clobber_regs, clobber_flags, has_point, description)
 */
#define T1_R11_ONLY T1_X86_REGBIT(T1_R11)
/* Shared helper/miss wrapper ABI: all allocatable registers survive,
 * except explicit normal-completion results. R11 and flags do not. */
#define T1_WRAPPER_PRESERVED T1_ALLOCATABLE
#define T1_WRAPPER_CLOBBERS T1_R11_ONLY
#define T1_MIR_PSEUDO_OPS(X)                                                                            \
    /* Block start; operand 0 = LABEL. */                                                               \
    X(BLOCK, 0, T1_XF_NONE, false, "block label")                                                       \
    /* Defines opnd 0 (VREG) from the element's home (LIVEIN), imm = element. */                        \
    X(HREAD, 0, T1_XF_NONE, false, "helper result")                                                     \
    X(BSPCALL, 0, T1_XF_NONE, false, "taken call BSP history")                                          \
    X(HGUARD, T1_R11_ONLY, T1_XF_ALL, true, "helper completion exit")                                   \
    X(LIVEIN, 0, T1_XF_NONE, false, "mov vreg, [r12 + home]")                                           \
    /* One edge's parallel copy: opnd pairs (dst VREG, src VREG) in copies[]; cycles go through R11. */ \
    X(PCOPY, T1_R11_ONLY, T1_XF_NONE, false, "parallel copy (R11 breaks cycles)")                       \
    /* Spill slot k (imm): opnd 0 VREG use / def. R14-relative, never the stack. */                     \
    X(SPILL, 0, T1_XF_NONE, false, "mov [r14 + spill + 8k], vreg")                                      \
    X(RELOAD, 0, T1_XF_NONE, false, "mov vreg, [r14 + spill + 8k]")                                     \
    /* Block charge: imm = charge. */                                                                   \
    X(CHARGE, 0, T1_XF_ALL, false, "sub r13, charge")                                                   \
    /* Back-edge checkpoint: imm = max_path; budget and event tests; taken path        \
     * saves the allocatable registers and exits with the point (no return). */                \
    X(CHECKPOINT, T1_R11_ONLY, T1_XF_ALL, true,                                                         \
      "cmp r13,max ; jb ; mov r11d,[r12+requested_exit] ; or r11d,[r14+kick] ; jnz")                    \
    /* Region exit (terminator): saves the allocatable registers, x->point = k, jmp common_exit. */     \
    X(EXIT, 0, T1_XF_NONE, true, "exit stub")                                                           \
    /* Memory site k: opnd 0 = data VREG (LOAD: def, zero-extended; STORE: use), opnd 1 =     \
     * address VREG (use), imm = enum t1_site_flags | bytes << 8 | site << 16. Tag check, alignment   \
     * test (unless T1_SITE_ALIGN_PROVEN), delta load, the access. The miss path saves and restores    \
     * all allocatable registers around t1_window_miss, and the host-fault path restores every GPR   \
     * and RFLAGS, so the site destroys only R11 and the flags. */         \
    X(SITE, T1_R11_ONLY, T1_XF_ALL, true, "tag check ; [align test] ; access")                          \
    /* Verification force counter of point k: dec + jz into the force path;    \
     * the force path (poison load or tag invalidation) uses only R11. */                        \
    X(FORCE, T1_R11_ONLY, T1_XF_ALL, true, "dec qword [r14+force[k]] ; jz force_k")                     \
    /* Completed native access: opnd 0 = address VREG, opnd 1 = value VREG, imm = point. */             \
    X(SLOG, T1_R11_ONLY, T1_XF_ALL, false, "store-log append")                                          \
    /* In-region exact helper: VREG uses = the read set written to homes; VREG defs = the     \
     * write set reloaded from homes after IA64_STEP_COMPLETE; any other result exits at the point.  \
     * The wrapper preserves all other allocatable GPRs; exceptional completion retains the       \
     * call-before capture, before result definitions. R11 and flags are destroyed. */         \
    X(HELPER, T1_WRAPPER_CLOBBERS, T1_XF_ALL, true, "call shared helper wrapper")                       \
    /* Memory fence for FENCE. */                                                                       \
    X(FENCE, 0, T1_XF_NONE, false, "mfence")

/* MIR op ids: x86 ops (enum t1_x86_op) first, pseudo-ops after them. */
enum t1_mir_pseudo {
    T1_MIR_PSEUDO_BASE = T1_X86_OP_COUNT,
#define T1_MIR_ENUM(name, regs, flags, point, text) T1_MIR_##name,
    T1_MIR_PSEUDO_SENTINEL = T1_MIR_PSEUDO_BASE - 1,
    T1_MIR_PSEUDO_OPS(T1_MIR_ENUM)
#undef T1_MIR_ENUM
        T1_MIR_OP_COUNT
};

/*
 * Alignment provenance: a SITE omits its alignment test only when
 * the IR known-bits analysis proved the address's low log2(bytes) bits
 * zero. Known bits start fully unknown at every LIVEIN and are the meet
 * over all incoming values at block parameters; nothing about live-in
 * values comes from the entry contract or from observed values (no
 * value specialization).
 */
enum t1_site_flags {
    T1_SITE_LOAD = 0,
    T1_SITE_STORE = 1u << 0,
    T1_SITE_ALIGN_PROVEN = 1u << 1, /* known-bits proof; test omitted */
    T1_SITE_STORE_LOG = 1u << 2,    /* T1_COMPILE_STORE_LOG: SLOG follows */
};

#define T1_MIR_MAX_OPNDS 4
struct t1_mir_insn {
    uint16_t op; /* enum t1_x86_op or enum t1_mir_pseudo */
    uint8_t nopnd;
    /* Two-address constraint: operand tie_def is written into the register
     * of operand tie_use (rule `tie`, and every x86 RW operand); 0xff = none. */
    uint8_t tie_def, tie_use;
    uint8_t cc; /* XCC family member, else T1_CC_COUNT */
    struct t1_mir_opnd opnd[T1_MIR_MAX_OPNDS];
    struct t1_mir_mem mem;
    uint32_t point;                 /* point id, or UINT32_MAX */
    uint32_t copies_first, ncopies; /* PCOPY: range of the block's copy list */
};

/*
 * Effective constraints of one instruction, the single answer for regalloc
 * and raverify. For x86 ops they derive from T1_X86_OPS: operand access
 * (R: use; W: def; RW: use and def tied to it), 32-bit writes define the
 * whole register, 8/16-bit writes define it while preserving the other
 * bits, implicit registers, flags read/def/undef (def and undef are both
 * clobbers unless a later instruction of the same rule or stub consumes
 * them); for pseudo-ops they are the table above.
 */
struct t1_mir_effects {
    uint8_t nuse, ndef;
    uint8_t use[T1_MIR_MAX_OPNDS + 2],
        def[T1_MIR_MAX_OPNDS];       /* operand indices; mem base/index as 0x80/0x81 */
    uint8_t fixed[T1_MIR_MAX_OPNDS]; /* per operand: required register or T1_X86_NOREG */
    uint16_t clobber_regs;           /* physical, beyond the defs */
    uint16_t flags_read, flags_clobber;
    bool point, terminator, call;
};
/* Implemented once (regalloc.c); raverify.c and the emitter use the same. */
void t1_mir_effects(const struct t1_mir_insn *insn, struct t1_mir_effects *out);
#endif
