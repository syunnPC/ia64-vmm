/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/semantics.h"
#include "ia64/profile.h"
#include "ia64/interrupt.h"
#include "ia64/tlb.h"
#include "ia64/mmu_log.h"

void ia64_psr_write(struct ia64_state *s, uint64_t value)
{
    /* SDM Vol.2 §3.3.2: only the selected bank of r16..r31 is visible.
     * Centralizing this also serves interruption entry and rfi. */
    if ((s->psr ^ value) & IA64_PSR_BN) {
        for (unsigned i = 0; i < 16; ++i) {
            uint64_t gr = s->gr[16 + i];
            bool nat = s->nat[16 + i];
            s->gr[16 + i] = s->bank_gr[i];
            s->nat[16 + i] = s->bank_nat[i];
            s->bank_gr[i] = gr;
            s->bank_nat[i] = nat;
        }
    }
    s->psr = value;
}

struct ia64_result ia64_system_registers(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64SystemOperands *op = &insn->operands.system;
    uint64_t value = ia64_gr_read(s, op->destination);
    bool write = false, indexed = false;
    uint64_t *bank = NULL, index = op->source;
    unsigned count = 0, bias = 0;
    /* User mask and CPUID are unprivileged; the remaining banks require CPL0. */
    switch (insn->opcode) {
    case IA64_OP_EPC: {
        /* SDM Rev.2.3 Vol.3 epc (3:53), Vol.2 Table 4-4. Ported from
         * the fork's target/ia64/arch/system.c:ia64_system_epc. */
        unsigned cpl = (s->psr >> 32) & 3;
        if ((s->ar[AR_PFS] >> 62) < cpl)
            return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
        unsigned promoted = 0;
        if (s->psr & IA64_PSR_IT) {
            const struct ia64_tlb_entry *entry = ia64_tlb_find(s, insn->address, 0);
            /* Execution follows a successful fetch; do not repeat its
             * walker, permission checks or RSE loads here. */
            if (!entry)
                return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, insn->address,
                                 "epc without fetched instruction translation");
            unsigned pl = (entry->pte >> 7) & 3;
            promoted = ((entry->pte >> 9) & 7) == 7 && pl < cpl ? pl : cpl;
        }
        ia64_psr_write(s, (s->psr & ~(UINT64_C(3) << 32)) | ((uint64_t)promoted << 32));
        return ia64_complete();
    }
    case IA64_OP_THASH:
    case IA64_OP_TTAG:
        return ia64_tlb(v, insn);
    case IA64_OP_MOV_CRGR:
    case IA64_OP_MOV_GRCR:
    case IA64_OP_RFI:
        return ia64_control(v, insn);
    case IA64_OP_MOV_CPUID_INDEXED:
        index = ia64_gr_read(s, op->register_index) & 255;
        if (ia64_nat_read(s, op->register_index))
            return ia64_fault(IA64_FAULT_NAT, index);
        if (index >= 5)
            return ia64_fault(IA64_FAULT_RESERVED, index);
        ia64_gr_write(s, op->destination, ia64_model_cpuid(v->cfg.cpu_model, index), false);
        return ia64_complete();
    case IA64_OP_MOV_UMGR:
        ia64_gr_write(s, op->destination, s->psr & 0x3e, false);
        return ia64_complete();
    case IA64_OP_MOV_GRUM:
    case IA64_OP_SUM_UM:
    case IA64_OP_RUM: {
        if (insn->opcode != IA64_OP_MOV_GRUM)
            value = op->immediate;
        else if (ia64_nat_read(s, op->destination))
            return ia64_fault(IA64_FAULT_NAT, value);
        if (value & ~UINT64_C(0x3e))
            return ia64_fault(IA64_FAULT_RESERVED, value);
        uint64_t psr = insn->opcode == IA64_OP_SUM_UM ? s->psr | value
                       : insn->opcode == IA64_OP_RUM  ? s->psr & ~value
                                                      : (s->psr & ~UINT64_C(0x3e)) | value;
        /* SDM Vol.3 sum/rum/mov um: PSR.up can only change if PSR.sp=0. */
        if (s->psr & (UINT64_C(1) << 20))
            psr = (psr & ~UINT64_C(4)) | (s->psr & 4);
        ia64_psr_write(s, psr);
        return ia64_complete();
    }
    default:
        break;
    }
    /* SDM Vol.3 itc/itr: PSR.ic=1 is an Illegal Operation fault, checked
     * before the privilege level. */
    if ((insn->opcode == IA64_OP_ITC_D || insn->opcode == IA64_OP_ITC_I ||
         insn->opcode == IA64_OP_ITR_D || insn->opcode == IA64_OP_ITR_I) &&
        (s->psr & IA64_PSR_IC))
        return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
    bool pmd_read = insn->opcode == IA64_OP_MOV_PMDGR || insn->opcode == IA64_OP_MOV_PMDGR_INDEXED;
    unsigned cpl = (s->psr >> 32) & 3;
    if (cpl && !pmd_read)
        return ia64_fault(IA64_FAULT_PRIVILEGED, insn->raw);
    switch (insn->opcode) {
    case IA64_OP_MOV_PSRGR:
        ia64_gr_write(s, op->destination, s->psr & (UINT64_C(0xffffffff) | (UINT64_C(3) << 35)),
                      false);
        return ia64_complete();
    case IA64_OP_MOV_GRPSR:
        if (ia64_nat_read(s, op->destination))
            return ia64_fault(IA64_FAULT_NAT, value);
        /* mov psr.l ignores the upper half; checking its reserved bits is
         * implementation optional. Defined high fields never change here. */
        if ((value & UINT64_C(0xffffffff)) &
            ~(UINT64_C(0x3e) | (UINT64_C(7) << 13) | (UINT64_C(0x7ff) << 17)))
            return ia64_fault(IA64_FAULT_RESERVED, value);
        ia64_psr_write(s, (s->psr & ~UINT64_C(0xffffffff)) | (value & UINT64_C(0xffffffff)));
        return ia64_complete();
    case IA64_OP_SSM:
    case IA64_OP_RSM:
        value = op->immediate;
        if (value & ~(UINT64_C(0x3e) | (UINT64_C(7) << 13) | (UINT64_C(0x7f) << 17)))
            return ia64_fault(IA64_FAULT_RESERVED, value);
        ia64_psr_write(s, insn->opcode == IA64_OP_SSM ? s->psr | value : s->psr & ~value);
        return ia64_complete();
    case IA64_OP_BSW0:
    case IA64_OP_BSW1:
        ia64_psr_write(s,
                       insn->opcode == IA64_OP_BSW1 ? s->psr | IA64_PSR_BN : s->psr & ~IA64_PSR_BN);
        return ia64_complete();
    case IA64_OP_MOV_RRGR:
    case IA64_OP_MOV_GRRR:
        index = ia64_gr_read(s, op->source);
        if (ia64_nat_read(s, op->source))
            return ia64_fault(IA64_FAULT_NAT, index);
        index >>= 61;
        if (insn->opcode == IA64_OP_MOV_GRRR) {
            if (ia64_nat_read(s, op->destination))
                return ia64_fault(IA64_FAULT_NAT, value);
            unsigned ps = (value >> 2) & 63;
            if ((value & ~UINT64_C(0xfffffffd)) || !ia64_page_size_supported(ps))
                return ia64_fault(IA64_FAULT_RESERVED, value);
            s->rr[index] = value;
            ia64_mmu_logged(v, IA64_MMU_W_RR, IA64_MMU_RR, 0, (unsigned)index);
        } else
            ia64_gr_write(s, op->destination, s->rr[index], false);
        return ia64_complete();
    case IA64_OP_MOV_PKRGR:
    case IA64_OP_MOV_PKRGR_INDEXED:
    case IA64_OP_MOV_GRPKR:
    case IA64_OP_MOV_GRPKR_INDEXED:
        bank = s->pkr;
        count = 16;
        indexed =
            insn->opcode == IA64_OP_MOV_PKRGR_INDEXED || insn->opcode == IA64_OP_MOV_GRPKR_INDEXED;
        write = insn->opcode == IA64_OP_MOV_GRPKR || insn->opcode == IA64_OP_MOV_GRPKR_INDEXED;
        break;
    case IA64_OP_MOV_DBRGR:
    case IA64_OP_MOV_DBRGR_INDEXED:
    case IA64_OP_MOV_GRDBR:
    case IA64_OP_MOV_GRDBR_INDEXED:
        bank = s->dbr;
        count = 8;
        bias = 8;
        indexed =
            insn->opcode == IA64_OP_MOV_DBRGR_INDEXED || insn->opcode == IA64_OP_MOV_GRDBR_INDEXED;
        write = insn->opcode == IA64_OP_MOV_GRDBR || insn->opcode == IA64_OP_MOV_GRDBR_INDEXED;
        break;
    case IA64_OP_MOV_IBRGR:
    case IA64_OP_MOV_IBRGR_INDEXED:
    case IA64_OP_MOV_GRIBR:
    case IA64_OP_MOV_GRIBR_INDEXED:
        bank = s->ibr;
        count = 8;
        bias = 12;
        indexed =
            insn->opcode == IA64_OP_MOV_IBRGR_INDEXED || insn->opcode == IA64_OP_MOV_GRIBR_INDEXED;
        write = insn->opcode == IA64_OP_MOV_GRIBR || insn->opcode == IA64_OP_MOV_GRIBR_INDEXED;
        break;
    case IA64_OP_MOV_PMCGR:
    case IA64_OP_MOV_PMCGR_INDEXED:
    case IA64_OP_MOV_GRPMC:
    case IA64_OP_MOV_GRPMC_INDEXED:
        bank = s->pmc;
        count = 64;
        bias = 32;
        indexed =
            insn->opcode == IA64_OP_MOV_PMCGR_INDEXED || insn->opcode == IA64_OP_MOV_GRPMC_INDEXED;
        write = insn->opcode == IA64_OP_MOV_GRPMC || insn->opcode == IA64_OP_MOV_GRPMC_INDEXED;
        break;
    case IA64_OP_MOV_PMDGR:
    case IA64_OP_MOV_PMDGR_INDEXED:
    case IA64_OP_MOV_GRPMD:
    case IA64_OP_MOV_GRPMD_INDEXED:
        bank = s->pmd;
        count = 64;
        bias = 64;
        indexed =
            insn->opcode == IA64_OP_MOV_PMDGR_INDEXED || insn->opcode == IA64_OP_MOV_GRPMD_INDEXED;
        write = insn->opcode == IA64_OP_MOV_GRPMD || insn->opcode == IA64_OP_MOV_GRPMD_INDEXED;
        break;
    case IA64_OP_MOV_MSRGR:
    case IA64_OP_MOV_GRMSR:
        bank = s->msr;
        count = 1024;
        indexed = true;
        write = insn->opcode == IA64_OP_MOV_GRMSR;
        break;
    default:
        return ia64_control(v, insn);
    }
    if (indexed) {
        index = ia64_gr_read(s, op->register_index);
        if (ia64_nat_read(s, op->register_index))
            return ia64_fault(IA64_FAULT_NAT, index);
        if (bank != s->msr)
            index &= 255;
    } else
        index -= bias;
    if (write && ia64_nat_read(s, op->destination))
        return ia64_fault(IA64_FAULT_NAT, value);
    if (index >= count) {
        if (bank == s->pmc || bank == s->pmd || bank == s->msr) {
            if (!write)
                ia64_gr_write(s, op->destination, 0, false);
            return ia64_complete();
        }
        return ia64_fault(IA64_FAULT_RESERVED, index);
    }
    bool generic_pmd =
        bank == s->pmd && index >= 4 && index < 4 + ia64_model_pmu_counters(v->cfg.cpu_model);
    if (bank == s->pmc && index < 4) {
        uint64_t mask = index ? 0 : ia64_model_pmc0_mask(v->cfg.cpu_model);
        if (write)
            bank[index] = value & mask;
        else
            ia64_gr_write(s, op->destination, bank[index] & mask, false);
        return ia64_complete();
    }
    if (write) {
        if (generic_pmd)
            value &= UINT64_C(0xffffffffffff);
        if (bank == s->pkr && (value & ~UINT64_C(0xffffff0f)))
            return ia64_fault(IA64_FAULT_RESERVED, value);
        if (bank == s->pkr && (value & 1))
            for (unsigned i = 0; i < count; ++i)
                if (i != index && (bank[i] & 1) &&
                    (bank[i] & UINT64_C(0xffffff00)) == (value & UINT64_C(0xffffff00))) {
                    bank[i] &= ~UINT64_C(1);
                    ia64_mmu_logged(v, IA64_MMU_W_PKR_DUP, IA64_MMU_PKR, 0, i);
                }
        bank[index] = value;
        if (bank == s->pkr)
            ia64_mmu_logged(v, IA64_MMU_W_PKR, IA64_MMU_PKR, 0, (unsigned)index);
    } else {
        bool hidden = bank == s->pmd && cpl &&
                      ((s->psr & (UINT64_C(1) << 20)) ||
                       (generic_pmd && (s->pmc[index] & (UINT64_C(1) << 6))));
        value = bank[index];
        if (generic_pmd) {
            uint64_t sign = UINT64_C(1) << ia64_model_pmd_sign_bit(v->cfg.cpu_model);
            value = ((value & ((sign << 1) - 1)) ^ sign) - sign;
        }
        ia64_gr_write(s, op->destination, hidden ? 0 : value, false);
    }
    return ia64_complete();
}
