/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared CPU ownership and native IA-32 backend interfaces. */
#ifndef IA64VMM_AMD64_IA32_H
#define IA64VMM_AMD64_IA32_H

#include "ia64vmm/ia32.h"

/* Intel SDM Vol.2 CPUID.1:ECX, also used by AMD processors. */
static inline uint32_t amd64_ia32_native_features(uint32_t ecx)
{
    return ((ecx & 1) ? IA32_NATIVE_SSE3 : 0) |
           ((ecx & (1u << 23)) ? IA32_NATIVE_POPCNT : 0);
}

struct amd64_virtualization_cpu;
enum amd64_virtualization_user { AMD64_VIRT_VBIOS, AMD64_VIRT_IA32 };
/* Owner CPU only, guest stopped; not IRQ-safe. Compatible users share CPU
 * enablement; final release restores host state. Release private VMCS/VMCB
 * state and leases first. Failure sets *out=NULL without a leaked reference.
 * VBIOS and native IA-32 containers share this lifecycle. */
bool amd64_virtualization_acquire(unsigned host_cpu, enum ia32_backend_kind backend,
                                  enum amd64_virtualization_user user,
                                  struct amd64_virtualization_cpu **out,
                                  struct ia32_setup_error *error);
void amd64_virtualization_release(struct amd64_virtualization_cpu *,
                                  enum amd64_virtualization_user user);

/* ASID 0 is reserved for the host, 1 for VBIOS, and >=2 for native guests.
 * VMX disables VPID and uses INVEPT. Release requires stopped execution;
 * a new lease requires invalidation before entry. Tag or generation exhaustion
 * fails setup without aliasing a live tag or wrapping a generation. */
struct amd64_virtualization_tag {
    uint32_t id; /* ASID, or VPID (0 only with VPID disabled) */
    uint64_t generation;
};
bool amd64_virtualization_tag_acquire(struct amd64_virtualization_cpu *,
                                      enum amd64_virtualization_user user,
                                      struct amd64_virtualization_tag *out,
                                      struct ia32_setup_error *error);
void amd64_virtualization_tag_release(struct amd64_virtualization_cpu *,
                                      enum amd64_virtualization_user user,
                                      const struct amd64_virtualization_tag *);

/* Owner CPU, immediately before entry. SVM returns pending TLB_CONTROL=1;
 * VMX performs baseline INVEPT with VPID disabled. Failure forbids entry.
 * A new lease or changed nested root requires invalidation. Native table
 * mutations record tag invalidation before the next entry. */
bool amd64_virtualization_tag_prepare(struct amd64_virtualization_cpu *,
                                      enum amd64_virtualization_user,
                                      const struct amd64_virtualization_tag *, uint64_t eptp,
                                      uint8_t *svm_tlb_control);
bool amd64_virtualization_tag_invalidate(struct amd64_virtualization_cpu *,
                                         enum amd64_virtualization_user,
                                         const struct amd64_virtualization_tag *);

/* Acknowledge SVM flush consumption only after a valid VMRUN. Invalid entry
 * leaves it pending. VMX INVEPT is effective even if later entry fails. */
void amd64_virtualization_tag_entered(struct amd64_virtualization_cpu *,
                                      enum amd64_virtualization_user,
                                      const struct amd64_virtualization_tag *, bool entered);

/* Preserve all enabled host xstate; size follows CPUID.0DH and XCR0/XSS.
 * Read XSS only when supported. Guest state uses the legacy format.
 * Assembly brackets every entry, including failure, and restores host state
 * before C helpers or IRQ dispatch. IRQ/NMI handlers use general registers only.
 * Nonzero XSS is unsupported; AMD/Hygon host x87 must be initial because
 * conditional no-MF saves do not preserve its complete legacy pointers. */
struct amd64_ia32_fp_owner;
struct amd64_ia32_fp_requirements {
    size_t bytes, alignment;
    uint64_t xcr0, xss;
    bool xsave;
    bool host_x87_initial_only, xgetbv1;
};
bool amd64_ia32_fp_requirements(struct amd64_ia32_fp_requirements *out,
                                struct ia32_setup_error *error);
bool amd64_ia32_fp_create(struct amd64_ia32_fp_owner **out, struct ia32_setup_error *error);
void amd64_ia32_fp_destroy(struct amd64_ia32_fp_owner *);
/* Assembly entry/exit bracket only, one owner per CPU, no nesting. */
void amd64_ia32_fp_enter(struct amd64_ia32_fp_owner *, const struct ia32_fp_image *guest);
void amd64_ia32_fp_leave(struct amd64_ia32_fp_owner *, struct ia32_fp_image *guest);

extern const struct ia32_backend_ops amd64_svm_ia32_ops;
extern const struct ia32_backend_ops amd64_vmx_ia32_ops;

#endif
