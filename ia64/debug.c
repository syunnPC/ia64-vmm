/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Breakpoint matching ported from target/ia64/arch/mmu.c in the GPL IA-64
 * QEMU fork. SDM Rev.2.3 Vol.2 §§7.1.1–2 defines the masks and byte ranges. */
#include "ia64/debug.h"

bool ia64_debug_match(const struct ia64_state *s, uint64_t address, unsigned size, unsigned rights,
                      unsigned level)
{
    bool execute = rights & IA64_ACCESS_EXECUTE;
    const uint64_t *regs = execute ? s->ibr : s->dbr;
    for (unsigned pair = 0; pair < 4; ++pair) {
        uint64_t control = regs[pair * 2 + 1];
        bool enabled = ((rights & (IA64_ACCESS_READ | IA64_ACCESS_EXECUTE)) && (control >> 63)) ||
                       ((rights & IA64_ACCESS_WRITE) && (control & (UINT64_C(1) << 62)));
        if (!enabled || !(control & (UINT64_C(1) << (56 + level))))
            continue;
        uint64_t mask = UINT64_C(0xff00000000000000) | (control & UINT64_C(0x00ffffffffffffff));
        if (execute)
            mask &= ~UINT64_C(15);
        for (unsigned byte = 0; byte < size; ++byte)
            if (!(((address + byte) ^ regs[pair * 2]) & mask))
                return true;
    }
    return false;
}
unsigned ia64_debug_armed(const struct ia64_state *s)
{
    /* The enables ia64_debug_match tests (SDM Rev.2.3 Vol.2 Table 7-1). */
    unsigned armed = 0;
    for (unsigned pair = 0; pair < 4; ++pair) {
        uint64_t ibr = s->ibr[pair * 2 + 1], dbr = s->dbr[pair * 2 + 1];
        if (ibr >> 63)
            armed |= (unsigned)(ibr >> 56) & 15;
        if (dbr >> 62)
            armed |= ((unsigned)(dbr >> 56) & 15) << IA64_DEBUG_ARMED_DATA_SHIFT;
    }
    return armed;
}
struct ia64_result ia64_debug_instruction(struct vcpu *v)
{
    if (!(v->s.psr & IA64_PSR_DB) || (v->s.psr & IA64_PSR_ID) ||
        !ia64_debug_match(&v->s, v->s.ip, 1, IA64_ACCESS_EXECUTE, (v->s.psr >> 32) & 3))
        return ia64_complete();
    struct ia64_result r = ia64_fault(IA64_FAULT_DEBUG, v->s.ip);
    r.fault_isr = UINT64_C(1) << 32;
    return r;
}
struct ia64_result ia64_debug_data(struct vcpu *v, uint64_t address, unsigned size, unsigned rights,
                                   unsigned level, bool speculative, bool rse)
{
    /* Fork arch/mmu.c: Madison faults a crossing access whenever an
     * access/privilege-matching DBR is enabled, independent of its address.
     * VHPT checks continue using exact register matches, as in the fork. */
    bool crossing = false;
    if (v->cfg.cpu_model == IA64_CPU_MADISON && (address & 15) + size > 16)
        for (unsigned pair = 0; pair < 4; ++pair) {
            uint64_t control = v->s.dbr[pair * 2 + 1];
            if ((control & (UINT64_C(1) << (56 + level))) &&
                (((rights & IA64_ACCESS_READ) && (control >> 63)) ||
                 ((rights & IA64_ACCESS_WRITE) && (control & (UINT64_C(1) << 62)))))
                crossing = true;
        }
    if (!(v->s.psr & IA64_PSR_DB) || (v->s.psr & IA64_PSR_DD) ||
        (!crossing && !ia64_debug_match(&v->s, address, size, rights, level)))
        return ia64_complete();
    if (speculative && ia64_defer_fault(v, IA64_FAULT_DEBUG))
        return (struct ia64_result){.kind = IA64_STEP_COMPLETE, .info = 1};
    struct ia64_result r = ia64_fault(IA64_FAULT_DEBUG, address);
    r.fault_isr = ((rights & IA64_ACCESS_READ) ? UINT64_C(1) << 34 : 0) |
                  ((rights & IA64_ACCESS_WRITE) ? UINT64_C(1) << 33 : 0) |
                  (speculative ? UINT64_C(1) << 36 : 0) | (rse ? UINT64_C(1) << 37 : 0);
    return r;
}
struct ia64_result ia64_completion_trap(struct vcpu *v, const Ia64Instruction *insn,
                                        uint64_t old_psr, bool branch, struct ia64_result result)
{
    /* SDM Vol.2 §§5.3, 8.1 Table 8-3: concurrent trap codes are combined;
     * FP > LP > TB > SS. rfi starts the restored stream without trapping. */
    if (insn->opcode == IA64_OP_RFI)
        return result;
    if (branch && v->s.rse_completion_pending && v->s.rse_cfle) {
        /* br.ret has committed, but the target-frame loads precede its
         * completion traps. Retirement must retain DA/DD for the first
         * mandatory reference (SDM Vol.2 §§5.3, 6.6). */
        v->s.psr |= old_psr & (IA64_PSR_DA | IA64_PSR_DD);
        return ia64_complete();
    }
    bool fp = result.fault == IA64_FAULT_FP_TRAP;
    uint64_t code = fp ? result.info | 1 : 0;
    if (branch && ((v->s.psr >> 32) & 3) > ((old_psr >> 32) & 3) && (old_psr & IA64_PSR_LP))
        code |= 2;
    if (branch && (old_psr & IA64_PSR_TB))
        code |= 4;
    if (old_psr & IA64_PSR_SS)
        code |= 8;
    if (!code)
        return ia64_complete();
    return ia64_fault(fp           ? IA64_FAULT_FP_TRAP
                      : (code & 2) ? IA64_FAULT_LOWER_PRIVILEGE
                      : (code & 4) ? IA64_FAULT_TAKEN_BRANCH
                                   : IA64_FAULT_SINGLE_STEP,
                      code);
}
