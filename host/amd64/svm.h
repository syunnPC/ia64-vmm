/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AMD SVM structures for the x86 real-mode container. Offsets and bits are from the AMD64
 * Architecture Programmer's Manual Vol.2 (24593), Appendix B (VMCB layout)
 * and chapter 15. The builders in svm_layout.c are pure so that hosted
 * tests can check them; svm.c owns MSRs, CPUID and VMRUN.
 */
#ifndef VMM_AMD64_SVM_H
#define VMM_AMD64_SVM_H
#include "ia64vmm/x86rm.h"

/* VMCB control area, APM Vol.2 Table B-1. */
#define VMCB_INTERCEPT_CR 0x000u
#define VMCB_INTERCEPT_DR 0x004u
#define VMCB_INTERCEPT_EXC 0x008u
#define VMCB_INTERCEPT_MISC1 0x00cu
#define VMCB_INTERCEPT_MISC2 0x010u
#define VMCB_IOPM_BASE 0x040u
#define VMCB_MSRPM_BASE 0x048u
#define VMCB_TSC_OFFSET 0x050u
#define VMCB_ASID 0x058u
#define VMCB_TLB_CONTROL 0x05cu
#define VMCB_INT_CTL 0x060u
#define VMCB_INT_VECTOR 0x064u
#define VMCB_INT_STATE 0x068u
#define VMCB_EXITCODE 0x070u
#define VMCB_EXITINFO1 0x078u
#define VMCB_EXITINFO2 0x080u
#define VMCB_EXITINTINFO 0x088u
#define VMCB_NP_CONTROL 0x090u
#define VMCB_EVENTINJ 0x0a8u
#define VMCB_N_CR3 0x0b0u
#define VMCB_CLEAN 0x0c0u
#define VMCB_NRIP 0x0c8u
/* State save area, APM Vol.2 Table B-2 (offsets from the VMCB start).
 * Each segment: selector u16, attrib u16, limit u32, base u64. */
#define VMCB_ES 0x400u
#define VMCB_CS 0x410u
#define VMCB_SS 0x420u
#define VMCB_DS 0x430u
#define VMCB_FS 0x440u
#define VMCB_GS 0x450u
#define VMCB_GDTR 0x460u
#define VMCB_LDTR 0x470u
#define VMCB_IDTR 0x480u
#define VMCB_TR 0x490u
#define VMCB_CPL 0x4cbu
#define VMCB_EFER 0x4d0u
#define VMCB_CR4 0x548u
#define VMCB_CR3 0x550u
#define VMCB_CR0 0x558u
#define VMCB_DR7 0x560u
#define VMCB_DR6 0x568u
#define VMCB_RFLAGS 0x570u
#define VMCB_RIP 0x578u
#define VMCB_RSP 0x5d8u
#define VMCB_RAX 0x5f8u
#define VMCB_CR2 0x640u
#define VMCB_GPAT 0x668u

/* Intercept vector 3 (offset 00Ch) and 4 (010h), APM Vol.2 Table B-1. */
#define SVM_MISC1_INTR (1u << 0)
#define SVM_MISC1_NMI (1u << 1)
#define SVM_MISC1_CPUID (1u << 18)
#define SVM_MISC1_INVD (1u << 22)
#define SVM_MISC1_HLT (1u << 24)
#define SVM_MISC1_INVLPGA (1u << 26)
#define SVM_MISC1_IOIO (1u << 27)
#define SVM_MISC1_MSR (1u << 28)
#define SVM_MISC1_SHUTDOWN (1u << 31)
#define SVM_MISC2_VMRUN (1u << 0)
#define SVM_MISC2_VMMCALL (1u << 1)
#define SVM_MISC2_VMLOAD (1u << 2)
#define SVM_MISC2_VMSAVE (1u << 3)
#define SVM_MISC2_STGI (1u << 4)
#define SVM_MISC2_CLGI (1u << 5)
#define SVM_MISC2_SKINIT (1u << 6)
#define SVM_MISC2_WBINVD (1u << 9)
#define SVM_MISC2_XSETBV (1u << 13)
#define SVM_V_INTR_MASKING (1u << 24) /* int_ctl, APM §15.21.1 */
#define SVM_NP_ENABLE 1u
#define SVM_EFER_SVME (1u << 12)

/* Exit codes, APM Vol.2 Appendix C. */
#define SVM_EXIT_EXCP_BASE 0x040u
#define SVM_EXIT_INTR 0x060u
#define SVM_EXIT_NMI 0x061u
#define SVM_EXIT_CPUID 0x072u
#define SVM_EXIT_INVD 0x076u
#define SVM_EXIT_HLT 0x078u
#define SVM_EXIT_IOIO 0x07bu
#define SVM_EXIT_MSR 0x07cu
#define SVM_EXIT_SHUTDOWN 0x07fu
#define SVM_EXIT_VMMCALL 0x081u
#define SVM_EXIT_WBINVD 0x089u
#define SVM_EXIT_NPF 0x400u
#define SVM_EXIT_INVALID UINT64_MAX

#define SVM_IOPM_SIZE 12288u
#define SVM_MSRPM_SIZE 8192u

/* Nested page table entries: the long-mode format (APM §15.25). */
#define NPT_P UINT64_C(0x001)
#define NPT_W UINT64_C(0x002)
#define NPT_U UINT64_C(0x004)
#define NPT_PWT UINT64_C(0x008)
#define NPT_PCD UINT64_C(0x010)
#define NPT_PS UINT64_C(0x080)
#define NPT_ADDR UINT64_C(0x000ffffffffff000)

struct svm_vmcb_params {
    uint64_t iopm_pa, msrpm_pa, n_cr3;
    uint32_t asid;
};
/* Control area and the fixed part of the real-mode state save area. */
void svm_vmcb_init(uint8_t *vmcb, const struct svm_vmcb_params *p);
/* Guest registers held in the VMCB (all but RBX..RBP, which VMRUN leaves
 * in the CPU's registers). */
void svm_vmcb_put_regs(uint8_t *vmcb, const struct x86rm_regs *r);
void svm_vmcb_get_regs(const uint8_t *vmcb, struct x86rm_regs *r);
/* pass: 8192 bytes, bit set = port not intercepted. */
void svm_iopm_build(uint8_t *iopm, const uint8_t *pass);
void svm_msrpm_build(uint8_t *msrpm);
/* Translate the exit fields; nRIP is used only with NRIPS. */
void svm_decode_exit(const uint8_t *vmcb, bool nrips, struct x86rm_exit *e);

struct svm_npt_ops {
    void *(*alloc)(void *ctx);                 /* one zeroed 4 KiB table */
    uint64_t (*pa)(void *ctx, const void *va); /* host physical of a VMM page */
    void *(*va)(void *ctx, uint64_t pa);       /* table pointer from its address */
    void *ctx;
    bool pages_1g; /* CPUID 8000_0001h EDX[26] */
};
/* Exactly the given mappings; false on misalignment, overlap, an address
 * beyond 48 bits or table exhaustion. */
bool svm_npt_build(const struct x86rm_mapping *map, unsigned count, const struct svm_npt_ops *ops,
                   uint64_t *root_pa);
typedef void (*svm_npt_leaf_fn)(void *arg, uint64_t gpa, uint64_t size, uint64_t entry);
/* Every present leaf in ascending guest address order. */
void svm_npt_enumerate(uint64_t root_pa, const struct svm_npt_ops *ops, svm_npt_leaf_fn fn,
                       void *arg);

/* The backend (svm.c), called only by host/amd64/x86rm.c behind the
 * hal_x86rm_* contract of ia64vmm/hal.h, with the same meaning. */
struct svm_container;
bool amd64_svm_probe(const char **why);
bool amd64_svm_create(const struct x86rm_config *cfg, struct svm_container **out, const char **why);
bool amd64_svm_run(struct svm_container *rm, struct x86rm_regs *r, struct x86rm_exit *e);
void amd64_svm_destroy(struct svm_container *rm);
#endif
