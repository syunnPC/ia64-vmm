/* SPDX-License-Identifier: GPL-2.0-or-later */
static void span_discard(struct t1_region **r)
{
    T1H_CHECK(*r && !(*r)->valid && !(*r)->retired);
    t1_region_discard(*r);
    *r = NULL;
}

static void span_survivor(struct t1_region *r, uint8_t *original, const uint8_t *bytes)
{
    T1H_CHECK(r->code == original && r->sealed && !memcmp(original, bytes, 2048));
    T1H_CHECK(((int (*)(void))(uintptr_t)original)() == 42);
}

static void span_accounting(uint64_t padding)
{
#if IA64VMM_TIER1_STATS
    struct t1_stats s;
    t1_get_stats(&s);
    const struct t1_arena_stats *a = &s.arena;
    T1H_CHECK(a->sealed_padding == padding);
    T1H_CHECK(s.arena_bytes == 4096 + a->code_bytes[0] + a->code_bytes[1] + a->code_bytes[2]);
    uint64_t chunks = a->chunks[0] + a->chunks[1] + a->chunks[2] + a->chunks[3];
    T1H_CHECK(chunks * T1_CHUNK_BYTES ==
              (a->chunks[0] + a->chunks[3]) * T1_CHUNK_BYTES + a->open_tail + a->closed_tail +
              a->unsealed_used + a->sealed_empty + a->sealed_code + a->sealed_padding + a->sealed_holes);
#else
    (void)padding;
#endif
}

static void span_rotation(struct t1_emitted *e)
{
    struct t1_region *keep[2], *placed[2];
    for (unsigned i = 0; i < 2; ++i) {
        unsigned chunk = UINT32_MAX;
        struct t1_region *fill = t1_region_new();
        keep[i] = t1_region_new();
        e->size = T1_CHUNK_BYTES - 16;
        T1H_CHECK(fill && keep[i] && t1_arena_place_background(fill, e, &chunk));
        e->size = 16;
        T1H_CHECK(t1_arena_place_background(keep[i], e, &chunk));
        t1_arena_seal_background(&chunk);
        T1H_CHECK(chunk == UINT32_MAX);
        t1_region_discard(fill);
    }
    /* Keep batching in a fresh private chunk while whole chunks remain. */
    struct t1_region *fresh = t1_region_new();
    unsigned fresh_chunk = UINT32_MAX;
    e->size = 16;
    T1H_CHECK(fresh && t1_arena_place_background(fresh, e, &fresh_chunk));
    T1H_CHECK(fresh_chunk != UINT32_MAX && !fresh->sealed &&
              fresh->chunk != keep[0]->chunk && fresh->chunk != keep[1]->chunk);
    t1_region_discard(fresh);
    t1_arena_close_background(&fresh_chunk);
    uint8_t *raw[12];
    for (unsigned i = 0; i < 12; ++i)
        T1H_CHECK((raw[i] = t1_arena_alloc(1)) != NULL);
    struct t1_region *blocker = t1_region_new();
    unsigned chunk = UINT32_MAX;
    e->size = T1_CHUNK_BYTES;
    T1H_CHECK(blocker && t1_arena_place_background(blocker, e, &chunk));
    t1_arena_seal_background(&chunk);
    T1H_CHECK(chunk == UINT32_MAX && !t1_arena_free_chunks());
    for (unsigned i = 0; i < 2; ++i) {
        unsigned chunk = UINT32_MAX;
        placed[i] = t1_region_new();
        e->size = 4096;
        T1H_CHECK(placed[i] && t1_arena_place_background(placed[i], e, &chunk));
        T1H_CHECK(chunk == UINT32_MAX && placed[i]->sealed);
        T1H_CHECK(placed[i]->chunk == keep[0]->chunk || placed[i]->chunk == keep[1]->chunk);
    }
    T1H_CHECK(placed[0]->chunk != placed[1]->chunk);
    for (unsigned i = 0; i < 2; ++i) {
        t1_region_discard(placed[i]);
        t1_region_discard(keep[i]);
    }
    t1_region_discard(blocker);
    for (unsigned i = 0; i < 12; ++i)
        t1_arena_free(raw[i], 1);
}

void arena_span_tests(void);
void arena_span_tests(void)
{
    bool ready = t1_ready;
    struct t1_config saved = t1_cfg;
    t1_shutdown();
    T1H_CHECK(t1_init(&(struct t1_config){.code_bytes = 1u << 20}));
#if IA64VMM_TIER1_STATS
    struct t1_stats before;
    t1_get_stats(&before);
#endif
    struct vcpu *v[2];
    for (unsigned i = 0; i < 2; ++i) {
        v[i] = vcpu_create(&(struct vcpu_config){.index = i});
        T1H_CHECK(v[i]);
        code_register_vcpu(v[i]);
    }
    uint8_t *raw[13];
    for (unsigned i = 0; i < 13; ++i)
        T1H_CHECK((raw[i] = t1_arena_alloc(1)) != NULL);
    uint8_t *buf = t1_workspace_alloc(16, 0);
    T1H_CHECK(buf);
    memset(buf, 0x90, T1_CHUNK_BYTES);
    static const uint8_t code[] = {0xb8, 42, 0, 0, 0, 0xc3};
    memcpy(buf, code, sizeof(code));
    struct t1_region *r[24] = {0};
    struct t1_emitted e = {.buf = buf};
    unsigned chunk = UINT32_MAX, private_chunk = UINT32_MAX;
    for (unsigned i = 0; i < 16; ++i) {
        e.size = i < 2 ? 2048 : i == 2 ? 8192 : 4096;
        r[i] = t1_region_new();
        T1H_CHECK(r[i] && t1_arena_place_background(r[i], &e, &chunk));
    }
    t1_arena_seal_background(&chunk);
    T1H_CHECK(chunk == UINT32_MAX && t1_region_publish(r[0]));
    uint8_t *original = r[0]->code;
    uint8_t bytes[2048];
    memcpy(bytes, original, sizeof(bytes));
    r[16] = t1_region_new();
    e.size = T1_CHUNK_BYTES;
    T1H_CHECK(r[16] && t1_arena_place_background(r[16], &e, &private_chunk));
    T1H_CHECK(!t1_arena_free_chunks() && !t1_arena_has_space(1));
    span_discard(&r[1]); /* A shared page still belongs to the survivor. */
    T1H_CHECK(!t1_arena_has_space(1));
    span_survivor(r[0], original, bytes);
    span_discard(&r[2]); /* One discarded pending allocation spans two pages. */
    T1H_CHECK(t1_arena_has_space(2) && !t1_arena_has_space(3));
    r[17] = t1_region_new();
    e.size = 4100;
    buf[1] = 77;
    T1H_CHECK(r[17] && t1_arena_place_background(r[17], &e, &chunk));
    T1H_CHECK(chunk == UINT32_MAX && r[17]->code == original + 4096 && r[17]->sealed);
    T1H_CHECK(((int (*)(void))(uintptr_t)r[17]->code)() == 77);
    T1H_CHECK(!t1_arena_has_space(1)); /* Pending code pins both reused pages. */
    span_accounting(4080);
    span_survivor(r[0], original, bytes);
    T1H_CHECK(t1_region_publish(r[17]));
    uint64_t old_link = t1_region_link(r[17]);
    t1_region_retire(r[17]);
    t1_reclaim_passed();
    T1H_CHECK(!t1_arena_has_space(1));
    code_quiescent(v[0]);
    t1_reclaim_passed();
    T1H_CHECK(!t1_arena_has_space(1));
    code_quiescent(v[1]);
    t1_reclaim_passed();
    r[17] = NULL;
    T1H_CHECK(t1_arena_has_space(2) && !t1_region_from_link(old_link));
    r[18] = t1_region_new();
    e.size = 8192;
    T1H_CHECK(r[18] && t1_arena_place_background(r[18], &e, &chunk));
    T1H_CHECK(r[18]->code == original + 4096);
    span_accounting(0); /* The previous 4080-byte seal gap is gone. */
    span_survivor(r[0], original, bytes);

    /* Isolated pages do not satisfy a larger contiguous request. */
    span_discard(&r[3]);
    span_discard(&r[5]);
    span_discard(&r[7]);
    T1H_CHECK(t1_arena_has_space(1) && !t1_arena_has_space(2));
    code_lock();
    t1_request_code_chunk(1);
    t1_request_code_chunk(4);
    t1_request_code_chunk(1);
    code_unlock();
    T1H_CHECK(t1_memory_pressure() == 1 + T1_DEFER_CHUNK);
    r[19] = t1_region_new();
    e.size = 4096;
    T1H_CHECK(r[19] && t1_arena_place_background(r[19], &e, &chunk));
    T1H_CHECK(r[19]->code == original + 3 * 4096);
    T1H_CHECK(t1_memory_pressure() == 1 + T1_DEFER_CHUNK);
    for (unsigned i = 8; i <= 11; ++i)
        span_discard(&r[i]);
    T1H_CHECK(t1_arena_has_space(4) && !t1_memory_pressure());
    code_lock();
    t1_request_code_chunk(4);
    code_unlock();
    r[20] = t1_region_new();
    e.size = 4 * 4096;
    T1H_CHECK(r[20] && t1_arena_place_background(r[20], &e, &chunk));
    T1H_CHECK(r[20]->code == original + 7 * 4096);
    T1H_CHECK(!t1_memory_pressure()); /* The successful large request satisfied it. */
    span_accounting(0);
    span_survivor(r[0], original, bytes);

    /* An empty private chunk and raw tails are not reusable spans. */
    span_discard(&r[16]);
    T1H_CHECK(!t1_arena_free_chunks() && !t1_arena_has_space(16));
    r[21] = t1_region_new();
    e.size = T1_CHUNK_BYTES;
    T1H_CHECK(r[21] && !t1_arena_place_background(r[21], &e, &chunk));
    T1H_CHECK(t1_memory_pressure() == 1 + T1_DEFER_CHUNK);
    t1_arena_close_background(&private_chunk);
    T1H_CHECK(t1_arena_has_space(16) && !t1_memory_pressure());
    T1H_CHECK(t1_arena_place_background(r[21], &e, &chunk));
    t1_arena_seal_background(&chunk);
    span_survivor(r[0], original, bytes);
#if IA64VMM_TIER1_STATS
    struct t1_stats after;
    t1_get_stats(&after);
    T1H_CHECK(after.span_reuses == before.span_reuses + 4 &&
              after.span_reuse_pages == before.span_reuse_pages + 9);
#endif
    for (unsigned i = 0; i < 24; ++i)
        if (r[i]) {
            if (r[i]->valid)
                t1_region_retire(r[i]);
            else
                t1_region_discard(r[i]);
        }
    for (unsigned i = 0; i < 2; ++i)
        code_quiescent(v[i]);
    t1_reclaim_passed();
    for (unsigned i = 0; i < 13; ++i)
        t1_arena_free(raw[i], 1);
    T1H_CHECK(t1_arena_free_chunks() == 15 && t1_arena_bytes_used() == 4096);
    span_rotation(&e);
    T1H_CHECK(t1_arena_free_chunks() == 15 && t1_arena_bytes_used() == 4096);
    for (unsigned i = 0; i < 2; ++i) {
        code_unregister_vcpu(v[i]);
        vcpu_destroy(v[i]);
    }
    t1_workspace_free(buf, 16);
    t1_shutdown();
    if (ready)
        T1H_CHECK(t1_init(&saved));
    puts("arena spans: survivor execution, shared/pending/cross-page/pinned code, "
         "fragmented/cyclic fits, maximum pressure, private/raw exclusion and padding reuse passed");
}
