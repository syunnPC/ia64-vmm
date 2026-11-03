/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AMD SVM backend of the x86 real-mode container: detection, enablement on the
 * owner CPU, with one VMRUN per hal_x86rm_run. Shared host enablement
 * and tag leases live in virtualization.c. host/amd64/x86rm.c calls it on an
 * AMD or Hygon CPU. Policy lives in platform/vbios.c.
 */
#include "host/amd64/amd64.h"
#include "host/amd64/simd.h"
#include "host/amd64/svm.h"
#include "host/amd64/ia32.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define MSR_VM_CR 0xc0010114u
#define VM_CR_LOCK (1u << 3)
#define VM_CR_SVMDIS (1u << 4)
#define MAX_TABLES 512u

/* Layout shared with svm_vmrun.S (G_* offsets). */
struct svm_gprs {
    uint64_t rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15;
};
void amd64_svm_vmrun(uint64_t vmcb_pa, uint64_t host_pa, struct svm_gprs *gprs,
                     const struct amd64_xstate_pair *xstate);

struct svm_container {
    uint8_t *vmcb, *host, *iopm, *msrpm;
    struct amd64_virtualization_cpu *owner;
    struct amd64_virtualization_tag tag;
    unsigned cpu;
    uint64_t root;
    struct amd64_xstate_pair xstate;
    void *tables[MAX_TABLES];
    unsigned table_count;
    bool nrips, enabled;
    struct svm_gprs gprs;
};
#define CONTAINER_PAGES ((sizeof(struct svm_container) + 4095) / 4096)

struct capabilities {
    bool nrips, pages_1g;
    uint32_t revision, asids;
};

static bool probe(struct capabilities *c, const char **why)
{
    uint32_t a, b, cx, d;
    memset(c, 0, sizeof(*c));
    amd64_cpuid(0x80000000, 0, &a, &b, &cx, &d);
    uint32_t max = a;
    if (max < 0x80000001) {
        *why = "no extended CPUID leaves";
        return false;
    }
    amd64_cpuid(0x80000001, 0, &a, &b, &cx, &d);
    c->pages_1g = (d & (1u << 26)) != 0;
    if (!(cx & (1u << 2))) {
        *why = "CPUID 8000_0001h ECX.SVM is clear";
        return false;
    }
    if (max < 0x8000000a) {
        *why = "CPUID leaf 8000_000Ah is absent";
        return false;
    }
    /* APM §15.30.1: VM_CR exists whenever CPUID reports SVM. SVMDIS is the
     * firmware's choice; it is reported, never cleared. */
    uint64_t vm_cr = amd64_rdmsr(MSR_VM_CR);
    if (vm_cr & VM_CR_SVMDIS) {
        *why = vm_cr & VM_CR_LOCK ? "SVM disabled by firmware (VM_CR.SVMDIS=1, LOCK=1)"
                                  : "SVM disabled (VM_CR.SVMDIS=1, LOCK=0)";
        return false;
    }
    amd64_cpuid(0x8000000a, 0, &a, &b, &cx, &d);
    c->revision = a & 0xff;
    c->asids = b;
    c->nrips = (d & (1u << 3)) != 0;
    if (!(d & 1)) {
        *why = "no nested paging (CPUID 8000_000Ah EDX.NP is clear)";
        return false;
    }
    if (b < 2) {
        *why = "fewer than two ASIDs";
        return false;
    }
    return true;
}

bool amd64_svm_probe(const char **why)
{
    struct capabilities c;
    return probe(&c, why);
}

/* Container pages come from hal_alloc_pages without HAL_PAGE_EXEC, so they
 * live in the HHDM: physical = virtual - HHDM offset. */
static uint64_t physical(const void *p)
{
    return (uintptr_t)p - amd64_hhdm;
}
static void *npt_alloc(void *ctx)
{
    struct svm_container *rm = ctx;
    if (rm->table_count == MAX_TABLES)
        return NULL;
    void *p = hal_alloc_pages(1, HAL_PAGE_ZERO);
    if (p)
        rm->tables[rm->table_count++] = p;
    return p;
}
static uint64_t npt_pa(void *ctx, const void *va)
{
    (void)ctx;
    return physical(va);
}
static void *npt_va(void *ctx, uint64_t pa)
{
    (void)ctx;
    return (void *)(uintptr_t)(amd64_hhdm + pa);
}

static void release(struct svm_container *rm)
{
    for (unsigned i = 0; i < rm->table_count; ++i)
        hal_free_pages(rm->tables[i], 1);
    rm->table_count = 0;
    if (rm->vmcb)
        hal_free_pages(rm->vmcb, 1);
    if (rm->host)
        hal_free_pages(rm->host, 1);
    if (rm->iopm)
        hal_free_pages(rm->iopm, SVM_IOPM_SIZE / 4096);
    if (rm->msrpm)
        hal_free_pages(rm->msrpm, SVM_MSRPM_SIZE / 4096);
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

bool amd64_svm_create(const struct x86rm_config *cfg, struct svm_container **out, const char **why)
{
    struct capabilities c;
    *out = NULL;
    if (!probe(&c, why))
        return false;
    struct svm_container *rm = hal_alloc_pages(CONTAINER_PAGES, HAL_PAGE_ZERO);
    if (!rm) {
        *why = "VMM reservation exhausted";
        return false;
    }
    rm->cpu = hal_cpu_index();
    struct ia32_setup_error error;
    if (!amd64_virtualization_acquire(rm->cpu, IA32_BACKEND_SVM, AMD64_VIRT_VBIOS, &rm->owner,
                                      &error) ||
        !amd64_virtualization_tag_acquire(rm->owner, AMD64_VIRT_VBIOS, &rm->tag, &error)) {
        *why = error.detail;
        release(rm);
        return false;
    }
    rm->vmcb = hal_alloc_pages(1, HAL_PAGE_ZERO);
    rm->host = hal_alloc_pages(1, HAL_PAGE_ZERO);
    rm->iopm = hal_alloc_pages(SVM_IOPM_SIZE / 4096, HAL_PAGE_ZERO);
    rm->msrpm = hal_alloc_pages(SVM_MSRPM_SIZE / 4096, HAL_PAGE_ZERO);
    /* XSAVE areas for every component in XCR0 (amd64_xstate_bytes). */
    rm->xstate.host = hal_alloc_pages(amd64_xstate_pages(), HAL_PAGE_ZERO);
    rm->xstate.guest = hal_alloc_pages(amd64_xstate_pages(), HAL_PAGE_ZERO);
    if (!rm->vmcb || !rm->host || !rm->iopm || !rm->msrpm || !rm->xstate.host ||
        !rm->xstate.guest) {
        release(rm);
        *why = "VMM reservation exhausted";
        return false;
    }
    amd64_xstate_init(rm->xstate.guest, amd64_xstate_bytes);
    svm_iopm_build(rm->iopm, cfg->io_pass);
    svm_msrpm_build(rm->msrpm);
    const struct svm_npt_ops ops = {npt_alloc, npt_pa, npt_va, rm, c.pages_1g};
    uint64_t root;
    if (!svm_npt_build(cfg->map, cfg->map_count, &ops, &root)) {
        release(rm);
        *why = "nested page table refused the map (alignment, overlap or table pool)";
        return false;
    }
    rm->root = root;
    const struct svm_vmcb_params params = {physical(rm->iopm), physical(rm->msrpm), root,
                                           rm->tag.id};
    svm_vmcb_init(rm->vmcb, &params);
    rm->nrips = c.nrips;
    rm->enabled = true;
    LOGI("SVM container: revision %u, %u ASIDs, ASID %u, NRIPS=%u, %u nested table pages",
         c.revision, c.asids, rm->tag.id, c.nrips, rm->table_count);
    *out = rm;
    return true;
}

bool amd64_svm_run(struct svm_container *rm, struct x86rm_regs *r, struct x86rm_exit *e)
{
    if (!rm || rm->cpu != hal_cpu_index() || !rm->enabled)
        return false;
    svm_vmcb_put_regs(rm->vmcb, r);
    rm->gprs = (struct svm_gprs){
        .rbx = r->rbx, .rcx = r->rcx, .rdx = r->rdx, .rsi = r->rsi, .rdi = r->rdi, .rbp = r->rbp};
    if (!amd64_virtualization_tag_prepare(rm->owner, AMD64_VIRT_VBIOS, &rm->tag, rm->root,
                                          &rm->vmcb[VMCB_TLB_CONTROL]))
        return false;
    /* The guest exits at least every 10 ms: the bound and the BDA tick of
     * platform/vbios.c are checked on every exit. */
    hal_timer_arm(hal_counter() + hal_counter_hz() / 100);
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli; clgi" : "=r"(flags)::"memory");
    amd64_svm_vmrun(physical(rm->vmcb), physical(rm->host), &rm->gprs, &rm->xstate);
    /* STI's shadow covers the NOP: a host interrupt held by GIF=0 (the
     * INTR exit's) is taken here, before any guest state is inspected. */
    __asm__ volatile("stgi; sti; nop" ::: "memory");
    if (!(flags & (1u << 9)))
        __asm__ volatile("cli" ::: "memory");
    uint64_t exitcode;
    memcpy(&exitcode, rm->vmcb + VMCB_EXITCODE, sizeof(exitcode));
    amd64_virtualization_tag_entered(rm->owner, AMD64_VIRT_VBIOS, &rm->tag,
                                     exitcode != SVM_EXIT_INVALID);
    svm_vmcb_get_regs(rm->vmcb, r);
    r->rbx = rm->gprs.rbx;
    r->rcx = rm->gprs.rcx;
    r->rdx = rm->gprs.rdx;
    r->rsi = rm->gprs.rsi;
    r->rdi = rm->gprs.rdi;
    r->rbp = rm->gprs.rbp;
    svm_decode_exit(rm->vmcb, rm->nrips, e);
    if (e->reason == X86RM_EXIT_CPUID)
        amd64_cpuid((uint32_t)r->rax, (uint32_t)r->rcx, &e->cpuid[0], &e->cpuid[1], &e->cpuid[2],
                    &e->cpuid[3]);
    /* INVD would discard the host's dirty lines: WBINVD for both. */
    if (e->reason == X86RM_EXIT_INVD || e->reason == X86RM_EXIT_WBINVD)
        __asm__ volatile("wbinvd" ::: "memory");
    return true;
}

void amd64_svm_destroy(struct svm_container *rm)
{
    if (!rm || rm->cpu != hal_cpu_index() || !rm->enabled)
        return;
    hal_timer_cancel();
    release(rm);
}
