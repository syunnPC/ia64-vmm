/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PAL-based interruptions. SDM Rev.2.3 Vol.2:
 * §11.4.1-2 INIT, held while PSR.mc = 1; §11.5.1-2 PMI, masked only by
 * PSR.ic = 0, PAL vectors 4-15 above SAL vectors 1-3 above vector 0
 * (Tables 11-14, 11-15); §11.3.1 machine checks, held while PSR.mc = 1;
 * Table 5-6: MCA > INIT > PMI > external interrupts; §11.6: INIT and PMI
 * end LIGHT_HALT; Table 4-1: a same-stream TR overlap of itc/itr, and a
 * TR overlap of ptc.l or the local ptc.g/ptc.ga, must machine check.
 * Entry: IIP/IPSR/IFS.v=0 only with PSR.ic = 1; PSR keeps mc, mfl, mfh
 * and pk, INIT and MCA set mc, every other bit is 0. */
#include "ia64/smp.h"
#include "ia64/tlb.h"
#define PAL_EVENT_INIT_IP (ENTRY + 0x7000)
#define PAL_EVENT_CHECK_IP (ENTRY + 0x7100)
#define PAL_EVENT_PMI_IP (ENTRY + 0x7200)
#define PAL_EVENT_KEPT (IA64_PSR_MC | IA64_PSR_MFL | IA64_PSR_MFH | IA64_PSR_PK)

static struct vcpu *pal_event_vcpu(bool entries)
{
    struct vcpu_config cfg = {.engine = selected_engine, .itc_icount = true, .cpu_model = IA64_CPU_MONTECITO};
    if (entries) { cfg.pale_init = PAL_EVENT_INIT_IP; cfg.pale_check = PAL_EVENT_CHECK_IP; }
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    struct ia64_entry_regs regs = {.ip = ENTRY, .rsc = 3};
    vcpu_reset(v, &regs);
    if (entries) ia64_pal_service_write(v, IA64VMM_SVC_PAL_PALE_PMI, PAL_EVENT_PMI_IP);
    return v;
}
/* The interrupted context: an IP and slot, PSR with interruption state
 * collection, and distinct banked registers and interruption CRs. */
static uint64_t pal_event_context(struct vcpu *v, uint64_t psr)
{
    v->s.ip = ENTRY + 0x40; v->s.slot = 1;
    v->s.psr = psr;
    v->s.gr[16] = 0x1016; v->s.bank_gr[0] = 0xb016;
    v->s.cr[CR_IIP] = 0x11; v->s.cr[CR_IPSR] = 0x22; v->s.cr[CR_IFS] = (UINT64_C(1) << 63) | 5;
    v->s.cr[CR_IFA] = 0x55;
    v->s.cfm = 7 | (3u << 7);
    return psr;
}
static void pal_event_entered(struct vcpu *v, uint64_t ip, uint64_t psr, uint64_t old_psr, bool collected)
{
    CHECK(v->s.ip == ip && v->s.slot == 0 && v->s.psr == psr);
    if (collected) {
        CHECK(v->s.cr[CR_IIP] == ENTRY + 0x40 && v->s.cr[CR_IPSR] == (old_psr | (UINT64_C(1) << 41)));
        CHECK(!(v->s.cr[CR_IFS] >> 63));
    } else {
        CHECK(v->s.cr[CR_IIP] == 0x11 && v->s.cr[CR_IPSR] == 0x22 && v->s.cr[CR_IFS] == ((UINT64_C(1) << 63) | 5));
    }
    /* The frame is unchanged: no cover before PAL. */
    CHECK(v->s.cfm == (7 | (3u << 7)));
    /* Bank 0 is selected. */
    if (old_psr & IA64_PSR_BN) CHECK(v->s.gr[16] == 0xb016 && v->s.bank_gr[0] == 0x1016);
    else CHECK(v->s.gr[16] == 0x1016 && v->s.bank_gr[0] == 0xb016);
}
static uint64_t pal_event_register(struct vcpu *v, unsigned offset)
{ return ia64_pal_service_read(v, offset); }

static void test_pal_init(void)
{
    const uint64_t modes[] = {
        IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN | IA64_PSR_DT | IA64_PSR_IT | IA64_PSR_RT |
            IA64_PSR_PK | IA64_PSR_MFL | IA64_PSR_MFH | IA64_PSR_DFH | (UINT64_C(3) << 32),
        IA64_PSR_IC | IA64_PSR_BN, IA64_PSR_I | IA64_PSR_BN, 0,
    };
    for (unsigned m = 0; m < IA64VMM_ARRAY_SIZE(modes); ++m) {
        struct vcpu *v = pal_event_vcpu(true);
        uint64_t psr = pal_event_context(v, modes[m]);
        CHECK(!vcpu_pal_interrupt_pending(v) && !ia64_external_pending(v));
        CHECK(vcpu_accept_pal_interrupt(v, IA64_IRQ_INIT, 0));
        /* PSR.i and TPR do not mask INIT; it ends LIGHT_HALT. */
        v->s.cr[CR_TPR] = UINT64_C(1) << 16;
        CHECK(vcpu_pal_interrupt_pending(v) && vcpu_halt_wake_pending(v));
        CHECK(ia64_external_requested(v) && ia64_external_pending(v));
        CHECK(ia64_external_deliver(v));
        bool collected = psr & IA64_PSR_IC;
        pal_event_entered(v, PAL_EVENT_INIT_IP, IA64_PSR_MC | (psr & PAL_EVENT_KEPT), psr, collected);
        CHECK(v->s.cr[CR_IFA] == 0x55);
        CHECK(pal_event_register(v, IA64VMM_SVC_PAL_EVENT) ==
              (IA64VMM_PAL_EVENT_INIT | (collected ? IA64VMM_PAL_EVENT_COLLECTED : 0)));
        CHECK(!vcpu_pal_interrupt_pending(v) && !ia64_external_deliver(v));
        vcpu_destroy(v);
    }
    /* PSR.mc = 1 holds INIT; an unmasked external interrupt is still taken. */
    struct vcpu *v = pal_event_vcpu(true);
    v->s.cr[CR_IVA] = ENTRY + 0x8000;
    uint64_t psr = pal_event_context(v, IA64_PSR_MC | IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN);
    CHECK(vcpu_accept_pal_interrupt(v, IA64_IRQ_INIT, 0));
    CHECK(!ia64_external_pending(v) && !ia64_external_deliver(v) && v->s.ip == ENTRY + 0x40);
    vcpu_raise_external(v, 0x50);
    CHECK(ia64_external_deliver(v) && v->s.ip == ENTRY + 0x8000 + 0x3000);
    CHECK(vcpu_pal_interrupt_pending(v) && ia64_ivr_read(v) == 0x50);
    ia64_eoi(v);
    /* Held until PSR.mc becomes 0. */
    psr = pal_event_context(v, IA64_PSR_IC | IA64_PSR_BN);
    CHECK(ia64_external_deliver(v));
    pal_event_entered(v, PAL_EVENT_INIT_IP, IA64_PSR_MC, psr, true);
    vcpu_destroy(v);
    /* Without a PALE_INIT (the reference firmware) INIT stops the vCPU. */
    v = pal_event_vcpu(false);
    CHECK(!vcpu_accept_pal_interrupt(v, IA64_IRQ_INIT, 0));
    CHECK(v->requested_exit == VCPU_EXIT_UNIMPLEMENTED + 1 && !vcpu_pal_interrupt_pending(v));
    vcpu_destroy(v);
}

static void test_pal_pmi(void)
{
    struct vcpu *v = pal_event_vcpu(true);
    /* Vectors 0-15 only. */
    CHECK(!vcpu_accept_pal_interrupt(v, IA64_IRQ_PMI, 16) && !vcpu_pal_interrupt_pending(v));
    CHECK(!v->requested_exit);
    const unsigned latched[] = {0, 2, 7, 1, 15};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(latched); ++i)
        CHECK(vcpu_accept_pal_interrupt(v, IA64_IRQ_PMI, latched[i]));
    /* Masked by PSR.ic = 0 alone, even with PSR.i = 1; a wake source. */
    pal_event_context(v, IA64_PSR_I | IA64_PSR_BN);
    CHECK(vcpu_halt_wake_pending(v) && !ia64_external_pending(v) && !ia64_external_deliver(v));
    /* Highest vector first: 15 > 7 > 2 > 1 > 0. PSR.i does not mask PMI;
     * PSR.mc is kept and does not mask it either. */
    const unsigned order[] = {15, 7, 2, 1, 0};
    const uint64_t modes[] = {IA64_PSR_IC | IA64_PSR_BN, IA64_PSR_IC | IA64_PSR_MC | IA64_PSR_BN,
        IA64_PSR_IC | IA64_PSR_I | IA64_PSR_DT | IA64_PSR_MFL | IA64_PSR_MFH | IA64_PSR_PK,
        IA64_PSR_IC | IA64_PSR_MC | IA64_PSR_I | IA64_PSR_IT | (UINT64_C(2) << 32),
        IA64_PSR_IC};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(order); ++i) {
        uint64_t psr = pal_event_context(v, modes[i]);
        CHECK(ia64_external_pending(v) && ia64_external_deliver(v));
        pal_event_entered(v, PAL_EVENT_PMI_IP, psr & PAL_EVENT_KEPT, psr, true);
        CHECK(pal_event_register(v, IA64VMM_SVC_PAL_PMI_VECTOR) == order[i]);
        CHECK(vcpu_pal_interrupt_pending(v) == (i + 1 < IA64VMM_ARRAY_SIZE(order)));
    }
    /* A PMI never records an INIT or machine check event. */
    CHECK(pal_event_register(v, IA64VMM_SVC_PAL_EVENT) == 0);
    /* One recognition per vector however often it is sent (SAL §6.3). */
    CHECK(vcpu_accept_pal_interrupt(v, IA64_IRQ_PMI, 3) && vcpu_accept_pal_interrupt(v, IA64_IRQ_PMI, 3));
    pal_event_context(v, IA64_PSR_IC);
    CHECK(ia64_external_deliver(v) && pal_event_register(v, IA64VMM_SVC_PAL_PMI_VECTOR) == 3);
    pal_event_context(v, IA64_PSR_IC);
    CHECK(!ia64_external_deliver(v));
    vcpu_destroy(v);
    /* A PMI before PALE_PMI registration stops the vCPU. */
    v = pal_event_vcpu(false);
    CHECK(!vcpu_accept_pal_interrupt(v, IA64_IRQ_PMI, 1));
    CHECK(v->requested_exit == VCPU_EXIT_UNIMPLEMENTED + 1 && !vcpu_pal_interrupt_pending(v));
    vcpu_destroy(v);
}

/* itr.d dtr[r10] = r9 overlapping DTR 0, from a state like test_tr_overlap. */
static Ia64Instruction pal_event_overlap(struct vcpu *v)
{
    v->s.rr[0] = v->s.rr[1] = 0x100030;
    v->s.tr[1][0] = (struct ia64_tlb_entry){.va = 0x900000, .rid = 0x1000,
        .page_shift = 20, .itir = 80, .pte = 0x900661, .valid = true};
    Ia64Instruction insn = instruction(IA64_OP_ITR_D);
    insn.unit = IA64_UNIT_M; insn.slot = 0;
    insn.operands.system.source = 9; insn.operands.system.register_index = 10;
    v->s.gr[9] = 0x940661; v->s.gr[10] = 1;
    v->s.cr[CR_IFA] = (UINT64_C(1) << 61) | 0x940000;
    v->s.cr[CR_ITIR] = 18u << 2;
    return insn;
}
static void test_pal_machine_check(void)
{
    /* With a PALE_CHECK the overlap raises a machine check before the
     * insert: neither insert nor purge (Table 4-1 "may"). itr requires
     * PSR.ic = 0, so IIP/IPSR/IFS are not collected. */
    struct vcpu *v = pal_event_vcpu(true);
    uint64_t psr = pal_event_context(v, IA64_PSR_BN | IA64_PSR_DT | IA64_PSR_RT | IA64_PSR_MFH);
    Ia64Instruction insn = pal_event_overlap(v);
    uint64_t seq = v->mmu_seq;
    CHECK(vcpu_accept_pal_interrupt(v, IA64_IRQ_INIT, 0));
    ia64_pal_service_write(v, IA64VMM_SVC_PAL_EXPECTED, 1);
    struct ia64_result r = ia64_execute(v, &insn);
    CHECK(r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_MACHINE_CHECK);
    CHECK(v->mmu_seq == seq && !v->s.tr[1][1].valid && v->s.tr[1][0].pte == 0x900661);
    ia64_deliver_fault(v, &insn, r);
    pal_event_entered(v, PAL_EVENT_CHECK_IP, IA64_PSR_MC | IA64_PSR_MFH, psr, false);
    CHECK(pal_event_register(v, IA64VMM_SVC_PAL_EVENT) == (IA64VMM_PAL_EVENT_MCA | IA64VMM_PAL_EVENT_TLB));
    CHECK(pal_event_register(v, IA64VMM_SVC_PAL_EXPECTED) == 1);
    /* MCA sets PSR.mc, which now holds the pending INIT. */
    CHECK(vcpu_pal_interrupt_pending(v) && !ia64_external_deliver(v));
    vcpu_destroy(v);
    /* With PSR.mc = 1 the machine check is held: the instruction completes
     * without effect and the MCA is taken once PSR.mc is 0, before INIT,
     * PMI and external interrupts (Table 5-6). */
    v = pal_event_vcpu(true);
    v->s.cr[CR_IVA] = ENTRY + 0x8000;
    pal_event_context(v, IA64_PSR_MC | IA64_PSR_BN);
    insn = pal_event_overlap(v);
    seq = v->mmu_seq;
    r = ia64_execute(v, &insn);
    CHECK(r.kind == IA64_STEP_COMPLETE && v->mmu_seq == seq && !v->s.tr[1][1].valid);
    CHECK(vcpu_pal_interrupt_pending(v) && vcpu_halt_wake_pending(v) && !ia64_external_deliver(v));
    CHECK(vcpu_accept_pal_interrupt(v, IA64_IRQ_INIT, 0) && vcpu_accept_pal_interrupt(v, IA64_IRQ_PMI, 4));
    vcpu_raise_external(v, 0x60);
    psr = pal_event_context(v, IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN);
    CHECK(ia64_external_deliver(v));
    pal_event_entered(v, PAL_EVENT_CHECK_IP, IA64_PSR_MC, psr, true);
    CHECK(pal_event_register(v, IA64VMM_SVC_PAL_EVENT) ==
          (IA64VMM_PAL_EVENT_MCA | IA64VMM_PAL_EVENT_TLB | IA64VMM_PAL_EVENT_COLLECTED));
    psr = pal_event_context(v, IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN);
    CHECK(ia64_external_deliver(v));
    pal_event_entered(v, PAL_EVENT_INIT_IP, IA64_PSR_MC, psr, true);
    psr = pal_event_context(v, IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN);
    CHECK(ia64_external_deliver(v));
    pal_event_entered(v, PAL_EVENT_PMI_IP, 0, psr, true);
    CHECK(pal_event_register(v, IA64VMM_SVC_PAL_PMI_VECTOR) == 4);
    pal_event_context(v, IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN);
    CHECK(ia64_external_deliver(v) && v->s.ip == ENTRY + 0x8000 + 0x3000 && ia64_ivr_read(v) == 0x60);
    vcpu_destroy(v);
}

/* SDM Vol.2 Table 4-2, updated by 248699-014 A-1008: this CPU does
 * not use VRN bits in TLB searches. A 64 KiB walker fill enclosing a
 * same-stream 4 KiB TR must machine check; another RID or stream must
 * not. Footnote h permits delivery at the next instruction boundary. */
static void pal_event_walker(struct vcpu *v, uint64_t address, unsigned tr_side, unsigned identifier)
{
    v->s.rr[0] = v->s.rr[1] = 0x100041;
    v->s.cr[CR_PTA] = 0x40000000 | 15u << 2 | 1;
    uint64_t hash = ia64_thash(&v->s, address), pte = ENTRY | 0x661;
    v->s.tr[1][1] = (struct ia64_tlb_entry){.va = hash & UINT64_C(0x1fffffffffff8000),
        .rid = 0x1000, .page_shift = 15, .itir = 15u << 2, .pte = pte, .valid = true};
    CHECK(gm_write(ENTRY + (hash & 0x7fff), &pte, sizeof(pte)));
    v->s.tr[tr_side][0] = (struct ia64_tlb_entry){.va = 0x12340000, .rid = identifier,
        .page_shift = 12, .itir = 12u << 2, .pte = pte, .valid = true};
}
static void test_pal_walker_check(void)
{
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned opposite = 0; opposite < 2; ++opposite)
            for (unsigned other_rid = 0; other_rid < 2; ++other_rid)
                for (unsigned vrn = 0; vrn < 2; ++vrn)
                    for (unsigned entries = 0; entries < 2; ++entries)
                        for (unsigned mode = 0; mode < 3; ++mode) {
        if (!side && !mode) continue; /* Instruction walks require PSR.ic. */
        bool held = mode == 2, collected = mode != 0;
        struct vcpu *v = pal_event_vcpu(entries);
        uint64_t psr = pal_event_context(v, IA64_PSR_DT | IA64_PSR_IT | IA64_PSR_BN |
            (collected ? IA64_PSR_IC : 0) | (held ? IA64_PSR_MC : 0));
        uint64_t address = (uint64_t)vrn << 61 | 0x12343000;
        unsigned tr_side = side ^ opposite;
        pal_event_walker(v, address, tr_side, other_rid ? 0x2000 : 0x1000);
        struct ia64_tlb_entry pinned = v->s.tr[tr_side][0];
        bool overlap = !opposite && !other_rid;
        /* A walker insert makes a held purge check non-continuable,
         * just as an overlapping software insert does. */
        if (overlap && entries && held)
            v->pal_mca_pending = IA64VMM_PAL_EVENT_MCA | IA64VMM_PAL_EVENT_TLB |
                (vrn ? 0 : IA64VMM_PAL_EVENT_TLB_PURGE);
        uint64_t seq = v->mmu_seq;
        struct ia64_translation t;
        struct ia64_result r = ia64_translate(v, address, side ? IA64_REF_DATA : IA64_REF_FETCH,
            side ? IA64_ACCESS_READ : IA64_ACCESS_EXECUTE, 0, !collected, &t);
        CHECK(!memcmp(&v->s.tr[tr_side][0], &pinned, sizeof(pinned)));
        if (overlap && !entries) {
            CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_UNIMPLEMENTED);
            CHECK(r.detail && !strcmp(r.detail, "VHPT insert overlaps TR; MCA delivery unavailable"));
            CHECK(v->mmu_seq == seq && !v->s.tc[side][0].valid && !v->pal_mca_pending);
        } else {
            CHECK(r.kind == IA64_STEP_COMPLETE && t.pa == ENTRY + 0x3000);
            CHECK(v->mmu_seq == seq + 1 && v->s.tc[side][0].valid);
            CHECK(vcpu_pal_interrupt_pending(v) == overlap);
            if (overlap) {
                /* The reference completes before the MCA. PSR.mc holds
                 * it until clear; collection names the next boundary. */
                CHECK(v->s.ip == ENTRY + 0x40);
                if (held) {
                    CHECK(!ia64_external_deliver(v));
                    v->s.psr &= ~IA64_PSR_MC; psr &= ~IA64_PSR_MC;
                }
                CHECK(ia64_external_deliver(v));
                pal_event_entered(v, PAL_EVENT_CHECK_IP, IA64_PSR_MC, psr, collected);
                CHECK(pal_event_register(v, IA64VMM_SVC_PAL_EVENT) ==
                    (IA64VMM_PAL_EVENT_MCA | IA64VMM_PAL_EVENT_TLB |
                     (collected ? IA64VMM_PAL_EVENT_COLLECTED : 0)));
                CHECK(!vcpu_pal_interrupt_pending(v) && !ia64_external_deliver(v));
            } else CHECK(!ia64_external_deliver(v));
        }
        vcpu_destroy(v);
    }
}

/* ptc.l/ptc.g/ptc.ga r10 = 0x900000 in region 1, r9 = 1 MiB << 2, over TR
 * 0 of `side` at 0x940000 (4 KiB, RID 0x1000). The range also covers an
 * opposite-stream TC on the TR's page and a same-stream TC beside it.
 * Region 1 has region 0's RID: purges match the RID, not the VRN (SDM
 * Vol.2 §4.1.1.4). */
static Ia64Instruction pal_event_purge(struct vcpu *v, Ia64Opcode opcode, unsigned side)
{
    v->s.rr[0] = v->s.rr[1] = 0x100030;
    v->s.tr[side][0] = (struct ia64_tlb_entry){.va = 0x940000, .rid = 0x1000,
        .page_shift = 12, .itir = 48, .pte = 0x940661, .valid = true};
    ia64_tc_insert(v, 0x950000, side, 0x950661, 12u << 2);
    ia64_tc_insert(v, 0x940000, !side, 0x940661, 12u << 2);
    Ia64Instruction insn = instruction(opcode);
    insn.unit = IA64_UNIT_M; insn.slot = 0;
    insn.operands.system.source = 9; insn.operands.system.register_index = 10;
    v->s.gr[9] = 20u << 2; v->s.gr[10] = (UINT64_C(1) << 61) | 0x900000;
    return insn;
}
/* Both TCs are gone (Table 4-1 "must purge"), each removal logged once;
 * the TR stays: TC purges do not invalidate TRs (Vol.2 §4.1.1.2,
 * §5.2.2.2.3). */
static void pal_event_purged(const struct vcpu *v, unsigned side, uint64_t seq)
{
    CHECK(!ia64_tlb_find(&v->s, 0x950000, side) && !ia64_tlb_find(&v->s, 0x940000, !side));
    CHECK(v->s.tr[side][0].valid && v->s.tr[side][0].pte == 0x940661);
    CHECK(v->mmu_seq == seq + 2);
}
#define PAL_EVENT_PURGE (IA64VMM_PAL_EVENT_MCA | IA64VMM_PAL_EVENT_TLB | IA64VMM_PAL_EVENT_TLB_PURGE)
static void test_pal_purge_check(void)
{
    /* Table 4-1: ptc.l and the local ptc.g/ptc.ga overlapping an ITR or a
     * DTR must machine check, and the TC part must still be purged. The
     * machine check may come after the purge, by the next instruction
     * serialization (footnote h): the purge completes and the machine
     * check is taken at the next instruction boundary, here slot 1 of
     * the same bundle, which IIP/IPSR.ri name with PSR.ic = 1; with
     * PSR.ic = 0 they keep their values. The event names a purge's TLB
     * check. */
    const Ia64Opcode forms[] = {IA64_OP_PTC_L, IA64_OP_PTC_G, IA64_OP_PTC_GA};
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned n = 0; n < 3; ++n)
            for (unsigned collect = 0; collect < 2; ++collect) {
                struct vcpu *v = pal_event_vcpu(true);
                uint64_t psr = pal_event_context(v, (collect ? IA64_PSR_IC : 0) | IA64_PSR_I | IA64_PSR_BN | IA64_PSR_DT);
                v->s.slot = 0;
                Ia64Instruction insn = pal_event_purge(v, forms[n], side);
                uint64_t seq = v->mmu_seq;
                struct ia64_result r = ia64_execute(v, &insn);
                CHECK(r.kind == IA64_STEP_COMPLETE);
                pal_event_purged(v, side, seq);
                CHECK(vcpu_pal_interrupt_pending(v) && ia64_external_pending(v));
                v->s.slot = 1;
                CHECK(ia64_external_deliver(v));
                pal_event_entered(v, PAL_EVENT_CHECK_IP, IA64_PSR_MC, psr, collect);
                CHECK(pal_event_register(v, IA64VMM_SVC_PAL_EVENT) ==
                      (PAL_EVENT_PURGE | (collect ? IA64VMM_PAL_EVENT_COLLECTED : 0)));
                CHECK(!vcpu_pal_interrupt_pending(v) && v->s.tr[side][0].valid);
                vcpu_destroy(v);
            }
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned n = 0; n < 3; ++n) {
            /* PSR.mc = 1 holds it; the purge is the same. */
            struct vcpu *v = pal_event_vcpu(true);
            pal_event_context(v, IA64_PSR_MC | IA64_PSR_IC | IA64_PSR_BN);
            Ia64Instruction insn = pal_event_purge(v, forms[n], side);
            uint64_t seq = v->mmu_seq;
            struct ia64_result r = ia64_execute(v, &insn);
            CHECK(r.kind == IA64_STEP_COMPLETE);
            pal_event_purged(v, side, seq);
            CHECK(vcpu_pal_interrupt_pending(v) && !ia64_external_pending(v) && !ia64_external_deliver(v));
            uint64_t psr = pal_event_context(v, IA64_PSR_IC | IA64_PSR_BN);
            CHECK(ia64_external_deliver(v));
            pal_event_entered(v, PAL_EVENT_CHECK_IP, IA64_PSR_MC, psr, true);
            CHECK(pal_event_register(v, IA64VMM_SVC_PAL_EVENT) == (PAL_EVENT_PURGE | IA64VMM_PAL_EVENT_COLLECTED));
            vcpu_destroy(v);
        }
    /* A purge smaller than the TR's page overlaps it: 4 KiB inside a
     * 256 KiB DTR. */
    struct vcpu *v = pal_event_vcpu(true);
    pal_event_context(v, IA64_PSR_IC | IA64_PSR_BN);
    v->s.rr[0] = 0x100030;
    v->s.tr[1][0] = (struct ia64_tlb_entry){.va = 0x940000, .rid = 0x1000,
        .page_shift = 18, .itir = 18u << 2, .pte = 0x940661, .valid = true};
    Ia64Instruction small_purge = instruction(IA64_OP_PTC_L);
    small_purge.unit = IA64_UNIT_M; small_purge.slot = 0;
    small_purge.operands.system.source = 9; small_purge.operands.system.register_index = 10;
    v->s.gr[9] = 12u << 2; v->s.gr[10] = 0x94f000;
    struct ia64_result small = ia64_execute(v, &small_purge);
    CHECK(small.kind == IA64_STEP_COMPLETE && vcpu_pal_interrupt_pending(v) && v->s.tr[1][0].valid);
    vcpu_destroy(v);
    /* A held insert's machine check keeps its cause when a purge overlaps
     * before it is taken: one machine check, not continuable. */
    v = pal_event_vcpu(true);
    pal_event_context(v, IA64_PSR_MC | IA64_PSR_BN);
    Ia64Instruction insn = pal_event_overlap(v);
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
    insn = pal_event_purge(v, IA64_OP_PTC_L, 1);
    v->s.gr[9] = 20u << 2; v->s.gr[10] = 0x900000;
    CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
    uint64_t psr = pal_event_context(v, IA64_PSR_IC | IA64_PSR_BN);
    CHECK(ia64_external_deliver(v));
    pal_event_entered(v, PAL_EVENT_CHECK_IP, IA64_PSR_MC, psr, true);
    CHECK(pal_event_register(v, IA64VMM_SVC_PAL_EVENT) ==
          (IA64VMM_PAL_EVENT_MCA | IA64VMM_PAL_EVENT_TLB | IA64VMM_PAL_EVENT_COLLECTED));
    CHECK(!vcpu_pal_interrupt_pending(v));
    vcpu_destroy(v);
    /* Must not machine check: ptc.e and ptr over a TR (ptr purges it), a
     * remote ptc.g over a TR (which it leaves), a purge of the page beside
     * the TR and one of another RID. */
    for (unsigned side = 0; side < 2; ++side) {
        struct vcpu *v = pal_event_vcpu(true);
        pal_event_context(v, IA64_PSR_IC | IA64_PSR_BN);
        Ia64Instruction insn = pal_event_purge(v, IA64_OP_PTC_E, side);
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
        CHECK(v->s.tr[side][0].valid && !ia64_tlb_find(&v->s, 0x950000, side));
        insn = pal_event_purge(v, IA64_OP_PTC_L, side);
        ia64_tlb_remote_purge(v, 0x1000, 0x900000, 20, false);
        CHECK(v->s.tr[side][0].valid && !ia64_tlb_find(&v->s, 0x950000, side));
        ia64_tlb_remote_purge(v, 0, 0, 0, true);
        CHECK(v->s.tr[side][0].valid);
        v->s.gr[9] = 12u << 2; v->s.gr[10] = 0x941000;
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE);
        v->s.rr[1] = 0x200030; v->s.gr[10] = (UINT64_C(1) << 61) | 0x940000;
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && v->s.tr[side][0].valid);
        insn = pal_event_purge(v, side ? IA64_OP_PTR_D : IA64_OP_PTR_I, side);
        CHECK(ia64_execute(v, &insn).kind == IA64_STEP_COMPLETE && !v->s.tr[side][0].valid);
        CHECK(!vcpu_pal_interrupt_pending(v) && !ia64_external_pending(v));
        vcpu_destroy(v);
    }
}

/* The per-processor PAL registers (platform.h), their reset values and
 * their independence between processors. */
static void test_pal_registers(void)
{
    struct vcpu *a = pal_event_vcpu(false), *b = pal_event_vcpu(false);
    const unsigned rw[] = {IA64VMM_SVC_PAL_XR0, IA64VMM_SVC_PAL_PALE_PMI, IA64VMM_SVC_PAL_SALE_PMI};
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(rw); ++i) {
        CHECK(pal_event_register(a, rw[i]) == 0);
        ia64_pal_service_write(a, rw[i], UINT64_C(0x8000000000123000) + i * 0x100);
        CHECK(pal_event_register(a, rw[i]) == UINT64_C(0x8000000000123000) + i * 0x100);
        CHECK(pal_event_register(b, rw[i]) == 0);
    }
    CHECK(pal_event_register(a, IA64VMM_SVC_PAL_EXPECTED) == 0);
    ia64_pal_service_write(a, IA64VMM_SVC_PAL_EXPECTED, 1);
    CHECK(pal_event_register(a, IA64VMM_SVC_PAL_EXPECTED) == 1 && pal_event_register(b, IA64VMM_SVC_PAL_EXPECTED) == 0);
    ia64_pal_service_write(a, IA64VMM_SVC_PAL_EXPECTED, 0);
    CHECK(pal_event_register(a, IA64VMM_SVC_PAL_EXPECTED) == 0);
    /* The event registers are read only. */
    ia64_pal_service_write(a, IA64VMM_SVC_PAL_EVENT, 7);
    ia64_pal_service_write(a, IA64VMM_SVC_PAL_PMI_VECTOR, 7);
    CHECK(pal_event_register(a, IA64VMM_SVC_PAL_EVENT) == 0 && pal_event_register(a, IA64VMM_SVC_PAL_PMI_VECTOR) == 0);
    /* Reset returns every register and pending event to 0. */
    ia64_pal_service_write(a, IA64VMM_SVC_PAL_EXPECTED, 1);
    CHECK(vcpu_accept_pal_interrupt(a, IA64_IRQ_PMI, 2));
    vcpu_reset(a, NULL);
    for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(rw); ++i) CHECK(pal_event_register(a, rw[i]) == 0);
    CHECK(pal_event_register(a, IA64VMM_SVC_PAL_EXPECTED) == 0 && !vcpu_pal_interrupt_pending(a));
    vcpu_destroy(a); vcpu_destroy(b);
}

/* Occurrences of text in the log since position `from`. */
static unsigned pal_event_logged(uint64_t from, const char *text)
{
    const struct log_ring *ring = log_get_ring();
    uint64_t end = ring->write_pos;
    CHECK(end - from < LOG_RING_SIZE);
    char line[256];
    unsigned found = 0, n = 0;
    for (uint64_t at = from; at < end; ++at) {
        char c = ring->data[at % LOG_RING_SIZE];
        if (c != '\n' && n < sizeof(line) - 1) { line[n++] = c; continue; }
        line[n] = 0; n = 0;
        if (strstr(line, text)) ++found;
    }
    return found;
}
/* Every engine takes the events at an instruction boundary: the guest
 * loops on a branch to itself until the event enters its PAL entry, whose
 * break.m 0x1ffffe ends the run. The overlap is a guest itr.d with
 * PSR.ic = 0. Each entry and overlap is logged once, also when lockstep
 * replays it. */
static void test_pal_event_engines(void)
{
    const uint64_t nop = UINT64_C(1) << 27, stop = (UINT64_C(1) << 36) | (UINT64_C(0xffffe) << 6);
    /* MIB template 0x11: nop.m, nop.i, br.cond.sptk.few to itself (B1, imm20b 0). */
    const uint64_t self = UINT64_C(4) << 37;
    const uint64_t itr = (UINT64_C(1) << 37) | (UINT64_C(0x0e) << 27) | (UINT64_C(10) << 20) | (UINT64_C(9) << 13);
    Ia64Instruction decoded = ia64_decode_insn(IA64_UNIT_M, itr, ENTRY + 0x900, 0);
    CHECK(decoded.opcode == IA64_OP_ITR_D && decoded.operands.system.source == 9 &&
          decoded.operands.system.register_index == 10);
    engine_set_break_hook(end_hook);
    for (unsigned kind = 0; kind < 3; ++kind) {
        struct vcpu *v = pal_event_vcpu(true);
        bundle_write(PAL_EVENT_INIT_IP, 0, stop, nop, nop);
        bundle_write(PAL_EVENT_CHECK_IP, 0, stop, nop, nop);
        bundle_write(PAL_EVENT_PMI_IP, 0, stop, nop, nop);
        bundle_write(ENTRY + 0x800, 0x11, nop, nop, self);
        bundle_write(ENTRY + 0x900, 0x11, itr, nop, self);
        v->s.ip = kind == 2 ? ENTRY + 0x900 : ENTRY + 0x800; v->s.slot = 0;
        v->s.psr = kind == 1 ? IA64_PSR_IC | IA64_PSR_BN : IA64_PSR_BN;
        if (kind == 2) pal_event_overlap(v);
        if (kind == 0) CHECK(vcpu_accept_pal_interrupt(v, IA64_IRQ_INIT, 0));
        if (kind == 1) CHECK(vcpu_accept_pal_interrupt(v, IA64_IRQ_PMI, 9));
        const uint64_t at[] = {PAL_EVENT_INIT_IP, PAL_EVENT_PMI_IP, PAL_EVENT_CHECK_IP};
        const char *const line[] = {"vCPU0 INIT at ip=", "vCPU0 PMI at ip=", "vCPU0 machine check at ip="};
        uint64_t from = log_get_ring()->write_pos;
        struct vcpu_exit out = engine_run(v, 1000);
        CHECK(out.reason == VCPU_EXIT_DEBUG && out.ip == at[kind]);
        CHECK(pal_event_logged(from, line[kind]) == 1);
        CHECK(pal_event_logged(from, "TLB overlap: ") == (kind == 2));
        CHECK(!v->s.tr[1][1].valid && (v->s.psr & IA64_PSR_MC) == (kind == 1 ? 0 : IA64_PSR_MC));
        vcpu_destroy(v);
    }
    engine_set_break_hook(NULL);
}
/* Every engine: a guest ptc.l, ptc.g or ptc.ga over DTR 0 with PSR.ic = 1,
 * alone in its instruction group (template M;;MI;;, as ptc.g requires),
 * completes its purge, and the machine check enters PALE_CHECK at the
 * next instruction: IIP is the bundle and IPSR.ri slot 1. The overlap and
 * the entry are logged once. The next bundle branches to itself. */
static void test_pal_purge_engines(void)
{
    const uint64_t nop = UINT64_C(1) << 27, stop = (UINT64_C(1) << 36) | (UINT64_C(0xffffe) << 6);
    const uint64_t self = UINT64_C(4) << 37;
    const Ia64Opcode forms[] = {IA64_OP_PTC_L, IA64_OP_PTC_G, IA64_OP_PTC_GA};
    const unsigned x6[] = {0x09, 0x0a, 0x0b};
    engine_set_break_hook(end_hook);
    for (unsigned n = 0; n < 3; ++n) {
        /* M45: ptc r3 = r10, r2 = r9. */
        const uint64_t ptc = (UINT64_C(1) << 37) | ((uint64_t)x6[n] << 27) | (UINT64_C(10) << 20) | (UINT64_C(9) << 13);
        Ia64Instruction decoded = ia64_decode_insn(IA64_UNIT_M, ptc, ENTRY + 0xa00, 0);
        CHECK(decoded.opcode == forms[n] && decoded.operands.system.register_index == 10 &&
              decoded.operands.system.source == 9);
        struct vcpu *v = pal_event_vcpu(true);
        bundle_write(PAL_EVENT_CHECK_IP, 0, stop, nop, nop);
        bundle_write(ENTRY + 0xa00, 0x0b, ptc, nop, nop);
        bundle_write(ENTRY + 0xa10, 0x11, nop, nop, self);
        pal_event_purge(v, forms[n], 1);
        uint64_t seq = v->mmu_seq;
        v->s.ip = ENTRY + 0xa00; v->s.slot = 0;
        v->s.psr = IA64_PSR_IC | IA64_PSR_BN;
        uint64_t from = log_get_ring()->write_pos;
        struct vcpu_exit out = engine_run(v, 1000);
        CHECK(out.reason == VCPU_EXIT_DEBUG && out.ip == PAL_EVENT_CHECK_IP);
        CHECK(pal_event_logged(from, "vCPU0 machine check at ip=") == 1);
        CHECK(pal_event_logged(from, "TLB purge overlap: ") == 1);
        CHECK(v->s.cr[CR_IIP] == ENTRY + 0xa00 && v->s.cr[CR_IPSR] == (IA64_PSR_IC | IA64_PSR_BN | (UINT64_C(1) << 41)));
        CHECK(pal_event_register(v, IA64VMM_SVC_PAL_EVENT) ==
              (PAL_EVENT_PURGE | IA64VMM_PAL_EVENT_COLLECTED));
        pal_event_purged(v, 1, seq);
        vcpu_destroy(v);
    }
    engine_set_break_hook(NULL);
}

static void test_pal_walker_engines(void)
{
    const uint64_t nop = UINT64_C(1) << 27, stop = (UINT64_C(1) << 36) | (UINT64_C(0xffffe) << 6);
    const uint64_t ld8 = (UINT64_C(4) << 37) | (UINT64_C(3) << 30) | (8u << 6) | (10u << 20);
    const uint64_t address = 0x12343000, value = UINT64_C(0x123456789abcdef0);
    engine_set_break_hook(end_hook);
    for (unsigned side = 0; side < 2; ++side) {
        struct vcpu *v = pal_event_vcpu(true);
        pal_event_walker(v, address, side, 0x1000);
        bundle_write(PAL_EVENT_CHECK_IP, 0, stop, nop, nop);
        bundle_write(ENTRY + 0xb00, 0, ld8, nop, nop);
        if (side) CHECK(gm_write(ENTRY + 0x3000, &value, sizeof(value)));
        else bundle_write(ENTRY + 0x3000, 0, nop, nop, nop);
        bundle_write(side ? ENTRY + 0xb10 : ENTRY + 0x3010, 0, UINT64_C(0x31) << 27, nop, nop);
        v->s.ip = side ? ENTRY + 0xb00 : address; v->s.slot = 0;
        v->s.gr[10] = address;
        uint64_t ip = v->s.ip;
        uint64_t psr = IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_BN | (side ? 0 : IA64_PSR_IT);
        v->s.psr = psr;
        uint64_t from = log_get_ring()->write_pos;
        struct vcpu_exit out = engine_run(v, 1000);
        CHECK(out.reason == VCPU_EXIT_DEBUG && out.ip == PAL_EVENT_CHECK_IP);
        CHECK(pal_event_logged(from, "VHPT overlap: ") == 1);
        CHECK(pal_event_logged(from, "vCPU0 machine check at ip=") == 1);
        /* Tier0 can finish its current bundle before the next event
         * boundary; every engine enters by the following srlz.i. */
        CHECK(v->s.cr[CR_IIP] == ip || v->s.cr[CR_IIP] == ip + 16);
        CHECK((v->s.cr[CR_IPSR] & ~(UINT64_C(3) << 41)) == psr);
        CHECK(v->s.cr[CR_IIP] == ip || ((v->s.cr[CR_IPSR] >> 41) & 3) <= 1);
        CHECK(v->s.tr[side][0].valid && v->s.tc[side][0].valid);
        if (side) CHECK(v->s.gr[8] == value);
        vcpu_destroy(v);
    }
    engine_set_break_hook(NULL);
}

static void test_pal_events(void)
{
    test_pal_init();
    test_pal_pmi();
    test_pal_machine_check();
    test_pal_purge_check();
    test_pal_walker_check();
    test_pal_registers();
    test_pal_event_engines();
    test_pal_purge_engines();
    test_pal_walker_engines();
}
