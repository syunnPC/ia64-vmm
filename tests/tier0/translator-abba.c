/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Same-process comparison of complete baseline/current Tier0 translators
 * and encoders. Both native variants use the same code and state addresses.
 * Native state is checked before and after timed ABBA blocks. */
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

void tier0_emit_baseline(struct x86_buffer *, struct tier0_region *, const struct ia64_state *);
/* Test-only objcopy exports these otherwise local callback symbols. The
 * address operand is the only expected byte difference for an unchanged
 * emission: each translator names its own copy of the same callback. */
unsigned inline_requested_baseline(struct vcpu *, const Ia64Instruction *,
                                   struct tier0_execution *);
unsigned inline_requested_current(struct vcpu *, const Ia64Instruction *, struct tier0_execution *);

/* Check the actual private helpers, including the defensive zero-request
 * return that the sole-consumer generated path cannot normally reach.
 * Apply each version's remaining cold-stub IP update and compare all state.
 * Opt-in: this checks the request-advance change, not an arbitrary baseline. */
static unsigned check_request_advance(void)
{
    const Ia64Opcode ops[] = {
        IA64_OP_ADDS,    IA64_OP_MOVL,     IA64_OP_NOP,     IA64_OP_MF,
        IA64_OP_BR_COND, IA64_OP_BR_CLOOP, IA64_OP_BR_CALL, IA64_OP_BR_CALL_INDIRECT,
        IA64_OP_BR_RET};
    const unsigned positions[] = {0, 1, TIER0_MAX_INSNS - 1};
    Ia64Instruction insns[TIER0_MAX_INSNS] = {{0}};
    struct tier0_region r = {.insns = insns, .count = TIER0_MAX_INSNS};
    struct vcpu *v[2] = {calloc(1, sizeof(struct vcpu)), calloc(1, sizeof(struct vcpu))};
    if (!v[0] || !v[1])
        abort();
    unsigned checks = 0;
    for (unsigned op = 0; op < IA64VMM_ARRAY_SIZE(ops); ++op)
        for (unsigned pos = 0; pos < IA64VMM_ARRAY_SIZE(positions); ++pos)
            for (unsigned slot = 0; slot < 3; ++slot)
                for (unsigned request = 0; request <= VCPU_EXIT_DEBUG + 1u; ++request)
                    for (unsigned initial_stop = 0; initial_stop < 2; ++initial_stop) {
                        unsigned at = positions[pos], slots = ops[op] == IA64_OP_MOVL ? 2 : 1;
                        if (slots == 2 && slot != 1)
                            continue;
                        Ia64Instruction *i = &insns[at];
                        *i = (Ia64Instruction){.opcode = ops[op],
                                               .valid = true,
                                               .slot = slot,
                                               .address = UINT64_C(0x100000) + 16 * at};
                        r.slots[at] = slots;
                        memset(v[0], 0, sizeof(*v[0]));
                        v[0]->s.ip = UINT64_C(0xfedcba9876543200);
                        v[0]->s.slot = 2;
                        v[0]->s.pr = UINT64_C(0x123456789abcdef);
                        v[0]->retired = 1234;
                        v[0]->requested_exit = request;
                        *v[1] = *v[0];
                        struct tier0_execution e[2] = {
                            {.region = &r,
                             .stop = initial_stop,
                             .result = {.kind = IA64_STEP_BRANCH, .info = 0x1234},
                             .out = {.reason = VCPU_EXIT_HALT,
                                     .ip = 0x4560,
                                     .slot = 1,
                                     .info = 99,
                                     .detail = "sentinel"}}};
                        e[1] = e[0];
                        unsigned result[2] = {inline_requested_baseline(v[0], i, &e[0]),
                                              inline_requested_current(v[1], i, &e[1])};
                        bool branch = op >= 4, last = at + 1 == r.count;
                        uint64_t next_ip = i->address + (slot + slots == 3 ? 16 : 0);
                        unsigned next_slot = (slot + slots) % 3;
                        if (!branch && (v[1]->s.ip != next_ip || v[1]->s.slot != next_slot))
                            abort();
                        for (unsigned variant = 0; variant < 2; ++variant)
                            if (!(branch && last) && (!variant || branch)) {
                                v[variant]->s.ip = next_ip;
                                v[variant]->s.slot = next_slot;
                            }
                        if (result[0] != (request != 0) || result[1] != result[0] ||
                            memcmp(v[0], v[1], sizeof(*v[0])) || v[0]->requested_exit ||
                            e[0].stop != (request ? true : initial_stop) || e[1].stop != e[0].stop)
                            abort();
                        if (request && (e[0].out.reason != request - 1 ||
                                        e[0].out.ip != i->address || e[0].out.slot != slot ||
                                        e[0].out.info || strcmp(e[0].out.detail, "requested exit")))
                            abort();
                        if (strcmp(e[0].out.detail, e[1].out.detail))
                            abort();
                        /* Different object files may use separate literals. */
                        e[1].out.detail = e[0].out.detail;
                        if (memcmp(&e[0], &e[1], sizeof(e[0])))
                            abort();
                        ++checks;
                    }
    free(v[0]);
    free(v[1]);
    return checks;
}

static void normalize_callback(uint8_t *code, size_t size, uintptr_t callback)
{
    for (size_t at = 0; at + 10 <= size; ++at) {
        uintptr_t value;
        if (code[at] != 0x49 || code[at + 1] != 0xbb)
            continue;
        memcpy(&value, code + at + 2, sizeof(value));
        if (value == callback)
            memset(code + at + 2, 0, sizeof(value));
    }
}

static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t))
        abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

/* Run both bodies at the same virtual address. Copying and permission
 * changes are outside the timers; the installed body is warmed separately. */
static void install_code(void *allocation, const void *code, size_t size, unsigned offset)
{
    hal_code_set_writable(allocation, 16);
    memcpy((uint8_t *)allocation + offset, (const uint8_t *)code + offset, size);
    hal_code_set_executable(allocation, 16);
}

static void fixture(Ia64Instruction *insns, const char *name, unsigned first, unsigned count)
{
    const Ia64Opcode constants[] = {IA64_OP_SUB, IA64_OP_XOR, IA64_OP_ANDCM, IA64_OP_SUB_ONE};
    const Ia64Opcode identities[] = {IA64_OP_AND, IA64_OP_OR};
    const Ia64Opcode compares[] = {IA64_OP_CMP_EQ,  IA64_OP_CMP_LT,  IA64_OP_CMP_LTU,
                                   IA64_OP_CMP4_EQ, IA64_OP_CMP4_LT, IA64_OP_CMP4_LTU};
    const Ia64Opcode immediates[] = {IA64_OP_CMP_EQ_IMM,  IA64_OP_CMP_LT_IMM,
                                     IA64_OP_CMP_LTU_IMM, IA64_OP_CMP4_EQ_IMM,
                                     IA64_OP_CMP4_LT_IMM, IA64_OP_CMP4_LTU_IMM};
    const Ia64Opcode extensions[] = {IA64_OP_SXT1, IA64_OP_ZXT1, IA64_OP_SXT2,
                                     IA64_OP_ZXT2, IA64_OP_SXT4, IA64_OP_ZXT4};
    const Ia64Opcode shifts[] = {IA64_OP_SHL, IA64_OP_SHR, IA64_OP_SHRU};
    const int64_t values[] = {-128, -1, 0, 1, 127, INT32_MIN, INT32_MAX};
    for (unsigned at = 0; at < count; ++at) {
        Ia64Instruction *i = &insns[at];
        *i = (Ia64Instruction){.opcode = IA64_OP_ADD,
                               .valid = true,
                               .unit = IA64_UNIT_I,
                               .address = UINT64_C(0x100000) + 16 * (at / 3),
                               .slot = at % 3,
                               .operands.integer = {.destination = first + at % 8,
                                                    .source1 = first + 8,
                                                    .source2 = first + 9,
                                                    .predicate1 = 6,
                                                    .predicate2 = 7}};
        if (!strcmp(name, "branch-false")) {
            i->opcode = IA64_OP_BR_COND;
            i->unit = IA64_UNIT_B;
            i->qp = 1;
            i->operands.branch = (IA64BranchOperands){.displacement = 16};
        } else if (!strncmp(name, "forwarded-home-", 15)) {
            i->operands.integer.destination = first;
            i->operands.integer.source1 = first;
            i->operands.integer.source2 = first;
            if (!strcmp(name, "forwarded-home-adds")) {
                i->opcode = IA64_OP_ADDS;
                i->operands.integer.immediate = 1;
            } else if (!strcmp(name, "forwarded-home-first"))
                i->operands.integer.source2 = 9;
            else if (!strcmp(name, "forwarded-home-second")) {
                i->opcode = IA64_OP_SUB;
                i->operands.integer.source1 = 9;
            } else if (!strcmp(name, "forwarded-home-stacked-control"))
                i->operands.integer.source2 = first + 8;
            else if (!strcmp(name, "forwarded-home-extract")) {
                i->opcode = IA64_OP_EXTRU;
                i->operands.integer.immediate = 7 | 13 << 6;
            } else if (strcmp(name, "forwarded-home-both"))
                abort();
        } else if (!strncmp(name, "predicate-forward-", 18)) {
            unsigned pair = at / 2, producer = first + pair % 8;
            if (!(at & 1)) {
                i->opcode = IA64_OP_ADDS;
                i->operands.integer.destination = producer;
                i->operands.integer.source2 = first + 8;
                i->operands.integer.immediate = pair;
            } else {
                i->opcode = compares[pair % IA64VMM_ARRAY_SIZE(compares)];
                i->operands.integer.source1 = producer;
                if (!strcmp(name, "predicate-forward-second")) {
                    i->operands.integer.source1 = first + 9;
                    i->operands.integer.source2 = producer;
                } else if (!strcmp(name, "predicate-forward-both"))
                    i->operands.integer.source2 = producer;
                else if (!strcmp(name, "predicate-forward-immediate")) {
                    i->opcode = immediates[pair % IA64VMM_ARRAY_SIZE(immediates)];
                    i->operands.integer.source2 = producer;
                    i->operands.integer.immediate = values[pair % IA64VMM_ARRAY_SIZE(values)];
                } else if (!strcmp(name, "predicate-forward-tbit") ||
                           !strcmp(name, "predicate-forward-tnat")) {
                    bool tnat = !strcmp(name, "predicate-forward-tnat");
                    i->opcode = tnat ? (pair & 1 ? IA64_OP_TNAT_Z : IA64_OP_TNAT_NZ)
                                     : (pair & 1 ? IA64_OP_TBIT_Z : IA64_OP_TBIT_NZ);
                    i->operands.integer.source2 = producer;
                    i->operands.integer.immediate = pair;
                } else if (strcmp(name, "predicate-forward-first") &&
                           strcmp(name, "predicate-forward-nat"))
                    abort();
            }
        } else if (!strcmp(name, "same-constant") || !strcmp(name, "same-identity") ||
                   !strcmp(name, "same-forward")) {
            i->opcode = !strcmp(name, "same-identity")
                            ? identities[at % IA64VMM_ARRAY_SIZE(identities)]
                            : constants[at % IA64VMM_ARRAY_SIZE(constants)];
            i->operands.integer.source2 = i->operands.integer.source1;
            if (!strcmp(name, "same-forward"))
                i->operands.integer.source1 = i->operands.integer.source2 = first + (at + 7) % 8;
        } else if (!strcmp(name, "cmp-reg") || !strcmp(name, "cmp-same") ||
                   !strcmp(name, "cmp-same-zero") || !strcmp(name, "cmp-same-nat")) {
            i->opcode = compares[at % IA64VMM_ARRAY_SIZE(compares)];
            if (!strcmp(name, "cmp-same-zero"))
                i->operands.integer.source1 = i->operands.integer.source2 = 0;
            else if (strcmp(name, "cmp-reg"))
                i->operands.integer.source2 = i->operands.integer.source1;
        } else if (!strcmp(name, "cmp-imm") || !strcmp(name, "cmp-wide")) {
            i->opcode = immediates[at % IA64VMM_ARRAY_SIZE(immediates)];
            i->operands.integer.immediate = !strcmp(name, "cmp-wide")
                                                ? (int64_t)(UINT64_C(0xfedcba9876543210) + at)
                                                : values[at % IA64VMM_ARRAY_SIZE(values)];
        } else if (!strcmp(name, "tbit")) {
            i->opcode = at & 1 ? IA64_OP_TBIT_Z : IA64_OP_TBIT_NZ;
            i->operands.integer.immediate = at % 64;
        } else if (!strcmp(name, "tnat"))
            i->opcode = at & 1 ? IA64_OP_TNAT_Z : IA64_OP_TNAT_NZ;
        else if (!strcmp(name, "extensions") || !strcmp(name, "extension-chain")) {
            i->opcode = extensions[at % IA64VMM_ARRAY_SIZE(extensions)];
            if (!strcmp(name, "extension-chain"))
                i->operands.integer.source2 = first + (at + 7) % 8;
        } else if (!strcmp(name, "variable-shifts"))
            i->opcode = shifts[at % IA64VMM_ARRAY_SIZE(shifts)];
        else if (!strcmp(name, "helper"))
            i->opcode = IA64_OP_POPCNT;
        else if (strcmp(name, "control"))
            abort();
    }
}

int main(int argc, char **argv)
{
    if (argc != 11)
        return 2;
    unsigned first = (unsigned)strtoul(argv[2], NULL, 0),
             mask = (unsigned)strtoul(argv[3], NULL, 0);
    unsigned blocks = (unsigned)strtoul(argv[4], NULL, 0),
             emits = (unsigned)strtoul(argv[5], NULL, 0);
    unsigned calls = (unsigned)strtoul(argv[6], NULL, 0),
             count = (unsigned)strtoul(argv[7], NULL, 0);
    unsigned offset = (unsigned)strtoul(argv[8], NULL, 0),
             budget = (unsigned)strtoul(argv[9], NULL, 0);
    unsigned request_checks = strtoul(argv[10], NULL, 0) ? check_request_advance() : 0;
    if (first > 118 || !blocks || !emits || !calls || !count || count > TIER0_MAX_INSNS ||
        offset > 48 || !budget || budget > count)
        return 2;
    hal_linux_host_isa_mask(mask);
    Ia64Instruction insns[TIER0_MAX_INSNS];
    fixture(insns, argv[1], first, count);
    struct vcpu *v[2] = {calloc(1, sizeof(struct vcpu)), calloc(1, sizeof(struct vcpu))};
    struct vcpu *ref = calloc(1, sizeof(*ref));
    struct vcpu *active = calloc(1, sizeof(*active));
    void *code[2] = {calloc(1, 65536), calloc(1, 65536)};
    void *allocation = hal_alloc_pages(16, HAL_PAGE_EXEC);
    void *buffer = calloc(1, 65536);
    if (!v[0] || !v[1] || !ref || !active || !code[0] || !code[1] || !allocation || !buffer)
        abort();
    v[0]->s.cfm = 96;
    v[0]->s.pr = 1;
    for (unsigned reg = 1; reg < 128; ++reg)
        v[0]->s.gr[reg] = UINT64_C(0xfedcba9876543210) + reg;
    if (!strcmp(argv[1], "cmp-same-nat") || !strcmp(argv[1], "predicate-forward-nat"))
        v[0]->s.nat[first + 8] = 1;
    *v[1] = *ref = *v[0];
    struct tier0_region r[2] = {{.valid = true, .insns = insns, .count = count},
                                {.valid = true, .insns = insns, .count = count}};
    struct tier0_homes homes = {0};
    for (unsigned reg = 0; reg < 96; ++reg)
        homes.gr_index[reg] = 32 + reg;
    for (unsigned p = 16; p < 64; ++p)
        homes.pr_mask[p - 16] = UINT64_C(1) << p;
    struct tier0_execution e[2] = {{.region = &r[0]}, {.region = &r[1]}};
    struct tier0_execution active_e;
    void (*emit[2])(struct x86_buffer *, struct tier0_region *,
                    const struct ia64_state *) = {tier0_emit_baseline, tier0_emit};
    void (*run)(struct vcpu *, uint64_t, struct tier0_execution *, const struct tier0_homes *) =
        (void *)((uint8_t *)allocation + offset);
    size_t bytes[2];
    for (unsigned variant = 0; variant < 2; ++variant) {
        memset(r[variant].slots, 1, sizeof(r[variant].slots));
        struct x86_buffer b = {.data = (uint8_t *)allocation + offset, .capacity = 65536 - offset};
        emit[variant](&b, &r[variant], &v[variant]->s);
        if (b.failed)
            abort();
        bytes[variant] = b.size;
        memcpy((uint8_t *)code[variant] + offset, (uint8_t *)allocation + offset, b.size);
    }
    bool same_bytes = false;
    if (bytes[0] == bytes[1]) {
        uint8_t *copy[2] = {malloc(bytes[0]), malloc(bytes[1])};
        if (!copy[0] || !copy[1])
            abort();
        for (unsigned variant = 0; variant < 2; ++variant)
            memcpy(copy[variant], (uint8_t *)code[variant] + offset, bytes[variant]);
        normalize_callback(copy[0], bytes[0], (uintptr_t)inline_requested_baseline);
        normalize_callback(copy[1], bytes[1], (uintptr_t)inline_requested_current);
        same_bytes = !memcmp(copy[0], copy[1], bytes[0]);
        free(copy[0]);
        free(copy[1]);
    }
    for (unsigned at = 0; at < budget; ++at) {
        ref->s.ip = insns[at].address;
        ref->s.slot = insns[at].slot;
        if (ia64_execute(ref, &insns[at]).kind != IA64_STEP_COMPLETE)
            abort();
        ++ref->retired;
    }
    unsigned next = insns[budget - 1].slot + 1;
    ref->s.ip = insns[budget - 1].address + (next == 3 ? 16 : 0);
    ref->s.slot = next % 3;
    for (unsigned variant = 0; variant < 2; ++variant) {
        install_code(allocation, code[variant], bytes[variant], offset);
        *active = *v[variant];
        active_e = e[variant];
        run(active, budget, &active_e, &homes);
        if (memcmp(&active->s, &ref->s, sizeof(ref->s)) || active->retired != ref->retired)
            abort();
        *v[variant] = *active;
        e[variant] = active_e;
    }
    enum { NATIVE_WARMUP_CALLS = 100 };
    printf(
        "{\"host_isa\":%u,\"same_bytes_except_callback_address\":%s,\"native_warmup_calls_per_sample\":%u,\"request_callback_checks\":%u,\"samples\":[",
        hal_host_isa(), same_bytes ? "true" : "false", NATIVE_WARMUP_CALLS, request_checks);
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
            install_code(allocation, code[variant], bytes[variant], offset);
            /* Keep the native state and execution-record addresses fixed
             * too; separate vCPU allocations can have different alignment. */
            *active = *v[variant];
            active_e = e[variant];
            for (unsigned n = 0; n < NATIVE_WARMUP_CALLS; ++n)
                run(active, budget, &active_e, &homes);
            begin = ns();
            for (unsigned n = 0; n < calls; ++n)
                run(active, budget, &active_e, &homes);
            uint64_t native_ns = ns() - begin;
            *v[variant] = *active;
            e[variant] = active_e;
            if (block) {
                printf("%s{\"variant\":\"%s\",\"block\":%u,\"position\":%u,\"bytes\":%zu,"
                       "\"emit_ns_per_region\":%.3f,\"run_ns_per_instruction\":%.3f}",
                       comma ? "," : "", variant ? "current" : "baseline", block, at,
                       bytes[variant], (double)emission_ns / emits,
                       (double)native_ns / calls / budget);
                comma = true;
            }
        }
    if (memcmp(&v[0]->s, &v[1]->s, sizeof(v[0]->s)) || v[0]->retired != v[1]->retired)
        abort();
    printf("],\"checksum\":\"%016" PRIx64 "\"}\n", v[0]->retired ^ v[0]->s.gr[first] ^ v[0]->s.pr);
    for (unsigned variant = 0; variant < 2; ++variant) {
        free(code[variant]);
        free(v[variant]);
    }
    hal_free_pages(allocation, 16);
    free(buffer);
    free(ref);
    free(active);
    return 0;
}
