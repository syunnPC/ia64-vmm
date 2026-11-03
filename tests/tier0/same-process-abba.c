/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Full baseline/current translators and emitters coexist in one process.
 * Timed native regions retain all ordinary budgets and notification polls. */
#define _POSIX_C_SOURCE 200809L
#include "engine/tier0/internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/host_isa.h"
#include "tests/hal/hal_linux.h"
#include "ia64/mmu.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if IA64VMM_TIER0_STATS || IA64VMM_TIER1_STATS
#error "Build both checkouts with TIER0_STATS=0 TIER1_STATS=0"
#endif

void tier0_emit_baseline(struct x86_buffer *, struct tier0_region *, const struct ia64_state *);

static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static void fixture(Ia64Instruction *insns, const char *name, unsigned first, unsigned count)
{
    static const Ia64Opcode immediate[] = {IA64_OP_ADDS, IA64_OP_ADDL, IA64_OP_AND_IMM,
                                           IA64_OP_OR_IMM, IA64_OP_XOR_IMM};
    bool gaps = !strcmp(name, "nop-gap") || !strcmp(name, "hint-gap");
    bool neutral = !strcmp(name, "neutral") || !strcmp(name, "neutral-independent") ||
                   !strcmp(name, "neutral-mixed");
    bool system = !strncmp(name, "system-", 7);
    bool memory = !strncmp(name, "memory-", 7);
    bool self = !strncmp(name, "self-", 5);
    for (unsigned at = 0; at < count; ++at) {
        unsigned operation = gaps ? at / 3 : at;
        unsigned previous = first + (operation + 7) % 8;
        Ia64Instruction *i = &insns[at];
        *i = (Ia64Instruction){.opcode = IA64_OP_ADD,
                               .valid = true,
                               .unit = IA64_UNIT_I,
                               .address = UINT64_C(0x100000) + 16 * (at / 3),
                               .slot = at % 3,
                               .operands.integer = {.destination = first + operation % 8,
                                                    .source1 = first + 8,
                                                    .source2 = first + 9,
                                                    .immediate = 3}};
        if (system) {
            if (at % 3 == 0) {
                i->opcode = IA64_OP_NOP;
                i->unit = IA64_UNIT_M;
            } else {
                i->opcode = !strcmp(name, "system-br")   ? IA64_OP_MOV_GRBR
                            : !strcmp(name, "system-pr") ? IA64_OP_MOV_GRPR
                                                         : IA64_OP_MOV_GRAR;
                i->operands.system =
                    (IA64SystemOperands){.destination = first + at % 8,
                                         .source = !strcmp(name, "system-pfs")     ? AR_PFS
                                                   : i->opcode == IA64_OP_MOV_GRAR ? AR_LC
                                                                                   : at % 8,
                                         .immediate = -2};
            }
        } else if (memory) {
            if (at % 3)
                i->opcode = IA64_OP_NOP;
            else {
                i->opcode = !strcmp(name, "memory-store") ? IA64_OP_ST8 : IA64_OP_LD8;
                i->unit = IA64_UNIT_M;
                i->reg_base_update = !strcmp(name, "memory-update");
                i->operands.memory = (IA64MemoryOperands){
                    .destination = first + operation % 8, .base = first + 8, .source = first + 9};
            }
        } else if (self) {
            i->opcode = !strcmp(name, "self-shifts")
                            ? IA64_OP_SHL_IMM
                            : immediate[at % IA64VMM_ARRAY_SIZE(immediate)];
            i->operands.integer.source2 = first + at % 8;
            i->operands.integer.immediate = i->opcode == IA64_OP_AND_IMM ? -1 : 0;
            if (!strcmp(name, "self-mixed") && at % 2)
                i->operands.integer.immediate = 1;
        } else if (gaps && at % 3) {
            i->opcode = !strcmp(name, "nop-gap") ? IA64_OP_NOP
                                                 : (at % 3 == 1 ? IA64_OP_HINT_I : IA64_OP_HINT_F);
            i->unit = i->opcode == IA64_OP_HINT_F ? IA64_UNIT_F : IA64_UNIT_I;
        } else if (gaps || !strcmp(name, "adjacent")) {
            i->opcode = IA64_OP_ADDS;
            i->operands.integer.source2 = previous;
        } else if (neutral) {
            i->opcode = immediate[at % IA64VMM_ARRAY_SIZE(immediate)];
            i->operands.integer.immediate = i->opcode == IA64_OP_AND_IMM ? -1 : 0;
            if (!strcmp(name, "neutral-mixed") && at % 2)
                i->operands.integer.immediate += i->opcode == IA64_OP_AND_IMM ? -1 : 1;
            if (strcmp(name, "neutral-independent"))
                i->operands.integer.source2 = previous;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 11)
        return 2;
    unsigned first = (unsigned)strtoul(argv[2], NULL, 0);
    unsigned mask = (unsigned)strtoul(argv[3], NULL, 0);
    unsigned blocks = (unsigned)strtoul(argv[4], NULL, 0);
    unsigned emits = (unsigned)strtoul(argv[5], NULL, 0);
    unsigned calls = (unsigned)strtoul(argv[6], NULL, 0);
    unsigned count = (unsigned)strtoul(argv[7], NULL, 0);
    unsigned offset = (unsigned)strtoul(argv[8], NULL, 0);
    bool ic = strtoul(argv[9], NULL, 0) != 0;
    uint64_t budget = strtoull(argv[10], NULL, 0);
    if (first > 110 || !blocks || !emits || !calls || !count || count > TIER0_MAX_INSNS ||
        offset > 48)
        return 2;
    hal_linux_host_isa_mask(mask);
    bool memory = !strncmp(argv[1], "memory-", 7), system = !strncmp(argv[1], "system-", 7);
    bool virt = !strcmp(argv[1], "memory-virtual");
    const uint64_t pa = UINT64_C(0x400000), va = UINT64_C(0x6000000008000000);
    static _Alignas(4096) uint8_t memory_bytes[4096];
    for (unsigned byte = 0; byte < sizeof(memory_bytes); ++byte)
        memory_bytes[byte] = byte * 17 + 3;
    if (memory &&
        !gm_add_region(&(struct gm_region){
            .base = pa, .size = sizeof(memory_bytes), .kind = GM_RAM, .hva = memory_bytes}))
        abort();
    Ia64Instruction insns[TIER0_MAX_INSNS];
    fixture(insns, argv[1], first, count);
    struct vcpu *v[2] = {calloc(1, sizeof(struct vcpu)), calloc(1, sizeof(struct vcpu))};
    struct vcpu *ref = calloc(1, sizeof(*ref));
    void *allocation[2] = {hal_alloc_pages(16, HAL_PAGE_EXEC), hal_alloc_pages(16, HAL_PAGE_EXEC)};
    void *buffer = calloc(1, 65536);
    if (!v[0] || !v[1] || !ref || !allocation[0] || !allocation[1] || !buffer)
        abort();
    v[0]->s.cfm = 96;
    v[0]->s.pr = 1;
    v[0]->s.psr = ic ? IA64_PSR_IC : 0;
    v[0]->s.ip = insns[0].address;
    v[0]->s.slot = insns[0].slot;
    for (unsigned reg = 1; reg < 128; ++reg) {
        v[0]->s.gr[reg] = UINT64_C(0xfedcba9876543210) + reg;
        v[0]->s.nat[reg] = !system && !memory && reg % 7 == 0;
        if (system)
            v[0]->s.gr[reg] &= ~((UINT64_C(15) << 58) | (UINT64_C(0x3fff) << 38));
    }
    if (memory) {
        v[0]->s.gr[first + 8] = (virt ? va : pa) + 128;
        if (!strcmp(argv[1], "memory-update"))
            v[0]->s.gr[first + 9] = 0;
        if (virt) {
            v[0]->s.psr |= IA64_PSR_DT;
            v[0]->s.tr[1][0] = (struct ia64_tlb_entry){.va = va & ((UINT64_C(1) << 61) - 1),
                                                       .pte = pa | UINT64_C(0x7e1),
                                                       .itir = 48,
                                                       .page_shift = 12,
                                                       .valid = true};
        }
    }
    *v[1] = *ref = *v[0];
    struct tier0_region r[2] = {{.insns = insns, .count = count}, {.insns = insns, .count = count}};
    struct tier0_homes homes = {0};
    for (unsigned reg = 0; reg < 96; ++reg)
        homes.gr_index[reg] = 32 + reg;
    for (unsigned p = 16; p < 64; ++p)
        homes.pr_mask[p - 16] = UINT64_C(1) << p;
    struct tier0_execution e[2] = {{.region = &r[0]}, {.region = &r[1]}};
    struct tier0_dtc_entry dtc[2][TIER0_DTC_SLOTS] = {0};
    const uint64_t no_code = 0;
    struct tier0_code_span span[2] = {{.first = pa >> 12, .pages = 1, .bits = &no_code},
                                      {.first = pa >> 12, .pages = 1, .bits = &no_code}};
    void (*emit[2])(struct x86_buffer *, struct tier0_region *,
                    const struct ia64_state *) = {tier0_emit_baseline, tier0_emit};
    void (*run[2])(struct vcpu *, uint64_t, struct tier0_execution *,
                   const struct tier0_homes *) = {(void *)((uint8_t *)allocation[0] + offset),
                                                  (void *)((uint8_t *)allocation[1] + offset)};
    size_t bytes[2];
    for (unsigned variant = 0; variant < 2; ++variant) {
        if (memory && !ia64_physical_region(v[variant], pa, 8))
            abort();
        e[variant].dtc = dtc[variant];
        e[variant].span = &span[variant];
        if (virt)
            dtc[variant][(va >> 12) & (TIER0_DTC_SLOTS - 1)] =
                (struct tier0_dtc_entry){.owner = v[variant],
                                         .page = va >> 12,
                                         .mmu_dseq = v[variant]->mmu_dseq,
                                         .host = memory_bytes,
                                         .pa = pa,
                                         .rights = IA64_ACCESS_READ | IA64_ACCESS_WRITE,
                                         .psr_class = tier0_dtc_class(v[variant]->s.psr)};
        memset(r[variant].slots, 1, sizeof(r[variant].slots));
        struct x86_buffer b = {.data = (uint8_t *)allocation[variant] + offset,
                               .capacity = 65536 - offset};
        emit[variant](&b, &r[variant], &v[variant]->s);
        if (b.failed)
            abort();
        bytes[variant] = b.size;
        b = (struct x86_buffer){.capacity = 65536 - offset};
        emit[variant](&b, &r[variant], &v[variant]->s);
        if (b.failed || b.size != bytes[variant])
            abort();
        hal_code_set_executable(allocation[variant], 16);
    }
    unsigned active = budget < count ? (unsigned)budget : count;
    uint8_t original[sizeof(memory_bytes)], expected[sizeof(memory_bytes)];
    memcpy(original, memory_bytes, sizeof(original));
    for (unsigned at = 0; at < active; ++at) {
        ref->s.ip = insns[at].address;
        ref->s.slot = insns[at].slot;
        if (ia64_execute(ref, &insns[at]).kind != IA64_STEP_COMPLETE)
            abort();
        ++ref->retired;
        if (ic)
            ref->last_successful_bundle = insns[at].address;
    }
    if (active) {
        unsigned slot = insns[active - 1].slot + 1;
        ref->s.ip = insns[active - 1].address + (slot == 3 ? 16 : 0);
        ref->s.slot = slot % 3;
    }
    memcpy(expected, memory_bytes, sizeof(expected));
    for (unsigned variant = 0; variant < 2; ++variant) {
        memcpy(memory_bytes, original, sizeof(original));
        run[variant](v[variant], budget, &e[variant], &homes);
        if (memcmp(&v[variant]->s, &ref->s, sizeof(ref->s)) ||
            v[variant]->retired != ref->retired ||
            v[variant]->last_successful_bundle != ref->last_successful_bundle || e[variant].fault ||
            e[variant].stop || memcmp(memory_bytes, expected, sizeof(expected)))
            abort();
    }
    printf("{\"host_isa\":%u,\"samples\":[", hal_host_isa());
    bool comma = false;
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
                run[variant](v[variant], budget, &e[variant], &homes);
            uint64_t native_ns = ns() - begin;
            if (block) {
                printf("%s{\"variant\":\"%s\",\"block\":%u,\"position\":%u,\"bytes\":%zu,"
                       "\"emit_ns_per_region\":%.3f,\"run_ns_per_call\":%.3f",
                       comma ? "," : "", variant ? "current" : "baseline", block, at,
                       bytes[variant], (double)emission_ns / emits, (double)native_ns / calls);
                if (active)
                    printf(",\"run_ns_per_instruction\":%.3f", (double)native_ns / calls / active);
                printf("}");
                comma = true;
            }
        }
    if (memcmp(&v[0]->s, &v[1]->s, sizeof(v[0]->s)) || v[0]->retired != v[1]->retired ||
        v[0]->last_successful_bundle != v[1]->last_successful_bundle)
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
