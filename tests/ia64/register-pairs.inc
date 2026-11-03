/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_register_pairs(void)
{
    struct vcpu *v = new_vcpu();
    const unsigned rotations[] = {0, 8, 16, 48, 96};
    for (unsigned i = 0; i < 128; ++i) {
        v->s.gr[i] = UINT64_C(0x123456789abcdef0) ^ (UINT64_C(0x9e3779b97f4a7c15) * i);
        v->s.nat[i] = (i & 1) != 0;
    }
    v->s.gr[0] = UINT64_MAX; v->s.nat[0] = true;
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(rotations); ++k)
        for (unsigned r = 0; r < 3; ++r)
            for (unsigned b = 0; b < 3; ++b) {
                unsigned sor = rotations[k], rrb = r == 0 ? 0 : r == 1 ? 1 : sor ? sor - 1 : 0;
                v->s.cfm = 96 | (UINT64_C(96) << 7) | ((uint64_t)(sor / 8) << 14) | ((uint64_t)rrb << 18);
                v->s.rse_bof = b == 0 ? 0 : b == 1 ? 1 : 95;
                for (unsigned reg = 0; reg < 128; ++reg) {
                    struct ia64_gr_value actual = ia64_gr_read_value(&v->s, reg);
                    CHECK(actual.value == ia64_gr_read(&v->s, reg));
                    CHECK(actual.nat == ia64_nat_read(&v->s, reg));
                }
            }
    vcpu_destroy(v);
}

extern struct ia64_result baseline_ia64_integer(struct vcpu *, const Ia64Instruction *) __attribute__((weak));
extern struct ia64_result baseline_ia64_simd(struct vcpu *, const Ia64Instruction *) __attribute__((weak));
static void bench_register_pairs(bool simd, unsigned pairs)
{
    static const struct { const char *name; Ia64Opcode integer, simd; int64_t immediate; } cases[] = {
        {"binary", IA64_OP_ADD, IA64_OP_PADD1, 0},
        {"subtract", IA64_OP_SUB, IA64_OP_PSUB4, 0},
        {"immediate", IA64_OP_ADDS, IA64_OP_PSHL2, 3},
        {"unary", IA64_OP_ZXT1, IA64_OP_CZX1_L, 0},
        {"compare", IA64_OP_CMP_EQ, IA64_OP_PCMP1_EQ, 0},
        {"saturating", IA64_OP_XOR, IA64_OP_PADD2, 1},
    };
    execute_bench_fn reference = simd ? baseline_ia64_simd : baseline_ia64_integer;
    CHECK(reference != NULL && pairs > 0 && pairs <= 100);
    struct vcpu *v = new_vcpu();
    enum { CALLS = 1024 * 1024 };
    for (unsigned pair = 0; pair < pairs; ++pair)
        for (unsigned segment = 0; segment < 4; ++segment) {
            unsigned candidate = (pair & 1) ^ (segment == 1 || segment == 2);
            execute_bench_fn target = candidate ? (simd ? ia64_simd : ia64_integer) : reference;
            printf("paired-segment pair=%u order=%u variant=%s\n", pair, segment, candidate ? "candidate" : "baseline");
            for (unsigned mode = 0; mode < IA64VMM_ARRAY_SIZE(cases); ++mode)
                for (unsigned stacked = 0; stacked < 2; ++stacked) {
                    unsigned dst = stacked ? 32 : 8, src1 = stacked ? 33 : 9, src2 = stacked ? 34 : 10;
                    v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(2) << 14) | (UINT64_C(15) << 18);
                    v->s.rse_bof = 95; v->s.pr = 1;
                    ia64_gr_write(&v->s, src1, UINT64_C(0x0102fe04f0061080), false);
                    ia64_gr_write(&v->s, src2, UINT64_C(0x010302058086b0a0), false);
                    ia64_gr_write(&v->s, dst, 0, false);
                    Ia64Instruction input = instruction(simd ? cases[mode].simd : cases[mode].integer);
                    input.operands.integer.destination = dst;
                    input.operands.integer.source1 = src1;
                    input.operands.integer.source2 = src2;
                    input.operands.integer.immediate = cases[mode].immediate;
                    CHECK(target(v, &input).kind == IA64_STEP_COMPLETE);
                    unsigned home = ia64_gr_index(&v->s, dst);
                    uint64_t one = v->s.gr[home] + v->s.nat[home] + v->s.pr;
                    clock_t cpu_start = clock();
                    uint64_t sum = 0, start = hal_counter();
                    for (unsigned k = 0; k < CALLS; ++k) {
                        target(v, &input);
                        sum += v->s.gr[home] + v->s.nat[home] + v->s.pr;
                    }
                    uint64_t elapsed = hal_counter() - start;
                    clock_t cpu_elapsed = clock() - cpu_start;
                    CHECK(sum == one * CALLS);
                    printf("%s-bench input=%s-%s calls=%u ns_per_call=%.3f cpu_ns_per_call=%.3f checksum=%llu\n",
                        simd ? "simdpairs" : "intpairs", cases[mode].name, stacked ? "stacked" : "static", CALLS,
                        (double)elapsed * 1e9 / hal_counter_hz() / CALLS,
                        (double)cpu_elapsed * 1e9 / CLOCKS_PER_SEC / CALLS, (unsigned long long)sum);
                }
        }
    vcpu_destroy(v);
}
