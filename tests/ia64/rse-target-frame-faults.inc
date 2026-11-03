/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Rev.2.3 Vol.2 §§5.3, 6.6–6.8 / Table 6-6: target-frame faults
 * precede instruction fetch and commit downward, including RNAT words. */
static bool interrupt_on_rse_fill;
static const struct gm_region *rse_fill_hook(struct vcpu *v, gpa_t pa, unsigned size)
{
    (void)size;
    if (interrupt_on_rse_fill && v->s.rse_cfle) {
        interrupt_on_rse_fill = false;
        vcpu_raise_external(v, 0x40);
    }
    return gm_lookup(pa);
}
static void test_rse_target_frame_faults(void)
{
    const enum ia64_fault_kind faults[] = {IA64_FAULT_ALT_DTLB, IA64_FAULT_DATA_NAT,
        IA64_FAULT_KEY_MISS, IA64_FAULT_KEY_PERMISSION, IA64_FAULT_ACCESS_RIGHTS,
        IA64_FAULT_ACCESS_BIT, IA64_FAULT_PAGE_NOT_PRESENT};
    const uint64_t ptes[] = {0, 0x7fd, 0x7e1, 0x7e1, 0x661, 0x7c1, 0x7e0};
    for (unsigned boundary = 0; boundary < 2; ++boundary)
      for (unsigned rf = 0; rf < 2; ++rf)
       for (unsigned f = 0; f < IA64VMM_ARRAY_SIZE(faults); ++f)
        for (unsigned covered = 0; covered < 2; ++covered) {
            struct vcpu *v = new_vcpu();
            uint64_t page = UINT64_C(0x2000000000200000);
            uint64_t bottom = page + (boundary ? 0xfe8 : 0x100);
            uint64_t top = ia64_rse_advance(bottom, 4);
            v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = top;
            v->s.ar[AR_RSC] = f == 4 ? 12 : 0;
            v->s.rse_bof = 4;
            v->s.rr[1] = 48;
            if (f) mmu_map(v, 1, 0, page, ENTRY + 0x5000, 12, f == 2 || f == 3 ? 1 : 0, ptes[f]);
            if (boundary) mmu_map(v, 1, 1, page + 0x1000, ENTRY + 0x6000, 12, 0, 0x7e1);
            for (unsigned i = 0; i < 4; ++i) {
                uint64_t address = ia64_rse_advance(bottom, i);
                uint64_t value = 100 + i;
                CHECK(gm_write(ENTRY + 0x5000 + address - page, &value, 8));
            }
            uint64_t nat = UINT64_C(1) << 62;
            if (boundary) CHECK(gm_write(ENTRY + 0x5ff8, &nat, 8));
            uint64_t psr = IA64_PSR_IC | IA64_PSR_RT | IA64_PSR_BN;
            if (f == 2 || f == 3) psr |= IA64_PSR_PK;
            v->s.pkr[0] = 1;
            if (f == 3) v->s.pkr[1] = 0x105;
            v->s.psr = rf ? 0 : psr;
            v->s.ar[AR_PFS] = 4 | (4u << 7) | (UINT64_C(17) << 52);
            v->s.br[0] = ENTRY + 0x100;
            v->s.cr[CR_IPSR] = psr | (UINT64_C(1) << 41);
            v->s.cr[CR_IFS] = (UINT64_C(1) << 63) | 4 | (4u << 7);
            v->s.cr[CR_IIP] = ENTRY + 0x100;
            v->s.cr[CR_IVA] = ENTRY + 0x8000;
            Ia64Instruction ret = instruction(rf ? IA64_OP_RFI : IA64_OP_BR_RET);
            ret.unit = IA64_UNIT_B; ret.slot = 2;
            struct ia64_result branch = ia64_execute(v, &ret);
            CHECK(branch.kind == IA64_STEP_BRANCH && branch.info == ENTRY + 0x100);
            CHECK(v->s.cfm == (4 | (4u << 7)) && v->s.rse_missing == 4 && v->s.rse_cfle);
            CHECK(v->s.ar[AR_BSP] == bottom && v->s.ar[AR_BSPSTORE] == top);
            v->s.ip = branch.info; v->s.slot = branch.next_slot;
            v->last_successful_bundle = ENTRY;
            struct ia64_translation fetch;
            struct ia64_result fault = ia64_translate(v, v->s.ip, IA64_REF_FETCH,
                IA64_ACCESS_EXECUTE, 0, false, &fetch);
            CHECK(fault.fault == faults[f] && fault.kind == IA64_STEP_FAULT);
            CHECK(fault.fault_isr == ((UINT64_C(1) << 34) | (UINT64_C(1) << 37)));
            CHECK(v->s.rse_missing == (boundary ? 2u : 4u));
            CHECK(v->s.ar[AR_BSPSTORE] > fault.info && fault.info >= bottom);
            if (boundary) CHECK(ia64_gr_read(&v->s, 34) == 102 && ia64_gr_read(&v->s, 35) == 103);
            Ia64Instruction target = instruction(IA64_OP_NOP);
            target.address = v->s.ip; target.slot = v->s.slot;
            ia64_deliver_fault(v, &target, fault);
            CHECK(v->s.cr[CR_IIP] == ENTRY + 0x100 && !v->s.cr[CR_IFS] && !v->s.rse_cfle);
            CHECK(((v->s.cr[CR_ISR] >> 37) & 3) == 3 && !(v->s.cr[CR_ISR] & (UINT64_C(1) << 43)));
            CHECK(((v->s.cr[CR_ISR] >> 41) & 3) == rf);
            if (covered) {
                Ia64Instruction cover = instruction(IA64_OP_COVER);
                CHECK(ia64_execute(v, &cover).kind == IA64_STEP_COMPLETE);
                CHECK(!v->s.rse_missing && v->s.rse_dirty == (boundary ? 2u : 0u));
                CHECK(v->s.cr[CR_IFS] == ((UINT64_C(1) << 63) | 4 | (4u << 7)));
            }
            mmu_map(v, 1, 0, page, ENTRY + 0x5000, 12, 0, 0x7e1);
            v->s.pkr[0] = 1; v->s.ar[AR_RSC] = 0;
            ret.opcode = IA64_OP_RFI;
            branch = ia64_execute(v, &ret);
            CHECK(branch.kind == IA64_STEP_BRANCH);
            v->s.ip = branch.info; v->s.slot = branch.next_slot;
            CHECK(ia64_rse_resume(v).kind == IA64_STEP_COMPLETE);
            CHECK(!v->s.rse_cfle && !v->s.rse_missing && v->s.ar[AR_BSPSTORE] == bottom);
            for (unsigned i = 0; i < 4; ++i) {
                CHECK(ia64_nat_read(&v->s, 32 + i) == (boundary && i == 1));
                if (!(boundary && i == 1)) CHECK(ia64_gr_read(&v->s, 32 + i) == 100 + i);
            }
            vcpu_destroy(v);
        }
    /* DA/DD cover one mandatory reference. A successful first word
     * consumes both, exposing the next access-bit fault. */
    struct vcpu *v = new_vcpu();
    uint64_t base = ENTRY + 0x5000;
    v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = base + 16;
    v->s.rse_bof = 2; v->s.ar[AR_RSC] = 0;
    v->s.psr = IA64_PSR_IC | IA64_PSR_RT | IA64_PSR_DA | IA64_PSR_DD;
    mmu_map(v, 1, 0, base, base, 12, 0, 0x7c1);
    CHECK(ia64_rse_restore(v, 2, 2).kind == IA64_STEP_COMPLETE);
    CHECK(ia64_rse_resume(v).fault == IA64_FAULT_ACCESS_BIT);
    CHECK(v->s.rse_missing == 1 && v->s.ar[AR_BSPSTORE] == base + 8);
    CHECK(!(v->s.psr & (IA64_PSR_DA | IA64_PSR_DD)));
    /* External interrupts are checked between mandatory references. */
    mmu_map(v, 1, 0, base, base, 12, 0, 0x7e1);
    v->s.psr |= IA64_PSR_I; v->s.cr[CR_IVA] = ENTRY;
    v->memory_hook = rse_fill_hook; interrupt_on_rse_fill = true;
    CHECK(ia64_rse_resume(v).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.ip == ENTRY + 0x3000 && !v->s.rse_cfle && !v->s.rse_missing);
    CHECK(v->s.cr[CR_ISR] == (UINT64_C(1) << 38));
    vcpu_destroy(v);
    /* Exercise the actual engine boundary, including Tier0's checked fetch:
     * the branch retires before a target fill faults, and the target's first
     * instruction cannot execute until rfi resumes all remaining loads. */
    v = new_vcpu();
    uint64_t va = UINT64_C(0x2000000000200100);
    v->s.psr = IA64_PSR_IC | IA64_PSR_RT | IA64_PSR_BN;
    v->s.ar[AR_RSC] = 0; v->s.rse_bof = 4;
    v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = va + 32;
    v->s.ar[AR_PFS] = 4 | (4u << 7); v->s.br[0] = ENTRY + 0x100;
    v->s.cr[CR_IVA] = ENTRY + 0x8000; v->s.rr[1] = 48;
    const uint64_t nop = UINT64_C(1) << 27;
    bundle_write(ENTRY, 0x11, nop, nop, UINT64_C(0x108001100));
    bundle_write(ENTRY + 0x100, 1, nop, nop, nop);
    bundle_write(ENTRY + 0x9000, 1, nop, nop, nop);
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
    CHECK(v->retired == 3 && v->s.ip == ENTRY + 0x100 && v->s.rse_missing == 4 && v->s.rse_cfle);
    struct vcpu_exit out = engine_run(v, 1);
    if (out.reason != VCPU_EXIT_BUDGET) {
        fprintf(stderr, "rse target frame faults engine=%s reason=%u detail=%s\n", engine_name, out.reason, out.detail);
        vcpu_dump(v);
    }
    CHECK(out.reason == VCPU_EXIT_BUDGET);
    CHECK(v->retired == 4 && v->s.ip == ENTRY + 0x9000 && v->s.slot == 1);
    CHECK(v->s.cr[CR_IIP] == ENTRY + 0x100 && v->s.cr[CR_IIPA] == ENTRY);
    CHECK(v->s.cr[CR_ISR] == ((UINT64_C(1) << 34) | (UINT64_C(3) << 37)));
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t value = 200 + i;
        CHECK(gm_write(ENTRY + 0x5100 + i * 8, &value, 8));
    }
    mmu_map(v, 1, 0, va, ENTRY + 0x5000, 12, 0, 0x7e1);
    Ia64Instruction rfi = instruction(IA64_OP_RFI);
    struct ia64_result branch = ia64_execute(v, &rfi);
    CHECK(branch.kind == IA64_STEP_BRANCH);
    v->s.ip = branch.info; v->s.slot = branch.next_slot;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
    CHECK(v->retired == 5 && v->s.ip == ENTRY + 0x100 && v->s.slot == 1);
    CHECK(!v->s.rse_cfle && !v->s.rse_missing && v->s.ar[AR_BSPSTORE] == va);
    for (unsigned i = 0; i < 4; ++i) CHECK(ia64_gr_read(&v->s, 32 + i) == 200 + i);
    vcpu_destroy(v);
    /* Deferred completion traps must also cross the real engine entry,
     * before target execution, both directly and after a fill interruption. */
    /* Zero also checks DA/DD retention when no completion trap is enabled. */
    const uint64_t modes[] = {0, IA64_PSR_LP, IA64_PSR_TB, IA64_PSR_SS,
        IA64_PSR_LP | IA64_PSR_TB | IA64_PSR_SS};
    for (unsigned interrupted = 0; interrupted < 2; ++interrupted)
      for (unsigned mode = 0; mode < IA64VMM_ARRAY_SIZE(modes); ++mode) {
        v = new_vcpu();
        v->s.slot = 2; v->s.cr[CR_IVA] = ENTRY + 0x8000;
        v->s.psr = IA64_PSR_IC | IA64_PSR_RT | IA64_PSR_BN | modes[mode] |
            IA64_PSR_DA | IA64_PSR_DD;
        v->s.rr[1] = 48; v->s.ar[AR_RSC] = 0; v->s.rse_bof = 4;
        v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = va + 32;
        v->s.ar[AR_PFS] = 4 | (4u << 7) | (UINT64_C(3) << 62);
        v->s.br[0] = ENTRY + 0x100;
        unsigned code = (modes[mode] & IA64_PSR_LP ? 2 : 0) |
            (modes[mode] & IA64_PSR_TB ? 4 : 0) | (modes[mode] & IA64_PSR_SS ? 8 : 0);
        enum ia64_fault_kind kind = code & 2 ? IA64_FAULT_LOWER_PRIVILEGE :
            code & 4 ? IA64_FAULT_TAKEN_BRANCH : IA64_FAULT_SINGLE_STEP;
        uint64_t handler = ENTRY + 0x8000 + (code & 2 ? 0x5e00 : code & 4 ? 0x5f00 : 0x6000);
        bundle_write(handler, 1, nop, nop, nop);
        if (!interrupted) mmu_map(v, 1, 0, va, ENTRY + 0x5000, 12, 0, 0x7e1);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK(v->retired == 1 && v->s.ip == ENTRY + 0x100 && v->s.rse_completion_pending);
        CHECK((v->s.psr & (IA64_PSR_DA | IA64_PSR_DD)) == (IA64_PSR_DA | IA64_PSR_DD));
        if (interrupted) {
            CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
            CHECK(v->s.cr[CR_IIP] == ENTRY + 0x100 && v->s.rse_completion_pending);
            mmu_map(v, 1, 0, va, ENTRY + 0x5000, 12, 0, 0x7e1);
            branch = ia64_execute(v, &rfi);
            CHECK(branch.kind == IA64_STEP_BRANCH);
            v->s.ip = branch.info; v->s.slot = branch.next_slot;
        }
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        if (!code) {
            CHECK(v->s.ip == ENTRY + 0x100 && v->s.slot == 1);
            CHECK(!v->s.rse_completion_pending && !v->s.rse_cfle);
            CHECK(!(v->s.psr & (IA64_PSR_DA | IA64_PSR_DD)));
            vcpu_destroy(v);
            continue;
        }
        CHECK(v->fault.kind == kind && v->fault.effect_committed);
        CHECK(v->s.ip == handler && v->s.slot == 1 && !v->s.rse_completion_pending);
        CHECK(v->s.cr[CR_IIP] == ENTRY + 0x100 && v->s.cr[CR_IIPA] == ENTRY);
        CHECK(v->s.cr[CR_ISR] == (code | (UINT64_C(2) << 41)));
        CHECK(!(v->s.cr[CR_IPSR] & (IA64_PSR_DA | IA64_PSR_DD)));
        vcpu_destroy(v);
      }
}

/* SDM Vol.2 §6.5.5: oversized outputs invalidate the returned frame;
 * the lazy RSE keeps BSPSTORE and needs no target-frame loads. */
static uint64_t bad_pfs_retreat(uint64_t address, unsigned count)
{
    while (count--) {
        address -= 8;
        if (((address >> 3) & 63) == 63) address -= 8;
    }
    return address;
}
static void test_rse_bad_pfs(void)
{
    const struct { unsigned dirty, sof, sol; bool bad; } cases[] = {
        {0, 0, 0, false}, {0, 96, 0, false},
        {1, 96, 1, false}, {1, 96, 0, true},
        {80, 24, 8, false}, {88, 24, 8, true},
        {2, 24, 8, false},
        {96, 96, 96, false}, {96, 96, 95, true}
    };
    const unsigned bofs[] = {0, 1, 95};
    struct vcpu *v = new_vcpu();
    Ia64Instruction ret = {.opcode = IA64_OP_BR_RET, .valid = true,
        .address = ENTRY, .unit = IA64_UNIT_B, .slot = 2};
    ret.operands.branch.target = 2;
    for (unsigned c = 0; c < IA64VMM_ARRAY_SIZE(cases); ++c) {
        for (unsigned b = 0; b < IA64VMM_ARRAY_SIZE(bofs); ++b) {
            unsigned dirty = cases[c].dirty, sof = cases[c].sof, sol = cases[c].sol;
            uint64_t cfm = sof | (sol << 7) | (UINT64_C(95) << 25) | (UINT64_C(47) << 32);
            if (sof >= 8) cfm |= (1u << 14) | (7u << 18);
            v->s.cfm = 96 - dirty;
            v->s.rse_dirty = dirty; v->s.rse_bof = bofs[b];
            v->s.rse_cfle = false; v->s.rse_missing = 0; v->s.rse_completion_pending = false;
            v->s.ar[AR_BSP] = ENTRY + (b ? 0x5200 : 0x51f0);
            v->s.ar[AR_BSPSTORE] = bad_pfs_retreat(v->s.ar[AR_BSP], dirty);
            v->s.ar[AR_RNAT] = 0x12345678;
            v->s.ar[AR_PFS] = cfm | (UINT64_C(41) << 52) | (UINT64_C(3) << 62);
            v->s.ar[AR_EC] = 9;
            v->s.br[2] = (ENTRY + 0x100) | 15;
            v->s.psr = IA64_PSR_IC | IA64_PSR_RT | (UINT64_C(1) << 32);
            struct ia64_state before = v->s;
            ret.qp = 6; v->s.pr &= ~(UINT64_C(1) << 6); before.pr = v->s.pr;
            CHECK(ia64_execute(v, &ret).kind == IA64_STEP_COMPLETE && !memcmp(&v->s, &before, sizeof(before)));
            ret.qp = 0;
            struct ia64_result result = ia64_execute(v, &ret);
            bool bad = cases[c].bad;
            CHECK(result.kind == IA64_STEP_BRANCH && result.info == ENTRY + 0x100);
            CHECK(v->s.cfm == (bad ? 0 : cfm));
            CHECK(v->s.rse_bof == (bofs[b] + 96 - sol) % 96);
            CHECK(v->s.rse_dirty == (dirty >= sol ? dirty - sol : 0));
            CHECK(v->s.ar[AR_BSP] == bad_pfs_retreat(before.ar[AR_BSP], sol));
            CHECK(v->s.ar[AR_BSPSTORE] == before.ar[AR_BSPSTORE] && v->s.ar[AR_RNAT] == 0x12345678);
            CHECK(v->s.rse_missing == (bad || sol <= dirty ? 0 : sol - dirty));
            CHECK(v->s.rse_cfle == (v->s.rse_missing != 0));
            CHECK(v->s.rse_completion_pending == (v->s.rse_missing != 0));
            CHECK(v->s.ar[AR_EC] == 41 && (v->s.psr >> 32 & 3) == 3 &&
                  v->s.ar[AR_PFS] == before.ar[AR_PFS] && v->s.br[2] == before.br[2]);
            CHECK(!memcmp(v->s.gr, before.gr, sizeof(before.gr)) &&
                  !memcmp(v->s.nat, before.nat, sizeof(before.nat)));
            if (bad) CHECK(ia64_rse_resume(v).kind == IA64_STEP_COMPLETE && !v->s.rse_completion_pending);
        }
    }
    vcpu_destroy(v);
    /* A bad-PFS return completes immediately: LP/TB/SS traps must not be
     * postponed behind nonexistent frame fills. */
    const uint64_t modes[] = {0, IA64_PSR_LP, IA64_PSR_TB, IA64_PSR_SS,
        IA64_PSR_LP | IA64_PSR_TB | IA64_PSR_SS};
    const uint64_t nop = UINT64_C(1) << 27;
    bundle_write(ENTRY, 0x11, nop, nop, UINT64_C(0x108001100));
    bundle_write(ENTRY + 0x100, 1, nop, nop, nop);
    for (unsigned mode = 0; mode < IA64VMM_ARRAY_SIZE(modes); ++mode) {
        v = new_vcpu(); v->s.slot = 2; v->s.cr[CR_IVA] = ENTRY;
        v->s.psr = IA64_PSR_IC | IA64_PSR_RT | modes[mode] | IA64_PSR_DA | IA64_PSR_DD;
        v->s.cfm = 8; v->s.rse_bof = 1; v->s.rse_dirty = 88;
        v->s.ar[AR_BSP] = ENTRY + 0x5200;
        v->s.ar[AR_BSPSTORE] = bad_pfs_retreat(v->s.ar[AR_BSP], 88);
        v->s.ar[AR_PFS] = 24 | (8u << 7) | (1u << 14) | (7u << 18) |
            (UINT64_C(95) << 25) | (UINT64_C(47) << 32) |
            (UINT64_C(41) << 52) | (UINT64_C(3) << 62);
        v->s.br[0] = ENTRY + 0x100;
        unsigned code = (modes[mode] & IA64_PSR_LP ? 2 : 0) |
            (modes[mode] & IA64_PSR_TB ? 4 : 0) | (modes[mode] & IA64_PSR_SS ? 8 : 0);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->retired == 1);
        CHECK(!v->s.cfm && !v->s.rse_cfle && !v->s.rse_missing && !v->s.rse_completion_pending);
        CHECK(v->s.rse_bof == 89 && v->s.rse_dirty == 80 &&
              v->s.ar[AR_BSP] == bad_pfs_retreat(ENTRY + 0x5200, 8) && v->s.ar[AR_EC] == 41);
        if (code) {
            CHECK(v->fault.effect_committed && v->s.cr[CR_IIP] == ENTRY + 0x100 &&
                  v->s.cr[CR_IIPA] == ENTRY && v->s.cr[CR_ISR] == (code | (UINT64_C(2) << 41)));
            CHECK((v->s.cr[CR_IPSR] >> 32 & 3) == 3 &&
                  !(v->s.cr[CR_IPSR] & (IA64_PSR_DA | IA64_PSR_DD)));
        } else CHECK(v->s.ip == ENTRY + 0x100 && (v->s.psr >> 32 & 3) == 3);
        vcpu_destroy(v);
    }
}
