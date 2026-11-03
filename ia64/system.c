/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/semantics.h"
#include "ia64/rse.h"

/* Implemented in engine/engine.c; this also keeps the platform hook out
 * of the architectural state and future generated-code ABI. */
bool ia64_break_hook(struct vcpu *v, uint64_t imm, uint64_t ip);

static bool illegal_ar(unsigned ar, IA64SlotUnit unit, bool write)
{
    /* SDM Vol.1 Table 3-3 / Vol.3 mov ar: keep M/I bank selection,
     * read-only BSP and the unimplemented RUC unchanged. Reserved ARs
     * in each bank retain stored values. */
    if (unit == IA64_UNIT_I)
        return ar <= 47;
    return (ar >= 64 && ar <= 111) || ar == 45 || (write && ar == AR_BSP);
}
static bool ignored_ar(unsigned ar)
{
    return (ar >= 48 && ar <= 63) || ar >= 112;
}
static bool reserved_pfs(uint64_t value)
{
    /* SDM Vol.1 §3.1.8.12: mov checks the reserved PFS fields only.
     * Frame recovery consumes PFM at br.ret (Vol.2 §6.5.5). */
    return value & ((UINT64_C(15) << 58) | (UINT64_C(0x3fff) << 38));
}
static bool reserved_fpsr(uint64_t value)
{
    return (value >> 58) || ((value >> 12) & 1) || ((value >> 8) & 3) == 1 ||
           ((value >> 21) & 3) == 1 || ((value >> 34) & 3) == 1 || ((value >> 47) & 3) == 1;
}

struct ia64_result ia64_system(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64SystemOperands *op = &insn->operands.system;
    /* Most system operations do not consume a GR. In particular, empty
     * bundle slots and transfers into a GR need no old destination value
     * or NaT, and no rotating/stacked-register address calculation. */
    uint64_t value;
    switch (insn->opcode) {
    case IA64_OP_VMSW:
        /* SDM Vol.3 vmsw: recognized on Montecito, no VM environment.
         * Madison absence is checked by ia64_execute before privilege. */
        return ia64_fault((s->psr >> 32) & 3 ? IA64_FAULT_PRIVILEGED : IA64_FAULT_VIRTUALIZATION,
                          insn->raw);
    case IA64_OP_ALLOC:
    case IA64_OP_COVER:
    case IA64_OP_FLUSHRS:
    case IA64_OP_LOADRS:
        return ia64_rse(v, insn);
    case IA64_OP_MF:
    case IA64_OP_MF_A:
    case IA64_OP_SYNC_I:
        return ia64_memory(v, insn);
    case IA64_OP_SRLZ:
    case IA64_OP_SRLZ_D:
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return ia64_complete();
    case IA64_OP_FWB:
        /* SDM Vol.3 fwb: mem_flush_pending_stores(), no interruptions. Guest
         * stores are host stores; a full host fence (x86 MFENCE) drains the
         * store and write-combining buffers, e.g. a WC framebuffer's. */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return ia64_complete();
    case IA64_OP_BRP:
        /* SDM Vol.3 brp: prediction hints have no architectural effects,
         * no predicate, and no instruction-specific faults. */
        return ia64_complete();
    case IA64_OP_NOP:
    case IA64_OP_HINT_I:
    case IA64_OP_HINT_B:
    case IA64_OP_HINT_F:
    case IA64_OP_HINT_X:
        return ia64_complete();
    case IA64_OP_HINT_M:
        if (insn->hint_m_reg_increment || insn->imm_base_update) {
            unsigned base = op->register_index;
            if (base >= 32 && base - 32u >= (s->cfm & 127))
                return ia64_fault(IA64_FAULT_ILLEGAL, base);
            value =
                ia64_gr_read(s, base) + (insn->hint_m_reg_increment ? ia64_gr_read(s, op->source)
                                                                    : (uint64_t)op->immediate);
            ia64_gr_write(s, base, value, ia64_nat_read(s, base));
        }
        return ia64_complete();
    case IA64_OP_BREAK: {
        /* The PAL-entry/copy trigger belongs here, before the platform hook. */
        uint64_t imm = insn->unit == IA64_UNIT_B ? 0 : (uint64_t)op->immediate & 0x1fffff;
        if (imm == 0x100000) {
            if (insn->unit == IA64_UNIT_M && insn->slot == 0 &&
                ia64_pal_entry_matches(v, insn->address))
                return ia64_pal_break(v);
            /* Outside a PAL entry, this immediate raises the architectural fault. */
            return ia64_fault(IA64_FAULT_BREAK, imm);
        }
        if (ia64_break_hook(v, imm, insn->address))
            return ia64_complete();
        return ia64_fault(IA64_FAULT_BREAK, imm);
    }
    case IA64_OP_MOV_BRGR:
        ia64_gr_write(s, op->destination, s->br[op->source & 7], false);
        break;
    case IA64_OP_MOV_GRBR:
        value = ia64_gr_read(s, op->destination);
        if (ia64_nat_read(s, op->destination))
            return ia64_fault(IA64_FAULT_NAT, op->destination);
        s->br[op->source & 7] = value;
        break;
    case IA64_OP_MOV_PRGR:
        ia64_gr_write(s, op->destination, ia64_pr_pack(s), false);
        break;
    case IA64_OP_MOV_GRPR:
        value = ia64_gr_read(s, op->destination);
        if (ia64_nat_read(s, op->destination))
            return ia64_fault(IA64_FAULT_NAT, op->destination);
        ia64_pr_unpack(s, value, (uint64_t)op->immediate);
        break;
    case IA64_OP_MOV_PR_ROT_IMM:
        ia64_pr_unpack(s, (uint64_t)op->immediate, ~UINT64_C(0xffff));
        break;
    case IA64_OP_MOV_IP:
    case IA64_OP_MOV_CURRENT_IP:
        ia64_gr_write(s, op->destination, insn->address, false);
        break;
    case IA64_OP_CLRRRB:
        s->cfm &= ~((UINT64_C(127) << 18) | (UINT64_C(127) << 25) | (UINT64_C(63) << 32));
        break;
    case IA64_OP_CLRRRB_PR:
        s->cfm &= ~(UINT64_C(63) << 32);
        break;
    case IA64_OP_MOV_ARGR:
    case IA64_OP_MOV_GRAR:
    case IA64_OP_MOV_IMMAR: {
        unsigned ar = op->source;
        bool write = insn->opcode != IA64_OP_MOV_ARGR;
        if (illegal_ar(ar, insn->unit, write))
            return ia64_fault(IA64_FAULT_ILLEGAL, ar);
        if ((ar == AR_BSPSTORE || ar == AR_RNAT) && (s->ar[AR_RSC] & 3))
            return ia64_fault(IA64_FAULT_ILLEGAL, ar);
        /* SDM Vol.1 §6.2.2.5 permits full readback of reserved/ignored FP
         * environment fields. Vol.2 §10.3: saving IA-32 state in Itanium
         * mode has no IA-32 execution side effects. */
        if (write) {
            if (insn->opcode == IA64_OP_MOV_GRAR) {
                value = ia64_gr_read(s, op->destination);
                if (ia64_nat_read(s, op->destination))
                    return ia64_fault(IA64_FAULT_NAT, op->destination);
            } else
                value = (uint64_t)op->immediate;
            if ((ar == AR_RSC && (value & ~(UINT64_C(31) | (UINT64_C(0x3fff) << 16)))) ||
                (ar == AR_FPSR && reserved_fpsr(value)) || (ar == AR_PFS && reserved_pfs(value)) ||
                (ar == AR_CFLG && (value >> 43)))
                return ia64_fault(IA64_FAULT_RESERVED, value);
            unsigned cpl = (s->psr >> 32) & 3;
            if ((ar <= 7 || ar == AR_ITC || ar == AR_CFLG) && cpl)
                return ia64_fault(IA64_FAULT_PRIVILEGED_REG, ar);
            if (ignored_ar(ar))
                return ia64_complete();
            if (ar == AR_ITC) {
                ia64_itc_write(v, value);
                break;
            }
            if (ar == AR_EFLAG) {
                /* SDM Vol.2 §10.3.2, Table 10-3: Itanium writes of IOPL
                 * (13:12) and VM (17) take effect only at PL0, and of IF (9)
                 * only when PSR.cpl <= EFLAG.iopl; otherwise those fields are
                 * silently kept. Ignored (1, 3, 5, 15) and reserved (63:22)
                 * bits take the permitted option of storing the value and
                 * returning it, as the fork does; no fault or intercept. */
                uint64_t old = s->ar[AR_EFLAG], keep = 0;
                if (cpl)
                    keep |= (UINT64_C(3) << 12) | (UINT64_C(1) << 17);
                if (cpl > ((old >> 12) & 3))
                    keep |= UINT64_C(1) << 9;
                value = (value & ~keep) | (old & keep);
            }
            if (ar == AR_RSC && ((value >> 2) & 3) < cpl)
                value = (value & ~UINT64_C(12)) | ((uint64_t)cpl << 2);
            if (ar == AR_BSPSTORE) {
                value &= ~UINT64_C(7);
                s->ar[AR_BSP] = ia64_rse_advance(value, s->rse_dirty);
                s->ar[AR_RNAT] = 0;
            }
            /* SDM Vol.2 §10.3.3.1 Fig.10-3: CFLG reserved bits fault;
             * ignored CR0 fields read zero in this implementation. */
            if (ar == AR_CFLG)
                value &= UINT64_C(0x000007ffe00501ff);
            if (ar == AR_RNAT)
                value &= ~(UINT64_C(1) << 63);
            if (ar == AR_EC)
                value &= 63;
            s->ar[ar] = value;
        } else {
            if (ar == AR_ITC && (s->psr & IA64_PSR_SI) && ((s->psr >> 32) & 3))
                return ia64_fault(IA64_FAULT_PRIVILEGED_REG, ar);
            value = ignored_ar(ar) ? 0 : ar == AR_ITC ? ia64_itc_read(v) : s->ar[ar];
            ia64_gr_write(s, op->destination, value, false);
        }
        break;
    }
    default:
        return ia64_system_registers(v, insn);
    }
    return ia64_complete();
}
