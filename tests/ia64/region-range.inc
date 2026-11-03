/* SPDX-License-Identifier: GPL-2.0-or-later */
static unsigned region_hook_calls;
static const struct gm_region *region_test_hook(struct vcpu *v, gpa_t gpa, unsigned size)
{
    (void)v; (void)gpa; (void)size;
    ++region_hook_calls;
    return NULL;
}
static void test_region_range(void)
{
    struct vcpu *v = new_vcpu();
    const struct gm_region *ram_region = gm_lookup(ENTRY);
    for (unsigned i = 0; i < RAM_SIZE / 4096; ++i) {
        gpa_t address = ENTRY + i * 4096;
        CHECK(ia64_physical_region(v, address, 8) == ram_region);
        CHECK(ia64_physical_region(v, address + 4088, 8) == ram_region);
    }
    CHECK(ia64_physical_region(v, ENTRY + RAM_SIZE - 8, 16) == NULL);
    CHECK(ia64_physical_region(v, ENTRY - 8, 8) == NULL);
    CHECK(ia64_physical_region(v, ENTRY + RAM_SIZE, 1) == NULL);
    CHECK(ia64_physical_region(v, ENTRY + 0x20000, 8) == gm_lookup(ENTRY + 0x20000));
    CHECK(ia64_physical_region(v, ENTRY, 8) == ram_region);
    CHECK(ia64_physical_region(v, ENTRY + 0x30000, 8) == gm_lookup(ENTRY + 0x30000));
    CHECK(ia64_physical_region(v, ENTRY + 0x30ff8, 16) == NULL);
    CHECK(ia64_physical_region(v, ENTRY + 0x40000, 8) == gm_lookup(ENTRY + 0x40000));
    CHECK(ia64_physical_region(v, UINT64_C(1) << (IA64_IMPL_PA_MSB + 1), 8) == NULL);
    CHECK(ia64_physical_region(v, ENTRY, 8) == ram_region);
    v->memory_hook = region_test_hook; region_hook_calls = 0;
    CHECK(ia64_physical_region(v, ENTRY + 4096, 8) == NULL);
    CHECK(ia64_physical_region(v, ENTRY + 8192, 8) == NULL);
    CHECK(region_hook_calls == 2);
    v->memory_hook = NULL;
    CHECK(ia64_physical_region(v, ENTRY + 8192, 8) == ram_region);
    /* A copied diagnostic cache pointing at another live region must be
     * checked by address before it is reused. */
    v->physical_region = gm_lookup(ENTRY + 0x30000);
    CHECK(ia64_physical_region(v, ENTRY, 8) == ram_region);
    vcpu_reset(v, NULL);
    CHECK(v->physical_region == NULL);
    CHECK(ia64_physical_region(v, ENTRY, 8) == ram_region);
    vcpu_destroy(v);
}

extern const struct gm_region *baseline_ia64_physical_region(struct vcpu *, gpa_t, unsigned) __attribute__((weak));
static void bench_region_paired(unsigned pairs)
{
    CHECK(baseline_ia64_physical_region != NULL && pairs > 0 && pairs <= 100);
    static const char *const names[] = {"within-page", "sequential-pages", "alternating-pages", "alternating-regions", "hole", "cross-boundary"};
    struct vcpu *v = new_vcpu();
    enum { CALLS = 4 * 1024 * 1024, INPUTS = 1024 };
    for (unsigned pair = 0; pair < pairs; ++pair)
        for (unsigned segment = 0; segment < 4; ++segment) {
            unsigned candidate = (pair & 1) ^ (segment == 1 || segment == 2);
            const struct gm_region *(*target)(struct vcpu *, gpa_t, unsigned) = candidate ?
                ia64_physical_region : baseline_ia64_physical_region;
            printf("paired-segment pair=%u order=%u variant=%s\n", pair, segment, candidate ? "candidate" : "baseline");
            for (unsigned mode = 0; mode < IA64VMM_ARRAY_SIZE(names); ++mode) {
                gpa_t addresses[INPUTS];
                unsigned size = mode == 5 ? 16 : 8;
                uint64_t expected = 0;
                for (unsigned i = 0; i < INPUTS; ++i) {
                    addresses[i] = ENTRY + (mode == 0 ? (i & 31) * 8 : mode == 1 ? (i & 15) * 4096 :
                        mode == 2 ? (i & 1) * 0xf000 : mode == 3 ? (i & 1) * 0x30000 :
                        mode == 4 ? 0x11000 : RAM_SIZE - 8);
                    const struct gm_region *r = gm_lookup(addresses[i]);
                    if (r && size > r->size - (addresses[i] - r->base)) r = NULL;
                    expected += r ? r->base + r->size + r->kind : 7;
                }
                v->physical_region = NULL;
                for (unsigned i = 0; i < INPUTS; ++i) (void)target(v, addresses[i], size);
                uint64_t sum = 0, start = hal_counter(); clock_t cpu_start = clock();
                for (unsigned i = 0; i < CALLS; ++i) {
                    const struct gm_region *r = target(v, addresses[i & (INPUTS - 1)], size);
                    sum += r ? r->base + r->size + r->kind : 7;
                }
                clock_t cpu_elapsed = clock() - cpu_start; uint64_t elapsed = hal_counter() - start;
                CHECK(sum == expected * (CALLS / INPUTS));
                printf("region-bench input=%s calls=%u ns_per_call=%.3f cpu_ns_per_call=%.3f checksum=%llu\n",
                    names[mode], CALLS, (double)elapsed * 1e9 / hal_counter_hz() / CALLS,
                    (double)cpu_elapsed * 1e9 / CLOCKS_PER_SEC / CALLS, (unsigned long long)sum);
            }
        }
    vcpu_destroy(v);
}
