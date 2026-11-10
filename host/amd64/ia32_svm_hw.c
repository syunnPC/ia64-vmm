/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/ia32_svm.h"
#include "host/amd64/amd64.h"
#include "host/amd64/simd.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"
static bool fail(struct ia32_setup_error *e, const char *detail, uint64_t raw)
{
    if (e)
        *e = (struct ia32_setup_error){.reason = IA32_EXIT_UNIMPLEMENTED,
                                       .host_cpu = hal_cpu_index(),
                                       .raw = raw,
                                       .detail = detail};
    return false;
}
static bool svm_vendor(void);
extern uint32_t amd64_ia32_fp_mxcsr_mask(void);
bool amd64_ia32_fp_requirements(struct amd64_ia32_fp_requirements *out, struct ia32_setup_error *e)
{
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0,%0; mov %%cr4,%1" : "=r"(cr0), "=r"(cr4));
    if ((cr0 & 12) || !(cr4 & (1u << 9)))
        return fail(e, "native FP host CR0/CR4 state is unqualified", cr0);
    uint32_t a, b, c, d;
    amd64_cpuid(1, 0, &a, &b, &c, &d);
    if ((d & 0x03000001) != 0x03000001 || !(c & (1u << 26)) || !(c & (1u << 27)))
        return fail(e, "native FP needs enabled XSAVE, FPU, FXSR and SSE", ((uint64_t)c << 32) | d);
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    uint64_t mask = (uint64_t)hi << 32 | lo;
    if ((mask & 3) != 3 || mask != amd64_xcr0)
        return fail(e, "native FP XCR0 differs from host policy", mask);
    amd64_cpuid(0xd, 0, &a, &b, &c, &d);
    if (b < AMD64_XSAVE_MIN_BYTES || b > amd64_xstate_bytes)
        return fail(e, "native FP XSAVE size exceeds host allocation", b);
    struct amd64_ia32_fp_requirements r = {.bytes = b,
                                           .alignment = 64,
                                           .xcr0 = mask,
                                           .xsave = true,
                                           .host_x87_initial_only = svm_vendor()};
    amd64_cpuid(0xd, 1, &a, &b, &c, &d);
    r.xgetbv1 = (a & 4) != 0;
    if (r.host_x87_initial_only && !r.xgetbv1)
        return fail(e, "native FP cannot qualify initial AMD/Hygon host x87 state", a);
    if (a & (1u << 3))
        r.xss = amd64_rdmsr(0xda0);
    if (r.xss)
        return fail(e, "native FP supervisor xstate save is unimplemented", r.xss);
    uint32_t mxcsr = amd64_ia32_fp_mxcsr_mask();
    if (!mxcsr)
        mxcsr = 0xffbf;
    if ((mxcsr & 0xffbf) != 0xffbf)
        return fail(e, "native FP lacks Madison MXCSR controls", mxcsr);
    *out = r;
    return true;
}
static bool svm_vendor(void)
{
    uint32_t a, b, c, d;
    char vendor[13];
    amd64_cpuid(0, 0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    return !strcmp(vendor, "AuthenticAMD") || !strcmp(vendor, "HygonGenuine");
}
bool amd64_svm_ia32_requirements(struct amd64_svm_ia32_requirements *out,
                                 struct ia32_setup_error *e)
{
    if (!svm_vendor())
        return fail(e, "native VMX backend is unavailable", 0);
    const char *why;
    if (!amd64_svm_probe(&why))
        return fail(e, why, 0);
    uint32_t a, b, c, d;
    amd64_cpuid(0x80000000, 0, &a, &b, &c, &d);
    uint32_t maximum = a;
    if (a < 0x80000008)
        return fail(e, "native SVM lacks physical width CPUID", a);
    amd64_cpuid(0x80000008, 0, &a, &b, &c, &d);
    unsigned width = a & 0xff;
    bool rdpru = (b & (1u << 4)) != 0, invlpgb = (b & (1u << 3)) != 0;
    if (width < 32 || width > 52)
        return fail(e, "native SVM physical width is unrepresentable", width);
    amd64_cpuid(0x8000000a, 0, &a, &b, &c, &d);
    if (b < 3)
        return fail(e, "native SVM needs host/VBIOS/native ASIDs", b);
    if (invlpgb && !(d & (1u << 24)))
        return fail(e, "native SVM TLBSYNC interception is unavailable", d);
    struct amd64_svm_ia32_requirements r = {.physical_bits = width,
                                            .rdpru = rdpru,
                                            .invlpgb = invlpgb,
                                            .caps = {.backend = IA32_BACKEND_SVM,
                                                     .host_cpu = hal_cpu_index(),
                                                     .nested_paging = true,
                                                     .nested_invalidate = true,
                                                     .descriptor_read_exit = true,
                                                     .bounded_exit = true,
                                                     .legacy_fp = true,
                                                     .nrip = (d & (1u << 3)) != 0,
                                                     .decode_assists = (d & (1u << 7)) != 0,
                                                     .flush_by_asid = (d & (1u << 6)) != 0}};
    uint64_t efer = amd64_rdmsr(0xc0000080);
    if (!(efer & (1u << 11)))
        return fail(e, "native SVM host EFER.NXE is clear", efer);
    if (efer & (1u << 14))
        return fail(e, "native legacy FP with host EFER.FFXSR is unqualified", efer);
    amd64_cpuid(0x80000001, 0, &a, &b, &c, &d);
    r.tsc_aux = (d & (1u << 27)) != 0;
    amd64_cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 7) {
        amd64_cpuid(7, 0, &a, &b, &c, &d);
        r.tsc_aux |= (c & (1u << 22)) != 0;
    }
    if (maximum >= 0x8000001f) {
        amd64_cpuid(0x8000001f, 0, &a, &b, &c, &d);
        if ((a & 1) && (amd64_rdmsr(0xc0010010) & (1u << 23)))
            return fail(e, "native SVM encrypted host backing is unqualified", a);
    }
    uint64_t cr0;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    if (cr0 & (UINT64_C(3) << 29))
        return fail(e, "native SVM host disables caching", cr0);
    /* NPT attributes index the HOST PAT, even with a private guest PAT. */
    uint64_t pat = amd64_rdmsr(0x277);
    const unsigned types[] = {6, 0, 1};
    for (unsigned i = 0; i < 3; ++i) {
        r.pat_flags[i] = UINT64_MAX;
        for (unsigned j = 0; j < 8; ++j)
            if (((pat >> (8 * j)) & 0xff) == types[i]) {
                r.pat_flags[i] =
                    ((j & 1) ? NPT_PWT : 0) | ((j & 2) ? NPT_PCD : 0) | ((j & 4) ? 0x80 : 0);
                break;
            }
    }
    if (r.pat_flags[IA32_MEMORY_WB] == UINT64_MAX || r.pat_flags[IA32_MEMORY_UC] == UINT64_MAX)
        return fail(e, "native SVM host PAT lacks WB/UC entries", pat);
    struct amd64_ia32_fp_requirements fp;
    if (!amd64_ia32_fp_requirements(&fp, e))
        return false;
    amd64_cpuid(1, 0, &a, &b, &c, &d);
    r.caps.native_features = amd64_ia32_native_features(c);
    *out = r;
    return true;
}
uint64_t amd64_svm_ia32_lock(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli; clgi" : "=r"(flags)::"memory");
    return flags;
}
void amd64_svm_ia32_unlock(uint64_t flags)
{
    /* Any pending host IRQ sees restored host FP/GS/TR and materialized state. */
    __asm__ volatile("stgi; sti; nop" ::: "memory");
    if (!(flags & (1u << 9)))
        __asm__ volatile("cli" ::: "memory");
}
void amd64_svm_ia32_run(uint8_t *vmcb, uint8_t *host, struct amd64_svm_ia32_gprs *g,
                        struct amd64_ia32_fp_owner *fp, struct ia32_native_state *state,
                        bool tsc_aux)
{
    uint64_t saved_aux = 0;
    if (tsc_aux) {
        saved_aux = amd64_rdmsr(0xc0000103);
        amd64_wrmsr(0xc0000103, 0);
    }
    amd64_svm_ia32_vmrun((uintptr_t)vmcb - amd64_hhdm, (uintptr_t)host - amd64_hhdm, g, fp,
                         &state->fp);
    if (tsc_aux)
        amd64_wrmsr(0xc0000103, saved_aux);
    uint64_t reason;
    memcpy(&reason, vmcb + VMCB_EXITCODE, 8);
    if (reason != SVM_EXIT_INVALID) {
        amd64_ia32_fp_complete(fp, &state->fp);
        amd64_svm_ia32_get(vmcb, g, state);
    }
}
