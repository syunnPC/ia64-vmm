/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_HOST_ISA_H
#define IA64VMM_HOST_ISA_H
#include "ia64vmm/types.h"

/* Scalar JIT capabilities, independent of guest CPUID and host XSTATE. */
#define HOST_ISA_BMI1 (1u << 0)
#define HOST_ISA_BMI2 (1u << 1)
#define HOST_ISA_LZCNT (1u << 2)
#define HOST_ISA_MOVBE (1u << 3)
#define HOST_ISA_ALL 15u
typedef void (*host_isa_cpuid_fn)(uint32_t, uint32_t, uint32_t *, uint32_t *, uint32_t *,
                                  uint32_t *);

/* Intel SDM Vol.2 CPUID: unsupported leaves must not be queried. LZCNT
 * has its own bit: BMI1 alone does not distinguish LZCNT from BSR. */
static inline unsigned host_isa_probe(host_isa_cpuid_fn cpuid)
{
    uint32_t a, b, c, d;
    unsigned features = 0;
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max = a;
    if (max >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        if (c & (1u << 22))
            features |= HOST_ISA_MOVBE;
    }
    if (max >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        if (b & (1u << 3))
            features |= HOST_ISA_BMI1;
        if (b & (1u << 8))
            features |= HOST_ISA_BMI2;
    }
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        if (c & (1u << 5))
            features |= HOST_ISA_LZCNT;
    }
    return features;
}

/* Boot policy shared by the product and shim. Callers acquire each CPU's
 * ready flag before copying its features into this immutable snapshot. */
struct host_isa_cpu_report {
    unsigned features;
    bool reported;
};

/* Decimal disable-only mask; first matching command-line token wins. */
static inline bool host_isa_parse_mask(const char *cmdline, unsigned *out)
{
    const char key[] = "host_isa_mask=";
    unsigned mask = HOST_ISA_ALL;
    const char *p = cmdline;
    while (*p) {
        while (*p && *p <= ' ')
            ++p;
        const char *end = p;
        while (*end > ' ')
            ++end;
        unsigned n = 0;
        while (n < sizeof(key) - 1 && p + n < end && p[n] == key[n])
            ++n;
        if (n == sizeof(key) - 1) {
            p += n;
            if (p == end)
                return false;
            mask = 0;
            while (p != end) {
                if (*p < '0' || *p > '9')
                    return false;
                mask = mask * 10 + (unsigned)(*p++ - '0');
                if (mask > HOST_ISA_ALL)
                    return false;
            }
            break;
        }
        p = end;
    }
    *out = mask;
    return true;
}

/* Pure publication decision: -1 invalid policy, 0 awaiting reports, 1 ready.
 * Never return a publishable value before every executor has reported. */
static inline int host_isa_resolve(const char *cmdline, const struct host_isa_cpu_report *cpus,
                                   unsigned count, unsigned *out)
{
    unsigned mask;
    if (!host_isa_parse_mask(cmdline, &mask))
        return -1;
    if (!count)
        return 0;
    unsigned features = HOST_ISA_ALL;
    for (unsigned i = 0; i < count; ++i) {
        if (!cpus[i].reported)
            return 0;
        features &= cpus[i].features;
    }
    features &= mask;
    *out = features;
    return 1;
}

static inline void host_isa_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b,
                                  uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(subleaf));
}
#endif
