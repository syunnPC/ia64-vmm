/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SMP end-to-end checks, BSP
 * side. ipl_main replaces the new firmware's IPL: it plays DXE (publishes the
 * per-CPU areas) and the OS (registers an OS_BOOT_RENDEZ handler, wakes the
 * APs with vector 0xff through the PIB), and checks what every CPU recorded.
 * One scenario per build (SMP_AP_SCENARIO_*):
 *   main:  every AP was held until its wake, entered with its own
 *          register set, ran the handler on its own
 *          area twice (two rendezvous rounds), sent an IPI back, and read an
 *          ITC on the BSP's time base; then a clean shutdown.
 *   nmi:   an NMI starts a held AP (it then halts in the firmware loop), and
 *          0xff later runs the handler.
 *   init:  an INIT to a held AP stops the VM: it has no firmware context.
 *   ptcg:  ptc.g on the BSP completes with one or multiple vCPUs;
 *          with one vCPU it purges and the run ends cleanly.
 *   ptcga: ptc.ga in AP 1's handler completes both rendezvous rounds.
 * A failed check writes its number to the panic register (0x534d50nn) and
 * the value to the debug register; progress goes to the debug register as
 * 0x534dRRAA (RR: round, AA: AP).
 */
#include "firmware/new/ap.h"
#include <ia64vmm/platform.h>

#define MAX_CPUS IA64VMM_VCPU_LIMIT /* the product limit */
/* Above the firmware code page an AP pins with ITR(0) (at most 1 MiB from
 * 0x100000): a local purge over a TR machine checks (SDM Vol.2 Table 4-1). */
#define PURGE_VA UINT64_C(0x200000)
#define SERVICE(offset) (*(volatile uint64_t *)(IA64VMM_SERVICE_BASE + (offset)))
#define UC UINT64_C(0x8000000000000000)
#define PSR_AC (UINT64_C(1) << 3)
#define PSR_IC (UINT64_C(1) << 13)
#define ITC_MS UINT64_C(400000)         /* IA64VMM_ITC_HZ / 1000 */
#define WAIT (UINT64_C(60000) * ITC_MS) /* any bounded wait: 60 s of ITC */
enum entry_word {
    E_R1,
    E_R8,
    E_R9,
    E_R10,
    E_R12,
    E_BSP,
    E_BSPSTORE,
    E_B0,
    E_RSC,
    E_K0,
    E_PSR,
    E_LID,
    E_ITC,
    E_SEEN,
    E_WORDS = 16
};
enum handler_word {
    H_R1,
    H_R12,
    H_B0,
    H_BSP,
    H_BSPSTORE,
    H_K1,
    H_IVA,
    H_TPR,
    H_RSC,
    H_PSR,
    H_LID,
    H_ITC,
    H_WORDS = 16
};
_Static_assert(E_SEEN < E_WORDS && H_ITC < H_WORDS, "slot sizes of smp-ap.S");

extern struct ap_wake_block ap_wake_block;
extern char smp_test_entry[], smp_test_handler[], ap_start[], ivt_base[], __gp[];
void smp_test_ptcg(uint64_t va);
void smp_test_ptcga(uint64_t va);
void ipl_main(const struct ia64vmm_handoff *handoff, uint64_t size, uint64_t id);
void smp_ap_round(uint64_t id);
const struct ia64vmm_handoff *unaligned_handoff;
uint64_t smp_entry_regs[MAX_CPUS][E_WORDS] __attribute__((aligned(64)));
uint64_t smp_handler_regs[MAX_CPUS][H_WORDS] __attribute__((aligned(64)));
/* Written by AP i's smp_ap_round: its rounds and the ITC it read there. */
static volatile uint64_t rounds[MAX_CPUS], ap_itc[MAX_CPUS][2];
/* The per-CPU areas DXE would allocate: fw_data + 16 MiB, above every
 * entry stack and backing store and unused by this image; 32 areas
 * (3.3 MiB) in the image's own .bss would move its short data out of
 * __gp's reach. */
#define AREAS_OFFSET UINT64_C(0x1000000)
_Static_assert(MAX_CPUS *PERCPU_STRIDE <= UINT64_C(0x1000000), "areas inside fw_data");
static uintptr_t areas;

static void fail(unsigned check, uint64_t value)
{
    SERVICE(IA64VMM_SVC_DEBUG) = value;
    SERVICE(IA64VMM_SVC_PANIC) = 0x534d5000u | check;
    for (;;)
        __asm__ volatile("hint @pause");
}
#define CHECK(n, condition, value)                                                                 \
    do {                                                                                           \
        if (!(condition))                                                                          \
            fail(n, value);                                                                        \
    } while (0)
static uint64_t itc(void)
{
    uint64_t value;
    __asm__ volatile("mov %0=ar.itc;;" : "=r"(value)::"memory");
    return value;
}
static uint64_t load_acquire(const volatile uint64_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
/* SDM Vol.2 §5.8.4.2: an IPI is an 8-byte store of mode << 8 | vector to
 * PIB + id << 12 | eid << 4, through the uncacheable physical alias. */
static void ipi(unsigned id, unsigned mode, unsigned vector)
{
    __asm__ volatile("mf;;" ::: "memory");
    *(volatile uint64_t *)(UC | IA64VMM_PIB_BASE | (uint64_t)id << 12) =
        (uint64_t)mode << 8 | vector;
}
static uint64_t ivr(void)
{
    uint64_t vector;
    __asm__ volatile("mov %0=cr.ivr;; srlz.d;;" : "=r"(vector)::"memory");
    return vector;
}
static void eoi(void)
{
    __asm__ volatile("mov cr.eoi=r0;; srlz.d;;" ::: "memory");
}
/* Busy wait of the BSP (PSR.i = 0): the APs run when the run lock passes. */
static void delay(uint64_t ticks)
{
    uint64_t start = itc();
    while (itc() - start < ticks)
        __asm__ volatile("hint @pause" ::: "memory");
}
static void wait_rounds(unsigned cpu, uint64_t round)
{
    uint64_t start = itc();
    while (load_acquire(&rounds[cpu]) < round)
        CHECK(40 + cpu, itc() - start < WAIT, round);
}

/* AP side, called by smp_test_handler on the AP's own stack and backing
 * store. */
void smp_ap_round(uint64_t id)
{
    if (id >= MAX_CPUS)
        fail(30, id);
    uint64_t round = rounds[id] + 1;
    ap_itc[id][round > 1] = itc();
#ifdef SMP_AP_SCENARIO_ptcga
    if (id == 1) {
        smp_test_ptcga(PURGE_VA);
        const char *marker = "SMP AP purge ptc.ga completed cpu=1\n";
        while (*marker)
            SERVICE(IA64VMM_SVC_CONSOLE) = (unsigned char)*marker++;
    }
#endif
    /* IPI back to the BSP (an AP is a sender too). */
    ipi(0, 0, 0xd0 + (unsigned)id);
    __atomic_store_n(&rounds[id], round, __ATOMIC_RELEASE);
}

static void check_entry(const struct ia64vmm_handoff *h, unsigned cpu)
{
    const uint64_t *e = smp_entry_regs[cpu];
    uint64_t data = h->fw_data_base;
    SERVICE(IA64VMM_SVC_DEBUG) = 0x534d4500u | cpu; /* the CPU checks 10-19 concern */
    CHECK(10, load_acquire(&e[E_SEEN]) == 1, cpu);
    CHECK(11, e[E_LID] == IA64VMM_CPU_LID(cpu), e[E_LID]);
    CHECK(12, e[E_R1] == (uintptr_t)__gp, e[E_R1]);
    CHECK(13, e[E_R8] == (uintptr_t)h, e[E_R8]);
    CHECK(14, e[E_R9] == sizeof(*h) && e[E_R10] == IA64VMM_PLATFORM_ID, e[E_R9]);
    /* CPU i's own stack and backing store in fw_data. */
    CHECK(15,
          e[E_R12] == data + IA64VMM_ENTRY_STACK_TOP_OFFSET - 16 - cpu * IA64VMM_ENTRY_STACK_STRIDE,
          e[E_R12]);
    CHECK(16,
          e[E_BSP] == data + IA64VMM_ENTRY_RSE_OFFSET + cpu * IA64VMM_ENTRY_RSE_STRIDE &&
              e[E_BSPSTORE] == e[E_BSP],
          e[E_BSP]);
    CHECK(17, e[E_B0] == (uintptr_t)smp_test_entry, e[E_B0]);
    CHECK(18, e[E_RSC] == 0 && e[E_PSR] == 0, e[E_PSR]);
    CHECK(19, e[E_K0] == IA64VMM_LEGACY_IO_BASE, e[E_K0]);
}
static void check_handler(unsigned cpu)
{
    const uint64_t *s = smp_handler_regs[cpu];
    SERVICE(IA64VMM_SVC_DEBUG) = 0x534d4800u | cpu; /* the CPU checks 20-26 concern */
    uint64_t area = areas + cpu * PERCPU_STRIDE;
    CHECK(20, s[H_LID] == IA64VMM_CPU_LID(cpu), s[H_LID]);
    CHECK(21, s[H_R1] == (uintptr_t)__gp, s[H_R1]);
    CHECK(22, s[H_R12] == area + PERCPU_STACK + PERCPU_STACK_SIZE - 16, s[H_R12]);
    CHECK(23,
          s[H_BSPSTORE] >= area + PERCPU_RSE && s[H_BSP] >= s[H_BSPSTORE] &&
              s[H_BSP] < area + PERCPU_RSE + PERCPU_RSE_SIZE,
          s[H_BSPSTORE]);
    CHECK(24, s[H_K1] == area + PERCPU_CONTEXT && s[H_IVA] == (uintptr_t)ivt_base, s[H_K1]);
    CHECK(25, s[H_RSC] == 0 && s[H_PSR] == (PSR_AC | PSR_IC), s[H_PSR]);
    CHECK(26, s[H_B0] > (uintptr_t)ap_start, s[H_B0]);
}
/* Each AP's IPI back to the BSP (vector 0xd0 + AP) reaches IRR at the
 * BSP's next slice boundary (its kick ends the slice); take them all through
 * IVR, highest first, and EOI each. */
static void take_ipis(unsigned count)
{
    uint64_t want = 0, got = 0, start = itc();
    for (unsigned cpu = 1; cpu < count; ++cpu)
        want |= UINT64_C(1) << cpu;
    while (got != want) {
        uint64_t vector = ivr();
        if (vector != 15) {
            CHECK(27, vector > 0xd0 && vector < 0xd0 + count && !(got >> (vector - 0xd0) & 1),
                  vector);
            got |= UINT64_C(1) << (vector - 0xd0);
            eoi();
        }
        CHECK(50, itc() - start < WAIT, got);
    }
}
/* One rendezvous round: wake every AP at once, wait for each handler. */
static __attribute__((unused)) void round_all(unsigned count, uint64_t round)
{
    uint64_t before = itc();
    for (unsigned cpu = 1; cpu < count; ++cpu)
        ipi(cpu, 0, 0xff);
    for (unsigned cpu = 1; cpu < count; ++cpu) {
        wait_rounds(cpu, round);
        uint64_t after = itc(), at = ap_itc[cpu][round > 1];
        /* The AP read its ITC between the BSP's two reads, on the same
         * time base (held APs keep counting from the VM-wide origin). */
        CHECK(28, at >= before && at <= after, at);
        check_handler(cpu);
        SERVICE(IA64VMM_SVC_DEBUG) = 0x534d0000u | (unsigned)round << 8 | cpu;
    }
    take_ipis(count);
}
static __attribute__((unused)) void shutdown(void)
{
    SERVICE(IA64VMM_SVC_RESET_STATUS) = 0;
    SERVICE(IA64VMM_SVC_RESET) = 2;
    for (;;)
        __asm__ volatile("hint @pause");
}

/* BSP role: _start calls this in place of the new firmware's IPL. */
void ipl_main(const struct ia64vmm_handoff *h, uint64_t size, uint64_t id)
{
    (void)size;
    unaligned_handoff = h;
    const char *identity = "SMP AP fixture " SMP_AP_NAME " BEGIN\n";
    while (*identity)
        SERVICE(IA64VMM_SVC_CONSOLE) = (unsigned char)*identity++;
    CHECK(1, id == IA64VMM_PLATFORM_ID && h->cpu_count >= 1 && h->cpu_count <= MAX_CPUS,
          h->cpu_count);
    unsigned count = h->cpu_count;
    check_entry(h, 0);
#ifdef SMP_AP_SCENARIO_ptcg
    /* ptc.g must complete. */
    smp_test_ptcg(PURGE_VA);
    const char *marker = "SMP AP purge ptc.g completed\n";
    while (*marker)
        SERVICE(IA64VMM_SVC_CONSOLE) = (unsigned char)*marker++;
    shutdown();
#endif
    CHECK(3, count >= 2, count);
    /* No AP has executed anything before its wake. Give an AP
     * that was not held time to run its entry. */
    delay(20 * ITC_MS);
    for (unsigned cpu = 1; cpu < count; ++cpu)
        CHECK(4, !load_acquire(&smp_entry_regs[cpu][E_SEEN]), cpu);
    /* What DXE (areas) and the OS (SAL_SET_VECTORS) do before the wake. */
    areas = (uintptr_t)(h->fw_data_base + AREAS_OFFSET);
    ap_publish(&ap_wake_block, areas, count);
    ap_register(&ap_wake_block, (uintptr_t)smp_test_handler, (uintptr_t)__gp);
#ifdef SMP_AP_SCENARIO_init
    /* A held AP takes no INIT; the VM stops. */
    ipi(1, 5, 0);
    delay(5000 * ITC_MS);
    fail(5, 1);
#endif
#ifdef SMP_AP_SCENARIO_nmi
    /* An NMI starts a held AP at the firmware entry; the loop
     * reads IVR 2, EOIs it and halts. 0xff then runs the handler. */
    ipi(1, 4, 0);
    uint64_t start = itc();
    while (!load_acquire(&smp_entry_regs[1][E_SEEN]))
        CHECK(6, itc() - start < WAIT, 1);
    check_entry(h, 1);
    delay(20 * ITC_MS);
    CHECK(7, rounds[1] == 0, rounds[1]);
    round_all(2, 1);
    shutdown();
#endif
#if defined(SMP_AP_SCENARIO_main) || defined(SMP_AP_SCENARIO_ptcga)
    round_all(count, 1);
    for (unsigned cpu = 1; cpu < count; ++cpu)
        check_entry(h, cpu);
    /* Repeat rendezvous: each AP returned into its firmware loop, halted,
     * and wakes again on 0xff. */
    round_all(count, 2);
    for (unsigned cpu = 1; cpu < count; ++cpu)
        CHECK(9, rounds[cpu] == 2, rounds[cpu]);
    shutdown();
#endif
    fail(60, 0);
}
