/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Vol.1 section 5.3: NaTVal has sign 0, exponent 0x1fffe and
 * significand 0. SDM Vol.3 stf: consumption faults before the store. */
static uint64_t native_fr_store(unsigned qp, unsigned f, unsigned base)
{
    uint64_t raw = (UINT64_C(6) << 37) | (UINT64_C(0x31) << 30) |
                   ((uint64_t)base << 20) | ((uint64_t)f << 13) | qp;
    Ia64Instruction in = ia64_decode_insn(IA64_UNIT_M, raw, 0, 0);
    CHECK(in.valid && in.opcode == IA64_OP_STF8 && in.qp == qp);
    CHECK(in.operands.memory.source == f && in.operands.memory.base == base);
    return raw;
}

static uint64_t native_fr_merge(unsigned qp, unsigned f, unsigned source)
{
    uint64_t raw = (UINT64_C(0x10) << 27) | ((uint64_t)source << 20) |
                   ((uint64_t)source << 13) | ((uint64_t)f << 6) | qp;
    Ia64Instruction in = ia64_decode_insn(IA64_UNIT_F, raw, 0, 1);
    CHECK(in.valid && in.opcode == IA64_OP_FMERGE_S && in.qp == qp);
    CHECK(in.operands.floating.destination == f && in.operands.floating.source1 == source &&
          in.operands.floating.source2 == source);
    return raw;
}

static uint64_t native_fr_get(unsigned qp, unsigned r, unsigned f)
{
    uint64_t raw = (UINT64_C(4) << 37) | (UINT64_C(0xe1) << 27) |
                   ((uint64_t)f << 13) | ((uint64_t)r << 6) | qp;
    Ia64Instruction in = ia64_decode_insn(IA64_UNIT_M, raw, 0, 0);
    CHECK(in.valid && in.opcode == IA64_OP_GETF_SIG && in.qp == qp);
    return raw;
}

/* Metadata fixtures share a validated page but never execute their bodies.
 * The target has two incoming edges, only one of which may change its FR. */
static void native_fr_plan_proof(struct t1_vcpu *tv, const struct t1_region *donor, unsigned fr)
{
    for (unsigned test = 0; test < 16; ++test) {
        unsigned change = test % 8, writer = test < 8 ? 2 : 0;
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
            r[k]->fr_guard[fr / 64] = UINT64_C(1) << (fr % 64);
        }
        if (change == 5)
            r[writer]->fr_guard[fr / 64] = 0;
        unsigned changed = change == 3 || change == 4 ? fr ^ 1 : fr;
        frame[0].element = T1_ELEMENT_FR(fr ^ 1);
        frame[1].element = change == 2 || change == 4 ? T1_ELEMENT_FR_META(changed)
                                                     : T1_ELEMENT_FR(changed);
        frame[1].kind = change == 6 ? T1_LOC_HOME : T1_LOC_CONST;
        frame[2].element = T1_ELEMENT_FR_META(fr);
        frame[2].kind = T1_LOC_CONST;
        r[writer]->frame = frame;
        for (unsigned k = 0; k < 3; ++k)
            for (unsigned edge = 0; edge < (k ? 1 : 2); ++edge) {
                unsigned to = !k ? edge + 1 : k == 1 ? 0 : 1;
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
                    point[k][p].frame_count = change == 7 ? 2 : change != 0 && change != 5;
                    point[k][p].frame_restore = change == 6 ? 0 : point[k][p].frame_count;
                }
                r[k]->npoints = p + 1;
                r[k]->fast_points |= UINT32_C(1) << p;
                tv->successors[slot] = (struct t1_successor){r[k]->link, r[to]->link, p};
            }
        t1_fast_prepare(tv, r[0]);
        CHECK(tv->x.fast && tv->fast.nnodes == 3);
        for (unsigned k = 0; k < 3; ++k) {
            struct t1_fast_node *n = &tv->fast.node[k];
            unsigned needed = (n->region == r[1] || (!writer && n->region == r[2])) &&
                              (change == 1 || change == 2 || change == 5 || change == 7);
            CHECK(n->nfr_guard == needed);
            if (needed)
                CHECK(n->fr_guard[0] == offsetof(struct vcpu, s.fr) + fr * sizeof(struct ia64_fr));
        }
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

static void native_fr_reuse(void)
{
    const unsigned reg[] = {6, 90, 127};
    for (unsigned kind = 0; kind < 6; ++kind) {
        unsigned fr = reg[kind % 3];
        const uint64_t head[2] = {UM_CODE + 0xd0000, UM_CODE + 0xd8000};
        for (unsigned k = 0; k < 2; ++k) {
            const struct ienc_bundle code[] = {
                {IENC_STOP(IENC_MII),
                 {native_fr_store(0, fr, 33), ienc_adds(0, 14 + k, 1, 14 + k), ienc_nop_i()}},
                {0x0d, {ienc_nop_m(), kind >= 3 ? native_fr_merge(7, 10, 9) : (UINT64_C(1) << 27),
                        ienc_nop_i()}},
                {IENC_STOP(IENC_MIB),
                 {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, head[1 - k] - head[k] - 32)}},
                {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, -48)}}};
            um_emit(head[k], code, 4);
        }
        struct vcpu *v = um_vcpu(head[0]), *ref = um_vcpu(head[0]);
        v->cfg.itc_icount = false;
        v->s.psr |= IA64_PSR_MFL | IA64_PSR_MFH;
        if (fr == 127)
            v->s.cfm |= UINT64_C(7) << 25;
        unsigned physical = ia64_fr_index(&v->s, fr);
        CHECK(fr != 127 || physical != fr);
        v->s.fr[physical] = (struct ia64_fr){42, IA64_FP_INTEGER_EXP, false};
        v->s.fr[9] = (struct ia64_fr){53, IA64_FP_INTEGER_EXP, false};
        um_gr(v, 33, UM_DATA + 0xb00, false);
        um_pr(v, 6, true);
        um_pr(v, 7, true);
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
            CHECK(region[k] && region[k]->fast_points && !region[k]->nhelper);
            CHECK(region[k]->fr_guard[physical / 64] == (UINT64_C(1) << (physical % 64)));
            link[k] = region[k]->link;
        }
        for (unsigned k = 0; k < 2; ++k) {
            bool found = false;
            for (unsigned p = 0; p < region[k]->npoints; ++p)
                if (region[k]->point[p].kind == T1_PT_EXIT && region[k]->point[p].ip == head[1 - k]) {
                    tv->successors[t1_successor_index(link[k], p)] =
                        (struct t1_successor){link[k], link[1 - k], p};
                    found = true;
                }
            CHECK(found);
        }
        v->s.ip = head[0];
        code_register_vcpu(v);
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        struct vcpu_exit out = native_pages_run(v, head, link, 1200);
        struct um_run observed = {0};
        CHECK(!um_compare(&snap, ref, &observed, v, &out, 1200, "native FR unchanged admission proof"));
        CHECK(tv->fast.completed > 32 && tv->fast.nnodes == 2);
        for (unsigned k = 0; k < 2; ++k)
            CHECK(tv->fast.node[k].nfr_guard == 0);
        um_snapshot_free(&snap);
        uint64_t before = tv->fast.completed;
        struct vcpu healthy = *v;
        v->s.ip = head[0];
        v->s.slot = 0;
        v->s.fr[physical] = ia64_fp_nat();
        um_snapshot_take(&snap, v);
        out = native_pages_run(v, head, link, 120);
        CHECK(!um_compare(&snap, ref, &observed, v, &out, 120,
                          "native FR ordinary admission after reuse"));
        CHECK(tv->fast.completed == before);
        um_snapshot_free(&snap);
        *v = healthy;
        t1_vcpu_state_restored(tv);
        const struct code_fetch fetch = {.psr = v->s.psr};
        CHECK(code_dependencies_enter(v, (struct code_dependency *)region[0]->dep, 1, &fetch));
        hostwin_enter(hw, v);
        tv->x.observed_epoch = hostwin_bind_epoch(hw);
        native_fr_plan_proof(tv, region[0], physical);
        code_unregister_vcpu(v);
        v->upper = NULL;
        t1_vcpu_destroy(tv);
        hostwin_destroy(hw);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    printf("unit_runtime: native FR proof reuse: 6 execution and 96 incoming-edge/component cases\n");
}

void native_fr_test(void);
void native_fr_test(void)
{
    umt_init();
    native_fr_reuse();
    unsigned cases = 0;
    const unsigned freg[] = {6, 90, 127, 6};
    for (unsigned kind = 0; kind < 4; ++kind) {
        const uint64_t head[2] = {UM_CODE + 0xc0000, UM_CODE + 0xc8000};
        const struct ienc_bundle source[] = {
            {0x0d,
             {ienc_nop_m(), native_fr_merge(7, freg[kind], 9), ienc_adds(0, 14, 1, 14)}},
            {IENC_STOP(IENC_MIB),
             {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, head[1] - head[0] - 16)}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, -32)}}};
        const struct ienc_bundle target[] = {
            {IENC_STOP(IENC_MII),
             {native_fr_store(8, freg[kind], 33), ienc_adds(0, 15, 1, 15), ienc_nop_i()}},
            {IENC_STOP(IENC_MIB),
             {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(6, head[0] - head[1] - 16)}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, -32)}}};
        const struct ienc_bundle two_guards[] = {
            {IENC_STOP(IENC_MII), {native_fr_get(8, 16, 6), ienc_nop_i(), ienc_nop_i()}},
            {IENC_STOP(IENC_MII),
             {native_fr_get(8, 17, 90), ienc_alu(0, IENC_ADD, 18, 16, 17), ienc_nop_i()}},
            {IENC_STOP(IENC_MIB),
             {ienc_ld(0, 8, false, 19, 18), ienc_adds(0, 15, 1, 15),
              ienc_br_cond(6, head[0] - head[1] - 32)}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, -48)}}};
        um_emit(head[0], source, 3);
        if (kind == 3)
            um_emit(head[1], two_guards, 4);
        else
            um_emit(head[1], target, 3);
        struct vcpu *v = um_vcpu(head[0]), *ref = um_vcpu(head[0]);
        v->cfg.itc_icount = false;
        v->s.psr |= IA64_PSR_MFL | IA64_PSR_MFH;
        if (kind == 2)
            v->s.cfm |= UINT64_C(7) << 25;
        unsigned physical = ia64_fr_index(&v->s, freg[kind]);
        CHECK(kind != 2 || physical != freg[kind]);
        v->s.fr[9] = (struct ia64_fr){kind == 3 ? UM_DATA + 0xb00 : UINT64_C(0x123456789abcdef0),
                                       IA64_FP_INTEGER_EXP, false};
        um_gr(v, 33, UM_DATA + 0xb00, false);
        um_pr(v, 6, true);
        um_pr(v, 7, true);
        um_pr(v, 8, true);
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
            CHECK(r && r->npages == 1 && r->fast_points && !r->nhelper);
            if (k) {
                CHECK(r->fr_guard[physical / 64] & (UINT64_C(1) << (physical % 64)));
                if (kind == 3)
                    CHECK(r->fr_guard[1] & (UINT64_C(1) << (90 - 64)));
            }
            else
                CHECK(!r->fr_guard[0] && !r->fr_guard[1] && !r->nat_guard[0] && !r->nat_guard[1]);
            link[k] = r->link;
            for (unsigned p = 0; p < r->npoints; ++p)
                if (r->point[p].kind == T1_PT_EXIT && r->point[p].ip == head[1 - k])
                    point[k] = p;
            CHECK(point[k] != UINT32_MAX);
        }
        for (unsigned k = 0; k < 2; ++k)
            tv->successors[t1_successor_index(link[k], point[k])] =
                (struct t1_successor){link[k], link[1 - k], point[k]};
        v->s.ip = head[0];
        code_register_vcpu(v);
        struct vcpu initial = *v;
        for (unsigned iteration = 0; iteration < 15; ++iteration, ++cases) {
            unsigned test = (iteration + 7) % 15;
            *v = initial;
            t1_vcpu_state_restored(tv);
            for (unsigned k = 0; k < 2; ++k)
                tv->successors[t1_successor_index(link[k], point[k])] =
                    (struct t1_successor){link[k], link[1 - k], point[k]};
            uint64_t before = tv->fast.completed;
            struct vcpu_exit out = native_pages_run(v, head, link, 1200);
            CHECK(out.reason == VCPU_EXIT_BUDGET && tv->fast.completed > before + 32);
            bool source_pred = test < 8 && (test & 1), target_pred = test >= 8 || (test & 2),
                 nat = test < 8 && (test & 4), blocked = source_pred && nat;
            um_pr(v, 7, source_pred);
            um_pr(v, 8, target_pred);
            if (nat)
                v->s.fr[9] = ia64_fp_nat();
            if (test >= 8 && test < 14) {
                struct ia64_fr value = ia64_fp_nat();
                if (test == 9) value.sign = true;
                if (test == 10) ++value.exponent;
                if (test == 11) --value.exponent;
                if (test == 12) value.significand = 1;
                if (test == 13) value.significand = UINT64_MAX;
                v->s.fr[physical] = value;
                blocked = test == 8;
            }
            if (test == 14) {
                v->s.fr[kind == 3 ? 90 : physical ^ 1] = ia64_fp_nat();
                blocked = kind == 3;
            }
            struct um_snapshot snap;
            um_snapshot_take(&snap, v);
            before = tv->fast.completed;
            out = native_pages_run(v, head, link, 120);
            struct um_run observed = {0};
            CHECK(!um_compare(&snap, ref, &observed, v, &out, 120,
                              "native FR NaT guard after source restoration"));
            CHECK(blocked ? tv->fast.completed == before
                                     : tv->fast.completed > before);
            um_snapshot_free(&snap);
        }
        code_unregister_vcpu(v);
        v->upper = NULL;
        t1_vcpu_destroy(tv);
        hostwin_destroy(hw);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    printf("unit_runtime: native FR guards: %u exact source/predicate/NaT/bank/rotation cases\n", cases);
}
