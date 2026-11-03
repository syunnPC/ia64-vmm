/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AP block and per-CPU areas. The block lives in
 * this image's data (EfiRuntimeServicesCode, hob.c), so it survives into the
 * OS: APs read it physically, SAL may write it in virtual mode through a
 * pointer converted at SetVirtualAddressMap (sal/runtime.c). */
#ifndef VMM_AP_H
#define VMM_AP_H
#define AP_WAKE_ENTRY 0
#define AP_WAKE_GP 8
#define AP_WAKE_BASE 16
#define AP_WAKE_STRIDE 24
#define AP_WAKE_COUNT 28
/* Area of CPU i at percpu_base + i * percpu_stride: the IVT context block
 * (ivt-layout.h) first, then the backing store growing up and the memory
 * stack growing down, so neither grows into the context or another CPU.
 * Last, the interruption stack for C calls from translated code (ivt.S,
 * CONTROL_InterruptStack). On an AP only ap_external_interrupt and the unaligned
 * dispatcher's rejection run there, both stopping the CPU; the dispatcher's
 * frame is about 4 KiB. */
#define PERCPU_CONTEXT 0x0
#define PERCPU_CONTEXT_SIZE 0x2000
#define PERCPU_RSE 0x2000
#define PERCPU_RSE_SIZE 0x8000
#define PERCPU_STACK 0xa000
#define PERCPU_STACK_SIZE 0x10000
#define PERCPU_INTERRUPT_STACK 0x1a000
#define PERCPU_INTERRUPT_STACK_SIZE 0x4000
#define PERCPU_STRIDE 0x1e000
#ifndef __ASSEMBLER__
#include <stdint.h>
struct ap_wake_block {
    uint64_t rendez_entry, rendez_gp;
    uint64_t percpu_base;
    uint32_t percpu_stride, count;
};
_Static_assert(__builtin_offsetof(struct ap_wake_block, rendez_gp) == AP_WAKE_GP &&
                   __builtin_offsetof(struct ap_wake_block, percpu_base) == AP_WAKE_BASE &&
                   __builtin_offsetof(struct ap_wake_block, percpu_stride) == AP_WAKE_STRIDE &&
                   __builtin_offsetof(struct ap_wake_block, count) == AP_WAKE_COUNT,
               "ap.S offsets");
/* Host tests replace the two store forms to record their order. */
#ifndef AP_STORE
#define AP_STORE(lvalue, value) ((lvalue) = (value))
#define AP_RELEASE(lvalue, value) __atomic_store_n(&(lvalue), (value), __ATOMIC_RELEASE)
#endif
/* An AP acquire-loads rendez_entry and then reads rendez_gp. */
static inline void ap_register(volatile struct ap_wake_block *ap, uint64_t entry, uint64_t gp)
{
    AP_STORE(ap->rendez_gp, gp);
    AP_RELEASE(ap->rendez_entry, entry);
}
/* An AP acquire-loads count and then reads base and stride (ap.S). */
static inline void ap_publish(volatile struct ap_wake_block *ap, uint64_t base, uint32_t count)
{
    AP_STORE(ap->percpu_base, base);
    AP_STORE(ap->percpu_stride, (uint32_t)PERCPU_STRIDE);
    AP_RELEASE(ap->count, count);
}
#endif
#endif
