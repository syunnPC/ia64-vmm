/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/ia32/clock.h"

static struct ia64_result general_protection(const struct ia32_native_state *n,
                                             struct ia32_interruption *record)
{
    struct ia32_record_input in = {.kind = IA32_RECORD_EXCEPTION,
                                   .phase = IA32_PHASE_BEFORE,
                                   .source = {(uint32_t)n->cs.base + n->eip.value},
                                   .vector = 13};
    if (ia32_make_record(&in, record) != IA32_STATE_OK)
        return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, 0, "IA-32 clock exception cannot be represented");
    return ia64_complete();
}
uint32_t ia32_clock_cpu_id(const struct vcpu *v)
{
    return v->cfg.index;
}
struct ia64_result ia32_clock(struct vcpu *v, struct ia32_native_state *n,
                              enum ia32_clock_kind kind, struct ia32_interruption *record)
{
    *record = (struct ia32_interruption){0};
    uint64_t value;
    switch (kind) {
    case IA32_CLOCK_RDTSC:
    case IA32_CLOCK_RDTSCP:
        /* Itanium SDM Vol.4 RDTSC: SI and TSD secure nonprivileged reads.
         * RDTSCP uses the same virtual ITC and security policy. Earlier
         * guest loads have completed at this engine instruction boundary. */
        if (n->cpl && ((v->s.psr & IA64_PSR_SI) || (n->cflg & (UINT64_C(1) << 34))))
            return general_protection(n, record);
        value = ia64_itc_read(v);
        if (kind == IA32_CLOCK_RDTSCP)
            n->gpr[IA32_ECX] = ia32_clock_cpu_id(v);
        break;
    case IA32_CLOCK_RDPMC: {
        /* SDM Vol.2 10.3.3.5 / Vol.4 RDPMC: ECX selects PMD[ECX+4].
         * The selected monitor's PMC.pm controls access, as for Itanium
         * PMD reads (Vol.2 Table 7-5), but denial raises #GP instead of 0. */
        uint32_t selector = n->gpr[IA32_ECX];
        if (selector >= ia64_model_pmu_counters(v->cfg.cpu_model))
            return general_protection(n, record);
        unsigned index = selector + 4;
        if (n->cpl && ((v->s.psr & (UINT64_C(1) << 20)) || !(n->cflg & (UINT64_C(1) << 40)) ||
                       (v->s.pmc[index] & (UINT64_C(1) << 6))))
            return general_protection(n, record);
        /* Use the same model-specific PMD read extension as mov=pmd[].
         * The immutable VM model determines the implemented count width. */
        uint64_t sign = UINT64_C(1) << ia64_model_pmd_sign_bit(v->cfg.cpu_model);
        value = ((v->s.pmd[index] & ((sign << 1) - 1)) ^ sign) - sign;
        break;
    }
    default:
        return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, kind, "IA-32 clock operation is unimplemented");
    }
    n->gpr[IA32_EAX] = (uint32_t)value;
    n->gpr[IA32_EDX] = (uint32_t)(value >> 32);
    return ia64_complete();
}
