/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compare generated packed arithmetic with the interpreter, including
 * aliases, old predicated destinations, NaTs and frame-relative homes.
 * Whole-state comparisons at captures also exercise live arithmetic at
 * memory-site and checkpoint exits. Product cases have verification flags off. */
static void t_packed_wrap(void)
{
    static const uint64_t edge[] = {
        0, UINT64_MAX, UINT64_C(0x7f7f7f7f7f7f7f7f), UINT64_C(0x8080808080808080),
        UINT64_C(0x0000ffff0000ffff), UINT64_C(0xffff0000ffff0000),
        UINT64_C(0x00000000ffffffff), UINT64_C(0xffffffff00000000)
    };
    static const unsigned bofs[] = {0, 44, 90, 95};
    unsigned runs = 0, regions = 0;
    uint64_t ip = UM_CODE + 0x6000;
    for (unsigned sub = 0; sub < 2; ++sub)
        for (unsigned width = 0; width < 3; ++width)
            for (unsigned stacked = 0; stacked < 2; ++stacked)
                for (unsigned flavor = 0; flavor < 5; ++flavor) {
                    uint32_t flags = flavor < 4 ? T1_COMPILE_VERIFY : 0;
                    unsigned alias = flavor % 4;
                    unsigned ra = stacked ? 32 : 14, rb = stacked ? 39 : 15;
                    unsigned dst = stacked ? 40 : 16, out = stacked ? 41 : 17;
                    unsigned constant = stacked ? 42 : 18, qp = flavor ? (stacked ? 21 : 6) : 0;
                    if (alias == 1) dst = ra;
                    if (alias == 2) dst = rb;
                    if (alias == 3) rb = ra;
                    struct ienc_bundle b[] = {
                        {IENC_STOP(IENC_MII), {ienc_packed(qp, sub, 8u << width, 0, dst, ra, rb),
                            ienc_alu(0, IENC_XOR, out, dst, constant), ienc_nop_i()}},
                        {IENC_STOP(IENC_MIB), {ienc_ld(0, 8, false, 19, 30), ienc_nop_i(), ienc_br_cloop(-16)}},
                        {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
                        {IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}}
                    };
                    um_emit(ip, b, 4);
                    struct vcpu *v = um_vcpu(ip), *ref = um_vcpu(ip);
                    v->s.cfm = um_cfm(24, 24, 16, 5, 43);
                    v->s.rse_bof = 44;
                    v->s.ar[AR_LC] = 2;
                    struct um_snapshot initial;
                    um_snapshot_take(&initial, v);
                    struct t1_compile_report report;
                    char label[100];
                    snprintf(label, sizeof(label), "packed %s%u stacked=%u alias=%u flags=%u",
                             sub ? "sub" : "add", 8u << width, stacked, alias, flags);
                    uint32_t id = umt_compile(v, flags, 0, 0, &report, label);
                    CHECK(id && report.insns == 6);
                    const struct t1_region *region = t1_region_get(id);
                    CHECK(region && region->loop && region->nhelper == 0);
                    ++regions;
                    for (unsigned state = 0; state < 16; ++state) {
                        um_snapshot_restore(&initial, v);
                        v->s.rse_bof = bofs[state / 4];
                        poison_outside_frame(v);
                        /* Vary only the packed inputs and old destination
                         * NaTs. Unrelated native memory inputs stay clear;
                         * every combination must enter generated code. */
                        for (unsigned r = 1; r < 56; ++r)
                            um_gr(v, r, rnd64(), false);
                        um_gr(v, dst, rnd64(), (state & 3) != 0);
                        um_gr(v, ra, state < 8 ? edge[state] : rnd64(), state & 1);
                        um_gr(v, rb, state < 8 ? edge[(state * 3 + 1) % 8] : rnd64(), state & 2);
                        um_gr(v, constant, rnd64(), false);
                        um_gr(v, 30, UM_DATA, false);
                        um_pr(v, qp, (state >> 2) & 1);
                        struct um_snapshot snap;
                        um_snapshot_take(&snap, v);
                        struct umt_run run;
                        unsigned first_budget = state < 2 ? 0 : 14;
                        for (unsigned budget = first_budget; budget <= 14; ++budget) {
                            CHECK(umt_case(v, ref, &snap, flags, budget, id, NULL, &run, false, label) == 0);
                            ++runs;
                            if (budget == 14) {
                                if (!run.run.entries)
                                    fprintf(stderr, "%s state=%u guards=%u, expected native entry\n",
                                            label, state, report.nat_guards);
                                CHECK(run.run.entries > 0);
                            }
                        }
                        if (state == 4 && flags) {
                            struct umt_arm forced = {.point = point_at(id, T1_PT_SITE, 0),
                                .occurrence = 1, .action = T1_FORCE_TRAP_EXIT};
                            CHECK(umt_case(v, ref, &snap, flags, 24, id, &forced, &run, false, label) == 0);
                            CHECK(run.forced == 1); ++runs;
                            struct umt_arm kick = {.point = UINT32_MAX, .before_run = kick_before};
                            CHECK(umt_case(v, ref, &snap, flags, 24, id, &kick, &run, false, label) == 0);
                            ++runs;
                            struct umt_arm req = {.point = UINT32_MAX, .before_run = hook_before,
                                .resume_requested = true};
                            CHECK(umt_case(v, ref, &snap, flags, 24, id, &req, &run, false, label) == 0);
                            t1_test_set_entry_hook(NULL, NULL);
                            CHECK(run.resumed == 1); ++runs;
                        }
                        um_snapshot_free(&snap);
                    }
                    um_snapshot_free(&initial);
                    umt_release(v); vcpu_destroy(v); vcpu_destroy(ref);
                }
    printf("unit_runtime: packed wrap: %u regions, %u whole-state comparisons, aliases/NaT/qp/BOF/budgets/exits\n",
           regions, runs);
}

/* Loads consume NaT; tnat tests it (SDM Vol.3 Table 2-58), and general
 * packed helpers propagate it. Check native entry and exact results. */
static void t_packed_consumers(void)
{
    unsigned runs = 0;
    const uint64_t ip = UM_CODE + 0x6800;
    for (unsigned consumer = 0; consumer < 3; ++consumer)
        for (unsigned stacked = 0; stacked < 2; ++stacked)
            for (unsigned verified = 0; verified < 2; ++verified) {
                unsigned dst = stacked ? 32 : 14, ra = stacked ? 39 : 15;
                unsigned rb = stacked ? 40 : 16, out = stacked ? 41 : 20;
                uint32_t flags = verified ? T1_COMPILE_VERIFY : 0;
                struct ienc_bundle b[] = {
                    {IENC_STOP(IENC_MII), {ienc_packed(0, false, 32, 0, dst, ra, rb),
                        ienc_nop_i(), ienc_nop_i()}},
                    {IENC_STOP(IENC_MIB), {
                        consumer == 0 ? ienc_ld(0, 8, false, out, dst) :
                        consumer == 2 ? ienc_packed(0, false, 8, 1, out, dst, 17) : ienc_nop_m(),
                        consumer == 1 ? ienc_tnat(0, 0, 0, 0, 6, 7, dst) : ienc_nop_i(),
                        ienc_br_cloop(-16)}},
                    {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
                    {IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}}
                };
                um_emit(ip, b, 4);
                struct vcpu *v = um_vcpu(ip), *ref = um_vcpu(ip);
                v->s.cfm = um_cfm(24, 24, 16, 5, 0);
                v->s.rse_bof = stacked ? 95 : 0;
                for (unsigned r = 1; r < 56; ++r) um_gr(v, r, 0, false);
                um_gr(v, ra, UM_DATA, false);
                v->s.ar[AR_LC] = 2;
                struct t1_compile_report report;
                char label[100];
                snprintf(label, sizeof(label), "packed consumer=%u stacked=%u verify=%u",
                         consumer, stacked, verified);
                uint32_t id = umt_compile(v, flags, 0, 0, &report, label);
                CHECK(id && report.insns == 6);
                const struct t1_region *region = t1_region_get(id);
                if (consumer == 0) {
                    /* G uses rotated frame offsets, not architectural
                     * names or the physical homes at BOF 95. */
                    unsigned ga = stacked ? 32 + (ra - 32 + 5) % 16 : ra;
                    unsigned gb = stacked ? 32 + (rb - 32 + 5) % 16 : rb;
                    if (!((region->nat_guard[ga / 64] >> (ga % 64)) & 1) ||
                        !((region->nat_guard[gb / 64] >> (gb % 64)) & 1))
                        fprintf(stderr, "%s guards=%#llx/%#llx count=%u\n", label,
                                (unsigned long long)region->nat_guard[0],
                                (unsigned long long)region->nat_guard[1], report.nat_guards);
                    CHECK((region->nat_guard[ga / 64] >> (ga % 64)) & 1);
                    CHECK((region->nat_guard[gb / 64] >> (gb % 64)) & 1);
                } else CHECK(!region->nat_guard[0] && !region->nat_guard[1]);
                for (unsigned nat = 0; nat < 4; ++nat) {
                    v->s.ip = ip; v->s.slot = 0;
                    v->s.ar[AR_LC] = 2;
                    um_gr(v, ra, UM_DATA, nat & 1);
                    um_gr(v, rb, 0, nat & 2);
                    struct um_snapshot snap;
                    um_snapshot_take(&snap, v);
                    for (unsigned budget = 0; budget <= 20; ++budget) {
                        struct umt_run run;
                        CHECK(!umt_case(v, ref, &snap, flags, budget, id, NULL, &run, false, label));
                        if (budget == 20) {
                            if (!nat || consumer != 0) CHECK(run.run.entries > 0);
                            if (nat && !consumer) CHECK(v->fault.kind == IA64_FAULT_NAT);
                            if (consumer == 1)
                                CHECK(((v->s.pr >> 6) & 3) == (nat ? 2u : 1u) &&
                                      ia64_nat_read(&v->s, dst) == (nat != 0));
                        }
                        ++runs;
                    }
                    um_snapshot_restore(&snap, v);
                    um_snapshot_free(&snap);
                }
                umt_release(v); vcpu_destroy(v); vcpu_destroy(ref);
            }
    printf("unit_runtime: packed consumers: %u whole-state comparisons, load/legacy helper/general helper, NaT and BOF\n", runs);
}
