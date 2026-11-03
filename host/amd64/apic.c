/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Intel SDM Vol.3, local APIC and TSC-deadline timer. Host IRQ paths are
 * integer-only; they publish wake bits, acknowledge hardware and return. */
#include "host/amd64/amd64.h"
#include "host/amd64/smp.h"
#include "host/amd64/time_math.h"
#include "host/amd64/hyperv.h"
#include "ia64vmm/log.h"
#include "ia64vmm/interrupts.h"
/* The explicit host-only smoke image omits the guest platform. */
extern void platform_irq_set_host(const struct platform_host_irq_ops *) __attribute__((weak));
extern bool platform_msi_interrupt(unsigned) __attribute__((weak));
extern bool platform_intx_interrupt(unsigned) __attribute__((weak));
static bool device_message(uint16_t, unsigned, unsigned, unsigned, uint64_t *, uint32_t *);
extern void vmm_profile_sample(unsigned cpu, uint64_t rip) __attribute__((weak));

#define VECTOR_TIMER 0xe0u
#define VECTOR_IRQ_RETIRE 0x30u
#define VECTOR_IPI 0xe1u
#define VECTOR_PROFILE 0xe2u
#define VECTOR_ERROR 0xfeu
#define VECTOR_SPURIOUS 0xffu
#define APIC_MASK (1u << 16)
_Static_assert(VECTOR_IRQ_RETIRE / 16 < VMM_IRQ_FIRST_VECTOR / 16, "IRQ retirement priority");
static bool x2apic;
static volatile uint32_t *mmio;
bool amd64_apic_extended(void)
{
    return x2apic;
}

static uint32_t apic_read(unsigned reg)
{
    return x2apic ? (uint32_t)amd64_rdmsr(0x800 + (reg >> 4)) : mmio[reg / 4];
}
static void apic_write(unsigned reg, uint32_t value)
{
    if (x2apic)
        amd64_wrmsr(0x800 + (reg >> 4), value);
    else
        mmio[reg / 4] = value;
}
static void eoi(void)
{
    apic_write(0xb0, 0);
}

void amd64_apic_init(unsigned index)
{
    uint32_t a, b, c, d;
    amd64_cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 9)))
        hal_panic("local APIC is required");
    uint64_t base = amd64_rdmsr(0x1b);
    if (!index) {
        static const struct platform_host_irq_ops direct = {
            .message = device_message,
            .retire = amd64_irq_retire,
        };
        if (platform_irq_set_host)
            platform_irq_set_host(&direct);
        x2apic = (c & (1u << 21)) != 0;
        if (!x2apic) {
            uint64_t pa = base & UINT64_C(0x000ffffffffff000);
            amd64_map_apic(pa);
            mmio = (volatile uint32_t *)(uintptr_t)(amd64_hhdm + pa);
        }
        /* The VMM owns interrupt delivery. No legacy PIC source is routed
         * to the host; guest interrupt conversion is separate. */
        hal_port_out(0x21, 0xff, 1);
        hal_port_out(0xa1, 0xff, 1);
    }
    if (x2apic && !(c & (1u << 21)))
        hal_panic("inconsistent x2APIC feature across CPUs");
    if (!x2apic && (base & (1u << 10)))
        hal_panic("Limine enabled unsupported x2APIC mode");
    amd64_wrmsr(0x1b, base | (1u << 11) | (x2apic ? 1u << 10 : 0));
    uint32_t id = apic_read(0x20);
    if (!x2apic)
        id >>= 24;
    if (id != amd64_cpus[index].apic_id)
        hal_panic("MP/APIC identity mismatch");
    apic_write(0x80, 0);
    apic_write(0xf0, (1u << 8) | VECTOR_SPURIOUS);
    unsigned lvt_max = (apic_read(0x30) >> 16) & 255;
    apic_write(0x320, APIC_MASK | VECTOR_TIMER);
    apic_write(0x350, APIC_MASK);
    apic_write(0x360, APIC_MASK);
    if (lvt_max >= 4)
        apic_write(0x340, APIC_MASK);
    if (lvt_max >= 5)
        apic_write(0x330, APIC_MASK);
    if (lvt_max >= 6)
        apic_write(0x2f0, APIC_MASK);
    apic_write(0x280, 0);
    (void)apic_read(0x280);
    apic_write(0x370, VECTOR_ERROR);
    struct amd64_cpu *cpu = &amd64_cpus[index];
    cpu->deadline_timer = c & (1u << 24);
    if (cpu->deadline_timer) {
        apic_write(0x320, (2u << 17) | VECTOR_TIMER);
        /* Serialize the xAPIC MMIO LVT write before the deadline MSR. */
        __asm__ volatile("mfence" ::: "memory");
        amd64_wrmsr(0x6e0, 0);
    } else {
        apic_write(0x3e0, 3); /* divide by 16 */
        apic_write(0x380, UINT32_MAX);
        uint64_t start = hal_counter(), span = hal_counter_hz() / 50;
        while (hal_counter() - start < span)
            __asm__ volatile("pause");
        uint32_t ticks = UINT32_MAX - apic_read(0x390);
        uint64_t elapsed = hal_counter() - start;
        apic_write(0x380, 0);
        cpu->timer_hz = (uint64_t)ticks * hal_counter_hz() / elapsed;
        if (!cpu->timer_hz)
            hal_panic("LAPIC one-shot calibration failed");
    }
    cpu->timer_cancelled = true;
    LOGI("CPU%u APIC=%u mode=%s timer=%s calibrated_hz=%llu", index, id,
         x2apic ? "x2APIC" : "xAPIC", cpu->deadline_timer ? "TSC-deadline" : "LAPIC one-shot",
         (unsigned long long)cpu->timer_hz);
}
void hal_timer_cancel(void)
{
    if (!amd64_cpus)
        return;
    struct amd64_cpu *cpu = amd64_current_cpu();
    /* A pending IRQ can clear timer_deadline after a new arm. Only an
     * explicit hardware stop proves that cancellation is redundant. */
    if (cpu->timer_cancelled)
        return;
    if (cpu->deadline_timer)
        amd64_wrmsr(0x6e0, 0);
    else {
        apic_write(0x320, APIC_MASK | VECTOR_TIMER);
        apic_write(0x380, 0);
    }
    cpu->timer_deadline = UINT64_MAX;
    cpu->timer_cancelled = true;
}
void hal_timer_arm(uint64_t deadline)
{
    if (deadline == UINT64_MAX) {
        hal_timer_cancel();
        return;
    }
    struct amd64_cpu *cpu = amd64_current_cpu();
    uint64_t now = hal_counter();
    /* Still armed for this deadline and not yet fired (the IRQ disarms the
     * record): reprogramming would only cost APIC/MSR writes. */
    if (deadline > now && __atomic_load_n(&cpu->timer_deadline, __ATOMIC_RELAXED) == deadline)
        return;
    if (deadline <= now) {
        __atomic_fetch_or(&cpu->mailbox.requests, HAL_CPU_WAKE, __ATOMIC_RELEASE);
        deadline = now + 1;
    }
    cpu->timer_cancelled = false;
    cpu->timer_deadline = deadline;
    if (cpu->deadline_timer)
        amd64_wrmsr(0x6e0, deadline);
    else {
        /* Round up and saturate long waits. A clamped one-shot may wake
         * early; the caller rechecks its absolute deadline before reentry. */
        uint64_t ticks = amd64_scale_ticks(deadline - now, cpu->timer_hz, hal_counter_hz(), true);
        if (ticks > UINT32_MAX)
            ticks = UINT32_MAX;
        if (!ticks)
            ticks = 1;
        apic_write(0x320, VECTOR_TIMER);
        apic_write(0x380, (uint32_t)ticks);
    }
}
void amd64_monitor(const unsigned *word)
{
    __asm__ volatile("monitor" ::"a"(word), "c"(0), "d"(0) : "memory");
}
void hal_idle_until(uint64_t deadline)
{
    struct amd64_cpu *cpu = amd64_current_cpu();
    uint64_t flags, start = hal_counter();
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
    hal_timer_arm(deadline);
    amd64_tlb_lazy_enter(cpu);
    unsigned state = __atomic_load_n(&amd64_idle_state, __ATOMIC_RELAXED);
    /* An IRQ that arrives after cli remains pending. STI's interrupt shadow
     * covers the HLT or MWAIT after it (SDM Vol.2 STI; MWAIT exits on an
     * interrupt HLT would take, Vol.3 Example 11-27), so neither a timer nor
     * a post that interrupts this CPU is lost; MWAIT (C1: EAX = ECX = 0)
     * also ends at a store to the mailbox after MONITOR armed it. */
    if (host_idle_enter(&cpu->mailbox, state, amd64_monitor) &&
        !__atomic_load_n(&cpu->stop, __ATOMIC_ACQUIRE) && hal_counter() < deadline) {
        if (state == HOST_IDLE_MONITOR)
            __asm__ volatile("sti; mwait; cli" ::"a"(0), "c"(0) : "memory");
        else
            __asm__ volatile("sti; hlt; cli" ::: "memory");
    }
    host_idle_leave(&cpu->mailbox);
    amd64_tlb_lazy_leave(cpu);
    hal_timer_cancel();
    cpu->idle_ticks += hal_counter() - start;
    if (flags & (1u << 9))
        __asm__ volatile("sti" ::: "memory");
}
static void send_command(unsigned index, uint32_t command)
{
    uint32_t id = amd64_cpus[index].apic_id;
    /* Publish WB mailbox writes before x2APIC's non-serializing WRMSR
     * (SDM Vol.3 x2APIC register access: MFENCE;LFENCE before WRMSR). */
    __asm__ volatile("mfence; lfence" ::: "memory");
    if (x2apic)
        amd64_wrmsr(0x830, ((uint64_t)id << 32) | command);
    else {
        if (id > 255)
            hal_panic("xAPIC destination exceeds eight bits");
        uint64_t flags;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
        uint64_t until = hal_counter() + hal_counter_hz();
        while (apic_read(0x300) & (1u << 12)) {
            if (hal_counter() >= until)
                hal_panic("xAPIC ICR delivery timed out");
            __asm__ volatile("pause");
        }
        /* Prevent an IRQ-context sender on this CPU from interleaving the
         * ICR pair. No receiver needs a lock held by the issuer. */
        apic_write(0x310, id << 24);
        apic_write(0x300, command);
        if (flags & (1u << 9))
            __asm__ volatile("sti" ::: "memory");
    }
}
void amd64_send_ipi(unsigned index)
{
    send_command(index, VECTOR_IPI);
}
void amd64_send_nmi(unsigned index)
{
    send_command(index, 4u << 8);
}
void amd64_irq_retire(const uint64_t vectors[4])
{
    if (!amd64_cpus || !(vectors[0] | vectors[1] | vectors[2] | vectors[3]))
        return;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    if (!(flags & (1u << 9)))
        hal_panic("IRQ retirement requires enabled host interrupts");
    static unsigned lock, sequence;
    uint64_t until = hal_counter() + hal_counter_hz();
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {
        if (hal_counter() >= until)
            hal_panic("IRQ retirement lock timed out");
        __asm__ volatile("pause");
    }
    if (!++sequence)
        ++sequence;
    /* Fixed IPI priority is below every device vector. With host TPR=0 and
     * IF clear in handlers, its ACK joins all device requests already in IRR. */
    for (unsigned i = 0; i < amd64_cpu_count; ++i) {
        struct amd64_cpu *cpu = &amd64_cpus[i];
        if (!__atomic_load_n(&cpu->ready, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&cpu->stopped, __ATOMIC_ACQUIRE))
            continue;
        __atomic_store_n(&cpu->irq_retire_request, sequence, __ATOMIC_RELEASE);
        send_command(i, VECTOR_IRQ_RETIRE);
    }
    for (unsigned i = 0; i < amd64_cpu_count; ++i) {
        struct amd64_cpu *cpu = &amd64_cpus[i];
        if (__atomic_load_n(&cpu->irq_retire_request, __ATOMIC_ACQUIRE) != sequence)
            continue;
        while (__atomic_load_n(&cpu->irq_retire_ack, __ATOMIC_ACQUIRE) != sequence &&
               !__atomic_load_n(&cpu->stopped, __ATOMIC_ACQUIRE)) {
            if (hal_counter() >= until)
                hal_panic("IRQ retirement acknowledgement timed out");
            __asm__ volatile("pause");
        }
    }
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
}
bool hal_text_contains(uint64_t address)
{
    return address >= (uintptr_t)__text_start && address < (uintptr_t)__text_end;
}
void hal_cpu_interrupts(unsigned index, uint64_t *timer, uint64_t *ipi)
{
    bool known = amd64_cpus && index < amd64_cpu_count;
    *timer = known ? __atomic_load_n(&amd64_cpus[index].timer_irqs, __ATOMIC_RELAXED) : 0;
    *ipi = known ? __atomic_load_n(&amd64_cpus[index].ipi_irqs, __ATOMIC_RELAXED) : 0;
}
void hal_cpu_sample(unsigned index)
{
    if (amd64_cpus && index < amd64_cpu_count &&
        __atomic_load_n(&amd64_cpus[index].ready, __ATOMIC_ACQUIRE))
        send_command(index, VECTOR_PROFILE);
}
static bool device_message(uint16_t bdf, unsigned cpu, unsigned vector, unsigned count,
                           uint64_t *address, uint32_t *data)
{
    (void)bdf;
    (void)count;
    if (!amd64_cpus || cpu >= amd64_cpu_count || vector < VMM_IRQ_FIRST_VECTOR ||
        vector > VMM_IRQ_LAST_VECTOR || amd64_cpus[cpu].apic_id >= 255)
        return false;
    /* Direct MSI carries eight destination bits even in x2APIC mode.
     * IDs >=255 need interrupt remapping, never truncate them. */
    *address = UINT64_C(0xfee00000) | ((uint64_t)amd64_cpus[cpu].apic_id << 12);
    *data = vector; /* Host fixed, edge delivery regardless of guest format. */
    return true;
}
void amd64_interrupt(unsigned vector, uint64_t rip)
{
    if (vector == VECTOR_SPURIOUS)
        return; /* spurious vectors have no EOI */
    if (vector == VECTOR_IRQ_RETIRE) {
        struct amd64_cpu *cpu = amd64_current_cpu();
        eoi();
        __atomic_store_n(&cpu->irq_retire_ack,
                         __atomic_load_n(&cpu->irq_retire_request, __ATOMIC_ACQUIRE),
                         __ATOMIC_RELEASE);
        return;
    }
    if (vector == VECTOR_PROFILE) {
        if (vmm_profile_sample)
            vmm_profile_sample(hal_cpu_index(), rip);
        eoi();
        return;
    }
    if (vector == VECTOR_TIMER) {
        struct amd64_cpu *cpu = amd64_current_cpu();
        __atomic_fetch_add(&cpu->timer_irqs, 1, __ATOMIC_RELAXED);
        /* One-shot: fired (possibly early for a clamped LAPIC count). */
        __atomic_store_n(&cpu->timer_deadline, UINT64_MAX, __ATOMIC_RELAXED);
        __atomic_fetch_or(&cpu->mailbox.requests, HAL_CPU_WAKE, __ATOMIC_RELEASE);
    } else if (vector == AMD64_HYPERV_VECTOR) {
        __atomic_fetch_or(&amd64_current_cpu()->mailbox.requests, HAL_CPU_WAKE, __ATOMIC_RELEASE);
    } else if (vector == VECTOR_IPI) {
        amd64_ipi_received();
        __atomic_fetch_or(&amd64_current_cpu()->mailbox.requests, HAL_CPU_WAKE, __ATOMIC_RELEASE);
    } else if (vector == VECTOR_ERROR) {
        uint32_t error = apic_read(0x280);
        apic_write(0x280, 0);
        eoi();
        char message[80];
        vmm_snprintf(message, sizeof(message), "local APIC error interrupt ESR=%08x", error);
        hal_panic(message);
    } else if (vector >= VMM_IRQ_FIRST_VECTOR && vector <= VMM_IRQ_LAST_VECTOR &&
               platform_msi_interrupt) {
        if (platform_intx_interrupt && platform_intx_interrupt(vector)) {
            eoi();
            return;
        }
        eoi();
        platform_msi_interrupt(vector);
        return;
    } else {
        eoi();
        char message[80];
        vmm_snprintf(message, sizeof(message), "unexpected external host interrupt vector=%u",
                     vector);
        hal_panic(message);
    }
    eoi();
}
uint64_t amd64_timer_interrupts(void)
{
    return __atomic_load_n(&amd64_current_cpu()->timer_irqs, __ATOMIC_RELAXED);
}
void amd64_timer_selftest(void)
{
    static const unsigned delays[] = {1, 10, 100};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(delays); ++i) {
        hal_cpu_poll();
        uint64_t before = amd64_timer_interrupts();
        uint64_t deadline = hal_counter() + hal_counter_hz() * delays[i] / 1000;
        uint64_t limit = deadline + hal_counter_hz();
        while (hal_counter() < deadline || amd64_timer_interrupts() == before) {
            if (hal_counter() >= limit)
                hal_panic("host timer interrupt delivery timed out");
            hal_cpu_poll();
            hal_idle_until(deadline);
        }
        uint64_t late = hal_counter() - deadline;
        if (late > hal_counter_hz() / 10)
            hal_panic("host timer accuracy exceeds 100 ms");
        LOGI("timer selftest CPU%u delay=%u ms lateness=%llu us interrupts=%llu", hal_cpu_index(),
             delays[i], (unsigned long long)(late / (hal_counter_hz() / 1000000)),
             (unsigned long long)(amd64_timer_interrupts() - before));
    }
    hal_timer_arm(hal_counter() + hal_counter_hz());
    hal_timer_cancel();
    struct amd64_cpu *cpu = amd64_current_cpu();
    if (cpu->deadline_timer ? amd64_rdmsr(0x6e0) != 0 : apic_read(0x380) != 0)
        hal_panic("timer cancel failed");
    LOGI("host timer selftest passed (1/10/100 ms one-shot and cancel)");
}
