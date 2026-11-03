/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test-only IR evaluator (see ir_eval.h). IR op semantics follow
 * ir_ops.def and the SDM Vol.3 instruction pages the ops are lifted from
 * (add, sub, and, andcm, or, xor, shladd, shl, shr, shr.u, extr, extr.u,
 * dep.z, sxt, zxt, cmp, cmp4, ld, st, br.cloop); they are deliberately
 * not shared with ia64/integer.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/ir_eval.h"
#include "engine/tier1/form.h"
#include "engine/cache/cache.h"
#include "ia64/semantics.h"
#include "ia64vmm/guest_mem.h"
#include "tests/tier1/bof.h"

static uint64_t mask(unsigned bits)
{
    return bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
}
static uint64_t sext(uint64_t v, unsigned bits)
{
    if (bits >= 64)
        return v;
    uint64_t sign = UINT64_C(1) << (bits - 1);
    return ((v & mask(bits)) ^ sign) - sign;
}

/* Homes at the vCPU's frame base: a stacked element is frame-relative. */
static uint64_t home_read(const struct vcpu *v, unsigned e)
{
    if (e >= 1 && e < 128)
        return v->s.gr[t1t_phys(v->s.rse_bof, e)];
    if (e >= 129 && e < 256)
        return v->s.nat[t1t_phys(v->s.rse_bof, e - 128)];
    if (e >= 257 && e < T1_ELEMENT_LC)
        return (v->s.pr >> (e - 256)) & 1;
    if (e == T1_ELEMENT_LC)
        return v->s.ar[AR_LC];
    if (e == T1_ELEMENT_LSB)
        return v->last_successful_bundle;
    if (e >= T1_ELEMENT_FR(2) && e <= T1_ELEMENT_FR(127))
        return v->s.fr[e - 323u].significand;
    if (e >= T1_ELEMENT_FR_META(2) && e <= T1_ELEMENT_FR_META(127))
        return v->s.fr[e - 452u].exponent | ((uint64_t)v->s.fr[e - 452u].sign << 17);
    if (e == T1_ELEMENT_PSR_MF)
        return v->s.psr & T1_PSR_MF_BITS;
    return 0;
}

static void home_write(struct vcpu *v, unsigned e, uint64_t x)
{
    if (e >= 1 && e < 128)
        v->s.gr[t1t_phys(v->s.rse_bof, e)] = x;
    else if (e >= 129 && e < 256)
        v->s.nat[t1t_phys(v->s.rse_bof, e - 128)] = x & 1;
    else if (e >= 257 && e < T1_ELEMENT_LC)
        v->s.pr = (v->s.pr & ~(UINT64_C(1) << (e - 256))) | ((x & 1) << (e - 256));
    else if (e == T1_ELEMENT_LC)
        v->s.ar[AR_LC] = x;
    else if (e == T1_ELEMENT_LSB)
        v->last_successful_bundle = x;
    else if (e >= T1_ELEMENT_FR(2) && e <= T1_ELEMENT_FR(127))
        v->s.fr[e - 323u].significand = x;
    else if (e >= T1_ELEMENT_FR_META(2) && e <= T1_ELEMENT_FR_META(127)) {
        v->s.fr[e - 452u].exponent = x & 0x1ffff;
        v->s.fr[e - 452u].sign = (x >> 17) & 1;
    } else if (e == T1_ELEMENT_PSR_MF)
        v->s.psr |= x & T1_PSR_MF_BITS;
}

/* Native access eligibility (: physical-mode WB RAM only). */
static uint8_t *native(struct vcpu *v, uint64_t addr, unsigned size, bool write)
{
    if (addr & (size - 1))
        return NULL;
    if ((v->s.psr & IA64_PSR_DT) || v->memory_hook)
        return NULL;
    if (addr >> 63)
        return NULL; /* UC alias */
    if (addr >> (IA64_IMPL_PA_MSB + 1))
        return NULL;
    const struct gm_region *r = gm_lookup(addr);
    if (!r || r->kind != GM_RAM || addr - r->base + size > r->size)
        return NULL;
    if (write && code_page_has_code(addr & ~UINT64_C(4095)))
        return NULL;
    return r->hva + (addr - r->base);
}

static bool cmp(unsigned rel, unsigned width, uint64_t a, uint64_t b)
{
    if (width == 32) {
        a = rel == T1_CMP_LT ? sext(a, 32) : (uint32_t)a;
        b = rel == T1_CMP_LT ? sext(b, 32) : (uint32_t)b;
    }
    switch (rel) {
    case T1_CMP_EQ:
        return a == b;
    case T1_CMP_LT:
        return (int64_t)a < (int64_t)b;
    default:
        return a < b;
    }
}

struct eval {
    const struct t1_ir *ir;
    struct vcpu *v;
    uint64_t (*val)[4];
    uint64_t *occ;
    uint64_t r13, budget, retired_entry;
    struct ir_eval_result *out;
};

static uint64_t V(const struct eval *E, t1_value x)
{
    return E->val[T1_VALUE_OP(x)][T1_VALUE_RESULT(x)];
}
static uint64_t opnd(const struct eval *E, uint32_t o, unsigned i)
{
    return V(E, E->ir->use[E->ir->op[o].use_first + i].value);
}

static void materialize(struct eval *E, uint32_t p)
{
    const struct t1_ir *ir = E->ir;
    const struct t1_ir_point *pt = &ir->point[p];
    for (unsigned k = 0; k < pt->frame_count; ++k) {
        const struct t1_use *u = &ir->use[pt->frame_first + k];
        if (!(u->flags & T1_USE_HOME))
            home_write(E->v, u->element, V(E, u->value));
    }
    E->v->s.ip = pt->ip;
    E->v->s.slot = pt->slot;
    E->out->point = p;
    E->out->consumed = E->budget - E->r13 + pt->retired_offset;
    E->v->retired = E->retired_entry + E->out->consumed;
}

static bool error(struct eval *E, const char *what, unsigned a)
{
    snprintf(E->out->error, sizeof(E->out->error), "%s (%u)", what, a);
    return false;
}

/* Returns 1 = continue, 0 = exited at a point, -1 = error. */
static int exec_op(struct eval *E, uint32_t o, const struct ir_eval_hooks *h)
{
    const struct t1_ir *ir = E->ir;
    const struct t1_op *op = &ir->op[o];
    uint64_t *r = E->val[o];
    uint64_t a = op->nuse > 0 ? opnd(E, o, 0) : 0, b = op->nuse > 1 ? opnd(E, o, 1) : 0;
    unsigned pos = T1_FIELD_POS(op->imm), len = T1_FIELD_LEN(op->imm);
    switch (op->code) {
    case T1_IR_CONST:
        r[0] = (uint64_t)op->imm;
        break;
    case T1_IR_LIVEIN:
    case T1_IR_NATIN:
        r[0] = home_read(E->v, (unsigned)op->imm);
        break;
    case T1_IR_SETF_SIG:
    case T1_IR_GETF_SIG:
        r[0] = a;
        break;
    case T1_IR_XMA_L:
        r[0] = a * b + opnd(E, o, 2);
        break;
    case T1_IR_XMA_H:
    case T1_IR_XMA_HU: {
        __uint128_t product = op->code == T1_IR_XMA_H
                                  ? (__uint128_t)((__int128)(int64_t)a * (__int128)(int64_t)b)
                                  : (__uint128_t)a * b;
        r[0] = (uint64_t)((product + opnd(E, o, 2)) >> 64);
        break;
    }
    case T1_IR_FRNATTEST:
        r[0] = !a && b == 0x1fffe;
        break;
    case T1_IR_ADD:
        r[0] = a + b;
        break;
    case T1_IR_SUB:
        r[0] = a - b;
        break;
    case T1_IR_AND:
        r[0] = a & b;
        break;
    case T1_IR_OR:
        r[0] = a | b;
        break;
    case T1_IR_XOR:
        r[0] = a ^ b;
        break;
    case T1_IR_ANDCM:
        r[0] = a & ~b;
        break;
    case T1_IR_SHL:
        r[0] = b > 63 ? 0 : a << b;
        break;
    case T1_IR_SHRU:
        r[0] = b > 63 ? 0 : a >> b;
        break;
    case T1_IR_SHR:
        r[0] = (uint64_t)((int64_t)a >> (b > 63 ? 63 : b));
        break;
    case T1_IR_SHL_IMM:
        r[0] = a << (op->imm & 63);
        break;
    case T1_IR_EXTRU:
        r[0] = (a >> pos) & mask(len);
        break;
    case T1_IR_EXTR:
        r[0] = sext((a >> pos) & mask(len), len);
        break;
    case T1_IR_DEPZ:
        r[0] = (a << pos) & (mask(len) << pos);
        break;
    case T1_IR_SXT:
        r[0] = sext(a, (unsigned)op->imm);
        break;
    case T1_IR_ZXT:
        r[0] = a & mask((unsigned)op->imm);
        break;
    case T1_IR_CLOOP:
        r[1] = a != 0;
        r[0] = a ? a - 1 : a;
        break;
    case T1_IR_CMP:
        r[0] = cmp(T1_CMP_REL(op->imm), T1_CMP_WIDTH(op->imm), a, b);
        break;
    case T1_IR_NOT1:
        r[0] = !a;
        break;
    case T1_IR_AND1:
        r[0] = a & b & 1;
        break;
    case T1_IR_OR1:
        r[0] = (a | b) & 1;
        break;
    case T1_IR_SELECT:
        r[0] = a & 1 ? b : opnd(E, o, 2);
        break;
    case T1_IR_NATCHK:
        if (a)
            return error(E, "NATCHK of a set NaT", o), -1;
        break;
    case T1_IR_FENCE:
        r[0] = 0;
        break;
    case T1_IR_LOAD:
    case T1_IR_STORE: {
        bool store = op->code == T1_IR_STORE;
        unsigned bytes = T1_ACCESS_BYTES(op->imm);
        uint64_t n = ++E->occ[op->point];
        uint8_t *p = native(E->v, a, bytes, store);
        if (h && h->force && h->force(h->arg, op->point, n))
            E->out->forced = true, p = NULL;
        if (!p) {
            materialize(E, op->point);
            return 0;
        }
        if (store)
            memcpy(p, &b, bytes);
        else {
            uint64_t x = 0;
            memcpy(&x, p, bytes);
            r[0] = x;
        }
        r[store ? 0 : 1] = 0;
        break;
    }
    case T1_IR_GHELPER:
    case T1_IR_HELPER: {
        uint64_t n = ++E->occ[op->point];
        if (h && h->force && h->force(h->arg, op->point, n)) {
            E->out->forced = true;
            materialize(E, op->point);
            return 0;
        }
        const struct t1_ir_point *pt = &ir->point[op->point];
        for (unsigned i = 1; i < op->nuse; ++i) {
            const struct t1_use *u = &ir->use[op->use_first + i];
            home_write(E->v, u->element, V(E, u->value));
        }
        E->v->s.ip = pt->ip;
        E->v->s.slot = pt->slot;
        Ia64Instruction copy = ir->form->insn[op->imm].insn;
        struct ia64_result res = ia64_execute(E->v, &copy);
        if (res.kind != IA64_STEP_COMPLETE) {
            materialize(E, op->point);
            return 0;
        }
        r[0] = op->wr[0] ? home_read(E->v, op->wr[0]) : 0;
        r[1] = op->wr[1] ? home_read(E->v, op->wr[1]) : 0;
        r[2] = 0;
        break;
    }
    case T1_IR_HREAD:
        r[0] = home_read(E->v, op->imm);
        break;
    case T1_IR_HGUARD:
        r[0] = 0;
        break;
    default:
        return error(E, "op without evaluation", op->code), -1;
    }
    return 1;
}

bool ir_eval_run(const struct t1_ir *ir, struct vcpu *v, uint64_t budget,
                 const struct ir_eval_hooks *h, struct ir_eval_result *out)
{
    memset(out, 0, sizeof(*out));
    struct eval E = {ir,
                     v,
                     calloc(ir->nops ? ir->nops : 1, sizeof(*E.val)),
                     calloc(ir->npoints ? ir->npoints : 1, sizeof(uint64_t)),
                     budget,
                     budget,
                     v->retired,
                     out};
    unsigned max_path = t1_ir_max_path(ir, T1_HEADER_BLOCK);
    uint64_t *args = calloc(T1_N_ELEMENTS * 4u, sizeof(uint64_t));
    bool ok = E.val && E.occ && args;
    if (ok && budget < max_path)
        ok = error(&E, "entered below max_path", max_path);
    uint16_t b = T1_ENTRY_BLOCK;
    while (ok) {
        const struct t1_block *blk = &ir->block[b];
        uint32_t o = blk->first;
        while (o != T1_NO_OP && ir->op[o].code == T1_IR_PARAM)
            o = ir->op[o].next;
        int st = 1;
        for (; o != T1_NO_OP && st == 1; o = ir->op[o].next)
            if (t1_op_desc(ir->op[o].code)->effect != T1_EFF_TERM)
                st = exec_op(&E, o, h);
            else
                break;
        if (st <= 0) {
            ok = st == 0;
            break;
        }
        if (o == T1_NO_OP) {
            ok = error(&E, "block without terminator", b);
            break;
        }
        const struct t1_op *t = &ir->op[o];
        E.r13 -= blk->charge;
        if (t->code == T1_IR_EXIT) {
            materialize(&E, t->point);
            break;
        }
        unsigned succ = 0;
        if (t->code == T1_IR_BRANCH)
            succ = (opnd(&E, o, 0) & 1) ? 0 : 1;
        if (t->code == T1_IR_BACKEDGE) {
            /* The checkpoint compares with its own header's max_path. */
            ++out->iterations;
            if (E.r13 < t1_ir_max_path(ir, blk->succ[0].target) ||
                (h && h->event && h->event(h->arg, out->iterations))) {
                materialize(&E, t->point);
                break;
            }
        }
        /* Parallel copy of the edge arguments into the target's params. */
        const struct t1_edge *e = &blk->succ[succ];
        for (unsigned k = 0; k < e->nargs; ++k)
            args[k] = V(&E, ir->use[e->args_first + k].value);
        unsigned k = 0;
        for (uint32_t q = ir->block[e->target].first;
             q != T1_NO_OP && ir->op[q].code == T1_IR_PARAM; q = ir->op[q].next)
            E.val[q][0] = args[k++];
        if (k != e->nargs) {
            ok = error(&E, "edge arity", b);
            break;
        }
        b = e->target;
    }
    free(E.val);
    free(E.occ);
    free(args);
    return ok;
}
