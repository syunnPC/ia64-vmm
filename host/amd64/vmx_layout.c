/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pure VMX data-structure builders for the real-mode container. No privileged instruction:
 * host/amd64/tests/vmx.c checks every structure hosted. */
#include "host/amd64/vmx.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

/* --- capabilities and control words ------------------ */

/* One control word: the planned 1s, the bits the capability MSR forces to
 * 1 (allowed-0 half, bits 31:0) added. A planned 1 the allowed-1 half
 * (bits 63:32) lacks, or a bit forced to 1 that is neither planned nor in
 * the word's default1 class (A.2), is a refusal. */
static bool control_word(const char *name, const char *msr_name, uint64_t msr, uint32_t planned,
                         uint32_t default1, uint32_t *word, char *why, size_t why_size)
{
    uint32_t allowed0 = (uint32_t)msr, allowed1 = (uint32_t)(msr >> 32);
    uint32_t missing = planned & ~allowed1, forced = allowed0 & ~planned & ~default1;
    if (missing || forced) {
        unsigned bit = (unsigned)__builtin_ctz(missing ? missing : forced);
        vmm_snprintf(why, why_size, "%s control bit %u cannot be %u (%s %llx)", name, bit,
                     missing ? 1u : 0u, msr_name, (unsigned long long)msr);
        return false;
    }
    *word = planned | allowed0;
    return true;
}

/* Host CR0/CR4 against IA32_VMX_CR*_FIXED0/1 (A.7, A.8): a bit FIXED0 sets
 * must be 1, a bit FIXED1 clears must be 0; bits in except are not checked. */
static bool fixed_bits(const char *reg, uint64_t value, uint64_t fixed0, uint64_t fixed1,
                       uint64_t except, char *why, size_t why_size)
{
    uint64_t bad = ((fixed0 & ~value) | (value & ~fixed1)) & ~except;
    if (!bad)
        return true;
    vmm_snprintf(why, why_size, "host %s %llx violates IA32_VMX_%s_FIXED0/1 (bit %u)", reg,
                 (unsigned long long)value, reg, (unsigned)__builtin_ctzll(bad));
    return false;
}

bool vmx_capabilities(uint32_t cpuid1_ecx, unsigned maxphyaddr, uint64_t host_cr0,
                      uint64_t host_cr4, vmx_rdmsr_fn rdmsr, void *ctx, struct vmx_caps *c,
                      char *why, size_t why_size)
{
    memset(c, 0, sizeof(*c));
    c->maxphyaddr = maxphyaddr;
    /* §26.6: CPUID.01H:ECX[5]; the MSRs below exist only when it is set. */
    if (!(cpuid1_ecx & (1u << 5))) {
        vmm_snprintf(why, why_size, "CPUID.01H:ECX.VMX is clear");
        return false;
    }
    /* §26.7: VMXON faults unless the lock bit and "VMXON outside SMX" are
     * set. The MSR is the firmware's; it is never written. */
    c->feature_control = rdmsr(ctx, MSR_FEATURE_CONTROL);
    if (!(c->feature_control & 1)) {
        vmm_snprintf(why, why_size,
                     "IA32_FEATURE_CONTROL unlocked (%llx): VMXON would fault; not written",
                     (unsigned long long)c->feature_control);
        return false;
    }
    if (!(c->feature_control & 4)) {
        vmm_snprintf(
            why, why_size,
            "VMX disabled by firmware (IA32_FEATURE_CONTROL=%llx: lock 1, VMXON outside SMX 0)",
            (unsigned long long)c->feature_control);
        return false;
    }
    /* A.1: revision 30:0, region size 44:32, 32-bit addresses 48, memory
     * type 53:50, true controls 55. */
    c->basic = rdmsr(ctx, MSR_VMX_BASIC);
    c->revision = (uint32_t)c->basic & 0x7fffffffu;
    c->vmcs_size = (uint32_t)(c->basic >> 32) & 0x1fffu;
    const char *field = !c->vmcs_size || c->vmcs_size > 4096 ? "VMCS size beyond 4096 bytes"
                        : (c->basic >> 48) & 1          ? "32-bit physical addresses only (bit 48)"
                        : ((c->basic >> 50) & 0xf) != 6 ? "VMCS memory type not write-back"
                                                        : NULL;
    if (field) {
        vmm_snprintf(why, why_size, "IA32_VMX_BASIC %llx: %s", (unsigned long long)c->basic, field);
        return false;
    }
    c->true_controls = (c->basic >> 55) & 1;
    c->pin_msr = rdmsr(ctx, c->true_controls ? MSR_VMX_TRUE_PINBASED : MSR_VMX_PINBASED);
    c->proc_msr = rdmsr(ctx, c->true_controls ? MSR_VMX_TRUE_PROCBASED : MSR_VMX_PROCBASED);
    c->exit_msr = rdmsr(ctx, c->true_controls ? MSR_VMX_TRUE_EXIT : MSR_VMX_EXIT);
    c->entry_msr = rdmsr(ctx, c->true_controls ? MSR_VMX_TRUE_ENTRY : MSR_VMX_ENTRY);
    c->misc = rdmsr(ctx, MSR_VMX_MISC);
    /* A.3.2: PROCBASED2 exists only with the 1-setting of bit 31. */
    if (!((c->proc_msr >> 32) & VMX_PROC_SECONDARY)) {
        vmm_snprintf(why, why_size, "no secondary processor-based controls");
        return false;
    }
    c->proc2_msr = rdmsr(ctx, MSR_VMX_PROCBASED2);
    uint32_t proc2_allowed1 = (uint32_t)(c->proc2_msr >> 32);
    const char *missing = !(proc2_allowed1 & VMX_PROC2_EPT)            ? "no EPT"
                          : !(proc2_allowed1 & VMX_PROC2_UNRESTRICTED) ? "no unrestricted guest"
                          : !(proc2_allowed1 & VMX_PROC2_WBINVD)       ? "no WBINVD exiting"
                                                                       : NULL;
    if (missing) {
        vmm_snprintf(why, why_size, "%s", missing);
        return false;
    }
    /* A.10: EPT_VPID_CAP exists with the 1-setting of "enable EPT". Bit 6
     * 4-level walk, 14 WB structures, 16/17 2 MiB/1 GiB leaves, 20 INVEPT,
     * 25 single-context, 26 all-context. */
    c->ept_vpid = rdmsr(ctx, MSR_VMX_EPT_VPID_CAP);
    missing = !((c->ept_vpid >> 6) & 1)    ? "EPT: no 4-level walk"
              : !((c->ept_vpid >> 14) & 1) ? "EPT: no write-back EPT structures"
              : !((c->ept_vpid >> 20) & 1) || !((c->ept_vpid >> 25) & 3) ? "EPT: no INVEPT"
                                                                         : NULL;
    if (missing) {
        vmm_snprintf(why, why_size, "%s", missing);
        return false;
    }
    c->ept_2m = (c->ept_vpid >> 16) & 1;
    c->ept_1g = (c->ept_vpid >> 17) & 1;
    c->invept_single = (c->ept_vpid >> 25) & 1;
    /* The five words. Exit and entry plan their default1 class
     * (save/load debug controls and reserved bits) as 1; with true controls
     * the pin-based and primary default1 bits, CR3-load/store exiting among
     * them, are 1 only where the CPU forces them. */
    if (!control_word("pin-based",
                      c->true_controls ? "IA32_VMX_TRUE_PINBASED_CTLS" : "IA32_VMX_PINBASED_CTLS",
                      c->pin_msr, VMX_PIN_EXTINT | VMX_PIN_NMI, VMX_PIN_DEFAULT1, &c->pin, why,
                      why_size) ||
        !control_word("primary processor-based",
                      c->true_controls ? "IA32_VMX_TRUE_PROCBASED_CTLS" : "IA32_VMX_PROCBASED_CTLS",
                      c->proc_msr, VMX_PROC_HLT | VMX_PROC_IO_BITMAPS | VMX_PROC_SECONDARY,
                      VMX_PROC_DEFAULT1, &c->proc, why, why_size) ||
        !control_word("secondary processor-based", "IA32_VMX_PROCBASED_CTLS2", c->proc2_msr,
                      VMX_PROC2_EPT | VMX_PROC2_WBINVD | VMX_PROC2_UNRESTRICTED, 0, &c->proc2, why,
                      why_size) ||
        !control_word("VM-exit",
                      c->true_controls ? "IA32_VMX_TRUE_EXIT_CTLS" : "IA32_VMX_EXIT_CTLS",
                      c->exit_msr, VMX_EXIT_DEFAULT1 | VMX_EXIT_HOST_64 | VMX_EXIT_LOAD_EFER,
                      VMX_EXIT_DEFAULT1, &c->exit, why, why_size) ||
        !control_word("VM-entry",
                      c->true_controls ? "IA32_VMX_TRUE_ENTRY_CTLS" : "IA32_VMX_ENTRY_CTLS",
                      c->entry_msr, VMX_ENTRY_DEFAULT1 | VMX_ENTRY_LOAD_EFER, VMX_ENTRY_DEFAULT1,
                      &c->entry, why, why_size))
        return false;
    c->cr0_fixed0 = rdmsr(ctx, MSR_VMX_CR0_FIXED0);
    c->cr0_fixed1 = rdmsr(ctx, MSR_VMX_CR0_FIXED1);
    c->cr4_fixed0 = rdmsr(ctx, MSR_VMX_CR4_FIXED0);
    c->cr4_fixed1 = rdmsr(ctx, MSR_VMX_CR4_FIXED1);
    /* §26.8: VMXON checks CR0/CR4 against them; CR4.VMXE is the one bit
     * create sets, so it is excepted. Nothing is adjusted. */
    if (!fixed_bits("CR0", host_cr0, c->cr0_fixed0, c->cr0_fixed1, 0, why, why_size) ||
        !fixed_bits("CR4", host_cr4, c->cr4_fixed0, c->cr4_fixed1, CR4_VMXE, why, why_size))
        return false;
    return true;
}

uint64_t vmx_cr0_mask(const struct vmx_caps *c)
{
    /* §27.6.6, §28.3: every bit VMX operation fixes is owned by the host
     * (PE and PG excepted: unrestricted guest), and CD/NW, which neither
     * VM entry nor VM exit changes (§29.3.2.1, §30.5.1). */
    return ((c->cr0_fixed0 | ~c->cr0_fixed1) & ~(CR0_PE | CR0_PG)) | CR0_CD | CR0_NW;
}

uint64_t vmx_cr4_mask(const struct vmx_caps *c)
{
    return c->cr4_fixed0 | ~c->cr4_fixed1;
}

/* --- access rights (Table 27-2) ------------------------------------------ */

uint32_t vmx_ar_from_attrib(uint16_t attrib, bool cs)
{
    /* Type, S, DPL and P (bits 7:0) are the same; AVL, L, D/B and G move
     * from 11:8 to 15:12. A segment other than CS that is not present is
     * written as unusable (bit 16), the state the processor itself gives a
     * null selector; CS is never unusable (§29.3.1.2). */
    uint32_t ar = (attrib & 0xffu) | ((uint32_t)(attrib & 0xf00u) << 4);
    if (!cs && !(attrib & 0x80u))
        ar |= 1u << 16;
    return ar;
}

uint16_t vmx_ar_to_attrib(uint32_t ar)
{
    if (ar & (1u << 16))
        return 0;
    return (uint16_t)((ar & 0xffu) | ((ar >> 4) & 0xf00u));
}

/* --- the VMCS field list ------------------------------ */

struct fields {
    struct vmx_field *out;
    unsigned n, max;
    bool overflow;
};
static void put(struct fields *f, uint32_t encoding, uint64_t value)
{
    if (f->n == f->max) {
        f->overflow = true;
        return;
    }
    f->out[f->n++] = (struct vmx_field){encoding, value};
}

/* The guest's CR0 and CR4 as VM entry needs them: the bits the
 * host owns (mask) take their VMX-operation values, CD/NW 0; the others are
 * the platform's. PE and PG are free under unrestricted guest. */
static uint64_t guest_cr0(const struct vmx_caps *c, uint64_t visible)
{
    uint64_t mask = vmx_cr0_mask(c);
    return (visible & ~mask) | (c->cr0_fixed0 & mask & ~(CR0_PE | CR0_PG | CR0_CD | CR0_NW));
}
static uint64_t guest_cr4(const struct vmx_caps *c, uint64_t visible)
{
    uint64_t mask = vmx_cr4_mask(c);
    return (visible & ~mask) | (c->cr4_fixed0 & mask);
}

unsigned vmx_vmcs_fields(const struct vmx_vmcs_params *p, struct vmx_field *out, unsigned max)
{
    const struct vmx_caps *c = p->caps;
    const struct vmx_host *h = p->host;
    struct fields f = {out, 0, max, false};
    /* Controls. Every exception exits (§27.6.3; with mask = match = 0 a #PF
     * follows bit 14 alone, §28.2), as the SVM container's do. */
    put(&f, VMCS_PIN_CONTROLS, c->pin);
    put(&f, VMCS_PROC_CONTROLS, c->proc);
    put(&f, VMCS_PROC2_CONTROLS, c->proc2);
    put(&f, VMCS_EXIT_CONTROLS, c->exit);
    put(&f, VMCS_ENTRY_CONTROLS, c->entry);
    put(&f, VMCS_EXCEPTION_BITMAP, UINT32_MAX);
    put(&f, VMCS_PF_ERROR_MASK, 0);
    put(&f, VMCS_PF_ERROR_MATCH, 0);
    put(&f, VMCS_CR3_TARGET_COUNT, 0);
    put(&f, VMCS_EXIT_MSR_STORE_COUNT, 0);
    put(&f, VMCS_EXIT_MSR_LOAD_COUNT, 0);
    put(&f, VMCS_ENTRY_MSR_LOAD_COUNT, 0);
    put(&f, VMCS_ENTRY_INTR_INFO, 0); /* nothing is ever injected */
    put(&f, VMCS_ENTRY_INTR_ERROR, 0);
    put(&f, VMCS_ENTRY_INSTR_LENGTH, 0);
    put(&f, VMCS_IO_BITMAP_A, p->io_bitmap_a);
    put(&f, VMCS_IO_BITMAP_B, p->io_bitmap_b);
    put(&f, VMCS_EPTP, p->eptp);
    put(&f, VMCS_CR0_MASK, vmx_cr0_mask(c));
    put(&f, VMCS_CR4_MASK, vmx_cr4_mask(c));
    put(&f, VMCS_CR0_SHADOW, VMX_CR0_SHADOW);
    put(&f, VMCS_CR4_SHADOW, VMX_CR4_SHADOW);
    /* Guest state that no call changes (§29.3.1.1, §29.3.1.5). */
    put(&f, VMCS_LINK_POINTER, UINT64_MAX);
    put(&f, VMCS_GUEST_DEBUGCTL, 0);
    put(&f, VMCS_GUEST_EFER, 0);
    put(&f, VMCS_GUEST_DR7, 0x400);
    put(&f, VMCS_GUEST_SYSENTER_CS, 0);
    put(&f, VMCS_GUEST_SYSENTER_ESP, 0);
    put(&f, VMCS_GUEST_SYSENTER_EIP, 0);
    put(&f, VMCS_GUEST_PENDING_DEBUG, 0);
    put(&f, VMCS_GUEST_INTERRUPTIBILITY, 0);
    put(&f, VMCS_GUEST_ACTIVITY, 0);
    /* LDTR (type 2) and TR (16-bit busy TSS, type 3): present system
     * segments, as the SVM container's (§29.3.1.2 TR must be usable). */
    put(&f, VMCS_GUEST_LDTR_SEL, 0);
    put(&f, VMCS_GUEST_LDTR_LIMIT, 0xffff);
    put(&f, VMCS_GUEST_LDTR_AR, 0x82);
    put(&f, VMCS_GUEST_LDTR_BASE, 0);
    put(&f, VMCS_GUEST_TR_SEL, 0);
    put(&f, VMCS_GUEST_TR_LIMIT, 0xffff);
    put(&f, VMCS_GUEST_TR_AR, 0x83);
    put(&f, VMCS_GUEST_TR_BASE, 0);
    /* Host state (§27.5, §29.2.2-§29.2.4); HOST_RSP is vmx_vmrun.S's. */
    put(&f, VMCS_HOST_ES_SEL, h->es);
    put(&f, VMCS_HOST_CS_SEL, h->cs);
    put(&f, VMCS_HOST_SS_SEL, h->ss);
    put(&f, VMCS_HOST_DS_SEL, h->ds);
    put(&f, VMCS_HOST_FS_SEL, h->fs);
    put(&f, VMCS_HOST_GS_SEL, h->gs);
    put(&f, VMCS_HOST_TR_SEL, h->tr);
    put(&f, VMCS_HOST_EFER, h->efer);
    put(&f, VMCS_HOST_SYSENTER_CS, h->sysenter_cs);
    put(&f, VMCS_HOST_CR0, h->cr0);
    put(&f, VMCS_HOST_CR3, h->cr3);
    put(&f, VMCS_HOST_CR4, h->cr4);
    put(&f, VMCS_HOST_FS_BASE, h->fs_base);
    put(&f, VMCS_HOST_GS_BASE, h->gs_base);
    put(&f, VMCS_HOST_TR_BASE, h->tr_base);
    put(&f, VMCS_HOST_GDTR_BASE, h->gdtr_base);
    put(&f, VMCS_HOST_IDTR_BASE, h->idtr_base);
    put(&f, VMCS_HOST_SYSENTER_ESP, h->sysenter_esp);
    put(&f, VMCS_HOST_SYSENTER_EIP, h->sysenter_eip);
    put(&f, VMCS_HOST_RIP, h->rip);
    return f.overflow ? 0 : f.n;
}

/* --- per-call registers ---------------------------------------------------- */

/* ES, CS, SS, DS, FS, GS: the order of the guest segment encodings. */
static struct x86rm_segment *segment_of(struct x86rm_regs *r, unsigned i)
{
    struct x86rm_segment *s[] = {&r->es, &r->cs, &r->ss, &r->ds, &r->fs, &r->gs};
    return s[i];
}

unsigned vmx_put_regs(const struct vmx_caps *c, const struct x86rm_regs *r, struct vmx_field *out,
                      unsigned max)
{
    struct fields f = {out, 0, max, false};
    /* Each kind of segment field steps by 2 from ES (B.1-B.4). */
    struct x86rm_regs copy = *r;
    for (unsigned i = 0; i < 6; ++i) {
        const struct x86rm_segment *s = segment_of(&copy, i);
        put(&f, VMCS_GUEST_ES_SEL + 2 * i, s->selector);
        put(&f, VMCS_GUEST_ES_LIMIT + 2 * i, s->limit);
        put(&f, VMCS_GUEST_ES_AR + 2 * i, vmx_ar_from_attrib(s->attrib, i == 1));
        put(&f, VMCS_GUEST_ES_BASE + 2 * i, s->base);
    }
    put(&f, VMCS_GUEST_GDTR_LIMIT, r->gdtr.limit);
    put(&f, VMCS_GUEST_GDTR_BASE, r->gdtr.base);
    put(&f, VMCS_GUEST_IDTR_LIMIT, r->idtr.limit);
    put(&f, VMCS_GUEST_IDTR_BASE, r->idtr.base);
    put(&f, VMCS_GUEST_RSP, r->rsp);
    put(&f, VMCS_GUEST_RIP, r->rip);
    put(&f, VMCS_GUEST_RFLAGS, r->rflags | 2);
    put(&f, VMCS_GUEST_CR0, guest_cr0(c, r->cr0));
    put(&f, VMCS_GUEST_CR3, r->cr3);
    put(&f, VMCS_GUEST_CR4, guest_cr4(c, r->cr4));
    return f.overflow ? 0 : f.n;
}

void vmx_get_regs(const struct vmx_caps *c, vmx_vmread_fn read, void *ctx, struct x86rm_regs *r)
{
    for (unsigned i = 0; i < 6; ++i) {
        struct x86rm_segment *s = segment_of(r, i);
        s->selector = (uint16_t)read(ctx, VMCS_GUEST_ES_SEL + 2 * i);
        s->limit = (uint32_t)read(ctx, VMCS_GUEST_ES_LIMIT + 2 * i);
        s->attrib = vmx_ar_to_attrib((uint32_t)read(ctx, VMCS_GUEST_ES_AR + 2 * i));
        s->base = read(ctx, VMCS_GUEST_ES_BASE + 2 * i);
    }
    r->gdtr = (struct x86rm_segment){0, 0, (uint32_t)read(ctx, VMCS_GUEST_GDTR_LIMIT),
                                     read(ctx, VMCS_GUEST_GDTR_BASE)};
    r->idtr = (struct x86rm_segment){0, 0, (uint32_t)read(ctx, VMCS_GUEST_IDTR_LIMIT),
                                     read(ctx, VMCS_GUEST_IDTR_BASE)};
    r->rsp = read(ctx, VMCS_GUEST_RSP);
    r->rip = read(ctx, VMCS_GUEST_RIP);
    r->rflags = read(ctx, VMCS_GUEST_RFLAGS);
    /* What the guest reads (§28.3): host-owned bits from the shadows. */
    uint64_t m0 = vmx_cr0_mask(c), m4 = vmx_cr4_mask(c);
    r->cr0 = (read(ctx, VMCS_GUEST_CR0) & ~m0) | (VMX_CR0_SHADOW & m0);
    r->cr3 = read(ctx, VMCS_GUEST_CR3);
    r->cr4 = (read(ctx, VMCS_GUEST_CR4) & ~m4) | (VMX_CR4_SHADOW & m4);
}

uint32_t vmx_entry_interruptibility(uint32_t saved, const struct x86rm_regs *r, uint16_t last_cs,
                                    uint64_t last_cs_base, uint64_t last_rip)
{
    /* The platform completed the instruction the last exit named
     * (another CS:RIP), so the STI or MOV SS shadow it covered is over; VM
     * entry would refuse STI blocking with IF = 0 (§29.3.1.5). */
    if (r->cs.selector != last_cs || r->cs.base != last_cs_base || r->rip != last_rip)
        return saved & ~3u;
    return saved;
}

/* --- I/O bitmaps (§27.6.4) ------------------------------------------------- */

void vmx_io_bitmaps(const uint8_t *pass, uint8_t *a, uint8_t *b)
{
    /* 1 = exit, the complement of io_pass; A covers 0000-7FFFh, B
     * 8000-FFFFh. A multi-byte access exits if any of its ports' bits is
     * set, one wrapping past FFFFh always (§28.1.3). */
    for (unsigned i = 0; i < 4096; ++i) {
        a[i] = (uint8_t)~pass[i];
        b[i] = (uint8_t)~pass[4096 + i];
    }
}

/* --- exit decoding ----------------------------------------------- */

bool vmx_decode_exit(const struct vmx_exit_fields *f, struct x86rm_exit *e)
{
    memset(e, 0, sizeof(*e));
    e->code = f->reason;
    e->info1 = f->qualification;
    e->info2 = f->idt_vectoring;
    /* Bit 31 (Table 27-19): VM entry failed while loading guest state
     * (33), MSRs (34) or on a machine check (41); the guest never ran. */
    if (f->reason & VMX_EXIT_ENTRY_FAILURE) {
        e->reason = X86RM_EXIT_INVALID;
        return true;
    }
    bool instruction = true;
    switch (f->reason & 0xffffu) {
    case VMX_EXIT_EXCEPTION_NMI: {
        /* Exiting-event identification, Table 27-20: vector 7:0, type
         * 10:8 (2 NMI, 3 hardware, 5/6 software exception), error code
         * valid 11. */
        unsigned type = (f->intr_info >> 8) & 7;
        e->reason = type == 2 ? X86RM_EXIT_NMI : X86RM_EXIT_EXCEPTION;
        e->vector = (uint8_t)f->intr_info;
        if (f->intr_info & (1u << 11))
            e->info1 = f->intr_error;
        instruction = false;
        break;
    }
    case VMX_EXIT_EXTERNAL_INTR:
        e->reason = X86RM_EXIT_INTR;
        instruction = false;
        break;
    case VMX_EXIT_TRIPLE_FAULT:
        e->reason = X86RM_EXIT_SHUTDOWN;
        instruction = false;
        break;
    case VMX_EXIT_CPUID:
        e->reason = X86RM_EXIT_CPUID;
        break;
    case VMX_EXIT_HLT:
        e->reason = X86RM_EXIT_HLT;
        break;
    case VMX_EXIT_INVD:
        e->reason = X86RM_EXIT_INVD;
        break;
    case VMX_EXIT_WBINVD:
        e->reason = X86RM_EXIT_WBINVD;
        break;
    case VMX_EXIT_VMCALL:
        e->reason = X86RM_EXIT_VMMCALL;
        break;
    case VMX_EXIT_RDMSR:
        e->reason = X86RM_EXIT_MSR;
        break;
    case VMX_EXIT_WRMSR:
        e->reason = X86RM_EXIT_MSR;
        e->msr_write = true;
        break;
    case VMX_EXIT_IO: {
        /* Table 30-5: size 2:0 (0, 1, 3 for 1, 2, 4 bytes), IN 3, string
         * 4, REP 5, port 31:16. */
        uint64_t q = f->qualification;
        e->reason = X86RM_EXIT_IO;
        e->size = (uint8_t)((q & 7) + 1);
        e->in = (q >> 3) & 1;
        e->string = (q >> 4) & 1;
        e->rep = (q >> 5) & 1;
        e->port = (uint16_t)(q >> 16);
        break;
    }
    case VMX_EXIT_EPT_VIOLATION: {
        /* Table 30-7: read 0, write 1, fetch 2; bits 5:3 the permissions
         * of the translation, zero when nothing maps the address. */
        uint64_t q = f->qualification;
        e->reason = X86RM_EXIT_NPF;
        e->gpa = f->guest_physical;
        e->npf_write = (q >> 1) & 1;
        e->npf_fetch = (q >> 2) & 1;
        e->npf_present = ((q >> 3) & 7) != 0;
        instruction = false;
        break;
    }
    case VMX_EXIT_EPT_MISCONFIG:
        e->gpa = f->guest_physical;
        return false;
    default:
        e->reason = X86RM_EXIT_OTHER;
        instruction = false;
        break;
    }
    /* §30.2.5: the instruction length is saved for each of these exits.
     * RIP + length wraps at 64 KiB in a 16-bit code segment (CS D/B,
     * access rights bit 14, clear), at 4 GiB otherwise. */
    if (instruction) {
        uint64_t next = f->rip + f->instr_length;
        e->next_rip = f->cs_ar & (1u << 14) ? next & 0xffffffffu : next & 0xffffu;
        e->next_rip_valid = true;
    }
    return true;
}

/* --- EPT (§31.3, Tables 31-2..31-7) ---------------------------------------- */

static unsigned shift_of(unsigned level)
{
    return 12 + 9 * (level - 1);
}

/* Install one leaf. Every entry on the path must be a table (non-leaf
 * R|W|X, bits 7:3 zero, Tables 31-2/31-4/31-6); the target entry must be
 * empty, so overlapping mappings are refused. */
static bool set_leaf(uint64_t *root, uint64_t gpa, unsigned level, uint64_t entry,
                     const struct vmx_ept_ops *ops)
{
    uint64_t *table = root;
    for (unsigned l = 4; l > level; --l) {
        uint64_t *slot = &table[(gpa >> shift_of(l)) & 511];
        if (!(*slot & (EPT_R | EPT_W | EPT_X))) {
            uint64_t *next = ops->alloc(ops->ctx);
            if (!next)
                return false;
            *slot = ops->pa(ops->ctx, next) | EPT_R | EPT_W | EPT_X;
        } else if (*slot & EPT_LARGE)
            return false;
        table = ops->va(ops->ctx, *slot & EPT_ADDR);
    }
    uint64_t *slot = &table[(gpa >> shift_of(level)) & 511];
    if (*slot & (EPT_R | EPT_W | EPT_X))
        return false;
    *slot = entry;
    return true;
}

bool vmx_ept_build(const struct x86rm_mapping *map, unsigned count, const struct vmx_ept_ops *ops,
                   uint64_t *root_pa)
{
    /* Bits 51:MAXPHYADDR of every address are reserved (Table 31-7). */
    uint64_t limit = ops->maxphyaddr >= 52 ? UINT64_C(1) << 52 : UINT64_C(1) << ops->maxphyaddr;
    uint64_t *root = ops->alloc(ops->ctx);
    if (!root)
        return false;
    for (unsigned i = 0; i < count; ++i) {
        const struct x86rm_mapping *m = &map[i];
        if (!m->size || ((m->gpa | m->size) & 4095) || m->gpa >= limit || m->size > limit - m->gpa)
            return false;
        /* Leaves: R and X always, W where the platform allows writes;
         * memory type WB (6) or UC (0) in bits 5:3 with ignore PAT, so the
         * type is the one written here whatever the MTRRs (§31.3.7.2). */
        uint64_t flags = EPT_R | EPT_X | (m->flags & X86RM_MAP_WRITE ? EPT_W : 0) |
                         (m->flags & X86RM_MAP_UC ? EPT_TYPE_UC : EPT_TYPE_WB) | EPT_IGNORE_PAT;
        for (uint64_t off = 0; off < m->size;) {
            uint64_t gpa = m->gpa + off, left = m->size - off, step = 4096;
            unsigned level = 1;
            uint64_t hpa;
            if (!m->hva) {
                hpa = gpa;
                if (ops->pages_1g && !(gpa & ((UINT64_C(1) << 30) - 1)) &&
                    left >= (UINT64_C(1) << 30)) {
                    level = 3;
                    step = UINT64_C(1) << 30;
                } else if (ops->pages_2m && !(gpa & ((UINT64_C(1) << 21) - 1)) &&
                           left >= (UINT64_C(1) << 21)) {
                    level = 2;
                    step = UINT64_C(1) << 21;
                }
            } else
                hpa = ops->pa(ops->ctx, m->hva + off);
            if ((hpa & ~EPT_ADDR) || hpa >= limit)
                return false;
            if (!set_leaf(root, gpa, level, hpa | flags | (level > 1 ? EPT_LARGE : 0), ops))
                return false;
            off += step;
        }
    }
    *root_pa = ops->pa(ops->ctx, root);
    return true;
}

static void enumerate(const uint64_t *table, unsigned level, uint64_t base,
                      const struct vmx_ept_ops *ops, vmx_ept_leaf_fn fn, void *arg)
{
    for (unsigned i = 0; i < 512; ++i) {
        uint64_t entry = table[i];
        if (!(entry & (EPT_R | EPT_W | EPT_X)))
            continue;
        uint64_t gpa = base + ((uint64_t)i << shift_of(level));
        if (level == 1 || (entry & EPT_LARGE))
            fn(arg, gpa, UINT64_C(1) << shift_of(level), entry);
        else
            enumerate(ops->va(ops->ctx, entry & EPT_ADDR), level - 1, gpa, ops, fn, arg);
    }
}

void vmx_ept_enumerate(uint64_t root_pa, const struct vmx_ept_ops *ops, vmx_ept_leaf_fn fn,
                       void *arg)
{
    enumerate(ops->va(ops->ctx, root_pa), 4, 0, ops, fn, arg);
}
