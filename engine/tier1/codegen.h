/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Private MIR and code-generation passes.
 * Blocks use final linear order, with contiguous loop bodies and separate
 * BRANCH edge blocks. Each block's terminator sequence begins at term; no
 * moves may be inserted inside it, so every branch sees the block-end state.
 *
 * Before allocation, VREG.fixed is a required register or NOREG. Afterwards
 * it is the assigned register; memory-capable operands use NOREG and imm as
 * the spill slot. Point frames acquire physical locations during allocation;
 * a site's FORCE must see the same locations as the site (raverify checks).
 *
 * HELPER calls materialize the read set from the point frame (read set ⊆ W,
 * never HOME), execute the immutable instruction and reload result registers.
 * The stub preserves other allocatable GPRs, clobbers R11/flags, and retains
 * the before-call capture on an exceptional exit without defining results.
 */
#ifndef IA64VMM_TIER1_CODEGEN_H
#define IA64VMM_TIER1_CODEGEN_H
#include "engine/tier1/ir.h"
#include "engine/tier1/mir.h"
#include "engine/tier1/exec.h"

#define T1_MIR_MAX_BLOCKS 256u
#define T1_MIR_MAX_INSNS 16384u
#define T1_MIR_MAX_VREGS 8192u
#define T1_MIR_MAX_COPIES 8192u
#define T1_MIR_NONE UINT32_MAX

/* The spill slot reserved for breaking parallel-copy cycles (emit.c); the
 * allocator assigns slots 0 .. T1_SPILL_SLOTS - 2. */
#define T1_SPILL_SCRATCH (T1_SPILL_SLOTS - 1u)

/* struct t1_mir_vreg.flags */
#define T1_VR_PARAM 1u      /* block parameter (defined by the PCOPYs of its edges) */
#define T1_VR_LOOP_PARAM 2u /* parameter of a loop header (loop-carried) */
#define T1_VR_REAL_USE 4u   /* has a use that needs it in a register inside a loop */
#define T1_VR_COPY 8u       /* tie or fixed-register copy made by isel */

struct t1_mir_vreg {
    uint8_t type;  /* enum t1_type of the value */
    uint8_t fixed; /* precolored register (fixed rule operand, pin), else NOREG */
    uint8_t flags;
    uint16_t element;         /* PARAM: the element it carries (0 if none) */
    t1_value value;           /* the IR value it holds, or T1_VALUE_NONE (temps, copies) */
    t1_vreg hint;             /* a copy partner: preferred same register */
    struct t1_loc header_loc; /* after allocation, loop parameters: location at their header */
};

/* One parallel-copy element (PCOPY, and the allocator's resolution moves).
 * Before allocation: dst/src vregs (src_imm when src == T1_VREG_NONE).
 * After allocation: dloc/sloc are the locations (T1_LOC_REG, T1_LOC_SPILL
 * or, for sources, T1_LOC_CONST). */
struct t1_mir_copy {
    t1_vreg dst, src;
    uint64_t src_imm;
    struct t1_loc dloc, sloc;
};

enum t1_mir_frame_kind { T1_MF_VREG, T1_MF_CONST, T1_MF_HOME };
struct t1_mir_frame {
    uint16_t element;
    uint8_t kind; /* enum t1_mir_frame_kind */
    t1_vreg vreg;
    uint64_t value; /* CONST */
};

struct t1_mir_block {
    uint32_t first, count; /* instruction range */
    uint32_t term;         /* first instruction of the terminator sequence */
    uint16_t ir_block;     /* T1_NO_BLOCK for an edge block */
    uint8_t nsucc;
    bool loop;            /* part of a loop body */
    bool back_edge;       /* ends in a back-edge to its loop header */
    uint8_t depth;        /* loops holding it (an edge block: holding both ends) */
    uint16_t loop_header; /* IR block of its innermost loop's header, T1_NO_BLOCK if none */
    uint16_t succ[2];
};

struct t1_mir_point {
    uint32_t insn;       /* the instruction carrying the point, T1_MIR_NONE if dead */
    uint32_t force_insn; /* its FORCE instruction (verification), else T1_MIR_NONE */
    uint32_t frame_first, frame_count;
    uint8_t kind; /* enum t1_point_kind */
};

struct t1_mir {
    const struct t1_ir *ir;
    uint32_t flags;       /* T1_COMPILE_* */
    unsigned window_bits; /* hostwin_bits: the tag shift of every site */
    uint16_t pin_element; /* test-only pinning (t1_compile_options) */
    uint8_t pin_reg;
    uint16_t allocatable; /* register pool (T1_ALLOCATABLE; tests may shrink it) */
    unsigned nblocks, ninsns, nvregs, ncopies, nframe, insn_cap, copy_cap, frame_cap;
    unsigned header; /* MIR block of the head (IR block 1) */
    struct t1_mir_block block[T1_MIR_MAX_BLOCKS];
    struct t1_mir_insn *insn;
    /* After allocation: the registers of each instruction's memory operand
     * base and index (the vregs stay in insn[].mem for raverify). */
    struct t1_mir_memreg {
        uint8_t base, index;
    } *memreg;
    struct t1_mir_vreg *vreg;
    struct t1_mir_copy *copy;
    struct t1_mir_frame *frame;
    struct t1_loc *frame_loc;   /* after allocation, parallel to frame[] */
    struct t1_mir_point *point; /* [ir->npoints] */
    /* Allocation results (regalloc.c). */
    unsigned spill_slots; /* slots used, <= T1_SPILL_SCRATCH */
    unsigned loop_value_spills, restore_only_spills, total_spills;
    /* Failure: reason, and the source instruction a region boundary should
     * be placed before (isel: no rule for an op), else T1_NO_INSN. */
    bool failed;
    bool capacity_failure; /* a fixed capacity ran out (spill slots): a shorter region may fit */
    const char *fail_reason;
    uint16_t fail_insn;
};

/* mir.c */
struct t1_mir *t1_mir_create(const struct t1_ir *ir);
void t1_mir_destroy(struct t1_mir *m);
bool t1_mir_fail(struct t1_mir *m, const char *reason);
/* Appends to the instruction array (the caller keeps block ranges). */
struct t1_mir_insn *t1_mir_append(struct t1_mir *m, uint16_t op);
t1_vreg t1_mir_new_vreg(struct t1_mir *m, uint8_t type, t1_value value);
uint32_t t1_mir_new_copies(struct t1_mir *m, unsigned n);
static inline bool t1_mir_is_pseudo(unsigned op)
{
    return op > T1_MIR_PSEUDO_SENTINEL && op < T1_MIR_OP_COUNT;
}
/* Printing for diagnostics (T1_DUMP-style tests). */
void t1_mir_print(const struct t1_mir *m, bool allocated, t1_ir_out_fn out, void *arg);

/* fma, fms, fnma, their pseudo-ops and fpma, fpms, fpnma: helpers whose
 * expansion first tries the host FP path (emit.c; HELPER opnd[3] = 1). */
static inline bool t1_fp_fast_opcode(Ia64Opcode op)
{
    return op == IA64_OP_FMA || op == IA64_OP_FMS || op == IA64_OP_FNMA || op == IA64_OP_FADD ||
           op == IA64_OP_FSUB || op == IA64_OP_FMPY || op == IA64_OP_FNORM || op == IA64_OP_FPMA ||
           op == IA64_OP_FPMS || op == IA64_OP_FPNMA;
}

/* isel.c: optimized IR -> MIR on virtual registers. */
bool t1_isel(struct t1_mir *m, uint32_t flags);

/* regalloc.c: linear scan with interval splitting; inserts moves,
 * spills and reloads, fills every location. t1_mir_effects is in mir.h. */
bool t1_regalloc(struct t1_mir *m);

/* raverify.c: forward dataflow over the allocated MIR. */
bool t1_raverify(const struct t1_mir *m, char *err, size_t err_size);

/* Element homes in struct vcpu (LIVEIN loads of static elements): byte
 * offset in struct vcpu, bit (predicates; else 0) and size in bytes (8, or
 * 1 for a NaT bool). False for an id that is not an element and for a
 * stacked GR/NaT (its home moves with the frame base, so
 * generated code never addresses it; LIVEINs read the entry image). frame.c. */
bool t1_element_home(uint16_t element, uint32_t *offset, unsigned *bit, unsigned *bytes);
/* Stacked GR (32..127) or NaT (NAT(32)..NAT(127)) element. */
static inline bool t1_element_stacked(uint16_t e)
{
    return (e >= T1_ELEMENT_GR(32) && e < T1_ELEMENT_NAT(0)) ||
           (e >= T1_ELEMENT_NAT(32) && e < T1_ELEMENT_PR(0));
}

/* ---- emission (emit.c) ---- */

/* Runtime stub addresses (stubs.c), in the first page of the arena. */
struct t1_stub_addrs {
    uintptr_t exit_save;   /* save the allocatable registers, then exit_nosave */
    uintptr_t exit_nosave; /* raw R13; optional native handoff, else shared epilogue */
    uintptr_t miss;        /* called: save, t1_window_miss, restore or exit */
    uintptr_t helper;      /* called: save, t1_helper_call, restore or exit */
    uintptr_t trap_entry;  /* the trap client's resume target */
};
extern struct t1_stub_addrs t1_stub;

/* Byte offsets of each point in the emitted code. host_offset: the access
 * instruction of a site; force_offset/force_resume: the forced trap's
 * faulting load and its resume target (the site's tag check). */
struct t1_emit_point {
    uint32_t host_offset, force_offset, force_resume, pre_offset;
    uint32_t fast_offset; /* optional cloned access, same allocation and point */
    uint32_t restore_offset;
    uint8_t addr_reg;
};

struct t1_emitted {
    struct t1_workspace *ws; /* the MIR's IR workspace, which holds buf and pt */
    uint8_t *buf;
    size_t size, cap, pages;
    struct t1_emit_point *pt; /* [ir->npoints] */
    size_t pt_pages;
    uint32_t loop_start, loop_end;
    uint32_t native_entry;    /* body after the SysV outer-entry prologue */
    uint32_t frame_in_offset; /* optional C-ABI entry-image leaf in the code */
    unsigned loop_host_insns;
    unsigned site_base; /* first x->site[] slot of the sites (t1_site_base) */
    /* The stacked LIVEIN elements the code reads from
     * [r14 + X_FRAME_IN(k)], k = index here. */
    uint16_t frame_in[T1_MAX_FRAME_IN];
    unsigned nframe_in;
    /* rel32 fields that reach the stubs: patched when the code is placed. */
    uint32_t abs_fix[1024];
    uintptr_t abs_target[1024];
    unsigned nabs;
};
bool t1_emit(const struct t1_mir *m, struct t1_emitted *out, char *err, size_t err_size);
/* Copies the code to its final address and patches the stub branches. */
void t1_emit_place(const struct t1_emitted *e, uint8_t *dst);
void t1_emit_free(struct t1_emitted *e);
/* The emitter's state while t1_emit runs (part of the compile workspace). */
size_t t1_emit_state_bytes(void);

/* The first x->site[] slot for a region's nsites sites (trap.c): slot 0
 * for verification regions, whose sites the dispatcher resets at every
 * entry; otherwise the next free range of a round-robin over
 * T1_SITE_SLOTS. */
unsigned t1_site_base(unsigned nsites, uint32_t flags);

/* Runtime entry points called by the stubs (C, normal context). */
uint32_t t1_helper_call(struct t1_exec *x);
#endif
