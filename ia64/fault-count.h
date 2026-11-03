/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_FAULT_COUNT_H
#define IA64VMM_IA64_FAULT_COUNT_H
#include "ia64/state.h"
/* Progress diagnostics: faults delivered to the guest IVT per vCPU
 * index (a lockstep shadow shares its primary's index). */
uint64_t ia64_faults_delivered(const struct vcpu *v);
/* Opt-in stop_fault=<enum ia64_fault_kind number>: the first delivery of
 * that kind that log_faults would log (not a routine one, below) requests
 * VCPU_EXIT_DEBUG after the IVT transfer (0 = off). */
void ia64_set_stop_fault(unsigned kind);
/* Opt-in log_faults=N: log the first N delivered faults other than TLB/VHPT
 * misses, access/dirty bits and (with PSR.it=0) unaligned references. */
void ia64_set_log_faults(unsigned count);
/* Opt-in break_ip (vcpu_set_break_ip): nonzero once one is set. The
 * interpreter then calls ia64_break_check before each instruction and Tier0
 * at each region entry, so Tier0 sees only region starts (function entries,
 * return addresses, branch targets). */
extern unsigned ia64_break_ips;
void ia64_break_check(struct vcpu *v);
#endif
