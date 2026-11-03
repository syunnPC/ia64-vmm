/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/semantics.h"
#include "ia64/fp.h"
#include "ia64/tlb.h"
#include "engine/cache/cache.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/guest_mem.h"
#include "ia64/fault-count.h"

const char *ia64_fault_name(enum ia64_fault_kind kind)
{
    static const char *const names[] = {"none",
                                        "Illegal Operation",
                                        "Register NaT Consumption",
                                        "Reserved Register/Field",
                                        "Privileged Operation",
                                        "Break Instruction",
                                        "Unaligned Data Reference",
                                        "Unsupported Data Reference",
                                        "Unimplemented Data Address",
                                        "Alternate Data TLB",
                                        "Data Page Not Present",
                                        "Data NaT Page Consumption",
                                        "Privileged Register",
                                        "Data Nested TLB",
                                        "Data Key Miss",
                                        "Disabled Floating-point Register",
                                        "Floating-point Exception",
                                        "Floating-point Exception trap",
                                        "Alternate Instruction TLB",
                                        "Instruction TLB",
                                        "Data TLB",
                                        "VHPT Translation",
                                        "Key Permission",
                                        "Access Rights",
                                        "Access Bit",
                                        "Dirty Bit",
                                        "Instruction NaT Page Consumption",
                                        "Unimplemented Instruction Address",
                                        "Debug",
                                        "Lower-Privilege Transfer trap",
                                        "Taken Branch trap",
                                        "Single Step trap",
                                        "Disabled Instruction Set Transition",
                                        "Virtualization",
                                        "Machine Check"};
    return (unsigned)kind < IA64VMM_ARRAY_SIZE(names) ? names[kind] : "unknown fault";
}
bool ia64_set_trace(struct vcpu *v, unsigned count)
{
    if (count > 65536)
        return false;
    struct ia64_trace_entry *p = NULL;
    if (count) {
        p = hal_alloc_pages(((size_t)count * sizeof(*p) + 4095) / 4096, HAL_PAGE_ZERO);
        if (!p)
            return false;
    }
    if (v->trace)
        hal_free_pages(v->trace, ((size_t)v->trace_count * sizeof(*p) + 4095) / 4096);
    v->trace = p;
    v->trace_count = count;
    v->trace_next = v->trace_used = 0;
    return true;
}
void ia64_trace_record(struct vcpu *v, const Ia64Instruction *insn)
{
    if (!v->trace_count)
        return;
    v->trace[v->trace_next] =
        (struct ia64_trace_entry){insn->address, insn->raw, insn->slot, insn->opcode};
    v->trace_next = (v->trace_next + 1) % v->trace_count;
    if (v->trace_used < v->trace_count)
        ++v->trace_used;
}
void ia64_trace_dump(const struct vcpu *v)
{
    for (unsigned i = 0; i < v->trace_used; ++i) {
        unsigned at = (v->trace_next + v->trace_count - v->trace_used + i) % v->trace_count;
        const struct ia64_trace_entry *t = &v->trace[at];
        LOGI("trace %016llx:%u %-20s raw=%011llx", (unsigned long long)t->ip, t->slot,
             ia64_opcode_info(t->opcode)->name, (unsigned long long)t->raw);
    }
}
static uint64_t break_ip[4], break_mem;
static unsigned break_count[4];
unsigned ia64_break_ips;
void vcpu_set_break_ip(uint64_t ip, unsigned count)
{
    if (ia64_break_ips < 4) {
        break_ip[ia64_break_ips] = ip;
        break_count[ia64_break_ips++] = count;
    }
}
void vcpu_set_break_mem(uint64_t pa)
{
    break_mem = pa;
}
void ia64_break_check(struct vcpu *v)
{
    const struct ia64_state *s = &v->s;
    for (unsigned b = 0; b < ia64_break_ips; ++b)
        if (break_count[b] && s->ip == break_ip[b] && s->slot == 0) {
            --break_count[b];
            LOGW("break_ip: vCPU%u at %llx (%u more) r8=%llx", v->cfg.index,
                 (unsigned long long)s->ip, break_count[b], (unsigned long long)ia64_gr_read(s, 8));
            vcpu_dump(v);
            uint64_t word;
            if (break_mem && gm_read(break_mem, &word, sizeof(word)))
                LOGW("break_mem %llx: %016llx", (unsigned long long)break_mem,
                     (unsigned long long)word);
        }
}
void vcpu_dump(const struct vcpu *v)
{
    const struct ia64_state *s = &v->s;
    LOGI("vcpu%u IP=%016llx:%u PSR=%016llx CFM=%016llx retired=%llu", v->cfg.index,
         (unsigned long long)s->ip, s->slot, (unsigned long long)s->psr, (unsigned long long)s->cfm,
         (unsigned long long)v->retired);
    for (unsigned i = 1; i < 128; ++i) {
        uint64_t value = ia64_gr_read(s, i);
        bool nat = ia64_nat_read(s, i);
        if (value || nat)
            LOGI(" r%u=%016llx NaT=%u", i, (unsigned long long)value, nat);
    }
    for (unsigned i = 1; i < 128; ++i) {
        struct ia64_fr value = ia64_fr_read(s, i);
        if (value.significand || value.exponent || value.sign)
            LOGI(" f%u=%u:%05x:%016llx", i, value.sign, value.exponent,
                 (unsigned long long)value.significand);
    }
    LOGI("PR=%016llx (physical, rrb.pr=%u)", (unsigned long long)s->pr,
         (unsigned)((s->cfm >> 32) & 63));
    for (unsigned i = 0; i < 8; ++i)
        LOGI(" b%u=%016llx", i, (unsigned long long)s->br[i]);
    static const unsigned ars[] = {0,      1,       2,       3,      4,           5,
                                   6,      7,       AR_RSC,  AR_BSP, AR_BSPSTORE, AR_RNAT,
                                   AR_CCV, AR_UNAT, AR_FPSR, AR_PFS, AR_LC,       AR_EC};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(ars); ++i)
        LOGI(" ar%u=%016llx", ars[i], (unsigned long long)s->ar[ars[i]]);
    LOGI(" ar.itc=%016llx", (unsigned long long)ia64_itc_read(v));
    static const unsigned crs[] = {CR_DCR, CR_IVA, CR_PTA, CR_IPSR, CR_ISR, CR_ITIR,
                                   CR_IIP, CR_IFA, CR_IFS, CR_IIM,  CR_ITM, CR_ITV};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(crs); ++i)
        LOGI(" cr%u=%016llx", crs[i], (unsigned long long)s->cr[crs[i]]);
    for (unsigned i = 0; i < 8; ++i)
        LOGI(" rr%u=%016llx", i, (unsigned long long)s->rr[i]);
    LOGI("RSE sof=%u sol=%u sor=%u rrb.gr=%u rrb.fr=%u bof=%u dirty=%u", (unsigned)(s->cfm & 127),
         (unsigned)((s->cfm >> 7) & 127), (unsigned)(((s->cfm >> 14) & 15) * 8),
         (unsigned)((s->cfm >> 18) & 127), (unsigned)((s->cfm >> 25) & 127), s->rse_bof,
         s->rse_dirty);
    if (v->fault.kind)
        LOGI("last %s fault at %016llx:%u raw=%011llx info=%016llx committed=%u",
             ia64_fault_name(v->fault.kind), (unsigned long long)v->fault.ip, v->fault.slot,
             (unsigned long long)v->fault.raw, (unsigned long long)v->fault.info,
             v->fault.effect_committed);
    ia64_instruction_dump(v);
    ia64_trace_dump(v);
}
void ia64_instruction_dump(const struct vcpu *v)
{
    const struct ia64_state *s = &v->s;
    uint8_t bytes[16];
    gpa_t gpa = s->ip & ~(UINT64_C(1) << 63);
    bool mapped = true;
    if (s->psr & IA64_PSR_IT) {
        /* A diagnostic must not walk a VHPT, insert translations or read
         * device registers. Use the resident instruction mapping only
         * (SDM Vol.2 §4.1 PTE PPN and page-offset composition). */
        const struct ia64_tlb_entry *entry = ia64_tlb_find(s, s->ip, 0);
        mapped = entry && (entry->pte & 1);
        if (mapped) {
            uint64_t offset = ia64_low_mask(entry->page_shift);
            gpa = (entry->pte & UINT64_C(0x0003fffffffff000) & ~offset) | (s->ip & offset);
        }
    }
    const struct gm_region *r = gm_lookup(gpa);
    if (mapped && r && r->kind != GM_DIRECT_MMIO && r->kind != GM_EMULATED &&
        gm_read(gpa, bytes, sizeof(bytes))) {
        uint64_t low = 0, high = 0;
        for (unsigned i = 0; i < 8; ++i) {
            low |= (uint64_t)bytes[i] << (i * 8);
            high |= (uint64_t)bytes[i + 8] << (i * 8);
        }
        struct ia64_bundle b;
        ia64_decode_bundle(low, high, s->ip, &b);
        LOGI("bundle %016llx %016llx template=%02x %s", (unsigned long long)low,
             (unsigned long long)high, b.template_code, ia64_template_info(b.template_code)->name);
        if (s->slot < 3) {
            const Ia64Instruction *insn = &b.insn[s->slot];
            LOGI("instruction %s raw=%011llx qp=p%u operands=%u,%u,%u imm=%016llx",
                 ia64_opcode_info(insn->opcode)->name, (unsigned long long)insn->raw, insn->qp,
                 insn->operands.common.destination, insn->operands.common.source1,
                 insn->operands.common.source2,
                 (unsigned long long)insn->operands.common.immediate);
        }
    }
}
