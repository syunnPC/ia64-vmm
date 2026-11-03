/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "tests/tier1/tail_fixture.h"
#include <pthread.h>
#include <sched.h>

static struct {
    struct t1_vcpu *tv;
    uint64_t link, tail, after, retired;
    uint32_t site;
    bool paused, release, stop, changed, acknowledged, exited;
} tail_writer;

static void tail_write_observer(struct t1_vcpu *tv, const struct vcpu *v,
                                 const struct t1_restore_record *record, void *unused)
{
    (void)v;
    (void)unused;
    if (tv != tail_writer.tv || tv->x.region->link != tail_writer.link)
        return;
    if (record->point_id == tail_writer.site && record->forced &&
        record->outcome == T1_OUTCOME_RETRY) {
        __atomic_store_n(&tail_writer.paused, true, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&tail_writer.release, __ATOMIC_ACQUIRE))
            sched_yield();
    }
    if (record->materialized && record->retired == tail_writer.retired + 9)
        tail_writer.exited = true;
}

static void *tail_write_peer(void *unused)
{
    (void)unused;
    while (!__atomic_load_n(&tail_writer.stop, __ATOMIC_ACQUIRE)) {
        if (!__atomic_load_n(&tail_writer.paused, __ATOMIC_ACQUIRE)) {
            sched_yield();
            continue;
        }
        code_lock();
        const struct t1_region *r = atomic_load_explicit(&tail_writer.tv->running, memory_order_seq_cst);
        UM_CHECK(r && r->valid && r->link == tail_writer.link && r->npages == 2);
        bool acquired = code_vmm_write_begin(tail_writer.tail, 8);
        UM_CHECK(!r->valid);
        struct t1_sync_wait wait;
        unsigned n = t1_sync_i_sample(NULL, &wait), held = UINT32_MAX;
        for (unsigned k = 0; k < n; ++k)
            if (wait.held[k].tv == tail_writer.tv)
                held = k;
        UM_CHECK(held != UINT32_MAX && t1_sync_i_held(&wait, held));
        __atomic_store_n((uint64_t *)(void *)um_ram(tail_writer.tail), tail_writer.after, __ATOMIC_RELEASE);
        code_cpu_write_end(acquired);
        tail_writer.changed = true;
        code_unlock();
        __atomic_store_n(&tail_writer.release, true, __ATOMIC_RELEASE);
        while (t1_sync_i_held(&wait, held))
            sched_yield();
        tail_writer.acknowledged = true;
        return NULL;
    }
    return NULL;
}

static void tail_head_fallthrough(void)
{
    const uint64_t head = UM_CODE + 0x62000, tail = head - 16;
    tail_fixture(head, tail);
    uint64_t low, high, generation;
    CHECK(code_source_read_physical(tail, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
    struct vcpu *v = um_vcpu(head), *ref = um_vcpu(head);
    um_gr(v, 34, 2, false);
    um_gr(v, 35, 1, false);
    um_pr(v, 8, false);
    um_pr(v, 9, false);
    um_pr(v, 10, true);
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "physical tail fallthrough head");
    CHECK(id);
    const struct t1_region *r = t1_region_get(id);
    CHECK(r && r->ninsns == 9 && r->npages == 2 && !r->loop);
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    struct umt_run run;
    for (unsigned budget = 1; budget <= 30; ++budget) {
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, budget, id, NULL, &run, false,
                 "physical tail: external fallthrough to original head");
        CHECK(v->retired == snap.v.retired + budget && run.exit.reason == VCPU_EXIT_BUDGET);
    }
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
}

static void tail_runtime_case(unsigned virt)
{
    const uint64_t physical_head = UM_CODE + 0x6e000, physical_tail = physical_head - 0x1100;
    const uint64_t head = physical_head + (virt ? (UINT64_C(1) << 61) + 0x2000000 : 0);
    const uint64_t tail = head - 0x1100;
    tail_fixture(physical_head, physical_tail);
    uint64_t low, high, generation;
    CHECK(code_source_read_physical(physical_tail, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
    struct vcpu *v = um_vcpu(head), *ref = um_vcpu(head);
    if (virt == 2)
        tail_virtual_pages(v, head, physical_head, tail, physical_tail);
    else if (virt) {
        um_virtual_code(v);
        v->s.tr[0][1].va += 0x2000000;
        ia64_mmu_changed(v, IA64_MMU_TR, 0, 1);
    }
    v->s.cfm = um_cfm(8, 8, 8, 5, 7);
    uint64_t value = UINT64_C(0x9876543210012345);
    memcpy(um_ram(UM_DATA + 0x500), &value, 8);
    um_gr(v, 33, UM_DATA + 0x500, false);
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "tail");
    CHECK(id);
    const struct t1_region *r = t1_region_get(id);
    CHECK(r && r->ninsns == 9 && r->npages == 2);
    for (unsigned p = 0; p < 2; ++p) {
        CHECK(r->page[p].pa == ((p ? physical_tail : physical_head) & ~UINT64_C(4095)));
        unsigned bit = (unsigned)((p ? tail : head) & 4095) / 16;
        for (unsigned word = 0; word < 4; ++word) {
            uint64_t expected = word == bit / 64 ? UINT64_C(1) << (bit % 64) : 0;
            if (!p && word == (bit + 1) / 64)
                expected |= UINT64_C(1) << ((bit + 1) % 64);
            CHECK(r->dep[p].mask[word] == expected);
        }
    }
    uint32_t site = UINT32_MAX;
    for (unsigned k = 0; k < t1_point_count(id); ++k) {
        struct t1_point_info point;
        CHECK(t1_point_get(id, k, &point));
        if (point.kind == T1_PT_SITE && point.ip == head)
            site = k;
    }
    CHECK(site != UINT32_MAX);
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    struct umt_run run;
    unsigned cases = 0;
    for (unsigned bof = 0; bof < 2; ++bof)
        for (unsigned path = 0; path < 8; ++path)
            for (unsigned nat = 0; nat < 4; ++nat) {
                snap.v.s.rse_bof = bof ? 95 : 0;
                um_gr(&snap.v, 33, UM_DATA + 0x500, false);
                um_gr(&snap.v, 34, path & 1 ? 0 : UINT64_MAX, nat & 1);
                um_gr(&snap.v, 35, 1, nat & 2);
                um_gr(&snap.v, 36, UINT64_MAX - 1, false);
                um_pr(&snap.v, 8, true);
                um_pr(&snap.v, 9, path & 4);
                um_pr(&snap.v, 10, path & 2);
                um_pr(&snap.v, 16, true);
                um_pr(&snap.v, 17, true);
                for (unsigned budget = 1; budget <= 20; ++budget, ++cases) {
                    umt_case(v, ref, &snap, T1_COMPILE_VERIFY, budget, id, NULL, &run, false,
                             "tail: predicates, NaT, rotating homes, budget");
                    CHECK(run.exit.reason == VCPU_EXIT_BUDGET && v->retired == snap.v.retired + budget);
                }
            }
    um_pr(&snap.v, 9, false);
    um_pr(&snap.v, 10, true);
    um_gr(&snap.v, 34, 0, false);
    um_gr(&snap.v, 35, 1, false);
    const enum t1_force_action actions[] = {
        T1_FORCE_TRAP_EXIT, T1_FORCE_MISS_EXIT, T1_FORCE_TRAP_RETRY};
    for (unsigned k = 0; k < 3; ++k, ++cases) {
        struct umt_arm arm = {.point = site, .occurrence = 1, .action = actions[k]};
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 25, id, &arm, &run, false,
                 "tail: source fault and retry");
        CHECK(run.forced == 1);
    }
    for (unsigned pred = 0; pred < 2; ++pred)
        for (unsigned fault = 0; fault < 2; ++fault, ++cases) {
            um_pr(&snap.v, 8, pred);
            um_gr(&snap.v, 33, fault ? UM_RAM_BASE + UM_RAM_SIZE + 0x1000 : UM_DATA + 0x500, !fault);
            umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 25, id, NULL, &run, false,
                     "tail: source access fault or NaT");
        }
    um_pr(&snap.v, 8, true);
    um_gr(&snap.v, 33, UM_DATA + 0x500, false);
    struct vcpu untimed = snap.v;
    for (unsigned deadline = 1; deadline <= 13; ++deadline, ++cases) {
        snap.v = untimed;
        snap.v.s.cr[CR_ITV] = 0xef;
        snap.v.s.cr[CR_ITM] = ia64_itc_read(&snap.v) + deadline;
        ia64_timer_program(&snap.v);
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 25, id, NULL, &run, false,
                 "tail: exact timer boundary");
        CHECK(v->timer_fired);
    }
    snap.v = untimed;
    um_snapshot_restore(&snap, v);
    struct t1_vcpu *tv = umt_tv(v);
    t1_vcpu_state_restored(tv);
    t1_test_set_entry_hook(NULL, NULL);
    memset(&tail_writer, 0, sizeof(tail_writer));
    tail_writer.tv = tv;
    tail_writer.link = r->link;
    tail_writer.tail = physical_tail;
    tail_writer.retired = v->retired;
    tail_writer.site = site;
    memcpy(&low, um_ram(physical_tail), 8);
    tail_writer.after = low ^ (UINT64_C(1) << 11);
    Ia64Instruction nop = ia64_decode_insn(IA64_UNIT_M,
        ia64_bundle_slot(tail_writer.after, high, 0), tail, 0);
    CHECK(nop.valid && nop.opcode == IA64_OP_NOP && nop.operands.system.immediate == 1);
    CHECK(t1_test_force(tv, id, site, 1, T1_FORCE_TRAP_RETRY, NULL, NULL));
    t1_test_set_restore_observer(tail_write_observer, NULL);
    code_register_vcpu(v);
    pthread_t peer;
    CHECK(!pthread_create(&peer, NULL, tail_write_peer, NULL));
    struct vcpu_exit out = t1_run(tv, 25);
    __atomic_store_n(&tail_writer.stop, true, __ATOMIC_RELEASE);
    CHECK(!pthread_join(peer, NULL));
    CHECK(tail_writer.changed && tail_writer.acknowledged && tail_writer.exited);
    CHECK(!t1_region_from_link(tail_writer.link));
    code_unregister_vcpu(v);
    t1_test_set_restore_observer(NULL, NULL);
    memcpy(snap.ram + physical_tail - UM_RAM_BASE, &tail_writer.after, 8);
    struct um_run observed = {0};
    CHECK(!um_compare(&snap, ref, &observed, v, &out, 25, "tail: live second-page write"));
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
    printf("unit_runtime: %s tail: %u whole-state cases, exact source masks and live second-page write with positive sync wait\n",
           virt == 2 ? "separate virtual" : virt ? "virtual" : "physical", cases);
}

static void tail_virtual_faults(void)
{
    const uint64_t physical_head = UM_CODE + 0x6e000, physical_tail = physical_head - 0x1100;
    const uint64_t head = (UINT64_C(2) << 61) + 0x1000, tail = head - 0x1100;
    tail_fixture(physical_head, physical_tail);
    /* A remap changes the discriminator outcome, so stale native code
     * cannot accidentally agree with the interpreter. */
    um_load(UM_CODE + 0x78000, um_ram(physical_tail & ~UINT64_C(4095)), 4096);
    const struct ienc_bundle remapped = {IENC_STOP(IENC_MIB), {ienc_nop_m(),
        ienc_cmp(10, IENC_EQ, false, false, 16, 17, 34, 35), ienc_br_cond(16, -0x80)}};
    um_emit(UM_CODE + 0x78000 + (physical_tail & 4095), &remapped, 1);
    uint64_t low, high, generation;
    CHECK(code_source_read_physical(physical_tail, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
    struct vcpu *v = um_vcpu(head), *ref = um_vcpu(head);
    tail_virtual_pages(v, head, physical_head, tail, physical_tail);
    um_gr(v, 34, 0, false);
    um_gr(v, 35, 1, false);
    um_pr(v, 8, false);
    um_pr(v, 10, true);
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "resident virtual tail");
    CHECK(id && t1_region_get(id)->ninsns == 9 && t1_region_get(id)->npages == 2);
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    static struct vcpu original;
    original = snap.v;
    struct umt_run run;
    unsigned cases = 0;
    for (unsigned change = 0; change < 11; ++change)
        for (unsigned bypass = 0; bypass < 2; ++bypass) {
            snap.v = original;
            tail_virtual_change(&snap.v, change, tail);
            um_pr(&snap.v, 9, bypass);
            for (unsigned budget = 1; budget <= 16; ++budget, ++cases) {
                umt_case(v, ref, &snap, T1_COMPILE_VERIFY, budget, id, NULL, &run, false,
                         "resident tail: changed target translation, exact fault and budget");
                CHECK(run.exit.reason == VCPU_EXIT_BUDGET && v->retired == snap.v.retired + budget);
                if (change && change < 10 && !bypass && budget > 6)
                    CHECK(v->s.cr[CR_IIP] == tail);
            }
        }
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
    printf("unit_runtime: separate virtual tail: %u whole-state translation/fault/budget cases\n", cases);
}

static void tail_runtime_test(void)
{
    tail_runtime_case(false);
    tail_runtime_case(true);
    tail_runtime_case(2);
    tail_virtual_faults();
    tail_head_fallthrough();
}
