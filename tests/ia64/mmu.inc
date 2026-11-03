/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/mmu.h"
#include "ia64/tlb.h"
#include "ia64/profile.h"

static void mmu_map(struct vcpu *v, unsigned side, unsigned slot, uint64_t va,
                    uint64_t pa, unsigned ps, unsigned key, uint64_t flags)
{
    v->s.tr[side][slot] = (struct ia64_tlb_entry){
        .va = va & ia64_low_mask(61) & ~ia64_low_mask(ps),
        .pte = pa | flags, .itir = ((uint64_t)key << 8) | (ps << 2),
        .rid = (v->s.rr[va >> 61] >> 8) & 0xffffff, .page_shift = ps, .valid = true};
    ia64_mmu_changed(v, IA64_MMU_TR, side, slot);
}
static struct ia64_result mmu_translate(struct vcpu *v, uint64_t va, bool fetch,
                                        unsigned rights, struct ia64_translation *out)
{
    return ia64_translate(v, va, fetch ? IA64_REF_FETCH : IA64_REF_DATA,
        rights, (v->s.psr >> 32) & 3, false, out);
}
static void test_mmu_geometry(void)
{
    struct vcpu *v = new_vcpu();
    struct ia64_translation out;
    v->s.psr = IA64_PSR_IC | IA64_PSR_IT | IA64_PSR_DT;
    for (unsigned region = 0; region < 8; ++region) {
        v->s.rr[region] = (region + 1u) << 8 | 48;
        for (unsigned ps = 12; ps <= 32; ++ps) {
            if (!ia64_page_size_supported(ps)) continue;
            uint64_t va = ((uint64_t)region << 61) | (UINT64_C(1) << 40) | 0x123;
            uint64_t pa = UINT64_C(1) << 38;
            for (unsigned side = 0; side < 2; ++side) {
                mmu_map(v, side, 0, va, pa, ps, 0, 0x7e1);
                CHECK(mmu_translate(v, va, !side, side ? 3 : 4, &out).kind == IA64_STEP_COMPLETE);
                CHECK(out.pa == pa + 0x123 && out.ma == IA64_MA_WB);
                CHECK(mmu_translate(v, va + (UINT64_C(1) << ps) - 0x124, !side, side ? 1 : 4, &out).kind == IA64_STEP_COMPLETE);
                CHECK(out.pa == pa + (UINT64_C(1) << ps) - 1);
                CHECK(mmu_translate(v, va + (UINT64_C(1) << ps), !side, side ? 1 : 4, &out).fault ==
                    (side ? IA64_FAULT_ALT_DTLB : IA64_FAULT_ALT_ITLB));
                v->s.rr[region] ^= 0x1000;
                CHECK(mmu_translate(v, va, !side, side ? 1 : 4, &out).fault ==
                    (side ? IA64_FAULT_ALT_DTLB : IA64_FAULT_ALT_ITLB));
                v->s.rr[region] ^= 0x1000;
            }
        }
    }
    /* Full implemented VA payload, including both signs and all VRNs. */
    for (unsigned region = 0; region < 8; ++region) {
        uint64_t va = ((uint64_t)region << 61) | ia64_low_mask(61);
        mmu_map(v, 1, 0, va, 0, 12, 0, 0x7e1);
        CHECK(mmu_translate(v, va, false, 1, &out).kind == IA64_STEP_COMPLETE && out.pa == 4095);
    }
    /* Every privilege-dependent entry in SDM Vol.2 Table 4-4. */
    for (unsigned ar = 0; ar < 8; ++ar) for (unsigned pl = 0; pl < 4; ++pl)
        for (unsigned cpl = 0; cpl < 4; ++cpl) {
            unsigned expected = ar == 7 ? cpl ? 4 : 5 : cpl > pl ? 0 :
                ar == 0 ? 1 : ar == 1 ? 5 : ar == 2 ? 3 : ar == 3 ? 7 :
                ar == 4 ? cpl == pl && cpl ? 1 : 3 : ar == 5 ? cpl ? 5 : 7 :
                cpl == pl && cpl ? 7 : 3;
            mmu_map(v, 1, 0, 0x400000, ENTRY, 12, 0, 0x61 | (ar << 9) | (pl << 7));
            v->s.psr = IA64_PSR_DT | IA64_PSR_IC | ((uint64_t)cpl << 32);
            for (unsigned rights = 1; rights <= 3; ++rights)
                CHECK(mmu_translate(v, 0x400000, false, rights, &out).fault ==
                    ((expected & rights) == rights ? IA64_FAULT_NONE : IA64_FAULT_ACCESS_RIGHTS));
        }
    vcpu_destroy(v);
}
static void test_mmu_faults(void)
{
    struct vcpu *v = new_vcpu();
    struct ia64_translation out;
    const uint64_t va = (UINT64_C(3) << 61) | 0x400123;
    struct { uint64_t flags; enum ia64_fault_kind fault; unsigned vector; } cases[] = {
        {0, IA64_FAULT_PAGE_NOT_PRESENT, 0x5000},
        {0x7fd, IA64_FAULT_DATA_NAT, 0x5600},
        {0x7e1, IA64_FAULT_KEY_MISS, 0x1c00},
        {0x7e1, IA64_FAULT_KEY_PERMISSION, 0x5100},
        {0x1e1, IA64_FAULT_ACCESS_RIGHTS, 0x5300},
        {0x781, IA64_FAULT_DIRTY_BIT, 0x2000},
        {0x7c1, IA64_FAULT_ACCESS_BIT, 0x2800},
    };
    v->s.rr[3] = 0x123400 | (14 << 2);
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(cases); ++i) {
        v->s.psr = IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_PK;
        v->s.ip = ENTRY; v->s.slot = 2; v->last_successful_bundle = ENTRY - 16;
        v->s.cr[CR_IVA] = ENTRY; v->s.cr[CR_IHA] = 0xbeef;
        v->s.pkr[0] = i == 3 ? 0x4207 : i == 2 ? 0 : 0x4201;
        mmu_map(v, 1, 0, va, ENTRY, 12, 0x42, cases[i].flags);
        struct ia64_result r = mmu_translate(v, va, false, 3, &out);
        CHECK(r.fault == cases[i].fault && r.info == va);
        Ia64Instruction insn = instruction(IA64_OP_XCHG8);
        ia64_deliver_fault(v, &insn, r);
        CHECK(v->s.ip == ENTRY + cases[i].vector && v->s.cr[CR_IFA] == va);
        CHECK(v->s.cr[CR_ITIR] == (0x123400 | 56) && v->s.cr[CR_IHA] == 0xbeef);
        CHECK(v->s.cr[CR_IIP] == ENTRY && v->s.cr[CR_IIPA] == ENTRY - 16 && v->s.cr[CR_IFS] == 0);
        CHECK(v->s.cr[CR_IPSR] & (UINT64_C(2) << 41));
        CHECK(v->s.cr[CR_ISR] == ((UINT64_C(2) << 41) | (UINT64_C(3) << 33) | (i == 1 ? 0x20 : 0)));
    }
    /* Vol.2 Table 8-2: fetch faults have ISR.x and ed=0 even on an ed page. */
    for (unsigned i = 0; i < 7; ++i) {
        unsigned vectors[] = {0x0c00, 0x5000, 0x5600, 0x1800, 0x5100, 0x5200, 0x2400};
        uint64_t flags[] = {0x7e1, 0, 0x7fd, 0x7e1, 0x7e1, 0x5e1, 0x7c1};
        v->s.psr = IA64_PSR_IT | IA64_PSR_IC | IA64_PSR_PK;
        v->s.ip = va & ~UINT64_C(15); v->s.slot = 1; v->s.cr[CR_IVA] = ENTRY;
        mmu_map(v, 0, 0, va, ENTRY, 12, 0x42, flags[i] | (UINT64_C(1) << 52));
        v->s.tr[0][0].valid = i != 0;
        v->s.pkr[0] = i == 3 ? 0 : i == 4 ? 0x4209 : 0x4201;
        struct ia64_result r = mmu_translate(v, va & ~UINT64_C(15), true, 4, &out);
        CHECK(r.kind == IA64_STEP_FAULT);
        Ia64Instruction insn = instruction(IA64_OP_NOP);
        ia64_deliver_fault(v, &insn, r);
        CHECK(v->s.ip == ENTRY + vectors[i]);
        CHECK(v->s.cr[CR_ISR] == ((UINT64_C(1) << 41) | (UINT64_C(1) << 32) | (i == 2 ? 0x20 : 0)));
    }
    v->s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
    mmu_map(v, 0, 0, va, ENTRY, 12, 0, 0x7c1);
    CHECK(mmu_translate(v, va, true, 4, &out).fault == IA64_FAULT_ACCESS_BIT);
    v->s.psr |= IA64_PSR_IA;
    CHECK(mmu_translate(v, va, true, 4, &out).kind == IA64_STEP_COMPLETE);
    mmu_map(v, 1, 0, va, ENTRY, 12, 0, 0x781);
    CHECK(mmu_translate(v, va, false, 2, &out).fault == IA64_FAULT_DIRTY_BIT);
    v->s.psr |= IA64_PSR_DA | IA64_PSR_DD;
    CHECK(mmu_translate(v, va, false, 3, &out).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.tr[1][0].pte == (ENTRY | 0x781));
    /* An unimplemented physical fetch uses Lower-Privilege Transfer, code 0x20. */
    v->s.psr = IA64_PSR_IC; v->s.ip = UINT64_C(1) << 50; v->s.slot = 2;
    v->s.cr[CR_IVA] = ENTRY;
    struct ia64_result r = mmu_translate(v, v->s.ip, true, 4, &out);
    CHECK(r.fault == IA64_FAULT_UNIMPL_INST);
    Ia64Instruction insn = instruction(IA64_OP_NOP);
    ia64_deliver_fault(v, &insn, r);
    CHECK(v->s.ip == ENTRY + 0x5e00 && v->s.cr[CR_IFA] == (UINT64_C(1) << 50));
    CHECK(v->s.cr[CR_ISR] == ((UINT64_C(2) << 41) | (UINT64_C(1) << 32) | 0x20));
    /* Data Nested TLB preserves every interruption resource. */
    v->s.tr[1][0].valid = false; v->s.psr = IA64_PSR_DT;
    for (unsigned c = 16; c <= 25; ++c) v->s.cr[c] = 0xabc000 + c;
    r = mmu_translate(v, va, false, 1, &out);
    CHECK(r.fault == IA64_FAULT_NESTED_DTLB);
    insn = instruction(IA64_OP_LD8);
    ia64_deliver_fault(v, &insn, r);
    CHECK(v->s.ip == ENTRY + 0x1400);
    for (unsigned c = 16; c <= 25; ++c) CHECK(v->s.cr[c] == 0xabc000 + c);
    vcpu_destroy(v);
}
static void test_mmu_walker(void)
{
    const uint64_t va = (UINT64_C(1) << 61) | 0x400128;
    struct vcpu *v = new_vcpu();
    struct ia64_translation out;
    for (unsigned format = 0; format < 2; ++format) {
        memset(v->s.tr, 0, sizeof(v->s.tr)); memset(v->s.tc, 0, sizeof(v->s.tc));
        v->s.rr[0] = 0x900 | 48; v->s.rr[1] = 0x700 | 49;
        v->s.cr[CR_PTA] = 0x800000 | (15 << 2) | 1 | (format << 8);
        v->s.psr = IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_IT;
        uint64_t hash = ia64_thash(&v->s, va);
        CHECK(mmu_translate(v, va, false, 1, &out).fault == IA64_FAULT_VHPT);
        struct ia64_result r = mmu_translate(v, va, true, 4, &out);
        Ia64Instruction insn = instruction(IA64_OP_NOP);
        v->s.ip = va; v->s.slot = 0; v->s.cr[CR_IVA] = ENTRY;
        ia64_deliver_fault(v, &insn, r);
        CHECK(v->s.ip == ENTRY && v->s.cr[CR_IFA] == va && v->s.cr[CR_IHA] == hash);
        CHECK(v->s.cr[CR_ITIR] == (v->s.rr[hash >> 61] & UINT64_C(0xfffffffc)));
        v->s.psr = IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_IT;
        mmu_map(v, 1, 0, hash, ENTRY, 16, 0, 0x7e1);
        gpa_t entry_pa = ENTRY + (hash & 0xffff);
        uint64_t words[] = {(ENTRY + 0x8000) | 0x7e1, 0x4200 | 48, ia64_ttag(&v->s, va), 0};
        CHECK(gm_write(entry_pa, words, format ? 32 : 8));
        CHECK(mmu_translate(v, va, false, 1, &out).kind == IA64_STEP_COMPLETE);
        CHECK(out.pa == ENTRY + 0x8128);
        const struct ia64_tlb_entry *hit = ia64_tlb_find(&v->s, va, 1);
        CHECK(hit && hit->itir == (format ? 0x4230 : 0x730));
        CHECK(mmu_translate(v, va, true, 4, &out).kind == IA64_STEP_COMPLETE);
        CHECK(ia64_tlb_find(&v->s, va, 0) != NULL);
        memset(v->s.tc, 0, sizeof(v->s.tc));
        words[0] = 0; CHECK(gm_write(entry_pa, words, format ? 32 : 8));
        CHECK(mmu_translate(v, va, false, 1, &out).fault == IA64_FAULT_PAGE_NOT_PRESENT);
        CHECK(ia64_tlb_find(&v->s, va, 1) != NULL);
        memset(v->s.tc, 0, sizeof(v->s.tc));
        words[0] = ENTRY | 0x7e3; CHECK(gm_write(entry_pa, words, format ? 32 : 8));
        CHECK(mmu_translate(v, va, false, 1, &out).fault == IA64_FAULT_DTLB);
        r = mmu_translate(v, va, false, 1, &out);
        v->s.ip = ENTRY; v->s.cr[CR_IVA] = ENTRY;
        insn = instruction(IA64_OP_LD8); ia64_deliver_fault(v, &insn, r);
        CHECK(v->s.ip == ENTRY + 0x800 && v->s.cr[CR_IFA] == va);
        CHECK(v->s.cr[CR_ITIR] == 0x730 && v->s.cr[CR_IHA] == hash);
        v->s.psr = IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_IT;
        if (format) {
            words[0] = ENTRY | 0x7e1; words[2] |= UINT64_C(1) << 63;
            CHECK(gm_write(entry_pa, words, 32));
            CHECK(mmu_translate(v, va, true, 4, &out).fault == IA64_FAULT_ITLB);
            words[2] = ia64_ttag(&v->s, va); words[1] = 0x4200 | (14 << 2);
        }
        words[0] = (ENTRY + 0x8000) | 0x7e1;
        CHECK(gm_write(entry_pa, words, format ? 32 : 8));
        v->s.tr[1][0].pte &= ~UINT64_C(32);
        CHECK(mmu_translate(v, va, false, 1, &out).fault == IA64_FAULT_DTLB);
        v->s.tr[1][0].pte |= 32;
        CHECK(mmu_translate(v, va, false, 1, &out).kind == IA64_STEP_COMPLETE);
        CHECK(out.pa == ENTRY + 0x8128);
        memset(v->s.tc, 0, sizeof(v->s.tc));
        v->s.cr[CR_DCR] = 2;
        for (unsigned j = 0; j < 4; ++j) words[j] = __builtin_bswap64(words[j]);
        CHECK(gm_write(entry_pa, words, format ? 32 : 8));
        CHECK(mmu_translate(v, va, false, 1, &out).kind == IA64_STEP_COMPLETE);
        v->s.cr[CR_DCR] = 0;
        memset(v->s.tc, 0, sizeof(v->s.tc));
        v->s.psr = IA64_PSR_DT; v->s.tr[1][0].valid = false;
        CHECK(mmu_translate(v, va, false, 1, &out).fault == IA64_FAULT_NESTED_DTLB);
        v->s.psr = IA64_PSR_IT | IA64_PSR_DT;
        CHECK(mmu_translate(v, va, true, 4, &out).fault == IA64_FAULT_ALT_ITLB);
    }
    vcpu_destroy(v);
}
static void test_mmu_accesses(void)
{
    struct vcpu *v = new_vcpu();
    const uint64_t va = (UINT64_C(4) << 61) | 0x400000;
    v->s.rr[4] = 0x500 | 48;
    v->s.psr = IA64_PSR_DT | IA64_PSR_IT | IA64_PSR_IC;
    mmu_map(v, 0, 0, ENTRY, ENTRY, 12, 0, 0x7e1 | (UINT64_C(1) << 52));
    mmu_map(v, 1, 0, va, ENTRY + 0x8000, 12, 0, 0x7e1);
    Ia64Instruction i = instruction(IA64_OP_LD8);
    v->s.gr[10] = va; v->s.gr[9] = 0x12345678;
    i.opcode = IA64_OP_ST8; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    i.opcode = IA64_OP_LD8; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0x12345678);
    i.opcode = IA64_OP_LD8S; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && !v->s.nat[8]);
    /* Every DCR bit governs its own deferred exception, via the code TLB.ed. */
    struct { unsigned bit; uint64_t pte, pkr; enum ia64_fault_kind kind; } deferrals[] = {
        {8, 0, 1, IA64_FAULT_ALT_DTLB}, {9, 0, 1, IA64_FAULT_PAGE_NOT_PRESENT},
        {10, 0x7e1, 0, IA64_FAULT_KEY_MISS}, {11, 0x7e1, 5, IA64_FAULT_KEY_PERMISSION},
        {12, 0x461, 1, IA64_FAULT_ACCESS_RIGHTS}, {13, 0x7c1, 1, IA64_FAULT_ACCESS_BIT},
    };
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(deferrals); ++k) {
        v->s.psr = IA64_PSR_DT | IA64_PSR_IT | IA64_PSR_IC | IA64_PSR_PK;
        mmu_map(v, 1, 0, va, ENTRY + 0x8000, 12, 0, deferrals[k].pte);
        v->s.tr[1][0].valid = k != 0; v->s.pkr[0] = deferrals[k].pkr;
        if (k == 4) v->s.psr |= UINT64_C(1) << 32;
        v->s.cr[CR_DCR] = 0;
        struct ia64_result r = ia64_execute(v, &i);
        CHECK(r.fault == deferrals[k].kind);
        v->s.cr[CR_DCR] = UINT64_C(1) << deferrals[k].bit;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
    }
    /* A deferred key miss must still expose a non-deferred rights fault. */
    mmu_map(v, 1, 0, va, ENTRY + 0x8000, 12, 0, 0x461);
    v->s.psr |= UINT64_C(1) << 32;
    v->s.pkr[0] = 0; v->s.cr[CR_DCR] = UINT64_C(1) << 10;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ACCESS_RIGHTS);
    /* A deferred NaT Page does not preclude a Key Miss (Vol.2 Table 5-3). */
    v->s.psr = IA64_PSR_DT | IA64_PSR_IC | IA64_PSR_PK;
    mmu_map(v, 1, 0, va, ENTRY + 0x8000, 12, 0, 0x7fd);
    v->s.pkr[0] = 0; v->s.cr[CR_DCR] = 0;
    i.opcode = IA64_OP_LD8S; CHECK(ia64_execute(v, &i).fault == IA64_FAULT_KEY_MISS);
    v->s.psr = IA64_PSR_DT | IA64_PSR_IC;
    mmu_map(v, 1, 0, va, ENTRY + 0x8000, 12, 0, 0x7fd);
    i.opcode = IA64_OP_LD8; CHECK(ia64_execute(v, &i).fault == IA64_FAULT_DATA_NAT);
    i.opcode = IA64_OP_LD8S; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
    for (unsigned ma = 4; ma <= 6; ++ma) {
        mmu_map(v, 1, 0, va, ENTRY + 0x8000, 12, 0, 0x7e1 | (ma << 2));
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.nat[8] == (ma != 6));
        i.opcode = IA64_OP_LD8; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        i.opcode = IA64_OP_LD8S;
    }
    /* A translated data write invalidates the decoded source by PA. */
    const uint64_t nop = UINT64_C(1) << 27;
    uint64_t add = (UINT64_C(8) << 37) | (8u << 6) | (20u << 13) | (21u << 20);
    bundle_write(ENTRY + 0x8000, 1, nop, add, nop);
    mmu_map(v, 0, 0, va, ENTRY + 0x8000, 12, 0, 0x7e1);
    v->s.ip = va; v->s.slot = 1; v->s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
    v->s.gr[20] = 17; v->s.gr[21] = 19;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.gr[8] == 36);
    bundle_write(ENTRY + 0x9000, 1, nop, nop, nop);
    v->s.tr[0][0].pte = (ENTRY + 0x9000) | 0x7e1;
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 0);
    v->s.ip = va; v->s.slot = 1; v->s.gr[8] = 99;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.gr[8] == 99);
    v->s.tr[0][0].pte = (ENTRY + 0x8000) | 0x7e1;
    ia64_mmu_changed(v, IA64_MMU_TR, 0, 0);
    mmu_map(v, 1, 0, va, ENTRY + 0x8000, 12, 0, 0x7e1);
    v->s.gr[10] = va + 8; v->s.gr[9] = nop >> 18 | nop << 23;
    i.opcode = IA64_OP_ST8; CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    v->s.ip = va; v->s.slot = 1;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.gr[8] == 99);
    /* Suppression retires once, including for an ordinary nop. */
    v->s.ip = va; v->s.slot = 0; v->s.psr |= IA64_PSR_DA | IA64_PSR_DD | IA64_PSR_IA;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
    CHECK(!(v->s.psr & (IA64_PSR_DA | IA64_PSR_DD | IA64_PSR_IA)));
    /* PSR.ia exempts one instruction (Vol.2 Table 3-2 note k, §5.5.4): once
     * slot 0 retired, slot 1 of the same bundle takes the Access Bit fault. */
    bundle_write(ENTRY + 0xa000, 1, nop, nop, nop);
    mmu_map(v, 0, 0, va, ENTRY + 0xa000, 12, 0, 0x7c1);
    v->s.ip = va; v->s.slot = 0; v->s.psr |= IA64_PSR_IA;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
    CHECK(v->s.ip == va && v->s.slot == 1 && !(v->s.psr & IA64_PSR_IA));
    CHECK(mmu_translate(v, va, true, 4, &(struct ia64_translation){0}).fault == IA64_FAULT_ACCESS_BIT);
    vcpu_destroy(v);
}
static void test_mmu_rse_and_pal(void)
{
    struct vcpu *v = new_vcpu();
    const uint64_t va = (UINT64_C(5) << 61) | 0x400000;
    v->s.rr[5] = 0x600 | 48;
    v->s.psr = IA64_PSR_IC | IA64_PSR_RT | IA64_PSR_DA | IA64_PSR_DD;
    v->s.ar[AR_RSC] = 0; v->s.ar[AR_BSPSTORE] = va; v->s.ar[AR_BSP] = va + 16;
    v->s.rse_bof = 2; v->s.rse_dirty = 2;
    v->s.gr[32] = 17; v->s.gr[33] = 19;
    mmu_map(v, 1, 0, va, ENTRY + 0x8000, 12, 0, 0x781);
    Ia64Instruction i = instruction(IA64_OP_FLUSHRS);
    struct ia64_result r = ia64_execute(v, &i);
    CHECK(r.fault == IA64_FAULT_DIRTY_BIT && (r.fault_isr & (UINT64_C(1) << 37)));
    CHECK(v->s.rse_dirty == 1 && v->s.ar[AR_BSPSTORE] == va + 8);
    CHECK(!(v->s.psr & (IA64_PSR_DA | IA64_PSR_DD)));
    CHECK(pal_word(v, ENTRY + 0x8000) == 17);
    v->s.psr |= IA64_PSR_DA;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.rse_dirty == 0 && v->s.ar[AR_BSPSTORE] == va + 16);
    CHECK(pal_word(v, ENTRY + 0x8008) == 19);
    v->s.tr[1][0].pte = (ENTRY + 0x8000) | 0x461;
    /* RSC.pl, rather than CPL, determines mandatory spill privilege. */
    v->s.ar[AR_RSC] = 12; v->s.rse_dirty = 1;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ACCESS_RIGHTS);
    v->s.ar[AR_RSC] = 0; v->s.psr |= UINT64_C(3) << 32;
    v->s.gr[33] = 23;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(pal_word(v, ENTRY + 0x8010) == 23);
    v->s.psr = IA64_PSR_RT | IA64_PSR_IC;
    v->s.ar[AR_BSP] = va + 16; v->s.ar[AR_BSPSTORE] = va + 16;
    v->s.rse_bof = 2; v->s.rse_dirty = 0;
    v->s.gr[32] = v->s.gr[33] = 0;
    CHECK(ia64_rse_restore(v, 2, 2).kind == IA64_STEP_COMPLETE);
    CHECK(ia64_rse_resume(v).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[32] == 17 && v->s.gr[33] == 19 && v->s.ar[AR_BSP] == va);
    /* PAL pointers follow PSR.dt, including an instruction portal alias. */
    v->s.psr = IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_IT;
    mmu_map(v, 0, 0, va, ENTRY, 12, 0, 0x7e1);
    mmu_map(v, 1, 0, va, ENTRY + 0x8000, 12, 0, 0x7e1);
    v->cfg.pal_entry = ENTRY;
    CHECK(ia64_pal_entry_matches(v, va));
    pal_args(v, PAL_HALT_INFO, va, 0, 0);
    CHECK(ia64_pal_dispatch(v).kind == IA64_STEP_COMPLETE);
    CHECK(pal_word(v, ENTRY + 0x8000) == (UINT64_C(3) << 60));
    v->s.psr = IA64_PSR_IT | IA64_PSR_IC;
    v->s.ip = va; v->s.tr[0][0].pte |= UINT64_C(1) << 52;
    v->s.nat[10] = true;
    i = instruction(IA64_OP_LD8);
    r = ia64_execute(v, &i); ia64_deliver_fault(v, &i, r);
    CHECK(!(v->s.cr[CR_ISR] & (UINT64_C(1) << 43)));
    vcpu_destroy(v);
}
static void test_epc(void)
{
    struct vcpu *v = new_vcpu();
    /* The real Debian gate bundle, including its B-unit epc in slot 2. */
    struct ia64_bundle bundle;
    ia64_decode_bundle(UINT64_C(0x00a023381e008811), UINT64_C(0x0040000000480000),
                       ENTRY, &bundle);
    Ia64Instruction i = bundle.insn[2];
    CHECK(i.opcode == IA64_OP_EPC && i.unit == IA64_UNIT_B && i.qp == 0);
    for (unsigned virt = 0; virt < 2; ++virt)
        for (unsigned cpl = 0; cpl < 4; ++cpl)
            for (unsigned ppl = 0; ppl < 4; ++ppl)
                for (unsigned ar = 0; ar < 8; ++ar)
                    for (unsigned pl = 0; pl < 4; ++pl) {
                        v->s.psr = IA64_PSR_IC | IA64_PSR_BN |
                            (virt ? IA64_PSR_IT : 0) | ((uint64_t)cpl << 32);
                        uint64_t before = v->s.psr;
                        v->s.ar[AR_PFS] = (uint64_t)ppl << 62;
                        mmu_map(v, 0, 0, ENTRY, ENTRY, 12, 0,
                                0x61 | (ar << 9) | (pl << 7));
                        /* Promotion uses the I-side, never a data alias. */
                        mmu_map(v, 1, 0, ENTRY, ENTRY, 12, 0, 0xe61);
                        struct ia64_result result = ia64_execute(v, &i);
                        unsigned expected = !virt ? 0 : ar == 7 && pl < cpl ? pl : cpl;
                        CHECK(result.kind == (ppl < cpl ? IA64_STEP_FAULT : IA64_STEP_COMPLETE));
                        CHECK(result.fault == (ppl < cpl ? IA64_FAULT_ILLEGAL : IA64_FAULT_NONE));
                        CHECK(v->s.psr == (ppl < cpl ? before :
                            (before & ~(UINT64_C(3) << 32)) | ((uint64_t)expected << 32)));
                        CHECK(v->s.ar[AR_PFS] == (uint64_t)ppl << 62);
                    }
    /* Fetching each slot after promotion must use the new privilege. */
    bundle_write(ENTRY, 0x11, UINT64_C(0x00008000000),
                 UINT64_C(0x00008000000), UINT64_C(0x00080000000));
    for (unsigned cpl = 0; cpl < 4; ++cpl) {
        v->s.ip = ENTRY; v->s.slot = 2;
        v->s.psr = IA64_PSR_IT | IA64_PSR_IC | ((uint64_t)cpl << 32);
        v->s.ar[AR_PFS] = UINT64_C(3) << 62;
        mmu_map(v, 0, 0, ENTRY, ENTRY, 12, 0, 0xe61);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK(((v->s.psr >> 32) & 3) == 0 && v->s.ip == ENTRY + 16 && v->s.slot == 0);
    }
    vcpu_destroy(v);
}
static void test_mmu(void)
{
    test_mmu_geometry(); test_mmu_faults(); test_mmu_walker(); test_mmu_accesses();
    test_mmu_rse_and_pal(); test_epc();
}
