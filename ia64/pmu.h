/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_PMU_H
#define IA64VMM_IA64_PMU_H
#include "ia64/state.h"

struct ia64_pmu_retirement {
    unsigned count, interrupt;
};
void ia64_pmu_reset(struct vcpu *v);
bool ia64_pmu_active(const struct vcpu *v);
struct ia64_result ia64_pmu_prepare(const struct vcpu *v, const Ia64Instruction *i,
                                    struct ia64_pmu_retirement *out);
void ia64_pmu_retire(struct vcpu *v, struct ia64_pmu_retirement pending);
#endif
