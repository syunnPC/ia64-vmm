/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * IA-64 to SSA, following Braun et al., "Simple and Efficient Construction of
 * SSA Form" (CC 2013). Seal forward blocks as scanned and loop headers after
 * all backedges exist; remove trivial parameters.
 *
 * Apply instructions in program order (SDM Vol.1 §3.4); stops add no semantics.
 * Names resolved by formation are physical except stacked GRs, which use
 * frame offsets. Predication uses SELECT for pure/compare results, AND1 for
 * .unc compares, branch-around for memory/fences (charge both paths), and
 * unconditional HELPER. Point frames capture every element in frame_set.
 */
#include "engine/tier1/form.h"
#include "engine/tier1/helpers.h"
#include "ia64/semantics.h"
#include "ia64/fp.h"
#include "ia64vmm/hal.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/string.h"

struct lifter {
    const struct t1_form *f;
    struct t1_ir *ir;
    uint16_t b; /* current block, T1_NO_BLOCK if unreachable */
    unsigned i; /* current instruction */
    bool sealed[T1_MAX_BLOCKS];
    uint16_t block_of[T1_MAX_INSNS]; /* leader -> block */
    unsigned nelem;
    uint16_t elem[T1_N_ELEMENTS]; /* frame_set in ascending order */
    t1_value scratch[T1_N_ELEMENTS];
    /* cur[b] is set to T1_VALUE_NONE when block b is first used (bit b of
     * rows), not for every block at every lift. */
    uint64_t rows;
    t1_value cur[T1_MAX_BLOCKS][T1_N_ELEMENTS];
    /* The NaTVal test of an FR value whose producer is known: its
     * significand value and the I1 that equals FRNATTEST of it. */
    t1_value fr_known_sig[128], fr_known_nat[128];
};
_Static_assert(T1_MAX_BLOCKS <= 64, "lifter rows bitmap");

static t1_value *row(struct lifter *L, uint16_t b)
{
    if (!((L->rows >> b) & 1)) {
        for (unsigned e = 0; e < T1_N_ELEMENTS; ++e)
            L->cur[b][e] = T1_VALUE_NONE;
        L->rows |= UINT64_C(1) << b;
    }
    return L->cur[b];
}

static uint8_t element_type(unsigned e)
{
    if (e == T1_ELEMENT_MEM)
        return T1_MEM;
    if ((e >= 128 && e < 256) || (e >= 256 && e < T1_ELEMENT_LC))
        return T1_I1;
    return T1_I64;
}

static t1_value read_var(struct lifter *L, uint16_t e, uint16_t b);

static void add_param_args(struct lifter *L, uint16_t b, uint16_t e)
{
    struct t1_ir *ir = L->ir;
    for (unsigned p = 0; p < ir->block[b].npred && !ir->failed; ++p) {
        uint16_t pb = ir->block[b].pred[p];
        unsigned slot = ir->block[b].pred_slot[p];
        t1_ir_edge_add_arg(ir, pb, slot, read_var(L, e, pb), e);
    }
}

static t1_value read_var(struct lifter *L, uint16_t e, uint16_t b)
{
    struct t1_ir *ir = L->ir;
    t1_value v = row(L, b)[e];
    if (v != T1_VALUE_NONE)
        return t1_ir_resolve(ir, v);
    if (ir->failed)
        return T1_VALUE_NONE;
    if (b == T1_ENTRY_BLOCK) {
        v = t1_ir_livein(ir, element_type(e), e);
    } else if (!L->sealed[b]) {
        /* Incomplete: its arguments are added when b is sealed. */
        uint32_t op = t1_ir_add_param(ir, b, element_type(e), e);
        if (op == T1_NO_OP)
            return T1_VALUE_NONE;
        v = T1_VALUE(op, 0);
    } else if (ir->block[b].npred == 1) {
        v = read_var(L, e, ir->block[b].pred[0]);
    } else {
        uint32_t op = t1_ir_add_param(ir, b, element_type(e), e);
        if (op == T1_NO_OP)
            return T1_VALUE_NONE;
        v = T1_VALUE(op, 0);
        L->cur[b][e] = v; /* breaks cycles through this block */
        add_param_args(L, b, e);
    }
    L->cur[b][e] = v;
    return v;
}

/* ---- values of the current block ---- */

static t1_value cst(struct lifter *L, uint8_t type, uint64_t value)
{
    return t1_ir_const(L->ir, type, value);
}
static t1_value gr(struct lifter *L, unsigned p)
{
    return p ? read_var(L, T1_ELEMENT_GR(p), L->b) : cst(L, T1_I64, 0);
}
static t1_value nat(struct lifter *L, unsigned p)
{
    return p ? read_var(L, T1_ELEMENT_NAT(p), L->b) : cst(L, T1_I1, 0);
}
static t1_value pr(struct lifter *L, unsigned q)
{
    return q ? read_var(L, T1_ELEMENT_PR(q), L->b) : cst(L, T1_I1, 1);
}
static void set(struct lifter *L, uint16_t e, t1_value v)
{
    row(L, L->b)[e] = v;
}

static t1_value op3(struct lifter *L, uint16_t code, uint8_t type, int64_t imm, t1_value a,
                    t1_value b, t1_value c, unsigned n)
{
    t1_value opnd[3] = {a, b, c};
    uint32_t o = t1_ir_append(L->ir, L->b, code, type, imm, opnd, n, (uint16_t)L->i);
    return o == T1_NO_OP ? T1_VALUE_NONE : T1_VALUE(o, 0);
}
static t1_value op1(struct lifter *L, uint16_t code, int64_t imm, t1_value a)
{
    return op3(L, code, 0, imm, a, 0, 0, 1);
}
static t1_value op2(struct lifter *L, uint16_t code, t1_value a, t1_value b)
{
    return op3(L, code, 0, 0, a, b, 0, 2);
}
static t1_value select_(struct lifter *L, uint8_t type, t1_value c, t1_value t, t1_value f)
{
    return op3(L, T1_IR_SELECT, type, 0, c, t, f, 3);
}

/* A GR result with its NaT, under qp (first row). */
static void write_gr(struct lifter *L, unsigned p, t1_value v, t1_value n, t1_value qp,
                     bool qp_true)
{
    if (!p)
        return;
    if (!qp_true) {
        v = select_(L, T1_I64, qp, v, gr(L, p));
        n = select_(L, T1_I1, qp, n, nat(L, p));
    }
    set(L, T1_ELEMENT_GR(p), v);
    set(L, T1_ELEMENT_NAT(p), n);
}

/* After the instruction completed (interp_run: last_successful_bundle =
 * insn.address when PSR.ic was 1, also for a false qp). */
static void retire(struct lifter *L)
{
    if (L->f->c.psr & IA64_PSR_IC)
        set(L, T1_ELEMENT_LSB, cst(L, T1_I64, L->f->insn[L->i].insn.address));
    ++L->ir->block[L->b].charge;
}

/* ---- points ---- */

static bool frame(struct lifter *L, uint32_t point)
{
    struct t1_ir *ir = L->ir;
    if (point == T1_NO_POINT)
        return false;
    for (unsigned k = 0; k < L->nelem; ++k)
        L->scratch[k] = read_var(L, L->elem[k], L->b);
    return !ir->failed && t1_ir_set_frame(ir, point, L->elem, L->scratch, NULL, L->nelem);
}

/* A point in frame state fs (struct t1_form fs[]). The entry's state of a
 * contract that leaves sof, sol and sor open is no known frame: only its
 * head alloc runs in it, whose helper point restores the entry's CFM
 * (materialize.c t1_point_frame). */
static uint32_t new_point(struct lifter *L, enum t1_point_kind kind, uint64_t ip, unsigned slot,
                          uint16_t block, uint16_t retired, unsigned fs, unsigned end)
{
    if (!fs && L->f->c.cfm_mask != ~UINT64_C(0) &&
        !(kind == T1_PT_HELPER && L->f->insn[L->i].cls == T1_IC_ALLOC)) {
        t1_ir_fail(L->ir, "point in an open frame");
        return T1_NO_POINT;
    }
    uint32_t p = t1_ir_new_point(L->ir, kind, ip, slot, (uint16_t)L->i, block, retired);
    if (p != T1_NO_POINT) {
        L->ir->point[p].fs = (uint8_t)fs;
        L->ir->point[p].end = (uint8_t)end;
    }
    return p;
}

static uint32_t point_here(struct lifter *L, enum t1_point_kind kind)
{
    const struct t1_insn *ti = &L->f->insn[L->i];
    return new_point(L, kind, ti->insn.address, ti->insn.slot, L->b, L->ir->block[L->b].charge,
                     ti->fs, T1_END_LIMIT);
}

static uint16_t block_for(struct lifter *L, unsigned insn)
{
    if (L->block_of[insn] == T1_NO_BLOCK)
        L->block_of[insn] = t1_ir_new_block(L->ir, 0);
    return L->block_of[insn];
}

/* Fills an exit block whose single incoming edge exists already: an exit
 * to ip/slot in frame state fs, for reason `end` (enum t1_end_class), or
 * formation's own when it leaves where formation stopped. */
static void fill_exit(struct lifter *L, uint16_t x, uint64_t ip, unsigned slot, unsigned fs,
                      unsigned end)
{
    const struct t1_form *f = L->f;
    if (ip == f->end_ip && slot == f->end_slot && fs == f->end_fs)
        end = f->end_class;
    uint16_t saved = L->b;
    L->sealed[x] = true;
    L->b = x;
    uint32_t p = new_point(L, T1_PT_EXIT, ip, slot, x, 0, fs, end);
    if (frame(L, p))
        t1_ir_set_exit(L->ir, x, p, (uint16_t)L->i);
    L->b = saved;
}

/* Fills a back-edge block whose single incoming edge exists already: its
 * checkpoint resumes at the loop header (instruction h, a bundle start). */
static void fill_backedge(struct lifter *L, uint16_t k, unsigned h)
{
    uint16_t saved = L->b;
    L->sealed[k] = true;
    L->b = k;
    const Ia64Instruction *head = &L->f->insn[h].insn;
    uint32_t p = new_point(L, T1_PT_CHECKPOINT, head->address, head->slot, k, 0, L->f->insn[h].fs,
                           T1_END_LIMIT);
    if (frame(L, p))
        t1_ir_set_backedge(L->ir, k, L->block_of[h], p, (uint16_t)L->i);
    L->b = saved;
}

/* The frame state on the taken path of instruction ti, and why a taken
 * exit leaves the region (enum t1_end_class). */
static unsigned taken_fs(const struct t1_insn *ti)
{
    return ti->cls == T1_IC_CALL || ti->cls == T1_IC_RET ? ti->fs_taken : ti->fs;
}
static unsigned taken_end(const struct t1_insn *ti)
{
    return ti->cls == T1_IC_CALL  ? T1_END_CALL
           : ti->cls == T1_IC_RET ? T1_END_RET
           : ti->cls == T1_IC_CHK ? T1_END_NAT
                                  : T1_END_BRANCH;
}

typedef void (*taken_fn)(struct lifter *L, const struct t1_insn *ti);

/* The fall-through block of instruction i, or an exit at the region's end. */
static uint16_t fall_block(struct lifter *L)
{
    return L->i + 1 < L->f->ninsns && !L->f->insn[L->i].fall_exit ? block_for(L, L->i + 1)
                                                                  : t1_ir_new_block(L->ir, 0);
}

/* The taken path of instruction i from block `from` (unconditionally):
 * to its internal target, its header (a back-edge) or an exit. */
static void take_path(struct lifter *L, uint16_t from)
{
    struct t1_ir *ir = L->ir;
    const struct t1_insn *ti = &L->f->insn[L->i];
    uint16_t taken =
        ti->edge == T1_EDGE_INTERNAL ? block_for(L, ti->target_insn) : t1_ir_new_block(ir, 0);
    if (ir->failed)
        return;
    t1_ir_set_jump(ir, from, taken);
    if (ti->edge == T1_EDGE_BACK)
        fill_backedge(L, taken, ti->target_insn);
    else if (ti->edge == T1_EDGE_EXIT)
        fill_exit(L, taken, ti->target, 0, taken_fs(ti), taken_end(ti));
}

/* Ends the current block with the transfer of instruction i: taken on cond
 * (always: unconditionally), else fall through. on_taken (if any)
 * adds the effects of the taken path only (a call's link and ar.pfs). */
static void transfer(struct lifter *L, t1_value cond, bool always, taken_fn on_taken)
{
    struct t1_ir *ir = L->ir;
    const struct t1_insn *ti = &L->f->insn[L->i];
    if (always) {
        if (on_taken)
            on_taken(L, ti);
        take_path(L, L->b);
        L->b = T1_NO_BLOCK;
        return;
    }
    uint16_t fall = fall_block(L), from = L->b;
    if (ir->failed)
        return;
    if (on_taken) {
        uint16_t t = t1_ir_new_block(ir, T1_BLK_SYNTHETIC);
        if (ir->failed || !t1_ir_set_branch(ir, from, cond, t, fall, (uint16_t)L->i))
            return;
        L->sealed[t] = true;
        L->b = t;
        on_taken(L, ti);
        take_path(L, t);
    } else {
        uint16_t taken =
            ti->edge == T1_EDGE_INTERNAL ? block_for(L, ti->target_insn) : t1_ir_new_block(ir, 0);
        if (ir->failed)
            return;
        t1_ir_set_branch(ir, from, cond, taken, fall, (uint16_t)L->i);
        if (ti->edge == T1_EDGE_BACK)
            fill_backedge(L, taken, ti->target_insn);
        else if (ti->edge == T1_EDGE_EXIT)
            fill_exit(L, taken, ti->target, 0, taken_fs(ti), taken_end(ti));
    }
    if (L->i + 1 >= L->f->ninsns || ti->fall_exit)
        fill_exit(L, fall, ti->next_ip, ti->next_slot, ti->fs, L->f->end_class);
    L->b = T1_NO_BLOCK;
}

/* Ends the current block with the branch of instruction i: taken on cond
 * (a constant 1 makes it unconditional), else fall through. */
static void branch(struct lifter *L, t1_value cond, bool always)
{
    transfer(L, cond, always, NULL);
}

/* ---- instruction classes ---- */

static t1_value nat_by_rule(struct lifter *L, const struct t1_insn *ti)
{
    switch (ia64_opcode_info(ti->insn.opcode)->nat_rule) {
    case IA64_NAT_SOURCE_R2:
        return nat(L, ti->src1);
    case IA64_NAT_SOURCE_R3:
        return nat(L, ti->src2);
    case IA64_NAT_SOURCES_R2_R3:
        return op2(L, T1_IR_OR1, nat(L, ti->src1), nat(L, ti->src2));
    case IA64_NAT_CLEAR:
        return cst(L, T1_I1, 0);
    default:
        t1_ir_fail(L->ir, "unexpected NaT rule");
        return T1_VALUE_NONE;
    }
}

static t1_value packed_wrap(struct lifter *L, Ia64Opcode opcode, t1_value a, t1_value b)
{
    unsigned bits = opcode == IA64_OP_PADD1 || opcode == IA64_OP_PSUB1   ? 8
                    : opcode == IA64_OP_PADD2 || opcode == IA64_OP_PSUB2 ? 16
                                                                         : 32;
    bool sub = opcode == IA64_OP_PSUB1 || opcode == IA64_OP_PSUB2 || opcode == IA64_OP_PSUB4;
    if (bits == 32) {
        /* With just one lane boundary, remove its carry/borrow from the
         * full-word result. Bit 32 of a^b^result is the incoming carry. */
        t1_value sum = op2(L, sub ? T1_IR_SUB : T1_IR_ADD, a, b);
        t1_value carry = op2(L, T1_IR_AND, op2(L, T1_IR_XOR, op2(L, T1_IR_XOR, a, b), sum),
                             cst(L, T1_I64, UINT64_C(1) << 32));
        return op2(L, sub ? T1_IR_ADD : T1_IR_SUB, sum, carry);
    }
    uint64_t high = bits == 8 ? UINT64_C(0x8080808080808080) : UINT64_C(0x8000800080008000);
    t1_value h = cst(L, T1_I64, high), low = cst(L, T1_I64, ~high);
    /* Guard bits stop carry/borrow at every boundary. XOR restores the
     * independently computed high result bits after low-part arithmetic. */
    t1_value sum =
        op2(L, sub ? T1_IR_SUB : T1_IR_ADD, op2(L, sub ? T1_IR_OR : T1_IR_AND, a, sub ? h : low),
            op2(L, T1_IR_AND, b, low));
    t1_value v = op2(L, T1_IR_XOR, sum, op2(L, T1_IR_AND, op2(L, T1_IR_XOR, a, b), h));
    return sub ? op2(L, T1_IR_XOR, v, h) : v;
}

static void lift_alu(struct lifter *L, const struct t1_insn *ti)
{
    const Ia64Instruction *in = &ti->insn;
    uint64_t imm = (uint64_t)in->operands.integer.immediate;
    unsigned pos = imm & 63;
    t1_value qp = pr(L, ti->qp), v;
    t1_value n = nat_by_rule(L, ti);
    t1_value a = T1_VALUE_NONE, b = gr(L, ti->src2);
    switch (in->opcode) {
    case IA64_OP_ADDP4:
    case IA64_OP_SHLADDP4:
    case IA64_OP_ADD:
    case IA64_OP_ADD_ONE:
    case IA64_OP_SUB:
    case IA64_OP_SUB_ONE:
    case IA64_OP_PADD1:
    case IA64_OP_PADD2:
    case IA64_OP_PADD4:
    case IA64_OP_PSUB1:
    case IA64_OP_PSUB2:
    case IA64_OP_PSUB4:
    case IA64_OP_AND:
    case IA64_OP_ANDCM:
    case IA64_OP_OR:
    case IA64_OP_XOR:
    case IA64_OP_SHLADD:
    case IA64_OP_SHL:
    case IA64_OP_SHR:
    case IA64_OP_SHRU:
    case IA64_OP_DEPZ:
        a = gr(L, ti->src1);
        break;
    default:
        break;
    }
    switch (in->opcode) {
    case IA64_OP_ADDP4:
    case IA64_OP_ADDP4_IMM:
    case IA64_OP_SHLADDP4: {
        /* SDM Vol.3 addp4/shladdp4: region bits come from b[31:30],
         * independently of the carry in the truncated 32-bit addition. */
        if (in->opcode == IA64_OP_ADDP4_IMM)
            a = cst(L, T1_I64, imm);
        if (in->opcode == IA64_OP_SHLADDP4)
            a = op1(L, T1_IR_SHL_IMM, imm & 63, a);
        t1_value sum = op1(L, T1_IR_ZXT, 32, op2(L, T1_IR_ADD, a, b));
        t1_value region = op1(L, T1_IR_SHL_IMM, 61, op1(L, T1_IR_EXTRU, T1_FIELD_IMM(30, 2), b));
        v = op2(L, T1_IR_OR, sum, region);
        break;
    }
    case IA64_OP_ADD:
        v = op2(L, T1_IR_ADD, a, b);
        break;
    case IA64_OP_ADD_ONE:
        v = op2(L, T1_IR_ADD, op2(L, T1_IR_ADD, a, b), cst(L, T1_I64, 1));
        break;
    case IA64_OP_SUB:
        v = op2(L, T1_IR_SUB, a, b);
        break;
    case IA64_OP_SUB_ONE:
        v = op2(L, T1_IR_SUB, op2(L, T1_IR_SUB, a, b), cst(L, T1_I64, 1));
        break;
    case IA64_OP_PADD1:
    case IA64_OP_PADD2:
    case IA64_OP_PADD4:
    case IA64_OP_PSUB1:
    case IA64_OP_PSUB2:
    case IA64_OP_PSUB4:
        v = packed_wrap(L, in->opcode, a, b);
        break;
    case IA64_OP_SUB_IMM:
        v = op2(L, T1_IR_SUB, cst(L, T1_I64, imm), b);
        break;
    case IA64_OP_ADDS:
    case IA64_OP_ADDL:
        v = op2(L, T1_IR_ADD, b, cst(L, T1_I64, imm));
        break;
    case IA64_OP_AND:
        v = op2(L, T1_IR_AND, a, b);
        break;
    case IA64_OP_ANDCM:
        v = op2(L, T1_IR_ANDCM, a, b);
        break;
    case IA64_OP_OR:
        v = op2(L, T1_IR_OR, a, b);
        break;
    case IA64_OP_XOR:
        v = op2(L, T1_IR_XOR, a, b);
        break;
    case IA64_OP_AND_IMM:
        v = op2(L, T1_IR_AND, cst(L, T1_I64, imm), b);
        break;
    case IA64_OP_ANDCM_IMM:
        v = op2(L, T1_IR_ANDCM, cst(L, T1_I64, imm), b);
        break;
    case IA64_OP_OR_IMM:
        v = op2(L, T1_IR_OR, cst(L, T1_I64, imm), b);
        break;
    case IA64_OP_XOR_IMM:
        v = op2(L, T1_IR_XOR, cst(L, T1_I64, imm), b);
        break;
    case IA64_OP_SHLADD:
        v = op2(L, T1_IR_ADD, op1(L, T1_IR_SHL_IMM, (int64_t)(imm & 63), a), b);
        break;
    /* The decoder puts the count in source1 and the shifted value in
     * source2 for all three register shifts (ia64/integer.c). */
    case IA64_OP_SHL:
        v = op2(L, T1_IR_SHL, b, a);
        break;
    case IA64_OP_SHR:
        v = op2(L, T1_IR_SHR, b, a);
        break;
    case IA64_OP_SHRU:
        v = op2(L, T1_IR_SHRU, b, a);
        break;
    case IA64_OP_SHL_IMM:
        v = op1(L, T1_IR_SHL_IMM, pos, b);
        break;
    case IA64_OP_EXTR:
    case IA64_OP_EXTRU:
        v = op1(L, in->opcode == IA64_OP_EXTR ? T1_IR_EXTR : T1_IR_EXTRU,
                T1_FIELD_IMM(pos, ia64_bitfield_effective_len(pos, imm >> 6)), b);
        break;
    case IA64_OP_DEPZ:
        v = op1(L, T1_IR_DEPZ,
                T1_FIELD_IMM(pos, ia64_bitfield_effective_len(pos, (imm >> 6) & 127)), a);
        break;
    case IA64_OP_DEPZ_IMM: /* dep.z r1 = imm8, pos, len: imm8 at bits 20:13 of the packed field */
        v = op1(L, T1_IR_DEPZ,
                T1_FIELD_IMM(pos, ia64_bitfield_effective_len(pos, (imm >> 6) & 127)),
                cst(L, T1_I64, (uint64_t)ia64_sign_extend(imm >> 13, 8)));
        break;
    case IA64_OP_SXT1:
        v = op1(L, T1_IR_SXT, 8, b);
        break;
    case IA64_OP_SXT2:
        v = op1(L, T1_IR_SXT, 16, b);
        break;
    case IA64_OP_SXT4:
        v = op1(L, T1_IR_SXT, 32, b);
        break;
    case IA64_OP_ZXT1:
        v = op1(L, T1_IR_ZXT, 8, b);
        break;
    case IA64_OP_ZXT2:
        v = op1(L, T1_IR_ZXT, 16, b);
        break;
    case IA64_OP_ZXT4:
        v = op1(L, T1_IR_ZXT, 32, b);
        break;
    case IA64_OP_MOVL:
        v = cst(L, T1_I64, imm);
        break;
    /* SDM Vol.3 mov ip: the IP of the bundle, i.e. the rebased address. */
    case IA64_OP_MOV_CURRENT_IP:
        v = cst(L, T1_I64, in->address);
        break;
    default:
        t1_ir_fail(L->ir, "ALU opcode without lifting");
        return;
    }
    write_gr(L, ti->dst, v, n, qp, ti->qp == 0);
    retire(L);
}

/* The compare rows ia64/integer.c executes (relation, cmp4, immediate,
 * predicate update). */
enum relation {
    REL_EQ,
    REL_NE,
    REL_LT,
    REL_LE,
    REL_GT,
    REL_GE,
    REL_LTU,
    REL_LEU,
    REL_GTU,
    REL_GEU
};
struct compare_desc {
    bool valid, word, immediate;
    enum relation relation;
    Ia64PredicateUpdate update;
};
static const struct compare_desc compares[IA64_OP_COUNT] = {
#include "ia64/decode/compare.inc"
};

/* rel(a, b) from CMP's eq, lt and ltu: gt and le swap the operands, ne, le
 * and ge negate. */
static t1_value relation(struct lifter *L, enum relation rel, unsigned width, t1_value a,
                         t1_value b)
{
    static const struct {
        uint8_t cmp;
        bool swap, negate;
    } r[] = {
        [REL_EQ] = {T1_CMP_EQ, false, false},   [REL_NE] = {T1_CMP_EQ, false, true},
        [REL_LT] = {T1_CMP_LT, false, false},   [REL_LE] = {T1_CMP_LT, true, true},
        [REL_GT] = {T1_CMP_LT, true, false},    [REL_GE] = {T1_CMP_LT, false, true},
        [REL_LTU] = {T1_CMP_LTU, false, false}, [REL_LEU] = {T1_CMP_LTU, true, true},
        [REL_GTU] = {T1_CMP_LTU, true, false},  [REL_GEU] = {T1_CMP_LTU, false, true},
    };
    t1_value c = op3(L, T1_IR_CMP, 0, T1_CMP_IMM(r[rel].cmp, width), r[rel].swap ? b : a,
                     r[rel].swap ? a : b, 0, 2);
    return r[rel].negate ? op1(L, T1_IR_NOT1, 0, c) : c;
}

/* cmp/cmp4 (normal, .unc, parallel) and tbit (SDM Vol.3; ia64/integer.c
 * predicate_result): c is the relation (tbit: the bit test), n the sources'
 * NaT. Normal: p1 = !n & c, p2 = !n & !c. Parallel, with hit = !n & c:
 * .and p1 &= hit, p2 &= hit; .or p1 |= hit, p2 |= hit; .or.andcm p1 |= hit,
 * p2 &= !hit. A false qp clears both targets of .unc and keeps them
 * otherwise. */
static void lift_cmp(struct lifter *L, const struct t1_insn *ti)
{
    const Ia64Instruction *in = &ti->insn;
    Ia64Opcode o = in->opcode;
    const struct compare_desc *d = &compares[o];
    t1_value qp = pr(L, ti->qp), c, n;
    Ia64PredicateUpdate update;
    if (d->valid) {
        t1_value a = d->immediate ? cst(L, T1_I64, (uint64_t)in->operands.integer.immediate)
                                  : gr(L, ti->src1);
        n = d->immediate ? nat(L, ti->src2) : op2(L, T1_IR_OR1, nat(L, ti->src1), nat(L, ti->src2));
        c = relation(L, d->relation, d->word ? 32 : 64, a, gr(L, ti->src2));
        update = d->update;
    } else {
        unsigned pos = (unsigned)in->operands.integer.immediate & 63;
        t1_value bit = op1(L, T1_IR_EXTRU, T1_FIELD_IMM(pos, 1), gr(L, ti->src2));
        c = op3(L, T1_IR_CMP, 0, T1_CMP_IMM(T1_CMP_EQ, 64), bit, cst(L, T1_I64, 0), 0, 2);
        if (o == IA64_OP_TBIT_NZ || o == IA64_OP_TBIT_NZ_OR_ANDCM)
            c = op1(L, T1_IR_NOT1, 0, c);
        n = nat(L, ti->src2);
        update = o == IA64_OP_TBIT_Z_OR_ANDCM || o == IA64_OP_TBIT_NZ_OR_ANDCM
                     ? IA64_PRED_UPDATE_OR_ANDCM
                     : in->pred_update;
    }
    t1_value ok = op1(L, T1_IR_NOT1, 0, n);
    t1_value t = op2(L, T1_IR_AND1, ok, c), f;
    switch (update) {
    case IA64_PRED_UPDATE_AND:
        f = op2(L, T1_IR_AND1, pr(L, ti->p2), t);
        t = op2(L, T1_IR_AND1, pr(L, ti->p1), t);
        break;
    case IA64_PRED_UPDATE_OR:
        f = op2(L, T1_IR_OR1, pr(L, ti->p2), t);
        t = op2(L, T1_IR_OR1, pr(L, ti->p1), t);
        break;
    case IA64_PRED_UPDATE_OR_ANDCM:
        f = op2(L, T1_IR_AND1, pr(L, ti->p2), op1(L, T1_IR_NOT1, 0, t));
        t = op2(L, T1_IR_OR1, pr(L, ti->p1), t);
        break;
    default:
        f = op2(L, T1_IR_AND1, ok, op1(L, T1_IR_NOT1, 0, c));
        break;
    }
    if (in->compare_unc) {
        t = op2(L, T1_IR_AND1, qp, t);
        f = op2(L, T1_IR_AND1, qp, f);
    } else if (ti->qp) {
        t = select_(L, T1_I1, qp, t, pr(L, ti->p1));
        f = select_(L, T1_I1, qp, f, pr(L, ti->p2));
    }
    if (ti->p1)
        set(L, T1_ELEMENT_PR(ti->p1), t);
    if (ti->p2)
        set(L, T1_ELEMENT_PR(ti->p2), f);
    retire(L);
}

/* Read set of a tnat helper (tnat reads only source2), in the HELPER
 * operand order documented in ir.h. */
static unsigned helper_read_set(const struct t1_insn *ti, uint16_t *e)
{
    unsigned n = 0;
    if (ti->qp)
        e[n++] = T1_ELEMENT_PR(ti->qp);
    if (ti->src2) {
        e[n++] = T1_ELEMENT_GR(ti->src2);
        e[n++] = T1_ELEMENT_NAT(ti->src2);
    }
    if (ti->p1)
        e[n++] = T1_ELEMENT_PR(ti->p1);
    if (ti->p2)
        e[n++] = T1_ELEMENT_PR(ti->p2);
    return n;
}

static void lift_helper(struct lifter *L, const struct t1_insn *ti)
{
    struct t1_ir *ir = L->ir;
    uint16_t e[8];
    t1_value reads[8];
    unsigned n = helper_read_set(ti, e);
    t1_value mem = read_var(L, T1_ELEMENT_MEM, L->b);
    for (unsigned k = 0; k < n; ++k)
        reads[k] = read_var(L, e[k], L->b);
    uint32_t p = point_here(L, T1_PT_HELPER);
    if (!frame(L, p))
        return;
    uint32_t op = t1_ir_append(ir, L->b, T1_IR_HELPER, 0, (int64_t)L->i, &mem, 1, (uint16_t)L->i);
    if (op == T1_NO_OP)
        return;
    for (unsigned k = 0; k < n; ++k)
        t1_ir_add_operand(ir, op, reads[k], e[k]);
    ir->op[op].point = p;
    ir->point[p].op = op;
    ir->op[op].wr[0] = ti->p1 ? T1_ELEMENT_PR(ti->p1) : 0;
    ir->op[op].wr[1] = ti->p2 ? T1_ELEMENT_PR(ti->p2) : 0;
    if (ti->p1)
        set(L, T1_ELEMENT_PR(ti->p1), T1_VALUE(op, T1_RES_HELPER_p1));
    if (ti->p2)
        set(L, T1_ELEMENT_PR(ti->p2), T1_VALUE(op, T1_RES_HELPER_p2));
    set(L, T1_ELEMENT_MEM, T1_VALUE(op, T1_RES_HELPER_mem));
    retire(L);
}

/* Generalized helpers use home snapshots so a write set can include
 * postincrement GRs or all predicates without widening SSA result ids.
 * FR and modified-bit homes join the snapshots; other AR/BR homes
 * remain authoritative throughout the region. */
static t1_value fr_nat(struct lifter *L, unsigned f);
static bool const_zero(struct lifter *L, t1_value v);
/* The NaTVal test of a class A helper's FR result: an FR result of these
 * instructions is NaTVal exactly when an FR source is (setf: the GR NaT;
 * ia64/fp.c, the NaT case precedes all others and no other result is the
 * NaTVal encoding). T1_VALUE_NONE when there is no such result. */
static t1_value helper_fr_nat(struct lifter *L, const struct t1_insn *ti)
{
    const struct t1_helper_row *row = t1_helper_row(ti->insn.opcode);
    if (row->cls != T1_HC_A || row->write[0].loc != T1_HL_FR)
        return T1_VALUE_NONE;
    t1_value n = cst(L, T1_I1, 0);
    for (unsigned k = 0; k < row->nread; ++k) {
        unsigned slot = row->read[k].slot;
        t1_value x = T1_VALUE_NONE;
        if (row->read[k].loc == T1_HL_GR)
            x = nat(L, ti->src1);
        if (row->read[k].loc == T1_HL_FR) {
            unsigned logical = slot == T1_HS_R2   ? ti->insn.operands.common.source1
                               : slot == T1_HS_R3 ? ti->insn.operands.common.source2
                                                  : ti->insn.operands.common.auxiliary1;
            if (logical >= 2)
                x = fr_nat(L, ti->fr[slot == T1_HS_R2 ? 1 : slot == T1_HS_R3 ? 2 : 3]);
        }
        if (x == T1_VALUE_NONE || const_zero(L, x))
            continue;
        n = const_zero(L, n) ? x : op2(L, T1_IR_OR1, n, x);
    }
    return n;
}

static void lift_general(struct lifter *L, const struct t1_insn *ti)
{
    struct t1_ir *ir = L->ir;
    uint16_t reads[T1_N_ELEMENTS], writes[T1_N_ELEMENTS];
    unsigned nr = t1_helper_elements(ti, false, reads);
    unsigned nw = t1_helper_elements(ti, true, writes);
    t1_value result_nat = ti->qp ? T1_VALUE_NONE : helper_fr_nat(L, ti);
    t1_value mem = read_var(L, T1_ELEMENT_MEM, L->b);
    uint32_t p = point_here(L, T1_PT_HELPER);
    if (!frame(L, p))
        return;
    uint32_t o = t1_ir_append(ir, L->b, T1_IR_GHELPER, 0, L->i, &mem, 1, L->i);
    if (o == T1_NO_OP)
        return;
    for (unsigned k = 0; k < nr; ++k)
        t1_ir_add_operand(ir, o, read_var(L, reads[k], L->b), reads[k]);
    ir->op[o].point = p;
    ir->point[p].op = o;
    mem = T1_VALUE(o, 0);
    const struct ia64_opcode_info *meta = ia64_opcode_info(ti->insn.opcode);
    t1_value old_nat = meta->writes_gr && meta->nat_rule == IA64_NAT_CLEAR && ti->dst && ti->qp
                           ? nat(L, ti->dst)
                           : cst(L, T1_I1, 0);
    for (unsigned k = 0; k < nw; ++k) {
        t1_value value;
        if (meta->writes_gr && meta->nat_rule == IA64_NAT_CLEAR &&
            writes[k] == T1_ELEMENT_NAT(ti->dst))
            value = ti->qp ? select_(L, T1_I1, pr(L, ti->qp), cst(L, T1_I1, 0), old_nat)
                           : cst(L, T1_I1, 0);
        else
            value = op3(L, T1_IR_HREAD, element_type(writes[k]), writes[k], mem, 0, 0, 1);
        /* Helpers may write the other FR bank without reading cached MF.
         * Dirty bits are monotone within a region; retain prior
         * native writes even when the helper's home did not yet hold them. */
        if (writes[k] == T1_ELEMENT_PSR_MF)
            value = op2(L, T1_IR_OR, read_var(L, T1_ELEMENT_PSR_MF, L->b), value);
        set(L, writes[k], value);
    }
    if (result_nat != T1_VALUE_NONE) {
        L->fr_known_sig[ti->fr[0]] = read_var(L, T1_ELEMENT_FR(ti->fr[0]), L->b);
        L->fr_known_nat[ti->fr[0]] = result_nat;
    }
    retire(L);
    const struct t1_helper_row *row = t1_helper_row(ti->insn.opcode);
    if (!(row->flags & (T1_HROW_STORE | T1_HROW_WALK))) {
        set(L, T1_ELEMENT_MEM, mem);
        return;
    }
    p = new_point(L, T1_PT_EXIT, ti->next_ip, ti->next_slot, L->b, ir->block[L->b].charge, ti->fs,
                  T1_END_LIMIT);
    if (!frame(L, p))
        return;
    o = t1_ir_append(ir, L->b, T1_IR_HGUARD, 0, 0, &mem, 1, L->i);
    if (o == T1_NO_OP)
        return;
    ir->op[o].point = p;
    ir->point[p].op = o;
    set(L, T1_ELEMENT_MEM, T1_VALUE(o, 0));
}

/* SDM Vol.3 getf/setf and FR logical operations: values, tags and NaTVal
 * travel separately so false qp preserves the entire architectural FR. */
static t1_value fr_sig(struct lifter *L, unsigned f)
{
    return f < 2 ? cst(L, T1_I64, f ? IA64_FP_INTEGER_BIT : 0)
                 : read_var(L, T1_ELEMENT_FR(f), L->b);
}
static t1_value fr_meta(struct lifter *L, unsigned f)
{
    return f < 2 ? cst(L, T1_I64, f ? IA64_FP_BIAS : 0) : read_var(L, T1_ELEMENT_FR_META(f), L->b);
}
static t1_value fr_nat(struct lifter *L, unsigned f)
{
    t1_value sig = fr_sig(L, f);
    if (f >= 2 && sig != T1_VALUE_NONE && sig == L->fr_known_sig[f])
        return L->fr_known_nat[f];
    return op2(L, T1_IR_FRNATTEST, sig, fr_meta(L, f));
}
/* FR f = (sig, meta) with PSR.mfl/mfh, under qp when `predicated`; an
 * unpredicated write with n records it as the NaTVal test of the value. */
static void set_fr(struct lifter *L, unsigned f, unsigned logical, t1_value sig, t1_value meta,
                   t1_value n, t1_value qp, bool predicated)
{
    t1_value mf = read_var(L, T1_ELEMENT_PSR_MF, L->b);
    t1_value bit = cst(L, T1_I64, logical < 32 ? IA64_PSR_MFL : IA64_PSR_MFH);
    if (predicated) {
        sig = select_(L, T1_I64, qp, sig, fr_sig(L, f));
        meta = select_(L, T1_I64, qp, meta, fr_meta(L, f));
        bit = select_(L, T1_I64, qp, bit, cst(L, T1_I64, 0));
    }
    set(L, T1_ELEMENT_FR(f), sig);
    set(L, T1_ELEMENT_FR_META(f), meta);
    set(L, T1_ELEMENT_PSR_MF, op2(L, T1_IR_OR, mf, bit));
    L->fr_known_sig[f] = predicated || n == T1_VALUE_NONE ? T1_VALUE_NONE : sig;
    L->fr_known_nat[f] = n;
}
static void write_fr(struct lifter *L, const struct t1_insn *ti, t1_value sig, t1_value meta)
{
    set_fr(L, ti->fr[0], ti->insn.operands.common.destination, sig, meta, T1_VALUE_NONE,
           ti->qp ? pr(L, ti->qp) : T1_VALUE_NONE, ti->qp != 0);
}
/* SDM Vol.3 alloc: r1 = ar.pfs, NaT clear, the new frame (formation's
 * frame state). When the dirty registers and the new frame exceed the 96
 * stacked registers it first spills (Vol.2 §6.5): the exact helper
 * (helper.c), which the code calls only then (emit.c compares
 * x->entry_dirty), and an exit after it when its stores retired the region.
 * Both paths give r1 the same value. */
static void lift_alloc(struct lifter *L, const struct t1_insn *ti)
{
    struct t1_ir *ir = L->ir;
    t1_value mem = read_var(L, T1_ELEMENT_MEM, L->b);
    t1_value pfs = read_var(L, T1_ELEMENT_PFS, L->b);
    uint32_t p = point_here(L, T1_PT_HELPER);
    if (!frame(L, p))
        return;
    uint32_t o = t1_ir_append(ir, L->b, T1_IR_GHELPER, 0, L->i, &mem, 1, L->i);
    if (o == T1_NO_OP)
        return;
    t1_ir_add_operand(ir, o, pfs, T1_ELEMENT_PFS);
    ir->op[o].point = p;
    ir->point[p].op = o;
    mem = T1_VALUE(o, 0);
    write_gr(L, ti->dst, pfs, cst(L, T1_I1, 0), cst(L, T1_I1, 1), true);
    retire(L);
    p = new_point(L, T1_PT_EXIT, ti->next_ip, ti->next_slot, L->b, ir->block[L->b].charge,
                  ti->fs_taken, T1_END_LIMIT);
    if (!frame(L, p))
        return;
    o = t1_ir_append(ir, L->b, T1_IR_HGUARD, 0, 0, &mem, 1, L->i);
    if (o == T1_NO_OP)
        return;
    ir->op[o].point = p;
    ir->point[p].op = o;
    set(L, T1_ELEMENT_MEM, T1_VALUE(o, 0));
}

static unsigned access_bytes(Ia64Opcode op)
{
    switch (op) {
    case IA64_OP_LD1:
    case IA64_OP_LD1S:
    case IA64_OP_LD1A:
    case IA64_OP_LD1SA:
    case IA64_OP_LD1C_CLR:
    case IA64_OP_LD1C_NC:
    case IA64_OP_ST1:
    case IA64_OP_ST1REL:
        return 1;
    case IA64_OP_LD2:
    case IA64_OP_LD2S:
    case IA64_OP_LD2A:
    case IA64_OP_LD2SA:
    case IA64_OP_LD2C_CLR:
    case IA64_OP_LD2C_NC:
    case IA64_OP_ST2:
    case IA64_OP_ST2REL:
        return 2;
    case IA64_OP_LD4:
    case IA64_OP_LD4S:
    case IA64_OP_LD4A:
    case IA64_OP_LD4SA:
    case IA64_OP_LD4C_CLR:
    case IA64_OP_LD4C_NC:
    case IA64_OP_ST4:
    case IA64_OP_ST4REL:
        return 4;
    default:
        return 8;
    }
}

static bool const_zero(struct lifter *L, t1_value v)
{
    const struct t1_op *op = &L->ir->op[T1_VALUE_OP(t1_ir_resolve(L->ir, v))];
    return op->code == T1_IR_CONST && op->imm == 0;
}
/* An exit before the current instruction when cond holds (it executes in
 * the lower tier); the instruction continues in a new block. */
static void exit_if(struct lifter *L, t1_value cond)
{
    struct t1_ir *ir = L->ir;
    const Ia64Instruction *in = &L->f->insn[L->i].insn;
    uint16_t x = t1_ir_new_block(ir, T1_BLK_SYNTHETIC), c = t1_ir_new_block(ir, T1_BLK_SYNTHETIC);
    if (ir->failed || !t1_ir_set_branch(ir, L->b, cond, x, c, (uint16_t)L->i))
        return;
    L->sealed[c] = true;
    fill_exit(L, x, in->address, in->slot, L->f->insn[L->i].fs, T1_END_NAT);
    L->b = c;
}
static t1_value eq_const(struct lifter *L, t1_value a, uint64_t value)
{
    return op3(L, T1_IR_CMP, T1_I1, T1_CMP_IMM(T1_CMP_EQ, 64), a, cst(L, T1_I64, value), 0, 2);
}
static t1_value field(struct lifter *L, t1_value a, unsigned pos, unsigned len)
{
    return op1(L, T1_IR_EXTRU, T1_FIELD_IMM(pos, len), a);
}
static t1_value deposit(struct lifter *L, t1_value a, unsigned pos, unsigned len)
{
    return op1(L, T1_IR_DEPZ, T1_FIELD_IMM(pos, len), a);
}
/* SDM Vol.1 §5.3.1 Figures 5-4, 5-5 (ia64/fp.c ia64_fp_from_ieee): a single
 * or double memory image to register format, a bit transfer. */
static void from_ieee(struct lifter *L, t1_value x, bool single, t1_value *sig, t1_value *meta)
{
    unsigned width = single ? 23 : 52, ebits = single ? 8 : 11;
    uint64_t base = single ? 0xff80 : 0xfc00;
    t1_value e = field(L, x, width, ebits);
    t1_value fraction = deposit(L, x, 63 - width, width);
    t1_value ez = eq_const(L, e, 0);
    *sig = op2(L, T1_IR_OR, fraction,
               select_(L, T1_I64, ez, cst(L, T1_I64, 0), cst(L, T1_I64, IA64_FP_INTEGER_BIT)));
    t1_value denormal =
        select_(L, T1_I64, eq_const(L, fraction, 0), cst(L, T1_I64, 0), cst(L, T1_I64, base + 1));
    t1_value exponent =
        select_(L, T1_I64, ez, denormal, op2(L, T1_IR_ADD, e, cst(L, T1_I64, base)));
    exponent = select_(L, T1_I64, eq_const(L, e, (1u << ebits) - 1),
                       cst(L, T1_I64, IA64_FP_SPECIAL_EXP), exponent);
    *meta = op2(L, T1_IR_OR, exponent, deposit(L, field(L, x, width + ebits, 1), 17, 1));
}
/* The inverse (ia64/fp.c ia64_fp_to_ieee): sign, exponent bits 16 and
 * 6:0/9:0, the exponent zero without the integer bit, the fraction. */
static t1_value to_ieee(struct lifter *L, t1_value sig, t1_value meta, bool single)
{
    unsigned width = single ? 23 : 52, low = single ? 7 : 10;
    t1_value e =
        op2(L, T1_IR_OR, deposit(L, field(L, meta, 16, 1), low, 1), field(L, meta, 0, low));
    e = select_(L, T1_I64, eq_const(L, field(L, sig, 63, 1), 0), cst(L, T1_I64, 0), e);
    t1_value top = op2(L, T1_IR_OR, deposit(L, field(L, meta, 17, 1), width + low + 1, 1),
                       deposit(L, e, width, low + 1));
    return op2(L, T1_IR_OR, top, field(L, sig, 63 - width, width));
}

/* The access itself, in the current block (the true path when predicated). */
static void lift_access(struct lifter *L, const struct t1_insn *ti)
{
    struct t1_ir *ir = L->ir;
    const Ia64Instruction *in = &ti->insn;
    bool store = ti->cls == T1_IC_STORE;
    unsigned fp = t1_fp_access(in->opcode);
    unsigned bytes = fp ? fp : access_bytes(in->opcode);
    if (ir->nsites >= T1_MAX_SITES) {
        t1_ir_fail(ir, "site limit");
        return;
    }
    unsigned site = ir->nsites++;
    /* ia64_memory reads base, value and increment before the access. */
    t1_value base = gr(L, ti->base), nbase = nat(L, ti->base);
    t1_value value = store ? gr(L, ti->source) : T1_VALUE_NONE, nvalue = T1_VALUE_NONE;
    if (store && fp) {
        /* stfs/stfd/stf8 [r3] = f2: the FR source; NaTVal is a Register
         * NaT Consumption fault (SDM Vol.3 stf), taken in the lower tier
         * when not known false here: an exit before the store, or, at the
         * region's first instruction where that exit would retire nothing,
         * the consumption check (an entry guard on a LIVEIN FR, else the
         * end of the region before the store). */
        unsigned f = in->operands.memory.source < 2 ? in->operands.memory.source : ti->fr[1];
        value = fp == 8 && in->opcode == IA64_OP_STF8
                    ? fr_sig(L, f)
                    : to_ieee(L, fr_sig(L, f), fr_meta(L, f), fp == 4);
        t1_value natval = f < 2 ? cst(L, T1_I1, 0) : fr_nat(L, f);
        nvalue = cst(L, T1_I1, 0);
        if (L->i == 0)
            nvalue = natval;
        else if (!const_zero(L, natval))
            exit_if(L, natval);
        if (ir->failed)
            return;
    } else if (store)
        nvalue = nat(L, ti->source);
    t1_value inc = in->reg_base_update ? gr(L, ti->source)
                                       : cst(L, T1_I64, (uint64_t)in->operands.memory.immediate);
    t1_value ninc = in->reg_base_update ? op2(L, T1_IR_OR1, nbase, nat(L, ti->source)) : nbase;
    /* Register NaT Consumption: must fold away or end the region; a
     * ld.s with a NaT base then defers in the lower tier. A native load's
     * value is never NaT: ld.s and ld.sa that would defer leave at the
     * site (form.c t1_opcode_class). */
    t1_value consumed = store ? op2(L, T1_IR_OR1, nbase, nvalue) : nbase;
    op3(L, T1_IR_NATCHK, 0, 0, consumed, 0, 0, 1);
    uint32_t p = point_here(L, T1_PT_SITE);
    if (!frame(L, p))
        return;
    ir->point[p].site = (uint16_t)site;
    ir->point[p].access_size = (uint8_t)bytes;
    ir->point[p].access_write = store;
    t1_value mem = read_var(L, T1_ELEMENT_MEM, L->b);
    int64_t imm = T1_ACCESS_IMM(bytes, store ? in->mem_release : in->mem_acquire, site);
    t1_value opnd[3] = {base, store ? value : mem, mem};
    uint32_t op = t1_ir_append(ir, L->b, store ? T1_IR_STORE : T1_IR_LOAD, 0, imm, opnd,
                               store ? 3 : 2, (uint16_t)L->i);
    if (op == T1_NO_OP)
        return;
    ir->op[op].point = p;
    ir->point[p].op = op;
    if (store) {
        set(L, T1_ELEMENT_MEM, T1_VALUE(op, T1_RES_STORE_mem));
    } else if (fp) {
        /* ldfs/ldfd/ldf8 (SDM Vol.1 §5.3.1): never NaTVal natively. */
        set(L, T1_ELEMENT_MEM, T1_VALUE(op, T1_RES_LOAD_mem));
        t1_value x = T1_VALUE(op, T1_RES_LOAD_value), sig = x,
                 meta = cst(L, T1_I64, IA64_FP_INTEGER_EXP);
        if (in->opcode != IA64_OP_LDF8)
            from_ieee(L, x, fp == 4, &sig, &meta);
        set_fr(L, ti->fr[0], in->operands.memory.destination, sig, meta, cst(L, T1_I1, 0),
               T1_VALUE_NONE, false);
    } else {
        set(L, T1_ELEMENT_MEM, T1_VALUE(op, T1_RES_LOAD_mem));
        set(L, T1_ELEMENT_GR(ti->dst), T1_VALUE(op, T1_RES_LOAD_value));
        set(L, T1_ELEMENT_NAT(ti->dst), cst(L, T1_I1, 0));
    }
    if (in->reg_base_update || in->imm_base_update) {
        set(L, T1_ELEMENT_GR(ti->base), op2(L, T1_IR_ADD, base, inc));
        set(L, T1_ELEMENT_NAT(ti->base), ninc);
    }
}

static void lift_fence(struct lifter *L, const struct t1_insn *ti)
{
    (void)ti;
    t1_value mem = read_var(L, T1_ELEMENT_MEM, L->b);
    set(L, T1_ELEMENT_MEM, op1(L, T1_IR_FENCE, 0, mem));
}

/* Loads, stores and mf under a qualifying predicate other than p0 sit in
 * a synthetic block entered by BRANCH qp: the head charges
 * the instructions before, the true block (the access) and the false
 * block each charge this one, and both join a new continuation block. */
static void predicated(struct lifter *L, const struct t1_insn *ti,
                       void (*body)(struct lifter *, const struct t1_insn *))
{
    struct t1_ir *ir = L->ir;
    if (!ti->qp) {
        body(L, ti);
        retire(L);
        return;
    }
    t1_value qp = pr(L, ti->qp);
    uint16_t t = t1_ir_new_block(ir, T1_BLK_SYNTHETIC), f = t1_ir_new_block(ir, T1_BLK_SYNTHETIC);
    if (ir->failed || !t1_ir_set_branch(ir, L->b, qp, t, f, (uint16_t)L->i))
        return;
    L->sealed[t] = L->sealed[f] = true;
    L->b = t;
    body(L, ti);
    retire(L);
    uint16_t done = L->b; /* body may continue in a later block */
    L->b = f;
    retire(L);
    uint16_t j = t1_ir_new_block(ir, T1_BLK_SYNTHETIC);
    if (ir->failed)
        return;
    t1_ir_set_jump(ir, done, j);
    t1_ir_set_jump(ir, f, j);
    L->sealed[j] = true;
    L->b = j;
}

static void lift_fpi(struct lifter *L, const struct t1_insn *ti)
{
    Ia64Opcode code = ti->insn.opcode;
    unsigned a = ti->fr[1], b = ti->fr[2], c = ti->fr[3];
    t1_value sig, meta, n;
    if (code == IA64_OP_GETF_SIG || code == IA64_OP_GETF_EXP) {
        sig = code == IA64_OP_GETF_SIG ? op1(L, T1_IR_GETF_SIG, 0, fr_sig(L, a)) : fr_meta(L, a);
        write_gr(L, ti->dst, sig, fr_nat(L, a), pr(L, ti->qp), !ti->qp);
    } else if (code == IA64_OP_GETF_S || code == IA64_OP_GETF_D) {
        /* SDM Vol.1 §5.3.2: the memory image (a NaTVal gives 0 with NaT). */
        sig = to_ieee(L, fr_sig(L, a), fr_meta(L, a), code == IA64_OP_GETF_S);
        write_gr(L, ti->dst, sig, fr_nat(L, a), pr(L, ti->qp), !ti->qp);
    } else {
        if (code == IA64_OP_SETF_S || code == IA64_OP_SETF_D) {
            n = nat(L, ti->src1);
            from_ieee(L, gr(L, ti->src1), code == IA64_OP_SETF_S, &sig, &meta);
        } else if (code == IA64_OP_SETF_SIG || code == IA64_OP_SETF_EXP) {
            t1_value value = gr(L, ti->src1);
            n = nat(L, ti->src1);
            sig = code == IA64_OP_SETF_SIG ? op1(L, T1_IR_SETF_SIG, 0, value)
                                           : cst(L, T1_I64, IA64_FP_INTEGER_BIT);
            meta = code == IA64_OP_SETF_SIG ? cst(L, T1_I64, IA64_FP_INTEGER_EXP)
                                            : op2(L, T1_IR_AND, value, cst(L, T1_I64, 0x3ffff));
        } else if (code == IA64_OP_FMERGE || code == IA64_OP_FMERGE_S ||
                   code == IA64_OP_FMERGE_SE) {
            /* SDM Vol.3 fmerge: the sign (.s/.ns) or sign and exponent (.se)
             * of f2 with f3; meta = sign << 17 | exponent. */
            const uint64_t sign = UINT64_C(1) << 17;
            sig = fr_sig(L, b);
            n = op2(L, T1_IR_OR1, fr_nat(L, a), fr_nat(L, b));
            if (code == IA64_OP_FMERGE_SE)
                meta = fr_meta(L, a);
            else {
                t1_value s2 = op2(L, T1_IR_AND, fr_meta(L, a), cst(L, T1_I64, sign));
                if (code == IA64_OP_FMERGE)
                    s2 = op2(L, T1_IR_XOR, s2, cst(L, T1_I64, sign));
                meta = op2(L, T1_IR_OR, s2,
                           op2(L, T1_IR_AND, fr_meta(L, b), cst(L, T1_I64, sign - 1)));
            }
        } else {
            t1_value av = fr_sig(L, a), bv = fr_sig(L, b);
            n = op2(L, T1_IR_OR1, fr_nat(L, a), fr_nat(L, b));
            meta = cst(L, T1_I64, IA64_FP_INTEGER_EXP);
            switch (code) {
            case IA64_OP_FSELECT:
                sig = op2(L, T1_IR_OR, op2(L, T1_IR_AND, bv, av),
                          op2(L, T1_IR_ANDCM, fr_sig(L, c), av));
                n = op2(L, T1_IR_OR1, n, fr_nat(L, c));
                break;
            /* SDM Vol.3 fpack, fpmerge, fpabs, fpnegabs (ia64/fp.c, fp_parallel.c):
             * single images and lane sign/exponent bits. */
            case IA64_OP_FPACK:
                sig =
                    op2(L, T1_IR_OR, op1(L, T1_IR_SHL_IMM, 32, to_ieee(L, av, fr_meta(L, a), true)),
                        to_ieee(L, bv, fr_meta(L, b), true));
                break;
            case IA64_OP_FPMERGE:
            case IA64_OP_FPMERGE_S:
            case IA64_OP_FPMERGE_SE: {
                t1_value mask = cst(L, T1_I64,
                                    code == IA64_OP_FPMERGE_SE ? UINT64_C(0xff800000ff800000)
                                                               : UINT64_C(0x8000000080000000));
                t1_value high = op2(L, T1_IR_AND, av, mask);
                if (code == IA64_OP_FPMERGE)
                    high = op2(L, T1_IR_XOR, high, mask);
                sig = op2(L, T1_IR_OR, high, op2(L, T1_IR_ANDCM, bv, mask));
                break;
            }
            case IA64_OP_FPABS:
            case IA64_OP_FPNEGABS:
                n = fr_nat(L, a);
                sig = code == IA64_OP_FPABS
                          ? op2(L, T1_IR_AND, av, cst(L, T1_I64, UINT64_C(0x7fffffff7fffffff)))
                          : op2(L, T1_IR_OR, av, cst(L, T1_I64, UINT64_C(0x8000000080000000)));
                break;
            case IA64_OP_XMA_L:
            case IA64_OP_XMA_H:
            case IA64_OP_XMA_HU:
            case IA64_OP_XMPY_HU:
                sig = op3(L,
                          code == IA64_OP_XMA_L   ? T1_IR_XMA_L
                          : code == IA64_OP_XMA_H ? T1_IR_XMA_H
                                                  : T1_IR_XMA_HU,
                          0, 0, bv, fr_sig(L, c), av, 3);
                n = op2(L, T1_IR_OR1, n, fr_nat(L, c));
                break;
            case IA64_OP_FAND:
                sig = op2(L, T1_IR_AND, av, bv);
                break;
            case IA64_OP_FANDCM:
                sig = op2(L, T1_IR_ANDCM, av, bv);
                break;
            case IA64_OP_FOR:
                sig = op2(L, T1_IR_OR, av, bv);
                break;
            case IA64_OP_FXOR:
                sig = op2(L, T1_IR_XOR, av, bv);
                break;
            case IA64_OP_FMIX_LR:
            case IA64_OP_FMIX_L:
            case IA64_OP_FMIX_R: {
                t1_value hi =
                    op1(L, T1_IR_EXTRU, T1_FIELD_IMM(code == IA64_OP_FMIX_R ? 0 : 32, 32), av);
                t1_value lo =
                    op1(L, T1_IR_EXTRU, T1_FIELD_IMM(code == IA64_OP_FMIX_L ? 32 : 0, 32), bv);
                sig = op2(L, T1_IR_OR, op1(L, T1_IR_SHL_IMM, 32, hi), lo);
                break;
            }
            case IA64_OP_FSXT_L:
            case IA64_OP_FSXT_R: {
                unsigned sh = code == IA64_OP_FSXT_L ? 32 : 0;
                t1_value hi = op1(L, T1_IR_EXTR, T1_FIELD_IMM(sh + 31, 1), av);
                t1_value lo = op1(L, T1_IR_EXTRU, T1_FIELD_IMM(sh, 32), bv);
                sig = op2(L, T1_IR_OR, op1(L, T1_IR_SHL_IMM, 32, hi), lo);
                break;
            }
            default:
                t1_ir_fail(L->ir, "unhandled native FP-int");
                return;
            }
        }
        sig = select_(L, T1_I64, n, cst(L, T1_I64, 0), sig);
        meta = select_(L, T1_I64, n, cst(L, T1_I64, IA64_FP_NAT_EXP), meta);
        write_fr(L, ti, sig, meta);
    }
    retire(L);
}

/* br.call (SDM Vol.3 br; ia64/rse.c ia64_rse_call) on its taken path:
 * ar.pfs = CFM | ar.ec{5:0} << 52 | PSR.cpl << 62 and the link BR = the
 * next bundle; the callee's frame is the taken path's frame state
 * (formation). */
static void call_effects(struct lifter *L, const struct t1_insn *ti)
{
    uint64_t cfm = L->f->fs[ti->fs].cfm, cpl = (L->f->c.psr >> 32) & 3;
    t1_value ec = op2(L, T1_IR_AND, read_var(L, T1_ELEMENT_EC, L->b), cst(L, T1_I64, 63));
    set(L, T1_ELEMENT_PFS,
        op2(L, T1_IR_OR, cst(L, T1_I64, cfm | cpl << 62), op1(L, T1_IR_SHL_IMM, 52, ec)));
    set(L, T1_ELEMENT_BR(ti->breg), cst(L, T1_I64, ti->insn.address + 16));
    set(L, T1_ELEMENT_MEM, op1(L, T1_IR_BSPCALL, 0, read_var(L, T1_ELEMENT_MEM, L->b)));
}

/* Matched or observed br.ret (formation supplied the caller; ia64/rse.c
 * ia64_rse_return): with the BR at the admitted target and ar.pfs
 * holding the caller's frame (PFM) at no lower privilege (PPL <= cpl, so
 * PSR.cpl stays), it returns into the caller's frame state, ar.ec =
 * PFS.pec; otherwise it leaves before effect and the lower tier returns.
 * Under a predicate the false path retires it and falls through. */
static void lift_ret(struct lifter *L, const struct t1_insn *ti)
{
    struct t1_ir *ir = L->ir;
    uint64_t cpl = (L->f->c.psr >> 32) & 3;
    if (ti->qp) {
        t1_value qp = pr(L, ti->qp);
        uint16_t t = t1_ir_new_block(ir, T1_BLK_SYNTHETIC),
                 f = t1_ir_new_block(ir, T1_BLK_SYNTHETIC);
        if (ir->failed || !t1_ir_set_branch(ir, L->b, qp, t, f, (uint16_t)L->i))
            return;
        L->sealed[t] = L->sealed[f] = true;
        L->b = f;
        retire(L);
        uint16_t fall = fall_block(L);
        if (ir->failed)
            return;
        t1_ir_set_jump(ir, f, fall);
        if (L->i + 1 >= L->f->ninsns || ti->fall_exit)
            fill_exit(L, fall, ti->next_ip, ti->next_slot, ti->fs, L->f->end_class);
        L->b = t;
    }
    t1_value pfs = read_var(L, T1_ELEMENT_PFS, L->b),
             br = read_var(L, T1_ELEMENT_BR(ti->breg), L->b);
    t1_value ok = op2(L, T1_IR_AND1,
                      op3(L, T1_IR_CMP, 0, T1_CMP_IMM(T1_CMP_EQ, 64),
                          op2(L, T1_IR_AND, br, cst(L, T1_I64, ~UINT64_C(15))),
                          cst(L, T1_I64, ti->target), 0, 2),
                      op3(L, T1_IR_CMP, 0, T1_CMP_IMM(T1_CMP_EQ, 64),
                          op2(L, T1_IR_AND, pfs, cst(L, T1_I64, (UINT64_C(1) << 38) - 1)),
                          cst(L, T1_I64, L->f->fs[ti->fs_taken].cfm), 0, 2));
    if (cpl < 3)
        ok = op2(L, T1_IR_AND1, ok,
                 op3(L, T1_IR_CMP, 0, T1_CMP_IMM(T1_CMP_LTU, 64), pfs,
                     cst(L, T1_I64, (cpl + 1) << 62), 0, 2));
    uint16_t done = t1_ir_new_block(ir, T1_BLK_SYNTHETIC), fail = t1_ir_new_block(ir, 0);
    if (ir->failed || !t1_ir_set_branch(ir, L->b, ok, done, fail, (uint16_t)L->i))
        return;
    fill_exit(L, fail, ti->insn.address, ti->insn.slot, ti->fs, T1_END_RET);
    L->sealed[done] = true;
    L->b = done;
    set(L, T1_ELEMENT_EC, op1(L, T1_IR_EXTRU, T1_FIELD_IMM(52, 6), pfs));
    retire(L);
    take_path(L, done);
    L->b = T1_NO_BLOCK;
}

static void lift_insn(struct lifter *L, const struct t1_insn *ti)
{
    switch (ti->cls) {
    case T1_IC_ALU:
        lift_alu(L, ti);
        break;
    case T1_IC_CMP:
        lift_cmp(L, ti);
        break;
    case T1_IC_LC: {
        const Ia64Instruction *in = &ti->insn;
        if (in->opcode == IA64_OP_MOV_ARGR) {
            write_gr(L, ti->dst, read_var(L, T1_ELEMENT_LC, L->b), cst(L, T1_I1, 0), pr(L, ti->qp),
                     ti->qp == 0);
        } else {
            t1_value value = in->opcode == IA64_OP_MOV_IMMAR
                                 ? cst(L, T1_I64, in->operands.system.immediate)
                                 : gr(L, ti->dst);
            if (in->opcode == IA64_OP_MOV_GRAR)
                op1(L, T1_IR_NATCHK, 0, nat(L, ti->dst));
            if (ti->qp)
                value = select_(L, T1_I64, pr(L, ti->qp), value, read_var(L, T1_ELEMENT_LC, L->b));
            set(L, T1_ELEMENT_LC, value);
        }
        retire(L);
        break;
    }
    case T1_IC_FPI:
        lift_fpi(L, ti);
        break;
    case T1_IC_GENERAL:
        lift_general(L, ti);
        break;
    case T1_IC_HELPER:
        lift_helper(L, ti);
        break;
    case T1_IC_LOAD:
    case T1_IC_STORE:
        predicated(L, ti, lift_access);
        break;
    case T1_IC_FENCE:
        predicated(L, ti, lift_fence);
        break;
    case T1_IC_NOP:
        retire(L);
        break;
    case T1_IC_CHKA: /* SDM Vol.3 pp.3:35-3:36 permits unconditional ALAT miss.
                     * Preserve the recovery branch without reading GR/FR/NaT. */
    case T1_IC_BR: {
        t1_value qp = pr(L, ti->qp);
        retire(L);
        branch(L, qp, ti->qp == 0);
        break;
    }
    case T1_IC_ALLOC:
        lift_alloc(L, ti);
        break;
    case T1_IC_CALL: {
        if (ti->insn.opcode == IA64_OP_BR_CALL_INDIRECT) {
            t1_value target = read_var(L, T1_ELEMENT_BR(ti->insn.operands.branch.target & 7), L->b);
            t1_value ok = op3(L, T1_IR_CMP, 0, T1_CMP_IMM(T1_CMP_EQ, 64),
                              op2(L, T1_IR_AND, target, cst(L, T1_I64, ~UINT64_C(15))),
                              cst(L, T1_I64, ti->target), 0, 2);
            uint16_t done = t1_ir_new_block(L->ir, T1_BLK_SYNTHETIC),
                     fail = t1_ir_new_block(L->ir, 0);
            if (L->ir->failed || !t1_ir_set_branch(L->ir, L->b, ok, done, fail, (uint16_t)L->i))
                return;
            fill_exit(L, fail, ti->insn.address, ti->insn.slot, ti->fs, T1_END_CALL);
            L->sealed[done] = true;
            L->b = done;
        }
        t1_value qp = pr(L, ti->qp);
        retire(L);
        transfer(L, qp, ti->qp == 0, call_effects);
        break;
    }
    case T1_IC_RET:
        lift_ret(L, ti);
        break;
    case T1_IC_CHK: {
        /* chk.s (ia64/load_store.c): to the recovery code when the GR's
         * NaT bit or the FR's NaTVal is set (natguard.c: a GR's NaT is read
         * at run time). */
        t1_value bit = !ti->insn.check_fp ? nat(L, ti->source)
                       : ti->source < 2   ? cst(L, T1_I1, 0)
                                          : fr_nat(L, ti->source);
        t1_value cond = ti->qp ? op2(L, T1_IR_AND1, pr(L, ti->qp), bit) : bit;
        retire(L);
        branch(L, cond, false);
        break;
    }
    case T1_IC_BRMOV: {
        /* mov r = b, mov b = r (ia64/system.c): a NaT source faults
         * (Register NaT Consumption). */
        const Ia64Instruction *in = &ti->insn;
        if (in->opcode == IA64_OP_MOV_BRGR) {
            write_gr(L, ti->dst, read_var(L, T1_ELEMENT_BR(ti->breg), L->b), cst(L, T1_I1, 0),
                     pr(L, ti->qp), ti->qp == 0);
        } else {
            op1(L, T1_IR_NATCHK, 0, nat(L, ti->dst));
            t1_value value = gr(L, ti->dst);
            if (ti->qp)
                value = select_(L, T1_I64, pr(L, ti->qp), value,
                                read_var(L, T1_ELEMENT_BR(ti->breg), L->b));
            set(L, T1_ELEMENT_BR(ti->breg), value);
        }
        retire(L);
        break;
    }
    case T1_IC_CLOOP: {
        /* br.cloop ignores qp (ia64_execute's `loop`). */
        t1_value lc = read_var(L, T1_ELEMENT_LC, L->b);
        uint32_t op = t1_ir_append(L->ir, L->b, T1_IR_CLOOP, 0, 0, &lc, 1, (uint16_t)L->i);
        if (op == T1_NO_OP)
            return;
        set(L, T1_ELEMENT_LC, T1_VALUE(op, T1_RES_CLOOP_next));
        retire(L);
        branch(L, T1_VALUE(op, T1_RES_CLOOP_taken), false);
        break;
    }
    default:
        t1_ir_fail(L->ir, "unknown instruction class");
    }
}

/* Elements each instruction may write, and W. */
static void element_sets(const struct t1_form *f, struct t1_ir *ir)
{
    for (unsigned i = 0; i < f->ninsns; ++i) {
        const struct t1_insn *ti = &f->insn[i];
        const Ia64Instruction *in = &ti->insn;
        if (f->c.psr & IA64_PSR_IC)
            t1_elem_add(ir->frame_set, T1_ELEMENT_LSB);
        switch (ti->cls) {
        case T1_IC_LOAD:
            if (t1_fp_access(in->opcode)) {
                t1_elem_add(ir->frame_set, T1_ELEMENT_FR(ti->fr[0]));
                t1_elem_add(ir->frame_set, T1_ELEMENT_FR_META(ti->fr[0]));
                t1_elem_add(ir->frame_set, T1_ELEMENT_PSR_MF);
                break;
            }
            /* fall through */
        case T1_IC_ALU:
            t1_elem_add(ir->frame_set, T1_ELEMENT_GR(ti->dst));
            t1_elem_add(ir->frame_set, T1_ELEMENT_NAT(ti->dst));
            break;
        case T1_IC_CMP:
            if (ti->p1)
                t1_elem_add(ir->frame_set, T1_ELEMENT_PR(ti->p1));
            if (ti->p2)
                t1_elem_add(ir->frame_set, T1_ELEMENT_PR(ti->p2));
            break;
        case T1_IC_LC:
            if (in->opcode == IA64_OP_MOV_ARGR) {
                t1_elem_add(ir->frame_set, T1_ELEMENT_GR(ti->dst));
                t1_elem_add(ir->frame_set, T1_ELEMENT_NAT(ti->dst));
            } else
                t1_elem_add(ir->frame_set, T1_ELEMENT_LC);
            break;
        case T1_IC_FPI: {
            if (in->opcode == IA64_OP_GETF_SIG || in->opcode == IA64_OP_GETF_EXP ||
                in->opcode == IA64_OP_GETF_S || in->opcode == IA64_OP_GETF_D) {
                t1_elem_add(ir->frame_set, T1_ELEMENT_GR(ti->dst));
                t1_elem_add(ir->frame_set, T1_ELEMENT_NAT(ti->dst));
            } else {
                t1_elem_add(ir->frame_set, T1_ELEMENT_FR(ti->fr[0]));
                t1_elem_add(ir->frame_set, T1_ELEMENT_FR_META(ti->fr[0]));
                t1_elem_add(ir->frame_set, T1_ELEMENT_PSR_MF);
            }
            break;
        }
        case T1_IC_GENERAL: {
            uint16_t e[T1_N_ELEMENTS];
            unsigned n = t1_helper_elements(ti, true, e);
            for (unsigned k = 0; k < n; ++k)
                t1_elem_add(ir->w_set, e[k]);
            n = t1_helper_elements(ti, false, e);
            /* Materializing helper operands also writes their homes. */
            for (unsigned k = 0; k < n; ++k)
                t1_elem_add(ir->w_set, e[k]);
            break;
        }
        case T1_IC_HELPER: {
            uint16_t e[8];
            unsigned n = helper_read_set(ti, e);
            for (unsigned k = 0; k < n; ++k)
                t1_elem_add(ir->w_set, e[k]);
            break;
        }
        case T1_IC_CLOOP:
            t1_elem_add(ir->frame_set, T1_ELEMENT_LC);
            break;
        case T1_IC_ALLOC:
            /* The helper writes r1's home (W) and reads ar.pfs's. */
            t1_elem_add(ir->w_set, T1_ELEMENT_GR(ti->dst));
            t1_elem_add(ir->w_set, T1_ELEMENT_NAT(ti->dst));
            t1_elem_add(ir->frame_set, T1_ELEMENT_PFS);
            break;
        case T1_IC_CALL:
            t1_elem_add(ir->frame_set, T1_ELEMENT_PFS);
            t1_elem_add(ir->frame_set, T1_ELEMENT_BR(ti->breg));
            break;
        case T1_IC_RET:
            t1_elem_add(ir->frame_set, T1_ELEMENT_EC);
            break;
        case T1_IC_BRMOV:
            if (in->opcode == IA64_OP_MOV_GRBR)
                t1_elem_add(ir->frame_set, T1_ELEMENT_BR(ti->breg));
            else {
                t1_elem_add(ir->frame_set, T1_ELEMENT_GR(ti->dst));
                t1_elem_add(ir->frame_set, T1_ELEMENT_NAT(ti->dst));
            }
            break;
        default:
            break;
        }
        if ((ti->cls == T1_IC_LOAD || ti->cls == T1_IC_STORE) &&
            (in->reg_base_update || in->imm_base_update)) {
            t1_elem_add(ir->frame_set, T1_ELEMENT_GR(ti->base));
            t1_elem_add(ir->frame_set, T1_ELEMENT_NAT(ti->base));
        }
    }
    for (unsigned w = 0; w < T1_ELEMENT_WORDS; ++w)
        ir->frame_set[w] |= ir->w_set[w];
}

/* Global trivial-parameter removal to a fixed point (Braun). */
static void remove_trivial_params(struct t1_ir *ir)
{
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned b = 0; b < ir->nblocks; ++b) {
            struct t1_block *blk = &ir->block[b];
            if (blk->flags & T1_BLK_DEAD)
                continue;
            unsigned pos = 0;
            for (uint32_t o = blk->first; o != T1_NO_OP && ir->op[o].code == T1_IR_PARAM;
                 o = ir->op[o].next, ++pos) {
                if (ir->op[o].fwd != T1_VALUE_NONE || (ir->op[o].flags & T1_OPF_DEAD))
                    continue;
                t1_value self = T1_VALUE(o, 0), same = T1_VALUE_NONE;
                bool trivial = true;
                for (unsigned p = 0; p < blk->npred && trivial; ++p) {
                    const struct t1_edge *e = &ir->block[blk->pred[p]].succ[blk->pred_slot[p]];
                    t1_value a = pos < e->nargs
                                     ? t1_ir_resolve(ir, ir->use[e->args_first + pos].value)
                                     : T1_VALUE_NONE;
                    if (a == self || a == same)
                        continue;
                    if (same != T1_VALUE_NONE || a == T1_VALUE_NONE)
                        trivial = false;
                    else
                        same = a;
                }
                if (trivial && same != T1_VALUE_NONE) {
                    t1_ir_forward(ir, o, same);
                    changed = true;
                }
            }
        }
    }
}

bool t1_lift(const struct t1_form *f, struct t1_ir *ir, unsigned *fail_insn)
{
    size_t pages = (sizeof(struct lifter) + 4095) / 4096;
    struct lifter *L = t1_ws_alloc(ir->ws, pages, 0);
    *fail_insn = f->ninsns;
    t1_ir_init(ir, f);
    if (!L)
        return t1_ir_fail(ir, "no memory for lifting");
    L->f = f;
    L->ir = ir;
    L->b = T1_NO_BLOCK;
    L->i = 0;
    L->nelem = 0;
    L->rows = 0;
    memset(L->sealed, 0, sizeof(L->sealed));
    for (unsigned f = 0; f < 128; ++f)
        L->fr_known_sig[f] = L->fr_known_nat[f] = T1_VALUE_NONE;
    for (unsigned i = 0; i < T1_MAX_INSNS; ++i)
        L->block_of[i] = T1_NO_BLOCK;
    element_sets(f, ir);
    for (unsigned e = 1; e < T1_N_ELEMENTS; ++e)
        if (t1_elem_has(ir->frame_set, e))
            L->elem[L->nelem++] = (uint16_t)e;

    /* Loop headers: the targets of back-edges (the head when f->loop). */
    bool header[T1_MAX_INSNS] = {false};
    for (unsigned i = 0; i < f->ninsns; ++i)
        if (f->insn[i].edge == T1_EDGE_BACK)
            header[f->insn[i].target_insn] = true;
    t1_ir_new_block(ir, 0); /* T1_ENTRY_BLOCK */
    t1_ir_new_block(ir, 0); /* T1_HEADER_BLOCK */
    t1_ir_set_jump(ir, T1_ENTRY_BLOCK, T1_HEADER_BLOCK);
    L->sealed[T1_ENTRY_BLOCK] = true;
    L->sealed[T1_HEADER_BLOCK] = !header[0];
    L->block_of[0] = T1_HEADER_BLOCK;
    L->b = T1_HEADER_BLOCK;
    for (unsigned i = 0; i < f->ninsns && !ir->failed; ++i) {
        const struct t1_insn *ti = &f->insn[i];
        L->i = i;
        if (ti->leader && i) {
            uint16_t nb = block_for(L, i);
            if (ir->failed)
                break;
            if (L->b != T1_NO_BLOCK)
                t1_ir_set_jump(ir, L->b, nb);
            L->b = nb;
            L->sealed[nb] = !header[i];
            if (!ir->block[nb].npred)
                L->b = T1_NO_BLOCK; /* unreachable */
        }
        if (L->b == T1_NO_BLOCK)
            continue;
        lift_insn(L, ti);
    }
    if (!ir->failed && L->b != T1_NO_BLOCK) {
        /* Fall-off exit at the first instruction not in the region. */
        uint16_t x = t1_ir_new_block(ir, 0);
        L->i = f->ninsns - 1;
        if (!ir->failed && t1_ir_set_jump(ir, L->b, x))
            fill_exit(L, x, f->end_ip, f->end_slot, f->end_fs, f->end_class);
    }
    /* Seal the loop headers: every parameter a header has now was made
     * while it was unsealed and gets its arguments; sealing one header may
     * add incomplete parameters to another, sealed later. */
    for (uint16_t b = 0; b < ir->nblocks && !ir->failed; ++b) {
        if (L->sealed[b])
            continue;
        L->sealed[b] = true;
        for (uint32_t o = ir->block[b].first;
             o != T1_NO_OP && ir->op[o].code == T1_IR_PARAM && !ir->failed; o = ir->op[o].next)
            add_param_args(L, b, (uint16_t)ir->op[o].imm);
    }
    if (ir->failed)
        *fail_insn = L->i;
    t1_ws_free(ir->ws, L, pages);
    if (ir->failed)
        return false;
    remove_trivial_params(ir);
    t1_ir_resolve_all(ir);
    t1_ir_compact_params(ir);
    t1_ir_remove_unreachable(ir);
    return true;
}
