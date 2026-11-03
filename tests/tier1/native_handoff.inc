/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A native successor inherits exactly one SysV save frame. Its entry
 * reloads LIVEINs from the current vCPU and receives the new R13 budget.
 * A point captures only registers required by its recovery frame. */
struct native_handoff_test {
    uintptr_t body, stack;
    unsigned remaining, calls;
};

uint32_t t1_native_test_enter(t1_entry_fn entry, struct vcpu *v, uint64_t budget, struct t1_exec *x);
__asm__(
    ".text\n"
    ".globl t1_native_test_enter\n"
    "t1_native_test_enter:\n"
    "push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
    "sub $8, %rsp\n"
    "mov %rdi, %rax\n mov %rsi, %rdi\n mov %rdx, %rsi\n mov %rcx, %rdx\n"
    "movabs $0x123456789abcdef0, %rbx\n movabs $0xfedcba9876543210, %rbp\n"
    "movabs $0x1122334455667788, %r12\n movabs $0x8877665544332211, %r13\n"
    "movabs $0xa1b2c3d4e5f60718, %r14\n movabs $0x1827364554637281, %r15\n"
    "call *%rax\n"
    "cmp $1, %eax\n jne 1f\n"
    "movabs $0x123456789abcdef0, %r10\n cmp %r10, %rbx\n jne 1f\n"
    "movabs $0xfedcba9876543210, %r10\n cmp %r10, %rbp\n jne 1f\n"
    "movabs $0x1122334455667788, %r10\n cmp %r10, %r12\n jne 1f\n"
    "movabs $0x8877665544332211, %r10\n cmp %r10, %r13\n jne 1f\n"
    "movabs $0xa1b2c3d4e5f60718, %r10\n cmp %r10, %r14\n jne 1f\n"
    "movabs $0x1827364554637281, %r10\n cmp %r10, %r15\n jne 1f\n"
    "mov $1, %eax\n jmp 2f\n 1: xor %eax, %eax\n 2:\n"
    "add $8, %rsp\n"
    "pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n ret\n");

/* Synthetic metadata isolates the native guard/commit contract. The body
 * retires three instructions; the callback is reached only after the
 * native lane declines. */
static uintptr_t native_fast_stop(struct t1_exec *x)
{
#if IA64VMM_TIER1_STATS
    t1_fast_count_exit(x->tv);
#endif
    ++*(unsigned *)x->handoff_arg;
    return 0;
}

static struct {
    uintptr_t restore;
    unsigned calls, kick_at;
    _Atomic uint64_t *generation;
} native_budget_restore;

static void native_budget_event(struct t1_exec *x, struct vcpu *v)
{
    ((void (*)(struct t1_exec *, struct vcpu *))native_budget_restore.restore)(x, v);
    if (++native_budget_restore.calls == native_budget_restore.kick_at) {
        if (native_budget_restore.generation)
            ++*native_budget_restore.generation;
        else
            vcpu_kick(v);
    }
}

static void native_fast_test(struct vcpu *v, uint8_t *code, const struct t1_emitted *e)
{
    static struct t1_vcpu tv;
    static struct vcpu saved;
    saved = *v;
    struct t1_point p = {.kind = T1_PT_EXIT, .ip = UM_CODE};
    struct t1_region r[2];
    struct t1_window_slot window[2];
    _Atomic(struct PageValidity *) page;
    _Atomic bool ready;
    _Atomic uint64_t generation, tick, generation2, tick2;
    _Atomic(struct PageValidity *) page2;
    _Atomic bool ready2;
    uint64_t epoch, admission, clock, entered, epoch2, admission2, entered2;
    uint8_t *nat_leaf = t1_arena_alloc(1);
    CHECK(nat_leaf != NULL);
    /* movb $1, nat[12](%rsi); ret: a restoration that makes the target
     * guard fail, after all pre-restoration checks have passed. */
    const uint8_t prefix[] = {0xc6, 0x86};
    memcpy(nat_leaf, prefix, sizeof(prefix));
    uint32_t displacement = offsetof(struct vcpu, s.nat) + 12;
    memcpy(nat_leaf + 2, &displacement, 4);
    nat_leaf[6] = 1;
    nat_leaf[7] = 0xc3;
    /* orq $PSR.mfl, psr(%rsi); ret. */
    const uint8_t mf_prefix[] = {0x48, 0x81, 0x8e};
    memcpy(nat_leaf + 32, mf_prefix, sizeof(mf_prefix));
    displacement = offsetof(struct vcpu, s.psr);
    memcpy(nat_leaf + 35, &displacement, 4);
    uint32_t mf = IA64_PSR_MFL;
    memcpy(nat_leaf + 39, &mf, 4);
    nat_leaf[43] = 0xc3;
    hal_code_set_executable(nat_leaf, 1);
    CHECK(e->pt[0].restore_offset);
    const unsigned expected_bound = 32;
    for (unsigned test = 0; test < 58; ++test) {
        unsigned budget = 3 * (expected_bound + 2);
        memset(&native_budget_restore, 0, sizeof(native_budget_restore));
        memset(&tv, 0, sizeof(tv));
        memset(r, 0, sizeof(r));
        memset(window, 0, sizeof(window));
        memset(v, 0, sizeof(*v));
        ready = true;
        epoch = generation = admission = 7;
        clock = entered = tick = 0;
        page = (struct PageValidity *)(void *)&page;
        page2 = (struct PageValidity *)(void *)&page2;
        ready2 = true;
        epoch2 = generation2 = admission2 = 11;
        entered2 = tick2 = 0;
        v->s.ip = UM_CODE;
        v->s.gr[12] = 42;
        v->retired = 100;
        tv.v = v;
        tv.x.v = v;
        tv.x.tv = &tv;
        tv.x.region = &r[0];
        tv.x.budget_entry = budget;
        tv.x.retired_entry = 100;
        tv.x.fast = &tv.fast.node[0];
        unsigned calls = 0;
        tv.x.handoff = native_fast_stop;
        tv.x.handoff_arg = &calls;
        for (unsigned k = 0; k < 2; ++k) {
            r[k].valid = true;
            r[k].fast_points = 1;
            r[k].npages = 1;
            r[k].link = k + 1;
            r[k].point = &p;
            window[k].link = r[k].link;
            struct t1_fast_node *n = &tv.fast.node[k];
            n->region = &r[k];
            n->entry = (uintptr_t)code + e->native_entry;
            n->max_path = 3;
            n->outgoing = 1;
            n->nguard = 1;
            n->nat_guard[0] = UINT64_C(1) << 12;
            n->guard[0] = offsetof(struct vcpu, s.nat) + 12;
            n->witness = (struct code_stay_witness){
                .owner = v, .page_word = &page, .page = page, .ready_word = &ready,
                .epoch_word = &epoch, .generation_word = &generation,
                .admission_word = &admission, .epoch = epoch, .generation = generation,
                .admission = admission, .clock = &clock, .entered = &entered, .entry_tick = &tick
            };
            n->edge[0] = (struct t1_fast_edge){&tv.fast.node[1], &p,
                                                 (uintptr_t)code + e->pt[0].restore_offset};
        }
        if (test >= 25 && test <= 36) {
            for (unsigned k = 0; k < 2; ++k) {
                struct t1_fast_node *n = &tv.fast.node[k];
                r[k].npages = 2;
                tv.fast.second[k] = (struct code_stay_witness){
                    .owner = v, .page_word = &page2, .page = page2, .ready_word = &ready2,
                    .epoch_word = &epoch2, .generation_word = &generation2,
                    .admission_word = &admission2, .epoch = epoch2, .generation = generation2,
                    .admission = admission2, .clock = &clock, .entered = &entered2,
                    .entry_tick = &tick2
                };
                n->second = &tv.fast.second[k];
            }
        }
        unsigned hops = 0;
        switch (test) {
        case 0: hops = expected_bound; break; /* full batch, then ordinary bridge */
        case 1: /* two accepted edges reach the self-edge fold threshold */
            tv.fast.node[0].window = &window[0];
            tv.fast.node[1].window = &window[1];
            window[1].attempts = 13;
            hops = 2;
            break;
        case 2:
            tv.fast.node[0].window = &window[0];
            window[0].entries = T1_WINDOW_FOLD - 1;
            break;
        case 3:
            tv.fast.node[1].window = &window[1];
            window[1].attempts = T1_WINDOW_FOLD - 1;
            break;
        case 4: r[1].valid = false; break; /* final publication check */
        case 5: tv.fast.node[0].edge[0].restore = (uintptr_t)nat_leaf; break;
        case 6: v->s.cr[CR_IRR0] = 1; break; /* no kick, even masked */
        case 7: ++epoch; break;
        case 8: ++generation; break;
        case 9: ++admission; break;
        case 10: ++v->mmu_seq; break;
        case 11: ++tv.x.observed_epoch; break;
        case 12: tv.x.bsp_called = 1; break;
        case 13: tv.fast.node[0].edge[0].restore = (uintptr_t)nat_leaf + 32; break;
        case 14:
            tv.fast.node[0].edge[0].restore = (uintptr_t)nat_leaf + 32;
            tv.fast.psr = v->s.psr = IA64_PSR_MFL;
            hops = expected_bound;
            break;
        case 15:
            tv.fast.node[1].window = &window[1];
            window[1].attempts = T1_WINDOW_FOLD - 1;
            break;
        case 16: ++admission; break;
        case 17:
            tv.x.entry_bof = v->s.rse_bof = 95;
            v->s.nat[126] = true;
            CHECK(!v->s.nat[127]);
            for (unsigned k = 0; k < 2; ++k) {
                r[k].nguard = 1;
                r[k].guard[0] = 95;
                tv.fast.node[k].nat_guard[0] = 0;
                tv.fast.node[k].nat_guard[1] = UINT64_C(1) << 63;
                tv.fast.node[k].guard[0] = offsetof(struct vcpu, s.nat) + 127;
            }
            break;
        case 18: tv.entry_seq = UINT64_MAX - 3; hops = expected_bound; break;
        case 19: budget = 5; break;
        case 20: budget = 6; hops = 1; break;
        case 21: budget = 7; hops = 1; break;
        case 22:
        case 23:
            native_budget_restore.restore = tv.fast.node[0].edge[0].restore;
            native_budget_restore.kick_at = test == 22 ? 9 : expected_bound - 1;
            for (unsigned k = 0; k < 2; ++k)
                tv.fast.node[k].edge[0].restore = (uintptr_t)native_budget_event;
            hops = native_budget_restore.kick_at;
            break;
        case 24:
            tv.fast.node[0].window = &window[0];
            tv.fast.node[1].window = &window[1];
            hops = 15;
            break;
        case 25: hops = expected_bound; break;
        case 26: /* aliases update the shared admission twice, in dependency order */
            for (unsigned k = 0; k < 2; ++k)
                tv.fast.second[k] = tv.fast.node[k].witness;
            hops = expected_bound;
            break;
        case 27: ready2 = false; break;
        case 28: ++epoch2; break;
        case 29: ++generation2; break;
        case 30: ++admission2; break;
        case 31: page2 = NULL; break;
        case 32: tv.fast.second[1].owner = NULL; break;
        case 33: ++tv.fast.second[1].token; break;
        case 34: ++tv.fast.second[1].iseq; break;
        case 35: ++generation; break;
        case 36: r[1].valid = false; break;
        case 54: break; /* PTA changes after preparation, before native admission. */
        case 55: tv.fast.node[0].edge[0].target = NULL; break;
        case 56:
            native_budget_restore.restore = tv.fast.node[0].edge[0].restore;
            native_budget_restore.kick_at = 1;
            native_budget_restore.generation = &generation;
            tv.fast.node[0].edge[0].restore = (uintptr_t)native_budget_event;
            hops = 1;
            break;
        case 57:
            tv.fast.node[1].edge[0].target = NULL;
            hops = 1;
            break;
        }
        native_timer_case(v, test, &hops);
        tv.x.budget_entry = budget;
        struct t1_stats stats = {0};
        tv.stats = &stats;
#if IA64VMM_TIER1_STATS
        tv.fast.diag_edges = test == 55 || test == 57 ? 1 : 2;
#endif
        if (test >= 15) {
            struct hostwin_config cfg = {40, 1, false};
            tv.hw = hostwin_create(&cfg, hostwin_linux_backend(), hostwin_linux_ctx());
            CHECK(tv.hw != NULL);
            hostwin_enter(tv.hw, v);
            code_register_vcpu(v);
            tv.stats = &stats;
            tv.fast.retirement = code_epoch_sample();
            tv.fast.nnodes = 2;
            t1_fast_prepare(&tv, &r[0]);
            CHECK(tv.x.fast == &tv.fast.node[0] && stats.chain_native_reuses == 1);
        }
        if (test == 54)
            v->s.cr[CR_PTA] ^= UINT64_C(1) << 16;
        uint64_t initial_seq = tv.entry_seq;
        native_timer.active = test >= 37 && test <= 53;
        CHECK(t1_native_test_enter((t1_entry_fn)(uintptr_t)code, v, budget, &tv.x) == 1);
        native_timer_check(v, test, hops);
        CHECK(calls == 1 && tv.fast.nrecord == hops && tv.fast.completed == hops);
#if IA64VMM_TIER1_STATS
        CHECK(stats.native_hops[hops] == 1);
        unsigned stop = tv.fast.diag_stop;
        CHECK(stop < T1_NATIVE_STOPS);
        CHECK(stats.native_stop[tv.fast.diag_edges > 1][hops != 0][stop] == 1);
        if (!test) CHECK(stop == T1_NATIVE_STOP_BATCH);
        if (test == 1 || test == 2 || test == 3 || test == 24)
            CHECK(stop == T1_NATIVE_STOP_WINDOW);
        if (test == 7 || test == 8 || test == 9 || test == 35 || test == 56)
            CHECK(stop == T1_NATIVE_STOP_WITNESS);
        if (test >= 27 && test <= 34) CHECK(stop == T1_NATIVE_STOP_SECOND);
        if (test == 55 || test == 57) CHECK(stop == T1_NATIVE_STOP_EDGE);
#endif
        CHECK(tv.x.budget_entry == budget - 3 * hops && tv.x.raw_r13 == budget - 3 * (hops + 1));
        CHECK(tv.x.retired_entry == 100 + 3 * hops && v->retired == 100 + 3 * hops);
        CHECK(tv.x.region == &r[hops != 0] && tv.x.fast == &tv.fast.node[hops != 0]);
        CHECK(tv.entry_seq == initial_seq + hops + (test == 4 || test == 36));
        CHECK(tv.running == (hops ? &r[1] : NULL));
        CHECK(clock == hops * (test >= 25 && test <= 36 ? 2 : 1));
        CHECK(entered == (test == 25 ? 2 * hops - 1 : clock) && tick == (hops != 0));
        CHECK(entered2 == (test == 25 ? 2 * hops : 0) && tick2 == (test == 25));
        CHECK(v->s.nat[12] == (test == 5));
        CHECK(v->s.psr == (test == 13 || test == 14 ? IA64_PSR_MFL : 0));
        if (test == 22 || test == 23)
            CHECK(native_budget_restore.calls == hops && v->irq_kick);
        if (test == 24)
            CHECK(window[1].attempts == 15 && window[1].entries == 14);
        if (test == 1)
            CHECK(window[0].entries == 1 && window[0].retired == 3 &&
                  window[1].entries == 1 && window[1].retired == 3 &&
                  window[1].attempts == T1_WINDOW_FOLD - 1);
        for (unsigned k = 0; k < hops; ++k) {
            const struct t1_fast_record *record = &tv.fast.record[k];
            CHECK(record->from == &tv.fast.node[k != 0] && record->to == &tv.fast.node[1] &&
                  record->consumed == 3 && !record->point);
        }
        if (test >= 15) {
            code_unregister_vcpu(v);
            hostwin_destroy(tv.hw);
        }
    }
    t1_arena_free(nat_leaf, 1);
    *v = saved;
    printf("unit_runtime: native edges: 32-hop bound, exact budget and late kick, self-edge policy fold, post-restore NaT/MF and final-valid failure, event/context/witness guards, two-page and alias recency, armed timer admission and expiry, callee saves\n");
}

/* Reusing a plan must never revive pointers across retirement, executor
 * registration or state restoration, even when the visible entry repeats. */
static void native_reuse_test(struct vcpu *v)
{
    static struct t1_vcpu tv;
    struct t1_stats stats = {0};
    static struct vcpu saved;
    saved = *v;
    struct hostwin_config cfg = {40, 1, false};
    struct hostwin_vcpu *hw = hostwin_create(&cfg, hostwin_linux_backend(), hostwin_linux_ctx());
    CHECK(hw != NULL);
    code_register_vcpu(v);
    for (unsigned pages = 1; pages <= 2; ++pages)
    for (unsigned test = 0; test < 18; ++test) {
        *v = saved;
        memset(&tv, 0, sizeof(tv));
        struct t1_region r = {.valid = true, .fast_points = 1, .sealed = true, .npages = 1,
                              .nguard = 2, .nguard_static = 1, .guard = {12, 95}};
        r.npages = pages;
        struct t1_region target = r;
        target.observed_returns = true;
        target.return_sof = 32;
        tv.v = v;
        tv.stats = &stats;
        tv.hw = hw;
        tv.x.v = v;
        tv.x.tv = &tv;
        tv.x.entry_bof = v->s.rse_bof % IA64_N_STACKED_PHYS;
        tv.x.entry_dirty = v->s.rse_dirty;
        tv.x.entry_bsp = v->s.ar[AR_BSP];
        hostwin_enter(hw, v);
        tv.fast.retirement = code_epoch_sample();
        tv.fast.nnodes = 2;
        tv.fast.node[0].region = &r;
        tv.fast.node[0].outgoing = 1;
        tv.fast.node[1].region = &target;
        for (unsigned k = 0; k < 2; ++k) {
            tv.fast.node[k].nguard = 2;
            tv.fast.node[k].nat_guard[0] = UINT64_C(1) << 12;
            tv.fast.node[k].nat_guard[1] = UINT64_C(1) << 63;
            tv.fast.node[k].guard[0] = offsetof(struct vcpu, s.nat) + 12;
            tv.fast.node[k].guard[1] = offsetof(struct vcpu, s.nat) +
                32 + (tv.x.entry_bof + 95) % IA64_N_STACKED_PHYS;
        }
        /* Invalid witness addresses are intentionally opaque to prepare;
         * only an admitted native edge may read them under a fresh pin. */
        tv.fast.node[0].witness.page_word = (const void *)hostwin_poison_va(hw);
        if (pages == 2)
            tv.fast.node[0].second = (const void *)hostwin_poison_va(hw);
        tv.fast.mmu_seq = v->mmu_seq;
        tv.fast.psr = v->s.psr;
        tv.fast.cfm = v->s.cfm;
        tv.fast.token = v->code_entry_generation;
        tv.fast.iseq = v->mmu_iseq;
        tv.fast.bof = tv.x.entry_bof;
        tv.fast.dirty = tv.x.entry_dirty;
        switch (test) {
        case 0: break;
        case 1:
            code_lock();
            code_retire_epoch();
            code_unlock();
            code_quiescent(v);
            break;
        case 2:
            code_unregister_vcpu(v);
            code_register_vcpu(v);
            break;
        case 3:
            t1_vcpu_state_restored(&tv);
            hostwin_enter(hw, v);
            break;
        case 4: tv.x.entry_bof = v->s.rse_bof = 95; break;
        case 5: tv.x.entry_dirty = v->s.rse_dirty = IA64_N_STACKED_PHYS - target.return_sof; break;
        case 6: tv.x.entry_bsp = ++v->s.ar[AR_BSP]; break;
        case 7: ++v->s.cfm; break;
        case 8: v->s.psr ^= IA64_PSR_MFL; break;
        case 9: ++v->mmu_seq; break;
        case 10: ++v->code_entry_generation; break;
        case 11: ++v->mmu_iseq; break;
        case 12: ++tv.x.observed_epoch; break;
        case 13:
            tv.x.entry_dirty = v->s.rse_dirty = IA64_N_STACKED_PHYS - target.return_sof + 1;
            break;
        case 14: v->s.cr[CR_PTA] ^= UINT64_C(1) << 16; break;
        case 15: tv.fast.nnodes = 0; break;
        case 16: tv.fast.node[0].region = &target; break;
        case 17: ++v->s.cfm; v->s.psr ^= IA64_PSR_MFL; break;
        }
        memset(&stats, 0, sizeof(stats));
        t1_fast_prepare(&tv, &r);
#if IA64VMM_TIER1_STATS
        for (unsigned reason = 0; reason < T1_NATIVE_REBUILDS; ++reason) {
            bool expected =
                ((test == 1 || test == 2) && reason == T1_NATIVE_REBUILD_RETIREMENT) ||
                ((test == 3 || test == 15) && reason == T1_NATIVE_REBUILD_EMPTY) ||
                ((test == 7 || test == 17) && reason == T1_NATIVE_REBUILD_CFM) ||
                ((test == 8 || test == 17) && reason == T1_NATIVE_REBUILD_PSR) ||
                (test == 9 && reason == T1_NATIVE_REBUILD_MMU) ||
                (test == 10 && reason == T1_NATIVE_REBUILD_TOKEN) ||
                (test == 11 && reason == T1_NATIVE_REBUILD_ISEQ) ||
                (test == 12 && reason == T1_NATIVE_REBUILD_BIND) ||
                (test == 13 && reason == T1_NATIVE_REBUILD_REBASE) ||
                (test == 14 && reason == T1_NATIVE_REBUILD_PTA) ||
                (test == 16 && reason == T1_NATIVE_REBUILD_REGION);
            CHECK(stats.native_rebuild[reason] == expected);
        }
#endif
        bool reuse = !test || (test >= 4 && test <= 6);
        CHECK(tv.x.fast == (reuse ? &tv.fast.node[0] : NULL));
#if IA64VMM_TIER1_STATS
        if (!reuse) {
            CHECK(stats.native_build_nodes[0] == 1 && stats.native_build_edges[0] == 1);
            CHECK(stats.native_node_reject[pages == 1 ? T1_NATIVE_NODE_WITNESS :
                                                     T1_NATIVE_NODE_GROUP] == 1);
        }
#endif
        if (test == 4)
            for (unsigned bof = 0; bof < IA64_N_STACKED_PHYS; ++bof) {
                tv.x.entry_bof = v->s.rse_bof = bof;
                tv.x.entry_bsp = (v->s.ar[AR_BSP] += 8);
                t1_fast_prepare(&tv, &r);
                CHECK(tv.x.fast == &tv.fast.node[0]);
                for (unsigned k = 0; k < 2; ++k)
                    CHECK(tv.fast.node[k].guard[0] == offsetof(struct vcpu, s.nat) + 12 &&
                          tv.fast.node[k].guard[1] == offsetof(struct vcpu, s.nat) +
                              32 + (bof + 95) % IA64_N_STACKED_PHYS);
            }
    }
    code_unregister_vcpu(v);
    hostwin_destroy(hw);
    *v = saved;
    printf("unit_runtime: native plan reuse: retirement/registration/state restore reject stale plans; compatible frame rebasing at every BOF and target return bounds\n");
}

static uintptr_t native_handoff_test_next(struct t1_exec *x)
{
    struct native_handoff_test *t = x->handoff_arg;
    uintptr_t stack;
    __asm__ volatile("mov %%rsp, %0" : "=r"(stack));
    if (!t->calls)
        t->stack = stack;
    CHECK(stack == t->stack);
    CHECK(x->point == 0 && x->raw_r13 == x->budget_entry - 3);
    CHECK(x->regs[T1_RAX] == x->v->s.gr[12]);
    ++t->calls;
    ++x->v->s.gr[12];
    x->budget_entry = 19 + t->calls;
    return --t->remaining ? t->body : 0;
}

static void native_handoff_test(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    struct vcpu *v = um_vcpu(UM_CODE);
    CHECK(ir != NULL);
    ir->npoints = ir->nuses = 1;
    ir->point[0].kind = T1_PT_EXIT;
    ir->point[0].frame_count = 1;
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m != NULL);
    m->nblocks = m->nframe = 1;
    m->frame[0] = (struct t1_mir_frame){T1_ELEMENT_GR(12), T1_MF_VREG, 0, 0};
    m->frame_loc[0] = (struct t1_loc){.kind = T1_LOC_REG, .reg = T1_RAX};
    struct t1_mir_insn *in = t1_mir_append(m, T1_MIR_LIVEIN);
    CHECK(in != NULL);
    in->opnd[0].fixed = T1_RAX;
    in->opnd[1].imm = T1_ELEMENT_GR(12);
    in = t1_mir_append(m, T1_MIR_CHARGE);
    CHECK(in != NULL);
    in->opnd[0].imm = 3;
    m->block[0].term = m->ninsns;
    m->point[0] = (struct t1_mir_point){m->ninsns, T1_MIR_NONE, 0, 1, T1_PT_EXIT};
    in = t1_mir_append(m, T1_MIR_EXIT);
    CHECK(in != NULL);
    in->point = 0;
    m->block[0].count = m->ninsns;
    struct t1_emitted e;
    char err[160];
    CHECK(t1_emit(m, &e, err, sizeof(err)));
    CHECK(e.native_entry && e.native_entry < e.size);
    size_t pages = (e.size + 4095) / 4096;
    uint8_t *code = t1_arena_alloc(pages);
    CHECK(code != NULL);
    t1_emit_place(&e, code);
    hal_code_set_executable(code, pages);
    static struct t1_exec x;
    memset(&x, 0, sizeof(x));
    for (unsigned r = 0; r < 16; ++r)
        x.regs[r] = UINT64_C(0xdeaddeaddeaddead);
    x.v = v;
    x.budget_entry = 19;
    struct native_handoff_test t = {.body = (uintptr_t)code + e.native_entry, .remaining = 65536};
    x.handoff = native_handoff_test_next;
    x.handoff_arg = &t;
    t1_entry_fn entry = (t1_entry_fn)(uintptr_t)code;
    CHECK(t1_native_test_enter(entry, v, x.budget_entry, &x) == 1);
    CHECK(t.calls == 65536 && !t.remaining);
    for (unsigned r = 1; r < 16; ++r)
        CHECK(x.regs[r] == UINT64_C(0xdeaddeaddeaddead));
    x.handoff = NULL;
    CHECK(entry(v, 99, &x) == T1_RET_POINT && x.raw_r13 == 96);
    native_fast_test(v, code, &e);
    native_reuse_test(v);
    t1_arena_free(code, pages);
    t1_emit_free(&e);
    t1_mir_destroy(m);
    free(ir);
    vcpu_destroy(v);
    printf("unit_runtime: 65536 native handoffs: constant stack, callee saves, successor LIVEINs and budget, sparse capture\n");
}
