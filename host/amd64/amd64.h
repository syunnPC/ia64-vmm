/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_H
#define VMM_AMD64_H
#include "ia64vmm/bootinfo.h"

#define AMD64_CODE_BASE UINT64_C(0xffffc00000000000)
extern uint64_t amd64_hhdm, amd64_image_pa, amd64_image_va;
extern char __image_start[], __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[], __data_start[], __image_end[];
extern char boot_stack[], boot_stack_top[];

static inline void amd64_cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c,
                               uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static inline uint64_t amd64_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void amd64_wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" ::"c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}
static inline void amd64_invlpg(uint64_t va)
{
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
}
static inline uint64_t amd64_image_phys(const void *p)
{
    return (uintptr_t)p - amd64_image_va + amd64_image_pa;
}
void amd64_serial_init(void);
void amd64_checkpoint_init(const struct boot_info *bi);
void amd64_stage(const char *name);
void amd64_checkpoint(const char *name);
void amd64_final_summary(int code, bool screen);
bool amd64_pause_enabled(void);
void amd64_cpu_init(void);
void amd64_memory_init(const struct boot_info *bi, uint64_t base, uint64_t size);
void amd64_time_init(const struct boot_info *bi);
uint64_t amd64_calibrate_tsc(const struct boot_info *bi, const char **source);
void amd64_iommu_handoff(const struct boot_info *bi);
void amd64_iommu_protect(const struct boot_info *bi);
void amd64_time_verify_cpu(void);
const char *amd64_time_source(void);
void amd64_shutdown_init(const struct boot_info *bi);
/* Only the exit= options, for a stop before ACPI is known. */
void amd64_shutdown_exit_options(const char *cmdline);
bool amd64_option(const char *cmdline, const char *option);
void amd64_acpi_poweroff(void);
void amd64_acpi_init(const struct boot_info *bi);
const uint8_t *amd64_acpi_table(const struct boot_info *bi, const char *signature);
const uint8_t *amd64_acpi_table_index(const struct boot_info *, const char *, unsigned index);
const uint8_t *amd64_acpi_dsdt(const struct boot_info *);
struct pci_host_ops;
void amd64_intx_init(const struct boot_info *, const struct pci_host_ops *, void *);
void amd64_pci_init(const struct boot_info *bi);
uint8_t *amd64_map_mmio(uint64_t base, uint64_t size);
bool amd64_readable(uint64_t va, size_t size);
/* Translate a canonical host backing pointer through the shared VMM map. */
bool amd64_memory_physical(uint64_t va, uint64_t *pa);
void amd64_memory_selftest(void);

/* Host-local interfaces; none retain a Limine response pointer. */
void amd64_cpu_init_index(unsigned index, void *stack_top);
void amd64_idt_gate(unsigned vector, void *handler);
uint64_t amd64_memory_root(void);
/* Private lower-half roots and shadow tables. A root holds
 * at most `share` tables of the pool, retired ones included until reaped. */
uint64_t amd64_memory_private_root(void);
struct amd64_shadow_root {
    uint64_t root;
    unsigned retired, tables, share;
};
bool amd64_shadow_init(void);
unsigned amd64_shadow_capacity(void);
uint64_t *amd64_shadow_leaf(struct amd64_shadow_root *, uint64_t va, bool create);
void amd64_shadow_unmap(struct amd64_shadow_root *, uint64_t va, uint64_t bytes);
void amd64_shadow_reap(struct amd64_shadow_root *);
unsigned amd64_shadow_used(void);
/* Three-second bound shared by window locks, pool lock and acknowledgement. */
void amd64_window_lock(unsigned *lock, unsigned target, const char *what);
void amd64_window_flush(unsigned cpu, uint64_t address, bool sync);
void amd64_hostwin_selftest(bool fatal, bool outside);
void amd64_simd_selftest(unsigned host_cpus);
/* fn(arg) with test values in YMM0-15, MXCSR and x87: NULL if they survive. */
const char *amd64_simd_selftest_call(const void *fn, void *arg, uint64_t *result);
void amd64_map_apic(uint64_t pa);
void amd64_tlb_shootdown(void);
void amd64_tlb_note(uint64_t va);
void amd64_apic_init(unsigned index);
bool amd64_apic_extended(void);
/* Sources must be quiesced and remapping invalidations completed first. */
void amd64_irq_retire(const uint64_t vectors[4]);
void amd64_timer_selftest(void);
void amd64_ipi_selftest(void);
/* After amd64_ipi_selftest: MONITOR/MWAIT idle when CPUID enumerates it,
 * monitor is true and every AP wakes from a store to its mailbox. */
void amd64_idle_selftest(bool monitor);
void amd64_smp_prepare(struct boot_info *bi);
void amd64_smp_online(unsigned index);
void amd64_smp_wait(void);
void amd64_helpers_init(void); /* after amd64_placement (hal_helper_*) */
void *amd64_smp_stack(unsigned index);
IA64VMM_NORETURN void amd64_cpu_loop(void *arg);
IA64VMM_NORETURN void amd64_enter_cpu(uint64_t cr3, void *stack_top, void (*entry)(void *),
                                      void *arg);
bool amd64_smp_stop(void);
bool amd64_smp_ready(void);
void amd64_interrupt(unsigned vector, uint64_t rip);
uint64_t amd64_timer_interrupts(void);
void amd64_send_ipi(unsigned cpu);
void amd64_send_nmi(unsigned cpu);
void amd64_reclaim(struct boot_info *bi);
void amd64_copy_acpi(struct boot_info *bi);
#endif
