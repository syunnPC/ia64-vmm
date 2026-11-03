/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native paths of generated regions and the loop top, each compared with
 * the interpreter. `build/tier0-test --native` runs only these.
 */
#include "ia64/smp.h"
static uint64_t native_helper_calls;
/* Runs count instructions on v (Tier0) and a copy of v (interpreter) from
 * the same state and RAM; both must leave the same state and RAM. */
static void native_compare(struct vcpu *v, struct vcpu *ref, uint64_t count)
{
    static uint8_t original[sizeof(ram)], expected[sizeof(ram)];
    *ref = *v; ref->cfg.engine = ENGINE_INTERP;
    ref->check_workspace = NULL; ref->check_workspace_pages = 0;
    memcpy(original, ram, sizeof(ram));
    struct vcpu_exit wanted = interp_run(ref, count);
    memcpy(expected, ram, sizeof(ram)); memcpy(ram, original, sizeof(ram));
    struct vcpu_exit got = engine_run(v, count);
    CHECK(got.reason == wanted.reason && got.ip == wanted.ip && got.slot == wanted.slot);
    CHECK(!memcmp(&v->s, &ref->s, sizeof(v->s)) && v->retired == ref->retired);
    CHECK(!memcmp(expected, ram, sizeof(ram)));
}
/* Inline physical references accept any address of the cached RAM region.
 * The helper counter checks that accesses on different pages stay native. */
static void test_native_memory_pages(struct vcpu *v, struct vcpu *ref)
{
    const gpa_t a = DATA + 0x1000, b = DATA + 0x2000, c = DATA + 0x3000, primed = DATA + 0x4000;
    for (enum engine_kind engine = ENGINE_TIER0; engine <= ENGINE_LOCKSTEP; ++engine) {
        code_cache_flush();
        bundle(BASE, 9, load(3,8,10), load(2,9,11), nop);
        bundle(BASE + 16, 0x11, store(3,9,12), nop, validator_branch(BASE + 16, BASE));
        CHECK(!code_page_has_code(a) && !code_page_has_code(b) && !code_page_has_code(c));
        restart(v, engine, BASE); v->s.psr = IA64_PSR_IC;
        v->s.gr[10] = a + 8; v->s.gr[11] = b + 4; v->s.gr[12] = c + 16;
        uint64_t va = UINT64_C(0x0123456789abcdef), vb = 0x89abcdef;
        CHECK(gm_write(a + 8, &va, 8) && gm_write(b + 4, &vb, 4));
        /* Translation, then the sealed region's first native entry. */
        validator_run(v, 6); validator_run(v, 6);
        for (unsigned round = 0; round < 4; ++round) {
            va += round; vb ^= round;
            CHECK(gm_write(a + 8, &va, 8) && gm_write(b + 4, &vb, 4));
            const struct gm_region *region = ia64_physical_region(v, primed, 8);
            CHECK(region != NULL);
            uint64_t helpers = native_helper_calls;
            native_compare(v, ref, 6);
            CHECK(v->s.gr[8] == va && v->s.gr[9] == vb);
            uint32_t stored; CHECK(gm_read(c + 16, &stored, 4) && stored == vb);
            if (engine == ENGINE_TIER0)
                CHECK(v->physical_region == region && native_helper_calls == helpers);
        }
    }
    code_cache_flush();
}
/* An address outside the cached region takes the exact path: below its
 * base, beyond its end, and in another RAM region or none. */
static void test_native_memory_foreign(struct vcpu *v, struct vcpu *ref)
{
    const gpa_t other = BASE + 0x60000, low = DATA + 0x1000;
    struct { gpa_t primed, load, store; enum vcpu_exit_reason reason; } cases[] = {
        {low, other + 8, other + 16, VCPU_EXIT_BUDGET},
        {other, low + 8, low + 16, VCPU_EXIT_BUDGET},
        {other, other + 4096 + 8, low, VCPU_EXIT_BAD_ACCESS},
        {low, DEVICE + 8, low, VCPU_EXIT_BUDGET},
    };
    for (unsigned at = 0; at < IA64VMM_ARRAY_SIZE(cases); ++at) {
        code_cache_flush();
        bundle(BASE, 9, load(3,8,10), store(3,8,11), nop);
        bundle(BASE + 16, 0x11, nop, nop, validator_branch(BASE + 16, BASE));
        restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC;
        v->s.gr[10] = low; v->s.gr[11] = low + 8;
        validator_run(v, 6); validator_run(v, 6);
        for (unsigned n = 0; n < sizeof(decoded_collision); ++n) decoded_collision[n] = (uint8_t)(n * 7 + 3);
        v->s.gr[10] = cases[at].load; v->s.gr[11] = cases[at].store;
        CHECK(ia64_physical_region(v, cases[at].primed, 8) != NULL);
        native_compare(v, ref, 6);
        CHECK(v->retired == (cases[at].reason == VCPU_EXIT_BUDGET ? 18 : 12));
    }
    code_cache_flush();
}
/* A store whose page holds decoded code takes the exact path whatever page
 * the region cache was primed with, so the decoded bundle is refreshed. */
static void test_native_store_code_page(struct vcpu *v)
{
    const gpa_t code = DATA + 0x5000, primed = DATA + 0x6000;
    for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS; mode <= CODE_VALIDITY_NONE; ++mode) {
        CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
        code_cache_flush();
        bundle(code, 1, nop, add(8,9,10), nop);
        bundle(BASE, 0x11, store(3,9,10), nop, validator_branch(BASE, BASE));
        restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; v->s.pr = 1;
        uint64_t replacement = 1 | (nop << 5) | (add(11,9,10) << 46);
        v->s.gr[9] = 0; v->s.gr[10] = primed;
        validator_run(v, 3); validator_run(v, 3); /* translate, then seal */
        const struct ia64_bundle *decoded;
        CHECK(code_get_bundle(v, code, &decoded).kind == IA64_STEP_COMPLETE);
        CHECK(decoded->insn[1].operands.integer.destination == 8 && code_page_has_code(code));
        v->s.gr[9] = replacement; v->s.gr[10] = code;
        v->physical_region = NULL;
        CHECK(ia64_physical_region(v, primed, 8) != NULL);
        validator_run(v, 3);
        CHECK(code_get_bundle(v, code, &decoded).kind == IA64_STEP_COMPLETE);
        CHECK(decoded->insn[1].operands.integer.destination == 11);
    }
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    code_cache_flush();
}
/* Calls of the exact helper from generated code: tier0-test links with
 * --wrap=tier0_helper (mk/tier0.mk), and only generated code (and these
 * tests) reach tier0_helper from outside engine/tier0/tier0.c. */
unsigned __real_tier0_helper(struct vcpu *v, const Ia64Instruction *i, struct tier0_execution *e);
unsigned __wrap_tier0_helper(struct vcpu *v, const Ia64Instruction *i, struct tier0_execution *e);
unsigned __wrap_tier0_helper(struct vcpu *v, const Ia64Instruction *i, struct tier0_execution *e)
{ ++native_helper_calls; return __real_tier0_helper(v, i, e); }
/* Calls of the data translation cache's fill (a cache miss). */
static uint64_t native_dtc_fills;
void *__real_tier0_data_pointer(struct vcpu *v, const Ia64Instruction *i, struct tier0_execution *e);
void *__wrap_tier0_data_pointer(struct vcpu *v, const Ia64Instruction *i, struct tier0_execution *e);
void *__wrap_tier0_data_pointer(struct vcpu *v, const Ia64Instruction *i, struct tier0_execution *e)
{ ++native_dtc_fills; return __real_tier0_data_pointer(v, i, e); }
/* Calls of the store check's slow path (the C check that also caches the
 * code-page bitmap span for the inline test). */
static uint64_t native_span_checks;
uint64_t __real_tier0_store_tracked(gpa_t page, struct tier0_code_span *span);
uint64_t __wrap_tier0_store_tracked(gpa_t page, struct tier0_code_span *span);
uint64_t __wrap_tier0_store_tracked(gpa_t page, struct tier0_code_span *span)
{ ++native_span_checks; return __real_tier0_store_tracked(page, span); }
/* One instruction in a sealed region against ia64_execute (equivalence);
 * native: generated code completed it without the exact helper. */
static void native_single(struct vcpu *v, struct vcpu *ref, const Ia64Instruction *i, bool native)
{
    uint64_t before = native_helper_calls;
    equivalence(v, ref, i);
    if (native && native_helper_calls != before)
        LOGE("exact helper for %s qp=%u slot=%u unit=%u", ia64_opcode_info(i->opcode)->name, i->qp, i->slot, i->unit);
    if (native) CHECK(native_helper_calls == before);
}
/* SDM Vol.3 br.cloop: LC != 0 decrements LC and branches; otherwise falls
 * through. qp is ignored (the decoder forces 0); slot 2 only. */
static void test_native_cloop(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t counts[] = {0, 1, 2, UINT64_MAX};
    for (unsigned trial = 0; trial < 512; ++trial) {
        unsigned slot = trial % 16 == 15 ? 1 : 2;
        randomized(v); v->s.slot = slot;
        if (trial & 1) v->s.ip |= UINT64_C(1) << 63;
        v->s.ar[AR_LC] = trial % 5 == 4 ? random64() : counts[trial % 5 % 4];
        Ia64Instruction i = {.opcode = IA64_OP_BR_CLOOP, .valid = true, .unit = IA64_UNIT_B,
            .address = v->s.ip, .slot = slot, .operands.branch.displacement =
                (int64_t)(random64() & ~UINT64_C(15)) >> (trial % 40)};
        i.operands.branch.displacement &= ~INT64_C(15);
        native_single(v, ref, &i, slot == 2);
    }
}
/* SDM Vol.3 mov ar (I-unit forms) for ar.lc and ar.pfs (whose reserved
 * fields fault, Vol.1 §3.1.8.12); M-unit forms of an I-unit AR are
 * illegal, and other ARs keep the exact helper. */
static void test_native_lc(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_MOV_IMMAR, IA64_OP_MOV_GRAR, IA64_OP_MOV_ARGR};
    const uint64_t pfs_reserved = (UINT64_C(15) << 58) | (UINT64_C(0x3fff) << 38);
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(ops); ++n)
        for (unsigned trial = 0; trial < 768; ++trial) {
            randomized(v);
            v->s.ar[AR_LC] = random64(); v->s.ar[AR_PFS] = random64() & ~pfs_reserved;
            bool m_unit = trial % 7 == 3, other = trial % 9 == 4, pfs = trial % 2;
            unsigned gr = trial % 3 == 0 ? 48 : trial % 11 == 5 ? 0 : 8;
            Ia64Instruction i = {.opcode = ops[n], .valid = true, .address = BASE, .slot = 1,
                .unit = m_unit ? IA64_UNIT_M : IA64_UNIT_I, .qp = trial % 4 ? 0 : trial % 64,
                .operands.system = {.destination = gr, .source = other ? AR_EC : pfs ? AR_PFS : AR_LC,
                    .immediate = (int64_t)random64() >> (trial % 56)}};
            if (pfs && ops[n] == IA64_OP_MOV_GRAR && trial % 5)
                ia64_gr_write(&v->s, gr, ia64_gr_read(&v->s, gr) & ~pfs_reserved, ia64_nat_read(&v->s, gr));
            bool native = !m_unit && !other;
            bool qp = ia64_pr_read(&v->s, i.qp);
            if (ops[n] == IA64_OP_MOV_GRAR && qp && ia64_nat_read(&v->s, gr)) native = false;
            if (ops[n] == IA64_OP_MOV_GRAR && qp && pfs && (ia64_gr_read(&v->s, gr) & pfs_reserved)) native = false;
            if (ops[n] == IA64_OP_MOV_IMMAR && pfs && ((uint64_t)i.operands.system.immediate & pfs_reserved))
                native = false;
            if (ops[n] == IA64_OP_MOV_ARGR && !gr) native = false;
            native_single(v, ref, &i, native);
        }
}
/* SDM Vol.3 mov b=r (NaT source faults), mov r=b, and the hints (no
 * architectural effect). */
static void test_native_branch_moves(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode hints[] = {IA64_OP_HINT_M, IA64_OP_HINT_I, IA64_OP_HINT_F, IA64_OP_HINT_X};
    for (unsigned trial = 0; trial < 512; ++trial) {
        randomized(v);
        unsigned gr = trial % 3 == 0 ? 48 : trial % 11 == 5 ? 0 : 8, b = trial % 8;
        Ia64Instruction to = {.opcode = IA64_OP_MOV_GRBR, .valid = true, .unit = IA64_UNIT_I, .address = BASE,
            .slot = 1, .qp = trial % 4 ? 0 : trial % 64, .operands.system = {.destination = gr, .source = b}};
        bool qp = ia64_pr_read(&v->s, to.qp);
        native_single(v, ref, &to, !(qp && ia64_nat_read(&v->s, gr)));
        randomized(v);
        Ia64Instruction from = to; from.opcode = IA64_OP_MOV_BRGR;
        native_single(v, ref, &from, gr != 0);
        randomized(v);
        Ia64Instruction hint = {.opcode = hints[trial % 4], .valid = true, .unit = IA64_UNIT_M, .address = BASE,
            .slot = 1, .qp = trial % 64};
        native_single(v, ref, &hint, true);
    }
}
/* SDM Vol.3 addp4/shladdp4: the low 32 bits of the sum, with bits 62:61
 * taken from r3 bits 31:30; NaT is the OR of the sources. */
static void test_native_addp4(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_ADDP4, IA64_OP_ADDP4_IMM, IA64_OP_SHLADDP4};
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(ops); ++n)
        for (unsigned trial = 0; trial < 384; ++trial) {
            randomized(v);
            Ia64Instruction i = {.opcode = ops[n], .valid = true, .unit = IA64_UNIT_I, .address = BASE,
                .slot = 1, .qp = trial % 64, .operands.integer = {.destination = 8, .source1 = 9,
                    .source2 = 10, .immediate = (int64_t)random64() >> 50}};
            if (ops[n] == IA64_OP_SHLADDP4) i.operands.integer.immediate = 1 + trial % 4;
            if (trial % 3 == 0) { i.operands.integer.destination = 48; i.operands.integer.source2 = 49; }
            if (trial % 5 == 0) i.operands.integer.source1 = 0;
            if (trial % 7 == 0) ia64_gr_write(&v->s, 10, (uint64_t)(trial & 3) << 30 | UINT64_C(0xffffffff), false);
            bool native = true;
            if (trial % 13 == 0) { i.operands.integer.destination = 0; native = false; }
            native_single(v, ref, &i, native);
        }
}
/* SDM Vol.3 cmp/cmp4 parallel types and tbit/tnat (all update types):
 * and clears both targets when the condition is false or a source is NaT,
 * or sets both when it is true, or.andcm sets p1 and clears p2 when it is
 * true; a normal-type tbit writes the condition and its complement. */
static void test_native_predicates(struct vcpu *v, struct vcpu *ref)
{
    enum relation { REL_EQ,REL_NE,REL_LT,REL_LE,REL_GT,REL_GE,REL_LTU,REL_LEU,REL_GTU,REL_GEU };
    struct compare_desc { bool valid, word, immediate; enum relation relation; Ia64PredicateUpdate update; };
    static const struct compare_desc compares[IA64_OP_COUNT] = {
#include "ia64/decode/compare.inc"
    };
    for (unsigned op = 0; op < IA64_OP_COUNT; ++op) {
        if (!compares[op].valid || compares[op].update == IA64_PRED_UPDATE_NORMAL) continue;
        for (unsigned trial = 0; trial < 160; ++trial) {
            randomized(v);
            Ia64Instruction i = {.opcode = op, .valid = true, .unit = IA64_UNIT_I, .address = BASE,
                .slot = 1, .qp = trial % 64, .operands.integer = {.source1 = trial % 4 ? 49 : 0,
                    .source2 = 50, .predicate1 = trial % 64, .predicate2 = (trial * 7 + 1) % 64,
                    .immediate = (int64_t)(trial % 2 ? random64() : 0) >> 50}};
            if (trial % 3 == 0) ia64_gr_write(&v->s, 49, ia64_gr_read(&v->s, 50), false);
            if (trial % 17 == 0) i.operands.integer.predicate2 = i.operands.integer.predicate1;
            bool nat = ia64_nat_read(&v->s, 50) ||
                (!compares[op].immediate && ia64_nat_read(&v->s, i.operands.integer.source1));
            bool native = i.operands.integer.predicate1 != i.operands.integer.predicate2 &&
                !(ia64_pr_read(&v->s, i.qp) && nat);
            native_single(v, ref, &i, native);
        }
    }
    const Ia64Opcode tests[] = {IA64_OP_TBIT_Z, IA64_OP_TBIT_NZ, IA64_OP_TNAT_Z, IA64_OP_TNAT_NZ};
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(tests); ++n)
        for (unsigned trial = 0; trial < 640; ++trial) {
            randomized(v);
            Ia64PredicateUpdate update = (Ia64PredicateUpdate)(trial % 4);
            Ia64Instruction i = {.opcode = tests[n], .valid = true, .unit = IA64_UNIT_I, .address = BASE,
                .slot = 1, .qp = trial % 64, .pred_update = update,
                .compare_unc = update == IA64_PRED_UPDATE_NORMAL && trial % 8 >= 4,
                .operands.integer = {.source2 = trial % 5 ? 50 : 0, .predicate1 = (trial / 4) % 64,
                    .predicate2 = (trial * 5 + 3) % 64, .immediate = trial % 64}};
            if (trial % 23 == 0) i.operands.integer.predicate2 = i.operands.integer.predicate1;
            bool tnat = tests[n] == IA64_OP_TNAT_Z || tests[n] == IA64_OP_TNAT_NZ;
            bool nat = !tnat && ia64_nat_read(&v->s, i.operands.integer.source2);
            bool native = i.operands.integer.predicate1 != i.operands.integer.predicate2 &&
                !(ia64_pr_read(&v->s, i.qp) && nat);
            native_single(v, ref, &i, native);
        }
}
static uint64_t native_hits(void)
{
    struct tier0_stats s; tier0_get_stats(&s);
    return s.hits;
}
/* Decoded programs: mov.i ar.lc=imm8 (I27) inside a region does not end it,
 * and a br.cloop loop (B1, btype 5) runs without the exact helper. */
static void test_native_loops(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t lc5 = (UINT64_C(0x0a) << 27) | ((uint64_t)AR_LC << 20) | (UINT64_C(5) << 13);
    const uint64_t adds1 = (UINT64_C(8) << 37) | (UINT64_C(2) << 34) | (UINT64_C(1) << 13) |
        (UINT64_C(8) << 20) | (UINT64_C(8) << 6);  /* adds r8 = 1, r8 */
    code_cache_flush();
    bundle(BASE, 1, nop, lc5, add(8,8,9));
    bundle(BASE + 16, 0x11, nop, nop, validator_branch(BASE + 16, BASE));
    restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; v->s.gr[9] = 3;
    const struct ia64_bundle *decoded;
    CHECK(code_get_bundle(v, BASE, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[1].opcode == IA64_OP_MOV_IMMAR && decoded->insn[1].operands.system.source == AR_LC);
    validator_run(v, 6); validator_run(v, 6);
    for (unsigned pass = 0; pass < 3; ++pass) {
        uint64_t hits = native_hits(), helpers = native_helper_calls;
        v->s.ar[AR_LC] = 0;
        native_compare(v, ref, 6);
        CHECK(native_hits() == hits + 1 && native_helper_calls == helpers && v->s.ar[AR_LC] == 5);
    }
    code_cache_flush();
    bundle(BASE, 0x11, adds1, nop, validator_branch(BASE, BASE) | (UINT64_C(5) << 6));
    bundle(BASE + 16, 0x11, nop, nop, validator_branch(BASE + 16, BASE + 16));
    restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; v->s.ar[AR_LC] = 100;
    CHECK(code_get_bundle(v, BASE, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[2].opcode == IA64_OP_BR_CLOOP);
    validator_run(v, 6);
    uint64_t helpers = native_helper_calls;
    v->s.ar[AR_LC] = 4;
    native_compare(v, ref, 15);  /* four taken, one through, two more */
    CHECK(native_helper_calls == helpers && v->s.ar[AR_LC] == 0 && v->s.ip == BASE + 16);
    code_cache_flush();
}
/* States that only the interpreter executes (SDM Vol.2 §3.3.2: PSR.be,
 * PSR.is, the single-instruction bits; §7.1: lp/tb/ss traps; slot 3 is not
 * an instruction): each runs no Tier0 region even right after the same IP
 * ran warm in a plain state, and matches the interpreter. */
static void test_native_interp_states(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t iva = BASE + 0x18000;
    const struct { uint64_t psr; unsigned slot; } cases[] = {
        {IA64_PSR_BE, 0}, {IA64_PSR_IS, 0}, {0, 3}, {IA64_PSR_LP, 0}, {IA64_PSR_TB, 0}, {IA64_PSR_SS, 0},
        {IA64_PSR_ID, 0}, {IA64_PSR_DA, 0}, {IA64_PSR_DD, 0}, {IA64_PSR_ED, 0}, {IA64_PSR_IA, 0},
    };
    for (unsigned at = 0; at < IA64VMM_ARRAY_SIZE(cases); ++at)
        for (unsigned budget = 1; budget <= 6; budget += 5) {
            code_cache_flush();
            for (unsigned offset = 0; offset < 0x8000; offset += 0x100) bundle(iva + offset, 1, nop, nop, nop);
            bundle(BASE, 9, load(3,8,10), add(12,12,13), nop);
            bundle(BASE + 16, 0x11, nop, add(14,14,13), validator_branch(BASE + 16, BASE));
            restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; v->s.cr[CR_IVA] = iva;
            v->s.gr[10] = DATA; v->s.gr[13] = 1;
            uint64_t value = UINT64_C(0x0102030405060708); CHECK(gm_write(DATA, &value, 8));
            validator_run(v, 6); validator_run(v, 6);
            v->s.psr |= cases[at].psr; v->s.slot = cases[at].slot;
            struct tier0_stats before, after; tier0_get_stats(&before);
            native_compare(v, ref, budget);
            tier0_get_stats(&after);
            /* Later instructions (after the interpreter cleared a
             * single-instruction bit, or in a trap handler) may run in Tier0. */
            if (budget == 1) CHECK(after.executed_instructions == before.executed_instructions);
        }
    code_cache_flush();
}
/* Bundle comparison through the boundary cache. Inside one engine_run call (one stay) a
 * DMA into a compared bundle, made here by the platform break hook (no
 * interruption), is not seen: an accepted window. The
 * call's return ends the stay, so the next entry compares again although
 * the boundary entry matches, and retires the region. */
static unsigned native_dma_calls;
static bool native_dma_armed;
static bool native_break(struct vcpu *v, uint64_t imm, uint64_t ip)
{
    (void)v; (void)imm; (void)ip;
    if (native_dma_armed && ++native_dma_calls == 2)
        dma_bundle(BASE + 16, 0x11, nop, add(8,10,0), validator_branch(BASE + 16, BASE));
    return true;
}
static void test_native_stay_reset(struct vcpu *v)
{
    const uint64_t break_b = UINT64_C(0x40) << 6;
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    code_cache_flush(); engine_set_break_hook(native_break);
    bundle(BASE, 0x11, nop, nop, break_b);
    bundle(BASE + 16, 0x11, nop, add(8,9,0), validator_branch(BASE + 16, BASE));
    restart(v, ENGINE_TIER0, BASE); v->s.gr[9] = 11; v->s.gr[10] = 22;
    for (unsigned n = 0; n < 3; ++n) validator_run(v, 6);
    native_dma_calls = 0; native_dma_armed = true;
    validator_run(v, 12);   /* the second pass's break DMAs: its add still runs the old bundle */
    native_dma_armed = false;
    CHECK(native_dma_calls == 2 && v->s.gr[8] == 11 && v->s.ip == BASE && !v->code_entry_generation);
    /* The boundary entry matched; entering retired the region. */
    struct tier0_stats before, after; tier0_get_stats(&before);
    validator_run(v, 6); CHECK(v->s.gr[8] == 22);
    tier0_get_stats(&after);
    CHECK(after.boundary_stale == before.boundary_stale + 1 && after.translations == before.translations + 1);
    engine_set_break_hook(NULL); code_cache_flush();
}
/* Virtual scalar references (PSR.dt): ld8 r8=[r10]; st8 [r11]=r9 in one
 * bundle, ld4 r12=[r13] and the loop branch in the next. Data pages A and
 * B are 4 KiB DTRs 0 and 1 of region 7 at DATA + 0x2000 and + 0x3000
 * (below the IVT that virtual_start places at BASE + 0x18000). */
#define NATIVE_DVA UINT64_C(0xe000000004000000)
static void native_virtual_setup(struct vcpu *v, uint64_t flags_a)
{
    code_cache_flush(); virtual_start(v, ENGINE_TIER0, VIRTUAL_IP);
    v->s.psr |= IA64_PSR_DT;
    virtual_map(v, 1, 0, NATIVE_DVA, DATA + 0x2000, 12, flags_a);
    virtual_map(v, 1, 1, NATIVE_DVA + 0x1000, DATA + 0x3000, 12, FETCH_FLAGS);
    ia64_mmu_changed(v, IA64_MMU_ALL, 0, 0);
    bundle(BASE, 9, load(3,8,10), store(3,9,11), nop);
    bundle(BASE + 16, 0x11, load(2,12,13), nop, validator_branch(VIRTUAL_IP + 16, VIRTUAL_IP));
    for (unsigned n = 0; n < 3; ++n) {
        uint64_t x = random64(), y = random64();
        CHECK(gm_write(DATA + 0x2008 + 0x1000 * n, &x, 8) && gm_write(DATA + 0x2100 + 0x1000 * n, &y, 8));
    }
    v->s.gr[9] = random64(); v->s.gr[10] = NATIVE_DVA + 8;
    v->s.gr[11] = NATIVE_DVA + 0x1000 + 16; v->s.gr[13] = NATIVE_DVA + 0x100;
    validator_run(v, 6); validator_run(v, 6);   /* translate, then seal */
}
/* Warm virtual references run without the exact helper and, after the
 * first reference to each page filled the executor's translation cache,
 * without a fill; they match the interpreter. */
static void test_native_virtual_warm(struct vcpu *v, struct vcpu *ref)
{
    native_virtual_setup(v, FETCH_FLAGS);
    for (unsigned pass = 0; pass < 4; ++pass) {
        uint64_t helpers = native_helper_calls, fills = native_dtc_fills, x = random64();
        CHECK(gm_write(DATA + 0x2008, &x, 8));
        v->s.gr[9] = random64();
        native_compare(v, ref, 6);
        uint64_t stored; CHECK(gm_read(DATA + 0x3010, &stored, 8));
        CHECK(native_helper_calls == helpers && v->s.gr[8] == x && stored == v->s.gr[9]);
        CHECK(native_dtc_fills == fills);
        CHECK(v->fault.kind == IA64_FAULT_NONE && v->s.ip == VIRTUAL_IP);
    }
}
/* SDM Vol.2 §4.1: instruction and data translations are separate here.
 * Real I-side writers and purges must preserve warm data admissions. */
static void native_tlb_instruction(struct vcpu *v, unsigned x6, Ia64Opcode opcode)
{
    uint64_t raw = UINT64_C(1) << 37 | (uint64_t)x6 << 27 | UINT64_C(20) << 20 |
                   UINT64_C(21) << 13;
    Ia64Instruction in = ia64_decode_insn(IA64_UNIT_M, raw, v->s.ip, 0);
    CHECK(in.valid && in.opcode == opcode);
    uint64_t psr = v->s.psr;
    v->s.psr &= ~IA64_PSR_IC;
    CHECK(ia64_execute(v, &in).kind == IA64_STEP_COMPLETE);
    v->s.psr = psr;
}
static void test_native_virtual_iside(struct vcpu *v, struct vcpu *ref)
{
    enum { ITC, ITR, ITC_OVERLAP, ITR_OVERLAP, PTR_TC, PTR_TR, PTC_L, PTC_G,
           PTC_GA, PTC_E, REMOTE, REMOTE_ALL, CASES };
    const uint64_t va = NATIVE_DVA + 0x20000;
    for (unsigned n = 0; n < CASES; ++n) {
        native_virtual_setup(v, FETCH_FLAGS);
        v->s.cr[CR_IFA] = va; v->s.cr[CR_ITIR] = 12 << 2;
        if (n == PTR_TR) {
            v->s.gr[20] = 2; v->s.gr[21] = (DATA + 0x4000) | FETCH_FLAGS;
            native_tlb_instruction(v, 0x0f, IA64_OP_ITR_I);
        } else if (n != ITC && n != ITR)
            ia64_tc_insert(v, va, 0, (DATA + 0x4000) | FETCH_FLAGS, 12 << 2);
        validator_run(v, 6);
        uint64_t seq = v->mmu_seq, data_seq = v->mmu_dseq, fills = native_dtc_fills;
        v->s.gr[20] = va; v->s.gr[21] = 12 << 2;
        switch (n) {
        case ITC: case ITC_OVERLAP:
            v->s.gr[21] = (DATA + 0x5000) | FETCH_FLAGS;
            native_tlb_instruction(v, 0x2f, IA64_OP_ITC_I); break;
        case ITR: case ITR_OVERLAP:
            v->s.gr[20] = 2; v->s.gr[21] = (DATA + 0x5000) | FETCH_FLAGS;
            native_tlb_instruction(v, 0x0f, IA64_OP_ITR_I); break;
        case PTR_TC: case PTR_TR: native_tlb_instruction(v, 0x0d, IA64_OP_PTR_I); break;
        case PTC_L: native_tlb_instruction(v, 0x09, IA64_OP_PTC_L); break;
        case PTC_G: native_tlb_instruction(v, 0x0a, IA64_OP_PTC_G); break;
        case PTC_GA: native_tlb_instruction(v, 0x0b, IA64_OP_PTC_GA); break;
        case PTC_E: native_tlb_instruction(v, 0x34, IA64_OP_PTC_E); break;
        case REMOTE: case REMOTE_ALL:
            ia64_tlb_remote_purge(v, (v->s.rr[va >> 61] >> 8) & 0xffffff, va, 12,
                                  n == REMOTE_ALL); break;
        }
        CHECK(v->mmu_seq != seq && v->mmu_dseq == data_seq);
        native_compare(v, ref, 6);
        if (native_dtc_fills != fills)
            LOGE("I-side writer %u refilled the data cache", n);
        CHECK(native_dtc_fills == fills && v->fault.kind == IA64_FAULT_NONE);
    }
    code_cache_flush();
}
/* Reset advances the private sequences. Snapshot restoration may repeat
 * one with a different mapping, so it must clear the executor's admissions. */
static void test_native_virtual_sequence_reuse(struct vcpu *v, struct vcpu *ref)
{
    static struct vcpu saved;
    for (unsigned restore = 0; restore < 2; ++restore) {
        native_virtual_setup(v, FETCH_FLAGS);
        if (restore) {
            v->s.tr[1][0].valid = false;
            ia64_mmu_changed(v, IA64_MMU_TR, 1, 0);
            saved = *v;
            ia64_tc_insert(v, NATIVE_DVA, 1, (DATA + 0x2000) | FETCH_FLAGS, 12 << 2);
            validator_run(v, 6);
        }
        uint64_t seq = v->mmu_seq, data_seq = v->mmu_dseq, fills = native_dtc_fills;
        if (restore) {
            *v = saved;
            tier0_vcpu_state_restored(v);
            ia64_tc_insert(v, NATIVE_DVA, 1, (DATA + 0x4000) | FETCH_FLAGS, 12 << 2);
            CHECK(v->mmu_seq == seq && v->mmu_dseq == data_seq);
        } else {
            struct ia64_state state = v->s;
            vcpu_reset(v, NULL);
            v->s = state;
            v->s.tr[1][0].pte = (DATA + 0x4000) | FETCH_FLAGS;
            CHECK(v->mmu_seq != seq && v->mmu_dseq != data_seq);
        }
        native_compare(v, ref, 6);
        uint64_t moved;
        CHECK(gm_read(DATA + 0x4008, &moved, 8) && v->s.gr[8] == moved);
        CHECK(native_dtc_fills > fills && v->fault.kind == IA64_FAULT_NONE);
    }
    code_cache_flush();
}
/* Every change of the translation, its rights or the vCPU after warm
 * entries (SDM Vol.2 §4.1: TLB, RR/PKR, PSR.cpl/pk; Table 5-6 fault
 * priority) gives the interpreter's result. Architectural writers log
 * their changes (ia64_mmu_changed); a restored state calls
 * tier0_vcpu_state_restored. */
static void test_native_virtual_changes(struct vcpu *v, struct vcpu *ref, struct vcpu *third)
{
    enum { NONE, DIRTY, ACCESSED, READ_ONLY, NAT_PAGE, UC, MMIO, ABSENT, MOVED,
           RESTORED, OTHER_VCPU, UNALIGNED, NAT_BASE, PURGE, HOOK, LOADED_ONLY, CASES };
    for (unsigned n = 0; n < CASES; ++n) {
        /* LOADED_ONLY: page A is clean (D = 0); its loads fill the cache. */
        native_virtual_setup(v, n == LOADED_ONLY ? FETCH_FLAGS & ~UINT64_C(0x40) : FETCH_FLAGS);
        if (n == PURGE) {
            v->s.tr[1][0].valid = false;
            ia64_tc_insert(v, NATIVE_DVA, 1, (DATA + 0x2000) | FETCH_FLAGS, 48);
            validator_run(v, 6);
        }
        native_compare(v, ref, 6);
        struct vcpu *actual = v, *expected_ref = ref;
        bool logged = true;
        enum ia64_fault_kind fault = IA64_FAULT_NONE;
        switch (n) {
        case DIRTY: v->s.tr[1][1].pte &= ~UINT64_C(0x40); fault = IA64_FAULT_DIRTY_BIT; break;
        case ACCESSED: v->s.tr[1][0].pte &= ~UINT64_C(0x20); fault = IA64_FAULT_ACCESS_BIT; break;
        case READ_ONLY: v->s.tr[1][1].pte &= ~(UINT64_C(7) << 9); fault = IA64_FAULT_ACCESS_RIGHTS; break;
        case NAT_PAGE: v->s.tr[1][0].pte |= UINT64_C(7) << 2; fault = IA64_FAULT_DATA_NAT; break;
        case UC: v->s.tr[1][0].pte |= UINT64_C(4) << 2; break;
        case MMIO: v->s.tr[1][0].pte = DEVICE | FETCH_FLAGS; break;
        case ABSENT: v->s.tr[1][0].valid = false; fault = IA64_FAULT_ALT_DTLB; break;
        case MOVED: v->s.tr[1][0].pte = (DATA + 0x4000) | FETCH_FLAGS; break;
        case RESTORED: v->s.tr[1][0].pte = (DATA + 0x4000) | FETCH_FLAGS; logged = false;
            tier0_vcpu_state_restored(v); break;
        case OTHER_VCPU:
            /* Another vCPU on the same executor index, at the same MMU
             * sequence, with page A mapped elsewhere. */
            *ref = *v; ref->check_workspace = NULL; ref->check_workspace_pages = 0;
            ref->s.tr[1][0].pte = (DATA + 0x4000) | FETCH_FLAGS; logged = false;
            actual = ref; expected_ref = third; break;
        case UNALIGNED: v->s.gr[10] += 1; logged = false; fault = IA64_FAULT_UNALIGNED; break;
        case NAT_BASE: v->s.nat[10] = true; logged = false; fault = IA64_FAULT_NAT; break;
        case PURGE: {
            Ia64Instruction i = {.opcode = IA64_OP_PTC_L, .valid = true, .unit = IA64_UNIT_M,
                .operands.system = {.source = 20, .register_index = 21}};
            v->s.gr[20] = 48; v->s.gr[21] = NATIVE_DVA;
            CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
            logged = false; fault = IA64_FAULT_ALT_DTLB; break;
        }
        case HOOK: v->memory_hook = no_region_hook; logged = false; break;
        case LOADED_ONLY: v->s.gr[11] = NATIVE_DVA + 16; logged = false; fault = IA64_FAULT_DIRTY_BIT; break;
        default: logged = false; break;
        }
        if (logged) ia64_mmu_changed(v, IA64_MMU_ALL, 0, 0);
        /* Three instructions reach the first fault's handler (nop
         * bundles); then the remaining three of the pass. */
        uint64_t reads_before = reads;
        native_compare(actual, expected_ref, 3);
        if (actual->fault.kind != fault)
            LOGE("virtual data case %u: fault %s, expected %s", n, ia64_fault_name(actual->fault.kind),
                 ia64_fault_name(fault));
        CHECK(actual->fault.kind == fault);
        native_compare(actual, expected_ref, 3);
        if (n == MMIO) CHECK(reads == reads_before + 4 && v->s.gr[8] == UINT64_C(0xfedcba9876543210));
        if (n == MOVED || n == RESTORED || n == OTHER_VCPU) {
            uint64_t moved; CHECK(gm_read(DATA + 0x4008, &moved, 8) && actual->s.gr[8] == moved);
        }
        v->memory_hook = NULL;
    }
    code_cache_flush();
}
/* Rights depend on PSR.cpl and PSR.pk, which the MMU sequence does not
 * cover: a translation one region used must not serve another region of
 * the same IP and sequence under another PSR. Both regions are warm, page A
 * then loses its rights for the second one (pl 0; a key without a PKR). */
static void test_native_virtual_class(struct vcpu *v, struct vcpu *ref)
{
    for (unsigned key = 0; key < 2; ++key) {
        native_virtual_setup(v, FETCH_FLAGS);
        const uint64_t base = v->s.psr, other = key ? base | IA64_PSR_PK : base | (UINT64_C(3) << 32);
        if (key) v->s.pkr[0] = 1;
        v->s.psr = other; v->s.ip = VIRTUAL_IP; validator_run(v, 6); validator_run(v, 6);
        if (key) v->s.tr[1][0].itir |= 256;
        else v->s.tr[1][0].pte &= ~(UINT64_C(3) << 7);
        ia64_mmu_changed(v, IA64_MMU_TR, 1, 0);
        v->s.psr = base; v->s.ip = VIRTUAL_IP; validator_run(v, 6);
        CHECK(v->fault.kind == IA64_FAULT_NONE);
        v->s.psr = other; v->s.ip = VIRTUAL_IP;
        native_compare(v, ref, 3);
        CHECK(v->fault.kind == (key ? IA64_FAULT_KEY_MISS : IA64_FAULT_ACCESS_RIGHTS));
    }
    code_cache_flush();
}
/* Architectural writers and purges after warm references, each against
 * the interpreter (SDM Vol.2 §4.1): an RR write switching region 3 to an
 * address space that maps the same VA elsewhere; a PKR write-disable;
 * a remote TC purge as apply_purges performs it (ia64_tlb_remote_purge). */
static void native_system(struct vcpu *v, Ia64Opcode opcode, uint64_t value, uint64_t index)
{
    Ia64Instruction i = {.opcode = opcode, .valid = true, .unit = IA64_UNIT_M, .address = v->s.ip,
        .operands.system = {.destination = 20, .source = 21, .register_index = 21}};
    v->s.gr[20] = value; v->s.gr[21] = index; v->s.nat[20] = v->s.nat[21] = false;
    CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
}
static void test_native_virtual_writers(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t va = UINT64_C(0x6000000004002000), rr_a = (UINT64_C(0x555) << 8) | (12 << 2),
                   rr_b = (UINT64_C(0x666) << 8) | (12 << 2);
    for (unsigned n = 0; n < 3; ++n) {
        native_virtual_setup(v, FETCH_FLAGS);
        v->s.rr[3] = rr_b; virtual_map(v, 1, 2, va, DATA + 0x4000, 12, FETCH_FLAGS);
        v->s.rr[3] = rr_a; virtual_map(v, 1, 3, va, DATA + 0x2000, 12, FETCH_FLAGS);
        if (n == 1) {
            v->s.psr |= IA64_PSR_PK; v->s.pkr[0] = (UINT64_C(7) << 8) | 1; v->s.pkr[1] = 1;
            v->s.tr[1][1].itir |= UINT64_C(7) << 8;
        }
        if (n == 2) {
            v->s.tr[1][1].valid = false;
            ia64_tc_insert(v, NATIVE_DVA + 0x1000, 1, (DATA + 0x3000) | FETCH_FLAGS, 12 << 2);
        }
        ia64_mmu_changed(v, IA64_MMU_ALL, 0, 0);
        v->s.gr[10] = n == 0 ? va + 8 : NATIVE_DVA + 8;
        v->s.ip = VIRTUAL_IP; v->s.slot = 0;
        validator_run(v, 6); validator_run(v, 6); validator_run(v, 6);
        uint64_t fills = native_dtc_fills;
        native_compare(v, ref, 6);
        CHECK(native_dtc_fills == fills && v->fault.kind == IA64_FAULT_NONE);
        enum ia64_fault_kind fault = IA64_FAULT_NONE;
        if (n == 0) native_system(v, IA64_OP_MOV_GRRR, rr_b, va);
        if (n == 1) { native_system(v, IA64_OP_MOV_GRPKR_INDEXED, (UINT64_C(7) << 8) | 3, 0);
                      fault = IA64_FAULT_KEY_PERMISSION; }
        if (n == 2) { ia64_tlb_remote_purge(v, 0x123400 >> 8, NATIVE_DVA + 0x1000, 12, false);
                      fault = IA64_FAULT_ALT_DTLB; }
        native_compare(v, ref, 3);
        CHECK(v->fault.kind == fault);
        if (n == 0) { uint64_t other; CHECK(gm_read(DATA + 0x4008, &other, 8) && v->s.gr[8] == other); }
    }
    code_cache_flush();
}
/* Base-update forms (SDM Vol.3 ld/st with imm9 or register increment):
 * the base is written after the reference, also when the load's
 * destination is the increment register. */
static void test_native_virtual_update(struct vcpu *v, struct vcpu *ref)
{
    for (unsigned trial = 0; trial < 64; ++trial) {
        native_virtual_setup(v, FETCH_FLAGS);
        bool store = trial & 1, reg = trial & 2;
        unsigned width = (trial >> 2) & 3;
        Ia64Instruction i = {.opcode = (store ? IA64_OP_ST1 : IA64_OP_LD1) + width, .valid = true,
            .unit = IA64_UNIT_M, .address = VIRTUAL_IP, .slot = 1, .imm_base_update = !reg, .reg_base_update = reg,
            .operands.memory = {.destination = 8, .source = trial & 16 ? 8 : 9, .base = 10,
                .immediate = trial & 32 ? -8 : 16}};
        if (reg) i.operands.memory.source = 9;
        v->s.ip = VIRTUAL_IP; v->s.slot = 1;
        v->s.gr[10] = NATIVE_DVA + 0x1000 + 64; v->s.gr[9] = (trial & 32) ? (uint64_t)-8 : 24;
        v->s.nat[9] = false;
        native_single(v, ref, &i, true);
        native_single(v, ref, &i, true);
    }
    code_cache_flush();
}
/* A 64 KiB data page: each of its 4 KiB pages has its own cache entry with
 * the right host address. */
static void test_native_virtual_large_page(struct vcpu *v, struct vcpu *ref)
{
    native_virtual_setup(v, FETCH_FLAGS);
    v->s.tr[1][0] = (struct ia64_tlb_entry){0};
    virtual_map(v, 1, 0, NATIVE_DVA, DATA, 16, FETCH_FLAGS);
    ia64_mmu_changed(v, IA64_MMU_TR, 1, 0);
    const uint64_t offsets[] = {0x2008, 0x5108, 0x7ff8, 0x3010};
    for (unsigned pass = 0; pass < 3; ++pass)
        for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(offsets); ++k) {
            uint64_t x = random64(); CHECK(gm_write(DATA + offsets[k], &x, 8));
            v->s.ip = VIRTUAL_IP; v->s.slot = 0;
            v->s.gr[10] = NATIVE_DVA + offsets[k]; v->s.gr[13] = NATIVE_DVA + offsets[(k + 1) % 4];
            v->s.gr[11] = NATIVE_DVA + 0x1000 + 16;
            uint64_t fills = native_dtc_fills;
            native_compare(v, ref, 6);
            CHECK(v->s.gr[8] == x && v->fault.kind == IA64_FAULT_NONE);
            if (pass == 2) CHECK(native_dtc_fills == fills);
        }
    code_cache_flush();
}
/* A virtual store into a page holding decoded code takes the exact path,
 * so the decoded bundle is refreshed: when the store's page is newly
 * mapped (a cache miss), and when the page was cached before its bundle
 * was decoded. */
static void test_native_virtual_code_store(struct vcpu *v)
{
    for (unsigned cached = 0; cached < 2; ++cached) {
        native_virtual_setup(v, FETCH_FLAGS);
        const gpa_t code = DATA + 0x5000;
        bundle(code, 1, nop, add(8,9,10), nop);
        v->s.tr[1][1].pte = code | FETCH_FLAGS; ia64_mmu_changed(v, IA64_MMU_TR, 1, 1);
        v->s.gr[11] = NATIVE_DVA + 0x1000 + 0x100;
        if (cached) validator_run(v, 6);   /* stores into the page while it holds no code */
        const struct ia64_bundle *decoded;
        CHECK(code_get_bundle(v, code, &decoded).kind == IA64_STEP_COMPLETE);
        CHECK(decoded->insn[1].operands.integer.destination == 8);
        v->s.gr[11] = NATIVE_DVA + 0x1000;
        v->s.gr[9] = 1 | (nop << 5) | (add(11,9,10) << 46);
        uint64_t helpers = native_helper_calls;
        validator_run(v, 6);
        CHECK(v->fault.kind == IA64_FAULT_NONE && native_helper_calls > helpers);
        CHECK(code_get_bundle(v, code, &decoded).kind == IA64_STEP_COMPLETE);
        CHECK(decoded->insn[1].operands.integer.destination == 11);
    }
    code_cache_flush();
}
/* Intel SDM Vol.2: CMP r64, r/m64 (REX.W 3B /r), ADD r64, r/m64 (REX.W 03
 * /r), TEST r/m8, imm8 (F6 /0 ib), CMP r/m8, imm8 (80 /7 ib), SHRD r/m64,
 * r64, imm8 (REX.W 0F AC /r ib), SUB r64, r/m64 (REX.W 2B /r), BT r/m64,
 * r64 (REX.W 0F A3 /r); checked against the GNU assembler's
 * encodings of the same operands. */
static void test_native_encoder(void)
{
    uint8_t bytes[96];
    struct x86_buffer b = {.data = bytes, .capacity = sizeof(bytes)};
    x86_cmp_load(&b, X_RAX, X_R9, 0); x86_cmp_load(&b, X_RAX, X_R9, 0x10);
    x86_cmp_load(&b, X_R12, X_R9, 8); x86_add_load(&b, X_R10, X_R9, 0x18);
    x86_test_byte(&b, X_R9, 0x28, 2); x86_cmp_byte(&b, X_R9, 0x29, 5);
    x86_cmp_load(&b, X_RCX, X_RBX, 0x80); x86_test_byte(&b, X_RAX, 0, 1);
    x86_shrd(&b, X_RDX, X_RAX, 13); x86_shrd(&b, X_R10, X_R9, 63); x86_shrd(&b, X_RAX, X_RDX, 0);
    x86_sub_load(&b, X_RAX, X_R11, 8); x86_bt_reg(&b, X_RDX, X_RAX); x86_bt_reg(&b, X_R10, X_R8);
    const uint8_t expected[] = {0x49,0x3b,0x01, 0x49,0x3b,0x41,0x10, 0x4d,0x3b,0x61,0x08,
        0x4d,0x03,0x51,0x18, 0x41,0xf6,0x41,0x28,0x02, 0x41,0x80,0x79,0x29,0x05,
        0x48,0x3b,0x8b,0x80,0x00,0x00,0x00, 0xf6,0x00,0x01,
        0x48,0x0f,0xac,0xc2,0x0d, 0x4d,0x0f,0xac,0xca,0x3f, 0x48,0x0f,0xac,0xd0,0x00,
        0x49,0x2b,0x43,0x08, 0x48,0x0f,0xa3,0xc2, 0x4d,0x0f,0xa3,0xc2};
    encoded(expected, sizeof(expected), &b);
}
/* Emission stops at the buffer's capacity: a field that does not fit sets
 * failed and writes nothing past the end. */
static void test_native_encoder_bounds(void)
{
    for (unsigned capacity = 0; capacity <= 12; ++capacity)
        for (unsigned width = 0; width < 3; ++width) {
            uint8_t bytes[32];
            memset(bytes, 0xa5, sizeof(bytes));
            struct x86_buffer b = {.data = bytes, .capacity = capacity};
            x86_u8(&b, 1);
            if (width == 0) x86_u8(&b, 2);
            else if (width == 1) x86_u32(&b, 0x05040302);
            else x86_u64(&b, UINT64_C(0x0908070605040302));
            unsigned need = 1 + (width == 0 ? 1 : width == 1 ? 4 : 8);
            CHECK(b.failed == (capacity < need));
            for (unsigned n = capacity; n < sizeof(bytes); ++n) CHECK(bytes[n] == 0xa5);
            for (unsigned n = 0; n < capacity && n < need; ++n) CHECK(bytes[n] == n + 1);
        }
}
/* SDM Vol.3 st.rel: a store with release ordering; generated code stores
 * inline after the helper's fence (physical and virtual), with base
 * update. */
static uint64_t store_rel(unsigned width, unsigned source, unsigned base)
{ return (UINT64_C(4) << 37) | ((uint64_t)(0x34 + width) << 30) | (source << 13) | (base << 20); }
static void test_native_release(struct vcpu *v, struct vcpu *ref)
{
    for (unsigned virt = 0; virt < 2; ++virt)
        for (unsigned width = 0; width < 4; ++width) {
            const gpa_t page = DATA + 0x2000;
            uint64_t ip = virt ? VIRTUAL_IP : BASE, address = virt ? NATIVE_DVA : page;
            code_cache_flush();
            if (virt) {
                virtual_start(v, ENGINE_TIER0, VIRTUAL_IP); v->s.psr |= IA64_PSR_DT;
                virtual_map(v, 1, 0, NATIVE_DVA, page, 12, FETCH_FLAGS); ia64_mmu_changed(v, IA64_MMU_ALL, 0, 0);
            } else { restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; }
            bundle(BASE, 0x11, store_rel(width, 9, 11), nop, validator_branch(ip, ip));
            const struct ia64_bundle *decoded;
            CHECK(code_get_bundle(v, BASE, &decoded).kind == IA64_STEP_COMPLETE);
            CHECK(decoded->insn[0].opcode == IA64_OP_ST1REL + width && decoded->insn[0].mem_release);
            v->s.gr[11] = address + 64;
            validator_run(v, 3); validator_run(v, 3);
            /* The release store is fenced: SFENCE (0F AE F8) in its code. */
            unsigned at = boundary_region(ip, BASE);
            CHECK(at < tier0_arena.capacity);
            const uint8_t *code = tier0_arena.regions[at].entry;
            bool fenced = false;
            for (size_t n = 0; n + 3 <= tier0_arena.regions[at].bytes; ++n)
                fenced |= code[n] == 0x0f && code[n + 1] == 0xae && code[n + 2] == 0xf8;
            CHECK(fenced);
            for (unsigned pass = 0; pass < 3; ++pass) {
                uint64_t helpers = native_helper_calls;
                v->s.gr[9] = random64(); v->s.gr[11] = address + 64 + 8 * pass;
                native_compare(v, ref, 3);
                uint64_t stored = 0; CHECK(gm_read(page + 64 + 8 * pass, &stored, 1u << width));
                CHECK(stored == (v->s.gr[9] & ia64_low_mask(8u << width)) && native_helper_calls == helpers);
            }
        }
    code_cache_flush();
}
/* Decoded programs: mov b6=r8, mov r9=b6, hint.m, mov.i ar.pfs=r8 and
 * mov r10=ar.pfs inside one region keep it one region entry per pass. */
static void test_native_keeps_region(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t to_b6 = (UINT64_C(7) << 33) | (UINT64_C(8) << 13) | (UINT64_C(6) << 6);
    const uint64_t from_b6 = (UINT64_C(0x31) << 27) | (UINT64_C(6) << 13) | (UINT64_C(9) << 6);
    const uint64_t to_pfs = (UINT64_C(0x2a) << 27) | ((uint64_t)AR_PFS << 20) | (UINT64_C(8) << 13);
    const uint64_t from_pfs = (UINT64_C(0x32) << 27) | ((uint64_t)AR_PFS << 20) | (UINT64_C(10) << 6);
    const uint64_t hint = (UINT64_C(1) << 27) | (UINT64_C(1) << 26);
    code_cache_flush();
    bundle(BASE, 1, hint, to_b6, from_b6);
    bundle(BASE + 16, 1, nop, to_pfs, from_pfs);
    bundle(BASE + 32, 0x11, nop, nop, validator_branch(BASE + 32, BASE));
    restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC;
    const struct ia64_bundle *decoded;
    CHECK(code_get_bundle(v, BASE, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[0].opcode == IA64_OP_HINT_M && decoded->insn[1].opcode == IA64_OP_MOV_GRBR &&
          decoded->insn[2].opcode == IA64_OP_MOV_BRGR);
    CHECK(code_get_bundle(v, BASE + 16, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[1].opcode == IA64_OP_MOV_GRAR && decoded->insn[2].opcode == IA64_OP_MOV_ARGR &&
          decoded->insn[1].operands.system.source == AR_PFS);
    v->s.gr[8] = 0x1234;
    validator_run(v, 9); validator_run(v, 9);
    for (unsigned pass = 0; pass < 3; ++pass) {
        uint64_t hits = native_hits(), helpers = native_helper_calls;
        v->s.gr[8] = 0x1000 + pass; v->s.gr[9] = v->s.gr[10] = 0;
        native_compare(v, ref, 9);
        CHECK(native_hits() == hits + 1 && native_helper_calls == helpers);
        CHECK(v->s.br[6] == v->s.gr[8] && v->s.gr[9] == v->s.gr[8] && v->s.gr[10] == v->s.gr[8]);
    }
    code_cache_flush();
}
/* Decoded programs (decoder -> operands -> native code): addp4 (A1),
 * addp4 with imm14 (A4), tbit.z.and (I16), cmp.eq.or (A6) and
 * cmp.ne.or.andcm (A6), whose predicates guard the following adds. */
static void test_native_decoded(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t addp4 = (UINT64_C(8) << 37) | (UINT64_C(2) << 29) | (UINT64_C(10) << 20) |
                           (UINT64_C(9) << 13) | (UINT64_C(8) << 6);
    const uint64_t addp4_imm = (UINT64_C(8) << 37) | (UINT64_C(3) << 34) | (UINT64_C(1) << 36) |
                               (UINT64_C(0x15) << 27) | (UINT64_C(10) << 20) | (UINT64_C(0x2a) << 13) |
                               (UINT64_C(19) << 6);
    const uint64_t tbit_and = (UINT64_C(5) << 37) | (UINT64_C(1) << 36) | (UINT64_C(7) << 27) |
                              (UINT64_C(11) << 20) | (UINT64_C(5) << 14) | (UINT64_C(6) << 6);
    const uint64_t cmp_or = (UINT64_C(0xd) << 37) | (UINT64_C(1) << 33) | (UINT64_C(9) << 27) |
                            (UINT64_C(13) << 20) | (UINT64_C(12) << 13) | (UINT64_C(8) << 6);
    const uint64_t cmp_or_andcm = (UINT64_C(0xe) << 37) | (UINT64_C(1) << 33) | (UINT64_C(1) << 12) |
                                  (UINT64_C(11) << 27) | (UINT64_C(13) << 20) | (UINT64_C(12) << 13) |
                                  (UINT64_C(10) << 6);
    code_cache_flush();
    bundle(BASE, 1, addp4, tbit_and, cmp_or);
    bundle(BASE + 16, 1, addp4_imm, cmp_or_andcm, add(14,14,15) | 6);
    bundle(BASE + 32, 1, add(16,16,15) | 7, add(17,17,15) | 8, add(18,18,15) | 9);
    bundle(BASE + 48, 0x11, add(20,20,15) | 10, add(21,21,15) | 11, validator_branch(BASE + 48, BASE));
    restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC;
    const struct ia64_bundle *decoded;
    CHECK(code_get_bundle(v, BASE, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[0].opcode == IA64_OP_ADDP4 && decoded->insn[1].opcode == IA64_OP_TBIT_Z &&
          decoded->insn[1].pred_update == IA64_PRED_UPDATE_AND && decoded->insn[2].opcode == IA64_OP_CMP_EQ_OR);
    CHECK(code_get_bundle(v, BASE + 16, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[0].opcode == IA64_OP_ADDP4_IMM && decoded->insn[1].opcode == IA64_OP_CMP_NE_OR_ANDCM);
    v->s.gr[15] = 1;
    validator_run(v, 12); validator_run(v, 12);
    for (unsigned pass = 0; pass < 64; ++pass) {
        uint64_t helpers = native_helper_calls;
        v->s.gr[9] = random64(); v->s.gr[10] = random64();
        v->s.gr[11] = pass & 1 ? UINT64_C(1) << 5 : random64();
        v->s.gr[12] = pass & 2 ? v->s.gr[13] : random64();
        v->s.pr = random64() | 1;
        native_compare(v, ref, 12);
        CHECK(native_helper_calls == helpers && v->s.ip == BASE);
    }
    code_cache_flush();
}
/* SDM Vol.3 ld.s: ld8.s r8=[r10]; ld1.s r12=[r13] (x6 7 and 4) and the
 * loop branch. A virtual write-back page the data translation cache holds
 * loads natively with the NaT bit clear; a NaT base, an uncacheable page and
 * physical addressing (Vol.2 §4.4.6 Table 4-12) defer through the exact
 * helper. Every case matches the interpreter. */
static void test_native_speculative(struct vcpu *v, struct vcpu *ref)
{
    for (unsigned n = 0; n < 4; ++n) {
        bool physical = n == 3;
        if (physical) { code_cache_flush(); restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; }
        else native_virtual_setup(v, FETCH_FLAGS);
        uint64_t ip = physical ? BASE : VIRTUAL_IP;
        bundle(BASE, 9, load(7,8,10), load(4,12,13), nop);
        bundle(BASE + 16, 0x11, nop, nop, validator_branch(ip + 16, ip));
        v->s.ip = ip; v->s.slot = 0;
        uint64_t x = random64(), y = random64();
        CHECK(gm_write(DATA + 0x2008, &x, 8) && gm_write(DATA + 0x2100, &y, 8));
        v->s.gr[10] = (physical ? DATA + 0x2000 : NATIVE_DVA) + 8;
        v->s.gr[13] = (physical ? DATA + 0x2000 : NATIVE_DVA) + 0x100;
        validator_run(v, 6); validator_run(v, 6);   /* translate, then seal */
        if (n == 1) v->s.nat[10] = true;
        if (n == 2) {
            v->s.tr[1][0].pte = (v->s.tr[1][0].pte & ~UINT64_C(0x1c)) | (UINT64_C(4) << 2);
            ia64_mmu_changed(v, IA64_MMU_TR, 1, 0);
        }
        uint64_t helpers = native_helper_calls;
        native_compare(v, ref, 6);
        if (!n) CHECK(native_helper_calls == helpers && v->s.gr[8] == x && !v->s.nat[8] &&
                      v->s.gr[12] == (y & 0xff) && !v->s.nat[12]);
        else CHECK(native_helper_calls > helpers && v->s.nat[8]);
        v->s.nat[10] = false;
    }
    code_cache_flush();
}
/* IIPA (SDM Vol.2 §3.3.5.6) names the last bundle that completed with
 * PSR.ic = 1: a region of two native bundles stopped after each of its
 * instructions records the interpreter's bundle. */
static void test_native_iipa(struct vcpu *v, struct vcpu *ref)
{
    code_cache_flush();
    bundle(BASE, 0, nop, add(8,8,9), nop);
    bundle(BASE + 16, 0x11, nop, add(8,8,9), validator_branch(BASE + 16, BASE));
    for (uint64_t budget = 1; budget <= 6; ++budget) {
        restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; v->s.gr[9] = 3;
        validator_run(v, 6); validator_run(v, 6);
        v->last_successful_bundle = 0;
        uint64_t helpers = native_helper_calls;
        native_compare(v, ref, budget);
        CHECK(native_helper_calls == helpers);
        CHECK(v->last_successful_bundle == ref->last_successful_bundle);
        CHECK(v->last_successful_bundle == (budget > 3 ? BASE + 16 : BASE));
    }
    code_cache_flush();
}
/* SDM Vol.3 dep (I15, len 1..16) and dep imm (I14, len 1..64): the low len
 * bits of r2 (of the sign-extended 1-bit immediate) replace bits
 * pos..pos+len-1 of r3, truncated at bit 63, and the rest of r3 is kept; the
 * NaT is that of the sources (of r3 alone for the immediate form). */
static void test_native_deposit(struct vcpu *v, struct vcpu *ref)
{
    static const unsigned edges[][2] = {{0, 1}, {63, 1}, {0, 16}, {48, 16}, {60, 16}, {0, 64}, {1, 64},
                                        {63, 64}, {32, 33}, {31, 33}, {8, 56}, {56, 8}};
    for (unsigned form = 0; form < 2; ++form)
        for (unsigned trial = 0; trial < 1536; ++trial) {
            randomized(v);
            unsigned max = form ? 64 : 16, pos = random64() % 64, len = 1 + random64() % max;
            if (trial < IA64VMM_ARRAY_SIZE(edges)) {
                pos = edges[trial][0]; len = edges[trial][1] > max ? max : edges[trial][1];
            }
            uint64_t imm = pos | ((uint64_t)len << 6) | (form ? (uint64_t)(trial & 1) << 13 : 0);
            Ia64Instruction i = {.opcode = form ? IA64_OP_DEP_IMM : IA64_OP_DEP, .valid = true,
                .unit = IA64_UNIT_I, .address = BASE, .slot = 1, .qp = trial % 3 ? 0 : trial % 64,
                .operands.integer = {.destination = 8, .source1 = 9, .source2 = 10, .immediate = (int64_t)imm}};
            if (trial % 5 == 0) { i.operands.integer.destination = 48; i.operands.integer.source2 = 49; }
            if (trial % 7 == 0) i.operands.integer.source1 = 0;
            if (trial % 11 == 0) i.operands.integer.source2 = 8;
            bool native = true;
            if (trial % 13 == 0) { i.operands.integer.destination = 0; native = false; }
            if (trial % 17 == 0) { i.operands.integer.destination = 48; v->s.cfm &= ~UINT64_C(127); native = false; }
            native_single(v, ref, &i, native);
        }
}
/* SDM Vol.3 shrp: the 128-bit concatenation r2:r3 shifted right by count,
 * low 64 bits; equal sources rotate. Native with and without BMI2. */
static void test_native_shrp(struct vcpu *v, struct vcpu *ref)
{
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL);
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        tier0_flush(); hal_linux_host_isa_mask(enabled ? HOST_ISA_ALL : 0);
        for (unsigned trial = 0; trial < 1024; ++trial) {
            randomized(v);
            Ia64Instruction i = {.opcode = IA64_OP_SHRP_IMM, .valid = true, .unit = IA64_UNIT_I,
                .address = BASE, .slot = 1, .qp = trial % 3 ? 0 : trial % 64,
                .operands.integer = {.destination = trial & 64 ? 48 : 8, .source1 = trial & 128 ? 0 : 9,
                    .source2 = trial % 5 == 0 ? 9 : 10, .immediate = trial % 64}};
            native_single(v, ref, &i, true);
        }
    }
    tier0_flush(); hal_linux_host_isa_mask(old);
}
/* Advanced and check loads with an ALAT of no entries (ia64/load_store.c):
 * ld.a is an ordinary load from write-back memory, ld.c.clr and ld.c.nc
 * always reload (SDM Vol.3 ld: a missing ALAT entry performs the load), and
 * ld.sa defers as ld.s. Physical and virtual write-back references run
 * natively; a NaT base, misalignment, an uncacheable page and physical ld.sa
 * keep the exact helper (ld.a then returns zero without reading). */
static void test_native_advanced(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_LD1A, IA64_OP_LD2A, IA64_OP_LD4A, IA64_OP_LD8A,
        IA64_OP_LD1C_CLR, IA64_OP_LD2C_CLR, IA64_OP_LD4C_CLR, IA64_OP_LD8C_CLR,
        IA64_OP_LD1C_NC, IA64_OP_LD2C_NC, IA64_OP_LD4C_NC, IA64_OP_LD8C_NC,
        IA64_OP_LD1SA, IA64_OP_LD2SA, IA64_OP_LD4SA, IA64_OP_LD8SA};
    for (unsigned virt = 0; virt < 2; ++virt)
        for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(ops); ++n)
            for (unsigned trial = 0; trial < 48; ++trial) {
                unsigned size = 1u << (n % 4);
                bool sa = ops[n] >= IA64_OP_LD1SA && ops[n] <= IA64_OP_LD8SA;
                if (virt) { native_virtual_setup(v, FETCH_FLAGS); v->s.cfm = 96; }
                else { randomized(v); v->s.psr = IA64_PSR_IC; }
                uint64_t page = virt ? NATIVE_DVA : DATA + 0x2000, offset = 8 * (trial % 64);
                bool nat = trial % 7 == 3, unaligned = size > 1 && trial % 11 == 5, uc = trial % 13 == 6;
                if (unaligned) offset += 1;
                for (unsigned k = 0; k < 8; ++k) ram[DATA + 0x2000 - BASE + offset + k] = (uint8_t)random64();
                ia64_gr_write(&v->s, 10, page + offset, nat);
                if (uc && virt) {
                    v->s.tr[1][0].pte |= UINT64_C(4) << 2; ia64_mmu_changed(v, IA64_MMU_TR, 1, 0);
                } else if (uc) ia64_gr_write(&v->s, 10, (page + offset) | UINT64_C(1) << 63, nat);
                if (!virt) (void)ia64_physical_region(v, DATA, 8);
                Ia64Instruction i = {.opcode = ops[n], .valid = true, .unit = IA64_UNIT_M,
                    .address = virt ? VIRTUAL_IP : BASE, .slot = 1, .qp = trial % 4 ? 0 : trial % 64,
                    .operands.memory = {.destination = trial % 3 ? 8 : 48, .base = 10}};
                if (virt) { v->s.ip = VIRTUAL_IP; v->s.slot = 1; }
                bool qp = ia64_pr_read(&v->s, i.qp);
                native_single(v, ref, &i, !(sa && !virt) && (!qp || (!nat && !unaligned && !uc)));
            }
    code_cache_flush();
}
/* SDM Vol.3 lfetch: no memory access and no alignment check; the plain form
 * never faults, lfetch.fault takes the faults of a load (NaT base, TLB,
 * rights); post-increment forms update the base. Native: the plain form, and
 * the faulting form on a virtual write-back page the data translation cache
 * holds; the rest keeps the exact helper. */
static void test_native_lfetch(struct vcpu *v, struct vcpu *ref)
{
    for (unsigned virt = 0; virt < 2; ++virt)
        for (unsigned trial = 0; trial < 384; ++trial) {
            bool fault = trial & 1, reg = trial % 6 == 2, imm = trial % 6 == 4, nat = trial % 7 == 3;
            if (virt) { native_virtual_setup(v, FETCH_FLAGS); v->s.cfm = 96; }
            else { randomized(v); v->s.psr = IA64_PSR_IC; }
            uint64_t address = virt ? (trial % 9 == 4 ? NATIVE_DVA + 0x10000 : NATIVE_DVA) + trial % 4096
                                    : DATA + trial % 4096;
            ia64_gr_write(&v->s, 10, address, nat);
            ia64_gr_write(&v->s, 9, (uint64_t)(int64_t)(int8_t)random64(), trial % 10 == 9);
            Ia64Instruction i = {.opcode = fault ? IA64_OP_LFETCH_FAULT : IA64_OP_LFETCH, .valid = true,
                .unit = IA64_UNIT_M, .address = virt ? VIRTUAL_IP : BASE, .slot = 1,
                .qp = trial % 5 ? 0 : trial % 64, .reg_base_update = reg, .imm_base_update = imm,
                .operands.memory = {.source = 9, .base = trial % 8 == 7 ? 48 : 10, .immediate = imm ? -64 : 0}};
            if (i.operands.memory.base == 48) ia64_gr_write(&v->s, 48, address, nat);
            if (virt) { v->s.ip = VIRTUAL_IP; v->s.slot = 1; }
            bool qp = ia64_pr_read(&v->s, i.qp);
            bool native = !fault || (virt && !nat && address < NATIVE_DVA + 0x2000);
            if ((reg || imm) && nat) native = false;
            native_single(v, ref, &i, (!fault || virt) && (!qp || native));
        }
    code_cache_flush();
}
/* SDM Vol.3 mov r=pr, mov pr=r,mask17 and mov pr.rot=imm44: bulk moves of
 * the 64 predicates (pr0 reads 1 and is not written); a NaT source of
 * mov pr=r faults. */
static void test_native_pr_moves(struct vcpu *v, struct vcpu *ref)
{
    const Ia64Opcode ops[] = {IA64_OP_MOV_PRGR, IA64_OP_MOV_GRPR, IA64_OP_MOV_PR_ROT_IMM};
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(ops); ++n)
        for (unsigned trial = 0; trial < 768; ++trial) {
            randomized(v);
            unsigned gr = trial % 3 == 0 ? 48 : trial % 11 == 5 ? 0 : 8;
            uint64_t mask = ops[n] == IA64_OP_MOV_PR_ROT_IMM ? (uint64_t)((int64_t)random64() >> 20) << 16 :
                            (uint64_t)((int64_t)(random64() << 47) >> 47) & ~UINT64_C(1);
            Ia64Instruction i = {.opcode = ops[n], .valid = true, .unit = IA64_UNIT_I, .address = BASE,
                .slot = 1, .qp = trial % 4 ? 0 : trial % 64,
                .operands.system = {.destination = gr, .source = 0, .immediate = (int64_t)mask}};
            bool qp = ia64_pr_read(&v->s, i.qp), native = true;
            if (ops[n] == IA64_OP_MOV_PRGR && !gr) native = false;
            if (ops[n] == IA64_OP_MOV_GRPR && qp && ia64_nat_read(&v->s, gr)) native = false;
            native_single(v, ref, &i, native);
        }
}
/* Decoded program (decoder -> operands -> native code), one region per pass
 * with no exact helper: dep (I15), shrp (I10), ld8.a and ld8.c.clr (M1),
 * lfetch (M13), mov pr=r and mov r=pr (I23, I25). */
static void test_native_item_forms(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t dep = (UINT64_C(4) << 37) | (UINT64_C(55) << 31) | (UINT64_C(11) << 27) |
                         (UINT64_C(10) << 20) | (UINT64_C(9) << 13) | (UINT64_C(8) << 6);  /* dep r8=r9,r10,8,12 */
    const uint64_t shrp = (UINT64_C(5) << 37) | (UINT64_C(3) << 34) | (UINT64_C(13) << 27) |
                          (UINT64_C(10) << 20) | (UINT64_C(9) << 13) | (UINT64_C(11) << 6);  /* shrp r11=r9,r10,13 */
    const uint64_t lfetch = (UINT64_C(6) << 37) | (UINT64_C(0x2c) << 30) | (UINT64_C(12) << 20);
    const uint64_t to_pr = (UINT64_C(3) << 33) | (UINT64_C(0xff) << 24) | (UINT64_C(14) << 13) |
                           (UINT64_C(0x7f) << 6);    /* mov pr=r14,0xfffe (s = 0) */
    const uint64_t from_pr = (UINT64_C(0x33) << 27) | (UINT64_C(15) << 6);
    code_cache_flush();
    bundle(BASE, 1, load(0x0b, 12, 13), dep, shrp);
    bundle(BASE + 16, 1, load(0x23, 12, 13), to_pr, from_pr);
    bundle(BASE + 32, 1, lfetch, nop, nop);
    bundle(BASE + 48, 0x11, nop, nop, validator_branch(BASE + 48, BASE));
    restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC;
    const struct ia64_bundle *decoded;
    CHECK(code_get_bundle(v, BASE, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[0].opcode == IA64_OP_LD8A && decoded->insn[1].opcode == IA64_OP_DEP &&
          decoded->insn[2].opcode == IA64_OP_SHRP_IMM && decoded->insn[2].operands.integer.source1 == 9);
    CHECK(code_get_bundle(v, BASE + 16, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[0].opcode == IA64_OP_LD8C_CLR && decoded->insn[1].opcode == IA64_OP_MOV_GRPR &&
          decoded->insn[2].opcode == IA64_OP_MOV_PRGR);
    CHECK(code_get_bundle(v, BASE + 32, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[0].opcode == IA64_OP_LFETCH);
    v->s.gr[13] = DATA + 0x2040; v->s.gr[12] = DATA + 0x100;
    validator_run(v, 12); validator_run(v, 12);
    for (unsigned pass = 0; pass < 16; ++pass) {
        uint64_t helpers = native_helper_calls, x = random64();
        CHECK(gm_write(DATA + 0x2040, &x, 8));
        v->s.gr[9] = random64(); v->s.gr[10] = random64(); v->s.gr[14] = random64();
        v->s.pr = random64() | 1; v->s.ip = BASE; v->s.slot = 0;
        (void)ia64_physical_region(v, DATA, 8);
        native_compare(v, ref, 12);
        CHECK(native_helper_calls == helpers && v->s.gr[12] == x && v->s.ip == BASE);
    }
    code_cache_flush();
}
/* SDM Vol.3 chk.s: branch to the recovery target (IP-relative) when the
 * source's NaT bit (GR form) or NaTVal (FR form) is set, else nothing. The GR
 * form is native; the FR form keeps the helper (PSR.dfl/dfh, NaTVal). */
static void test_native_chk_s(struct vcpu *v, struct vcpu *ref)
{
    for (unsigned trial = 0; trial < 1024; ++trial) {
        randomized(v);
        bool fp = trial % 8 == 7;
        unsigned source = fp ? 2 + trial % 100 : trial % 5 == 0 ? 48 : trial % 13 == 6 ? 0 : 9;
        if (!fp && trial % 3 == 0) ia64_gr_write(&v->s, source, random64(), true);
        if (fp && trial % 3 == 0) ia64_fr_write(&v->s, source, ia64_fp_nat());
        Ia64Instruction i = {.opcode = IA64_OP_CHK_S, .valid = true, .unit = trial & 1 ? IA64_UNIT_I : IA64_UNIT_M,
            .address = BASE, .slot = trial % 3, .qp = trial % 4 ? 0 : trial % 64, .check_fp = fp,
            .operands.memory = {.source = source, .immediate = (int64_t)(random64() & ~UINT64_C(15)) >> (trial % 40)}};
        i.operands.memory.immediate &= ~INT64_C(15);
        v->s.slot = i.slot;
        native_single(v, ref, &i, !fp);
    }
}
static uint64_t adds_imm(unsigned dest, unsigned imm7, unsigned source)
{ return (UINT64_C(8) << 37) | (UINT64_C(2) << 34) | ((uint64_t)imm7 << 13) | ((uint64_t)source << 20) | (dest << 6); }
/* chk.s.i (I20) and chk.s.m with an FR (M21): r2/f2 at bits 19:13 and the
 * bundle displacement imm20b in 12:6, 32:20 and the sign at 36. */
static uint64_t chk_s(bool m_fp, unsigned source, int64_t bytes)
{
    uint64_t f = (uint64_t)(bytes / 16) & 0x1fffff;
    return (m_fp ? (UINT64_C(1) << 37) | (UINT64_C(3) << 33) : UINT64_C(1) << 33) | ((uint64_t)source << 13) |
           ((f & 0x7f) << 6) | (((f >> 7) & 0x1fff) << 20) | (((f >> 20) & 1) << 36);
}
/* Decoded program: a not-taken chk.s keeps its region going (one region
 * entry per pass, no exact helper for the GR form, one for the FR form); a
 * taken one leaves the region at its recovery target with the exact IP,
 * slot and retired count. */
static void test_native_chk_s_region(struct vcpu *v, struct vcpu *ref)
{
    for (unsigned fp = 0; fp < 2; ++fp) {
        code_cache_flush();
        if (fp) bundle(BASE, 1, chk_s(true, 9, 48), adds_imm(8, 1, 8), adds_imm(10, 1, 10));
        else bundle(BASE, 1, adds_imm(8, 1, 8), chk_s(false, 9, 48), adds_imm(10, 1, 10));
        bundle(BASE + 16, 1, adds_imm(11, 1, 11), nop, adds_imm(13, 1, 13));
        bundle(BASE + 32, 0x11, nop, nop, validator_branch(BASE + 32, BASE));
        bundle(BASE + 48, 0x11, adds_imm(12, 1, 12), nop, validator_branch(BASE + 48, BASE + 32));
        restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC;
        const struct ia64_bundle *decoded;
        CHECK(code_get_bundle(v, BASE, &decoded).kind == IA64_STEP_COMPLETE);
        CHECK(decoded->insn[fp ? 0 : 1].opcode == IA64_OP_CHK_S && decoded->insn[fp ? 0 : 1].check_fp == fp);
        validator_run(v, 9); validator_run(v, 9);
        for (unsigned pass = 0; pass < 4; ++pass) {
            uint64_t hits = native_hits(), helpers = native_helper_calls;
            native_compare(v, ref, 9);
            CHECK(native_hits() == hits + 1 && native_helper_calls == helpers + fp && v->s.ip == BASE);
        }
        if (fp) ia64_fr_write(&v->s, 9, ia64_fp_nat()); else v->s.nat[9] = true;
        for (uint64_t budget = 1; budget <= 9; ++budget) {
            v->s.ip = BASE; v->s.slot = 0;
            native_compare(v, ref, budget);
        }
    }
    code_cache_flush();
}
/* The upper tier's loop heads (tier0.h step 2): a branch taken from the
 * middle of a region is a back-edge only when its target is at or below the
 * branch itself, not below the region's last instruction. */
static unsigned loop_hot_calls, trace_hot_calls;
static uint64_t loop_sequence;
static const unsigned loop_due = 1;
static uint64_t loop_probe(struct vcpu *v, gpa_t pa) { (void)v; (void)pa; return 0; }
static enum tier0_upper_status loop_enter(struct vcpu *v, uint64_t *link, uint64_t budget, struct vcpu_exit *out)
{ (void)v; (void)link; (void)budget; (void)out; return TIER0_UPPER_DECLINED; }
static enum tier0_upper_hot loop_hot(struct vcpu *v, gpa_t pa, bool loop_head)
{ (void)v; (void)pa; ++*(loop_head ? &loop_hot_calls : &trace_hot_calls); return TIER0_HOT_REJECTED; }
static void loop_reclaim(void) {}
static void superblock_program(void);
static void test_native_loop_heads(struct vcpu *v)
{
    static const struct tier0_upper_ops ops = {.probe = loop_probe, .enter = loop_enter, .hot = loop_hot,
                                             .reclaim = loop_reclaim, .sequence = &loop_sequence, .loop_threshold = 1,
                                             .heads = &loop_sequence, .reclaim_due = &loop_due};
    code_cache_flush(); tier0_set_upper(&ops);
    /* chk.s at BASE (slot 1) jumps forward to BASE + 16, below the region's
     * last instruction at BASE + 32: not a loop head. */
    bundle(BASE, 1, nop, chk_s(false, 9, 16), nop);
    bundle(BASE + 16, 1, adds_imm(11, 1, 11), nop, nop);
    bundle(BASE + 32, 0x11, nop, nop, validator_branch(BASE + 32, BASE + 48));
    bundle(BASE + 48, 0x11, nop, nop, validator_branch(BASE + 48, BASE + 48));
    restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; v->s.nat[9] = true;
    loop_hot_calls = trace_hot_calls = 0;
    for (unsigned pass = 0; pass < 4; ++pass) { v->s.ip = BASE; v->s.slot = 0; validator_run(v, 3); }
    CHECK(loop_hot_calls == 0 && v->s.ip == BASE + 16 && v->s.slot == 1);
    /* The self-loop at BASE + 48 is one. */
    v->s.ip = BASE + 48; v->s.slot = 0; validator_run(v, 12);
    CHECK(loop_hot_calls > 0);
    /* With an upper tier, regions end at branches again, also where a
     * region translated without one exists (the key tells them apart). */
    tier0_set_upper(NULL);
    superblock_program();
    restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; v->s.gr[9] = 1; v->s.gr[10] = 2;
    validator_run(v, 15); validator_run(v, 15);
    uint64_t hits = native_hits();
    v->s.ip = BASE; v->s.slot = 0; validator_run(v, 15);
    CHECK(native_hits() == hits + 1);
    tier0_set_upper(&ops);
    v->s.ip = BASE; v->s.slot = 0; validator_run(v, 15); validator_run(v, 15);
    hits = native_hits();
    v->s.ip = BASE; v->s.slot = 0; validator_run(v, 15);
    CHECK(native_hits() > hits + 1 && v->s.ip == BASE);
    tier0_set_upper(NULL); code_cache_flush();
}
/* Stores test the code-page bitmap of their page inline (code validity
 * active): warm stores to untracked pages call no C check; a page that
 * becomes tracked after its span was cached, a tracked page of another RAM
 * region and, with validity inactive, a page with decoded code all take the
 * exact path, which refreshes the decoded bundle; reconfiguring frees the
 * bitmaps and the cached span with them. Physical and virtual stores. */
static void native_span_store(struct vcpu *v, gpa_t code, uint64_t store_address, unsigned dest)
{
    const struct ia64_bundle *decoded;
    uint64_t ip = v->s.psr & IA64_PSR_IT ? VIRTUAL_IP : BASE;
    /* Untracked stores first: the region and translation caches and the
     * span now cover the page. */
    v->s.gr[11] = store_address;
    for (unsigned n = 0; n < 2; ++n) { v->s.gr[9] = random64(); v->s.ip = ip; v->s.slot = 0; validator_run(v, 3); }
    bundle(code, 1, nop, add(8,9,10), nop);
    CHECK(code_get_bundle(v, code, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[1].operands.integer.destination == 8);
    uint64_t helpers = native_helper_calls;
    v->s.gr[9] = 1 | (nop << 5) | (add(dest,9,10) << 46);
    v->s.ip = ip; v->s.slot = 0;
    validator_run(v, 3);
    CHECK(native_helper_calls > helpers);
    CHECK(code_get_bundle(v, code, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[1].operands.integer.destination == dest);
}
static void test_native_store_span(struct vcpu *v)
{
    const gpa_t untracked = DATA + 0x4000, code = DATA + 0x5000, other = BASE + 0x60000;
    for (unsigned virt = 0; virt < 2; ++virt) {
        CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
        if (virt) {
            native_virtual_setup(v, FETCH_FLAGS);
            v->s.tr[1][1].pte = untracked | FETCH_FLAGS; ia64_mmu_changed(v, IA64_MMU_TR, 1, 1);
            bundle(BASE, 0x11, store(3,9,11), nop, validator_branch(VIRTUAL_IP, VIRTUAL_IP));
            v->s.gr[11] = NATIVE_DVA + 0x1000 + 0x80; v->s.ip = VIRTUAL_IP; v->s.slot = 0;
        } else {
            code_cache_flush();
            bundle(BASE, 0x11, store(3,9,11), nop, validator_branch(BASE, BASE));
            restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; v->s.pr = 1;
            v->s.gr[11] = untracked + 0x80;
        }
        validator_run(v, 3); validator_run(v, 3);   /* translate, then seal */
        uint64_t span_checks = native_span_checks, helpers = native_helper_calls;
        for (unsigned pass = 0; pass < 4; ++pass) {
            v->s.gr[9] = random64(); v->s.ip = virt ? VIRTUAL_IP : BASE; v->s.slot = 0;
            if (!virt) (void)ia64_physical_region(v, DATA, 8);
            validator_run(v, 3);
            uint64_t stored; CHECK(gm_read(untracked + 0x80, &stored, 8) && stored == v->s.gr[9]);
        }
        CHECK(native_span_checks <= span_checks + 1 && native_helper_calls == helpers);
        /* Tracked after the span was cached: the same span, a set bit. */
        if (virt) { v->s.tr[1][1].pte = code | FETCH_FLAGS; ia64_mmu_changed(v, IA64_MMU_TR, 1, 1); }
        native_span_store(v, code, virt ? NATIVE_DVA + 0x1000 : code, 11);
        /* Another RAM region (its own span). */
        if (virt) { v->s.tr[1][1].pte = other | FETCH_FLAGS; ia64_mmu_changed(v, IA64_MMU_TR, 1, 1); }
        native_span_store(v, other, virt ? NATIVE_DVA + 0x1000 : other, 12);
        /* Validity inactive: the C check scans the decoded caches. */
        CHECK(code_validity_configure(CODE_VALIDITY_NONE, CODE_SNAPSHOT_DEFAULT_BYTES));
        if (virt) native_virtual_setup(v, FETCH_FLAGS);
        else { restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; v->s.pr = 1; }
        if (virt) { v->s.tr[1][1].pte = code | FETCH_FLAGS; ia64_mmu_changed(v, IA64_MMU_TR, 1, 1);
                    bundle(BASE, 0x11, store(3,9,11), nop, validator_branch(VIRTUAL_IP, VIRTUAL_IP)); }
        else bundle(BASE, 0x11, store(3,9,11), nop, validator_branch(BASE, BASE));
        v->s.gr[11] = virt ? NATIVE_DVA + 0x1000 + 0x80 : untracked + 0x80;
        validator_run(v, 3); validator_run(v, 3);
        native_span_store(v, code, virt ? NATIVE_DVA + 0x1000 : code, 13);
    }
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    code_cache_flush();
}
/* cmp.eq p1,p2=r2,r3 (A6). */
static uint64_t cmp_eq(unsigned p1, unsigned p2, unsigned r2, unsigned r3)
{ return (UINT64_C(0xe) << 37) | ((uint64_t)p2 << 27) | ((uint64_t)r3 << 20) | ((uint64_t)r2 << 13) | ((uint64_t)p1 << 6); }
/* Regions continue past a not-taken IP-relative br.cond and br.cloop, and
 * past the predicate moves, mov r=ip, mf and AR reads: one region entry
 * per pass without the exact helper; a taken branch from the middle leaves
 * with the exact IP, slot and retired count at every budget split. */
static void superblock_program(void)
{
    const uint64_t mf = UINT64_C(0x22) << 27, from_pr = (UINT64_C(0x33) << 27) | (UINT64_C(16) << 6);
    const uint64_t to_pr = (UINT64_C(3) << 33) | (UINT64_C(0xff) << 24) | (UINT64_C(16) << 13) | (UINT64_C(0x7f) << 6);
    const uint64_t current_ip = (UINT64_C(0x30) << 27) | (UINT64_C(17) << 6);
    code_cache_flush();
    bundle(BASE, 0, cmp_eq(6, 7, 9, 10), adds_imm(8, 1, 8), from_pr);
    bundle(BASE + 16, 0x11, mf, adds_imm(12, 1, 12), validator_branch(BASE + 16, BASE + 80) | 6);
    bundle(BASE + 32, 0, adds_imm(13, 1, 13), to_pr, current_ip);
    bundle(BASE + 48, 0x11, adds_imm(14, 1, 14), nop, validator_branch(BASE + 48, BASE + 96) | (UINT64_C(5) << 6));
    bundle(BASE + 64, 0x11, adds_imm(18, 1, 18), nop, validator_branch(BASE + 64, BASE));
    bundle(BASE + 80, 0x11, adds_imm(15, 1, 15), nop, validator_branch(BASE + 80, BASE));
    bundle(BASE + 96, 0x11, adds_imm(19, 1, 19), nop, validator_branch(BASE + 96, BASE));
}
static void test_native_superblock(struct vcpu *v, struct vcpu *ref)
{
    superblock_program();
    restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; v->s.gr[9] = 1; v->s.gr[10] = 2;
    const struct ia64_bundle *decoded;
    CHECK(code_get_bundle(v, BASE, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[0].opcode == IA64_OP_CMP_EQ);
    CHECK(code_get_bundle(v, BASE + 32, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[1].opcode == IA64_OP_MOV_GRPR && decoded->insn[2].opcode == IA64_OP_MOV_CURRENT_IP);
    CHECK(code_get_bundle(v, BASE + 16, &decoded).kind == IA64_STEP_COMPLETE);
    CHECK(decoded->insn[0].opcode == IA64_OP_MF && decoded->insn[2].opcode == IA64_OP_BR_COND);
    validator_run(v, 15); validator_run(v, 15);   /* translate, then seal */
    for (unsigned pass = 0; pass < 4; ++pass) {
        uint64_t hits = native_hits(), helpers = native_helper_calls;
        v->s.ip = BASE; v->s.slot = 0; v->s.ar[AR_LC] = 0; v->s.gr[16] = random64();
        native_compare(v, ref, 15);
        CHECK(native_hits() == hits + 1 && native_helper_calls == helpers && v->s.ip == BASE);
    }
    /* mf keeps the helper's full fence: MFENCE (0F AE F0) in the region. */
    unsigned at = boundary_region(BASE, BASE);
    CHECK(at < tier0_arena.capacity);
    const uint8_t *code = tier0_arena.regions[at].entry;
    bool fenced = false;
    for (size_t n = 0; n + 3 <= tier0_arena.regions[at].bytes; ++n)
        fenced |= code[n] == 0x0f && code[n + 1] == 0xae && code[n + 2] == 0xf0;
    CHECK(fenced);
    /* Taken from the middle: br.cond to BASE + 80 (6th instruction), the
     * br.cloop to BASE + 96 (12th) with LC = 1, at every budget split. */
    for (unsigned cloop = 0; cloop < 2; ++cloop)
        for (uint64_t budget = 1; budget <= 16; ++budget) {
            v->s.ip = BASE; v->s.slot = 0; v->s.gr[10] = cloop ? 2 : 1; v->s.ar[AR_LC] = cloop;
            native_compare(v, ref, budget);
        }
    code_cache_flush();
}
/* br.ctop falling through rotates the registers (LC = 0, EC = 1: EC
 * becomes 0), so the instructions after it run with other homes: the
 * region ends there and the next one resolves them (SDM Vol.3 br.ctop). */
static void test_native_superblock_rotation(struct vcpu *v, struct vcpu *ref)
{
    code_cache_flush();
    bundle(BASE, 0x11, adds_imm(33, 1, 32), nop, validator_branch(BASE, BASE) | (UINT64_C(7) << 6));
    bundle(BASE + 16, 1, adds_imm(40, 0, 33), adds_imm(41, 0, 34), nop);
    bundle(BASE + 32, 0x11, nop, nop, validator_branch(BASE + 32, BASE + 32));
    for (unsigned pass = 0; pass < 3; ++pass) {
        restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC;
        v->s.cfm = 24 | (UINT64_C(1) << 14);   /* sof 24, eight rotating registers */
        for (unsigned r = 32; r < 56; ++r) ia64_gr_write(&v->s, r, random64(), false);
        v->s.ar[AR_LC] = 0; v->s.ar[AR_EC] = 1;
        native_compare(v, ref, 6);
    }
    code_cache_flush();
}
/* A request (an emulated device's write requests an exit) after a not-taken
 * branch stops at the instruction after the store, as the interpreter does;
 * an external interrupt raised there is delivered at the region's end, the
 * next Tier0 boundary (asynchronous delivery at restorable
 * boundaries). */
static void test_native_superblock_events(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t iva = BASE + 0x18000;
    for (unsigned interrupt = 0; interrupt < 2; ++interrupt) {
        code_cache_flush();
        for (unsigned offset = 0; offset < 0x8000; offset += 0x100) bundle(iva + offset, 1, nop, nop, nop);
        bundle(BASE, 0x11, cmp_eq(6, 7, 9, 10), adds_imm(8, 1, 8), validator_branch(BASE, BASE + 64) | 6);
        bundle(BASE + 16, 9, store(3,9,11), adds_imm(20, 1, 20), adds_imm(21, 1, 21));
        bundle(BASE + 32, 0x11, nop, nop, validator_branch(BASE + 32, BASE));
        bundle(BASE + 64, 0x11, nop, nop, validator_branch(BASE + 64, BASE));
        restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; v->s.cr[CR_IVA] = iva;
        v->s.gr[9] = 1; v->s.gr[10] = 2; v->s.gr[11] = DATA + 0x100;
        validator_run(v, 9); validator_run(v, 9);   /* translate, then seal */
        v->s.ip = BASE; v->s.slot = 0; v->s.gr[11] = DEVICE + 8; v->s.gr[20] = v->s.gr[21] = 0;
        if (!interrupt) {
            request_exit = true;
            native_compare(v, ref, 9);
            request_exit = false;
            CHECK(v->s.ip == BASE + 16 && v->s.slot == 1 && v->s.gr[20] == 0);
        } else {
            v->s.psr |= IA64_PSR_I; v->s.cr[CR_ITV] = 1u << 16; raise_on_write = 150;
            struct vcpu_exit out = engine_run(v, 9);
            raise_on_write = 0;
            CHECK(out.reason == VCPU_EXIT_BUDGET && v->s.gr[20] == 1 && v->s.gr[21] == 1 && v->s.ip == BASE);
            CHECK(engine_run(v, 1).reason == VCPU_EXIT_BUDGET);
            CHECK(v->s.cr[CR_IIP] == BASE && v->s.ip == iva + 0x3000 && v->s.slot == 1 && ia64_ivr_read(v) == 150);
        }
    }
    code_cache_flush();
}
/* Register stack frames for br.call, br.ret and alloc (SDM Vol.2 §6.1): any
 * frame shape and rotating bases, BOF anywhere in the 96-register ring, a
 * dirty count that fits beside the frame, BSP on or next to an RNAT
 * collection slot, any CPL. */
static void native_frame(struct vcpu *v, unsigned trial)
{
    static const unsigned slots[] = {0, 1, 61, 62, 63};
    randomized(v);
    unsigned sof = trial % 7 == 0 ? 96 : trial % 7 == 1 ? 0 : (unsigned)(random64() % 97);
    unsigned sol = trial % 5 == 0 ? sof : trial % 5 == 1 ? 0 : (unsigned)(random64() % (sof + 1));
    unsigned sor = (unsigned)(random64() % (sof / 8 + 1));
    uint64_t rrb = trial % 3 ? (sor ? random64() % (sor * 8) : 0) | (random64() % 96) << 7 | (random64() % 48) << 14 : 0;
    v->s.cfm = sof | (uint64_t)sol << 7 | (uint64_t)sor << 14 | rrb << 18;
    v->s.rse_bof = trial % 11 == 0 ? 95 : (unsigned)(random64() % IA64_N_STACKED_PHYS);
    v->s.rse_dirty = (unsigned)(random64() % (IA64_N_STACKED_PHYS + 1 - sof));
    unsigned slot = trial % 4 ? slots[random64() % 5] : (unsigned)(random64() % 64);
    v->s.ar[AR_BSP] = (random64() & ~UINT64_C(0x1ff)) | (uint64_t)slot << 3;
    v->s.ar[AR_BSPSTORE] = DATA + 0x6000;
    v->s.ar[AR_EC] = random64(); v->s.ar[AR_PFS] = random64();
    v->s.psr = IA64_PSR_IC | (random64() % 4) << 32;
}
/* SDM Vol.2 §6.5.5: compare PFM outputs with the free partition before
 * subtracting SOL from the dirty count. Equality restores PFM; overflow
 * clears every CFM field while the other return effects still occur. */
static void test_native_return_overlap(struct vcpu *v, struct vcpu *ref)
{
    static const struct { unsigned sof, sol, dirty, bsp_slot; bool overlap; } cases[] = {
        {96, 0, 0, 0, false}, {96, 0, 1, 63, true},
        {96, 4, 4, 1, false}, {96, 4, 5, 0, true}, {32, 32, 96, 62, false},
    };
    for (unsigned at = 0; at < IA64VMM_ARRAY_SIZE(cases); ++at) {
        restart(v, ENGINE_TIER0, BASE);
        unsigned sol = cases[at].sol;
        uint64_t pfm = cases[at].sof | (uint64_t)sol << 7 | UINT64_C(1) << 14 |
            (UINT64_C(7) | UINT64_C(95) << 7 | UINT64_C(47) << 14) << 18;
        uint64_t bsp = DATA + 0x2000 + 8 * cases[at].bsp_slot, store = DATA + 0x1000;
        v->s.psr = IA64_PSR_IC | UINT64_C(3) << 32;
        v->s.cfm = 0; v->s.rse_bof = 1; v->s.rse_dirty = cases[at].dirty;
        v->s.ar[AR_BSP] = bsp; v->s.ar[AR_BSPSTORE] = store;
        v->s.ar[AR_PFS] = pfm | UINT64_C(37) << 52 | UINT64_C(2) << 62;
        v->s.ar[AR_EC] = 11; v->s.br[2] = BASE + 0x10f; v->s.slot = 2;
        Ia64Instruction i = {.opcode = IA64_OP_BR_RET, .valid = true, .unit = IA64_UNIT_B,
            .address = BASE, .slot = 2, .operands.branch.target = 2};
        native_single(v, ref, &i, true);
        for (unsigned n = 0; n < sol; ++n) {
            do { bsp -= 8; } while (((bsp >> 3) & 63) == 63);
        }
        CHECK(v->s.cfm == (cases[at].overlap ? 0 : pfm));
        CHECK(v->s.rse_bof == (1 + IA64_N_STACKED_PHYS - sol) % IA64_N_STACKED_PHYS);
        CHECK(v->s.rse_dirty == cases[at].dirty - sol && v->s.ar[AR_BSP] == bsp);
        CHECK(v->s.ar[AR_EC] == 37 && (v->s.psr >> 32 & 3) == 3);
        CHECK(v->s.ip == BASE + 0x100 && v->s.slot == 0);
        CHECK(v->s.ar[AR_BSPSTORE] == store && !v->s.rse_missing && !v->s.rse_cfle);
    }
}
/* SDM Vol.3 br.call and br.ret, Vol.2 §6.1 and Table 6-2: a call saves CFM,
 * EC and CPL in PFS, preserves the caller's locals (BOF and the dirty count
 * advance, BSP steps over RNAT collection slots), leaves the outputs as the
 * frame and links the next bundle; a return restores CFM and EC from PFS,
 * moves BOF, the dirty count and BSP back by PFS.sol and never raises the
 * privilege. Native unless a return needs mandatory loads (PFS.sol above the
 * dirty count) or lowers the privilege (PFS.ppl > CPL); an indirect call
 * reads its target before it writes the link register. */
static void test_native_calls(struct vcpu *v, struct vcpu *ref)
{
    for (unsigned trial = 0; trial < 4608; ++trial) {
        native_frame(v, trial);
        unsigned form = trial % 3, cpl = (v->s.psr >> 32) & 3;
        Ia64Instruction i = {.valid = true, .unit = IA64_UNIT_B, .address = BASE, .slot = 1 + trial % 2,
                             .qp = trial % 4 == 3 ? (unsigned)(random64() % 64) : 0};
        IA64BranchOperands *op = &i.operands.branch;
        op->link = random64() % 8; op->target = trial % 5 == 0 ? op->link : random64() % 8;
        bool native = true;
        if (form == 0) {
            i.opcode = IA64_OP_BR_CALL;
            op->displacement = ((int64_t)random64() >> (trial % 40)) & ~INT64_C(15);
        } else if (form == 1) i.opcode = IA64_OP_BR_CALL_INDIRECT;
        else {
            i.opcode = IA64_OP_BR_RET;
            unsigned dirty = v->s.rse_dirty;
            unsigned sol = trial % 6 == 2 ? dirty + 1 + (unsigned)(random64() % (127 - dirty)) :
                           (unsigned)(random64() % (dirty + 1));
            unsigned sof = trial % 9 == 5 || sol > 96 ? (unsigned)(random64() % 128) : sol + (unsigned)(random64() % (97 - sol));
            unsigned ppl = trial % 7 < 4 ? cpl : (unsigned)(random64() % 4);
            uint64_t pfm = sof | (uint64_t)sol << 7 | (random64() % 16) << 14 | (random64() & 0xfffff) << 18;
            uint64_t reserved = trial % 8 == 1 ? random64() & ((UINT64_C(0x3fff) << 38) | (UINT64_C(15) << 58)) : 0;
            v->s.ar[AR_PFS] = pfm | (random64() % 64) << 52 | (uint64_t)ppl << 62 | reserved;
            if (trial % 13 == 0) v->s.rse_missing = 1 + (unsigned)(random64() % 8);
            native = sol <= dirty && ppl <= cpl;
        }
        v->s.slot = i.slot;
        native_single(v, ref, &i, native || !ia64_pr_read(&v->s, i.qp));
    }
}
/* SDM Vol.3 alloc: the frame shape from the immediates (sof <= 96, sol <=
 * sof, 8 * sor <= sof, r1 inside the new frame and qp 0, else Illegal
 * Operation), rotating bases kept (a SOR change while one is nonzero is a
 * Reserved Register/Field fault), GR[r1] = PFS with NaT clear, and a spill
 * when the dirty registers and the new frame exceed the 96 physical ones
 * (Vol.2 §6.5). Native unless it faults, spills or finds an incomplete frame. */
static void test_native_alloc(struct vcpu *v, struct vcpu *ref)
{
    for (unsigned trial = 0; trial < 4096; ++trial) {
        native_frame(v, trial);
        unsigned sof = trial % 9 == 0 ? 97 + (unsigned)(random64() % 31) : (unsigned)(random64() % 97);
        unsigned sol = trial % 10 == 0 ? (unsigned)(random64() % 128) : (unsigned)(random64() % (sof + 1));
        unsigned old_sor = (v->s.cfm >> 14) & 15;
        unsigned sor = trial % 6 == 0 ? old_sor : trial % 8 == 0 ? (unsigned)(random64() % 16) :
                       (unsigned)(random64() % (sof / 8 + 1));
        unsigned dest = trial % 17 == 0 ? 0 : trial % 13 == 0 ? (unsigned)(random64() % 32) :
                        32 + (unsigned)(random64() % (sof + 2 > 96 ? 96 : sof + 2));
        unsigned qp = trial % 19 == 0 ? 1 + (unsigned)(random64() % 63) : 0;
        if (trial % 7 == 0) v->s.rse_dirty = (unsigned)(random64() % 97);
        if (trial % 23 == 0) v->s.rse_missing = 1;
        Ia64Instruction i = {.opcode = IA64_OP_ALLOC, .valid = true, .unit = IA64_UNIT_M, .address = BASE,
            .slot = 0, .qp = qp, .operands.system = {.destination = dest, .immediate = sof | sol << 7 | sor << 14}};
        v->s.slot = 0;
        bool legal = !qp && sof <= 96 && sol <= sof && sor * 8 <= sof && dest && (dest < 32 || dest - 32 < sof);
        bool native = legal && !v->s.rse_missing && v->s.rse_dirty + sof <= 96 &&
                      (sor == old_sor || !((v->s.cfm >> 18) & 0xfffff));
        native_single(v, ref, &i, native);
    }
}
/* alloc r1=ar.pfs,sof,sol,sor (M34), br.call.sptk.many b1=target (B3),
 * br.ret.sptk.many b2 (B4) and br.call.dptk.few b1=b2 (B5). */
static uint64_t alloc_insn(unsigned r1, unsigned sof, unsigned sol, unsigned sor)
{
    return (UINT64_C(1) << 37) | (UINT64_C(6) << 33) | ((uint64_t)sor << 27) | ((uint64_t)sol << 20) |
           ((uint64_t)sof << 13) | ((uint64_t)r1 << 6);
}
static uint64_t br_call(gpa_t from, gpa_t to, unsigned b1)
{
    uint64_t d = (to - from) >> 4;
    return (UINT64_C(5) << 37) | (((d >> 20) & 1) << 36) | ((d & 0xfffff) << 13) | (UINT64_C(1) << 12) |
           ((uint64_t)b1 << 6);
}
static uint64_t br_ret(unsigned qp, unsigned b2)
{ return (UINT64_C(0x21) << 27) | ((uint64_t)b2 << 13) | (UINT64_C(1) << 12) | (UINT64_C(4) << 6) | qp; }
static uint64_t br_call_indirect(unsigned qp, unsigned b1, unsigned b2)
{ return (UINT64_C(1) << 37) | (UINT64_C(5) << 32) | ((uint64_t)b2 << 13) | ((uint64_t)b1 << 6) | qp; }
/* Decoded program: a loop allocates a frame and calls a function that
 * allocates its own, writes r39 (outside the caller's one-register output
 * frame, so legal only in the frame of the alloc before it) and returns.
 * Regions continue past alloc and past the qualified call and return: three
 * region entries per pass without the exact helper; every budget split and
 * the taken qualified forms leave the interpreter's state. A shrinking alloc
 * makes a later write outside the new frame fault. With an upper tier, alloc
 * and every branch end their regions. */
static void test_native_calls_program(struct vcpu *v, struct vcpu *ref)
{
    static const struct tier0_upper_ops ops = {.probe = loop_probe, .enter = loop_enter, .hot = loop_hot,
                                             .reclaim = loop_reclaim, .sequence = &loop_sequence, .loop_threshold = 1,
                                             .heads = &loop_sequence, .reclaim_due = &loop_due};
    const gpa_t func = BASE + 0x100, shrink = BASE + 0x200;
    const uint64_t iva = BASE + 0x18000;
    CHECK(alloc_insn(34, 8, 5, 0) == UINT64_C(0x2c00510880) && alloc_insn(40, 29, 20, 2) == UINT64_C(0x2c1143aa00));
    CHECK(br_call(0x20, 0x50, 3) == UINT64_C(0xa0000070c0) && br_call(0x40, 0x20, 0) == UINT64_C(0xb1ffffd000));
    CHECK(br_ret(0, 5) == UINT64_C(0x10800b100) && br_call_indirect(7, 1, 6) == UINT64_C(0x250000c047));
    for (unsigned linked = 0; linked < 2; ++linked) {
        code_cache_flush(); tier0_set_upper(linked ? &ops : NULL);
        for (unsigned offset = 0; offset < 0x8000; offset += 0x100) bundle(iva + offset, 1, nop, nop, nop);
        bundle(BASE, 0, alloc_insn(34, 4, 3, 0), adds_imm(35, 1, 35), adds_imm(33, 1, 33));
        bundle(BASE + 16, 0x13, nop, br_call_indirect(7, 0, 7), br_call(BASE + 16, func, 0));
        bundle(BASE + 32, 0x11, adds_imm(32, 1, 32), nop, validator_branch(BASE + 32, BASE));
        bundle(func, 0, alloc_insn(36, 8, 5, 0), adds_imm(39, 1, 39), adds_imm(37, 1, 37));
        bundle(func + 16, 0x13, nop, br_ret(6, 0), br_ret(0, 0));
        bundle(shrink, 0, alloc_insn(32, 2, 0, 0), adds_imm(36, 1, 36), nop);
        bundle(shrink + 16, 0x11, nop, nop, validator_branch(shrink + 16, shrink));
        restart(v, ENGINE_TIER0, BASE); v->s.psr = IA64_PSR_IC; v->s.cr[CR_IVA] = iva;
        v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = DATA + 0x1e8; v->s.rse_bof = 94;
        v->s.pr &= ~UINT64_C(0xc0); v->s.br[7] = func;
        const struct ia64_bundle *decoded;
        CHECK(code_get_bundle(v, BASE + 16, &decoded).kind == IA64_STEP_COMPLETE);
        CHECK(decoded->insn[1].opcode == IA64_OP_BR_CALL_INDIRECT && decoded->insn[2].opcode == IA64_OP_BR_CALL);
        CHECK(code_get_bundle(v, func, &decoded).kind == IA64_STEP_COMPLETE);
        CHECK(decoded->insn[0].opcode == IA64_OP_ALLOC && decoded->insn[0].operands.system.destination == 36);
        CHECK(code_get_bundle(v, func + 16, &decoded).kind == IA64_STEP_COMPLETE);
        CHECK(decoded->insn[1].opcode == IA64_OP_BR_RET && decoded->insn[2].opcode == IA64_OP_BR_RET);
        validator_run(v, 15); validator_run(v, 15);   /* translate, then seal */
        for (unsigned pass = 0; pass < 4; ++pass) {
            uint64_t hits = native_hits(), helpers = native_helper_calls;
            native_compare(v, ref, 15);
            CHECK(native_hits() == hits + (linked ? 7 : 3) && native_helper_calls == helpers);
            CHECK(v->s.ip == BASE && v->s.slot == 0 && v->s.cfm == (4 | 3 << 7) && !v->s.rse_dirty);
        }
        for (unsigned taken = 0; taken < 3; ++taken)
            for (uint64_t budget = 1; budget <= 16; ++budget) {
                v->s.ip = BASE; v->s.slot = 0;
                v->s.pr = (v->s.pr & ~UINT64_C(0xc0)) | (taken ? UINT64_C(0x20) << taken : 0);
                native_compare(v, ref, budget);
            }
        v->s.ip = shrink; v->s.slot = 0; v->s.cfm = 8;
        native_compare(v, ref, 4);
        CHECK(v->s.cr[CR_IIP] == shrink && ((v->s.cr[CR_ISR] >> 41) & 3) == 1);
    }
    tier0_set_upper(NULL); code_cache_flush();
}
static void test_native(struct vcpu *v, struct vcpu *ref)
{
    test_native_encoder_bounds();
    test_native_encoder();
    struct vcpu *third = vcpu_create(&(struct vcpu_config){.engine = ENGINE_TIER0, .itc_icount = true});
    CHECK(third != NULL);
    test_native_memory_pages(v, ref);
    test_native_memory_foreign(v, ref);
    test_native_store_code_page(v);
    test_native_cloop(v, ref);
    test_native_lc(v, ref);
    test_native_addp4(v, ref);
    test_native_predicates(v, ref);
    test_native_loops(v, ref);
    test_native_interp_states(v, ref);
    test_native_stay_reset(v);
    test_native_virtual_iside(v, ref);
    test_native_virtual_sequence_reuse(v, ref);
    test_native_virtual_warm(v, ref);
    test_native_virtual_changes(v, ref, third);
    test_native_virtual_class(v, ref);
    test_native_virtual_code_store(v);
    test_native_virtual_writers(v, ref);
    test_native_virtual_update(v, ref);
    test_native_virtual_large_page(v, ref);
    test_native_branch_moves(v, ref);
    test_native_release(v, ref);
    test_native_keeps_region(v, ref);
    test_native_decoded(v, ref);
    test_native_iipa(v, ref);
    test_native_speculative(v, ref);
    test_native_deposit(v, ref);
    test_native_shrp(v, ref);
    test_native_advanced(v, ref);
    test_native_lfetch(v, ref);
    test_native_pr_moves(v, ref);
    test_native_item_forms(v, ref);
    test_native_chk_s(v, ref);
    test_native_chk_s_region(v, ref);
    test_native_loop_heads(v);
    test_native_store_span(v);
    test_native_superblock(v, ref);
    test_native_superblock_rotation(v, ref);
    test_native_superblock_events(v, ref);
    test_native_return_overlap(v, ref);
    test_native_calls(v, ref);
    test_native_alloc(v, ref);
    test_native_calls_program(v, ref);
    vcpu_destroy(third);
}
