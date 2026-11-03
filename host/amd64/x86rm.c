/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The x86 real-mode container's HAL calls. The backend is chosen once, at the first probe, from the CPUID
 * leaf 0 vendor string: AuthenticAMD and HygonGenuine run AMD SVM (svm.c),
 * GenuineIntel runs Intel VMX (vmx.c), anything else has none. The vendor only names which architecture's virtualization to look for;
 * the backend's probe then reads its own feature bits and MSRs, so nothing
 * is assumed from it.
 */
#include "host/amd64/amd64.h"
#include "host/amd64/svm.h"
#include "host/amd64/vmx.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

enum backend { BACKEND_UNCHOSEN, BACKEND_NONE, BACKEND_SVM, BACKEND_VMX };
static enum backend backend;
static const struct x86rm_info infos[] = {
    [BACKEND_UNCHOSEN] = {"hardware virtualization", {0, 0, 0}},
    [BACKEND_NONE] = {"hardware virtualization", {0, 0, 0}},
    [BACKEND_SVM] = {"SVM", {0x0f, 0x01, 0xd9}}, /* VMMCALL (APM Vol.3) */
    [BACKEND_VMX] = {"VMX", {0x0f, 0x01, 0xc1}}, /* VMCALL (SDM Vol.2B; VMMCALL is #UD on Intel) */
};
struct x86rm {
    struct svm_container *svm;
    struct vmx_container *vmx;
    unsigned cpu;
};
static char refusal[48];

static void choose(void)
{
    if (backend != BACKEND_UNCHOSEN)
        return;
    uint32_t a, b, c, d;
    char vendor[13];
    amd64_cpuid(0, 0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    for (unsigned i = 0; i < 12; ++i)
        if (vendor[i] < 0x20 || vendor[i] > 0x7e)
            vendor[i] = '?';
    if (!strcmp(vendor, "AuthenticAMD") || !strcmp(vendor, "HygonGenuine"))
        backend = BACKEND_SVM;
    else if (!strcmp(vendor, "GenuineIntel"))
        backend = BACKEND_VMX;
    else
        backend = BACKEND_NONE;
    vmm_snprintf(refusal, sizeof(refusal), "CPU vendor %s", vendor);
}

const struct x86rm_info *hal_x86rm_info(void)
{
    choose();
    return &infos[backend];
}

bool hal_x86rm_probe(const char **why)
{
    choose();
    if (backend == BACKEND_SVM)
        return amd64_svm_probe(why);
    if (backend == BACKEND_VMX)
        return amd64_vmx_probe(why);
    *why = refusal;
    return false;
}

static bool create(const struct x86rm_config *cfg, struct x86rm **out, const char **why)
{
    *out = NULL;
    choose();
    struct x86rm *rm = hal_alloc_pages(1, HAL_PAGE_ZERO);
    if (!rm) {
        *why = "VMM reservation exhausted";
        return false;
    }
    rm->cpu = hal_cpu_index();
    bool ok = false;
    if (backend == BACKEND_SVM)
        ok = amd64_svm_create(cfg, &rm->svm, why);
    else if (backend == BACKEND_VMX)
        ok = amd64_vmx_create(cfg, &rm->vmx, why);
    else
        *why = refusal;
    if (!ok) {
        hal_free_pages(rm, 1);
        return false;
    }
    *out = rm;
    return true;
}

bool hal_x86rm_create(const struct x86rm_config *cfg, struct x86rm **out, const char **why)
{
    return create(cfg, out, why);
}

bool hal_x86rm_reclaim_create(const struct x86rm_config *cfg, struct x86rm **out, const char **why)
{
    return create(cfg, out, why);
}

bool hal_x86rm_run(struct x86rm *rm, struct x86rm_regs *regs, struct x86rm_exit *exit)
{
    if (!rm || rm->cpu != hal_cpu_index())
        return false;
    if (rm->svm)
        return amd64_svm_run(rm->svm, regs, exit);
    if (rm->vmx)
        return amd64_vmx_run(rm->vmx, regs, exit);
    return false;
}

void hal_x86rm_destroy(struct x86rm *rm)
{
    if (!rm || rm->cpu != hal_cpu_index())
        return;
    if (rm->svm)
        amd64_svm_destroy(rm->svm);
    if (rm->vmx)
        amd64_vmx_destroy(rm->vmx);
    hal_free_pages(rm, 1);
}
