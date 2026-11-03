/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/debug.h"
#include "engine/tier0/tier0.h"

static void test_debug_brp(void)
{
    struct vcpu *v = new_vcpu();
    const uint64_t raws[] = {UINT64_C(0xe800000048), UINT64_C(7) << 37,
        (UINT64_C(2) << 37) | (UINT64_C(0x10) << 27),
        (UINT64_C(2) << 37) | (UINT64_C(0x11) << 27)};
    uint64_t nop = UINT64_C(1) << 27;
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(raws); ++n) {
        bundle_write(ENTRY, 0x11, nop, nop, raws[n]);
        const struct ia64_bundle *b;
        CHECK(code_get_bundle(v, ENTRY, &b).kind == IA64_STEP_COMPLETE);
        CHECK(b->insn[2].opcode == IA64_OP_BRP && b->insn[2].qp == 0);
        struct ia64_state before = v->s;
        CHECK(ia64_execute(v, &b->insn[2]).kind == IA64_STEP_COMPLETE);
        CHECK(!memcmp(&before, &v->s, sizeof(before)));
        v->s.ip = ENTRY; v->s.slot = 2;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
        CHECK(v->s.ip == ENTRY + 16 && v->s.slot == 0);
    }
    vcpu_destroy(v);
}
static void test_debug_data_enables(void)
{
    /* SDM Vol.2 7.1.2: PSR.db enables DBRs and PSR.dd suppresses them. */
    for (unsigned model = IA64_CPU_MONTECITO; model <= IA64_CPU_MADISON; ++model)
        for (unsigned translated = 0; translated < 2; ++translated) {
            struct vcpu *v = model_vcpu(model);
            uint64_t address = translated ? UINT64_C(0x2000000000200000) : ENTRY + 0x8000;
            if (translated) mmu_map(v, 1, 0, address, ENTRY + 0x8000, 12, 0, 0x7e1);
            v->s.gr[10] = v->s.dbr[0] = address;
            v->s.dbr[1] = UINT64_C(0xc1ffffffffffffff);
            v->s.gr[9] = UINT64_C(0x123456789abcdef0);
            for (unsigned flags = 0; flags < 4; ++flags)
                for (unsigned store = 0; store < 2; ++store) {
                    v->s.psr = IA64_PSR_IC | (translated ? IA64_PSR_DT : 0) |
                        (flags & 1 ? IA64_PSR_DB : 0) | (flags & 2 ? IA64_PSR_DD : 0);
                    uint64_t value = 123, actual;
                    CHECK(gm_write(ENTRY + 0x8000, &value, 8));
                    v->s.gr[8] = 99;
                    Ia64Instruction i = instruction(store ? IA64_OP_ST8 : IA64_OP_LD8);
                    struct ia64_result r = ia64_execute(v, &i);
                    if (flags == 1) {
                        CHECK(r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_DEBUG);
                        CHECK(v->s.gr[8] == 99);
                    } else {
                        CHECK(r.kind == IA64_STEP_COMPLETE);
                        if (store) value = v->s.gr[9];
                        else CHECK(v->s.gr[8] == value);
                    }
                    CHECK(gm_read(ENTRY + 0x8000, &actual, 8) && actual == value);
                }
            vcpu_destroy(v);
        }
}
static void test_debug_registers(void)
{
    struct vcpu *v = new_vcpu();
    const uint64_t mask = UINT64_C(0x00ffffffffffffff);
    uint64_t address = ENTRY + 0x8000;
    for (unsigned pair = 0; pair < 4; ++pair) {
        for (unsigned level = 0; level < 4; ++level) {
            memset(v->s.dbr, 0, sizeof(v->s.dbr));
            v->s.dbr[pair * 2] = address + 7;
            v->s.dbr[pair * 2 + 1] = mask | (UINT64_C(1) << (56 + level)) | (UINT64_C(1) << 63);
            CHECK(ia64_debug_match(&v->s, address, 8, IA64_ACCESS_READ, level));
            CHECK(!ia64_debug_match(&v->s, address, 7, IA64_ACCESS_READ, level));
            CHECK(!ia64_debug_match(&v->s, address, 8, IA64_ACCESS_WRITE, level));
            CHECK(!ia64_debug_match(&v->s, address, 8, IA64_ACCESS_READ, (level + 1) % 4));
            v->s.dbr[pair * 2 + 1] &= ~(UINT64_C(1) << 63);
            v->s.dbr[pair * 2 + 1] |= UINT64_C(1) << 62;
            CHECK(ia64_debug_match(&v->s, address, 8, IA64_ACCESS_WRITE, level));
            CHECK(ia64_debug_match(&v->s, address, 8, IA64_ACCESS_READ | IA64_ACCESS_WRITE, level));
            CHECK(!ia64_debug_match(&v->s, address, 8, IA64_ACCESS_READ, level));
        }
    }
    memset(v->s.dbr, 0, sizeof(v->s.dbr));
    v->s.psr = IA64_PSR_DB | IA64_PSR_IC;
    v->s.dbr[0] = address; v->s.dbr[1] = mask | (UINT64_C(1) << 56) | (UINT64_C(1) << 63);
    v->s.gr[10] = address; v->s.gr[8] = 99;
    Ia64Instruction i = instruction(IA64_OP_LD8);
    i.operands.memory = (IA64MemoryOperands){.destination = 8, .base = 10};
    struct ia64_result r = ia64_execute(v, &i);
    CHECK(r.fault == IA64_FAULT_DEBUG && v->s.gr[8] == 99);
    v->s.cr[CR_IVA] = ENTRY; v->s.slot = i.slot;
    ia64_deliver_fault(v, &i, r);
    CHECK(v->s.ip == ENTRY + 0x5900 && v->s.cr[CR_IFA] == address);
    CHECK(v->s.cr[CR_ISR] == ((UINT64_C(1) << 34) | (UINT64_C(1) << 41)));
    v->s.psr = IA64_PSR_DB | IA64_PSR_DD;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    v->s.psr = IA64_PSR_DB;
    v->s.gr[10] = address + 1; v->s.dbr[0] = address + 1;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_DEBUG); /* before alignment */
    v->s.nat[10] = true;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT);
    v->s.nat[10] = false;
    uint64_t va = UINT64_C(0x2000000000200000);
    v->s.psr = IA64_PSR_DB | IA64_PSR_DT | IA64_PSR_IC;
    v->s.gr[10] = va; v->s.dbr[0] = va;
    mmu_map(v, 1, 0, va, address, 12, 0, 0x781);
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ACCESS_BIT);
    v->s.tr[1][0].pte |= 32;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_DEBUG);
    v->s.psr |= IA64_PSR_ID; /* id does not suppress a data breakpoint */
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_DEBUG);
    v->s.psr |= IA64_PSR_DD;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    /* High address bits always compare, even with every mask bit clear. */
    v->s.dbr[0] = 0; v->s.dbr[1] = (UINT64_C(1) << 56) | (UINT64_C(1) << 63);
    CHECK(ia64_debug_match(&v->s, address, 8, IA64_ACCESS_READ, 0));
    CHECK(!ia64_debug_match(&v->s, va, 8, IA64_ACCESS_READ, 0));
    /* Vol.2 Table 5-3: even a deferred translation miss can coexist with
     * a non-deferred debug fault; an unimplemented address precludes it. */
    v->s.ip = ENTRY;
    v->s.psr = IA64_PSR_DB | IA64_PSR_DT | IA64_PSR_IT | IA64_PSR_IC;
    mmu_map(v, 0, 0, ENTRY, ENTRY, 12, 0, UINT64_C(0x7e1) | (UINT64_C(1) << 52));
    v->s.cr[CR_DCR] = UINT64_C(1) << 8;
    v->s.gr[10] = va + 0x1000; v->s.dbr[0] = v->s.gr[10];
    v->s.dbr[1] = UINT64_C(0x81ffffffffffffff);
    i.opcode = IA64_OP_LD8S;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_DEBUG);
    v->s.cr[CR_DCR] |= UINT64_C(1) << 14;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
    v->s.cr[CR_DCR] = 0; v->s.gr[10] = UINT64_C(1) << 50;
    v->s.psr &= ~IA64_PSR_DT;
    v->s.dbr[0] = v->s.gr[10];
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE && v->s.nat[8]);
    /* ld16 and FP extended accesses compare all sixteen bytes. */
    v->s.psr = IA64_PSR_DB; v->s.gr[10] = address;
    v->s.dbr[0] = address + 15;
    i.opcode = IA64_OP_LD16;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_DEBUG);
    i.opcode = IA64_OP_LDFE;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_DEBUG);
    vcpu_destroy(v);
}
static void test_debug_traps(void)
{
    uint64_t nop = UINT64_C(1) << 27;
    /* Armed instruction breakpoints cross the actual engine entry for
     * all enable/suppression combinations, even on a false predicate. */
    for (unsigned flags = 0; flags < 4; ++flags) {
        struct vcpu *v = new_vcpu();
        bundle_write(ENTRY, 0, nop | 1, nop | 1, nop);
        bundle_write(ENTRY + 0x5900, 0, nop, nop, nop);
        v->s.psr = IA64_PSR_IC | (flags & 1 ? IA64_PSR_DB : 0) |
            (flags & 2 ? IA64_PSR_ID : 0);
        v->s.ibr[0] = ENTRY + 15; v->s.ibr[1] = UINT64_C(0x81ffffffffffffff);
        v->s.cr[CR_IVA] = ENTRY; v->s.pr = 1;
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->retired == 1);
        if (flags == 1) {
            CHECK(v->fault.kind == IA64_FAULT_DEBUG && !v->fault.effect_committed);
            CHECK(v->s.cr[CR_IIP] == ENTRY && v->s.cr[CR_ISR] == (UINT64_C(1) << 32));
            CHECK(v->s.ip == ENTRY + 0x5900 && v->s.slot == 1);
        } else {
            CHECK(v->s.ip == ENTRY && v->s.slot == 1);
            CHECK(!(v->s.psr & IA64_PSR_ID));
            if (flags == 3) {
                CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
                CHECK(v->fault.kind == IA64_FAULT_DEBUG && v->s.ip == ENTRY + 0x5900);
                CHECK(((v->s.cr[CR_IPSR] >> 41) & 3) == 1);
            }
        }
        vcpu_destroy(v);
    }
    bundle_write(ENTRY, 0, nop, nop, nop);
    for (unsigned slot = 0; slot < 3; ++slot) {
        struct vcpu *v = new_vcpu();
        v->s.psr = IA64_PSR_IC | IA64_PSR_DB | IA64_PSR_SS | IA64_PSR_ID | IA64_PSR_DA | IA64_PSR_DD;
        v->s.slot = slot; v->s.cr[CR_IVA] = ENTRY;
        v->s.ibr[0] = ENTRY + 15;
        v->s.ibr[1] = UINT64_C(0x81ffffffffffffff);
        struct tier0_stats before, after; tier0_get_stats(&before);
        CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->retired == 1);
        CHECK(v->fault.kind == IA64_FAULT_SINGLE_STEP && v->fault.effect_committed);
        CHECK(v->s.ip == ENTRY + 0x6000 && v->s.cr[CR_IIPA] == ENTRY);
        CHECK(v->s.cr[CR_IIP] == (slot == 2 ? ENTRY + 16 : ENTRY));
        CHECK(((v->s.cr[CR_IPSR] >> 41) & 3) == (slot + 1) % 3);
        CHECK(v->s.cr[CR_ISR] == (8 | ((uint64_t)slot << 41)));
        CHECK(!(v->s.cr[CR_IPSR] & (IA64_PSR_ID | IA64_PSR_DA | IA64_PSR_DD)));
        tier0_get_stats(&after);
        if (selected_engine == ENGINE_LOCKSTEP) CHECK(after.unchecked_instructions == before.unchecked_instructions);
        vcpu_destroy(v);
    }
    struct vcpu *v = new_vcpu();
    v->s.psr = IA64_PSR_IC | IA64_PSR_DB; v->s.cr[CR_IVA] = ENTRY;
    v->s.ibr[0] = ENTRY + 15; v->s.ibr[1] = UINT64_C(0x81ffffffffffffff);
    CHECK(ia64_debug_instruction(v).fault == IA64_FAULT_DEBUG);
    /* Predicate-false instruction still raises an instruction breakpoint. */
    bundle_write(ENTRY, 0, nop | 1, nop, nop);
    struct ia64_result debug = ia64_debug_instruction(v);
    Ia64Instruction i = instruction(IA64_OP_NOP); i.slot = 0;
    ia64_deliver_fault(v, &i, debug);
    CHECK(v->s.cr[CR_ISR] == (UINT64_C(1) << 32) && v->s.cr[CR_IFA] == ENTRY);
    v->s.psr = IA64_PSR_IC | IA64_PSR_TB | IA64_PSR_LP | IA64_PSR_SS;
    uint64_t old = v->s.psr; v->s.psr |= UINT64_C(3) << 32;
    i.opcode = IA64_OP_BR_RET;
    struct ia64_result trap = ia64_completion_trap(v, &i, old, true, ia64_complete());
    CHECK(trap.fault == IA64_FAULT_LOWER_PRIVILEGE && trap.info == 14);
    trap = ia64_completion_trap(v, &i, old, true, ia64_fault(IA64_FAULT_FP_TRAP, 0x2000));
    CHECK(trap.fault == IA64_FAULT_FP_TRAP && trap.info == 0x200f);
    i.opcode = IA64_OP_RFI;
    CHECK(ia64_completion_trap(v, &i, old, true, ia64_complete()).kind == IA64_STEP_COMPLETE);
    vcpu_destroy(v);

    /* Actual demoting br.ret: trap saves the destination PC/CPL, while
     * ISR.ei and IIPA name the completed branch. */
    v = new_vcpu(); v->s.cr[CR_IVA] = ENTRY;
    v->s.psr = IA64_PSR_IC | IA64_PSR_LP | IA64_PSR_TB | IA64_PSR_SS;
    v->s.ar[AR_PFS] = UINT64_C(3) << 62; v->s.br[0] = ENTRY + 0x200;
    bundle_write(ENTRY, 0x11, nop, nop, UINT64_C(0x108001100));
    v->s.slot = 2;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->retired == 1);
    CHECK(v->fault.kind == IA64_FAULT_LOWER_PRIVILEGE && v->fault.effect_committed);
    CHECK(v->s.ip == ENTRY + 0x5e00 && v->s.cr[CR_IIP] == ENTRY + 0x200);
    CHECK(((v->s.cr[CR_IPSR] >> 32) & 3) == 3 && v->s.cr[CR_ISR] == (14 | (UINT64_C(2) << 41)));
    CHECK(v->s.cr[CR_IIPA] == ENTRY);
    /* rfi restores ID/DD and SS but does not trap itself. The first resumed
     * instruction consumes ID; SS then saves the following slot. */
    bundle_write(ENTRY + 0x200, 0, nop, nop, nop);
    bundle_write(ENTRY + 0x5e00, 0x11, nop, nop, UINT64_C(8) << 27);
    v->s.slot = 2;
    v->s.cr[CR_IPSR] |= IA64_PSR_DB | IA64_PSR_ID | IA64_PSR_DD;
    v->s.ibr[0] = ENTRY + 0x200; v->s.ibr[1] = UINT64_C(0x88ffffffffffffff);
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.ip == ENTRY + 0x200 && v->s.slot == 0);
    CHECK(v->s.psr & IA64_PSR_ID);
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.ip == ENTRY + 0x6000);
    CHECK(v->s.cr[CR_IIP] == ENTRY + 0x200 && ((v->s.cr[CR_IPSR] >> 41) & 3) == 1);
    CHECK(!(v->s.cr[CR_IPSR] & (IA64_PSR_ID | IA64_PSR_DD)));
    CHECK(v->s.cr[CR_IIPA] == ENTRY + 0x200 && v->s.cr[CR_ISR] == 8);
    vcpu_destroy(v);

    /* MLX retires one instruction but ISR.ei identifies the X slot (2). */
    v = new_vcpu(); v->s.cr[CR_IVA] = ENTRY; v->s.psr = IA64_PSR_IC | IA64_PSR_SS;
    bundle_write(ENTRY, 4, nop, 0, UINT64_C(6) << 37 | (8u << 6));
    v->s.slot = 1;
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET && v->s.ip == ENTRY + 0x6000);
    CHECK(v->s.cr[CR_IIP] == ENTRY + 16 && v->s.cr[CR_ISR] == (8 | (UINT64_C(2) << 41)));
    vcpu_destroy(v);
}
static void test_debug_rse(void)
{
    struct vcpu *v = new_vcpu();
    uint64_t backing = ENTRY + 0x8800;
    v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = backing;
    v->s.ar[AR_RSC] = 12; /* RSC.PL=3; CPL remains zero. */
    v->s.cfm = 2 | (2u << 7);
    ia64_gr_write(&v->s, 32, 71, false); ia64_gr_write(&v->s, 33, 72, false);
    Ia64Instruction cover = instruction(IA64_OP_COVER), flush = instruction(IA64_OP_FLUSHRS);
    CHECK(ia64_execute(v, &cover).kind == IA64_STEP_COMPLETE);
    v->s.psr = IA64_PSR_DB | IA64_PSR_DD | IA64_PSR_DA;
    v->s.dbr[0] = backing; v->s.dbr[1] = UINT64_C(0x4800000000000000); /* all bytes, write, PL3 */
    struct ia64_result r = ia64_execute(v, &flush);
    CHECK(r.fault == IA64_FAULT_DEBUG && r.info == backing + 8);
    CHECK(r.fault_isr == ((UINT64_C(1) << 33) | (UINT64_C(1) << 37)));
    CHECK(v->s.ar[AR_BSPSTORE] == backing + 8 && v->s.rse_dirty == 1);
    CHECK(!(v->s.psr & (IA64_PSR_DD | IA64_PSR_DA)));
    uint64_t word = 0; CHECK(gm_read(backing, &word, 8) && word == 71);
    v->s.psr |= IA64_PSR_DD;
    CHECK(ia64_execute(v, &flush).kind == IA64_STEP_COMPLETE && !v->s.rse_dirty);
    vcpu_destroy(v);
}
static void test_debug_pal(void)
{
    for (unsigned virt = 0; virt < 2; ++virt) {
        struct vcpu *v = new_vcpu();
        uint64_t va = UINT64_C(0x2000000000200000), buf = ENTRY + 0x8000;
        if (virt) {
            v->s.psr = IA64_PSR_IC | IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_RT;
            mmu_map(v, 0, 0, va, ENTRY, 12, 0, 0x7e1);
            mmu_map(v, 1, 0, va, ENTRY, 12, 0, 0x7e1);
            mmu_map(v, 1, 1, va + 0x8000, buf, 12, 0, 0x7e1);
        }
        uint64_t caller = 8 | (4u << 7);
        v->s.cfm = caller; v->s.ar[AR_BSP] = virt ? va + 0x8000 : buf;
        v->s.ar[AR_BSPSTORE] = v->s.ar[AR_BSP];
        v->s.rse_bof = 92;
        for (unsigned n = 0; n < 4; ++n) ia64_gr_write(&v->s, 32 + n, n + 70, n == 1);
        Ia64Instruction call = instruction(IA64_OP_BR_CALL); call.operands.branch.link = 0;
        CHECK(ia64_rse_call(v, &call, virt ? va : ENTRY).kind == IA64_STEP_BRANCH);
        CHECK(v->s.rse_bof == 0 && v->s.rse_dirty == 4);
        /* Argument registers are the callee inputs, after br.call renames. */
        ia64_gr_write(&v->s, 28, PAL_BRAND_INFO, false);
        ia64_gr_write(&v->s, 32, PAL_BRAND_INFO, false);
        ia64_gr_write(&v->s, 33, 0, false);
        ia64_gr_write(&v->s, 34, virt ? va + 0x8000 + 0x200 : buf + 0x200, false);
        ia64_gr_write(&v->s, 35, 0, false);
        /* Spill the caller's locals; PAL must fill them on real return. */
        Ia64Instruction flush = instruction(IA64_OP_FLUSHRS);
        CHECK(ia64_execute(v, &flush).kind == IA64_STEP_COMPLETE && v->s.rse_dirty == 0);
        v->s.ip = virt ? va : ENTRY; v->cfg.pal_entry = ENTRY;
        Ia64Instruction portal = pal_break_insn(v->s.ip);
        /* Failure to restore the caller must precede procedure output. */
        struct ia64_state saved = v->s;
        uint8_t before_buffer = ram[0x8200];
        v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = ENTRY + RAM_SIZE + 32;
        struct ia64_state invalid = v->s;
        if (!virt) {
            CHECK(ia64_execute(v, &portal).kind == IA64_STEP_EXIT);
            CHECK(!memcmp(&invalid, &v->s, sizeof(invalid)) && ram[0x8200] == before_buffer);
        } else {
            CHECK(ia64_execute(v, &portal).kind == IA64_STEP_BRANCH);
            CHECK(v->s.rse_missing == 4 && v->s.rse_cfle);
            CHECK(ia64_rse_resume(v).fault == IA64_FAULT_ALT_DTLB);
        }
        v->s = saved;
        struct ia64_result r = ia64_execute(v, &portal);
        CHECK(r.kind == IA64_STEP_BRANCH && r.info == ENTRY + 16);
        CHECK(v->s.cfm == caller && v->s.rse_bof == 92 && v->s.ar[AR_BSP] == (virt ? va + 0x8000 : buf));
        CHECK(v->s.gr[8] == 0 && ram[0x8200] == 'V');
        CHECK(ia64_rse_resume(v).kind == IA64_STEP_COMPLETE);
        for (unsigned n = 0; n < 4; ++n) {
            CHECK(ia64_nat_read(&v->s, 32 + n) == (n == 1));
            if (n != 1) CHECK(ia64_gr_read(&v->s, 32 + n) == n + 70);
        }
        vcpu_destroy(v);
    }
    struct vcpu *v = new_vcpu();
    uint64_t buf = ENTRY + 0x9000, va = UINT64_C(0x4000000000800000);
    mmu_map(v, 1, 3, va, ENTRY + 0x8000, 12, 0xabcdef, 0x7e1);
    pal_args(v, PAL_VM_TR_READ, 3, 1, buf);
    CHECK(ia64_pal_dispatch(v).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0 && v->s.gr[9] == 15);
    uint64_t words[4]; CHECK(gm_read(buf, words, sizeof(words)));
    CHECK(words[0] == v->s.tr[1][3].pte && words[1] == v->s.tr[1][3].itir);
    CHECK(words[2] == (v->s.tr[1][3].va | 1));
    CHECK(words[3] == (((uint64_t)v->s.tr[1][3].rid << 8) | (12u << 2)));
    vcpu_destroy(v);
}
static void test_debug_ia32_state(void)
{
    struct vcpu *v = new_vcpu();
    const unsigned ars[] = {AR_FCR, AR_SSD, AR_CFLG, AR_FSR, AR_FIR, AR_FDR};
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(ars); ++n) {
        Ia64Instruction i = instruction(IA64_OP_MOV_GRAR);
        i.unit = IA64_UNIT_M; i.operands.system.source = ars[n];
        uint64_t value = ars[n] == AR_CFLG ? UINT64_C(0x000007ffe00501ff) : UINT64_C(0x123456789abcdef0);
        v->s.gr[8] = value;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        i.opcode = IA64_OP_MOV_ARGR; v->s.nat[8] = true;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        CHECK(v->s.gr[8] == value && !v->s.nat[8]);
        i.unit = IA64_UNIT_I;
        CHECK(ia64_execute(v, &i).fault == IA64_FAULT_ILLEGAL);
    }
    /* Windows 2003 context save: SDM Vol.2 Table 10-3 EFLAG writes. */
    {
        uint64_t saved = v->s.gr[8];
        Ia64Instruction e = instruction(IA64_OP_MOV_GRAR);
        e.unit = IA64_UNIT_M; e.operands.system.source = AR_EFLAG;
        v->s.psr = 0; v->s.gr[8] = UINT64_C(0xfedcba9876543fff);
        CHECK(ia64_execute(v, &e).kind == IA64_STEP_COMPLETE);
        CHECK(v->s.ar[AR_EFLAG] == UINT64_C(0xfedcba9876543fff)); /* PL0: all bits. */
        e.opcode = IA64_OP_MOV_ARGR; v->s.gr[8] = 0;
        CHECK(ia64_execute(v, &e).kind == IA64_STEP_COMPLETE && v->s.gr[8] == UINT64_C(0xfedcba9876543fff));
        e.unit = IA64_UNIT_I;
        CHECK(ia64_execute(v, &e).fault == IA64_FAULT_ILLEGAL);
        e.unit = IA64_UNIT_M; e.opcode = IA64_OP_MOV_GRAR;
        /* PL3 with IOPL=3: IF is writable, IOPL and VM are kept. */
        v->s.ar[AR_EFLAG] = (UINT64_C(3) << 12) | (UINT64_C(1) << 17);
        v->s.psr = UINT64_C(3) << 32; v->s.gr[8] = UINT64_C(1) << 9 | 0x801;
        CHECK(ia64_execute(v, &e).kind == IA64_STEP_COMPLETE);
        CHECK(v->s.ar[AR_EFLAG] == ((UINT64_C(3) << 12) | (UINT64_C(1) << 17) | (UINT64_C(1) << 9) | 0x801));
        /* PL3 with IOPL=1: IF, IOPL and VM are all kept. */
        v->s.ar[AR_EFLAG] = UINT64_C(1) << 12; v->s.gr[8] = UINT64_C(0x23200) | 0x40;
        CHECK(ia64_execute(v, &e).kind == IA64_STEP_COMPLETE && v->s.ar[AR_EFLAG] == ((UINT64_C(1) << 12) | 0x40));
        /* PL1 with IOPL=1: IF writable (clears), IOPL kept. */
        v->s.ar[AR_EFLAG] = (UINT64_C(1) << 12) | (UINT64_C(1) << 9); v->s.psr = UINT64_C(1) << 32; v->s.gr[8] = 0;
        CHECK(ia64_execute(v, &e).kind == IA64_STEP_COMPLETE && v->s.ar[AR_EFLAG] == UINT64_C(1) << 12);
        v->s.nat[8] = true;
        CHECK(ia64_execute(v, &e).fault == IA64_FAULT_NAT);
        v->s.nat[8] = false; v->s.psr = 0; v->s.gr[8] = saved;
    }
    /* Windows 2003 ntoskrnl: fwb completes at any CPL, no fault. */
    {
        Ia64Instruction f = instruction(IA64_OP_FWB);
        f.unit = IA64_UNIT_M; v->s.psr = UINT64_C(3) << 32;
        CHECK(ia64_opcode_supported(IA64_OP_FWB));
        CHECK(ia64_execute(v, &f).kind == IA64_STEP_COMPLETE);
        v->s.psr = 0;
    }
    Ia64Instruction i = instruction(IA64_OP_MOV_GRAR);
    i.unit = IA64_UNIT_M; i.operands.system.source = AR_CFLG;
    v->s.psr = UINT64_C(3) << 32; v->s.nat[8] = true;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_NAT);
    v->s.nat[8] = false;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_RESERVED);
    v->s.gr[8] = 0;
    CHECK(ia64_execute(v, &i).fault == IA64_FAULT_PRIVILEGED_REG);
    i.opcode = IA64_OP_MOV_ARGR;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    vcpu_destroy(v);
}
static void test_debug_pal_ia32(void)
{
    test_debug_brp(); test_debug_data_enables(); test_debug_registers(); test_debug_traps(); test_debug_rse(); test_debug_pal(); test_debug_ia32_state();
}
