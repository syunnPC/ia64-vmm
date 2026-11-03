/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bundle coherence: the analogues of tests/tier1/coherence_tests.c for the
 * engines without Tier1 -- the interpreter (decoded-cache dependents),
 * Tier0 and lockstep -- in compare-always and epoch-compare. Two code
 * pages P and Q, a loop X (P) -> Y (Q) -> X, one region per page; one
 * bundle on each page that no region holds (never executed, so not read).
 *   boundary  DMA (with the storage epoch) into X's or Y's bundle, or the
 *             uncovered bundle of either page, while the vCPU executes the
 *             other page: the region holding it is retired, the other is
 *             not, and the new code runs; an uncovered change retires
 *             nothing (no region, no decoded bundle).
 *   race      DMA from the test hook at Tier0's CODE_TEST_PUBLISH (between
 *             the emission and the second publication check of X's first
 *             translation): into X's bundle (the translation is discarded
 *             and redone with the new code) or into the uncovered bundle
 *             (no mismatch, no retry).
 *   reuse     all of page P rewritten (X with another add): X is
 *             retired and runs the new code, Y is not. */
#define COH_P (BASE + 0x1000)
#define COH_Q (BASE + 0x2000)
static void coh_program(void)
{
    bundle(COH_P, 1, nop, add(8,8,9), nop);
    bundle(COH_P + 16, 0x11, nop, nop, validator_branch(COH_P + 16, COH_Q));
    bundle(COH_P + 0x800, 1, nop, add(12,12,9), nop);
    bundle(COH_Q, 1, nop, add(11,11,9), nop);
    bundle(COH_Q + 16, 0x11, nop, nop, validator_branch(COH_Q + 16, COH_P));
    bundle(COH_Q + 0x800, 1, nop, add(12,12,9), nop);
}
static struct tier0_region *coh_region(uint64_t ip)
{
    for (unsigned i = 0; tier0_arena.regions && i < tier0_arena.capacity; ++i)
        if (tier0_arena.regions[i].valid && tier0_arena.regions[i].key.ip == ip) return &tier0_arena.regions[i];
    return NULL;
}
/* Serial of the live region at ip, 0 if none. */
static uint64_t coh_serial(uint64_t ip)
{
    const struct tier0_region *r = coh_region(ip);
    return r ? r->serial : 0;
}
/* The loop's state after two passes from P (24 instructions): X and Y
 * translated (Tier0, lockstep) or decoded (interpreter). */
static void coh_warm(struct vcpu *v, enum engine_kind engine, enum code_validity_mode mode)
{
    CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
    coh_program();
    restart(v, engine, COH_P); v->s.gr[9] = 1; v->s.gr[10] = 2;
    validator_run(v, 24);
    CHECK(v->s.ip == COH_P && v->s.gr[8] == 2 && v->s.gr[11] == 2);
    if (engine != ENGINE_INTERP) CHECK(coh_serial(COH_P) && coh_serial(COH_Q));
}
static gpa_t coh_race_target;
static unsigned coh_race_calls;
static bool coh_race_epoch;
static void coh_race_hook(gpa_t page, enum code_test_stage stage)
{
    if (page != COH_P || stage != CODE_TEST_PUBLISH || coh_race_calls++) return;
    unsigned dest = coh_race_target == COH_P ? 8 : 12;
    dma_bundle(coh_race_target, 1, nop, add(dest,dest,10), nop);
    if (coh_race_epoch) code_epoch_bump(CODE_DEVICE_STORAGE);
}
static void test_coh_tier0(struct vcpu *v)
{
    const gpa_t targets[] = {COH_P, COH_P + 0x800, COH_Q, COH_Q + 0x800};
    unsigned cases = 0;
    for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS; mode <= CODE_VALIDITY_EPOCH_COMPARE; ++mode)
        for (enum engine_kind engine = ENGINE_INTERP; engine <= ENGINE_LOCKSTEP; ++engine) {
            bool regions = engine != ENGINE_INTERP;
            for (unsigned t = 0; t < 4; ++t) {
                gpa_t target = targets[t];
                bool on_p = target < COH_Q, covered = !(target & 0xfff);
                coh_warm(v, engine, mode);
                /* Into Y (the other page, 3 instructions) for a change on
                 * P; past X0 for a change on Q. */
                validator_run(v, on_p ? 9 : 3);
                CHECK((v->s.ip & ~UINT64_C(4095)) == (on_p ? COH_Q : COH_P));
                uint64_t xs = coh_serial(COH_P), ys = coh_serial(COH_Q), r8 = v->s.gr[8], r11 = v->s.gr[11];
                struct tier0_stats t0, t1; struct code_validity_stats c0, c1; struct code_cache_stats d0, d1;
                tier0_get_stats(&t0); code_validity_get_stats(&c0); code_cache_get_stats(&d0);
                unsigned dest = target == COH_P ? 8 : target == COH_Q ? 11 : 12;
                dma_bundle(target, 1, nop, add(dest,dest,10), nop);
                if (mode == CODE_VALIDITY_EPOCH_COMPARE) code_epoch_bump(CODE_DEVICE_STORAGE);
                validator_run(v, 24);
                tier0_get_stats(&t1); code_validity_get_stats(&c1); code_cache_get_stats(&d1);
                /* Two more executions of X0 and of Y0 each. */
                CHECK(v->s.gr[8] - r8 == (target == COH_P ? 4u : 2u));
                CHECK(v->s.gr[11] - r11 == (target == COH_Q ? 4u : 2u));
                CHECK(v->s.gr[12] == 0);
                CHECK(c1.invalidations - c0.invalidations == covered);
                CHECK(d1.decodes - d0.decodes == covered);
                if (regions) {
                    CHECK((coh_serial(COH_P) == xs) == (target != COH_P) && coh_serial(COH_P));
                    CHECK((coh_serial(COH_Q) == ys) == (target != COH_Q) && coh_serial(COH_Q));
                    CHECK(t1.invalidations - t0.invalidations == covered);
                }
                ++cases;
            }
            /* Page reuse: page P rewritten by one DMA (another X: r10,
             * and nothing else on the page). */
            coh_warm(v, engine, mode);
            validator_run(v, 9);
            uint64_t ys = coh_serial(COH_Q), r8 = v->s.gr[8];
            memset(ram + (COH_P - BASE), 0, 4096);
            dma_bundle(COH_P, 1, nop, add(8,8,10), nop);
            dma_bundle(COH_P + 16, 0x11, nop, nop, validator_branch(COH_P + 16, COH_Q));
            if (mode == CODE_VALIDITY_EPOCH_COMPARE) code_epoch_bump(CODE_DEVICE_STORAGE);
            validator_run(v, 24);
            CHECK(v->s.gr[8] - r8 == 4);
            if (regions) CHECK(coh_serial(COH_Q) == ys && coh_serial(COH_P));
            ++cases;
            /* Race at X's first translation (Tier0, lockstep). */
            for (unsigned k = 0; regions && k < 2; ++k) {
                CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
                coh_program();
                restart(v, engine, COH_P); v->s.gr[9] = 1; v->s.gr[10] = 2;
                coh_race_target = k ? COH_P + 0x800 : COH_P; coh_race_calls = 0;
                coh_race_epoch = mode == CODE_VALIDITY_EPOCH_COMPARE;
                struct code_validity_stats c0, c1; struct tier0_stats t0, t1;
                code_validity_get_stats(&c0); tier0_get_stats(&t0);
                code_validity_set_test_hook(coh_race_hook);
                validator_run(v, 24);
                code_validity_set_test_hook(NULL);
                code_validity_get_stats(&c1); tier0_get_stats(&t1);
                CHECK(coh_race_calls >= 1);
                /* Covered: the discarded translation's mismatch, X runs
                 * r10 from the start; uncovered: no mismatch at all. */
                CHECK(c1.mismatches - c0.mismatches == !k);
                CHECK(v->s.gr[8] == (k ? 2u : 4u) && v->s.gr[11] == 2 && v->s.gr[12] == 0);
                ++cases;
            }
        }
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    LOGI("test-tier0: COH analogues: %u cases (interp, Tier0, lockstep; compare-always, epoch-compare)", cases);
}
