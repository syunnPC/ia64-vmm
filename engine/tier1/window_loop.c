/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/tier1/window_loop.h"
#include "ia64vmm/string.h"

static t1_value operand(const struct t1_ir *ir, unsigned o, unsigned n)
{
    return t1_ir_resolve(ir, t1_ir_opnd(ir, o, n)->value);
}

static bool phi_inputs(const struct t1_ir *ir, uint16_t header, uint16_t back, t1_value v,
                       t1_value *entry, t1_value *feedback)
{
    if (v == T1_VALUE_NONE || T1_VALUE_RESULT(v))
        return false;
    unsigned o = T1_VALUE_OP(v), pos = 0;
    if (ir->op[o].code != T1_IR_PARAM || ir->op[o].block != header || ir->op[o].type != T1_I64)
        return false;
    uint32_t at = ir->block[header].first;
    for (; at != o && at != T1_NO_OP && ir->op[at].code == T1_IR_PARAM; at = ir->op[at].next)
        ++pos;
    if (at != o || ir->block[header].npred != 2)
        return false;
    *entry = *feedback = T1_VALUE_NONE;
    for (unsigned k = 0; k < 2; ++k) {
        unsigned from = ir->block[header].pred[k];
        const struct t1_edge *e = &ir->block[from].succ[ir->block[header].pred_slot[k]];
        if (pos >= e->nargs)
            return false;
        t1_value in = t1_ir_resolve(ir, ir->use[e->args_first + pos].value);
        if (from == back)
            *feedback = in;
        else
            *entry = in;
    }
    return *entry != T1_VALUE_NONE && *feedback != T1_VALUE_NONE;
}

bool t1_window_loop_plan(const struct t1_mir *m, struct t1_window_loop *p)
{
    memset(p, 0, sizeof(*p));
    if (m->flags || m->window_bits < 12 || m->window_bits > 53)
        return false;
    const struct t1_ir *ir = m->ir;
    uint16_t h = T1_NO_BLOCK;
    unsigned depth = 0;
    for (unsigned b = 0; b < m->nblocks; ++b)
        if (m->block[b].depth > depth) {
            h = m->block[b].loop_header;
            depth = m->block[b].depth;
        }
    if (h == T1_NO_BLOCK || h >= ir->nblocks)
        return false;
    const struct t1_block *head = &ir->block[h];
    uint32_t term = t1_ir_terminator(ir, h);
    if (term == T1_NO_OP || ir->op[term].code != T1_IR_BRANCH || head->nsucc != 2)
        return false;
    uint16_t back = head->succ[0].target;
    if (back == h || back >= ir->nblocks || head->succ[1].target == h ||
        head->succ[1].target == back)
        return false;
    const struct t1_block *tail = &ir->block[back];
    uint32_t bt = t1_ir_terminator(ir, back);
    if (bt == T1_NO_OP || ir->op[bt].code != T1_IR_BACKEDGE || tail->npred != 1 ||
        tail->pred[0] != h || tail->nsucc != 1 || tail->succ[0].target != h || tail->first != bt)
        return false;
    uint16_t dom[T1_MAX_BLOCKS];
    if (!t1_ir_dominators(ir, dom) || !t1_ir_dominates(dom, h, back))
        return false;
    t1_value cond = operand(ir, term, 0);
    unsigned co = T1_VALUE_OP(cond);
    if (T1_VALUE_RESULT(cond) != 1 || ir->op[co].code != T1_IR_CLOOP || ir->op[co].block != h)
        return false;
    t1_value seed, feedback;
    if (!phi_inputs(ir, h, back, operand(ir, co, 0), &seed, &feedback) ||
        feedback != T1_VALUE(co, 0) || T1_VALUE_RESULT(seed) ||
        ir->op[T1_VALUE_OP(seed)].code != T1_IR_CONST)
        return false;
    uint64_t lc = (uint64_t)ir->op[T1_VALUE_OP(seed)].imm;
    if (lc > 63)
        return false;
    unsigned nsite = 0;
    p->header = p->entry = UINT16_MAX;
    /* MIR edge blocks belong to the loop only when both ends do. */
    for (unsigned b = 0; b < m->nblocks; ++b) {
        const struct t1_mir_block *blk = &m->block[b];
        if (blk->loop_header != h)
            continue;
        p->member[b] = true;
        p->insns += blk->count;
        if (p->insns > 128 ||
            (blk->ir_block != T1_NO_BLOCK && blk->ir_block != h && blk->ir_block != back))
            return false;
        if (blk->ir_block == h)
            p->header = (uint16_t)b;
        for (uint32_t i = blk->first; i < blk->first + blk->count; ++i) {
            const struct t1_mir_insn *in = &m->insn[i];
            if (in->op == T1_MIR_SITE) {
                if (nsite == 2 || blk->ir_block != h || in->opnd[1].kind != T1_MO_VREG)
                    return false;
                const struct t1_mir_vreg *vr = &m->vreg[in->opnd[1].vreg];
                t1_value address = t1_ir_resolve(ir, vr->value);
                if (!(vr->flags & T1_VR_LOOP_PARAM) ||
                    !phi_inputs(ir, h, back, address, &seed, &feedback) ||
                    T1_VALUE_RESULT(feedback))
                    return false;
                unsigned add = T1_VALUE_OP(feedback);
                if (ir->op[add].code != T1_IR_ADD || ir->op[add].block != h)
                    return false;
                t1_value step = operand(ir, add, operand(ir, add, 0) == address ? 1 : 0);
                if ((operand(ir, add, 0) != address && operand(ir, add, 1) != address) ||
                    T1_VALUE_RESULT(step) || ir->op[T1_VALUE_OP(step)].code != T1_IR_CONST)
                    return false;
                uint64_t stride = (uint64_t)ir->op[T1_VALUE_OP(step)].imm;
                unsigned bytes = (unsigned)((in->opnd[2].imm >> 8) & 255);
                if (!stride || stride > INT32_MAX ||
                    (bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) || stride % bytes ||
                    stride > (INT32_MAX - (bytes - 1u)) / (lc ? lc : 1) ||
                    vr->header_loc.kind != T1_LOC_REG || vr->header_loc.reg == T1_R11)
                    return false;
                uint64_t span = stride * lc + bytes - 1;
                if (span >= (UINT64_C(1) << m->window_bits))
                    return false;
                p->access[nsite].point = in->point;
                p->access[nsite].site = (uint16_t)((uint64_t)in->opnd[2].imm >> 16);
                p->access[nsite].reg = vr->header_loc.reg;
                p->access[nsite].bytes = (uint8_t)bytes;
                p->access[nsite++].span = (uint32_t)span;
            } else if (t1_mir_is_pseudo(in->op)) {
                if (in->op != T1_MIR_PCOPY && in->op != T1_MIR_SPILL && in->op != T1_MIR_RELOAD &&
                    in->op != T1_MIR_CHARGE && in->op != T1_MIR_CHECKPOINT &&
                    in->op != T1_MIR_BLOCK)
                    return false;
            } else if (t1_x86_info(in->op)->attr & T1_XA_CALL)
                return false;
        }
    }
    if (nsite != 2 || p->header == UINT16_MAX)
        return false;
    unsigned entries = 0;
    for (unsigned b = 0; b < m->nblocks; ++b)
        for (unsigned k = 0; k < m->block[b].nsucc; ++k) {
            unsigned to = m->block[b].succ[k];
            if (!p->member[b] && p->member[to]) {
                const struct t1_mir_block *blk = &m->block[b];
                if (++entries != 1 || to != p->header || blk->nsucc != 1 || blk->count <= 1 ||
                    blk->term != blk->first + blk->count - 1 || m->insn[blk->term].op != T1_X86_JMP)
                    return false;
                p->entry = (uint16_t)b;
            }
        }
    if (entries != 1)
        return false;
    /* The fast SITE has no flag-writing checks. Only actual native
     * definitions count until CHARGE's SUB overwrites all incoming flags.
     * Variable and zero-count shifts may preserve their incoming flags. */
    uint16_t defined = 0;
    const struct t1_mir_block *blk = &m->block[p->header];
    for (uint32_t i = blk->first; i < blk->first + blk->count; ++i) {
        const struct t1_mir_insn *in = &m->insn[i];
        if (in->op == T1_MIR_CHARGE)
            return true;
        if (t1_mir_is_pseudo(in->op))
            continue;
        const struct t1_x86_info *info = t1_x86_info(in->op);
        if ((info->attr & T1_XA_BRANCH) || (info->flags_read & ~defined))
            return false;
        bool shift = in->op == T1_X86_SHL64_RCL || in->op == T1_X86_SHR64_RCL ||
                     in->op == T1_X86_SAR64_RCL ||
                     ((in->op == T1_X86_SHL64_RI || in->op == T1_X86_SHR64_RI ||
                       in->op == T1_X86_SAR64_RI) &&
                      !(in->opnd[1].imm & 63));
        if (!shift)
            defined = (defined & ~info->flags_undef) | info->flags_def;
    }
    return false;
}
