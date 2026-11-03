/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Table-only boundary tests never execute synthetic addresses. */
static void retained_fixup_offsets(void)
{
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    struct t1_emitted *e = calloc(1, sizeof(*e));
    CHECK(ir && e);
    t1_ir_init(ir, NULL);
    struct t1_mir *m = t1_mir_create(ir);
    CHECK(m && sizeof(struct t1_fixup) == 8);
    uintptr_t saved = t1_stub.exit_save;
    uintptr_t bases[] = {saved, 0, 1, UINTPTR_MAX - 1, UINTPTR_MAX};
    static const struct { uintptr_t delta; bool accepted; } offsets[] = {
        {0, true}, {1, true}, {UINTPTR_MAX, true},
        {INT32_MAX, true}, {UINTPTR_MAX - (uintptr_t)INT32_MAX, true},
        {(uintptr_t)INT32_MAX + 1, false}, {UINTPTR_MAX - (uintptr_t)INT32_MAX - 1, false},
        {UINT32_MAX, false}, {UINTPTR_MAX / 2, false},
    };
    unsigned layouts = 0;
    for (unsigned b = 0; b < sizeof(bases) / sizeof(bases[0]); ++b) {
        t1_stub.exit_save = bases[b];
        for (unsigned test = 0; test < sizeof(offsets) / sizeof(offsets[0]); ++test) {
            e->nabs = 3;
            for (unsigned k = 0; k < 3; ++k) e->abs_fix[k] = 1 + 5 * k;
            e->abs_target[0] = bases[b];
            e->abs_target[1] = bases[b] + offsets[test].delta;
            e->abs_target[2] = bases[b] + 1;
            struct t1_memory_stats before, after;
            t1_memory_get_stats(&before);
            struct t1_region *r = t1_region_new();
            CHECK(r);
            bool ok = t1_frame_build(r, m, e);
            CHECK(ok == offsets[test].accepted && r->meta);
            if (ok) {
                CHECK(r->nfix == 3 && r->meta_pages == t1_frame_pages(m, e));
                for (unsigned k = 0; k < r->nfix; ++k) {
                    CHECK(r->fix[k].at == e->abs_fix[k]);
                    CHECK(bases[b] + (intptr_t)r->fix[k].stub_offset == e->abs_target[k]);
                }
            } else CHECK(!r->valid && !r->nfix);
            /* The invalid middle entry leaves a partial table, as a
             * rejected compile does. Normal discard must free it all. */
            t1_region_discard(r);
            t1_memory_get_stats(&after);
            CHECK(after.used_bytes == before.used_bytes && after.workspace_bytes == before.workspace_bytes);
            ++layouts;
        }
    }
    t1_stub.exit_save = saved;
    static const unsigned counts[] = {0, 1, 2, 239, 240, 241, 479, 480, 481, 511, 512, 513, 1024};
    const uintptr_t targets[] = {t1_stub.exit_save, t1_stub.exit_nosave, t1_stub.miss,
                                t1_stub.helper, t1_stub.trap_entry};
    for (unsigned test = 0; test < sizeof(counts) / sizeof(counts[0]); ++test) {
        e->nabs = counts[test];
        for (unsigned k = 0; k < e->nabs; ++k) {
            e->abs_fix[k] = 1 + 5 * k;
            e->abs_target[k] = targets[k % (sizeof(targets) / sizeof(targets[0]))];
        }
        struct t1_region *r = t1_region_new();
        CHECK(r && t1_frame_build(r, m, e));
        CHECK(r->nfix == e->nabs && r->meta_pages == t1_frame_pages(m, e));
        for (unsigned k = 0; k < r->nfix; ++k)
            CHECK(r->fix[k].at == e->abs_fix[k] &&
                  saved + (intptr_t)r->fix[k].stub_offset == e->abs_target[k]);
        /* No point/frame/helper tables in this isolated layout. */
        CHECK(r->meta_pages == (e->nabs * 8u + 224u + 4095u) / 4096u);
        if (e->nabs == 1024) CHECK(r->meta_pages == 3);
        t1_region_discard(r);
        ++layouts;
    }
    /* Reuse one retained table at three code addresses. The arena patches
     * both initial placement and recycled code through the same path. */
    e->nabs = 10;
    e->size = 5 * e->nabs;
    e->buf = calloc(1, e->size);
    CHECK(e->buf);
    for (unsigned k = 0; k < e->nabs; ++k) {
        e->buf[5 * k] = k & 1 ? 0xe8 : 0xe9; /* Rel32 CALL and JMP fields. */
        e->abs_fix[k] = 5 * k + 1;
        e->abs_target[k] = targets[k % (sizeof(targets) / sizeof(targets[0]))];
    }
    struct t1_memory_stats before, after;
    t1_memory_get_stats(&before);
    uint64_t used = t1_arena_bytes_used();
    struct t1_region *r = t1_region_new();
    CHECK(r && t1_frame_build(r, m, e));
    unsigned chunk = UINT32_MAX;
    uintptr_t previous = 0;
    for (unsigned turn = 0; turn < 3; ++turn) {
        CHECK(t1_arena_place_background(r, e, &chunk));
        CHECK((uintptr_t)r->code != previous);
        previous = (uintptr_t)r->code;
        for (unsigned k = 0; k < e->nabs; ++k) {
            int32_t displacement;
            memcpy(&displacement, r->code + e->abs_fix[k], sizeof(displacement));
            CHECK(r->code[5 * k] == e->buf[5 * k]);
            CHECK((uintptr_t)(r->code + e->abs_fix[k] + 4) + (intptr_t)displacement == e->abs_target[k]);
        }
        t1_arena_seal_background(&chunk);
        CHECK(r->sealed);
        t1_arena_release(r);
    }
    t1_arena_close_background(&chunk);
    t1_region_discard(r);
    t1_memory_get_stats(&after);
    CHECK(after.used_bytes == before.used_bytes && after.workspace_bytes == before.workspace_bytes);
    CHECK(t1_arena_bytes_used() == used);
    free(e->buf);
    t1_mir_destroy(m); free(e); free(ir);
    printf("unit_runtime: compact retained fixups: %u layouts, signed32 limits, modular address boundaries, "
           "partial-failure cleanup, all stub targets, 0..1024 fixups and three placements\n", layouts);
}
