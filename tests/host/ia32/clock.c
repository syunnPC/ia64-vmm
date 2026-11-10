/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Intel Itanium SDM Rev.2.3 Vol.2 Tables 3-2/7-5, 10.3.3.5,
 * Vol.4 RDPMC/RDTSC; Intel64/IA32 SDM Vol.2B RDTSCP. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/ia32/clock.h"

static struct vcpu v;
static struct ia32_native_state n, before;
static unsigned checks, clock_reads;
static uint64_t virtual_time = UINT64_C(0x8765432112345678);
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
uint64_t ia64_itc_read(const struct vcpu *cpu)
{
    CHECK(cpu == &v);
    ++clock_reads;
    return virtual_time;
}
static void init(unsigned model, unsigned cpl)
{
    memset(&v, 0, sizeof(v));
    memset(&n, 0, sizeof(n));
    v.cfg.cpu_model = v.s.cpu_model = model;
    v.cfg.index = 0x89abcdef;
    v.cfg.lid = UINT64_C(0xfedcba9876543210);
    v.s.psr = (uint64_t)cpl << 32;
    n.cpl = cpl;
    n.eip.value = 0x1234;
    n.cs.base = 0x100000;
    n.eflag = 0x10246;
    n.interruptibility = 2;
    n.pending_debug = 0x4000;
    for (unsigned i = 0; i < IA32_GPRS; ++i)
        n.gpr[i] = 0xdead0000 + i;
    clock_reads = 0;
}
static struct ia32_interruption execute(enum ia32_clock_kind kind, bool fault)
{
    struct ia32_interruption record = {0};
    before = n;
    struct ia64_result result = ia32_clock(&v, &n, kind, &record);
    CHECK(result.kind == IA64_STEP_COMPLETE);
    CHECK(record.valid == fault);
    if (fault) {
        CHECK(!memcmp(&before, &n, sizeof(n)));
        CHECK(record.phase == IA32_PHASE_BEFORE && record.ivt == 0x6900);
        CHECK(record.isr == UINT64_C(0x000d0000) && record.iip == 0x101234);
        CHECK(record.iipa_valid && record.iipa == 0x101234 && !record.ifa_valid);
    } else {
        struct ia32_native_state unchanged = n;
        unchanged.gpr[IA32_EAX] = before.gpr[IA32_EAX];
        unchanged.gpr[IA32_EDX] = before.gpr[IA32_EDX];
        if (kind == IA32_CLOCK_RDTSCP)
            unchanged.gpr[IA32_ECX] = before.gpr[IA32_ECX];
        CHECK(!memcmp(&before, &unchanged, sizeof(n)));
    }
    return record;
}
static uint64_t result(void)
{
    return (uint64_t)n.gpr[IA32_EDX] << 32 | n.gpr[IA32_EAX];
}
static void timestamp_permissions(void)
{
    const enum ia32_clock_kind kinds[] = {IA32_CLOCK_RDTSC, IA32_CLOCK_RDTSCP};
    for (unsigned kind = 0; kind < 2; ++kind)
        for (unsigned cpl = 0; cpl < 4; ++cpl)
            for (unsigned secure = 0; secure < 4; ++secure) {
                init(IA64_CPU_MADISON, cpl);
                if (secure & 1)
                    v.s.psr |= UINT64_C(1) << 23;
                if (secure & 2)
                    n.cflg |= UINT64_C(1) << 34;
                /* Performance security is unrelated to timestamp access. */
                v.s.psr |= UINT64_C(1) << 20;
                bool fault = cpl && secure;
                execute(kinds[kind], fault);
                CHECK(clock_reads == !fault);
                if (!fault) {
                    CHECK(result() == virtual_time);
                    if (kind)
                        CHECK(n.gpr[IA32_ECX] == 0x89abcdef);
                }
            }
    init(IA64_CPU_MONTECITO, 3);
    n.eflag |= 1u << 17; /* VM86 still uses authoritative PSR.cpl/CFLG. */
    n.cflg = UINT64_C(1) << 40;
    execute(IA32_CLOCK_RDTSCP, false);
    CHECK(result() == virtual_time && clock_reads == 1);
    CHECK(ia32_clock_cpu_id(&v) == 0x89abcdef);
    virtual_time = UINT64_MAX;
    execute(IA32_CLOCK_RDTSC, false);
    CHECK(result() == UINT64_MAX && clock_reads == 2);
}
static void counter_selection(void)
{
    const uint64_t samples[] = {0,
                                UINT64_C(0x123456789abc),
                                UINT64_C(0x7fffffffffff),
                                UINT64_C(0x800000000000),
                                UINT64_C(0xffffffffffff),
                                UINT64_MAX};
    const uint64_t madison[] = {0,
                                UINT64_C(0x123456789abc),
                                UINT64_C(0x7fffffffffff),
                                UINT64_C(0xffff800000000000),
                                UINT64_MAX,
                                UINT64_MAX};
    const uint64_t montecito[] = {0,         UINT64_C(0x123456789abc), UINT64_MAX, 0, UINT64_MAX,
                                  UINT64_MAX};
    for (unsigned model = 0; model < 2; ++model) {
        unsigned count = model == IA64_CPU_MADISON ? 4 : 12;
        for (unsigned index = 0; index < count; ++index)
            for (unsigned value = 0; value < sizeof(samples) / sizeof(samples[0]); ++value) {
                init(model, 0);
                n.gpr[IA32_ECX] = index;
                v.s.pmd[index + 4] = samples[value];
                execute(IA32_CLOCK_RDPMC, false);
                CHECK(result() == (model == IA64_CPU_MADISON ? madison[value] : montecito[value]));
                CHECK(!clock_reads && n.gpr[IA32_ECX] == index);
            }
        const uint32_t invalid[] = {count, 63, 255, 0x10000, 0x40000000, 0x80000000, UINT32_MAX};
        for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            init(model, 0);
            n.gpr[IA32_ECX] = invalid[i];
            execute(IA32_CLOCK_RDPMC, true);
            CHECK(!clock_reads);
        }
    }
}
static void counter_permissions(void)
{
    for (unsigned cpl = 0; cpl < 4; ++cpl)
        for (unsigned secure = 0; secure < 8; ++secure) {
            init(IA64_CPU_MADISON, cpl);
            n.gpr[IA32_ECX] = 2;
            v.s.pmd[6] = 0x987654321;
            if (secure & 1)
                v.s.psr |= UINT64_C(1) << 20;
            if (!(secure & 2))
                n.cflg |= UINT64_C(1) << 40;
            if (secure & 4)
                v.s.pmc[6] |= 64;
            /* ITC controls must not secure performance monitor reads. */
            v.s.psr |= UINT64_C(1) << 23;
            n.cflg |= UINT64_C(1) << 34;
            bool fault = cpl && secure;
            execute(IA32_CLOCK_RDPMC, fault);
            if (!fault)
                CHECK(result() == 0x987654321);
            CHECK(!clock_reads);
        }
    init(IA64_CPU_MADISON, 3);
    n.gpr[IA32_ECX] = 2;
    n.cflg = UINT64_C(1) << 40;
    v.s.pmc[2] = 64; /* PMC2 is not the configuration of PMD6. */
    v.s.pmd[6] = 42;
    execute(IA32_CLOCK_RDPMC, false);
    CHECK(result() == 42);
    v.s.pmc[6] = 64;
    execute(IA32_CLOCK_RDPMC, true);
}
static void invalid_kind(void)
{
    init(IA64_CPU_MADISON, 0);
    before = n;
    struct ia32_interruption record = {0};
    struct ia64_result r = ia32_clock(&v, &n, (enum ia32_clock_kind)99, &record);
    CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_UNIMPLEMENTED);
    CHECK(!record.valid && !memcmp(&before, &n, sizeof(n)) && !clock_reads);
}
int main(void)
{
    timestamp_permissions();
    counter_selection();
    counter_permissions();
    invalid_kind();
    printf(
        "ia32 clock: %u checks, virtual timestamps/CPU ID, PMD mapping/width and privilege controls passed\n",
        checks);
    return 0;
}
