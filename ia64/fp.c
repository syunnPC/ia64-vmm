/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/fp.h"
#include "ia64/fp_tables.h"

static unsigned clz64(uint64_t v)
{
    return v ? (unsigned)__builtin_clzll(v) : 64;
}

struct ia64_fr ia64_fr_read(const struct ia64_state *s, unsigned reg)
{
    if (!reg)
        return (struct ia64_fr){0};
    if (reg == 1)
        return (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_BIAS, false};
    return s->fr[ia64_fr_index(s, reg)];
}
void ia64_fr_write(struct ia64_state *s, unsigned reg, struct ia64_fr value)
{
    if (reg < 2)
        return;
    value.exponent &= 0x1ffff;
    s->fr[ia64_fr_index(s, reg)] = value;
    s->psr |= reg < 32 ? IA64_PSR_MFL : IA64_PSR_MFH;
}
bool ia64_fr_is_nat(struct ia64_fr v)
{
    return !v.sign && v.exponent == IA64_FP_NAT_EXP && !v.significand;
}
struct ia64_fr ia64_fp_nat(void)
{
    return (struct ia64_fr){.exponent = IA64_FP_NAT_EXP};
}

/* SDM Vol.1 section 5.3: these are bit transfers, with no rounding. */
struct ia64_fr ia64_fp_from_ieee(uint64_t bits, bool single)
{
    unsigned width = single ? 23 : 52, shift = 63 - width;
    unsigned mask = single ? 0xff : 0x7ff;
    unsigned base = single ? 0xff80 : 0xfc00;
    unsigned exp = (bits >> width) & mask;
    uint64_t frac = bits & ((UINT64_C(1) << width) - 1);
    struct ia64_fr out = {.sign = (bits >> (single ? 31 : 63)) & 1};
    out.significand = frac << shift;
    if (exp) {
        out.significand |= IA64_FP_INTEGER_BIT;
        out.exponent = exp == mask ? IA64_FP_SPECIAL_EXP : base + exp;
    } else if (frac)
        out.exponent = base + 1;
    return out;
}
uint64_t ia64_fp_to_ieee(struct ia64_fr v, bool single)
{
    unsigned width = single ? 23 : 52;
    uint64_t exp = single ? ((v.exponent >> 9) & 0x80) | (v.exponent & 0x7f)
                          : ((v.exponent >> 6) & 0x400) | (v.exponent & 0x3ff);
    if (!(v.significand & IA64_FP_INTEGER_BIT))
        exp = 0;
    return ((uint64_t)v.sign << (single ? 31 : 63)) | (exp << width) |
           ((v.significand >> (63 - width)) & ((UINT64_C(1) << width) - 1));
}
struct ia64_result ia64_fp_disabled(const struct ia64_state *s, unsigned a, unsigned b, unsigned c,
                                    unsigned d)
{
    unsigned regs[] = {a, b, c, d}, code = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (regs[i] >= 2 && regs[i] < 32 && (s->psr & IA64_PSR_DFL))
            code |= 1;
        if (regs[i] >= 32 && (s->psr & IA64_PSR_DFH))
            code |= 2;
    }
    return code ? ia64_fault(IA64_FAULT_FP_DISABLED, code) : ia64_complete();
}

static bool unsupported(struct ia64_fr v)
{
    return v.exponent == IA64_FP_SPECIAL_EXP && !(v.significand & IA64_FP_INTEGER_BIT);
}
static bool infinity(struct ia64_fr v)
{
    return v.exponent == IA64_FP_SPECIAL_EXP && v.significand == IA64_FP_INTEGER_BIT;
}
static bool is_nan(struct ia64_fr v)
{
    return v.exponent == IA64_FP_SPECIAL_EXP && v.significand != IA64_FP_INTEGER_BIT;
}
static bool snan(struct ia64_fr v)
{
    return is_nan(v) && !(v.significand & IA64_FP_QUIET_BIT);
}
static bool unnormal(struct ia64_fr v)
{
    return v.exponent != IA64_FP_SPECIAL_EXP &&
           (v.exponent ? !(v.significand & IA64_FP_INTEGER_BIT) : v.significand != 0);
}
static struct ia64_fr indefinite(void)
{
    return (struct ia64_fr){IA64_FP_INTEGER_BIT | IA64_FP_QUIET_BIT, IA64_FP_SPECIAL_EXP, true};
}
static struct ia64_fr int_value(uint64_t value)
{
    return (struct ia64_fr){value, IA64_FP_INTEGER_EXP, false};
}

static unsigned sf_shift(unsigned sf)
{
    return 6 + (sf & 3) * 13;
}
static unsigned controls(const struct ia64_state *s, unsigned sf)
{
    return (s->ar[AR_FPSR] >> sf_shift(sf)) & 0x7f;
}

/* Unsupported, SNaN and QNaN responses precede all other exceptions.
 * SDM Vol.1 section 5.4.6 gives f4, f2, f3 payload selection priority. */
static bool nan_response(struct ia64_fr a, struct ia64_fr b, struct ia64_fr c, struct ia64_fr *out,
                         unsigned *flags)
{
    if (unsupported(a) || unsupported(b) || unsupported(c)) {
        *out = indefinite();
        *flags |= FP_V;
        return true;
    }
    if (snan(a) || snan(b) || snan(c))
        *flags |= FP_V;
    if (is_nan(c) || is_nan(a) || is_nan(b)) {
        *out = is_nan(c) ? c : is_nan(a) ? a : b;
        out->significand |= IA64_FP_QUIET_BIT;
        return true;
    }
    return false;
}

static int magnitude(struct ia64_fr a, struct ia64_fr b)
{
    if (infinity(a))
        return infinity(b) ? 0 : 1;
    if (infinity(b))
        return -1;
    if (!a.significand)
        return b.significand ? -1 : 0;
    if (!b.significand)
        return 1;
    unsigned ash = clz64(a.significand), bsh = clz64(b.significand);
    int ae = (a.exponent ? (int)a.exponent : 0xc001) - (int)ash;
    int be = (b.exponent ? (int)b.exponent : 0xc001) - (int)bsh;
    if (ae != be)
        return ae < be ? -1 : 1;
    uint64_t am = a.significand << ash, bm = b.significand << bsh;
    return am == bm ? 0 : am < bm ? -1 : 1;
}
static int compare(struct ia64_fr a, struct ia64_fr b, bool absolute)
{
    int cmp = magnitude(a, b);
    if (absolute || (!a.significand && !b.significand))
        return cmp;
    if (a.sign != b.sign)
        return a.sign ? -1 : 1;
    return a.sign ? -cmp : cmp;
}

static bool class_member(struct ia64_fr a, unsigned bits)
{
    bool n = ia64_fr_is_nat(a), ib = (a.significand & IA64_FP_INTEGER_BIT) != 0;
    bool sm = bits & (a.sign ? 2 : 1);
    bool tm = ((bits & 4) && !a.significand && !a.exponent) || ((bits & 8) && unnormal(a)) ||
              ((bits & 16) && a.exponent && a.exponent != IA64_FP_SPECIAL_EXP && ib) ||
              ((bits & 32) && infinity(a));
    return n ? (bits & 256) != 0
             : (sm && tm) || ((bits & 64) && ib && snan(a)) ||
                   ((bits & 128) && ib && is_nan(a) && !snan(a));
}

static bool to_integer(struct ia64_fr a, bool uns, unsigned rc, uint64_t *value, bool *inexact,
                       bool *fpa)
{
    if (a.exponent == IA64_FP_SPECIAL_EXP)
        return false;
    if (!a.significand) {
        *value = 0;
        return true;
    }
    unsigned sh = clz64(a.significand);
    int exp = (a.exponent ? (int)a.exponent : 0xc001) - (int)sh;
    if (exp > IA64_FP_INTEGER_EXP)
        return false;
    struct ia64_fp_raw raw = {.sign = a.sign, .mant = a.significand << sh};
    __uint128_t rounded = ia64_fp_round_right(&raw, IA64_FP_INTEGER_EXP - exp, rc, inexact, fpa);
    if (rounded > UINT64_MAX)
        return false;
    if (uns) {
        if (a.sign && rounded)
            return false;
    } else if (rounded > (a.sign ? ((__uint128_t)1 << 63) : INT64_MAX))
        return false;
    *value = a.sign ? -(uint64_t)rounded : (uint64_t)rounded;
    return true;
}

static bool arithmetic(struct ia64_fr a, struct ia64_fr b, struct ia64_fr c, unsigned operation,
                       unsigned rc, bool multiply_only, struct ia64_fp_raw *raw,
                       struct ia64_fr *out, unsigned *flags)
{
    if (nan_response(a, b, c, out, flags))
        return true;
    bool ps = b.sign ^ c.sign ^ (operation == 2);
    bool as = a.sign ^ (operation == 1);
    bool bi = infinity(b), ci = infinity(c), ai = infinity(a);
    /* Pseudo-zero times infinity returns infinity after the D response;
     * canonical zero times infinity is Invalid (SDM Vol.1 Table 5-2). */
    bool invalid = (bi && !c.significand && !unnormal(c)) ||
                   (ci && !b.significand && !unnormal(b)) ||
                   (!multiply_only && (bi || ci) && ai && ps != as);
    if (invalid) {
        *out = indefinite();
        *flags |= FP_V;
        return true;
    }
    if (unnormal(a) || unnormal(b) || unnormal(c))
        *flags |= FP_D;
    if (bi || ci) {
        *out = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_SPECIAL_EXP, ps};
        return true;
    }
    if (ai) {
        *out = a;
        out->sign = as;
        return true;
    }
    if (multiply_only)
        a.sign = ps ^ (operation == 1);
    ia64_fp_muladd(&a, &b, &c, operation, rc, raw);
    return false;
}

struct ia64_result ia64_fp(struct vcpu *v, const Ia64Instruction *insn)
{
    if (!ia64_opcode_supported(insn->opcode))
        return ia64_unimplemented(insn);
    switch (insn->opcode) {
    case IA64_OP_FPABS:
    case IA64_OP_FPNEG:
    case IA64_OP_FPNEGABS:
    case IA64_OP_FPMERGE:
    case IA64_OP_FPMERGE_S:
    case IA64_OP_FPMERGE_SE:
    case IA64_OP_FPMIN:
    case IA64_OP_FPMAX:
    case IA64_OP_FPAMIN:
    case IA64_OP_FPAMAX:
    case IA64_OP_FPCMP:
    case IA64_OP_FPCVT:
    case IA64_OP_FPMA:
    case IA64_OP_FPMS:
    case IA64_OP_FPNMA:
    case IA64_OP_FPRCPA:
    case IA64_OP_FPRSQRTA:
        return ia64_fp_parallel(v, insn);
    default:
        break;
    }
    struct ia64_state *s = &v->s;
    const IA64FloatingOperands *op = &insn->operands.floating;
    unsigned dst = op->destination, r2 = op->source1, r3 = op->source2, r4 = op->auxiliary1;
    bool get = insn->opcode == IA64_OP_GETF_SIG || insn->opcode == IA64_OP_GETF_EXP ||
               insn->opcode == IA64_OP_GETF_S || insn->opcode == IA64_OP_GETF_D;
    bool set = insn->opcode == IA64_OP_SETF_SIG || insn->opcode == IA64_OP_SETF_EXP ||
               insn->opcode == IA64_OP_SETF_S || insn->opcode == IA64_OP_SETF_D;
    bool pair = insn->opcode == IA64_OP_FCMP || insn->opcode == IA64_OP_FCLASS;
    bool status = insn->opcode == IA64_OP_FSETC || insn->opcode == IA64_OP_FCLRF ||
                  insn->opcode == IA64_OP_FCHKF;
    bool unary = get || set || insn->opcode == IA64_OP_FCVT_XF || insn->opcode == IA64_OP_FCVT_FX ||
                 insn->opcode == IA64_OP_FCVT_FXU || insn->opcode == IA64_OP_FMOV ||
                 insn->opcode == IA64_OP_FCLASS;
    bool four = insn->opcode == IA64_OP_FMA || insn->opcode == IA64_OP_FMS ||
                insn->opcode == IA64_OP_FNMA || insn->opcode == IA64_OP_FSELECT ||
                insn->opcode == IA64_OP_XMA_L || insn->opcode == IA64_OP_XMA_H ||
                insn->opcode == IA64_OP_XMA_HU || insn->opcode == IA64_OP_XMPY_HU;
    if (!status && !get && !pair && dst < 2)
        return ia64_fault(IA64_FAULT_ILLEGAL, dst);
    if (!status) {
        struct ia64_result dis =
            ia64_fp_disabled(s, get || pair ? 0 : dst, set ? 0 : r2, unary ? 0 : r3, four ? r4 : 0);
        if (dis.kind != IA64_STEP_COMPLETE)
            return dis;
    }
    struct ia64_fr a = ia64_fr_read(s, r2), b = ia64_fr_read(s, r3), c = ia64_fr_read(s, r4);
    struct ia64_fr out = {0};
    bool approx = insn->opcode == IA64_OP_FRCPA || insn->opcode == IA64_OP_FRSQRTA;
    if (get) {
        uint64_t value = insn->opcode == IA64_OP_GETF_SIG ? a.significand
                         : insn->opcode == IA64_OP_GETF_EXP
                             ? a.exponent | ((uint64_t)a.sign << 17)
                             : ia64_fp_to_ieee(a, insn->opcode == IA64_OP_GETF_S);
        ia64_gr_write(s, dst, value, ia64_fr_is_nat(a));
        return ia64_complete();
    }
    if (set) {
        uint64_t value = ia64_gr_read(s, r2);
        if (ia64_nat_read(s, r2))
            out = ia64_fp_nat();
        else if (insn->opcode == IA64_OP_SETF_SIG)
            out = int_value(value);
        else if (insn->opcode == IA64_OP_SETF_EXP)
            out = (struct ia64_fr){IA64_FP_INTEGER_BIT, value & 0x1ffff, (value >> 17) & 1};
        else
            out = ia64_fp_from_ieee(value, insn->opcode == IA64_OP_SETF_S);
        ia64_fr_write(s, dst, out);
        return ia64_complete();
    }
    if (status) {
        unsigned sf = op->auxiliary1 & 3;
        unsigned shift = sf_shift(sf);
        if (insn->opcode == IA64_OP_FSETC) {
            unsigned value = (controls(s, 0) & r2) | r3;
            if ((!sf && (value & 64)) || ((value >> 2) & 3) == 1)
                return ia64_fault(IA64_FAULT_RESERVED, 0x30);
            s->ar[AR_FPSR] =
                (s->ar[AR_FPSR] & ~(UINT64_C(0x7f) << shift)) | ((uint64_t)value << shift);
        } else if (insn->opcode == IA64_OP_FCLRF)
            s->ar[AR_FPSR] &= ~(UINT64_C(0x3f) << (shift + 7));
        else {
            unsigned f = (s->ar[AR_FPSR] >> (shift + 7)) & 63;
            unsigned f0 = (s->ar[AR_FPSR] >> 13) & 63;
            if ((f & ~(s->ar[AR_FPSR] & 63)) || (f & ~f0))
                return (struct ia64_result){.kind = IA64_STEP_BRANCH,
                                            .info = insn->address + (uint64_t)op->immediate};
        }
        return ia64_complete();
    }
    struct ia64_fp_context ctx = {.a = a, .b = b, .c = c, .fpsr = s->ar[AR_FPSR]};
    struct ia64_result result = ia64_fp_compute(&ctx, insn);
    if (result.kind != IA64_STEP_COMPLETE &&
        !(result.kind == IA64_STEP_FAULT && result.fault == IA64_FAULT_FP_TRAP))
        return result;
    s->ar[AR_FPSR] = ctx.fpsr;
    if (pair) {
        ia64_pr_write(s, op->auxiliary1, ctx.pr1);
        ia64_pr_write(s, op->auxiliary2, ctx.pr2);
    } else {
        ia64_fr_write(s, dst, ctx.out);
        if (approx)
            ia64_pr_write(s, op->auxiliary2, ctx.pr2);
    }
    return result;
}

/* The scalar and packed lanes share the same exception/rounding core.
 * No architectural register file or external vCPU state is consulted. */
struct ia64_result ia64_fp_compute(struct ia64_fp_context *ctx, const Ia64Instruction *insn)
{
    const IA64FloatingOperands *op = &insn->operands.floating;
    unsigned sf = op->status_field, ctl = (ctx->fpsr >> sf_shift(sf)) & 0x7f;
    unsigned mask = sf && (ctl & 0x40) ? 0x3f : ctx->fpsr & 0x3f;
    unsigned r2 = op->source1;
    bool unary = insn->opcode == IA64_OP_FCVT_XF || insn->opcode == IA64_OP_FCVT_FX ||
                 insn->opcode == IA64_OP_FCVT_FXU || insn->opcode == IA64_OP_FMOV ||
                 insn->opcode == IA64_OP_FCLASS;
    bool four = insn->opcode == IA64_OP_FMA || insn->opcode == IA64_OP_FMS ||
                insn->opcode == IA64_OP_FNMA || insn->opcode == IA64_OP_FSELECT ||
                insn->opcode == IA64_OP_XMA_L || insn->opcode == IA64_OP_XMA_H ||
                insn->opcode == IA64_OP_XMA_HU || insn->opcode == IA64_OP_XMPY_HU;
    struct ia64_fr a = ctx->a, b = ctx->b, c = ctx->c, out = {0};
    unsigned flags = 0;
    bool fpa = false, pr1 = false, pr2 = false;
    bool has_nat =
        ia64_fr_is_nat(a) || (!unary && ia64_fr_is_nat(b)) || (four && ia64_fr_is_nat(c));
    if (insn->opcode == IA64_OP_FRSQRTA)
        has_nat = ia64_fr_is_nat(b);
    if (has_nat && insn->opcode != IA64_OP_FCLASS)
        out = ia64_fp_nat();
    else
        switch (insn->opcode) {
        case IA64_OP_FMOV:
            out = a;
            break;
        case IA64_OP_FMERGE:
        case IA64_OP_FMERGE_S:
        case IA64_OP_FMERGE_SE:
            out = b;
            out.sign = a.sign ^ (insn->opcode == IA64_OP_FMERGE);
            if (insn->opcode == IA64_OP_FMERGE_SE)
                out.exponent = a.exponent;
            break;
        case IA64_OP_FAND:
            out = int_value(a.significand & b.significand);
            break;
        case IA64_OP_FANDCM:
            out = int_value(a.significand & ~b.significand);
            break;
        case IA64_OP_FOR:
            out = int_value(a.significand | b.significand);
            break;
        case IA64_OP_FXOR:
            out = int_value(a.significand ^ b.significand);
            break;
        case IA64_OP_FSELECT:
            out = int_value((b.significand & a.significand) | (c.significand & ~a.significand));
            break;
        case IA64_OP_FSWAP:
        case IA64_OP_FSWAP_NL:
        case IA64_OP_FSWAP_NR: {
            uint32_t hi = b.significand, lo = a.significand >> 32;
            if (insn->opcode == IA64_OP_FSWAP_NL)
                hi ^= 0x80000000u;
            if (insn->opcode == IA64_OP_FSWAP_NR)
                lo ^= 0x80000000u;
            out = int_value(((uint64_t)hi << 32) | lo);
            break;
        }
        case IA64_OP_FMIX_LR:
        case IA64_OP_FMIX_L:
        case IA64_OP_FMIX_R: {
            uint32_t hi = insn->opcode == IA64_OP_FMIX_R ? a.significand : a.significand >> 32;
            uint32_t lo = insn->opcode == IA64_OP_FMIX_L ? b.significand >> 32 : b.significand;
            out = int_value(((uint64_t)hi << 32) | lo);
            break;
        }
        case IA64_OP_FSXT_L:
        case IA64_OP_FSXT_R: {
            unsigned shift = insn->opcode == IA64_OP_FSXT_L ? 32 : 0;
            uint64_t hi = ((a.significand >> (shift + 31)) & 1) ? UINT32_MAX : 0;
            out = int_value((hi << 32) | (uint32_t)(b.significand >> shift));
            break;
        }
        case IA64_OP_FPACK:
            out = int_value((ia64_fp_to_ieee(a, true) << 32) | ia64_fp_to_ieee(b, true));
            break;
        case IA64_OP_XMA_L:
        case IA64_OP_XMA_H:
        case IA64_OP_XMA_HU:
        case IA64_OP_XMPY_HU: {
            __uint128_t product = (__uint128_t)b.significand * c.significand;
            if (insn->opcode == IA64_OP_XMA_H) {
                if (b.significand >> 63)
                    product -= (__uint128_t)c.significand << 64;
                if (c.significand >> 63)
                    product -= (__uint128_t)b.significand << 64;
            }
            /* SDM Vol.3 xma: the addend is zero-extended in every form. */
            __uint128_t addend = a.significand;
            product += addend;
            out = int_value(insn->opcode == IA64_OP_XMA_L ? (uint64_t)product
                                                          : (uint64_t)(product >> 64));
            break;
        }
        case IA64_OP_FCVT_XF: {
            uint64_t mag = a.significand;
            out.sign = mag >> 63;
            if (out.sign)
                mag = -mag;
            if (mag) {
                unsigned sh = clz64(mag);
                out.significand = mag << sh;
                out.exponent = IA64_FP_INTEGER_EXP - sh;
            }
            break;
        }
        case IA64_OP_FCVT_FX:
        case IA64_OP_FCVT_FXU: {
            uint64_t value = UINT64_C(1) << 63;
            bool inexact = false;
            unsigned rc = (op->immediate & 2) ? 3 : (ctl >> 4) & 3;
            if (!to_integer(a, insn->opcode == IA64_OP_FCVT_FXU, rc, &value, &inexact, &fpa))
                flags = FP_V;
            else {
                if (unnormal(a))
                    flags |= FP_D;
                if (inexact)
                    flags |= FP_I;
            }
            out = int_value(value);
            break;
        }
        case IA64_OP_FCLASS:
            pr1 = class_member(a, op->immediate);
            pr2 = ia64_fr_is_nat(a) && !(op->immediate & 256) ? false : !pr1;
            break;
        case IA64_OP_FCMP:
        case IA64_OP_FMIN:
        case IA64_OP_FMAX:
        case IA64_OP_FAMIN:
        case IA64_OP_FAMAX: {
            bool cmp = insn->opcode == IA64_OP_FCMP;
            bool unordered = is_nan(a) || is_nan(b);
            bool quiet = cmp && (op->immediate == 0 || op->immediate == 3);
            if (unsupported(a) || unsupported(b) || snan(a) || snan(b) || (unordered && !quiet))
                flags |= FP_V;
            if (!unordered && (unnormal(a) || unnormal(b)))
                flags |= FP_D;
            int rel =
                unordered
                    ? 0
                    : compare(a, b, insn->opcode == IA64_OP_FAMIN || insn->opcode == IA64_OP_FAMAX);
            if (cmp) {
                pr1 = op->immediate == 3 ? unordered
                                         : !unordered && (op->immediate == 0   ? rel == 0
                                                          : op->immediate == 1 ? rel < 0
                                                                               : rel <= 0);
                pr2 = !pr1;
            } else {
                bool min = insn->opcode == IA64_OP_FMIN || insn->opcode == IA64_OP_FAMIN;
                out = !unordered && (min ? rel < 0 : rel > 0) ? a : b;
            }
            break;
        }
        case IA64_OP_FRCPA:
        case IA64_OP_FRSQRTA: {
            bool sqrt = insn->opcode == IA64_OP_FRSQRTA;
            if (sqrt)
                a = b;
            if (nan_response(a, sqrt ? (struct ia64_fr){0} : b, (struct ia64_fr){0}, &out, &flags))
                break;
            bool az = !a.significand, bz = !b.significand, ai = infinity(a), bi = infinity(b);
            if ((sqrt && a.sign && !az) || (!sqrt && ((az && bz) || (ai && bi)))) {
                out = indefinite();
                flags = FP_V;
                break;
            }
            if (!sqrt && bz && !az && !ai) {
                out = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_SPECIAL_EXP, a.sign ^ b.sign};
                flags = FP_Z;
                break;
            }
            if (unnormal(a) || (!sqrt && unnormal(b)))
                flags |= FP_D;
            if (sqrt && (az || ai)) {
                out = a;
                if (az)
                    out.exponent = 0;
                break;
            }
            if (!sqrt && (ai || bi || az || bz)) {
                out = (struct ia64_fr){.sign = a.sign ^ b.sign};
                if (ai) {
                    out.exponent = IA64_FP_SPECIAL_EXP;
                    out.significand = IA64_FP_INTEGER_BIT;
                }
                break;
            }
            struct ia64_fr den = sqrt ? a : b;
            unsigned sh = clz64(den.significand);
            int de = (den.exponent ? (int)den.exponent : 0xc001) - (int)sh;
            den.significand <<= sh;
            unsigned nsh = clz64(a.significand);
            int ne = (a.exponent ? (int)a.exponent : 0xc001) - (int)nsh;
            int diff = ne - de;
            if ((sqrt && de <= 64) ||
                (!sqrt && (de <= 0 || de >= 0x1fffc || diff <= 2 - IA64_FP_BIAS ||
                           diff >= IA64_FP_BIAS || ne <= 64))) {
                /* SDM Rev.2.3 Vol.2 §5.5, FP Fault vector: ISR.code.swa=bit 3.
             * No FR, predicate or FPSR effect precedes software assistance. */
                return ia64_fault(IA64_FAULT_FP, 8);
            }
            if (sqrt) {
                unsigned index = ((de & 1) << 7) | ((den.significand >> 56) & 127);
                out = (struct ia64_fr){(uint64_t)ia64_recip_sqrt_table[index] << 53,
                                       (uint32_t)(IA64_FP_BIAS - 1 - ((de - IA64_FP_BIAS) >> 1)),
                                       false};
            } else
                out = (struct ia64_fr){(uint64_t)ia64_recip_table[(den.significand >> 55) & 255]
                                           << 53,
                                       2 * IA64_FP_BIAS - 1 - de, den.sign};
            pr2 = true;
            break;
        }
        case IA64_OP_FADD:
        case IA64_OP_FSUB:
        case IA64_OP_FMPY:
        case IA64_OP_FMA:
        case IA64_OP_FMS:
        case IA64_OP_FNMA:
        case IA64_OP_FNORM: {
            unsigned operation = insn->opcode == IA64_OP_FMS    ? 1
                                 : insn->opcode == IA64_OP_FNMA ? 2
                                                                : 0;
            bool mul =
                insn->opcode == IA64_OP_FMPY || (four && !r2) || insn->opcode == IA64_OP_FNORM;
            if (insn->opcode == IA64_OP_FADD || insn->opcode == IA64_OP_FSUB) {
                c = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_BIAS, false};
                if (insn->opcode == IA64_OP_FSUB) {
                    struct ia64_fr left = a;
                    a = b;
                    b = left;
                    operation = 1;
                }
            } else if (insn->opcode == IA64_OP_FMPY) {
                c = b;
                b = a;
                a = (struct ia64_fr){0};
            } else if (insn->opcode == IA64_OP_FNORM) {
                a = (struct ia64_fr){0};
                c = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_BIAS, false};
            }
            struct ia64_fp_raw raw = {0};
            if (!arithmetic(a, b, c, operation, (ctl >> 4) & 3, mul, &raw, &out, &flags)) {
                if (flags & ~mask & 7)
                    return ia64_fault(IA64_FAULT_FP, flags & ~mask & 7);
                if (!ia64_fp_round(&raw, ctl, mask, op->precision, &out, &flags, &fpa))
                    return ia64_fault(IA64_FAULT_RESERVED, 0x30);
            }
            break;
        }
        default:
            return ia64_unimplemented(insn);
        }
    if (flags & ~mask & 7)
        return ia64_fault(IA64_FAULT_FP, flags & ~mask & 7);
    ctx->fpsr |= (uint64_t)flags << (sf_shift(sf) + 7);
    ctx->out = out;
    ctx->pr1 = pr1;
    ctx->pr2 = pr2;
    unsigned enabled = flags & ~mask & 0x38;
    if (enabled) {
        if (enabled & 0x18)
            enabled |= flags & FP_I;
        return ia64_fault(IA64_FAULT_FP_TRAP,
                          1 | ((uint64_t)enabled << 8) | (fpa ? UINT64_C(1) << 14 : 0));
    }
    return ia64_complete();
}
