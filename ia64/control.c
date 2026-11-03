/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/interrupt.h"
#include "ia64/rse.h"
#include "ia64/tlb.h"
#include "ia64vmm/interrupts.h"
extern bool platform_irq_update_lid(struct vcpu *, uint64_t) __attribute__((weak));

static bool cfm_valid(uint64_t value)
{
    unsigned sof = value & 127, sol = (value >> 7) & 127, sor = ((value >> 14) & 15) * 8;
    unsigned rrb = (value >> 18) & 127;
    return sof <= 96 && sol <= sof && sor <= sof && (sor ? rrb < sor : !rrb) &&
           ((value >> 25) & 127) < 96 && ((value >> 32) & 63) < 48;
}
static bool cr_defined(unsigned cr)
{
    return cr <= 2 || cr == 8 || cr == 16 || cr == 17 || (cr >= 19 && cr <= 25) ||
           (cr >= 64 && cr <= 74) || cr == 80 || cr == 81;
}
static bool reserved(unsigned cr, uint64_t value)
{
    switch (cr) {
    case CR_DCR:
        return (value >> 15) || (value & 0xf8);
    case CR_PTA:
        return (value & 0x7e02) || ((value >> 2) & 63) < 15;
    case CR_IPSR:
        return (value >> 46) || (value & (UINT64_C(0xf) << 28)) || (value & 0x11fc1) ||
               ((value >> 41) & 3) == 3;
    case CR_ISR:
        return (value >> 44) || (value & (UINT64_C(255) << 24)) || ((value >> 41) & 3) == 3;
    /* SDM Vol.2 Table 3-9 / Vol.3 mov cr: IFM is not a reserved field. */
    case CR_IFS:
        return value & (UINT64_C(0x1ffffff) << 38);
    case CR_LID:
        return value & 0xffff;
    case CR_TPR:
        return value & 0xff00;
    case CR_ITV:
    case CR_PMV:
    case CR_CMCV:
        return value & 0xef00;
    case CR_LRR0:
    case CR_LRR1: {
        unsigned mode = (value >> 8) & 7;
        return (value & 0x4800) || mode == 1 || mode == 3 || mode == 6;
    }
    default:
        return false;
    }
}
struct ia64_result ia64_control(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64SystemOperands *op = &insn->operands.system;
    if (insn->opcode == IA64_OP_RFI) {
        if ((s->psr >> 32) & 3)
            return ia64_fault(IA64_FAULT_PRIVILEGED, insn->raw);
        uint64_t psr = s->cr[CR_IPSR], ifs = s->cr[CR_IFS];
        if (psr & IA64_PSR_IS) {
            /* Fork arch/rse.c: reject absent ISA before restored PSR/RSE. */
            if (v->cfg.cpu_model == IA64_CPU_MONTECITO)
                return ia64_fault(IA64_FAULT_DISABLED_ISA, insn->raw);
            return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, psr,
                             "IA-32 native backend not implemented (cpu_model=madison)");
        }
        if (reserved(CR_IPSR, psr) || reserved(CR_IFS, ifs))
            return ia64_fault(IA64_FAULT_RESERVED, psr);
        if (ifs >> 63) {
            /* Frame restoration requires a usable CFM; these constraints
             * do not apply to mov cr.ifs (SDM Vol.2 Table 3-9). */
            if (!cfm_valid(ifs))
                return ia64_fault(IA64_FAULT_RESERVED, ifs);
            /* rfi undoes cover by IFM.sof, whereas br.ret undoes a call by
             * PFM.sol. Mandatory fills use the restored translation mode. */
            struct ia64_result r = ia64_rse_restore(v, ifs & ((UINT64_C(1) << 38) - 1), ifs & 127);
            if (r.kind != IA64_STEP_COMPLETE)
                return r;
        }
        s->rse_cfle = true;
        ia64_psr_write(s, psr);
        return (struct ia64_result){.kind = IA64_STEP_BRANCH,
                                    .info = s->cr[CR_IIP] & ~UINT64_C(15),
                                    .next_slot = (psr >> 41) & 3};
    }
    if (insn->opcode != IA64_OP_MOV_CRGR && insn->opcode != IA64_OP_MOV_GRCR)
        return ia64_tlb(v, insn);
    unsigned cr = op->source;
    bool write = insn->opcode == IA64_OP_MOV_GRCR;
    uint64_t value = ia64_gr_read(s, op->destination);
    /* Undefined model CRs are per-vCPU
     * storage. Defined CRs retain SDM Vol.2 Table 3-3 access restrictions;
     * in particular reserved CR18 is accessible even with PSR.ic set. */
    bool defined = cr_defined(cr);
    if (defined && (((s->psr & IA64_PSR_IC) && cr >= 16 && cr <= 25) ||
                    (write && (cr == CR_IVR || (cr >= CR_IRR0 && cr < CR_IRR0 + 4)))))
        return ia64_fault(IA64_FAULT_ILLEGAL, cr);
    if ((s->psr >> 32) & 3)
        return ia64_fault(IA64_FAULT_PRIVILEGED, insn->raw);
    if (write) {
        if (ia64_nat_read(s, op->destination))
            return ia64_fault(IA64_FAULT_NAT, value);
        if (reserved(cr, value))
            return ia64_fault(IA64_FAULT_RESERVED, value);
        /* Resolve the old match with its original vector and mask before
         * changing the timer (SDM Vol.2 §§3.3.4.2, 5.8.3.5–6). */
        if (cr == CR_ITM || cr == CR_ITV)
            ia64_timer_poll(v);
        switch (cr) {
        case CR_IVA:
            value &= ~UINT64_C(0x7fff);
            break;
        case CR_IHA:
            value &= ~UINT64_C(3);
            break;
        case CR_LID:
            value &= UINT64_C(0xffff0000);
            if (platform_irq_update_lid && !platform_irq_update_lid(v, value))
                return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, value, "duplicate platform LID");
            break;
        case CR_TPR:
            value &= UINT64_C(0x100f0);
            break;
        case CR_EOI:
            ia64_eoi(v);
            value = 0;
            break;
        case CR_ITV:
        case CR_PMV:
        case CR_CMCV:
            value &= UINT64_C(0x100ff);
            break;
        case CR_LRR0:
        case CR_LRR1:
            value &= UINT64_C(0x1a7ff);
            break;
        default:
            break;
        }
        s->cr[cr] = value;
        if (cr == CR_IIPA)
            v->last_successful_bundle = value;
        if (cr == CR_ITM)
            ia64_timer_program(v);
    } else {
        value = cr == CR_IVR                        ? ia64_ivr_read(v)
                : cr == CR_EOI                      ? 0
                : cr >= CR_IRR0 && cr < CR_IRR0 + 4 ? __atomic_load_n(&s->cr[cr], __ATOMIC_ACQUIRE)
                                                    : s->cr[cr];
        ia64_gr_write(s, op->destination, value, false);
    }
    return ia64_complete();
}
