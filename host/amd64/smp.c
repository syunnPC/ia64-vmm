/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/amd64.h"
#include "host/amd64/smp.h"
#include "ia64vmm/host_isa.h"
#include "ia64vmm/host_simd.h"
#include "host/amd64/simd.h"
#include "host/amd64/topology.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

struct amd64_cpu *amd64_cpus;
unsigned amd64_cpu_count;
unsigned amd64_idle_state = HOST_IDLE_HALT;
static unsigned common_host_isa, common_host_simd;
unsigned hal_host_isa(void)
{
    return __atomic_load_n(&common_host_isa, __ATOMIC_ACQUIRE);
}
unsigned hal_host_simd(void)
{
    return __atomic_load_n(&common_host_simd, __ATOMIC_ACQUIRE);
}
static struct boot_info *inventory;
static unsigned ready;
static bool gs_enabled;
/* Advanced only by amd64_tlb_shootdown, which memory.c serializes. */
static unsigned tlb_generation;
#define TLB_BATCH 64u
static uint64_t tlb_addresses[TLB_BATCH];
static unsigned tlb_count;
static void tlb_flush_all(void)
{
    /* Upper-half leaves are global. VMM overflow/lazy shootdowns
     * must flush those too, unlike a window-only CR3 reload. */
    uint64_t cr4, cr3;
    __asm__ volatile("mov %%cr3,%0; mov %0,%%cr3" : "=r"(cr3)::"memory");
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    __asm__ volatile("mov %0,%%cr4; mov %1,%%cr4" ::"r"(cr4 & ~UINT64_C(128)), "r"(cr4) : "memory");
}
/* Generation order, wrap-safe. */
static bool tlb_behind(unsigned ack, unsigned generation)
{
    return (int)(generation - ack) > 0;
}
void amd64_tlb_note(uint64_t va)
{
    /* Called only for present -> changed/not-present PTEs (a not-present PTE
     * is never cached, SDM Vol.3 §4.10.2.3). memory.c owns the allocator
     * lock until every receiver acknowledges. */
    if (tlb_count < TLB_BATCH)
        tlb_addresses[tlb_count++] = va;
    else
        tlb_count = TLB_BATCH + 1;
}

unsigned hal_cpu_index(void)
{
    unsigned index = 0;
    if (__atomic_load_n(&gs_enabled, __ATOMIC_ACQUIRE))
        __asm__ volatile("movl %%gs:0,%0" : "=r"(index));
    return index;
}
struct amd64_cpu *amd64_current_cpu(void)
{
    return &amd64_cpus[hal_cpu_index()];
}
bool amd64_smp_ready(void)
{
    return __atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 0;
}

void amd64_smp_prepare(struct boot_info *bi)
{
    unsigned mask;
    if (!host_isa_parse_mask(bi->cmdline, &mask))
        hal_panic("invalid host_isa_mask (decimal 0..15)");
    inventory = bi;
    amd64_cpu_count = bi->host_cpu_count;
    amd64_cpus = hal_alloc_pages(
        IA64VMM_ALIGN_UP(amd64_cpu_count * sizeof(*amd64_cpus), 4096) / 4096, HAL_PAGE_ZERO);
    if (!amd64_cpus)
        hal_panic("cannot allocate host CPU mailboxes");
    for (unsigned i = 0; i < amd64_cpu_count; ++i) {
        struct amd64_cpu *cpu = &amd64_cpus[i];
        cpu->index = i;
        cpu->root = amd64_memory_private_root();
        cpu->apic_id = bi->cpus[i].apic_id;
        void *stack = hal_alloc_pages(16, HAL_PAGE_ZERO);
        if (!stack)
            hal_panic("cannot allocate host CPU stacks");
        cpu->stack_top = (uint8_t *)stack + 65536;
        cpu->timer_deadline = UINT64_MAX;
        LOGI("relocation CPU%u stack=%016llx..%016llx", i,
             (unsigned long long)((uintptr_t)stack - amd64_hhdm),
             (unsigned long long)((uintptr_t)cpu->stack_top - amd64_hhdm));
    }
}
void *amd64_smp_stack(unsigned index)
{
    return amd64_cpus[index].stack_top;
}
void amd64_smp_online(unsigned index)
{
    struct amd64_cpu *cpu = &amd64_cpus[index];
    amd64_wrmsr(0xc0000101, (uintptr_t)cpu);
    __atomic_store_n(&gs_enabled, true, __ATOMIC_RELEASE);
    amd64_cpu_init_index(index, cpu->stack_top);
    cpu->host_isa = host_isa_probe(amd64_cpuid);
    cpu->host_simd = host_simd_probe(amd64_cpuid, host_simd_xcr0);
    /* Reassert the CPU-local base after descriptor setup. */
    amd64_wrmsr(0xc0000101, (uintptr_t)cpu);
    amd64_time_verify_cpu();
    amd64_apic_init(index);
    __asm__ volatile("mov %0,%%cr3" ::"r"(cpu->root) : "memory");
    if (!amd64_topology(&inventory->cpus[index], amd64_cpuid))
        LOGW("CPU%u APIC=%u: CPUID physical topology unavailable", index, cpu->apic_id);
    __asm__ volatile("sti" ::: "memory");
    __atomic_store_n(&cpu->ready, 1, __ATOMIC_RELEASE);
    if (index)
        hal_cpu_request(0, HAL_CPU_WAKE);
}
void amd64_smp_wait(void)
{
    uint64_t until = hal_counter() + hal_counter_hz() * 10;
    for (unsigned i = 0; i < amd64_cpu_count; ++i)
        while (!__atomic_load_n(&amd64_cpus[i].ready, __ATOMIC_ACQUIRE)) {
            if (hal_counter() >= until)
                hal_panic("AP did not leave Limine before reclaim");
            hal_cpu_poll();
            enum hal_shutdown_kind kind;
            int code;
            if (hal_shutdown_requested(&kind, &code))
                hal_shutdown(kind, code);
            if (!__atomic_load_n(&amd64_cpus[i].ready, __ATOMIC_ACQUIRE))
                hal_idle_until(until);
        }
    struct host_isa_cpu_report reports[BOOT_MAX_CPUS];
    for (unsigned i = 0; i < amd64_cpu_count; ++i) {
        reports[i].reported = __atomic_load_n(&amd64_cpus[i].ready, __ATOMIC_ACQUIRE);
        reports[i].features = reports[i].reported ? amd64_cpus[i].host_isa : 0;
    }
    unsigned features;
    if (host_isa_resolve(inventory->cmdline, reports, amd64_cpu_count, &features) != 1)
        hal_panic("host ISA publication requires every CPU report and a valid mask");
    __atomic_store_n(&common_host_isa, features, __ATOMIC_RELEASE);
    LOGI("host scalar ISA=%x (intersection of %u CPUs)", features, amd64_cpu_count);
    unsigned simd = HOST_SIMD_ALL;
    for (unsigned i = 0; i < amd64_cpu_count; ++i)
        simd &= amd64_cpus[i].host_simd;
    __atomic_store_n(&common_host_simd, simd, __ATOMIC_RELEASE);
    LOGI(
        "host SIMD baseline SSE4.2 POPCNT AVX, XCR0=%llx XSAVE area %u bytes; dispatch AVX2=%u FMA=%u "
        "(intersection of %u CPUs)",
        (unsigned long long)host_simd_xcr0(), amd64_xstate_bytes, (simd & HOST_SIMD_AVX2) != 0,
        (simd & HOST_SIMD_FMA) != 0, amd64_cpu_count);
    __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
}
void hal_cpu_request(unsigned index, unsigned requests)
{
    if (!amd64_cpus || index >= amd64_cpu_count)
        return;
    struct amd64_cpu *cpu = &amd64_cpus[index];
    if (requests & HAL_CPU_STOP)
        __atomic_store_n(&cpu->stop, 1, __ATOMIC_RELEASE);
    /* Only a halted CPU needs the IPI (host_mailbox_post). */
    if (host_mailbox_post(&cpu->mailbox, requests) &&
        __atomic_load_n(&cpu->ready, __ATOMIC_ACQUIRE))
        amd64_send_ipi(index);
}
void hal_cpu_wake(unsigned index)
{
    hal_cpu_request(index, HAL_CPU_WAKE);
}
unsigned hal_cpu_poll(void)
{
    if (!amd64_cpus)
        return 0;
    return __atomic_exchange_n(&amd64_current_cpu()->mailbox.requests, 0, __ATOMIC_ACQ_REL);
}
bool hal_cpu_busy(unsigned index)
{
    return index < amd64_cpu_count && __atomic_load_n(&amd64_cpus[index].busy, __ATOMIC_ACQUIRE);
}
bool hal_cpu_start(unsigned index, hal_cpu_work_fn work, void *arg)
{
    if (!amd64_smp_ready() || hal_cpu_index() != 0 || index == 0 || index >= amd64_cpu_count ||
        !work || __atomic_load_n(&amd64_cpus[index].stop, __ATOMIC_ACQUIRE))
        return false;
    struct amd64_cpu *cpu = &amd64_cpus[index];
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&cpu->busy, &expected, 1, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return false;
    cpu->work_arg = arg;
    __atomic_store_n(&cpu->work, work, __ATOMIC_RELEASE);
    hal_cpu_request(index, HAL_CPU_WAKE);
    return true;
}
unsigned hal_vcpu_cpu(unsigned vcpu)
{
    if (!inventory)
        return UINT32_MAX;
    for (unsigned i = 0; i < amd64_cpu_count; ++i)
        if (inventory->cpus[i].vcpu_index == vcpu)
            return i;
    return UINT32_MAX;
}
unsigned hal_vcpu_count(void)
{
    return inventory ? inventory->vcpu_count : 0;
}
/* Written once on CPU 0 after placement, before any callback reads them. */
static unsigned helper_count, helper_cpu[BOOT_MAX_CPUS], helper_rank[BOOT_MAX_CPUS];
void amd64_helpers_init(void)
{
    helper_count = amd64_helpers(inventory, helper_cpu, helper_rank);
}
unsigned hal_helper_count(void)
{
    return helper_count;
}
unsigned hal_helper_cpu(unsigned i)
{
    return i < helper_count ? helper_cpu[i] : UINT32_MAX;
}
unsigned hal_helper_rank(unsigned i)
{
    return i < helper_count ? helper_rank[i] : 0;
}

static void window_receive(struct amd64_cpu *cpu)
{
    unsigned request = __atomic_load_n(&cpu->window_request, __ATOMIC_ACQUIRE);
    if (request == __atomic_load_n(&cpu->window_ack, __ATOMIC_RELAXED))
        return;
    uint64_t address = __atomic_load_n(&cpu->window_address, __ATOMIC_ACQUIRE);
    if (address == UINTPTR_MAX) {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3,%0; mov %0,%%cr3" : "=r"(cr3)::"memory");
    } else
        amd64_invlpg(address);
    __atomic_store_n(&cpu->window_ack, request, __ATOMIC_RELEASE);
}
void amd64_window_lock(unsigned *lock, unsigned target, const char *what)
{
    uint64_t until = hal_counter() + hal_counter_hz() * 3;
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {
        if (hal_counter() >= until) {
            LOGE("host window wait timed out: %s issuer CPU%u target CPU%u", what, hal_cpu_index(),
                 target);
            hal_panic("host window wait timed out");
        }
        __asm__ volatile("pause");
    }
}
void amd64_window_flush(unsigned index, uint64_t address, bool sync)
{
    struct amd64_cpu *cpu = &amd64_cpus[index];
    /* All window mutations finish synchronously, including idle targets.
     * Keep the interface's sync argument; there is no unused lazy no-IPI
     * path. Receivers never acquire the publisher lock. */
    (void)sync;
    amd64_window_lock(&cpu->window_lock, index, "mailbox publisher lock");
    if (__atomic_load_n(&cpu->stopped, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&cpu->window_lock, 0, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&cpu->window_address, address, __ATOMIC_RELEASE);
    unsigned request = __atomic_add_fetch(&cpu->window_request, 1, __ATOMIC_RELEASE);
    if (index == hal_cpu_index())
        window_receive(cpu);
    else
        amd64_send_ipi(index);
    uint64_t until = hal_counter() + hal_counter_hz() * 3;
    while (__atomic_load_n(&cpu->window_ack, __ATOMIC_ACQUIRE) != request) {
        if (__atomic_load_n(&cpu->stopped, __ATOMIC_ACQUIRE))
            break;
        if (hal_counter() >= until) {
            LOGE(
                "host window wait timed out: acknowledgement issuer CPU%u target CPU%u request=%u ack=%u",
                hal_cpu_index(), index, request,
                __atomic_load_n(&cpu->window_ack, __ATOMIC_ACQUIRE));
            hal_panic("host window wait timed out");
        }
        __asm__ volatile("pause");
    }
    __atomic_store_n(&cpu->window_lock, 0, __ATOMIC_RELEASE);
}
void amd64_ipi_received(void)
{
    struct amd64_cpu *cpu = amd64_current_cpu();
    __atomic_fetch_add(&cpu->ipi_irqs, 1, __ATOMIC_RELAXED);
    window_receive(cpu);
    unsigned request = __atomic_load_n(&cpu->tlb_request, __ATOMIC_ACQUIRE);
    /* A lazy wakeup may already have flushed past this request. */
    if (tlb_behind(__atomic_load_n(&cpu->tlb_ack, __ATOMIC_RELAXED), request)) {
        if (tlb_count > TLB_BATCH) {
            tlb_flush_all();
        } else {
            /* SDM Vol.3 §4.10.4.1: INVLPG also discards every translation
             * for a large page containing this address when it was split. */
            for (unsigned i = 0; i < tlb_count; ++i)
                amd64_invlpg(tlb_addresses[i]);
        }
        __atomic_store_n(&cpu->tlb_ack, request, __ATOMIC_RELEASE);
    }
}
void amd64_tlb_lazy_enter(struct amd64_cpu *cpu)
{
    __atomic_store_n(&cpu->tlb_lazy, 1, __ATOMIC_SEQ_CST);
}
void amd64_tlb_lazy_leave(struct amd64_cpu *cpu)
{
    /* Dekker pairing with amd64_tlb_shootdown: this CPU stores lazy=0 then
     * loads the generation; the issuer advances the generation then loads
     * lazy. Either the issuer sees this CPU active and sends an IPI, or this
     * load sees the new generation and flushes here (or both). */
    __atomic_store_n(&cpu->tlb_lazy, 0, __ATOMIC_SEQ_CST);
    unsigned generation = __atomic_load_n(&tlb_generation, __ATOMIC_SEQ_CST);
    if (tlb_behind(__atomic_load_n(&cpu->tlb_ack, __ATOMIC_RELAXED), generation)) {
        tlb_flush_all();
        __atomic_store_n(&cpu->tlb_ack, generation, __ATOMIC_RELEASE);
    }
}
void amd64_tlb_shootdown(void)
{
    /* Only not-present -> present changes: nothing can be cached anywhere. */
    if (!tlb_count)
        return;
    if (!amd64_smp_ready()) {
        tlb_count = 0;
        return;
    }
    /* memory.c serializes issuers and has already invalidated locally with
     * INVLPG. The IRQ receiver invalidates and acknowledges; it never
     * acquires the allocator lock. Lazy (idle) CPUs are not
     * interrupted: they flush when they leave idle. */
    unsigned sequence = __atomic_add_fetch(&tlb_generation, 1, __ATOMIC_SEQ_CST);
    unsigned self = hal_cpu_index();
    __atomic_store_n(&amd64_cpus[self].tlb_ack, sequence, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < amd64_cpu_count; ++i) {
        struct amd64_cpu *cpu = &amd64_cpus[i];
        if (i == self || __atomic_load_n(&cpu->stopped, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&cpu->tlb_lazy, __ATOMIC_SEQ_CST))
            continue;
        __atomic_store_n(&cpu->tlb_request, sequence, __ATOMIC_RELEASE);
        amd64_send_ipi(i);
    }
    uint64_t until = hal_counter() + hal_counter_hz() * 3;
    for (unsigned i = 0; i < amd64_cpu_count; ++i) {
        struct amd64_cpu *cpu = &amd64_cpus[i];
        if (i == self || __atomic_load_n(&cpu->tlb_request, __ATOMIC_ACQUIRE) != sequence)
            continue;
        while (!__atomic_load_n(&cpu->stopped, __ATOMIC_ACQUIRE) &&
               tlb_behind(__atomic_load_n(&cpu->tlb_ack, __ATOMIC_ACQUIRE), sequence)) {
            if (hal_counter() >= until)
                hal_panic("host TLB shootdown timed out");
            __asm__ volatile("pause");
        }
    }
    tlb_count = 0;
}
void amd64_cpu_loop(void *arg)
{
    unsigned index = (unsigned)(uintptr_t)arg;
    amd64_smp_online(index);
    struct amd64_cpu *cpu = &amd64_cpus[index];
    for (;;) {
        if (__atomic_load_n(&cpu->stop, __ATOMIC_ACQUIRE)) {
            hal_timer_cancel();
            __asm__ volatile("cli" ::: "memory");
            __atomic_store_n(&cpu->stopped, 1, __ATOMIC_RELEASE);
            for (;;)
                __asm__ volatile("hlt");
        }
        hal_cpu_poll();
        hal_cpu_work_fn work = __atomic_exchange_n(&cpu->work, NULL, __ATOMIC_ACQ_REL);
        if (work) {
            work(cpu->work_arg);
            hal_timer_cancel();
            __atomic_store_n(&cpu->busy, 0, __ATOMIC_RELEASE);
            hal_cpu_request(0, HAL_CPU_WAKE);
        } else
            hal_idle_until(UINT64_MAX);
    }
}
bool amd64_smp_stop(void)
{
    if (!amd64_smp_ready())
        return true;
    for (unsigned i = 1; i < amd64_cpu_count; ++i)
        hal_cpu_request(i, HAL_CPU_STOP);
    uint64_t until = hal_counter() + hal_counter_hz() * 3;
    for (unsigned i = 1; i < amd64_cpu_count; ++i)
        while (!__atomic_load_n(&amd64_cpus[i].stopped, __ATOMIC_ACQUIRE)) {
            if (hal_counter() >= until) {
                static const char error[] =
                    "[error] cooperative stop timed out; emergency NMI stop (state may be incomplete)\n";
                hal_debug_write(error, sizeof(error) - 1);
                for (unsigned j = 1; j < amd64_cpu_count; ++j)
                    if (!__atomic_load_n(&amd64_cpus[j].stopped, __ATOMIC_ACQUIRE))
                        amd64_send_nmi(j);
                uint64_t emergency = hal_counter() + hal_counter_hz();
                for (unsigned j = 1; j < amd64_cpu_count; ++j)
                    while (!__atomic_load_n(&amd64_cpus[j].stopped, __ATOMIC_ACQUIRE)) {
                        if (hal_counter() >= emergency) {
                            static const char failure[] =
                                "[error] emergency CPU stop failed; host halted\n";
                            hal_debug_write(failure, sizeof(failure) - 1);
                            for (;;)
                                __asm__ volatile("cli; hlt");
                        }
                        __asm__ volatile("pause");
                    }
                return false;
            }
            /* A panic may have entered with IF=0; service peer TLB requests
             * while waiting, so no sender lock is needed by this receiver. */
            __asm__ volatile("sti; pause" ::: "memory");
        }
    return true;
}

struct ipi_probe {
    unsigned target, done;
    uint64_t sample, irqs;
};
static void probe(void *arg)
{
    struct ipi_probe *p = arg;
    struct amd64_cpu *cpu = amd64_current_cpu();
    if (hal_cpu_index() != p->target)
        hal_panic("CPU callback dispatched to wrong CPU");
    hal_cpu_poll();
    uint64_t before = __atomic_load_n(&cpu->ipi_irqs, __ATOMIC_RELAXED);
    __atomic_store_n(&p->done, 1, __ATOMIC_RELEASE);
    /* Management sends the test IPI only after this readiness handshake. */
    uint64_t until = hal_counter() + hal_counter_hz() * 3;
    for (;;) {
        hal_cpu_poll();
        if (__atomic_load_n(&cpu->ipi_irqs, __ATOMIC_RELAXED) != before)
            break;
        if (hal_counter() >= until)
            hal_panic("IPI selftest target delivery timed out");
        hal_idle_until(until);
    }
    p->sample = hal_counter();
    p->irqs = __atomic_load_n(&cpu->ipi_irqs, __ATOMIC_RELAXED) - before;
    __atomic_store_n(&p->done, 2, __ATOMIC_RELEASE);
}
void amd64_ipi_selftest(void)
{
    if (amd64_cpu_count < 2)
        hal_panic("IPI selftest requires two CPUs");
    for (unsigned i = 1; i < amd64_cpu_count; ++i) {
        struct ipi_probe p = {.target = i};
        if (!hal_cpu_start(i, probe, &p))
            hal_panic("cannot dispatch IPI selftest");
        uint64_t until = hal_counter() + hal_counter_hz() * 3;
        while (__atomic_load_n(&p.done, __ATOMIC_ACQUIRE) < 1) {
            if (hal_counter() >= until)
                hal_panic("IPI selftest readiness timed out");
            __asm__ volatile("pause");
        }
        uint64_t before = hal_counter();
        /* An IPI whatever the target's idle state. */
        __atomic_fetch_or(&amd64_cpus[i].mailbox.requests, HAL_CPU_WAKE, __ATOMIC_SEQ_CST);
        amd64_send_ipi(i);
        while (__atomic_load_n(&p.done, __ATOMIC_ACQUIRE) < 2 || hal_cpu_busy(i)) {
            if (hal_counter() >= until)
                hal_panic("IPI selftest delivery timed out");
            __asm__ volatile("pause");
        }
        uint64_t after = hal_counter();
        if (!p.irqs || p.sample < before || p.sample > after) {
            LOGE("IPI selftest CPU%u irqs=%llu before=%llu sample=%llu after=%llu", i,
                 (unsigned long long)p.irqs, (unsigned long long)before,
                 (unsigned long long)p.sample, (unsigned long long)after);
            hal_panic("IPI delivery / cross-CPU TSC consistency failed");
        }
        LOGI(
            "IPI selftest CPU0 -> CPU%u APIC=%u acknowledged, TSC bracket=%llu ticks latency<=%llu us",
            i, amd64_cpus[i].apic_id, (unsigned long long)(after - before),
            (unsigned long long)((after - before) / (hal_counter_hz() / 1000000)));
    }
    LOGI("host IPI selftest passed");
}

/* One AP waits with MONITOR/MWAIT on its mailbox, as hal_idle_until does,
 * until a store arrives or its timer bounds the wait. */
struct mwait_probe {
    unsigned phase, returns;
    const char *why;
    uint64_t woke, timers, ipis;
};
static void mwait_probe(void *arg)
{
    struct mwait_probe *p = arg;
    struct amd64_cpu *cpu = amd64_current_cpu();
    hal_cpu_poll();
    p->why = host_mwait_unusable(amd64_cpuid);
    if (!p->why) {
        uint64_t timers = cpu->timer_irqs, ipis = cpu->ipi_irqs;
        uint64_t until = hal_counter() + hal_counter_hz() / 10;
        __asm__ volatile("cli" ::: "memory");
        hal_timer_arm(until);
        __atomic_store_n(&p->phase, 1, __ATOMIC_SEQ_CST);
        for (;;) {
            amd64_monitor(&cpu->mailbox.requests);
            if (__atomic_load_n(&cpu->mailbox.requests, __ATOMIC_SEQ_CST) || hal_counter() >= until)
                break;
            __asm__ volatile("sti; mwait; cli" ::"a"(0), "c"(0) : "memory");
            ++p->returns;
        }
        p->woke = hal_counter();
        hal_timer_cancel();
        __asm__ volatile("sti" ::: "memory");
        p->timers = cpu->timer_irqs - timers;
        p->ipis = cpu->ipi_irqs - ipis;
        hal_cpu_poll();
    }
    __atomic_store_n(&p->phase, 2, __ATOMIC_RELEASE);
}
/* A hypervisor may enumerate MONITOR yet return from MWAIT at once or
 * ignore stores: each AP must sleep until the store (at most a few early
 * returns), wake within 1 ms of it, and take neither its timer nor an IPI. */
void amd64_idle_selftest(bool monitor)
{
    static char reason[96];
    const char *why = monitor ? host_mwait_unusable(amd64_cpuid) : "host_mwait=off";
    uint64_t hz = hal_counter_hz();
    for (unsigned i = 1; !why && i < amd64_cpu_count; ++i) {
        struct mwait_probe p = {0};
        if (!hal_cpu_start(i, mwait_probe, &p))
            hal_panic("cannot dispatch MWAIT selftest");
        uint64_t until = hal_counter() + hz;
        while (!__atomic_load_n(&p.phase, __ATOMIC_ACQUIRE)) {
            if (hal_counter() >= until)
                hal_panic("MWAIT selftest readiness timed out");
            __asm__ volatile("pause");
        }
        uint64_t before = UINT64_MAX;
        if (__atomic_load_n(&p.phase, __ATOMIC_ACQUIRE) == 1) {
            uint64_t settle = hal_counter() + hz / 5000;
            while (hal_counter() < settle)
                __asm__ volatile("pause");
            before = hal_counter();
            __atomic_fetch_or(&amd64_cpus[i].mailbox.requests, HAL_CPU_WAKE, __ATOMIC_SEQ_CST);
        }
        while (__atomic_load_n(&p.phase, __ATOMIC_ACQUIRE) < 2 || hal_cpu_busy(i)) {
            if (hal_counter() >= until)
                hal_panic("MWAIT selftest timed out");
            __asm__ volatile("pause");
        }
        if (p.why) {
            vmm_snprintf(reason, sizeof(reason), "CPU%u: %s", i, p.why);
            why = reason;
        } else if (p.timers || p.ipis || p.returns > 4 || p.woke < before ||
                   p.woke - before > hz / 1000) {
            vmm_snprintf(
                reason, sizeof(reason),
                "CPU%u MWAIT: %u early returns, timer=%llu IPI=%llu, wake after %lld ticks", i,
                p.returns, (unsigned long long)p.timers, (unsigned long long)p.ipis,
                (long long)(p.woke - before));
            why = reason;
        } else
            LOGI("MWAIT selftest CPU%u: woke %llu ns after a mailbox store, %u early returns", i,
                 (unsigned long long)((p.woke - before) * 1000000000 / hz), p.returns);
    }
    if (why) {
        LOGI("host idle: HLT, woken by IPI (%s)", why);
        return;
    }
    __atomic_store_n(&amd64_idle_state, HOST_IDLE_MONITOR, __ATOMIC_RELEASE);
    LOGI("host idle: MONITOR/MWAIT (C1) on the mailbox line; posts to idle CPUs need no IPI");
}
