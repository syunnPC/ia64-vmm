/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tier0 under PSR.db (SDM Vol.2 §7.1). Every case compares Tier0 and
 * lockstep with interp_run and asserts which engine retired the work. */
#include "ia64/debug.h"

#define DEBUG_IVA (BASE + 0x18000)
#define DEBUG_MASK UINT64_C(0x00ffffffffffffff)
#define DEBUG_XR (UINT64_C(1) << 63) /* IBR.x, DBR.r */
#define DEBUG_W (UINT64_C(1) << 62)
#define DEBUG_PLM(pl) (UINT64_C(1) << (56 + (pl)))

static uint64_t mov_ibr(unsigned index, unsigned value)
{ return (UINT64_C(1) << 37) | (UINT64_C(2) << 27) | ((uint64_t)index << 20) | ((uint64_t)value << 13); }
static uint64_t mov_dbr(unsigned index, unsigned value)
{ return (UINT64_C(1) << 37) | (UINT64_C(1) << 27) | ((uint64_t)index << 20) | ((uint64_t)value << 13); }
static uint64_t mov_psr_l(unsigned value)
{ return (UINT64_C(1) << 37) | (UINT64_C(0x2d) << 27) | ((uint64_t)value << 13); }
static const uint64_t debug_flushrs = UINT64_C(0x0c) << 27, debug_br_ret = UINT64_C(0x108001100);

static void debug_start(struct vcpu *v, enum engine_kind engine, uint64_t psr)
{
    code_cache_flush(); restart(v, engine, BASE);
    v->s.psr = IA64_PSR_IC | IA64_PSR_DB | psr; v->s.cr[CR_IVA] = DEBUG_IVA;
    for (unsigned offset = 0; offset < 0x8000; offset += 0x100) bundle(DEBUG_IVA + offset, 1, nop, nop, nop);
}
struct debug_route { uint64_t tier0, interpreted; };
static struct debug_route debug_compare(struct vcpu *v, struct vcpu *ref, unsigned budget)
{
    struct tier0_stats before, after; tier0_get_stats(&before);
    virtual_compare(v, ref, budget);
    tier0_get_stats(&after);
    return (struct debug_route){after.executed_instructions - before.executed_instructions,
        after.interpreter_instructions[TIER0_INTERP_DEBUG] - before.interpreter_instructions[TIER0_INTERP_DEBUG]};
}
static void debug_fault(const struct vcpu *v, uint64_t ip, unsigned slot, uint64_t isr, uint64_t ifa)
{
    CHECK(v->fault.kind == IA64_FAULT_DEBUG && v->fault.ip == ip && v->fault.slot == slot);
    CHECK(v->s.cr[CR_IIP] == ip && (v->s.cr[CR_ISR] & ~(UINT64_C(3) << 41)) == isr && v->s.cr[CR_IFA] == ifa);
}
/* ld8 r8=[r10]; st8 [r11]=r9; add; nop; add; br: inline physical accesses. */
static void debug_loop(struct vcpu *v)
{
    bundle(BASE, 9, load(3,8,10), store(3,9,11), add(12,12,13));
    bundle(BASE + 16, 0x11, nop, add(14,14,13), validator_branch(BASE + 16, BASE));
    v->s.gr[9] = 77; v->s.gr[10] = DATA; v->s.gr[11] = DATA + 8; v->s.gr[13] = 1;
}
/* Two iterations without a pair at CPL run in Tier0. They also seal the
 * region and leave DATA in the physical-page cache: Tier0's next ld8/st8
 * take the inline path, which alone skips ia64_debug_data (lockstep keeps
 * them in helpers). Cold first-use regions run every instruction in a helper. */
static void debug_warm(struct vcpu *v, struct vcpu *ref, enum engine_kind engine,
                       unsigned budget, bool store)
{
    struct tier0_stats before, after; tier0_get_stats(&before);
    struct debug_route route = debug_compare(v, ref, budget);
    tier0_get_stats(&after);
    CHECK(route.tier0 == budget && !route.interpreted && v->fault.kind == IA64_FAULT_NONE);
    CHECK(v->s.ip == BASE && v->s.slot == 0);
    if (IA64VMM_TIER0_STATS && engine == ENGINE_TIER0)
        CHECK(after.inlined[IA64_OP_LD8] > before.inlined[IA64_OP_LD8] &&
              (!store || after.inlined[IA64_OP_ST8] > before.inlined[IA64_OP_ST8]));
}
static void debug_unarmed(struct vcpu *v, struct vcpu *ref, enum engine_kind engine)
{
    /* (a) No pair enabled, then every address, mask, plm and ignored field
     * set with x/r/w clear (IBR bit 62 is ignored, not a write enable). */
    for (unsigned fields = 0; fields < 2; ++fields) {
        debug_start(v, engine, 0); debug_loop(v);
        for (unsigned pair = 0; fields && pair < 4; ++pair) {
            v->s.ibr[pair * 2] = BASE; v->s.ibr[pair * 2 + 1] = (UINT64_C(0x7f) << 56) | DEBUG_MASK;
            v->s.dbr[pair * 2] = DATA; v->s.dbr[pair * 2 + 1] = (UINT64_C(0x3f) << 56) | DEBUG_MASK;
        }
        debug_warm(v, ref, engine, 12, true);
        CHECK(v->s.gr[12] == 2 && v->s.gr[14] == 2);
    }
}
static void debug_other_level(struct vcpu *v, struct vcpu *ref, enum engine_kind engine)
{
    /* (b) Pairs enabled only for PL3 cover the code and the data: CPL 0
     * regions run in Tier0; at CPL 3 each pair faults where interp does. */
    for (unsigned cpl = 0; cpl < 4; cpl += 3)
        for (unsigned side = 0; side < 2; ++side) {
            debug_start(v, engine, (uint64_t)cpl << 32); debug_loop(v);
            v->s.ibr[0] = BASE; v->s.dbr[0] = DATA;
            uint64_t ibr = (side ? 0 : DEBUG_XR) | DEBUG_PLM(3) | DEBUG_MASK;
            uint64_t dbr = DEBUG_XR | DEBUG_W | DEBUG_PLM(3) | (DEBUG_MASK & ~UINT64_C(15));
            if (!cpl) { v->s.ibr[1] = ibr; v->s.dbr[1] = dbr; }
            debug_warm(v, ref, engine, 12, true);
            if (!cpl) continue;
            v->s.ibr[1] = ibr; v->s.dbr[1] = dbr;
            struct debug_route route = debug_compare(v, ref, 2);
            CHECK(route.interpreted == 1 && route.tier0 == 1);
            if (!side) debug_fault(v, BASE, 0, UINT64_C(1) << 32, BASE);
            else debug_fault(v, BASE, 0, UINT64_C(1) << 34, DATA);
        }
}
static void debug_instruction_pair(struct vcpu *v, struct vcpu *ref, enum engine_kind engine)
{
    /* (c) An armed pair at CPL keeps the interpreter (no armed pair in the
     * Windows rows): inside the loop it faults at the second
     * bundle, after the first one retired; outside the loop nothing faults. */
    for (unsigned inside = 0; inside < 2; ++inside) {
        debug_start(v, engine, 0);
        bundle(BASE, 1, add(8,8,9), add(8,8,9), add(8,8,9));
        bundle(BASE + 16, 1, add(8,8,9), add(8,8,9), add(8,8,9));
        bundle(BASE + 32, 0x11, nop, add(8,8,9), validator_branch(BASE + 32, BASE));
        v->s.gr[9] = 1;
        v->s.ibr[0] = inside ? BASE + 16 : BASE + 0x1000;
        v->s.ibr[1] = DEBUG_XR | DEBUG_PLM(0) | DEBUG_MASK;
        struct debug_route route = debug_compare(v, ref, inside ? 4 : 18);
        CHECK(route.interpreted == (inside ? 4 : 18) && !route.tier0);
        if (inside) {
            debug_fault(v, BASE + 16, 0, UINT64_C(1) << 32, BASE + 16);
            CHECK(v->s.gr[8] == 3);
        } else CHECK(v->fault.kind == IA64_FAULT_NONE && v->s.gr[8] == 14);
    }
}
static void debug_data_pairs(struct vcpu *v, struct vcpu *ref, enum engine_kind engine)
{
    /* (d) Read and write pairs at CPL against the inline physical ld8/st8;
     * each pair also faces the other access kind, which it never matches. */
    for (unsigned kind = 0; kind < 3; ++kind) {
        debug_start(v, engine, 0); debug_loop(v); debug_warm(v, ref, engine, 12, true);
        uint64_t sentinel = UINT64_C(0x5a5a5a5a5a5a5a5a); CHECK(gm_write(DATA + 8, &sentinel, 8));
        v->s.dbr[0] = kind ? DATA + 8 : DATA;
        v->s.dbr[1] = (kind == 1 ? DEBUG_W : DEBUG_XR) | DEBUG_PLM(0) | DEBUG_MASK;
        v->s.dbr[2] = DATA; v->s.dbr[3] = kind == 2 ? DEBUG_W | DEBUG_PLM(0) | DEBUG_MASK : 0;
        struct debug_route route = debug_compare(v, ref, kind == 2 ? 6 : kind + 1);
        CHECK(!route.tier0 && route.interpreted == (kind == 2 ? 6u : kind + 1));
        uint64_t word; CHECK(gm_read(DATA + 8, &word, 8));
        if (!kind) debug_fault(v, BASE, 0, UINT64_C(1) << 34, DATA);
        else if (kind == 1) {
            debug_fault(v, BASE, 1, UINT64_C(1) << 33, DATA + 8);
            CHECK(word == sentinel);
        } else CHECK(v->fault.kind == IA64_FAULT_NONE && word == 77);
    }
}
static void debug_rse(struct vcpu *v, struct vcpu *ref, enum engine_kind engine)
{
    /* (d) Mandatory RSE references use RSC.pl (SDM Vol.2 §6.6), here PL3
     * at CPL 0: Tier0 runs the regions; the spill in the flushrs helper and
     * the fill at the br.ret target's fetch fault where interp does. */
    const gpa_t backing = DATA + 0x800;
    debug_start(v, engine, 0);
    bundle(BASE, 1, add(14,14,13), add(15,15,13), nop);
    bundle(BASE + 16, 9, debug_flushrs, nop, nop);
    const struct ia64_bundle *decoded;
    CHECK(code_get_bundle(v, BASE + 16, &decoded).kind == IA64_STEP_COMPLETE &&
          decoded->insn[0].opcode == IA64_OP_FLUSHRS);
    v->s.ar[AR_RSC] = 12; v->s.rse_bof = 4; v->s.rse_dirty = 4;
    v->s.ar[AR_BSPSTORE] = backing; v->s.ar[AR_BSP] = ia64_rse_advance(backing, 4);
    for (unsigned n = 32; n < 128; ++n) v->s.gr[n] = n * 0x101;
    v->s.dbr[0] = backing + 16; v->s.dbr[1] = DEBUG_W | DEBUG_PLM(3) | DEBUG_MASK;
    struct debug_route route = debug_compare(v, ref, 4);
    CHECK(route.tier0 == 4 && !route.interpreted);
    debug_fault(v, BASE + 16, 0, (UINT64_C(1) << 37) | (UINT64_C(1) << 33), backing + 16);
    CHECK(v->s.ar[AR_BSPSTORE] == backing + 16 && v->s.rse_dirty == 2);

    debug_start(v, engine, 0);
    bundle(BASE, 0x11, nop, nop, debug_br_ret);
    bundle(BASE + 0x100, 1, nop, add(40,32,33), nop);
    /* Caller CFM sof 10, sol 8: its eight locals return from the store. */
    v->s.ar[AR_PFS] = 10 | (UINT64_C(8) << 7); v->s.br[0] = BASE + 0x100;
    v->s.ar[AR_RSC] = 12; v->s.rse_bof = 2; v->s.rse_dirty = 0; v->s.cfm = 2;
    v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = backing + 64;
    v->s.dbr[0] = backing + 24; v->s.dbr[1] = DEBUG_XR | DEBUG_PLM(3) | DEBUG_MASK;
    route = debug_compare(v, ref, 4);
    CHECK(route.tier0 == 4 && !route.interpreted);
    /* ISR.ir: the fill belongs to the incomplete frame (SDM Vol.2 §6.8). */
    debug_fault(v, BASE + 0x100, 0, (UINT64_C(1) << 38) | (UINT64_C(1) << 37) | (UINT64_C(1) << 34),
                backing + 24);
}
static void debug_walker(struct vcpu *v, struct vcpu *ref, enum engine_kind engine)
{
    /* (d) VHPT walker references are PL0 reads (SDM Vol.2 Table 7-1): a
     * PL0 pair on the entry aborts the walk of a CPL 3 Tier0 region's load
     * into a Data TLB fault; without the pair the walk inserts the entry. */
    const uint64_t va = UINT64_C(0x2000000000400000);
    for (unsigned armed = 0; armed < 2; ++armed) {
        code_cache_flush(); virtual_start(v, engine, VIRTUAL_IP);
        v->s.psr |= IA64_PSR_DT | IA64_PSR_DB | (UINT64_C(3) << 32);
        v->s.rr[1] = (UINT64_C(0x55) << 8) | (12u << 2) | 1;
        v->s.cr[CR_PTA] = DATA | (15u << 2) | 1;
        uint64_t hash = ia64_thash(&v->s, va), pte = (DATA + 0x1000) | FETCH_FLAGS, word = 4242;
        virtual_map(v, 1, 0, hash, DATA, 15, FETCH_FLAGS);
        CHECK(gm_write(DATA + (hash & 0x7fff), &pte, 8) && gm_write(DATA + 0x1000, &word, 8));
        bundle(BASE, 1, add(14,14,13), add(15,15,13), nop);
        bundle(BASE + 16, 9, load(3,8,10), nop, nop);
        v->s.gr[10] = va; v->s.gr[13] = 1;
        v->s.dbr[0] = hash; v->s.dbr[1] = armed ? DEBUG_XR | DEBUG_PLM(0) | DEBUG_MASK : 0;
        struct debug_route route = debug_compare(v, ref, 4);
        CHECK(route.tier0 == 4 && !route.interpreted);
        if (armed) CHECK(v->fault.kind == IA64_FAULT_DTLB && v->fault.ip == VIRTUAL_IP + 16 &&
                         v->s.cr[CR_IFA] == va && !ia64_tlb_find(&v->s, va, 1));
        else CHECK(v->fault.kind == IA64_FAULT_NONE && v->s.gr[8] == 4242);
    }
}
static void debug_writes(struct vcpu *v, struct vcpu *ref, enum engine_kind engine)
{
    /* (e) IBR/DBR writes inside a stay: each mov ends its region, so the
     * next slot of the same bundle (IBR) or the warm inline load (DBR)
     * meets the newly armed pair. Two passes first write a disabled pair. */
    for (unsigned dbr = 0; dbr < 2; ++dbr) {
        debug_start(v, engine, 0);
        bundle(BASE, 9, dbr ? mov_dbr(20,21) : mov_ibr(20,21), dbr ? mov_dbr(22,23) : mov_ibr(22,23), nop);
        bundle(BASE + 16, 9, load(3,8,10), nop, nop);
        bundle(BASE + 32, 0x11, nop, nop, validator_branch(BASE + 32, BASE));
        v->s.gr[10] = DATA; v->s.gr[20] = 0; v->s.gr[21] = dbr ? DATA : BASE; v->s.gr[22] = 1;
        debug_warm(v, ref, engine, 18, false);
        v->s.gr[23] = DEBUG_XR | DEBUG_PLM(0) | DEBUG_MASK;
        struct debug_route route = debug_compare(v, ref, dbr ? 4 : 3);
        CHECK(route.tier0 == 2 && route.interpreted == (dbr ? 2u : 1u));
        if (dbr) debug_fault(v, BASE + 16, 0, UINT64_C(1) << 34, DATA);
        else debug_fault(v, BASE, 2, UINT64_C(1) << 32, BASE);
    }
    /* Disarming in an interpreter step returns the loop to Tier0. */
    debug_start(v, engine, 0);
    bundle(BASE, 9, mov_ibr(22,0), nop, nop);
    bundle(BASE + 16, 0x11, nop, add(8,8,9), validator_branch(BASE + 16, BASE + 16));
    v->s.gr[9] = 1; v->s.gr[22] = 1;
    v->s.ibr[0] = BASE + 0x1000; v->s.ibr[1] = DEBUG_XR | DEBUG_PLM(0) | DEBUG_MASK;
    struct debug_route route = debug_compare(v, ref, 12);
    CHECK(route.interpreted == 1 && route.tier0 == 11 && !v->s.ibr[1] && v->s.gr[8] == 3);
    /* A pair armed outside tier0_run (here by the test) applies at its next call. */
    route = debug_compare(v, ref, 3);
    CHECK(route.tier0 == 3 && v->fault.kind == IA64_FAULT_NONE);
    v->s.ibr[0] = BASE + 16; v->s.ibr[1] = DEBUG_XR | DEBUG_PLM(0) | DEBUG_MASK;
    route = debug_compare(v, ref, 1);
    CHECK(route.interpreted == 1 && !route.tier0);
    debug_fault(v, BASE + 16, 0, UINT64_C(1) << 32, BASE + 16);
    /* mov psr.l setting PSR.db ends its region before the armed next slot;
     * br.ret demoting CPL to 3 reaches a pair enabled only for PL3. */
    debug_start(v, engine, 0); v->s.psr &= ~IA64_PSR_DB;
    bundle(BASE, 9, mov_psr_l(20), nop, nop);
    v->s.gr[20] = IA64_PSR_IC | IA64_PSR_DB;
    v->s.ibr[0] = BASE; v->s.ibr[1] = DEBUG_XR | DEBUG_PLM(0) | DEBUG_MASK;
    route = debug_compare(v, ref, 2);
    CHECK(route.tier0 == 1 && route.interpreted == 1);
    debug_fault(v, BASE, 1, UINT64_C(1) << 32, BASE);
    debug_start(v, engine, 0);
    bundle(BASE, 0x11, nop, nop, debug_br_ret);
    bundle(BASE + 0x100, 1, nop, nop, nop);
    v->s.ar[AR_PFS] = UINT64_C(3) << 62; v->s.br[0] = BASE + 0x100;
    v->s.ibr[0] = BASE + 0x100; v->s.ibr[1] = DEBUG_XR | DEBUG_PLM(3) | DEBUG_MASK;
    route = debug_compare(v, ref, 4);
    CHECK(route.tier0 == 3 && route.interpreted == 1);
    debug_fault(v, BASE + 0x100, 0, UINT64_C(1) << 32, BASE + 0x100);
    CHECK(((v->s.cr[CR_IPSR] >> 32) & 3) == 3);
}
static void debug_lockstep_reference(struct vcpu *v)
{
    /* The lockstep reference applies the fetch-side check: a region
     * executed despite an armed pair at its CPL diverges. */
    for (unsigned armed = 0; armed < 2; ++armed) {
        restart(v, ENGINE_LOCKSTEP, BASE); v->s.psr = IA64_PSR_IC | IA64_PSR_DB;
        v->s.ibr[0] = BASE; v->s.ibr[1] = armed ? DEBUG_XR | DEBUG_PLM(0) | DEBUG_MASK : 0;
        Ia64Instruction i = {.opcode = IA64_OP_ADDS, .valid = true, .address = BASE, .slot = 0,
            .operands.integer = {.destination = 8, .source2 = 8, .immediate = 1}};
        struct tier0_region region = {.valid = true, .key.ip = BASE, .count = 1, .insns = &i, .slots = {1}};
        struct tier0_execution e = {.region = &region, .check = tier0_check_begin(v)};
        CHECK(e.check != NULL);
        CHECK(!tier0_helper(v, &i, &e) && v->retired == 1);
        CHECK(tier0_check_finish(&e, v) == !armed); tier0_check_destroy(e.check);
    }
}
/* Management may request stop after a region's last retirement, while
 * lockstep is still replaying it. It must not become a guest divergence,
 * and replay must retain the request for the next engine boundary. */
static void debug_lockstep_stop(struct vcpu *v)
{
    restart(v, ENGINE_LOCKSTEP, BASE); v->s.psr = IA64_PSR_IC;
    Ia64Instruction i = {.opcode = IA64_OP_ADDS, .valid = true, .address = BASE,
        .operands.integer = {.destination = 8, .source2 = 8, .immediate = 1}};
    struct tier0_region region = {.valid = true, .key.ip = BASE, .count = 1, .insns = &i, .slots = {1}};
    struct tier0_execution e = {.region = &region, .check = tier0_check_begin(v)};
    CHECK(e.check != NULL);
    CHECK(!tier0_helper(v, &i, &e));
    vcpu_request_exit(v, VCPU_EXIT_DEBUG);
    CHECK(tier0_check_finish(&e, v));
    CHECK(v->requested_exit == VCPU_EXIT_DEBUG + 1);
    tier0_check_destroy(e.check);
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_DEBUG);
}
/* An EXIT returns before tier0_helper clears the synchronous mailbox.
 * Inject a differing request into that path while both engines produce
 * exactly the same unbacked-load EXIT, PC and architectural state. */
static void debug_lockstep_sync_stop(struct vcpu *v)
{
    restart(v, ENGINE_LOCKSTEP, BASE); v->s.psr = IA64_PSR_IC;
    bundle(BASE, 9, load(3,8,10), nop, nop);
    v->s.gr[10] = BASE + 0x80000;
    const struct ia64_bundle *decoded;
    CHECK(code_get_bundle(v, BASE, &decoded).kind == IA64_STEP_COMPLETE);
    Ia64Instruction i = decoded->insn[0];
    struct tier0_region region = {.valid = true, .key.ip = BASE, .count = 1, .insns = &i, .slots = {1}};
    struct tier0_execution e = {.region = &region, .check = tier0_check_begin(v)};
    CHECK(e.check != NULL);
    vcpu_request_exit(v, VCPU_EXIT_BAD_ACCESS);
    CHECK(tier0_helper(v, &i, &e) && e.stop && e.result.kind == IA64_STEP_EXIT);
    CHECK(e.result.exit == VCPU_EXIT_BAD_ACCESS && !v->retired);
    CHECK(!tier0_check_finish(&e, v));
    CHECK(v->requested_exit == VCPU_EXIT_BAD_ACCESS + 1);
    tier0_check_destroy(e.check);
    CHECK(engine_run(v, 1).reason == VCPU_EXIT_BAD_ACCESS);
}
static void test_debug(struct vcpu *v, struct vcpu *ref)
{
    /* Every IBR/DBR, PSR.db and CPL writer is a form that ends a region. */
    const Ia64Opcode writers[] = {IA64_OP_MOV_GRIBR, IA64_OP_MOV_GRIBR_INDEXED, IA64_OP_MOV_GRDBR,
        IA64_OP_MOV_GRDBR_INDEXED, IA64_OP_MOV_GRPSR, IA64_OP_RFI, IA64_OP_EPC, IA64_OP_BR_RET,
        IA64_OP_BREAK, IA64_OP_VMSW};
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(writers); ++n) {
        enum ia64_family f = ia64_opcode_info(writers[n])->family;
        CHECK(f == IA64_FAMILY_BRANCH || f == IA64_FAMILY_SYSTEM);
    }
    for (enum engine_kind engine = ENGINE_TIER0; engine <= ENGINE_LOCKSTEP; ++engine) {
        debug_unarmed(v, ref, engine); debug_other_level(v, ref, engine);
        debug_instruction_pair(v, ref, engine); debug_data_pairs(v, ref, engine);
        debug_rse(v, ref, engine); debug_walker(v, ref, engine); debug_writes(v, ref, engine);
    }
    debug_lockstep_reference(v);
    debug_lockstep_stop(v);
    debug_lockstep_sync_stop(v);
    test_check_bytes();
    restart(v, ENGINE_TIER0, BASE);
}
