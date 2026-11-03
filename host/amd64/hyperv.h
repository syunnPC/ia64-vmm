/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_HYPERV_H
#define VMM_AMD64_HYPERV_H
#include "ia64vmm/types.h"
#define AMD64_HYPERV_VECTOR 0xf1u
#define AMD64_HYPERV_SINT 2u
/* CPUID-only detection. A Hyper-V compatible Hv#1 interface is required. */
bool amd64_hyperv_detect(void);
bool amd64_hyperv_reference_available(void);
/* 100ns units; call only when reference_available is true. */
uint64_t amd64_hyperv_reference(void);
/* Zero when frequency MSRs are unavailable. */
uint64_t amd64_hyperv_tsc_hz(void);
/* Called by VMBus on its management CPU. SynIC IRQs only require normal APIC
 * EOI; messages and channel events are consumed by the polling path. */
bool amd64_hyperv_start(void);
uint32_t amd64_hyperv_vp(void);
uint16_t amd64_hyperv_post(uint32_t connection, const void *data, unsigned bytes);
uint16_t amd64_hyperv_signal(uint32_t connection);
/* 1: message, 0: empty, -1: invalid message. */
int amd64_hyperv_message(void *out, unsigned capacity, unsigned *bytes);
void amd64_hyperv_events(void);
/* Only after the VMBus host confirms unload; pages remain reserved. */
void amd64_hyperv_stop(void);
#endif
