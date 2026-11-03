/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_PROFILE_H
#define IA64VMM_IA64_PROFILE_H
#include <stdint.h>
#include <stdbool.h>
#include "ia64/decode/opcode.h"
#include "ia64vmm/cpu_model.h"

/* Fork target/ia64/arch/mmu.c hash/tag pair and cpu.c Montecito id 8.
 * The software walker is synchronous in the single-vCPU, no-DMA configuration. */
#define IA64_VHPT_WALKER 1u
#define IA64_HASH_TAG_ID 8u
static inline bool ia64_page_size_supported(unsigned shift)
{
    return shift < 64 && ((UINT64_C(0x155557000) >> shift) & 1);
}

/* Fork target/ia64/cpu.c: Montecito 9050 C2; SDM Vol.1 §3.1.11.
 * Feature bits 32..63 tested by tf are all zero on this model. */
#define IA64_CPUID_FEATURES UINT64_C(5)
/* SDM Rev.2.3 Vol.1 Table 3-8: cz (32) and x2 (33) control the
 * instruction_implemented checks, including their fault priority. */
static inline bool ia64_model_instruction(unsigned model, Ia64Opcode opcode)
{
    switch (opcode) {
    case IA64_OP_LD16:
    case IA64_OP_ST16:
    case IA64_OP_CMP8XCHG16:
    case IA64_OP_VMSW:
        return model != IA64_CPU_MADISON;
    case IA64_OP_CLZ:
        return (ia64_model_cpuid(model, 4) >> 32) & 1;
    case IA64_OP_MPY4:
    case IA64_OP_MPYSHL4:
        return (ia64_model_cpuid(model, 4) >> 33) & 1;
    default:
        return true;
    }
}
/* Fork cpu.c: Madison 64 ITR/DTR, Montecito 32; both sides use
 * IA64_TLB_MAX=128 at level 2 (arch/pal.c pal_vm_info). */
static inline unsigned ia64_model_tr_count(unsigned model, unsigned side)
{
    (void)side;
    return model == IA64_CPU_MADISON ? 64u : 32u;
}
static inline unsigned ia64_model_tc_count(unsigned model, unsigned side)
{
    (void)model;
    (void)side;
    return 128u;
}
/* Legacy default-profile helpers retained for existing tests/callers. */
static inline bool ia64_profile_instruction(Ia64Opcode opcode)
{
    return ia64_model_instruction(IA64_CPU_MONTECITO, opcode);
}
static inline uint64_t ia64_cpuid(unsigned index)
{
    return ia64_model_cpuid(IA64_CPU_MONTECITO, index);
}
#endif
