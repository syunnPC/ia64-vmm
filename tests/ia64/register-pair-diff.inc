/* SPDX-License-Identifier: GPL-2.0-or-later */
static uint64_t pair_diff_random(uint64_t *state)
{
    *state ^= *state << 13; *state ^= *state >> 7; *state ^= *state << 17;
    return *state;
}
static void test_register_pair_differential(void)
{
    CHECK(baseline_ia64_integer != NULL && baseline_ia64_simd != NULL);
    struct vcpu *actual = new_vcpu(), *expected = new_vcpu();
    uint64_t random = UINT64_C(0xafe1387920cb456d);
    unsigned instructions = 0;
    for (unsigned opcode = 0; opcode < IA64_OP_COUNT; ++opcode) {
        unsigned family = ia64_opcode_info(opcode)->family;
        if (family != IA64_FAMILY_INTEGER && family != IA64_FAMILY_SIMD) continue;
        for (unsigned k = 0; k < 256; ++k) {
            unsigned sor = k & 1 ? 16 : k & 2 ? 96 : 0;
            unsigned rotation = sor ? (unsigned)(pair_diff_random(&random) % sor) : 0;
            actual->s.cfm = 96 | (UINT64_C(96) << 7) | ((uint64_t)(sor / 8) << 14) | ((uint64_t)rotation << 18);
            actual->s.rse_bof = (unsigned)(pair_diff_random(&random) % 96);
            actual->s.pr = pair_diff_random(&random) | 1;
            for (unsigned reg = 0; reg < 128; ++reg) {
                actual->s.gr[reg] = pair_diff_random(&random);
                actual->s.nat[reg] = (pair_diff_random(&random) & 1) != 0;
            }
            actual->s.gr[0] = UINT64_MAX; actual->s.nat[0] = true;
            expected->s = actual->s;
            Ia64Instruction input = instruction(opcode);
            unsigned dst = (unsigned)(pair_diff_random(&random) & 127);
            input.operands.integer.destination = dst;
            input.operands.integer.source1 = k & 4 ? dst : (unsigned)(pair_diff_random(&random) & 127);
            input.operands.integer.source2 = k & 8 ? dst : k & 16 ? 0 : (unsigned)(pair_diff_random(&random) & 127);
            input.operands.integer.immediate = k & 3;
            input.pred_update = k & 3;
            if (opcode == IA64_OP_DEP || opcode == IA64_OP_DEP_IMM ||
                opcode == IA64_OP_DEPZ || opcode == IA64_OP_DEPZ_IMM ||
                opcode == IA64_OP_EXTR || opcode == IA64_OP_EXTRU)
                input.operands.integer.immediate = (k & 63) | ((uint64_t)(1 + (k % 64)) << 6);
            struct ia64_result a = family == IA64_FAMILY_INTEGER ? ia64_integer(actual, &input) : ia64_simd(actual, &input);
            struct ia64_result b = family == IA64_FAMILY_INTEGER ? baseline_ia64_integer(expected, &input) : baseline_ia64_simd(expected, &input);
            CHECK(a.kind == b.kind && a.fault == b.fault && a.exit == b.exit && a.info == b.info);
            CHECK(memcmp(&actual->s, &expected->s, sizeof(actual->s)) == 0);
            ++instructions;
        }
    }
    printf("register-pair differential: %u instructions, %u checks passed\n", instructions, checks);
    vcpu_destroy(expected); vcpu_destroy(actual);
}
