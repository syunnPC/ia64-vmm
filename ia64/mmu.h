/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_MMU_H
#define IA64VMM_IA64_MMU_H
#include "ia64/semantics.h"

enum ia64_reference {
    IA64_REF_DATA,
    IA64_REF_FETCH,
    IA64_REF_RSE,
    IA64_REF_TPA,
    IA64_REF_TAK,
    IA64_REF_PROBE,
    IA64_REF_PROBE_FAULT,
    IA64_REF_FC,
};
enum ia64_access_right { IA64_ACCESS_READ = 1, IA64_ACCESS_WRITE = 2, IA64_ACCESS_EXECUTE = 4 };
enum ia64_memory_attribute {
    IA64_MA_WB = 0,
    IA64_MA_UC = 4,
    IA64_MA_UCE = 5,
    IA64_MA_WC = 6,
    IA64_MA_NAT = 7
};
struct ia64_translation {
    gpa_t pa;
    uint64_t pte;
    unsigned ma;
    bool deferred, granted;
    /* Vol.2 Table 5-3: an unimplemented address precludes Debug even
     * when deferred; a deferred translation/access fault does not. */
    bool debug_precluded;
    /* The TR or TC entry the virtual translation used (NULL for physical
     * addressing and before the entry is found); valid until the next TLB
     * write. hostwin's one access check per miss. */
    const struct ia64_tlb_entry *entry;
};
/* Translation does not perform the original access or change guest A/D bits.
 * Query references search the DTLB even with PSR.dt=0 (SDM Vol.3 probe/tpa). */
struct ia64_result ia64_translate(struct vcpu *v, uint64_t address, enum ia64_reference reference,
                                  unsigned rights, unsigned level, bool speculative,
                                  struct ia64_translation *out);
/* Reset derived translations after replacing architectural state (snapshots). */
void ia64_mmu_cache_reset(struct vcpu *v);
bool ia64_defer_fault(struct vcpu *v, enum ia64_fault_kind fault);
/* IA64_ACCESS_READ/WRITE that a data access through entry at
 * privilege level would be granted with PSR.da = 0, i.e. without the
 * one-shot Dirty/Access-bit suppression: key (PSR.pk and the PKRs),
 * PTE.ar/pl, PTE.a, and PTE.d for writes. The entry is present and not
 * NaTPage (the access check passed). Never walks, never faults. */
unsigned ia64_lasting_rights(const struct ia64_state *s, const struct ia64_tlb_entry *entry,
                             unsigned level);
static inline bool ia64_ma_speculative(unsigned ma)
{
    return ma == IA64_MA_WB || ma == IA64_MA_WC;
}
#endif
