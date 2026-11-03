/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Intel VMX structures for the x86 real-mode container. "SDM" is the Intel 64 and IA-32
 * Architectures Software Developer's Manual, order 325462-093US: VMCS field
 * encodings from Vol.3D Appendix B, capability MSRs from Appendix A,
 * control and exit bits from Vol.3C chapters 27-31. The builders in
 * vmx_layout.c are pure so that hosted tests can check them; vmx.c owns
 * CR4, the MSRs, CPUID and the VMX instructions.
 */
#ifndef VMM_AMD64_VMX_H
#define VMM_AMD64_VMX_H
#include "ia64vmm/x86rm.h"

/* VMCS field encodings, SDM Appendix B (B.1-B.4). */
#define VMCS_GUEST_ES_SEL 0x0800u
#define VMCS_GUEST_CS_SEL 0x0802u
#define VMCS_GUEST_SS_SEL 0x0804u
#define VMCS_GUEST_DS_SEL 0x0806u
#define VMCS_GUEST_FS_SEL 0x0808u
#define VMCS_GUEST_GS_SEL 0x080au
#define VMCS_GUEST_LDTR_SEL 0x080cu
#define VMCS_GUEST_TR_SEL 0x080eu
#define VMCS_HOST_ES_SEL 0x0c00u
#define VMCS_HOST_CS_SEL 0x0c02u
#define VMCS_HOST_SS_SEL 0x0c04u
#define VMCS_HOST_DS_SEL 0x0c06u
#define VMCS_HOST_FS_SEL 0x0c08u
#define VMCS_HOST_GS_SEL 0x0c0au
#define VMCS_HOST_TR_SEL 0x0c0cu
#define VMCS_IO_BITMAP_A 0x2000u
#define VMCS_IO_BITMAP_B 0x2002u
#define VMCS_EPTP 0x201au
#define VMCS_GUEST_PHYSICAL 0x2400u
#define VMCS_LINK_POINTER 0x2800u
#define VMCS_GUEST_DEBUGCTL 0x2802u
#define VMCS_GUEST_EFER 0x2806u
#define VMCS_HOST_EFER 0x2c02u
#define VMCS_PIN_CONTROLS 0x4000u
#define VMCS_PROC_CONTROLS 0x4002u
#define VMCS_EXCEPTION_BITMAP 0x4004u
#define VMCS_PF_ERROR_MASK 0x4006u
#define VMCS_PF_ERROR_MATCH 0x4008u
#define VMCS_CR3_TARGET_COUNT 0x400au
#define VMCS_EXIT_CONTROLS 0x400cu
#define VMCS_EXIT_MSR_STORE_COUNT 0x400eu
#define VMCS_EXIT_MSR_LOAD_COUNT 0x4010u
#define VMCS_ENTRY_CONTROLS 0x4012u
#define VMCS_ENTRY_MSR_LOAD_COUNT 0x4014u
#define VMCS_ENTRY_INTR_INFO 0x4016u
#define VMCS_ENTRY_INTR_ERROR 0x4018u
#define VMCS_ENTRY_INSTR_LENGTH 0x401au
#define VMCS_PROC2_CONTROLS 0x401eu
#define VMCS_INSTRUCTION_ERROR 0x4400u
#define VMCS_EXIT_REASON 0x4402u
#define VMCS_EXIT_INTR_INFO 0x4404u
#define VMCS_EXIT_INTR_ERROR 0x4406u
#define VMCS_IDT_VECTORING_INFO 0x4408u
#define VMCS_EXIT_INSTR_LENGTH 0x440cu
#define VMCS_GUEST_ES_LIMIT 0x4800u /* CS..TR follow at +2 each */
#define VMCS_GUEST_LDTR_LIMIT 0x480cu
#define VMCS_GUEST_TR_LIMIT 0x480eu
#define VMCS_GUEST_GDTR_LIMIT 0x4810u
#define VMCS_GUEST_IDTR_LIMIT 0x4812u
#define VMCS_GUEST_ES_AR 0x4814u /* CS..TR follow at +2 each */
#define VMCS_GUEST_LDTR_AR 0x4820u
#define VMCS_GUEST_TR_AR 0x4822u
#define VMCS_GUEST_INTERRUPTIBILITY 0x4824u
#define VMCS_GUEST_ACTIVITY 0x4826u
#define VMCS_GUEST_SYSENTER_CS 0x482au
#define VMCS_HOST_SYSENTER_CS 0x4c00u
#define VMCS_CR0_MASK 0x6000u
#define VMCS_CR4_MASK 0x6002u
#define VMCS_CR0_SHADOW 0x6004u
#define VMCS_CR4_SHADOW 0x6006u
#define VMCS_EXIT_QUALIFICATION 0x6400u
#define VMCS_GUEST_CR0 0x6800u
#define VMCS_GUEST_CR3 0x6802u
#define VMCS_GUEST_CR4 0x6804u
#define VMCS_GUEST_ES_BASE 0x6806u /* CS..TR follow at +2 each */
#define VMCS_GUEST_LDTR_BASE 0x6812u
#define VMCS_GUEST_TR_BASE 0x6814u
#define VMCS_GUEST_GDTR_BASE 0x6816u
#define VMCS_GUEST_IDTR_BASE 0x6818u
#define VMCS_GUEST_DR7 0x681au
#define VMCS_GUEST_RSP 0x681cu
#define VMCS_GUEST_RIP 0x681eu
#define VMCS_GUEST_RFLAGS 0x6820u
#define VMCS_GUEST_PENDING_DEBUG 0x6822u
#define VMCS_GUEST_SYSENTER_ESP 0x6824u
#define VMCS_GUEST_SYSENTER_EIP 0x6826u
#define VMCS_HOST_CR0 0x6c00u
#define VMCS_HOST_CR3 0x6c02u
#define VMCS_HOST_CR4 0x6c04u
#define VMCS_HOST_FS_BASE 0x6c06u
#define VMCS_HOST_GS_BASE 0x6c08u
#define VMCS_HOST_TR_BASE 0x6c0au
#define VMCS_HOST_GDTR_BASE 0x6c0cu
#define VMCS_HOST_IDTR_BASE 0x6c0eu
#define VMCS_HOST_SYSENTER_ESP 0x6c10u
#define VMCS_HOST_SYSENTER_EIP 0x6c12u
#define VMCS_HOST_RSP 0x6c14u /* written by vmx_vmrun.S */
#define VMCS_HOST_RIP 0x6c16u

/* Capability MSRs, SDM Appendix A; IA32_FEATURE_CONTROL, §26.7. */
#define MSR_FEATURE_CONTROL 0x03au
#define MSR_VMX_BASIC 0x480u
#define MSR_VMX_PINBASED 0x481u
#define MSR_VMX_PROCBASED 0x482u
#define MSR_VMX_EXIT 0x483u
#define MSR_VMX_ENTRY 0x484u
#define MSR_VMX_MISC 0x485u
#define MSR_VMX_CR0_FIXED0 0x486u
#define MSR_VMX_CR0_FIXED1 0x487u
#define MSR_VMX_CR4_FIXED0 0x488u
#define MSR_VMX_CR4_FIXED1 0x489u
#define MSR_VMX_PROCBASED2 0x48bu
#define MSR_VMX_EPT_VPID_CAP 0x48cu
#define MSR_VMX_TRUE_PINBASED 0x48du
#define MSR_VMX_TRUE_PROCBASED 0x48eu
#define MSR_VMX_TRUE_EXIT 0x48fu
#define MSR_VMX_TRUE_ENTRY 0x490u

/* Controls (§27.6.1 Table 27-5, §27.6.2 Tables 27-6/27-7, §27.7.1
 * Table 27-14, §27.8.1 Table 27-17). The default1 classes (Appendix A.2:
 * forced to 1 by the non-true capability MSRs) of each word follow. */
#define VMX_PIN_EXTINT (1u << 0)
#define VMX_PIN_NMI (1u << 3)
#define VMX_PROC_HLT (1u << 7)
#define VMX_PROC_CR3_LOAD (1u << 15)
#define VMX_PROC_CR3_STORE (1u << 16)
#define VMX_PROC_IO_BITMAPS (1u << 25)
#define VMX_PROC_MSR_BITMAPS (1u << 28)
#define VMX_PROC_SECONDARY (1u << 31)
#define VMX_PROC2_EPT (1u << 1)
#define VMX_PROC2_WBINVD (1u << 6)
#define VMX_PROC2_UNRESTRICTED (1u << 7)
#define VMX_EXIT_SAVE_DEBUG (1u << 2)
#define VMX_EXIT_HOST_64 (1u << 9)
#define VMX_EXIT_ACK_INTR (1u << 15)
#define VMX_EXIT_LOAD_EFER (1u << 21)
#define VMX_ENTRY_LOAD_DEBUG (1u << 2)
#define VMX_ENTRY_IA32E (1u << 9)
#define VMX_ENTRY_LOAD_EFER (1u << 15)
#define VMX_PIN_DEFAULT1 UINT32_C(0x00000016)   /* bits 1, 2, 4 */
#define VMX_PROC_DEFAULT1 UINT32_C(0x0401e172)  /* 1, 4-6, 8, 13-16, 26 */
#define VMX_EXIT_DEFAULT1 UINT32_C(0x00036dff)  /* 0-8, 10, 11, 13, 14, 16, 17 */
#define VMX_ENTRY_DEFAULT1 UINT32_C(0x000011ff) /* 0-8, 12 */

#define CR0_PE UINT64_C(0x00000001)
#define CR0_ET UINT64_C(0x00000010)
#define CR0_NW UINT64_C(0x20000000)
#define CR0_CD UINT64_C(0x40000000)
#define CR0_PG UINT64_C(0x80000000)
#define CR4_VMXE UINT64_C(0x00002000)

/* Basic exit reasons, SDM Appendix C. */
#define VMX_EXIT_EXCEPTION_NMI 0u
#define VMX_EXIT_EXTERNAL_INTR 1u
#define VMX_EXIT_TRIPLE_FAULT 2u
#define VMX_EXIT_CPUID 10u
#define VMX_EXIT_HLT 12u
#define VMX_EXIT_INVD 13u
#define VMX_EXIT_VMCALL 18u
#define VMX_EXIT_IO 30u
#define VMX_EXIT_RDMSR 31u
#define VMX_EXIT_WRMSR 32u
#define VMX_EXIT_EPT_VIOLATION 48u
#define VMX_EXIT_EPT_MISCONFIG 49u
#define VMX_EXIT_WBINVD 54u
#define VMX_EXIT_ENTRY_FAILURE (1u << 31)

/* EPT entries, §31.3.2 Tables 31-2..31-7, and the EPTP, Table 27-9. */
#define EPT_R UINT64_C(0x001)
#define EPT_W UINT64_C(0x002)
#define EPT_X UINT64_C(0x004)
#define EPT_TYPE_UC UINT64_C(0x000) /* bits 5:3 */
#define EPT_TYPE_WB UINT64_C(0x030)
#define EPT_IGNORE_PAT UINT64_C(0x040)
#define EPT_LARGE UINT64_C(0x080)
#define EPT_ADDR UINT64_C(0x000ffffffffff000)
#define EPTP_WB_4LEVEL UINT64_C(0x01e) /* type 6, walk length 4 - 1 */

/* One VMWRITE (or the value a VMREAD returned). The VMCS layout is
 * implementation-specific (§27.2): only encodings name its fields. */
struct vmx_field {
    uint32_t encoding;
    uint64_t value;
};
#define VMX_MAX_FIELDS 112u

/* The capability MSRs read in the order vmx_capabilities reads them and the five control
 * words chosen from them. rdmsr is called only for MSRs that exist on a
 * processor reporting what was read before (Appendix A). */
typedef uint64_t (*vmx_rdmsr_fn)(void *ctx, uint32_t msr);
struct vmx_caps {
    uint64_t feature_control, basic, misc, ept_vpid;
    uint64_t pin_msr, proc_msr, proc2_msr, exit_msr,
        entry_msr; /* allowed-0 31:0, allowed-1 63:32 */
    uint64_t cr0_fixed0, cr0_fixed1, cr4_fixed0, cr4_fixed1;
    uint32_t revision, vmcs_size;
    bool true_controls, ept_2m, ept_1g, invept_single;
    unsigned maxphyaddr;
    uint32_t pin, proc, proc2, exit, entry; /* the words written */
};
bool vmx_capabilities(uint32_t cpuid1_ecx, unsigned maxphyaddr, uint64_t host_cr0,
                      uint64_t host_cr4, vmx_rdmsr_fn rdmsr, void *ctx, struct vmx_caps *c,
                      char *why, size_t why_size);
/* The guest's CR0/CR4: masks, read shadows and the values VMX operation
 * fixes. */
uint64_t vmx_cr0_mask(const struct vmx_caps *c);
uint64_t vmx_cr4_mask(const struct vmx_caps *c);
#define VMX_CR0_SHADOW CR0_ET
#define VMX_CR4_SHADOW UINT64_C(0)

/* Host state for the host-state area (§27.5), read by vmx.c. */
struct vmx_host {
    uint64_t cr0, cr3, cr4, efer, fs_base, gs_base, tr_base, gdtr_base, idtr_base;
    uint64_t sysenter_esp, sysenter_eip, rip;
    uint32_t sysenter_cs;
    uint16_t es, cs, ss, ds, fs, gs, tr;
};
struct vmx_vmcs_params {
    const struct vmx_caps *caps;
    const struct vmx_host *host;
    uint64_t io_bitmap_a, io_bitmap_b, eptp;
};
/* Every field written once at create: controls, the fixed guest state
 * (LDTR, TR, MSRs, DR7, link pointer, activity) and the host state but
 * its RSP. Returns the count, 0 if max is too small. */
unsigned vmx_vmcs_fields(const struct vmx_vmcs_params *p, struct vmx_field *out, unsigned max);
/* The guest registers of struct x86rm_regs as VMCS fields, and back
 * (read calls VMREAD). RAX..R15 are not in the VMCS. */
unsigned vmx_put_regs(const struct vmx_caps *c, const struct x86rm_regs *r, struct vmx_field *out,
                      unsigned max);
typedef uint64_t (*vmx_vmread_fn)(void *ctx, uint32_t encoding);
void vmx_get_regs(const struct vmx_caps *c, vmx_vmread_fn read, void *ctx, struct x86rm_regs *r);
/* Interruptibility to write before an entry: blocking by STI and
 * by MOV SS cleared when the platform completed the instruction the last
 * exit named (the registers carry another CS:RIP). */
uint32_t vmx_entry_interruptibility(uint32_t saved, const struct x86rm_regs *r, uint16_t last_cs,
                                    uint64_t last_cs_base, uint64_t last_rip);

/* Access rights, APM attribute format <-> VMCS (Table 27-2). */
uint32_t vmx_ar_from_attrib(uint16_t attrib, bool cs);
uint16_t vmx_ar_to_attrib(uint32_t ar);

/* I/O bitmaps A (0000-7FFFh) and B (8000-FFFFh), 1 = exit, from io_pass
 * (8192 bytes, 1 = pass). */
void vmx_io_bitmaps(const uint8_t *pass, uint8_t *a, uint8_t *b);

/* The exit fields vmx.c reads, decoded into the contract: code = the exit
 * reason (bit 31 kept), info1 = the exception error code when valid, else
 * the exit qualification, info2 = the IDT-vectoring information (§27.9.3).
 * False: an EPT misconfiguration, a backend error (the EPT is the VMM's). */
struct vmx_exit_fields {
    uint32_t reason, intr_info, intr_error, idt_vectoring, instr_length, cs_ar;
    uint64_t qualification, guest_physical, rip;
};
bool vmx_decode_exit(const struct vmx_exit_fields *f, struct x86rm_exit *e);

struct vmx_ept_ops {
    void *(*alloc)(void *ctx);                 /* one zeroed 4 KiB table */
    uint64_t (*pa)(void *ctx, const void *va); /* host physical of a VMM page */
    void *(*va)(void *ctx, uint64_t pa);       /* table pointer from its address */
    void *ctx;
    bool pages_2m, pages_1g; /* EPT_VPID_CAP bits 16, 17 */
    unsigned maxphyaddr;     /* CPUID 80000008H:EAX[7:0] */
};
/* Exactly the given mappings; false on misalignment, overlap, an address
 * at or above 2^MAXPHYADDR or table exhaustion. */
bool vmx_ept_build(const struct x86rm_mapping *map, unsigned count, const struct vmx_ept_ops *ops,
                   uint64_t *root_pa);
typedef void (*vmx_ept_leaf_fn)(void *arg, uint64_t gpa, uint64_t size, uint64_t entry);
/* Every present leaf in ascending guest address order. */
void vmx_ept_enumerate(uint64_t root_pa, const struct vmx_ept_ops *ops, vmx_ept_leaf_fn fn,
                       void *arg);

/* The backend (vmx.c), called only by host/amd64/x86rm.c behind the
 * hal_x86rm_* contract of ia64vmm/hal.h, with the same meaning. */
struct vmx_container;
bool amd64_vmx_probe(const char **why);
bool amd64_vmx_create(const struct x86rm_config *cfg, struct vmx_container **out, const char **why);
bool amd64_vmx_run(struct vmx_container *rm, struct x86rm_regs *r, struct x86rm_exit *e);
void amd64_vmx_destroy(struct vmx_container *rm);
#endif
