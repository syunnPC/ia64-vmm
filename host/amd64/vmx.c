/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Intel VMX backend of the x86 real-mode container: detection from CPUID and
 * the IA32_VMX_* MSRs and one VM entry per hal_x86rm_run. Shared host
 * enablement and tag leases live in virtualization.c.
 * host/amd64/x86rm.c calls it on an Intel CPU. The VMCS contents come from
 * vmx_layout.c; this file owns the private VMCS and entry state.
 * "SDM": Intel 64 and IA-32 Architectures Software Developer's Manual,
 * order 325462-093US, Vol.3C chapters 26-33.
 */
#include "host/amd64/amd64.h"
#include "host/amd64/simd.h"
#include "host/amd64/vmx.h"
#include "host/amd64/ia32.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define MSR_SYSENTER_CS 0x174u
#define MSR_SYSENTER_ESP 0x175u
#define MSR_SYSENTER_EIP 0x176u
#define MSR_EFER 0xc0000080u
#define MSR_FS_BASE 0xc0000100u
#define MSR_GS_BASE 0xc0000101u
#define MAX_TABLES 512u

/* Layout shared with vmx_vmrun.S (G_* offsets). */
struct vmx_gprs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15;
};
int amd64_vmx_vmrun(struct vmx_gprs *gprs, int launched, const struct amd64_xstate_pair *xstate);
extern char amd64_vmx_exit[];

struct vmx_container {
    uint8_t *vmcs, *io_a, *io_b;
    struct amd64_virtualization_cpu *owner;
    struct amd64_virtualization_tag tag;
    unsigned cpu;
    struct vmx_field fields[VMX_MAX_FIELDS];
    struct amd64_xstate_pair xstate;
    void *tables[MAX_TABLES];
    unsigned table_count;
    struct vmx_caps caps;
    uint64_t eptp;
    bool launched, enabled, vmcs_active;
    struct vmx_gprs gprs;
    uint16_t last_cs;
    uint64_t last_cs_base, last_rip;
};
#define CONTAINER_PAGES ((sizeof(struct vmx_container) + 4095) / 4096)
static char refusals[BOOT_MAX_CPUS][160];
#define refusal refusals[hal_cpu_index()]

/* --- VMX instructions (SDM Chapter 33): CF=1 VMfailInvalid, ZF=1
 * VMfailValid; "setna" catches both. ------------------------------------- */

static bool vmx_clear(uint64_t pa)
{
    bool bad;
    __asm__ volatile("vmclear %1; setna %0" : "=q"(bad) : "m"(pa) : "cc", "memory");
    return !bad;
}
static bool vmx_ptrld(uint64_t pa)
{
    bool bad;
    __asm__ volatile("vmptrld %1; setna %0" : "=q"(bad) : "m"(pa) : "cc", "memory");
    return !bad;
}
static bool vmx_write(uint64_t field, uint64_t value)
{
    bool bad;
    __asm__ volatile("vmwrite %2, %1; setna %0"
                     : "=q"(bad)
                     : "r"(field), "rm"(value)
                     : "cc", "memory");
    return !bad;
}
static uint64_t vmx_read(uint64_t field)
{
    uint64_t value = 0;
    __asm__ volatile("vmread %1, %0" : "=rm"(value) : "r"(field) : "cc");
    return value;
}
static uint64_t vmread_fn(void *ctx, uint32_t encoding)
{
    (void)ctx;
    return vmx_read(encoding);
}
static uint64_t read_cr0(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}
static uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static uint64_t rdmsr_fn(void *ctx, uint32_t msr)
{
    (void)ctx;
    return amd64_rdmsr(msr);
}

/* CPUID, then the MSRs in the order vmx_capabilities reads them
 * (each exists only when what was read before says so). */
static bool probe(struct vmx_caps *c, const char **why)
{
    uint32_t a, b, cx, d;
    amd64_cpuid(1, 0, &a, &b, &cx, &d);
    uint32_t ecx1 = cx;
    unsigned maxphyaddr = 36;
    amd64_cpuid(0x80000000, 0, &a, &b, &cx, &d);
    if (a >= 0x80000008) {
        amd64_cpuid(0x80000008, 0, &a, &b, &cx, &d);
        maxphyaddr = a & 0xff;
    }
    if (!vmx_capabilities(ecx1, maxphyaddr, read_cr0(), read_cr4(), rdmsr_fn, NULL, c, refusal,
                          sizeof(refusal))) {
        *why = refusal;
        return false;
    }
    return true;
}

bool amd64_vmx_probe(const char **why)
{
    struct vmx_caps c;
    return probe(&c, why);
}

/* Container pages come from hal_alloc_pages without HAL_PAGE_EXEC, so they
 * live in the HHDM: physical = virtual - HHDM offset. */
static uint64_t physical(const void *p)
{
    return (uintptr_t)p - amd64_hhdm;
}
static void *ept_alloc(void *ctx)
{
    struct vmx_container *rm = ctx;
    if (rm->table_count == MAX_TABLES)
        return NULL;
    void *p = hal_alloc_pages(1, HAL_PAGE_ZERO);
    if (p)
        rm->tables[rm->table_count++] = p;
    return p;
}
static uint64_t ept_pa(void *ctx, const void *va)
{
    (void)ctx;
    return physical(va);
}
static void *ept_va(void *ctx, uint64_t pa)
{
    (void)ctx;
    return (void *)(uintptr_t)(amd64_hhdm + pa);
}

static void release(struct vmx_container *rm)
{
    for (unsigned i = 0; i < rm->table_count; ++i)
        hal_free_pages(rm->tables[i], 1);
    rm->table_count = 0;
    if (rm->vmcs)
        hal_free_pages(rm->vmcs, 1);
    if (rm->io_a)
        hal_free_pages(rm->io_a, 1);
    if (rm->io_b)
        hal_free_pages(rm->io_b, 1);
    if (rm->xstate.host)
        hal_free_pages(rm->xstate.host, amd64_xstate_pages());
    if (rm->xstate.guest)
        hal_free_pages(rm->xstate.guest, amd64_xstate_pages());
    if (rm->owner) {
        amd64_virtualization_tag_release(rm->owner, AMD64_VIRT_VBIOS, &rm->tag);
        amd64_virtualization_release(rm->owner, AMD64_VIRT_VBIOS);
    }
    hal_free_pages(rm, CONTAINER_PAGES);
}

/* Never free a VMCS that hardware may still own. */
static bool leave(struct vmx_container *rm)
{
    if (rm->vmcs_active && !vmx_clear(physical(rm->vmcs)))
        return false;
    rm->vmcs_active = false;
    return true;
}

/* The host-state area (§27.5): this CPU's state as it runs now,
 * with CR4.VMXE set. HOST_RSP is written by vmx_vmrun.S at each entry. */
static void host_state(struct vmx_host *h)
{
    struct {
        uint16_t limit;
        uint64_t base;
    } IA64VMM_PACKED gdtr, idtr;
    uint16_t sel;
    memset(h, 0, sizeof(*h));
    __asm__ volatile("sgdt %0; sidt %1" : "=m"(gdtr), "=m"(idtr));
    __asm__ volatile("mov %%cs, %0" : "=r"(sel));
    h->cs = sel;
    __asm__ volatile("mov %%ss, %0" : "=r"(sel));
    h->ss = sel;
    __asm__ volatile("mov %%ds, %0" : "=r"(sel));
    h->ds = sel;
    __asm__ volatile("mov %%es, %0" : "=r"(sel));
    h->es = sel;
    __asm__ volatile("mov %%fs, %0" : "=r"(sel));
    h->fs = sel;
    __asm__ volatile("mov %%gs, %0" : "=r"(sel));
    h->gs = sel;
    __asm__ volatile("str %0" : "=r"(sel));
    h->tr = sel;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    h->cr0 = read_cr0();
    h->cr3 = cr3;
    h->cr4 = read_cr4();
    h->efer = amd64_rdmsr(MSR_EFER);
    h->fs_base = amd64_rdmsr(MSR_FS_BASE);
    h->gs_base = amd64_rdmsr(MSR_GS_BASE); /* the per-CPU pointer */
    h->sysenter_cs = (uint32_t)amd64_rdmsr(MSR_SYSENTER_CS);
    h->sysenter_esp = amd64_rdmsr(MSR_SYSENTER_ESP);
    h->sysenter_eip = amd64_rdmsr(MSR_SYSENTER_EIP);
    h->gdtr_base = gdtr.base;
    h->idtr_base = idtr.base;
    /* The 16-byte TSS descriptor at GDT + TR: base 23:0 in bits 39:16,
     * 31:24 in 63:56, 63:32 in the next quadword (SDM Vol.3A §9.2.3). */
    const uint64_t *d = (const uint64_t *)(uintptr_t)(gdtr.base + (h->tr & ~7u));
    h->tr_base =
        ((d[0] >> 16) & 0xffffff) | (((d[0] >> 56) & 0xff) << 24) | ((d[1] & 0xffffffffu) << 32);
    h->rip = (uintptr_t)amd64_vmx_exit;
}

static bool write_fields(const struct vmx_field *f, unsigned n, uint32_t *failed)
{
    for (unsigned i = 0; i < n; ++i)
        if (!vmx_write(f[i].encoding, f[i].value)) {
            *failed = f[i].encoding;
            return false;
        }
    return true;
}

static const char *instruction_failure(const char *what)
{
    /* VMfailValid leaves the VM-instruction error (Table 33-1); with
     * VMfailInvalid there is no current VMCS to hold one. */
    uint64_t error = vmx_read(VMCS_INSTRUCTION_ERROR);
    vmm_snprintf(refusal, sizeof(refusal), "%s failed (VM-instruction error %llu)", what,
                 (unsigned long long)error);
    return refusal;
}

bool amd64_vmx_create(const struct x86rm_config *cfg, struct vmx_container **out, const char **why)
{
    struct vmx_caps c;
    *out = NULL;
    if (!probe(&c, why))
        return false;
    struct vmx_container *rm = hal_alloc_pages(CONTAINER_PAGES, HAL_PAGE_ZERO);
    if (!rm) {
        *why = "VMM reservation exhausted";
        return false;
    }
    rm->cpu = hal_cpu_index();
    rm->caps = c;
    struct ia32_setup_error error;
    if (!amd64_virtualization_acquire(rm->cpu, IA32_BACKEND_VMX, AMD64_VIRT_VBIOS, &rm->owner,
                                      &error) ||
        !amd64_virtualization_tag_acquire(rm->owner, AMD64_VIRT_VBIOS, &rm->tag, &error)) {
        *why = error.detail;
        release(rm);
        return false;
    }
    rm->vmcs = hal_alloc_pages(1, HAL_PAGE_ZERO);
    rm->io_a = hal_alloc_pages(1, HAL_PAGE_ZERO);
    rm->io_b = hal_alloc_pages(1, HAL_PAGE_ZERO);
    /* XSAVE areas for every component in XCR0 (amd64_xstate_bytes). */
    rm->xstate.host = hal_alloc_pages(amd64_xstate_pages(), HAL_PAGE_ZERO);
    rm->xstate.guest = hal_alloc_pages(amd64_xstate_pages(), HAL_PAGE_ZERO);
    if (!rm->vmcs || !rm->io_a || !rm->io_b || !rm->xstate.host || !rm->xstate.guest) {
        release(rm);
        *why = "VMM reservation exhausted";
        return false;
    }
    amd64_xstate_init(rm->xstate.guest, amd64_xstate_bytes);
    vmx_io_bitmaps(cfg->io_pass, rm->io_a, rm->io_b);
    const struct vmx_ept_ops ops = {ept_alloc, ept_pa,   ept_va,      rm,
                                    c.ept_2m,  c.ept_1g, c.maxphyaddr};
    uint64_t root;
    if (!vmx_ept_build(cfg->map, cfg->map_count, &ops, &root)) {
        release(rm);
        *why = "EPT refused the map (alignment, overlap, MAXPHYADDR or table pool)";
        return false;
    }
    /* Table 27-9: WB paging structures (6), walk length 4 - 1 in 5:3. */
    rm->eptp = root | EPTP_WB_4LEVEL;
    memcpy(rm->vmcs, &c.revision, 4);
    if (!vmx_clear(physical(rm->vmcs))) {
        *why = "VMCLEAR failed";
        goto fail;
    }
    if (!vmx_ptrld(physical(rm->vmcs))) {
        *why = instruction_failure("VMPTRLD");
        goto fail;
    }
    rm->vmcs_active = true;
    struct vmx_host host;
    host_state(&host);
    const struct vmx_vmcs_params params = {&rm->caps, &host, physical(rm->io_a), physical(rm->io_b),
                                           rm->eptp};
    struct vmx_field *fields = rm->fields;
    unsigned n = vmx_vmcs_fields(&params, fields, VMX_MAX_FIELDS);
    uint32_t failed = 0;
    if (!n) {
        *why = "VMCS field list overflow";
        goto fail;
    }
    if (!write_fields(fields, n, &failed)) {
        vmm_snprintf(refusal, sizeof(refusal), "VMWRITE %x failed", failed);
        *why = refusal;
        goto fail;
    }
    rm->enabled = true;
    LOGI("VMX container: revision %x, VMCS %u bytes, %u EPT table pages; VPID disabled", c.revision,
         c.vmcs_size, rm->table_count);
    *out = rm;
    return true;
fail:
    if (!leave(rm))
        hal_panic("VMX: VMCLEAR failed during setup unwind");
    release(rm);
    return false;
}

bool amd64_vmx_run(struct vmx_container *rm, struct x86rm_regs *r, struct x86rm_exit *e)
{
    if (!rm || rm->cpu != hal_cpu_index() || !rm->enabled)
        return false;
    if (!vmx_ptrld(physical(rm->vmcs)))
        return false;
    uint8_t tlb;
    if (!amd64_virtualization_tag_prepare(rm->owner, AMD64_VIRT_VBIOS, &rm->tag, rm->eptp, &tlb))
        return false;
    struct vmx_field *fields = rm->fields;
    unsigned n = vmx_put_regs(&rm->caps, r, fields, VMX_MAX_FIELDS);
    uint32_t failed = 0;
    if (!n || !write_fields(fields, n, &failed)) {
        LOGE("VMX: VMWRITE %x of the guest registers failed", failed);
        return false;
    }
    uint32_t blocking = (uint32_t)vmx_read(VMCS_GUEST_INTERRUPTIBILITY);
    vmx_write(VMCS_GUEST_INTERRUPTIBILITY,
              vmx_entry_interruptibility(blocking, r, rm->last_cs, rm->last_cs_base, rm->last_rip));
    rm->gprs.rax = r->rax;
    rm->gprs.rbx = r->rbx;
    rm->gprs.rcx = r->rcx;
    rm->gprs.rdx = r->rdx;
    rm->gprs.rsi = r->rsi;
    rm->gprs.rdi = r->rdi;
    rm->gprs.rbp = r->rbp;
    /* The guest exits at least every 10 ms (external-interrupt exiting,
     * whatever its IF): the bound and the BDA tick of platform/vbios.c are
     * checked on every exit. */
    hal_timer_arm(hal_counter() + hal_counter_hz() / 100);
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
    int status = amd64_vmx_vmrun(&rm->gprs, rm->launched, &rm->xstate);
    struct vmx_exit_fields f = {0};
    if (!status) {
        f.reason = (uint32_t)vmx_read(VMCS_EXIT_REASON);
        /* Only a successful VMLAUNCH makes the VMCS launched (§27.1); an
         * entry that failed while loading guest state (bit 31, §29.8) left
         * it clear, so the next entry is a VMLAUNCH again. */
        if (!(f.reason & VMX_EXIT_ENTRY_FAILURE))
            rm->launched = true;
        f.intr_info = (uint32_t)vmx_read(VMCS_EXIT_INTR_INFO);
        /* §30.5.3: an NMI exit consumed the NMI and left NMI blocking on.
         * The host's own handler runs through the host IDT; its IRET ends
         * the blocking. */
        if (!(f.reason & VMX_EXIT_ENTRY_FAILURE) &&
            (f.reason & 0xffffu) == VMX_EXIT_EXCEPTION_NMI && (f.intr_info & (1u << 31)) &&
            ((f.intr_info >> 8) & 7) == 2)
            __asm__ volatile("int $2" ::: "memory");
    }
    /* STI's shadow covers the NOP: a host interrupt left pending in the
     * LAPIC by the external-interrupt exit (not acknowledged) is taken here,
     * before any guest state is inspected. */
    __asm__ volatile("sti; nop" ::: "memory");
    if (!(flags & (1u << 9)))
        __asm__ volatile("cli" ::: "memory");
    if (status) {
        if (status == 1)
            LOGE("VMX: %s failed: VMfailInvalid", rm->launched ? "VMRESUME" : "VMLAUNCH");
        else
            LOGE(
                "VMX: %s failed: VM-instruction error %llu (7: control fields, 8: host-state fields)",
                rm->launched ? "VMRESUME" : "VMLAUNCH",
                (unsigned long long)vmx_read(VMCS_INSTRUCTION_ERROR));
        return false;
    }
    f.intr_error = (uint32_t)vmx_read(VMCS_EXIT_INTR_ERROR);
    f.idt_vectoring = (uint32_t)vmx_read(VMCS_IDT_VECTORING_INFO);
    f.instr_length = (uint32_t)vmx_read(VMCS_EXIT_INSTR_LENGTH);
    f.cs_ar = (uint32_t)vmx_read(VMCS_GUEST_ES_AR + 2);
    f.qualification = vmx_read(VMCS_EXIT_QUALIFICATION);
    f.guest_physical = vmx_read(VMCS_GUEST_PHYSICAL);
    f.rip = vmx_read(VMCS_GUEST_RIP);
    vmx_get_regs(&rm->caps, vmread_fn, NULL, r);
    r->rax = rm->gprs.rax;
    r->rbx = rm->gprs.rbx;
    r->rcx = rm->gprs.rcx;
    r->rdx = rm->gprs.rdx;
    r->rsi = rm->gprs.rsi;
    r->rdi = rm->gprs.rdi;
    r->rbp = rm->gprs.rbp;
    rm->last_cs = r->cs.selector;
    rm->last_cs_base = r->cs.base;
    rm->last_rip = r->rip;
    if (!vmx_decode_exit(&f, e)) {
        LOGE("VMX: EPT misconfiguration at GPA %llx (the VMM's EPT is wrong)",
             (unsigned long long)f.guest_physical);
        return false;
    }
    if (e->reason == X86RM_EXIT_CPUID)
        amd64_cpuid((uint32_t)r->rax, (uint32_t)r->rcx, &e->cpuid[0], &e->cpuid[1], &e->cpuid[2],
                    &e->cpuid[3]);
    /* INVD would discard the host's dirty lines: WBINVD for both. */
    if (e->reason == X86RM_EXIT_INVD || e->reason == X86RM_EXIT_WBINVD)
        __asm__ volatile("wbinvd" ::: "memory");
    return true;
}

void amd64_vmx_destroy(struct vmx_container *rm)
{
    if (!rm || rm->cpu != hal_cpu_index() || !rm->enabled)
        return;
    hal_timer_cancel();
    if (!leave(rm))
        hal_panic("VMX: VMCLEAR failed during destroy");
    release(rm);
}

static uint64_t vmx_native_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static void vmx_native_irq_restore(uint64_t flags, bool dispatch, bool nmi)
{
    if (nmi)
        __asm__ volatile("int $2" ::: "memory");
    if (dispatch)
        __asm__ volatile("sti; nop" ::: "memory");
    else if (flags & (1u << 9))
        __asm__ volatile("sti" ::: "memory");
    if (!(flags & (1u << 9)))
        __asm__ volatile("cli" ::: "memory");
}
static bool vmx_native_tsc_aux_supported(void)
{
    uint32_t a, b, c, d;
    amd64_cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        amd64_cpuid(0x80000001, 0, &a, &b, &c, &d);
        if (d & (1u << 27))
            return true;
    }
    amd64_cpuid(0, 0, &a, &b, &c, &d);
    if (a < 7)
        return false;
    amd64_cpuid(7, 0, &a, &b, &c, &d);
    return (c & (1u << 22)) != 0;
}
static uint64_t vmx_native_tsc_aux_enter(bool supported)
{
    if (!supported)
        return 0;
    uint64_t saved = amd64_rdmsr(0xc0000103);
    amd64_wrmsr(0xc0000103, 0);
    return saved;
}
static void vmx_native_tsc_aux_leave(bool supported, uint64_t saved)
{
    if (supported)
        amd64_wrmsr(0xc0000103, saved);
}

#include "host/amd64/vmx_ia32.inc"
