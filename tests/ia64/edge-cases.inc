/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/profile.h"
#include "ia64/fp.h"

static void test_edge_alloc(void)
{
    struct vcpu *v = new_vcpu();
    Ia64Instruction i = instruction(IA64_OP_ALLOC);
    i.qp = 5;
    for (unsigned pr = 0; pr < 2; ++pr) {
        ia64_pr_write(&v->s, 5, pr);
        struct ia64_state saved = v->s;
        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
        CHECK(!memcmp(&saved, &v->s, sizeof(saved)));
    }
    vcpu_destroy(v);
}
static void test_edge_profile(void)
{
    struct vcpu *v = new_vcpu();
    const Ia64Opcode ops[] = {IA64_OP_CLZ, IA64_OP_MPY4, IA64_OP_MPYSHL4};
    for (unsigned k = 0; k < 3; ++k) {
        Ia64Instruction i = instruction(ops[k]);
        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
        i.qp = 5;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    }
    for (unsigned bit = 32; bit < 64; ++bit) {
        Ia64Instruction i = instruction(IA64_OP_TF_NZ);
        i.operands.integer.immediate = bit;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        CHECK(ia64_pr_read(&v->s, 6) == ((ia64_cpuid(4) >> bit) & 1));
    }
    vcpu_destroy(v);
}
static void test_edge_coverage(void)
{
    struct vcpu *v = new_vcpu();
    const Ia64Opcode ops[] = {IA64_OP_MUX, IA64_OP_SUM, IA64_OP_MPYSH, IA64_OP_MPYUH};
    for (unsigned k = 0; k < 4; ++k) {
        CHECK(!ia64_opcode_supported(ops[k]));
        Ia64Instruction i = instruction(ops[k]);
        CHECK(ia64_execute(v, &i).exit == VCPU_EXIT_UNIMPLEMENTED);
    }
    vcpu_destroy(v);
}
static void test_edge_psr(void)
{
    /* c4.inc supersedes these guards: disabled breakpoint pairs do not fault,
     * and SS is delivered after the instruction retires. */
    const unsigned bits[] = {40, 26, 25, 24, 39};
    uint64_t nop = UINT64_C(1) << 27;
    bundle_write(ENTRY, 0, nop, nop, nop);
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(bits); ++k) {
        struct vcpu *v = new_vcpu();
        v->s.psr = IA64_PSR_IC | (UINT64_C(1) << bits[k]);
        v->s.cr[CR_IVA] = ENTRY;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->retired == 1);
        CHECK(v->s.ip == (bits[k] == 40 ? ENTRY + 0x6000 : ENTRY));
        vcpu_destroy(v);
    }
}
static void test_edge_parallel(void)
{
    struct vcpu *v = new_vcpu();
    Ia64Instruction i = ia64_decode_insn(IA64_UNIT_F, UINT64_C(0x13048800180), ENTRY, 1);
    const Ia64Opcode ops[] = {IA64_OP_FPMA, IA64_OP_FPMS, IA64_OP_FPNMA};
    CHECK(i.opcode == IA64_OP_FPMA);
    for (unsigned k = 0; k < 3; ++k) {
        i.opcode = ops[k]; v->s.ar[AR_FPSR] = 0x327;
        v->s.fr[8] = (struct ia64_fr){UINT64_C(0x7f0000000d800000), IA64_FP_INTEGER_EXP, false};
        v->s.fr[9] = (struct ia64_fr){UINT64_C(0x408000000d800000), IA64_FP_INTEGER_EXP, false};
        struct ia64_result r = ia64_execute(v, &i);
        CHECK(r.fault == IA64_FAULT_FP_TRAP && r.info == 0x901);
        CHECK(v->s.fr[6].significand == (UINT64_C(0x5b800000) |
            (k == 2 ? UINT64_C(0x8000000080000000) : 0)));
        CHECK(v->s.ar[AR_FPSR] == 0x30327);
    }
    for (int a = -126; a <= 127; a += 8)
        for (int b = -126; b <= 127; b += 17) {
            uint32_t left = (a + 127u) << 23, right = (b + 127u) << 23;
            v->s.fr[8].significand = left | ((uint64_t)left << 32);
            v->s.fr[9].significand = right | ((uint64_t)right << 32);
            v->s.ar[AR_FPSR] = 0x327; i.opcode = IA64_OP_FPMA;
            struct ia64_result r = ia64_execute(v, &i);
            bool trap = a + b < -126 || a + b > 127;
            CHECK(trap ? r.fault == IA64_FAULT_FP_TRAP : r.kind == IA64_STEP_COMPLETE);
            unsigned exponent = (a + b + 127u) & 255;
            uint32_t expected = exponent << 23;
            CHECK(v->s.fr[6].significand == (expected | ((uint64_t)expected << 32)));
        }
    vcpu_destroy(v);
}
static void test_edge_fp_spec(void)
{
    struct vcpu *v = new_vcpu();
    /* Rev.2.3 Vol.1 Fig.5-12 explicitly uses FLAGS.i |= tmp_i on enabled
     * underflow; an exact tiny result sets U without I. */
    struct ia64_fp_raw raw = {.exp = IA64_FP_BIAS - 127, .mant = IA64_FP_INTEGER_BIT};
    struct ia64_fr out; unsigned flags = 0; bool fpa = false;
    CHECK(ia64_fp_round(&raw, 0, 63 & ~FP_U, 1, &out, &flags, &fpa));
    CHECK(flags == FP_U);
    raw.tail = true; flags = 0;
    CHECK(ia64_fp_round(&raw, 0, 63 & ~FP_U, 1, &out, &flags, &fpa));
    CHECK(flags == (FP_U | FP_I));
    /* Vol.3 fms/fnma take the propagated-NaN default before sign negation. */
    Ia64Instruction i = instruction(IA64_OP_FMS);
    i.operands.floating = (IA64FloatingOperands){.destination = 6, .source1 = 8,
        .source2 = 9, .auxiliary1 = 10, .precision = 1};
    const struct ia64_fr qnan = {IA64_FP_INTEGER_BIT | IA64_FP_QUIET_BIT | 123, IA64_FP_SPECIAL_EXP, true};
    for (unsigned op = 0; op < 2; ++op) for (unsigned source = 8; source <= 10; ++source) {
        i.opcode = op ? IA64_OP_FNMA : IA64_OP_FMS;
        for (unsigned n = 8; n <= 10; ++n)
            v->s.fr[n] = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_BIAS, false};
        v->s.fr[source] = qnan;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        CHECK(v->s.fr[6].sign == qnan.sign && v->s.fr[6].significand == qnan.significand);
    }
    /* Vol.3 fcvt.fx rounds before constructing the integer. A negative
     * fraction rounded to zero is representable in the unsigned format. */
    i.opcode = IA64_OP_FCVT_FXU; i.operands.floating.immediate = 2;
    v->s.fr[8] = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_BIAS - 2, true};
    v->s.ar[AR_FPSR] = 0x33f;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && !v->s.fr[6].significand);
    CHECK(((v->s.ar[AR_FPSR] >> 13) & 63) == FP_I);
    vcpu_destroy(v);
}
static void test_edge_swa(void)
{
    struct vcpu *v = new_vcpu();
    Ia64Instruction i = instruction(IA64_OP_FRCPA);
    i.unit = IA64_UNIT_F;
    i.operands.floating = (IA64FloatingOperands){.destination = 6, .source1 = 8,
        .source2 = 9, .auxiliary2 = 7};
    v->s.fr[8] = (struct ia64_fr){IA64_FP_INTEGER_BIT, 1, false};
    v->s.fr[9] = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_BIAS, false};
    struct ia64_state saved = v->s;
    struct ia64_result r = ia64_execute(v, &i);
    CHECK(r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_FP && r.info == 8);
    CHECK(!memcmp(&saved, &v->s, sizeof(saved)));
    /* Real delivery through each selected engine preserves the faulting IP
     * and predicate/FR/FPSR effects, then executes one handler instruction. */
    uint64_t raw = (UINT64_C(1) << 33) | (UINT64_C(7) << 27) |
                   (UINT64_C(9) << 20) | (UINT64_C(8) << 13) | (UINT64_C(6) << 6);
    uint64_t nop = UINT64_C(1) << 27;
    bundle_write(ENTRY, 0x0c, nop, raw, nop);
    bundle_write(ENTRY + 0x5c00, 0, nop, nop, nop);
    v->s.ip = ENTRY; v->s.slot = 1; v->s.psr = IA64_PSR_IC; v->s.cr[CR_IVA] = ENTRY;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
    CHECK(v->s.ip == ENTRY + 0x5c00 && v->s.slot == 1 && v->retired == 1);
    CHECK(v->s.cr[CR_IIP] == ENTRY && ((v->s.cr[CR_IPSR] >> 41) & 3) == 1);
    CHECK(v->s.cr[CR_ISR] == (UINT64_C(1) << 41 | 8));
    CHECK(v->s.ar[AR_FPSR] == saved.ar[AR_FPSR]);
    CHECK(!memcmp(v->s.fr, saved.fr, sizeof(saved.fr)) && v->s.pr == saved.pr);
    /* Packed assists identify the lane in ISR.code. */
    for (unsigned lane = 0; lane < 2; ++lane) {
        i.opcode = IA64_OP_FPRCPA;
        v->s.fr[8] = (struct ia64_fr){UINT64_C(0x3f8000003f800000), IA64_FP_INTEGER_EXP, false};
        v->s.fr[9] = v->s.fr[8];
        v->s.fr[8].significand &= ~(UINT64_C(0xffffffff) << (32 * lane));
        v->s.fr[8].significand |= UINT64_C(1) << (32 * lane);
        r = ia64_execute(v, &i);
        CHECK(r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_FP && r.info == (lane ? 8u : 128u));
    }
    /* A large packed divisor can make refinement unsafe in single range.
     * Request the architecturally permitted software assist for that lane. */
    v->s.fr[8] = (struct ia64_fr){UINT64_C(0x3f8000003f800000), IA64_FP_INTEGER_EXP, false};
    v->s.fr[9] = (struct ia64_fr){UINT64_C(0x7e8000003f800000), IA64_FP_INTEGER_EXP, false};
    saved = v->s; r = ia64_execute(v, &i);
    CHECK(r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_FP && r.info == 8);
    CHECK(!memcmp(&saved, &v->s, sizeof(saved)));
    vcpu_destroy(v);
}
static void test_edge_trap(void)
{
    struct vcpu *v = new_vcpu();
    /* An enabled overflow commits once even when the destination aliases a
     * multiplicand. Replaying the helper would multiply by four twice. */
    uint64_t nop = UINT64_C(1) << 27;
    uint64_t raw = (UINT64_C(8) << 37) | (UINT64_C(1) << 36) |
        (UINT64_C(9) << 27) | (UINT64_C(6) << 20) | (UINT64_C(6) << 6);
    bundle_write(ENTRY, 0x0c, nop, raw, nop);
    v->s.fr[6] = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_BIAS + 127, false};
    v->s.fr[9] = (struct ia64_fr){IA64_FP_INTEGER_BIT, IA64_FP_BIAS + 2, false};
    v->s.ar[AR_FPSR] = 0x327; v->s.ip = ENTRY; v->s.slot = 1;
    v->s.psr = IA64_PSR_IC; v->s.cr[CR_IVA] = ENTRY;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
    CHECK(v->retired == 1 && v->s.ip == ENTRY + 0x5d00 && !v->s.slot);
    CHECK(v->s.fr[6].exponent == IA64_FP_BIAS + 129);
    CHECK(v->s.cr[CR_IIP] == ENTRY && ((v->s.cr[CR_IPSR] >> 41) & 3) == 2);
    CHECK(v->s.cr[CR_ISR] == (UINT64_C(1) << 41 | 0x801));
    CHECK(v->fault.kind == IA64_FAULT_FP_TRAP && v->fault.effect_committed);
    vcpu_destroy(v);
}
static void test_edge_cases(const char *only)
{
    if (!only || !strcmp(only, "alloc")) test_edge_alloc();
    if (!only || !strcmp(only, "profile")) test_edge_profile();
    if (!only || !strcmp(only, "coverage")) test_edge_coverage();
    if (!only || !strcmp(only, "psr")) test_edge_psr();
    if (!only || !strcmp(only, "parallel")) test_edge_parallel();
    if (!only || !strcmp(only, "swa")) test_edge_swa();
    if (!only || !strcmp(only, "fp-spec")) test_edge_fp_spec();
    if (!only || !strcmp(only, "trap")) test_edge_trap();
}
