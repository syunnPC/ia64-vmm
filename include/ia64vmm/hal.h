/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host abstraction layer. Everything that differs between the bare-metal
 * AMD64 VMM (host/amd64) and the Linux unit-test shim
 * (tests/hal) is behind this interface; core/, platform/, ia64/ and engine/
 * never call an OS API or execute a privileged AMD64 instruction directly.
 *
 * Context column: "any" = may be called from any vCPU/management context,
 * never from a host interrupt handler unless stated. No function blocks
 * unless stated.
 */
#ifndef IA64VMM_HAL_H
#define IA64VMM_HAL_H

#include "ia64vmm/types.h"

/* --- diagnostics ------------------------------------------------------ */

/* Raw byte sink for the log ring's mirrors (serial on bare metal / QEMU,
 * stderr when hosted). The framebuffer console is a separate sink owned by
 * core/log. Safe from any context including the panic path. */
void hal_debug_write(const char *s, size_t n);
/* A named stop for real-hardware photographs: the bare-metal HAL logs it and
 * waits pause_sec= seconds; hosted HALs only log it. */
void hal_checkpoint(const char *name);

/* Stops every vCPU, dumps what it can, never returns. */
IA64VMM_NORETURN void hal_panic(const char *msg);

/* --- memory ------------------------------------------------------------ */

#define HAL_PAGE_EXEC (1u << 0) /* region will hold generated AMD64 code */
#define HAL_PAGE_ZERO (1u << 1)

/* Allocates contiguous pages from the VMM reservation (never guest RAM).
 * Returns NULL on exhaustion. Pages are writable, not executable. */
void *hal_alloc_pages(size_t npages, unsigned flags);
void hal_free_pages(void *p, size_t npages);
/* Reservation accounting, including allocator metadata. false when the HAL
 * has no finite reservation (unit-test shim); *out is then untouched. */
struct hal_memory_stats {
    uint64_t total_bytes, used_bytes, peak_bytes;
};
bool hal_memory_get_stats(struct hal_memory_stats *out);

/* W^X for code-cache pages allocated with HAL_PAGE_EXEC: toggles a range
 * between writable+non-executable and read-only+executable. */
void hal_code_set_writable(void *p, size_t npages);
void hal_code_set_executable(void *p, size_t npages);

/* Save and disable local maskable interrupts; restore the previous IF.
 * Management-only, paired around pre-guest PCI decode changes. */
uint64_t hal_irq_save(void);
void hal_irq_restore(uint64_t flags);

/* --- time -------------------------------------------------------------- */

/* Monotonic, invariant counter and its frequency (TSC on bare metal,
 * CLOCK_MONOTONIC-derived when hosted). Used for guest ITC/ITM. */
uint64_t hal_counter(void);
uint64_t hal_counter_hz(void);

/* Unix seconds UTC; 0 when unavailable (platform falls back to CMOS). */
uint64_t hal_wallclock_seconds(void);

/* Idles the calling host CPU until hal_counter() >= deadline or a host
 * event arrives; may return early (spurious wakeups are fine). */
void hal_idle_until(uint64_t counter_deadline);
/* Drain host input on the management CPU, outside interrupt context. */
void hal_input_poll(void);

/* Calling CPU, any execution context except IRQ: arm/cancel its local
 * one-shot timer in hal_counter units. UINT64_MAX means cancel. Expired
 * deadlines cause an immediate wakeup; arming does not block. */
void hal_timer_arm(uint64_t counter_deadline);
void hal_timer_cancel(void);

/* --- fixed host CPU execution ------------------------------------------ */

#define HAL_CPU_WAKE (1u << 0)
#define HAL_CPU_STOP (1u << 1)
#define HAL_CPU_SHUTDOWN (1u << 2)

/* Any execution/IRQ context, nonblocking. A request publishes its bits in
 * the CPU's mailbox and interrupts the CPU only while it halts in
 * hal_idle_until; a CPU waiting with MONITOR/MWAIT wakes from the store, a
 * running one sees the bits at its next poll or idle check
 * (ia64vmm/host_wake.h). Receivers never acquire sender locks. Polling
 * consumes bits on the calling CPU; idle checks the mailbox with interrupts
 * disabled after publishing how it waits, so a request cannot be lost
 * across sleep. */
unsigned hal_cpu_index(void);
/* Immutable boot-time intersection on every possible JIT executor. Zero
 * until all CPUs have reported; optional instructions never require AVX. */
unsigned hal_host_isa(void);
/* HOST_SIMD_* (ia64vmm/host_simd.h) on every CPU, the same publication. */
unsigned hal_host_simd(void);
void hal_cpu_request(unsigned cpu, unsigned requests);
/* A HAL_CPU_WAKE request through the same mailbox protocol. */
void hal_cpu_wake(unsigned cpu);
unsigned hal_cpu_poll(void);
/* Management, profile_hz (core/profile.h): interrupt that CPU once to
 * sample the RIP it was executing; no request bit is set. */
void hal_cpu_sample(unsigned cpu);
/* Whether address lies in the VMM image's code (not generated code). */
bool hal_text_contains(uint64_t address);
/* Any context: the host timer interrupts and IPIs (requests, TLB and
 * window shootdowns; not profile samples) that CPU has taken so far; 0
 * where the HAL does not count them. */
void hal_cpu_interrupts(unsigned cpu, uint64_t *timer, uint64_t *ipi);

/* Management context only, nonblocking. Dispatch one callback on an idle
 * non-management CPU. Callback runs with host interrupts enabled and must
 * return at a restorable boundary when STOP is observed. No allocation or
 * guest-state access is performed by the host interrupt handler. */
typedef void (*hal_cpu_work_fn)(void *arg);
bool hal_cpu_start(unsigned cpu, hal_cpu_work_fn work, void *arg);
bool hal_cpu_busy(unsigned cpu);
/* Fixed placement selected at boot: the host CPU of vCPU i, UINT32_MAX for
 * none, and the number of vCPUs. Bare metal: the boot placement (vcpus=,
 * placement=). Hosted single-thread mode: CPU 0 for vCPU 0; threaded mode:
 * CPU i + 1 for vCPU i. core creates hal_vcpu_count() vCPUs and runs
 * vCPU i's executor on hal_vcpu_cpu(i). */
unsigned hal_vcpu_cpu(unsigned vcpu);
unsigned hal_vcpu_count(void);
/* Helper CPUs: host CPUs that are neither the
 * management CPU nor placed for a vCPU, in rank order; UINT32_MAX / 0
 * for i >= hal_helper_count(). Rank 1: a core without vCPU or management;
 * 2: SMT sibling of the management CPU; 3: SMT sibling of a vCPU CPU. */
unsigned hal_helper_count(void);
unsigned hal_helper_cpu(unsigned i);
unsigned hal_helper_rank(unsigned i);

/* --- x86 port I/O (used only by platform/port bridge) ------------------ */

uint32_t hal_port_in(uint16_t port, unsigned size); /* size 1,2,4 */
void hal_port_out(uint16_t port, uint32_t value, unsigned size);

/* --- x86 real-mode container (types in ia64vmm/x86rm.h) ----------- */

/* Management CPU only, before any guest runs. probe changes no state; *why
 * names the missing capability (the hosted HAL has none). create enables
 * hardware virtualization on the calling CPU and builds the container;
 * destroy restores every MSR and control register it changed. run executes
 * the guest until its next
 * exit; the HAL guarantees an exit at least every ~10 ms whatever the
 * guest's interrupt flag, and returns false only on a backend error. */
struct x86rm;
struct x86rm_config;
struct x86rm_regs;
struct x86rm_exit;
struct x86rm_info;
bool hal_x86rm_probe(const char **why);
/* The backend probe chose (AMD SVM or Intel VMX on bare metal, chosen by
 * the CPU vendor): its name and hypercall instruction. Never NULL. */
const struct x86rm_info *hal_x86rm_info(void);
bool hal_x86rm_create(const struct x86rm_config *cfg, struct x86rm **out, const char **why);
bool hal_x86rm_run(struct x86rm *rm, struct x86rm_regs *regs, struct x86rm_exit *exit);
void hal_x86rm_destroy(struct x86rm *rm);
/* Display reclaim: after all IA-64 guest executors have
 * stopped and been joined, the management CPU may create a diagnostic
 * container again. run/destroy also apply to this returned handle. Same
 * mechanism and restoration guarantees as create; no concurrent guest. */
bool hal_x86rm_reclaim_create(const struct x86rm_config *cfg, struct x86rm **out, const char **why);
/* After the card's VBIOS set text mode 3: the VMM screen console (log
 * mirror, guest serial tail, failure screen) moves to the VGA text buffer
 * at B8000h. False when the host has no such console (hosted). */
bool hal_console_vga_text(void);

/* --- lifecycle --------------------------------------------------------- */

enum hal_shutdown_kind {
    HAL_SHUTDOWN_POWER_OFF,
    HAL_SHUTDOWN_RESET,
    HAL_SHUTDOWN_HALT, /* stop and leave the screen readable */
};

/* Management context, nonblocking: an asynchronous host shutdown/panic
 * requested by another CPU. It is performed only after stopping executors. */
bool hal_shutdown_requested(enum hal_shutdown_kind *kind, int *exit_code);

/* exit_code is reported to the test harness (QEMU isa-debug-exit / hosted
 * process exit status): 0 = guest requested a normal shutdown after
 * success, nonzero = failure class (see core/vm.h VMM_EXIT_*). */
IA64VMM_NORETURN void hal_shutdown(enum hal_shutdown_kind kind, int exit_code);

#endif
