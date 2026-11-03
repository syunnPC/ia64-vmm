/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "model.h"
#include "fpswa.h"

static unsigned ctl(uint64_t fpsr, unsigned sf)
{
    return (fpsr >> (6 + 13 * sf)) & 127;
}
static unsigned mask(uint64_t fpsr, unsigned sf)
{
    return sf && (ctl(fpsr, sf) & 64) ? 63 : fpsr & 63;
}
static struct ia64_fr unpack(struct fpswa_fr f)
{
    return (struct ia64_fr){f.significand, f.sign_exponent & 0x1ffff, (f.sign_exponent >> 17) & 1};
}
static struct fpswa_fr pack(struct ia64_fr f)
{
    return (struct fpswa_fr){f.significand, f.exponent | ((uint64_t)f.sign << 17)};
}
static struct fpswa_ret error(unsigned n, uint64_t detail)
{
    return (struct fpswa_ret){UINT64_MAX, (uint64_t)n << 56, detail, 0};
}
static unsigned physical_fr(unsigned r, uint64_t ifs)
{
    return r < 32 ? r : 32 + (r - 32 + ((ifs >> 25) & 127)) % 96;
}
static struct fpswa_fr *saved(struct fpswa_call *c, unsigned r)
{
    if (r < 2)
        return &c->live[r];
    uint64_t bits = r < 64 ? c->state->mask_low : c->state->mask_high;
    if (!(bits & (UINT64_C(1) << (r % 64))))
        return &c->live[r];
    struct fpswa_fr *p;
    unsigned first;
    if (r < 6) {
        p = c->state->low_preserved;
        first = 2;
    } else if (r < 16) {
        p = c->state->low_volatile;
        first = 6;
    } else if (r < 32) {
        p = c->state->high_preserved;
        first = 16;
    } else {
        p = c->state->high_volatile;
        first = 32;
    }
    if (!p || ((uintptr_t)p & 15))
        return NULL;
    return p + (r - first);
}
static bool arithmetic(Ia64Opcode op)
{
    switch (op) {
    case IA64_OP_FMA:
    case IA64_OP_FMS:
    case IA64_OP_FNMA:
    case IA64_OP_FADD:
    case IA64_OP_FSUB:
    case IA64_OP_FMPY:
    case IA64_OP_FNORM:
    case IA64_OP_FRCPA:
    case IA64_OP_FRSQRTA:
    case IA64_OP_FCMP:
    case IA64_OP_FMIN:
    case IA64_OP_FMAX:
    case IA64_OP_FAMIN:
    case IA64_OP_FAMAX:
    case IA64_OP_FCVT_FX:
    case IA64_OP_FCVT_FXU:
    case IA64_OP_FPMA:
    case IA64_OP_FPMS:
    case IA64_OP_FPNMA:
    case IA64_OP_FPRCPA:
    case IA64_OP_FPRSQRTA:
    case IA64_OP_FPMIN:
    case IA64_OP_FPMAX:
    case IA64_OP_FPAMIN:
    case IA64_OP_FPAMAX:
    case IA64_OP_FPCMP:
    case IA64_OP_FPCVT:
        return true;
    default:
        return false;
    }
}
static bool parallel(Ia64Opcode op)
{
    switch (op) {
    case IA64_OP_FPMA:
    case IA64_OP_FPMS:
    case IA64_OP_FPNMA:
    case IA64_OP_FPRCPA:
    case IA64_OP_FPRSQRTA:
    case IA64_OP_FPMIN:
    case IA64_OP_FPMAX:
    case IA64_OP_FPAMIN:
    case IA64_OP_FPAMAX:
    case IA64_OP_FPCMP:
    case IA64_OP_FPCVT:
        return true;
    default:
        return false;
    }
}

/* Raw quotient/sqrt construction ported from the fork's target/ia64/arch/fp.c
 * ia64_fpswa_prepare_divide/prepare_sqrt. All IEEE rounding and exceptions
 * remain in the audited ia64/fp_round.c. Integer division avoids libgcc's
 * FP-based division, which could itself request FPSWA at exponent limits. */
static __uint128_t divide(__uint128_t n, __uint128_t d, __uint128_t *rem)
{
    __uint128_t q = 0, r = 0;
    for (unsigned k = 128; k; --k) {
        r = (r << 1) | ((n >> (k - 1)) & 1);
        if (r >= d) {
            r -= d;
            q |= (__uint128_t)1 << (k - 1);
        }
    }
    *rem = r;
    return q;
}
static uint64_t isqrt(__uint128_t n, __uint128_t *rem)
{
    uint64_t hi = n >> 64;
    unsigned bits = hi ? 128 - __builtin_clzll(hi) : 64 - __builtin_clzll((uint64_t)n);
    __uint128_t x = (__uint128_t)1 << ((bits + 1) / 2);
    for (;;) {
        __uint128_t unused, next = (x + divide(n, x, &unused)) >> 1;
        if (next >= x)
            break;
        x = next;
    }
    *rem = n - x * x;
    return x;
}
static struct ia64_fr normalize(struct ia64_fr f, int32_t *exp, unsigned *flags)
{
    unsigned sh = __builtin_clzll(f.significand);
    if (sh || !f.exponent)
        *flags |= FP_D;
    *exp = (f.exponent ? (int32_t)f.exponent : 0xc001) - (int32_t)sh;
    f.significand <<= sh;
    return f;
}
static struct ia64_result finish(struct vcpu *v, const Ia64Instruction *i, struct ia64_fp_raw *raw,
                                 unsigned flags, bool pred, unsigned precision)
{
    const IA64FloatingOperands *op = &i->operands.floating;
    unsigned sf = op->status_field, masks = mask(v->s.ar[AR_FPSR], sf);
    if (flags & ~masks & 7)
        return ia64_fault(IA64_FAULT_FP, flags & ~masks & 7);
    struct ia64_fr out;
    bool fpa = false;
    if (!ia64_fp_round(raw, ctl(v->s.ar[AR_FPSR], sf), masks, precision, &out, &flags, &fpa))
        return ia64_fault(IA64_FAULT_RESERVED, 0x30);
    ia64_fr_write(&v->s, op->destination, out);
    ia64_pr_write(&v->s, op->auxiliary2, pred);
    v->s.ar[AR_FPSR] |= (uint64_t)flags << (13 + 13 * sf);
    unsigned enabled = flags & ~masks & 0x38;
    if (enabled) {
        if (enabled & 0x18)
            enabled |= flags & FP_I;
        return ia64_fault(IA64_FAULT_FP_TRAP, 1 | ((uint64_t)enabled << 8) | (fpa ? 1u << 14 : 0));
    }
    return ia64_complete();
}
static struct ia64_result quotient(struct vcpu *v, const Ia64Instruction *i, bool sqrt,
                                   unsigned precision)
{
    const IA64FloatingOperands *op = &i->operands.floating;
    struct ia64_fr a = ia64_fr_read(&v->s, sqrt ? op->source2 : op->source1);
    struct ia64_fr b = ia64_fr_read(&v->s, op->source2);
    unsigned flags = 0;
    int32_t ae, be;
    a = normalize(a, &ae, &flags);
    b = normalize(b, &be, &flags);
    struct ia64_fp_raw raw = {0};
    __uint128_t rem;
    if (sqrt) {
        int32_t exp = ae - IA64_FP_BIAS, odd = exp & 1;
        raw.exp = IA64_FP_BIAS + (exp - odd) / 2;
        raw.mant = isqrt((__uint128_t)a.significand << (63 + odd), &rem);
        raw.tail = rem != 0;
        raw.half_greater = rem > raw.mant;
    } else {
        unsigned sh = a.significand < b.significand;
        raw.sign = a.sign ^ b.sign;
        raw.exp = IA64_FP_BIAS + ae - be - (int32_t)sh;
        raw.mant = divide((__uint128_t)a.significand << (63 + sh), b.significand, &rem);
        raw.tail = rem != 0;
        raw.half_greater = (rem << 1) > b.significand;
        raw.half_equal = raw.tail && (rem << 1) == b.significand;
    }
    return finish(v, i, &raw, flags, false, precision);
}
static struct ia64_result scalar(struct vcpu *v, const Ia64Instruction *i)
{
    struct ia64_result r = ia64_fp(v, i);
    if (r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_FP && (r.info & 8) &&
        (i->opcode == IA64_OP_FRCPA || i->opcode == IA64_OP_FRSQRTA))
        return quotient(v, i, i->opcode == IA64_OP_FRSQRTA, 0);
    return r;
}
static struct ia64_result packed_approx(struct vcpu *v, const Ia64Instruction *i)
{
    const IA64FloatingOperands *op = &i->operands.floating;
    unsigned sf = op->status_field, shift = 13 + 13 * sf, masks = mask(v->s.ar[AR_FPSR], sf);
    bool sqrt = i->opcode == IA64_OP_FPRSQRTA, pred = true;
    struct ia64_fr a = ia64_fr_read(&v->s, op->source1), b = ia64_fr_read(&v->s, op->source2);
    if (ia64_fr_is_nat(b) || (!sqrt && ia64_fr_is_nat(a))) {
        ia64_fr_write(&v->s, op->destination, ia64_fp_nat());
        ia64_pr_write(&v->s, op->auxiliary2, false);
        return ia64_complete();
    }
    uint64_t data = 0, faults = 0, traps = 0;
    unsigned flags = 0;
    struct vcpu lane = {0};
    for (unsigned k = 0; k < 2; ++k) {
        lane.s.fr[7] = ia64_fp_from_ieee((uint32_t)(a.significand >> (32 * k)), true);
        lane.s.fr[8] = ia64_fp_from_ieee((uint32_t)(b.significand >> (32 * k)), true);
        lane.s.ar[AR_FPSR] =
            (masks | 7) | ((uint64_t)(ctl(v->s.ar[AR_FPSR], sf) & ~2u) << (6 + 13 * sf));
        lane.s.pr = 1;
        Ia64Instruction si = {.valid = true,
                              .unit = IA64_UNIT_F,
                              .opcode = sqrt ? IA64_OP_FRSQRTA : IA64_OP_FRCPA,
                              .operands.floating = {.destination = 6,
                                                    .source1 = 7,
                                                    .source2 = 8,
                                                    .auxiliary2 = 11,
                                                    .status_field = sf,
                                                    .precision = 1}};
        struct ia64_result r = ia64_fp(&lane, &si);
        struct ia64_fr n = lane.s.fr[sqrt ? 8 : 7], d = lane.s.fr[8];
        bool finite = n.exponent != IA64_FP_SPECIAL_EXP && d.exponent != IA64_FP_SPECIAL_EXP;
        bool ordinary = finite && n.significand && d.significand && (!sqrt || !n.sign);
        if (ordinary) {
            unsigned ignored = 0;
            int32_t ne, de;
            normalize(n, &ne, &ignored);
            normalize(d, &de, &ignored);
            bool safe = !(ignored & FP_D) && ne > IA64_FP_BIAS - 127 + 24 &&
                        (sqrt || (de < IA64_FP_BIAS + 126 && ne - de > 2 - 127 && ne - de < 127));
            if (!safe || (r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_FP && (r.info & 8)))
                r = quotient(&lane, &si, sqrt, 1);
        }
        if (r.kind == IA64_STEP_EXIT ||
            (r.kind == IA64_STEP_FAULT && r.fault != IA64_FAULT_FP_TRAP))
            return r;
        unsigned lf = (lane.s.ar[AR_FPSR] >> shift) & 63;
        flags |= lf;
        faults |= (uint64_t)(lf & ~masks & 7) << (k ? 0 : 4);
        if (r.fault == IA64_FAULT_FP_TRAP) {
            traps |= ((r.info >> 8) & 0x38) << (k ? 8 : 4);
            if (r.info & (1u << 14))
                traps |= 1u << (k ? 14 : 10);
        }
        struct ia64_fr out = lane.s.fr[6];
        if (ia64_pr_read(&lane.s, 11)) {
            struct ia64_fp_raw raw = {
                .sign = out.sign, .exp = (int32_t)out.exponent, .mant = out.significand};
            unsigned unused = 0;
            bool fpa = false;
            ia64_fp_round(&raw, 0, 63, 1, &out, &unused, &fpa);
        }
        uint32_t bits = ia64_fp_to_ieee(out, true);
        if (traps & ((uint64_t)0x18 << (k ? 8 : 4))) {
            bits = ((uint32_t)out.sign << 31) |
                   (((out.exponent - IA64_FP_BIAS + 127) & 255) << 23) |
                   ((out.significand >> 40) & 0x7fffff);
        }
        /* Packed rsqrt has reciprocal special responses (SDM Vol.3 fprsqrta). */
        if (sqrt && !n.significand && finite)
            bits = ((uint32_t)n.sign << 31) | 0x7f800000;
        else if (sqrt && n.exponent == IA64_FP_SPECIAL_EXP &&
                 n.significand == IA64_FP_INTEGER_BIT && !n.sign)
            bits = 0;
        data |= (uint64_t)bits << (32 * k);
        pred &= ia64_pr_read(&lane.s, 11);
    }
    if (faults)
        return ia64_fault(IA64_FAULT_FP, faults);
    v->s.ar[AR_FPSR] |= (uint64_t)flags << shift;
    ia64_fr_write(&v->s, op->destination, (struct ia64_fr){data, IA64_FP_INTEGER_EXP, false});
    ia64_pr_write(&v->s, op->auxiliary2, pred);
    return traps ? ia64_fault(IA64_FAULT_FP_TRAP, traps | 1) : ia64_complete();
}

static bool bounded(struct ia64_fr input, unsigned indication, bool fpa, unsigned controls,
                    unsigned masks, unsigned precision, bool simd, struct ia64_fr *out,
                    unsigned *flags, bool *up)
{
    unsigned pc = precision == 1 ? 0 : precision == 2 ? 2 : (controls >> 2) & 3;
    if (pc == 1)
        return false;
    unsigned shift = pc == 0 ? 40 : pc == 2 ? 11 : 0;
    int32_t min = (controls & 2) ? 1 : precision == 1 ? 0xff81 : precision == 2 ? 0xfc01 : 0xc001;
    int32_t max = (controls & 2)   ? 0x1fffe
                  : precision == 1 ? 0x1007e
                  : precision == 2 ? 0x103fe
                                   : 0x13ffe;
    struct ia64_fp_raw raw = {
        .sign = input.sign, .exp = (int32_t)input.exponent, .mant = input.significand};
    int32_t modulus = simd ? 256 : 0x20000;
    if ((indication & FP_U) && raw.exp >= min)
        raw.exp -= modulus;
    if ((indication & FP_O) && raw.exp <= max)
        raw.exp += modulus;
    if (indication & FP_I) {
        raw.tail = true;
        if (fpa) {
            /* Undo a carry across a binade as well as ordinary rounding up.
             * The direction of the discarded fraction is sufficient when
             * denormalization discards at least one more bit (SDM 8.1.1.2). */
            if (raw.mant == IA64_FP_INTEGER_BIT) {
                --raw.exp;
                raw.mant = UINT64_MAX << shift;
            } else
                raw.mant -= UINT64_C(1) << shift;
        }
    }
    if (((indication & (FP_U | FP_O)) && !raw.mant) || ((indication & FP_U) && raw.exp >= min) ||
        ((indication & FP_O) && raw.exp <= max))
        return false;
    if (!(indication & (FP_U | FP_O))) {
        *out = input;
        *flags |= indication & FP_I;
        *up = fpa;
        return true;
    }
    return ia64_fp_round(&raw, controls, masks, precision, out, flags, up);
}
static struct fpswa_ret trap(struct fpswa_call *c, const Ia64Instruction *i, bool simd)
{
    const IA64FloatingOperands *op = &i->operands.floating;
    unsigned sf = op->status_field, controls = ctl(*c->fpsr, sf), masks = mask(*c->fpsr, sf);
    unsigned dest = physical_fr(op->destination, *c->ifs);
    if (dest < 2)
        return error(16, i->raw);
    struct fpswa_fr *p = saved(c, dest);
    if (!p)
        return error(5, dest);
    struct ia64_fr input = unpack(*p), out = input;
    uint64_t data = 0, code = *c->isr & 0xffff;
    unsigned flags = 0, enabled = 0;
    for (unsigned k = 0; k < (simd ? 2u : 1u); ++k) {
        unsigned offset = simd ? (k ? 8 : 4) : 8;
        unsigned indication = (*c->isr >> offset) & 0x38;
        bool fpa = (*c->isr >> (simd ? (k ? 14 : 10) : 14)) & 1, up = false;
        uint32_t bits = input.significand >> (32 * k);
        struct ia64_fr lane =
            simd ? (struct ia64_fr){IA64_FP_INTEGER_BIT | ((uint64_t)(bits & 0x7fffff) << 40),
                                    IA64_FP_BIAS - 127 + ((bits >> 23) & 255), bits >> 31}
                 : input;
        unsigned lf = 0;
        if (indication & masks) {
            if (!bounded(lane, indication, fpa, simd ? controls & ~2u : controls, masks,
                         simd ? 1 : op->precision, simd, &out, &lf, &up))
                return error(8, k);
            /* A SWA response is completing; preserve enabled indications in
             * the other lane while replacing this lane's masked indication. */
            unsigned e = lf & ~masks & 0x38;
            if (e & 0x18)
                e |= lf & FP_I;
            code &= ~((uint64_t)0x38 << offset);
            code &= ~(UINT64_C(1) << (simd ? (k ? 14 : 10) : 14));
            code |= (uint64_t)e << offset;
            if (e && up)
                code |= UINT64_C(1) << (simd ? (k ? 14 : 10) : 14);
            enabled |= e;
        } else {
            out = lane;
            enabled |= indication & ~masks;
        }
        flags |= lf;
        if (simd)
            data |= (uint64_t)((indication & masks) ? ia64_fp_to_ieee(out, true) : bits)
                    << (32 * k);
    }
    *p = simd ? (struct fpswa_fr){data, IA64_FP_INTEGER_EXP} : pack(out);
    *c->fpsr |= (uint64_t)flags << (13 + 13 * sf);
    *c->ipsr |= UINT64_C(1) << (dest < 32 ? 4 : 5);
    if (enabled) {
        *c->isr = (*c->isr & ~UINT64_C(0xffff)) | code;
        return (struct fpswa_ret){simd ? 5 : 1, 0, 0, 0};
    }
    return (struct fpswa_ret){0};
}

struct fpswa_ret fpswa_emulate(struct fpswa_call *c)
{
    if (!c || c->trap_type > 1 || !c->bundle || !c->ipsr || !c->fpsr || !c->isr || !c->preds ||
        !c->ifs || !c->state ||
        (((uintptr_t)c->bundle | (uintptr_t)c->ipsr | (uintptr_t)c->fpsr | (uintptr_t)c->isr |
          (uintptr_t)c->preds | (uintptr_t)c->ifs | (uintptr_t)c->state) &
         7))
        return error(5, 0);
    unsigned slot = c->trap_type ? (*c->ipsr >> 41) & 3 : (*c->isr >> 41) & 3;
    const IA64TemplateInfo *t = ia64_template_info(c->bundle[0] & 31);
    if (slot > 2 || !t->defined || t->units[slot] != IA64_UNIT_F)
        return error(2, slot);
    Ia64Instruction i =
        ia64_decode_insn(IA64_UNIT_F, ia64_bundle_slot(c->bundle[0], c->bundle[1], slot), 0, slot);
    if (!i.valid || !arithmetic(i.opcode))
        return error(3, i.raw);
    bool simd = parallel(i.opcode);
    unsigned sf = i.operands.floating.status_field, masks = mask(*c->fpsr, sf);
    if (c->trap_type && !(*c->isr & (simd ? 0x88 : 8)))
        return (struct fpswa_ret){simd ? 5 : 1, 0, 0, 0};
    if (!c->trap_type) {
        /* Traps contain no explicit SWA bit: a masked O/U/I indication asks
         * for exponent-bounded rounding (SDM Vol.2 §§5.5, 8.1.1.2). */
        unsigned hi = (*c->isr >> 8) & 0x38, lo = simd ? (*c->isr >> 4) & 0x38 : 0;
        if (!((hi | lo) & masks))
            return (struct fpswa_ret){simd ? 5 : 1, 0, 0, 0};
        return trap(c, &i, simd);
    }
    struct vcpu v = {0};
    v.s.cfm = *c->ifs;
    v.s.pr = *c->preds;
    v.s.ar[AR_FPSR] = *c->fpsr;
    v.s.fr[1] = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_BIAS, false};
    unsigned regs[4] = {i.operands.floating.destination, i.operands.floating.source1,
                        i.operands.floating.source2, i.operands.floating.auxiliary1};
    bool cmp = i.opcode == IA64_OP_FCMP;
    if (cmp)
        regs[0] = regs[3] = 0;
    if (i.opcode != IA64_OP_FMA && i.opcode != IA64_OP_FMS && i.opcode != IA64_OP_FNMA &&
        i.opcode != IA64_OP_FPMA && i.opcode != IA64_OP_FPMS && i.opcode != IA64_OP_FPNMA)
        regs[3] = 0;
    if (!cmp && regs[0] < 2)
        return error(3, i.raw);
    struct fpswa_fr *dest = NULL;
    for (unsigned k = 0; k < 4; ++k) {
        unsigned r = physical_fr(regs[k], *c->ifs);
        if (r < 2)
            continue;
        struct fpswa_fr *p = saved(c, r);
        if (!p)
            return error(5, r);
        v.s.fr[r] = unpack(*p);
        if (!k)
            dest = p;
    }
    if (!ia64_pr_read(&v.s, i.qp))
        return error(4, i.qp);
    struct ia64_result r;
    if (i.opcode == IA64_OP_FPRCPA || i.opcode == IA64_OP_FPRSQRTA)
        r = packed_approx(&v, &i);
    else
        r = scalar(&v, &i);
    if (r.kind == IA64_STEP_EXIT ||
        (r.kind == IA64_STEP_FAULT && r.fault != IA64_FAULT_FP && r.fault != IA64_FAULT_FP_TRAP))
        return error(18, r.info);
    if (r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_FP) {
        if (r.info & (simd ? 0x88 : 8))
            return error(18, r.info);
        *c->isr = (*c->isr & ~UINT64_C(0xffff)) | r.info;
        return (struct fpswa_ret){simd ? 5 : 1, 0, 0, 0};
    }
    if (dest)
        *dest = pack(ia64_fr_read(&v.s, i.operands.floating.destination));
    *c->ipsr |= v.s.psr & (IA64_PSR_MFL | IA64_PSR_MFH);
    *c->fpsr = v.s.ar[AR_FPSR];
    *c->preds = v.s.pr;
    if (r.kind == IA64_STEP_FAULT) {
        *c->isr = (*c->isr & ~UINT64_C(0xffff)) | r.info;
        return (struct fpswa_ret){simd ? 7 : 3, 0, 0, 0};
    }
    return (struct fpswa_ret){0};
}
