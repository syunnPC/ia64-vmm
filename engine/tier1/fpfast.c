/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The FP expansion table and its walker (fpfast.h). */
#include "engine/tier1/fpfast.h"
#include "ia64/decode/decode.h"
#include "build/tier1/fpfast_rules.h"

static bool packed(unsigned opcode)
{
    return opcode == IA64_OP_FPMA || opcode == IA64_OP_FPMS || opcode == IA64_OP_FPNMA;
}

const struct t1_fpx_rule *t1_fpx_select(unsigned opcode, unsigned precision, bool addend_f0,
                                        bool fma3, bool approx)
{
    for (unsigned k = 0; k < t1_fpx_rule_count; ++k) {
        const struct t1_fpx_rule *r = &t1_fpx_rules[k];
        if (r->opcode != opcode || (!packed(opcode) && r->precision != precision))
            continue;
        if ((r->addend == T1_FPX_F0 && !addend_f0) || (r->addend == T1_FPX_REG && addend_f0))
            continue;
        if ((r->fma3 && !fma3) || (r->mode == T1_FPX_APPROX && !approx))
            continue;
        return r;
    }
    return NULL;
}

static bool add_fixup(uint32_t *list, unsigned *n, unsigned max, size_t at)
{
    if (at == SIZE_MAX || *n >= max)
        return false;
    list[(*n)++] = (uint32_t)at;
    return true;
}

bool t1_fpx_emit(const struct t1_fpx_rule *r, const struct t1_fpx_io *io, uint32_t *bail,
                 unsigned *nbail, unsigned maxbail, uint32_t *done, unsigned *ndone,
                 unsigned maxdone)
{
    enum { PENDING = 8 };
    uint32_t pending[T1_FPX_MAX_LABELS][PENDING];
    unsigned npending[T1_FPX_MAX_LABELS] = {0};
    struct t1_x86_buf *b = io->b;
    bool ok = r->nlabel <= T1_FPX_MAX_LABELS;
    for (unsigned k = 0; ok && k < r->ninsn; ++k) {
        const struct t1_fpx_insn *x = &r->insn[k];
        int64_t v = x->expr == T1_FPX_NO_EXPR ? x->imm : t1_iexpr_eval(r->expr, x->expr, io->imm);
        struct t1_x86_insn i = {.op = x->op};
        switch (x->kind) {
        case T1_FPX_X86: {
            const struct t1_x86_info *info = t1_x86_info(x->op);
            if (info->opnd[0] == T1_XO_REL) {
                size_t at = io->x86(io->ctx, &i);
                if (x->label == T1_FPX_BAIL)
                    ok = add_fixup(bail, nbail, maxbail, at);
                else if (x->label == T1_FPX_DONE)
                    ok = add_fixup(done, ndone, maxdone, at);
                else
                    ok = x->label < r->nlabel &&
                         add_fixup(pending[x->label], &npending[x->label], PENDING, at);
                break;
            }
            i.opnd[0].reg = T1_R11;
            if (info->nopnd > 1) {
                if (info->opnd[1] == T1_XO_R64_R)
                    i.opnd[1].reg = T1_R11;
                else
                    i.opnd[1].imm = v;
            }
            io->x86(io->ctx, &i);
            break;
        }
        case T1_FPX_SSE:
            if (x->op == T1_SSE_PSLLQ)
                t1_sse_psllq(b, x->a, (unsigned)v);
            else if (x->op == T1_SSE_FMADD213SD || x->op == T1_SSE_FMADD213SS ||
                     x->op == T1_SSE_FMADD213PS)
                t1_sse_fma(b, (enum t1_sse_op)x->op, x->a, x->b, x->c);
            else
                t1_sse_rr(b, (enum t1_sse_op)x->op, x->a, x->b);
            break;
        case T1_FPX_IN:
            io->in(io->ctx, x->a);
            break;
        case T1_FPX_OUT_R:
            i.op = T1_X86_STORE64;
            i.mem = (struct t1_x86_mem){T1_R14, T1_X86_NOREG, 0, io->out(io->ctx, x->a)};
            i.opnd[1].reg = T1_R11;
            io->x86(io->ctx, &i);
            break;
        case T1_FPX_OUT_X:
            t1_sse_store(b, x->b, io->out(io->ctx, x->a));
            break;
        case T1_FPX_OUT_I:
            i.op = T1_X86_STORE64_I32;
            i.mem = (struct t1_x86_mem){T1_R14, T1_X86_NOREG, 0, io->out(io->ctx, x->a)};
            i.opnd[1].imm = v;
            io->x86(io->ctx, &i);
            break;
        case T1_FPX_LABEL:
            if (x->label >= r->nlabel) {
                ok = false;
                break;
            }
            for (unsigned j = 0; j < npending[x->label]; ++j)
                t1_x86_patch_rel32(b, pending[x->label][j], b->size);
            npending[x->label] = 0;
            break;
        default:
            ok = false;
        }
    }
    for (unsigned l = 0; l < T1_FPX_MAX_LABELS; ++l)
        ok = ok && !npending[l];
    return ok && !b->failed;
}
