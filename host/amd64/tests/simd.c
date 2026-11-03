/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * selftest=simd:
 *  - every CPU enabled the SIMD state: CR4.OSFXSR, OSXMMEXCPT and OSXSAVE,
 *    XCR0 = the boot CPU's mask (with x87, SSE and AVX), MXCSR 1F80H and
 *    x87 control word 037FH at C entry (Intel SDM Vol.3A §16.1.3, Vol.1 §13.3);
 *  - YMM0-15, MXCSR, the x87 control word and the x87 stack loaded on a CPU
 *    survive timer, IPI and profile interrupts taken while it spins;
 *  - the native x86 container keeps both sides' state: the host's YMM0-15,
 *    MXCSR and x87 state across every run, the guest's XMM, YMM, MXCSR and
 *    x87 state across exits while the host runs other values, the guest
 *    starts from the initial state, sees the host's XCR0, and its XSETBV
 *    exits (XCR0 unchanged).
 */
#include "host/amd64/amd64.h"
#include "host/amd64/simd.h"
#include "host/amd64/smp.h"
#include "core/log_internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"
#include "ia64vmm/x86rm.h"

struct simd_state {
    uint8_t ymm[16][32];
    uint8_t st[2][16];
    uint32_t mxcsr;
    uint16_t fcw, fsw;
} IA64VMM_ALIGNED(32);
_Static_assert(__builtin_offsetof(struct simd_state, st) == 512 &&
                   __builtin_offsetof(struct simd_state, mxcsr) == 544 &&
                   __builtin_offsetof(struct simd_state, fcw) == 548 &&
                   __builtin_offsetof(struct simd_state, fsw) == 550,
               "simd_access.S offsets");
uint64_t amd64_simd_probe_call(const struct simd_state *in, struct simd_state *out, const void *fn,
                               void *a, void *b, void *c);
uint64_t amd64_simd_wait(volatile uint32_t *flags);
extern const uint8_t amd64_simd_guest[], amd64_simd_guest_end[];

#define FAIL(...)                                                                                  \
    do {                                                                                           \
        char m_[160];                                                                              \
        vmm_snprintf(m_, sizeof(m_), "selftest=simd: " __VA_ARGS__);                               \
        hal_panic(m_);                                                                             \
    } while (0)
/* Extended-precision 1.0 and pi (SDM Vol.1 §4.2.2: 64-bit significand with
 * explicit integer bit, 15-bit exponent biased by 3FFFH). */
static const uint8_t one[10] = {0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x3f};
static const uint8_t pi[10] = {0x35, 0xc2, 0x68, 0x21, 0xa2, 0xda, 0x0f, 0xc9, 0x00, 0x40};
/* RC = round up, FTZ, all exceptions masked, PE set; RC = truncate. */
#define TEST_MXCSR 0xdfa0u
#define TEST_FCW 0x0f7fu
/* FNINIT then two loads: TOP = 6 (SDM Vol.1 §8.1.3), no exception flags. */
#define FSW_TWO_LOADS 0x3000u

static void pattern(struct simd_state *s, unsigned seed)
{
    memset(s, 0, sizeof(*s));
    for (unsigned r = 0; r < 16; ++r)
        for (unsigned b = 0; b < 32; ++b)
            s->ymm[r][b] = (uint8_t)(seed * 61 + r * 37 + b * 11 + (b >= 16 ? 0x80 : 1));
    memcpy(s->st[0], one, 10);
    memcpy(s->st[1], pi, 10);
    s->mxcsr = TEST_MXCSR ^ (seed & 1 ? 0x6000u : 0);
    s->fcw = TEST_FCW ^ (seed & 1 ? 0x0400u : 0);
}
static const char *differs(const struct simd_state *in, const struct simd_state *out)
{
    for (unsigned r = 0; r < 16; ++r)
        if (memcmp(in->ymm[r], out->ymm[r], 16))
            return "XMM low half";
        else if (memcmp(in->ymm[r] + 16, out->ymm[r] + 16, 16))
            return "YMM upper half";
    if (in->mxcsr != out->mxcsr)
        return "MXCSR";
    if (in->fcw != out->fcw)
        return "x87 control word";
    if (out->fsw != FSW_TWO_LOADS)
        return "x87 status word";
    if (memcmp(in->st[0], out->st[0], 10) || memcmp(in->st[1], out->st[1], 10))
        return "x87 stack";
    return NULL;
}

/* --- every CPU ------------------------------------------------------------ */

struct cpu_test {
    unsigned cpu;
    volatile uint32_t flags[2];
    uint64_t cr4, xcr0, timer, ipi;
    uint32_t mxcsr;
    uint16_t fcw;
    const char *problem;
    struct simd_state in, out;
};
static struct cpu_test test;

static void on_cpu(void *arg)
{
    struct cpu_test *t = arg;
    uint64_t flags;
    __asm__ volatile("mov %%cr4,%0; pushfq; pop %1" : "=r"(t->cr4), "=r"(flags));
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    t->xcr0 = (uint64_t)hi << 32 | lo;
    __asm__ volatile("stmxcsr %0; fnstcw %1" : "=m"(t->mxcsr), "=m"(t->fcw));
    if (!(flags & (1u << 9))) {
        t->problem = "interrupts disabled";
        return;
    }
    hal_cpu_interrupts(t->cpu, &t->timer, &t->ipi);
    hal_timer_arm(hal_counter() + hal_counter_hz() / 500);
    pattern(&t->in, t->cpu);
    amd64_simd_probe_call(&t->in, &t->out, (const void *)amd64_simd_wait, (void *)t->flags, NULL,
                          NULL);
    t->problem = differs(&t->in, &t->out);
}

/* From another CPU: IPIs and profile samples while the target spins, until
 * its own timer interrupt has also been taken. */
static void kick(void *arg)
{
    struct cpu_test *t = arg;
    uint64_t hz = hal_counter_hz(), until = hal_counter() + hz * 2;
    while (!__atomic_load_n(&t->flags[0], __ATOMIC_ACQUIRE))
        if (hal_counter() >= until) {
            t->problem = "target did not start";
            goto done;
        }
    for (unsigned i = 0; i < 8; ++i) {
        uint64_t timer, ipi, before;
        hal_cpu_interrupts(t->cpu, &timer, &before);
        amd64_send_ipi(t->cpu);
        hal_cpu_sample(t->cpu);
        do {
            if (hal_counter() >= until) {
                t->problem = "IPI not taken";
                goto done;
            }
            __asm__ volatile("pause");
            hal_cpu_interrupts(t->cpu, &timer, &ipi);
        } while (ipi == before);
    }
    for (;;) {
        uint64_t timer, ipi;
        hal_cpu_interrupts(t->cpu, &timer, &ipi);
        if (timer != t->timer)
            break;
        if (hal_counter() >= until) {
            t->problem = "timer interrupt not taken";
            goto done;
        }
        __asm__ volatile("pause");
    }
done:
    __atomic_store_n(&t->flags[1], 1, __ATOMIC_RELEASE);
}

static void wait_idle(unsigned cpu)
{
    uint64_t until = hal_counter() + hal_counter_hz() * 5;
    while (hal_cpu_busy(cpu)) {
        if (hal_counter() >= until)
            FAIL("CPU%u did not finish", cpu);
        hal_cpu_poll();
        log_drain();
        if (hal_cpu_busy(cpu))
            hal_idle_until(until);
    }
}

static void every_cpu(unsigned count)
{
    if (count < 2)
        FAIL("needs two CPUs");
    for (unsigned cpu = 0; cpu < count; ++cpu) {
        struct cpu_test *t = &test;
        memset(t, 0, sizeof(*t));
        t->cpu = cpu;
        if (cpu) {
            if (!hal_cpu_start(cpu, on_cpu, t))
                FAIL("cannot dispatch CPU%u", cpu);
            kick(t);
            wait_idle(cpu);
        } else {
            if (!hal_cpu_start(1, kick, t))
                FAIL("cannot dispatch the CPU0 kicker");
            on_cpu(t);
            wait_idle(1);
        }
        uint64_t timer, ipi;
        hal_cpu_interrupts(cpu, &timer, &ipi);
        const uint64_t os = (1u << 9) | (1u << 10) | (1u << 18);
        if ((t->cr4 & os) != os || t->xcr0 != amd64_xcr0 ||
            (t->xcr0 & AMD64_XCR0_BASELINE) != AMD64_XCR0_BASELINE || (t->xcr0 & AMD64_XCR0_AMX) ||
            t->mxcsr != AMD64_MXCSR_DEFAULT || t->fcw != AMD64_FCW_DEFAULT)
            FAIL("CPU%u CR4=%llx XCR0=%llx MXCSR=%x FCW=%x", cpu, (unsigned long long)t->cr4,
                 (unsigned long long)t->xcr0, t->mxcsr, t->fcw);
        if (t->problem)
            FAIL("CPU%u: %s", cpu, t->problem);
        LOGI(
            "selftest=simd: CPU%u state XCR0 = boot mask, MXCSR=1f80 FCW=037f, interrupts preserved YMM0-15 MXCSR x87; "
            "XCR0=%llx, %llu timer, %llu IPI",
            cpu, (unsigned long long)t->xcr0, (unsigned long long)(timer - t->timer),
            (unsigned long long)(ipi - t->ipi));
    }
}

/* --- native x86 container ---------------------------------------------- */

enum {
    GUEST_CODE = 0x1000,
    GUEST_DATA = 0x2000,
    D_XMM = 0x000,
    D_YMM = 0x080,
    D_MXCSR = 0x180,
    D_FCW = 0x184,
    D_ST = 0x190,
    O_XMM = 0x200,
    O_MXCSR = 0x280,
    O_FCW = 0x284,
    O_FSW = 0x286,
    O_ST = 0x290,
    O_XCR0 = 0x2b0,
    I_XMM0 = 0x2c0,
    I_MXCSR = 0x2d0,
    I_FCW = 0x2d4,
    O_YMM = 0x300,
    D_GDT = 0x400,
    D_GDTR = 0x420,
};
#define GUEST_MXCSR 0x3f80u /* RC = round down */
#define GUEST_FCW 0x077fu   /* RC = round up */

static uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint16_t get16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static void segment(struct x86rm_segment *s, uint16_t attrib)
{
    *s = (struct x86rm_segment){0, attrib, 0xffff, 0};
}

/* One phase: run until HLT (host interrupts exit and re-enter), each run
 * inside the harness with this phase's host state. */
static void phase(struct x86rm *rm, struct x86rm_regs *r, struct x86rm_exit *e, unsigned n,
                  bool last)
{
    static struct simd_state in, out;
    pattern(&in, 100 + n);
    for (unsigned runs = 0;; ++runs) {
        if (runs == 1000)
            FAIL("native phase %u did not halt", n);
        bool ok = amd64_simd_probe_call(&in, &out, (const void *)hal_x86rm_run, rm, r, e) & 0xff;
        const char *problem = differs(&in, &out);
        if (problem)
            FAIL("native phase %u changed the host's %s", n, problem);
        if (!ok)
            FAIL("native phase %u: container run failed", n);
        if (e->reason == X86RM_EXIT_INTR || e->reason == X86RM_EXIT_NMI)
            continue;
        if (last)
            return;
        if (e->reason != X86RM_EXIT_HLT)
            FAIL("native phase %u: exit %u code %llx at %llx", n, e->reason,
                 (unsigned long long)e->code, (unsigned long long)r->rip);
        r->rip += 1; /* HLT: F4 */
        return;
    }
}

static void native(void)
{
    const char *why;
    if (!hal_x86rm_probe(&why)) {
        LOGI("selftest=simd: native x86 container unavailable: %s", why);
        return;
    }
    const char *backend = hal_x86rm_info()->name;
    uint8_t *code = hal_alloc_pages(1, HAL_PAGE_ZERO), *data = hal_alloc_pages(1, HAL_PAGE_ZERO);
    static uint8_t io_pass[8192];
    if (!code || !data)
        FAIL("no pages for the native guest");
    if (((uintptr_t)amd64_simd_guest_end - (uintptr_t)amd64_simd_guest) > 4096)
        FAIL("guest program too large");
    memcpy(code, amd64_simd_guest, ((uintptr_t)amd64_simd_guest_end - (uintptr_t)amd64_simd_guest));
    for (unsigned i = 0; i < 0x80; ++i)
        data[D_XMM + i] = (uint8_t)(0xc3 ^ (i * 7));
    for (unsigned i = 0; i < 0x100; ++i)
        data[D_YMM + i] = (uint8_t)(0x5a + i * 13);
    uint32_t mxcsr = GUEST_MXCSR;
    uint16_t fcw = GUEST_FCW;
    memcpy(data + D_MXCSR, &mxcsr, 4);
    memcpy(data + D_FCW, &fcw, 2);
    memcpy(data + D_ST, pi, 10);
    memcpy(data + D_ST + 16, one, 10);
    const uint64_t gdt[3] = {0, UINT64_C(0x00cf9a000000ffff), UINT64_C(0x00cf92000000ffff)};
    memcpy(data + D_GDT, gdt, sizeof(gdt));
    uint16_t limit = sizeof(gdt) - 1;
    uint32_t base = GUEST_DATA + D_GDT;
    memcpy(data + D_GDTR, &limit, 2);
    memcpy(data + D_GDTR + 2, &base, 4);
    const struct x86rm_mapping map[] = {
        {GUEST_CODE, 4096, code, X86RM_MAP_WRITE},
        {GUEST_DATA, 4096, data, X86RM_MAP_WRITE},
    };
    const struct x86rm_config cfg = {map, 2, io_pass};
    struct x86rm *rm;
    if (!hal_x86rm_create(&cfg, &rm, &why))
        FAIL("native container: %s", why);
    struct x86rm_regs r = {0};
    segment(&r.cs, 0x9b);
    segment(&r.ds, 0x93);
    segment(&r.es, 0x93);
    segment(&r.ss, 0x93);
    segment(&r.fs, 0x93);
    segment(&r.gs, 0x93);
    r.gdtr = (struct x86rm_segment){0, 0, 0xffff, 0};
    r.idtr = (struct x86rm_segment){0, 0, 0x3ff, 0};
    r.rip = GUEST_CODE;
    r.rsp = 0xff0;
    r.rflags = 2;
    r.cr0 = 0x10;
    r.cr4 = (1u << 9) | (1u << 10) | (1u << 18);
    struct x86rm_exit e;
    for (unsigned n = 1; n <= 3; ++n)
        phase(rm, &r, &e, n, false);
    phase(rm, &r, &e, 4, true);
    hal_x86rm_destroy(rm);
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    uint64_t xsetbv_exit =
        backend[0] == 'S' ? 0x8d : 55; /* APM Vol.2 Table C-1; SDM Vol.3C Appendix C */
    if (e.reason != X86RM_EXIT_OTHER || (e.code & 0xffff) != xsetbv_exit)
        FAIL("guest XSETBV: exit %u code %llx, expected the XSETBV intercept", e.reason,
             (unsigned long long)e.code);
    if (((uint64_t)hi << 32 | lo) != amd64_xcr0)
        FAIL("host XCR0 changed to %x:%x", hi, lo);
    static const uint8_t zero[16];
    if (memcmp(data + I_XMM0, zero, 16) || get32(data + I_MXCSR) != AMD64_MXCSR_DEFAULT ||
        get16(data + I_FCW) != AMD64_FCW_DEFAULT)
        FAIL("guest did not start from the initial state: MXCSR=%x FCW=%x", get32(data + I_MXCSR),
             get16(data + I_FCW));
    if (memcmp(data + O_XMM, data + D_XMM, 0x80))
        FAIL("guest XMM0-7 lost across an exit");
    if (get32(data + O_MXCSR) != GUEST_MXCSR || get16(data + O_FCW) != GUEST_FCW ||
        get16(data + O_FSW) != FSW_TWO_LOADS || memcmp(data + O_ST, pi, 10) ||
        memcmp(data + O_ST + 16, one, 10))
        FAIL("guest MXCSR/x87 lost across an exit: MXCSR=%x FCW=%x FSW=%x", get32(data + O_MXCSR),
             get16(data + O_FCW), get16(data + O_FSW));
    if (get32(data + O_XCR0) != (uint32_t)amd64_xcr0 ||
        get32(data + O_XCR0 + 4) != (uint32_t)(amd64_xcr0 >> 32))
        FAIL("guest XGETBV saw %x:%x", get32(data + O_XCR0 + 4), get32(data + O_XCR0));
    if (memcmp(data + O_YMM, data + D_YMM, 0x100))
        FAIL("guest YMM0-7 lost across an exit");
    hal_free_pages(code, 1);
    hal_free_pages(data, 1);
    LOGI(
        "selftest=simd: native x86 container (%s): host YMM0-15 MXCSR x87 preserved; guest starts from the "
        "initial state, guest state kept across exits; guest XSETBV intercepted, host XCR0 unchanged; XCR0=%llx",
        backend, (unsigned long long)amd64_xcr0);
}

const char *amd64_simd_selftest_call(const void *fn, void *arg, uint64_t *result)
{
    struct simd_state in, out;
    pattern(&in, 200 + hal_cpu_index());
    *result = amd64_simd_probe_call(&in, &out, fn, arg, NULL, NULL);
    return differs(&in, &out);
}

void amd64_simd_selftest(unsigned count)
{
    every_cpu(count);
    native();
    LOGI("selftest=simd passed");
}
