/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pure SVM data-structure builders for the real-mode container. No privileged instruction:
 * host/amd64/tests/svm.c checks every structure hosted. */
#include "host/amd64/svm.h"
#include "ia64vmm/string.h"

static void put16(uint8_t *p, unsigned off, uint16_t v)
{
    memcpy(p + off, &v, 2);
}
static void put32(uint8_t *p, unsigned off, uint32_t v)
{
    memcpy(p + off, &v, 4);
}
static void put64(uint8_t *p, unsigned off, uint64_t v)
{
    memcpy(p + off, &v, 8);
}
static uint16_t get16(const uint8_t *p, unsigned off)
{
    uint16_t v;
    memcpy(&v, p + off, 2);
    return v;
}
static uint32_t get32(const uint8_t *p, unsigned off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}
static uint64_t get64(const uint8_t *p, unsigned off)
{
    uint64_t v;
    memcpy(&v, p + off, 8);
    return v;
}

static void put_segment(uint8_t *vmcb, unsigned off, const struct x86rm_segment *s)
{
    put16(vmcb, off, s->selector);
    put16(vmcb, off + 2, s->attrib);
    put32(vmcb, off + 4, s->limit);
    put64(vmcb, off + 8, s->base);
}
static void get_segment(const uint8_t *vmcb, unsigned off, struct x86rm_segment *s)
{
    s->selector = get16(vmcb, off);
    s->attrib = get16(vmcb, off + 2);
    s->limit = get32(vmcb, off + 4);
    s->base = get64(vmcb, off + 8);
}

void svm_vmcb_init(uint8_t *vmcb, const struct svm_vmcb_params *p)
{
    memset(vmcb, 0, 4096);
    /* Every exception exits: the default IVT entries are IRETs, so a
     * reflected fault would only re-execute the faulting instruction. */
    put32(vmcb, VMCB_INTERCEPT_EXC, UINT32_MAX);
    put32(vmcb, VMCB_INTERCEPT_MISC1,
          SVM_MISC1_INTR | SVM_MISC1_NMI | SVM_MISC1_CPUID | SVM_MISC1_INVD | SVM_MISC1_HLT |
              SVM_MISC1_INVLPGA | SVM_MISC1_IOIO | SVM_MISC1_MSR | SVM_MISC1_SHUTDOWN);
    /* VMRUN's intercept is mandatory (APM §15.5.1). The guest's EFER.SVME
     * must be set, which makes the other SVM instructions legal in the
     * guest: they all exit. VMRUN leaves XCR0 to the host's value, so the
     * guest's XSETBV exits too. */
    put32(vmcb, VMCB_INTERCEPT_MISC2,
          SVM_MISC2_VMRUN | SVM_MISC2_VMMCALL | SVM_MISC2_VMLOAD | SVM_MISC2_VMSAVE |
              SVM_MISC2_STGI | SVM_MISC2_CLGI | SVM_MISC2_SKINIT | SVM_MISC2_WBINVD |
              SVM_MISC2_XSETBV);
    put64(vmcb, VMCB_IOPM_BASE, p->iopm_pa);
    put64(vmcb, VMCB_MSRPM_BASE, p->msrpm_pa);
    put32(vmcb, VMCB_ASID, p->asid);
#if defined(IA64VMM_SVM_TEST_NO_VINTR_MASKING)
    /* Test-only mutation: without
     * V_INTR_MASKING the guest's CLI masks host interrupts and the
     * cli-loop selftest never exits. Never set in product builds. */
    put32(vmcb, VMCB_INT_CTL, 0);
#else
    /* APM §15.21.1: guest RFLAGS.IF then masks only virtual interrupts;
     * physical ones follow the host IF saved at VMRUN (1), and the INTR
     * intercept turns each into an exit whatever the guest's CLI did. */
    put32(vmcb, VMCB_INT_CTL, SVM_V_INTR_MASKING);
#endif
    put64(vmcb, VMCB_NP_CONTROL, SVM_NP_ENABLE);
    put64(vmcb, VMCB_N_CR3, p->n_cr3);
    /* Real-mode descriptor caches: LDTR type 2, TR busy 16-bit TSS (3);
     * VMLOAD loads both, so they must be valid present system segments. */
    const struct x86rm_segment ldtr = {0, 0x82, 0xffff, 0}, tr = {0, 0x83, 0xffff, 0};
    put_segment(vmcb, VMCB_LDTR, &ldtr);
    put_segment(vmcb, VMCB_TR, &tr);
    vmcb[VMCB_CPL] = 0;
    put64(vmcb, VMCB_EFER, SVM_EFER_SVME);
    put64(vmcb, VMCB_DR7, 0x400);
    put64(vmcb, VMCB_DR6, 0xffff0ff0);
    /* Power-on PAT; guest paging is off, so only its entry 0 (WB) applies. */
    put64(vmcb, VMCB_GPAT, UINT64_C(0x0007040600070406));
}

void svm_vmcb_put_regs(uint8_t *vmcb, const struct x86rm_regs *r)
{
    put64(vmcb, VMCB_RAX, r->rax);
    put64(vmcb, VMCB_RSP, r->rsp);
    put64(vmcb, VMCB_RIP, r->rip);
    put64(vmcb, VMCB_RFLAGS, r->rflags | 2);
    put64(vmcb, VMCB_CR0, r->cr0);
    put64(vmcb, VMCB_CR3, r->cr3);
    put64(vmcb, VMCB_CR4, r->cr4);
    put_segment(vmcb, VMCB_ES, &r->es);
    put_segment(vmcb, VMCB_CS, &r->cs);
    put_segment(vmcb, VMCB_SS, &r->ss);
    put_segment(vmcb, VMCB_DS, &r->ds);
    put_segment(vmcb, VMCB_FS, &r->fs);
    put_segment(vmcb, VMCB_GS, &r->gs);
    put_segment(vmcb, VMCB_GDTR, &r->gdtr);
    put_segment(vmcb, VMCB_IDTR, &r->idtr);
}

void svm_vmcb_get_regs(const uint8_t *vmcb, struct x86rm_regs *r)
{
    r->rax = get64(vmcb, VMCB_RAX);
    r->rsp = get64(vmcb, VMCB_RSP);
    r->rip = get64(vmcb, VMCB_RIP);
    r->rflags = get64(vmcb, VMCB_RFLAGS);
    r->cr0 = get64(vmcb, VMCB_CR0);
    r->cr3 = get64(vmcb, VMCB_CR3);
    r->cr4 = get64(vmcb, VMCB_CR4);
    get_segment(vmcb, VMCB_ES, &r->es);
    get_segment(vmcb, VMCB_CS, &r->cs);
    get_segment(vmcb, VMCB_SS, &r->ss);
    get_segment(vmcb, VMCB_DS, &r->ds);
    get_segment(vmcb, VMCB_FS, &r->fs);
    get_segment(vmcb, VMCB_GS, &r->gs);
    get_segment(vmcb, VMCB_GDTR, &r->gdtr);
    get_segment(vmcb, VMCB_IDTR, &r->idtr);
}

void svm_iopm_build(uint8_t *iopm, const uint8_t *pass)
{
    /* APM §15.10.1: bit p intercepts port p; a multi-byte access checks
     * every byte's bit, and the three bits past FFFFh catch wrap-around. */
    memset(iopm, 0xff, SVM_IOPM_SIZE);
    for (unsigned port = 0; port < 65536; ++port)
        if (pass[port / 8] & (1u << (port & 7)))
            iopm[port / 8] &= (uint8_t)~(1u << (port & 7));
}

void svm_msrpm_build(uint8_t *msrpm)
{
    /* APM §15.11: every RDMSR/WRMSR of the three covered ranges exits; MSRs
     * outside the map always exit. */
    memset(msrpm, 0xff, SVM_MSRPM_SIZE);
}

void svm_decode_exit(const uint8_t *vmcb, bool nrips, struct x86rm_exit *e)
{
    memset(e, 0, sizeof(*e));
    uint64_t code = get64(vmcb, VMCB_EXITCODE);
    uint64_t info1 = get64(vmcb, VMCB_EXITINFO1), info2 = get64(vmcb, VMCB_EXITINFO2);
    e->code = code;
    e->info1 = info1;
    e->info2 = info2;
    /* nRIP is defined for intercepted instructions only (APM §15.7.1). */
    bool instruction = true;
    switch (code) {
    case SVM_EXIT_IOIO:
        /* APM §15.10.2: EXITINFO1 TYPE(0)=IN, STR(2), REP(3), SZ8/16/32
         * (4..6), PORT(31:16); EXITINFO2 is the next RIP. */
        e->reason = X86RM_EXIT_IO;
        e->in = info1 & 1;
        e->string = (info1 >> 2) & 1;
        e->rep = (info1 >> 3) & 1;
        e->size = info1 & 0x10 ? 1 : info1 & 0x20 ? 2 : info1 & 0x40 ? 4 : 0;
        e->port = (uint16_t)(info1 >> 16);
        e->next_rip = info2;
        e->next_rip_valid = true;
        return;
    case SVM_EXIT_MSR:
        e->reason = X86RM_EXIT_MSR;
        e->msr_write = info1 & 1;
        break;
    case SVM_EXIT_CPUID:
        e->reason = X86RM_EXIT_CPUID;
        break;
    case SVM_EXIT_HLT:
        e->reason = X86RM_EXIT_HLT;
        break;
    case SVM_EXIT_VMMCALL:
        e->reason = X86RM_EXIT_VMMCALL;
        break;
    case SVM_EXIT_INVD:
        e->reason = X86RM_EXIT_INVD;
        break;
    case SVM_EXIT_WBINVD:
        e->reason = X86RM_EXIT_WBINVD;
        break;
    case SVM_EXIT_INTR:
        e->reason = X86RM_EXIT_INTR;
        instruction = false;
        break;
    case SVM_EXIT_NMI:
        e->reason = X86RM_EXIT_NMI;
        instruction = false;
        break;
    case SVM_EXIT_SHUTDOWN:
        e->reason = X86RM_EXIT_SHUTDOWN;
        instruction = false;
        break;
    case SVM_EXIT_NPF:
        /* APM §15.25.6: EXITINFO1 is a #PF error code (P 0, RW 1, ID 4),
         * EXITINFO2 the faulting guest physical address. */
        e->reason = X86RM_EXIT_NPF;
        instruction = false;
        e->gpa = info2;
        e->npf_present = info1 & 1;
        e->npf_write = (info1 >> 1) & 1;
        e->npf_fetch = (info1 >> 4) & 1;
        break;
    case SVM_EXIT_INVALID:
        e->reason = X86RM_EXIT_INVALID;
        instruction = false;
        break;
    default:
        if (code >= SVM_EXIT_EXCP_BASE && code < SVM_EXIT_EXCP_BASE + 32) {
            e->reason = X86RM_EXIT_EXCEPTION;
            e->vector = (uint8_t)(code - SVM_EXIT_EXCP_BASE);
        } else
            e->reason = X86RM_EXIT_OTHER;
        instruction = false;
        break;
    }
    if (nrips && instruction) {
        e->next_rip = get64(vmcb, VMCB_NRIP);
        e->next_rip_valid = true;
    }
}

static unsigned shift_of(unsigned level)
{
    return 12 + 9 * (level - 1);
}

/* Install one leaf. Every entry on the path must be a table; the target
 * entry must be empty, so overlapping mappings are refused. */
static bool set_leaf(uint64_t *root, uint64_t gpa, unsigned level, uint64_t entry,
                     const struct svm_npt_ops *ops)
{
    uint64_t *table = root;
    for (unsigned l = 4; l > level; --l) {
        uint64_t *slot = &table[(gpa >> shift_of(l)) & 511];
        if (!(*slot & NPT_P)) {
            uint64_t *next = ops->alloc(ops->ctx);
            if (!next)
                return false;
            /* Non-leaf nested entries: present, writable, user (nested
             * walks are user accesses, APM §15.25.5); leaves decide. */
            *slot = ops->pa(ops->ctx, next) | NPT_P | NPT_W | NPT_U;
        } else if (*slot & NPT_PS)
            return false;
        table = ops->va(ops->ctx, *slot & NPT_ADDR);
    }
    uint64_t *slot = &table[(gpa >> shift_of(level)) & 511];
    if (*slot & NPT_P)
        return false;
    *slot = entry;
    return true;
}

bool svm_npt_build(const struct x86rm_mapping *map, unsigned count, const struct svm_npt_ops *ops,
                   uint64_t *root_pa)
{
    uint64_t *root = ops->alloc(ops->ctx);
    if (!root)
        return false;
    for (unsigned i = 0; i < count; ++i) {
        const struct x86rm_mapping *m = &map[i];
        if (!m->size || ((m->gpa | m->size) & 4095) || m->gpa > (UINT64_C(1) << 48) ||
            m->size > (UINT64_C(1) << 48) - m->gpa)
            return false;
        /* U always; W only where the platform allows writes; UC = PCD|PWT,
         * host PAT entry 3, WB = entry 0 (APM §15.25.8 type combining). */
        uint64_t flags = NPT_P | NPT_U | (m->flags & X86RM_MAP_WRITE ? NPT_W : 0) |
                         (m->flags & X86RM_MAP_UC ? NPT_PCD | NPT_PWT : 0);
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
                } else if (!(gpa & ((UINT64_C(1) << 21) - 1)) && left >= (UINT64_C(1) << 21)) {
                    level = 2;
                    step = UINT64_C(1) << 21;
                }
            } else
                hpa = ops->pa(ops->ctx, m->hva + off);
            if (hpa & ~NPT_ADDR)
                return false;
            if (!set_leaf(root, gpa, level, hpa | flags | (level > 1 ? NPT_PS : 0), ops))
                return false;
            off += step;
        }
    }
    *root_pa = ops->pa(ops->ctx, root);
    return true;
}

static void enumerate(const uint64_t *table, unsigned level, uint64_t base,
                      const struct svm_npt_ops *ops, svm_npt_leaf_fn fn, void *arg)
{
    for (unsigned i = 0; i < 512; ++i) {
        uint64_t entry = table[i];
        if (!(entry & NPT_P))
            continue;
        uint64_t gpa = base + ((uint64_t)i << shift_of(level));
        if (level == 1 || (entry & NPT_PS))
            fn(arg, gpa, UINT64_C(1) << shift_of(level), entry);
        else
            enumerate(ops->va(ops->ctx, entry & NPT_ADDR), level - 1, gpa, ops, fn, arg);
    }
}

void svm_npt_enumerate(uint64_t root_pa, const struct svm_npt_ops *ops, svm_npt_leaf_fn fn,
                       void *arg)
{
    enumerate(ops->va(ops->ctx, root_pa), 4, 0, ops, fn, arg);
}
