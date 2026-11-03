/* SPDX-License-Identifier: GPL-2.0-or-later */
static void test_opcode_predication(void)
{
    /* Fault priority crosses integer, bit-test and floating predicate pairs,
     * including immediate forms and both ends of the compare families. */
    const Ia64Opcode pairs[] = {
        IA64_OP_CMP_EQ, IA64_OP_CMP_LTU_IMM, IA64_OP_CMP4_EQ,
        IA64_OP_CMP4_LTU_IMM, IA64_OP_CMP4_EQ_IMM, IA64_OP_CMP4_NE_OR_IMM,
        IA64_OP_TBIT_Z, IA64_OP_TBIT_NZ, IA64_OP_TBIT_Z_OR_ANDCM,
        IA64_OP_TBIT_NZ_OR_ANDCM, IA64_OP_TNAT_Z, IA64_OP_TNAT_NZ,
        IA64_OP_TNAT_NZ_AND, IA64_OP_FCMP, IA64_OP_FCLASS,
        IA64_OP_TF_Z, IA64_OP_TF_NZ
    };
    struct vcpu *v = new_vcpu();
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(pairs); ++k) {
        Ia64Instruction i = instruction(pairs[k]);
        i.qp = 1;
        i.operands.common.auxiliary1 = i.operands.common.auxiliary2 = 6;
        v->s.pr = 1;
        i.compare_unc = true;
        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
        i.compare_unc = false;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        CHECK(v->s.pr == 1);
        v->s.pr |= UINT64_C(1) << 1;
        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
        i.operands.common.auxiliary2 = 7;
        i.compare_unc = true;
        v->s.pr = 1 | (UINT64_C(1) << 6) | (UINT64_C(1) << 7);
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        CHECK(v->s.pr == 1);
    }
    const Ia64Opcode loops[] = {IA64_OP_BR_CLOOP, IA64_OP_BR_CTOP,
        IA64_OP_BR_CEXIT, IA64_OP_BR_WTOP, IA64_OP_BR_WEXIT};
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(loops); ++k) {
        Ia64Instruction i = instruction(loops[k]);
        i.qp = 1; i.slot = 1; v->s.pr = 1;
        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
    }
    Ia64Instruction i = instruction(IA64_OP_ADD);
    i.qp = 1; i.compare_unc = true;
    i.operands.common.auxiliary1 = i.operands.common.auxiliary2 = 6;
    v->s.pr = 1 | (UINT64_C(1) << 6); v->s.gr[8] = 99;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 99 && v->s.pr == (1 | (UINT64_C(1) << 6)));
    vcpu_destroy(v);
}

/* Predecoded execution only. It deliberately includes the result ABI and
 * register access, but excludes fetch, translation and bundle decoding. */
typedef struct ia64_result (*execute_bench_fn)(struct vcpu *, const Ia64Instruction *);
extern struct ia64_result baseline_ia64_execute(struct vcpu *, const Ia64Instruction *) __attribute__((weak));
static execute_bench_fn execute_bench_target = ia64_execute;
static void bench_execute(void)
{
    static const char *const names[] = {"add", "and", "cmp", "sub", "false", "mixed"};
    static const Ia64Opcode mixed[] = {IA64_OP_ADD, IA64_OP_AND, IA64_OP_SUB, IA64_OP_XOR};
    static const uint64_t wants[] = {23, 2, 206, 13, 78, 0};
    enum { CALLS = 4 * 1024 * 1024 };
    struct vcpu *v = new_vcpu();
    for (unsigned mode = 0; mode < IA64VMM_ARRAY_SIZE(names); ++mode) {
        Ia64Instruction input[4];
        for (unsigned k = 0; k < 4; ++k) {
            Ia64Opcode op = mode == 0 || mode == 4 ? IA64_OP_ADD :
                mode == 1 ? IA64_OP_AND : mode == 2 ? IA64_OP_CMP_EQ :
                mode == 3 ? IA64_OP_SUB : mixed[k];
            input[k] = instruction(op);
            input[k].qp = mode == 4 ? 1 : 0;
        }
        v->s.gr[9] = 17; v->s.gr[10] = 5; v->s.gr[8] = 77; v->s.pr = 1;
        for (unsigned k = 0; k < 4; ++k)
            CHECK(execute_bench_target(v, &input[k]).kind == IA64_STEP_COMPLETE);
        clock_t cpu_start = clock();
        uint64_t sum = 0, start = hal_counter();
        for (unsigned k = 0; k < CALLS; ++k) {
            execute_bench_target(v, &input[k & 3]);
            sum += v->s.gr[8] + v->s.pr;
        }
        uint64_t elapsed = hal_counter() - start;
        clock_t cpu_elapsed = clock() - cpu_start;
        uint64_t want = mode == 5 ? UINT64_C(59) * (CALLS / 4) :
            wants[mode] * CALLS;
        CHECK(sum == want);
        printf("execute-bench input=%s calls=%u ns_per_call=%.3f cpu_ns_per_call=%.3f checksum=%llu\n",
            names[mode], CALLS, (double)elapsed * 1e9 / hal_counter_hz() / CALLS,
            (double)cpu_elapsed * 1e9 / CLOCKS_PER_SEC / CALLS,
            (unsigned long long)sum);
    }
    vcpu_destroy(v);
}

/* Both implementations run in the same process in alternating ABBA/BAAB
 * order. The extra indirect call is shared by both measured variants. */
static void bench_execute_paired(unsigned pairs)
{
    CHECK(baseline_ia64_execute != NULL && pairs > 0 && pairs <= 100);
    for (unsigned pair = 0; pair < pairs; ++pair)
        for (unsigned segment = 0; segment < 4; ++segment) {
            unsigned candidate = (pair & 1) ^ (segment == 1 || segment == 2);
            execute_bench_target = candidate ? ia64_execute : baseline_ia64_execute;
            printf("paired-segment pair=%u order=%u variant=%s\n",
                pair, segment, candidate ? "candidate" : "baseline");
            bench_execute();
        }
    execute_bench_target = ia64_execute;
}
