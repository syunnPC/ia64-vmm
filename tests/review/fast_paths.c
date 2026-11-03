/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "tests/review/review.h"
#include "ia64/mmu.h"
#include "ia64/mmu_log.h"
#include "ia64/tlb.h"
#include "ia64/profile.h"
#include "ia64/smp.h"
#include "ia64/rse.h"
#include "ia64/interrupt.h"
#include "ia64/memory.h"
#include "engine/tier0/internal.h"
#include "ia64vmm/guest_mem.h"

static struct vcpu *cpu(void)
{
    struct vcpu_config cfg = {.itc_hz = 3};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    return v;
}
/* Drive exactly the operands consumed by the instruction semantic layer. */
static void insert(struct vcpu *v, uint64_t va, unsigned side, uint64_t pte, uint64_t itir, bool tr,
                   unsigned index)
{
    Ia64Instruction i = {.opcode = tr     ? side ? IA64_OP_ITR_D : IA64_OP_ITR_I
                                   : side ? IA64_OP_ITC_D
                                          : IA64_OP_ITC_I};
    i.operands.system.source = 2;
    i.operands.system.register_index = 3;
    v->s.gr[2] = pte;
    v->s.gr[3] = index;
    v->s.cr[CR_IFA] = va;
    v->s.cr[CR_ITIR] = itir;
    CHECK(ia64_tlb(v, &i).kind == IA64_STEP_COMPLETE);
}
static void tc_insert(struct vcpu *v, uint64_t va, unsigned side, uint64_t pte, uint64_t itir)
{
    insert(v, va, side, pte, itir, false, 0);
}
static void purge_instruction(struct vcpu *v, Ia64Opcode opcode, uint64_t va, unsigned shift)
{
    Ia64Instruction i = {.opcode = opcode};
    i.operands.system.source = 2;
    i.operands.system.register_index = 3;
    v->s.gr[2] = shift << 2;
    v->s.gr[3] = va;
    CHECK(ia64_tlb(v, &i).kind == IA64_STEP_COMPLETE);
}
static void translation(struct vcpu *v, uint64_t va, unsigned side, unsigned rights,
                        enum ia64_fault_kind expected, gpa_t pa)
{
    const struct ia64_tlb_entry *expected_entry = ia64_tlb_find(&v->s, va, side);
    struct ia64_translation t;
    struct ia64_result r =
        ia64_translate(v, va, side ? IA64_REF_DATA : IA64_REF_FETCH, rights, 0, false, &t);
    if (t.entry != expected_entry)
        fprintf(stderr, "lookup va=%llx side=%u psr=%llx got=%p expected=%p fault=%u\n",
                (unsigned long long)va, side, (unsigned long long)v->s.psr, (void *)t.entry,
                (void *)expected_entry, r.fault);
    CHECK(t.entry == expected_entry);
    CHECK(r.fault == expected);
    CHECK(r.kind == (expected ? IA64_STEP_FAULT : IA64_STEP_COMPLETE));
    if (!expected)
        CHECK(t.pa == pa);
}
static void microtlb_tests(void)
{
    struct vcpu *v = cpu(), *other = cpu();
    v->s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
    other->s.psr = v->s.psr;
    const uint64_t va = 0x200000, pte = 0x100000 | 0x661;
    tc_insert(v, va, 1, pte, 12 << 2);
    tc_insert(v, va, 0, pte + 0x1000, 12 << 2);
    for (unsigned i = 0; i < 3; ++i) {
        translation(v, va + 8, 1, 1, 0, 0x100008);
        translation(v, va + 8, 0, 4, 0, 0x101008);
    }
    CHECK(v->microtlb.entry[0][0].valid && v->microtlb.entry[1][0].valid);
    /* Independent vCPU, equal VA/RID and entirely different PA. */
    tc_insert(other, va, 1, pte + 0x4000, 12 << 2);
    translation(other, va, 1, 1, 0, 0x104000);
    translation(v, va, 1, 1, 0, 0x100000);
    /* Real itr invalidates the overlapping TC before publishing its TR. */
    insert(v, va, 1, pte + 0x2000, 12 << 2, true, 0);
    translation(v, va, 1, 1, 0, 0x102000);
    CHECK(v->microtlb.entry[1][0].tr);
    purge_instruction(v, IA64_OP_PTR_D, va, 12);
    translation(v, va, 1, 1, IA64_FAULT_ALT_DTLB, 0);
    tc_insert(v, va, 1, pte, 12 << 2);
    translation(v, va, 1, 1, 0, 0x100000);
    /* Access checks execute again on a hit; Dirty precedes Access. */
    for (unsigned bits = 0; bits < 4; ++bits) {
        tc_insert(v, va, 1, (pte & ~UINT64_C(96)) | (bits << 5), 12 << 2);
        translation(v, va, 1, 2,
                    bits & 2 ? bits & 1 ? 0 : IA64_FAULT_ACCESS_BIT : IA64_FAULT_DIRTY_BIT,
                    0x100000);
        translation(v, va, 1, 2,
                    bits & 2 ? bits & 1 ? 0 : IA64_FAULT_ACCESS_BIT : IA64_FAULT_DIRTY_BIT,
                    0x100000);
    }
    tc_insert(v, va, 1, pte, 12 << 2);
    translation(v, va, 1, 1, 0, 0x100000);
    v->s.psr |= IA64_PSR_PK;
    translation(v, va, 1, 1, IA64_FAULT_KEY_MISS, 0);
    v->s.pkr[0] = 1;
    ia64_mmu_logged(v, IA64_MMU_W_PKR, IA64_MMU_PKR, 0, 0);
    translation(v, va, 1, 1, 0, 0x100000);
    v->s.pkr[0] = 5;
    ia64_mmu_logged(v, IA64_MMU_W_PKR, IA64_MMU_PKR, 0, 0);
    translation(v, va, 1, 1, IA64_FAULT_KEY_PERMISSION, 0);
    v->s.psr &= ~IA64_PSR_PK;
    translation(v, va, 1, 1, 0, 0x100000);
    /* RR invalidation cannot keep a translation of the old RID. */
    v->s.rr[0] = 1 << 8;
    ia64_mmu_logged(v, IA64_MMU_W_RR, IA64_MMU_RR, 0, 0);
    translation(v, va, 1, 1, IA64_FAULT_ALT_DTLB, 0);
    v->s.rr[0] = 0;
    ia64_mmu_changed(v, IA64_MMU_RR, 0, 0);
    translation(v, va, 1, 1, 0, 0x100000);
    ia64_tlb_remote_purge(v, 0, va, 12, false);
    translation(v, va, 1, 1, IA64_FAULT_ALT_DTLB, 0);
    translation(v, va, 0, 4, IA64_FAULT_ALT_ITLB, 0);
    tc_insert(v, va, 1, pte, 12 << 2);
    translation(v, va, 1, 1, 0, 0x100000);
    /* Lose a D-side record behind I-side-only TC records. No live entry is
     * manually invalidated: correctness is checked separately from replay. */
    translation(v, va, 0, 4, IA64_FAULT_ALT_ITLB, 0);
    tc_insert(v, va, 0, pte + 0x1000, 12 << 2);
    translation(v, va, 0, 4, 0, 0x101000);
    tc_insert(v, va + 0x1000, 1, pte, 12 << 2);
    for (unsigned i = 0; i < IA64_MMU_LOG; ++i)
        tc_insert(v, va + 0x10000 + i * 4096, 0, pte + 0x1000, 12 << 2);
    translation(v, va, 0, 4, 0, 0x101000);
    CHECK(!v->microtlb.entry[1][0].valid);
    translation(v, va, 1, 1, 0, 0x100000);
    /* With no overrun, an unrelated D insert must invalidate the D side
     * while preserving the warm I side. This tests log replay itself. */
    tc_insert(v, va + 0x2000, 1, pte, 12 << 2);
    translation(v, va, 0, 4, 0, 0x101000);
    CHECK(!v->microtlb.entry[1][0].valid);
    translation(v, va, 1, 1, 0, 0x100000);
    const Ia64Opcode purges[] = {IA64_OP_PTC_L, IA64_OP_PTC_E, IA64_OP_PTC_G, IA64_OP_PTC_GA};
    for (unsigned i = 0; i < sizeof(purges) / sizeof(purges[0]); ++i) {
        for (unsigned side = 0; side < 2; ++side) {
            tc_insert(v, va, side, pte, 12 << 2);
            translation(v, va, side, side ? 1 : 4, 0, 0x100000);
        }
        purge_instruction(v, purges[i], va, 12);
        translation(v, va, 1, 1, IA64_FAULT_ALT_DTLB, 0);
        translation(v, va, 0, 4, IA64_FAULT_ALT_ITLB, 0);
    }
    tc_insert(v, va, 1, pte, 12 << 2);
    translation(v, va, 1, 1, 0, 0x100000);
    tc_insert(v, va, 0, pte, 12 << 2);
    translation(v, va, 0, 4, 0, 0x100000);
    translation(v, va, 1, 1, 0, 0x100000);
    v->s.ip = va;
    Ia64Instruction at = {.opcode = IA64_OP_NOP, .address = va}, rfi = {.opcode = IA64_OP_RFI};
    const Ia64Opcode modes[] = {IA64_OP_RSM, IA64_OP_SSM, IA64_OP_BSW1, IA64_OP_BSW0};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        Ia64Instruction mode = {.opcode = modes[i]};
        mode.operands.system.immediate = IA64_PSR_I;
        CHECK(ia64_system_registers(v, &mode).kind == IA64_STEP_COMPLETE);
        translation(v, va + 0x8000, 1, 1, IA64_FAULT_ALT_DTLB, 0);
        CHECK(v->microtlb.entry[0][0].valid && v->microtlb.entry[1][0].valid);
    }
    ia64_deliver_fault(v, &at, ia64_fault(IA64_FAULT_BREAK, 0));
    translation(v, va, 1, 1, 0, 0x100000);
    CHECK(v->microtlb.entry[0][0].valid);
    struct ia64_result returned = ia64_control(v, &rfi);
    CHECK(returned.kind == IA64_STEP_BRANCH);
    v->s.ip = returned.info;
    v->s.slot = returned.next_slot;
    translation(v, va, 0, 4, 0, 0x100000);
    CHECK(v->microtlb.entry[1][0].valid);
    v->s.cr[CR_PTA] = 15 << 2;
    translation(v, va + 0x8000, 1, 1, IA64_FAULT_ALT_DTLB, 0);
    CHECK(v->microtlb.entry[0][0].valid && v->microtlb.entry[1][0].valid);
    /* Restore boundary invalidates even an equal sequence with new contents. */
    tc_insert(v, va, 1, pte, 12 << 2);
    translation(v, va, 1, 1, 0, 0x100000);
    ia64_mmu_cache_reset(v);
    CHECK(!v->microtlb.entry[1][0].valid);
    vcpu_reset(v, NULL);
    CHECK(!v->microtlb.entry[0][0].valid && !v->microtlb.entry[1][0].valid);
    vcpu_destroy(other);
    vcpu_destroy(v);
}

/* The fixture scales with the arrays: shrinking any search/insert/purge
 * loop misses a live last-slot translation. Also run with enlarged arrays. */
static void bounds_tests(void)
{
    struct vcpu *v = cpu();
    const uint64_t va = 0xa00000, pte = 0x100000 | 0x661;
    const Ia64Opcode forms[] = {IA64_OP_PTC_L, IA64_OP_PTC_E, IA64_OP_PTR_I, IA64_OP_PTR_D};
    v->s.psr = IA64_PSR_DT | IA64_PSR_IT | IA64_PSR_IC;
    const unsigned count = sizeof(v->s.tc[0]) / sizeof(v->s.tc[0][0]);
    CHECK(count == IA64_TC_ENTRIES);
    for (unsigned form = 0; form <= sizeof(forms) / sizeof(forms[0]); ++form) {
        for (unsigned side = 0; side < 2; ++side) {
            for (unsigned i = 0; i < count; ++i)
                tc_insert(v, va + i * 4096, side, pte + i * 4096, 12 << 2);
            for (unsigned i = 0; i < count; ++i)
                translation(v, va + i * 4096, side, side ? 1 : 4, 0, 0x100000 + i * 4096);
        }
        /* The round-robin replacement must also reach the final slot. */
        for (unsigned side = 0; side < 2; ++side) {
            for (unsigned i = 0; i < count; ++i)
                tc_insert(v, va + i * 4096, side, pte + (count + i) * 4096, 12 << 2);
            for (unsigned i = 0; i < count; ++i)
                translation(v, va + i * 4096, side, side ? 1 : 4, 0, 0x100000 + (count + i) * 4096);
        }
        if (form == sizeof(forms) / sizeof(forms[0]))
            ia64_tlb_remote_purge(v, 0, 0, 0, true);
        else {
            purge_instruction(v, forms[form], va, 28);
            if (forms[form] == IA64_OP_PTR_I)
                purge_instruction(v, IA64_OP_PTR_D, va, 28);
            if (forms[form] == IA64_OP_PTR_D)
                purge_instruction(v, IA64_OP_PTR_I, va, 28);
        }
        for (unsigned side = 0; side < 2; ++side)
            for (unsigned i = 0; i < count; ++i)
                translation(v, va + i * 4096, side, side ? 1 : 4,
                            side ? IA64_FAULT_ALT_DTLB : IA64_FAULT_ALT_ITLB, 0);
    }
    for (unsigned side = 0; side < 2; ++side) {
        unsigned n = ia64_model_tr_count(v->cfg.cpu_model, side);
        for (unsigned i = 0; i < n; ++i)
            insert(v, va + i * 4096, side, pte + i * 4096, 12 << 2, true, i);
        for (unsigned i = 0; i < n; ++i)
            translation(v, va + i * 4096, side, side ? 1 : 4, 0, 0x100000 + i * 4096);
        purge_instruction(v, side ? IA64_OP_PTR_D : IA64_OP_PTR_I, va, 28);
        for (unsigned i = 0; i < n; ++i)
            translation(v, va + i * 4096, side, side ? 1 : 4,
                        side ? IA64_FAULT_ALT_DTLB : IA64_FAULT_ALT_ITLB, 0);
    }
    vcpu_destroy(v);
}

static uint8_t backing[8192] __attribute__((aligned(4096)));
static void rse_tests(void)
{
    struct gm_region region = {
        .base = 0x500000, .size = sizeof(backing), .hva = backing, .kind = GM_RAM};
    CHECK(gm_add_region(&region));
    struct vcpu *v = cpu();
    Ia64Instruction flush = {.opcode = IA64_OP_FLUSHRS};
    v->s.psr = IA64_PSR_RT | IA64_PSR_IC;
    tc_insert(v, 0x600000, 1, 0x500000 | 0x661, 12 << 2);
    tc_insert(v, 0x601000, 1, 0x501000 | 0x661, 12 << 2);
    static struct tier0_dtc_entry dtc[TIER0_DTC_SLOTS];
    struct tier0_execution execution = {.dtc = dtc};
    Ia64Instruction access = {.opcode = IA64_OP_LD8};
    access.operands.memory.base = 8;
    v->s.psr |= IA64_PSR_DT;
    v->s.gr[8] = 0x600010;
    CHECK(tier0_data_pointer(v, &access, &execution) == backing + 16);
    CHECK(tier0_data_pointer(v, &access, &execution) == backing + 16);
    v->s.psr &= ~IA64_PSR_DT;
    v->s.ar[AR_BSPSTORE] = 0x600ff0;
    v->s.rse_dirty = 4;
    for (unsigned i = 124; i < 128; ++i) {
        v->s.gr[i] = i;
        v->s.nat[i] = false;
    }
    v->s.nat[125] = true;
    CHECK(ia64_rse(v, &flush).kind == IA64_STEP_COMPLETE);
    uint64_t value;
    CHECK(gm_read(0x500ff0, &value, 8) && value == 124);
    CHECK(gm_read(0x500ff8, &value, 8) && value == 0);
    CHECK(gm_read(0x501000, &value, 8) && value == 0);
    CHECK(gm_read(0x501008, &value, 8) && value == 126);
    CHECK(gm_read(0x501010, &value, 8) && value == 127);
    CHECK(v->s.rse_dirty == 0 && v->s.ar[AR_BSPSTORE] == 0x601018);
    Ia64Instruction loads = {.opcode = IA64_OP_LOADRS};
    v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE];
    v->s.ar[AR_RSC] = 40u << 16;
    for (unsigned i = 124; i < 128; ++i) {
        v->s.gr[i] = 999;
        v->s.nat[i] = false;
    }
    CHECK(ia64_rse(v, &loads).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[124] == 124 && v->s.gr[125] == 0 && v->s.nat[125]);
    CHECK(v->s.gr[126] == 126 && v->s.gr[127] == 127 && v->s.rse_dirty == 4);
    v->s.ar[AR_RSC] = 3;
    /* Successful first page commits; the next page's dirty fault does not. */
    tc_insert(v, 0x601000, 1, 0x501000 | 0x621, 12 << 2);
    v->s.ar[AR_BSPSTORE] = 0x600ff0;
    v->s.rse_dirty = 2;
    struct ia64_result r = ia64_rse(v, &flush);
    CHECK(r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_DIRTY_BIT);
    CHECK(r.translation_fault && (r.fault_isr & (UINT64_C(1) << 37)));
    CHECK(v->s.rse_dirty == 1 && v->s.ar[AR_BSPSTORE] == 0x601000);
    /* DA applies to one reference only, and must not create a reusable grant. */
    tc_insert(v, 0x600000, 1, 0x500000 | 0x601, 12 << 2);
    v->s.ar[AR_BSPSTORE] = 0x600000;
    v->s.rse_dirty = 2;
    v->s.psr |= IA64_PSR_DA;
    r = ia64_rse(v, &flush);
    CHECK(r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_DIRTY_BIT);
    CHECK(v->s.rse_dirty == 1 && v->s.ar[AR_BSPSTORE] == 0x600008);
    CHECK(!(v->s.psr & IA64_PSR_DA));
    tc_insert(v, 0x600000, 1, 0x500000 | 0x661, 12 << 2);
    v->s.ar[AR_BSPSTORE] = 0x600003;
    v->s.rse_dirty = 2;
    CHECK(ia64_rse(v, &flush).exit == VCPU_EXIT_BAD_ACCESS);
    /* RSC.be = 1 stores big-endian words on the bound page too (SDM Vol.2 §6.4). */
    v->s.ar[AR_BSPSTORE] = 0x600000;
    v->s.ar[AR_RSC] |= 16;
    CHECK(ia64_rse(v, &flush).kind == IA64_STEP_COMPLETE && v->s.rse_dirty == 0);
    CHECK(gm_read(0x500000, &value, 8) && value == __builtin_bswap64(126));
    CHECK(gm_read(0x500008, &value, 8) && value == __builtin_bswap64(127));
    vcpu_destroy(v);
}

static uint64_t hook(const struct vcpu *v, uint64_t value)
{
    (void)v;
    return value;
}
static void timer_tests(void)
{
    struct vcpu *v = cpu();
    review_mock_clock = 1;
    review_mock_hz = 10;
    review_mock_counter = 0;
    v->clock_origin = v->clock_offset = 0;
    v->s.cr[CR_ITV] = 0xef;
    v->s.cr[CR_ITM] = 2;
    ia64_timer_program(v);
    ia64_timer_poll(v);
    CHECK(v->timer_cache.valid);
    review_mock_counter = 6;
    ia64_timer_poll(v);
    CHECK(!v->timer_fired);
    review_mock_counter = 7;
    ia64_timer_poll(v);
    CHECK(v->timer_fired);
    CHECK(v->s.cr[CR_IRR0 + 3] & (UINT64_C(1) << 47));
    /* Reprogram a warm deadline to an earlier match. Cache invalidation,
     * rather than redundant value comparisons, must expose the new match. */
    review_mock_counter = 0;
    v->clock_origin = v->clock_offset = 0;
    v->s.cr[CR_ITM] = 30;
    ia64_timer_program(v);
    ia64_timer_poll(v);
    CHECK(v->timer_cache.valid);
    v->s.cr[CR_ITM] = 1;
    ia64_timer_program(v);
    review_mock_counter = 4;
    ia64_timer_poll(v);
    CHECK(v->timer_fired);
    review_mock_counter = 0;
    v->clock_offset = 0;
    v->s.cr[CR_ITM] = 30;
    ia64_timer_program(v);
    ia64_timer_poll(v);
    CHECK(v->timer_cache.valid);
    ia64_itc_write(v, 29);
    review_mock_counter = 4;
    ia64_timer_poll(v);
    CHECK(v->timer_fired);
    /* Match, past ITM/wrap, mask and icount retain the original decisions. */
    uint64_t seed = 1;
    for (unsigned i = 0; i < 1000; ++i) {
        seed = seed * UINT64_C(6364136223846793005) + 1;
        v->clock_origin = seed;
        v->clock_offset = seed >> 1;
        v->cfg.itc_hz = (seed % 100) + 1;
        review_mock_hz = ((seed >> 8) % 100) + 1;
        review_mock_counter = seed + 100;
        v->s.cr[CR_ITM] = ia64_itc_read(v) + (i % 17);
        ia64_timer_program(v);
        uint64_t last = v->timer_last;
        bool fired = false;
        for (unsigned n = 0; n < 40; ++n) {
            review_mock_counter += n % 3;
            uint64_t now = ia64_itc_read(v);
            fired |= now - last >= v->s.cr[CR_ITM] - last;
            last = now;
            ia64_timer_poll(v);
            CHECK(v->timer_fired == fired);
        }
    }
    v->cfg.itc_hz = 3;
    review_mock_hz = 10;
    review_mock_counter = 0;
    v->clock_origin = 2;
    v->clock_offset = 0;
    v->s.cr[CR_ITM] = ia64_itc_read(v) + 2;
    ia64_timer_program(v);
    ia64_timer_poll(v);
    CHECK(!v->timer_cache.valid);
    review_mock_counter = 2;
    ia64_timer_poll(v);
    CHECK(v->timer_fired);
    review_mock_counter = 0;
    v->clock_origin = 0;
    v->clock_offset = UINT64_MAX - 1;
    v->s.cr[CR_ITM] = 1;
    v->s.cr[CR_ITV] = 1 << 16;
    ia64_timer_program(v);
    ia64_timer_poll(v);
    review_mock_counter = 10;
    ia64_timer_poll(v);
    CHECK(v->timer_fired);
    review_mock_counter = 0;
    v->clock_offset = 0;
    v->s.cr[CR_ITM] = UINT64_MAX;
    ia64_timer_program(v);
    ia64_timer_poll(v);
    CHECK(!v->timer_fired && !v->timer_cache.valid);
    v->s.cr[CR_ITM] = 10;
    ia64_timer_program(v);
    ia64_timer_poll(v);
    ia64_itc_write(v, 9);
    review_mock_counter = 4;
    ia64_timer_poll(v);
    CHECK(v->timer_fired);
    v->clock_hook = hook;
    v->s.cr[CR_ITM] = ia64_itc_read(v) + 10;
    ia64_timer_program(v);
    ia64_timer_poll(v);
    CHECK(!v->timer_cache.valid);
    v->clock_hook = NULL;
    v->cfg.itc_icount = true;
    v->clock_offset = 0;
    v->retired = 0;
    v->s.cr[CR_ITM] = 1;
    ia64_timer_program(v);
    ia64_timer_poll(v);
    CHECK(!v->timer_fired && !v->timer_cache.valid);
    v->retired = 1;
    ia64_timer_poll(v);
    CHECK(v->timer_fired);
    review_mock_clock = 0;
    vcpu_destroy(v);
}
/* SDM Vol.2 Part I s3.3.4.2: ITC == ITM raises the interrupt. s5.8.3.5: the
 * IRR bit is cleared only when software reads IVR. Writing ITM afterwards
 * therefore does not withdraw the request: it is delivered although ITC is
 * below the new ITM (a stall between a handler's ITM write and its ITC
 * check is such a case). Only the new match raises the next interrupt. */
static void timer_pending_tests(void)
{
    struct vcpu_config cfg = {.itc_hz = 10};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    review_mock_clock = 1;
    review_mock_hz = 10;
    review_mock_counter = 0;
    v->clock_origin = v->clock_offset = 0;
    v->s.cr[CR_ITV] = 0xef;
    v->s.cr[CR_ITM] = 5;
    ia64_timer_program(v);
    review_mock_counter = 7;
    ia64_timer_poll(v);
    CHECK(v->timer_fired);
    v->s.cr[CR_ITM] = 100;
    ia64_timer_program(v);
    CHECK(!v->timer_fired);
    CHECK(ia64_itc_read(v) < v->s.cr[CR_ITM]);
    CHECK(ia64_ivr_read(v) == 0xef);
    ia64_eoi(v);
    CHECK(ia64_ivr_read(v) == 15);
    review_mock_counter = 99;
    CHECK(ia64_ivr_read(v) == 15);
    review_mock_counter = 100;
    CHECK(ia64_ivr_read(v) == 0xef);
    review_mock_clock = 0;
    vcpu_destroy(v);
}
REVIEW_SUITE("microtlb", microtlb_tests)
REVIEW_SUITE("rse-bound-page", rse_tests)
REVIEW_SUITE("timer-cache", timer_tests)
REVIEW_SUITE("timer-cache-pending", timer_pending_tests)

REVIEW_SUITE("tlb-bounds", bounds_tests)
