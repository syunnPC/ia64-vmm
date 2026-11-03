/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/virtualization_hw.h"
#include "host/amd64/amd64.h"
#include "host/amd64/svm.h"
#include "host/amd64/vmx.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"
#include "ia64vmm/log.h"

#define MSR_EFER 0xc0000080u
#define MSR_VM_HSAVE_PA 0xc0010117u
static uint64_t cr0(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}
static uint64_t cr4(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}
static void set_cr0(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr0" ::"r"(v) : "memory");
}
static void set_cr4(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr4" ::"r"(v) : "memory");
}

bool amd64_virtualization_hw_enable(enum ia32_backend_kind backend,
                                    struct amd64_virtualization_hw *h, const char **why)
{
    uint32_t a, b, c, d;
    char vendor[13];
    amd64_cpuid(0, 0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    bool svm = backend == IA32_BACKEND_SVM;
    if ((svm && strcmp(vendor, "AuthenticAMD") != 0 && strcmp(vendor, "HygonGenuine") != 0) ||
        (!svm && strcmp(vendor, "GenuineIntel") != 0)) {
        *why = "virtualization CPU vendor mismatch";
        return false;
    }
    if (!(svm ? amd64_svm_probe(why) : amd64_vmx_probe(why)))
        return false;
    h->saved_cr0 = cr0();
    h->saved_cr4 = cr4();
    if (svm) {
        h->saved_efer = amd64_rdmsr(MSR_EFER);
        if (h->saved_efer & SVM_EFER_SVME) {
            *why = "SVM already owned outside shared lifecycle";
            return false;
        }
        h->saved_hsave = amd64_rdmsr(MSR_VM_HSAVE_PA);
        amd64_cpuid(0x8000000a, 0, &a, &b, &c, &d);
        h->tags = b;
    } else {
        if (h->saved_cr4 & CR4_VMXE) {
            *why = "VMX already owned outside shared lifecycle";
            return false;
        }
        h->invept_single = (amd64_rdmsr(MSR_VMX_EPT_VPID_CAP) >> 25) & 1;
    }
    h->page = hal_alloc_pages(1, HAL_PAGE_ZERO);
    if (!h->page) {
        *why = "virtualization hardware page allocation failed";
        return false;
    }
    uint64_t pa = (uintptr_t)h->page - amd64_hhdm;
    if (svm) {
        amd64_wrmsr(MSR_VM_HSAVE_PA, pa);
        amd64_wrmsr(MSR_EFER, h->saved_efer | SVM_EFER_SVME);
        return true;
    }
    uint32_t revision = amd64_rdmsr(MSR_VMX_BASIC) & 0x7fffffffu;
    memcpy(h->page, &revision, sizeof(revision));
    set_cr4(h->saved_cr4 | CR4_VMXE);
    bool bad;
    __asm__ volatile("vmxon %1; setna %0" : "=q"(bad) : "m"(pa) : "cc", "memory");
    if (!bad)
        return true;
    set_cr4(h->saved_cr4);
    hal_free_pages(h->page, 1);
    h->page = NULL;
    *why = "VMXON failed (CF/ZF set): another VMM, SMX, or A20M";
    return false;
}
void amd64_virtualization_hw_disable(enum ia32_backend_kind backend,
                                     struct amd64_virtualization_hw *h)
{
    if (backend == IA32_BACKEND_SVM) {
        amd64_wrmsr(MSR_EFER, h->saved_efer);
        amd64_wrmsr(MSR_VM_HSAVE_PA, h->saved_hsave);
        LOGI("SVM container closed: EFER=%llx VM_HSAVE_PA=%llx restored",
             (unsigned long long)amd64_rdmsr(MSR_EFER),
             (unsigned long long)amd64_rdmsr(MSR_VM_HSAVE_PA));
    } else {
        __asm__ volatile("vmxoff" ::: "cc", "memory");
        set_cr0(h->saved_cr0);
        set_cr4(h->saved_cr4);
        LOGI("VMX container closed: VMXOFF, CR4 %llx restored", (unsigned long long)h->saved_cr4);
    }
    hal_free_pages(h->page, 1);
    h->page = NULL;
}
bool amd64_virtualization_hw_invalidate(const struct amd64_virtualization_hw *h, uint32_t tag,
                                        uint64_t eptp)
{
    if (tag)
        return false; /* VPID remains disabled in every VMCS. */
    struct {
        uint64_t eptp, zero;
    } desc = {eptp, 0};
    uint64_t type = h->invept_single ? 1 : 2;
    bool bad;
    __asm__ volatile("invept %1, %2; setna %0" : "=q"(bad) : "m"(desc), "r"(type) : "cc", "memory");
    return !bad;
}
