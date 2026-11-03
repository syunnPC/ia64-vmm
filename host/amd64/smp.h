/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_SMP_H
#define VMM_AMD64_SMP_H
#include "ia64vmm/hal.h"
#include "ia64vmm/host_wake.h"
struct amd64_cpu {
    unsigned index; /* first: hal_cpu_index reads %gs:0 */
    unsigned apic_id, ready, busy, stop, stopped;
    unsigned host_isa, host_simd;
    unsigned tlb_request, tlb_ack, tlb_lazy;
    unsigned window_lock, window_request, window_ack;
    unsigned irq_retire_request, irq_retire_ack;
    uint64_t root, window_address;
    uint64_t timer_hz, timer_irqs, ipi_irqs, idle_ticks, timer_deadline;
    bool deadline_timer;
    bool timer_cancelled; /* Owner-only; IRQ delivery does not prove a stop. */
    void *stack_top;
    hal_cpu_work_fn work;
    void *work_arg;
    struct host_mailbox mailbox; /* HAL_CPU_* requests; a line of its own */
};
extern struct amd64_cpu *amd64_cpus;
extern unsigned amd64_cpu_count;
/* How hal_idle_until waits: HOST_IDLE_HALT, or HOST_IDLE_MONITOR once
 * amd64_idle_selftest has seen every AP wake from a mailbox store. */
extern unsigned amd64_idle_state;
void amd64_monitor(const unsigned *word);
struct amd64_cpu *amd64_current_cpu(void);
void amd64_ipi_received(void);
/* Idle context with interrupts disabled: while lazy, a CPU touches only its
 * stack, mailbox and APIC (never remapped) and receives no TLB IPIs; leaving
 * performs a full flush if any shootdown was skipped meanwhile. */
void amd64_tlb_lazy_enter(struct amd64_cpu *cpu);
void amd64_tlb_lazy_leave(struct amd64_cpu *cpu);
#endif
