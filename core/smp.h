/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_CORE_SMP_H
#define IA64VMM_CORE_SMP_H
#include "ia64vmm/engine.h"
#include "ia64vmm/platform.h"

struct platform_state;

/* core/smp.h -- a VM of N vCPUs. */
enum vm_cpu_state {
    VM_CPU_UNSTARTED,  /* bound; the executor has not entered its callback */
    VM_CPU_RESET_HOLD, /* AP executor running, waiting for its first wake */
    VM_CPU_RUNNING,
    VM_CPU_STOPPED, /* executor left its loop; exit and code are valid */
};
struct vm_cpu {
    struct vcpu *v;
    unsigned index, host_cpu;        /* index == v->cfg.index == LID id */
    unsigned state;                  /* enum vm_cpu_state; release stores by the
                                     executor, acquire loads by everyone else */
    struct vcpu_exit exit;           /* terminal exit, valid in VM_CPU_STOPPED */
    int code;                        /* VMM_EXIT_* of this executor */
    uint64_t exec_ticks, idle_ticks; /* per-vCPU milestone and sample counters */
};
struct vm {
    unsigned count; /* vCPUs = handoff cpu_count */
    struct vm_cpu cpu[IA64VMM_MAX_CPUS];
    struct platform_state *platform;
    uint64_t itc_origin;     /* common origin of every vCPU's ITC */
    unsigned stop;           /* VM-wide stop request (atomic) */
    unsigned first_terminal; /* index + 1 of the first terminal executor (CAS) */
    bool serialize;          /* smp_serialize=1 */
};
struct vm *vm_current(void);                    /* NULL outside vm_run_guest */
struct vm_cpu *vm_cpu_of(const struct vcpu *v); /* NULL: no route (lockstep shadow) */
void vm_cpu_park(struct vcpu *v);               /* parked = 1 (seq_cst), then re-check the
                                         mailbox and the purge tail */
void vm_cpu_unpark(struct vcpu *v); /* parked = 0 (seq_cst), then ia64_smp_apply_purges */
void vm_run_lock(struct vcpu *v);   /* smp_serialize: held around engine_run only; */
void vm_run_unlock(struct vcpu *v); /* no-ops when the VM is not serialized */

/*
 * The executor framework core/vm.c drives. core/smp.c owns the executor loop,
 * reset-hold, terminal exits, stop-all and the management loop; core/vm.c
 * sets the VM up and supplies the guest-specific steps below. Tests drive
 * the same functions with their own steps (tests/host/smp/).
 */

/* Guest-specific steps of one executor loop; every hook runs on the
 * executor's own host CPU with ctx = struct vm_exec.ctx. NULL hooks are
 * skipped (slice: a full budget). */
struct vm_exec_ops {
    /* Each slice, before the guest deadline test (sample_sec lines). */
    void (*sample)(void *ctx, struct vm_cpu *c);
    /* Each slice, after the interrupt poll: the budget of its engine_run
     * (vCPU 0's post-ExitBootServices observation single-steps). */
    uint64_t (*slice)(void *ctx, struct vm_cpu *c);
    /* A DEBUG exit while the VM is not stopping: true when it was vCPU 0's
     * one-shot ExitBootServices request and the loop goes on. */
    bool (*debug_exit)(void *ctx, struct vm_cpu *c);
    /* Once, after the executor left its loop and before it publishes
     * VM_CPU_STOPPED: milestone and exit diagnostics. */
    void (*stopped)(void *ctx, struct vm_cpu *c);
};
struct vm_exec {
    const struct vm_exec_ops *ops;
    void *ctx;
    uint64_t start, hz;     /* hal_counter() when the run started; its frequency */
    uint64_t timeout;       /* guest timeout_sec, checked by every executor; 0: none */
    uint64_t halt_retry_us; /* PAL_HALT_LIGHT retry of vCPU 0 before ExitBootServices
                                     0: none */
};
/* Full engine_run budget of a slice (instructions); under smp_serialize a
 * slice that starts while another executor waits for the run lock gets the
 * short budget, so vCPUs that spin on each other rotate quickly. */
#define VM_SLICE_BUDGET 100000u
#define VM_SERIAL_SLICE_BUDGET 10000u

/* Management CPU, before any vCPU exists: makes the one VM of count vCPUs
 * current, takes its ITC origin and resets the run lock. NULL
 * when count is 0 or above IA64VMM_MAX_CPUS, or a VM exists. The caller
 * then fills cpu[i].v and cpu[i].host_cpu and binds every route. */
struct vm *vm_create(unsigned count, struct platform_state *platform, bool serialize);
/* After vm_run returned and every vCPU was destroyed: vm_current() is NULL. */
void vm_destroy(struct vm *vm);
/* Starts one executor per vCPU on cpu[i].host_cpu, then runs the
 * management loop (log drain, host shutdown requests, the watchdog at
 * hal_counter() >= watchdog, stop-all) until every executor stopped and its
 * host CPU is idle. A single vCPU placed on the calling CPU (the hosted
 * single-thread harness) runs on the calling thread instead. Returns the
 * first terminal executor's VMM_EXIT_* code; VMM_EXIT_SETUP when an
 * executor could not be started. */
int vm_run(struct vm *vm, const struct vm_exec *exec, uint64_t watchdog);
/* The two halves of vm_run, for tests that place a stop between them.
 * vm_start returns false (and stops what it started) when an executor
 * cannot start; vm_wait is the management loop. */
bool vm_start(struct vm *vm, const struct vm_exec *exec);
int vm_wait(struct vm *vm, uint64_t watchdog);
/* Stop-all (any CPU): vm.stop, then for every executor
 * not yet stopped a DEBUG exit request and HAL_CPU_STOP, or HAL_CPU_WAKE
 * while it is VM_CPU_UNSTARTED (a STOP before the CPU took its
 * work leaves it busy forever) or when the guest restarts
 * (vm_restart_decided: the next boot reuses the host
 * CPUs, and HAL_CPU_STOP stops a CPU for good). watchdog: the management
 * watchdog fired, so every executor reports where it was stopped. */
void vm_stop_all(struct vm *vm, bool watchdog);
/* The first terminal exit was a reset request that restarts the guest,
 * decided when it was taken, unless a watchdog stop
 * came first. */
bool vm_restart_decided(const struct vm *vm);

/* Parse purge_panic_ms before starting executors; default 30000,
 * 0 disables panic while retaining once-per-second diagnostics. */
bool vm_purge_configure(const char *cmdline);

#endif
