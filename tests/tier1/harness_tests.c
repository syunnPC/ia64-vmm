/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Self-tests of the harness pieces the restoration tests rely on
 * driven by the interpreter and Tier0:
 *
 *   store log    GVA pairing at the stepped boundary and in the callback;
 *                two GVAs of one PA (gather-alias data alias), post-increment
 *                stores; logs of stepped reference, interp_run and Tier0
 *                helpers equal
 *   schedule     DMA and UNMAP applied at the same retired count on both
 *                sides compare equal, the guest really refaults after
 *                UNMAP, and a DMA missing from the reference is detected
 *   observer     in-place captures at many boundaries of an gather-virt run,
 *                strict and after-translation phases, a DMA resolved at a
 *                capture: all equal to the reference stopped there; a corrupted
 *                old value of r18 in one capture fails
 *                in place while the final comparison passes (the harness
 *                half of wrong-old-value); a page difference is named
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/engines.h"
#include "tests/tier1/observe.h"
#include "tests/tier1/programs.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/results.h"
#include "tests/tier1/storelog.h"
#include "engine/interp/interp.h"
#include "ia64/mmu.h"
#include "ia64vmm/guest_mem.h"

#define BIG_BUDGET UINT64_C(50000000)
#define CHECK T1H_CHECK

static struct vcpu *ref, *v;
static struct t1h_logs ref_logs, logs;
static struct snapshot snap;
static struct run_result reference, actual;

static void open_program(const char *name, struct guest_params q)
{
    const struct guest_program *p = guest_program(name);
    struct vcpu_config cfg = guest_vcpu_config(p);
    ref = vcpu_create(&cfg);
    v = vcpu_create(&cfg);
    CHECK(ref && v);
    t1h_bind(ref, &ref_logs);
    t1h_bind(v, &logs);
    guest_setup(p, &q, ref);
    snapshot_take(&snap, ref);
    ref_logs.stores_on = logs.stores_on = true;
}

static void close_program(void)
{
    t1h_unbind(ref);
    t1h_unbind(v);
    vcpu_destroy(ref);
    vcpu_destroy(v);
}

static struct vcpu_exit run_reference(struct schedule *sched)
{
    if (sched)
        schedule_rearm(sched);
    snapshot_restore(&snap, ref);
    struct vcpu_exit e = ref_run(ref, BIG_BUDGET, sched);
    CHECK(e.reason == VCPU_EXIT_DEBUG && ref_logs.last_break_imm == T1H_BREAK_END);
    result_take(&reference, &e, ref);
    return e;
}

/* ---- store log --------------------------------------------------------- */

static void store_log_tests(void)
{
    /* bench = true restarts the output pointer every pass, so pass 2 stores
     * to the same PAs through VRN 5 that pass 1 used through VRN 3. */
    struct guest_params q = guest_sweep_params(guest_program("gather_alias"));
    q.bench = true;
    open_program("gather_alias", q);
    run_reference(NULL);
    CHECK(t1h_logs_ok(&ref_logs) && ref_logs.store_n >= 4 &&
          ref_logs.paired_stepped == ref_logs.store_n);
    unsigned aliased = 0;
    for (size_t i = 0; i < ref_logs.store_n; ++i)
        for (size_t j = i + 1; j < ref_logs.store_n; ++j) {
            uint64_t a = ref_logs.stores[i].gva, b = ref_logs.stores[j].gva;
            if (a != b && (a & ia64_low_mask(61)) == (b & ia64_low_mask(61)))
                ++aliased;
        }
    CHECK(aliased > 0);
    /* Post-increment: consecutive stores of one pass are 8 bytes apart. */
    CHECK(ref_logs.stores[1].gva == ref_logs.stores[0].gva + 8);
    for (unsigned k = 0; k < t1h_engine_count(); ++k) {
        struct t1h_engine_vcpu ev;
        t1h_engine_bind(&ev, t1h_engine_at(k), v);
        snapshot_restore(&snap, v);
        t1h_engine_restored(&ev);
        struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
        result_take(&actual, &e, v);
        /* Not stepped: every store is derived in the proxy callback.
         * Tier1 steps its lower tier (ref_lower) and logs its native
         * stores itself. `tier0+tier1`:
         * Tier0's stores are paired in the callback (tier0_helper
         * sets ip/slot before ia64_execute), Tier1's are native records,
         * and at least one store is Tier1's. */
        const char *name = t1h_engine_at(k)->name;
        if (!strcmp(name, "tier1"))
            CHECK(logs.paired_callback + logs.paired_stepped <= logs.store_n);
        else if (!strcmp(name, "tier0+tier1"))
            CHECK(!logs.paired_stepped && logs.paired_callback < logs.store_n);
        else
            CHECK(logs.paired_callback == logs.store_n && !logs.paired_stepped);
        CHECK(result_compare(&reference, &actual, true, "store log") == 0);
        t1h_engine_release(&ev);
    }
    /* A wrong GVA or a missing record is a difference. */
    actual.logs.stores[2].gva ^= UINT64_C(3) << 61;
    CHECK(result_compare(&reference, &actual, true, T1H_EXPECTED_DIFFERENCE) == 1);
    actual.logs.stores[2].gva ^= UINT64_C(3) << 61;
    --actual.logs.store_n;
    CHECK(result_compare(&reference, &actual, true, T1H_EXPECTED_DIFFERENCE) == 1);
    printf("harness: store log: %zu stores, %u GVA pairs aliasing one PA, stepped and "
           "callback pairing equal for %u engine(s)\n",
           ref_logs.store_n, aliased, t1h_engine_count());
    /* Keep one output word in range: every other completed store must
     * still be counted, including stores emitted by compiled regions. */
    size_t total_stores = reference.logs.store_n;
    gpa_t first_word = reference.logs.stores[0].gva & ia64_low_mask(61);
    const struct storelog_range one_word = {first_word, first_word + 8};
    storelog_set_ranges(&one_word, 1);
    run_reference(NULL);
    CHECK(reference.logs.store_n > 0 && reference.logs.stores_outside > 0);
    CHECK(reference.logs.store_n + reference.logs.stores_outside == total_stores);
    /* Reads and non-store helpers outside the log ranges keep ordinary
     * RAM classification (SDM Vol.3 speculative-load semantics). */
    CHECK(ref->memory_hook(ref, T1H_TABLE, 8)->kind == GM_RAM);
    for (unsigned k = 0; k < t1h_engine_count(); ++k) {
        struct t1h_engine_vcpu ev;
        t1h_engine_bind(&ev, t1h_engine_at(k), v);
        snapshot_restore(&snap, v);
        t1h_engine_restored(&ev);
        struct vcpu_exit e = t1h_engine_run(&ev, BIG_BUDGET);
        result_take(&actual, &e, v);
        CHECK(result_compare(&reference, &actual, true, "ranged store counts") == 0);
        ++actual.logs.stores_outside;
        CHECK(result_compare(&reference, &actual, true, T1H_EXPECTED_DIFFERENCE) == 1);
        --actual.logs.stores_outside;
        CHECK(result_compare(&reference, &actual, false, "stores disabled") == 0);
        t1h_engine_release(&ev);
    }
    storelog_default_ranges();
    close_program();
}

/* ---- schedule ---------------------------------------------------------- */

static void schedule_tests(void)
{
    open_program("gather_virt", guest_sweep_params(guest_program("gather_virt")));
    run_reference(NULL);
    uint64_t total = ref->retired;
    uint64_t refills = t1h_bank0(ref, 20) + t1h_bank0(ref, 29);
    static struct schedule sched;
    schedule_clear(&sched);
    for (unsigned page = 0; page < 40; ++page)
        schedule_add(&sched, (struct injection){.at = total / 2,
                                                .kind = INJ_UNMAP,
                                                .address = (UINT64_C(2) << 61) |
                                                           (T1H_TABLE_VA + page * UINT64_C(4096))});
    schedule_add(&sched, (struct injection){.at = total / 3,
                                            .kind = INJ_DMA,
                                            .address = T1H_STATUS + 8,
                                            .value = 0xd1a,
                                            .width = 8});
    run_reference(&sched);
    CHECK(schedule_complete(&sched));
    /* Pages touched again after the purge were refilled by the guest (the
     * Data TLB handler, or Page Not Present after the walker found p = 0). */
    CHECK(t1h_bank0(ref, 20) + t1h_bank0(ref, 29) > refills);
    unsigned comparisons = 0;
    for (unsigned k = 0; k < t1h_engine_count(); ++k) {
        struct t1h_engine_vcpu ev;
        t1h_engine_bind(&ev, t1h_engine_at(k), v);
        schedule_rearm(&sched);
        snapshot_restore(&snap, v);
        t1h_engine_restored(&ev);
        struct vcpu_exit e = t1h_engine_run_sched(&ev, BIG_BUDGET, &sched);
        result_take(&actual, &e, v);
        CHECK(result_compare(&reference, &actual, true, "schedule") == 0);
        ++comparisons;
        t1h_engine_release(&ev);
    }
    /* The reference without the DMA differs from the last run with it. */
    sched.n = 40;
    run_reference(&sched);
    CHECK(result_compare(&reference, &actual, true, T1H_EXPECTED_DIFFERENCE) >= 1);
    /* Injections applied by the run itself (as from an entry hook) and
     * replayed by the reference at the recorded boundary. */
    static struct schedule recorded;
    schedule_clear(&recorded);
    snapshot_restore(&snap, v);
    struct vcpu_exit e = interp_run(v, total / 2);
    CHECK(e.reason == VCPU_EXIT_BUDGET);
    for (unsigned page = 0; page < 40; ++page)
        schedule_apply_now(&recorded, v,
                           (struct injection){.kind = INJ_UNMAP,
                                              .address = (UINT64_C(2) << 61) |
                                                         (T1H_TABLE_VA + page * UINT64_C(4096))});
    e = interp_run(v, BIG_BUDGET);
    result_take(&actual, &e, v);
    run_reference(&recorded);
    CHECK(result_compare(&reference, &actual, true, "schedule (recorded)") == 0);
    ++comparisons;
    printf("harness: schedule: UNMAP x40 and DMA at fixed boundaries, and injections recorded "
           "by the run: %u run(s) equal; a DMA missing on one side is detected\n",
           comparisons);
    close_program();
}

/* ---- observer ---------------------------------------------------------- */

static void observer_tests(void)
{
    open_program("gather_virt", guest_sweep_params(guest_program("gather_virt")));
    run_reference(NULL);
    uint64_t total = ref->retired;
    static struct schedule sched;
    static struct observer o;
    schedule_clear(&sched);
    observe_init(&o, 256, &sched);

    /* The run under test: the interpreter, stopped at every 5th boundary,
     * where a restore observer would capture. At a memory access
     * the capture is taken after the access check (the natural-fault
     * phase); one DMA is applied first, as a before-resolve callback does. */
    snapshot_restore(&snap, v);
    unsigned translated = 0;
    bool dma_done = false;
    for (uint64_t at = 0; at < total; at += 5) {
        struct vcpu_exit e = interp_run(v, at - v->retired);
        CHECK(e.reason == VCPU_EXIT_BUDGET && v->retired == at);
        if (!dma_done && at >= total / 2) {
            /* What a before-resolve callback does: boundary still unknown. */
            struct schedule_hook hook = {
                &sched, {.kind = INJ_DMA, .address = T1H_STATUS + 16, .value = 0xd3a, .width = 8}};
            schedule_before_resolve(v, &hook);
            CHECK(sched.e[sched.n - 1].at == T1H_AT_PENDING);
            dma_done = true;
        }
        Ia64Instruction insn;
        bool access = storelog_decode(v, v->s.ip, v->s.slot, &insn) &&
                      (ia64_opcode_is_load(insn.opcode) || ia64_opcode_is_store(insn.opcode)) &&
                      ia64_pr_read(&v->s, insn.qp);
        if (access) {
            uint64_t gva = ia64_gr_read(&v->s, insn.operands.memory.base);
            bool write = ia64_opcode_is_store(insn.opcode);
            struct ia64_translation t;
            (void)ia64_translate(v, gva, IA64_REF_DATA,
                                 write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ, (v->s.psr >> 32) & 3,
                                 false, &t);
            observe_capture(&o, v, true, gva, write, 0, 0, at);
            ++translated;
        } else {
            observe_capture(&o, v, false, 0, false, 0, 0, at);
        }
    }
    struct vcpu_exit e = interp_run(v, BIG_BUDGET);
    result_take(&actual, &e, v);
    CHECK(dma_done && schedule_complete(&sched) && translated > 0);

    /* Captures equal the reference at the same boundaries, and the end. */
    CHECK(observe_verify(&o, &snap, ref, &sched, "observer") == 0);
    struct vcpu_exit r = ref_run(ref, BIG_BUDGET, &sched);
    result_take(&reference, &r, ref);
    CHECK(result_compare(&reference, &actual, true, "observer final") == 0);

    /* wrong-old-value, harness half: a wrong old r18 at one capture (before S2
     * overwrites it) is caught in place; the final state is unaffected. */
    unsigned victim = o.n / 2;
    o.c[victim].v.s.gr[ia64_gr_index(&o.c[victim].v.s, 18)] ^= 0x10;
    CHECK(observe_verify(&o, &snap, ref, &sched, T1H_EXPECTED_DIFFERENCE) == 1);
    r = ref_run(ref, BIG_BUDGET, &sched);
    result_take(&reference, &r, ref);
    CHECK(result_compare(&reference, &actual, true, "observer final") == 0);
    o.c[victim].v.s.gr[ia64_gr_index(&o.c[victim].v.s, 18)] ^= 0x10;
    /* A page difference is a difference of that capture. */
    o.c[victim].hash[(T1H_OUT - T1H_RAM_BASE) >> 12] ^= 1;
    CHECK(observe_verify(&o, &snap, ref, &sched, T1H_EXPECTED_DIFFERENCE) == 1);
    o.c[victim].hash[(T1H_OUT - T1H_RAM_BASE) >> 12] ^= 1;
    /* Without the DMA in the reference's schedule, captures after it differ. */
    sched.n = 0;
    CHECK(observe_verify(&o, &snap, ref, &sched, T1H_EXPECTED_DIFFERENCE) > 0);
    printf("harness: observer: %u in-place captures (%u after translation, one DMA) equal "
           "to the reference; a corrupted capture fails in place while the end state "
           "is equal\n",
           o.n, translated);
    observe_free(&o);
    close_program();
}

void harness_tests(void);
void harness_tests(void)
{
    snapshot_init(&snap);
    result_init(&reference);
    result_init(&actual);
    store_log_tests();
    schedule_tests();
    observer_tests();
    result_free(&reference);
    result_free(&actual);
    snapshot_free(&snap);
}
