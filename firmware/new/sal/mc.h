/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SAL machine check and INIT state. It lives in this
 * image's data, like ap_wake_block: SALE_ENTRY (sal/entry.S) runs in physical mode
 * and reads it at its link address; SAL_SET_VECTORS and
 * SAL_REGISTER_PHYSICAL_ADDR write it through a pointer the SAL runtime
 * converts at SetVirtualAddressMap. */
#ifndef VMM_SAL_MC_H
#define VMM_SAL_MC_H
/* Handlers: entry, gp, length_cs (SAL §9 SAL_SET_VECTORS). */
#define SAL_MC_OS_MCA 0
#define SAL_MC_OS_INIT 24 /* monarch; the non-monarch handler follows */
#define SAL_MC_HANDLER 24
#define SAL_MC_PAL_PROC 72    /* GR8 of OS_MCA/OS_INIT (SAL §4.8, §5.3) */
#define SAL_MC_SAL_PROC 80    /* GR9 */
#define SAL_MC_SAL_GP 88      /* GR10 */
#define SAL_MC_ACTIVE 96      /* processors in SAL_INIT */
#define SAL_MC_GENERATION 104 /* odd while the handlers change */
/* One 4 KB min-state area per processor, IA64VMM_VCPU_LIMIT of them. */
#define SAL_MIN_STATE_CPUS 32
#ifndef __ASSEMBLER__
#include <stdint.h>
struct sal_handler {
    uint64_t entry, gp, length_cs;
};
struct sal_mc {
    struct sal_handler os_mca, os_init[2];
    uint64_t pal_proc, sal_proc, sal_gp, active, generation;
};
_Static_assert(__builtin_offsetof(struct sal_mc, os_init) == SAL_MC_OS_INIT &&
                   sizeof(struct sal_handler) == SAL_MC_HANDLER &&
                   __builtin_offsetof(struct sal_mc, pal_proc) == SAL_MC_PAL_PROC &&
                   __builtin_offsetof(struct sal_mc, sal_proc) == SAL_MC_SAL_PROC &&
                   __builtin_offsetof(struct sal_mc, sal_gp) == SAL_MC_SAL_GP &&
                   __builtin_offsetof(struct sal_mc, active) == SAL_MC_ACTIVE &&
                   __builtin_offsetof(struct sal_mc, generation) == SAL_MC_GENERATION,
               "sal/entry.S offsets");
/* SAL_SET_VECTORS is not re-entrant (SAL §9 Table 9-2): the OS calls it on
 * one processor at a time.
 * SALE_ENTRY takes a handler only when the generation is even and the same
 * before and after its reads. Writes count handlers at h. */
static inline void sal_handlers_set(volatile struct sal_mc *mc, volatile struct sal_handler *h,
                                    const struct sal_handler *value, unsigned count)
{
    uint64_t generation = mc->generation;
    __atomic_store_n(&mc->generation, generation + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (unsigned i = 0; i < count; ++i) {
        h[i].entry = value[i].entry;
        h[i].gp = value[i].gp;
        h[i].length_cs = value[i].length_cs;
    }
    __atomic_store_n(&mc->generation, generation + 2, __ATOMIC_RELEASE);
}
#endif
#endif
