/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/semantics.h"
#include "ia64/interrupt.h"
#include "ia64/rse.h"
#include "engine/cache/cache.h"
#include "engine/interp/interp.h"
#include "core/log_internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/interrupts.h"
#include "ia64vmm/platform.h"
#include "ia64/decode/decode.h"

#define ENTRY UINT64_C(0x100000)
#define RAM_SIZE 0x10000u
static uint8_t ram[RAM_SIZE];
static unsigned checks;
unsigned test_fp(void);
unsigned test_hostfp(void);
/* select the same runner and guest images for every engine. */
static enum engine_kind selected_engine = ENGINE_INTERP;
static const char *engine_name = "interp";
struct mmio_access {
    uint64_t offset, value;
    unsigned size;
    bool write;
};
struct mmio_state {
    unsigned reads, writes, size, logged;
    uint64_t offset, value;
    bool halt;
    struct mmio_access log[4];
};
static struct mmio_state mmio;
static uint8_t firmware_ram[4096], direct_mmio[4096] __attribute__((aligned(16)));
static bool mock_clock;
static uint64_t mock_counter, mock_frequency;
uint64_t __real_hal_counter(void);
uint64_t __real_hal_counter_hz(void);
uint64_t __wrap_hal_counter(void)
{
    return mock_clock ? mock_counter : __real_hal_counter();
}
uint64_t __wrap_hal_counter_hz(void)
{
    return mock_clock ? mock_frequency : __real_hal_counter_hz();
}
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static struct vcpu *model_vcpu(unsigned model)
{
    struct vcpu_config cfg = {.engine = selected_engine, .itc_icount = true, .cpu_model = model};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    struct ia64_entry_regs regs = {.ip = ENTRY, .rsc = 3};
    vcpu_reset(v, &regs);
    return v;
}
static struct vcpu *new_vcpu(void)
{
    return model_vcpu(IA64_CPU_MONTECITO);
}
static Ia64Instruction instruction(Ia64Opcode opcode)
{
    return (Ia64Instruction){
        .opcode = opcode,
        .valid = true,
        .address = ENTRY,
        .unit = IA64_UNIT_I,
        .slot = 1,
        .operands.integer = {
            .destination = 8, .source1 = 9, .source2 = 10, .predicate1 = 6, .predicate2 = 7}};
}
static void bundle_write(gpa_t gpa, unsigned t, uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t low = t | (a << 5) | (b << 46), high = (b >> 18) | (c << 23);
    uint8_t bytes[16];
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = low >> (i * 8);
        bytes[i + 8] = high >> (i * 8);
    }
    CHECK(gm_write(gpa, bytes, 16));
}
static bool end_hook(struct vcpu *v, uint64_t imm, uint64_t ip)
{
    (void)ip;
    if (imm != 0x1ffffe)
        return false;
    vcpu_request_exit(v, VCPU_EXIT_DEBUG);
    return true;
}
static uint64_t mmio_read(void *opaque, struct vcpu *v, uint64_t offset, unsigned size)
{
    struct mmio_state *m = opaque;
    ++m->reads;
    m->offset = offset;
    m->size = size;
    if (m->logged < 4)
        m->log[m->logged++] = (struct mmio_access){offset, 0, size, false};
    if (m->halt)
        vcpu_request_exit(v, VCPU_EXIT_HALT);
    return UINT64_C(0xfedcba9876543210);
}
static void mmio_write(void *opaque, struct vcpu *v, uint64_t offset, uint64_t value, unsigned size)
{
    struct mmio_state *m = opaque;
    ++m->writes;
    m->offset = offset;
    m->size = size;
    m->value = value;
    if (m->logged < 4)
        m->log[m->logged++] = (struct mmio_access){offset, value, size, true};
    if (m->halt)
        vcpu_request_exit(v, VCPU_EXIT_HALT);
}
static const struct gm_mmio_ops mmio_ops = {mmio_read, mmio_write};

static void test_templates(void)
{
    static const char *const names[32] = {"MII", "MII", "MII", "MII", "MLX", "MLX", NULL,  NULL,
                                          "MMI", "MMI", "MMI", "MMI", "MFI", "MFI", "MMF", "MMF",
                                          "MIB", "MIB", "MBB", "MBB", NULL,  NULL,  "BBB", "BBB",
                                          "MMB", "MMB", NULL,  NULL,  "MFB", "MFB", NULL,  NULL};
    struct vcpu *v = new_vcpu();
    for (unsigned t = 0; t < 32; ++t) {
        const IA64TemplateInfo *info = ia64_template_info(t);
        CHECK(info->defined == (names[t] != NULL));
        if (names[t])
            CHECK(!strcmp(info->name, names[t]));
        CHECK(info->stop_after[2] == ((t & 1) && info->defined));
        uint64_t a = UINT64_C(0x1123456789a), b = UINT64_C(0x1fedcba9876),
                 c = UINT64_C(0x15555555555);
        uint64_t low = t | (a << 5) | (b << 46), high = (b >> 18) | (c << 23);
        CHECK(ia64_bundle_slot(low, high, 0) == a);
        CHECK(ia64_bundle_slot(low, high, 1) == b);
        CHECK(ia64_bundle_slot(low, high, 2) == c);
        if (!info->defined) {
            bundle_write(ENTRY, t, 0, 0, 0);
            v->s.ip = ENTRY;
            v->s.slot = 0;
            CHECK(engine_run(v, 1).reason == VCPU_EXIT_BAD_ACCESS);
            CHECK(v->fault.kind == IA64_FAULT_ILLEGAL && v->s.slot == 0);
        }
    }
    vcpu_destroy(v);
}

static void test_integer(void)
{
    struct vcpu *v = new_vcpu();
    struct {
        Ia64Opcode op;
        uint64_t a, b;
        int64_t imm;
        uint64_t expected;
    } cases[] = {
        {IA64_OP_ADD, UINT64_MAX, 1, 0, 0},
        {IA64_OP_ADD_ONE, UINT64_MAX, 0, 0, 0},
        {IA64_OP_SUB, 0, 1, 0, UINT64_MAX},
        {IA64_OP_SUB_ONE, 0, 0, 0, UINT64_MAX},
        {IA64_OP_ADDS, 0, 0, -8192, UINT64_C(0xffffffffffffe000)},
        {IA64_OP_ADDL, 0, 1, -2097152, UINT64_C(0xffffffffffe00001)},
        {IA64_OP_ANDCM, 0xff, 0xf, 0, 0xf0},
        {IA64_OP_OR_IMM, 0, 0x1200, -1, UINT64_MAX},
        {IA64_OP_SHL, 64, 1, 0, 0},
        {IA64_OP_SHL, UINT64_MAX, 1, 0, 0},
        {IA64_OP_SHL, 63, 1, 0, UINT64_C(1) << 63},
        {IA64_OP_SHRU, 65, UINT64_MAX, 0, 0},
        {IA64_OP_SHR, 99, UINT64_C(1) << 63, 0, UINT64_MAX},
        {IA64_OP_SHR_IMM, 0, UINT64_C(0x8000000000000000), 4, UINT64_C(0xf800000000000000)},
        {IA64_OP_SHLADD, 7, 2, 4, 114},
        {IA64_OP_ADDP4, 1, UINT64_C(0x80000000), 0, UINT64_C(0x4000000080000001)},
        {IA64_OP_SXT1, 0, 0x80, 0, UINT64_C(0xffffffffffffff80)},
        {IA64_OP_ZXT2, 0, UINT64_MAX, 0, 65535},
        {IA64_OP_DEP, UINT64_MAX, 0, 63 | (1 << 6), UINT64_C(1) << 63},
        {IA64_OP_DEPZ, UINT64_MAX, 0, 60 | (8 << 6), UINT64_C(0xf000000000000000)},
        {IA64_OP_DEP_IMM, 0, 0xff, 4 | (4 << 6), 15},
        {IA64_OP_EXTR, 0, UINT64_MAX, 60 | (8 << 6), UINT64_MAX},
        {IA64_OP_EXTRU, 0, UINT64_MAX, 64 << 6, UINT64_MAX},
        {IA64_OP_SHRP_IMM, 0x1234, 0x5678, 0, 0x5678},
        {IA64_OP_SHRP_IMM, 1, 0, 63, 2},
        {IA64_OP_POPCNT, 0, UINT64_MAX, 0, 64},
    };
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(cases); ++i) {
        Ia64Instruction insn = instruction(cases[i].op);
        v->s.gr[9] = cases[i].a;
        v->s.gr[10] = cases[i].b;
        insn.operands.integer.immediate = cases[i].imm;
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
        CHECK(v->s.gr[8] == cases[i].expected);
    }
    Ia64Instruction insn = instruction(IA64_OP_ADD);
    v->s.nat[9] = true;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
    insn.operands.integer.destination = 9;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.nat[9]);
    insn = instruction(IA64_OP_MOVL);
    insn.operands.integer.immediate = -1;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && !v->s.nat[8]);
    insn.operands.integer.destination = 0;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_ILLEGAL && v->s.gr[0] == 0);
    insn.operands.integer.destination = 32;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_ILLEGAL);
    insn.qp = 1;
    v->s.pr = 1;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
    vcpu_destroy(v);
}

static void test_popcnt(void)
{
    struct vcpu *v = new_vcpu();
    Ia64Instruction insn = instruction(IA64_OP_POPCNT);
    uint64_t random = UINT64_C(0x243f6a8885a308d3);
    for (unsigned trial = 0; trial < 4096; ++trial) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        uint64_t x = trial < 64    ? UINT64_C(1) << trial
                     : trial < 128 ? ~(UINT64_C(1) << (trial - 64))
                     : trial < 193 ? ia64_low_mask(trial - 128)
                                   : random;
        unsigned expected = 0;
        /* Independent oracle: add every bit, including zero bits. */
        for (unsigned bit = 0; bit < 64; ++bit)
            expected += (x >> bit) & 1;
        for (unsigned nat = 0; nat < 2; ++nat) {
            v->s.gr[10] = x;
            v->s.nat[10] = nat;
            insn.operands.integer.destination = trial & 1 ? 8 : 10;
            CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
            CHECK(v->s.gr[insn.operands.integer.destination] == expected);
            CHECK(v->s.nat[insn.operands.integer.destination] == (bool)nat);
        }
    }
    insn.qp = 1;
    v->s.pr = 1;
    v->s.gr[8] = 123;
    v->s.nat[8] = true;
    insn.operands.integer.destination = 8;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 123 && v->s.nat[8]);
    vcpu_destroy(v);
}

static void test_predicates(void)
{
    struct vcpu *v = new_vcpu();
    const Ia64Opcode types[] = {IA64_OP_CMP_EQ, IA64_OP_CMP_EQ_AND, IA64_OP_CMP_EQ_OR,
                                IA64_OP_CMP_EQ_OR_ANDCM};
    for (unsigned type = 0; type < 4; ++type)
        for (unsigned old = 0; old < 4; ++old)
            for (unsigned eq = 0; eq < 2; ++eq)
                for (unsigned nat = 0; nat < 2; ++nat) {
                    Ia64Instruction insn = instruction(types[type]);
                    v->s.gr[9] = 1;
                    v->s.gr[10] = eq ? 1 : 2;
                    v->s.nat[9] = nat;
                    v->s.pr = 1 | ((uint64_t)old << 6);
                    bool a, b;
                    if (type == 0) {
                        a = eq && !nat;
                        b = !eq && !nat;
                    } else if (type == 1) {
                        a = (old & 1) && eq && !nat;
                        b = (old & 2) && eq && !nat;
                    } else if (type == 2) {
                        a = (old & 1) || (eq && !nat);
                        b = (old & 2) || (eq && !nat);
                    } else {
                        a = (old & 1) || (eq && !nat);
                        b = (old & 2) && (!eq || nat);
                    }
                    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
                    CHECK(ia64_pr_read(&v->s, 6) == a && ia64_pr_read(&v->s, 7) == b);
                }
    Ia64Instruction insn = instruction(IA64_OP_CMP_EQ);
    insn.qp = 6;
    insn.compare_unc = true;
    v->s.gr[9] = v->s.gr[10] = 7;
    v->s.nat[9] = false;
    v->s.pr = 1 | (1u << 6);
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
    CHECK(ia64_pr_read(&v->s, 6) && !ia64_pr_read(&v->s, 7));
    v->s.pr = 1 | (1u << 7);
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
    CHECK(!ia64_pr_read(&v->s, 6) && !ia64_pr_read(&v->s, 7));
    insn.operands.integer.predicate2 = 6;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_ILLEGAL);
    insn = instruction(IA64_OP_CMP4_LT);
    v->s.gr[9] = 0xffffffff;
    v->s.gr[10] = 0;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && ia64_pr_read(&v->s, 6));
    insn.opcode = IA64_OP_CMP4_LTU;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && !ia64_pr_read(&v->s, 6));
    insn = instruction(IA64_OP_TNAT_NZ);
    v->s.nat[10] = true;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && ia64_pr_read(&v->s, 6));
    insn.opcode = IA64_OP_TBIT_Z;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && !(v->s.pr & (3u << 6)));
    vcpu_destroy(v);
}

static void test_rotation(void)
{
    struct vcpu *v = new_vcpu();
    v->s.cfm = 8 | (8u << 7) | (1u << 14);
    v->s.gr[39] = 0x1234;
    v->s.nat[39] = true;
    v->s.fr[127].significand = 123;
    ia64_pr_write(&v->s, 63, true);
    ia64_rotate(&v->s);
    CHECK(ia64_gr_read(&v->s, 32) == 0x1234 && ia64_nat_read(&v->s, 32));
    CHECK(v->s.fr[ia64_fr_index(&v->s, 32)].significand == 123 && ia64_pr_read(&v->s, 16));
    CHECK(ia64_pr_pack(&v->s) == ((UINT64_C(1) << 63) | 1));
    ia64_pr_unpack(&v->s, 1u << 16, UINT64_MAX);
    CHECK(v->s.pr == ((1u << 16) | 1));
    Ia64Instruction insn = instruction(IA64_OP_CLRRRB_PR);
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && ia64_pr_read(&v->s, 16));
    insn.opcode = IA64_OP_CLRRRB;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && ia64_gr_read(&v->s, 32) == 0);
    const Ia64Opcode loops[] = {IA64_OP_BR_CTOP, IA64_OP_BR_CEXIT, IA64_OP_BR_WTOP,
                                IA64_OP_BR_WEXIT};
    for (unsigned k = 0; k < 4; ++k)
        for (unsigned lc = 0; lc < 2; ++lc)
            for (unsigned ec = 0; ec < 3; ++ec)
                for (unsigned qp = 0; qp < 2; ++qp) {
                    vcpu_reset(v, NULL);
                    v->s.cfm = 8 | (8u << 7) | (1u << 14);
                    v->s.ar[AR_LC] = lc;
                    v->s.ar[AR_EC] = ec;
                    ia64_pr_write(&v->s, 6, qp);
                    insn = instruction(loops[k]);
                    insn.qp = 6;
                    insn.slot = 2;
                    bool active = (k < 2 ? lc != 0 : qp != 0) || ec > 1;
                    bool rotated = k < 2 ? lc || ec : qp || ec;
                    struct ia64_result r = ia64_execute(v, &insn);
                    CHECK((r.kind == IA64_STEP_BRANCH) == ((k & 1) ? !active : active));
                    CHECK(((v->s.cfm >> 32) & 63) == (rotated ? 47 : 0));
                    CHECK(v->s.ar[AR_EC] == (k < 2 ? lc   ? ec
                                                     : ec ? ec - 1
                                                          : 0
                                             : qp  ? ec
                                             : ec  ? ec - 1
                                                   : 0));
                    CHECK(v->s.ar[AR_LC] == (k < 2 && lc ? lc - 1 : lc));
                }
    vcpu_destroy(v);
}

static void test_simd(void)
{
    struct vcpu *v = new_vcpu();
    struct {
        Ia64Opcode op;
        uint64_t a, b;
        int64_t imm;
        uint64_t result;
    } cases[] = {
        {IA64_OP_PADD1, UINT64_MAX, 0x0101010101010101, 0, 0},
        {IA64_OP_PADD1, 0x7f7f7f7f7f7f7f7f, 0x0101010101010101, 1, 0x7f7f7f7f7f7f7f7f},
        {IA64_OP_PSUB2, 0, 0x0001000100010001, 2, 0},
        {IA64_OP_PADD2, 0x0001000100010001, UINT64_MAX, 3, 0},
        {IA64_OP_PAVG1, 0x0202020202020202, 0x0303030303030303, 0, 0x0303030303030303},
        {IA64_OP_PCMP1_GT, 0x8080808080808080, 0, 0, 0},
        {IA64_OP_PMPY2_R, 0x000000030000fffe, 0x0000000400000005, 0, 0x0000000cfffffff6},
        {IA64_OP_PSHL2, UINT64_MAX, 0, 16, 0},
        {IA64_OP_PSHR2, 0, 0x8000800080008000, 16, UINT64_MAX},
        {IA64_OP_PSHR2_U, 0, UINT64_MAX, 16, 0},
        {IA64_OP_PSHLADD2, 0x4000400040004000, UINT64_MAX, 1, 0x7fff7fff7fff7fff},
        {IA64_OP_PSHRADD2, 0x8000800080008000, UINT64_MAX, 1, 0xbfffbfffbfffbfff},
        {IA64_OP_PSAD1, UINT64_MAX, 0, 0, 2040},
        {IA64_OP_MUX1, 0x0706050403020100, 0, 11, 0x0001020304050607},
        {IA64_OP_UNPACK4_L, 0x1122334455667788, 0x99aabbccddeeff00, 0, 0x55667788ddeeff00},
        {IA64_OP_CZX1_L, 0, 0x0100010101010101, 0, 1},
        {IA64_OP_CZX2_R, 0, UINT64_MAX, 0, 4},
    };
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(cases); ++i) {
        Ia64Instruction insn = instruction(cases[i].op);
        insn.operands.simd.immediate = cases[i].imm;
        v->s.gr[9] = cases[i].a;
        v->s.gr[10] = cases[i].b;
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
        CHECK(v->s.gr[8] == cases[i].result);
    }
    vcpu_destroy(v);
}

static void test_cache_and_exits(void)
{
    struct vcpu *v = new_vcpu();
    const struct ia64_bundle *b;
    uint64_t nop = UINT64_C(1) << 27;
    bundle_write(ENTRY, 1, nop, nop, nop);
    struct code_cache_stats before, after;
    code_cache_get_stats(&before);
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.slot == 1);
    CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET && v->s.ip == ENTRY + 16 && v->s.slot == 0);
    code_cache_get_stats(&after);
    CHECK(after.decodes >= before.decodes + 1 && after.hits >= before.hits + 2);
    bundle_write(ENTRY, 1, 0, nop, nop);
    v->s.ip = ENTRY;
    v->s.slot = 0;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BAD_ACCESS);
    CHECK(v->fault.kind == IA64_FAULT_BREAK && v->retired == 3 && v->s.slot == 0);
    CHECK(ia64_physical_write(v, ENTRY, 8, 1 | (nop << 5)).kind == IA64_STEP_COMPLETE);
    CHECK(code_get_bundle(v, ENTRY, &b).kind == IA64_STEP_COMPLETE &&
          b->insn[0].opcode == IA64_OP_NOP);
    bundle_write(ENTRY + 4096, 1, nop, nop, nop);
    CHECK(code_get_bundle(v, ENTRY + 4096, &b).kind == IA64_STEP_COMPLETE);
    code_cache_get_stats(&before);
    code_notify_cpu_write(ENTRY + 4095, 2);
    CHECK(code_get_bundle(v, ENTRY, &b).kind == IA64_STEP_COMPLETE);
    CHECK(code_get_bundle(v, ENTRY + 4096, &b).kind == IA64_STEP_COMPLETE);
    code_cache_get_stats(&after);
    /* The last byte of page 1 misses ENTRY's bundle; page 2 overlaps. */
    CHECK(after.decodes == before.decodes + 1);
    code_notify_vmm_write(UINT64_MAX - 2, 100);
    code_notify_vmm_write(ENTRY, 0);
    /* Store into this bundle's final slot while executing its first slot.
     * Re-entry within a decoded bundle must observe CPU invalidation. */
    uint64_t st8 = (UINT64_C(4) << 37) | (UINT64_C(0x33) << 30) | (9u << 13) | (10u << 20);
    uint64_t add = (UINT64_C(8) << 37) | (8u << 6) | (20u << 13) | (21u << 20);
    bundle_write(ENTRY, 1, st8, nop, nop);
    v->s.ip = ENTRY;
    v->s.slot = 0;
    v->s.gr[9] = (nop >> 18) | (add << 23);
    v->s.gr[10] = ENTRY + 8;
    v->s.gr[20] = 7;
    v->s.gr[21] = 8;
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->s.gr[8] == 15);
    vcpu_request_exit(v, VCPU_EXIT_HALT);
    uint64_t retired = v->retired;
    CHECK(engine_run(v, 5).reason == VCPU_EXIT_HALT && v->retired == retired);
    v->s.ip = ENTRY;
    v->s.slot = 1;
    v->cfg.engine = ENGINE_TIER0;
    struct vcpu_exit out = engine_run(v, 1);
    CHECK(out.reason == VCPU_EXIT_BUDGET && v->s.slot == 2);
    v->cfg.engine = ENGINE_LOCKSTEP;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
    v->cfg.engine = ENGINE_INTERP;
    v->s.ip = ENTRY + RAM_SIZE;
    out = engine_run(v, 1);
    CHECK(out.reason == VCPU_EXIT_BAD_ACCESS && out.info == ENTRY + RAM_SIZE);
    /* UM.be = 1: the st8 stores its bytes most significant first. */
    v->s.ip = ENTRY;
    v->s.slot = 0;
    v->s.psr = IA64_PSR_BE;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
    uint64_t stored;
    CHECK(gm_read(ENTRY + 8, &stored, 8) && stored == __builtin_bswap64(v->s.gr[9]));
    v->s.psr = IA64_PSR_I;
    vcpu_raise_external(v, 200);
    CHECK(v->s.cr[CR_IRR0 + 3] == (1u << 8));
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BAD_ACCESS);
    bundle_write(ENTRY, 1, nop, nop, nop);
    v->s.ip = ENTRY;
    v->s.slot = 0;
    v->s.psr = 0;
    CHECK(ia64_set_trace(v, 2));
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->trace_used == 1);
    CHECK(v->trace[0].ip == ENTRY && v->trace[0].slot == 0 && v->trace[0].raw == nop);
    CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET && v->trace_used == 2);
    CHECK(v->trace_next == 1 && v->trace[1].slot == 1 && v->trace[0].slot == 2);
    CHECK(v->trace[0].opcode == IA64_OP_NOP && v->trace[1].opcode == IA64_OP_NOP);
    CHECK(ia64_set_trace(v, 0));
    v->s.ip = ENTRY;
    v->s.slot = 0;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
    CHECK(!v->trace && !v->trace_count && !v->trace_used);
    /* Exit requests take priority over a simultaneous kick. Neither retires
     * an instruction, and each notification is consumed exactly once. */
    v->s.ip = ENTRY;
    v->s.slot = 0;
    retired = v->retired;
    vcpu_kick(v);
    vcpu_request_exit(v, VCPU_EXIT_HALT);
    out = engine_run(v, 3);
    CHECK(out.reason == VCPU_EXIT_HALT && out.ip == ENTRY && out.slot == 0);
    CHECK(v->retired == retired && !v->requested_exit && v->irq_kick);
    out = engine_run(v, 3);
    CHECK(out.reason == VCPU_EXIT_BUDGET && out.ip == ENTRY && out.slot == 0);
    CHECK(v->retired == retired && !v->irq_kick);
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->retired == retired + 1);
    /* Tier0 owns kicks during an interpreter fallback or checked replay. */
    v->cfg.engine = ENGINE_TIER0;
    vcpu_kick(v);
    CHECK(interp_run(v, 1).reason == VCPU_EXIT_BUDGET);
    CHECK(v->retired == retired + 2 && v->irq_kick);
    v->cfg.engine = ENGINE_LOCKSTEP;
    CHECK(interp_run(v, 1).reason == VCPU_EXIT_BUDGET);
    CHECK(v->retired == retired + 3 && v->irq_kick);
    vcpu_destroy(v);
}

/* PAL_HALT_LIGHT wake (vcpu_halt_wake_pending): each pending source with
 * its own mask (SDM Vol.2 Table 5-8), independent of PSR.i. */
static void test_halt_wake(void)
{
    struct vcpu *v = new_vcpu();
    struct {
        uint64_t irr0, irr3, tpr;
        bool wake;
    } cases[] = {
        {0, 0, 0, false},
        {0, UINT64_C(1) << 63, 0, true},                  /* vector 255 */
        {UINT64_C(1) << 32, 0, 2u << 4, false},           /* vector 32, mic 2 */
        {UINT64_C(1) << 32, 0, 1u << 4, true},            /* vector 32, mic 1 */
        {0, UINT64_C(1) << 63, UINT64_C(1) << 16, false}, /* mmi masks vectors */
        {1, 0, 15u << 4, true},                           /* ExtINT: mic does not mask it */
        {1, 0, UINT64_C(1) << 16, false},                 /* ExtINT: mmi does */
        {1, UINT64_C(1) << 63, 0, true},                  /* ExtINT does not hide vector 255 */
        {1 | UINT64_C(1) << 32, 0, 1u << 4, true},
        {4, 0, UINT64_C(1) << 16 | 15u << 4, true}, /* NMI ignores TPR */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (unsigned r = 0; r < 4; ++r)
            v->s.cr[CR_IRR0 + r] = 0;
        v->s.cr[CR_IRR0] = cases[i].irr0;
        v->s.cr[CR_IRR0 + 3] = cases[i].irr3;
        v->s.cr[CR_TPR] = cases[i].tpr;
        v->s.psr = 0;
        CHECK(vcpu_halt_wake_pending(v) == cases[i].wake);
    }
    vcpu_destroy(v);
}

/* External interrupt pending (SDM Vol.2 §5.8.1-§5.8.2, Table 5-8): the
 * highest of NMI, ExtINT and vectors 16-255, masked by a higher-or-equal
 * in-service vector, TPR.mmi and TPR.mic; IRR bits 1 and 3-15 are reserved.
 * Expected values follow those rules directly; the engine loops' filter
 * ia64_external_requested is PSR.i with a nonempty IRR. */
static unsigned pending_highest(const uint64_t bits[4])
{
    if (bits[0] & 4)
        return 2;
    if (bits[0] & 1)
        return 0;
    for (unsigned vector = 255; vector >= 16; --vector)
        if ((bits[vector / 64] >> (vector % 64)) & 1)
            return vector;
    return 15;
}
static bool pending_expected(const uint64_t irr[4], const uint64_t service[4], uint64_t tpr)
{
    unsigned vector = pending_highest(irr), in_service = pending_highest(service);
    if (vector == 15 || in_service == 2 || vector == in_service)
        return false;
    if (vector == 2)
        return true;
    if (tpr & (UINT64_C(1) << 16))
        return false;
    if (in_service == 0)
        return false;
    if (vector == 0)
        return true;
    if (in_service != 15 && vector <= in_service)
        return false;
    return (vector >> 4) > ((tpr >> 4) & 15);
}
static void test_external_pending(void)
{
    struct vcpu *v = new_vcpu();
    uint64_t x = UINT64_C(0x9e3779b97f4a7c15);
    for (unsigned trial = 0; trial < 200000; ++trial) {
        uint64_t irr[4] = {0}, service[4] = {0};
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        /* Mostly sparse: no request, reserved bits only, single vectors. */
        unsigned shape = trial % 8;
        for (unsigned n = 0; n < (shape < 2 ? 0u : shape - 1); ++n) {
            unsigned vector = (unsigned)((x >> (8 * n)) & 255);
            irr[vector / 64] |= UINT64_C(1) << (vector % 64);
        }
        if (shape == 1)
            irr[0] = (x & UINT64_C(0xfffa)) | 2;
        if (trial % 3 == 0) {
            unsigned vector = (unsigned)((x >> 40) & 255);
            service[vector / 64] |= UINT64_C(1) << (vector % 64);
        }
        uint64_t tpr = ((x >> 48) & 0xf0) | (trial % 11 == 0 ? UINT64_C(1) << 16 : 0);
        for (unsigned r = 0; r < 4; ++r) {
            v->s.cr[CR_IRR0 + r] = irr[r];
            v->in_service[r] = service[r];
        }
        v->s.cr[CR_TPR] = tpr;
        /* ia64_external_requested filters exactly PSR.i and a nonempty IRR,
         * and never hides a pending interrupt. */
        bool any = irr[0] | irr[1] | irr[2] | irr[3];
        v->s.psr = 0;
        CHECK(!ia64_external_pending(v) && !ia64_external_requested(v));
        v->s.psr = IA64_PSR_I;
        bool pending = ia64_external_pending(v);
        CHECK(pending == pending_expected(irr, service, tpr));
        CHECK(ia64_external_requested(v) == any && (!pending || ia64_external_requested(v)));
    }
    vcpu_destroy(v);
}

static void test_registers(void)
{
    struct vcpu *v = new_vcpu();
    struct ia64_entry_regs entry = {.ip = ENTRY,
                                    .bsp = ENTRY + 0x8000,
                                    .bspstore = ENTRY + 0x8000,
                                    .rsc = 0,
                                    .k0 = UINT64_C(0x80000000ffff0000)};
    vcpu_reset(v, &entry);
    CHECK(v->s.ar[AR_BSP] == entry.bsp && v->s.ar[AR_BSPSTORE] == entry.bspstore);
    CHECK(v->s.ar[AR_RSC] == 0 && v->s.ar[0] == entry.k0);
    vcpu_reset(v, NULL);
    CHECK(v->s.fr[0].significand == 0 && v->s.fr[0].exponent == 0 && !v->s.fr[0].sign);
    CHECK(v->s.fr[1].significand == (UINT64_C(1) << 63) && v->s.fr[1].exponent == 0xffff);
    Ia64Instruction insn = instruction(IA64_OP_MOV_GRBR);
    insn.operands.system.source = 3;
    v->s.gr[8] = 123;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.br[3] == 123);
    v->s.nat[8] = true;
    v->s.gr[8] = 321;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_NAT && v->s.br[3] == 123);
    insn.opcode = IA64_OP_MOV_BRGR;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 123 && !v->s.nat[8]);
    insn = instruction(IA64_OP_MOV_GRAR);
    insn.unit = IA64_UNIT_M;
    insn.operands.system.source = AR_CCV;
    v->s.gr[8] = UINT64_MAX;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.ar[AR_CCV] == UINT64_MAX);
    insn.operands.system.source = AR_RSC;
    v->s.gr[8] = 1u << 5;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_RESERVED && v->s.ar[AR_RSC] == 3);
    insn.operands.system.source = AR_BSPSTORE;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_ILLEGAL);
    v->s.ar[AR_RSC] = 0;
    v->s.gr[8] = ENTRY + 0x4000;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.ar[AR_BSPSTORE] == ENTRY + 0x4000 && v->s.ar[AR_BSP] == ENTRY + 0x4000);
    /* BSP must point past an RNAT slot, even with no dirty GRs. */
    v->s.gr[8] = ENTRY + 0x41f8;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.ar[AR_BSPSTORE] == ENTRY + 0x41f8 && v->s.ar[AR_BSP] == ENTRY + 0x4200);
    v->s.rse_dirty = 64;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.ar[AR_BSPSTORE] == ENTRY + 0x41f8 && v->s.ar[AR_BSP] == ENTRY + 0x4408);
    v->s.rse_dirty = 0;
    insn.operands.system.source = 0;
    v->s.psr = UINT64_C(1) << 32;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_PRIVILEGED_REG);
    v->s.psr = 0;
    insn.operands.system.source = 8;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.ar[8] == v->s.gr[8]);
    insn.operands.system.source = AR_LC;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_ILLEGAL);
    insn.unit = IA64_UNIT_I;
    insn.operands.system.source = AR_PFS;
    v->s.gr[8] = 97;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.ar[AR_PFS] == 97);
    v->s.gr[8] = 0x4010; /* sor=8, sol=0, sof=16. */
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.ar[AR_PFS] == 0x4010);
    v->s.gr[8] = UINT64_C(0x818000094824ca9b); /* fuzz seed 41 */
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.ar[AR_PFS] == v->s.gr[8]);
    for (unsigned bit = 0; bit < 64; ++bit) {
        if ((bit >= 38 && bit <= 51) || (bit >= 58 && bit <= 61)) {
            v->s.gr[8] = UINT64_C(1) << bit;
            uint64_t saved = v->s.ar[AR_PFS];
            CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_RESERVED && v->s.ar[AR_PFS] == saved);
        }
    }
    insn.operands.system.source = AR_EC;
    v->s.gr[8] = 0xffff;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.ar[AR_EC] == 63);
    insn.unit = IA64_UNIT_M;
    insn.operands.system.source = AR_ITC;
    v->s.gr[8] = 999;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && ia64_itc_read(v) == 999);
    v->retired += 10;
    CHECK(ia64_itc_read(v) == 1009);
    insn.opcode = IA64_OP_MOV_ARGR;
    v->s.psr = IA64_PSR_SI | (UINT64_C(1) << 32);
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_PRIVILEGED_REG);
    v->s.psr = 0;
    insn.opcode = IA64_OP_MOV_IP;
    insn.address = ENTRY + 0x20;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.gr[8] == ENTRY + 0x20);
    vcpu_destroy(v);
}

static void test_clock(void)
{
    const struct {
        uint64_t host_hz, guest_hz, ticks;
    } cases[] = {{1000000000, 400000000, 1000000123},
                 {3, UINT64_MAX, 2},
                 {UINT64_MAX, UINT64_MAX, UINT64_MAX - 1},
                 {UINT64_MAX - 33, UINT64_MAX - 5, UINT64_MAX - 34},
                 {UINT64_C(1) << 63, UINT64_MAX, (UINT64_C(1) << 63) - 1},
                 {97, UINT64_MAX - 3, UINT64_MAX}};
    mock_clock = true;
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(cases); ++i) {
        mock_counter = 0;
        mock_frequency = cases[i].host_hz;
        struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_hz = cases[i].guest_hz};
        struct vcpu *v = vcpu_create(&cfg);
        CHECK(v != NULL);
        mock_counter = cases[i].ticks;
        uint64_t expected = (__uint128_t)cases[i].ticks * cases[i].guest_hz / cases[i].host_hz;
        CHECK(ia64_itc_read(v) == expected);
        ia64_itc_write(v, 123);
        CHECK(ia64_itc_read(v) == 123);
        vcpu_destroy(v);
    }
    mock_clock = false;
}

static void test_memory(void)
{
    struct vcpu *v = new_vcpu();
    const Ia64Opcode loads[] = {IA64_OP_LD1, IA64_OP_LD2, IA64_OP_LD4, IA64_OP_LD8};
    const Ia64Opcode stores[] = {IA64_OP_ST1, IA64_OP_ST2, IA64_OP_ST4, IA64_OP_ST8};
    for (unsigned n = 0; n < 4; ++n) {
        unsigned size = 1u << n;
        Ia64Instruction insn = instruction(stores[n]);
        insn.unit = IA64_UNIT_M;
        v->s.gr[10] = ENTRY + 0x8000;
        v->s.gr[9] = UINT64_C(0xfedcba9876543210);
        insn.imm_base_update = true;
        insn.operands.memory.immediate = size;
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE &&
              v->s.gr[10] == ENTRY + 0x8000 + size);
        insn = instruction(loads[n]);
        v->s.gr[10] = (UINT64_C(1) << 63) | (ENTRY + 0x8000);
        v->s.nat[8] = true;
        insn.mem_acquire = true;
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && !v->s.nat[8]);
        CHECK(v->s.gr[8] == (UINT64_C(0xfedcba9876543210) & ia64_low_mask(size * 8)));
        v->s.gr[10] = ENTRY + 0x20000 + 8;
        insn.opcode = stores[n];
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && mmio.size == size &&
              mmio.offset == 8);
        CHECK(mmio.value == (v->s.gr[9] & ia64_low_mask(size * 8)));
        insn.opcode = loads[n];
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && mmio.size == size);
        v->s.gr[10] = ENTRY + 0x30000 + 8;
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
        v->s.gr[10] = ENTRY + 0x40000 + 8;
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
    }
    /* SDM Vol.3 ld/st: postincrement uses the old source, including its
     * NaT, even when a load overwrites that source. */
    v->s.cfm = 96 | (UINT64_C(96) << 7) | (UINT64_C(2) << 14) | (UINT64_C(15) << 18);
    v->s.rse_bof = 95;
    for (unsigned nat = 0; nat < 2; ++nat) {
        Ia64Instruction load = instruction(IA64_OP_LD8);
        load.unit = IA64_UNIT_M;
        load.reg_base_update = true;
        load.operands.memory.destination = load.operands.memory.source = 33;
        load.operands.memory.base = 32;
        ia64_gr_write(&v->s, 32, ENTRY + 0x8100, false);
        ia64_gr_write(&v->s, 33, 8, nat);
        uint64_t datum = UINT64_C(0x123456789abcdef0);
        CHECK(gm_write(ENTRY + 0x8100, &datum, 8));
        CHECK(ia64_execute(v, &load).kind == IA64_STEP_COMPLETE);
        CHECK(ia64_gr_read(&v->s, 32) == ENTRY + 0x8108 && ia64_nat_read(&v->s, 32) == (bool)nat);
        CHECK(ia64_gr_read(&v->s, 33) == datum && !ia64_nat_read(&v->s, 33));
        Ia64Instruction store = instruction(IA64_OP_ST8SPILL);
        store.unit = IA64_UNIT_M;
        store.imm_base_update = true;
        store.operands.memory.base = 32;
        store.operands.memory.source = 33;
        store.operands.memory.immediate = 8;
        ia64_gr_write(&v->s, 32, ENTRY + 0x8108, false);
        ia64_gr_write(&v->s, 33, datum, nat);
        CHECK(ia64_execute(v, &store).kind == IA64_STEP_COMPLETE);
        uint64_t spilled;
        CHECK(gm_read(ENTRY + 0x8108, &spilled, 8));
        CHECK(spilled == (nat ? 0 : datum));
        CHECK(((v->s.ar[AR_UNAT] >> 33) & 1) == nat);
        CHECK(ia64_gr_read(&v->s, 32) == ENTRY + 0x8110 && !ia64_nat_read(&v->s, 32));
    }
    v->s.cfm = 0;
    v->s.rse_bof = 0;
    Ia64Instruction insn = instruction(IA64_OP_LD8);
    v->s.gr[10] = ENTRY + 3;
    v->s.gr[8] = 0x1234;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_UNALIGNED && v->s.gr[8] == 0x1234);
    v->s.nat[10] = true;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_NAT);
    v->s.nat[10] = false;
    v->s.gr[10] = ENTRY + RAM_SIZE;
    struct ia64_result r = ia64_execute(v, &insn);
    CHECK(r.exit == VCPU_EXIT_BAD_ACCESS && r.info == ENTRY + RAM_SIZE);
    v->s.gr[10] = ENTRY + 0x8000;
    v->s.gr[9] = 8;
    v->s.nat[9] = true;
    insn.reg_base_update = true;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.nat[10]);
    CHECK(v->s.gr[10] == ENTRY + 0x8008);
    v->s.nat[10] = false;
    v->s.gr[10] = ENTRY + 0x8208;
    insn = instruction(IA64_OP_ST8SPILL);
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && (v->s.ar[AR_UNAT] & 2));
    uint64_t value;
    CHECK(ia64_physical_read(v, v->s.gr[10], 8, &value).kind == IA64_STEP_COMPLETE && value == 0);
    insn.opcode = IA64_OP_LD8FILL;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
    v->s.nat[9] = false;
    v->s.gr[9] = 123;
    insn.opcode = IA64_OP_ST8SPILL;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && !(v->s.ar[AR_UNAT] & 2));
    insn.opcode = IA64_OP_LD8FILL;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && !v->s.nat[8] && v->s.gr[8] == 123);
    insn.opcode = IA64_OP_LD8A;
    v->s.gr[10] = ENTRY + 0x20000;
    unsigned reads = mmio.reads;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0 &&
          mmio.reads == reads);
    insn.opcode = IA64_OP_LD8C_CLR;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && mmio.reads == reads + 1);
    insn.opcode = IA64_OP_CHK_A_CLR;
    insn.operands.memory.immediate = -16;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_BRANCH &&
          ia64_execute(v, &insn).info == ENTRY - 16);
    insn.opcode = IA64_OP_CHK_S;
    v->s.nat[9] = true;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_BRANCH);
    v->s.nat[9] = false;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
    insn = instruction(IA64_OP_CMPXCHG8);
    v->s.gr[10] = ENTRY + 0x8208;
    v->s.ar[AR_CCV] = 123;
    v->s.gr[9] = 456;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 123);
    CHECK(ia64_physical_read(v, v->s.gr[10], 8, &value).kind == IA64_STEP_COMPLETE && value == 456);
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 456);
    insn.opcode = IA64_OP_FETCHADD8;
    insn.operands.memory.immediate = -16;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 456);
    CHECK(ia64_physical_read(v, v->s.gr[10], 8, &value).kind == IA64_STEP_COMPLETE && value == 440);
    insn.opcode = IA64_OP_XCHG8;
    v->s.gr[9] = 99;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 440);
    insn.opcode = IA64_OP_CMPXCHG1;
    v->s.ar[AR_CCV] = 0x163;
    v->s.gr[9] = 0x55;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 99);
    CHECK(ia64_physical_read(v, v->s.gr[10], 1, &value).kind == IA64_STEP_COMPLETE && value == 99);
    v->s.gr[10] |= UINT64_C(1) << 63;
    CHECK(ia64_execute(v, &insn).fault == IA64_FAULT_UNSUPPORTED_DATA);
    /* A wider ar.ccv never matches: one device read, no write. */
    v->s.gr[10] = ENTRY + 0x20000;
    reads = mmio.reads;
    unsigned writes = mmio.writes;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0x10);
    CHECK(mmio.reads == reads + 1 && mmio.writes == writes && mmio.size == 1);
    /* A real MMIO exit happens after destination and instruction pointer
     * commit; re-entry must never repeat the device read. */
    uint64_t ld8 = (UINT64_C(4) << 37) | (UINT64_C(3) << 30) | (8u << 6) | (10u << 20);
    uint64_t nop = UINT64_C(1) << 27;
    bundle_write(ENTRY, 1, ld8, nop, nop);
    v->s.ip = ENTRY;
    v->s.slot = 0;
    mmio.halt = true;
    reads = mmio.reads;
    struct vcpu_exit out = engine_run(v, 3);
    CHECK(out.reason == VCPU_EXIT_HALT && out.ip == ENTRY && out.slot == 0);
    CHECK(v->s.slot == 1 && v->s.gr[8] == UINT64_C(0xfedcba9876543210) && mmio.reads == reads + 1);
    mmio.halt = false;
    CHECK(engine_run(v, 2).reason == VCPU_EXIT_BUDGET && mmio.reads == reads + 1);
    /* A 16-byte FP spill/fill or paired load to a device is two aligned
     * 8-byte transactions, lower address first (SDM Vol.2 §4.4.9 lists
     * only ldfe/stfe as possibly unsupported on non-WB pages). */
    Ia64Instruction fp = instruction(IA64_OP_STF_SPILL);
    fp.operands.memory.base = 10;
    fp.operands.memory.source = 6;
    v->s.fr[6] = (struct ia64_fr){UINT64_C(0x8000000000000001), 0x1003e, true};
    v->s.gr[10] = ENTRY + 0x20000 + 16;
    writes = mmio.writes;
    reads = mmio.reads;
    CHECK(ia64_execute(v, &fp).kind == IA64_STEP_COMPLETE && mmio.writes == writes + 2);
    CHECK(mmio.offset == 24 && mmio.size == 8 && mmio.value == (0x1003e | (UINT64_C(1) << 17)));
    fp = instruction(IA64_OP_LDF_FILL);
    fp.operands.memory.base = 10;
    CHECK(ia64_execute(v, &fp).kind == IA64_STEP_COMPLETE && mmio.reads == reads + 2 &&
          mmio.offset == 24);
    CHECK(v->s.fr[8].significand == UINT64_C(0xfedcba9876543210) && v->s.fr[8].exponent == 0x03210);
    fp = instruction(IA64_OP_LDFP8);
    fp.operands.memory.base = 10;
    fp.operands.memory.source = 7;
    CHECK(ia64_execute(v, &fp).kind == IA64_STEP_COMPLETE && mmio.reads == reads + 4);
    fp = instruction(IA64_OP_LDFPS);
    fp.operands.memory.base = 10;
    fp.operands.memory.source = 7;
    CHECK(ia64_execute(v, &fp).kind == IA64_STEP_COMPLETE && mmio.reads == reads + 5 &&
          mmio.size == 8);
    fp = instruction(IA64_OP_STFE);
    fp.operands.memory.base = 10;
    fp.operands.memory.source = 6;
    CHECK(ia64_execute(v, &fp).fault == IA64_FAULT_UNSUPPORTED_DATA && mmio.writes == writes + 2);
    vcpu_destroy(v);
}

static void load_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    size_t n = fread(ram, 1, sizeof(ram), f);
    CHECK(n && n < sizeof(ram) && !ferror(f) && feof(f));
    fclose(f);
    code_notify_vmm_write(ENTRY, sizeof(ram));
}
static void expectations(struct vcpu *v, const char *image)
{
    const char *base = strrchr(image, '/');
    base = base ? base + 1 : image;
    char source[256];
    snprintf(source, sizeof(source), "tests/ia64/%s", base);
    char *ext = strrchr(source, '.');
    CHECK(ext != NULL);
    strcpy(ext, ".S");
    FILE *f = fopen(source, "r");
    CHECK(f != NULL);
    char line[512];
    unsigned count = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "EXPECT ");
        if (!p)
            continue;
        unsigned reg;
        unsigned long long value;
        if (sscanf(p, "EXPECT r%u = %llx", &reg, &value) == 2) {
            CHECK(reg < 128);
            if (ia64_gr_read(&v->s, reg) != value)
                fprintf(stderr, "%s: r%u=%llx expected %llx\n", image, reg,
                        (unsigned long long)ia64_gr_read(&v->s, reg), value);
            CHECK(ia64_gr_read(&v->s, reg) == value);
            ++count;
        } else if (sscanf(p, "EXPECT p%u = %llx", &reg, &value) == 2) {
            CHECK(reg < 64);
            CHECK(ia64_pr_read(&v->s, reg) == (value != 0));
            ++count;
        } else if (sscanf(p, "EXPECT nat%u = %llx", &reg, &value) == 2) {
            CHECK(reg < 128);
            CHECK(ia64_nat_read(&v->s, reg) == (value != 0));
            ++count;
        } else {
            unsigned long long address;
            if (sscanf(p, "EXPECT mem[%llx] = %llx", &address, &value) == 2) {
                uint8_t bytes[8];
                uint64_t actual = 0;
                CHECK(gm_read(address, bytes, sizeof(bytes)));
                for (unsigned i = 0; i < 8; ++i)
                    actual |= (uint64_t)bytes[i] << (i * 8);
                CHECK(actual == value);
                ++count;
            } else {
                fprintf(stderr, "unsupported expectation: %s", p);
                exit(1);
            }
        }
    }
    fclose(f);
    CHECK(count != 0);
}
static void run_image(const char *path, bool bench)
{
    load_image(path);
    struct vcpu *v = new_vcpu();
    engine_set_break_hook(end_hook);
    clock_t cpu_start = bench ? clock() : 0;
    uint64_t start = hal_counter();
    struct vcpu_exit out = engine_run(v, UINT64_C(24000000));
    uint64_t elapsed = hal_counter() - start;
    clock_t cpu_elapsed = bench ? clock() - cpu_start : 0;
    if (out.reason != VCPU_EXIT_DEBUG) {
        fprintf(stderr, "%s: exit %u at %llx:%u %s\n", path, out.reason, (unsigned long long)out.ip,
                out.slot, out.detail ? out.detail : "?");
        vcpu_dump(v);
        exit(1);
    }
    if (bench) {
        uint64_t rate = v->retired * hal_counter_hz() / elapsed;
        printf("%s: %llu instructions, %llu instructions/s\n", engine_name,
               (unsigned long long)v->retired, (unsigned long long)rate);
        printf("process_cpu: %.9f seconds\n", (double)cpu_elapsed / CLOCKS_PER_SEC);
        struct code_validity_stats validity;
        code_validity_get_stats(&validity);
        printf("validator: comparisons=%llu ticks=%llu hz=%llu snapshot_bytes=%llu\n",
               (unsigned long long)validity.comparisons,
               (unsigned long long)validity.comparison_ticks, (unsigned long long)hal_counter_hz(),
               (unsigned long long)validity.snapshot_bytes);
    } else {
        expectations(v, path);
        printf("guest image: %s passed (%llu instructions)\n", path,
               (unsigned long long)v->retired);
    }
    vcpu_destroy(v);
    engine_set_break_hook(NULL);
}
#include "tests/ia64/arch-state.inc"
#include "tests/ia64/edge-cases.inc"
#include "tests/ia64/fp-memory-ifs-um.inc"
/* PAL tests use the same mapped RAM and instruction-check helpers. */
#include "tests/ia64/pal-tests.h"
#include "tests/ia64/model-tests.h"
#include "tests/ia64/mmu.inc"
#include "tests/ia64/device-atomic.inc"
#include "tests/ia64/dahr.inc"
#include "tests/ia64/debug-pal-ia32.inc"
#include "tests/ia64/rse-target-frame-faults.inc"
#include "tests/ia64/pal-events.inc"
#include "tests/ia64/popcnt-bench.inc"
#include "tests/ia64/opcode-metadata.inc"
#include "tests/ia64/bundle-reuse.inc"

#include "tests/ia64/system-bench.inc"
#include "tests/ia64/register-pairs.inc"
#include "tests/ia64/register-pair-diff.inc"
#include "tests/ia64/region-range.inc"
#include "tests/ia64/pmu.inc"

int main(int argc, char **argv)
{
    log_set_level(LOG_ERROR);
    if (argc > 1 && !strncmp(argv[1], "--engine=", 9)) {
        engine_name = argv[1] + 9;
        if (!strcmp(engine_name, "tier0"))
            selected_engine = ENGINE_TIER0;
        else if (!strcmp(engine_name, "tier1"))
            selected_engine = ENGINE_TIER1;
        else if (!strcmp(engine_name, "lockstep"))
            selected_engine = ENGINE_LOCKSTEP;
        else
            CHECK(!strcmp(engine_name, "interp"));
        --argc;
        ++argv;
    }
    if (argc > 1 && !strncmp(argv[1], "--code-validity=", 16)) {
        enum code_validity_mode validity;
        CHECK(code_validity_parse_mode(argv[1] + 16, strlen(argv[1] + 16), &validity));
        CHECK(code_validity_configure(validity, CODE_SNAPSHOT_DEFAULT_BYTES));
        --argc;
        ++argv;
    }
    if (argc == 2 && !strcmp(argv[1], "--hostfp")) {
        test_hostfp();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--coverage")) {
        for (unsigned i = 0; i < IA64_OP_COUNT; ++i)
            if (ia64_opcode_reachable(i) && !ia64_opcode_supported(i))
                puts(ia64_opcode_info(i)->name);
        return 0;
    }
    CHECK(gm_add_region(&(struct gm_region){
        .base = ENTRY, .size = RAM_SIZE, .kind = GM_RAM, .hva = ram, .name = "test-ram"}));
    CHECK(gm_add_region(&(struct gm_region){.base = ENTRY + 0x20000,
                                            .size = 4096,
                                            .kind = GM_EMULATED,
                                            .ops = &mmio_ops,
                                            .opaque = &mmio,
                                            .name = "test-device"}));
    CHECK(gm_add_region(&(struct gm_region){.base = ENTRY + 0x30000,
                                            .size = 4096,
                                            .kind = GM_FIRMWARE,
                                            .hva = firmware_ram,
                                            .name = "test-firmware"}));
    CHECK(gm_add_region(&(struct gm_region){.base = ENTRY + 0x40000,
                                            .size = 4096,
                                            .kind = GM_DIRECT_MMIO,
                                            .hva = direct_mmio,
                                            .name = "test-direct"}));
    CHECK(gm_add_region(&(struct gm_region){.base = COUNTER,
                                            .size = 4096,
                                            .kind = GM_EMULATED,
                                            .ops = &counter_ops,
                                            .opaque = &counter,
                                            .name = "test-counter"}));
    if (argc == 3 && !strcmp(argv[1], "--model-tests")) {
        if (selected_engine == ENGINE_TIER1) {
            /* Match product setup: allocate the arena before attaching executors. */
            CHECK(tier0_configure_cmdline(""));
            /* The final-IR verification policy here; every other Tier1
             * suite compiles under the default (after every pass). */
            CHECK(engine_tier1_configure("tier1_hot=1 tier1_verify=final",
                                         CODE_VALIDITY_COMPARE_ALWAYS));
        }
        test_pal_results();
        test_model(argv[2]);
        if (selected_engine == ENGINE_TIER1)
            engine_tier1_destroy();
        printf("model tests (%s): %u checks passed\n", engine_name, checks);
        return 0;
    }
    if (argc == 2 && !strncmp(argv[1], "--audit=", 8)) {
        if (!strncmp(argv[1] + 8, "sys-", 4)) {
            test_fp_memory_ifs_um(argv[1] + 12);
            printf("audit: %u checks passed\n", checks);
            return 0;
        }
        test_edge_cases(argv[1] + 8);
        printf("audit: %u checks passed\n", checks);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--bench-popcnt")) {
        bench_popcnt();
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--bench-execute-paired")) {
        bench_execute_paired((unsigned)strtoul(argv[2], NULL, 10));
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--bench-execute")) {
        bench_execute();
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--bench-decode-paired")) {
        bench_decode_paired((unsigned)strtoul(argv[2], NULL, 10));
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--bench-decode")) {
        bench_decode();
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--bench")) {
        run_image(argv[2], true);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--bench-system-paired")) {
        bench_system_paired((unsigned)strtoul(argv[2], NULL, 10));
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--bench-system")) {
        bench_system();
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--bench-intpairs-paired")) {
        bench_register_pairs(false, (unsigned)strtoul(argv[2], NULL, 10));
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--bench-simdpairs-paired")) {
        bench_register_pairs(true, (unsigned)strtoul(argv[2], NULL, 10));
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--test-register-pair-differential")) {
        test_register_pair_differential();
        return 0;
    }
    test_register_pairs();
    if (argc == 3 && !strcmp(argv[1], "--bench-region-paired")) {
        bench_region_paired((unsigned)strtoul(argv[2], NULL, 10));
        return 0;
    }
    test_region_range();
    test_opcode_predication();
    test_bundle_reuse();
    test_templates();
    test_integer();
    test_popcnt();
    test_predicates();
    test_rotation();
    test_simd();
    test_cache_and_exits();
    test_registers();
    test_halt_wake();
    test_external_pending();
    test_clock();
    test_memory();
    test_pal();
    test_pal_events();
    test_arch_memory();
    test_arch_memory16();
    test_arch_rse();
    test_arch_registers();
    test_arch_pmu_registers();
    test_arch_interrupts();
    test_arch_tlb();
    test_arch_rnat_and_calls();
    test_pmu_retirement();
    test_pmu_controls();
    test_pmu_filters();
    test_pmu_transitions();
    test_pmu_faults();
    test_pmu_nops_and_squashed();
    test_arch_fault_priority();
    test_edge_cases(NULL);
    test_fp_memory_ifs_um(NULL);
    test_mmu();
    test_device_atomics();
    test_device_atomic_threads();
    test_dahr();
    test_debug_pal_ia32();
    test_rse_target_frame_faults();
    test_rse_bad_pfs();
    checks += test_fp();
    checks += test_hostfp();
    for (int i = 1; i < argc; ++i)
        run_image(argv[i], false);
    printf("test-ia64: %u checks passed\n", checks);
    return 0;
}
