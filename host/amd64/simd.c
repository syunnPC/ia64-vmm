/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host SIMD baseline check and XSAVE area setup (host/amd64/simd.h). */
#include "host/amd64/simd.h"
#include "ia64vmm/string.h"

unsigned amd64_simd_missing(host_isa_cpuid_fn cpuid)
{
    static const struct {
        unsigned feature;
        bool edx;
        unsigned bit;
    } bits[] = {
        {AMD64_SIMD_FPU, true, 0},      {AMD64_SIMD_FXSR, true, 24},
        {AMD64_SIMD_SSE, true, 25},     {AMD64_SIMD_SSE2, true, 26},
        {AMD64_SIMD_SSE3, false, 0},    {AMD64_SIMD_SSSE3, false, 9},
        {AMD64_SIMD_SSE41, false, 19},  {AMD64_SIMD_SSE42, false, 20},
        {AMD64_SIMD_POPCNT, false, 23}, {AMD64_SIMD_XSAVE, false, 26},
        {AMD64_SIMD_AVX, false, 28},
    };
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max = a;
    if (max < 1)
        return AMD64_SIMD_ALL;
    cpuid(1, 0, &a, &b, &c, &d);
    unsigned missing = 0;
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(bits); ++i)
        if (!((bits[i].edx ? d : c) & (1u << bits[i].bit)))
            missing |= bits[i].feature;
    if (max < 0xd)
        return missing | AMD64_SIMD_XCR0;
    cpuid(0xd, 0, &a, &b, &c, &d);
    if ((a & AMD64_XCR0_BASELINE) != AMD64_XCR0_BASELINE)
        missing |= AMD64_SIMD_XCR0;
    return missing;
}

const char *amd64_simd_describe(unsigned missing, char *out, size_t size)
{
    static const char *const names[] = {"FPU",    "FXSR",  "SSE",    "SSE2",
                                        "SSE3",   "SSSE3", "SSE4.1", "SSE4.2",
                                        "POPCNT", "XSAVE", "AVX",    "XCR0-AVX-state"};
    size_t used = 0;
    if (!size)
        return out;
    out[0] = 0;
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(names); ++i) {
        if (!(missing & (1u << i)))
            continue;
        for (const char *p = used ? " " : ""; *p && used + 1 < size; ++p)
            out[used++] = *p;
        for (const char *p = names[i]; *p && used + 1 < size; ++p)
            out[used++] = *p;
        out[used] = 0;
    }
    return out;
}

bool amd64_xstate_init(void *area, size_t bytes)
{
    if (bytes < AMD64_XSAVE_MIN_BYTES)
        return false;
    uint8_t *p = area;
    memset(p, 0, bytes);
    p[0] = AMD64_FCW_DEFAULT & 0xff;
    p[1] = AMD64_FCW_DEFAULT >> 8;
    p[24] = AMD64_MXCSR_DEFAULT & 0xff;
    p[25] = AMD64_MXCSR_DEFAULT >> 8;
    return true;
}

uint64_t amd64_xcr0_mask(uint64_t supported)
{
    return supported & ~AMD64_XCR0_AMX;
}
