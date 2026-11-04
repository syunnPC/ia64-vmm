/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host IEEE execution of IA-64 FP arithmetic (ia64/hostfp.h).
 *
 * The classification cases follow the SDM Rev.2.3 Vol.1 tables: register
 * encodings (Table 5-2), status field controls and computation models
 * (Tables 5-4..5-6), the exception rules of §5.4 (Figures 5-11, 5-12) and
 * §5.3.3 (fma with a +0.0 addend register is not the IEEE multiply). The
 * host side follows the Intel SDM Vol.1 §4.8.4 Table 4-9 (RC encoding),
 * §4.9.1.5 (tininess after rounding; masked underflow only when tiny and
 * inexact) and §10.2.3 (MXCSR). Execution is compared with the soft-float,
 * the reference: result register, FPSR, PSR and outcome bit-identical.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/hostfp.h"
#include "ia64vmm/host_simd.h"

static int simd_override = -1;
unsigned __real_hal_host_simd(void);
unsigned __wrap_hal_host_simd(void)
{
    return simd_override < 0 ? __real_hal_host_simd() : (unsigned)simd_override;
}

static unsigned checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define IBIT IA64_FP_INTEGER_BIT
static struct ia64_fr fr(bool sign, unsigned exponent, uint64_t significand)
{
    return (struct ia64_fr){significand, exponent, sign};
}
static bool same(struct ia64_fr a, struct ia64_fr b)
{
    return a.sign == b.sign && a.exponent == b.exponent && a.significand == b.significand;
}
static const struct ia64_fr one = {IBIT, 0xffff, false}, zero = {0, 0, false};

/* Operand classification through a multiply b * 1.0. */
static enum ia64_hostfp_reason classify(struct ia64_fr b, unsigned controls, unsigned precision)
{
    struct ia64_hostfp_plan plan;
    return ia64_hostfp_plan(zero, b, one, 0, true, controls, precision, 0, &plan);
}

static void test_models(void)
{
    struct ia64_hostfp_plan p;
    /* Table 5-6: the .s/.d completer selects 24/53 bits whatever pc says;
     * without a completer pc 00/10/11 select 24/53/64 bits, 01 is reserved.
     * wre and ftz change the range and tiny results only. */
    for (unsigned low = 0; low < 4; ++low)
        for (unsigned pc = 0; pc < 4; ++pc) {
            unsigned controls = low | pc << 2;
            CHECK(ia64_hostfp_plan(zero, one, one, 0, true, controls, 1, 0, &p) ==
                  IA64_HOSTFP_DOWN);
            CHECK(p.format == IA64_HOSTFP_SINGLE && p.result == IA64_HOSTFP_SINGLE &&
                  !p.exact_only);
            CHECK(ia64_hostfp_plan(zero, one, one, 0, true, controls, 2, 0, &p) ==
                  IA64_HOSTFP_DOWN);
            CHECK(p.format == IA64_HOSTFP_DOUBLE && p.result == IA64_HOSTFP_DOUBLE &&
                  !p.exact_only);
            enum ia64_hostfp_reason r =
                ia64_hostfp_plan(zero, one, one, 0, true, controls, 0, 0, &p);
            if (pc == 1) {
                CHECK(r == IA64_HOSTFP_PRECISION);
                continue;
            }
            CHECK(r == IA64_HOSTFP_DOWN);
            CHECK(p.result == (pc ? IA64_HOSTFP_DOUBLE : IA64_HOSTFP_SINGLE));
            CHECK(p.exact_only == (pc == 3));
            /* Approximation still preserves the 64-bit computation model. */
            CHECK(ia64_hostfp_plan(zero, one, one, 0, true, controls, 0, IA64_HOSTFP_OPT_APPROX,
                                   &p) == IA64_HOSTFP_DOWN);
            CHECK(p.exact_only == (pc == 3));
        }
}

static void test_operands(void)
{
    /* Table 5-2 encodings, for the single (.s) and double (.d) models. */
    static const struct {
        struct ia64_fr v;
        enum ia64_hostfp_reason single, dbl;
    } cases[] = {
        {{0, 0, false}, IA64_HOSTFP_DOWN, IA64_HOSTFP_DOWN},         /* +0 (FR 0) */
        {{0, 0, true}, IA64_HOSTFP_DOWN, IA64_HOSTFP_DOWN},          /* -0 */
        {{IBIT, 0xffff, false}, IA64_HOSTFP_DOWN, IA64_HOSTFP_DOWN}, /* +1 (FR 1) */
        {{UINT64_C(0xffffff0000000000), 0x1007e, true},
         IA64_HOSTFP_DOWN,
         IA64_HOSTFP_DOWN},                                              /* single max */
        {{IBIT, 0x0ff81, false}, IA64_HOSTFP_DOWN, IA64_HOSTFP_DOWN},    /* single min normal */
        {{IBIT, 0x0ff80, false}, IA64_HOSTFP_OPERAND, IA64_HOSTFP_DOWN}, /* below single range */
        {{IBIT, 0x1007f, false}, IA64_HOSTFP_OPERAND, IA64_HOSTFP_DOWN}, /* above single range */
        {{IBIT | UINT64_C(1) << 39, 0xffff, false},
         IA64_HOSTFP_OPERAND,
         IA64_HOSTFP_DOWN}, /* 25 bits */
        {{UINT64_C(0xfffffffffffff800), 0x103fe, false},
         IA64_HOSTFP_OPERAND,
         IA64_HOSTFP_DOWN},                                                 /* double max */
        {{IBIT, 0x0fc01, true}, IA64_HOSTFP_OPERAND, IA64_HOSTFP_DOWN},     /* double min normal */
        {{IBIT, 0x0fc00, false}, IA64_HOSTFP_OPERAND, IA64_HOSTFP_OPERAND}, /* below double range */
        {{IBIT, 0x103ff, false}, IA64_HOSTFP_OPERAND, IA64_HOSTFP_OPERAND}, /* above double range */
        {{IBIT | UINT64_C(1) << 10, 0xffff, false},
         IA64_HOSTFP_OPERAND,
         IA64_HOSTFP_OPERAND},                                                 /* 54 bits */
        {{IBIT | 1, 0xffff, false}, IA64_HOSTFP_OPERAND, IA64_HOSTFP_OPERAND}, /* 64 bits */
        {{IBIT, 0x00001, false},
         IA64_HOSTFP_OPERAND,
         IA64_HOSTFP_OPERAND}, /* register format min */
        {{IBIT, 0x1fffe, false},
         IA64_HOSTFP_OPERAND,
         IA64_HOSTFP_OPERAND}, /* register format max */
        {{IBIT, 0x1ffff, false}, IA64_HOSTFP_SPECIAL, IA64_HOSTFP_SPECIAL}, /* +infinity */
        {{IBIT, 0x1ffff, true}, IA64_HOSTFP_SPECIAL, IA64_HOSTFP_SPECIAL},  /* -infinity */
        {{IBIT | IA64_FP_QUIET_BIT, 0x1ffff, true},
         IA64_HOSTFP_SPECIAL,
         IA64_HOSTFP_SPECIAL}, /* QNaN indefinite */
        {{IBIT | 1, 0x1ffff, false}, IA64_HOSTFP_SPECIAL, IA64_HOSTFP_SPECIAL}, /* SNaN */
        {{1, 0x1ffff, false}, IA64_HOSTFP_SPECIAL, IA64_HOSTFP_SPECIAL},        /* pseudo-NaN */
        {{0, 0x1ffff, false}, IA64_HOSTFP_SPECIAL, IA64_HOSTFP_SPECIAL},   /* pseudo-infinity */
        {{0, 0x00001, false}, IA64_HOSTFP_UNNORMAL, IA64_HOSTFP_UNNORMAL}, /* pseudo-zero */
        {{0, 0x1003e, false}, IA64_HOSTFP_UNNORMAL, IA64_HOSTFP_UNNORMAL}, /* pseudo-zero */
        {{0, 0x1fffe, true}, IA64_HOSTFP_UNNORMAL, IA64_HOSTFP_UNNORMAL},  /* pseudo-zero */
        {{UINT64_C(1) << 62, 0x0ff81, false},
         IA64_HOSTFP_UNNORMAL,
         IA64_HOSTFP_UNNORMAL}, /* single denormal */
        {{UINT64_C(1) << 62, 0x0fc01, true},
         IA64_HOSTFP_UNNORMAL,
         IA64_HOSTFP_UNNORMAL},                                            /* double denormal */
        {{5, 0x1003e, false}, IA64_HOSTFP_UNNORMAL, IA64_HOSTFP_UNNORMAL}, /* positive integer */
        {{1, 0, false}, IA64_HOSTFP_UNNORMAL, IA64_HOSTFP_UNNORMAL}, /* double-extended denormal */
        {{IBIT, 0, false}, IA64_HOSTFP_UNNORMAL, IA64_HOSTFP_UNNORMAL}, /* pseudo-denormal */
        {{UINT64_C(1) << 62, 0xffff, false},
         IA64_HOSTFP_UNNORMAL,
         IA64_HOSTFP_UNNORMAL}, /* unnormal */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i)
        for (unsigned controls = 0; controls < 4; ++controls) {
            CHECK(classify(cases[i].v, controls, 1) == cases[i].single);
            CHECK(classify(cases[i].v, controls, 2) == cases[i].dbl);
            /* Every operand is classified: the same value as c and as an addend. */
            struct ia64_hostfp_plan p;
            CHECK(ia64_hostfp_plan(zero, one, cases[i].v, 0, true, controls, 2, 0, &p) ==
                  cases[i].dbl);
            enum ia64_hostfp_reason r =
                ia64_hostfp_plan(cases[i].v, one, one, 0, false, controls, 2, 0, &p);
            CHECK(r == cases[i].dbl);
        }
    /* fp_approx widens precision only: operands are rounded to double, but
     * specials, unnormals and values beyond double's range stay exact. */
    struct ia64_hostfp_plan p;
    CHECK(ia64_hostfp_plan(zero, fr(0, 0xffff, IBIT | 1), one, 0, true, 0, 2,
                           IA64_HOSTFP_OPT_APPROX, &p) == IA64_HOSTFP_DOWN);
    CHECK(p.b == UINT64_C(0x3ff0000000000000));
    CHECK(ia64_hostfp_plan(zero, fr(0, 0xffff, UINT64_MAX), one, 0, true, 0, 2,
                           IA64_HOSTFP_OPT_APPROX, &p) == IA64_HOSTFP_DOWN);
    CHECK(p.b == UINT64_C(0x4000000000000000)); /* rounds up to 2.0 */
    /* Ties to even: 1 + 2^-53 to 1, 1 + 2^-52 + 2^-53 to 1 + 2^-51. */
    CHECK(ia64_hostfp_plan(zero, fr(0, 0xffff, IBIT | 0x400), one, 0, true, 0, 2,
                           IA64_HOSTFP_OPT_APPROX, &p) == IA64_HOSTFP_DOWN);
    CHECK(p.b == UINT64_C(0x3ff0000000000000));
    CHECK(ia64_hostfp_plan(zero, fr(0, 0xffff, IBIT | 0xc00), one, 0, true, 0, 2,
                           IA64_HOSTFP_OPT_APPROX, &p) == IA64_HOSTFP_DOWN);
    CHECK(p.b == UINT64_C(0x3ff0000000000002));
    CHECK(ia64_hostfp_plan(zero, fr(1, 0x103fe, UINT64_MAX), one, 0, true, 0, 2,
                           IA64_HOSTFP_OPT_APPROX, &p) == IA64_HOSTFP_OPERAND);
    CHECK(ia64_hostfp_plan(zero, fr(0, 0x103ff, IBIT), one, 0, true, 0, 2, IA64_HOSTFP_OPT_APPROX,
                           &p) == IA64_HOSTFP_OPERAND);
    CHECK(ia64_hostfp_plan(zero, fr(0, 0x0fc00, IBIT), one, 0, true, 0, 2, IA64_HOSTFP_OPT_APPROX,
                           &p) == IA64_HOSTFP_OPERAND);
    CHECK(ia64_hostfp_plan(zero, fr(0, 0x1ffff, IBIT), one, 0, true, 0, 2, IA64_HOSTFP_OPT_APPROX,
                           &p) == IA64_HOSTFP_SPECIAL);
    CHECK(ia64_hostfp_plan(zero, fr(0, 0x0fc01, 1u << 20), one, 0, true, 0, 2,
                           IA64_HOSTFP_OPT_APPROX, &p) == IA64_HOSTFP_UNNORMAL);
    /* A single result from double operands computes in double. */
    CHECK(ia64_hostfp_plan(zero, fr(0, 0xffff, IBIT | 1u << 20), one, 0, true, 0, 1,
                           IA64_HOSTFP_OPT_APPROX, &p) == IA64_HOSTFP_DOWN);
    CHECK(p.format == IA64_HOSTFP_DOUBLE && p.result == IA64_HOSTFP_SINGLE);
}

static void test_kinds(void)
{
    struct ia64_hostfp_plan p;
    struct ia64_fr three = fr(0, 0x10000, UINT64_C(0xc000000000000000));
    struct ia64_fr wide = fr(0, 0xffff, UINT64_C(0xfffffffffffff800)); /* 53 bits */
    /* fmpy and fma with FR 0: the IEEE multiply. */
    CHECK(ia64_hostfp_plan(zero, wide, wide, 0, true, 0, 2, 0, &p) == IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MUL);
    /* Multiplicand FR 1 (fadd, fsub): the product is the operand. */
    CHECK(ia64_hostfp_plan(wide, wide, one, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MULADD);
    /* An exact product needs no fused operation. */
    CHECK(ia64_hostfp_plan(wide, three, three, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MULADD);
    /* A fused product with more than 53 bits needs FMA3; fp_approx rounds twice. */
    CHECK(ia64_hostfp_plan(one, wide, wide, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_NOFMA);
    CHECK(ia64_hostfp_plan(one, wide, wide, 0, false, 0, 2, IA64_HOSTFP_OPT_FMA3, &p) ==
          IA64_HOSTFP_DOWN);
    CHECK(p.kind == IA64_HOSTFP_FMA);
    CHECK(ia64_hostfp_plan(one, wide, wide, 0, false, 0, 2, IA64_HOSTFP_OPT_APPROX, &p) ==
          IA64_HOSTFP_DOWN);
    CHECK(p.kind == IA64_HOSTFP_MULADD);
    /* (2^27-1)(2^26-1) has 53 bits, (2^27-1)^2 has 54; (2^26+1)^2 = 2^52 +
     * 2^27 + 1 has 53 although both factors have 27. */
    struct ia64_fr b27 = fr(0, 0xffff, UINT64_C(0xffffffe000000000)),
                   b26 = fr(0, 0xffff, UINT64_C(0xffffffc000000000));
    struct ia64_fr p27 = fr(0, 0xffff, IBIT | UINT64_C(1) << 37);
    CHECK(ia64_hostfp_plan(one, b27, b26, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MULADD);
    CHECK(ia64_hostfp_plan(one, b27, b27, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_NOFMA);
    CHECK(ia64_hostfp_plan(one, p27, p27, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MULADD);
    /* Single: (2^12-1)^2 has 24 bits, (2^13-1)(2^12-1) has 25. */
    struct ia64_fr s12 = fr(0, 0xffff, UINT64_C(0xfff0000000000000)),
                   s13 = fr(0, 0xffff, UINT64_C(0xfff8000000000000));
    CHECK(ia64_hostfp_plan(one, s12, s12, 0, false, 0, 1, 0, &p) == IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MULADD);
    CHECK(ia64_hostfp_plan(one, s13, s12, 0, false, 0, 1, 0, &p) == IA64_HOSTFP_NOFMA);
    /* An exact product outside the normal range is not formed separately. */
    struct ia64_fr big = fr(0, 0xffff + 600, IBIT), tiny = fr(0, 0xffff - 600, IBIT);
    CHECK(ia64_hostfp_plan(one, big, big, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_NOFMA);
    CHECK(ia64_hostfp_plan(one, tiny, tiny, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_NOFMA);
    CHECK(ia64_hostfp_plan(one, big, tiny, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MULADD);
    /* The product range: 2^-511 2^-511 is the min normal, 2^-511 2^-512 a
     * denormal; 2^511 2^511 is normal, 2^512 2^511 counts as out of range
     * (the significand product may reach 2^(e+1)). */
    struct ia64_fr m511 = fr(0, 0xffff - 511, IBIT), m512 = fr(0, 0xffff - 512, IBIT);
    struct ia64_fr p511 = fr(0, 0xffff + 511, IBIT), p512 = fr(0, 0xffff + 512, IBIT);
    CHECK(ia64_hostfp_plan(one, m511, m511, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MULADD);
    CHECK(ia64_hostfp_plan(one, m511, m512, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_NOFMA);
    CHECK(ia64_hostfp_plan(one, p511, p511, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MULADD);
    CHECK(ia64_hostfp_plan(one, p512, p511, 0, false, 0, 2, 0, &p) == IA64_HOSTFP_NOFMA);
    /* Signs: fms negates the addend, fnma the product (b). */
    CHECK(ia64_hostfp_plan(three, one, three, 1, false, 0, 2, 0, &p) == IA64_HOSTFP_DOWN);
    CHECK(p.a == UINT64_C(0xc008000000000000) && p.b == UINT64_C(0x3ff0000000000000) &&
          p.c == UINT64_C(0x4008000000000000));
    CHECK(ia64_hostfp_plan(three, one, three, 2, false, 0, 2, 0, &p) == IA64_HOSTFP_DOWN);
    CHECK(p.a == UINT64_C(0x4008000000000000) && p.b == UINT64_C(0xbff0000000000000));
    CHECK(ia64_hostfp_plan(zero, one, three, 0, true, 0, 1, 0, &p) == IA64_HOSTFP_DOWN);
    CHECK(p.b == 0x3f800000 && p.c == 0x40400000);
}

static void test_accept(void)
{
    struct ia64_hostfp_plan p;
    struct ia64_fr out;
    unsigned flags;
    CHECK(ia64_hostfp_plan(zero, one, one, 0, true, 0, 2, 0, &p) == IA64_HOSTFP_DOWN);
    /* 1.5: Table 5-2 IEEE double normal, biased exponent 0x0FFFF. */
    flags = 0;
    CHECK(ia64_hostfp_accept(&p, UINT64_C(0x3ff8000000000000), 0, 63, &out, &flags) ==
          IA64_HOSTFP_DOWN);
    CHECK(same(out, fr(0, 0xffff, UINT64_C(0xc000000000000000))) && !flags);
    CHECK(ia64_hostfp_accept(&p, UINT64_C(0x3ff8000000000000), IA64_HOSTFP_PE, 63, &out, &flags) ==
          IA64_HOSTFP_DOWN);
    CHECK(flags == FP_I);
    /* Any host flag other than PE: invalid, denormal, zero divide, overflow, underflow. */
    for (unsigned f = 1; f < 32; f <<= 1) {
        flags = 0;
        CHECK(ia64_hostfp_accept(&p, UINT64_C(0x3ff8000000000000), f | IA64_HOSTFP_PE, 63, &out,
                                 &flags) == IA64_HOSTFP_RESULT);
        CHECK(!flags);
    }
    /* Denormal results: the IA-64 range may be wider, FTZ or an enabled
     * underflow trap differ even when exact (§5.4.3). */
    CHECK(ia64_hostfp_accept(&p, 1, 0, 63, &out, &flags) == IA64_HOSTFP_RESULT);
    CHECK(ia64_hostfp_accept(&p, UINT64_C(0x800fffffffffffff), 0, 63, &out, &flags) ==
          IA64_HOSTFP_RESULT);
    CHECK(ia64_hostfp_accept(&p, UINT64_C(0x7ff0000000000000), 0, 63, &out, &flags) ==
          IA64_HOSTFP_RESULT);
    CHECK(ia64_hostfp_accept(&p, UINT64_C(0x7ff8000000000000), 0, 63, &out, &flags) ==
          IA64_HOSTFP_RESULT);
    /* Zeros and the min normal with known flags are results. */
    CHECK(ia64_hostfp_accept(&p, UINT64_C(0x8000000000000000), 0, 63, &out, &flags) ==
          IA64_HOSTFP_DOWN);
    CHECK(same(out, fr(1, 0, 0)));
    CHECK(ia64_hostfp_accept(&p, UINT64_C(0x0010000000000000), 0, 63, &out, &flags) ==
          IA64_HOSTFP_DOWN);
    CHECK(same(out, fr(0, 0x0fc01, IBIT)));
    /* Inexact with the inexact trap enabled (traps.id clear, Table 5-3). */
    CHECK(ia64_hostfp_accept(&p, UINT64_C(0x3ff8000000000000), IA64_HOSTFP_PE, 63 & ~FP_I, &out,
                             &flags) == IA64_HOSTFP_TRAP);
    CHECK(ia64_hostfp_accept(&p, UINT64_C(0x3ff8000000000000), 0, 0, &out, &flags) ==
          IA64_HOSTFP_DOWN);
    /* 64-bit precision: only exact host results. */
    CHECK(ia64_hostfp_plan(zero, one, one, 0, true, 12, 0, 0, &p) == IA64_HOSTFP_DOWN &&
          p.exact_only);
    CHECK(ia64_hostfp_accept(&p, UINT64_C(0x3ff8000000000000), IA64_HOSTFP_PE, 63, &out, &flags) ==
          IA64_HOSTFP_PRECISION);
    CHECK(ia64_hostfp_accept(&p, UINT64_C(0x3ff8000000000000), 0, 63, &out, &flags) ==
          IA64_HOSTFP_DOWN);
    /* Single results: biased exponent 0x0FF80 + e. */
    CHECK(ia64_hostfp_plan(zero, one, one, 0, true, 0, 1, 0, &p) == IA64_HOSTFP_DOWN);
    CHECK(ia64_hostfp_accept(&p, 0x00800000, 0, 63, &out, &flags) == IA64_HOSTFP_DOWN);
    CHECK(same(out, fr(0, 0x0ff81, IBIT)));
    CHECK(ia64_hostfp_accept(&p, 0xff7fffff, 0, 63, &out, &flags) == IA64_HOSTFP_DOWN);
    CHECK(same(out, fr(1, 0x1007e, UINT64_C(0xffffff0000000000))));
    CHECK(ia64_hostfp_accept(&p, 0x007fffff, 0, 63, &out, &flags) == IA64_HOSTFP_RESULT);
}

static void test_packed_plans(void)
{
    struct ia64_hostfp_packed p;
    unsigned flags = 0;
    const uint64_t ones = UINT64_C(0x3f8000003f800000), threes = UINT64_C(0x4040000040400000);
    CHECK(ia64_hostfp_plan_packed(0, ones, threes, 0, true, 0, &p) == IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MUL);
    CHECK(ia64_hostfp_plan_packed(ones, ones, threes, 0, false, 0, &p) == IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MULADD);
    /* Lane signs: fms negates the addend lanes, fnma the b lanes. */
    CHECK(ia64_hostfp_plan_packed(ones, ones, threes, 1, false, 0, &p) == IA64_HOSTFP_DOWN);
    CHECK(p.a == UINT64_C(0xbf800000bf800000) && p.b == ones);
    CHECK(ia64_hostfp_plan_packed(ones, ones, threes, 2, false, 0, &p) == IA64_HOSTFP_DOWN);
    CHECK(p.a == ones && p.b == UINT64_C(0xbf800000bf800000));
    /* Either lane: denormal (D), infinity/NaN, zero. */
    CHECK(ia64_hostfp_plan_packed(0, UINT64_C(0x3f80000000000001), threes, 0, true, 0, &p) ==
          IA64_HOSTFP_UNNORMAL);
    CHECK(ia64_hostfp_plan_packed(0, UINT64_C(0x807fffff3f800000), threes, 0, true, 0, &p) ==
          IA64_HOSTFP_UNNORMAL);
    CHECK(ia64_hostfp_plan_packed(0, UINT64_C(0x3f8000007f800000), threes, 0, true, 0, &p) ==
          IA64_HOSTFP_SPECIAL);
    CHECK(ia64_hostfp_plan_packed(UINT64_C(0x7fc0000000000000), ones, threes, 0, false, 0, &p) ==
          IA64_HOSTFP_SPECIAL);
    CHECK(ia64_hostfp_plan_packed(0, UINT64_C(0x8000000000000000), threes, 0, true, 0, &p) ==
          IA64_HOSTFP_DOWN);
    const uint64_t wide = UINT64_C(0x3fffffff3fffffff);
    /* SDM Vol.1 5.3.3: the f0 addend selects multiply; an inexact
     * product needs no fused-add capability in either packed lane. */
    const uint64_t factors[] = {ones, wide, UINT64_C(0x3f8000003fffffff),
                                UINT64_C(0x3fffffff3f800000)};
    for (unsigned k = 0; k < sizeof(factors) / sizeof(*factors); ++k)
        for (unsigned operation = 0; operation < 3; ++operation)
            for (unsigned opts = 0; opts < 4; ++opts) {
                CHECK(ia64_hostfp_plan_packed(0, factors[k], factors[k], operation, true, opts,
                                              &p) == IA64_HOSTFP_DOWN);
                CHECK(p.kind == IA64_HOSTFP_MUL && p.a == 0);
                CHECK(p.b == (factors[k] ^ (operation == 2 ? UINT64_C(0x8000000080000000) : 0)) &&
                      p.c == factors[k]);
            }
    /* 24-bit products: fused needs FMA3 unless exact in 24 bits per lane. */
    CHECK(ia64_hostfp_plan_packed(ones, wide, wide, 0, false, 0, &p) == IA64_HOSTFP_NOFMA);
    CHECK(ia64_hostfp_plan_packed(ones, wide, wide, 0, false, IA64_HOSTFP_OPT_FMA3, &p) ==
              IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_FMA);
    CHECK(ia64_hostfp_plan_packed(ones, wide, wide, 0, false, IA64_HOSTFP_OPT_APPROX, &p) ==
              IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MULADD);
    CHECK(ia64_hostfp_plan_packed(ones, wide, ones, 0, false, 0, &p) == IA64_HOSTFP_DOWN &&
          p.kind == IA64_HOSTFP_MULADD);
    /* Results: either lane denormal; flags as scalar. */
    CHECK(ia64_hostfp_accept_packed(UINT64_C(0x3f8000003f800000), 0, 63, &flags) ==
              IA64_HOSTFP_DOWN &&
          !flags);
    CHECK(ia64_hostfp_accept_packed(UINT64_C(0x3f800000bf800000), IA64_HOSTFP_PE, 63, &flags) ==
              IA64_HOSTFP_DOWN &&
          flags == FP_I);
    CHECK(ia64_hostfp_accept_packed(UINT64_C(0x3f80000000000001), 0, 63, &flags) ==
          IA64_HOSTFP_RESULT);
    CHECK(ia64_hostfp_accept_packed(UINT64_C(0x807fffff3f800000), 0, 63, &flags) ==
          IA64_HOSTFP_RESULT);
    CHECK(ia64_hostfp_accept_packed(UINT64_C(0x3f8000003f800000), 16 | IA64_HOSTFP_PE, 63,
                                    &flags) == IA64_HOSTFP_RESULT);
    CHECK(ia64_hostfp_accept_packed(UINT64_C(0x3f8000003f800000), IA64_HOSTFP_PE, 31, &flags) ==
          IA64_HOSTFP_TRAP);
}

/* ---- execution against the soft-float ---- */

static uint64_t rnd(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

/* Edge values in register format: Table 5-2 classes at the host format
 * boundaries, with both signs. */
static struct ia64_fr edge(unsigned k, bool sign)
{
    static const struct {
        unsigned e;
        uint64_t s;
    } v[] = {
        {0, 0},
        {0xffff, IBIT},
        {0x0ff81, IBIT},
        {0x1007e, UINT64_C(0xffffff0000000000)},
        {0x0fc01, IBIT},
        {0x103fe, UINT64_C(0xfffffffffffff800)},
        {0x0fc00, IBIT},
        {0x103ff, IBIT},
        {0x0fc01, UINT64_C(0xfffffffffffff800)},
        {0x0fc02, IBIT},
        {0x0ff80, IBIT},
        {0x1007f, IBIT},
        {0xffff, UINT64_C(0xfffffffffffff800)},
        {0xfffe, UINT64_C(0xfffffffffffff800)},
        {0xffff, IBIT | UINT64_C(0x800)},
        {0x1ffff, IBIT},
        {0x1ffff, IBIT | IA64_FP_QUIET_BIT},
        {0x1ffff, IBIT | 1},
        {0x1ffff, 1},
        {0x0fc01, UINT64_C(1) << 62},
        {0x0ff81, UINT64_C(1) << 62},
        {0x1003e, 7},
        {0x00001, 0},
        {0, 1},
        {0x10000, UINT64_C(0xc000000000000000)},
        {0xfffe, UINT64_C(0xffffff0000000000)},
        {0x0fc34, IBIT},
        {0x103c9, IBIT},
        {0x0ff98, UINT64_C(0xc000000000000000)},
        {0x10067, UINT64_C(0xa000000000000000)},
    };
    unsigned i = k % (sizeof(v) / sizeof(*v));
    return fr(sign, v[i].e, v[i].s);
}
/* Random values of a class: 0 single, 1 double, 2 double near the range
 * ends, 3 any register format, 4 edges. */
static struct ia64_fr value(uint64_t *s, unsigned cls)
{
    uint64_t r = rnd(s);
    bool sign = r >> 63;
    switch (cls) {
    case 0:
        return fr(sign, 0xffff - 30 + (unsigned)(r % 61),
                  IBIT | (rnd(s) & UINT64_C(0x7fffff0000000000)));
    case 1:
        return fr(sign, 0xffff - 60 + (unsigned)(r % 121),
                  IBIT | (rnd(s) & UINT64_C(0x7ffffffffffff800)));
    case 2: {
        unsigned e =
            r & 2 ? 0x0fc01 + (unsigned)((r >> 8) % 60) : 0x103fe - (unsigned)((r >> 8) % 60);
        if (r & 4)
            e = r & 2 ? 0x0ff81 + (unsigned)((r >> 8) % 30) : 0x1007e - (unsigned)((r >> 8) % 30);
        return fr(sign, e, IBIT | (rnd(s) & UINT64_C(0x7ffffffffffff800)));
    }
    case 3:
        return fr(sign, (unsigned)(r >> 20) & 0x1ffff, rnd(s));
    default:
        return edge((unsigned)(r >> 8), sign);
    }
}

/* The state ia64_fp reads or writes for these instructions. */
static void sync(struct vcpu *to, const struct vcpu *from)
{
    memcpy(to->s.fr, from->s.fr, sizeof(to->s.fr));
    to->s.ar[AR_FPSR] = from->s.ar[AR_FPSR];
    to->s.psr = from->s.psr;
    to->s.pr = from->s.pr;
    to->s.cfm = from->s.cfm;
}
static bool same_state(const struct vcpu *a, const struct vcpu *b)
{
    for (unsigned i = 0; i < 128; ++i)
        if (!same(a->s.fr[i], b->s.fr[i]))
            return false;
    return a->s.ar[AR_FPSR] == b->s.ar[AR_FPSR] && a->s.psr == b->s.psr && a->s.pr == b->s.pr;
}
static bool same_result(struct ia64_result a, struct ia64_result b)
{
    return a.kind == b.kind && a.fault == b.fault && a.info == b.info;
}

static uint64_t downgraded(void)
{
    struct ia64_hostfp_stats st;
    ia64_hostfp_stats(0, &st);
    return st.candidate[IA64_HOSTFP_SCALAR][IA64_HOSTFP_DOWN] +
           st.candidate[IA64_HOSTFP_PARALLEL][IA64_HOSTFP_DOWN];
}

/* One instruction through ia64_hostfp on v and through ia64_fp on a copy. */
static void compare(struct vcpu *v, struct vcpu *ref, const Ia64Instruction *insn)
{
    sync(ref, v);
    struct ia64_result want = ia64_fp(ref, insn);
    struct ia64_result got = ia64_hostfp(v, insn);
    if (!same_result(got, want) || !same_state(v, ref)) {
        const IA64FloatingOperands *op = &insn->operands.floating;
        fprintf(
            stderr,
            "hostfp: opcode %u sf %u pc %u r %u,%u,%u fpsr %llx: kind %u/%u fault %u/%u info %llx/%llx\n",
            insn->opcode, op->status_field, op->precision, op->source1, op->source2, op->auxiliary1,
            (unsigned long long)ref->s.ar[AR_FPSR], got.kind, want.kind, got.fault, want.fault,
            (unsigned long long)got.info, (unsigned long long)want.info);
        fprintf(stderr, "  got %u:%05x:%016llx fpsr %llx, want %u:%05x:%016llx fpsr %llx\n",
                v->s.fr[6].sign, v->s.fr[6].exponent, (unsigned long long)v->s.fr[6].significand,
                (unsigned long long)v->s.ar[AR_FPSR], ref->s.fr[6].sign, ref->s.fr[6].exponent,
                (unsigned long long)ref->s.fr[6].significand,
                (unsigned long long)ref->s.ar[AR_FPSR]);
        CHECK(false);
    }
    ++checks;
}

static Ia64Instruction scalar(Ia64Opcode opcode, unsigned sf, unsigned precision)
{
    Ia64Instruction i = {
        .valid = true, .opcode = opcode, .unit = IA64_UNIT_F, .slot = 1, .address = 0x100000};
    i.operands.floating = (IA64FloatingOperands){.destination = 6,
                                                 .source1 = 7,
                                                 .source2 = 8,
                                                 .auxiliary1 = 9,
                                                 .precision = precision,
                                                 .status_field = sf};
    return i;
}

/* FPSR with all traps disabled (most), random enables, td and controls. */
static uint64_t fpsr(uint64_t *s, unsigned sf, bool typical)
{
    uint64_t r = rnd(s), traps = typical || (r & 3) ? 63 : (r >> 2) & 63;
    unsigned controls = typical ? 0x0c | (unsigned)((r >> 8) & 0x30) : (unsigned)(r >> 8) & 0x7f;
    if (!sf)
        controls &= 0x3f;
    if ((controls & 0x0c) == 0x04 && !(r & 0x100000))
        controls |= 0x08;
    uint64_t flags = (r >> 20) & 63;
    uint64_t value = traps | (uint64_t)(controls | flags << 7) << (6 + 13 * sf);
    /* Other status fields keep random contents. */
    for (unsigned o = 0; o < 4; ++o)
        if (o != sf)
            value |= (rnd(s) & 0x1fff & (o ? 0x1fff : 0x1fbf)) << (6 + 13 * o);
    return value;
}

static void test_scalar_execution(struct vcpu *v, struct vcpu *ref, unsigned options)
{
    static const Ia64Opcode ops[] = {IA64_OP_FMA,  IA64_OP_FMS,  IA64_OP_FNMA, IA64_OP_FADD,
                                     IA64_OP_FSUB, IA64_OP_FMPY, IA64_OP_FNORM};
    uint64_t seed = UINT64_C(0x5eed00f1a7c0ffee) ^ options;
    uint64_t before = downgraded();
    for (unsigned n = 0; n < 400000; ++n) {
        unsigned sf = n & 3, precision = (n >> 2) % 3;
        Ia64Opcode opcode = ops[(n >> 4) % 7];
        Ia64Instruction i = scalar(opcode, sf, precision);
        IA64FloatingOperands *op = &i.operands.floating;
        uint64_t r = rnd(&seed);
        /* FR 0 addends (fmpy semantics) and FR 1 multiplicands (fadd). */
        if ((r & 15) == 0)
            op->source1 = 0;
        if ((r & 15) == 1)
            op->auxiliary1 = 1;
        if ((r & 15) == 2)
            op->source2 = 1;
        if (opcode == IA64_OP_FNORM)
            op->source1 = 0;
        unsigned cls = (unsigned)(r >> 8) % 5;
        if (precision == 1 && cls == 1 && (r & 0x10000))
            cls = 0;
        v->s.ar[AR_FPSR] = fpsr(&seed, sf, (r >> 24) & 1);
        v->s.fr[7] = value(&seed, (r >> 32) & 1 ? cls : (unsigned)(r >> 40) % 5);
        v->s.fr[8] = value(&seed, cls);
        v->s.fr[9] = value(&seed, (r >> 33) & 1 ? cls : (unsigned)(r >> 44) % 5);
        if ((r >> 48) % 23 == 0)
            v->s.fr[7 + (unsigned)(r >> 52) % 3] = ia64_fp_nat();
        if ((r >> 56) % 13 == 0)
            v->s.fr[9] = fr((r >> 60) & 1, 0, 0);
        if ((r >> 56) % 13 == 1)
            v->s.fr[7] = fr((r >> 60) & 1, 0, 0);
        v->s.psr = (r >> 61) & 1 ? IA64_PSR_DFH : 0;
        if ((r >> 62) == 3 && !(n & 0xff))
            v->s.psr |= IA64_PSR_DFL;
        compare(v, ref, &i);
    }
    /* Typical operands must reach the host path. */
    CHECK(downgraded() - before > 50000);
}

/* SDM Vol.1 §5.3.3: fmpy is the IEEE multiply; fma with a register holding
 * +0.0 is not. -1 * +0 is -0 in fmpy, but -0 + +0 is +0 when rounding to
 * nearest and -0 toward -infinity. */
static void test_signed_zero(struct vcpu *v, struct vcpu *ref)
{
    for (unsigned rc = 0; rc < 4; ++rc)
        for (unsigned precision = 0; precision < 3; ++precision) {
            Ia64Instruction m = scalar(IA64_OP_FMPY, 0, precision),
                            f = scalar(IA64_OP_FMA, 0, precision);
            m.operands.floating.source1 = 7;
            m.operands.floating.source2 = 8;
            v->s.ar[AR_FPSR] = 63 | (uint64_t)(0x0c | rc << 4) << 6;
            v->s.fr[7] = fr(1, 0xffff, IBIT);
            v->s.fr[8] = zero;
            uint64_t before = downgraded();
            compare(v, ref, &m);
            CHECK(same(v->s.fr[6], fr(1, 0, 0)));
            CHECK(downgraded() == before + 1);
            v->s.fr[7] = zero;
            v->s.fr[8] = fr(1, 0xffff, IBIT);
            v->s.fr[9] = zero;
            compare(v, ref, &f);
            CHECK(same(v->s.fr[6], fr(rc == 1, 0, 0)));
            CHECK(downgraded() == before + 2);
        }
}

/* Intel SDM Vol.1 §4.9.1.5 and SDM Vol.1 §5.4.3 both detect tininess after
 * rounding with an unbounded exponent; a masked underflow needs inexact. */
static void test_tininess(struct vcpu *v, struct vcpu *ref)
{
    /* (1 - 2^-52)(2^-1022 + 2^-1074): rounds to the min normal, tiny only
     * toward zero/-infinity. (1 - 2^-53) 2^-1022: tiny, exact unbounded,
     * inexact bounded. 2^-1 2^-1022: an exact denormal (FTZ, wre and an
     * enabled underflow trap change it). */
    static const uint64_t pairs[][2] = {
        {UINT64_C(0x3fefffffffffffff) - 1, UINT64_C(0x0010000000000001)},
        {UINT64_C(0x3fefffffffffffff), UINT64_C(0x0010000000000000)},
        {UINT64_C(0x3fe0000000000000), UINT64_C(0x0010000000000000)},
        {UINT64_C(0x3ff0000000000001), UINT64_C(0x000fffffffffffff) + 1},
    };
    for (unsigned k = 0; k < 4; ++k)
        for (unsigned rc = 0; rc < 4; ++rc)
            for (unsigned controls = 0; controls < 4; ++controls)
                for (unsigned traps = 0; traps < 2; ++traps)
                    for (unsigned precision = 0; precision < 3; ++precision) {
                        Ia64Instruction m = scalar(IA64_OP_FMPY, 0, precision);
                        m.operands.floating.source1 = 7;
                        m.operands.floating.source2 = 8;
                        v->s.ar[AR_FPSR] =
                            (traps ? 63 & ~FP_U : 63) | (uint64_t)(controls | 8 | rc << 4) << 6;
                        v->s.fr[7] = ia64_fp_from_ieee(pairs[k][0], false);
                        v->s.fr[8] = ia64_fp_from_ieee(pairs[k][1], false);
                        compare(v, ref, &m);
                    }
    /* Fused (fnma: a - b c, one rounding): 2^-1021 - (1 - 2^-53) 2^-1022 =
     * (1 + 2^-53) 2^-1022 rounds to the min normal or above it, not tiny;
     * 2^-1021 - (1 + 2^-52)(1 + 2^-52) 2^-1022 is tiny after rounding. */
    static const uint64_t triples[][3] = {
        {UINT64_C(0x0020000000000000), UINT64_C(0x3fefffffffffffff), UINT64_C(0x0010000000000000)},
        {UINT64_C(0x0020000000000000), UINT64_C(0x3ff0000000000001), UINT64_C(0x0010000000000001)},
    };
    for (unsigned k = 0; k < 2; ++k)
        for (unsigned rc = 0; rc < 4; ++rc)
            for (unsigned controls = 0; controls < 4; ++controls)
                for (unsigned traps = 0; traps < 2; ++traps)
                    for (unsigned precision = 0; precision < 3; ++precision) {
                        Ia64Instruction m = scalar(IA64_OP_FNMA, 0, precision);
                        v->s.ar[AR_FPSR] =
                            (traps ? 63 & ~FP_U : 63) | (uint64_t)(controls | 8 | rc << 4) << 6;
                        for (unsigned f = 0; f < 3; ++f)
                            v->s.fr[7 + f] = ia64_fp_from_ieee(triples[k][f], false);
                        compare(v, ref, &m);
                    }
}

static void test_packed_execution(struct vcpu *v, struct vcpu *ref)
{
    static const Ia64Opcode ops[] = {IA64_OP_FPMA, IA64_OP_FPMS, IA64_OP_FPNMA};
    static const uint32_t lanes[] = {0,          0x80000000, 0x3f800000, 0x00800000, 0x7f7fffff,
                                     0x007fffff, 0x7f800000, 0x7fc00000, 0x7f800001, 0x00000001,
                                     0x3fffffff, 0x0d800000, 0x71800000, 0x40490fdb};
    uint64_t seed = UINT64_C(0x9a9a5a5a01234567) ^ ia64_hostfp_options();
    uint64_t before = downgraded();
    for (unsigned n = 0; n < 200000; ++n) {
        unsigned sf = n & 3;
        Ia64Instruction i = scalar(ops[(n >> 2) % 3], sf, 0);
        uint64_t r = rnd(&seed);
        if ((r & 7) == 0)
            i.operands.floating.source1 = 0;
        for (unsigned k = 0; k < 3; ++k) {
            uint64_t lo = rnd(&seed), hi = rnd(&seed);
            uint32_t l0 = (r >> (8 + k)) & 1 ? lanes[lo % 14] : (uint32_t)lo & 0xbfffffff;
            uint32_t l1 = (r >> (12 + k)) & 1 ? lanes[hi % 14] : (uint32_t)hi & 0xbfffffff;
            if ((r >> (16 + k)) & 1) {
                l0 = (l0 & 0x807fffff) | 0x3f000000 | ((uint32_t)lo & 0x00800000);
            }
            if ((r >> (20 + k)) & 1) {
                l1 = (l1 & 0x807fffff) | 0x3f000000 | ((uint32_t)hi & 0x00800000);
            }
            v->s.fr[7 + k] = fr(0, IA64_FP_INTEGER_EXP, (uint64_t)l1 << 32 | l0);
        }
        if ((r >> 40) % 29 == 0)
            v->s.fr[7 + (unsigned)(r >> 44) % 3] = ia64_fp_nat();
        if ((r >> 48) % 31 == 0)
            v->s.fr[8] = one;
        v->s.ar[AR_FPSR] = fpsr(&seed, sf, (r >> 24) & 1);
        v->s.psr = 0;
        compare(v, ref, &i);
    }
    CHECK(downgraded() - before > 20000);
}

/* Approximation remains available for explicit double precision. */
static void test_approx(struct vcpu *v, struct vcpu *ref)
{
    ia64_hostfp_configure(IA64_HOSTFP_CFG_APPROX | IA64_HOSTFP_CFG_EXPERIMENTAL);
    Ia64Instruction i = scalar(IA64_OP_FMA, 1, 2);
    v->s.ar[AR_FPSR] = 63 | (uint64_t)0x0e << 19; /* sf1: pc 11, wre */
    v->s.fr[7] = fr(0, 0xffff, IBIT);
    v->s.fr[8] = fr(0, 0xffff, IBIT | UINT64_C(1) << 11);
    v->s.fr[9] = fr(0, 0xffff - 53, IBIT);
    uint64_t before = downgraded();
    CHECK(ia64_hostfp(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(downgraded() == before + 1);
    /* (1 + 2^-52) 2^-53 + 1 is above the double midpoint 1 + 2^-53. */
    CHECK(same(v->s.fr[6], fr(0, 0xffff, IBIT | UINT64_C(1) << 11)));
    v->s.fr[9] = fr(0, 0x1ffff, IBIT | IA64_FP_QUIET_BIT | 5);
    compare(v, ref, &i);
    v->s.fr[9] = fr(0, 0x0fc01, 1);
    compare(v, ref, &i);
    ia64_hostfp_configure(0);
}

static void test_approx_extended(struct vcpu *v, struct vcpu *ref)
{
    for (unsigned nofma = 0; nofma < 2; ++nofma) {
        ia64_hostfp_configure(IA64_HOSTFP_CFG_APPROX | IA64_HOSTFP_CFG_EXPERIMENTAL |
                              (nofma ? IA64_HOSTFP_CFG_NOFMA3 : 0));
        for (unsigned sf = 0; sf < 4; ++sf)
            for (unsigned rc = 0; rc < 4; ++rc) {
                uint64_t fpsr = 63 | (uint64_t)(0x0e | rc << 4) << (6 + sf * 13);
                Ia64Instruction i = scalar(IA64_OP_FMPY, sf, 0);
                v->s.ar[AR_FPSR] = fpsr;
                /* (2^53 + 1) * 1 must retain the bit below double precision. */
                struct ia64_fr input = fr(0, 0xffff + 53, IBIT | UINT64_C(1) << 10);
                v->s.fr[7] = input;
                v->s.fr[8] = one;
                uint64_t before = downgraded();
                compare(v, ref, &i);
                CHECK(same(v->s.fr[6], input));
                CHECK(v->s.ar[AR_FPSR] == fpsr && downgraded() == before);

                /* An exact result with exact double operands can still use the host. */
                v->s.fr[7] = one;
                compare(v, ref, &i);
                CHECK(same(v->s.fr[6], one));
                CHECK(v->s.ar[AR_FPSR] == fpsr && downgraded() == before + 1);

                /* 1 - (1 + 2^-52) * (1 - 2^-52) = 2^-104 requires fusion. */
                i = scalar(IA64_OP_FNMA, sf, 0);
                v->s.fr[7] = one;
                v->s.fr[8] = ia64_fp_from_ieee(UINT64_C(0x3ff0000000000001), false);
                v->s.fr[9] = ia64_fp_from_ieee(UINT64_C(0x3feffffffffffffe), false);
                compare(v, ref, &i);
                CHECK(same(v->s.fr[6], fr(0, 0xffff - 104, IBIT)));
                CHECK(v->s.ar[AR_FPSR] == fpsr);

                /* The exact sum is 1 + 2^-53 + 2^-105: round once to 64 bits. */
                i = scalar(IA64_OP_FMA, sf, 0);
                v->s.fr[9] = fr(0, 0xffff - 53, IBIT);
                before = downgraded();
                compare(v, ref, &i);
                CHECK(same(v->s.fr[6], fr(0, 0xffff, IBIT | UINT64_C(1) << 10 | (rc == 2))));
                CHECK(v->s.ar[AR_FPSR] == (fpsr | (uint64_t)FP_I << (13 + sf * 13)));
                CHECK(downgraded() == before);
            }
    }
    ia64_hostfp_configure(0);
}

static void test_modes(void)
{
    /* Dispatch follows the CPU intersection, even if the current CPU has FMA. */
    for (unsigned features = 0; features <= HOST_SIMD_ALL; ++features) {
        simd_override = (int)features;
        CHECK(ia64_hostfp_fma3() == !!(features & HOST_SIMD_FMA));
        ia64_hostfp_configure(0);
        CHECK(!!(ia64_hostfp_options() & IA64_HOSTFP_OPT_FMA3) == !!(features & HOST_SIMD_FMA));
    }
    simd_override = -1;

    ia64_hostfp_configure(0);
    CHECK(!(ia64_hostfp_options() & (IA64_HOSTFP_OPT_EXPERIMENTAL | IA64_HOSTFP_OPT_APPROX)));
    ia64_hostfp_configure(IA64_HOSTFP_CFG_APPROX);
    CHECK(!(ia64_hostfp_options() & (IA64_HOSTFP_OPT_EXPERIMENTAL | IA64_HOSTFP_OPT_APPROX)));
    ia64_hostfp_configure(IA64_HOSTFP_CFG_EXPERIMENTAL);
    CHECK((ia64_hostfp_options() & (IA64_HOSTFP_OPT_EXPERIMENTAL | IA64_HOSTFP_OPT_APPROX)) ==
          IA64_HOSTFP_OPT_EXPERIMENTAL);
    ia64_hostfp_configure(IA64_HOSTFP_CFG_EXPERIMENTAL | IA64_HOSTFP_CFG_APPROX);
    CHECK((ia64_hostfp_options() & (IA64_HOSTFP_OPT_EXPERIMENTAL | IA64_HOSTFP_OPT_APPROX)) ==
          (IA64_HOSTFP_OPT_EXPERIMENTAL | IA64_HOSTFP_OPT_APPROX));
    ia64_hostfp_configure(IA64_HOSTFP_CFG_SOFT | IA64_HOSTFP_CFG_EXPERIMENTAL);
    CHECK(ia64_hostfp_options() & IA64_HOSTFP_OPT_SOFT);
    ia64_hostfp_configure(0);
}

unsigned test_hostfp(void)
{
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *v = vcpu_create(&cfg), *ref = vcpu_create(&cfg);
    CHECK(v && ref);
    ia64_hostfp_configure(0);
    test_modes();
    test_models();
    test_operands();
    test_kinds();
    test_accept();
    test_packed_plans();
    ia64_hostfp_stats_reset();
    test_signed_zero(v, ref);
    test_tininess(v, ref);
    /* With host FMA3 where present, and without (Sandy Bridge). */
    for (unsigned nofma = 0; nofma < 2; ++nofma) {
        ia64_hostfp_configure(nofma ? IA64_HOSTFP_CFG_NOFMA3 : 0);
        test_scalar_execution(v, ref, ia64_hostfp_options());
        test_packed_execution(v, ref);
    }
    ia64_hostfp_configure(0);
    test_approx(v, ref);
    test_approx_extended(v, ref);
    vcpu_destroy(v);
    vcpu_destroy(ref);
    printf("hostfp (FMA3 %s): %u checks passed\n", ia64_hostfp_fma3() ? "yes" : "no", checks);
    return checks;
}
