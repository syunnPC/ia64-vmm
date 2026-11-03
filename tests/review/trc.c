/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "tests/review/review.h"
#include "ia64/tlb.h"
#include "ia64/smp.h"
#include "ia64/mmu.h"
#include "ia64/profile.h"
#include "ia64/interrupt.h"
#include "ia64vmm/guest_mem.h"

static struct ia64_result insert(struct vcpu *v, unsigned side, bool tr, unsigned slot, uint64_t va,
                                 unsigned ps)
{
    Ia64Instruction i = {.opcode = tr     ? side ? IA64_OP_ITR_D : IA64_OP_ITR_I
                                   : side ? IA64_OP_ITC_D
                                          : IA64_OP_ITC_I};
    i.operands.system.source = 2;
    i.operands.system.register_index = 3;
    v->s.gr[2] = 0x100000 | 0x661;
    v->s.gr[3] = slot;
    v->s.cr[CR_IFA] = va;
    v->s.cr[CR_ITIR] = ps << 2;
    return ia64_tlb(v, &i);
}
static void lookup(struct vcpu *v, uint64_t address, unsigned side)
{
    const struct ia64_tlb_entry *expected = ia64_tlb_find(&v->s, address, side);
    CHECK(ia64_tlb_lookup(v, address, side) == expected);
    struct ia64_translation t;
    struct ia64_result r = ia64_translate(v, address, side ? IA64_REF_DATA : IA64_REF_FETCH,
                                          side ? 1 : 4, 0, false, &t);
    CHECK(t.entry == expected);
    CHECK(r.kind == (expected ? IA64_STEP_COMPLETE : IA64_STEP_FAULT));
}
static void purge_range(struct vcpu *v, Ia64Opcode opcode, uint64_t va, unsigned ps)
{
    Ia64Instruction i = {.opcode = opcode};
    i.operands.system.source = 2;
    i.operands.system.register_index = 3;
    v->s.gr[2] = ps << 2;
    v->s.gr[3] = va;
    CHECK(ia64_tlb(v, &i).kind == IA64_STEP_COMPLETE);
}
static void purge_instruction(struct vcpu *v, Ia64Opcode opcode, uint64_t va)
{
    purge_range(v, opcode, va, 12);
}
static void rr_write(struct vcpu *v, unsigned rid)
{
    Ia64Instruction i = {.opcode = IA64_OP_MOV_GRRR};
    i.operands.system.source = 2;
    i.operands.system.destination = 3;
    v->s.gr[2] = 0;
    v->s.gr[3] = (rid << 8) | (12 << 2);
    CHECK(ia64_system_registers(v, &i).kind == IA64_STEP_COMPLETE);
}
static void index_guards(struct vcpu *v, struct vcpu *saved)
{
    for (unsigned side = 0; side < 2; ++side) {
        vcpu_reset(v, NULL);
        v->s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
        CHECK(insert(v, side, false, 0, 0x400000, 12).kind == IA64_STEP_COMPLETE);
        CHECK(ia64_tlb_lookup(v, 0x400008, side) == &v->s.tc[side][0]);
        CHECK(insert(v, side, true, 0, 0x400000, 12).kind == IA64_STEP_COMPLETE);
        /* TR insertion removes the old TC and invalidates its cached lookup.
         * Table 4-1 forbids a second TR or TC over the pinned page. */
        CHECK(!v->s.tc[side][0].valid);
        CHECK(insert(v, side, true, 1, 0x400000, 16).kind == IA64_STEP_EXIT);
        CHECK(insert(v, side, false, 0, 0x400000, 12).kind == IA64_STEP_EXIT);
        CHECK(insert(v, side, true, 1, 0x410000, 16).kind == IA64_STEP_COMPLETE);
        CHECK(ia64_tlb_lookup(v, 0x400008, side) == &v->s.tr[side][0]);
        CHECK(ia64_tlb_find(&v->s, 0x400008, side) == &v->s.tr[side][0]);
        lookup(v, 0x400008, side);
        rr_write(v, 9);
        lookup(v, 0x400008, side);
        CHECK(!ia64_tlb_lookup(v, 0x400008, side));
        rr_write(v, 0);
        CHECK(ia64_tlb_lookup(v, 0x400008, side) == &v->s.tr[side][0]);
        purge_instruction(v, side ? IA64_OP_PTR_D : IA64_OP_PTR_I, 0x400000);
        CHECK(!ia64_tlb_lookup(v, 0x400008, side));
        lookup(v, 0x400008, side);
        CHECK(insert(v, side, false, 0, 0x500000, 12).kind == IA64_STEP_COMPLETE);
        CHECK(ia64_tlb_lookup(v, 0x500008, side));
        purge_instruction(v, IA64_OP_PTC_L, 0x500000);
        CHECK(!ia64_tlb_lookup(v, 0x500008, side));
        lookup(v, 0x500008, side);
        CHECK(insert(v, side, false, 0, 0x600000, 12).kind == IA64_STEP_COMPLETE);
        CHECK(insert(v, side, true, 0, 0x700000, 12).kind == IA64_STEP_COMPLETE);
        CHECK(ia64_tlb_lookup(v, 0x600008, side));
        purge_instruction(v, IA64_OP_PTC_E, 0);
        CHECK(!ia64_tlb_lookup(v, 0x600008, side));
        CHECK(ia64_tlb_lookup(v, 0x700008, side) == &v->s.tr[side][0]);
        lookup(v, 0x600008, side);
        lookup(v, 0x700008, side);
    }
    vcpu_reset(v, NULL);
    CHECK(insert(v, 1, false, 0, 0x400000, 12).kind == IA64_STEP_COMPLETE);
    CHECK(ia64_tlb_lookup(v, 0x400000, 1));
    uint64_t seq = v->tlb_index.seq;
    CHECK(insert(v, 1, false, 0, 0x500000, 12).kind == IA64_STEP_COMPLETE);
    /* Lose the D-side insertion beneath I-side records. Query the index
     * directly: microTLB overrun recovery must not mask a missing rebuild. */
    for (unsigned i = 0; i < IA64_MMU_LOG; ++i)
        CHECK(insert(v, 0, false, 0, 0x800000 + i * 4096, 12).kind == IA64_STEP_COMPLETE);
    CHECK(v->mmu_seq - seq > IA64_MMU_LOG);
    CHECK(ia64_tlb_lookup(v, 0x500008, 1) == &v->s.tc[1][1]);
    CHECK(ia64_tlb_lookup(v, 0x500008, 1) == ia64_tlb_find(&v->s, 0x500008, 1));

    vcpu_reset(v, NULL);
    v->s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
    CHECK(insert(v, 1, false, 0, 0x400000, 12).kind == IA64_STEP_COMPLETE);
    CHECK(ia64_tlb_lookup(v, 0x400000, 1));
    CHECK(!ia64_tlb_lookup(v, 0x900000, 1));
    *saved = *v;
    CHECK(insert(saved, 1, false, 0, 0x900000, 12).kind == IA64_STEP_COMPLETE);
    /* Restore architectural contents at an equal sequence while retaining
     * the old derived index. The new slot is absent from its bucket chains. */
    seq = v->mmu_seq;
    v->s = saved->s;
    ia64_mmu_cache_reset(v);
    CHECK(v->mmu_seq == seq);
    CHECK(ia64_tlb_lookup(v, 0x900008, 1) == &v->s.tc[1][1]);
    lookup(v, 0x900008, 1);
}

static uint8_t vhpt_ram[65536] __attribute__((aligned(4096)));
static void walker_tests(struct vcpu *v)
{
    const uint64_t va = 0x400128, pa = 0x78000000;
    for (unsigned format = 0; format < 2; ++format)
        for (unsigned side = 0; side < 2; ++side) {
            vcpu_reset(v, NULL);
            CHECK(insert(v, side, true, 0x100, 0x300000, 12).kind == IA64_STEP_COMPLETE);
            CHECK(v->s.tr[side][0].valid && v->s.tr[side][0].va == 0x300000);
            CHECK(ia64_tlb_lookup(v, 0x300008, side) == &v->s.tr[side][0]);
            vcpu_reset(v, NULL);
            v->s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
            v->s.rr[0] = (9 << 8) | (12 << 2) | 1;
            v->s.cr[CR_PTA] = 0x800000 | (15 << 2) | 1 | (format << 8);
            uint64_t hash = ia64_thash(&v->s, va);
            /* Map the real VHPT RAM through itr.d and its MMU log. */
            Ia64Instruction map = {.opcode = IA64_OP_ITR_D};
            map.operands.system.source = 2;
            map.operands.system.register_index = 3;
            v->s.cr[CR_IFA] = hash;
            v->s.cr[CR_ITIR] = 16 << 2;
            v->s.gr[2] = pa | 0x661;
            v->s.gr[3] = 0;
            CHECK(ia64_tlb(v, &map).kind == IA64_STEP_COMPLETE);
            /* Exercise nonzero/wrapping replacement cursors and all 128 slots.
             * The walker result must be the slot just inserted, even before
             * the derived index has replayed its publication record. */
            for (unsigned slot = 0; slot < 129; ++slot) {
                purge_instruction(v, IA64_OP_PTC_L, va);
                uint64_t words[] = {0x120000 | 0x661, 12 << 2, ia64_ttag(&v->s, va), 0};
                if (format && (slot & 1))
                    words[0] = 0x130000 | 0x661;
                CHECK(gm_write(pa + (hash & 0xffff), words, format ? 32 : 8));
                struct ia64_translation out;
                unsigned next = v->s.tc_next[side] % 128;
                struct ia64_result r =
                    ia64_translate(v, va, side ? IA64_REF_DATA : IA64_REF_FETCH,
                                   side ? IA64_ACCESS_READ : IA64_ACCESS_EXECUTE, 0, false, &out);
                CHECK(r.kind == IA64_STEP_COMPLETE);
                CHECK(out.entry == &v->s.tc[side][next]);
                CHECK(out.pa == (words[0] & ~UINT64_C(4095)) + 0x128);
                CHECK(out.entry->itir == (format ? 12u << 2 : v->s.rr[0] & UINT64_C(0xfffffffc)));
                CHECK(ia64_tlb_lookup(v, va, side) == out.entry);
                lookup(v, va, side);
            }
        }
}
static uint64_t random_next(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}
static void trc_tests(void)
{
    CHECK(gm_add_region(&(struct gm_region){.base = 0x78000000,
                                            .size = sizeof(vhpt_ram),
                                            .kind = GM_RAM,
                                            .hva = vhpt_ram,
                                            .name = "TRC VHPT"}));
    for (unsigned model = 0; model < 2; ++model) {
        struct vcpu_config cfg = {.cpu_model = model, .itc_hz = 400000000};
        struct vcpu *v = vcpu_create(&cfg), *saved = malloc(sizeof(*saved));
        CHECK(v && saved);
        index_guards(v, saved);
        walker_tests(v);
        vcpu_reset(v, NULL);
        unsigned tr_count = model == IA64_CPU_MADISON ? 64 : 32;
        CHECK(ia64_model_tr_count(model, 0) == tr_count &&
              ia64_model_tr_count(model, 1) == tr_count);
        for (unsigned side = 0; side < 2; ++side) {
            CHECK(insert(v, side, true, tr_count - 1, 0x200000, 12).kind == IA64_STEP_COMPLETE);
            v->s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
            lookup(v, 0x200000, side);
            uint64_t words[4], valid;
            CHECK(ia64_pal_tr_snapshot(v, tr_count - 1, side, words, &valid));
            CHECK(valid == 15 && words[0] == (0x100000 | 0x661));
            uint64_t seq = v->mmu_seq;
            *saved = *v;
            struct ia64_result r = insert(v, side, true, tr_count, 0x300000, 12);
            CHECK(r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_RESERVED);
            CHECK(v->mmu_seq == seq && !memcmp(v->s.tr, saved->s.tr, sizeof(v->s.tr)) &&
                  !memcmp(v->s.tc, saved->s.tc, sizeof(v->s.tc)));
            CHECK(!ia64_pal_tr_snapshot(v, tr_count, side, words, &valid));
            CHECK(!valid);
            /* SDM Vol.3 itr: architected Reserved Register/Field exception. */
            Ia64Instruction at = {.opcode = side ? IA64_OP_ITR_D : IA64_OP_ITR_I,
                                  .address = 0x400000};
            ia64_deliver_fault(v, &at, r);
            CHECK(v->s.ip == v->s.cr[CR_IVA] + 0x5400 && (v->s.cr[CR_ISR] & 0xffff) == 0x30);
            vcpu_reset(v, NULL);
            v->s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
            for (unsigned i = 0; i < 128; ++i)
                CHECK(insert(v, side, false, 0, 0x800000 + i * 4096, 12).kind ==
                      IA64_STEP_COMPLETE);
            for (unsigned i = 0; i < 128; ++i) {
                CHECK(v->s.tc[side][i].valid && v->s.tc[side][i].va == 0x800000 + i * 4096);
                lookup(v, 0x800000 + i * 4096, side);
            }
            CHECK(insert(v, side, false, 0, 0xa00000, 12).kind == IA64_STEP_COMPLETE);
            CHECK(v->s.tc[side][0].va == 0xa00000 && v->s.tc_next[side] == 129);
            lookup(v, 0x800000, side);
            lookup(v, 0xa00000, side);
        }
        /* Mixed sizes, all regions/RIDs, collisions, replacements, multiple
         * unobserved writes and overrun. Literal first/last slots above are
         * independent of the implementation's capacity constants. */
        vcpu_reset(v, NULL);
        v->s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
        for (unsigned r = 0; r < 8; ++r)
            v->s.rr[r] = (r + 1u) << 8;
        const unsigned shifts[] = {12, 13, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32};
        uint64_t seed = 0x123456789abcdef;
        for (unsigned i = 0; i < 12000; ++i) {
            uint64_t x = random_next(&seed), va = ((x & 7) << 61) | ((x & 0xffffff) << 12);
            unsigned side = (x >> 4) & 1, ps = shifts[(x >> 8) % 12];
            /* Keep mixed-size inserts legal, including TR replacements
             * whose new range encloses another slot (SDM Table 4-1). */
            purge_range(v, side ? IA64_OP_PTR_D : IA64_OP_PTR_I, va, ps);
            CHECK(insert(v, side, (x >> 5) & 1, (x >> 16) % tr_count, va, ps).kind ==
                  IA64_STEP_COMPLETE);
            if (i % 23 == 0)
                continue;
            lookup(v, va, side);
            lookup(v, va ^ 0x2000, side);
            lookup(v, va, !side);
            if (i % 29 == 0) {
                ia64_tlb_remote_purge(v, (v->s.rr[va >> 61] >> 8) & 0xffffff, va, ps, false);
                lookup(v, va, side);
            }
        }
        *saved = *v;
        ia64_tlb_remote_purge(v, 0, 0, 0, true);
        lookup(v, 0x200000, 1);
        *v = *saved;
        ia64_mmu_cache_reset(v);
        CHECK(!v->tlb_index.ready && !v->microtlb.entry[0][0].valid);
        for (unsigned side = 0; side < 2; ++side)
            for (unsigned i = 0; i < tr_count; ++i)
                lookup(v, v->s.tr[side][i].va, side);
        vcpu_reset(v, NULL);
        CHECK(v->s.cpu_model == model);
        for (unsigned side = 0; side < 2; ++side) {
            for (unsigned i = 0; i < IA64_ITR_COUNT; ++i)
                CHECK(!v->s.tr[side][i].valid);
            for (unsigned i = 0; i < 128; ++i)
                CHECK(!v->s.tc[side][i].valid);
        }
        free(saved);
        vcpu_destroy(v);
    }
}
REVIEW_SUITE("trc", trc_tests)
