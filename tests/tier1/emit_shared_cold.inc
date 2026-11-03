/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Physical MIR expansion checks: two distinct memory sites and two
 * distinct checkpoints. Each pair of failure branches shares exactly its
 * own stub. Checkpoint expansions also execute at every budget boundary
 * and with either notification word set, including verification builds. */
static unsigned shared_cold_targets(const struct t1_emitted *e, size_t target[8])
{
    unsigned n = 0;
    for (size_t at = 0; at < e->size;) {
        struct t1_x86_decoded d;
        CHECK(t1_x86_decode(e->buf + at, e->size - at, &d));
        if (d.op >= T1_X86_JCC && d.op <= T1_X86_JCC_LAST) {
            int32_t rel;
            memcpy(&rel, e->buf + at + d.length - 4, sizeof(rel));
            CHECK(n < 8);
            target[n++] = (size_t)((int64_t)at + d.length + rel);
        }
        at += d.length;
    }
    return n;
}

static void emit_shared_cold(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    UM_CHECK(ir != NULL);
    for (unsigned with_sites = 0; with_sites < 2; ++with_sites)
    for (unsigned flags = 0; flags < (with_sites ? 8u : 2u); ++flags) {
        t1_ir_init(ir, NULL);
        t1_ir_new_block(ir, 0);
        unsigned start = with_sites ? 2 : 0;
        unsigned count = start + 3;
        ir->nsites = (uint16_t)start;
        for (unsigned p = 0; p < count; ++p) {
            uint8_t kind = p < start ? T1_PT_SITE : p + 1 < count ? T1_PT_CHECKPOINT : T1_PT_EXIT;
            CHECK(t1_ir_new_point(ir, kind, UM_CODE, 0, 0, 0, 0) == p);
            CHECK(t1_ir_set_frame(ir, p, NULL, NULL, NULL, 0));
        }
        struct t1_mir *m = t1_mir_create(ir);
        UM_CHECK(m != NULL);
        m->nblocks = 1;
        m->window_bits = 40;
        m->flags = flags & 1 ? T1_COMPILE_VERIFY : 0;
        for (unsigned p = 0; p < count; ++p) {
            uint16_t op = p < start ? T1_MIR_SITE : p + 1 < count ? T1_MIR_CHECKPOINT : T1_MIR_EXIT;
            struct t1_mir_insn *in = t1_mir_append(m, op);
            UM_CHECK(in != NULL);
            in->point = p;
            m->point[p].insn = m->ninsns - 1;
            if (p < start) {
                in->opnd[0].fixed = T1_RBX;
                in->opnd[1].fixed = T1_RAX;
                unsigned bytes = flags & 4 ? 1 : 8;
                in->opnd[2].imm = (int64_t)((p << 16) | (bytes << 8) | (flags & 2 ? T1_SITE_ALIGN_PROVEN : 0));
            } else in->opnd[0].imm = (p - start + 1) * 8;
        }
        m->block[0].first = 0;
        m->block[0].count = m->ninsns;
        m->block[0].term = m->ninsns - 1;
        struct t1_emitted e;
        char err[200];
        CHECK(t1_emit(m, &e, err, sizeof(err)));
        size_t target[8];
        unsigned ckpt = with_sites ? (flags & 6 ? 2 : 4) : 0;
        CHECK(shared_cold_targets(&e, target) == ckpt + 4);
        if (with_sites && !(flags & 6))
            CHECK(target[0] == target[1] && target[2] == target[3] && target[0] != target[2]);
        CHECK(target[ckpt] == target[ckpt + 1] && target[ckpt + 2] == target[ckpt + 3]);
        CHECK(target[ckpt] != target[ckpt + 2]);
        unsigned exit_fixups = 0;
        for (unsigned k = 0; k < e.nabs; ++k) exit_fixups += e.abs_target[k] == t1_stub.exit_nosave;
        CHECK(exit_fixups == 3);   /* one per checkpoint, plus the normal exit */
        if (!with_sites) {
            size_t pages = (e.size + 4095) / 4096;
            uint8_t *code = t1_arena_alloc(pages);
            CHECK(code != NULL);
            t1_emit_place(&e, code);
            hal_code_set_executable(code, pages);
            t1_entry_fn fn = (t1_entry_fn)(uintptr_t)code;
            struct vcpu *v = um_vcpu(UM_CODE);
            static struct t1_exec xc;
            static const uint64_t budgets[] = {0, 7, 8, 15, 16, UINT64_MAX};
            for (unsigned b = 0; b < sizeof(budgets) / sizeof(budgets[0]); ++b)
            for (unsigned event = 0; event < 4; ++event) {
                memset(&xc, 0, sizeof(xc));
                v->requested_exit = event & 1;
                v->irq_kick = event >> 1;
                unsigned point = budgets[b] < 8 || event ? 0 : budgets[b] < 16 ? 1 : 2;
                CHECK(fn(v, budgets[b], &xc) == T1_RET_POINT);
                CHECK(xc.point == point && xc.raw_r13 == budgets[b]);
                CHECK(v->requested_exit == (event & 1) && v->irq_kick == (event >> 1));
            }
            vcpu_destroy(v);
            t1_arena_free(code, pages);
        }
        t1_emit_free(&e);
        t1_mir_destroy(m);
    }
    free(ir);
    printf("unit_runtime: shared site/checkpoint cold stubs: distinct point targets, budget and notification paths\n");
}
