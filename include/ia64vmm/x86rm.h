/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * x86 real-mode container.
 *
 * The HAL runs x86 code natively in a hardware-virtualized guest (AMD SVM
 * with nested paging, or Intel VMX with EPT and unrestricted guest, on bare
 * metal) whose physical address space holds
 * exactly the mappings listed in struct x86rm_config. Platform code owns
 * the policy: what is mapped, which ports reach hardware without an exit,
 * and the answer to every exit. The HAL owns the mechanism and the
 * hardware state. Functions are declared in ia64vmm/hal.h.
 *
 * Context: management CPU (the BSP) only. Initial creation is before any
 * guest runs; the hal_x86rm_reclaim_create interface also permits
 * diagnostic reuse after every IA-64 guest executor has stopped and joined.
 */
#ifndef IA64VMM_X86RM_H
#define IA64VMM_X86RM_H

#include "ia64vmm/types.h"

#define X86RM_MAP_WRITE (1u << 0) /* guest may write; else read only */
#define X86RM_MAP_UC (1u << 1)    /* uncacheable; else write-back */

/* One guest physical range, 4 KiB aligned. hva = VMM pages from
 * hal_alloc_pages backing the whole range; hva = NULL maps the host
 * physical range at the same address (identity: device windows). */
struct x86rm_mapping {
    uint64_t gpa, size;
    uint8_t *hva;
    unsigned flags;
};

struct x86rm_config {
    const struct x86rm_mapping *map;
    unsigned map_count;
    /* 8192 bytes: bit p set = port p reaches the hardware without an exit.
     * Every other port access exits. */
    const uint8_t *io_pass;
};

/* Segment as the hardware keeps it (APM Vol.2 Appendix B attribute format:
 * type 3:0, S 4, DPL 6:5, P 7, AVL 8, L 9, D/B 10, G 11). The VMX backend
 * converts to and from the VMCS access rights (SDM Vol.3C Table 27-2: bits
 * 11:8 here are its 15:12) and writes a segment other than CS with P = 0
 * as unusable (bit 16); an unusable segment reads back as attrib 0. */
struct x86rm_segment {
    uint16_t selector, attrib;
    uint32_t limit;
    uint64_t base;
};

/* Guest state at entry and after every exit. Real-mode code cannot name
 * R8-R15; the HAL runs the guest with them zero and does not report them. */
struct x86rm_regs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t rip, rflags, cr0, cr3, cr4;
    struct x86rm_segment es, cs, ss, ds, fs, gs, gdtr, idtr;
};

enum x86rm_exit_reason {
    X86RM_EXIT_IO,
    X86RM_EXIT_MSR,
    X86RM_EXIT_CPUID,
    X86RM_EXIT_HLT,
    X86RM_EXIT_VMMCALL, /* the backend's hypercall (x86rm_info.hypercall) */
    X86RM_EXIT_INVD,    /* the HAL has executed WBINVD in its place */
    X86RM_EXIT_WBINVD,  /* the HAL has executed WBINVD */
    X86RM_EXIT_INTR,    /* a host interrupt; the host has handled it */
    X86RM_EXIT_NMI,     /* the host's NMI handler has run */
    X86RM_EXIT_EXCEPTION,
    X86RM_EXIT_NPF,      /* access outside the configured map */
    X86RM_EXIT_SHUTDOWN, /* triple fault */
    X86RM_EXIT_INVALID,  /* the hardware refused the guest state (SVM
                            * VMEXIT_INVALID, VMX exit reason bit 31) */
    X86RM_EXIT_OTHER,    /* any other intercept (SVM instructions, VMX CR
                            * access, ...): code is the raw reason */
};

struct x86rm_exit {
    enum x86rm_exit_reason reason;
    uint64_t code, info1, info2; /* backend's raw exit code and details */
    /* Next instruction pointer when the hardware reports it: IOIO exits
     * always, the others only with next-RIP saving (SVM NRIPS); on VMX
     * every instruction exit (the VM-exit instruction length). */
    uint64_t next_rip;
    bool next_rip_valid;
    /* X86RM_EXIT_IO */
    uint16_t port;
    uint8_t size; /* 1, 2 or 4 */
    bool in, string, rep;
    /* X86RM_EXIT_MSR: wrmsr when true */
    bool msr_write;
    /* X86RM_EXIT_EXCEPTION */
    uint8_t vector;
    /* X86RM_EXIT_NPF */
    uint64_t gpa;
    bool npf_write, npf_fetch, npf_present;
    /* X86RM_EXIT_CPUID: the executing host CPU's EAX, EBX, ECX, EDX for the
     * guest's EAX/ECX at the exit. */
    uint32_t cpuid[4];
};

/* The backend behind the four calls (hal_x86rm_info, valid after
 * hal_x86rm_probe): its name for the log, and the 3-byte instruction the
 * container's stubs end with to hand a call back to the platform. */
struct x86rm_info {
    const char *name;     /* "SVM", "VMX"; hosted: "hardware virtualization" */
    uint8_t hypercall[3]; /* VMMCALL 0F 01 D9 (SVM), VMCALL 0F 01 C1 (VMX) */
};

#endif
