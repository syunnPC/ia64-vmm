/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * IA-64 FP arithmetic on host IEEE single/double where bit-identical to the
 * soft-float (docs/floating-point.md).
 *
 * An operation runs on the host only when (SDM Rev.2.3 Vol.1):
 *  - its precision is that of a host format: .s/.d, or no completer with
 *    sf.pc 00/10 (Table 5-6); pc 11 (64 bits) only when the host result is
 *    exact. Every IA-64 exponent range (8, 11, 15, 17 bits) contains the
 *    host format's normal range, so wre does not change the host format;
 *  - every operand is a canonical zero or a normal value exactly
 *    representable in that format (Table 5-2): no NaN, infinity, NaTVal,
 *    unnormal (they set D, §5.2) or extra precision/range;
 *  - the host raised no flag other than inexact and the result is not a
 *    denormal. Tininess is detected after rounding by both (§5.4.3, Intel
 *    SDM Vol.1 §4.9.1.5), so a normal host result with no UE is not tiny
 *    in any IA-64 range; overflow raises OE. This leaves FTZ, wre and the
 *    o/u trap responses (exponent mod 2^17) to the soft-float;
 *  - an inexact result has its inexact trap disabled (Table 5-3, td).
 * The host rounds with MXCSR.RC = FPSR.sfx.rc (Table 5-5, Intel SDM Vol.1
 * Table 4-9 use the same encoding). The flags map one to one: MXCSR bits
 * 5:0 IE DE ZE OE UE PE are the status field's v d z o u i.
 * A product with an addend is fused (§5.4.8): host FMA3, or a separate
 * multiply that is exact. For 24/53-bit precision, fp_approx=1 also accepts
 * double-rounded fused operations and operands rounded to double. The
 * 64-bit model always preserves operands, fusion and result precision.
 */
#include "ia64/hostfp.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/platform.h"

static unsigned options, config;
static bool configured;

void ia64_hostfp_configure(unsigned flags)
{
    bool experimental = flags & IA64_HOSTFP_CFG_EXPERIMENTAL;
    options = (experimental ? IA64_HOSTFP_OPT_EXPERIMENTAL : 0) |
              (experimental && (flags & IA64_HOSTFP_CFG_APPROX) ? IA64_HOSTFP_OPT_APPROX : 0) |
              (!(flags & IA64_HOSTFP_CFG_NOFMA3) && ia64_hostfp_fma3() ? IA64_HOSTFP_OPT_FMA3 : 0) |
              (flags & IA64_HOSTFP_CFG_SOFT || !ia64_hostfp_host() ? IA64_HOSTFP_OPT_SOFT : 0);
    config = flags;
    configured = true;
}
/* Unconfigured (tests): exact, host FMA3 where present. */
unsigned ia64_hostfp_options(void)
{
    if (!configured)
        ia64_hostfp_configure(0);
    return options;
}
void engine_fp_configure(unsigned flags)
{
    ia64_hostfp_configure(flags & 31);
}

/* ---- formats ---- */

struct format {
    unsigned precision, width, ebits;
    uint32_t low, high; /* register exponents of the normal range */
};
static const struct format formats[] = {
    [IA64_HOSTFP_SINGLE] = {24, 23, 8, 0x0ff81, 0x1007e},
    [IA64_HOSTFP_DOUBLE] = {53, 52, 11, 0x0fc01, 0x103fe},
};

static bool is_zero(struct ia64_fr v)
{
    return !v.exponent && !v.significand;
}
static bool is_special(struct ia64_fr v)
{
    return v.exponent == IA64_FP_SPECIAL_EXP;
}
/* Table 5-2: unnormalized numbers, including pseudo-zeros, denormals and
 * integers; exponent 0 with a set integer bit is a pseudo-denormal. */
static bool is_unnormal(struct ia64_fr v)
{
    return !is_special(v) && !is_zero(v) && (!v.exponent || !(v.significand & IA64_FP_INTEGER_BIT));
}

static uint64_t encode(bool sign, uint32_t exponent, uint64_t significand, unsigned format)
{
    const struct format *f = &formats[format];
    uint64_t biased = exponent - (f->low - 1);
    uint64_t fraction = (significand << 1) >> (64 - f->width);
    return (uint64_t)sign << (f->width + f->ebits) | biased << f->width | fraction;
}
/* A normal register value, exactly. */
static bool pack(struct ia64_fr v, unsigned format, uint64_t *bits)
{
    const struct format *f = &formats[format];
    if (is_zero(v)) {
        *bits = (uint64_t)v.sign << (f->width + f->ebits);
        return true;
    }
    if (v.exponent < f->low || v.exponent > f->high ||
        (v.significand & ((UINT64_C(1) << (64 - f->precision)) - 1)))
        return false;
    *bits = encode(v.sign, v.exponent, v.significand, format);
    return true;
}
/* fp_approx: a normal register value rounded to the nearest double. */
static bool round_double(struct ia64_fr v, uint64_t *bits)
{
    if (pack(v, IA64_HOSTFP_DOUBLE, bits))
        return true;
    uint64_t keep = v.significand >> 11, rest = v.significand & 0x7ff;
    uint32_t exponent = v.exponent;
    if (rest > 0x400 || (rest == 0x400 && (keep & 1))) {
        ++keep;
        if (keep >> 53) {
            keep >>= 1;
            ++exponent;
        }
    }
    if (exponent < formats[IA64_HOSTFP_DOUBLE].low || exponent > formats[IA64_HOSTFP_DOUBLE].high)
        return false;
    *bits = encode(v.sign, exponent, keep << 11, IA64_HOSTFP_DOUBLE);
    return true;
}

/* The exact product of two host values is a normal of the format. */
static bool product_exact(uint64_t b, uint64_t c, unsigned format)
{
    const struct format *f = &formats[format];
    uint64_t mask = (UINT64_C(1) << f->width) - 1;
    unsigned eb = (b >> f->width) & ((1u << f->ebits) - 1),
             ec = (c >> f->width) & ((1u << f->ebits) - 1);
    if (!eb || !ec)
        return true; /* a zero factor */
    uint64_t mb = (b & mask) | (mask + 1), mc = (c & mask) | (mask + 1);
    mb >>= __builtin_ctzll(mb);
    mc >>= __builtin_ctzll(mc);
    __uint128_t p = (__uint128_t)mb * mc;
    unsigned length = p >> 64 ? 128 - (unsigned)__builtin_clzll((uint64_t)(p >> 64))
                              : 64 - (unsigned)__builtin_clzll((uint64_t)p);
    int bias = (1 << (f->ebits - 1)) - 1, e = (int)eb + (int)ec - 2 * bias;
    return length <= f->precision && e >= 1 - bias && e + 1 <= bias;
}

enum ia64_hostfp_reason ia64_hostfp_plan(struct ia64_fr a, struct ia64_fr b, struct ia64_fr c,
                                         unsigned operation, bool multiply, unsigned controls,
                                         unsigned precision, unsigned opts,
                                         struct ia64_hostfp_plan *plan)
{
    bool extended = !precision && ((controls >> 2) & 3) == 3;
    bool approx = (opts & IA64_HOSTFP_OPT_APPROX) && !extended;
    *plan = (struct ia64_hostfp_plan){0};
    if (precision == 1)
        plan->result = IA64_HOSTFP_SINGLE;
    else if (precision == 2)
        plan->result = IA64_HOSTFP_DOUBLE;
    else if (precision)
        return IA64_HOSTFP_PRECISION;
    else
        switch ((controls >> 2) & 3) {
        case 0:
            plan->result = IA64_HOSTFP_SINGLE;
            break;
        case 2:
            plan->result = IA64_HOSTFP_DOUBLE;
            break;
        case 3:
            plan->result = IA64_HOSTFP_DOUBLE;
            plan->exact_only = !approx;
            break;
        default:
            return IA64_HOSTFP_PRECISION;
        }
    const struct ia64_fr in[3] = {b, c, a};
    unsigned n = multiply ? 2 : 3;
    for (unsigned i = 0; i < n; ++i)
        if (is_special(in[i]))
            return IA64_HOSTFP_SPECIAL;
    for (unsigned i = 0; i < n; ++i)
        if (is_unnormal(in[i]))
            return IA64_HOSTFP_UNNORMAL;
    uint64_t bits[3] = {0};
    plan->format = plan->result;
    bool packed = true;
    for (unsigned i = 0; i < n && packed; ++i)
        packed = pack(in[i], plan->format, &bits[i]);
    if (!packed) {
        if (!approx)
            return IA64_HOSTFP_OPERAND;
        plan->format = IA64_HOSTFP_DOUBLE;
        for (unsigned i = 0; i < n; ++i)
            if (!round_double(in[i], &bits[i]))
                return IA64_HOSTFP_OPERAND;
    }
    bool exact = product_exact(bits[0], bits[1], plan->format);
    if (multiply)
        plan->kind = IA64_HOSTFP_MUL;
    else if (exact)
        plan->kind = IA64_HOSTFP_MULADD;
    else if (opts & IA64_HOSTFP_OPT_FMA3)
        plan->kind = IA64_HOSTFP_FMA;
    else if (approx)
        plan->kind = IA64_HOSTFP_MULADD;
    else
        return IA64_HOSTFP_NOFMA;
    const struct format *f = &formats[plan->format];
    uint64_t sign = UINT64_C(1) << (f->width + f->ebits);
    plan->b = bits[0] ^ (operation == 2 ? sign : 0);
    plan->c = bits[1];
    plan->a = multiply ? 0 : bits[2] ^ (operation == 1 ? sign : 0);
    return IA64_HOSTFP_DOWN;
}

/* Exponent field and fraction of a host result. */
static void fields(uint64_t bits, unsigned format, unsigned *exponent, uint64_t *fraction)
{
    const struct format *f = &formats[format];
    *exponent = (bits >> f->width) & ((1u << f->ebits) - 1);
    *fraction = bits & ((UINT64_C(1) << f->width) - 1);
}

enum ia64_hostfp_reason ia64_hostfp_accept(const struct ia64_hostfp_plan *plan, uint64_t bits,
                                           unsigned host_flags, unsigned masks, struct ia64_fr *out,
                                           unsigned *flags)
{
    unsigned exponent;
    uint64_t fraction;
    if (host_flags & (IA64_HOSTFP_PE - 1))
        return IA64_HOSTFP_RESULT;
    fields(bits, plan->result, &exponent, &fraction);
    if (exponent == (1u << formats[plan->result].ebits) - 1 || (!exponent && fraction))
        return IA64_HOSTFP_RESULT;
    bool inexact = host_flags & IA64_HOSTFP_PE;
    if (inexact && plan->exact_only)
        return IA64_HOSTFP_PRECISION;
    if (inexact && !(masks & FP_I))
        return IA64_HOSTFP_TRAP;
    *out = ia64_fp_from_ieee(bits, plan->result == IA64_HOSTFP_SINGLE);
    if (inexact)
        *flags |= FP_I;
    return IA64_HOSTFP_DOWN;
}

/* ---- parallel singles ---- */

static enum ia64_hostfp_reason lane_class(uint32_t x)
{
    unsigned exponent = (x >> 23) & 255;
    if (exponent == 255)
        return IA64_HOSTFP_SPECIAL;
    if (!exponent && (x & 0x7fffff))
        return IA64_HOSTFP_UNNORMAL;
    return IA64_HOSTFP_DOWN;
}

enum ia64_hostfp_reason ia64_hostfp_plan_packed(uint64_t a, uint64_t b, uint64_t c,
                                                unsigned operation, bool multiply, unsigned opts,
                                                struct ia64_hostfp_packed *plan)
{
    const uint64_t in[3] = {b, c, a}, signs = UINT64_C(0x8000000080000000);
    unsigned n = multiply ? 2 : 3;
    enum ia64_hostfp_reason worst = IA64_HOSTFP_DOWN;
    for (unsigned i = 0; i < n; ++i)
        for (unsigned l = 0; l < 2; ++l) {
            enum ia64_hostfp_reason r = lane_class((uint32_t)(in[i] >> (32 * l)));
            if (r == IA64_HOSTFP_SPECIAL)
                return r;
            if (r != IA64_HOSTFP_DOWN)
                worst = r;
        }
    if (worst != IA64_HOSTFP_DOWN)
        return worst;
    if (multiply)
        plan->kind = IA64_HOSTFP_MUL;
    else if (product_exact((uint32_t)b, (uint32_t)c, IA64_HOSTFP_SINGLE) &&
             product_exact(b >> 32, c >> 32, IA64_HOSTFP_SINGLE))
        plan->kind = IA64_HOSTFP_MULADD;
    else if (opts & IA64_HOSTFP_OPT_FMA3)
        plan->kind = IA64_HOSTFP_FMA;
    else if (opts & IA64_HOSTFP_OPT_APPROX)
        plan->kind = IA64_HOSTFP_MULADD;
    else
        return IA64_HOSTFP_NOFMA;
    plan->b = b ^ (operation == 2 ? signs : 0);
    plan->c = c;
    plan->a = multiply ? 0 : a ^ (operation == 1 ? signs : 0);
    return IA64_HOSTFP_DOWN;
}

enum ia64_hostfp_reason ia64_hostfp_accept_packed(uint64_t bits, unsigned host_flags,
                                                  unsigned masks, unsigned *flags)
{
    if (host_flags & (IA64_HOSTFP_PE - 1))
        return IA64_HOSTFP_RESULT;
    for (unsigned l = 0; l < 2; ++l) {
        uint32_t x = (uint32_t)(bits >> (32 * l));
        if (lane_class(x) != IA64_HOSTFP_DOWN)
            return IA64_HOSTFP_RESULT;
    }
    bool inexact = host_flags & IA64_HOSTFP_PE;
    if (inexact && !(masks & FP_I))
        return IA64_HOSTFP_TRAP;
    if (inexact)
        *flags |= FP_I;
    return IA64_HOSTFP_DOWN;
}

/* ---- statistics ---- */

static uint64_t counts[IA64VMM_MAX_CPUS][IA64_HOSTFP_CLASSES][IA64_HOSTFP_REASONS];
static uint64_t others[IA64VMM_MAX_CPUS];

static void bump(uint64_t *counter)
{
    __atomic_store_n(counter, __atomic_load_n(counter, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
}
static void count(const struct vcpu *v, unsigned cls, enum ia64_hostfp_reason reason)
{
    if (v->cfg.index < IA64VMM_MAX_CPUS)
        bump(&counts[v->cfg.index][cls][reason]);
}

void ia64_hostfp_stats(unsigned index, struct ia64_hostfp_stats *out)
{
    *out = (struct ia64_hostfp_stats){0};
    if (index >= IA64VMM_MAX_CPUS)
        return;
    for (unsigned c = 0; c < IA64_HOSTFP_CLASSES; ++c)
        for (unsigned r = 0; r < IA64_HOSTFP_REASONS; ++r)
            out->candidate[c][r] = __atomic_load_n(&counts[index][c][r], __ATOMIC_RELAXED);
    out->other = __atomic_load_n(&others[index], __ATOMIC_RELAXED);
}
void ia64_hostfp_stats_reset(void)
{
    for (unsigned i = 0; i < IA64VMM_MAX_CPUS; ++i) {
        for (unsigned c = 0; c < IA64_HOSTFP_CLASSES; ++c)
            for (unsigned r = 0; r < IA64_HOSTFP_REASONS; ++r)
                __atomic_store_n(&counts[i][c][r], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&others[i], 0, __ATOMIC_RELAXED);
    }
}
void engine_fp_log(const struct vcpu *v)
{
    struct ia64_hostfp_stats st;
    ia64_hostfp_stats(v->cfg.index, &st);
    static const char *const cls[] = {"scalar", "parallel"};
    for (unsigned c = 0; c < IA64_HOSTFP_CLASSES; ++c) {
        const uint64_t *n = st.candidate[c];
        LOGI(
            "vCPU%u fp %s host=%llu precision=%llu operand=%llu special=%llu unnormal=%llu nofma=%llu "
            "result=%llu trap=%llu unavailable=%llu",
            v->cfg.index, cls[c], (unsigned long long)n[IA64_HOSTFP_DOWN],
            (unsigned long long)n[IA64_HOSTFP_PRECISION],
            (unsigned long long)n[IA64_HOSTFP_OPERAND], (unsigned long long)n[IA64_HOSTFP_SPECIAL],
            (unsigned long long)n[IA64_HOSTFP_UNNORMAL], (unsigned long long)n[IA64_HOSTFP_NOFMA],
            (unsigned long long)n[IA64_HOSTFP_RESULT], (unsigned long long)n[IA64_HOSTFP_TRAP],
            (unsigned long long)n[IA64_HOSTFP_HOST]);
    }
    LOGI("vCPU%u fp other=%llu options=%u", v->cfg.index, (unsigned long long)st.other, options);
}
const char *ia64_hostfp_reason_name(enum ia64_hostfp_reason reason)
{
    static const char *const names[] = {
        [IA64_HOSTFP_DOWN] = "host",         [IA64_HOSTFP_PRECISION] = "precision",
        [IA64_HOSTFP_OPERAND] = "operand",   [IA64_HOSTFP_SPECIAL] = "special",
        [IA64_HOSTFP_UNNORMAL] = "unnormal", [IA64_HOSTFP_NOFMA] = "nofma",
        [IA64_HOSTFP_RESULT] = "result",     [IA64_HOSTFP_TRAP] = "trap",
        [IA64_HOSTFP_HOST] = "unavailable",
    };
    return reason < IA64_HOSTFP_REASONS ? names[reason] : "?";
}

/* ---- ia64_execute entry ---- */

static unsigned sf_shift(unsigned sf)
{
    return 6 + (sf & 3) * 13;
}

/* Check mode: the soft-float must agree; its effects (FR, FPSR, PSR.mf)
 * are undone before the host result commits. */
static void verify(struct vcpu *v, const Ia64Instruction *insn, unsigned dst, struct ia64_fr out,
                   uint64_t fpsr)
{
    struct ia64_state *s = &v->s;
    unsigned index = ia64_fr_index(s, dst);
    struct ia64_fr old = s->fr[index];
    uint64_t old_fpsr = s->ar[AR_FPSR], old_psr = s->psr;
    struct ia64_result r = ia64_fp(v, insn);
    struct ia64_fr soft = s->fr[index];
    bool equal = r.kind == IA64_STEP_COMPLETE && s->ar[AR_FPSR] == fpsr && soft.sign == out.sign &&
                 soft.exponent == out.exponent && soft.significand == out.significand;
    if (!equal) {
        LOGE(
            "hostfp: ip %llx slot %u opcode %u: host %u:%05x:%016llx fpsr %llx, soft %u:%05x:%016llx fpsr %llx kind %u",
            (unsigned long long)insn->address, insn->slot, insn->opcode, out.sign, out.exponent,
            (unsigned long long)out.significand, (unsigned long long)fpsr, soft.sign, soft.exponent,
            (unsigned long long)soft.significand, (unsigned long long)s->ar[AR_FPSR], r.kind);
        hal_panic("hostfp: host result differs from the soft-float");
    }
    s->fr[index] = old;
    s->ar[AR_FPSR] = old_fpsr;
    s->psr = old_psr;
}

/* Without host execution, statistics classify by the soft-float's own
 * result: the same conditions on the result and the flags it raised. */
static enum ia64_hostfp_reason soft_reason(struct vcpu *v, const Ia64Instruction *insn,
                                           const struct ia64_hostfp_plan *plan)
{
    const IA64FloatingOperands *op = &insn->operands.floating;
    unsigned shift = sf_shift(op->status_field) + 7;
    struct ia64_fp_context ctx = {.a = ia64_fr_read(&v->s, op->source1),
                                  .b = ia64_fr_read(&v->s, op->source2),
                                  .c = ia64_fr_read(&v->s, op->auxiliary1),
                                  .fpsr = v->s.ar[AR_FPSR] & ~(UINT64_C(63) << shift)};
    struct ia64_result r = ia64_fp_compute(&ctx, insn);
    unsigned flags = (ctx.fpsr >> shift) & 63;
    uint64_t bits;
    if (r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_FP_TRAP && flags == FP_I)
        return IA64_HOSTFP_TRAP;
    if (r.kind != IA64_STEP_COMPLETE || (flags & ~FP_I) || !pack(ctx.out, plan->result, &bits))
        return IA64_HOSTFP_RESULT;
    if ((flags & FP_I) && plan->exact_only)
        return IA64_HOSTFP_PRECISION;
    return IA64_HOSTFP_DOWN;
}

static bool scalar(struct vcpu *v, const Ia64Instruction *insn, struct ia64_result *result)
{
    struct ia64_state *s = &v->s;
    const IA64FloatingOperands *op = &insn->operands.floating;
    unsigned dst = op->destination, r2 = op->source1, r3 = op->source2, r4 = op->auxiliary1;
    Ia64Opcode code = insn->opcode;
    bool four = code == IA64_OP_FMA || code == IA64_OP_FMS || code == IA64_OP_FNMA;
    /* ia64_fp's order: support, f0/f1 destination, disabled registers. */
    if (!ia64_opcode_supported(code) || dst < 2 ||
        ia64_fp_disabled(s, dst, r2, r3, four ? r4 : 0).kind != IA64_STEP_COMPLETE)
        return false;
    struct ia64_fr a = ia64_fr_read(s, r2), b = ia64_fr_read(s, r3), c = ia64_fr_read(s, r4);
    if (ia64_fr_is_nat(a) || ia64_fr_is_nat(b) || (four && ia64_fr_is_nat(c))) {
        count(v, IA64_HOSTFP_SCALAR, IA64_HOSTFP_SPECIAL);
        return false;
    }
    /* ia64_fp_compute's operand rewrites of the pseudo-ops (Table 5-13). */
    unsigned operation = code == IA64_OP_FMS ? 1 : code == IA64_OP_FNMA ? 2 : 0;
    bool multiply = code == IA64_OP_FMPY || (four && !r2) || code == IA64_OP_FNORM;
    const struct ia64_fr one = {IA64_FP_INTEGER_BIT, IA64_FP_BIAS, false};
    if (code == IA64_OP_FADD || code == IA64_OP_FSUB) {
        c = one;
        if (code == IA64_OP_FSUB) {
            struct ia64_fr left = a;
            a = b;
            b = left;
            operation = 1;
        }
    } else if (code == IA64_OP_FMPY) {
        c = b;
        b = a;
        a = (struct ia64_fr){0};
    } else if (code == IA64_OP_FNORM) {
        a = (struct ia64_fr){0};
        c = one;
    }
    unsigned sf = op->status_field & 3;
    uint64_t fpsr = s->ar[AR_FPSR];
    unsigned controls = (fpsr >> sf_shift(sf)) & 0x7f;
    unsigned masks = sf && (controls & 0x40) ? 0x3f : fpsr & 0x3f;
    struct ia64_hostfp_plan plan;
    enum ia64_hostfp_reason reason = ia64_hostfp_plan(a, b, c, operation, multiply, controls,
                                                      op->precision, ia64_hostfp_options(), &plan);
    uint64_t bits = 0;
    unsigned host_flags = 0, flags = 0;
    struct ia64_fr out = {0};
    if (reason == IA64_HOSTFP_DOWN) {
        if (!(config & IA64_HOSTFP_CFG_SOFT) &&
            ia64_hostfp_run(&plan, (controls >> 4) & 3, &bits, &host_flags))
            reason = ia64_hostfp_accept(&plan, bits, host_flags, masks, &out, &flags);
        else {
            reason =
                config & IA64_HOSTFP_CFG_CLASSIFY ? soft_reason(v, insn, &plan) : IA64_HOSTFP_HOST;
            count(v, IA64_HOSTFP_SCALAR, reason);
            return false;
        }
    }
    count(v, IA64_HOSTFP_SCALAR, reason);
    if (reason != IA64_HOSTFP_DOWN)
        return false;
    fpsr |= (uint64_t)flags << (sf_shift(sf) + 7);
    if (config & IA64_HOSTFP_CFG_CHECK)
        verify(v, insn, dst, out, fpsr);
    s->ar[AR_FPSR] = fpsr;
    ia64_fr_write(s, dst, out);
    *result = ia64_complete();
    return true;
}

static bool packed(struct vcpu *v, const Ia64Instruction *insn, struct ia64_result *result)
{
    struct ia64_state *s = &v->s;
    const IA64FloatingOperands *op = &insn->operands.floating;
    unsigned dst = op->destination;
    /* ia64_fp_parallel's order: f0/f1 destination, disabled registers, NaTVal. */
    if (!ia64_opcode_supported(insn->opcode) || dst < 2 ||
        ia64_fp_disabled(s, dst, op->source1, op->source2, op->auxiliary1).kind !=
            IA64_STEP_COMPLETE)
        return false;
    struct ia64_fr a = ia64_fr_read(s, op->source1), b = ia64_fr_read(s, op->source2),
                   c = ia64_fr_read(s, op->auxiliary1);
    if (ia64_fr_is_nat(a) || ia64_fr_is_nat(b) || ia64_fr_is_nat(c)) {
        count(v, IA64_HOSTFP_PARALLEL, IA64_HOSTFP_SPECIAL);
        return false;
    }
    unsigned operation = insn->opcode == IA64_OP_FPMS ? 1 : insn->opcode == IA64_OP_FPNMA ? 2 : 0;
    unsigned sf = op->status_field & 3;
    uint64_t fpsr = s->ar[AR_FPSR];
    unsigned controls = (fpsr >> sf_shift(sf)) & 0x7f;
    unsigned masks = sf && (controls & 0x40) ? 0x3f : fpsr & 0x3f;
    struct ia64_hostfp_packed plan;
    enum ia64_hostfp_reason reason =
        ia64_hostfp_plan_packed(a.significand, b.significand, c.significand, operation,
                                !op->source1, ia64_hostfp_options(), &plan);
    uint64_t bits = 0;
    unsigned host_flags = 0, flags = 0;
    if (reason == IA64_HOSTFP_DOWN) {
        if (!(config & IA64_HOSTFP_CFG_SOFT) &&
            ia64_hostfp_run_packed(&plan, (controls >> 4) & 3, &bits, &host_flags))
            reason = ia64_hostfp_accept_packed(bits, host_flags, masks, &flags);
        else
            reason = IA64_HOSTFP_HOST;
    }
    count(v, IA64_HOSTFP_PARALLEL, reason);
    if (reason != IA64_HOSTFP_DOWN)
        return false;
    struct ia64_fr out = {bits, IA64_FP_INTEGER_EXP, false};
    fpsr |= (uint64_t)flags << (sf_shift(sf) + 7);
    if (config & IA64_HOSTFP_CFG_CHECK)
        verify(v, insn, dst, out, fpsr);
    s->ar[AR_FPSR] = fpsr;
    ia64_fr_write(s, dst, out);
    *result = ia64_complete();
    return true;
}

struct ia64_result ia64_hostfp(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_result result;
    switch (insn->opcode) {
    case IA64_OP_FMA:
    case IA64_OP_FMS:
    case IA64_OP_FNMA:
    case IA64_OP_FADD:
    case IA64_OP_FSUB:
    case IA64_OP_FMPY:
    case IA64_OP_FNORM:
        if (scalar(v, insn, &result))
            return result;
        break;
    case IA64_OP_FPMA:
    case IA64_OP_FPMS:
    case IA64_OP_FPNMA:
        if (packed(v, insn, &result))
            return result;
        break;
    /* The other arithmetic instructions (Table 5-12). */
    case IA64_OP_FCMP:
    case IA64_OP_FMIN:
    case IA64_OP_FMAX:
    case IA64_OP_FAMIN:
    case IA64_OP_FAMAX:
    case IA64_OP_FCVT_FX:
    case IA64_OP_FCVT_FXU:
    case IA64_OP_FCVT_XF:
    case IA64_OP_FRCPA:
    case IA64_OP_FRSQRTA:
    case IA64_OP_FPRCPA:
    case IA64_OP_FPRSQRTA:
    case IA64_OP_FPCMP:
    case IA64_OP_FPMIN:
    case IA64_OP_FPMAX:
    case IA64_OP_FPAMIN:
    case IA64_OP_FPAMAX:
    case IA64_OP_FPCVT:
        if (v->cfg.index < IA64VMM_MAX_CPUS)
            bump(&others[v->cfg.index]);
        break;
    default:
        break;
    }
    return ia64_fp(v, insn);
}
