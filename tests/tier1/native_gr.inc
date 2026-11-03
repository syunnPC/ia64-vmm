/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NaT restoration stores the low bit, including for noncanonical constants.
 * Test both the generated leaf and the C table with an independent oracle. */
static void native_gr_restore_parity(void)
{
    static const uint64_t values[] = {0, 1, 2, 3, UINT64_C(1) << 63,
        (UINT64_C(1) << 63) | 1, UINT64_MAX - 1, UINT64_MAX};
    enum { N = 16 };
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    CHECK(ir != NULL);
    ir->npoints = 1;
    ir->nuses = ir->point[0].frame_count = N;
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m != NULL);
    m->nframe = N;
    m->point[0] = (struct t1_mir_point){0, T1_MIR_NONE, 0, N, T1_PT_EXIT};
    struct t1_frame_entry frame[N];
    for (unsigned k = 0; k < N; ++k) {
        struct t1_loc loc = {T1_LOC_CONST, 0, 0, values[k % 8]};
        frame[k] = t1_frame_entry_make(T1_ELEMENT_NAT(k < 8 ? k + 1 : k + 88), &loc);
        m->frame[k].element = frame[k].element;
        m->frame_loc[k] = loc;
    }
    uint8_t *code = hal_alloc_pages(1, HAL_PAGE_EXEC);
    CHECK(code != NULL);
    struct t1_emit_point ep = {0};
    struct t1_emitted out = {.buf = code, .size = 1, .cap = 4096, .pt = &ep};
    t1_restore_emit(m, &out);
    CHECK(ep.restore_offset != 0);
    hal_code_set_executable(code, 1);
    struct t1_region r = {.code = code, .frame = frame};
    struct t1_point p = {.kind = T1_PT_EXIT, .frame_count = N, .frame_restore = N,
                         .plan = {0, 0, 8, N, N, N, N}, .ip = UM_CODE + 16};
    static struct vcpu v, want;
    static struct t1_exec x;
    for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof)
        for (unsigned leaf = 0; leaf < 2; ++leaf) {
            memset(&v, 0, sizeof(v));
            memset(&x, 0, sizeof(x));
            for (unsigned k = 0; k < 128; ++k)
                v.s.nat[k] = (k & 1) != 0;
            want = v;
            for (unsigned k = 0; k < N; ++k)
                want.s.nat[t1t_phys(bof, frame[k].element - 128)] = values[k % 8] & 1;
            want.s.ip = p.ip;
            x.region = &r;
            x.v = &v;
            x.entry_bof = bof;
            p.restore_offset = leaf ? ep.restore_offset : 0;
            t1_materialize(&x, &p);
            CHECK(!memcmp(&v, &want, sizeof(v)));
        }
    hal_free_pages(code, 1);
    t1_mir_destroy(m);
    free(ir);
}

/* Two incoming edges require a union of residual checks. Metadata fixtures
 * borrow a validated dependency; their synthetic bodies never execute. */
static void native_gr_plan_proof(struct t1_vcpu *tv, const struct t1_region *donor)
{
    static const unsigned regs[] = {12, 32, 63, 64, 127};
    for (unsigned gi = 0; gi < sizeof(regs) / sizeof(regs[0]); ++gi)
        for (unsigned test = 0; test < 68; ++test) {
            unsigned gr = regs[gi], change = test % 17, writer = (test / 17) % 2 ? 0 : 2;
            bool self = test >= 34;
            uint64_t bit = UINT64_C(1) << (gr % 64);
            struct t1_region *r[3];
            struct t1_point point[3][T1_FAST_POINTS] = {{{0}}};
            struct t1_frame_entry frame[3] = {{0}};
            memset(&tv->fast, 0, sizeof(tv->fast));
            memset(tv->successors, 0, sizeof(tv->successors));
            bool used[T1_SUCCESSORS] = {0};
            for (unsigned k = 0; k < 3; ++k) {
                r[k] = t1_region_new();
                CHECK(r[k] != NULL);
                r[k]->c = donor->c;
                r[k]->c.ip = r[k]->c.entry_pa = donor->page[0].pa + 16 * k;
                r[k]->npages = 1;
                r[k]->dep[0].page = donor->dep[0].page;
                memcpy(r[k]->dep[0].mask, donor->dep[0].mask, sizeof(r[k]->dep[0].mask));
                r[k]->valid = r[k]->sealed = true;
                r[k]->point = point[k];
                r[k]->max_path = 3;
                r[k]->nat_guard[0] = UINT64_C(1) << 11;
                r[k]->nat_guard[1] = UINT64_C(1) << 62;
                r[k]->nat_guard[gr / 64] |= bit;
            }
            if (change >= 14)
                r[writer]->nat_guard[gr / 64] &= ~bit;
            for (unsigned k = 0; k < 3; ++k)
                t1_nat_guard_list(r[k]);
            frame[0].element = T1_ELEMENT_NAT(gr);
            frame[0].kind = T1_LOC_CONST;
            frame[0].value = 1;
            frame[1].element = change == 2 ? T1_ELEMENT_GR(gr) : T1_ELEMENT_NAT(gr);
            frame[1].kind = T1_LOC_CONST;
            if (change == 1)
                frame[1].kind = T1_LOC_HOME;
            if (change == 3)
                frame[1].element = T1_ELEMENT_NAT(gr ^ 1);
            if (change >= 4 && change <= 9)
                frame[1].value = change < 8 ? change - 4 :
                    (UINT64_C(1) << 63) | (change - 8);
            if (change >= 10 && change <= 13)
                frame[1].kind = T1_LOC_REG + change - 10;
            if (change == 16)
                frame[1].value = 2;
            frame[2] = frame[0]; /* Outside the point's frame. */
            r[writer]->frame = frame;
            for (unsigned k = 0; k < 3; ++k)
                for (unsigned edge = 0; edge < (k ? 1 : 2); ++edge) {
                    unsigned to = !k ? edge + 1 : k == 1 ? (self ? 1 : 0) : 1;
                    unsigned p = 0;
                    while (p < T1_FAST_POINTS && used[t1_successor_index(r[k]->link, p)])
                        ++p;
                    CHECK(p < T1_FAST_POINTS);
                    unsigned slot = t1_successor_index(r[k]->link, p);
                    used[slot] = true;
                    point[k][p] = (struct t1_point){.kind = T1_PT_EXIT, .ip = r[to]->c.ip,
                                                   .restore_offset = 1};
                    if (k == writer) {
                        point[k][p].frame_first = 1;
                        point[k][p].frame_count = change != 0 && change != 14;
                        point[k][p].frame_restore = change == 1 ? 0 : point[k][p].frame_count;
                    }
                    r[k]->npoints = p + 1;
                    r[k]->fast_points |= UINT32_C(1) << p;
                    tv->successors[slot] = (struct t1_successor){r[k]->link, r[to]->link, p};
                }
            t1_fast_prepare(tv, r[0]);
            CHECK(tv->x.fast && tv->fast.nnodes == 3);
            for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof) {
                tv->x.entry_bof = tv->v->s.rse_bof = bof;
                t1_fast_prepare(tv, r[bof % 3]);
                CHECK(tv->x.fast && tv->x.fast->region == r[bof % 3]);
                for (unsigned k = 0; k < 3; ++k) {
                    struct t1_fast_node *n = &tv->fast.node[k];
                    bool required = change == 5 || change == 7 || change == 9 ||
                                    (change >= 10 && change <= 14);
                    unsigned needed = (n->region == r[1] || (!writer && n->region == r[2])) && required;
                    if (self && n->region == r[0]) {
                        CHECK(n->nguard == r[0]->nguard);
                        for (unsigned j = 0; j < n->nguard; ++j) {
                            unsigned id = j < r[0]->nguard_static ? r[0]->guard[j] : 32 + r[0]->guard[j];
                            CHECK(n->guard[j] == offsetof(struct vcpu, s.nat) + t1t_phys(bof, id));
                        }
                        continue;
                    }
                    CHECK(n->nguard == needed);
                    if (needed)
                        CHECK(n->guard[0] == offsetof(struct vcpu, s.nat) + t1t_phys(bof, gr));
                }
            }
            r[0]->nhelper = 1;
            t1_fast_prepare(tv, r[0]);
            CHECK(!tv->x.fast);
            r[0]->nhelper = 0;
            tv->x.fast = NULL;
            memset(&tv->fast, 0, sizeof(tv->fast));
            memset(tv->successors, 0, sizeof(tv->successors));
            for (unsigned k = 0; k < 3; ++k) {
                r[k]->frame = NULL;
                r[k]->valid = false;
                t1_region_discard(r[k]);
            }
        }
}

/* SDM Vol.3: ld clears its destination NaT; packed add propagates its
 * sources' bits; a false qualifying predicate preserves the destination.
 * The next region consumes that destination as a load address. */
static void native_gr_restored(void)
{
    unsigned cases = 0;
    const uint64_t head[2] = {UM_CODE + 0xf0000, UM_CODE + 0xf8000};
    for (unsigned kind = 0; kind < 4; ++kind) {
        unsigned qp = kind & 1 ? 7 : 0;
        const struct ienc_bundle source[] = {
            {IENC_STOP(IENC_MII),
             {kind < 2 ? ienc_ld(qp, 8, false, 33, 34) : ienc_packed(qp, false, 32, 0, 33, 35, 36),
              ienc_adds(0, 14, 1, 14), ienc_nop_i()}},
            {IENC_STOP(IENC_MIB),
             {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, head[1] - head[0] - 16)}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, -32)}}};
        const struct ienc_bundle target[] = {
            {IENC_STOP(IENC_MII),
             {ienc_ld(0, 8, false, 16, 33), ienc_adds(0, 15, 1, 15), ienc_nop_i()}},
            {IENC_STOP(IENC_MIB),
             {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, head[0] - head[1] - 16)}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, -32)}}};
        um_emit(head[0], source, 3);
        um_emit(head[1], target, 3);
        struct vcpu *v = um_vcpu(head[0]), *ref = um_vcpu(head[0]);
        v->cfg.itc_icount = false;
        v->s.cfm = um_cfm(24, 24, 16, 5, 0);
        v->s.rse_bof = 95;
        uint64_t address = UM_DATA + 0xe00;
        memcpy(um_ram(UM_DATA + 0xd00), &address, 8);
        code_notify_vmm_write(UM_DATA + 0xd00, 8);
        um_gr(v, 33, address, false);
        um_gr(v, 34, UM_DATA + 0xd00, false);
        um_gr(v, 35, address, false);
        um_gr(v, 36, 0, false);
        um_pr(v, 6, true);
        um_pr(v, 7, true);
        struct hostwin_config cfg = {40, 1, false};
        struct hostwin_vcpu *hw = hostwin_create(&cfg, hostwin_linux_backend(), hostwin_linux_ctx());
        CHECK(hw != NULL);
        struct t1_vcpu *tv = t1_vcpu_create(v, hw);
        CHECK(tv != NULL);
        v->upper = tv;
        t1_test_set_compile_options(tv, &(struct t1_compile_options){0});
        uint64_t link[2];
        const struct t1_region *region[2];
        uint32_t point[2] = {UINT32_MAX, UINT32_MAX};
        unsigned id = t1t_element_reg(&v->s, 33);
        for (unsigned k = 0; k < 2; ++k) {
            v->s.ip = head[k];
            struct t1_compile_report report;
            CHECK(t1_compile(tv, &(struct t1_compile_options){0}, &report) == T1_COMPILED);
            region[k] = t1_region_get(report.region_id);
            CHECK(region[k] && region[k]->fast_points && !region[k]->nhelper);
            link[k] = region[k]->link;
            for (unsigned p = 0; p < region[k]->npoints; ++p)
                if (region[k]->point[p].kind == T1_PT_EXIT && region[k]->point[p].ip == head[1 - k])
                    point[k] = p;
            CHECK(point[k] != UINT32_MAX);
        }
        CHECK(region[1]->nat_guard[id / 64] & (UINT64_C(1) << (id % 64)));
        v->s.ip = head[0];
        code_register_vcpu(v);
        struct vcpu initial = *v;
        for (unsigned test = 0; test < 14; ++test, ++cases) {
            *v = initial;
            t1_vcpu_state_restored(tv);
            for (unsigned k = 0; k < 2; ++k)
                tv->successors[t1_successor_index(link[k], point[k])] =
                    (struct t1_successor){link[k], link[1 - k], point[k]};
            struct vcpu_exit out = native_pages_run(v, head, link, 1200);
            CHECK(out.reason == VCPU_EXIT_BUDGET && tv->fast.completed > 32);
            for (unsigned k = 0; k < tv->fast.nnodes; ++k)
                if (tv->fast.node[k].region == region[1])
                    CHECK(((tv->fast.node[k].nat_guard[id / 64] >> (id % 64)) & 1) == (kind >= 2));
            CHECK(v->s.ip == head[0] && !v->s.slot);
            bool pred = test >= 8 || (test & 1), old_nat = test < 8 && (test & 2),
                 input_nat = test >= 8 || (test & 4);
            um_pr(v, 7, pred);
            um_gr(v, 33, address, old_nat);
            um_gr(v, 35, address, input_nat);
            static const unsigned budgets[] = {5, 6, 7, 11, 12, 13};
            uint64_t budget = test >= 8 ? budgets[test - 8] : 120;
            struct um_snapshot snap;
            um_snapshot_take(&snap, v);
            uint64_t before = tv->fast.completed, entries = region[0]->entries;
            out = native_pages_run(v, head, link, budget);
            struct um_run observed = {0};
            CHECK(!um_compare(&snap, ref, &observed, v, &out, budget,
                              "native GR restoration, predicate, NaT and exact budget"));
            bool blocked = qp && !pred ? old_nat : kind >= 2 && input_nat;
            if (blocked)
                CHECK(tv->fast.completed == before);
            else if (budget == 120)
                CHECK(tv->fast.completed > before);
            if (kind >= 2 && pred && input_nat && budget >= region[0]->max_path)
                CHECK(region[0]->entries > entries);
            um_snapshot_free(&snap);
        }
        code_unregister_vcpu(v);
        v->upper = NULL;
        t1_vcpu_destroy(tv);
        hostwin_destroy(hw);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    printf("unit_runtime: native GR restored NaT: %u exact load/packed/predicate/fault/budget cases\n", cases);
}

void native_gr_test(void);
void native_gr_test(void)
{
    umt_init();
    native_gr_restore_parity();
    const uint64_t head[2] = {UM_CODE + 0xe0000, UM_CODE + 0xe8000};
    for (unsigned k = 0; k < 2; ++k) {
        const struct ienc_bundle code[] = {
            {IENC_STOP(IENC_MII),
             {ienc_ld(0, 8, false, 16, 33), ienc_adds(0, 14 + k, 1, 14 + k), ienc_nop_i()}},
            {IENC_STOP(IENC_MIB),
             {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, head[1 - k] - head[k] - 16)}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, -32)}}};
        um_emit(head[k], code, 3);
    }
    struct vcpu *v = um_vcpu(head[0]), *ref = um_vcpu(head[0]);
    v->cfg.itc_icount = false;
    v->s.cfm = um_cfm(24, 24, 16, 5, 0);
    um_gr(v, 33, UM_DATA + 0xc00, false);
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
    const struct t1_region *region[2];
    for (unsigned k = 0; k < 2; ++k) {
        v->s.ip = head[k];
        struct t1_compile_report report;
        CHECK(t1_compile(tv, &(struct t1_compile_options){0}, &report) == T1_COMPILED);
        region[k] = t1_region_get(report.region_id);
        CHECK(region[k] && region[k]->fast_points && !region[k]->nhelper && region[k]->nguard);
        link[k] = region[k]->link;
    }
    for (unsigned k = 0; k < 2; ++k)
        for (unsigned p = 0; p < region[k]->npoints; ++p)
            if (region[k]->point[p].kind == T1_PT_EXIT && region[k]->point[p].ip == head[1 - k])
                tv->successors[t1_successor_index(link[k], p)] =
                    (struct t1_successor){link[k], link[1 - k], p};
    v->s.ip = head[0];
    code_register_vcpu(v);
    struct t1_stats before_retry, after_retry;
    t1_get_stats(&before_retry);
    struct um_snapshot snap;
    um_snapshot_take(&snap, v);
    struct vcpu_exit out = native_pages_run(v, head, link, 1200);
    struct um_run observed = {0};
    CHECK(!um_compare(&snap, ref, &observed, v, &out, 1200, "native GR unchanged admission proof"));
    CHECK(tv->fast.completed > 32 && tv->fast.nnodes == 2);
    t1_get_stats(&after_retry);
    if (IA64VMM_TIER1_STATS)
        CHECK(after_retry.window_misses > before_retry.window_misses &&
              after_retry.trap_fills > before_retry.trap_fills);
    for (unsigned k = 0; k < 2; ++k)
        CHECK(tv->fast.node[k].nguard == 0);
    um_snapshot_free(&snap);
    struct vcpu healthy = *v;
    for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof) {
        t1t_rebase_bof(&v->s, bof);
        v->s.ip = head[bof % 2];
        v->s.slot = 0;
        uint64_t before = tv->fast.completed;
        um_snapshot_take(&snap, v);
        out = native_pages_run(v, head, link, 120);
        CHECK(!um_compare(&snap, ref, &observed, v, &out, 120, "native GR rotating BOF reuse"));
        CHECK(tv->fast.completed > before && tv->fast.bof == bof);
        um_snapshot_free(&snap);
    }
    for (unsigned first = 0; first < 2; ++first) {
        *v = healthy;
        v->s.ip = head[first];
        v->s.slot = 0;
        v->s.nat[ia64_gr_index(&v->s, 33)] = true;
        uint64_t before = tv->fast.completed;
        um_snapshot_take(&snap, v);
        out = native_pages_run(v, head, link, 120);
        CHECK(!um_compare(&snap, ref, &observed, v, &out, 120, "native GR full C admission"));
        CHECK(tv->fast.completed == before);
        um_snapshot_free(&snap);
    }
    *v = healthy;
    t1_vcpu_state_restored(tv);
    const struct code_fetch fetch = {.psr = v->s.psr};
    CHECK(code_dependencies_enter(v, (struct code_dependency *)region[0]->dep, 1, &fetch));
    hostwin_enter(hw, v);
    tv->x.observed_epoch = hostwin_bind_epoch(hw);
    native_gr_plan_proof(tv, region[0]);
    code_unregister_vcpu(v);
    v->upper = NULL;
    t1_vcpu_destroy(tv);
    hostwin_destroy(hw);
    vcpu_destroy(v);
    vcpu_destroy(ref);
    native_gr_restored();
    printf("unit_runtime: native GR proof: restore parity, cyclic execution, full C admission, 340 incoming/self-edge graphs at every BOF\n");
}
