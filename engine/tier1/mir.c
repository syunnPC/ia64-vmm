/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The machine function container (codegen.h): allocation of the MIR
 * arrays, appending instructions, vregs and copies, and printing for
 * diagnostics. Freestanding.
 */
#include "engine/tier1/codegen.h"
#include "ia64vmm/hal.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

static size_t pages_for(size_t bytes)
{
    return (bytes + 4095) / 4096;
}

static size_t insn_bytes(unsigned n)
{
    return (size_t)n * sizeof(struct t1_mir_insn);
}

/* The instruction, memory-register, vreg and copy arrays are written as
 * they grow (t1_mir_append, t1_mir_new_vreg, t1_mir_new_copies, the
 * allocator's rewrite) and read below their counts: they are not cleared. */
struct t1_mir *t1_mir_create(const struct t1_ir *ir)
{
    struct t1_workspace *ws = ir->ws;
    struct t1_mir *m = t1_ws_alloc(ws, pages_for(sizeof(*m)), HAL_PAGE_ZERO);
    if (!m)
        return NULL;
    m->ir = ir;
    m->insn_cap = T1_MIR_MAX_INSNS;
    m->copy_cap = T1_MIR_MAX_COPIES;
    /* Each point contributes its frame at most once. Ordinary operands
     * and edge arguments in the IR use pool need no MIR frame storage.
     * Keep the old use-pool limit as well, including for malformed IR. */
    uint64_t frames = 0;
    for (unsigned p = 0; p < ir->npoints; ++p)
        frames += ir->point[p].frame_count;
    if (frames > ir->nuses)
        frames = ir->nuses;
    m->frame_cap = frames ? (unsigned)frames : 1;
    m->insn = t1_ws_alloc(ws, pages_for(insn_bytes(m->insn_cap)), 0);
    m->memreg = t1_ws_alloc(ws, pages_for(m->insn_cap * sizeof(*m->memreg)), 0);
    m->vreg = t1_ws_alloc(ws, pages_for(T1_MIR_MAX_VREGS * sizeof(struct t1_mir_vreg)), 0);
    m->copy = t1_ws_alloc(ws, pages_for(m->copy_cap * sizeof(struct t1_mir_copy)), 0);
    /* Selection writes every live frame field before allocation reads it.
     * Keep location padding zero: locations are copied into frame tables. */
    m->frame = t1_ws_alloc(ws, pages_for(m->frame_cap * sizeof(struct t1_mir_frame)), 0);
    m->frame_loc = t1_ws_alloc(ws, pages_for(m->frame_cap * sizeof(struct t1_loc)), HAL_PAGE_ZERO);
    m->point = t1_ws_alloc(
        ws, pages_for((ir->npoints ? ir->npoints : 1) * sizeof(struct t1_mir_point)), 0);
    if (!m->insn || !m->memreg || !m->vreg || !m->copy || !m->frame || !m->frame_loc || !m->point) {
        t1_mir_destroy(m);
        return NULL;
    }
    for (unsigned p = 0; p < ir->npoints; ++p)
        m->point[p] = (struct t1_mir_point){T1_MIR_NONE, T1_MIR_NONE, 0, 0, ir->point[p].kind};
    m->allocatable = T1_ALLOCATABLE;
    m->pin_reg = T1_X86_NOREG;
    m->fail_insn = T1_NO_INSN;
    return m;
}

void t1_mir_destroy(struct t1_mir *m)
{
    if (!m)
        return;
    const struct t1_ir *ir = m->ir;
    struct t1_workspace *ws = ir->ws;
    t1_ws_free(ws, m->point,
               pages_for((ir->npoints ? ir->npoints : 1) * sizeof(struct t1_mir_point)));
    t1_ws_free(ws, m->frame_loc, pages_for(m->frame_cap * sizeof(struct t1_loc)));
    t1_ws_free(ws, m->frame, pages_for(m->frame_cap * sizeof(struct t1_mir_frame)));
    t1_ws_free(ws, m->copy, pages_for(m->copy_cap * sizeof(struct t1_mir_copy)));
    t1_ws_free(ws, m->vreg, pages_for(T1_MIR_MAX_VREGS * sizeof(struct t1_mir_vreg)));
    t1_ws_free(ws, m->memreg, pages_for(m->insn_cap * sizeof(*m->memreg)));
    t1_ws_free(ws, m->insn, pages_for(insn_bytes(m->insn_cap)));
    t1_ws_free(ws, m, pages_for(sizeof(*m)));
}

bool t1_mir_fail(struct t1_mir *m, const char *reason)
{
    if (!m->failed) {
        m->failed = true;
        m->fail_reason = reason;
    }
    return false;
}

struct t1_mir_insn *t1_mir_append(struct t1_mir *m, uint16_t op)
{
    if (m->ninsns >= m->insn_cap) {
        t1_mir_fail(m, "MIR instruction limit");
        return NULL;
    }
    struct t1_mir_insn *i = &m->insn[m->ninsns++];
    memset(i, 0, sizeof(*i));
    i->op = op;
    i->tie_def = i->tie_use = 0xff;
    i->cc = T1_CC_COUNT;
    i->point = UINT32_MAX;
    i->mem.base_kind = i->mem.index_kind = T1_MO_NONE;
    for (unsigned k = 0; k < T1_MIR_MAX_OPNDS; ++k) {
        i->opnd[k].kind = T1_MO_NONE;
        i->opnd[k].fixed = T1_X86_NOREG;
        i->opnd[k].vreg = T1_VREG_NONE;
    }
    const struct t1_x86_info *x = t1_x86_info(op);
    if (x)
        i->cc = x->cc;
    return i;
}

t1_vreg t1_mir_new_vreg(struct t1_mir *m, uint8_t type, t1_value value)
{
    if (m->nvregs >= T1_MIR_MAX_VREGS) {
        t1_mir_fail(m, "vreg limit");
        return T1_VREG_NONE;
    }
    t1_vreg v = m->nvregs++;
    m->vreg[v] = (struct t1_mir_vreg){
        .type = type, .fixed = T1_X86_NOREG, .value = value, .hint = T1_VREG_NONE};
    return v;
}

uint32_t t1_mir_new_copies(struct t1_mir *m, unsigned n)
{
    if (n > m->copy_cap - m->ncopies) {
        t1_mir_fail(m, "copy limit");
        return T1_MIR_NONE;
    }
    uint32_t first = m->ncopies;
    m->ncopies += n;
    memset(&m->copy[first], 0, n * sizeof(struct t1_mir_copy));
    return first;
}

/* ---- printing ---- */

static const char *const reg_names[16] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
};

static const char *const pseudo_names[] = {
#define T1_MIR_NAME(name, regs, flags, point, text) #name,
    T1_MIR_PSEUDO_OPS(T1_MIR_NAME)
#undef T1_MIR_NAME
};

static const char *op_name(unsigned op)
{
    if (t1_mir_is_pseudo(op))
        return pseudo_names[op - T1_MIR_BLOCK];
    const struct t1_x86_info *x = t1_x86_info(op);
    return x ? x->name : "?";
}

struct line {
    char buf[256];
    size_t n;
};

static void put(struct line *l, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (l->n < sizeof(l->buf))
        l->n += (size_t)vmm_vsnprintf(l->buf + l->n, sizeof(l->buf) - l->n, fmt, ap);
    va_end(ap);
    if (l->n >= sizeof(l->buf))
        l->n = sizeof(l->buf) - 1;
}

static void put_vreg(struct line *l, t1_vreg v, uint8_t reg, int64_t slot, bool allocated)
{
    if (!allocated || reg == T1_X86_NOREG) {
        if (allocated)
            put(l, "v%u:s%lld", v, (long long)slot);
        else if (reg != T1_X86_NOREG)
            put(l, "v%u:%s", v, reg_names[reg & 15]);
        else
            put(l, "v%u", v);
    } else {
        put(l, "v%u:%s", v, reg_names[reg & 15]);
    }
}

static void put_loc(struct line *l, const struct t1_loc *x)
{
    switch (x->kind) {
    case T1_LOC_REG:
        put(l, "%s", reg_names[x->reg & 15]);
        break;
    case T1_LOC_SPILL:
        put(l, "s%u", x->slot);
        break;
    case T1_LOC_CONST:
        put(l, "#%llx", (unsigned long long)x->value);
        break;
    case T1_LOC_HOME:
        put(l, "home");
        break;
    default:
        put(l, "?");
        break;
    }
}

void t1_mir_print(const struct t1_mir *m, bool allocated, t1_ir_out_fn out, void *arg)
{
    struct line l;
    for (unsigned b = 0; b < m->nblocks; ++b) {
        const struct t1_mir_block *blk = &m->block[b];
        l.n = 0;
        put(&l, "B%u%s (ir %d) succ", b, blk->loop ? " loop" : "",
            blk->ir_block == T1_NO_BLOCK ? -1 : (int)blk->ir_block);
        for (unsigned s = 0; s < blk->nsucc; ++s)
            put(&l, " B%u", blk->succ[s]);
        out(arg, l.buf);
        for (uint32_t i = blk->first; i < blk->first + blk->count; ++i) {
            const struct t1_mir_insn *in = &m->insn[i];
            l.n = 0;
            put(&l, "  %4u%s %s", i, i == blk->term ? "*" : " ", op_name(in->op));
            for (unsigned k = 0; k < in->nopnd; ++k) {
                const struct t1_mir_opnd *o = &in->opnd[k];
                put(&l, k ? ", " : " ");
                switch (o->kind) {
                case T1_MO_VREG:
                    put_vreg(&l, o->vreg, o->fixed, o->imm, allocated);
                    break;
                case T1_MO_PREG:
                    put(&l, "%s", reg_names[o->vreg & 15]);
                    break;
                case T1_MO_IMM:
                    put(&l, "#%lld", (long long)o->imm);
                    break;
                case T1_MO_LABEL:
                    put(&l, "B%lld", (long long)o->imm);
                    break;
                case T1_MO_MEM:
                    put(&l, "[mem]");
                    break;
                default:
                    put(&l, "-");
                    break;
                }
            }
            if (in->op == T1_MIR_PCOPY) {
                for (uint32_t c = in->copies_first; c < in->copies_first + in->ncopies; ++c) {
                    const struct t1_mir_copy *cp = &m->copy[c];
                    if (allocated) {
                        put(&l, " v%u:", cp->dst);
                        put_loc(&l, &cp->dloc);
                        put(&l, "<-");
                        if (cp->src != cp->dst && cp->src != T1_VREG_NONE)
                            put(&l, "v%u:", cp->src);
                        put_loc(&l, &cp->sloc);
                    } else if (cp->src == T1_VREG_NONE) {
                        put(&l, " v%u<-#%llx", cp->dst, (unsigned long long)cp->src_imm);
                    } else {
                        put(&l, " v%u<-v%u", cp->dst, cp->src);
                    }
                }
            }
            if (in->point != UINT32_MAX)
                put(&l, " @P%u", in->point);
            out(arg, l.buf);
        }
    }
}
