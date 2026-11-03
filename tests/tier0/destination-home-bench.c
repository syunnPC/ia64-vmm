/* SPDX-License-Identifier: GPL-2.0-or-later */
/* One unchanged driver links against the two checkouts. Timing separates
 * the NULL-data measuring pass, actual emission and native execution;
 * lookup, arena publication and engine dispatch are outside these loops. */
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
    const Ia64Opcode extensions[] = {IA64_OP_SXT1, IA64_OP_SXT2, IA64_OP_SXT4,
                                     IA64_OP_ZXT1, IA64_OP_ZXT2, IA64_OP_ZXT4};
    for (unsigned at = 0; at < TIER0_MAX_INSNS; ++at) {
        Ia64Instruction *i = &insns[at];
        unsigned dest = first + at % 8;
        *i = (Ia64Instruction){
            .opcode = IA64_OP_ADD,
            .valid = true,
            .unit = IA64_UNIT_I,
            .address = UINT64_C(0x100000) + 16 * (at / 3),
            .slot = at % 3,
            .operands.integer = {.destination = dest, .source1 = 4, .source2 = 5, .immediate = 3}};
        if (!strcmp(name, "home-first"))
            i->operands.integer.source1 = dest;
        else if (!strcmp(name, "home-second"))
            i->operands.integer.source2 = dest;
        else if (!strcmp(name, "home-immediate")) {
            i->opcode = IA64_OP_ADDS;
            i->operands.integer.source2 = dest;
        } else if (!strcmp(name, "home-extended")) {
            i->opcode = extensions[at % IA64VMM_ARRAY_SIZE(extensions)];
            i->operands.integer.source2 = dest;
        }
    }
}
int main(int argc, char **argv)
{
    if (argc != 9)
        return 2;
    unsigned mask = (unsigned)strtoul(argv[2], NULL, 0);
    unsigned first = (unsigned)strtoul(argv[3], NULL, 0);
    unsigned measure_count = (unsigned)strtoul(argv[4], NULL, 0);
    unsigned emit_count = (unsigned)strtoul(argv[5], NULL, 0);
    unsigned run_count = (unsigned)strtoul(argv[6], NULL, 0);
    unsigned count = (unsigned)strtoul(argv[7], NULL, 0);
    unsigned offset = (unsigned)strtoul(argv[8], NULL, 0);
    if (!measure_count || !emit_count || !run_count || first > 120 || !count ||
        count > TIER0_MAX_INSNS || offset > 48)
        return 2;
    hal_linux_host_isa_mask(mask);
    Ia64Instruction insns[TIER0_MAX_INSNS];
    fixture(insns, argv[1], first);
    struct vcpu *v = calloc(1, sizeof(*v)), *ref = calloc(1, sizeof(*ref));
    void *allocation = hal_alloc_pages(16, HAL_PAGE_EXEC);
    if (!v || !ref || !allocation)
        abort();
    void *code = (uint8_t *)allocation + offset;
    v->s.cfm = 96;
    v->s.pr = 1;
    for (unsigned reg = 1; reg < 128; ++reg)
        v->s.gr[reg] = UINT64_C(0xfedcba9876543210) + reg;
    struct tier0_region r = {.insns = insns, .count = count};
    memset(r.slots, 1, sizeof(r.slots));
    struct tier0_homes homes = {0};
    for (unsigned reg = 0; reg < 96; ++reg)
        homes.gr_index[reg] = 32 + reg;
    for (unsigned p = 16; p < 64; ++p)
        homes.pr_mask[p - 16] = UINT64_C(1) << p;
    struct tier0_execution e = {.region = &r};
    struct x86_buffer b = {0};
    uint64_t begin = ns();
    for (unsigned n = 0; n < measure_count; ++n) {
        b = (struct x86_buffer){.capacity = 65536 - offset};
        tier0_emit(&b, &r, &v->s);
        if (b.failed)
            abort();
    }
    uint64_t measure_ns = ns() - begin;
    size_t measured_bytes = b.size;
    begin = ns();
    for (unsigned n = 0; n < emit_count; ++n) {
        b = (struct x86_buffer){.data = code, .capacity = 65536 - offset};
        tier0_emit(&b, &r, &v->s);
        if (b.failed || b.size != measured_bytes)
            abort();
    }
    uint64_t emit_ns = ns() - begin;
    hal_code_set_executable(allocation, 16);
    void (*fn)(struct vcpu *, uint64_t, struct tier0_execution *, const struct tier0_homes *) =
        code;
    *ref = *v;
    for (unsigned at = 0; at < r.count; ++at) {
        if (ia64_execute(ref, &insns[at]).kind != IA64_STEP_COMPLETE)
            abort();
        ++ref->retired;
    }
    unsigned slot = insns[r.count - 1].slot + 1;
    ref->s.ip = insns[r.count - 1].address + (slot == 3 ? 16 : 0);
    ref->s.slot = slot % 3;
    fn(v, r.count, &e, &homes);
    if (memcmp(&v->s, &ref->s, sizeof(v->s)) || v->retired != ref->retired)
        abort();
    begin = ns();
    for (unsigned n = 0; n < run_count; ++n)
        fn(v, r.count, &e, &homes);
    uint64_t run_ns = ns() - begin;
    printf("{\"bytes\":%zu,\"host_isa\":%u,\"measure_ns_per_region\":%.3f,"
           "\"emit_ns_per_region\":%.3f,\"run_ns_per_instruction\":%.3f,\"checksum\":\"%016" PRIx64
           "\"}\n",
           b.size, hal_host_isa(), (double)measure_ns / measure_count, (double)emit_ns / emit_count,
           (double)run_ns / run_count / r.count, v->retired ^ v->s.gr[first] ^ v->s.pr);
    hal_free_pages(allocation, 16);
    free(v);
    free(ref);
    return 0;
}
