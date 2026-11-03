/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Both regions cross a physical page boundary. Their taken branches leave
 * the formation request, so every transition requires a real region entry. */
static struct {
    struct t1_vcpu *tv;
    uint64_t pa, after, old_link, completed;
    bool ready, stop, changed;
} native_pages_writer;

static void *native_pages_peer(void *unused)
{
    (void)unused;
    struct t1_vcpu *tv = native_pages_writer.tv;
    __atomic_store_n(&native_pages_writer.ready, true, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&native_pages_writer.stop, __ATOMIC_ACQUIRE)) {
        if (atomic_load_explicit(&tv->fast.completed, memory_order_acquire) < native_pages_writer.completed + 32) {
            sched_yield();
            continue;
        }
        code_lock();
        const struct t1_region *r = atomic_load_explicit(&tv->running, memory_order_seq_cst);
        if (!r || !r->valid || r->npages != 2) {
            code_unlock();
            sched_yield();
            continue;
        }
        native_pages_writer.old_link = r->link;
        native_pages_writer.pa = r->page[1].pa;
        uint64_t *word = (uint64_t *)(void *)um_ram(native_pages_writer.pa);
        uint64_t low = __atomic_load_n(word, __ATOMIC_ACQUIRE), high;
        memcpy(&high, word + 1, 8);
        /* M-unit nop's ignored immediate bit 0 is bundle bit 11. */
        native_pages_writer.after = low ^ (UINT64_C(1) << 11);
        Ia64Instruction nop = ia64_decode_insn(IA64_UNIT_M,
            ia64_bundle_slot(native_pages_writer.after, high, 0), native_pages_writer.pa, 0);
        UM_CHECK(nop.valid && nop.opcode == IA64_OP_NOP && nop.operands.system.immediate == 1);
        bool acquired = code_vmm_write_begin(native_pages_writer.pa, 8);
        __atomic_store_n(word, native_pages_writer.after, __ATOMIC_RELEASE);
        code_cpu_write_end(acquired);
        UM_CHECK(!r->valid && !t1_region_from_link(r->link));
        native_pages_writer.changed = true;
        code_unlock();
        return NULL;
    }
    return NULL;
}

/* Exercise the product upper-entry API with already learned links. The
 * small lower-tier remainder also covers budget rejection after a batch. */
static struct vcpu_exit native_pages_run(struct vcpu *v, const uint64_t head[2],
                                         uint64_t link[2], uint64_t budget)
{
    uint64_t start = v->retired;
    struct vcpu_exit out = {0};
    while (v->retired - start < budget) {
        unsigned k = v->s.ip == head[1];
        if (!v->s.slot && v->s.ip == head[k]) {
            enum t1_upper_status status = t1_upper_enter(v, &link[k], budget - (v->retired - start), &out);
            if (status == T1_UPPER_STOP)
                return out;
            if (status == T1_UPPER_RAN)
                continue;
        }
        out = interp_run(v, 1);
        if (out.reason != VCPU_EXIT_BUDGET)
            return out;
    }
    return (struct vcpu_exit){VCPU_EXIT_BUDGET, v->s.ip, v->s.slot, 0, NULL};
}

/* Metadata-only graph fixtures share a real validated dependency. No
 * synthetic instruction body executes; the real two-page ring below
 * separately checks generated execution and invalidation. */
#include "tests/tier1/native_connections.inc"

static void native_plan_priority(struct t1_vcpu *tv, const struct t1_region *donor)
{
    struct vcpu *v = tv->v;
    static struct t1_stats stats;
    struct t1_stats *saved_stats = tv->stats;
    tv->stats = &stats;
    struct t1_region *region[9];
    struct t1_point point[9][T1_FAST_POINTS] = {{{0}}};
    for (unsigned k = 0; k < 9; ++k) {
        region[k] = t1_region_new();
        CHECK(region[k] != NULL);
        struct t1_region *r = region[k];
        r->c = donor->c;
        r->c.ip = r->c.entry_pa = donor->page[0].pa + 16 * k;
        r->npages = k == 8 ? 2 : 1;
        for (unsigned p = 0; p < r->npages; ++p) {
            r->dep[p].page = donor->dep[p].page;
            memcpy(r->dep[p].mask, donor->dep[p].mask, sizeof(r->dep[p].mask));
        }
        r->valid = r->sealed = true;
        r->point = point[k];
        r->max_path = 3;
    }
    code_register_vcpu(v);
    const struct code_fetch fetch = {.psr = v->s.psr};
    CHECK(code_dependencies_enter(v, (struct code_dependency *)donor->dep, donor->npages, &fetch));
    hostwin_enter(tv->hw, v);
    tv->x.observed_epoch = hostwin_bind_epoch(tv->hw);
    for (unsigned test = 0; test < 3; ++test) {
        unsigned hot = test ? 6 : 8;
        bool used[T1_SUCCESSORS] = {0};
        memset(tv->successors, 0, sizeof(tv->successors));
        memset(&tv->fast, 0, sizeof(tv->fast));
        for (unsigned k = 0; k < 9; ++k) {
            struct t1_region *r = region[k];
            r->npoints = r->fast_points = 0;
            if (k >= hot && k != 8)
                continue;
            for (unsigned edge = 0; edge < (k ? 1 : 2); ++edge) {
                unsigned target = !k && !edge ? 8 : k == 8 ? 0 : (k + 1) % hot;
                unsigned p = 0;
                while (p < T1_FAST_POINTS && used[t1_successor_index(r->link, p)])
                    ++p;
                CHECK(p < T1_FAST_POINTS);
                unsigned slot = t1_successor_index(r->link, p);
                used[slot] = true;
                point[k][p] = (struct t1_point){.kind = T1_PT_EXIT, .ip = region[target]->c.ip,
                                               .restore_offset = 1};
                r->npoints = p + 1;
                r->fast_points |= UINT32_C(1) << p;
                tv->successors[slot] = (struct t1_successor){r->link, region[target]->link, p};
            }
        }
        unsigned first = test == 2 ? 8 : 0;
        memset(&stats, 0, sizeof(stats));
        t1_fast_prepare(tv, region[first]);
#if IA64VMM_TIER1_STATS
        CHECK(stats.native_rebuild[T1_NATIVE_REBUILD_EMPTY] == 1);
        CHECK(stats.native_build_nodes[test ? 7 : 8] == 1);
        CHECK(stats.native_build_edges[8] == 1 && tv->fast.diag_edges == 8);
        CHECK(stats.native_node_reject[T1_NATIVE_NODE_CAPACITY] == (test == 0));
#endif
        CHECK(tv->x.fast == &tv->fast.node[0] && tv->fast.node[0].region == region[first]);
        CHECK(tv->fast.nnodes == (test ? 7 : 8));
        for (unsigned k = 0; k < hot; ++k) {
            bool found = false;
            for (unsigned n = 0; n < tv->fast.nnodes; ++n)
                found |= tv->fast.node[n].region == region[k];
            CHECK(found);
        }
        bool cold = false;
        for (unsigned n = 0; n < tv->fast.nnodes; ++n)
            cold |= tv->fast.node[n].region == region[8];
        CHECK(cold == (test != 0));
    }
#if IA64VMM_TIER1_STATS
    for (unsigned test = 0; test < 8; ++test) {
        struct t1_region *from = region[0], *to = region[1];
        memset(&stats, 0, sizeof(stats));
        memset(&tv->fast, 0, sizeof(tv->fast));
        memset(tv->successors, 0, sizeof(tv->successors));
        from->npoints = from->fast_points = 1;
        to->npoints = to->fast_points = 0;
        point[0][0] = (struct t1_point){.kind = T1_PT_EXIT, .ip = to->c.ip,
                                       .restore_offset = 1};
        unsigned slot = t1_successor_index(from->link, 0);
        tv->successors[slot] = (struct t1_successor){from->link, to->link, 0};
        uint64_t psr = v->s.psr;
        if (test == 0) to->nhelper = 1;
        if (test == 1) to->policy = true;
        if (test == 2) to->dep[0].page = NULL;
        if (test == 3) v->s.psr |= IA64_PSR_IT;
        if (test == 4) to->npages = 2;
        if (test == 5) tv->successors[slot].from = 0;
        if (test == 6) to->valid = false;
        t1_fast_prepare(tv, from);
        static const unsigned rejected[] = {
            T1_NATIVE_NODE_SHAPE, T1_NATIVE_NODE_WINDOW, T1_NATIVE_NODE_WITNESS,
            T1_NATIVE_NODE_CONTRACT, T1_NATIVE_NODE_GROUP, T1_NATIVE_NODE_SUCCESSOR,
            T1_NATIVE_NODE_CONTRACT};
        if (test < 7) {
            CHECK(stats.native_node_reject[rejected[test]] == 1);
            bool negative = test == 0 || test == 3 || test == 5 || test == 6;
            CHECK(stats.native_topology_negative == negative);
            CHECK(stats.native_build_edges[0] == !negative);
            CHECK(stats.native_build_nodes[1] == !negative);
        } else {
            CHECK(stats.native_build_nodes[2] == 1 && stats.native_build_edges[1] == 1);
            CHECK(tv->fast.diag_edges == 1);
        }
        v->s.psr = psr;
        to->nhelper = 0;
        to->policy = false;
        to->npages = 1;
        to->dep[0].page = donor->dep[0].page;
        to->valid = true;
    }
#endif
    tv->x.fast = NULL;
    memset(&tv->fast, 0, sizeof(tv->fast));
    memset(tv->successors, 0, sizeof(tv->successors));
    code_unregister_vcpu(v);
    for (unsigned k = 0; k < 9; ++k) {
        region[k]->valid = false;
        t1_region_discard(region[k]);
    }
    tv->stats = saved_stats;
}

static void native_pages_test(void)
{
    const uint64_t head[2] = {UM_CODE + 0x80ff0, UM_CODE + 0x88ff0};
    for (unsigned k = 0; k < 2; ++k) {
        const struct ienc_bundle code[] = {
            {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_adds(0, 14 + k, 1, 14 + k), ienc_nop_i()}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(),
                ienc_br_cond(6, (int64_t)(head[1 - k] - (head[k] + 16)))}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, -32)}},
            {IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}}
        };
        um_emit(head[k], code, sizeof(code) / sizeof(code[0]));
    }
    struct vcpu *v = um_vcpu(head[0]), *ref = um_vcpu(head[0]);
    um_pr(v, 6, true);
    struct hostwin_config cfg = {40, 1, false};
    struct hostwin_vcpu *hw = hostwin_create(&cfg, hostwin_linux_backend(), hostwin_linux_ctx());
    CHECK(hw != NULL);
    struct t1_vcpu *tv = t1_vcpu_create(v, hw);
    CHECK(tv != NULL);
    v->upper = tv;
    t1_test_set_compile_options(tv, &(struct t1_compile_options){0});
    t1_test_set_restore_observer(NULL, NULL);
    t1_test_set_entry_hook(NULL, NULL);
    uint64_t link[2];
    uint32_t point[2] = {UINT32_MAX, UINT32_MAX};
    for (unsigned k = 0; k < 2; ++k) {
        v->s.ip = head[k];
        struct t1_compile_report report;
        CHECK(t1_compile(tv, &(struct t1_compile_options){0}, &report) == T1_COMPILED);
        const struct t1_region *r = t1_region_get(report.region_id);
        CHECK(r && r->npages == 2 && r->ninsns == 9 && r->fast_points);
        link[k] = r->link;
        for (unsigned p = 0; p < r->npoints; ++p)
            if (r->point[p].kind == T1_PT_EXIT && r->point[p].ip == head[1 - k])
                point[k] = p;
        CHECK(point[k] != UINT32_MAX);

    }
    native_connections_test(tv, t1_region_from_link(link[0]));
    native_plan_priority(tv, t1_region_from_link(link[0]));
    for (unsigned k = 0; k < 2; ++k)
        tv->successors[t1_successor_index(link[k], point[k])] =
            (struct t1_successor){link[k], link[1 - k], point[k]};
    v->s.ip = head[0];
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    code_register_vcpu(v);
    const uint64_t budget = 60000;
    struct vcpu_exit out = native_pages_run(v, head, link, budget);
    CHECK(out.reason == VCPU_EXIT_BUDGET && tv->fast.completed > 32);
    bool group = false;
    for (unsigned k = 0; k < tv->fast.nnodes; ++k) {
        const struct t1_fast_node *n = &tv->fast.node[k];
        CHECK(n->region->npages == 2 && n->second);
        struct code_fetch fetch = {.psr = v->s.psr};
        struct code_stay_witness single;
        CHECK(!code_stay_witness_take(v, &n->region->dep[1], &fetch, &single));
        group = true;
    }
    CHECK(group);
    code_unregister_vcpu(v);
    struct um_run observed = {0};
    CHECK(!um_compare(&snap, ref, &observed, v, &out, budget, "two physical pages"));
    um_snapshot_free(&snap);

#if IA64VMM_TIER1_STATS
    /* A successor-cache miss relearns the edge through the real upper API.
     * A masked IRR then rejects first native edges without delivering it. */
    code_register_vcpu(v);
    out = native_pages_run(v, head, link, 600);
    CHECK(out.reason == VCPU_EXIT_BUDGET && tv->fast.nnodes);
    uint64_t learned = tv->stats->native_discard_learned;
    memset(tv->successors, 0, sizeof(tv->successors));
    out = native_pages_run(v, head, link, 600);
    CHECK(out.reason == VCPU_EXIT_BUDGET && tv->stats->native_discard_learned > learned);
    uint64_t failed = tv->stats->native_discard_failed;
    uint64_t irr = v->s.cr[CR_IRR0], psr = v->s.psr;
    v->s.psr &= ~IA64_PSR_I;
    v->s.cr[CR_IRR0] |= 1;
    out = native_pages_run(v, head, link, 600);
    CHECK(out.reason == VCPU_EXIT_BUDGET && tv->stats->native_discard_failed > failed);
    v->s.cr[CR_IRR0] = irr;
    v->s.psr = psr;
    code_unregister_vcpu(v);
#endif

    /* Continue from the same state. A real write to the nonentry page
     * retires a running region while native edges hold the epoch pin. */
    um_snapshot_take(&snap, v);
    code_register_vcpu(v);
    memset(&native_pages_writer, 0, sizeof(native_pages_writer));
    native_pages_writer.tv = tv;
    native_pages_writer.completed = atomic_load_explicit(&tv->fast.completed, memory_order_acquire);
    pthread_t peer;
    CHECK(!pthread_create(&peer, NULL, native_pages_peer, NULL));
    while (!__atomic_load_n(&native_pages_writer.ready, __ATOMIC_ACQUIRE))
        sched_yield();
    out = native_pages_run(v, head, link, budget);
    __atomic_store_n(&native_pages_writer.stop, true, __ATOMIC_RELEASE);
    CHECK(!pthread_join(peer, NULL));
    CHECK(native_pages_writer.changed && !t1_region_from_link(native_pages_writer.old_link));
    code_unregister_vcpu(v);
    memcpy(snap.ram + native_pages_writer.pa - UM_RAM_BASE, &native_pages_writer.after, 8);
    CHECK(!um_compare(&snap, ref, &observed, v, &out, budget, "two pages: nonentry source write"));
    um_snapshot_free(&snap);
    /* Recompile the invalidated source and rebuild learned links. A new
     * native plan must admit the changed second-page snapshot. */
    uint64_t resume = v->s.ip;
    for (unsigned k = 0; k < 2; ++k) {
        v->s.ip = head[k];
        struct t1_compile_report report;
        CHECK(t1_compile(tv, &(struct t1_compile_options){0}, &report) == T1_COMPILED);
        const struct t1_region *r = t1_region_get(report.region_id);
        CHECK(r && r->npages == 2 && r->link != native_pages_writer.old_link);
        link[k] = r->link;
        for (unsigned p = 0; p < r->npoints; ++p)
            if (r->point[p].kind == T1_PT_EXIT && r->point[p].ip == head[1 - k])
                point[k] = p;
    }
    for (unsigned k = 0; k < 2; ++k)
        tv->successors[t1_successor_index(link[k], point[k])] =
            (struct t1_successor){link[k], link[1 - k], point[k]};
    v->s.ip = resume;
    um_snapshot_take(&snap, v);
    uint64_t completed = atomic_load_explicit(&tv->fast.completed, memory_order_acquire);
    code_register_vcpu(v);
    out = native_pages_run(v, head, link, budget);
    CHECK(tv->fast.completed > completed + 32);
    code_unregister_vcpu(v);
    CHECK(!um_compare(&snap, ref, &observed, v, &out, budget, "two pages: replacement admission"));
    um_snapshot_free(&snap);
    v->upper = NULL;
    t1_vcpu_destroy(tv);
    hostwin_destroy(hw);
    vcpu_destroy(v);
    vcpu_destroy(ref);
    printf("unit_runtime: two-page native entries: admitted nonentry page, counted hops, exact budget and peer source write\n");
}
