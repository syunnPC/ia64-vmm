/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The default IA-32 entry must stop without touching native/architectural
 * state, and unavailable publication must not leak a fabricated snapshot. */
#include "ia64/state.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

_Static_assert(VCPU_EXIT_DEBUG == 5 && IA64_STATE_VERSION == 4,
               "existing exit numbers and TRC architectural layout version");
_Static_assert(offsetof(struct vcpu, irq_kick) == offsetof(struct vcpu, requested_exit) + 4,
               "Tier1 mailbox pair unchanged");
_Static_assert(_Generic((ia32_token_t){0}, gpa_t: 0, default: 1), "token is not GPA");
_Static_assert(_Generic((ia32_eip_t){0}, ia32_linear_t: 0, default: 1), "EIP is not linear IP");
static unsigned dumps, logs;
void vcpu_dump(const struct vcpu *v)
{
    (void)v;
    ++dumps;
}
void vmm_log(enum log_level level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
    ++logs;
}

int main(void)
{
    static struct vcpu v, before;
    static struct vcpu_isa_snapshot sample, sample_before;
    const struct vcpu_run_budget budgets[] = {
        {0},
        {.ia64_instructions = 1, .native = {.deadline = 123, .instructions = 1}},
        {.ia64_instructions = UINT64_MAX, .native = {.deadline = UINT64_MAX}},
    };
    if (hal_ia32_backend() || engine_ia32_glue())
        return 1;
    for (unsigned isa = 0; isa < 2; ++isa) {
        memset(&v, 0, sizeof(v));
        v.native = NULL;
        v.s.gr[8] = UINT64_C(0xaaaaaaaa80000001);
        v.s.nat[8] = true;
        v.s.ar[AR_CSD] = UINT64_C(0xfedcba9876543210);
        v.retired = 123;
        v.irq_kick = 1;
        v.s.psr = isa ? IA64_PSR_IS : 0;
        v.s.ip = isa ? UINT64_C(0x1234567b) : UINT64_C(0x4000000010000010);
        v.s.slot = 2;
        memcpy(&before, &v, sizeof(v));
        for (unsigned b = 0; b < IA64VMM_ARRAY_SIZE(budgets); ++b) {
            struct vcpu_run_exit out = engine_run_bounded(&v, &budgets[b]);
            if (out.stop != VCPU_RUN_EXIT || out.reason != VCPU_EXIT_UNIMPLEMENTED || !out.detail ||
                out.position.isa != (isa ? VCPU_ISA_IA32 : VCPU_ISA_IA64) ||
                out.position.ip != v.s.ip || out.position.slot != (isa ? 0u : 2u) ||
                out.position.eip_valid || out.native_exit_valid || out.ia64_retired ||
                out.native_usage.instructions_valid || out.native_usage.instructions ||
                out.native_usage.residence_ticks || memcmp(&v, &before, sizeof(v)))
                return 2;
        }
        memset(&sample, 0xa5, sizeof(sample));
        memcpy(&sample_before, &sample, sizeof(sample));
        if (vcpu_snapshot_isa(&v, &sample) || memcmp(&sample, &sample_before, sizeof(sample)))
            return 3;
    }
    /* Only IA-64 may call the legacy dump (which decodes bundles). */
    return dumps == 3 && logs > 6 ? 0 : 4;
}
