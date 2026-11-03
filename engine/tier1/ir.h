/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tier1 SSA IR and pass interfaces; formation and lifting are in form.h.
 * Values are (op, result), with ops in one array and each block's ops linked:
 * PARAMs first, one terminator last. Edges carry arguments in parameter order.
 * BACKEDGE edges target dominating loop headers; other edges form a DAG. Block 0
 * holds deduplicated LIVEINs and CONSTs and jumps to head block 1.
 *
 * Uses, edge arguments, HELPER reads and frames share a t1_use pool; element
 * identifies architectural entries. Forwarded uses must be resolved after each
 * pass; verification rejects references to forwarded or dead ops.
 *
 * HELPER operands: MEM, qp, source2 GR/NaT (tnat has no source1), p1, p2;
 * omit r0/p0. imm is the source instruction index; wr[] names result predicates.
 *
 * Every point captures all elements in frame_set (possible writes plus W).
 * T1_USE_HOME means LIVEIN(element) is still in its home. The frame rule may
 * use HOME within W only with a per-point proof, independently verified.
 */
#ifndef IA64VMM_TIER1_IR_H
#define IA64VMM_TIER1_IR_H
#include "engine/tier1/rules.h"
#include "engine/tier1/tier1.h"

/* Capacities of one region's IR (limits bound the inputs). */
#define T1_IR_MAX_OPS 16384u
#define T1_IR_MAX_USES (1u << 17)
/* Hash buckets of the entry block's CONST and LIVEIN ops. */
#define T1_ENTRY_SLOTS 4096u
#define T1_N_ELEMENTS T1_ELEMENT_LIMIT /* includes FPI significand/metadata homes */
#define T1_ELEMENT_WORDS ((T1_N_ELEMENTS + 63u) / 64u)

#define T1_NO_OP UINT32_MAX
#define T1_NO_BLOCK 0xffffu
#define T1_NO_INSN 0xffffu
#define T1_NO_POINT UINT32_MAX
#define T1_ENTRY_BLOCK 0u
#define T1_HEADER_BLOCK 1u

/* ir_ops.def effect classes. */
enum t1_effect {
    T1_EFF_PURE,
    T1_EFF_PSEUDO,
    T1_EFF_LOAD,
    T1_EFF_STORE,
    T1_EFF_ORDER,
    T1_EFF_HELPER,
    T1_EFF_TERM,
    T1_EFF_PARAM
};
/* ir_ops.def immediate layouts. */
enum t1_imm_layout {
    T1_IMM_NONE,
    T1_IMM_VALUE,
    T1_IMM_ELEMENT,
    T1_IMM_COUNT,
    T1_IMM_FIELD,
    T1_IMM_WIDTH,
    T1_IMM_CMP,
    T1_IMM_ACCESS,
    T1_IMM_HELPER,
    T1_IMM_POINT
};
/* Operand/result types of ir_ops.def beyond enum t1_type. */
#define T1_TYPE_T 3u   /* the op's type parameter (struct t1_op.type) */
#define T1_TYPE_VAR 4u /* variable-length operand tail */

struct t1_op_desc {
    const char *name;
    uint8_t imm, effect, nopnd, nres;
    uint8_t opnd_type[4], res_type[4];
    const char *res_name[4];
};
const struct t1_op_desc *t1_op_desc(unsigned code);

/* Packed immediates (ir_ops.def). */
#define T1_FIELD_IMM(pos, len) ((int64_t)(pos) | ((int64_t)(len) << 8))
#define T1_FIELD_POS(imm) ((unsigned)((imm) & 0xff))
#define T1_FIELD_LEN(imm) ((unsigned)(((imm) >> 8) & 0xff))
#define T1_CMP_REL(imm) ((unsigned)((imm) & 0xff))
#define T1_CMP_WIDTH(imm) ((unsigned)(((imm) >> 8) & 0xff))
#define T1_ACCESS_IMM(bytes, ordered, site)                                                        \
    ((int64_t)(bytes) | ((int64_t)((ordered) ? 1 : 0) << 8) | ((int64_t)(site) << 16))
#define T1_ACCESS_BYTES(imm) ((unsigned)((imm) & 0xff))
#define T1_ACCESS_ORDERED(imm) ((((imm) >> 8) & 1) != 0)
#define T1_ACCESS_SITE(imm) ((unsigned)(((imm) >> 16) & 0xffff))

/* struct t1_use.flags */
#define T1_USE_HOME 1u /* frame entry: home holds LIVEIN(element); value NONE */

struct t1_use {
    t1_value value;
    uint16_t element;
    uint16_t flags;
};

/* struct t1_op.flags */
#define T1_OPF_DEAD 1u
#define T1_OPF_MARK 0x80u /* pass-local scratch (DCE); clear between passes */

struct t1_op {
    uint16_t code; /* enum t1_ir_op */
    uint8_t type;  /* result type of T-typed ops (enum t1_type) */
    uint8_t flags;
    uint16_t block;
    uint16_t insn; /* source instruction index (form.h), T1_NO_INSN if none */
    int64_t imm;
    uint32_t prev, next; /* block order */
    uint32_t use_first;  /* operands: use[use_first .. use_first + nuse) */
    uint16_t nuse, use_cap;
    uint16_t wr[2];      /* HELPER: elements of results p1, p2 */
    uint32_t point;      /* point carried by this op, or T1_NO_POINT */
    t1_value fwd;        /* forwarding target of result 0, or T1_VALUE_NONE */
    uint32_t entry_next; /* next CONST/LIVEIN in its hash bucket, or T1_NO_OP */
};

struct t1_edge {
    uint16_t target;
    uint16_t nargs, cap;
    uint32_t args_first;
};

/* struct t1_block.flags */
#define T1_BLK_DEAD 1u
#define T1_BLK_SYNTHETIC 2u /* created for a predicated memory op or fence */

struct t1_block {
    uint32_t first, last; /* op list; last is the terminator once set */
    uint16_t charge;      /* source instructions retired by this block */
    uint8_t flags, nsucc;
    struct t1_edge succ[2]; /* BRANCH: [0] true target, [1] false target */
    uint8_t npred;
    uint8_t pred[T1_MAX_BLOCKS], pred_slot[T1_MAX_BLOCKS]; /* (block, succ index) */
    uint16_t param_end; /* last linked PARAM op + 1; zero if none */
};

struct t1_ir_point {
    uint8_t kind; /* enum t1_point_kind */
    uint8_t slot; /* resume slot */
    uint8_t access_size, access_write;
    uint8_t dead;  /* removed with its block */
    uint16_t insn; /* source instruction (sites, helpers; exits: the
                                 branch or the boundary instruction) */
    uint16_t block;
    uint16_t retired_offset;           /* Instructions of the block completed before */
    uint16_t site;                     /* sites: x->site[] index */
    uint8_t fs;                        /* the frame state at the point (struct t1_form fs[]) */
    uint8_t end;                       /* EXIT: enum t1_end_class */
    uint32_t op;                       /* the op carrying the point */
    uint64_t ip;                       /* resume IP (rebased guest IP) */
    uint32_t frame_first, frame_count; /* struct t1_use entries */
};

struct t1_form;
struct t1_workspace;
struct t1_ir {
    const struct t1_form *form; /* instruction copies, contract */
    /* The compile context's workspace the passes carve their arrays from
     * (memory.h); NULL: the pool. Set by the owner, kept by t1_ir_init. */
    struct t1_workspace *ws;
    unsigned nops, nblocks, nuses, npoints, nsites;
    bool failed;
    const char *fail_reason;
    /* Elements a frame covers: every element the region may write, and W. */
    uint64_t frame_set[T1_ELEMENT_WORDS];
    /* W: elements whose home an in-region HELPER may write. */
    uint64_t w_set[T1_ELEMENT_WORDS];
    /* NaT guard set G (element GR numbers: static p, stacked 32 + o
     * frame-relative), set by t1_natguard. */
    uint64_t nat_guard[2];
    uint64_t fr_guard[2]; /* FPI: source FR LIVEINs proven non-NaTVal */
    bool nat_folded;
    /* A NATCHK that did not fold makes its instruction a region
     * boundary; natguard records the first such instruction here (the
     * caller re-forms the region to end before it), else T1_NO_INSN. */
    uint16_t boundary_insn;
    /* Entry-block value index (ir.c entry_value): bucket heads by (code,
     * type, imm), T1_NO_OP if empty. Chains may contain an op that DCE
     * removed or natguard turned into NATIN; lookups check the op itself. */
    uint32_t entry_slot[T1_ENTRY_SLOTS];
    struct t1_block block[T1_MAX_BLOCKS];
    struct t1_ir_point point[T1_MAX_POINTS];
    struct t1_op op[T1_IR_MAX_OPS];
    struct t1_use use[T1_IR_MAX_USES];
};

static inline bool t1_elem_has(const uint64_t *set, unsigned e)
{
    return (set[e / 64] >> (e % 64)) & 1;
}
static inline void t1_elem_add(uint64_t *set, unsigned e)
{
    set[e / 64] |= UINT64_C(1) << (e % 64);
}

/* ---- builder (ir.c) ---- */
void t1_ir_init(struct t1_ir *ir, const struct t1_form *form);
/* Fails the IR (ir->failed, reason) and returns false; callers stop. */
bool t1_ir_fail(struct t1_ir *ir, const char *reason);
uint16_t t1_ir_new_block(struct t1_ir *ir, uint8_t flags);
/* Appends before the block's terminator (if set). Returns the op index or
 * T1_NO_OP on capacity failure. */
uint32_t t1_ir_append(struct t1_ir *ir, uint16_t block, uint16_t code, uint8_t type, int64_t imm,
                      const t1_value *opnds, unsigned n, uint16_t insn);
/* Deduplicated constant in the entry block. */
t1_value t1_ir_const(struct t1_ir *ir, uint8_t type, uint64_t value);
/* LIVEIN of an element in the entry block (deduplicated). */
t1_value t1_ir_livein(struct t1_ir *ir, uint8_t type, uint16_t element);
/* New PARAM at the end of the block's parameter list; the caller adds
 * one argument to every incoming edge. */
uint32_t t1_ir_add_param(struct t1_ir *ir, uint16_t block, uint8_t type, uint16_t element);
/* HELPER read-set and other variable operands. */
bool t1_ir_add_operand(struct t1_ir *ir, uint32_t op, t1_value v, uint16_t element);
/* Terminators. Edges register the predecessor in the target. */
bool t1_ir_set_jump(struct t1_ir *ir, uint16_t block, uint16_t target);
bool t1_ir_set_branch(struct t1_ir *ir, uint16_t block, t1_value cond, uint16_t t, uint16_t f,
                      uint16_t insn);
bool t1_ir_set_backedge(struct t1_ir *ir, uint16_t block, uint16_t header, uint32_t point,
                        uint16_t insn);
bool t1_ir_set_exit(struct t1_ir *ir, uint16_t block, uint32_t point, uint16_t insn);
bool t1_ir_edge_add_arg(struct t1_ir *ir, uint16_t block, unsigned succ, t1_value v,
                        uint16_t element);
/* Points; the frame is set once with t1_ir_set_frame. */
uint32_t t1_ir_new_point(struct t1_ir *ir, enum t1_point_kind kind, uint64_t ip, unsigned slot,
                         uint16_t insn, uint16_t block, uint16_t retired_offset);
/* Sets the point's frame in one allocation (n entries; value NONE with
 * T1_USE_HOME in flags[i] for a home entry; flags may be NULL). */
bool t1_ir_set_frame(struct t1_ir *ir, uint32_t point, const uint16_t *element,
                     const t1_value *value, const uint16_t *flags, unsigned n);

/* ---- queries ---- */
uint8_t t1_ir_value_type(const struct t1_ir *ir, t1_value v);
unsigned t1_ir_nres(const struct t1_ir *ir, uint32_t op);
static inline const struct t1_use *t1_ir_opnd(const struct t1_ir *ir, uint32_t op, unsigned i)
{
    return &ir->use[ir->op[op].use_first + i];
}
static inline uint32_t t1_ir_terminator(const struct t1_ir *ir, uint16_t b)
{
    uint32_t t = ir->block[b].last;
    return t != T1_NO_OP && t1_op_desc(ir->op[t].code)->effect == T1_EFF_TERM ? t : T1_NO_OP;
}
/* Longest charge sum from a loop header (or the head) to a checkpoint or
 * exit (max_path), over the forward (non-BACKEDGE) edges. */
unsigned t1_ir_max_path(const struct t1_ir *ir, uint16_t header);
/* The loop forest from the BACKEDGE edges: body[h] holds
 * the blocks of the natural loop of header h (bit b: block b; 0: h heads
 * no loop), header[b] the innermost loop header of block b and depth[b] the
 * number of loops holding it (T1_NO_BLOCK and 0 outside every loop). */
struct t1_ir_loops {
    uint64_t body[T1_MAX_BLOCKS];
    uint16_t header[T1_MAX_BLOCKS];
    uint8_t depth[T1_MAX_BLOCKS];
};
void t1_ir_loops(const struct t1_ir *ir, struct t1_ir_loops *out);
/* Immediate dominators of the live blocks (idom[entry] = entry); dead and
 * unreachable blocks get T1_NO_BLOCK. Returns false on a malformed CFG. */
bool t1_ir_dominators(const struct t1_ir *ir, uint16_t idom[T1_MAX_BLOCKS]);
bool t1_ir_dominates(const uint16_t idom[T1_MAX_BLOCKS], uint16_t a, uint16_t b);

/* ---- rewriting ---- */
t1_value t1_ir_resolve(const struct t1_ir *ir, t1_value v);
void t1_ir_forward(struct t1_ir *ir, uint32_t op, t1_value to);
/* Rewrites every use to its resolved value. */
void t1_ir_resolve_all(struct t1_ir *ir);
/* Unlinks an op from its block and marks it dead. */
void t1_ir_remove(struct t1_ir *ir, uint32_t op);
/* Removes dead PARAMs and their arguments on every incoming edge. */
void t1_ir_compact_params(struct t1_ir *ir);
/* Marks a block dead, removes its outgoing edges (and the predecessor
 * entries they made), its ops and its points. */
void t1_ir_kill_block(struct t1_ir *ir, uint16_t b);
/* Replaces a BRANCH terminator by a JUMP along succ[keep]. */
void t1_ir_branch_to_jump(struct t1_ir *ir, uint16_t b, unsigned keep);
/* Kills every block not reachable from the entry block. */
void t1_ir_remove_unreachable(struct t1_ir *ir);

/* ---- printing ---- */
typedef void (*t1_ir_out_fn)(void *arg, const char *text);
void t1_ir_print(const struct t1_ir *ir, t1_ir_out_fn out, void *arg);

/* ---- passes ---- */
/* verify.c. flags: T1_VERIFY_NAT_FOLDED after t1_natguard (no NATCHK, no
 * LIVEIN or PARAM of a NaT element, NaT frame and read-set entries CONST 0). */
#define T1_VERIFY_NAT_FOLDED 1u
bool t1_ir_verify(const struct t1_ir *ir, unsigned flags, char *err, size_t err_size);
/* natguard.c: computes G from the remaining LIVEIN(E_NAT p) uses,
 * replaces each by CONST 0 and folds. Returns |G|. */
unsigned t1_natguard(struct t1_ir *ir);
/* opt.c. */
void t1_opt_fold(struct t1_ir *ir);       /* Identities, I1 algebra, trivial params */
void t1_opt_frames(struct t1_ir *ir);     /* Frame rule (HOME entries) */
void t1_opt_predicates(struct t1_ir *ir); /* Constant and negated branches */
void t1_opt_dce(struct t1_ir *ir);        /* With frame pseudo-uses */
/* Whole pipeline after lifting, verifying after each pass when verify is
 * set: fold, predicates, fold, frames, DCE, natguard, predicates, fold,
 * frames, DCE. On false, ir->boundary_insn != T1_NO_INSN asks for a region that
 * ends before that instruction. */
bool t1_optimize(struct t1_ir *ir, bool verify, char *err, size_t err_size);
/* Known bits of every I64 result 0: kb[op].zero (one) has bit i set
 * when bit i is known 0 (1). LIVEINs are fully unknown; block parameters
 * are the meet over all incoming values, iterated to a fixed point over
 * loop edges. kb has room for ir->nops entries. */
struct t1_known {
    uint64_t zero, one;
};
void t1_known_bits(const struct t1_ir *ir, struct t1_known *kb);
/* roots[o] requests the same result-0 fact as t1_known_bits. Other outputs
 * are UNKNOWN. All arrays have nops entries. Returns false and fails ir
 * if workspace allocation fails; no output facts may then be consumed. */
bool t1_known_bits_demand(struct t1_ir *ir, struct t1_known *kb, const uint8_t *roots);
#endif
