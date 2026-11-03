/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Port of target/ia64/arch/pal.c from the GPL-2.0-or-later IA-64 QEMU
 * fork, commit 5f67e29d0fb6bea1f3bbae86d2af2674036f1b9c. Argument validation,
 * PAL encodings and the two-bundle copy portal follow that implementation.
 * CPU geometry and clocks describe this VMM; no QEMU runtime is imported. */
#include "ia64/pal.h"
#include "ia64/rse.h"
#include "ia64vmm/hal.h"
#include "ia64/memory.h"
#include "ia64/profile.h"
#include "ia64/mmu.h"
#include "ia64/tlb.h"
#include "ia64vmm/platform.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#define PAL_OK 0
#define PAL_UNIMPLEMENTED (-1)
#define PAL_INVALID (-2)
#define PAL_ERROR (-3)
#define PAL_NO_INFO (-6)
#define PAL_BEYOND_MAX (-8)
#define UC (UINT64_C(1) << 63)
#define PA_LIMIT (UINT64_C(1) << (IA64_IMPL_PA_MSB + 1))
#define RATIO(n, d) (((uint64_t)(n) << 32) | (d))

static bool physical_range(uint64_t address, uint64_t size)
{
    uint64_t pa = address & ~UC;
    return size <= PA_LIMIT && pa < PA_LIMIT && pa <= PA_LIMIT - size;
}

bool ia64_pal_entry_matches(const struct vcpu *v, uint64_t ip)
{
    /* Physical UC aliases execute the same portal, not a new copy. */
    uint64_t pa = ip & ~UC;
    if (v->s.psr & IA64_PSR_IT) {
        const struct ia64_tlb_entry *entry = ia64_tlb_find(&v->s, ip, 0);
        if (!entry || !(entry->pte & 1))
            return false;
        pa = (entry->pte & (PA_LIMIT - 4096) & ~ia64_low_mask(entry->page_shift)) |
             (ip & ia64_low_mask(entry->page_shift));
    }
    return (v->cfg.pal_entry && pa == v->cfg.pal_entry) ||
           (v->s.pal.copy_valid && pa == v->s.pal.copy_addr);
}

static struct ia64_result buffer_address(struct vcpu *v, uint64_t address, bool physical,
                                         bool write, gpa_t *pa)
{
    if (physical) {
        *pa = address & ~UC;
        return ia64_complete();
    }
    struct ia64_translation translated;
    struct ia64_result r =
        ia64_translate(v, address, IA64_REF_DATA, write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ,
                       (v->s.psr >> 32) & 3, false, &translated);
    if (r.kind == IA64_STEP_COMPLETE)
        *pa = translated.pa;
    return r;
}
/* Validate the full virtual range before output writes, including page and
 * physical-region boundaries. Physical PAL pointers bypass the data MMU. */
static struct ia64_result buffer_check(struct vcpu *v, uint64_t address, uint64_t size,
                                       unsigned alignment, bool physical, bool write)
{
    if (address & (alignment - 1))
        return ia64_fault(IA64_FAULT_UNALIGNED, address);
    if ((physical || !(v->s.psr & IA64_PSR_DT)) && !physical_range(address, size))
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, address, "PAL buffer outside physical width");
    while (size) {
        gpa_t pa;
        struct ia64_result result = buffer_address(v, address, physical, write, &pa);
        if (result.kind != IA64_STEP_COMPLETE)
            return result;
        const struct gm_region *r = ia64_physical_region(v, pa, 1);
        if (!r || (r->kind == GM_EMULATED && (write ? !r->ops->write : !r->ops->read)))
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, pa, "unmapped PAL buffer");
        uint64_t span = r->size - (pa - r->base);
        if (!physical && (v->s.psr & IA64_PSR_DT) && span > 4096 - (address & 4095))
            span = 4096 - (address & 4095);
        if (span > size)
            span = size;
        /* A multi-byte device access must remain a single bus transaction. */
        if ((pa & (alignment - 1)) || (span < size && (span & (alignment - 1))))
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, pa, "split PAL device access");
        address += span;
        size -= span;
    }
    return ia64_complete();
}

static struct ia64_result write_words(struct vcpu *v, uint64_t address, const uint64_t *words,
                                      unsigned count, bool physical)
{
    struct ia64_result r = buffer_check(v, address, count * 8u, 8, physical, true);
    if (r.kind != IA64_STEP_COMPLETE)
        return r;
    for (unsigned i = 0; i < count; ++i) {
        gpa_t pa;
        r = buffer_address(v, address + i * 8u, physical, true, &pa);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        r = ia64_physical_write(v, pa, 8, words[i]);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
    }
    return ia64_complete();
}

struct cache_info {
    bool unified;
    unsigned attribute, ways, line, stride, store_latency, load_latency, tag_lsb, size;
};
static bool cache_info(unsigned model, uint64_t level, uint64_t type, struct cache_info *c)
{
    if (level >= 3 || type < 1 || type > 2 || (level == 2 && type != 2) ||
        (model == IA64_CPU_MADISON && level == 1 && type != 2))
        return false;
    *c = (struct cache_info){.ways = 4,
                             .line = 6,
                             .stride = 6,
                             .store_latency = type == 1 ? 255 : 1,
                             .load_latency = 1,
                             .tag_lsb = 12,
                             .size = 16 * 1024};
    if (level == 1) {
        c->unified = model == IA64_CPU_MADISON;
        c->attribute = type == 1 ? 0 : 1;
        c->ways = 8;
        c->line = c->stride = 7;
        c->load_latency = type == 1 ? 7 : 5;
        c->tag_lsb = type == 1 ? 17 : 15;
        c->size = type == 1 ? 1024 * 1024 : 256 * 1024;
    } else if (level == 2) {
        c->unified = true;
        c->attribute = 1;
        c->ways = 12;
        c->line = c->stride = 7;
        c->load_latency = 14;
        c->tag_lsb = model == IA64_CPU_MADISON ? 18 : 20;
        c->size = ia64_model_l3_size(model);
    }
    return true;
}

/* SDM Table 11-112 defines no IA-32-disable bit. Madison set 16 has
 * no qualified implementation-specific controls; PSR.di gates transitions. */
static int feature_status(unsigned model, uint64_t set)
{
    if (model == IA64_CPU_MADISON)
        return !set || set == 16 ? PAL_OK : set < 16 ? PAL_INVALID : PAL_BEYOND_MAX;
    if (!set || set == 18)
        return PAL_OK;
    if (set < 16)
        return PAL_INVALID;
    return set < 18 ? 1 : PAL_BEYOND_MAX;
}

bool ia64_pal_tr_snapshot(const struct vcpu *v, uint64_t index, uint64_t side, uint64_t words[4],
                          uint64_t *valid)
{
    *valid = 0;
    for (unsigned i = 0; i < 4; ++i)
        words[i] = 0;
    if (side > 1 || index >= ia64_model_tr_count(v->cfg.cpu_model, side))
        return false;
    const struct ia64_tlb_entry *tr = &v->s.tr[side][index];
    if (tr->valid) {
        words[0] = tr->pte;
        words[1] = tr->itir;
        words[2] = tr->va | 1;
        words[3] = ((uint64_t)tr->rid << 8) | ((uint64_t)tr->page_shift << 2);
        *valid = 15;
    }
    return true;
}

void ia64_pal_tr_select(struct vcpu *v, uint64_t selector)
{
    if (!v)
        return;
    v->pal_tr_selector = selector;
    v->pal_tr_valid = 0;
    v->pal_tr_status = -2;
}
void ia64_pal_tr_read(struct vcpu *v, uint64_t buffer)
{
    if (!v)
        return;
    v->pal_tr_buffer = buffer;
    v->pal_tr_pending = true;
    vcpu_request_exit(v, VCPU_EXIT_BUDGET);
}
/* SDM Vol.2 Figure 11-2 and §11.3.3. Capture precedes collection, bank
 * switching and every PAL scratch-register write. X-resources retain the
 * interrupted handler's first interruption state when PSR.ic is clear. */
void ia64_pal_capture_state(struct vcpu *v)
{
    const struct ia64_state *s = &v->s;
    uint64_t *m = v->pal_min_state;
    m[0] = 0;
    for (unsigned i = 1; i < 16; ++i) {
        m[i] = s->gr[i];
        m[0] |= (uint64_t)s->nat[i] << i;
    }
    bool bank = s->psr & IA64_PSR_BN;
    for (unsigned i = 0; i < 16; ++i) {
        m[16 + i] = bank ? s->bank_gr[i] : s->gr[16 + i];
        m[32 + i] = bank ? s->gr[16 + i] : s->bank_gr[i];
        m[0] |= (uint64_t)(bank ? s->bank_nat[i] : s->nat[16 + i]) << (16 + i);
        m[0] |= (uint64_t)(bank ? s->nat[16 + i] : s->bank_nat[i]) << (32 + i);
    }
    m[48] = ia64_pr_pack(s);
    m[49] = s->br[0];
    m[50] = s->ar[AR_RSC];
    m[51] = s->ip;
    m[52] = (s->psr & ~(UINT64_C(3) << 41)) | ((uint64_t)s->slot << 41);
    m[53] = (UINT64_C(1) << 63) | s->cfm;
    m[54] = s->psr & IA64_PSR_IC ? m[51] : s->cr[CR_IIP];
    m[55] = s->psr & IA64_PSR_IC ? m[52] : s->cr[CR_IPSR];
    m[56] = s->psr & IA64_PSR_IC ? m[53] : s->cr[CR_IFS];
    m[57] = s->br[1];
    /* PAL-private words below SAL's 0x200 start: flags and original
     * missing prefix, followed by the deferred branch completion site. */
    m[58] = s->rse_cfle | ((uint64_t)s->rse_completion_pending << 1) |
            ((uint64_t)s->rse_completion_demoted << 2) | ((uint64_t)s->rse_missing << 8);
    m[59] = s->rse_completion_psr;
    m[60] = s->rse_completion_ip;
    m[61] = s->rse_completion_slot;
    v->pal_state_valid = true;
}

static bool state_area(uint64_t address, unsigned count)
{
    if (!(address & UC) || (address & 511) || !physical_range(address, 4096))
        return false;
    for (unsigned i = 0; i < count; ++i) {
        uint64_t pa = (address & ~UC) + i * 8;
        const struct gm_region *r = gm_lookup(pa);
        if (!r || (r->kind != GM_RAM && r->kind != GM_FIRMWARE) || 8 > r->size - (pa - r->base))
            return false;
    }
    return true;
}
static bool state_psr(uint64_t psr)
{
    return !(psr >> 46) && !(psr & (UINT64_C(15) << 28)) && !(psr & 0x11fc1) &&
           ((psr >> 41) & 3) != 3;
}
static bool state_ifs(uint64_t ifs)
{
    if (ifs & (UINT64_C(0x1ffffff) << 38))
        return false;
    if (!(ifs >> 63))
        return true;
    unsigned sof = ifs & 127, sol = (ifs >> 7) & 127, sor = ((ifs >> 14) & 15) * 8;
    unsigned rrb = (ifs >> 18) & 127;
    return sof <= 96 && sol <= sof && sor <= sof && (sor ? rrb < sor : !rrb) &&
           ((ifs >> 25) & 127) < 96 && ((ifs >> 32) & 63) < 48;
}
static bool complete_state_service(struct vcpu *v)
{
    unsigned operation = v->pal_state_pending;
    if (!operation)
        return true;
    v->pal_state_pending = 0;
    bool fresh = v->pal_state_address & 1;
    uint64_t address = v->pal_state_address & ~UINT64_C(1), m[62];
    unsigned count = operation == 1 || !fresh ? 62 : 57;
    if ((operation == 1 && fresh) || !state_area(address, count))
        return false;
    if (operation == 1) {
        if (!v->pal_state_valid)
            return false;
        for (unsigned i = 0; i < count; ++i)
            if (!gm_write((address & ~UC) + i * 8, &v->pal_min_state[i], 8))
                return false;
        struct ia64_state *s = &v->s;
        Ia64Instruction cover = {.opcode = IA64_OP_COVER};
        if (ia64_rse(v, &cover).kind != IA64_STEP_COMPLETE)
            return false;
        s->ar[AR_RSC] &= ~UINT64_C(3);
        s->rse_completion_pending = false;
        return true;
    }
    for (unsigned i = 0; i < count; ++i)
        if (!gm_read((address & ~UC) + i * 8, &m[i], 8))
            return false;
    if ((m[50] & ~(UINT64_C(31) | (UINT64_C(0x3fff) << 16))) || (m[52] & IA64_PSR_IS) ||
        !state_psr(m[52]) || !state_ifs(m[53]) || !(m[53] >> 63) ||
        (!(m[52] & IA64_PSR_IC) && (!state_psr(m[55]) || !state_ifs(m[56]))) ||
        (!fresh && ((m[58] & ~UINT64_C(0x7f07)) || (m[58] >> 8) > 96 || m[61] > 2)))
        return false;
    struct ia64_state *s = &v->s;
    /* Undo PAL's cover using the return frame's SOF. Missing registers
     * remain subject to ordinary mandatory fills after the return. */
    if (ia64_rse_restore(v, m[53] & ((UINT64_C(1) << 38) - 1), m[53] & 127).kind !=
        IA64_STEP_COMPLETE)
        return false;
    ia64_psr_write(s, m[52]);
    for (unsigned i = 1; i < 16; ++i) {
        s->gr[i] = m[i];
        s->nat[i] = (m[0] >> i) & 1;
    }
    bool bank = s->psr & IA64_PSR_BN;
    for (unsigned i = 0; i < 16; ++i) {
        s->gr[16 + i] = m[(bank ? 32 : 16) + i];
        s->nat[16 + i] = (m[0] >> ((bank ? 32 : 16) + i)) & 1;
        s->bank_gr[i] = m[(bank ? 16 : 32) + i];
        s->bank_nat[i] = (m[0] >> ((bank ? 16 : 32) + i)) & 1;
    }
    ia64_pr_unpack(s, m[48], UINT64_MAX);
    s->br[0] = m[49];
    s->ar[AR_RSC] = m[50];
    if (!fresh)
        s->br[1] = m[57];
    s->cr[CR_IIP] = m[m[52] & IA64_PSR_IC ? 51 : 54];
    s->cr[CR_IPSR] = m[m[52] & IA64_PSR_IC ? 52 : 55];
    s->cr[CR_IFS] = m[m[52] & IA64_PSR_IC ? 53 : 56];
    /* An ordinary interruption may have suspended an incomplete frame.
     * Its handler must run before those original mandatory loads resume. */
    s->rse_cfle = fresh || (m[58] & 1) || (!(m[58] >> 8) && s->rse_missing);
    s->rse_completion_pending = !fresh && (m[58] & 2);
    s->rse_completion_demoted = !fresh && (m[58] & 4);
    s->rse_completion_psr = fresh ? 0 : m[59];
    s->rse_completion_ip = fresh ? 0 : m[60];
    s->rse_completion_slot = fresh ? 0 : m[61];
    s->ip = m[51] & ~UINT64_C(15);
    s->slot = (m[52] >> 41) & 3;
    v->pal_state_valid = false;
    v->code_entry_generation = 0;
    return true;
}

void ia64_pal_complete_services(struct vcpu *v)
{
    if (!complete_state_service(v)) {
        LOGE("vCPU%u: invalid PAL min-state save/resume", v->cfg.index);
        vcpu_request_exit(v, VCPU_EXIT_UNIMPLEMENTED);
    }
    if (!v->pal_tr_pending)
        return;
    v->pal_tr_pending = false;
    uint64_t buffer = v->pal_tr_buffer, words[4], valid, pa = buffer & ~UC;
    v->pal_tr_status = -2;
    v->pal_tr_valid = 0;
    if (!ia64_pal_tr_snapshot(v, v->pal_tr_selector & 255, v->pal_tr_selector >> 8, words,
                              &valid) ||
        (buffer & 7) || !physical_range(buffer, 32))
        return;
    /* Only ordinary RAM: avoid recursive service access and partial MMIO effects. */
    for (unsigned i = 0; i < 4; ++i) {
        const struct gm_region *r = gm_lookup(pa + i * 8);
        if (!r || r->kind != GM_RAM || 8 > r->size - (pa + i * 8 - r->base))
            return;
    }
    for (unsigned i = 0; i < 4; ++i)
        if (!gm_write(pa + i * 8, words + i, 8)) {
            v->pal_tr_status = -3;
            return;
        }
    v->pal_tr_status = 0;
    v->pal_tr_valid = valid;
}
uint64_t ia64_pal_tr_status(const struct vcpu *v)
{
    return v ? (uint64_t)v->pal_tr_status : (uint64_t)-2;
}
uint64_t ia64_pal_tr_valid(const struct vcpu *v)
{
    return v ? v->pal_tr_valid : 0;
}

uint64_t ia64_pal_service_read(const struct vcpu *v, unsigned offset)
{
    if (!v)
        return 0;
    switch (offset) {
    case IA64VMM_SVC_PAL_XR0:
        return v->pal_xr0;
    case IA64VMM_SVC_PAL_EXPECTED:
        return v->pal_expected;
    case IA64VMM_SVC_PAL_PALE_PMI:
        return v->pal_pale_pmi;
    case IA64VMM_SVC_PAL_SALE_PMI:
        return v->pal_sale_pmi;
    case IA64VMM_SVC_PAL_EVENT:
        return v->pal_event;
    case IA64VMM_SVC_PAL_PMI_VECTOR:
        return v->pal_pmi_vector;
    default:
        return 0;
    }
}
void ia64_pal_service_write(struct vcpu *v, unsigned offset, uint64_t value)
{
    if (!v)
        return;
    switch (offset) {
    case IA64VMM_SVC_PAL_STATE_SAVE:
    case IA64VMM_SVC_PAL_STATE_RESUME:
        v->pal_state_address = value;
        v->pal_state_pending = offset == IA64VMM_SVC_PAL_STATE_SAVE ? 1 : 2;
        vcpu_request_exit(v, VCPU_EXIT_BUDGET);
        break;
    case IA64VMM_SVC_PAL_XR0:
        v->pal_xr0 = value;
        break;
    case IA64VMM_SVC_PAL_EXPECTED:
        v->pal_expected = value != 0;
        break;
    case IA64VMM_SVC_PAL_PALE_PMI:
        v->pal_pale_pmi = value;
        break;
    case IA64VMM_SVC_PAL_SALE_PMI:
        v->pal_sale_pmi = value;
        break;
    default:
        LOGW("vCPU%u: write to read-only PAL service register %x ignored", v->cfg.index, offset);
        break;
    }
}

struct ia64_result ia64_pal_dispatch(struct vcpu *v)
{
    struct ia64_state *s = &v->s;
    struct ia64_pal_state *p = &s->pal;
    /* The index is r28 even for stacked calls (SDM table 11-60).
     * Consume its NaT before using its bits to select a convention. */
    if (ia64_nat_read(s, 28))
        return ia64_fault(IA64_FAULT_NAT, 28);
    uint64_t index = ia64_gr_read(s, 28);
    if (index == PAL_ENTER_IA_32_ENV) {
        /* This optional system-environment call has a special GR4 status
         * ABI (EDK2 IndustryStandard/Pal.h), not the normal PAL convention.
         * Application-mode IA-32 support does not provide this environment. */
        ia64_gr_write(s, 4, PAL_UNIMPLEMENTED, false);
        for (unsigned i = 0; i < 4; ++i)
            ia64_gr_write(s, 8 + i, i ? 0 : (uint64_t)PAL_UNIMPLEMENTED, false);
        return ia64_complete();
    }
    unsigned first = index & 0x100 ? 33 : 29;
    if ((index & 0x100) && (s->cfm & 127) < 4)
        return ia64_fault(IA64_FAULT_ILLEGAL, 35);
    uint64_t a = ia64_gr_read(s, first), b = ia64_gr_read(s, first + 1);
    uint64_t c = ia64_gr_read(s, first + 2);
    /* SDM Vol.2 NaT Consumption / §11.10.2: match native PAL entry;
     * a NaT argument faults before any procedure effects. */
    for (unsigned i = first; i < first + 3; ++i)
        if (ia64_nat_read(s, i))
            return ia64_fault(IA64_FAULT_NAT, i);
    bool zero = !a && !b && !c, halt = false;
    uint64_t out[4] = {PAL_OK, 0, 0, 0};
    struct ia64_result r = ia64_complete();
    switch (index) {
    case PAL_VERSION:
        if (!zero)
            out[0] = PAL_INVALID;
        else
            out[1] = out[2] = ia64_model_pal_version(v->cfg.cpu_model);
        break;
    case PAL_RSE_INFO:
        if (!zero)
            out[0] = PAL_INVALID;
        else
            out[1] = IA64_N_STACKED_PHYS;
        break;
    case PAL_VM_SUMMARY:
        if (!zero)
            out[0] = PAL_INVALID;
        else {
            out[1] = IA64_VHPT_WALKER | ((uint64_t)(IA64_IMPL_PA_MSB + 1) << 1) |
                     ((uint64_t)IA64_KEY_BITS << 8) | (UINT64_C(15) << 16) |
                     ((uint64_t)IA64_HASH_TAG_ID << 24) |
                     ((uint64_t)(ia64_model_tr_count(v->cfg.cpu_model, 1) - 1) << 32) |
                     ((uint64_t)(ia64_model_tr_count(v->cfg.cpu_model, 0) - 1) << 40) |
                     (UINT64_C(4) << 48) | (UINT64_C(2) << 56);
            out[2] = IA64_IMPL_VA_MSB | ((uint64_t)IA64_RID_BITS << 8);
        }
        break;
    case PAL_VM_INFO:
        if (a >= 2 || b < 1 || b > 2 || c)
            out[0] = PAL_INVALID;
        else {
            /* Fork arch/pal.c: L1 is 32 entries, 4 KiB only; L2 has
             * preferred page size and TR support (SDM Vol.2 PAL_VM_INFO). */
            unsigned entries = a ? ia64_model_tc_count(v->cfg.cpu_model, b - 1) : 32;
            out[1] = 1 | ((uint64_t)entries << 8) | ((uint64_t)entries << 16) |
                     (a ? (UINT64_C(1) << 32) | (UINT64_C(1) << 34) : 0);
            out[2] = a ? IA64_PAGE_SIZE_MASK : UINT64_C(1) << 12;
        }
        break;
    case PAL_VM_PAGE_SIZE:
        if (!zero)
            out[0] = PAL_INVALID;
        else {
            out[1] = IA64_PAGE_SIZE_MASK;
            out[2] = IA64_PURGE_PAGE_SIZE_MASK;
        }
        break;
    case PAL_PTCE_INFO:
        if (!zero)
            out[0] = PAL_INVALID;
        else
            out[2] = (UINT64_C(1) << 32) | 1;
        break;
    case PAL_FREQ_BASE:
        if (!zero)
            out[0] = PAL_INVALID;
        else
            out[1] = IA64VMM_PLATFORM_BASE_HZ;
        break;
    case PAL_FREQ_RATIOS:
        if (!zero)
            out[0] = PAL_INVALID;
        else {
            out[1] = RATIO(IA64VMM_PROC_RATIO_NUM, IA64VMM_PROC_RATIO_DEN);
            out[2] = RATIO(IA64VMM_BUS_RATIO_NUM, IA64VMM_BUS_RATIO_DEN);
            out[3] = RATIO(IA64VMM_ITC_RATIO_NUM, IA64VMM_ITC_RATIO_DEN);
        }
        break;
    case PAL_HALT_LIGHT:
        if (!zero)
            out[0] = PAL_INVALID;
        else
            halt = true;
        break;
    case PAL_HALT:
        /* Only the coherent light-halt state is modeled. */
        out[0] = PAL_UNIMPLEMENTED;
        break;
    case PAL_HALT_INFO: {
        uint64_t states[8] = {UINT64_C(3) << 60};
        if ((a & 7) || b || c)
            out[0] = PAL_INVALID;
        else
            r = write_words(v, a, states, 8, false);
        break;
    }
    case PAL_PREFETCH_VIS:
        out[0] = a <= 1 && !b && !c ? 1 : PAL_INVALID;
        break;
    case PAL_CACHE_FLUSH:
        if (a < 1 || a > 4 || (b & ~UINT64_C(3)))
            out[0] = PAL_INVALID;
        else if (a != 2) {
            /* An instruction visibility point discards every decoded page,
             * including code changed by a writer outside the CPU helpers. */
            code_notify_vmm_write(0, PA_LIMIT);
        }
        break;
    case PAL_CACHE_INIT:
        if (a != UINT64_MAX && (a >= 3 || b < 1 || b > 3 || c > 1))
            out[0] = PAL_INVALID;
        else
            code_notify_vmm_write(0, PA_LIMIT);
        break;
    case PAL_CACHE_LINE_INIT:
        if ((a & UC) || !physical_range(a, 1) || c)
            out[0] = PAL_INVALID;
        else {
            /* SDM Vol.2 p.2:377 initializes cache arrays without backing
             * memory. This model has no cache arrays; serialize only. */
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
        }
        break;
    case PAL_CACHE_SUMMARY:
        if (!zero)
            out[0] = PAL_INVALID;
        else {
            out[1] = 3;
            out[2] = v->cfg.cpu_model == IA64_CPU_MADISON ? 4 : 5;
        }
        break;
    case PAL_CACHE_INFO:
    case PAL_CACHE_PROT_INFO:
    case PAL_CACHE_SHARED_INFO: {
        if (index == PAL_CACHE_SHARED_INFO && v->cfg.cpu_model == IA64_CPU_MADISON) {
            out[0] = PAL_UNIMPLEMENTED;
            break;
        }
        struct cache_info ci;
        if ((index != PAL_CACHE_SHARED_INFO && c) || !cache_info(v->cfg.cpu_model, a, b, &ci)) {
            out[0] = PAL_INVALID;
            break;
        }
        if (index == PAL_CACHE_SHARED_INFO) {
            if (c)
                out[0] = PAL_INVALID;
            else {
                unsigned width = v->cfg.cores_per_socket;
                out[1] = 1;
                /* SDM Fig.11-9: cid is unique within the package. */
                out[2] = (uint64_t)(v->cfg.index % width) << 32;
                out[3] = v->cfg.index;
            }
        } else if (index == PAL_CACHE_PROT_INFO) {
            /* The model has no ECC/parity cache arrays, as in the fork. */
            uint64_t tag = (UINT64_C(1) << 30) | (ci.tag_lsb << 8) | (IA64_IMPL_PA_MSB << 14);
            out[1] = 64 | (tag << 32);
        } else {
            out[1] = ci.unified | ((uint64_t)ci.attribute << 1) | ((uint64_t)ci.ways << 8) |
                     ((uint64_t)ci.line << 16) | ((uint64_t)ci.stride << 24) |
                     ((uint64_t)ci.store_latency << 32) | ((uint64_t)ci.load_latency << 40);
            out[2] = ci.size | (UINT64_C(12) << 32) | ((uint64_t)ci.tag_lsb << 40) |
                     ((uint64_t)IA64_IMPL_PA_MSB << 48);
        }
        break;
    }
    case PAL_MEM_ATTRIB:
        if (!zero)
            out[0] = PAL_INVALID;
        else
            out[1] = 0x71; /* WB, UC, UCE and WC, fork Itanium2 mask. */
        break;
    case PAL_BUS_GET_FEATURES:
        if (!zero)
            out[0] = PAL_INVALID;
        else {
            out[1] = out[3] = PAL_BUS_FEATURES;
            out[2] = PAL_BUS_FEATURES & ~p->bus_disabled;
        }
        break;
    case PAL_BUS_SET_FEATURES:
        if (b || c)
            out[0] = PAL_INVALID;
        else
            p->bus_disabled = PAL_BUS_FEATURES & ~a;
        break;
    case PAL_PROC_GET_FEATURES:
    case PAL_PROC_SET_FEATURES:
        out[0] = c || (index == PAL_PROC_GET_FEATURES && a) ? PAL_INVALID
                                                            : feature_status(v->cfg.cpu_model, b);
        if (!out[0] && b == 18) {
            if (index == PAL_PROC_SET_FEATURES)
                p->proc_disabled = PAL_PROC_FEATURES & ~a;
            else {
                out[1] = out[3] = PAL_PROC_FEATURES;
                out[2] = PAL_PROC_FEATURES & ~p->proc_disabled;
            }
        }
        break;
    case PAL_DEBUG_INFO:
        if (!zero)
            out[0] = PAL_INVALID;
        else {
            out[1] = IA64VMM_ARRAY_SIZE(s->ibr) / 2;
            out[2] = IA64VMM_ARRAY_SIZE(s->dbr) / 2;
        }
        break;
    case PAL_FIXED_ADDR:
        if (!zero)
            out[0] = PAL_INVALID;
        else
            out[1] = v->cfg.index & 0xffff;
        break;
    case PAL_LOGICAL_TO_PHYSICAL: {
        if (v->cfg.cpu_model == IA64_CPU_MADISON) {
            out[0] = PAL_UNIMPLEMENTED;
            break;
        }
        unsigned width = v->cfg.cores_per_socket, socket = v->cfg.index / width;
        unsigned cores = v->cfg.cpu_count ? ia64_model_socket_cores(v->cfg.cpu_model,
                                                                    v->cfg.cpu_count, v->cfg.index)
                                          : width;
        uint64_t number = a == UINT64_MAX ? v->cfg.index % width : a;
        if (number >= cores || b || c)
            out[0] = PAL_INVALID;
        else {
            /* Both firmwares use the same model-bounded, populated sockets. */
            out[1] =
                cores | (UINT64_C(1) << 16) | ((uint64_t)cores << 32) | ((uint64_t)socket << 48);
            out[2] = number << 32;
            out[3] = socket * width + number;
        }
        break;
    }
    case PAL_REGISTER_INFO:
        if (a > 3 || b || c)
            out[0] = PAL_INVALID;
        else {
            static const uint64_t masks[4][2] = {{UINT64_C(0x000011117f2f00ff), 7},
                                                 {0, 0},
                                                 {UINT64_C(0x0000000003fb0107), 0x307ff},
                                                 {0, 2}};
            out[1] = masks[a][0];
            out[2] = masks[a][1];
        }
        break;
    case PAL_PERF_MON_INFO: {
        uint64_t masks[16] = {0x3fff, 0, 0, 0, 0x3ffff, 0, 0, 0, 0xf0, 0, 0, 0, 0xf0, 0, 0, 0};
        if (!a || (a & 7) || b || c)
            out[0] = PAL_INVALID;
        else {
            r = write_words(v, a, masks, 16, false);
            out[1] = 0x08123004;
        }
        break;
    }
    case PAL_PLATFORM_ADDR: {
        uint64_t address = b & ~UC;
        if (a > 1 || c || !physical_range(address, 1))
            out[0] = PAL_INVALID;
        else if (address != (a ? IA64VMM_LEGACY_IO_BASE : IA64VMM_PIB_BASE))
            out[0] = PAL_UNIMPLEMENTED;
        else if (a)
            p->io_block_addr = address;
        else
            p->interrupt_block_addr = address;
        break;
    }
    case PAL_COPY_INFO:
        /* Pal.h PAL_PLATFORM_INFO supplies controller/processor counts.
         * This fixed-size image is independent of both counts and min-state. */
        if (a)
            out[0] = PAL_INVALID;
        else
            out[1] = out[2] = PAL_COPY_SIZE;
        break;
    case PAL_COPY_PAL: {
        uint64_t target = a & ~UC;
        if (c > 1 || b < PAL_COPY_SIZE || (target & (PAL_COPY_SIZE - 1)) ||
            !physical_range(target, b)) {
            out[0] = PAL_INVALID;
            break;
        }
        if (!c) {
            /* break.m + br.many b0: dispatch already pops the stacked frame. */
            static const uint64_t portal[4] = {
                UINT64_C(0x000002000000000a), UINT64_C(0x0004000000000200),
                UINT64_C(0x0000000100000011), UINT64_C(0x0080000800000200)};
            r = write_words(v, target, portal, 4, true);
            if (r.kind != IA64_STEP_COMPLETE)
                break;
            code_notify_vmm_write(target, sizeof(portal));
        }
        /* Keep the latest copy per vCPU; APs reuse it without writing. */
        p->copy_addr = target;
        p->copy_valid = true;
        break;
    }
    case PAL_BRAND_INFO:
        if (c) {
            out[0] = PAL_INVALID;
            break;
        }
        if (!a) {
            const char *brand = ia64_model_brand(v->cfg.cpu_model);
            size_t brand_size = strlen(brand) + 1;
            if (!b) {
                out[0] = PAL_INVALID;
                break;
            }
            r = buffer_check(v, b, brand_size, 1, false, true);
            if (r.kind != IA64_STEP_COMPLETE)
                break;
            for (unsigned i = 0; i < brand_size; ++i) {
                gpa_t pa;
                r = buffer_address(v, b + i, false, true, &pa);
                if (r.kind != IA64_STEP_COMPLETE)
                    break;
                r = ia64_physical_write(v, pa, 1, (uint8_t)brand[i]);
                if (r.kind != IA64_STEP_COMPLETE)
                    break;
            }
            out[1] = brand_size - 1;
        } else if (a == 16)
            out[1] = IA64VMM_PLATFORM_BASE_HZ * IA64VMM_PROC_RATIO_NUM / IA64VMM_PROC_RATIO_DEN;
        else if (a == 17)
            out[1] = (uint64_t)ia64_model_l3_size(v->cfg.cpu_model) *
                     (v->cfg.cpu_count ? ia64_model_socket_cores(v->cfg.cpu_model, v->cfg.cpu_count,
                                                                 v->cfg.index)
                                       : v->cfg.cores_per_socket);
        else if (a == 18)
            out[1] = IA64VMM_PLATFORM_BASE_HZ * IA64VMM_BUS_RATIO_NUM / IA64VMM_BUS_RATIO_DEN;
        else
            out[0] = PAL_INVALID;
        break;
    case PAL_TEST_PROC: {
        /* SDM Fig.11-44: test_phase is bits 63:56, buffer_size is 55:0. */
        uint64_t size = b & ((UINT64_C(1) << 56) - 1);
        if ((a & UC) || !physical_range(a, size) || (b >> 56) > 1 || (c & ~UINT64_C(255)) ||
            !(c & 1) || (s->psr & IA64_PSR_DT))
            out[0] = PAL_INVALID;
        else {
            Ia64Instruction purge = {.opcode = IA64_OP_PTC_E};
            r = ia64_tlb(v, &purge);
            out[1] = 4; /* No modeled physical failure sources. */
        }
        break;
    }
    case PAL_TEST_INFO:
        /* SDM Rev.2.3 PAL_TEST_INFO (37), phases 0 and 1. */
        if (a > 1 || b || c)
            out[0] = PAL_INVALID;
        else
            out[2] = 1;
        break;
    case PAL_MC_REGISTER_MEM:
        /* b is the size in KB (0 = not passed); the required size is 4 KB. */
        if (!(a & UC) || (a & 511) || !physical_range(a, 4096) || c)
            out[0] = PAL_INVALID;
        else if (b && b != 4) {
            out[0] = PAL_INVALID;
            out[1] = 4;
        } else
            p->mc_save_addr = a;
        break;
    case PAL_MC_DRAIN:
    case PAL_MC_CLEAR_LOG:
        /* No physical machine-check source/log is modeled. Transactions
         * finish synchronously; SAL can query/clear the empty log. */
        if (!zero)
            out[0] = PAL_INVALID;
        break;
    case PAL_MC_EXPECTED:
        if (a > 1 || b || c)
            out[0] = PAL_INVALID;
        else {
            out[1] = p->mc_expected;
            p->mc_expected = a != 0;
        }
        break;
    case PAL_MC_DYNAMIC_STATE:
        /* Optional, implementation-specific MCA dynamic-state format.
         * Do not report a successful copy without producing that state. */
        out[0] = a > 1 || c || (a == 1 && (b & 7)) ? PAL_INVALID : PAL_UNIMPLEMENTED;
        break;
    case PAL_MC_ERROR_INFO: {
        uint64_t structure = (b >> 8) & UINT64_C(0xffffffffff);
        bool valid = a < 2 || (a == 2 && !(b >> 48) && !(b & 255) && structure &&
                               !(structure & (structure - 1)) && (c & 7) <= 4);
        out[0] = valid ? PAL_NO_INFO : PAL_INVALID;
        break;
    }
    case PAL_MC_RESUME:
        if (a > 1 || c > 1 || !(b & UC) || (b & 511) || !physical_range(b, 4096))
            out[0] = PAL_INVALID;
        else
            r = ia64_exit(VCPU_EXIT_UNIMPLEMENTED, b,
                          "PAL_MC_RESUME: machine-check/RSE context not built");
        break;
    case PAL_PMI_ENTRYPOINT:
        if ((a & 255) || !physical_range(a, 1) || b || c)
            out[0] = PAL_INVALID;
        else
            p->pmi_entry = a;
        break;
    case PAL_VM_TR_READ:
        if (b > 1 || a >= ia64_model_tr_count(v->cfg.cpu_model, b) || (c & 7))
            out[0] = PAL_INVALID;
        else {
            /* SDM Vol.2 PAL_VM_TR_READ / Fig.4-5: PTE, ITIR, IFA, RR.
             * The required procedure reads the architected TR, never a TC. */
            uint64_t words[4];
            ia64_pal_tr_snapshot(v, a, b, words, &out[1]);
            r = write_words(v, c, words, 4, true);
        }
        break;
    default:
        out[0] = PAL_UNIMPLEMENTED;
        vmm_log(LOG_WARN, "PAL[%u] unimplemented index 0x%llx\n", v->cfg.index,
                (unsigned long long)index);
        break;
    }
    if (r.kind != IA64_STEP_COMPLETE)
        return r;
    for (unsigned i = 0; i < 4; ++i)
        ia64_gr_write(s, 8 + i, out[i], false);
    /* Request an exit only after results commit. The engine retires the
     * break and advances its slot before observing this request. */
    if (halt)
        vcpu_request_exit(v, VCPU_EXIT_HALT);
    return ia64_complete();
}

bool ia64_pal_stacked_ready(const struct vcpu *v)
{
    (void)v;
    return true;
}

struct ia64_result ia64_pal_return_frame(struct vcpu *v)
{
    Ia64Instruction ret = {
        .opcode = IA64_OP_BR_RET, .address = v->s.ip, .slot = v->s.slot, .valid = true};
    return ia64_rse_return(v, &ret, v->s.br[0] & ~UINT64_C(15));
}

struct ia64_result ia64_pal_break(struct vcpu *v)
{
    if (!(ia64_gr_read(&v->s, 28) & 0x100))
        return ia64_pal_dispatch(v);
    /* SDM Vol.2 §11.10.2: return through the real br.ret RSE transition.
     * Diagnose unavailable physical RAM before procedure effects, but leave
     * mandatory loads/debug checks to the committed target. Preloading on a
     * scratch vCPU would consume DA/DD or completion traps before the return. */
    /* The scratch vCPU (~37 KiB) must not live on the executor's 64 KiB
     * host stack under Tier0/helper frames: take it from the VMM heap. */
    size_t pages = (sizeof(*v) + 4095) / 4096;
    struct vcpu *returned = hal_alloc_pages(pages, 0);
    if (!returned)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, 0, "PAL stacked return: no scratch memory");
    *returned = *v;
    struct ia64_result ret = ia64_pal_return_frame(returned);
    if (ret.kind == IA64_STEP_BRANCH && !(returned->s.psr & IA64_PSR_RT)) {
        for (uint64_t a = returned->s.ar[AR_BSP]; a < returned->s.ar[AR_BSPSTORE]; a += 8) {
            gpa_t pa = a & ~(UINT64_C(1) << 63);
            if (pa >> (IA64_IMPL_PA_MSB + 1))
                break; /* Architectural target fault. */
            const struct gm_region *region = ia64_physical_region(returned, pa, 8);
            if (!region) {
                ret = ia64_exit(VCPU_EXIT_BAD_ACCESS, pa, "unmapped RSE backing store");
                break;
            }
            if (region->kind == GM_EMULATED || region->kind == GM_DIRECT_MMIO) {
                ret = ia64_exit(VCPU_EXIT_UNIMPLEMENTED, a, "RSE backing store on MMIO");
                break;
            }
        }
    }
    if (ret.kind == IA64_STEP_BRANCH) {
        struct ia64_result r = ia64_pal_dispatch(v);
        if (r.kind == IA64_STEP_COMPLETE) {
            struct ia64_state *s = &v->s, *t = &returned->s;
            for (unsigned i = 32; i < 128; ++i) {
                s->gr[i] = t->gr[i];
                s->nat[i] = t->nat[i];
            }
            s->cfm = t->cfm;
            s->rse_bof = t->rse_bof;
            s->rse_dirty = t->rse_dirty;
            s->rse_missing = t->rse_missing;
            s->rse_cfle = t->rse_cfle;
            s->rse_completion_pending = t->rse_completion_pending;
            s->rse_completion_psr = t->rse_completion_psr;
            s->rse_completion_ip = t->rse_completion_ip;
            s->rse_completion_slot = t->rse_completion_slot;
            s->rse_completion_demoted = t->rse_completion_demoted;
            s->ar[AR_BSP] = t->ar[AR_BSP];
            s->ar[AR_BSPSTORE] = t->ar[AR_BSPSTORE];
            s->ar[AR_RNAT] = t->ar[AR_RNAT];
            s->ar[AR_EC] = t->ar[AR_EC];
            s->psr = t->psr;
        } else
            ret = r;
    }
    hal_free_pages(returned, pages);
    return ret;
}
