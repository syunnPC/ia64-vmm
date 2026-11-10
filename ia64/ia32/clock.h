/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64_IA32_CLOCK_H
#define IA64_IA32_CLOCK_H
#include "ia64/semantics.h"
#include "ia64vmm/ia32_state.h"

enum ia32_clock_kind { IA32_CLOCK_RDTSC, IA32_CLOCK_RDTSCP, IA32_CLOCK_RDPMC };
/* Invoke after decoding/prefix admission at a completed instruction boundary.
 * Success updates EDX:EAX and RDTSCP's ECX; faults leave native state unchanged.
 * The caller owns EIP/RF completion and guest interruption delivery. RDTSCP's
 * virtual TSC_AUX signature is the configured vCPU index, never a host CPU ID. */
struct ia64_result ia32_clock(struct vcpu *, struct ia32_native_state *, enum ia32_clock_kind,
                              struct ia32_interruption *);
uint32_t ia32_clock_cpu_id(const struct vcpu *);
#endif
