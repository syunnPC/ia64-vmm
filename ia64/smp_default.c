/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Weak single-vCPU definitions of ia64/smp.h
 * for binaries that link ia64/ without core/ (the standalone CPU, engine and
 * Tier1 tests): there is no other vCPU, so no global purge has a target and
 * none is ever posted. core/smp.c overrides all of them in the VMM.
 */
#include "ia64/smp.h"
#include "ia64vmm/hal.h"

__attribute__((weak)) bool ia64_smp_active(void)
{
    return false;
}

__attribute__((weak)) void ia64_smp_global_purge(struct vcpu *v, uint32_t rid, uint64_t va,
                                                 unsigned shift, bool alat)
{
    (void)v;
    (void)rid;
    (void)va;
    (void)shift;
    (void)alat;
    /* ptc.g/ptc.ga release also orders host WC memory in standalone tests
     * (SDM Vol.2 §4.4.5; Intel SDM Vol.3A §11.2.2, §14.3). One vCPU
     * still owes the release, including WC visibility to devices. */
    __asm__ volatile("mfence" ::: "memory");
}

__attribute__((weak)) void ia64_smp_apply_purges(struct vcpu *v)
{
    (void)v;
}

__attribute__((weak)) uint64_t ia64_smp_itc_origin(void)
{
    return hal_counter();
}

__attribute__((weak)) void ia64_smp_purge_hook(struct vcpu *v, uint32_t rid, uint64_t va,
                                               unsigned shift, bool all)
{
    (void)v;
    (void)rid;
    (void)va;
    (void)shift;
    (void)all;
}

__attribute__((weak)) bool ia64_smp_replay_purges(struct vcpu *v)
{
    (void)v;
    return false;
}

__attribute__((weak)) void ia64_smp_sync_i(struct vcpu *v)
{
    (void)v;
}
