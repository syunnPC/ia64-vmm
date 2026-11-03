/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host operations of ia64/hostfp.c: SSE2 scalar/packed arithmetic and FMA3
 * (VEX) with MXCSR = all exceptions masked, RC = FPSR.sfx.rc, FTZ and DAZ
 * clear (Intel SDM Vol.1 §10.2.3). The flags are cleared before and read
 * after the operation; the caller's MXCSR control bits are restored (SysV:
 * callee-saved), its status bits are not (caller-saved). Builds without
 * SIMD in C (-mgeneral-regs-only: no __SSE2__) report no host execution.
 */
#include "ia64/hostfp.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/host_simd.h"

#ifdef __SSE2__
static uint32_t control(unsigned rc)
{
    return 0x1f80u | (rc & 3) << 13;
}
static void restore(uint32_t saved, uint32_t used)
{
    if ((saved & ~UINT32_C(0x3f)) != used)
        __asm__ volatile("ldmxcsr %0" ::"m"(saved));
}

bool ia64_hostfp_run(const struct ia64_hostfp_plan *p, unsigned rc, uint64_t *bits,
                     unsigned *host_flags)
{
    uint32_t used = control(rc), saved, status;
    uint64_t out = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(saved));
    bool dbl = p->format == IA64_HOSTFP_DOUBLE, narrow = dbl && p->result == IA64_HOSTFP_SINGLE;
    if (p->kind == IA64_HOSTFP_FMA && !ia64_hostfp_fma3())
        return false;
    /* xmm0 = a, xmm1 = b, xmm2 = c; the result is in xmm1. */
#define HOSTFP_OP(text)                                                                            \
    __asm__ volatile("movq %[a], %%xmm0\n\tmovq %[b], %%xmm1\n\tmovq %[c], %%xmm2\n\t"             \
                     "ldmxcsr %[ctl]\n\t" text "\n\tstmxcsr %[st]\n\tmovq %%xmm1, %[out]"          \
                     : [st] "=m"(status), [out] "=m"(out)                                          \
                     : [a] "m"(p->a), [b] "m"(p->b), [c] "m"(p->c), [ctl] "m"(used)                \
                     : "xmm0", "xmm1", "xmm2")
    switch (p->kind * 4 + dbl * 2 + narrow) {
    case IA64_HOSTFP_MUL * 4 + 0:
        HOSTFP_OP("mulss %%xmm2, %%xmm1");
        break;
    case IA64_HOSTFP_MUL * 4 + 2:
        HOSTFP_OP("mulsd %%xmm2, %%xmm1");
        break;
    case IA64_HOSTFP_MUL * 4 + 3:
        HOSTFP_OP("mulsd %%xmm2, %%xmm1\n\tcvtsd2ss %%xmm1, %%xmm1");
        break;
    case IA64_HOSTFP_MULADD * 4 + 0:
        HOSTFP_OP("mulss %%xmm2, %%xmm1\n\taddss %%xmm0, %%xmm1");
        break;
    case IA64_HOSTFP_MULADD * 4 + 2:
        HOSTFP_OP("mulsd %%xmm2, %%xmm1\n\taddsd %%xmm0, %%xmm1");
        break;
    case IA64_HOSTFP_MULADD * 4 + 3:
        HOSTFP_OP("mulsd %%xmm2, %%xmm1\n\taddsd %%xmm0, %%xmm1\n\tcvtsd2ss %%xmm1, %%xmm1");
        break;
    case IA64_HOSTFP_FMA * 4 + 0:
        HOSTFP_OP("vfmadd213ss %%xmm0, %%xmm2, %%xmm1");
        break;
    case IA64_HOSTFP_FMA * 4 + 2:
        HOSTFP_OP("vfmadd213sd %%xmm0, %%xmm2, %%xmm1");
        break;
    case IA64_HOSTFP_FMA * 4 + 3:
        HOSTFP_OP("vfmadd213sd %%xmm0, %%xmm2, %%xmm1\n\tvcvtsd2ss %%xmm1, %%xmm1, %%xmm1");
        break;
    default:
        return false;
    }
#undef HOSTFP_OP
    restore(saved, used);
    *bits = dbl && !narrow ? out : (uint32_t)out;
    *host_flags = status & 63;
    return true;
}

bool ia64_hostfp_run_packed(const struct ia64_hostfp_packed *p, unsigned rc, uint64_t *bits,
                            unsigned *host_flags)
{
    uint32_t used = control(rc), saved, status;
    uint64_t out = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(saved));
    if (p->kind == IA64_HOSTFP_FMA && !ia64_hostfp_fma3())
        return false;
    /* movq zeroes lanes 2 and 3, which then raise no flag. */
#define HOSTFP_OP(text)                                                                            \
    __asm__ volatile("movq %[a], %%xmm0\n\tmovq %[b], %%xmm1\n\tmovq %[c], %%xmm2\n\t"             \
                     "ldmxcsr %[ctl]\n\t" text "\n\tstmxcsr %[st]\n\tmovq %%xmm1, %[out]"          \
                     : [st] "=m"(status), [out] "=m"(out)                                          \
                     : [a] "m"(p->a), [b] "m"(p->b), [c] "m"(p->c), [ctl] "m"(used)                \
                     : "xmm0", "xmm1", "xmm2")
    switch (p->kind) {
    case IA64_HOSTFP_MUL:
        HOSTFP_OP("mulps %%xmm2, %%xmm1");
        break;
    case IA64_HOSTFP_MULADD:
        HOSTFP_OP("mulps %%xmm2, %%xmm1\n\taddps %%xmm0, %%xmm1");
        break;
    case IA64_HOSTFP_FMA:
        HOSTFP_OP("vfmadd213ps %%xmm0, %%xmm2, %%xmm1");
        break;
    default:
        return false;
    }
#undef HOSTFP_OP
    restore(saved, used);
    *bits = out;
    *host_flags = status & 63;
    return true;
}

bool ia64_hostfp_host(void)
{
    return true;
}

/* Every executor must support FMA and its enabled SIMD state. */
bool ia64_hostfp_fma3(void)
{
    return (hal_host_simd() & HOST_SIMD_FMA) != 0;
}

#else
bool ia64_hostfp_run(const struct ia64_hostfp_plan *p, unsigned rc, uint64_t *bits,
                     unsigned *host_flags)
{
    (void)p;
    (void)rc;
    (void)bits;
    (void)host_flags;
    return false;
}
bool ia64_hostfp_run_packed(const struct ia64_hostfp_packed *p, unsigned rc, uint64_t *bits,
                            unsigned *host_flags)
{
    (void)p;
    (void)rc;
    (void)bits;
    (void)host_flags;
    return false;
}
bool ia64_hostfp_fma3(void)
{
    return false;
}
bool ia64_hostfp_host(void)
{
    return false;
}
#endif
