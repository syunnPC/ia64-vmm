/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Connections are hints, independent of the instruction-fetch permission
 * and source-byte proof required at each entry (SDM Vol.2 §§4.1, 4.4). */
static struct t1_region *native_connection_retire;

static void native_connection_witness(gpa_t page, enum code_test_stage stage)
{
    (void)page;
    if (stage != CODE_TEST_WITNESS || !native_connection_retire)
        return;
    native_connection_retire->valid = false;
    native_connection_retire = NULL;
    code_lock();
    code_retire_epoch();
    code_unlock();
}

static void native_connections_test(struct t1_vcpu *tv, const struct t1_region *donor)
{
    struct vcpu *v = tv->v;
    uint64_t cfm = v->s.cfm;
    struct t1_stats stats = {0}, *saved_stats = tv->stats;
    tv->stats = &stats;
    code_register_vcpu(v);
    const struct code_fetch fetch = {.psr = v->s.psr};
    CHECK(code_dependencies_enter(v, (struct code_dependency *)donor->dep, donor->npages,
                                  &fetch));
    hostwin_enter(tv->hw, v);
    tv->x.observed_epoch = hostwin_bind_epoch(tv->hw);
    for (unsigned test = 0; test < 26; ++test) {
        struct t1_region *r[3];
        struct t1_point point = {.kind = T1_PT_EXIT, .restore_offset = 1};
        memset(&tv->fast, 0, sizeof(tv->fast));
        memset(tv->successors, 0, sizeof(tv->successors));
        memset(&stats, 0, sizeof(stats));
        v->s.cfm = cfm;
        for (unsigned k = 0; k < 3; ++k) {
            r[k] = t1_region_new();
            CHECK(r[k] != NULL);
            r[k]->c = donor->c;
            r[k]->c.ip = r[k]->c.entry_pa = donor->page[0].pa + 16 * k;
            r[k]->npages = 1;
            r[k]->dep[0].page = donor->dep[0].page;
            memcpy(r[k]->dep[0].mask, donor->dep[0].mask, sizeof(r[k]->dep[0].mask));
            r[k]->valid = r[k]->sealed = true;
            r[k]->max_path = 3;
        }
        point.ip = r[1]->c.ip;
        r[0]->point = &point;
        r[0]->npoints = r[0]->fast_points = 1;
        unsigned slot = t1_successor_index(r[0]->link, 0);
        if (test != 1)
            tv->successors[slot] = (struct t1_successor){r[0]->link, r[1]->link, 0};
        if (test == 2) {
            r[1]->policy = true;
            tv->windows.slot[r[1]->id % T1_WINDOW_SLOTS].link = 0;
        }
        if (test == 3) {
            r[1]->dep[0].page = donor->dep[1].page;
            memcpy(r[1]->dep[0].mask, donor->dep[1].mask, sizeof(r[1]->dep[0].mask));
        }
        if (test == 4) {
            r[0]->c.cfm = r[0]->c.cfm_mask = 0;
            r[1]->c.cfm_mask = 1;
            r[1]->c.cfm = (v->s.cfm ^ 1) & 1;
        }
        if (test == 8)
            r[1]->sealed = false;
        if (test == 9) {
            r[1]->c.psr_mask = IA64_PSR_MFL;
            r[1]->c.psr = v->s.psr & IA64_PSR_MFL;
        }
        if (test == 10) {
            r[1]->c.cfm_mask = 1;
            r[1]->c.cfm = v->s.cfm & 1;
        }
        if (test >= 14 && test <= 21) {
            r[2]->c = r[1]->c;
            if (test == 14) ++r[2]->c.abi;
            if (test == 15) r[2]->c.flags = T1_COMPILE_VERIFY;
            if (test == 16) r[2]->c.slot = 1;
            if (test == 17) r[2]->c.entry_pa += 16;
            if (test == 18) r[2]->nhelper = 1;
            if (test == 19) r[2]->npages = CODE_STAY_WITNESS_PAGES + 1;
            if (test == 20) r[2]->npoints = T1_FAST_POINTS + 1;
            if (test == 21) r[2]->nframe_in = 1;
        }
        if (test == 22) {
            r[1]->observed_returns = true;
            r[1]->return_sof = 32;
        }
        if (test == 24) {
            r[1]->policy = true;
            tv->windows.slot[r[1]->id % T1_WINDOW_SLOTS].link = r[1]->link;
        }
        t1_fast_prepare(tv, r[0]);
        CHECK((tv->x.fast != NULL) == (test == 0 || (test >= 5 && test != 8)));
        if (test == 0) {
            /* A direct-mapped successor collision loses the hint, not
             * the previously proved static connection. */
            tv->successors[slot] = (struct t1_successor){0};
            ++v->mmu_seq;
        } else if (test == 1) {
            tv->successors[slot] = (struct t1_successor){r[0]->link, r[1]->link, 0};
        } else if (test == 2) {
            tv->windows.slot[r[1]->id % T1_WINDOW_SLOTS].link = r[1]->link;
        } else if (test == 3) {
            CHECK(code_dependencies_enter(v, r[1]->dep, 1, &fetch));
        } else if (test == 4) {
            v->s.cfm ^= 1;
        } else if (test == 5 || test == 7) {
            /* A new retirement epoch must reject opaque cached addresses
             * before touching them, even if source and context repeat. */
            tv->fast.node[1].region = (void *)hostwin_poison_va(tv->hw);
            r[1]->valid = false;
            code_lock();
            code_retire_epoch();
            code_unlock();
            code_quiescent(v);
            if (test == 7) {
                uint64_t old = r[1]->link;
                struct t1_contract contract = r[1]->c;
                t1_region_discard(r[1]);
                r[1] = t1_region_new();
                CHECK(r[1] && r[1]->link != old);
                r[1]->c = contract;
                r[1]->npages = 1;
                r[1]->dep[0].page = donor->dep[0].page;
                memcpy(r[1]->dep[0].mask, donor->dep[0].mask, sizeof(r[1]->dep[0].mask));
                r[1]->valid = r[1]->sealed = true;
                r[1]->max_path = 3;
                t1_fast_prepare(tv, r[0]);
                CHECK(!tv->x.fast && !t1_region_from_link(old));
                tv->successors[slot] = (struct t1_successor){r[0]->link, r[1]->link, 0};
            }
        } else if (test == 8) {
            r[1]->sealed = true;
        } else if (test >= 9) {
            /* A retained positive proof must lose eligibility when any
             * contract input or learned target changes. Sealing, dirty
             * partitions and policy ownership remain admission checks. */
            uint64_t psr = v->s.psr;
            unsigned model = v->cfg.cpu_model, dirty = v->s.rse_dirty;
            bool icount = v->cfg.itc_icount;
            struct hostwin_vcpu *hw = tv->hw, *other = NULL;
            tv->fast.nnodes = 0;
            if (test == 9) v->s.psr ^= IA64_PSR_MFL;
            if (test == 10) v->s.cfm ^= 1;
            if (test == 11) v->cfg.cpu_model ^= 1;
            if (test == 12) v->cfg.itc_icount = !icount;
            if (test == 13) {
                struct hostwin_config cfg = {hostwin_bits(hw) + 1, 1, false};
                other = hostwin_create(&cfg, hostwin_linux_backend(), hostwin_linux_ctx());
                CHECK(other != NULL);
                hostwin_enter(other, v);
                tv->hw = other;
            }
            if (test >= 14 && test <= 21)
                tv->successors[slot] = (struct t1_successor){r[0]->link, r[2]->link, 0};
            if (test == 22)
                v->s.rse_dirty = IA64_N_STACKED_PHYS - r[1]->return_sof + 1;
            if (test == 23) r[1]->sealed = false;
            if (test == 24) tv->windows.slot[r[1]->id % T1_WINDOW_SLOTS].link = 0;
            if (test == 25) {
                native_connection_retire = r[1];
                code_validity_set_test_hook(native_connection_witness);
            }
            static struct vcpu unchanged;
            unchanged = *v;
            t1_fast_prepare(tv, r[0]);
            CHECK(!tv->x.fast && !memcmp(v, &unchanged, sizeof(*v)));
            if (test == 25) {
                CHECK(!native_connection_retire);
                code_validity_set_test_hook(NULL);
            }
            v->s.psr = psr;
            v->s.cfm = cfm;
            v->cfg.cpu_model = model;
            v->cfg.itc_icount = icount;
            v->s.rse_dirty = dirty;
            r[1]->sealed = true;
            tv->windows.slot[r[1]->id % T1_WINDOW_SLOTS].link = r[1]->link;
            tv->successors[slot] = (struct t1_successor){r[0]->link, r[1]->link, 0};
            tv->hw = hw;
            if (other)
                hostwin_destroy(other);
        } else {
            /* A head with no learned native successor must not discard
             * the usable graph reached again after that head. */
            r[2]->point = &point;
            r[2]->npoints = r[2]->fast_points = 1;
            for (unsigned k = 0; k < 3; ++k) {
                t1_fast_prepare(tv, r[2]);
                CHECK(!tv->x.fast);
            }
#if IA64VMM_TIER1_STATS
            CHECK(stats.native_topology_negative == 3);
            CHECK(stats.native_node_reject[T1_NATIVE_NODE_SUCCESSOR] == 1);
#endif
        }
        static struct vcpu unchanged;
        unchanged = *v;
        t1_fast_prepare(tv, r[0]);
        CHECK(!memcmp(v, &unchanged, sizeof(*v)));
        CHECK((tv->x.fast != NULL) == (test != 5 && test != 25));
        if (test == 6)
            CHECK(stats.chain_native_reuses == 1);
        if (test != 5 && test != 25)
            CHECK(tv->x.fast->edge[0].target &&
                  tv->x.fast->edge[0].target->region == r[1]);
        tv->x.fast = NULL;
        for (unsigned k = 0; k < 3; ++k) {
            r[k]->valid = false;
            t1_region_discard(r[k]);
        }
    }
    v->s.cfm = cfm;
    memset(&tv->fast, 0, sizeof(tv->fast));
    memset(tv->successors, 0, sizeof(tv->successors));
    code_unregister_vcpu(v);
    v->code_entry_generation = 0;
    tv->stats = saved_stats;
}
