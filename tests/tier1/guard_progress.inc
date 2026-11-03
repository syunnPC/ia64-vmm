/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/upper.h"
#include "engine/interp/interp.h"

static struct guard_progress_state {
    uint64_t call, target, retired;
    unsigned slot, entries, lower, pending;
    enum tier0_upper_status (*enter)(struct vcpu *, uint64_t *, uint64_t, struct vcpu_exit *);
    struct vcpu_exit (*step)(struct vcpu *, uint64_t);
} guard_progress;

static void guard_progress_entry(struct t1_vcpu *tv, uint32_t id, void *arg)
{
    (void)id; (void)arg;
    if (tv->v->s.ip == guard_progress.call && tv->v->s.slot == guard_progress.slot)
        CHECK(++guard_progress.entries == 1);
}

static struct vcpu_exit guard_progress_lower(struct vcpu *v, uint64_t budget)
{
    CHECK(v->s.ip == guard_progress.call && v->s.slot == guard_progress.slot && budget == 1);
    CHECK(++guard_progress.lower == 1);
    struct vcpu_exit out = guard_progress.step(v, budget);
    CHECK(out.reason == VCPU_EXIT_BUDGET);
    out.reason = VCPU_EXIT_DEBUG;
    return out;
}

/* Stop at the callee's lookup, before its first instruction. The only
 * lower-tier retirement after the guard must be the call. */
static enum tier0_upper_status guard_progress_upper(struct vcpu *v, uint64_t *link, uint64_t budget,
                                                    struct vcpu_exit *out)
{
    if (v->s.ip == guard_progress.target) {
        CHECK(v->retired == guard_progress.retired && v->s.slot == 0);
        ++guard_progress.lower;
        *out = (struct vcpu_exit){VCPU_EXIT_BUDGET, v->s.ip, v->s.slot, 0, NULL};
        return TIER0_UPPER_STOP;
    }
    enum tier0_upper_status st = guard_progress.enter(v, link, budget, out);
    CHECK(st == TIER0_UPPER_RAN_PENDING && v->s.ip == guard_progress.call &&
          v->s.slot == guard_progress.slot);
    CHECK(++guard_progress.pending == 1);
    return st;
}

/* A guard at the entry must let the lower tier execute that instruction
 * once. A preceding chained region's retirement does not change this. */
static void t_guard_progress(void)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        bool product = mode != 1, chain = mode == 2;
        if (chain && t1_cfg.chain_off) continue;
        for (unsigned at = 0; at < (chain ? 1u : 2u); ++at) {
            unsigned slot = at ? 2 : 0;
            uint64_t call = PROG + 0x40, leaf = PROG + 0x80, target = PROG + 0xc0;
            struct ienc_bundle b[14];
            for (unsigned i = 0; i < 14; ++i)
                b[i] = (struct ienc_bundle){IENC_STOP(IENC_MII),
                                          {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}};
            b[0] = (struct ienc_bundle){IENC_STOP(IENC_MIB),
                                       {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(0x40)}};
            b[4] = slot ? (struct ienc_bundle){IENC_STOP(IENC_MIB),
                       {ienc_nop_m(), ienc_nop_i(), ienc_br_call_indirect(0, 0, 2)}} :
                       (struct ienc_bundle){IENC_STOP(IENC_BBB),
                       {ienc_br_call_indirect(0, 0, 2), ienc_nop_b(), ienc_nop_b()}};
            b[5] = (struct ienc_bundle){IENC_STOP(IENC_MIB),
                                       {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}};
            b[8] = (struct ienc_bundle){IENC_STOP(IENC_MIB),
                                       {ienc_nop_m(), ienc_nop_i(), ienc_br_ret(0, 0)}};
            b[12] = (struct ienc_bundle){IENC_STOP(IENC_MIB),
                                        {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}};
            um_emit(PROG, b, 14);
            struct vcpu *v = um_vcpu(call), *ref = um_vcpu(call);
            v->s.slot = slot;
            v->s.cfm = um_cfm(6, 3, 0, 0, 0);
            v->s.rse_bof = 95;
            v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = UM_DATA;
            v->s.ar[AR_EC] = 40;
            v->s.ar[AR_LC] = 1;
            v->s.br[0] = 0x12345678;
            v->s.br[2] = leaf | 15;
            struct t1_vcpu *tv = umt_tv(v);
            uint32_t cid = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "guard progress");
            CHECK(cid);
            v->s.br[2] = target | 7;
            struct um_snapshot snap;
            um_snapshot_take(&snap, v);
            uint64_t instructions = 1;
            if (product) {
                v->s.ip = target;
                v->s.slot = 0;
                v->s.cfm = 3;
                CHECK(umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "guard progress stop"));
                um_snapshot_restore(&snap, v);
                v->upper = tv;
                if (chain) {
                    v->s.ip = PROG;
                    v->s.slot = 0;
                    uint32_t aid = umt_compile(v, T1_COMPILE_VERIFY, 0, 0, NULL, "guard progress predecessor");
                    CHECK(aid);
                    struct vcpu_exit out;
                    uint64_t link = t1_region_link(t1_region_get(aid));
                    CHECK(t1_upper_enter(v, &link, 64, &out) == T1_UPPER_RAN && v->s.ip == call);
                    link = t1_region_link(t1_region_get(cid));
                    CHECK(t1_upper_enter(v, &link, 64, &out) == T1_UPPER_RAN_PENDING);
                    struct t1_successor successors[T1_SUCCESSORS];
                    memcpy(successors, tv->successors, sizeof(successors));
                    um_snapshot_restore(&snap, v);
                    v->s.ip = PROG;
                    v->s.slot = 0;
                    snap.v = *v;
                    t1_vcpu_state_restored(tv);
                    memcpy(tv->successors, successors, sizeof(successors));
                    instructions += 3;
                } else t1_vcpu_state_restored(tv);
                struct tier0_upper_ops ops = *engine_tier1_ops();
                tv->nexits = 0;
                guard_progress = (struct guard_progress_state){.call = call, .target = target, .slot = slot,
                    .retired = snap.v.retired + instructions, .enter = ops.enter};
                ops.enter = guard_progress_upper;
                v->cfg.engine = ENGINE_TIER1;
                v->upper = tv;
                tier0_vcpu_state_restored(v);
                tier0_set_upper(&ops);
                t1_test_set_entry_hook(guard_progress_entry, NULL);
                struct vcpu_exit out = tier0_run(v, 64);
                tier0_set_upper(NULL);
                t1_test_set_entry_hook(NULL, NULL);
                CHECK(out.reason == VCPU_EXIT_BUDGET && guard_progress.pending == 1 && guard_progress.lower == 1);
            } else {
                t1_vcpu_state_restored(tv);
                guard_progress = (struct guard_progress_state){.call = call, .slot = slot, .step = t1_cfg.lower};
                t1_cfg.lower = guard_progress_lower;
                t1_test_set_entry_hook(guard_progress_entry, NULL);
                struct vcpu_exit out = t1_run(tv, 64);
                t1_test_set_entry_hook(NULL, NULL);
                t1_cfg.lower = guard_progress.step;
                CHECK(out.reason == VCPU_EXIT_DEBUG && guard_progress.lower == 1 && guard_progress.entries == 1);
            }
            CHECK(v->retired == snap.v.retired + instructions && v->s.ip == target && v->s.slot == 0);
            struct vcpu_exit normalized = {VCPU_EXIT_BUDGET, v->s.ip, v->s.slot, 0, NULL};
            struct um_run run = {0};
            CHECK(!um_compare(&snap, ref, &run, v, &normalized, instructions, "guard progress"));
            struct t1_exit_info xi;
            unsigned zero = 0;
            for (unsigned i = 0; i < t1_exit_count(tv); ++i)
                if (t1_exit_get(tv, i, &xi) && xi.region_id == cid && !xi.consumed) {
                    CHECK(xi.kind == T1_PT_EXIT && xi.lower_tier_step_followed);
                    ++zero;
                }
            CHECK(zero == 1 && (!chain || guard_progress.entries == 1));
            um_snapshot_free(&snap);
            umt_release(v);
            vcpu_destroy(v); vcpu_destroy(ref);
        }
    }
    puts("unit_runtime: guard progress: entry calls in slots 0/2 and a chained guard execute once in the lower tier");
}
