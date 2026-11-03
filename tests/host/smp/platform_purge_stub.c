/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The platform fixture substitutes the CPU model and has one vCPU. Any
 * remote purge here would be a fixture defect, never a successful NOP. */
#include "ia64/smp.h"
#include "ia64vmm/hal.h"
/* No engine code or arena is linked into this CPU-model substitute. */
void code_quiescent(struct vcpu *v)
{
    (void)v;
}
void code_unregister_vcpu(struct vcpu *v)
{
    (void)v;
}
uint64_t code_epoch_sample(void)
{
    hal_panic("platform fixture unexpectedly waited in sync.i");
}
void ia64_tlb_remote_purge(struct vcpu *v, uint32_t rid, uint64_t va, unsigned shift, bool all)
{
    (void)v;
    (void)rid;
    (void)va;
    (void)shift;
    (void)all;
    hal_panic("platform fixture unexpectedly applied a remote TC purge");
}
void ia64_smp_purge_hook(struct vcpu *v, uint32_t rid, uint64_t va, unsigned shift, bool all)
{
    (void)v;
    (void)rid;
    (void)va;
    (void)shift;
    (void)all;
    hal_panic("platform fixture unexpectedly recorded a remote TC purge");
}
