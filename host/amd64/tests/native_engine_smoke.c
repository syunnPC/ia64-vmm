/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual native execution through IA-64 state/MMU/exit policy, without firmware. */
#include "host/amd64/amd64.h"
#include "ia64/ia32/run.h"
#include "ia64/tlb.h"
#include "ia64vmm/bootinfo.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"
#include "engine/cache/validity.h"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            hal_panic("native engine smoke: " #x);                                                 \
    } while (0)
static struct vcpu v;
static unsigned reads, writes;
static uint32_t results[3];
static uint64_t mmio_read(void *opaque, struct vcpu *cpu, uint64_t offset, unsigned size)
{
    (void)opaque;
    (void)cpu;
    CHECK(size == 4 && (offset == 0 || offset == 8 || offset == 24));
    ++reads;
    return offset == 0 ? 0x12345678 : offset == 8 ? 0x3f800000 : 0;
}
static void mmio_write(void *opaque, struct vcpu *cpu, uint64_t offset, uint64_t value,
                       unsigned size)
{
    (void)opaque;
    (void)cpu;
    CHECK(size == 4 && (offset == 4 || offset == 12 || offset == 16));
    ++writes;
    results[offset == 4 ? 0 : offset == 12 ? 1 : 2] = value;
}
static void setup(uint8_t *ram, gpa_t base)
{
    if (v.native)
        engine_ia32_glue()->detach(&v);
    memset(&v, 0, sizeof(v));
    memset(ram, 0, 16384);
    v.cfg.cpu_model = v.s.cpu_model = IA64_CPU_MADISON;
    v.cfg.engine = ENGINE_INTERP;
    v.s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC | IA64_PSR_IS;
    v.s.rr[0] = 12u << 2;
    v.s.ar[AR_CSD] = UINT64_C(0xc9bfffff00000000);
    v.s.ar[AR_SSD] = UINT64_C(0xc93fffff00000000);
    v.s.gr[24] = v.s.gr[27] = v.s.gr[28] = v.s.gr[29] = v.s.ar[AR_SSD];
    v.s.ar[AR_FCR] = UINT64_C(0x1f800000037f);
    v.s.ar[AR_FSR] = UINT64_C(0x55550000);
    v.s.ar[AR_EFLAG] = 2;
    v.s.ar[AR_CFLG] = (UINT64_C(1) << 41) | 1;
    v.s.cr[CR_IVA] = 0x8000;
    for (unsigned i = 0; i < 4; ++i) {
        ia64_tc_insert(&v, i * 4096, 0, base + i * 4096 + 0x661, 12u << 2);
        ia64_tc_insert(&v, i * 4096, 1, base + i * 4096 + 0x661, 12u << 2);
    }
}
static void memory_cases(uint8_t *ram, gpa_t base)
{
    static const struct gm_mmio_ops mmio = {.read = mmio_read, .write = mmio_write};
    struct gm_region region = {.base = 0x500000, .size = 4096, .kind = GM_EMULATED, .ops = &mmio};
    CHECK(gm_add_region(&region));
    setup(ram, base);
    const uint8_t program[] = {0xa1, 0,    0x20, 0,    0,    0x83, 0xc0, 5,    0xa3, 4,    0x20,
                               0,    0,    0xd9, 0x05, 8,    0x20, 0,    0,    0xd9, 0x1d, 12,
                               0x20, 0,    0,    0xf3, 0x0f, 0x10, 0x05, 8,    0x20, 0,    0,
                               0xf3, 0x0f, 0x58, 0xc0, 0xf3, 0x0f, 0x11, 0x05, 16,   0x20, 0,
                               0,    0x0f, 0xb8, 0x3f, 0x12, 0,    0};
    memcpy(ram, program, sizeof(program));
    ia64_tc_insert(&v, 0x2000, 1, 0x500661, 12u << 2);
    struct vcpu_run_budget budget = {.ia64_instructions = 1,
                                     .native = {.deadline = hal_counter() + hal_counter_hz()}};
    struct vcpu_run_exit out = engine_ia32_glue()->run(&v, &budget);
    LOGI("native MMIO smoke stop=%u reason=%u ip=%llx count=%llu detail=%s", out.stop, out.reason,
         (unsigned long long)v.s.ip, (unsigned long long)out.native_usage.instructions,
         out.detail ? out.detail : "");
    CHECK(out.stop == VCPU_RUN_YIELD && !(v.s.psr & IA64_PSR_IS) && v.s.ip == 0x1230);
    LOGI("native MMIO reads=%u writes=%u values=%x,%x,%x", reads, writes, results[0], results[1],
         results[2]);
    CHECK(out.native_usage.instructions == 9 && reads == 3 && writes == 3);
    CHECK(results[0] == 0x1234567d && results[1] == 0x3f800000 && results[2] == 0x40000000);
    setup(ram, base);
    const uint8_t split[] = {0xa1, 0, 0, 0, 0, 0x0f, 0xb8, 0x3f, 0x12, 0, 0};
    memcpy(ram, split, sizeof(split));
    uint32_t value = 0x78563412;
    memcpy(ram + 0x2000, &value, 4);
    ia64_tc_insert(&v, 0, 1, base + 0x2661, 12u << 2);
    budget.native.deadline = hal_counter() + hal_counter_hz();
    out = engine_ia32_glue()->run(&v, &budget);
    CHECK(out.native_usage.instructions == 2 && !(v.s.psr & IA64_PSR_IS) && v.s.gr[8] == value);
    CHECK((uint32_t)v.s.ar[AR_CSD] == 0 && (uint32_t)v.s.gr[27] == 0);
    /* Unmasked overflow suppresses the store; precision still writes. */
    setup(ram, base);
    const uint8_t overflow[] = {0xd9, 0x05, 0, 0x20, 0,    0,    0xd8, 0xc0, 0xd9, 0x2d,
                                16,   0x20, 0, 0,    0xd9, 0x1d, 4,    0x30, 0,    0};
    memcpy(ram, overflow, sizeof(overflow));
    value = 0x7f7fffff;
    memcpy(ram + 0x2000, &value, 4);
    uint16_t cw = 0x0377;
    memcpy(ram + 0x2010, &cw, 2);
    ia64_tc_insert(&v, 0x3000, 1, 0x500661, 12u << 2);
    reads = writes = 0;
    budget.native.deadline = hal_counter() + hal_counter_hz();
    budget.native.instructions = 4;
    out = engine_ia32_glue()->run(&v, &budget);
    CHECK(out.native_usage.instructions == 4 && v.s.ip == sizeof(overflow));
    CHECK(!reads && !writes && (v.s.ar[AR_FSR] & 0x88) == 0x88);
    setup(ram, base);
    const uint8_t precision[] = {0xd9, 0x05, 0, 0x20, 0,    0, 0xd9, 0x2d, 16,
                                 0x20, 0,    0, 0xdb, 0x1d, 4, 0x30, 0,    0};
    memcpy(ram, precision, sizeof(precision));
    value = 0x3fc00000;
    memcpy(ram + 0x2000, &value, 4);
    cw = 0x035f;
    memcpy(ram + 0x2010, &cw, 2);
    ia64_tc_insert(&v, 0x3000, 1, 0x500661, 12u << 2);
    budget.native.deadline = hal_counter() + hal_counter_hz();
    budget.native.instructions = 3;
    out = engine_ia32_glue()->run(&v, &budget);
    CHECK(out.native_usage.instructions == 3 && v.s.ip == sizeof(precision));
    CHECK(!reads && writes == 1 && results[0] == 2 && (v.s.ar[AR_FSR] & 0xa0) == 0xa0);
    setup(ram, base);
    const uint8_t vm86[] = {0xb8, 0x78, 0x56, 0x05, 1, 0, 0x2e, 0xa3, 0, 0x20};
    memcpy(ram, vm86, sizeof(vm86));
    v.s.psr |= UINT64_C(3) << 32;
    v.s.ar[AR_EFLAG] |= 0x20000;
    v.s.gr[24] = v.s.gr[27] = v.s.gr[28] = v.s.gr[29] = UINT64_C(0x0f30ffff00000000);
    v.s.gr[17] = UINT64_C(0x56781234);
    /* Cached real/VM86 limits are independent of the visible selectors. */
    v.s.ar[AR_CSD] = v.s.ar[AR_SSD] = UINT64_C(0x0f30ffff00000000);
    for (unsigned i = 0; i < 4; ++i) {
        ia64_tc_insert(&v, i * 4096, 0, base + i * 4096 + 0x7e1, 12u << 2);
        ia64_tc_insert(&v, i * 4096, 1, base + i * 4096 + 0x7e1, 12u << 2);
    }
    budget.native.deadline = hal_counter() + hal_counter_hz();
    out = engine_ia32_glue()->run(&v, &budget);
    CHECK(out.native_usage.instructions == 3 && v.s.ip == sizeof(vm86));
    uint16_t stored;
    memcpy(&stored, ram + 0x2000, 2);
    CHECK(stored == 0x5679 && (v.s.ar[AR_EFLAG] & 0x20000));
    CHECK((v.s.gr[17] & 0xffffffff) == 0x56781234 && (uint32_t)v.s.ar[AR_CSD] == 0);
    engine_ia32_glue()->detach(&v);
    LOGI("native engine MMIO and independent I/D translation passed");
}
static void fault_cases(uint8_t *ram, gpa_t base)
{
    /* Itanium SDM Vol.2 Table 10-9: #DE/#XM use IA_32_Exception;
     * illegal opcodes use IA_32_Intercept before operand accesses. */
    const uint8_t div[] = {0xf7, 0x35, 24, 0x20, 0, 0};
    const uint8_t divss[] = {0xf3, 0x0f, 0x5e, 0x05, 24, 0x20, 0, 0};
    for (unsigned simd = 0; simd < 2; ++simd) {
        setup(ram, base);
        memcpy(ram, simd ? divss : div, simd ? sizeof(divss) : sizeof(div));
        ia64_tc_insert(&v, 0x2000, 1, 0x500661, 12u << 2);
        v.s.gr[8] = 0x12345678;
        v.s.ar[AR_CFLG] |= UINT64_C(1) << 42;
        v.s.ar[AR_FCR] &= ~(UINT64_C(1) << 39);
        /* DIVSS 0/0 faults with MXCSR.IE set and XMM0 unchanged. */
        v.s.fr[16].significand = UINT64_C(0x4433221100000000);
        reads = writes = 0;
        struct vcpu_run_budget budget = {
            .ia64_instructions = 1, .native = {.deadline = hal_counter() + hal_counter_hz()}};
        struct vcpu_run_exit out = engine_ia32_glue()->run(&v, &budget);
        unsigned vector = simd ? 19 : 0;
        CHECK(out.stop == VCPU_RUN_YIELD && out.native_usage.instructions_valid);
        CHECK(!out.native_usage.instructions && reads == 1 && !writes);
        CHECK(!(v.s.psr & IA64_PSR_IS) && v.s.ip == 0xe900 && v.s.cr[CR_IIP] == 0);
        CHECK(v.s.cr[CR_ISR] == (uint64_t)vector << 16);
        CHECK(out.native_exit_valid && out.native_exit.reason == IA32_EXIT_EXCEPTION);
        CHECK(out.native_exit.exception.valid && out.native_exit.exception.vector == vector);
        CHECK(out.native_exit.phase == IA32_PHASE_BEFORE && !out.native_exit.vectoring.valid);
        CHECK(v.s.gr[8] == 0x12345678 && !v.s.gr[10]);
        CHECK(v.s.fr[16].significand == UINT64_C(0x4433221100000000));
        CHECK(((v.s.ar[AR_FSR] >> 32) & 0x3f) == simd);
        LOGI("native engine MMIO #%s passed: one read, no writes, status=%llx",
             simd ? "XM" : "DE", (unsigned long long)v.s.ar[AR_FSR]);
    }
    setup(ram, base);
    const uint8_t reserved[] = {0xf2, 0x0f, 0x52, 0x05, 24, 0x20, 0, 0};
    memcpy(ram, reserved, sizeof(reserved));
    ia64_tc_insert(&v, 0x2000, 1, 0x500661, 12u << 2);
    reads = writes = 0;
    struct vcpu_run_budget budget = {
        .ia64_instructions = 1, .native = {.deadline = hal_counter() + hal_counter_hz()}};
    struct vcpu_run_exit out = engine_ia32_glue()->run(&v, &budget);
    CHECK(out.stop == VCPU_RUN_YIELD && !out.native_usage.instructions);
    CHECK(!(v.s.psr & IA64_PSR_IS) && v.s.ip == 0xea00 && v.s.cr[CR_IIP] == 0);
    CHECK(v.s.cr[CR_ISR] == 0x1026 && (v.s.cr[CR_IIM] & 0xffff) == 0x520f);
    CHECK(!out.native_exit_valid && !reads && !writes);
    engine_ia32_glue()->detach(&v);
    LOGI("native engine illegal F2 RSQRT intercept passed without MMIO");
}
static void data_cs_case(uint8_t *ram, gpa_t base)
{
    /* Itanium SDM Vol.1 Table 6-4 permits protected data segments in CSD. */
    setup(ram, base);
    const uint8_t add[] = {0x83, 0xc0, 5};
    memcpy(ram, add, sizeof(add));
    v.s.ar[AR_CSD] = UINT64_C(0xc93fffff00000000);
    v.s.gr[8] = 7;
    reads = writes = 0;
    struct vcpu_run_budget budget = {
        .ia64_instructions = 1,
        .native = {.deadline = hal_counter() + hal_counter_hz(), .instructions = 1}};
    struct vcpu_run_exit out = engine_ia32_glue()->run(&v, &budget);
    CHECK(out.stop == VCPU_RUN_YIELD && out.native_usage.instructions == 1);
    CHECK((v.s.psr & IA64_PSR_IS) && v.s.ip == sizeof(add) && v.s.gr[8] == 12);
    CHECK(v.s.ar[AR_CSD] == UINT64_C(0xc93fffff00000000));
    CHECK(out.native_exit_valid && out.native_exit.reason == IA32_EXIT_STEP);
    CHECK(!reads && !writes);
    engine_ia32_glue()->detach(&v);
    LOGI("native engine protected data CS arithmetic passed");
}
static void conforming_ds_case(uint8_t *ram, gpa_t base)
{
    setup(ram, base);
    const uint8_t load[] = {0xa1, 0, 0x20, 0, 0};
    memcpy(ram, load, sizeof(load));
    uint32_t value = 0x5a961234;
    memcpy(ram + 0x2000, &value, sizeof(value));
    /* A readable conforming code cache has a finite expand-up limit. */
    v.s.gr[27] = UINT64_C(0x09f0ffff00000000);
    reads = writes = 0;
    struct vcpu_run_budget budget = {
        .ia64_instructions = 1,
        .native = {.deadline = hal_counter() + hal_counter_hz(), .instructions = 1}};
    struct vcpu_run_exit out = engine_ia32_glue()->run(&v, &budget);
    CHECK(out.stop == VCPU_RUN_YIELD && out.native_usage.instructions == 1);
    CHECK((v.s.psr & IA64_PSR_IS) && v.s.ip == sizeof(load) && v.s.gr[8] == value);
    CHECK(v.s.gr[27] == UINT64_C(0x09f0ffff00000000));
    CHECK(out.native_exit_valid && out.native_exit.reason == IA32_EXIT_STEP);
    CHECK(!reads && !writes);
    engine_ia32_glue()->detach(&v);
    LOGI("native engine conforming code DS read passed");
}
static void fp_check(unsigned cycle, unsigned step, uint64_t fir, uint64_t fdr)
{
    if (v.s.ar[AR_FIR] != fir || v.s.ar[AR_FDR] != fdr)
        LOGE("native engine FP cycle=%u step=%u ip=%llx expected FIR=%llx FDR=%llx got FIR=%llx "
             "FDR=%llx",
             cycle, step, (unsigned long long)v.s.ip, (unsigned long long)fir,
             (unsigned long long)fdr, (unsigned long long)v.s.ar[AR_FIR],
             (unsigned long long)v.s.ar[AR_FDR]);
    CHECK(v.s.ar[AR_FIR] == fir && v.s.ar[AR_FDR] == fdr);
}
static void fp_retention_case(uint8_t *ram, gpa_t base)
{
    /* APM Vol.2 11.5.10 allows an outer XSAVE to omit no-MF error
     * pointers. Check the semantic environment at every instruction/IRQ
     * boundary, including register x87 operations and FLDCW. */
    setup(ram, base);
    const uint8_t program[] = {
        0xd9, 0xe8,                                     /* FLD1 */
        0xd9, 0x1d, 4, 0x20, 0, 0,                     /* FSTP [2004h] */
        0x90,                                           /* NOP */
        0xd9, 0xe8, 0xd9, 0xe8,                        /* FLD1; FLD1 */
        0xde, 0xc1,                                     /* FADDP ST(1), ST(0) */
        0xd9, 0x2d, 16, 0x20, 0, 0,                    /* FLDCW [2010h] */
        0xd9, 0x1d, 8, 0x20, 0, 0,                     /* FSTP [2008h] */
        0xf3, 0x0f, 0x10, 0x05, 8, 0x20, 0, 0,        /* MOVSS XMM0,[2008h] */
        0xf3, 0x0f, 0x58, 0xc0,                         /* ADDSS XMM0,XMM0 */
        0xf3, 0x0f, 0x11, 0x05, 12, 0x20, 0, 0,       /* MOVSS [200ch],XMM0 */
        0x90, 0xeb, 0xce,                               /* NOP; JMP 0 */
    };
    memcpy(ram, program, sizeof(program));
    const uint32_t next[] = {2, 8, 9, 11, 13, 15, 21, 27, 35, 39, 47, 48, 0};
    const uint64_t fir[] = {
        UINT64_C(0x1e8000000000000), UINT64_C(0x11d000000000002), UINT64_C(0x11d000000000002),
        UINT64_C(0x1e8000000000009), UINT64_C(0x1e800000000000b), UINT64_C(0x6c100000000000d),
        UINT64_C(0x6c100000000000d), UINT64_C(0x11d000000000015), UINT64_C(0x11d000000000015),
        UINT64_C(0x11d000000000015), UINT64_C(0x11d000000000015), UINT64_C(0x11d000000000015),
        UINT64_C(0x11d000000000015),
    };
    const uint32_t operand[] = {0, 0x2004, 0, 0, 0, 0, 0, 0x2008, 0, 0, 0, 0, 0};
    uint64_t expected_fdr = 0, irq_before, irq_after, timers;
    unsigned host_exits = 0;
    hal_cpu_interrupts(hal_cpu_index(), &timers, &irq_before);
    for (unsigned cycle = 0; cycle < 64; ++cycle) {
        uint16_t cw = cycle & 1 ? 0x0b7f : 0x077f;
        memcpy(ram + 0x2010, &cw, sizeof(cw));
        for (unsigned step = 0; step < IA64VMM_ARRAY_SIZE(next); ++step) {
            uint64_t before_fir = v.s.ar[AR_FIR], before_fdr = v.s.ar[AR_FDR];
            uint32_t before_ip = v.s.ip;
            bool completed = false;
            for (unsigned attempt = 0; attempt < 8; ++attempt) {
                struct vcpu_run_budget budget = {
                    .ia64_instructions = 1,
                    .native = {.deadline = hal_counter() + hal_counter_hz(), .instructions = 1}};
                bool interrupt = !attempt && !((cycle + step) & 7);
                uint64_t flags = interrupt ? hal_irq_save() : 0;
                if (interrupt)
                    amd64_send_ipi(hal_cpu_index());
                struct vcpu_run_exit out = engine_ia32_glue()->run(&v, &budget);
                if (interrupt)
                    hal_irq_restore(flags);
                CHECK(out.stop == VCPU_RUN_YIELD && (v.s.psr & IA64_PSR_IS));
                CHECK(out.native_usage.instructions_valid);
                if (out.native_exit_valid &&
                    (out.native_exit.reason == IA32_EXIT_HOST_IRQ ||
                     out.native_exit.reason == IA32_EXIT_HOST_NMI))
                    ++host_exits;
                if (!out.native_usage.instructions) {
                    CHECK(v.s.ip == before_ip);
                    fp_check(cycle, step, before_fir, before_fdr);
                    continue;
                }
                CHECK(out.native_usage.instructions == 1 && v.s.ip == next[step]);
                if (operand[step])
                    expected_fdr = operand[step];
                fp_check(cycle, step, fir[step], expected_fdr);
                if (step >= 6)
                    CHECK((v.s.ar[AR_FCR] & 0xc00) == (cw & 0xc00));
                completed = true;
                break;
            }
            CHECK(completed);
        }
    }
    hal_cpu_interrupts(hal_cpu_index(), &timers, &irq_after);
    CHECK(irq_after > irq_before && host_exits);
    uint32_t result[3];
    memcpy(result, ram + 0x2004, sizeof(result));
    CHECK(result[0] == 0x3f800000 && result[1] == 0x40000000 && result[2] == 0x40800000);
    engine_ia32_glue()->detach(&v);
    LOGI("native engine FP environment passed across 832 instructions and %u native IRQ exits",
         host_exits);
}
void vmm_main(const struct boot_info *bi)
{
    (void)bi;
    uint8_t *ram = hal_alloc_pages(4, HAL_PAGE_ZERO);
    CHECK(ram);
    gpa_t base = 0x400000;
    CHECK(base != (uintptr_t)ram - amd64_hhdm);
    LOGI("native engine smoke GPA=%llx backing HPA=%llx", (unsigned long long)base,
         (unsigned long long)((uintptr_t)ram - amd64_hhdm));
    struct gm_region region = {
        .base = base, .size = 16384, .kind = GM_RAM, .hva = ram, .name = "native engine smoke RAM"};
    CHECK(gm_add_region(&region));
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    const uint8_t program[] = {0x90, 0xb8, 0x78, 0x56, 0x34, 0x12, 0x83, 0xc0, 0x05, 0xa3,
                               0x00, 0x20, 0x00, 0x00, 0xd9, 0xe8, 0xd9, 0x1d, 0x04, 0x20,
                               0x00, 0x00, 0xf3, 0x0f, 0x10, 0x05, 0x04, 0x20, 0x00, 0x00,
                               0xf3, 0x0f, 0x58, 0xc0, 0xf3, 0x0f, 0x11, 0x05, 0x08, 0x20,
                               0x00, 0x00, 0x0f, 0xb8, 0x3f, 0x12, 0x00, 0x00};
    memcpy(ram, program, sizeof(program));
    v.cfg.cpu_model = v.s.cpu_model = IA64_CPU_MADISON;
    v.cfg.engine = ENGINE_INTERP;
    v.s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC | IA64_PSR_IS;
    v.s.rr[0] = 12u << 2;
    v.s.ar[AR_CSD] = UINT64_C(0xc9bfffff00000000);
    v.s.ar[AR_SSD] = UINT64_C(0xc93fffff00000000);
    v.s.gr[24] = v.s.gr[27] = v.s.gr[28] = v.s.gr[29] = v.s.ar[AR_SSD];
    v.s.ar[AR_FCR] = UINT64_C(0x1f800000037f);
    v.s.ar[AR_FSR] = UINT64_C(0x55550000);
    v.s.ar[AR_EFLAG] = 2;
    v.s.ar[AR_CFLG] = (UINT64_C(1) << 41) | 1;
    v.s.cr[CR_IVA] = 0x8000;
    for (unsigned i = 0; i < 4; ++i) {
        ia64_tc_insert(&v, i * 4096, 0, base + i * 4096 + 0x661, 12u << 2);
        ia64_tc_insert(&v, i * 4096, 1, base + i * 4096 + 0x661, 12u << 2);
    }
    const struct engine_ia32_ops *ops = engine_ia32_glue();
    CHECK(ops);
    struct vcpu_run_budget budget = {.ia64_instructions = 1,
                                     .native = {.deadline = hal_counter() + hal_counter_hz()}};
    struct vcpu_run_exit out = ops->run(&v, &budget);
    LOGI("native engine smoke stop=%u reason=%u ip=%llx psr=%llx instructions=%llu detail=%s",
         out.stop, out.reason, (unsigned long long)v.s.ip, (unsigned long long)v.s.psr,
         (unsigned long long)out.native_usage.instructions, out.detail ? out.detail : "");
    struct vcpu_isa_snapshot snapshot;
    if (out.stop == VCPU_RUN_EXIT && vcpu_snapshot_isa(&v, &snapshot)) {
        const struct ia32_native_state *n = &snapshot.native_state;
        const struct ia32_segment *segments[] = {&n->es, &n->cs,   &n->ss, &n->ds,  &n->fs,
                                                 &n->gs, &n->ldtr, &n->tr, &n->gdtr};
        for (unsigned i = 0; i < 9; ++i)
            LOGI("native engine smoke segment%u base=%llx limit=%x sel=%x attr=%x unusable=%u", i,
                 (unsigned long long)segments[i]->base, segments[i]->limit, segments[i]->selector,
                 segments[i]->attrib, segments[i]->unusable);
        static struct ia64_state trial;
        for (unsigned i = 0; i < 8; ++i) {
            trial = v.s;
            LOGI("native engine smoke import group%u status=%u", i,
                 ia32_state_import(&trial, n, UINT64_C(1) << i, 0, false));
        }
    }
    uint32_t stored;
    memcpy(&stored, ram + 0x2000, sizeof(stored));
    CHECK(out.stop == VCPU_RUN_YIELD && !(v.s.psr & IA64_PSR_IS));
    CHECK(v.s.ip == 0x1230 && v.s.gr[1] == sizeof(program));
    CHECK(v.s.gr[8] == 0x1234567d && stored == 0x1234567d);
    memcpy(&stored, ram + 0x2004, sizeof(stored));
    CHECK(stored == 0x3f800000);
    memcpy(&stored, ram + 0x2008, sizeof(stored));
    CHECK(stored == 0x40000000 && v.s.fr[16].significand == 0x40000000);
    LOGI("native engine x87 FIR=%llx FDR=%llx", (unsigned long long)v.s.ar[AR_FIR],
         (unsigned long long)v.s.ar[AR_FDR]);
    CHECK((uint32_t)v.s.ar[AR_FIR] == 0x10 && (uint32_t)v.s.ar[AR_FDR] == 0x2004);
    CHECK(!v.retired && out.native_usage.instructions == 10);
    CHECK(vcpu_snapshot_isa(&v, &snapshot));
    CHECK(snapshot.native_instructions == 10 && snapshot.position.isa == VCPU_ISA_IA64);
    ops->detach(&v);
    memory_cases(ram, base);
    fault_cases(ram, base);
    data_cs_case(ram, base);
    conforming_ds_case(ram, base);
    fp_retention_case(ram, base);
    LOGI(
        "native engine smoke PASSED: integer/store, x87, scalar SSE and JMPE through real backend");
    hal_shutdown(HAL_SHUTDOWN_POWER_OFF, 0);
}
