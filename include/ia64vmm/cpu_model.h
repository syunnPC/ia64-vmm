/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_CPU_MODEL_H
#define IA64VMM_CPU_MODEL_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Immutable VM identity; zero preserves the entry default for old callers.
 * SDM Rev.2.3 Vol.1 §3.1.11 and fork target/ia64/cpu.c alias profiles.
 * TR/TC resources follow the fork models; ITC remains virtual at 400 MHz. */
enum ia64_cpu_model { IA64_CPU_MONTECITO, IA64_CPU_MADISON };
static inline bool ia64_cpu_model_valid(unsigned model)
{
    return model <= IA64_CPU_MADISON;
}
static inline const char *ia64_cpu_model_name(unsigned model)
{
    return model == IA64_CPU_MADISON ? "madison" : "montecito";
}
static inline uint64_t ia64_model_cpuid(unsigned model, unsigned index)
{
    switch (index) {
    case 0:
        return UINT64_C(0x49656e69756e6547);
    case 1:
        return UINT64_C(0x6c65746e);
    case 3:
        return model == IA64_CPU_MADISON ? UINT64_C(0x1f010504) : UINT64_C(0x20000704);
    case 4:
        return model == IA64_CPU_MADISON ? 1 : 5;
    default:
        return 0; /* Architectural reader checks reserved indices. */
    }
}
static inline uint64_t ia64_model_pal_version(unsigned model)
{
    return model == IA64_CPU_MADISON ? UINT64_C(0x0000057301000573) : UINT64_C(0x0000096801000968);
}
static inline const char *ia64_model_brand(unsigned model)
{
    return model == IA64_CPU_MADISON ? "VMM Madison-compatible IA-64 CPU 1.60GHz 3MB"
                                     : "VMM Montecito-compatible IA-64 CPU 1.60GHz 12MB";
}
static inline unsigned ia64_model_l3_size(unsigned model)
{
    return (model == IA64_CPU_MADISON ? 3u : 12u) * 1024u * 1024u;
}
/* Intel 251110-003 Tables 10-7/10-8; 308065-001 Tables 3-4/3-7/3-8.
 * Montecito reads PMD bits 63:47 as copies of bit 46. Madison extends bit 47. */
static inline unsigned ia64_model_pmu_counters(unsigned model)
{
    return model == IA64_CPU_MADISON ? 4 : 12;
}
static inline unsigned ia64_model_pmd_sign_bit(unsigned model)
{
    return model == IA64_CPU_MADISON ? 47 : 46;
}
static inline uint64_t ia64_model_pmc0_mask(unsigned model)
{
    return model == IA64_CPU_MADISON ? UINT64_C(0xf1) : UINT64_C(0xfff1);
}
/* One thread per core, at most the model's populated cores per socket.
 * Madison is single-core; Montecito has two cores. */
static inline unsigned ia64_model_socket_width(unsigned model, unsigned count)
{
    return model == IA64_CPU_MADISON || count < 2 ? 1 : 2;
}
static inline unsigned ia64_model_socket_cores(unsigned model, unsigned count, unsigned index)
{
    unsigned width = ia64_model_socket_width(model, count);
    unsigned remaining = count - (index / width) * width;
    return remaining < width ? remaining : width;
}
/* Bounded option token; the existing first-key reader supplies precedence. */
static inline bool ia64_cpu_model_parse(const char *value, size_t len, enum ia64_cpu_model *model)
{
    for (unsigned m = IA64_CPU_MONTECITO; m <= IA64_CPU_MADISON; ++m) {
        const char *name = ia64_cpu_model_name(m);
        size_t i = 0;
        while (i < len && name[i] && name[i] == value[i])
            ++i;
        if (i == len && !name[i]) {
            *model = (enum ia64_cpu_model)m;
            return true;
        }
    }
    return false;
}
#endif
