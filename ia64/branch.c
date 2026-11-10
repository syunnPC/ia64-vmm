/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/semantics.h"
#include "ia64/rse.h"
#include "ia64/ia32/run.h"

struct ia64_result ia64_branch(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64BranchOperands *op = &insn->operands.branch;
    bool taken = true;
    uint64_t target = insn->address + (uint64_t)op->displacement;
    switch (insn->opcode) {
    case IA64_OP_BR_IA:
        /* SDM Vol.3 br: dirty RSE precedes DI, before any state commit.
         * Fork target/ia64/arch/rse.c:ia64_rse_br_ia. */
        if (s->ar[AR_BSPSTORE] != s->ar[AR_BSP])
            return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
        if ((s->psr & IA64_PSR_DI) || v->cfg.cpu_model == IA64_CPU_MONTECITO)
            return ia64_fault(IA64_FAULT_DISABLED_ISA, insn->raw);
        return ia32_enter(v, IA32_ENTRY_BRANCH, s->br[op->target]);
    case IA64_OP_BR_CALL:
    case IA64_OP_BRL_CALL:
        return ia64_rse_call(v, insn, target);
    case IA64_OP_BR_CALL_INDIRECT:
        return ia64_rse_call(v, insn, s->br[op->target] & ~UINT64_C(15));
    case IA64_OP_BR_RET:
        return ia64_rse_return(v, insn, s->br[op->target] & ~UINT64_C(15));
    case IA64_OP_BR_COND:
    case IA64_OP_BRL_COND:
        break;
    case IA64_OP_BR_INDIRECT:
        target = s->br[op->target] & ~UINT64_C(15);
        break;
    case IA64_OP_BR_CLOOP:
        taken = s->ar[AR_LC] != 0;
        if (taken)
            --s->ar[AR_LC];
        break;
    case IA64_OP_BR_CTOP:
    case IA64_OP_BR_CEXIT:
        taken = s->ar[AR_LC] != 0 || s->ar[AR_EC] > 1;
        if (s->ar[AR_LC]) {
            --s->ar[AR_LC];
            ia64_pr_write(s, 63, true);
            ia64_rotate(s);
        } else if (s->ar[AR_EC]) {
            --s->ar[AR_EC];
            ia64_pr_write(s, 63, false);
            ia64_rotate(s);
        } else {
            ia64_pr_write(s, 63, false);
        }
        if (insn->opcode == IA64_OP_BR_CEXIT)
            taken = !taken;
        break;
    case IA64_OP_BR_WTOP:
    case IA64_OP_BR_WEXIT: {
        bool qp = ia64_pr_read(s, insn->qp);
        taken = qp || s->ar[AR_EC] > 1;
        ia64_pr_write(s, 63, false);
        if (qp || s->ar[AR_EC]) {
            if (!qp)
                --s->ar[AR_EC];
            ia64_rotate(s);
        }
        if (insn->opcode == IA64_OP_BR_WEXIT)
            taken = !taken;
        break;
    }
    default:
        return ia64_unimplemented(insn);
    }
    if (!taken)
        return ia64_complete();
    return (struct ia64_result){.kind = IA64_STEP_BRANCH, .info = target};
}
