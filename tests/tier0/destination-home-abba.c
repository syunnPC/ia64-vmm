/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Both translators and the emitted regions share one process. The fixture
 * constructor is exactly the independent process driver's constructor. */
#define main destination_home_execution_benchmark_main
#include "tests/tier0/destination-home-bench.c"
#undef main

void tier0_emit_baseline(struct x86_buffer *, struct tier0_region *, const struct ia64_state *);

int main(int argc, char **argv)
{
    if (argc != 7)
        return 2;
    unsigned first = (unsigned)strtoul(argv[2], NULL, 0);
    unsigned mask = (unsigned)strtoul(argv[3], NULL, 0);
    unsigned blocks = (unsigned)strtoul(argv[4], NULL, 0);
    unsigned emits = (unsigned)strtoul(argv[5], NULL, 0);
    unsigned calls = (unsigned)strtoul(argv[6], NULL, 0);
    if (first > 120 || !blocks || !emits || !calls)
        return 2;
    hal_linux_host_isa_mask(mask);
    Ia64Instruction insns[TIER0_MAX_INSNS];
    fixture(insns, argv[1], first);
    struct vcpu *v[2] = {calloc(1, sizeof(struct vcpu)), calloc(1, sizeof(struct vcpu))};
    struct vcpu *ref = calloc(1, sizeof(*ref));
    void *allocation[2] = {hal_alloc_pages(16, HAL_PAGE_EXEC), hal_alloc_pages(16, HAL_PAGE_EXEC)};
    void *buffer = calloc(1, 65536);
    if (!v[0] || !v[1] || !ref || !allocation[0] || !allocation[1] || !buffer)
        abort();
    v[0]->s.cfm = 96;
    v[0]->s.pr = 1;
    for (unsigned reg = 1; reg < 128; ++reg)
        v[0]->s.gr[reg] = UINT64_C(0xfedcba9876543210) + reg;
    *v[1] = *ref = *v[0];
    struct tier0_region r[2] = {{.insns = insns, .count = TIER0_MAX_INSNS},
                                {.insns = insns, .count = TIER0_MAX_INSNS}};
    struct tier0_homes homes = {0};
    for (unsigned reg = 0; reg < 96; ++reg)
        homes.gr_index[reg] = 32 + reg;
    for (unsigned p = 16; p < 64; ++p)
        homes.pr_mask[p - 16] = UINT64_C(1) << p;
    struct tier0_execution e[2] = {{.region = &r[0]}, {.region = &r[1]}};
    void (*emit[2])(struct x86_buffer *, struct tier0_region *,
                    const struct ia64_state *) = {tier0_emit_baseline, tier0_emit};
    void (*run[2])(struct vcpu *, uint64_t, struct tier0_execution *,
                   const struct tier0_homes *) = {allocation[0], allocation[1]};
    size_t bytes[2];
    for (unsigned variant = 0; variant < 2; ++variant) {
        memset(r[variant].slots, 1, sizeof(r[variant].slots));
        struct x86_buffer b = {.data = allocation[variant], .capacity = 65536};
        emit[variant](&b, &r[variant], &v[variant]->s);
        if (b.failed)
            abort();
        bytes[variant] = b.size;
        hal_code_set_executable(allocation[variant], 16);
    }
    for (unsigned at = 0; at < TIER0_MAX_INSNS; ++at) {
        if (ia64_execute(ref, &insns[at]).kind != IA64_STEP_COMPLETE)
            abort();
        ++ref->retired;
    }
    ref->s.ip = insns[TIER0_MAX_INSNS - 1].address + 16;
    ref->s.slot = 0;
    for (unsigned variant = 0; variant < 2; ++variant) {
        run[variant](v[variant], TIER0_MAX_INSNS, &e[variant], &homes);
        if (memcmp(&v[variant]->s, &ref->s, sizeof(ref->s)) || v[variant]->retired != ref->retired)
            abort();
    }
    printf("{\"host_isa\":%u,\"samples\":[", hal_host_isa());
    bool comma = false;
    /* Warm up both paths, then place each variant symmetrically in every
     * ABBA/BAAB block. Emission and native calls have separate intervals. */
    for (unsigned block = 0; block <= blocks; ++block)
        for (unsigned at = 0; at < 4; ++at) {
            unsigned variant = ((at == 1 || at == 2) ? 1u : 0u) ^ (block & 1);
            uint64_t begin = ns();
            for (unsigned n = 0; n < emits; ++n) {
                struct x86_buffer b = {.data = buffer, .capacity = 65536};
                emit[variant](&b, &r[variant], &v[variant]->s);
                if (b.failed || b.size != bytes[variant])
                    abort();
            }
            uint64_t emission_ns = ns() - begin;
            begin = ns();
            for (unsigned n = 0; n < calls; ++n)
                run[variant](v[variant], TIER0_MAX_INSNS, &e[variant], &homes);
            uint64_t native_ns = ns() - begin;
            if (block) {
                printf("%s{\"variant\":\"%s\",\"block\":%u,\"position\":%u,\"bytes\":%zu,"
                       "\"emit_ns_per_region\":%.3f,\"run_ns_per_instruction\":%.3f}",
                       comma ? "," : "", variant ? "current" : "baseline", block, at,
                       bytes[variant], (double)emission_ns / emits,
                       (double)native_ns / calls / TIER0_MAX_INSNS);
                comma = true;
            }
        }
    if (memcmp(&v[0]->s, &v[1]->s, sizeof(v[0]->s)) || v[0]->retired != v[1]->retired)
        abort();
    printf("],\"checksum\":\"%016" PRIx64 "\"}\n", v[0]->retired ^ v[0]->s.gr[first] ^ v[0]->s.pr);
    for (unsigned variant = 0; variant < 2; ++variant) {
        hal_free_pages(allocation[variant], 16);
        free(v[variant]);
    }
    free(buffer);
    free(ref);
    return 0;
}
