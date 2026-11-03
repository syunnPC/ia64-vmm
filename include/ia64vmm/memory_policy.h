/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_MEMORY_POLICY_H
#define IA64VMM_MEMORY_POLICY_H
#include <stdint.h>
/* Unknown/small reserves retain the original defaults. Late cache budgets
 * are also bounded by free memory after mandatory lazy allocations. */
static inline uint64_t vmm_tier1_code_bytes(uint64_t reserve_bytes)
{
    uint64_t mib = reserve_bytes / 32 / 1048576;
    return (mib < 16 ? 16 : mib > 512 ? 512 : mib) * UINT64_C(1048576);
}
static inline uint64_t vmm_cache_budget_limit(uint64_t reserve_bytes)
{
    uint64_t bytes = (reserve_bytes / 16) & ~UINT64_C(4095);
    return bytes<(UINT64_C(48) << 20) ? UINT64_C(48) << 20 : bytes>(UINT64_C(1) << 30)
               ? UINT64_C(1) << 30
               : bytes;
}
static inline uint64_t vmm_tier1_pool_limit(uint64_t reserve_bytes)
{
    uint64_t bytes = (reserve_bytes / 8) & ~UINT64_C(4095);
    return bytes<(UINT64_C(48) << 20) ? UINT64_C(48) << 20 : bytes>(UINT64_C(1) << 30)
               ? UINT64_C(1) << 30
               : bytes;
}
#endif
