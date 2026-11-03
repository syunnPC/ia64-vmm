/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Intel SDM Vol.3A, Sections 13.5.4 and 13.5.4.1: zero disarms either timer mode;
 * an interrupt for an old deadline may still arrive after reprogramming. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/amd64.h"
#include "host/amd64/smp.h"

static void timer_cpuid(uint32_t, uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
static uint64_t timer_rdmsr(uint32_t);
static void timer_wrmsr(uint32_t, uint64_t);
#define amd64_cpuid timer_cpuid
#define amd64_rdmsr timer_rdmsr
#define amd64_wrmsr timer_wrmsr
#include "host/amd64/apic.c"

static struct amd64_cpu cpu;
struct amd64_cpu *amd64_cpus;
unsigned amd64_cpu_count = 1;
uint64_t amd64_hhdm;
static uint64_t registers[0x900], counter;
static uint32_t mmio_registers[0x400 / 4];
static unsigned timer_writes;
static bool use_deadline, interrupt_on_arm;
static bool pending_vectors[256];
static unsigned device_irqs, barrier_ipis;
bool platform_msi_interrupt(unsigned vector)
{
    assert(vector >= VMM_IRQ_FIRST_VECTOR && vector <= VMM_IRQ_LAST_VECTOR);
    ++device_irqs;
    assert(cpu.irq_retire_ack != cpu.irq_retire_request);
    return true;
}

static void timer_cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c,
                        uint32_t *d)
{
    assert(leaf == 1 && sub == 0);
    *a = *b = 0;
    *c = (1u << 21) | (use_deadline ? 1u << 24 : 0);
    *d = 1u << 9;
}
static uint64_t timer_rdmsr(uint32_t reg)
{
    assert(reg < IA64VMM_ARRAY_SIZE(registers));
    if (reg == 0x839)
        return UINT32_MAX - 100;
    return registers[reg];
}
static void timer_wrmsr(uint32_t reg, uint64_t value)
{
    assert(reg < IA64VMM_ARRAY_SIZE(registers));
    if (reg == 0x830) {
        unsigned vector = value & 255;
        assert(vector >= 16 && vector < VMM_IRQ_FIRST_VECTOR);
        ++barrier_ipis;
        pending_vectors[vector] = true;
        /* APIC fixed interrupts select the highest vector priority. */
        for (unsigned next = 256; next-- > 16;)
            if (pending_vectors[next]) {
                pending_vectors[next] = false;
                amd64_interrupt(next, 0);
            }
    }
    if (reg == 0x6e0 || reg == 0x832 || reg == 0x838)
        ++timer_writes;
    /* Deliver an already-pending interrupt while a new arm is in flight. */
    if (interrupt_on_arm && value && (reg == 0x6e0 || reg == 0x838)) {
        interrupt_on_arm = false;
        amd64_interrupt(VECTOR_TIMER, 0);
    }
    registers[reg] = value;
}
struct amd64_cpu *amd64_current_cpu(void)
{
    return &cpu;
}
unsigned hal_cpu_index(void)
{
    return 0;
}
uint64_t hal_counter(void)
{
    return counter++;
}
uint64_t hal_counter_hz(void)
{
    return 1000;
}
void hal_port_out(uint16_t port, uint32_t value, unsigned size)
{
    (void)port;
    (void)value;
    (void)size;
}
void amd64_map_apic(uint64_t pa)
{
    (void)pa;
    abort();
}
void amd64_ipi_received(void)
{
    abort();
}
void vmm_log(enum log_level level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}
int vmm_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    (void)buf;
    (void)size;
    (void)fmt;
    abort();
}
void hal_panic(const char *message)
{
    fprintf(stderr, "%s\n", message);
    abort();
}

static bool armed(void)
{
    return use_deadline ? registers[0x6e0] != 0 : registers[0x838] != 0;
}
static void check_cancelled(void)
{
    assert(!armed());
    if (!use_deadline)
        assert(registers[0x832] & APIC_MASK);
    assert(cpu.timer_deadline == UINT64_MAX);
}
static void init(bool deadline)
{
    memset(&cpu, 0, sizeof(cpu));
    memset(registers, 0, sizeof(registers));
    cpu.timer_deadline = UINT64_MAX;
    amd64_cpus = &cpu;
    mmio = mmio_registers;
    use_deadline = deadline;
    counter = 100;
    registers[0x803] = 6u << 16;
    registers[0x6e0] = UINT64_MAX;
    registers[0x838] = UINT32_MAX;
    amd64_apic_init(0);
    check_cancelled();
}
static void semantics(bool deadline)
{
    init(deadline);
    uint64_t due = counter + 100;
    hal_timer_arm(due);
    assert(armed() && cpu.timer_deadline == due);
    unsigned before = timer_writes;
    hal_timer_arm(due);
    assert(timer_writes == before);
    hal_timer_cancel();
    check_cancelled();

    hal_timer_arm(counter + 100);
    due = cpu.timer_deadline;
    /* Hardware expiry precedes delivery; rearming the same absolute
     * deadline must still program a fresh one-shot after that IRQ. */
    registers[use_deadline ? 0x6e0 : 0x838] = 0;
    amd64_interrupt(VECTOR_TIMER, 0);
    assert(!armed());
    hal_timer_arm(due);
    assert(armed());
    hal_timer_cancel();
    check_cancelled();

    /* Cancellation cannot retract an interrupt already pending in the APIC. */
    amd64_interrupt(VECTOR_TIMER, 0);
    assert(cpu.mailbox.requests & HAL_CPU_WAKE);
    hal_timer_arm(counter + 100);
    assert(armed());
    amd64_interrupt(VECTOR_TIMER, 0);
    assert(cpu.timer_deadline == UINT64_MAX && armed());
    hal_timer_arm(UINT64_MAX);
    check_cancelled();

    interrupt_on_arm = true;
    hal_timer_arm(counter + 100);
    assert(!interrupt_on_arm && armed() && cpu.timer_deadline == UINT64_MAX);
    hal_timer_cancel();
    check_cancelled();

    cpu.mailbox.requests = HAL_CPU_STOP;
    hal_timer_arm(0);
    assert(armed() && cpu.mailbox.requests == (HAL_CPU_STOP | HAL_CPU_WAKE));
    hal_timer_cancel();
    check_cancelled();
    assert(cpu.mailbox.requests == (HAL_CPU_STOP | HAL_CPU_WAKE));
}
static void repeated_cancel(bool deadline)
{
    init(deadline);
    unsigned before = timer_writes;
    hal_timer_cancel();
    hal_timer_arm(UINT64_MAX);
    assert(timer_writes == before);
    hal_timer_arm(counter + 100);
    before = timer_writes;
    hal_timer_cancel();
    assert(timer_writes == before + (deadline ? 1 : 2));
    before = timer_writes;
    amd64_interrupt(VECTOR_TIMER, 0);
    hal_timer_cancel();
    hal_timer_arm(UINT64_MAX);
    check_cancelled();
    assert(timer_writes == before);
}
static void irq_retirement(void)
{
    init(true);
    cpu.ready = 1;
    pending_vectors[0x40] = pending_vectors[0xdf] = true;
    uint64_t vectors[4] = {0, 1, 0, UINT64_C(1) << 31};
    amd64_irq_retire(vectors);
    assert(device_irqs == 2 && barrier_ipis == 1);
    assert(cpu.irq_retire_ack == cpu.irq_retire_request && cpu.irq_retire_ack);
    pending_vectors[0x40] = true;
    amd64_irq_retire(vectors);
    assert(device_irqs == 3 && barrier_ipis == 2);
    cpu.stopped = 1;
    amd64_irq_retire(vectors);
    assert(barrier_ipis == 2);
}
int main(void)
{
    hal_timer_cancel(); /* Before the host CPU array exists. */
    semantics(false);
    semantics(true);
    puts("APIC timer: cancellation, pending interrupt, rearm and wake semantics passed");
    fflush(stdout);
    repeated_cancel(false);
    repeated_cancel(true);
    puts("APIC timer: repeated cancellation avoids hardware writes in both modes");
    irq_retirement();
    puts("APIC retirement: device-priority dispatch precedes the barrier acknowledgement");
    return 0;
}
