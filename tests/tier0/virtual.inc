/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_TIER0_VIRTUAL_TESTS
#define IA64VMM_TIER0_VIRTUAL_TESTS
#include "ia64/mmu.h"
#include "ia64/tlb.h"
#include "engine/interp/interp.h"

#define VIRTUAL_IP UINT64_C(0xe000000002000000)
#define VIRTUAL_IVA UINT64_C(0x0000000000180000)
#define FETCH_FLAGS UINT64_C(0x7e1)

static void virtual_map(struct vcpu *v, unsigned side, unsigned index, uint64_t va,
                        gpa_t pa, unsigned shift, uint64_t flags)
{
    v->s.tr[side][index] = (struct ia64_tlb_entry){
        .va = va & ia64_low_mask(61) & ~ia64_low_mask(shift), .pte = pa | flags,
        .itir = shift << 2, .rid = (v->s.rr[va >> 61] >> 8) & 0xffffff,
        .page_shift = shift, .valid = true};
}
static void virtual_start(struct vcpu *v, enum engine_kind engine, uint64_t ip)
{
    restart(v, engine, ip);
    v->s.psr = IA64_PSR_IT | IA64_PSR_IC | IA64_PSR_BN;
    v->s.rr[ip >> 61] = 0x123400 | 48;
    virtual_map(v, 0, 0, ip, BASE, 12, FETCH_FLAGS);
    v->s.cr[CR_IVA] = VIRTUAL_IVA;
    virtual_map(v, 0, 1, VIRTUAL_IVA, BASE + 0x18000, 15, FETCH_FLAGS);
    for (unsigned offset = 0; offset < 0x8000; offset += 0x100)
        bundle(BASE + 0x18000 + offset, 1, nop, nop, nop);
}
static void virtual_compare(struct vcpu *v, struct vcpu *ref, unsigned budget)
{
    /* These comparisons have no stores/device I/O: the reference can share RAM.
     * The fixtures edit TLB entries, RR and PKR directly: log that, as the
     * architectural writers do. */
    ia64_mmu_changed(v, IA64_MMU_ALL, 0, 0);
    *ref = *v; ref->cfg.engine = ENGINE_INTERP;
    ref->check_workspace = NULL; ref->check_workspace_pages = 0;
    struct vcpu_exit expected = interp_run(ref, budget);
    struct vcpu_exit actual = engine_run(v, budget);
    CHECK(actual.reason == expected.reason && actual.ip == expected.ip && actual.slot == expected.slot);
    CHECK(!memcmp(&v->s, &ref->s, sizeof(v->s)) && v->retired == ref->retired);
    CHECK(v->fault.kind == ref->fault.kind && v->fault.ip == ref->fault.ip);
    CHECK(v->last_successful_bundle == ref->last_successful_bundle);
}
static void virtual_aliases(struct vcpu *v, struct vcpu *ref, enum engine_kind engine)
{
    uint64_t mov_ip = (UINT64_C(0x30) << 27) | (8u << 6);
    for (unsigned region = 0; region < 8; ++region) {
        uint64_t ip = ((uint64_t)region << 61) | 0x2000000;
        code_cache_flush(); virtual_start(v, engine, ip);
        bundle(BASE, 0x11, nop, mov_ip, validator_branch(ip, ip + 16));
        bundle(BASE + 16, 0x11, nop, nop, validator_branch(ip + 16, ip));
        struct tier0_stats before, after; tier0_get_stats(&before);
        virtual_compare(v, ref, 12); tier0_get_stats(&after);
        CHECK(v->s.gr[8] == ip && v->s.ip == ip);
        CHECK(after.executed_instructions == before.executed_instructions + 12);
        CHECK(after.virtual_instructions == before.virtual_instructions + 12);
        CHECK(tier0_page_has_code(BASE));
        const void *allocation, *entry; bool executable;
        CHECK(tier0_test_mapping(v, &allocation, &entry, &executable) && executable);
    }
    /* Both virtual aliases coexist; neither inherits the other's mov-IP value. */
    uint64_t alias = VIRTUAL_IP + 0x100000;
    virtual_start(v, engine, VIRTUAL_IP);
    virtual_map(v, 0, 2, alias, BASE, 12, FETCH_FLAGS);
    bundle(BASE, 0x11, nop, mov_ip, validator_branch(VIRTUAL_IP, VIRTUAL_IP));
    virtual_compare(v, ref, 6);
    v->s.ip = alias; virtual_compare(v, ref, 6); CHECK(v->s.gr[8] == alias);
    v->s.ip = VIRTUAL_IP; virtual_compare(v, ref, 3); CHECK(v->s.gr[8] == VIRTUAL_IP);
}
static void virtual_mapping_changes(struct vcpu *v, struct vcpu *ref, enum engine_kind engine)
{
    code_cache_flush(); virtual_start(v, engine, VIRTUAL_IP);
    bundle(BASE, 0x11, nop, add(8,9,0), validator_branch(VIRTUAL_IP, VIRTUAL_IP));
    bundle(BASE + 4096, 0x11, nop, add(8,10,0), validator_branch(VIRTUAL_IP, VIRTUAL_IP));
    v->s.gr[9] = 11; v->s.gr[10] = 22;
    virtual_compare(v, ref, 6); CHECK(v->s.gr[8] == 11);
    v->s.tr[0][0].pte = BASE + 4096 + FETCH_FLAGS;
    virtual_compare(v, ref, 6); CHECK(v->s.gr[8] == 22);
    v->s.tr[0][0].valid = false;
    ia64_tc_insert(v, VIRTUAL_IP, 0, BASE | FETCH_FLAGS, 48);
    virtual_compare(v, ref, 6); CHECK(v->s.gr[8] == 11);
    ia64_tc_insert(v, VIRTUAL_IP, 0, (BASE + 4096) | FETCH_FLAGS, 48);
    virtual_compare(v, ref, 6); CHECK(v->s.gr[8] == 22);
    /* Replacing an RR selects another RID's entry, including the same PA. */
    v->s.rr[7] ^= 0x100;
    ia64_tc_insert(v, VIRTUAL_IP, 0, BASE | FETCH_FLAGS, 48);
    virtual_compare(v, ref, 6); CHECK(v->s.gr[8] == 11);
    v->s.cr[CR_PTA] ^= 0x8000; virtual_compare(v, ref, 3);
    /* Rights, access bits, keys, privilege and purges must reject a warm key. */
    const enum ia64_fault_kind faults[] = {IA64_FAULT_PAGE_NOT_PRESENT, IA64_FAULT_INST_NAT,
        IA64_FAULT_ACCESS_RIGHTS, IA64_FAULT_ACCESS_BIT, IA64_FAULT_KEY_MISS,
        IA64_FAULT_KEY_PERMISSION, IA64_FAULT_ACCESS_RIGHTS, IA64_FAULT_ALT_ITLB};
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(faults); ++n) {
        virtual_start(v, engine, VIRTUAL_IP); v->s.gr[9] = 11;
        virtual_compare(v, ref, 6);
        switch (n) {
        case 0: v->s.tr[0][0].pte &= ~UINT64_C(1); break;
        case 1: v->s.tr[0][0].pte |= 7u << 2; break;
        case 2: v->s.tr[0][0].pte &= ~(UINT64_C(7) << 9); break;
        case 3: v->s.tr[0][0].pte &= ~UINT64_C(32); break;
        case 4: v->s.psr |= IA64_PSR_PK; v->s.pkr[0] = 1; v->s.tr[0][0].itir |= 256; break;
        case 5: v->s.psr |= IA64_PSR_PK; v->s.pkr[0] = 1;
            v->s.pkr[1] = 0x109; v->s.tr[0][0].itir |= 256; break;
        case 6: v->s.psr |= UINT64_C(3) << 32; v->s.tr[0][0].pte &= ~(UINT64_C(3) << 7); break;
        default: v->s.tr[0][0].valid = false; break;
        }
        virtual_compare(v, ref, 1);
        if (v->fault.kind != faults[n] || v->fault.ip != VIRTUAL_IP)
            LOGE("fetch case %u got=%s ip=%llx expected=%s", n, ia64_fault_name(v->fault.kind),
                (unsigned long long)v->fault.ip, ia64_fault_name(faults[n]));
        CHECK(v->fault.kind == faults[n] && v->fault.ip == VIRTUAL_IP);
        CHECK(v->s.cr[CR_IIP] == VIRTUAL_IP && v->s.gr[8] == 11);
    }
    const Ia64Opcode purges[] = {IA64_OP_PTC_L, IA64_OP_PTC_G, IA64_OP_PTC_GA, IA64_OP_PTC_E, IA64_OP_PTR_I};
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(purges); ++n) {
        virtual_start(v, engine, VIRTUAL_IP);
        if (purges[n] != IA64_OP_PTR_I) {
            v->s.tr[0][0].valid = false;
            ia64_tc_insert(v, VIRTUAL_IP, 0, BASE | FETCH_FLAGS, 48);
        }
        virtual_compare(v, ref, 6);
        Ia64Instruction i = {.opcode = purges[n], .valid = true, .unit = IA64_UNIT_M,
            .operands.system = {.source = 9, .register_index = 10}};
        v->s.gr[9] = 48; v->s.gr[10] = VIRTUAL_IP;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
        virtual_compare(v, ref, 1); CHECK(v->fault.kind == IA64_FAULT_ALT_ITLB);
    }
    /* An itc.i inside a compiled region changes the next slot's physical source
     * (with PSR.ic=0: itc under PSR.ic=1 is an Illegal Operation, SDM Vol.3). */
    virtual_start(v, engine, VIRTUAL_IP); v->s.tr[0][0].valid = false; v->s.psr &= ~IA64_PSR_IC;
    ia64_tc_insert(v, VIRTUAL_IP, 0, BASE | FETCH_FLAGS, 48);
    uint64_t itc = (UINT64_C(1) << 37) | (UINT64_C(0x2f) << 27) | (9u << 13);
    bundle(BASE, 0x0b, itc, add(8,11,0), nop);
    bundle(BASE + 4096, 1, nop, add(8,10,0), nop);
    v->s.gr[9] = (BASE + 4096) | FETCH_FLAGS; v->s.gr[10] = 22; v->s.gr[11] = 11;
    v->s.cr[CR_IFA] = VIRTUAL_IP; v->s.cr[CR_ITIR] = 48;
    virtual_compare(v, ref, 2); CHECK(v->s.gr[8] == 22);
}
static void virtual_page_edge(struct vcpu *v, struct vcpu *ref, enum engine_kind engine)
{
    virtual_start(v, engine, VIRTUAL_IP);
    bundle(BASE + 4096 - 16, 1, nop, add(8,9,0), nop);
    bundle(BASE + 4096, 1, add(8,10,0), nop, nop);
    v->s.ip += 4096 - 16; v->s.gr[9] = 11; v->s.gr[10] = 22;
    struct tier0_stats before, after; tier0_get_stats(&before);
    virtual_compare(v, ref, 4); tier0_get_stats(&after);
    CHECK(v->fault.kind == IA64_FAULT_ALT_ITLB && v->fault.ip == VIRTUAL_IP + 4096);
    CHECK(v->s.gr[8] == 11 && after.fetch_faults == before.fetch_faults + 1);
    CHECK(!tier0_page_has_code(BASE + 4096));
}
static void virtual_walker(struct vcpu *v, struct vcpu *ref, enum engine_kind engine)
{
    for (unsigned format = 0; format < 2; ++format) {
        virtual_start(v, engine, VIRTUAL_IP);
        v->s.psr |= IA64_PSR_DT; v->s.tr[0][0].valid = false;
        v->s.rr[7] = 49; v->s.cr[CR_PTA] = DATA | (15u << 2) | 1 | (format ? 0x100 : 0);
        uint64_t hash = ia64_thash(&v->s, VIRTUAL_IP);
        virtual_map(v, 1, 0, hash, DATA, 15, FETCH_FLAGS);
        uint64_t words[] = {BASE | FETCH_FLAGS, 48, ia64_ttag(&v->s, VIRTUAL_IP), 0};
        CHECK(gm_write(DATA + (hash & 0x7fff), words, format ? 32 : 8));
        bundle(BASE, 0x11, nop, add(8,9,0), validator_branch(VIRTUAL_IP, VIRTUAL_IP));
        v->s.gr[9] = 42; virtual_compare(v, ref, 6); CHECK(v->s.gr[8] == 42);
        /* Compiling the last bundle cannot insert the next page's TC entry. */
        v->s.ip = VIRTUAL_IP + 4096 - 16;
        bundle(BASE + 4096 - 16, 1, nop, nop, nop);
        unsigned before = v->s.tc_next[0]; virtual_compare(v, ref, 3);
        CHECK(v->s.tc_next[0] == before && !ia64_tlb_find(&v->s, VIRTUAL_IP + 4096, 0));
    }
}
static void virtual_interrupts(struct vcpu *v, struct vcpu *ref, enum engine_kind engine)
{
    virtual_start(v, engine, VIRTUAL_IP);
    bundle(BASE, 0x11, nop, add(8,9,0), validator_branch(VIRTUAL_IP, VIRTUAL_IP));
    bundle(BASE + 0x1b000, 0x11, nop, add(16,16,17), UINT64_C(8) << 27);
    v->s.gr[16] = 101; v->s.gr[17] = 7;
    v->s.bank_gr[0] = 202; v->s.bank_gr[1] = 3;
    v->s.psr |= IA64_PSR_I; v->s.gr[9] = 11;
    vcpu_raise_external(v, 150);
    struct tier0_stats before, after; tier0_get_stats(&before);
    virtual_compare(v, ref, 2);
    CHECK(!(v->s.psr & (IA64_PSR_IC | IA64_PSR_BN)) && v->s.gr[16] == 205);
    CHECK(ia64_ivr_read(v) == 150); ia64_eoi(v);
    v->s.cr[CR_IPSR] |= IA64_PSR_DA | IA64_PSR_DD | IA64_PSR_ED | IA64_PSR_IA | IA64_PSR_ID;
    virtual_compare(v, ref, 1); CHECK(v->s.ip == VIRTUAL_IP && v->s.gr[16] == 101);
    virtual_compare(v, ref, 6); tier0_get_stats(&after);
    CHECK(v->s.gr[8] == 11 && !(v->s.psr & (IA64_PSR_DA | IA64_PSR_DD | IA64_PSR_ED | IA64_PSR_IA | IA64_PSR_ID)));
    CHECK(after.handler_instructions == before.handler_instructions + 3);
    CHECK(after.interpreter_instructions[TIER0_INTERP_SINGLE_INSTRUCTION] ==
        before.interpreter_instructions[TIER0_INTERP_SINGLE_INSTRUCTION] + 1);
    CHECK(after.unchecked_instructions == before.unchecked_instructions);
    /* A deterministic timer match inside a region enters the same banked handler. */
    virtual_start(v, engine, VIRTUAL_IP); v->s.psr |= IA64_PSR_I;
    v->s.cr[CR_ITV] = 150; v->s.cr[CR_ITM] = 2; ia64_timer_program(v);
    virtual_compare(v, ref, 4); CHECK(v->timer_fired && v->s.ip == VIRTUAL_IVA + 0x3000);
}
static void test_virtual(struct vcpu *v, struct vcpu *ref)
{
    for (enum engine_kind engine = ENGINE_TIER0; engine <= ENGINE_LOCKSTEP; ++engine) {
        virtual_aliases(v, ref, engine); virtual_mapping_changes(v, ref, engine);
        virtual_page_edge(v, ref, engine); virtual_walker(v, ref, engine);
        virtual_interrupts(v, ref, engine);
    }
}
#endif
