/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/tlb.h"
#include "ia64/mmu.h"
#include "ia64/debug.h"
#include "ia64/profile.h"
#include "ia64/mmu_log.h"
#include "ia64/smp.h"
#include "ia64vmm/log.h"
#include "ia64vmm/platform.h"

#define VA_PAYLOAD ((UINT64_C(1) << 61) - 1)
static unsigned rid(const struct ia64_state *s, uint64_t address)
{
    return (s->rr[address >> 61] >> 8) & 0xffffff;
}
static bool matches(const struct ia64_tlb_entry *entry, uint64_t address, unsigned identifier,
                    unsigned shift)
{
    unsigned max = entry->page_shift > shift ? entry->page_shift : shift;
    return entry->valid && entry->rid == identifier &&
           !((entry->va ^ (address & VA_PAYLOAD)) & ~ia64_low_mask(max));
}
const struct ia64_tlb_entry *ia64_tlb_find(const struct ia64_state *s, uint64_t address,
                                           unsigned side)
{
    unsigned identifier = rid(s, address);
    for (unsigned i = 0; i < ia64_model_tr_count(s->cpu_model, side); ++i)
        if (matches(&s->tr[side][i], address, identifier, 0))
            return &s->tr[side][i];
    for (unsigned i = 0; i < ia64_model_tc_count(s->cpu_model, side); ++i)
        if (matches(&s->tc[side][i], address, identifier, 0))
            return &s->tc[side][i];
    return NULL;
}
/* A Tier0 lockstep shadow replays the instruction; overlaps are logged once. */
extern bool tier0_is_shadow(const struct vcpu *) __attribute__((weak));
static bool shadow(const struct vcpu *v)
{
    return tier0_is_shadow && tier0_is_shadow(v);
}
/* Every purge goes through here: the MMU log (mmu_log.h) records the
 * valid -> invalid transition. */
static void purge(struct vcpu *v, enum ia64_mmu_writer writer, bool tr, unsigned side, unsigned i)
{
    struct ia64_tlb_entry *e = tr ? &v->s.tr[side][i] : &v->s.tc[side][i];
    if (!e->valid)
        return;
    e->valid = false;
    ia64_mmu_logged(v, writer, tr ? IA64_MMU_TR : IA64_MMU_TC, side, i);
}
void ia64_tc_insert(struct vcpu *v, uint64_t address, unsigned side, uint64_t pte, uint64_t itir)
{
    struct ia64_state *s = &v->s;
    unsigned ps = (itir >> 2) & 63, identifier = rid(s, address);
    for (unsigned i = 0; i < ia64_model_tc_count(s->cpu_model, side); ++i)
        if (matches(&s->tc[side][i], address, identifier, ps))
            purge(v, IA64_MMU_W_TC_INSERT_PURGE, false, side, i);
    /* Deterministic FIFO round robin, separately on each side. Purges do
     * not rewind the cursor; replacement and walker fills share it. */
    unsigned index = s->tc_next[side]++ % ia64_model_tc_count(s->cpu_model, side);
    s->tc[side][index] = (struct ia64_tlb_entry){.va = address & VA_PAYLOAD & ~ia64_low_mask(ps),
                                                 .rid = identifier,
                                                 .pte = pte,
                                                 .itir = itir,
                                                 .page_shift = ps,
                                                 .valid = true};
    ia64_mmu_logged(v, IA64_MMU_W_TC_INSERT, IA64_MMU_TC, side, index);
}
bool ia64_vhpt_insert(struct vcpu *v, uint64_t address, unsigned side, uint64_t pte, uint64_t itir)
{
    struct ia64_state *s = &v->s;
    unsigned ps = (itir >> 2) & 63, identifier = rid(s, address);
    /* SDM Vol.2 Table 4-2, 248699-014 A-1008: without VRN matching a
     * same-stream TR overlap must machine check. Keep the TR and insert
     * the TC entry (both permitted). Footnote h allows the MCA after the
     * reference: take it at the next boundary unless PSR.mc holds it. */
    for (unsigned i = 0; i < ia64_model_tr_count(s->cpu_model, side); ++i) {
        const struct ia64_tlb_entry *pinned = &s->tr[side][i];
        if (!matches(pinned, address, identifier, ps))
            continue;
        if (!shadow(v))
            vmm_log(v->cfg.pale_check ? LOG_WARN : LOG_ERROR,
                    "VHPT overlap: %s va=%llx ps=%u rid=%x; TR%u va=%llx ps=%u",
                    side ? "DTC" : "ITC", (unsigned long long)(address & VA_PAYLOAD), ps,
                    identifier, i, (unsigned long long)pinned->va, pinned->page_shift);
        if (!v->cfg.pale_check)
            return false;
        v->pal_mca_pending = IA64VMM_PAL_EVENT_MCA | IA64VMM_PAL_EVENT_TLB;
        break;
    }
    ia64_tc_insert(v, address, side, pte, itir);
    return true;
}
/* Remote TC purges leave TRs untouched (SDM Vol.2 Table 4-1), including
 * the conservative whole-TC purge used when the SMP log overruns. */
void ia64_tlb_remote_purge(struct vcpu *v, uint32_t identifier, uint64_t address, unsigned shift,
                           bool all)
{
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned i = 0; i < ia64_model_tc_count(v->cfg.cpu_model, side); ++i)
            if (all || matches(&v->s.tc[side][i], address, identifier, shift))
                purge(v, IA64_MMU_W_PTC_G_REMOTE, false, side, i);
}
uint64_t ia64_thash(const struct ia64_state *s, uint64_t address)
{
    unsigned size = (s->cr[CR_PTA] >> 2) & 63, ps = (s->rr[address >> 61] >> 2) & 63;
    if (size < 15 || size > ((s->cr[CR_PTA] & 0x100) ? 61u : 52u))
        return address;
    if (ps < 12)
        ps = 12;
    uint64_t hpn = (address & VA_PAYLOAD) >> ps, mask = ia64_low_mask(size);
    uint64_t base = s->cr[CR_PTA] & ~UINT64_C(0x7fff);
    /* SDM Vol.2 §4.1.5: short format is architectural. Long format uses
     * the fork's implementation-specific hash/tag pair (PAL hash_tag_id=8). */
    if (s->cr[CR_PTA] & 0x100)
        return (base & ~mask) | (((hpn ^ rid(s, address)) << 5) & mask);
    return (address & ~VA_PAYLOAD) | ((base & VA_PAYLOAD & ~mask) | ((hpn << 3) & mask));
}
uint64_t ia64_ttag(const struct ia64_state *s, uint64_t address)
{
    unsigned ps = (s->rr[address >> 61] >> 2) & 63;
    if (ps < 12)
        ps = 12;
    return ((address & VA_PAYLOAD) >> ps) ^ ((uint64_t)rid(s, address) << 39);
}
unsigned ia64_tlb_permissions(uint64_t pte, unsigned level)
{
    /* Access-rights table follows target/ia64/arch/mmu.c in the GPL fork;
     * SDM Vol.2 §4.1.1, Table 4-3 defines its privilege-dependent forms. */
    unsigned ar = (pte >> 9) & 7, pl = (pte >> 7) & 3;
    if (ar == 7)
        return level ? 4 : 5;
    if (level > pl)
        return 0;
    switch (ar) {
    case 0:
        return 1;
    case 1:
        return 5;
    case 2:
        return 3;
    case 3:
        return 7;
    case 4:
        return !level || level < pl ? 3 : 1;
    case 5:
        return level ? 5 : 7;
    default:
        return level && level == pl ? 7 : 3;
    }
}
static struct ia64_result query(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    bool probe = insn->opcode == IA64_OP_PROBE_R || insn->opcode == IA64_OP_PROBE_W ||
                 insn->opcode == IA64_OP_PROBE_RW;
    unsigned source = probe ? insn->operands.memory.base : insn->operands.system.register_index;
    unsigned destination = insn->operands.common.destination;
    if (probe && !insn->probe_fault &&
        (!destination || insn->opcode == IA64_OP_PROBE_RW ||
         (destination >= 32 && destination - 32u >= (s->cfm & 127))))
        return ia64_fault(IA64_FAULT_ILLEGAL, destination);
    uint64_t address = ia64_gr_read(s, source), value = 0;
    bool nat = ia64_nat_read(s, source);
    if (insn->opcode == IA64_OP_THASH || insn->opcode == IA64_OP_TTAG) {
        if (insn->opcode == IA64_OP_THASH)
            value = ia64_thash(s, address);
        else
            value = ia64_ttag(s, address);
        ia64_gr_write(s, destination, value, nat);
        return ia64_complete();
    }
    if (nat)
        return ia64_fault(IA64_FAULT_NAT, address);
    unsigned level = (s->psr >> 32) & 3;
    if (probe) {
        unsigned requested = insn->probe_imm ? insn->operands.memory.immediate & 3
                                             : ia64_gr_read(s, insn->operands.memory.source) & 3;
        if (!insn->probe_imm && ia64_nat_read(s, insn->operands.memory.source))
            return ia64_fault(IA64_FAULT_NAT, address);
        if (requested > level)
            level = requested;
    }
    enum ia64_reference reference = insn->opcode == IA64_OP_TAK ? IA64_REF_TAK
                                    : probe
                                        ? insn->probe_fault ? IA64_REF_PROBE_FAULT : IA64_REF_PROBE
                                        : IA64_REF_TPA;
    unsigned needed = !probe                            ? 0
                      : insn->opcode == IA64_OP_PROBE_R ? IA64_ACCESS_READ
                      : insn->opcode == IA64_OP_PROBE_W ? IA64_ACCESS_WRITE
                                                        : IA64_ACCESS_READ | IA64_ACCESS_WRITE;
    struct ia64_translation translated;
    struct ia64_result r = ia64_translate(v, address, reference, needed, level, false, &translated);
    if (r.kind != IA64_STEP_COMPLETE)
        return r;
    if (probe && insn->probe_fault) {
        r = ia64_debug_data(v, address, 1, needed, level, false, false);
        if (r.kind != IA64_STEP_COMPLETE)
            r.fault_isr |= (UINT64_C(1) << 35) | 5;
        return r;
    }
    if (insn->opcode == IA64_OP_TAK) {
        const struct ia64_tlb_entry *entry = ia64_tlb_find(s, address, 1);
        value = translated.granted && entry ? entry->itir & UINT64_C(0xffffff00) : 1;
    } else
        value = probe ? translated.granted : translated.pa;
    ia64_gr_write(s, destination, value, false);
    return ia64_complete();
}
struct ia64_result ia64_tlb(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64SystemOperands *op = &insn->operands.system;
    bool probe = insn->opcode == IA64_OP_PROBE_R || insn->opcode == IA64_OP_PROBE_W ||
                 insn->opcode == IA64_OP_PROBE_RW;
    bool hash = insn->opcode == IA64_OP_THASH || insn->opcode == IA64_OP_TTAG;
    if (!probe && !hash && ((s->psr >> 32) & 3))
        return ia64_fault(IA64_FAULT_PRIVILEGED, insn->raw);
    if (probe || insn->opcode == IA64_OP_TAK || insn->opcode == IA64_OP_TPA ||
        insn->opcode == IA64_OP_THASH || insn->opcode == IA64_OP_TTAG)
        return query(v, insn);
    bool tr = insn->opcode == IA64_OP_ITR_D || insn->opcode == IA64_OP_ITR_I;
    bool insert = tr || insn->opcode == IA64_OP_ITC_D || insn->opcode == IA64_OP_ITC_I;
    bool ptr = insn->opcode == IA64_OP_PTR_D || insn->opcode == IA64_OP_PTR_I;
    bool all = insn->opcode == IA64_OP_PTC_E;
    if (!insert && !ptr && !all && insn->opcode != IA64_OP_PTC_L && insn->opcode != IA64_OP_PTC_G &&
        insn->opcode != IA64_OP_PTC_GA)
        return ia64_unimplemented(insn);
    unsigned side = insn->opcode == IA64_OP_ITR_D || insn->opcode == IA64_OP_ITC_D ||
                    insn->opcode == IA64_OP_PTR_D;
    uint64_t source = ia64_gr_read(s, op->source), address = ia64_gr_read(s, op->register_index);
    if ((!all && ia64_nat_read(s, op->source)) ||
        ((tr || ptr || !insert) && ia64_nat_read(s, op->register_index)))
        return ia64_fault(IA64_FAULT_NAT, address);
    if (insert) {
        /* SDM Vol.3 itr: only r3{7:0} selects the TR slot. */
        unsigned slot = address & 0xff;
        unsigned ps = (s->cr[CR_ITIR] >> 2) & 63;
        unsigned ma = (source >> 2) & 7;
        if ((tr && slot >= ia64_model_tr_count(s->cpu_model, side)) ||
            !ia64_page_size_supported(ps) || (s->cr[CR_ITIR] & 3) ||
            ((source & 1) && ((source & ((UINT64_C(3) << 50) | 2)) || (s->cr[CR_ITIR] >> 32) ||
                              (ma > 0 && ma < 4))))
            return ia64_fault(IA64_FAULT_RESERVED, source);
        uint64_t va = s->cr[CR_IFA] & VA_PAYLOAD & ~ia64_low_mask(ps);
        unsigned identifier = rid(s, s->cr[CR_IFA]);
        /* SDM Rev 2.3 Vol.2 §4.1.1.1 and Table 4-1: both ITC and
         * ITR must machine-check on a same-stream TR overlap, including
         * the slot being replaced; insert and purge are "may", and this
         * CPU does neither. The machine check precedes the insert; with
         * PSR.mc = 1 it is held and the insert completes without effect
         * (§11.3.1). Without a PALE_CHECK the vCPU stops before mutation. */
        for (unsigned i = 0; i < ia64_model_tr_count(s->cpu_model, side); ++i) {
            const struct ia64_tlb_entry *old = &s->tr[side][i];
            if (!matches(old, va, identifier, ps))
                continue;
            if (!shadow(v))
                vmm_log(
                    v->cfg.pale_check ? LOG_WARN : LOG_ERROR,
                    "TLB overlap: %s%u va=%llx ps=%u rid=%x pte=%llx; %s%u va=%llx ps=%u rid=%x pte=%llx",
                    tr ? (side ? "DTR" : "ITR") : (side ? "DTC" : "ITC"),
                    tr ? slot : s->tc_next[side] % ia64_model_tc_count(s->cpu_model, side),
                    (unsigned long long)va, ps, identifier, (unsigned long long)source,
                    side ? "DTR" : "ITR", i, (unsigned long long)old->va, old->page_shift, old->rid,
                    (unsigned long long)old->pte);
            if (!v->cfg.pale_check)
                return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, s->cr[CR_IFA],
                                 "TLB insert overlaps TR; MCA delivery unavailable");
            if (!(s->psr & IA64_PSR_MC))
                return ia64_fault(IA64_FAULT_MACHINE_CHECK, IA64VMM_PAL_EVENT_TLB);
            v->pal_mca_pending = IA64VMM_PAL_EVENT_MCA | IA64VMM_PAL_EVENT_TLB;
            return ia64_complete();
        }
        for (unsigned i = 0; i < ia64_model_tc_count(s->cpu_model, side); ++i)
            if (matches(&s->tc[side][i], va, identifier, ps))
                purge(v, IA64_MMU_W_INSERT_PURGE, false, side, i);
        unsigned index = tr ? slot : s->tc_next[side]++ % ia64_model_tc_count(s->cpu_model, side);
        struct ia64_tlb_entry *entry = tr ? &s->tr[side][index] : &s->tc[side][index];
        *entry = (struct ia64_tlb_entry){.va = va,
                                         .rid = identifier,
                                         .pte = source,
                                         .itir = s->cr[CR_ITIR],
                                         .page_shift = ps,
                                         .valid = true};
        ia64_mmu_logged(v, tr ? IA64_MMU_W_ITR : IA64_MMU_W_ITC, tr ? IA64_MMU_TR : IA64_MMU_TC,
                        side, index);
        return ia64_complete();
    }
    unsigned shift = (source >> 2) & 63, identifier = rid(s, address);
    address &= ~ia64_low_mask(shift);
    /* SDM Rev 2.3 Vol.2 Table 4-1 and §5.2.2.2.3: ptc.l and the local part
     * of ptc.g/ptc.ga must machine-check when they overlap a TR of either
     * stream; they still purge the TCs and broadcast, and leave the TR
     * (§4.1.1.2). The machine check may follow the purge up to the next
     * instruction serialization (footnote h): it is latched when the
     * instruction completes and taken at the next boundary unless PSR.mc
     * holds it; a machine check already pending keeps its cause. Without a
     * PALE_CHECK the vCPU stops before any effect. */
    bool machine_check = false;
    for (unsigned d = 0; d < 2 && !ptr && !all && !machine_check; ++d)
        for (unsigned i = 0; i < ia64_model_tr_count(s->cpu_model, d); ++i) {
            const struct ia64_tlb_entry *pinned = &s->tr[d][i];
            if (!matches(pinned, address, identifier, shift))
                continue;
            if (!shadow(v))
                vmm_log(
                    v->cfg.pale_check ? LOG_WARN : LOG_ERROR,
                    "TLB purge overlap: %s va=%llx ps=%u rid=%x; %s%u va=%llx ps=%u rid=%x pte=%llx",
                    insn->opcode == IA64_OP_PTC_L   ? "ptc.l"
                    : insn->opcode == IA64_OP_PTC_G ? "ptc.g"
                                                    : "ptc.ga",
                    (unsigned long long)(address & VA_PAYLOAD), shift, identifier,
                    d ? "DTR" : "ITR", i, (unsigned long long)pinned->va, pinned->page_shift,
                    pinned->rid, (unsigned long long)pinned->pte);
            if (!v->cfg.pale_check)
                return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, address,
                                 "TLB purge overlaps TR; MCA delivery unavailable");
            machine_check = true;
            break;
        }
    /* Local part of the coherence operation. Immediate completion is permitted by
     * SDM Vol.2 §3.2; srlz need not defer bookkeeping to a later boundary. */
    enum ia64_mmu_writer writer = ptr                             ? IA64_MMU_W_PTR_TC
                                  : all                           ? IA64_MMU_W_PTC_E
                                  : insn->opcode == IA64_OP_PTC_L ? IA64_MMU_W_PTC_L
                                  : insn->opcode == IA64_OP_PTC_G ? IA64_MMU_W_PTC_G
                                                                  : IA64_MMU_W_PTC_GA;
    for (unsigned d = 0; d < 2; ++d) {
        if (ptr && d != side)
            continue;
        for (unsigned i = 0; i < ia64_model_tc_count(s->cpu_model, d); ++i)
            if (all || matches(&s->tc[d][i], address, identifier, shift))
                purge(v, writer, false, d, i);
        if (ptr)
            for (unsigned i = 0; i < ia64_model_tr_count(s->cpu_model, d); ++i)
                if (matches(&s->tr[d][i], address, identifier, shift))
                    purge(v, IA64_MMU_W_PTR_TR, true, d, i);
    }
    if (insn->opcode == IA64_OP_PTC_G || insn->opcode == IA64_OP_PTC_GA)
        if (!ia64_smp_replay_purges(v))
            ia64_smp_global_purge(v, identifier, address, shift, insn->opcode == IA64_OP_PTC_GA);
    if (machine_check && !v->pal_mca_pending)
        v->pal_mca_pending =
            IA64VMM_PAL_EVENT_MCA | IA64VMM_PAL_EVENT_TLB | IA64VMM_PAL_EVENT_TLB_PURGE;
    return ia64_complete();
}
