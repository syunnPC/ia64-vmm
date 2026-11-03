/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Per-vCPU entry stacks and backing stores must be disjoint and inside firmware data. */
#include <stdio.h>
#include "ia64vmm/engine.h"
#include "ia64vmm/platform.h"

#define FAIL(...)                                                                                  \
    do {                                                                                           \
        fprintf(stderr, "entry-abi: " __VA_ARGS__);                                                \
        fputc('\n', stderr);                                                                       \
        return false;                                                                              \
    } while (0)
#define FW_DATA UINT64_C(0x1e000000)      /* any 64 KiB-aligned base */
#define FW_DATA_SIZE UINT64_C(0x2000000)  /* platform/handoff.c FW_DATA_SIZE */
#define HANDOFF_OFFSET UINT64_C(0x100000) /* platform/handoff.c: *gpa = data + 0x100000 */

struct area {
    const char *name;
    uint64_t base, size;
};
static bool overlap(struct area a, struct area b)
{
    return a.base < b.base + b.size && b.base < a.base + a.size;
}

bool smp_entry_abi_test(void);
bool smp_entry_abi_test(void)
{
    struct ia64vmm_handoff h = {.fw_data_base = FW_DATA, .fw_data_size = FW_DATA_SIZE};
    const uint64_t gpa = FW_DATA + HANDOFF_OFFSET, entry = 0x100000, gp = 0x154bb0;
    const struct area fixed[] = {
        {"handoff", gpa, sizeof(h)},
    };
    struct area stack[IA64VMM_VCPU_LIMIT], rse[IA64VMM_VCPU_LIMIT];
    for (unsigned i = 0; i < IA64VMM_VCPU_LIMIT; ++i) {
        struct ia64_entry_regs r;
        platform_entry_regs(&h, gpa, entry, gp, i, &r);
        /* r12 is 16 bytes below the top of CPU i's stack; the
         * firmware's fw_ap_stack_top(i) is fw_data + top - i * stride. */
        if (r.gr[12] !=
                FW_DATA + IA64VMM_ENTRY_STACK_TOP_OFFSET - 16 - i * IA64VMM_ENTRY_STACK_STRIDE ||
            r.bsp != FW_DATA + IA64VMM_ENTRY_RSE_OFFSET + i * IA64VMM_ENTRY_RSE_STRIDE ||
            r.bspstore != r.bsp)
            FAIL("CPU %u: r12=%llx bsp=%llx", i, (unsigned long long)r.gr[12],
                 (unsigned long long)r.bsp);
        if (r.ip != entry || r.br0 != entry || r.gr[1] != gp || r.gr[8] != gpa ||
            r.gr[9] != sizeof(h) || r.gr[10] != IA64VMM_PLATFORM_ID || r.psr || r.rsc ||
            r.k0 != IA64VMM_LEGACY_IO_BASE)
            FAIL("CPU %u: the shared entry registers differ from the BSP's", i);
        for (unsigned g = 2; g < 32; ++g)
            if (g != 8 && g != 9 && g != 10 && g != 12 && r.gr[g])
                FAIL("CPU %u: r%u=%llx at entry", i, g, (unsigned long long)r.gr[g]);
        stack[i] = (struct area){"entry stack", r.gr[12] + 16 - IA64VMM_ENTRY_STACK_STRIDE,
                                 IA64VMM_ENTRY_STACK_STRIDE};
        rse[i] = (struct area){"early backing store", r.bsp, IA64VMM_ENTRY_RSE_STRIDE};
    }
    for (unsigned i = 0; i < IA64VMM_VCPU_LIMIT; ++i) {
        const struct area *mine[2] = {&stack[i], &rse[i]};
        for (unsigned m = 0; m < 2; ++m) {
            struct area a = *mine[m];
            if (a.base < FW_DATA || a.base + a.size > FW_DATA + FW_DATA_SIZE)
                FAIL("CPU %u %s outside fw_data", i, a.name);
            for (unsigned j = 0; j < IA64VMM_VCPU_LIMIT; ++j)
                if (((m == 1 || j != i) && overlap(a, stack[j])) ||
                    ((m == 0 || j != i) && overlap(a, rse[j])))
                    FAIL("CPU %u %s overlaps CPU %u's", i, a.name, j);
            for (unsigned f = 0; f < sizeof(fixed) / sizeof(fixed[0]); ++f)
                if (overlap(a, fixed[f]))
                    FAIL("CPU %u %s overlaps the %s", i, a.name, fixed[f].name);
        }
    }
    return true;
}
