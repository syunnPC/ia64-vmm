/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/ia32/memory.h"
#include "ia64/mmu.h"
#include "ia64/tlb.h"
#include "ia64vmm/string.h"
#include "engine/cache/cache.h"
#include "engine/cache/validity.h"
#include "ia64vmm/hal.h"

#define PAGE_MASK (~UINT64_C(4095))
#define PTE_PPN ((UINT64_C(1) << 50) - 4096)

static struct ia64_result unsupported(uint32_t address, const char *detail)
{
    return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, address, detail);
}
static bool same_entry(const struct ia64_tlb_entry *a, const struct ia64_tlb_entry *b)
{
    return a && a->valid == b->valid && a->va == b->va && a->pte == b->pte && a->itir == b->itir &&
           a->rid == b->rid && a->page_shift == b->page_shift;
}
static gpa_t physical(const struct ia64_tlb_entry *entry, uint32_t address)
{
    uint64_t mask = ia64_low_mask(entry->page_shift);
    return ((entry->pte & PTE_PPN) & ~mask) | (address & mask);
}
static void remember(const struct ia64_state *s, const struct ia64_tlb_entry *entry,
                     struct ia32_memory_page *p)
{
    p->source = *entry;
    for (unsigned i = 0; i < IA64_ITR_COUNT; ++i)
        if (entry == &s->tr[p->side][i]) {
            p->tr = true;
            p->index = i;
            return;
        }
    for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i)
        if (entry == &s->tc[p->side][i]) {
            p->index = i;
            return;
        }
}
static const struct ia64_tlb_entry *source(const struct ia64_state *s,
                                           const struct ia32_memory_page *p)
{
    return p->tr ? &s->tr[p->side][p->index] : &s->tc[p->side][p->index];
}

static bool sources_current(const struct vcpu *v, const struct ia32_authorization *a)
{
    for (unsigned i = 0; i < a->count; ++i)
        if (!same_entry(source(&v->s, &a->page[i]), &a->page[i].source) ||
            ia64_tlb_find(&v->s, (uint32_t)a->page[i].mapping.token.value, a->page[i].side) !=
                source(&v->s, &a->page[i]))
            return false;
    return true;
}
bool ia32_memory_current(const struct vcpu *v, const struct ia32_authorization *a)
{
    return a->count && v->mmu_seq == a->mmu_seq && v->s.psr == a->psr && v->s.rr[0] == a->rr &&
           v->s.cr[CR_PTA] == a->pta && sources_current(v, a);
}

struct ia64_result ia32_memory_authorize(struct vcpu *v, struct ia32_access *access,
                                         struct ia32_authorization *out)
{
    *out = (struct ia32_authorization){0};
    access->gpa_valid = false;
    unsigned rights;
    switch (access->kind) {
    case IA32_ACCESS_FETCH:
        rights = IA64_ACCESS_EXECUTE;
        break;
    case IA32_ACCESS_READ:
    case IA32_ACCESS_DESCRIPTOR:
        rights = IA64_ACCESS_READ;
        break;
    case IA32_ACCESS_WRITE:
        rights = IA64_ACCESS_WRITE;
        break;
    case IA32_ACCESS_RMW:
        rights = IA64_ACCESS_READ | IA64_ACCESS_WRITE;
        break;
    default:
        return unsupported(access->linear.value, "unclassified native memory reference");
    }
    if (!access->linear_valid || !access->width_valid || !access->width)
        return unsupported(access->linear.value, "native memory reference lacks complete range");
    unsigned max_pages = access->kind == IA32_ACCESS_FETCH ? 2 : 8;
    if (((uint64_t)(access->linear.value & 4095) + access->width + 4095) / 4096 > max_pages)
        return unsupported(access->linear.value,
                           "native reference exceeds qualified translation resources");
    if ((v->s.psr & (IA64_PSR_IT | IA64_PSR_DT)) != (IA64_PSR_IT | IA64_PSR_DT) || v->s.rse_cfle)
        return unsupported(access->linear.value,
                           "native translation requires virtual I/D and completed RSE entry");
    struct ia32_authorization a = {0};
    uint32_t address = access->linear.value;
    unsigned remaining = access->width, level = (v->s.psr >> 32) & 3;
    bool fetch = access->kind == IA32_ACCESS_FETCH;
    gpa_t first = 0;
    while (remaining) {
        struct ia64_translation translation;
        struct ia64_result r = ia64_translate(v, address, fetch ? IA64_REF_FETCH : IA64_REF_DATA,
                                              rights, level, false, &translation);
        if (r.kind != IA64_STEP_COMPLETE) {
            access->linear.value = address;
            return r;
        }
        if (!translation.entry || translation.deferred || !translation.granted)
            return unsupported(address, "native translation has no lasting virtual source");
        unsigned lasting = fetch ? ((translation.pte & 32) ? IA64_ACCESS_EXECUTE : 0)
                                 : ia64_lasting_rights(&v->s, translation.entry, level);
        struct ia32_memory_page *p = &a.page[a.count++];
        p->gpa = translation.pa & PAGE_MASK;
        p->side = !fetch;
        remember(&v->s, translation.entry, p);
        const struct gm_region *region = ia64_physical_region(v, p->gpa, 4096);
        p->indirect = !region || (region->kind != GM_RAM && region->kind != GM_FIRMWARE);
        unsigned span = 4096 - (address & 4095);
        if (span > remaining)
            span = remaining;
        for (unsigned i = 0; i < span; ++i) {
            const struct gm_region *part = ia64_physical_region(v, translation.pa + i, 1);
            if (!part || (fetch && part->kind != GM_RAM && part->kind != GM_FIRMWARE) ||
                (part->kind == GM_EMULATED &&
                 (((rights & IA64_ACCESS_READ) && !part->ops->read) ||
                  ((rights & IA64_ACCESS_WRITE) && !part->ops->write))))
                return ia64_exit(VCPU_EXIT_BAD_ACCESS, translation.pa + i,
                                 "IA-32 operand backing is unavailable");
        }
        if (translation.ma != IA64_MA_WB && translation.ma != IA64_MA_UC &&
            translation.ma != IA64_MA_UCE && translation.ma != IA64_MA_WC)
            return unsupported(address, "native memory attribute is not representable");
        p->mapping = (struct ia32_mapping){
            .token = {address & PAGE_MASK},
            .rights = rights,
            .memory_type = translation.ma == IA64_MA_WB ? IA32_MEMORY_WB
                           : (translation.ma == IA64_MA_UC || translation.ma == IA64_MA_UCE)
                               ? IA32_MEMORY_UC
                               : IA32_MEMORY_WC};
        /* Neither EPT nor NPT represents a present write-only leaf. Add
         * read permission only when the same authoritative translation,
         * including its protection key, grants a lasting read lease. */
        if (!fetch && (rights & IA64_ACCESS_WRITE) && (lasting & IA64_ACCESS_READ))
            p->mapping.rights |= IA32_MAP_READ;
        /* NPT needs READ for EXEC. Authorize it only from an existing,
         * lasting D-side mapping of the same bytes and attribute. */
        if (fetch) {
            const struct ia64_tlb_entry *data = ia64_tlb_find(&v->s, address, 1);
            if (data && (data->pte & 1) && ((data->pte >> 2) & 7) == translation.ma &&
                physical(data, address) == translation.pa &&
                (ia64_lasting_rights(&v->s, data, level) & IA64_ACCESS_READ))
                p->mapping.rights |= IA32_MAP_READ;
        }
        if (a.count == 1)
            first = translation.pa;
        unsigned bytes = 4096 - (address & 4095);
        if (bytes > remaining)
            bytes = remaining;
        remaining -= bytes;
        address += bytes;
    }
    a.mmu_seq = v->mmu_seq;
    a.psr = v->s.psr;
    a.rr = v->s.rr[0];
    a.pta = v->s.cr[CR_PTA];
    /* A later walker fill may replace an earlier translation. */
    if (!ia32_memory_current(v, &a))
        return unsupported(access->linear.value,
                           "native reference translations did not remain co-resident");
    access->gpa = first;
    access->gpa_valid = true;
    *out = a;
    return ia64_complete();
}

struct ia64_result ia32_memory_authorize_ranges(struct vcpu *v, struct ia32_access *access,
                                                unsigned ranges, struct ia32_authorization *out)
{
    *out = (struct ia32_authorization){0};
    if (!ranges || ranges > 10)
        return unsupported((uint32_t)v->s.ip, "invalid native reference range count");
    struct ia32_authorization a = {0};
    unsigned pages[2] = {0};
    for (unsigned i = 0; i < ranges; ++i) {
        struct ia32_authorization part;
        struct ia64_result r = ia32_memory_authorize(v, &access[i], &part);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        for (unsigned j = 0; j < part.count; ++j) {
            struct ia32_memory_page *p = &part.page[j];
            bool duplicate = false;
            for (unsigned k = 0; k < a.count; ++k) {
                struct ia32_memory_page *q = &a.page[k];
                if (p->side == q->side && p->mapping.token.value == q->mapping.token.value &&
                    p->gpa == q->gpa && same_entry(&p->source, &q->source)) {
                    q->mapping.rights |= p->mapping.rights;
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;
            if (a.count == IA32_MEMORY_PAGES || ++pages[p->side] > (p->side ? 8u : 2u))
                return unsupported((uint32_t)p->mapping.token.value,
                                   "native instruction exceeds translation resources");
            a.page[a.count++] = *p;
        }
    }
    a.mmu_seq = v->mmu_seq;
    a.psr = v->s.psr;
    a.rr = v->s.rr[0];
    a.pta = v->s.cr[CR_PTA];
    if (!sources_current(v, &a))
        return unsupported((uint32_t)v->s.ip,
                           "native instruction translations did not remain co-resident");
    a.ranges = ranges;
    memcpy(a.access, access, ranges * sizeof(*access));
    *out = a;
    return ia64_complete();
}

struct ia64_result ia32_memory_fetch(struct vcpu *v, ia32_linear_t address, uint8_t *out,
                                     unsigned bytes)
{
    if (!bytes || bytes > 15)
        return unsupported(address.value, "invalid native instruction fetch width");
    for (unsigned i = 0; i < bytes; ++i) {
        struct ia32_access access = {.kind = IA32_ACCESS_FETCH,
                                     .linear_valid = true,
                                     .linear = {address.value + i},
                                     .width_valid = true,
                                     .width = 1};
        struct ia32_authorization a;
        struct ia64_result r = ia32_memory_authorize(v, &access, &a);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        if (code_validity_active()) {
            uint64_t low, high, generation;
            r = code_source_read_physical(access.gpa & ~UINT64_C(15), &low, &high, &generation);
            if (r.kind != IA64_STEP_COMPLETE)
                return r;
        }
        const struct gm_region *region = ia64_physical_region(v, access.gpa, 1);
        out[i] = region->hva[access.gpa - region->base];
    }
    return ia64_complete();
}
