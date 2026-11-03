/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_HOST_SIMD_H
#define IA64VMM_HOST_SIMD_H
#include "ia64vmm/host_isa.h"

/* SIMD beyond the build baseline (SSE4.2, POPCNT, AVX), used only through
 * runtime dispatch on hal_host_simd(): the intersection over every CPU. */
#define HOST_SIMD_AVX2 (1u << 0)
#define HOST_SIMD_FMA (1u << 1)
#define HOST_SIMD_ALL 3u

/* Intel SDM Vol.1 §14.7.1 (AVX2) and §14.5.3 (FMA): CPUID.1:ECX.OSXSAVE[27]
 * and AVX[28], XGETBV(0) with XCR0[2:1] = 11b, then CPUID.(07H,0):EBX[5]
 * for AVX2 and CPUID.1:ECX[12] for FMA. XGETBV is #UD without OSXSAVE, so
 * xcr0 is called only after that bit is seen. */
static inline unsigned host_simd_probe(host_isa_cpuid_fn cpuid, uint64_t (*xcr0)(void))
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max = a;
    if (max < 1)
        return 0;
    cpuid(1, 0, &a, &b, &c, &d);
    if ((c & (3u << 27)) != (3u << 27) || (xcr0() & 6) != 6)
        return 0;
    unsigned features = 0;
    if (c & (1u << 12))
        features |= HOST_SIMD_FMA;
    if (max >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        if (b & (1u << 5))
            features |= HOST_SIMD_AVX2;
    }
    return features;
}

/* XGETBV with ECX = 0 (SDM Vol.2 XGETBV); only after OSXSAVE is known set. */
static inline uint64_t host_simd_xcr0(void)
{
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (uint64_t)hi << 32 | lo;
}
#endif
