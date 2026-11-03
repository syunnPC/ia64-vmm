/* SPDX-License-Identifier: GPL-2.0-or-later */
#if IA64VMM_TIER1_STATS
static unsigned arena_occupancy_snapshots;
static void arena_occupancy_expect(const struct t1_arena_stats *want)
{
    struct t1_stats s;
    t1_get_stats(&s);
    if (memcmp(&s.arena, want, sizeof(*want))) {
        const uint64_t *got = (const void *)&s.arena, *expected = (const void *)want;
        for (unsigned i = 0; i < sizeof(*want) / sizeof(*expected); ++i)
            if (got[i] != expected[i])
                fprintf(stderr, "arena occupancy field %u: got %llu expected %llu\n", i,
                        (unsigned long long)got[i], (unsigned long long)expected[i]);
    }
    T1H_CHECK(!memcmp(&s.arena, want, sizeof(*want)));
    const struct t1_arena_stats *a = &s.arena;
    T1H_CHECK(s.arena_bytes == 4096 + a->code_bytes[0] + a->code_bytes[1] + a->code_bytes[2]);
    uint64_t chunks = a->chunks[0] + a->chunks[1] + a->chunks[2] + a->chunks[3];
    T1H_CHECK(chunks * T1_CHUNK_BYTES ==
              (a->chunks[0] + a->chunks[3]) * T1_CHUNK_BYTES + a->open_tail + a->closed_tail +
              a->unsealed_used + a->sealed_empty + a->sealed_code + a->sealed_padding + a->sealed_holes);
    ++arena_occupancy_snapshots;
}
#endif

void arena_occupancy_tests(void);
void arena_occupancy_tests(void)
{
#if IA64VMM_TIER1_STATS
    bool ready = t1_ready;
    struct t1_config saved = t1_cfg;
    arena_occupancy_snapshots = 0;
    t1_shutdown();
    T1H_CHECK(t1_init(&(struct t1_config){.code_bytes = 1u << 20}));
    struct t1_arena_stats want = {.chunks = {15, 0, 0, 0}};
    arena_occupancy_expect(&want);
    uint8_t *raw = t1_arena_alloc(1);
    T1H_CHECK(raw);
    want.chunks[0] = 14;
    want.chunks[3] = 1;
    arena_occupancy_expect(&want);

    uint8_t buf[8192] = {0};
    struct t1_emitted e = {.buf = buf, .size = 1000};
    struct t1_region *a = t1_region_new(), *b = t1_region_new();
    unsigned chunk = UINT32_MAX;
    T1H_CHECK(a && b && t1_arena_place_background(a, &e, &chunk));
    e.size = 4100;
    T1H_CHECK(t1_arena_place_background(b, &e, &chunk));
    T1H_CHECK(b->code == a->code + 1008);
    uint8_t *original = a->code;
    want.chunks[0] = 13;
    want.chunks[1] = 1;
    want.code_bytes[0] = 5120;
    want.open_tail = T1_CHUNK_BYTES - 5120;
    want.unsealed_used = 5120;
    arena_occupancy_expect(&want);

    /* A first entry may seal a prefix through another pending region. */
    t1_arena_seal_for_entry(a);
    T1H_CHECK(a->sealed && !b->sealed);
    want.unsealed_used = 1024;
    want.sealed_code = 4096;
    arena_occupancy_expect(&want);
    t1_arena_seal_background(&chunk);
    T1H_CHECK(a->sealed && b->sealed);
    want.open_tail = T1_CHUNK_BYTES - 8192;
    want.unsealed_used = 0;
    want.sealed_code = 5120;
    want.sealed_padding = 3072;
    arena_occupancy_expect(&want);

    struct vcpu *v = vcpu_create(&(struct vcpu_config){0});
    T1H_CHECK(v);
    code_register_vcpu(v);
    T1H_CHECK(t1_region_publish(a) && t1_region_publish(b));
    want.code_bytes[0] = 0;
    want.code_bytes[1] = 5120;
    arena_occupancy_expect(&want);
    t1_region_retire(b);
    want.code_bytes[1] = 1008;
    want.code_bytes[2] = 4112;
    arena_occupancy_expect(&want);
    t1_reclaim();
    arena_occupancy_expect(&want); /* Retirement is still pinned. */
    code_quiescent(v);
    t1_reclaim();
    want.code_bytes[2] = 0;
    want.sealed_code = 1008;
    want.sealed_empty = 4096;
    want.sealed_padding = 0;
    want.sealed_holes = 3088;
    arena_occupancy_expect(&want);

    struct t1_region *c = t1_region_new();
    e.size = 17;
    T1H_CHECK(c && t1_arena_place_background(c, &e, &chunk));
    T1H_CHECK(c->code == a->code + 8192);
    want.code_bytes[0] = 32;
    want.open_tail -= 32;
    want.unsealed_used = 32;
    arena_occupancy_expect(&want);
    t1_arena_seal_background(&chunk);
    want.open_tail = T1_CHUNK_BYTES - 12288;
    want.unsealed_used = 0;
    want.sealed_code = 1040;
    want.sealed_padding = 4064;
    arena_occupancy_expect(&want);
    t1_region_discard(c);
    want.code_bytes[0] = 0;
    want.sealed_code = 1008;
    want.sealed_empty = 8192;
    want.sealed_padding = 0;
    arena_occupancy_expect(&want);

    t1_arena_close_background(&chunk);
    T1H_CHECK(chunk == UINT32_MAX);
    want.chunks[1] = 0;
    want.chunks[2] = 1;
    want.closed_tail = want.open_tail;
    want.open_tail = 0;
    arena_occupancy_expect(&want);
    t1_region_retire(a);
    want.code_bytes[1] = 0;
    want.code_bytes[2] = 1008;
    t1_reclaim();
    arena_occupancy_expect(&want);
    code_quiescent(v);
    t1_reclaim();
    want = (struct t1_arena_stats){.chunks = {14, 0, 0, 1}};
    arena_occupancy_expect(&want);

    /* Reusing the chunk must not retain its earlier seal-padding history. */
    struct t1_region *d = t1_region_new();
    e.size = sizeof(buf);
    T1H_CHECK(d && t1_arena_place_background(d, &e, &chunk) && d->code == original);
    want.chunks[0] = 13;
    want.chunks[1] = 1;
    want.code_bytes[0] = 8192;
    want.open_tail = T1_CHUNK_BYTES - 8192;
    want.unsealed_used = 8192;
    arena_occupancy_expect(&want);
    t1_arena_seal_background(&chunk);
    want.unsealed_used = 0;
    want.sealed_code = 8192;
    arena_occupancy_expect(&want);
    t1_region_discard(d);
    want.code_bytes[0] = 0;
    want.sealed_code = 0;
    want.sealed_empty = 8192;
    arena_occupancy_expect(&want);
    t1_arena_close_background(&chunk);
    want = (struct t1_arena_stats){.chunks = {14, 0, 0, 1}};
    arena_occupancy_expect(&want);
    t1_arena_free(raw, 1);
    want.chunks[0] = 15;
    want.chunks[3] = 0;
    arena_occupancy_expect(&want);
    code_unregister_vcpu(v);
    vcpu_destroy(v);
    t1_shutdown();
    if (ready)
        T1H_CHECK(t1_init(&saved));
    printf("arena occupancy: %u exact snapshots, 16-byte alignment, pending and cross-page code, "
           "partial seals, survivor holes, epoch-pinned retirement and reuse passed\n",
           arena_occupancy_snapshots);
#endif
}
