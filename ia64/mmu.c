/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/mmu.h"
#include "ia64/tlb.h"
#include "ia64/memory.h"
#include "ia64/profile.h"
#include "ia64/debug.h"
#include "ia64/rse.h"
#include "ia64/smp.h"

#define VA_PAYLOAD ((UINT64_C(1) << 61) - 1)
#define PTE_PPN ((UINT64_C(1) << 50) - 4096)

static bool implemented(uint64_t address)
{
    uint64_t hole = VA_PAYLOAD & ~ia64_low_mask(IA64_IMPL_VA_MSB + 1);
    return (address & hole) == ((address & (UINT64_C(1) << IA64_IMPL_VA_MSB)) ? hole : 0);
}
static struct ia64_result fault(enum ia64_fault_kind kind, uint64_t address,
                                enum ia64_reference reference, unsigned rights, bool speculative)
{
    struct ia64_result r = ia64_fault(kind, address);
    r.translation_fault = true;
    if (rights & IA64_ACCESS_EXECUTE)
        r.fault_isr = UINT64_C(1) << 32;
    else {
        if (rights & IA64_ACCESS_READ)
            r.fault_isr |= UINT64_C(1) << 34;
        if (rights & IA64_ACCESS_WRITE)
            r.fault_isr |= UINT64_C(1) << 33;
        if (speculative)
            r.fault_isr |= UINT64_C(1) << 36;
        if (reference == IA64_REF_RSE)
            r.fault_isr |= UINT64_C(1) << 37;
    }
    return r;
}
bool ia64_defer_fault(struct vcpu *v, enum ia64_fault_kind kind)
{
    /* SDM Rev.2.3 Vol.2 Table 5-4. A deferred key fault does not preclude
     * a concurrent, non-deferred access-rights or access-bit fault. */
    if (kind == IA64_FAULT_UNIMPL_DATA || kind == IA64_FAULT_DATA_NAT ||
        kind == IA64_FAULT_UNSUPPORTED_DATA || kind == IA64_FAULT_NAT)
        return true;
    if (!(v->s.psr & IA64_PSR_IC))
        return true;
    if (!(v->s.psr & IA64_PSR_IT))
        return false;
    const struct ia64_tlb_entry *code = ia64_tlb_lookup(v, v->s.ip, 0);
    if (!code || !(code->pte & (UINT64_C(1) << 52)))
        return false;
    unsigned bit;
    switch (kind) {
    case IA64_FAULT_UNALIGNED:
        return true;
    case IA64_FAULT_ALT_DTLB:
    case IA64_FAULT_NESTED_DTLB:
    case IA64_FAULT_DTLB:
    case IA64_FAULT_VHPT:
        bit = 8;
        break;
    case IA64_FAULT_PAGE_NOT_PRESENT:
        bit = 9;
        break;
    case IA64_FAULT_KEY_MISS:
        bit = 10;
        break;
    case IA64_FAULT_KEY_PERMISSION:
        bit = 11;
        break;
    case IA64_FAULT_ACCESS_RIGHTS:
        bit = 12;
        break;
    case IA64_FAULT_ACCESS_BIT:
        bit = 13;
        break;
    case IA64_FAULT_DEBUG:
        bit = 14;
        break;
    default:
        return false;
    }
    return (v->s.cr[CR_DCR] >> bit) & 1;
}
static enum ia64_fault_kind key_fault(const struct ia64_state *s,
                                      const struct ia64_tlb_entry *entry, unsigned rights)
{
    if (!(s->psr & IA64_PSR_PK))
        return IA64_FAULT_NONE;
    uint64_t key = entry->itir & UINT64_C(0xffffff00);
    for (unsigned i = 0; i < 16; ++i) {
        uint64_t pkr = s->pkr[i];
        if (!(pkr & 1) || (pkr & UINT64_C(0xffffff00)) != key)
            continue;
        unsigned disable = ((rights & 1) ? 4 : 0) | ((rights & 2) ? 2 : 0) | ((rights & 4) ? 8 : 0);
        return (pkr & disable) ? IA64_FAULT_KEY_PERMISSION : IA64_FAULT_NONE;
    }
    return IA64_FAULT_KEY_MISS;
}
static gpa_t physical(const struct ia64_tlb_entry *entry, uint64_t address)
{
    return (entry->pte & PTE_PPN & ~ia64_low_mask(entry->page_shift)) |
           (address & ia64_low_mask(entry->page_shift));
}
static bool valid_pte(uint64_t pte)
{
    unsigned ma = (pte >> 2) & 7;
    return !(pte & 1) || (!(pte & ((UINT64_C(3) << 50) | 2)) && (!ma || ma >= 4));
}
static const struct ia64_tlb_entry *micro_find(struct vcpu *v, uint64_t address, unsigned side);
static enum ia64_fault_kind walk(struct vcpu *v, uint64_t address, unsigned side,
                                 const struct ia64_tlb_entry **result, bool tak)
{
    struct ia64_state *s = &v->s;
    uint64_t pta = s->cr[CR_PTA], rr = s->rr[address >> 61];
    unsigned size = (pta >> 2) & 63, ps = (rr >> 2) & 63;
    bool long_format = pta & 0x100;
    enum ia64_fault_kind miss = side ? IA64_FAULT_DTLB : IA64_FAULT_ITLB;
    if (size < 15 || size > (long_format ? 61u : 52u) || !ia64_page_size_supported(ps))
        return miss;
    uint64_t hash = ia64_thash(s, address);
    const struct ia64_tlb_entry *mapping = micro_find(v, hash, 1);
    if (!implemented(hash) || !mapping)
        return IA64_FAULT_VHPT;
    /* Vol.2 §4.1.7: walker references are CPL0, WB only; a fault other
     * than a translation miss aborts the walk and raises the original miss. */
    if (!(mapping->pte & 1) || ((mapping->pte >> 2) & 7) != IA64_MA_WB ||
        key_fault(s, mapping, IA64_ACCESS_READ) ||
        !(ia64_tlb_permissions(mapping->pte, 0) & IA64_ACCESS_READ) ||
        (!(mapping->pte & 32) && !(s->psr & IA64_PSR_DA)))
        return miss;
    if (!tak && (s->psr & IA64_PSR_DB) && !(s->psr & IA64_PSR_DD) &&
        ia64_debug_match(s, hash, long_format ? 32 : 8, IA64_ACCESS_READ, 0))
        return miss;
    gpa_t pa = physical(mapping, hash);
    const struct gm_region *region = ia64_physical_region(v, pa, long_format ? 32 : 8);
    if (!region || (region->kind != GM_RAM && region->kind != GM_FIRMWARE))
        return miss;
    uint64_t pte, itir = rr & UINT64_C(0xfffffffc), tag;
    if (long_format) {
        /* SDM Vol.2 §4.1.7: a second snapshot detects
         * changes during an SMP walk. Software must still follow the ti
         * protocol; two equal snapshots cannot validate a torn update. */
        if (ia64_physical_read(v, pa + 16, 8, &tag).kind != IA64_STEP_COMPLETE ||
            ia64_physical_read(v, pa, 8, &pte).kind != IA64_STEP_COMPLETE ||
            ia64_physical_read(v, pa + 8, 8, &itir).kind != IA64_STEP_COMPLETE)
            return miss;
        if (ia64_smp_active()) {
            uint64_t tag2, pte2, itir2;
            if (ia64_physical_read(v, pa + 16, 8, &tag2).kind != IA64_STEP_COMPLETE ||
                ia64_physical_read(v, pa, 8, &pte2).kind != IA64_STEP_COMPLETE ||
                ia64_physical_read(v, pa + 8, 8, &itir2).kind != IA64_STEP_COMPLETE ||
                tag != tag2 || pte != pte2 || itir != itir2)
                return miss;
        }
        if (s->cr[CR_DCR] & 2) {
            itir = __builtin_bswap64(itir);
            tag = __builtin_bswap64(tag);
        }
        if ((tag >> 63) || tag != ia64_ttag(s, address))
            return miss;
    } else if (ia64_physical_read(v, pa, 8, &pte).kind != IA64_STEP_COMPLETE)
        return miss;
    if (s->cr[CR_DCR] & 2)
        pte = __builtin_bswap64(pte);
    if (!valid_pte(pte) || (itir & 3) || ((pte & 1) && (itir >> 32)) ||
        !ia64_page_size_supported((itir >> 2) & 63))
        return miss;
    if (!ia64_vhpt_insert(v, address, side, pte, itir))
        return IA64_FAULT_MACHINE_CHECK;
    /* The walk began on a miss and has just published this TC slot. Use
     * it directly; lookup caches consume its log record on the next access. */
    *result = &s->tc[side][(s->tc_next[side] - 1) % ia64_model_tc_count(v->cfg.cpu_model, side)];
    return IA64_FAULT_NONE;
}
void ia64_mmu_cache_reset(struct vcpu *v)
{
    v->microtlb = (struct ia64_microtlb){.seq = v->mmu_seq};
    v->tlb_index.ready = false;
}
static const struct ia64_tlb_entry *micro_find(struct vcpu *v, uint64_t address, unsigned side)
{
    struct ia64_microtlb *m = &v->microtlb;
    /* Only entry locations are cached. PSR access checks run below on
     * every reference; PTA affects the walker on a miss, never this lookup. */
    if (v->mmu_seq - m->seq > IA64_MMU_LOG)
        ia64_mmu_cache_reset(v);
    else if (m->seq != v->mmu_seq) {
        unsigned sides = 0;
        for (uint64_t seq = m->seq; seq != v->mmu_seq; ++seq) {
            struct ia64_mmu_change c = v->mmu_log[seq % IA64_MMU_LOG];
            sides |= c.kind == IA64_MMU_TC || c.kind == IA64_MMU_TR ? 1u << c.side : 3u;
        }
        for (unsigned d = 0; d < 2; ++d)
            if (sides & (1u << d))
                for (unsigned i = 0; i < IA64_MICROTLB_ENTRIES; ++i)
                    m->entry[d][i].valid = false;
        m->seq = v->mmu_seq;
    }
    uint64_t page = address & ~UINT64_C(4095);
    struct ia64_microtlb_entry *e = &m->entry[side][(address >> 12) & (IA64_MICROTLB_ENTRIES - 1)];
    if (e->valid && e->page == page) {
        const struct ia64_tlb_entry *hit =
            e->tr ? &v->s.tr[side][e->index] : &v->s.tc[side][e->index];
        if (hit->valid && hit->rid == ((v->s.rr[address >> 61] >> 8) & 0xffffff) &&
            !((hit->va ^ (address & ia64_low_mask(61))) & ~ia64_low_mask(hit->page_shift)))
            return hit;
        e->valid = false;
    }
    const struct ia64_tlb_entry *found = ia64_tlb_lookup(v, address, side);
    if (found) {
        /* Search priority stays in tlb.c. Only successful lookups bind;
         * misses must continue to run the VHPT walker/fault machinery. */
        bool tr = false;
        unsigned index = 0;
        /* Both arrays are contiguous within s; compare integer addresses
         * rather than subtract pointers from different arrays. */
        uintptr_t location = (uintptr_t)found, base = (uintptr_t)v->s.tr[side];
        if (location >= base && location < base + sizeof(v->s.tr[side])) {
            tr = true;
            index = (unsigned)((location - base) / sizeof(*found));
        }
        if (!tr)
            index = (unsigned)(found - v->s.tc[side]);
        *e = (struct ia64_microtlb_entry){.page = page, .index = index, .tr = tr, .valid = true};
    }
    return found;
}
struct ia64_result ia64_translate(struct vcpu *v, uint64_t address, enum ia64_reference reference,
                                  unsigned rights, unsigned level, bool speculative,
                                  struct ia64_translation *out)
{
    struct ia64_state *s = &v->s;
    *out = (struct ia64_translation){.granted = true};
    if (reference == IA64_REF_FETCH && s->rse_cfle) {
        struct ia64_result r = ia64_rse_resume(v);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        /* A mandatory-load interrupt or deferred branch trap can change
         * the stream before either engine translates the instruction. */
        address = s->ip;
        level = (s->psr >> 32) & 3;
    }
    bool fetch = reference == IA64_REF_FETCH, rse = reference == IA64_REF_RSE;
    bool query = reference == IA64_REF_TPA || reference == IA64_REF_TAK;
    bool flush = reference == IA64_REF_FC;
    bool probe = reference == IA64_REF_PROBE;
    bool virt = query || probe ||
                (s->psr & (fetch ? IA64_PSR_IT
                           : rse ? IA64_PSR_RT
                                 : IA64_PSR_DT));
    if (!virt) {
        out->pa = address & ~(UINT64_C(1) << 63);
        out->ma = address >> 63 ? IA64_MA_UC : IA64_MA_WB;
        if (!(out->pa >> (IA64_IMPL_PA_MSB + 1)))
            return ia64_complete();
        if (speculative) {
            out->deferred = out->debug_precluded = true;
            return ia64_complete();
        }
        return fault(fetch ? IA64_FAULT_UNIMPL_INST : IA64_FAULT_UNIMPL_DATA, address, reference,
                     rights, speculative);
    }
    enum ia64_fault_kind kind = IA64_FAULT_NONE;
    const struct ia64_tlb_entry *entry = NULL;
    if (!implemented(address))
        kind = fetch ? IA64_FAULT_UNIMPL_INST : IA64_FAULT_UNIMPL_DATA;
    else {
        entry = micro_find(v, address, fetch ? 0 : 1);
        if (!entry) {
            bool enabled = (s->cr[CR_PTA] & 1) && (s->rr[address >> 61] & 1) &&
                           (s->psr & IA64_PSR_DT) && (!fetch || (s->psr & IA64_PSR_IC));
            kind = enabled ? walk(v, address, fetch ? 0 : 1, &entry, reference == IA64_REF_TAK)
                   : fetch ? IA64_FAULT_ALT_ITLB
                           : IA64_FAULT_ALT_DTLB;
            if (kind == IA64_FAULT_MACHINE_CHECK)
                return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, address,
                                 "VHPT insert overlaps TR; MCA delivery unavailable");
            if (kind && !fetch && !(s->psr & IA64_PSR_IC))
                kind = IA64_FAULT_NESTED_DTLB;
        }
    }
    if (kind) {
        out->debug_precluded = kind == IA64_FAULT_UNIMPL_DATA;
        if (reference == IA64_REF_TAK || (probe && kind == IA64_FAULT_UNIMPL_DATA)) {
            out->granted = false;
            return ia64_complete();
        }
        if (speculative && ia64_defer_fault(v, kind)) {
            out->deferred = true;
            return ia64_complete();
        }
        return fault(kind, address, reference, rights, speculative);
    }
    out->pa = physical(entry, address);
    out->pte = entry->pte;
    out->ma = (entry->pte >> 2) & 7;
    out->entry = entry;
    if (reference == IA64_REF_TAK) {
        out->granted = entry->pte & 1;
        return ia64_complete();
    }
    if (!(entry->pte & 1))
        kind = IA64_FAULT_PAGE_NOT_PRESENT;
    else if (out->ma == IA64_MA_NAT)
        kind = fetch ? IA64_FAULT_INST_NAT : IA64_FAULT_DATA_NAT;
    if (kind) {
        bool nat_page = kind == IA64_FAULT_DATA_NAT || kind == IA64_FAULT_INST_NAT;
        if (speculative && ia64_defer_fault(v, kind)) {
            out->deferred = true;
            /* Table 5-3: a NaT Page precludes no key or rights fault. */
            if (!nat_page)
                return ia64_complete();
        } else
            return fault(kind, address, reference, rights, speculative);
    }
    if (query)
        return ia64_complete();
    if (flush) {
        if (level && !(ia64_tlb_permissions(entry->pte, level) & IA64_ACCESS_READ))
            return fault(IA64_FAULT_ACCESS_RIGHTS, address, reference, rights, false);
        return ia64_complete();
    }
    enum ia64_fault_kind checks[4] = {
        key_fault(s, entry, rights),
        (ia64_tlb_permissions(entry->pte, level) & rights) == rights ? IA64_FAULT_NONE
                                                                     : IA64_FAULT_ACCESS_RIGHTS,
        (!fetch && (rights & IA64_ACCESS_WRITE) && !(entry->pte & 64) && !(s->psr & IA64_PSR_DA))
            ? IA64_FAULT_DIRTY_BIT
            : IA64_FAULT_NONE,
        (!(entry->pte & 32) && !(s->psr & (fetch ? IA64_PSR_IA : IA64_PSR_DA)))
            ? IA64_FAULT_ACCESS_BIT
            : IA64_FAULT_NONE,
    };
    /* Vol.2 Table 5-6 is authoritative: Dirty Bit precedes Access Bit. */
    for (unsigned i = 0; i < 4; ++i) {
        kind = checks[i];
        if (!kind)
            continue;
        if (probe && i >= 2)
            continue;
        if (probe && kind != IA64_FAULT_KEY_MISS) {
            out->granted = false;
            continue;
        }
        if (speculative && ia64_defer_fault(v, kind)) {
            out->deferred = true;
            continue;
        }
        return fault(kind, address, reference, rights, speculative);
    }
    return ia64_complete();
}
unsigned ia64_lasting_rights(const struct ia64_state *s, const struct ia64_tlb_entry *entry,
                             unsigned level)
{
    /* The data checks of ia64_translate for this entry with PSR.da = 0:
     * key, rights, then Dirty Bit (writes) and Access Bit. PSR.da covers
     * only the first restart instruction (SDM Vol.2 Table 3-2), so rights
     * that exist only through it last for one access. Pure: no walk, no
     * fault. */
    unsigned granted = ia64_tlb_permissions(entry->pte, level), lasting = 0;
    if (!(entry->pte & 32))
        return 0;
    if ((granted & IA64_ACCESS_READ) && !key_fault(s, entry, IA64_ACCESS_READ))
        lasting |= IA64_ACCESS_READ;
    if ((granted & IA64_ACCESS_WRITE) && (entry->pte & 64) &&
        !key_fault(s, entry, IA64_ACCESS_WRITE))
        lasting |= IA64_ACCESS_WRITE;
    return lasting;
}
