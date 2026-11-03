/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "engine/tier0/internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"
#include "core/log_internal.h"
#include "ia64/rse.h"
#include "ia64/fp.h"
#include "ia64vmm/host_isa.h"
#include "tests/hal/hal_linux.h"
#include <stdio.h>
#include <sys/mman.h>

#define BASE UINT64_C(0x100000)
#define DATA (BASE + 0x10000)
#define DEVICE (BASE + 0x30000)
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            LOGE("%s:%u: %s", __FILE__, __LINE__, #c);                                             \
            hal_panic("Tier0 test failed");                                                        \
        }                                                                                          \
    } while (0)
static unsigned checks;
static uint64_t rng = UINT64_C(0x4fe92892b431aa17);
static uint8_t ram[0x20000], device_bytes[4096];
static _Alignas(16) uint8_t validator_collision[4096], decoded_collision[4096];
static _Alignas(16) uint8_t alignment_backing[64];
static unsigned reads, writes;
static unsigned breaks;
static bool request_exit;
static unsigned raise_on_write;
static bool arena_invalidate_on_write;
static void arena_write_hook(struct vcpu *v);
static uint64_t random64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}
static uint64_t device_read(void *opaque, struct vcpu *v, uint64_t offset, unsigned size)
{
    (void)opaque;
    (void)offset;
    (void)size;
    ++reads;
    if (request_exit)
        vcpu_request_exit(v, VCPU_EXIT_HALT);
    return UINT64_C(0xfedcba9876543210);
}
static void device_write(void *opaque, struct vcpu *v, uint64_t offset, uint64_t value,
                         unsigned size)
{
    (void)opaque;
    (void)offset;
    (void)value;
    (void)size;
    ++writes;
    if (arena_invalidate_on_write)
        arena_write_hook(v);
    if (request_exit)
        vcpu_request_exit(v, VCPU_EXIT_HALT);
    if (raise_on_write)
        vcpu_raise_external(v, raise_on_write);
}
static const struct gm_mmio_ops ops = {device_read, device_write};
static bool break_hook(struct vcpu *v, uint64_t imm, uint64_t ip)
{
    (void)imm;
    (void)ip;
    ++breaks;
    vcpu_request_exit(v, VCPU_EXIT_HALT);
    return true;
}
static void encoded(const uint8_t *expected, size_t n, const struct x86_buffer *b)
{
    CHECK(!b->failed && b->size == n && !memcmp(expected, b->data, n));
}
/* Intel SDM Vol.2: VEX.W1, L=0. Distinct high registers exercise R/B/vvvv. */
static void test_host_isa_encoder(void)
{
    uint8_t bytes[64];
    struct x86_buffer b = {.data = bytes, .capacity = sizeof(bytes)};
    x86_shiftx(&b, 4, X_R8, X_R8, X_R15);
    x86_shiftx(&b, 5, X_R8, X_R8, X_R15);
    x86_shiftx(&b, 7, X_R8, X_R8, X_R15);
    x86_andn(&b, X_R8, X_R8, X_R15);
    x86_bextr(&b, X_R8, X_R8, X_R15);
    x86_lzcnt(&b, X_R8, X_R15);
    x86_rorx(&b, X_R8, X_R8, 17);
    const uint8_t expected[] = {0xc4, 0x42, 0x81, 0xf7, 0xc0, 0xc4, 0x42, 0x83, 0xf7,
                                0xc0, 0xc4, 0x42, 0x82, 0xf7, 0xc0, 0xc4, 0x42, 0xb8,
                                0xf2, 0xc7, 0xc4, 0x42, 0x80, 0xf7, 0xc0, 0xf3, 0x4d,
                                0x0f, 0xbd, 0xc7, 0xc4, 0x43, 0xfb, 0xf0, 0xc0, 17};
    encoded(expected, sizeof(expected), &b);
}
static void test_encoder(void)
{
    uint8_t bytes[128];
    struct x86_buffer b = {.data = bytes, .capacity = sizeof(bytes)};
    x86_push(&b, X_R12);
    x86_pop(&b, X_R15);
    x86_mov(&b, X_R12, X_RDI);
    x86_load(&b, X_RAX, X_R12, 0x12345678, 8);
    x86_store(&b, X_RSP, -4, X_RSI, 1);
    const uint8_t expected[] = {0x41, 0x54, 0x41, 0x5f, 0x49, 0x89, 0xfc, 0x49, 0x8b, 0x84,
                                0x24, 0x78, 0x56, 0x34, 0x12, 0x40, 0x88, 0x74, 0x24, 0xfc};
    encoded(expected, sizeof(expected), &b);
    b.size = 0;
    x86_load(&b, X_R10, X_RBP, 8, 1);
    x86_load(&b, X_RCX, X_R9, -16, 2);
    x86_store(&b, X_R15, 0, X_R8, 2);
    x86_alu(&b, 0x39, X_RAX, X_RDX, true);
    x86_shift(&b, 7, X_RAX, 63, false);
    x86_setcc(&b, X_LE, X_RCX);
    const uint8_t expected2[] = {0x44, 0x0f, 0xb6, 0x55, 8,    0x41, 0x0f, 0xb7,
                                 0x49, 0xf0, 0x66, 0x45, 0x89, 0x07, 0x39, 0xd0,
                                 0x48, 0xc1, 0xf8, 0x3f, 0x0f, 0x9e, 0xc1};
    encoded(expected2, sizeof(expected2), &b);
    /* Frame-relative homes: [R12 + R11*scale + disp] checked
     * against objdump: mov rax,0x10(%r12,%r11,8); movzbl 0x20(%r12,%r11,1),
     * %edx; mov %rax,0x30(%r12,%r11,8); mov %dl,0x40(%r12,%r11,1). */
    b.size = 0;
    x86_load_index(&b, X_RAX, X_R12, X_R11, 3, 0x10, 8);
    x86_load_index(&b, X_RDX, X_R12, X_R11, 0, 0x20, 1);
    x86_store_index(&b, X_R12, X_R11, 3, 0x30, X_RAX, 8);
    x86_store_index(&b, X_R12, X_R11, 0, 0x40, X_RDX, 1);
    const uint8_t expected_index[] = {0x4b, 0x8b, 0x44, 0xdc, 0x10, 0x43, 0x0f,
                                      0xb6, 0x54, 0x1c, 0x20, 0x4b, 0x89, 0x44,
                                      0xdc, 0x30, 0x43, 0x88, 0x54, 0x1c, 0x40};
    encoded(expected_index, sizeof(expected_index), &b);
    b.size = 0;
    size_t jump = x86_jcc(&b, X_E);
    x86_u8(&b, 0x90);
    x86_patch(&b, jump, b.size);
    size_t back = x86_jmp(&b);
    x86_patch(&b, back, 0);
    const uint8_t expected3[] = {0x0f, 0x84, 1, 0, 0, 0, 0x90, 0xe9, 0xf4, 0xff, 0xff, 0xff};
    encoded(expected3, sizeof(expected3), &b);
    b.size = 0;
    b.capacity = 2;
    x86_mov_imm(&b, X_RAX, 42);
    CHECK(b.failed);
    b.failed = false;
    x86_patch(&b, 100, 0);
    CHECK(b.failed);
    void *code = hal_alloc_pages(1, HAL_PAGE_EXEC);
    CHECK(code != NULL);
    b = (struct x86_buffer){.data = code, .capacity = 4096};
    x86_mov_imm(&b, X_RAX, UINT64_C(0xaabbccdd11223344));
    x86_u8(&b, 0xc3);
    hal_code_set_executable(code, 1);
    CHECK(((uint64_t (*)(void))code)() == UINT64_C(0xaabbccdd11223344));
    hal_code_set_writable(code, 1);
    b.size = 0;
    x86_mov_imm(&b, X_RAX, 99);
    x86_u8(&b, 0xc3);
    hal_code_set_executable(code, 1);
    CHECK(((uint64_t (*)(void))code)() == 99);
    hal_free_pages(code, 1);
}
static void randomized(struct vcpu *v)
{
    vcpu_reset(v, NULL);
    v->s.ip = BASE;
    v->s.slot = 1;
    for (unsigned r = 0; r < 128; ++r) {
        v->s.gr[r] = random64();
        v->s.nat[r] = (random64() & 3) == 0;
    }
    for (unsigned r = 0; r < 8; ++r)
        v->s.br[r] = random64();
    unsigned sor = (random64() % 13) * 8;
    v->s.rse_bof = random64() % IA64_N_STACKED_PHYS;
    v->s.cfm = 96 | (UINT64_C(96) << 7) | ((uint64_t)(sor / 8) << 14) |
               ((sor ? random64() % sor : 0) << 18) | ((random64() % 96) << 25) |
               ((random64() % 48) << 32);
    v->s.pr = random64() | 1;
}
static void expect_advance(struct vcpu *v, const Ia64Instruction *i, const struct ia64_result *r)
{
    if (r->kind == IA64_STEP_COMPLETE || r->kind == IA64_STEP_BRANCH) {
        ++v->retired;
        v->fault.kind = IA64_FAULT_NONE;
        if (r->kind == IA64_STEP_BRANCH) {
            v->s.ip = r->info;
            v->s.slot = 0;
        } else {
            v->s.ip = i->address;
            v->s.slot = i->slot + (i->opcode == IA64_OP_MOVL ? 2 : 1);
            if (v->s.slot == 3) {
                v->s.ip += 16;
                v->s.slot = 0;
            }
        }
    }
}
static void equivalence(struct vcpu *actual, struct vcpu *reference, const Ia64Instruction *i)
{
    *reference = *actual;
    struct ia64_result expected = ia64_execute(reference, i);
    expect_advance(reference, i, &expected);
    struct ia64_result got = tier0_test_instruction(actual, i);
    if (memcmp(&actual->s, &reference->s, sizeof(actual->s))) {
        LOGE("opcode=%s qp=%u cfm=%llx a=%llx b=%llx imm=%llx", ia64_opcode_info(i->opcode)->name,
             i->qp, (unsigned long long)reference->s.cfm,
             (unsigned long long)ia64_gr_read(&reference->s, i->operands.integer.source1),
             (unsigned long long)ia64_gr_read(&reference->s, i->operands.integer.source2),
             (unsigned long long)i->operands.integer.immediate);
    }
    CHECK(!memcmp(&actual->s, &reference->s, sizeof(actual->s)));
    CHECK(actual->retired == reference->retired);
    CHECK(got.kind == expected.kind && got.fault == expected.fault &&
          (got.kind == IA64_STEP_COMPLETE || got.info == expected.info));
}
/* SDM Vol.3 shrp: identical sources rotate; unequal sources concatenate.
 * Exercise all counts, including zero, aliases, NaTs and false predicates. */
static void test_host_isa_rotate(struct vcpu *v, struct vcpu *ref)
{
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush();
        hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned trial = 0; trial < 1024; ++trial) {
            randomized(v);
            Ia64Instruction i = {.opcode = IA64_OP_SHRP_IMM,
                                 .valid = true,
                                 .unit = IA64_UNIT_I,
                                 .address = BASE,
                                 .slot = 1,
                                 .qp = trial & 1,
                                 .operands.integer = {.destination = trial & 2 ? 9 : 8,
                                                      .source1 = 9,
                                                      .source2 = trial & 4 ? 10 : 9,
                                                      .immediate = trial % 64}};
            ia64_gr_write(&v->s, 9, trial & 8 ? random64() : 0, trial & 16);
            equivalence(v, ref, &i);
        }
    }
    tier0_flush();
    hal_linux_host_isa_mask(old);
}
static void test_equivalence(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode alu[] = {IA64_OP_ADD,
                              IA64_OP_ADD_ONE,
                              IA64_OP_SUB,
                              IA64_OP_SUB_ONE,
                              IA64_OP_SUB_IMM,
                              IA64_OP_AND,
                              IA64_OP_OR,
                              IA64_OP_XOR,
                              IA64_OP_ANDCM,
                              IA64_OP_AND_IMM,
                              IA64_OP_ANDCM_IMM,
                              IA64_OP_OR_IMM,
                              IA64_OP_XOR_IMM,
                              IA64_OP_ADDS,
                              IA64_OP_ADDL,
                              IA64_OP_SHLADD,
                              IA64_OP_SHL,
                              IA64_OP_SHR,
                              IA64_OP_SHRU,
                              IA64_OP_SHL_IMM,
                              IA64_OP_SHR_IMM,
                              IA64_OP_SHRU_IMM,
                              IA64_OP_EXTR,
                              IA64_OP_EXTRU,
                              IA64_OP_DEPZ,
                              IA64_OP_DEPZ_IMM,
                              IA64_OP_ZXT1,
                              IA64_OP_ZXT2,
                              IA64_OP_ZXT4,
                              IA64_OP_SXT1,
                              IA64_OP_SXT2,
                              IA64_OP_SXT4,
                              IA64_OP_MOVL,
                              IA64_OP_MOV_IP,
                              IA64_OP_MOV_CURRENT_IP,
                              IA64_OP_NOP};
    struct tier0_stats before, after;
    tier0_get_stats(&before);
    for (unsigned n = 0; n < sizeof(alu) / sizeof(*alu); ++n)
        for (unsigned trial = 0; trial < 128; ++trial) {
            randomized(v);
            Ia64Instruction i = {.opcode = alu[n],
                                 .valid = true,
                                 .unit = IA64_UNIT_I,
                                 .address = BASE,
                                 .slot = 1,
                                 .qp = trial % 64,
                                 .operands.integer = {.destination = 8,
                                                      .source1 = 9,
                                                      .source2 = 10,
                                                      .immediate = (int64_t)random64()}};
            if (trial % 3 == 0) {
                i.operands.integer.destination = 48;
                i.operands.integer.source1 = 48;
                i.operands.integer.source2 = 49;
            }
            if (trial % 5 == 0)
                i.operands.integer.source2 = 0;
            if (trial % 7 == 0)
                i.operands.integer.destination = 0;
            if (trial % 11 == 0)
                v->s.cfm &= ~UINT64_C(127); /* illegal stacked destination */
            if (i.opcode == IA64_OP_SHLADD)
                i.operands.integer.immediate = 1 + trial % 4;
            if (i.opcode == IA64_OP_EXTR || i.opcode == IA64_OP_EXTRU || i.opcode == IA64_OP_DEPZ ||
                i.opcode == IA64_OP_DEPZ_IMM)
                i.operands.integer.immediate =
                    (trial % 64) | ((1 + trial % 64) << 6) | ((random64() & 255) << 13);
            if (i.opcode == IA64_OP_SHL || i.opcode == IA64_OP_SHR || i.opcode == IA64_OP_SHRU) {
                uint64_t counts[] = {0, 1, 31, 32, 63, 64, 65, UINT64_MAX};
                ia64_gr_write(&v->s, i.operands.integer.source1, counts[trial % 8], trial % 2);
            }
            if (trial % 19 == 0)
                i.encoding_class = IA64_ENCODING_RESERVED;
            if (trial % 23 == 0)
                i.placement_illegal = true;
            if (trial % 29 == 0)
                i.reserved_field = true;
            equivalence(v, ref, &i);
        }
    /* Use the canonical opcode metadata, including immediate and cmp4 forms. */
    enum relation {
        REL_EQ,
        REL_NE,
        REL_LT,
        REL_LE,
        REL_GT,
        REL_GE,
        REL_LTU,
        REL_LEU,
        REL_GTU,
        REL_GEU
    };
    struct compare_desc {
        bool valid, word, immediate;
        enum relation relation;
        Ia64PredicateUpdate update;
    };
    static const struct compare_desc compares[IA64_OP_COUNT] = {
#include "ia64/decode/compare.inc"
    };
    for (unsigned op = 0; op < IA64_OP_COUNT; ++op)
        if (compares[op].valid)
            for (unsigned trial = 0; trial < 96; ++trial) {
                randomized(v);
                Ia64Instruction i = {
                    .opcode = op,
                    .valid = true,
                    .unit = IA64_UNIT_I,
                    .address = BASE,
                    .slot = 1,
                    .qp = trial % 64,
                    .compare_unc = trial % 2,
                    .operands.integer = {.source1 = 49,
                                         .source2 = 50,
                                         .predicate1 = trial % 64,
                                         .predicate2 = (trial + 1) % 64,
                                         .immediate = (int64_t)(trial % 2 ? random64() : 0)}};
                if (trial % 3 == 0) {
                    ia64_gr_write(&v->s, 49, (UINT64_C(1) << 63) + (trial & 3), false);
                    ia64_gr_write(&v->s, 50, (UINT64_C(1) << 31) + (trial & 3), false);
                }
                if (trial % 7 == 0)
                    i.operands.integer.predicate2 = i.operands.integer.predicate1;
                equivalence(v, ref, &i);
            }
    tier0_get_stats(&after);
    for (unsigned n = 0; IA64VMM_TIER0_STATS && n < sizeof(alu) / sizeof(*alu); ++n)
        CHECK(after.inlined[alu[n]] > before.inlined[alu[n]]);
    for (unsigned n = 0; n < IA64_OP_COUNT; ++n)
        if (IA64VMM_TIER0_STATS && compares[n].valid &&
            compares[n].update == IA64_PRED_UPDATE_NORMAL)
            CHECK(after.inlined[n] > before.inlined[n]);
    for (unsigned trial = 0; trial < 128; ++trial) {
        randomized(v);
        v->s.slot = 2;
        if (trial & 1)
            v->s.ip |= UINT64_C(1) << 63;
        Ia64Instruction i = {.opcode = IA64_OP_BR_COND,
                             .valid = true,
                             .unit = IA64_UNIT_B,
                             .address = v->s.ip,
                             .slot = 2,
                             .qp = trial % 64,
                             .operands.branch.displacement = (int64_t)(random64() & ~UINT64_C(15))};
        equivalence(v, ref, &i);
    }
}
static void test_memory_fast(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode loads[] = {IA64_OP_LD1, IA64_OP_LD2, IA64_OP_LD4, IA64_OP_LD8};
    const Ia64Opcode stores[] = {IA64_OP_ST1, IA64_OP_ST2, IA64_OP_ST4, IA64_OP_ST8};
    struct tier0_stats before, after;
    tier0_get_stats(&before);
    for (unsigned width = 0; width < 4; ++width)
        for (unsigned store = 0; store < 2; ++store)
            for (unsigned trial = 0; trial < 96; ++trial) {
                randomized(v);
                unsigned size = 1u << width;
                uint64_t address = DATA + 128;
                if (trial % 7 == 0)
                    address |= UINT64_C(1) << 63;
                if (trial % 11 == 0)
                    address += 1;
                ia64_gr_write(&v->s, 10, address, trial % 9 == 0);
                (void)ia64_physical_region(v, DATA, size);
                Ia64Instruction i = {
                    .opcode = store ? stores[width] : loads[width],
                    .valid = true,
                    .address = BASE,
                    .slot = 1,
                    .unit = IA64_UNIT_M,
                    .qp = trial % 64,
                    .mem_acquire = !store && (trial & 1),
                    .mem_release = store && (trial & 1),
                    .operands.memory = {.destination = 8, .source = 9, .base = 10}};
                uint8_t original[8], expected[8];
                for (unsigned j = 0; j < 8; ++j)
                    original[j] = ram[DATA - BASE + 128 + j] = random64();
                *ref = *v;
                struct ia64_result r = ia64_execute(ref, &i);
                expect_advance(ref, &i, &r);
                memcpy(expected, ram + DATA - BASE + 128, 8);
                memcpy(ram + DATA - BASE + 128, original, 8);
                struct ia64_result got = tier0_test_instruction(v, &i);
                CHECK(got.kind == r.kind && got.fault == r.fault);
                CHECK(!memcmp(&v->s, &ref->s, sizeof(v->s)) && v->retired == ref->retired);
                CHECK(!memcmp(expected, ram + DATA - BASE + 128, 8));
            }
    tier0_get_stats(&after);
    for (unsigned i = 0; IA64VMM_TIER0_STATS && i < 4; ++i) {
        CHECK(after.inlined[loads[i]] > before.inlined[loads[i]]);
        CHECK(after.inlined[stores[i]] > before.inlined[stores[i]]);
    }
}
static void bundle(uint64_t ip, unsigned t, uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t low = t | (a << 5) | (b << 46), high = (b >> 18) | (c << 23);
    uint8_t bytes[16];
    for (unsigned n = 0; n < 8; ++n) {
        bytes[n] = low >> (8 * n);
        bytes[n + 8] = high >> (8 * n);
    }
    CHECK(gm_write(ip, bytes, 16));
}
static const uint64_t nop = UINT64_C(1) << 27;
static void restart(struct vcpu *v, enum engine_kind engine, uint64_t ip);
static void test_os_dispatch(struct vcpu *v)
{
    const enum engine_kind modes[] = {ENGINE_TIER0, ENGINE_LOCKSTEP};
    for (unsigned k = 0; k < 2; ++k) {
        code_cache_flush();
        restart(v, modes[k], BASE);
        bundle(BASE, 0, nop, nop, nop);
        v->s.psr = IA64_PSR_I | IA64_PSR_IC;
        v->s.cr[CR_ITV] = 150; /* unmasked, no pending interrupt */
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
        CHECK(tier0_page_has_code(BASE) && v->retired == 3);
        /* A masked IRR also permits generated execution. */
        code_cache_flush();
        restart(v, modes[k], BASE);
        v->s.psr = IA64_PSR_I | IA64_PSR_IC;
        v->s.cr[CR_ITV] = 150;
        vcpu_raise_external(v, 150);
        v->s.cr[CR_TPR] = 1u << 16;
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
        CHECK(tier0_page_has_code(BASE) && v->retired == 3);
    }
}
static void test_hash_reuse(struct vcpu *v)
{
    code_cache_flush();
    restart(v, ENGINE_TIER0, BASE);
    /* Many entries sharing one IP exercise collision chains and BOF.
     * An ending branch keeps each key to a small region. */
    bundle(BASE, 0x10, nop, nop, (UINT64_C(4) << 37) | 1);
    struct tier0_stats before, after;
    for (unsigned pass = 0; pass < 2; ++pass) {
        if (pass)
            tier0_get_stats(&before);
        for (unsigned n = 0; n < 96; ++n) {
            v->s.ip = BASE;
            v->s.slot = 0;
            v->s.rse_bof = n;
            CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
            CHECK(v->s.ip == BASE + 16 && v->s.slot == 0);
        }
    }
    tier0_get_stats(&after);
    if (IA64VMM_TIER0_STATS) {
        CHECK(after.translations == before.translations && after.hits == before.hits + 96);
        CHECK(after.lookup_probes - before.lookup_probes < 384);
    }
    code_notify_cpu_write(BASE, 16);
    CHECK(!tier0_page_has_code(BASE));
    v->s.ip = BASE;
    v->s.slot = 0;
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && tier0_page_has_code(BASE));
}
static uint64_t load(unsigned width, unsigned dest, unsigned base)
{
    return (UINT64_C(4) << 37) | ((uint64_t)width << 30) | (dest << 6) | (base << 20);
}
static uint64_t store(unsigned width, unsigned source, unsigned base)
{
    return (UINT64_C(4) << 37) | ((uint64_t)(0x30 + width) << 30) | (source << 13) | (base << 20);
}
static uint64_t add(unsigned dest, unsigned a, unsigned b)
{
    return (UINT64_C(8) << 37) | (dest << 6) | (a << 13) | (b << 20);
}
static void restart(struct vcpu *v, enum engine_kind engine, uint64_t ip)
{
    vcpu_reset(v, &(struct ia64_entry_regs){.ip = ip, .rsc = 3});
    v->cfg.engine = engine;
}
static void test_regions(struct vcpu *v)
{
    /* Diagnostic regions have distinct keys: an engine switch cannot reuse
     * an unchecked region containing an opaque platform break. */
    bundle(BASE, 1, nop, 0, nop);
    restart(v, ENGINE_TIER0, BASE);
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
    restart(v, ENGINE_LOCKSTEP, BASE);
    engine_set_break_hook(break_hook);
    CHECK(engine_run(v, 2).reason == VCPU_EXIT_HALT && breaks == 1 && v->retired == 2 &&
          v->s.slot == 2);
    engine_set_break_hook(NULL);
    /* A predicated-off store must neither capture nor replay its address. */
    bundle(BASE, 1, store(3, 9, 10) | 5, add(8, 8, 9), nop);
    restart(v, ENGINE_LOCKSTEP, BASE);
    v->s.psr = IA64_PSR_IC;
    v->s.gr[10] = DATA + 4096;
    v->s.gr[9] = 42;
    uint64_t untouched = 123;
    CHECK(gm_write(DATA + 4096, &untouched, 8));
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->s.gr[8] == 42);
    CHECK(!memcmp(ram + DATA + 4096 - BASE, &untouched, 8));
    /* Suppression retirement remains interpreter-owned, with checked replay. */
    struct tier0_stats boundary_before, boundary_after;
    tier0_get_stats(&boundary_before);
    restart(v, ENGINE_LOCKSTEP, BASE);
    v->s.psr = UINT64_C(1) << 43;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
    tier0_get_stats(&boundary_after);
    CHECK(boundary_after.unchecked_instructions == boundary_before.unchecked_instructions);
    CHECK(boundary_after.checked_regions == boundary_before.checked_regions + 1);
    CHECK(boundary_after.interpreter_instructions[TIER0_INTERP_SINGLE_INSTRUCTION] ==
          boundary_before.interpreter_instructions[TIER0_INTERP_SINGLE_INSTRUCTION] + 1);
    /* The replay copy's fetch reads the check overlay: it must not retarget
     * the page's validity record, which would retire the page's regions. */
    for (unsigned n = 0; n < 3; ++n) {
        v->s.psr = UINT64_C(1) << 43;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        v->s.ip = BASE;
        v->s.slot = 0;
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
        v->s.ip = BASE;
        v->s.slot = 0;
    }
    tier0_get_stats(&boundary_before);
    v->s.psr = UINT64_C(1) << 43;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
    v->s.ip = BASE;
    v->s.slot = 0;
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
    tier0_get_stats(&boundary_after);
    CHECK(boundary_after.invalidations == boundary_before.invalidations &&
          boundary_after.translations == boundary_before.translations &&
          boundary_after.checked_regions == boundary_before.checked_regions + 2);
    /* A checked interpreter step storing into its own bundle: the store's
     * notification drops the decoded entry, so the replay copy decodes the
     * bundle from its own overlay view (cache.c) instead of the shared cache. */
    bundle(BASE, 1, store(3, 9, 10), nop, nop);
    restart(v, ENGINE_LOCKSTEP, BASE);
    v->s.psr = UINT64_C(1) << 43;
    uint64_t self_high;
    memcpy(&self_high, ram + 8, 8);
    v->s.gr[9] = self_high;
    v->s.gr[10] = BASE + 8;
    tier0_get_stats(&boundary_before);
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->retired == 1 && v->s.slot == 1);
    tier0_get_stats(&boundary_after);
    CHECK(boundary_after.checked_regions == boundary_before.checked_regions + 1);
    for (enum engine_kind engine = ENGINE_TIER0; engine <= ENGINE_LOCKSTEP; ++engine) {
        code_cache_flush();
        for (unsigned n = 0; n < 48; ++n)
            bundle(BASE + n * 16, 1, nop, add(8, 8, 9), nop);
        restart(v, engine, BASE);
        v->s.gr[9] = 1;
        for (unsigned n = 0; n < 100; ++n) {
            CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->retired == n + 1);
            CHECK(v->s.ip == BASE + ((n + 1) / 3) * 16 && v->s.slot == (n + 1) % 3);
        }
        CHECK(v->s.gr[8] == 33);
        bundle(BASE + 4096 - 16, 1, nop, nop, nop);
        bundle(BASE + 4096, 1, nop, nop, nop);
        restart(v, engine, BASE + 4096 - 16);
        CHECK(engine_run(v, 4).reason == VCPU_EXIT_BUDGET && v->s.ip == BASE + 4096 &&
              v->s.slot == 1);
        /* A device store commits before a middle-in-region fault. */
        bundle(BASE, 9, store(3, 9, 10), load(3, 8, 11), nop);
        restart(v, engine, BASE);
        v->s.gr[9] = 42;
        v->s.gr[10] = DEVICE;
        v->s.gr[11] = DATA + 1;
        unsigned old_writes = writes;
        /* The fault is delivered through the IVT (Unaligned Reference,
         * vector 0x5a00) with IIP/IPSR.ri at the load; the committed store
         * is not reissued. */
        uint64_t iva = BASE + 0x18000;
        bundle(iva + 0x5a00, 1, nop, nop, nop);
        v->s.cr[CR_IVA] = iva;
        v->s.psr |= IA64_PSR_IC;
        struct vcpu_exit out = engine_run(v, 2);
        CHECK(out.reason == VCPU_EXIT_BUDGET && v->retired == 2 && writes == old_writes + 1);
        CHECK(v->fault.kind == IA64_FAULT_UNALIGNED && v->s.cr[CR_IIP] == BASE &&
              ((v->s.cr[CR_IPSR] >> 41) & 3) == 1 && v->s.cr[CR_IFA] == DATA + 1);
        CHECK((v->s.ip & ~UINT64_C(15)) == iva + 0x5a00);
        /* Resume the load as a handler's rfi would, now aligned. */
        v->s.ip = BASE;
        v->s.slot = 1;
        v->s.psr = v->s.cr[CR_IPSR] & ~(UINT64_C(3) << 41);
        v->s.gr[11] = DATA;
        CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET && writes == old_writes + 1);
        /* Requested exits are after-effect; both engines must resume past MMIO. */
        bundle(BASE, 9, load(3, 8, 10), store(3, 9, 10), nop);
        restart(v, engine, BASE);
        v->s.gr[10] = DEVICE;
        request_exit = true;
        unsigned old_reads = reads;
        old_writes = writes;
        out = engine_run(v, 3);
        CHECK(out.reason == VCPU_EXIT_HALT && v->retired == 1 && v->s.slot == 1 &&
              reads == old_reads + 1);
        CHECK(out.ip == BASE && out.slot == 0);
        out = engine_run(v, 2);
        CHECK(out.reason == VCPU_EXIT_HALT && v->retired == 2 && v->s.slot == 2 &&
              writes == old_writes + 1);
        request_exit = false;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && reads == old_reads + 1 &&
              writes == old_writes + 1);
        for (unsigned width = 0; width < 4; ++width) {
            bundle(BASE, 9, load(width, 8, 10), store(width, 9, 10), nop);
            restart(v, engine, BASE);
            v->s.gr[9] = UINT64_MAX;
            v->s.gr[10] = DEVICE + 4096 + 16;
            CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->retired == 3);
            for (unsigned byte = 0; byte < (1u << width); ++byte)
                CHECK(device_bytes[16 + byte] == 255);
        }
        bundle(BASE, 1, load(1, 8, 10), nop, nop);
        restart(v, engine, BASE);
        v->s.gr[10] = DEVICE + 8192;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BAD_ACCESS && v->retired == 0);
        restart(v, engine, BASE);
        v->s.gr[10] = DEVICE + 12288;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BAD_ACCESS && v->retired == 0);
        restart(v, engine, BASE);
        v->s.gr[10] = DEVICE | (UINT64_C(1) << 63);
        old_reads = reads;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && reads == old_reads + 1);
        bundle(BASE, 9, load(3, 8, 10) | 1, store(3, 9, 10) | 1, nop);
        restart(v, engine, BASE);
        v->s.gr[10] = DEVICE;
        v->s.nat[9] = v->s.nat[10] = true;
        old_reads = reads;
        old_writes = writes;
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && reads == old_reads &&
              writes == old_writes);
        /* Previously translated code must invalidate before a helper store. */
        uint64_t remote = BASE + 8192;
        bundle(remote, 1, nop, add(8, 20, 21), nop);
        restart(v, engine, remote);
        v->s.gr[20] = 7;
        v->s.gr[21] = 8;
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->s.gr[8] == 15);
        bundle(BASE, 1, store(3, 9, 10), nop, nop);
        restart(v, engine, BASE);
        v->s.gr[9] = 1 | (nop << 5) | (add(22, 20, 21) << 46);
        v->s.gr[10] = remote;
        v->s.gr[20] = 7;
        v->s.gr[21] = 8;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        v->s.ip = remote;
        v->s.slot = 0;
        v->s.gr[8] = 99;
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->s.gr[8] == 99 && v->s.gr[22] == 15);
        bundle(remote, 1, nop, add(8, 20, 21), nop);
        v->s.ip = remote;
        v->s.slot = 0;
        v->s.gr[20] = 2;
        v->s.gr[21] = 3;
        CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->s.gr[8] == 5);
    }
    /* Distinct entry premises (frame shape, PSR) retain separate packed
     * regions; BOF and rotating bases are resolved at entry and never
     * add one. */
    code_cache_flush();
    bundle(BASE, 1, nop, add(48, 48, 49), nop);
    restart(v, ENGINE_TIER0, BASE);
    struct tier0_stats before, after;
    tier0_get_stats(&before);
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned n = 0; n < 140; ++n) {
            unsigned sof = 18 + n % 70;
            v->s.ip = BASE;
            v->s.slot = 0;
            v->s.rse_bof = (n * 7 + pass * 31) % 96;
            v->s.cfm = sof | ((uint64_t)sof << 7) | (UINT64_C(2) << 14) |
                       ((uint64_t)((n + pass * 5) % 16) << 18) |
                       ((uint64_t)((n + pass) % 96) << 25) |
                       ((uint64_t)((n + 3 * pass) % 48) << 32);
            v->s.psr = (uint64_t)(n / 70) << 32;
            CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET);
        }
    tier0_get_stats(&after);
    CHECK(after.translations == before.translations + 140);
    if (IA64VMM_TIER0_STATS)
        CHECK(after.allocated_bytes - before.allocated_bytes <
              (after.translations - before.translations) * 32768);
}
static void test_divergence(struct vcpu *v)
{
    restart(v, ENGINE_LOCKSTEP, BASE);
    Ia64Instruction i = {.opcode = IA64_OP_ADDS,
                         .valid = true,
                         .address = BASE,
                         .slot = 0,
                         .operands.integer = {.destination = 8, .source2 = 8, .immediate = 1}};
    struct tier0_region region = {.key.ip = BASE, .count = 1, .insns = &i, .slots = {1}};
    struct tier0_execution e = {.region = &region, .check = tier0_check_begin(v)};
    CHECK(e.check != NULL);
    v->s.gr[8] = 2;
    v->retired = 1;
    v->s.slot = 1;
    CHECK(!tier0_check_finish(&e, v));
    tier0_check_destroy(e.check);
}
static void test_atomic_backing(struct vcpu *v)
{
    restart(v, ENGINE_LOCKSTEP, BASE);
    v->s.slot = 1;
    v->s.gr[10] = DEVICE + 16384;
    Ia64Instruction i = {.opcode = IA64_OP_XCHG8,
                         .valid = true,
                         .address = BASE,
                         .slot = 1,
                         .unit = IA64_UNIT_M,
                         .operands.memory = {.destination = 8, .source = 9, .base = 10}};
    struct tier0_region r = {.valid = true, .key.ip = BASE, .count = 1, .insns = &i, .slots = {1}};
    struct tier0_execution e = {.region = &r, .check = tier0_check_begin(v)};
    CHECK(e.check != NULL);
    CHECK(tier0_helper(v, &i, &e) == 1 && e.result.exit == VCPU_EXIT_BAD_ACCESS);
    CHECK(tier0_check_finish(&e, v));
    tier0_check_destroy(e.check);
}
static void checked_helper(struct vcpu *v, Ia64Instruction *i)
{
    struct tier0_region r = {.valid = true, .key.ip = BASE, .count = 1, .insns = i, .slots = {1}};
    struct tier0_execution e = {.region = &r, .check = tier0_check_begin(v)};
    CHECK(e.check != NULL);
    (void)tier0_helper(v, i, &e);
    CHECK(!e.fault && e.result.kind == IA64_STEP_COMPLETE);
    CHECK(tier0_check_finish(&e, v));
    tier0_check_destroy(e.check);
}
static void test_helper_overlay(struct vcpu *v)
{
    restart(v, ENGINE_LOCKSTEP, BASE);
    v->s.ar[AR_RSC] = 0;
    v->s.rse_dirty = 96;
    v->s.ar[AR_BSPSTORE] = DATA + 4080;
    v->s.ar[AR_BSP] = ia64_rse_advance(v->s.ar[AR_BSPSTORE], 96);
    for (unsigned n = 32; n < 128; ++n) {
        v->s.gr[n] = n * 123;
        v->s.nat[n] = n % 3 == 0;
    }
    Ia64Instruction i = {
        .valid = true, .address = BASE, .unit = IA64_UNIT_M, .opcode = IA64_OP_FLUSHRS};
    checked_helper(v, &i); /* Mandatory GR and RNAT writes cross a page. */
    CHECK(v->s.rse_dirty == 0 && v->s.ar[AR_BSPSTORE] == v->s.ar[AR_BSP]);
    v->cfg.itc_icount = false;
    i.opcode = IA64_OP_MOV_ARGR;
    i.operands.system.source = AR_ITC;
    i.operands.system.destination = 8;
    checked_helper(v, &i); /* Replay wall-clock samples, without forcing icount. */
    i.opcode = IA64_OP_MOV_GRAR;
    v->s.gr[8] = UINT64_C(0xfedcba9876543210);
    checked_helper(v, &i);
    CHECK(v->timer_last == v->s.gr[8]);
    v->cfg.itc_icount = true;
}
#include "tests/tier0/validity.inc"
#include "tests/tier0/cache-tests.inc"
#include "tests/tier0/virtual.inc"
#include "tests/tier0/stays.inc"
#include "tests/tier0/stay-witness.inc"
#include "tests/tier0/frame-keys.inc"
#include "tests/tier0/bundle-coherence.inc"
#include "tests/tier0/check-bytes.inc"
#include "tests/tier0/debug.inc"
#include "tests/tier0/coh.inc"
#include "tests/tier0/restart.inc"
#include "tests/tier0/native-paths.inc"
#include "tests/tier0/boundary.inc"
#include "tests/tier0/native.inc"
#include "tests/tier0/extensions.inc"
#include "tests/tier0/immediate-alu.inc"
#include "tests/tier0/extract.inc"
#include "tests/tier0/stacked-operands.inc"
#include "tests/tier0/compact-encoder.inc"
#include "tests/tier0/same-source.inc"
#include "tests/tier0/predicate-homes.inc"
#include "tests/tier0/compare-immediate.inc"
#include "tests/tier0/immediate-shifts.inc"
#include "tests/tier0/forward-values.inc"
#include "tests/tier0/nop-forwarding.inc"
#include "tests/tier0/neutral-immediates.inc"
#include "tests/tier0/same-value-folds.inc"
#include "tests/tier0/predicate-forward.inc"
#include "tests/tier0/forwarded-home.inc"
#include "tests/tier0/forward-combined.inc"
#include "tests/tier0/extension-forward.inc"
#include "tests/tier0/compare-same.inc"
#include "tests/tier0/destination-homes.inc"
#include "tests/tier0/system-homes.inc"
#include "tests/tier0/memory-homes.inc"
#include "tests/tier0/budget-stubs.inc"
#include "tests/tier0/self-moves.inc"
#include "tests/tier0/request-advance.inc"
#include "tests/tier0/timer.inc"
int main(int argc, char **argv)
{
    log_set_level(LOG_ERROR);
    CHECK(gm_add_region(
        &(struct gm_region){.base = BASE, .size = sizeof(ram), .hva = ram, .kind = GM_RAM}));
    CHECK(gm_add_region(
        &(struct gm_region){.base = DEVICE, .size = 4096, .kind = GM_EMULATED, .ops = &ops}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = DEVICE + 4096, .size = 4096, .kind = GM_DIRECT_MMIO, .hva = device_bytes}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = DEVICE + 8192, .size = 32, .kind = GM_DIRECT_MMIO, .hva = device_bytes + 1}));
    static const struct gm_mmio_ops unavailable;
    CHECK(gm_add_region(&(struct gm_region){
        .base = DEVICE + 12288, .size = 32, .kind = GM_EMULATED, .ops = &unavailable}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = DEVICE + 16384, .size = 32, .kind = GM_RAM, .hva = device_bytes + 1}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = BASE + 0x40000, .size = 4096, .kind = GM_RAM, .hva = validator_collision}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = BASE + 0x60000, .size = 4096, .kind = GM_RAM, .hva = decoded_collision}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = DEVICE + 24576, .size = 5, .kind = GM_RAM, .hva = device_bytes + 64}));
    CHECK(gm_add_region(&(struct gm_region){
        .base = DEVICE + 28672, .size = 4096, .kind = GM_EMULATED, .ops = &helper_exit_ops}));
    CHECK(gm_add_region(&(struct gm_region){.base = DEVICE + 32769,
                                            .size = sizeof(alignment_backing),
                                            .kind = GM_RAM,
                                            .hva = alignment_backing}));
    struct vcpu_config cfg = {.engine = ENGINE_TIER0, .itc_icount = true};
    struct vcpu *v = vcpu_create(&cfg), *ref = vcpu_create(&cfg);
    CHECK(v && ref);
    if (argc == 2 && !strcmp(argv[1], "--bench-bundle-stores")) {
        bench_bundle_stores(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
        code_cache_flush();
        tier0_destroy();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--boundary")) {
        test_boundary(v, ref);
        vcpu_destroy(v);
        vcpu_destroy(ref);
        code_cache_flush();
        tier0_destroy();
        log_set_level(LOG_INFO);
        LOGI("test-tier0 --boundary: %u checks passed", checks);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--native")) {
        test_native(v, ref);
        vcpu_destroy(v);
        vcpu_destroy(ref);
        code_cache_flush();
        tier0_destroy();
        log_set_level(LOG_INFO);
        LOGI("test-tier0 --native: %u checks passed", checks);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--stat-variants")) {
        test_stat_variants(v);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--bench-native-paths")) {
        bench_native_paths(v);
        vcpu_destroy(v);
        vcpu_destroy(ref);
        code_cache_flush();
        tier0_destroy();
        return 0;
    }
    test_native_extensions(v, ref);
    test_native_immediate_alu(v, ref);
    test_native_extract(v, ref);
    test_native_stacked_operands(v, ref);
    test_native_same_source(v, ref);
    test_native_predicate_homes(v, ref);
    test_cmp_immediate_encoder();
    test_native_compare_immediate(v, ref);
    test_native_immediate_shifts(v, ref);
    test_native_forward_values(v, ref);
    test_native_nop_forwarding(v, ref);
    test_native_neutral_immediates(v, ref);
    test_native_same_value_folds(v, ref);
    test_native_same_value_identities(v, ref);
    test_native_predicate_forward(v, ref);
    test_native_forwarded_home(v, ref);
    test_native_forward_combined(v, ref);
    test_native_extension_forward(v, ref);
    test_native_compare_same(v, ref);
    test_native_destination_homes(v, ref);
    test_native_system_homes(v, ref);
    test_native_memory_homes(v, ref);
    test_native_budget_stubs(v, ref);
    test_native_self_moves(v, ref);
    test_request_advance(v, ref);
    test_provisional_source_read(v);
    test_x86_immediate_encoder();
    test_compact_encoder();
    test_staged_pointer_sizing(v);
    test_native_code_tracking(v);
    test_host_isa_encoder();
    test_host_isa_rotate(v, ref);
    test_encoder();
    test_equivalence(v, ref);
    test_memory_fast(v, ref);
    test_regions(v);
    test_divergence(v);
    test_atomic_backing(v);
    test_helper_overlay(v);
    test_os_dispatch(v);
    test_hash_reuse(v);
    test_timer_writes(v);
    test_timer_poll(v);
    test_native_helper_exits(v);
    test_store_to_code_bundle(v);
    test_budget_prefix_regions(v, ref);
    test_native_memory_guards(v, ref);
    test_stacked_operand_use(v);
    test_decoded_cache_conflicts(v);
    test_validator(v, ref);
    test_arena(v);
    test_virtual(v, ref);
    test_stays(v);
    test_stay_witnesses(v);
    test_bundle_coherence(v);
    test_coh_tier0(v);
    test_hotness_restart(v, false);
    test_hotness_restart(v, true);
    test_frame_keys(v, ref);
    test_frame_incomplete(v, ref);
    test_debug(v, ref);
    test_boundary(v, ref);
    test_native(v, ref);
    if (!IA64VMM_TIER0_STATS) {
        struct tier0_stats stats;
        tier0_get_stats(&stats);
        for (unsigned i = 0; i < IA64_OP_COUNT; ++i)
            CHECK(!stats.inlined[i] && !stats.helpers[i]);
        CHECK(stats.publications && stats.lookups && stats.protection_changes);
    }
    vcpu_destroy(v);
    vcpu_destroy(ref);
    code_cache_flush();
    tier0_destroy();
    log_set_level(LOG_INFO);
    LOGI("test-tier0: %u checks passed", checks);
    return 0;
}
