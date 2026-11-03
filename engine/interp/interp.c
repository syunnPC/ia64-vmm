/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/cache/coherence-inline.h"
#include "engine/interp/interp.h"
#include "engine/cache/cache.h"
#include "engine/tier0/tier0.h"
#include "ia64/interrupt.h"
#include "ia64/mmu.h"
#include "ia64/debug.h"
#include "ia64/fault-count.h"
#include "ia64/pmu.h"

static struct vcpu_exit exit_at(const struct vcpu *v, enum vcpu_exit_reason reason, uint64_t info,
                                const char *detail)
{
    return (struct vcpu_exit){reason, v->s.ip, v->s.slot, info, detail};
}

/* Unlike Tier0, the interpreter checks at every instruction and promises a
 * one-instruction kick latency (kick-delivery). The locked exchange is a full
 * barrier, which orders this vCPU's earlier retirement before the check; a
 * plain load may be satisfied first under store-load reordering (Intel SDM
 * Vol.3A §9.2.3.4), so a kick raised while the previous instruction looked
 * unretired was seen one instruction late. */
static unsigned take_notification(unsigned *word)
{
    return __atomic_exchange_n(word, 0, __ATOMIC_ACQ_REL);
}

static struct vcpu_exit run(struct vcpu *v, uint64_t budget, bool fallback)
{
    struct ia64_state *s = &v->s;
    while (budget) {
        if (!tier0_is_shadow(v))
            code_quiescent_fast(v);
        if (ia64_break_ips)
            ia64_break_check(v);
        unsigned requested = take_notification(&v->requested_exit);
        if (requested)
            return exit_at(v, requested - 1, 0, "requested exit");
        /* Tier0-owned fallback may stop before retirement, leaving the
         * kick to its outer checkpoint. Direct and checked stepping keep
         * their requested retirement boundary. */
        if (v->cfg.engine == ENGINE_INTERP ? take_notification(&v->irq_kick)
                                           : (fallback && !v->check_context &&
                                              __atomic_load_n(&v->irq_kick, __ATOMIC_ACQUIRE)))
            return exit_at(v, VCPU_EXIT_BUDGET, 0, "interrupt mailbox kick");
        if (s->psr & IA64_PSR_IS)
            return exit_at(v, VCPU_EXIT_UNIMPLEMENTED, s->psr, "psr.is: IA-32 not built");
        if (s->slot > 2)
            return exit_at(v, VCPU_EXIT_BAD_ACCESS, s->slot, "invalid restart slot");
        ia64_timer_poll(v);
        if (ia64_external_deliver(v))
            continue;
        const struct ia64_bundle *bundle;
        struct ia64_translation translated;
        struct ia64_result result = ia64_translate(v, s->ip, IA64_REF_FETCH, IA64_ACCESS_EXECUTE,
                                                   (s->psr >> 32) & 3, false, &translated);
        /* Skip the aggregate-return helper when instruction breakpoints
         * are disabled or suppressed for this instruction. Fetch faults
         * still have priority over enabled instruction breakpoints. */
        if (result.kind == IA64_STEP_COMPLETE &&
            (s->psr & (IA64_PSR_DB | IA64_PSR_ID)) == IA64_PSR_DB)
            result = ia64_debug_instruction(v);
        if (result.kind == IA64_STEP_FAULT) {
            v->fault =
                (struct ia64_fault_site){result.fault, s->ip, 0, result.info, s->slot, false};
            Ia64Instruction fetch = {.opcode = IA64_OP_NOP, .address = s->ip, .slot = s->slot};
            ia64_deliver_fault(v, &fetch, result);
            continue;
        }
        if (result.kind == IA64_STEP_EXIT)
            return exit_at(v, result.exit, result.info, result.detail);
        /* The decoded-bundle cache is keyed by physical address. Every fetch
         * rechecks the current RR/TR/TC/PKR state before accessing that cache. */
        result = code_page_enter(v, translated.pa, false);
        if (result.kind == IA64_STEP_EXIT)
            return exit_at(v, result.exit, result.info, result.detail);
        result = code_get_bundle(v, translated.pa, &bundle);
        if (result.kind == IA64_STEP_EXIT)
            return exit_at(v, result.exit, result.info, result.detail);
        unsigned step_slots = bundle->long_instruction && s->slot == 1 ? 2 : 1;
        Ia64Instruction insn = bundle->insn[s->slot];
        uint64_t old_psr = s->psr;
        uint64_t suppression =
            old_psr & (IA64_PSR_DA | IA64_PSR_DD | IA64_PSR_IA | IA64_PSR_ED | IA64_PSR_ID);
        /* The cache is keyed by GPA; the executing alias owns the IP in
         * FaultSite/branch/mov-IP semantics, not the cache's source key. */
        insn.address = s->ip;
        struct ia64_pmu_retirement pmu;
        result = ia64_pmu_prepare(v, &insn, &pmu);
        if (result.kind == IA64_STEP_EXIT)
            return exit_at(v, result.exit, result.info, result.detail);
        if (!ia64_template_info(bundle->template_code)->defined ||
            (bundle->long_instruction && s->slot == 2))
            result = ia64_fault(IA64_FAULT_ILLEGAL, insn.raw);
        else
            result = ia64_execute(v, &insn);
        if (v->trace_count)
            ia64_trace_record(v, &insn);
        if (result.kind == IA64_STEP_FAULT) {
            bool committed = result.fault == IA64_FAULT_FP_TRAP;
            v->fault = (struct ia64_fault_site){result.fault, s->ip,   insn.raw,
                                                result.info,  s->slot, committed};
            if (committed) {
                /* A completing trap (FP trap): the instruction retired, so
                 * IIP/IPSR.ri name the next instruction while ISR.ei and IIPA
                 * name the trapping one (SDM Vol.2 §5.1.1). */
                if (old_psr & IA64_PSR_IC)
                    v->last_successful_bundle = insn.address;
                /* Vol.2 §5.5: retirement consumes suppression before traps
                 * collect IPSR, so it cannot affect the following instruction. */
                s->psr &= ~suppression;
                ++v->retired;
                if (pmu.count)
                    ia64_pmu_retire(v, pmu);
                --budget;
                s->slot += step_slots;
                if (s->slot == 3) {
                    s->slot = 0;
                    s->ip += 16;
                }
            }
            if (committed)
                result = ia64_completion_trap(v, &insn, old_psr, false, result);
            ia64_deliver_fault(v, &insn, result);
            continue;
        }
        if (result.kind == IA64_STEP_EXIT)
            return exit_at(v, result.exit, result.info, result.detail);
        if (old_psr & IA64_PSR_IC)
            v->last_successful_bundle = insn.address;
        if (insn.opcode != IA64_OP_RFI)
            s->psr &= ~suppression;
        ++v->retired;
        if (pmu.count)
            ia64_pmu_retire(v, pmu);
        --budget;
        if (result.kind == IA64_STEP_BRANCH) {
            s->ip = result.info;
            s->slot = result.next_slot;
        } else {
            s->slot += step_slots;
            if (s->slot == 3) {
                s->slot = 0;
                s->ip += 16;
            }
        }
        /* Normal retirement needs no completion helper with traps disabled.
         * A deferred br.ret fill is an exception even without trap enables:
         * it must retain DA/DD for the first mandatory RSE reference. */
        bool branch = result.kind == IA64_STEP_BRANCH;
        if ((old_psr & IA64_PSR_SS) || (branch && ((old_psr & (IA64_PSR_LP | IA64_PSR_TB)) ||
                                                   (s->rse_completion_pending && s->rse_cfle)))) {
            struct ia64_result trap =
                ia64_completion_trap(v, &insn, old_psr, branch, ia64_complete());
            if (trap.kind == IA64_STEP_FAULT) {
                v->fault = (struct ia64_fault_site){trap.fault, insn.address, insn.raw,
                                                    trap.info,  insn.slot,    true};
                ia64_deliver_fault(v, &insn, trap);
            }
        }
        requested = take_notification(&v->requested_exit);
        if (requested) {
            struct vcpu_exit out = {requested - 1, insn.address, insn.slot, 0, "requested exit"};
            return out;
        }
    }
    return exit_at(v, VCPU_EXIT_BUDGET, 0, NULL);
}

struct vcpu_exit interp_run(struct vcpu *v, uint64_t budget)
{
    return run(v, budget, false);
}

struct vcpu_exit interp_run_fallback(struct vcpu *v, uint64_t budget)
{
    return run(v, budget, true);
}
