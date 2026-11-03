/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hosted HAL modes.
 * Single-thread: one host CPU, CPU 0, which every thread reports; vCPU 0
 * runs on it (hal_vcpu_cpu(0) == 0) and hal_cpu_start always fails.
 * Threaded: host CPUs 0 .. vcpus + helpers; CPU 0 is the main thread and
 * the management CPU, CPU i + 1 runs vCPU i, CPUs vcpus + 1 .. are helpers
 * (rank 1). Each non-zero CPU gets its own thread on its first
 * hal_cpu_start, as host/amd64/smp.c dispatches to an AP. Threads the HAL
 * did not create report CPU 0.
 */
#ifndef IA64VMM_HAL_LINUX_H
#define IA64VMM_HAL_LINUX_H

#include "ia64vmm/platform.h"
#include "ia64vmm/types.h"
/* Quiescent tests only: flush generated code before changing this mask.
 * Enabling bits never enables a feature absent from a probed CPU. */
unsigned hal_linux_host_isa_mask(unsigned mask);

#define HAL_LINUX_MAX_HELPERS 64u
#define HAL_LINUX_MAX_CPUS (1u + IA64VMM_MAX_CPUS + HAL_LINUX_MAX_HELPERS)

/* Main thread, before vmm_main and before any other thread uses the HAL;
 * at most once. Threaded mode is required for vcpus != 1 or helpers != 0.
 * pins: NULL, or exactly one Linux CPU per host CPU (sched_setaffinity of
 * host CPU k's thread to Linux CPU pins[k], measurements only). Returns NULL
 * or an error message. */
const char *hal_linux_configure(bool threaded, unsigned vcpus, unsigned helpers,
                                const unsigned *pins, unsigned pin_count);

/* The idle state of every host CPU, HOST_IDLE_HALT or HOST_IDLE_MONITOR
 * (ia64vmm/host_wake.h), from IA64VMM_HOSTED_IDLE (halt, the default, or
 * monitor); and the interrupts sent to host CPU cpu so far. Tests may
 * define hal_linux_monitor_event, called as a monitoring CPU arms. */
unsigned hal_linux_idle_state(void);
unsigned hal_linux_interrupts(unsigned cpu);

#endif
