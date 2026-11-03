/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_SMP_H
#define IA64VMM_IA64_SMP_H
#include "ia64vmm/types.h"

struct vcpu;

/* The CPU model's view of the coherence domain. core/smp.c defines them;
 * ia64/smp_default.c gives weak single-vCPU definitions so that ia64/ links
 * without core/ (standalone tests): no other vCPU, nothing to post. */
bool ia64_smp_active(void); /* more than one vCPU exists */
void ia64_smp_global_purge(struct vcpu *v, uint32_t rid, uint64_t va, unsigned shift,
                           bool alat);      /* issuer, after its local purge */
void ia64_smp_apply_purges(struct vcpu *v); /* target; owner thread only */
/* The VM-wide ITC origin
 * (hal_counter() units) that vcpu_reset gives every vCPU, so all ITCs read
 * the same value at the same host instant, held APs included. core/smp.c
 * returns struct vm's itc_origin while a VM exists; the weak default (and
 * core outside a VM) returns hal_counter(), each reset's own origin. */
uint64_t ia64_smp_itc_origin(void);

/* Target operation in tlb.c: matching TC entries, or the whole TC on overrun. */
void ia64_tlb_remote_purge(struct vcpu *v, uint32_t rid, uint64_t va, unsigned shift, bool all);

/* Observer for a remote purge applied inside the issuer's wait.
 * The weak default does nothing; a lockstep recorder can emit LOCKSTEP_PURGE with
 * PURGE_ALL for all=true (zero rid/va/shift). Called on the owner only. */
void ia64_smp_purge_hook(struct vcpu *v, uint32_t rid, uint64_t va, unsigned shift, bool all);

/* Lockstep shadow issuer: consume the purges at this instruction, without posting. */
bool ia64_smp_replay_purges(struct vcpu *v);

/* v executes sync.i. Returns once no
 * other executor runs generated code retired before it. Weak default: none. */
void ia64_smp_sync_i(struct vcpu *v);
#endif
