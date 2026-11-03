/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional instruction-helper microbenchmark. No decode/dispatch/guest
 * memory timing is implied. Input setup and oracle checks are not timed. */
static void bench_popcnt(void)
{
    static const char *names[] = {"zero", "one-bit", "random", "all-bits"};
    enum { INPUTS = 1024, REPEATS = 4096 };
    uint64_t input[INPUTS], expected[INPUTS];
    struct vcpu *v = new_vcpu();
    Ia64Instruction insn = instruction(IA64_OP_POPCNT);
    for (unsigned mode = 0; mode < 4; ++mode) {
        uint64_t state = UINT64_C(0x243f6a8885a308d3), want = 0;
        for (unsigned i = 0; i < INPUTS; ++i) {
            state ^= state << 13; state ^= state >> 7; state ^= state << 17;
            input[i] = mode == 0 ? 0 : mode == 1 ? UINT64_C(1) << (i % 64) :
                       mode == 2 ? state : UINT64_MAX;
            expected[i] = 0;
            for (unsigned bit = 0; bit < 64; ++bit) expected[i] += (input[i] >> bit) & 1;
            want += expected[i];
            v->s.gr[10] = input[i];
            CHECK(ia64_integer(v, &insn).kind == IA64_STEP_COMPLETE);
            CHECK(v->s.gr[8] == expected[i]);
        }
        uint64_t sum = 0, start = hal_counter();
        for (unsigned rep = 0; rep < REPEATS; ++rep)
            for (unsigned i = 0; i < INPUTS; ++i) {
                v->s.gr[10] = input[i];
                ia64_integer(v, &insn);
                sum += v->s.gr[8];
            }
        uint64_t elapsed = hal_counter() - start;
        CHECK(sum == want * REPEATS);
        printf("popcnt-bench input=%s calls=%u ns_per_call=%.3f checksum=%llu\n",
               names[mode], INPUTS * REPEATS,
               (double)elapsed * 1e9 / hal_counter_hz() / (INPUTS * REPEATS),
               (unsigned long long)sum);
    }
    vcpu_destroy(v);
}
