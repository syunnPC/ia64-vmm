/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "host/amd64/topology.h"
#include "host/amd64/time_math.h"
#include "host/amd64/simd.h"
#include "ia64vmm/host_simd.h"
#include "core/log_internal.h"
#include "ia64vmm/hal.h"

#define CHECK(x) assert(x)
#include "tests/host/smp/watchdog.h"

struct leaf {
    uint32_t function, sub, a, b, c, d;
};
static struct leaf fixture[16];
static unsigned leaves;
static void query(uint32_t function, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c,
                  uint32_t *d)
{
    *a = *b = *c = *d = 0;
    for (unsigned i = 0; i < leaves; ++i) {
        struct leaf l = fixture[i];
        if (l.function == function && l.sub == sub) {
            *a = l.a;
            *b = l.b;
            *c = l.c;
            *d = l.d;
            return;
        }
    }
}
static void add(uint32_t function, uint32_t sub, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    fixture[leaves++] = (struct leaf){function, sub, a, b, c, d};
}
static void topology_tests(void)
{
    struct boot_cpu_info cpu = {.apic_id = 0x123};
    add(0, 0, 0x1f, 0x756e6547, 0, 0);
    add(0x1f, 0, 1, 2, 1 << 8, 0x123);
    add(0x1f, 1, 5, 16, 2 << 8, 0x123);
    add(0x1f, 2, 8, 64, 5 << 8, 0x123); /* die level above core */
    assert(amd64_topology(&cpu, query));
    assert(cpu.package_id == 1 && cpu.core_id == 17 && cpu.smt_id == 1 &&
           cpu.topology_leaf == 0x1f);
    cpu.apic_id = 0x124;
    assert(!amd64_topology(&cpu, query)); /* MP/CPUID disagreement */
    leaves = 0;
    cpu.apic_id = 21;
    add(0, 0, 0x1f, 0x756e6547, 0, 0); /* empty 1F falls back to B */
    add(0xb, 0, 1, 2, 1 << 8, 21);
    add(0xb, 1, 4, 8, 2 << 8, 21);
    assert(amd64_topology(&cpu, query));
    assert(cpu.package_id == 1 && cpu.core_id == 2 && cpu.smt_id == 1 && cpu.topology_leaf == 0xb);
    leaves = 0;
    add(0, 0, 4, 0x756e6547, 0, 0);
    add(1, 0, 0, (6 << 16) | (21u << 24), 0, 1u << 28);
    add(4, 0, (2u << 26) | 1, 0, 0, 0); /* three physical cores, two SMT */
    assert(amd64_topology(&cpu, query));
    assert(cpu.package_id == 2 && cpu.core_id == 2 && cpu.smt_id == 1);
    leaves = 0;
    cpu.apic_id = 37;
    add(0, 0, 0xd, 0x68747541, 0, 0);
    add(1, 0, (8u << 20) | (15u << 8), (37u << 24) | (8 << 16), 0, 1u << 28);
    add(0x80000000, 0, 0x8000001e, 0, 0, 0);
    add(0x80000001, 0, 0, 0, 1u << 22, 0);
    add(0x80000008, 0, 0, 0, (6 << 12) | 31, 0);
    add(0x8000001e, 0, 37, (1 << 8) | 18, 0, 0);
    assert(amd64_topology(&cpu, query));
    assert(cpu.core_id == 18 && cpu.smt_id == 1 && cpu.package_id == 0);
    fixture[1].a = (6u << 20) | (15u << 8); /* Bulldozer units are not SMT cores */
    assert(!amd64_topology(&cpu, query));
    leaves = 0;
    add(0, 0, 1, 0, 0, 0);
    add(1, 0, 0, (37u << 24) | (4 << 16), 0, 1u << 28);
    assert(!amd64_topology(&cpu, query));

    static struct boot_info bi;
    bi.host_cpu_count = 6;
    bi.cpus[0] =
        (struct boot_cpu_info){.apic_id = 5, .core_id = 2, .smt_id = 1, .topology_valid = true};
    bi.cpus[1] = (struct boot_cpu_info){.apic_id = 10, .core_id = 5, .topology_valid = true};
    bi.cpus[2] = (struct boot_cpu_info){.apic_id = 4, .core_id = 2, .topology_valid = true};
    bi.cpus[3] =
        (struct boot_cpu_info){.apic_id = 11, .core_id = 5, .smt_id = 1, .topology_valid = true};
    bi.cpus[4] = (struct boot_cpu_info){.apic_id = 64, .core_id = 32, .topology_valid = true};
    bi.cpus[5] = (struct boot_cpu_info){
        .apic_id = 256, .package_id = 1, .core_id = 2, .topology_valid = true};
    assert(amd64_placement(&bi, AMD64_PLACE_PHYSICAL_CORE, 3));
    assert(bi.cpus[1].vcpu_index == 0 && bi.cpus[4].vcpu_index == 1 && bi.cpus[5].vcpu_index == 2);
    assert(bi.cpus[0].vcpu_index == UINT32_MAX && bi.cpus[2].vcpu_index == UINT32_MAX &&
           bi.cpus[3].vcpu_index == UINT32_MAX);
    assert(!amd64_placement(&bi, AMD64_PLACE_PHYSICAL_CORE, 4));
    assert(amd64_placement(&bi, AMD64_PLACE_LOGICAL_THREAD, 5));
    bi.cpus[2].topology_valid = false;
    assert(!amd64_placement(&bi, AMD64_PLACE_PHYSICAL_CORE, 1));
    assert(amd64_placement(&bi, AMD64_PLACE_LOGICAL_THREAD, 2));
    assert(!amd64_placement(&bi, AMD64_PLACE_LOGICAL_THREAD, 0));

    /* CPUID.1AH type comes from each logical CPU, independent of MP order. */
    leaves = 0;
    cpu.apic_id = 0x10;
    add(0, 0, 0x1f, 0x756e6547, 0, 0);
    add(0x1a, 0, 0x40000001, 0, 0, 0);
    add(0x1f, 0, 0, 1, 1 << 8, 0x10);
    add(0x1f, 1, 5, 8, 2 << 8, 0x10);
    assert(amd64_topology(&cpu, query) && cpu.core_type == 0x40);
    fixture[1].a = 0x20000002;
    assert(amd64_topology(&cpu, query) && cpu.core_type == 0x20);
    fixture[1].a = 0;
    assert(amd64_topology(&cpu, query) && cpu.core_type == 0);

    memset(&bi, 0, sizeof(bi));
    bi.host_cpu_count = 6;
    for (unsigned i = 0; i < 6; ++i) {
        bi.cpus[i].core_id = i;
        bi.cpus[i].topology_valid = true;
        bi.cpus[i].core_type = i < 3 ? 0x20 : 0x40;
    }
    assert(amd64_placement(&bi, AMD64_PLACE_PHYSICAL_CORE, 4));
    assert(bi.cpus[3].vcpu_index == 0 && bi.cpus[4].vcpu_index == 1 && bi.cpus[5].vcpu_index == 2 &&
           bi.cpus[1].vcpu_index == 3);
    bi.cpus[5].core_id = 3; /* P-core SMT sibling, different MP slot. */
    assert(amd64_placement(&bi, AMD64_PLACE_PHYSICAL_CORE, 3));
    assert(bi.cpus[5].vcpu_index == UINT32_MAX && bi.cpus[1].vcpu_index == 2);
    assert(amd64_placement(&bi, AMD64_PLACE_LOGICAL_THREAD, 3));
    assert(bi.cpus[5].vcpu_index == 2);
}

/* Helpers of a cores x threads package whose MP order lists the given APIC
 * IDs (NULL: ascending); the count per rank must equal the rank rule of
 * amd64_helpers. */
static void expect_helpers(unsigned cores, unsigned threads, const unsigned *mp_order,
                           enum amd64_placement_mode mode, unsigned vcpus, unsigned rank1,
                           unsigned rank2, unsigned rank3)
{
    static struct boot_info bi;
    memset(&bi, 0, sizeof(bi));
    bi.host_cpu_count = cores * threads;
    for (unsigned i = 0; i < bi.host_cpu_count; ++i) {
        unsigned apic = mp_order ? mp_order[i] : i;
        bi.cpus[i] = (struct boot_cpu_info){.apic_id = apic,
                                            .core_id = apic / threads,
                                            .smt_id = apic % threads,
                                            .topology_valid = true};
    }
    assert(amd64_placement(&bi, mode, vcpus));
    unsigned cpu[BOOT_MAX_CPUS], rank[BOOT_MAX_CPUS], count[4] = {0};
    unsigned n = amd64_helpers(&bi, cpu, rank);
    assert(n == bi.host_cpu_count - 1 - vcpus);
    for (unsigned i = 0; i < n; ++i) {
        const struct boot_cpu_info *helper = &bi.cpus[cpu[i]];
        assert(!helper->management && helper->vcpu_index == UINT32_MAX);
        assert(rank[i] >= 1 && rank[i] <= 3);
        /* Rank order, MP order within a rank. */
        assert(!i || rank[i - 1] < rank[i] || (rank[i - 1] == rank[i] && cpu[i - 1] < cpu[i]));
        ++count[rank[i]];
    }
    assert(count[1] == rank1 && count[2] == rank2 && count[3] == rank3);
}
static void helper_tests(void)
{
    /* QEMU runner and VMware backend: N cores, one thread each. */
    expect_helpers(4, 1, NULL, AMD64_PLACE_PHYSICAL_CORE, 1, 2, 0, 0);
    expect_helpers(4, 1, NULL, AMD64_PLACE_PHYSICAL_CORE, 2, 1, 0, 0);
    expect_helpers(4, 1, NULL, AMD64_PLACE_PHYSICAL_CORE, 3, 0, 0, 0);
    expect_helpers(5, 1, NULL, AMD64_PLACE_PHYSICAL_CORE, 4, 0, 0, 0);
    expect_helpers(8, 1, NULL, AMD64_PLACE_PHYSICAL_CORE, 4, 3, 0, 0);
    expect_helpers(5, 1, NULL, AMD64_PLACE_PHYSICAL_CORE, 2, 2, 0, 0);
    /* Synthetic 2C/4T and 8C/16T topologies, siblings adjacent or primaries first. */
    static const unsigned two_core_split[] = {0, 2, 1, 3};
    static const unsigned eight_core_split[] = {0, 2, 4, 6, 8, 10, 12, 14,
                                                1, 3, 5, 7, 9, 11, 13, 15};
    for (unsigned order = 0; order < 2; ++order) {
        expect_helpers(2, 2, order ? two_core_split : NULL, AMD64_PLACE_PHYSICAL_CORE, 1, 0, 1, 1);
        expect_helpers(2, 2, order ? two_core_split : NULL, AMD64_PLACE_LOGICAL_THREAD, 2, 0, 0, 1);
        expect_helpers(2, 2, order ? two_core_split : NULL, AMD64_PLACE_LOGICAL_THREAD, 3, 0, 0, 0);
        expect_helpers(8, 2, order ? eight_core_split : NULL, AMD64_PLACE_PHYSICAL_CORE, 4, 6, 1,
                       4);
        expect_helpers(8, 2, order ? eight_core_split : NULL, AMD64_PLACE_PHYSICAL_CORE, 7, 0, 1,
                       7);
    }
    /* A core holding the management CPU and a vCPU (SMT-4, logical-thread):
     * its third thread competes with the vCPU, rank 3. An unverified CPU's
     * core is unknown and may be a vCPU's: rank 3. */
    static struct boot_info bi;
    memset(&bi, 0, sizeof(bi));
    bi.host_cpu_count = 6;
    for (unsigned i = 0; i < 6; ++i)
        bi.cpus[i] = (struct boot_cpu_info){
            .apic_id = i, .core_id = i / 4, .smt_id = i % 4, .topology_valid = true};
    assert(amd64_placement(&bi, AMD64_PLACE_LOGICAL_THREAD, 1) && bi.cpus[1].vcpu_index == 0);
    unsigned cpu[BOOT_MAX_CPUS], rank[BOOT_MAX_CPUS];
    assert(amd64_helpers(&bi, cpu, rank) == 4);
    assert(cpu[0] == 4 && rank[0] == 1 && cpu[1] == 5 && rank[1] == 1);
    assert(cpu[2] == 2 && rank[2] == 3 && cpu[3] == 3 && rank[3] == 3);
    bi.cpus[5].topology_valid = false;
    assert(amd64_placement(&bi, AMD64_PLACE_LOGICAL_THREAD, 1));
    assert(amd64_helpers(&bi, cpu, rank) == 4);
    assert(cpu[0] == 4 && rank[0] == 1 && cpu[1] == 2 && cpu[2] == 3 && cpu[3] == 5 &&
           rank[3] == 3);
}

/* Core mode rule: every physical core, the management core included, holds at
 * most one vCPU; the management CPU itself is never a vCPU. Returns whether
 * the placement obeys that and holds exactly `expected` vCPUs. */
static bool core_rule_holds(const struct boot_info *bi, unsigned expected)
{
    unsigned total = 0;
    for (unsigned i = 0; i < bi->host_cpu_count; ++i) {
        if (bi->cpus[i].vcpu_index == UINT32_MAX)
            continue;
        ++total;
        if (i == bi->management_cpu)
            return false;
        for (unsigned j = i + 1; j < bi->host_cpu_count; ++j)
            if (bi->cpus[j].vcpu_index != UINT32_MAX &&
                bi->cpus[j].package_id == bi->cpus[i].package_id &&
                bi->cpus[j].core_id == bi->cpus[i].core_id)
                return false;
    }
    return total == expected;
}
/* cores x threads, ascending APIC IDs; CPU0 (core 0, thread 0) is management. */
static void smt_fixture(struct boot_info *bi, unsigned cores, unsigned threads)
{
    memset(bi, 0, sizeof(*bi));
    bi->host_cpu_count = cores * threads;
    for (unsigned i = 0; i < bi->host_cpu_count; ++i)
        bi->cpus[i] = (struct boot_cpu_info){
            .apic_id = i, .core_id = i / threads, .smt_id = i % threads, .topology_valid = true};
}
static void core_placement_tests(void)
{
    static struct boot_info bi;
    const enum amd64_placement_mode core = AMD64_PLACE_CORE;
    /* 2C4T: core0 = management + vCPU0 on its sibling, core1 = vCPU1 + helper. */
    smt_fixture(&bi, 2, 2);
    assert(amd64_placement(&bi, core, 2) && core_rule_holds(&bi, 2));
    assert(bi.cpus[1].vcpu_index == 0 && bi.cpus[2].vcpu_index == 1);
    assert(bi.cpus[0].vcpu_index == UINT32_MAX && bi.cpus[3].vcpu_index == UINT32_MAX);
    assert(!amd64_placement(&bi, core, 3)); /* two cores hold two vCPUs */
    assert(amd64_placement(&bi, core, 1) && bi.cpus[1].vcpu_index == 0 && core_rule_holds(&bi, 1));
    /* 8C16T: eight vCPUs, one per core; MP order of the threads is irrelevant. */
    static const unsigned split[] = {0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15};
    for (unsigned order = 0; order < 2; ++order) {
        smt_fixture(&bi, 8, 2);
        if (order)
            for (unsigned i = 0; i < 16; ++i) {
                bi.cpus[i].apic_id = split[i];
                bi.cpus[i].core_id = split[i] / 2;
                bi.cpus[i].smt_id = split[i] % 2;
            }
        assert(amd64_placement(&bi, core, 8) && core_rule_holds(&bi, 8));
        assert(!amd64_placement(&bi, core, 9));
        /* The management core's vCPU is its sibling thread. */
        for (unsigned i = 1; i < 16; ++i)
            if (bi.cpus[i].core_id == bi.cpus[0].core_id)
                assert(bi.cpus[i].vcpu_index != UINT32_MAX);
    }
    /* 4C4T: no SMT sibling on the management core, so it holds no vCPU. */
    smt_fixture(&bi, 4, 1);
    assert(amd64_placement(&bi, core, 3) && core_rule_holds(&bi, 3));
    assert(bi.cpus[0].vcpu_index == UINT32_MAX && bi.cpus[1].vcpu_index == 0);
    assert(!amd64_placement(&bi, core, 4));
    /* Unverified topology cannot prove the cores distinct. */
    smt_fixture(&bi, 2, 2);
    bi.cpus[3].topology_valid = false;
    assert(!amd64_placement(&bi, core, 1));
    assert(amd64_placement(&bi, AMD64_PLACE_LOGICAL_THREAD, 1));
    assert(!amd64_placement(&bi, core, 0));
    /* Two packages whose core IDs repeat are four cores. */
    smt_fixture(&bi, 2, 2);
    bi.cpus[2].package_id = bi.cpus[3].package_id = 1;
    bi.cpus[2].core_id = bi.cpus[3].core_id = 0;
    assert(amd64_placement(&bi, core, 2) && core_rule_holds(&bi, 2));
    /* Hybrid: P cores, then unknown, then E; MP order within a class. Cores
     * 0-1 are E (core 0 holds management), cores 2-3 are P. */
    smt_fixture(&bi, 4, 2);
    for (unsigned i = 0; i < 8; ++i)
        bi.cpus[i].core_type = i < 4 ? 0x20 : 0x40;
    assert(amd64_placement(&bi, core, 4) && core_rule_holds(&bi, 4));
    assert(bi.cpus[4].vcpu_index == 0 && bi.cpus[6].vcpu_index == 1 && bi.cpus[1].vcpu_index == 2 &&
           bi.cpus[2].vcpu_index == 3);
    /* A P management core: its sibling comes first. */
    for (unsigned i = 0; i < 8; ++i)
        bi.cpus[i].core_type = i < 4 ? 0x40 : 0x20;
    assert(amd64_placement(&bi, core, 3) && core_rule_holds(&bi, 3));
    assert(bi.cpus[1].vcpu_index == 0 && bi.cpus[2].vcpu_index == 1 && bi.cpus[4].vcpu_index == 2);
    assert(bi.cpus[6].vcpu_index == UINT32_MAX);
    for (unsigned i = 0; i < 8; ++i)
        bi.cpus[i].core_type = 0;
    bi.cpus[3].core_type = 0x40; /* a P thread outranks earlier unknown ones */
    assert(amd64_placement(&bi, core, 2) && bi.cpus[3].vcpu_index == 0 &&
           bi.cpus[1].vcpu_index == 1);
    /* The existing modes keep their rules. */
    smt_fixture(&bi, 2, 2);
    assert(amd64_placement(&bi, AMD64_PLACE_PHYSICAL_CORE, 1) && bi.cpus[2].vcpu_index == 0);
    assert(!amd64_placement(&bi, AMD64_PLACE_PHYSICAL_CORE, 2));
    assert(amd64_placement(&bi, AMD64_PLACE_LOGICAL_THREAD, 3));

    /* The rule check must catch broken placements. */
    smt_fixture(&bi, 2, 2);
    assert(amd64_placement(&bi, core, 2));
    bi.cpus[0].vcpu_index = 2; /* vCPU on the management CPU */
    assert(!core_rule_holds(&bi, 3));
    assert(amd64_placement(&bi, core, 2));
    bi.cpus[3].vcpu_index = 2; /* two vCPUs on core 1 */
    assert(!core_rule_holds(&bi, 3));
    assert(amd64_placement(&bi, core, 2));
    bi.cpus[2].vcpu_index = UINT32_MAX; /* a core left without its vCPU */
    assert(!core_rule_holds(&bi, 2));
    /* A physical-core placement is not a core placement: its management core is empty. */
    assert(amd64_placement(&bi, AMD64_PLACE_PHYSICAL_CORE, 1) && core_rule_holds(&bi, 1));
    assert(bi.cpus[1].vcpu_index == UINT32_MAX);

    /* Helpers, ranks as in amd64_helpers: a thread sharing a core
     * with a vCPU, the management core's included, is rank 3. */
    static const unsigned two_core_split[] = {0, 2, 1, 3};
    for (unsigned order = 0; order < 2; ++order) {
        expect_helpers(2, 2, order ? two_core_split : NULL, core, 2, 0, 0, 1);
        expect_helpers(8, 2, order ? split : NULL, core, 8, 0, 0, 7);
    }
    /* Fewer vCPUs than cores: MP order picks the cores, so a vCPU may sit on
     * another core and leave the management core's sibling as a rank-2 helper. */
    expect_helpers(2, 2, NULL, core, 1, 2, 0, 0);
    expect_helpers(2, 2, two_core_split, core, 1, 0, 1, 1);
    expect_helpers(8, 2, NULL, core, 4, 8, 0, 3);
    expect_helpers(8, 2, split, core, 4, 6, 1, 4);
    expect_helpers(8, 2, NULL, core, 1, 14, 0, 0);
    expect_helpers(8, 2, split, core, 1, 12, 1, 1);
    expect_helpers(4, 1, NULL, core, 3, 0, 0, 0);
    expect_helpers(4, 1, NULL, core, 1, 2, 0, 0);
}

#define WRITERS 4u
#define RECORDS 500u
static void *writer(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < RECORDS; ++i) {
        LOGI("writer=%u record=%u", id, i);
        if (!((i + 1) % 128))
            watchdog_progress();
    }
    return NULL;
}
static void concurrent_log_test(void)
{
    const struct log_ring *r = log_get_ring();
    uint64_t start = r->write_pos;
    pthread_t threads[WRITERS];
    for (unsigned i = 0; i < WRITERS; ++i)
        assert(!pthread_create(&threads[i], NULL, writer, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < WRITERS; ++i)
        assert(!pthread_join(threads[i], NULL));
    bool seen[WRITERS][RECORDS] = {0};
    unsigned records = 0;
    for (uint64_t at = start; at < r->write_pos;) {
        char line[128];
        unsigned n = 0, id, record;
        while (at < r->write_pos && n < sizeof(line) - 1) {
            char c = r->data[at++ % LOG_RING_SIZE];
            if (c == '\n')
                break;
            line[n++] = c;
        }
        line[n] = 0;
        assert(sscanf(line, "[info] writer=%u record=%u", &id, &record) == 2);
        assert(id < WRITERS && record < RECORDS && !seen[id][record]);
        seen[id][record] = true;
        ++records;
    }
    assert(records == WRITERS * RECORDS);
}
static void *waker(void *arg)
{
    (void)arg;
    struct timespec ts = {.tv_nsec = 1000000};
    nanosleep(&ts, NULL);
    hal_cpu_request(0, HAL_CPU_WAKE);
    return NULL;
}
static void wake_test(void)
{
    for (unsigned i = 0; i < 50; ++i) {
        hal_cpu_request(0, HAL_CPU_WAKE);
        hal_idle_until(UINT64_MAX); /* notification before sleep */
        assert(hal_cpu_poll() & HAL_CPU_WAKE);
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, waker, NULL));
        /* Spurious wakes and scheduler delays are allowed by hal.h. The
         * watchdog detects a lost wake without a latency requirement. */
        while (!(hal_cpu_poll() & HAL_CPU_WAKE))
            hal_idle_until(UINT64_MAX);
        assert(!pthread_join(thread, NULL));
        watchdog_progress();
    }
    hal_timer_arm(hal_counter() + 1000);
    hal_timer_cancel();
}
/* Intel SDM Vol.2 CPUID: leaf 1 EDX FPU[0] FXSR[24] SSE[25] SSE2[26]; ECX
 * SSE3[0] SSSE3[9] SSE4.1[19] SSE4.2[20] POPCNT[23] XSAVE[26] AVX[28];
 * leaf 0DH subleaf 0 EAX: XCR0 bits the CPU supports (x87 0, SSE 1, AVX 2). */
static const struct {
    unsigned feature;
    bool edx;
    unsigned bit;
} baseline_bits[] = {
    {AMD64_SIMD_FPU, true, 0},     {AMD64_SIMD_FXSR, true, 24},   {AMD64_SIMD_SSE, true, 25},
    {AMD64_SIMD_SSE2, true, 26},   {AMD64_SIMD_SSE3, false, 0},   {AMD64_SIMD_SSSE3, false, 9},
    {AMD64_SIMD_SSE41, false, 19}, {AMD64_SIMD_SSE42, false, 20}, {AMD64_SIMD_POPCNT, false, 23},
    {AMD64_SIMD_XSAVE, false, 26}, {AMD64_SIMD_AVX, false, 28},
};
static void baseline_fixture(uint32_t max, uint32_t ecx, uint32_t edx, uint32_t xcr0_bits)
{
    leaves = 0;
    add(0, 0, max, 0x756e6547, 0, 0);
    add(1, 0, 0, 0, ecx, edx);
    add(0xd, 0, xcr0_bits, 832, 832, 0);
}
static void simd_baseline_tests(void)
{
    uint32_t ecx = 0, edx = 0;
    for (unsigned i = 0; i < sizeof(baseline_bits) / sizeof(*baseline_bits); ++i)
        *(baseline_bits[i].edx ? &edx : &ecx) |= 1u << baseline_bits[i].bit;
    /* Sandy Bridge: also MMX, SSE stepping bits, OSXSAVE clear before setup. */
    baseline_fixture(0xd, ecx | (1u << 1) | (1u << 13), edx | (1u << 23), 7);
    assert(amd64_simd_missing(query) == 0);
    for (unsigned i = 0; i < sizeof(baseline_bits) / sizeof(*baseline_bits); ++i) {
        uint32_t bit = 1u << baseline_bits[i].bit;
        baseline_fixture(0xd, baseline_bits[i].edx ? ecx : ecx & ~bit,
                         baseline_bits[i].edx ? edx & ~bit : edx, 7);
        assert(amd64_simd_missing(query) == baseline_bits[i].feature);
    }
    /* XCR0 cannot hold SSE or AVX state, or leaf 0DH is not enumerated. */
    for (uint32_t bits = 0; bits < 8; ++bits) {
        baseline_fixture(0xd, ecx, edx, bits | 0x2e0);
        assert(amd64_simd_missing(query) == (bits == 7 ? 0 : AMD64_SIMD_XCR0));
    }
    baseline_fixture(0xc, ecx, edx, 7);
    assert(amd64_simd_missing(query) == AMD64_SIMD_XCR0);
    baseline_fixture(0, ecx, edx, 7);
    assert(amd64_simd_missing(query) == AMD64_SIMD_ALL);
    char text[160];
    assert(!strcmp(amd64_simd_describe(AMD64_SIMD_SSE42 | AMD64_SIMD_AVX, text, sizeof(text)),
                   "SSE4.2 AVX"));
    assert(!strcmp(amd64_simd_describe(AMD64_SIMD_XCR0, text, sizeof(text)), "XCR0-AVX-state"));
    amd64_simd_describe(AMD64_SIMD_ALL, text, 12);
    assert(strlen(text) == 11);
    leaves = 0;
}

/* SDM Vol.1 §13.4.1 legacy region (Table 10-2 FXSAVE layout: FCW bytes 1:0,
 * MXCSR bytes 27:24) and §13.4.2 header (XSTATE_BV bytes 519:512, XCOMP_BV
 * 527:520, the rest of the 64 bytes reserved zero); §13.8.1: XRSTOR with
 * XSTATE_BV = 0 loads the initial configuration, MXCSR from memory. */
static void xstate_area_tests(void)
{
    uint8_t area[1024];
    for (unsigned bytes = 0; bytes < AMD64_XSAVE_MIN_BYTES; bytes += 64)
        assert(!amd64_xstate_init(area, bytes));
    memset(area, 0xa5, sizeof(area));
    assert(amd64_xstate_init(area, 832));
    for (unsigned i = 0; i < 832; ++i) {
        uint8_t expected = i == 0 ? 0x7f : i == 1 ? 0x03 : i == 24 ? 0x80 : i == 25 ? 0x1f : 0;
        assert(area[i] == expected);
    }
    for (unsigned i = 832; i < sizeof(area); ++i)
        assert(area[i] == 0xa5);
}

/* SDM Vol.1 §13.3: XCR0 bits are state components, CPUID.(0DH,0):EAX/EDX
 * the ones XSETBV accepts; bits 17-18 are AMX TILECFG/TILEDATA, which the
 * VMM never enables. Everything else supported stays, groups intact. */
static void xcr0_mask_tests(void)
{
    static const struct {
        uint64_t supported, mask;
    } cases[] = {
        {0x7, 0x7},         /* Sandy Bridge: x87 SSE AVX */
        {0x1f, 0x1f},       /* with MPX BNDREGS/BNDCSR */
        {0x2e7, 0x2e7},     /* AVX-512 opmask/ZMM, PKRU */
        {0x602e7, 0x2e7},   /* AMX tiles removed */
        {0xe02e7, 0x802e7}, /* AMX removed, bit 19 kept */
        {(UINT64_C(1) << 32) | 0x60007, (UINT64_C(1) << 32) | 7},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i)
        assert(amd64_xcr0_mask(cases[i].supported) == cases[i].mask);
}

/* SDM Vol.1 §14.5.3 and §14.7.1 detection sequences. */
static uint64_t fake_xcr0;
static unsigned xcr0_reads;
static uint64_t read_fake_xcr0(void)
{
    ++xcr0_reads;
    return fake_xcr0;
}
static void host_simd_tests(void)
{
    for (unsigned avx2 = 0; avx2 < 2; ++avx2)
        for (unsigned fma = 0; fma < 2; ++fma)
            for (unsigned os = 0; os < 4; ++os)
                for (uint64_t xcr0 = 0; xcr0 < 8; ++xcr0)
                    for (uint32_t max = 1; max <= 7; max += 6) {
                        leaves = 0;
                        add(0, 0, max, 0x68747541, 0, 0);
                        add(1, 0, 0, 0,
                            (fma ? 1u << 12 : 0) | (os & 1 ? 1u << 27 : 0) |
                                (os & 2 ? 1u << 28 : 0),
                            0);
                        add(7, 0, 0, avx2 ? 1u << 5 : 0, 0, 0);
                        fake_xcr0 = xcr0 | 0x2e0;
                        xcr0_reads = 0;
                        unsigned got = host_simd_probe(query, read_fake_xcr0);
                        bool enabled = os == 3 && (xcr0 & 6) == 6;
                        unsigned expected = enabled ? (fma ? HOST_SIMD_FMA : 0) |
                                                          (avx2 && max >= 7 ? HOST_SIMD_AVX2 : 0)
                                                    : 0;
                        assert(got == expected);
                        assert((os & 1) || !xcr0_reads); /* XGETBV is #UD without OSXSAVE */
                    }
    leaves = 0;
    add(0, 0, 0, 0x68747541, 0, 0);
    xcr0_reads = 0;
    assert(host_simd_probe(query, read_fake_xcr0) == 0 && xcr0_reads == 0);
    leaves = 0;
}
static void scale_test(void)
{
    const uint64_t values[] = {
        0, 1, 2, 3, 127, 1234567, UINT64_C(1) << 32, UINT64_C(1) << 63, UINT64_MAX};
    for (unsigned i = 0; i < sizeof(values) / sizeof(*values); ++i)
        for (unsigned j = 0; j < sizeof(values) / sizeof(*values); ++j)
            for (unsigned k = 1; k < sizeof(values) / sizeof(*values); ++k)
                for (unsigned ceil = 0; ceil < 2; ++ceil) {
                    __uint128_t product = (__uint128_t)values[i] * values[j];
                    __uint128_t expected = product / values[k];
                    if (ceil && product % values[k])
                        ++expected;
                    if (expected > UINT64_MAX)
                        expected = UINT64_MAX;
                    assert(amd64_scale_ticks(values[i], values[j], values[k], ceil) ==
                           (uint64_t)expected);
                }
}
int main(void)
{
    watchdog_start("foundation", 300);
    topology_tests();
    watchdog_progress();
    helper_tests();
    core_placement_tests();
    watchdog_progress();
    scale_test();
    simd_baseline_tests();
    xstate_area_tests();
    xcr0_mask_tests();
    host_simd_tests();
    watchdog_progress();
    assert(amd64_scale_ticks(10, 3, 4, false) == 7);
    assert(amd64_scale_ticks(10, 3, 4, true) == 8);
    assert(amd64_scale_ticks(UINT64_MAX, UINT64_MAX, UINT64_MAX, true) == UINT64_MAX);
    assert(amd64_scale_ticks(UINT64_MAX, UINT64_MAX, 1, true) == UINT64_MAX);
    assert(amd64_scale_ticks(UINT64_MAX, 2, UINT64_MAX, true) == 2);
    assert(amd64_scale_ticks(123, 456, 0, true) == UINT64_MAX);
    concurrent_log_test();
    wake_test();
    alarm(0);
    puts(
        "test-foundation: CPUID topology, SIMD baseline and dispatch probes, XSAVE area, sparse/SMT placement, helper ranks, concurrent ring writers, futex wake-before/during-wait passed");
    return 0;
}
