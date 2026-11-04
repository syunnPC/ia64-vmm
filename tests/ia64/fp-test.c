/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
#include "ia64/fp.h"
#include "ia64/hostfp.h"
#include "ia64vmm/guest_mem.h"
#include "build/tests/ia64/fp-compiler.h"

static unsigned checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static Ia64Instruction instruction(Ia64Opcode opcode)
{
    return (Ia64Instruction){.valid = true,
                             .opcode = opcode,
                             .unit = IA64_UNIT_F,
                             .slot = 1,
                             .address = 0x100000,
                             .operands.floating = {.destination = 6,
                                                   .source1 = 7,
                                                   .source2 = 8,
                                                   .auxiliary1 = 9,
                                                   .auxiliary2 = 10,
                                                   .precision = 2}};
}
static bool equal(struct ia64_fr a, struct ia64_fr b)
{
    return a.sign == b.sign && a.exponent == b.exponent && a.significand == b.significand;
}
static unsigned flags(struct vcpu *v, unsigned sf)
{
    return (v->s.ar[AR_FPSR] >> (13 + sf * 13)) & 63;
}
static void control(struct vcpu *v, unsigned sf, unsigned ctl, unsigned masks)
{
    v->s.ar[AR_FPSR] = masks | ((uint64_t)ctl << (6 + 13 * sf));
}
static struct ia64_fr power(int exponent, bool sign)
{
    return (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_BIAS + exponent, sign};
}

static void test_rounding(struct vcpu *v)
{
    /* 1 +/- half an ulp, odd/even ties, every destination precision and
     * status field; source precision deliberately exceeds the destination. */
    const unsigned widths[] = {24, 53, 64};
    for (unsigned sf = 0; sf < 4; ++sf)
        for (unsigned p = 0; p < 3; ++p)
            for (unsigned dynamic = 0; dynamic < (p == 2 ? 1u : 2u); ++dynamic)
                for (unsigned rc = 0; rc < 4; ++rc)
                    for (unsigned sign = 0; sign < 2; ++sign)
                        for (unsigned odd = 0; odd < 2; ++odd) {
                            unsigned bits = widths[p], ctlpc = p == 0 ? 0 : p == 1 ? 2 : 3;
                            Ia64Instruction i = instruction(IA64_OP_FMA);
                            i.operands.floating.status_field = sf;
                            i.operands.floating.precision = p == 2 || dynamic ? 0 : p + 1;
                            control(v, sf, (ctlpc << 2) | (rc << 4), 63);
                            v->s.fr[7] = power(-(int)bits, sign);
                            v->s.fr[8] = power(0, sign);
                            v->s.fr[8].significand |= (uint64_t)odd << (64 - bits);
                            v->s.fr[9] = power(0, false);
                            bool up = rc == 0 ? odd : rc == 1 ? sign : rc == 2 ? !sign : false;
                            struct ia64_fr expected = power(0, sign);
                            expected.significand += (uint64_t)(odd + up) << (64 - bits);
                            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
                            CHECK(equal(v->s.fr[6], expected));
                            CHECK(flags(v, sf) == FP_I);
                        }
    Ia64Instruction i = instruction(IA64_OP_FMA);
    i.operands.floating.precision = 1;
    control(v, 0, 12, 63);
    v->s.fr[7] = power(0, true);
    v->s.fr[8] = power(0, false);
    v->s.fr[8].significand += UINT64_C(1) << 39;
    v->s.fr[9] = power(0, false);
    v->s.fr[9].significand -= UINT64_C(1) << 39;
    /* Normalize 1-2^-24 without rounding before the multiply. */
    v->s.fr[9].significand <<= 1;
    --v->s.fr[9].exponent;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(equal(v->s.fr[6], power(-48, true)) && !flags(v, 0));

    for (unsigned precision = 0; precision < 3; ++precision)
        for (unsigned wre = 0; wre < 2; ++wre)
            for (unsigned ftz = 0; ftz < 2; ++ftz) {
                i = instruction(IA64_OP_FMPY);
                i.operands.floating.precision = precision;
                control(v, 0, 12 | (wre << 1) | ftz, 63);
                int emin = wre ? -65534 : precision == 1 ? -126 : precision == 2 ? -1022 : -16382;
                v->s.fr[7] = power(emin - 1, false);
                v->s.fr[8] = power(0, false);
                if (wre)
                    v->s.fr[7] = (struct ia64_fr){UINT64_C(1) << 62, 1, false};
                CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
                if (ftz)
                    CHECK(!v->s.fr[6].significand &&
                          flags(v, 0) == (unsigned)(FP_U | FP_I | (wre ? FP_D : 0)));
                else {
                    CHECK(v->s.fr[6].significand == (UINT64_C(1) << 62));
                    CHECK(v->s.fr[6].exponent ==
                          (!precision && !wre ? 0u : (unsigned)(IA64_FP_BIAS + emin)));
                    CHECK(flags(v, 0) == (wre ? FP_D : 0));
                }
            }
    /* The value of an extended denormal uses exponent 0xc001. */
    i = instruction(IA64_OP_FNORM);
    i.operands.floating.precision = 0;
    v->s.fr[8] = (struct ia64_fr){1, 0, false};
    control(v, 0, 12, 63);
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(equal(v->s.fr[6], v->s.fr[8]) && flags(v, 0) == FP_D);
}

static void test_specials(struct vcpu *v)
{
    const struct ia64_fr values[] = {
        {0, 0, false},
        {0, 0, true},
        {IA64_FP_INTEGER_BIT, IA64_FP_BIAS, false},
        {IA64_FP_INTEGER_BIT, IA64_FP_BIAS, true},
        {1, 0xfc01, false},
        {0, IA64_FP_INTEGER_EXP, false},
        {IA64_FP_INTEGER_BIT, IA64_FP_SPECIAL_EXP, false},
        {IA64_FP_INTEGER_BIT, IA64_FP_SPECIAL_EXP, true},
        {IA64_FP_INTEGER_BIT | IA64_FP_QUIET_BIT | 123, IA64_FP_SPECIAL_EXP, false},
        {IA64_FP_INTEGER_BIT | 456, IA64_FP_SPECIAL_EXP, false},
        {1, IA64_FP_SPECIAL_EXP, false},
        {0, IA64_FP_NAT_EXP, false}};
    Ia64Instruction i = instruction(IA64_OP_FMA);
    for (unsigned sf = 0; sf < 4; ++sf)
        for (unsigned a = 0; a < sizeof(values) / sizeof(*values); ++a)
            for (unsigned b = 0; b < sizeof(values) / sizeof(*values); ++b) {
                i.operands.floating.status_field = sf;
                v->s.fr[7] = values[a];
                v->s.fr[8] = values[b];
                v->s.fr[9] = power(0, false);
                control(v, sf, 12, 63);
                CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
                struct ia64_fr expected = v->s.fr[6];
                unsigned f = flags(v, sf);
                if (a == 11 || b == 11)
                    CHECK(ia64_fr_is_nat(expected) && !f);
                if (a == 10 || b == 10) {
                    if (a != 11 && b != 11)
                        CHECK(f == FP_V);
                }
                if (a == 8 && b < 8)
                    CHECK(equal(expected, values[8]) && !f);
                /* Every immediate fault must leave destination, flags and
                 * modified-FR bits unchanged, including aliased operands. */
                if (f & 7) {
                    control(v, sf, 12, 63 & ~f);
                    v->s.fr[6] = power(42, false);
                    v->s.psr = 0;
                    uint64_t fpsr = v->s.ar[AR_FPSR];
                    struct ia64_result r = ia64_execute(v, &i);
                    CHECK(r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_FP);
                    CHECK(equal(v->s.fr[6], power(42, false)) && v->s.ar[AR_FPSR] == fpsr &&
                          !v->s.psr);
                }
            }
    i = instruction(IA64_OP_FMA);
    control(v, 0, 12, 63);
    v->s.fr[7] = values[8];
    v->s.fr[8] = values[9];
    v->s.fr[9] = values[8];
    v->s.fr[9].significand += 789;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(equal(v->s.fr[6], v->s.fr[9]) && flags(v, 0) == FP_V);
    i.qp = 6;
    v->s.pr = 1;
    v->s.psr = IA64_PSR_DFL;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    i.qp = 0;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_FP_DISABLED);
    v->s.psr = IA64_PSR_DFH;
    i.operands.floating.source2 = 32;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_FP_DISABLED);
    v->s.psr = 0;
}

static void test_register_ops(struct vcpu *v)
{
    uint64_t seed = UINT64_C(0xe6a30bcaf00d1234);
    const Ia64Opcode set_ops[] = {IA64_OP_SETF_S, IA64_OP_SETF_D, IA64_OP_SETF_SIG,
                                  IA64_OP_SETF_EXP};
    const Ia64Opcode get_ops[] = {IA64_OP_GETF_S, IA64_OP_GETF_D, IA64_OP_GETF_SIG,
                                  IA64_OP_GETF_EXP};
    for (unsigned kind = 0; kind < 4; ++kind)
        for (unsigned n = 0; n < 128; ++n) {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            Ia64Instruction i = instruction(set_ops[kind]);
            i.operands.floating.source1 = 20;
            v->s.gr[20] = seed;
            v->s.nat[20] = n == 0;
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
            i.opcode = get_ops[kind];
            i.operands.floating.destination = 8;
            i.operands.floating.source1 = 6;
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
            CHECK(ia64_nat_read(&v->s, 8) == (n == 0));
            if (n)
                CHECK(v->s.gr[8] == (seed & (kind == 0   ? UINT32_MAX
                                             : kind == 3 ? UINT64_C(0x3ffff)
                                                         : UINT64_MAX)));
        }
    v->s.nat[20] = false;
    const struct {
        Ia64Opcode op;
        uint64_t expected;
    } bit_ops[] = {{IA64_OP_FAND, UINT64_C(0x020406080a0c0e00)},
                   {IA64_OP_FANDCM, 0},
                   {IA64_OP_FOR, UINT64_C(0x123456789abcdef0)},
                   {IA64_OP_FXOR, UINT64_C(0x1030507090b0d0f0)},
                   {IA64_OP_FSWAP, UINT64_C(0x9abcdef002040608)},
                   {IA64_OP_FSWAP_NL, UINT64_C(0x1abcdef002040608)},
                   {IA64_OP_FSWAP_NR, UINT64_C(0x9abcdef082040608)},
                   {IA64_OP_FMIX_LR, UINT64_C(0x020406089abcdef0)},
                   {IA64_OP_FMIX_L, UINT64_C(0x0204060812345678)},
                   {IA64_OP_FMIX_R, UINT64_C(0x0a0c0e009abcdef0)},
                   {IA64_OP_FSXT_L, UINT64_C(0x0000000012345678)},
                   {IA64_OP_FSXT_R, UINT64_C(0x000000009abcdef0)}};
    for (unsigned n = 0; n < sizeof(bit_ops) / sizeof(*bit_ops); ++n) {
        Ia64Instruction i = instruction(bit_ops[n].op);
        v->s.fr[7] = (struct ia64_fr){UINT64_C(0x020406080a0c0e00), 0x1ffff, true};
        v->s.fr[8] = (struct ia64_fr){UINT64_C(0x123456789abcdef0), 0x1ffff, true};
        control(v, 0, 12, 63);
        uint64_t expected = bit_ops[n].expected;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        CHECK(equal(v->s.fr[6], (struct ia64_fr){expected, IA64_FP_INTEGER_EXP, false}) &&
              !flags(v, 0));
        v->s.fr[8] = ia64_fp_nat();
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && ia64_fr_is_nat(v->s.fr[6]));
    }
    Ia64Instruction i = instruction(IA64_OP_XMA_H);
    v->s.fr[7] = (struct ia64_fr){UINT64_MAX, IA64_FP_INTEGER_EXP, false};
    v->s.fr[8] = v->s.fr[7];
    v->s.fr[9] = (struct ia64_fr){1, IA64_FP_INTEGER_EXP, false};
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && !v->s.fr[6].significand);
    i.opcode = IA64_OP_XMA_HU;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.fr[6].significand == 1);
    /* Rotating accesses use logical FR numbers for both inputs and writes. */
    v->s.cfm = UINT64_C(95) << 25;
    i = instruction(IA64_OP_FMERGE_S);
    i.operands.floating.source1 = 32;
    i.operands.floating.source2 = 33;
    i.operands.floating.destination = 34;
    v->s.fr[127] = power(7, true);
    v->s.fr[32] = power(9, false);
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && equal(v->s.fr[33], power(9, true)));
    CHECK(v->s.psr & IA64_PSR_MFH);
    v->s.cfm = 0;
}

static void test_approximation_and_status(struct vcpu *v)
{
    Ia64Instruction i = instruction(IA64_OP_FRCPA);
    control(v, 0, 12, 63);
    v->s.fr[7] = power(0, false);
    v->s.fr[8] = power(0, false);
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.fr[6].significand == UINT64_C(0xff80000000000000) &&
          v->s.fr[6].exponent == IA64_FP_BIAS - 1 && ia64_pr_read(&v->s, 10));
    v->s.fr[8] = (struct ia64_fr){0};
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && flags(v, 0) == FP_Z &&
          !ia64_pr_read(&v->s, 10));
    CHECK(v->s.fr[6].exponent == IA64_FP_SPECIAL_EXP &&
          v->s.fr[6].significand == IA64_FP_INTEGER_BIT);
    control(v, 0, 12, 63 & ~FP_Z);
    v->s.fr[6] = power(15, false);
    v->s.pr |= UINT64_C(1) << 10;
    uint64_t fpsr = v->s.ar[AR_FPSR];
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_FP);
    CHECK(equal(v->s.fr[6], power(15, false)) && v->s.ar[AR_FPSR] == fpsr &&
          ia64_pr_read(&v->s, 10));
    control(v, 0, 12, 63 & ~FP_D);
    v->s.fr[8] = (struct ia64_fr){1, 1, false};
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_FP && ia64_execute(v, &i).info == 8);
    CHECK(equal(v->s.fr[6], power(15, false)) && ia64_pr_read(&v->s, 10));
    i.opcode = IA64_OP_FRSQRTA;
    v->s.fr[8] = (struct ia64_fr){0, 0, true};
    control(v, 0, 12, 63);
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && equal(v->s.fr[6], v->s.fr[8]) &&
          !ia64_pr_read(&v->s, 10));
    v->s.fr[8] = power(1, true);
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && flags(v, 0) == FP_V);
    v->s.fr[8] = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_SPECIAL_EXP, false};
    control(v, 0, 12, 63);
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && equal(v->s.fr[6], v->s.fr[8]));
    i.opcode = IA64_OP_FPRCPA;
    v->s.fr[7] = (struct ia64_fr){UINT64_C(0x3f8000003f800000), IA64_FP_INTEGER_EXP, false};
    v->s.fr[8] = v->s.fr[7];
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE &&
          v->s.fr[6].significand == UINT64_C(0x3f7f80003f7f8000) && ia64_pr_read(&v->s, 10));
    i.opcode = IA64_OP_FPRSQRTA;
    v->s.fr[8].significand = UINT64_C(0x0000000080000000);
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE &&
          v->s.fr[6].significand == UINT64_C(0x7f800000ff800000) && !ia64_pr_read(&v->s, 10));
    v->s.fr[8].significand = UINT64_C(0x3f80000000000001);
    v->s.fr[6] = power(13, false);
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_FP && ia64_execute(v, &i).info == 128 &&
          equal(v->s.fr[6], power(13, false)));
    for (unsigned sf = 0; sf < 4; ++sf) {
        i = instruction(IA64_OP_FSETC);
        i.operands.floating.auxiliary1 = sf;
        i.operands.floating.source1 = 0x7f;
        i.operands.floating.source2 = sf ? 64 : 0;
        v->s.ar[AR_FPSR] = 63 | (UINT64_C(12) << 6) | (UINT64_C(63) << (13 + 13 * sf));
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        CHECK(((v->s.ar[AR_FPSR] >> (6 + 13 * sf)) & 127) == (unsigned)(12 | (sf ? 64 : 0)));
        i.opcode = IA64_OP_FCLRF;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && !flags(v, sf));
        i.opcode = IA64_OP_FCHKF;
        i.operands.floating.immediate = 64;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        v->s.ar[AR_FPSR] |= (uint64_t)FP_V << (13 + 13 * sf);
        v->s.ar[AR_FPSR] &= ~(uint64_t)FP_V;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_BRANCH);
    }
}

static uint64_t random_bits(uint64_t *seed)
{
    *seed ^= *seed << 13;
    *seed ^= *seed >> 7;
    *seed ^= *seed << 17;
    return *seed;
}
static void test_host_fma(struct vcpu *v)
{
    static const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    static const Ia64Opcode ops[] = {IA64_OP_FMA,  IA64_OP_FMS,  IA64_OP_FNMA,
                                     IA64_OP_FADD, IA64_OP_FSUB, IA64_OP_FMPY};
    uint64_t seed = UINT64_C(0xa3f1b9298674c123);
    for (unsigned rc = 0; rc < 4; ++rc)
        for (unsigned single = 0; single < 2; ++single)
            for (unsigned n = 0; n < 2000; ++n) {
                uint64_t bits[3];
                struct ia64_fr input[3];
                for (unsigned j = 0; j < 3; ++j) {
                    uint64_t r = random_bits(&seed);
                    bits[j] = single ? (uint32_t)r : r;
                    /* Finite operands including subnormals and near overflow. */
                    if (single && ((bits[j] >> 23) & 255) == 255)
                        bits[j] ^= UINT64_C(1) << 23;
                    if (!single && ((bits[j] >> 52) & 2047) == 2047)
                        bits[j] ^= UINT64_C(1) << 52;
                    input[j] = ia64_fp_from_ieee(bits[j], single);
                }
                unsigned operation = n % 6;
                Ia64Instruction i = instruction(ops[operation]);
                i.operands.floating.precision = single ? 1 : 2;
                control(v, 0, 12 | (rc << 4), 63);
                v->s.fr[7] = input[2];
                v->s.fr[8] = input[0];
                v->s.fr[9] = input[1];
                if (operation >= 3) {
                    v->s.fr[7] = input[0];
                    v->s.fr[8] = input[1];
                }
                CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
                CHECK(fesetround(modes[rc]) == 0);
                feclearexcept(FE_ALL_EXCEPT);
                uint64_t expected;
                if (single) {
                    float a, b, c;
                    uint32_t aa = bits[0], bb = bits[1], cc = bits[2], res;
                    memcpy(&a, &aa, 4);
                    memcpy(&b, &bb, 4);
                    memcpy(&c, &cc, 4);
                    float result = operation == 0   ? fmaf(a, b, c)
                                   : operation == 1 ? fmaf(a, b, -c)
                                   : operation == 2 ? fmaf(-a, b, c)
                                   : operation == 3 ? a + b
                                   : operation == 4 ? a - b
                                                    : a * b;
                    memcpy(&res, &result, 4);
                    expected = res;
                } else {
                    double a, b, c;
                    memcpy(&a, bits, 8);
                    memcpy(&b, bits + 1, 8);
                    memcpy(&c, bits + 2, 8);
                    double result = operation == 0   ? fma(a, b, c)
                                    : operation == 1 ? fma(a, b, -c)
                                    : operation == 2 ? fma(-a, b, c)
                                    : operation == 3 ? a + b
                                    : operation == 4 ? a - b
                                                     : a * b;
                    memcpy(&expected, &result, 8);
                }
                unsigned host = fetestexcept(FE_INEXACT | FE_UNDERFLOW | FE_OVERFLOW);
                CHECK(ia64_fp_to_ieee(v->s.fr[6], single) == expected);
                unsigned want = (host & FE_INEXACT ? FP_I : 0) | (host & FE_OVERFLOW ? FP_O : 0) |
                                (host & FE_UNDERFLOW ? FP_U : 0);
                CHECK((flags(v, 0) & (FP_O | FP_U | FP_I)) == want);
            }
    CHECK(fesetround(FE_TONEAREST) == 0);
}

static void test_memory(struct vcpu *v)
{
    uint64_t data[2] = {UINT64_C(0x0123456789abcdef), UINT64_C(0xfffffffffffb2345)};
    CHECK(gm_write(0x10ff00, data, 16));
    Ia64Instruction i = instruction(IA64_OP_LDF_FILL);
    i.operands.memory.base = 20;
    v->s.gr[20] = 0x10ff00;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.fr[6].significand == data[0] && v->s.fr[6].exponent == (data[1] & 0x1ffff) &&
          v->s.fr[6].sign);
    i.opcode = IA64_OP_STF_SPILL;
    i.operands.memory.source = 6;
    v->s.gr[20] += 16;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    uint64_t read[2];
    CHECK(gm_read(v->s.gr[20], read, 16));
    CHECK(read[0] == data[0] && read[1] == (data[1] & 0x3ffff));
    i.opcode = IA64_OP_LDF_FILL;
    i.imm_base_update = true;
    i.operands.memory.immediate = 16;
    v->s.gr[20] = 0x10fff8;
    struct ia64_fr before = v->s.fr[6];
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_UNALIGNED);
    CHECK(v->s.gr[20] == 0x10fff8 && equal(before, v->s.fr[6]));
    v->s.gr[20] = 0x110000;
    CHECK(ia64_execute(v, &i).exit == VCPU_EXIT_BAD_ACCESS);
    CHECK(v->s.gr[20] == 0x110000 && equal(before, v->s.fr[6]));
    i.opcode = IA64_OP_LDFPD;
    i.operands.memory.source = 8;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
    i.operands.memory.source = 7;
    i.fp_load_speculative = true;
    v->s.nat[20] = true;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(ia64_fr_is_nat(v->s.fr[6]) && ia64_fr_is_nat(v->s.fr[7]) && v->s.nat[20]);
    v->s.nat[20] = false;
    i = instruction(IA64_OP_CHK_S);
    i.check_fp = true;
    i.operands.memory.source = 6;
    i.operands.memory.immediate = 64;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_BRANCH);
    v->s.psr = IA64_PSR_DFL;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_FP_DISABLED);
    v->s.psr = 0;

    /* Spill/fill and paired loads to UC pages are ordinary references
     * (SDM Vol.2 §4.4.9 lists only ldfe/stfe as possibly unsupported). */
    {
        const uint64_t uc = UINT64_C(1) << 63;
        const uint64_t pattern[2] = {UINT64_C(0x8000000000000001), UINT64_C(0x1003e)};
        CHECK(gm_write(0x10ff00, pattern, 16));
        Ia64Instruction u = instruction(IA64_OP_LDF_FILL);
        u.operands.memory.base = 20;
        v->s.gr[20] = uc | 0x10ff00;
        CHECK(ia64_execute(v, &u).kind == IA64_STEP_COMPLETE);
        CHECK(v->s.fr[6].significand == pattern[0] && v->s.fr[6].exponent == 0x1003e &&
              !v->s.fr[6].sign);
        u.opcode = IA64_OP_STF_SPILL;
        u.operands.memory.source = 6;
        v->s.gr[20] = uc | 0x10ff10;
        CHECK(ia64_execute(v, &u).kind == IA64_STEP_COMPLETE);
        CHECK(gm_read(0x10ff10, read, 16) && read[0] == pattern[0] && read[1] == pattern[1]);
        u = instruction(IA64_OP_LDFP8);
        u.operands.memory.base = 20;
        u.operands.memory.source = 7;
        v->s.gr[20] = uc | 0x10ff00;
        CHECK(ia64_execute(v, &u).kind == IA64_STEP_COMPLETE);
        CHECK(v->s.fr[6].significand == pattern[0] && v->s.fr[7].significand == pattern[1]);
        u = instruction(IA64_OP_STFE);
        u.operands.memory.base = 20;
        u.operands.memory.source = 6;
        CHECK(ia64_execute(v, &u).fault == IA64_FAULT_UNSUPPORTED_DATA);
    }

    /* Extended transfers ignore padding on load and preserve it on store. */
    data[0] = IA64_FP_INTEGER_BIT;
    data[1] = UINT64_C(0xa5b6c7d8e9fa3fff);
    CHECK(gm_write(0x10ff00, data, 16));
    i = instruction(IA64_OP_LDFE);
    i.operands.memory.base = 20;
    v->s.gr[20] = 0x10ff00;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(equal(v->s.fr[6], power(0, false)));
    v->s.fr[6] = power(1, true);
    i.opcode = IA64_OP_STFE;
    i.operands.memory.source = 6;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(gm_read(0x10ff00, read, 16));
    CHECK(read[0] == IA64_FP_INTEGER_BIT && read[1] == UINT64_C(0xa5b6c7d8e9fac000));
    i.opcode = IA64_OP_LDFE;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && equal(v->s.fr[6], power(1, true)));

    const struct {
        Ia64Opcode load, store;
        uint64_t bits;
        struct ia64_fr value;
    } formats[] = {
        {IA64_OP_LDFS, IA64_OP_STFS, UINT64_C(0x80000001), {UINT64_C(1) << 40, 0xff81, true}},
        {IA64_OP_LDFD,
         IA64_OP_STFD,
         UINT64_C(0x7ff8000000000123),
         {UINT64_C(0xc000000000091800), IA64_FP_SPECIAL_EXP, false}},
        {IA64_OP_LDF8,
         IA64_OP_STF8,
         UINT64_C(0xfedcba9876543210),
         {UINT64_C(0xfedcba9876543210), IA64_FP_INTEGER_EXP, false}}};
    for (unsigned n = 0; n < sizeof(formats) / sizeof(*formats); ++n) {
        data[0] = formats[n].bits;
        data[1] = UINT64_MAX;
        CHECK(gm_write(0x10ff00, data, 16));
        i = instruction(formats[n].load);
        i.operands.memory.base = 20;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE &&
              equal(v->s.fr[6], formats[n].value));
        i.opcode = formats[n].store;
        i.operands.memory.source = 6;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        CHECK(gm_read(0x10ff00, read, 16) && read[0] == data[0] && read[1] == data[1]);
    }
    const Ia64Opcode pairs[] = {IA64_OP_LDFPS, IA64_OP_LDFPD, IA64_OP_LDFP8};
    const uint64_t pair_data[][2] = {{UINT64_C(0xc00000003f800000), UINT64_MAX},
                                     {UINT64_C(0x3ff0000000000000), UINT64_C(0xc000000000000000)},
                                     {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)}};
    for (unsigned n = 0; n < 3; ++n) {
        CHECK(gm_write(0x10ff00, pair_data[n], 16));
        i = instruction(pairs[n]);
        i.operands.memory.base = 20;
        i.operands.memory.source = 7;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        if (n == 2) {
            CHECK(equal(v->s.fr[6], (struct ia64_fr){UINT64_C(0x0123456789abcdef),
                                                     IA64_FP_INTEGER_EXP, false}));
            CHECK(equal(v->s.fr[7], formats[2].value));
        } else {
            CHECK(equal(v->s.fr[6], power(0, false)));
            CHECK(equal(v->s.fr[7], power(1, true)));
        }
    }

    i = instruction(IA64_OP_LDFD);
    i.operands.memory.base = 20;
    i.fp_load_speculative = true;
    i.imm_base_update = true;
    i.operands.memory.immediate = 8;
    v->s.gr[20] = 0x10ff03;
    v->s.psr = 0;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && ia64_fr_is_nat(v->s.fr[6]));
    CHECK(v->s.gr[20] == 0x10ff0b && !v->s.nat[20]);
    v->s.gr[20] = 0x10ff03;
    v->s.psr = IA64_PSR_IC;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_UNALIGNED && v->s.gr[20] == 0x10ff03);
    v->s.psr |= IA64_PSR_IT;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_UNALIGNED && v->s.gr[20] == 0x10ff03);
    v->s.psr = IA64_PSR_IC | (UINT64_C(1) << 43);
    v->s.gr[20] = 0x110000;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && ia64_fr_is_nat(v->s.fr[6]));
    v->s.psr = IA64_PSR_IC;
    v->s.gr[20] = (UINT64_C(1) << (IA64_IMPL_PA_MSB + 1)) | 3;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && ia64_fr_is_nat(v->s.fr[6]));
    CHECK(v->s.gr[20] == ((UINT64_C(1) << (IA64_IMPL_PA_MSB + 1)) | 11));
    i.fp_load_speculative = false;
    CHECK(ia64_execute(v, &i).exit == VCPU_EXIT_BAD_ACCESS);
    CHECK(v->s.gr[20] == ((UINT64_C(1) << (IA64_IMPL_PA_MSB + 1)) | 11) &&
          ia64_fr_is_nat(v->s.fr[6]));
    i.fp_load_speculative = true;
    v->s.psr = 0;
    v->s.gr[20] = 0x120000;
    i.imm_base_update = false;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && ia64_fr_is_nat(v->s.fr[6]));
    i.fp_load_speculative = false;
    i.fp_load_advanced = true;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && equal(v->s.fr[6], (struct ia64_fr){0}));
    i.opcode = IA64_OP_LDF8;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE &&
          equal(v->s.fr[6], (struct ia64_fr){0, IA64_FP_INTEGER_EXP, false}));
    i = instruction(IA64_OP_CHK_A_CLR);
    i.check_fp = true;
    i.operands.memory.source = 6;
    i.operands.memory.immediate = 64;
    v->s.psr = IA64_PSR_DFL;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_BRANCH);
    v->s.psr = 0;
}

static void test_parallel(struct vcpu *v)
{
    const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    uint64_t seed = UINT64_C(0x12345678a3abcdef);
    for (unsigned rc = 0; rc < 4; ++rc)
        for (unsigned n = 0; n < 512; ++n) {
            uint32_t input[3][2];
            for (unsigned k = 0; k < 3; ++k)
                for (unsigned l = 0; l < 2; ++l) {
                    input[k][l] = random_bits(&seed);
                    if (((input[k][l] >> 23) & 255) == 255)
                        input[k][l] ^= 1u << 23;
                }
            Ia64Instruction i = instruction(IA64_OP_FPMA);
            control(v, 0, 12 | (rc << 4) | 2, 63); /* WRE must be ignored. */
            v->s.fr[7] = (struct ia64_fr){((uint64_t)input[2][1] << 32) | input[2][0],
                                          IA64_FP_INTEGER_EXP, false};
            v->s.fr[8] = (struct ia64_fr){((uint64_t)input[0][1] << 32) | input[0][0],
                                          IA64_FP_INTEGER_EXP, false};
            v->s.fr[9] = (struct ia64_fr){((uint64_t)input[1][1] << 32) | input[1][0],
                                          IA64_FP_INTEGER_EXP, false};
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
            uint64_t expected = 0;
            CHECK(fesetround(modes[rc]) == 0);
            for (unsigned l = 0; l < 2; ++l) {
                float a, b, c;
                uint32_t res;
                memcpy(&a, &input[0][l], 4);
                memcpy(&b, &input[1][l], 4);
                memcpy(&c, &input[2][l], 4);
                float value = fmaf(a, b, c);
                memcpy(&res, &value, 4);
                expected |= (uint64_t)res << (l * 32);
            }
            CHECK(equal(v->s.fr[6], (struct ia64_fr){expected, IA64_FP_INTEGER_EXP, false}));
        }
    CHECK(fesetround(FE_TONEAREST) == 0);
    Ia64Instruction i = instruction(IA64_OP_FPMA);
    control(v, 0, 12, 63 & ~FP_V);
    v->s.fr[7] = (struct ia64_fr){UINT64_C(0x0000000000000000), IA64_FP_INTEGER_EXP, false};
    v->s.fr[8] = (struct ia64_fr){UINT64_C(0x000000007f800000), IA64_FP_INTEGER_EXP, false};
    v->s.fr[9] = (struct ia64_fr){UINT64_C(0x7f80000000000000), IA64_FP_INTEGER_EXP, false};
    v->s.fr[6] = power(17, false);
    uint64_t fpsr = v->s.ar[AR_FPSR];
    struct ia64_result r = ia64_execute(v, &i);
    CHECK(r.fault == IA64_FAULT_FP && r.info == 0x11);
    CHECK(equal(v->s.fr[6], power(17, false)) && v->s.ar[AR_FPSR] == fpsr);
    i.opcode = IA64_OP_FPCVT;
    v->s.fr[7].significand = UINT64_C(0x4f00000000000001);
    r = ia64_execute(v, &i);
    CHECK(r.fault == IA64_FAULT_FP && r.info == FP_V);
    control(v, 0, 12, 63);
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.fr[6].significand == UINT64_C(0x8000000000000000) &&
          flags(v, 0) == (FP_D | FP_I | FP_V));
}

static void test_completion_trap(struct vcpu *v)
{
    for (unsigned sf = 0; sf < 4; ++sf)
        for (unsigned rc = 0; rc < 4; ++rc)
            for (unsigned kind = 0; kind < 3; ++kind) {
                Ia64Instruction i = instruction(kind == 2 ? IA64_OP_FADD : IA64_OP_FMPY);
                i.operands.floating.precision = 1;
                i.operands.floating.status_field = sf;
                unsigned flag = kind == 0 ? FP_O : kind == 1 ? FP_U : FP_I;
                /* Enabled underflow takes precedence over FTZ. */
                control(v, sf, 12 | (rc << 4) | 1, 63 & ~flag);
                v->s.fr[7] = power(kind == 0 ? 100 : kind == 1 ? -100 : 0, false);
                v->s.fr[8] = power(kind == 0 ? 100 : kind == 1 ? -100 : -25, false);
                struct ia64_fr expected = power(kind == 0 ? 200 : kind == 1 ? -200 : 0, false);
                if (kind == 2 && rc == 2)
                    expected.significand += UINT64_C(1) << 40;
                struct ia64_result r = ia64_execute(v, &i);
                CHECK(r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_FP_TRAP);
                CHECK(r.info == (1u | (flag << 8) | (kind == 2 && rc == 2 ? 1u << 14 : 0)));
                CHECK(equal(v->s.fr[6], expected) && flags(v, sf) == flag);
            }
    Ia64Instruction masked = instruction(IA64_OP_FMPY);
    masked.operands.floating.precision = 1;
    masked.operands.floating.status_field = 3;
    control(v, 3, 12 | 64, 0);
    v->s.fr[7] = v->s.fr[8] = power(100, false);
    CHECK(ia64_execute(v, &masked).kind == IA64_STEP_COMPLETE);
    CHECK(flags(v, 3) == (FP_O | FP_I) &&
          equal(v->s.fr[6], (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_SPECIAL_EXP, false}));
    /* Raw F1 fmpy.s f6=f7,f8 in slot 1 of MFI. */
    uint64_t raw = (UINT64_C(8) << 37) | (UINT64_C(1) << 36) | (UINT64_C(8) << 27) |
                   (UINT64_C(7) << 20) | (UINT64_C(6) << 6);
    uint64_t bundle[2] = {0xc | (raw << 46), raw >> 18};
    CHECK(gm_write(0x100000, bundle, 16));
    vcpu_reset(v, NULL);
    v->s.ip = 0x100000;
    v->s.slot = 1;
    control(v, 0, 12, 63 & ~FP_O);
    v->s.fr[7] = power(100, false);
    v->s.fr[8] = power(100, false);
    /* Floating-point Exception trap: the instruction retires, then the trap
     * is delivered with IIP/IPSR.ri at the next instruction and ISR.ei/IIPA
     * at the trapping one (SDM Vol.2 §5.1.1, vector 0x5d00). */
    uint64_t iva = 0x108000;
    v->s.cr[CR_IVA] = iva;
    v->s.psr |= IA64_PSR_IC | IA64_PSR_DA | IA64_PSR_DD | IA64_PSR_IA | IA64_PSR_ED;
    struct vcpu_exit e = engine_run(v, 1);
    CHECK(e.reason == VCPU_EXIT_BUDGET && v->fault.kind == IA64_FAULT_FP_TRAP);
    CHECK(v->fault.effect_committed && v->retired == 1 && v->fault.slot == 1);
    CHECK(v->s.ip == iva + 0x5d00 && v->s.slot == 0);
    CHECK(v->s.cr[CR_IIP] == 0x100000 && ((v->s.cr[CR_IPSR] >> 41) & 3) == 2);
    CHECK(!(v->s.cr[CR_IPSR] & (IA64_PSR_DA | IA64_PSR_DD | IA64_PSR_IA | IA64_PSR_ED)));
    CHECK(((v->s.cr[CR_ISR] >> 41) & 3) == 1 && v->s.cr[CR_IIPA] == 0x100000);
    CHECK((v->s.cr[CR_ISR] & 0xffff) == (1u | ((unsigned)FP_O << 8)));
    CHECK(equal(v->s.fr[6], power(200, false)) && flags(v, 0) == FP_O);
    CHECK(v->s.psr & IA64_PSR_MFL);
}

static void run_compiler_function(struct vcpu *v, unsigned offset)
{
    v->s.ip = 0x100000 + offset;
    v->s.slot = 0;
    for (unsigned steps = 0; steps < 512; ++steps) {
        uint64_t words[2];
        CHECK(gm_read(v->s.ip, words, 16));
        const IA64TemplateInfo *t = ia64_template_info(words[0] & 31);
        uint64_t raw = ia64_bundle_slot(words[0], words[1], v->s.slot);
        Ia64Instruction i = ia64_decode_insn(t->units[v->s.slot], raw, v->s.ip, v->s.slot);
        if (i.opcode == IA64_OP_BR_RET)
            return;
        struct vcpu_exit e = engine_run(v, 1);
        if (e.reason != VCPU_EXIT_BUDGET) {
            fprintf(stderr, "compiler sequence exit at %llx:%u: %s, fault %u info %llx\n",
                    (unsigned long long)e.ip, e.slot, e.detail ? e.detail : "?", v->fault.kind,
                    (unsigned long long)v->fault.info);
            vcpu_dump(v);
        }
        CHECK(e.reason == VCPU_EXIT_BUDGET);
    }
    CHECK(false);
}

static void test_compiler(struct vcpu *v)
{
    const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    CHECK(sizeof(fp_guest_code) < 0x10000);
    CHECK(gm_write(0x100000, fp_guest_code, sizeof(fp_guest_code)));
    const uint64_t edge[] = {0,
                             1,
                             2,
                             3,
                             UINT64_C(0x7fffffff),
                             UINT64_C(0x80000000),
                             UINT64_C(0xffffffff),
                             UINT64_C(0x7fffffffffffffff),
                             UINT64_C(0x8000000000000000),
                             UINT64_MAX};
    uint64_t seed = UINT64_C(0xa3facc0011223344);
    for (unsigned fn = 0; fn < sizeof(fp_guest_functions) / sizeof(*fp_guest_functions); ++fn) {
        const char *name = fp_guest_functions[fn].name;
        for (unsigned n = 0; n < 1024; ++n) {
            vcpu_reset(v, NULL);
            v->s.cfm = 2;
            unsigned rc = n / 256;
            /* GCC's inline double sqrt ends with fma.d.s1, whose rounding
             * remains nearest; division/float sqrt/conversions use .s0. */
            CHECK(fesetround(!strcmp(name, "sq") ? FE_TONEAREST : modes[rc]) == 0);
            v->s.ar[AR_FPSR] |= (uint64_t)rc << 10;
            uint64_t a = n < 100 ? edge[n / 10] : random_bits(&seed);
            uint64_t b = n < 100 ? edge[n % 10] : random_bits(&seed);
            uint64_t expected = 0;
            bool floating = false, single = false;
            if (!strcmp(name, "sd") || !strcmp(name, "sm") || !strcmp(name, "ud") ||
                !strcmp(name, "um") || !strcmp(name, "sd32") || !strcmp(name, "sm32") ||
                !strcmp(name, "ud32") || !strcmp(name, "um32")) {
                bool uns = name[0] == 'u', mod = name[1] == 'm', narrow = strlen(name) == 4;
                if (narrow) {
                    a = (uint32_t)a;
                    b = (uint32_t)b;
                }
                if (!b)
                    b = 1;
                if (uns)
                    expected = mod ? a % b : a / b;
                else {
                    int64_t sa = narrow ? (int32_t)a : (int64_t)a,
                            sb = narrow ? (int32_t)b : (int64_t)b;
                    if ((sa == INT64_MIN || (narrow && sa == INT32_MIN)) && sb == -1) {
                        b = 1;
                        sb = 1;
                    }
                    expected = (uint64_t)(mod ? sa % sb : sa / sb);
                }
                if (narrow)
                    expected = (uint32_t)expected;
                v->s.gr[32] = a;
                v->s.gr[33] = b;
            } else if (!strcmp(name, "dd") || !strcmp(name, "fd") || !strcmp(name, "sq") ||
                       !strcmp(name, "fsq")) {
                floating = true;
                single = !strcmp(name, "fd") || !strcmp(name, "fsq");
                bool sq = !strcmp(name, "sq") || !strcmp(name, "fsq");
                if (single) {
                    uint32_t aa = a, bb = b, res;
                    if (((aa >> 23) & 255) == 255)
                        aa ^= 1u << 23;
                    if (((bb >> 23) & 255) == 255)
                        bb ^= 1u << 23;
                    if (sq)
                        aa &= 0x7fffffff;
                    float fa, fb;
                    memcpy(&fa, &aa, 4);
                    memcpy(&fb, &bb, 4);
                    float result = sq ? sqrtf(fa) : fa / fb;
                    memcpy(&res, &result, 4);
                    expected = res;
                    v->s.fr[8] = ia64_fp_from_ieee(aa, true);
                    v->s.fr[9] = ia64_fp_from_ieee(bb, true);
                } else {
                    if (((a >> 52) & 2047) == 2047)
                        a ^= UINT64_C(1) << 52;
                    if (((b >> 52) & 2047) == 2047)
                        b ^= UINT64_C(1) << 52;
                    if (sq)
                        a &= UINT64_C(0x7fffffffffffffff);
                    double fa, fb;
                    memcpy(&fa, &a, 8);
                    memcpy(&fb, &b, 8);
                    double result = sq ? sqrt(fa) : fa / fb;
                    memcpy(&expected, &result, 8);
                    v->s.fr[8] = ia64_fp_from_ieee(a, false);
                    v->s.fr[9] = ia64_fp_from_ieee(b, false);
                }
            } else if (!strcmp(name, "si") || !strcmp(name, "ui") || !strcmp(name, "sif") ||
                       !strcmp(name, "uif")) {
                floating = true;
                single = strlen(name) == 3;
                v->s.gr[32] = a;
                if (single) {
                    float result = name[0] == 's' ? (float)(int64_t)a : (float)a;
                    uint32_t res;
                    memcpy(&res, &result, 4);
                    expected = res;
                } else {
                    double result = name[0] == 's' ? (double)(int64_t)a : (double)a;
                    memcpy(&expected, &result, 8);
                }
            } else if (!strcmp(name, "is") || !strcmp(name, "iu")) {
                double value = name[1] == 's' ? (double)((int64_t)a / 2) : (double)(a / 2);
                uint64_t bits;
                memcpy(&bits, &value, 8);
                v->s.fr[8] = ia64_fp_from_ieee(bits, false);
                expected = name[1] == 's' ? (uint64_t)(int64_t)value : (uint64_t)value;
            } else
                CHECK(false);
            run_compiler_function(v, fp_guest_functions[fn].offset);
            uint64_t actual =
                floating ? ia64_fp_to_ieee(v->s.fr[8], single) : ia64_gr_read(&v->s, 8);
            if (strlen(name) == 4 && !floating)
                actual = (uint32_t)actual;
            if (actual != expected)
                fprintf(stderr, "GCC %s case %u: a=%llx b=%llx got %llx expected %llx\n", name, n,
                        (unsigned long long)a, (unsigned long long)b, (unsigned long long)actual,
                        (unsigned long long)expected);
            CHECK(actual == expected);
        }
        printf("GCC %s: 1024 edge/seeded inputs passed\n", name);
    }
    CHECK(fesetround(FE_TONEAREST) == 0);
}

/* GCC implements integer division with extended-precision FP refinements. */
static void test_compiler_integer_approx(struct vcpu *v)
{
    const uint64_t numerators[] = {
        UINT64_C(0x020000004000c601),
        UINT64_C(0x020000004000c600),
        UINT64_C(0x020000004000c602),
        0,
        1,
        4,
        5,
        6,
        (UINT64_C(1) << 53) - 1,
        UINT64_C(1) << 53,
        (UINT64_C(1) << 53) + 1,
        (UINT64_C(1) << 57) - 1,
        UINT64_C(1) << 57,
        (UINT64_C(1) << 57) + 1,
        (UINT64_C(1) << 63) - 1,
        UINT64_C(1) << 63,
        UINT64_MAX - 1,
        UINT64_MAX,
    };
    const uint64_t divisors[] = {5, 1, 3, 7, UINT64_C(1) << 32, UINT64_MAX};
    for (unsigned nofma = 0; nofma < 2; ++nofma) {
        ia64_hostfp_configure(IA64_HOSTFP_CFG_EXPERIMENTAL | IA64_HOSTFP_CFG_APPROX |
                              (nofma ? IA64_HOSTFP_CFG_NOFMA3 : 0));
        for (unsigned fn = 0; fn < sizeof(fp_guest_functions) / sizeof(*fp_guest_functions); ++fn) {
            const char *name = fp_guest_functions[fn].name;
            if (strcmp(name, "ud") && strcmp(name, "um"))
                continue;
            for (unsigned n = 0; n < sizeof(numerators) / sizeof(*numerators); ++n)
                for (unsigned d = 0; d < sizeof(divisors) / sizeof(*divisors); ++d) {
                    uint64_t a = numerators[n], b = divisors[d];
                    uint64_t expected = name[1] == 'm' ? a % b : a / b;
                    vcpu_reset(v, NULL);
                    v->s.cfm = 2;
                    v->s.gr[32] = a;
                    v->s.gr[33] = b;
                    run_compiler_function(v, fp_guest_functions[fn].offset);
                    uint64_t actual = ia64_gr_read(&v->s, 8);
                    if (actual != expected)
                        fprintf(
                            stderr,
                            "GCC approximate %s nofma=%u: a=%llx b=%llx got %llx expected %llx\n",
                            name, nofma, (unsigned long long)a, (unsigned long long)b,
                            (unsigned long long)actual, (unsigned long long)expected);
                    CHECK(actual == expected);
                }
        }
    }
    ia64_hostfp_configure(0);
}

unsigned test_fp(void)
{
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    test_rounding(v);
    test_specials(v);
    test_register_ops(v);
    test_approximation_and_status(v);
    test_host_fma(v);
    test_memory(v);
    test_parallel(v);
    test_completion_trap(v);
    test_compiler(v);
    test_compiler_integer_approx(v);
    vcpu_destroy(v);
    printf("FP: %u checks passed\n", checks);
    return checks;
}
