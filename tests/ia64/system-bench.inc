/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A common predecoded harness is linked against each production system.o.
 * The checksum covers both the source and destination register classes.
 * Separate-process measurements include ia64_execute; ABBA measurements
 * call both system implementations through the same function pointer. */
extern struct ia64_result baseline_ia64_system(struct vcpu *, const Ia64Instruction *) __attribute__((weak));
static execute_bench_fn system_bench_target = ia64_execute;
static void bench_system(void)
{
    static const struct {
        const char *name;
        Ia64Opcode opcode;
        unsigned destination, source;
    } fixtures[] = {
        {"nop", IA64_OP_NOP, 0, 0},
        {"hint-i", IA64_OP_HINT_I, 0, 0},
        {"hint-m", IA64_OP_HINT_M, 0, 0},
        {"brp", IA64_OP_BRP, 0, 0},
        {"br-to-static", IA64_OP_MOV_BRGR, 8, 1},
        {"br-to-stacked", IA64_OP_MOV_BRGR, 32, 1},
        {"static-to-br", IA64_OP_MOV_GRBR, 8, 1},
        {"stacked-to-br", IA64_OP_MOV_GRBR, 32, 1},
        {"imm-to-ar", IA64_OP_MOV_IMMAR, 0, AR_LC},
        {"ar-to-stacked", IA64_OP_MOV_ARGR, 32, AR_LC},
        {"stacked-to-ar", IA64_OP_MOV_GRAR, 32, AR_LC},
        {"pr-to-stacked", IA64_OP_MOV_PRGR, 32, 0},
    };
    enum { CALLS = 4 * 1024 * 1024 };
    struct vcpu *v = new_vcpu();
    for (unsigned mode = 0; mode < IA64VMM_ARRAY_SIZE(fixtures); ++mode) {
        v->s.cfm = 16 | (UINT64_C(16) << 7) | (UINT64_C(2) << 14) | (UINT64_C(7) << 18);
        v->s.rse_bof = 95;
        ia64_gr_write(&v->s, 8, 19, false);
        ia64_gr_write(&v->s, 32, 23, false);
        v->s.br[1] = 17; v->s.ar[AR_LC] = 11; v->s.pr = 1;
        Ia64Instruction input = instruction(fixtures[mode].opcode);
        input.unit = IA64_UNIT_I;
        input.operands.system.destination = fixtures[mode].destination;
        input.operands.system.source = fixtures[mode].source;
        input.operands.system.immediate = 13;
        for (unsigned warm = 0; warm < 4; ++warm)
            CHECK(system_bench_target(v, &input).kind == IA64_STEP_COMPLETE);
        unsigned home = ia64_gr_index(&v->s, 32);
        uint64_t one = v->s.gr[8] + v->s.gr[home] + v->s.br[1] + v->s.ar[AR_LC] + v->s.pr;
        clock_t cpu_start = clock();
        uint64_t sum = 0, start = hal_counter();
        for (unsigned k = 0; k < CALLS; ++k) {
            system_bench_target(v, &input);
            sum += v->s.gr[8] + v->s.gr[home] + v->s.br[1] + v->s.ar[AR_LC] + v->s.pr;
        }
        uint64_t elapsed = hal_counter() - start;
        clock_t cpu_elapsed = clock() - cpu_start;
        CHECK(sum == one * CALLS);
        printf("system-bench input=%s calls=%u ns_per_call=%.3f cpu_ns_per_call=%.3f checksum=%llu\n",
            fixtures[mode].name, CALLS, (double)elapsed * 1e9 / hal_counter_hz() / CALLS,
            (double)cpu_elapsed * 1e9 / CLOCKS_PER_SEC / CALLS,
            (unsigned long long)sum);
    }
    vcpu_destroy(v);
}

static void bench_system_paired(unsigned pairs)
{
    CHECK(baseline_ia64_system != NULL && pairs > 0 && pairs <= 100);
    for (unsigned pair = 0; pair < pairs; ++pair)
        for (unsigned segment = 0; segment < 4; ++segment) {
            unsigned candidate = (pair & 1) ^ (segment == 1 || segment == 2);
            system_bench_target = candidate ? ia64_system : baseline_ia64_system;
            printf("paired-segment pair=%u order=%u variant=%s\n", pair, segment,
                candidate ? "candidate" : "baseline");
            bench_system();
        }
    system_bench_target = ia64_execute;
}
