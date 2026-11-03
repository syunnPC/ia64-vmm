/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic native-region benchmark. codegen_bench.py links this unchanged
 * driver against each checkout's objects; no assembler or guest image is
 * needed. Execution times exclude the Tier0 lookup/dispatch loop. */
#define _POSIX_C_SOURCE 200809L
#include "engine/tier0/internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/host_isa.h"
#include "tests/hal/hal_linux.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static void fixture(Ia64Instruction *insns, const char *name, unsigned first)
{
    const Ia64Opcode ext[] = {IA64_OP_SXT1, IA64_OP_SXT2, IA64_OP_SXT4,
                              IA64_OP_ZXT1, IA64_OP_ZXT2, IA64_OP_ZXT4};
    const Ia64Opcode imm[] = {IA64_OP_ADDS, IA64_OP_ADDL, IA64_OP_AND_IMM, IA64_OP_OR_IMM,
                              IA64_OP_XOR_IMM};
    const int64_t imm_value[] = {127, 4096, -128, 127, 1};
    for (unsigned at = 0; at < TIER0_MAX_INSNS; ++at) {
        Ia64Instruction *i = &insns[at];
        *i = (Ia64Instruction){.opcode = IA64_OP_ADD,
                               .valid = true,
                               .unit = IA64_UNIT_I,
                               .address = UINT64_C(0x100000) + 16 * (at / 3),
                               .slot = at % 3,
                               .operands.integer = {.destination = first + at % 8,
                                                    .source1 = first + (at + 2) % 8,
                                                    .source2 = first + (at + 1) % 8,
                                                    .predicate1 = 6,
                                                    .predicate2 = 7}};
        if (!strcmp(name, "extensions"))
            i->opcode = ext[at % 6];
        else if (!strcmp(name, "immediates")) {
            i->opcode = imm[at % 5];
            i->operands.integer.immediate = imm_value[at % 5];
        } else if (!strcmp(name, "extract")) {
            i->opcode = at & 1 ? IA64_OP_EXTRU : IA64_OP_EXTR;
            i->operands.integer.immediate = at % 4 < 2 ? 7 | (13 << 6) : 13 | (51 << 6);
        } else if (!strcmp(name, "compact")) {
            i->opcode = at % 3 == 0 ? IA64_OP_SHL_IMM : at % 3 == 1 ? IA64_OP_CMP_EQ : IA64_OP_ADDS;
            i->operands.integer.immediate = at % 3 == 2 ? 4096 : 1;
        }
    }
}
int main(int argc, char **argv)
{
    if (argc != 5)
        return 2;
    unsigned mask = (unsigned)strtoul(argv[2], NULL, 0);
    unsigned emit_count = (unsigned)strtoul(argv[3], NULL, 0);
    unsigned run_count = (unsigned)strtoul(argv[4], NULL, 0);
    if (!emit_count || !run_count)
        return 2;
    hal_linux_host_isa_mask(mask);
    Ia64Instruction insns[TIER0_MAX_INSNS];
    unsigned first = !strcmp(argv[1], "stacked") ? 48 : 8;
    fixture(insns, argv[1], first);
    struct vcpu *v = calloc(1, sizeof(*v)), *ref = calloc(1, sizeof(*ref));
    void *code = hal_alloc_pages(16, HAL_PAGE_EXEC);
    if (!v || !ref || !code)
        abort();
    v->s.cfm = 96;
    v->s.pr = 1;
    for (unsigned reg = 1; reg < 128; ++reg)
        v->s.gr[reg] = UINT64_C(0xfedcba9876543210) + reg;
    struct tier0_region r = {.insns = insns, .count = TIER0_MAX_INSNS};
    memset(r.slots, 1, sizeof(r.slots));
    struct tier0_homes homes = {0};
    for (unsigned reg = 0; reg < 96; ++reg)
        homes.gr_index[reg] = 32 + reg;
    struct tier0_execution e = {.region = &r};
    struct x86_buffer b = {0};
    uint64_t begin = ns();
    for (unsigned n = 0; n < emit_count; ++n) {
        b = (struct x86_buffer){.data = code, .capacity = 65536};
        tier0_emit(&b, &r, &v->s);
        if (b.failed)
            abort();
    }
    uint64_t emit_ns = ns() - begin;
    hal_code_set_executable(code, 16);
    void (*fn)(struct vcpu *, uint64_t, struct tier0_execution *, const struct tier0_homes *) =
        code;
    *ref = *v;
    for (unsigned at = 0; at < r.count; ++at) {
        if (ia64_execute(ref, &insns[at]).kind != IA64_STEP_COMPLETE)
            abort();
        ++ref->retired;
    }
    ref->s.ip = insns[r.count - 1].address + 16;
    ref->s.slot = 0;
    fn(v, r.count, &e, &homes);
    if (memcmp(&v->s, &ref->s, sizeof(v->s)) || v->retired != ref->retired)
        abort();
    begin = ns();
    for (unsigned n = 0; n < run_count; ++n)
        fn(v, r.count, &e, &homes);
    uint64_t run_ns = ns() - begin;
    printf(
        "{\"bytes\":%zu,\"host_isa\":%u,\"emit_ns_per_region\":%.3f,\"run_ns_per_instruction\":%.3f,\"checksum\":\"%016" PRIx64
        "\"}\n",
        b.size, hal_host_isa(), (double)emit_ns / emit_count, (double)run_ns / run_count / r.count,
        v->retired ^ v->s.gr[first]);
    hal_free_pages(code, 16);
    free(v);
    free(ref);
    return 0;
}
