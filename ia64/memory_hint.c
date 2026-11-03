/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/memory.h"
#include "ia64/mmu.h"
#include "ia64/debug.h"

struct ia64_result ia64_memory_hint(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64MemoryOperands *op = &insn->operands.memory;
    bool flush = insn->opcode == IA64_OP_FC || insn->opcode == IA64_OP_FC_I;
    bool faulting = flush || insn->opcode == IA64_OP_LFETCH_FAULT;
    bool update = insn->reg_base_update || insn->imm_base_update;
    if (update && (!op->base || (op->base >= 32 && op->base - 32u >= (s->cfm & 127))))
        return ia64_fault(IA64_FAULT_ILLEGAL, op->base);
    bool qualify = faulting && (flush || !(s->psr & IA64_PSR_ED));
    if (!update && !qualify)
        return ia64_complete();
    struct ia64_gr_value base = ia64_gr_read_value(s, op->base);
    uint64_t address = base.value;
    bool nat = base.nat;
    gpa_t gpa = address & ~(UINT64_C(1) << 63);
    /* SDM Vol.3 lfetch: no memory transaction, no alignment fault; ed
     * suppresses the faulting form too. Postincrement always propagates NaT. */
    if (qualify) {
        if (nat)
            return ia64_fault(IA64_FAULT_NAT, address);
        struct ia64_translation translated;
        struct ia64_result r =
            ia64_translate(v, address, flush ? IA64_REF_FC : IA64_REF_DATA, IA64_ACCESS_READ,
                           (s->psr >> 32) & 3, false, &translated);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        if (!flush) {
            r = ia64_debug_data(v, address, 1, IA64_ACCESS_READ, (s->psr >> 32) & 3, false, false);
            if (r.kind != IA64_STEP_COMPLETE) {
                r.fault_isr |= (UINT64_C(1) << 35) | 4;
                return r;
            }
        }
        gpa = translated.pa;
    }
    if (flush) {
        /* Montecito has 128-byte cache lines. Both forms may conservatively
         * invalidate decoded instructions; neither reads MMIO. */
        code_notify_cpu_write(gpa & ~UINT64_C(127), 128);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    if (update) {
        struct ia64_gr_value inc = insn->reg_base_update
                                       ? ia64_gr_read_value(s, op->source)
                                       : (struct ia64_gr_value){.value = (uint64_t)op->immediate};
        ia64_gr_write(s, op->base, address + inc.value, nat || inc.nat);
    }
    return ia64_complete();
}
