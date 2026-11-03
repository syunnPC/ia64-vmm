/* SPDX-License-Identifier: GPL-2.0-or-later */
static void check_decoded_equal(const Ia64Instruction *a, const Ia64Instruction *b)
{
    /* Compare semantic fields, never unspecified C structure padding. */
#define SAME(f) CHECK(a->f == b->f)
    SAME(opcode); SAME(unit); SAME(raw); SAME(address); SAME(slot); SAME(qp);
    SAME(encoding_class); SAME(pred_update); SAME(hint_m_reg_increment);
    SAME(reg_base_update); SAME(imm_base_update); SAME(mem_acquire); SAME(mem_release);
    SAME(compare_unc); SAME(clear_p2_before_predicate); SAME(check_fp);
    SAME(fp_load_speculative); SAME(fp_load_advanced); SAME(fp_load_check);
    SAME(fp_load_check_clear); SAME(probe_fault); SAME(probe_imm);
    SAME(placement_illegal); SAME(reserved_field); SAME(valid);
    SAME(operands.common.destination); SAME(operands.common.source1);
    SAME(operands.common.source2); SAME(operands.common.auxiliary1);
    SAME(operands.common.auxiliary2); SAME(operands.common.branch1);
    SAME(operands.common.branch2); SAME(operands.common.status_field);
    SAME(operands.common.precision); SAME(operands.common.immediate);
#undef SAME
}
static void test_bundle_reuse(void)
{
    struct ia64_bundle a, b;
    const uint64_t nop = UINT64_C(1) << 27;
    const uint64_t nlow = (nop << 5) | (nop << 46), nhigh = (nop >> 18) | (nop << 23);
    memset(&a, 0xa5, sizeof(a));
    ia64_decode_bundle(4 | (nop << 5), ((UINT64_C(6) << 37) | (8u << 6)) << 23, ENTRY, &a);
    CHECK(a.long_instruction && a.insn[1].opcode == IA64_OP_MOVL);
    ia64_decode_bundle(nlow, nhigh, ENTRY + 16, &a);
    CHECK(!a.long_instruction && a.template_code == 0);
    for (unsigned k = 0; k < 3; ++k) {
        CHECK(a.insn[k].valid && a.insn[k].opcode == IA64_OP_NOP);
        CHECK(a.insn[k].address == ENTRY + 16 && a.insn[k].slot == k);
        CHECK(!a.insn[k].placement_illegal && !a.insn[k].compare_unc);
    }
    a.long_instruction = true;
    ia64_decode_bundle(4 | (nop << 5), (UINT64_C(15) << 37) << 23, ENTRY, &a);
    CHECK(!a.long_instruction && !a.insn[1].valid);
    a.long_instruction = true;
    ia64_decode_bundle(6, 0, ENTRY, &a);
    CHECK(!a.long_instruction);
    for (unsigned k = 0; k < 3; ++k) CHECK(!a.insn[k].valid);

    /* Every template and varied reserved, valid and long encodings must
     * produce the same fields when replacing unrelated previous contents. */
    uint64_t seed = UINT64_C(0x243f6a8885a308d3);
    for (unsigned trial = 0; trial < 1024; ++trial) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint64_t low = (seed & ~UINT64_C(31)) | (trial & 31);
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint64_t high = seed, address = ENTRY + 16u * trial;
        memset(&a, 0xa5, sizeof(a)); memset(&b, 0x5a, sizeof(b));
        ia64_decode_bundle(low, high, address, &a);
        ia64_decode_bundle(low, high, address, &b);
        CHECK(a.low == low && b.low == low && a.high == high && b.high == high);
        CHECK(a.template_code == (trial & 31) && a.template_code == b.template_code);
        CHECK(a.long_instruction == b.long_instruction);
        for (unsigned k = 0; k < 3; ++k) check_decoded_equal(&a.insn[k], &b.insn[k]);
    }
}
static uint64_t decoded_sum(const struct ia64_bundle *b)
{
    uint64_t sum = b->template_code + b->long_instruction;
    for (unsigned k = 0; k < 3; ++k)
        sum += b->insn[k].opcode + b->insn[k].operands.common.immediate +
            b->insn[k].valid + b->insn[k].placement_illegal;
    return sum;
}
typedef void (*decode_bench_fn)(uint64_t, uint64_t, gpa_t, struct ia64_bundle *);
extern void baseline_ia64_decode_bundle(uint64_t, uint64_t, gpa_t, struct ia64_bundle *) __attribute__((weak));
static decode_bench_fn decode_bench_target = ia64_decode_bundle;
static void bench_decode(void)
{
    static const char *const names[] = {"nop", "mlx", "mixed"};
    enum { INPUTS = 64, REPEATS = 32768 };
    uint64_t lows[INPUTS], highs[INPUTS];
    const uint64_t nop = UINT64_C(1) << 27;
    struct ia64_bundle bundle;
    for (unsigned mode = 0; mode < 3; ++mode) {
        uint64_t want = 0, seed = UINT64_C(0x243f6a8885a308d3);
        for (unsigned k = 0; k < INPUTS; ++k) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            lows[k] = mode == 0 ? (nop << 5) | (nop << 46) :
                mode == 1 ? 4 | (nop << 5) | ((uint64_t)k << 46) : seed;
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            highs[k] = mode == 0 ? (nop >> 18) | (nop << 23) :
                mode == 1 ? ((UINT64_C(6) << 37) | (8u << 6)) << 23 : seed;
            decode_bench_target(lows[k], highs[k], ENTRY, &bundle);
            want += decoded_sum(&bundle);
        }
        clock_t cpu_start = clock();
        uint64_t sum = 0, start = hal_counter();
        for (unsigned rep = 0; rep < REPEATS; ++rep)
            for (unsigned k = 0; k < INPUTS; ++k) {
                decode_bench_target(lows[k], highs[k], ENTRY, &bundle);
                sum += decoded_sum(&bundle);
            }
        uint64_t elapsed = hal_counter() - start;
        clock_t cpu_elapsed = clock() - cpu_start;
        CHECK(sum == want * REPEATS);
        printf("decode-bench input=%s calls=%u ns_per_call=%.3f cpu_ns_per_call=%.3f checksum=%llu\n",
            names[mode], INPUTS * REPEATS,
            (double)elapsed * 1e9 / hal_counter_hz() / (INPUTS * REPEATS),
            (double)cpu_elapsed * 1e9 / CLOCKS_PER_SEC / (INPUTS * REPEATS),
            (unsigned long long)sum);
    }
}

/* Both implementations run in the same process in alternating ABBA/BAAB
 * order. The extra indirect call is shared by both measured variants. */
static void bench_decode_paired(unsigned pairs)
{
    CHECK(baseline_ia64_decode_bundle != NULL && pairs > 0 && pairs <= 100);
    for (unsigned pair = 0; pair < pairs; ++pair)
        for (unsigned segment = 0; segment < 4; ++segment) {
            unsigned candidate = (pair & 1) ^ (segment == 1 || segment == 2);
            decode_bench_target = candidate ? ia64_decode_bundle : baseline_ia64_decode_bundle;
            printf("paired-segment pair=%u order=%u variant=%s\n",
                pair, segment, candidate ? "candidate" : "baseline");
            bench_decode();
        }
    decode_bench_target = ia64_decode_bundle;
}
