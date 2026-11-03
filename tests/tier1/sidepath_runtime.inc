/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "tests/tier1/sidepath_fixture.h"
#include "ia64/interrupt.h"
#include <pthread.h>
#include <sched.h>

static struct {
    struct t1_vcpu *tv;
    uint64_t link, pa, after, retired;
    uint32_t site, checkpoint;
    bool ready, paused, release, stop, changed, acknowledged, checkpoint_seen;
} sidepath_writer;

static void sidepath_write_observer(struct t1_vcpu *tv, const struct vcpu *v,
                                    const struct t1_restore_record *record, void *unused)
{
    (void)v;
    (void)unused;
    /* Point IDs are local to a region. The run may enter a replacement
     * after the source write retires the generation being observed. */
    if (tv != sidepath_writer.tv || !tv->x.region ||
        tv->x.region->link != sidepath_writer.link)
        return;
    if (record->point_id == sidepath_writer.site && record->forced &&
        record->outcome == T1_OUTCOME_RETRY) {
        __atomic_store_n(&sidepath_writer.paused, true, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&sidepath_writer.release, __ATOMIC_ACQUIRE))
            sched_yield();
    }
    if (record->point_id == sidepath_writer.checkpoint && record->materialized) {
        UM_CHECK(record->retired == sidepath_writer.retired + 12);
        sidepath_writer.checkpoint_seen = true;
    }
}

static void *sidepath_write_peer(void *unused)
{
    (void)unused;
    __atomic_store_n(&sidepath_writer.ready, true, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&sidepath_writer.stop, __ATOMIC_ACQUIRE)) {
        if (!__atomic_load_n(&sidepath_writer.paused, __ATOMIC_ACQUIRE)) {
            sched_yield();
            continue;
        }
        code_lock();
        const struct t1_region *r = atomic_load_explicit(&sidepath_writer.tv->running,
                                                        memory_order_seq_cst);
        if (!r || !r->valid || r->link != sidepath_writer.link) {
            code_unlock();
            sched_yield();
            continue;
        }
        bool acquired = code_vmm_write_begin(sidepath_writer.pa, 8);
        UM_CHECK(!r->valid);
        struct t1_sync_wait wait;
        unsigned n = t1_sync_i_sample(NULL, &wait);
        UM_CHECK(n && t1_sync_i_held(&wait, 0));
        uint64_t *word = (uint64_t *)(void *)um_ram(sidepath_writer.pa);
        __atomic_store_n(word, sidepath_writer.after, __ATOMIC_RELEASE);
        code_cpu_write_end(acquired);
        sidepath_writer.changed = true;
        code_unlock();
        __atomic_store_n(&sidepath_writer.release, true, __ATOMIC_RELEASE);
        /* The source write retires and kicks. Instruction synchronization
         * subsequently waits for the sampled stale entry to leave. */
        for (unsigned k = 0; k < n; ++k)
            while (t1_sync_i_held(&wait, k))
                sched_yield();
        sidepath_writer.acknowledged = true;
        return NULL;
    }
    return NULL;
}

static void sidepath_live_write(struct vcpu *v, struct vcpu *ref, struct um_snapshot *snap,
                                 uint32_t id, uint64_t side, uint32_t site, uint32_t checkpoint)
{
    um_snapshot_restore(snap, v);
    struct t1_vcpu *tv = umt_tv(v);
    t1_vcpu_state_restored(tv);
    t1_test_set_restore_observer(NULL, NULL);
    t1_test_set_entry_hook(NULL, NULL);
    t1_test_set_compile_options(tv, &(struct t1_compile_options){.flags = T1_COMPILE_VERIFY});
    const struct t1_region *r = t1_region_get(id);
    CHECK(r && r->valid && r->npages == 2);
    uint64_t low, high;
    memcpy(&low, um_ram(side), 8);
    memcpy(&high, um_ram(side + 8), 8);
    /* Slot1 starts at bit46; nop.m's ignored immediate starts at bit6. */
    uint64_t after = low ^ (UINT64_C(1) << 52);
    Ia64Instruction nop = ia64_decode_insn(IA64_UNIT_M, ia64_bundle_slot(after, high, 1), side, 1);
    CHECK(nop.valid && nop.opcode == IA64_OP_NOP && nop.operands.system.immediate == 1);
    memset(&sidepath_writer, 0, sizeof(sidepath_writer));
    sidepath_writer.tv = tv;
    sidepath_writer.link = r->link;
    sidepath_writer.pa = side;
    sidepath_writer.after = after;
    sidepath_writer.retired = v->retired;
    sidepath_writer.site = site;
    sidepath_writer.checkpoint = checkpoint;
    CHECK(t1_test_force(tv, id, site, 1, T1_FORCE_TRAP_RETRY, NULL, NULL));
    t1_test_set_restore_observer(sidepath_write_observer, NULL);
    code_register_vcpu(v);
    pthread_t peer;
    CHECK(!pthread_create(&peer, NULL, sidepath_write_peer, NULL));
    while (!__atomic_load_n(&sidepath_writer.ready, __ATOMIC_ACQUIRE))
        sched_yield();
    const uint64_t budget = 1000000;
    struct vcpu_exit out = t1_run(tv, budget);
    __atomic_store_n(&sidepath_writer.stop, true, __ATOMIC_RELEASE);
    CHECK(!pthread_join(peer, NULL));
    CHECK(sidepath_writer.changed && sidepath_writer.acknowledged &&
          sidepath_writer.checkpoint_seen && !t1_region_from_link(sidepath_writer.link));
    code_unregister_vcpu(v);
    t1_test_set_restore_observer(NULL, NULL);
    CHECK(!memcmp(um_ram(side), &after, 8));
    memcpy(snap->ram + side - UM_RAM_BASE, &after, 8);
    struct um_run observed = {0};
    CHECK(!um_compare(snap, ref, &observed, v, &out, budget, "side path active code write"));
}

static void sidepath_runtime_case(bool virt)
{
    const uint64_t physical_head = UM_CODE + 0x72000, physical_side = physical_head + 0x1100;
    const uint64_t head = physical_head + (virt ? (UINT64_C(1) << 61) + 0x2000000 : 0);
    const uint64_t side = head + 0x1100;
    sidepath_fixture(physical_head, physical_side);
    uint64_t low, high, generation;
    CHECK(code_source_read_physical(physical_side, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
    struct vcpu *v = um_vcpu(head), *ref = um_vcpu(head);
    if (virt) {
        um_virtual_code(v);
        v->s.tr[0][1].va += 0x2000000;
        ia64_mmu_changed(v, IA64_MMU_TR, 0, 1);
    }
    uint64_t value = UINT64_C(0x123456789abcdef0);
    memcpy(um_ram(UM_DATA + 0x800), &value, sizeof(value));
    um_gr(v, 33, UM_DATA + 0x800, false);
    um_gr(v, 34, UM_DATA + 0x880, false);
    uint32_t id = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "side path");
    CHECK(id);
    const struct t1_region *r = t1_region_get(id);
    CHECK(r && r->ninsns == 15 && r->npages == 2);
    uint32_t site[2] = {UINT32_MAX, UINT32_MAX};
    uint32_t ckpt[2] = {UINT32_MAX, UINT32_MAX};
    for (unsigned k = 0; k < t1_point_count(id); ++k) {
        struct t1_point_info p;
        CHECK(t1_point_get(id, k, &p));
        if (p.kind == T1_PT_SITE && p.ip == side)
            site[0] = k;
        if (p.kind == T1_PT_SITE && p.ip == side + 16)
            site[1] = k;
        if (p.kind == T1_PT_CHECKPOINT && p.insn_index == 5)
            ckpt[0] = k;
        if (p.kind == T1_PT_CHECKPOINT && p.insn_index == 14)
            ckpt[1] = k;
    }
    CHECK(site[0] != UINT32_MAX && site[1] != UINT32_MAX);
    CHECK(ckpt[0] != UINT32_MAX && ckpt[1] != UINT32_MAX);
    unsigned cases = 0;
    struct umt_run run;
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    for (unsigned bof = 0; bof < 2; ++bof)
        for (unsigned path = 0; path < 4; ++path)
            for (unsigned pred = 0; pred < 2; ++pred) {
                snap.v.s.rse_bof = bof ? 95 : 0;
                um_gr(&snap.v, 33, UM_DATA + 0x800, false);
                um_gr(&snap.v, 34, UM_DATA + 0x880, false);
                um_pr(&snap.v, 6, path & 1);
                um_pr(&snap.v, 7, path & 2);
                um_pr(&snap.v, 8, path & 2);
                um_pr(&snap.v, 9, pred);
                for (unsigned budget = 1; budget <= 37; ++budget, ++cases) {
                    umt_case(v, ref, &snap, T1_COMPILE_VERIFY, budget, id, NULL, &run, false,
                             "side path budgets/predication");
                    CHECK(run.exit.reason == VCPU_EXIT_BUDGET &&
                          v->retired == snap.v.retired + budget);
                }
            }
    um_pr(&snap.v, 6, true);
    um_pr(&snap.v, 7, true);
    um_pr(&snap.v, 8, true);
    um_pr(&snap.v, 9, true);
    static const enum t1_force_action actions[] = {
        T1_FORCE_TRAP_EXIT, T1_FORCE_MISS_EXIT, T1_FORCE_TRAP_RETRY};
    for (unsigned point = 0; point < 2; ++point)
        for (unsigned a = 0; a < 3; ++a)
            for (unsigned occurrence = 1; occurrence <= 2; ++occurrence, ++cases) {
                struct umt_arm arm = {
                    .point = site[point], .occurrence = occurrence, .action = actions[a]};
                umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 37, id, &arm, &run, false,
                         "side path forced memory fault/retry");
                CHECK(run.forced == 1);
            }
    /* A false memory predicate suppresses the access even with a NaT
     * base. True predicates retain exact load/store fault IP and state. */
    for (unsigned point = 0; point < 2; ++point)
        for (unsigned fault = 0; fault < 2; ++fault)
            for (unsigned pred = 0; pred < 2; ++pred, ++cases) {
                um_gr(&snap.v, 33, UM_DATA + 0x800, false);
                um_gr(&snap.v, 34, UM_DATA + 0x880, false);
                um_gr(&snap.v, 33 + point,
                      fault ? UM_RAM_BASE + UM_RAM_SIZE + 0x1000 : UM_DATA + 0x800 + point * 0x80,
                      !fault);
                um_pr(&snap.v, 9, pred);
                umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 37, id, NULL, &run, false,
                         "side path real fault/NaT/predicate");
            }
    um_gr(&snap.v, 33, UM_DATA + 0x800, false);
    um_gr(&snap.v, 34, UM_DATA + 0x880, false);
    um_pr(&snap.v, 9, true);
    struct umt_arm kick = {.point = UINT32_MAX, .before_run = kick_before};
    for (unsigned path = 0; path < 2; ++path, ++cases) {
        um_pr(&snap.v, 6, path);
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 60, id, &kick, &run, false,
                 "side path checkpoint kick");
        CHECK(kick_exit(v, ckpt[path], path ? 12 : 6));
    }
    /* Deadlines before, within and after the appended path use exact
     * instruction counts, even when no old region boundary remains. */
    struct vcpu untimed = snap.v;
    for (unsigned deadline = 1; deadline <= 25; ++deadline, ++cases) {
        snap.v = untimed;
        um_pr(&snap.v, 6, true);
        snap.v.s.cr[CR_ITV] = 0xef;
        snap.v.s.cr[CR_ITM] = ia64_itc_read(&snap.v) + deadline;
        ia64_timer_program(&snap.v);
        umt_case(v, ref, &snap, T1_COMPILE_VERIFY, 37, id, NULL, &run, false,
                 "side path exact timer deadline");
        CHECK(v->timer_fired);
    }
    snap.v = untimed;
    /* A peer changes an ignored nop immediate on the appended page
     * while this region runs; synchronization waits for the stale entry. */
    sidepath_live_write(v, ref, &snap, id, physical_side, site[0], ckpt[1]);
    um_snapshot_free(&snap);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
    printf("unit_runtime: %s side path: %u exact budget, predication, wrapped frame, memory fault/NaT, "
           "retry and independent checkpoint cases; live appended-page write and synchronization acknowledgement\n", virt ? "virtual" : "physical", cases);
}

static void sidepath_runtime_test(void)
{
    sidepath_runtime_case(false);
    sidepath_runtime_case(true);
}
