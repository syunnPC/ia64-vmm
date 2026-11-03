/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/rse.h"
#include "ia64/memory.h"
#include "ia64/mmu.h"
#include "ia64/debug.h"
#include "ia64/interrupt.h"
#include "ia64/tlb.h"

static bool collection(uint64_t address)
{
    return ((address >> 3) & 63) == 63;
}
static uint64_t collection_address(uint64_t address)
{
    return address | UINT64_C(0x1f8);
}
/* Backing-store cursor over data slots: every 64th slot (address bits 8:3
 * all ones) holds an RNAT collection, so 63 data slots precede each
 * collection (SDM Vol.2 §6.1). These are the closed forms of Table 6-2
 * (br.call: BSP + n + (BSP{8:3} + n)/63; br.ret: BSP - n - (62 - BSP{8:3}
 * + n)/63), in 64-bit unsigned arithmetic that wraps like a slot-by-slot
 * walk. Advancing from a collection slot first steps onto the next data
 * slot; retreating zero slots stays put, even on a collection slot. */
uint64_t ia64_rse_advance(uint64_t address, unsigned count)
{
    if (collection(address))
        address += 8;
    uint64_t slot = (address >> 3) & 63;
    return address + 8 * ((uint64_t)count + (slot + count) / 63);
}
static uint64_t retreat(uint64_t address, unsigned count)
{
    if (!count)
        return address;
    uint64_t slot = (address >> 3) & 63;
    return address - 8 * ((uint64_t)count + ((uint64_t)count + 62 - slot) / 63);
}
#ifdef IA64VMM_HOSTED
/* tests/review/rse_cursor.c compares the cursor with the per-slot walk. */
uint64_t ia64_rse_test_retreat(uint64_t address, unsigned count);
uint64_t ia64_rse_test_retreat(uint64_t address, unsigned count)
{
    return retreat(address, count);
}
#endif
static unsigned physical(unsigned bof, int offset)
{
    return 32 + (bof + IA64_N_STACKED_PHYS + offset) % IA64_N_STACKED_PHYS;
}
/* A binding lives only for one mandatory spill/fill operation. It cannot
 * survive a return, fault, snapshot or interruption; hooks/debug and
 * one-reference suppression retain the qualified path (Vol.2 §6.6). No
 * MMU/PSR/RSC/PTA writer runs inside a bound loop; resume returns on
 * interruption, so only the current page needs a reuse check. */
struct rse_page {
    uint64_t va, pa;
    uint8_t *host;
    bool valid;
};
static struct ia64_result qualify(struct vcpu *v, struct rse_page *page, uint64_t address,
                                  bool write, gpa_t *pa, uint8_t **host)
{
    *host = NULL;
    if (!(address & 7) && page->valid && page->va == (address & ~UINT64_C(4095))) {
        *pa = page->pa + (address & 4095);
        *host = page->host + (address & 4095);
        return ia64_complete();
    }
    page->valid = false;
    /* Mandatory backing-store references use RSC.pl and PSR.rt, independently
     * of CPL and PSR.dt (SDM Vol.2 §4.1.1.6, §6.6). */
    struct ia64_translation translated;
    struct ia64_result result =
        ia64_translate(v, address, IA64_REF_RSE, write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ,
                       (v->s.ar[AR_RSC] >> 2) & 3, false, &translated);
    if (result.kind != IA64_STEP_COMPLETE)
        return result;
    result = ia64_debug_data(v, address, 8, write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ,
                             (v->s.ar[AR_RSC] >> 2) & 3, false, true);
    if (result.kind != IA64_STEP_COMPLETE)
        return result;
    *pa = translated.pa;
    const struct gm_region *r = ia64_physical_region(v, *pa, 8);
    if (!r)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, *pa, "unmapped RSE backing store");
    if (r->kind == GM_EMULATED || r->kind == GM_DIRECT_MMIO)
        return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, address, "RSE backing store on MMIO");
    uint64_t base = *pa & ~UINT64_C(4095);
    if (!(address & 7) && !v->memory_hook &&
        !(v->s.psr & (IA64_PSR_DB | IA64_PSR_DA | IA64_PSR_DD)) && translated.ma == IA64_MA_WB &&
        (r->kind == GM_RAM || r->kind == GM_FIRMWARE) && base >= r->base &&
        base - r->base < r->size && 4096 <= r->size - (base - r->base) &&
        !((uintptr_t)(r->hva + (base - r->base)) & 7)) {
        *page = (struct rse_page){.va = address & ~UINT64_C(4095),
                                  .pa = base,
                                  .host = r->hva + (base - r->base),
                                  .valid = true};
        *host = page->host + (address & 4095);
    }
    return ia64_complete();
}
static struct ia64_result load(struct vcpu *v, struct rse_page *page, uint64_t address,
                               uint64_t *value)
{
    gpa_t pa;
    uint8_t *host;
    struct ia64_result r = qualify(v, page, address, false, &pa, &host);
    if (r.kind != IA64_STEP_COMPLETE)
        return r;
    if (host) {
        *value = __atomic_load_n((uint64_t *)host, __ATOMIC_RELAXED);
        r = ia64_complete();
    } else
        r = ia64_physical_read(v, pa, 8, value);
    if (r.kind == IA64_STEP_COMPLETE) {
        *value = ia64_byte_order(*value, 8, v->s.ar[AR_RSC] & 16);
        v->s.psr &= ~(IA64_PSR_DA | IA64_PSR_DD);
    }
    return r;
}
static struct ia64_result spill(struct vcpu *v, unsigned count)
{
    struct ia64_state *s = &v->s;
    struct rse_page page = {0};
    if (!count && !collection(s->ar[AR_BSPSTORE]))
        return ia64_complete();
    /* Commit each successful mandatory reference and backing-store cursor.
     * A later fault resumes from that cursor, without reissuing prior writes.
     * PSR.da/dd cover only the first reference (Vol.2 §5.5.4). */
    do {
        uint64_t address = s->ar[AR_BSPSTORE];
        gpa_t pa;
        uint8_t *host;
        struct ia64_result r = qualify(v, &page, address, true, &pa, &host);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        unsigned index = physical(s->rse_bof, -(int)s->rse_dirty);
        bool rnat_word = collection(address);
        uint64_t value = rnat_word       ? s->ar[AR_RNAT] & ~(UINT64_C(1) << 63)
                         : s->nat[index] ? 0
                                         : s->gr[index];
        /* RSC.be orders registers and NaT collections (SDM Vol.2 §6.4). */
        value = ia64_byte_order(value, 8, s->ar[AR_RSC] & 16);
        if (host) {
            bool acquired = code_cpu_write_begin(pa, 8);
            __atomic_store_n((uint64_t *)host, value, __ATOMIC_RELAXED);
            code_cpu_write_end(acquired);
            r = ia64_complete();
        } else
            r = ia64_physical_write(v, pa, 8, value);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        s->psr &= ~(IA64_PSR_DA | IA64_PSR_DD);
        if (rnat_word)
            s->ar[AR_RNAT] = 0;
        else {
            uint64_t bit = UINT64_C(1) << ((address >> 3) & 63);
            s->ar[AR_RNAT] = (s->ar[AR_RNAT] & ~bit) | (s->nat[index] ? bit : 0);
            --s->rse_dirty;
            --count;
        }
        s->ar[AR_BSPSTORE] += 8;
    } while (count || collection(s->ar[AR_BSPSTORE]));
    return ia64_complete();
}
static struct ia64_result ensure_capacity(struct vcpu *v, unsigned sof)
{
    unsigned total = v->s.rse_dirty + sof;
    return total > IA64_N_STACKED_PHYS ? spill(v, total - IA64_N_STACKED_PHYS) : ia64_complete();
}

/* The page of [address, end) at or after address: qualify's conditions
 * for every reference to it, without a VHPT walk. */
static bool resident_page(struct vcpu *v, uint64_t address, uint64_t end)
{
    const struct ia64_state *s = &v->s;
    if ((address & 7) || ((s->psr & IA64_PSR_RT) && !ia64_tlb_find(s, address, 1)))
        return false;
    struct ia64_translation t;
    if (ia64_translate(v, address, IA64_REF_RSE, IA64_ACCESS_WRITE, (s->ar[AR_RSC] >> 2) & 3, false,
                       &t)
                .kind != IA64_STEP_COMPLETE ||
        t.ma != IA64_MA_WB)
        return false;
    uint64_t last = (address | 4095) < end - 1 ? address | 4095 : end - 1;
    const struct gm_region *r = ia64_physical_region(v, t.pa, (unsigned)(last - address + 1));
    return r && (r->kind == GM_RAM || r->kind == GM_FIRMWARE);
}

bool ia64_rse_spill_resident(struct vcpu *v, unsigned sof)
{
    const struct ia64_state *s = &v->s;
    unsigned total = s->rse_dirty + sof;
    if (total <= IA64_N_STACKED_PHYS)
        return true;
    if (s->psr & (IA64_PSR_DB | IA64_PSR_DA | IA64_PSR_DD))
        return false;
    /* The stores spill() makes: the data slots of count registers and the
     * collections among them and right after them. */
    uint64_t first = s->ar[AR_BSPSTORE], end = first;
    unsigned count = total - IA64_N_STACKED_PHYS;
    do {
        if (!collection(end))
            --count;
        end += 8;
    } while (count || collection(end));
    for (uint64_t at = first; at < end; at = (at | 4095) + 1)
        if (!resident_page(v, at, end))
            return false;
    return true;
}
static void preserve(struct ia64_state *s, unsigned count)
{
    s->ar[AR_BSP] = ia64_rse_advance(s->ar[AR_BSP], count);
    s->rse_bof = (s->rse_bof + count) % IA64_N_STACKED_PHYS;
    s->rse_dirty += count;
}

/* Fetch into a temporary image before changing BOF/CFM or touching a GR;
 * restoration never re-reads memory to recover an earlier instruction. */
static struct ia64_result fill_image(struct vcpu *v, uint64_t start, unsigned count,
                                     uint64_t values[96], bool nats[96])
{
    struct rse_page page = {0};
    uint64_t address = start, rnat = 0, last_collection = UINT64_MAX;
    for (unsigned i = 0; i < count; ++i) {
        uint64_t word = collection_address(address);
        struct ia64_result r;
        if (word != last_collection) {
            if (word == collection_address(v->s.ar[AR_BSPSTORE]))
                rnat = v->s.ar[AR_RNAT];
            else {
                r = load(v, &page, word, &rnat);
                if (r.kind != IA64_STEP_COMPLETE)
                    return r;
            }
            last_collection = word;
        }
        r = load(v, &page, address, &values[i]);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        nats[i] = (rnat >> ((address >> 3) & 63)) & 1;
        address = ia64_rse_advance(address, 1);
    }
    return ia64_complete();
}

struct ia64_result ia64_rse_call(struct vcpu *v, const Ia64Instruction *insn, uint64_t target)
{
    struct ia64_state *s = &v->s;
    uint64_t pfs = s->cfm | ((s->ar[AR_EC] & 63) << 52) | (((s->psr >> 32) & 3) << 62);
    unsigned sof = s->cfm & 127, sol = (s->cfm >> 7) & 127;
    preserve(s, sol);
    s->cfm = sof - sol;
    s->ar[AR_PFS] = pfs;
    s->br[insn->operands.branch.link & 7] = insn->address + 16;
    return (struct ia64_result){.kind = IA64_STEP_BRANCH, .info = target};
}
struct ia64_result ia64_rse_restore(struct vcpu *v, uint64_t cfm, unsigned sol)
{
    struct ia64_state *s = &v->s;
    unsigned missing = sol > s->rse_dirty ? sol - s->rse_dirty : 0;
    uint64_t bsp = retreat(s->ar[AR_BSP], sol);
    s->rse_bof = (s->rse_bof + 96 - sol) % 96;
    s->rse_dirty = sol > s->rse_dirty ? 0 : s->rse_dirty - sol;
    s->ar[AR_BSP] = bsp;
    s->cfm = cfm;
    s->rse_missing = missing;
    s->rse_cfle = missing != 0;
    return ia64_complete();
}
/* Ported from target/ia64/arch/rse.c in the GPL IA-64 QEMU fork:
 * complete_frame_loads / resume_incomplete_frame. The lazy implementation
 * has no clean partition; missing counts the unloaded prefix of this frame.
 * Each word is committed downward, including RNAT words (Vol.2 Table 6-6),
 * so a later fault never reloads a successful reference. */
struct ia64_result ia64_rse_resume(struct vcpu *v)
{
    struct ia64_state *s = &v->s;
    if (!s->rse_cfle)
        return ia64_complete();
    struct rse_page page = {0};
    while (s->ar[AR_BSPSTORE] > s->ar[AR_BSP]) {
        ia64_timer_poll(v);
        if (ia64_external_deliver(v))
            return ia64_complete();
        uint64_t address = s->ar[AR_BSPSTORE] - 8, value;
        struct ia64_result r = load(v, &page, address, &value);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        if (collection(address))
            s->ar[AR_RNAT] = value & ~(UINT64_C(1) << 63);
        else {
            unsigned at = physical(s->rse_bof, --s->rse_missing);
            s->gr[at] = value;
            s->nat[at] = (s->ar[AR_RNAT] >> ((address >> 3) & 63)) & 1;
        }
        s->ar[AR_BSPSTORE] = address;
    }
    ia64_timer_poll(v);
    if (ia64_external_deliver(v))
        return ia64_complete();
    s->rse_cfle = false;
    if (s->rse_completion_pending) {
        uint64_t psr = s->rse_completion_psr;
        uint64_t code = (s->rse_completion_demoted && (psr & IA64_PSR_LP) ? 2 : 0) |
                        (psr & IA64_PSR_TB ? 4 : 0) | (psr & IA64_PSR_SS ? 8 : 0);
        s->rse_completion_pending = false;
        s->psr &= ~(psr & (IA64_PSR_DA | IA64_PSR_DD | IA64_PSR_ID | IA64_PSR_ED | IA64_PSR_IA));
        if (code) {
            if (psr & IA64_PSR_IC)
                v->last_successful_bundle = s->rse_completion_ip;
            Ia64Instruction source = {.opcode = IA64_OP_BR_RET,
                                      .unit = IA64_UNIT_B,
                                      .address = s->rse_completion_ip,
                                      .slot = s->rse_completion_slot};
            struct ia64_result trap = ia64_fault(code & 2   ? IA64_FAULT_LOWER_PRIVILEGE
                                                 : code & 4 ? IA64_FAULT_TAKEN_BRANCH
                                                            : IA64_FAULT_SINGLE_STEP,
                                                 code);
            v->fault =
                (struct ia64_fault_site){trap.fault, source.address, 0, code, source.slot, true};
            ia64_deliver_fault(v, &source, trap);
        }
    }
    return ia64_complete();
}
struct ia64_result ia64_rse_return(struct vcpu *v, const Ia64Instruction *insn, uint64_t target)
{
    struct ia64_state *s = &v->s;
    uint64_t pfs = s->ar[AR_PFS];
    uint64_t old_psr = s->psr;
    unsigned sof = pfs & 127, sol = (pfs >> 7) & 127;
    /* SDM Vol.2 §6.5.5: outputs may not overlap the dirty partition.
     * With a valid PFM, overflow implies sol < dirty, so the empty frame
     * needs no fills and the lazy RSE leaves BSPSTORE unchanged. */
    uint64_t cfm =
        sof - sol > IA64_N_STACKED_PHYS - s->rse_dirty ? 0 : pfs & ((UINT64_C(1) << 38) - 1);
    struct ia64_result r = ia64_rse_restore(v, cfm, sol);
    if (r.kind != IA64_STEP_COMPLETE)
        return r;
    s->ar[AR_EC] = (pfs >> 52) & 63;
    unsigned cpl = (s->psr >> 32) & 3, ppl = pfs >> 62;
    if (cpl < ppl)
        s->psr = (s->psr & ~(UINT64_C(3) << 32)) | ((uint64_t)ppl << 32);
    if (s->rse_missing) {
        s->rse_completion_pending = true;
        s->rse_completion_psr = old_psr;
        s->rse_completion_ip = insn->address;
        s->rse_completion_slot = insn->slot;
        s->rse_completion_demoted = cpl < ppl;
    }
    return (struct ia64_result){.kind = IA64_STEP_BRANCH, .info = target};
}
struct ia64_result ia64_rse(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64SystemOperands *op = &insn->operands.system;
    if (s->rse_missing && insn->opcode != IA64_OP_COVER)
        return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, insn->raw,
                         "RSE operation with incomplete frame (undefined)");
    switch (insn->opcode) {
    case IA64_OP_ALLOC: {
        unsigned sof = op->immediate & 127, sol = (op->immediate >> 7) & 127;
        unsigned sor = (op->immediate >> 14) & 15;
        /* SDM Vol.3 alloc: the rotating region fits SOF, not just SOL. */
        if (sof > 96 || sol > sof || sor * 8 > sof || !op->destination ||
            (op->destination >= 32 && op->destination - 32u >= sof))
            return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
        if (sor != ((s->cfm >> 14) & 15) && (s->cfm & (UINT64_C(0xfffff) << 18)))
            return ia64_fault(IA64_FAULT_RESERVED, insn->raw);
        struct ia64_result r = ensure_capacity(v, sof);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        s->cfm = (s->cfm & ~UINT64_C(0x3ffff)) | sof | (sol << 7) | (sor << 14);
        ia64_gr_write(s, op->destination, s->ar[AR_PFS], false);
        return ia64_complete();
    }
    case IA64_OP_COVER:
        if (!(s->psr & IA64_PSR_IC))
            s->cr[CR_IFS] = (UINT64_C(1) << 63) | s->cfm;
        preserve(s, s->cfm & 127);
        s->rse_dirty -= s->rse_missing;
        s->rse_missing = 0;
        s->rse_cfle = false;
        s->cfm = 0;
        return ia64_complete();
    case IA64_OP_FLUSHRS:
        return spill(v, s->rse_dirty);
    case IA64_OP_LOADRS: {
        unsigned bytes = ((s->ar[AR_RSC] >> 16) & 0x3fff) & ~7u;
        if ((s->ar[AR_RSC] & 3) || ((s->cfm & 127) && bytes))
            return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
        uint64_t tear = s->ar[AR_BSP] - bytes;
        unsigned count = 0;
        for (uint64_t a = tear; a != s->ar[AR_BSP]; a += 8)
            if (!collection(a))
                ++count;
        if (count > IA64_N_STACKED_PHYS)
            return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
        unsigned needed = count > s->rse_dirty ? count - s->rse_dirty : 0;
        uint64_t values[96];
        bool nats[96];
        struct ia64_result r = fill_image(v, ia64_rse_advance(tear, 0), needed, values, nats);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        struct rse_page page = {0};
        uint64_t rnat = s->ar[AR_RNAT], word = collection_address(tear);
        if (word < collection_address(s->ar[AR_BSPSTORE])) {
            r = load(v, &page, word, &rnat);
            if (r.kind != IA64_STEP_COMPLETE)
                return r;
        } else if (word > collection_address(s->ar[AR_BSPSTORE]))
            rnat = 0;
        for (unsigned i = 0; i < needed; ++i) {
            unsigned at = physical(s->rse_bof, (int)i - (int)count);
            s->gr[at] = values[i];
            s->nat[at] = nats[i];
        }
        s->rse_dirty = count;
        s->ar[AR_BSPSTORE] = tear;
        /* Readback is undefined, but a subsequent flush must retain NaTs
         * of earlier spilled slots in this collection (SDM Vol.2 §6.6). */
        s->ar[AR_RNAT] = rnat;
        return ia64_complete();
    }
    default:
        return ia64_unimplemented(insn);
    }
}
