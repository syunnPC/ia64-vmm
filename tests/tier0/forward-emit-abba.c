/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reuse exactly the execution benchmark's fixtures, with its entry point
 * renamed. Both translators share this process and the unchanged helpers. */
#define main forward_execution_benchmark_main
#include "tests/tier0/forward-bench.c"
#undef main

void tier0_emit_baseline(struct x86_buffer *, struct tier0_region *, const struct ia64_state *);

int main(int argc, char **argv)
{
    if (argc != 6)
        return 2;
    unsigned first = strtoul(argv[2], NULL, 0) ? 48 : 8;
    unsigned mask = (unsigned)strtoul(argv[3], NULL, 0);
    unsigned blocks = (unsigned)strtoul(argv[4], NULL, 0);
    unsigned count = (unsigned)strtoul(argv[5], NULL, 0);
    if (!blocks || !count)
        return 2;
    hal_linux_host_isa_mask(mask);
    Ia64Instruction insns[TIER0_MAX_INSNS];
    fixture(insns, argv[1], first);
    struct vcpu *v = calloc(1, sizeof(*v));
    void *code = calloc(1, 65536);
    if (!v || !code)
        abort();
    v->s.cfm = 96;
    v->s.pr = 1;
    struct tier0_region r = {.insns = insns, .count = TIER0_MAX_INSNS};
    memset(r.slots, 1, sizeof(r.slots));
    void (*emit[2])(struct x86_buffer *, struct tier0_region *,
                    const struct ia64_state *) = {tier0_emit_baseline, tier0_emit};
    size_t bytes[2] = {0};
    printf("{\"host_isa\":%u,\"samples\":[", hal_host_isa());
    bool comma = false;
    /* One ABBA warmup block, then alternating ABBA/BAAB blocks. Each
     * variant gets two observations at symmetric positions per block. */
    for (unsigned block = 0; block <= blocks; ++block)
        for (unsigned at = 0; at < 4; ++at) {
            unsigned variant = ((at == 1 || at == 2) ? 1u : 0u) ^ (block & 1);
            uint64_t begin = ns();
            for (unsigned n = 0; n < count; ++n) {
                struct x86_buffer b = {.data = code, .capacity = 65536};
                emit[variant](&b, &r, &v->s);
                if (b.failed || (bytes[variant] && bytes[variant] != b.size))
                    abort();
                bytes[variant] = b.size;
            }
            uint64_t elapsed = ns() - begin;
            if (block) {
                printf("%s{\"variant\":\"%s\",\"block\":%u,\"position\":%u,"
                       "\"bytes\":%zu,\"emit_ns_per_region\":%.3f}",
                       comma ? "," : "", variant ? "current" : "baseline", block, at,
                       bytes[variant], (double)elapsed / count);
                comma = true;
            }
        }
    printf("]}\n");
    free(code);
    free(v);
    return 0;
}
