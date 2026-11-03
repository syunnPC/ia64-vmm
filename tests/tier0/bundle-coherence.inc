/* SPDX-License-Identifier: GPL-2.0-or-later */
static unsigned stay_retired;
static void stay_retire(struct code_dependency *d) { (void)d; ++stay_retired; }
/* The stay events that force the next comparison. */
static void test_stay_rules(struct vcpu *v)
{
    for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS; mode <= CODE_VALIDITY_EPOCH_COMPARE; ++mode)
        for (enum engine_kind engine = ENGINE_INTERP; engine <= ENGINE_LOCKSTEP; ++engine) {
            /* A storage epoch change ends the stay although v never leaves
             * P: the DMA-written bundle is compared before it runs again. */
            CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
            bundle(BASE, 0x11, nop, add(8,9,0), validator_branch(BASE, BASE));
            restart(v, engine, BASE); v->s.gr[9] = 11; v->s.gr[10] = 22;
            validator_run(v, 3); validator_run(v, 3); CHECK(v->s.gr[8] == 11);
            dma_bundle(BASE, 0x11, nop, add(8,10,0), validator_branch(BASE, BASE));
            code_epoch_bump(CODE_DEVICE_STORAGE);
            validator_run(v, 3); CHECK(v->s.gr[8] == 22);
        }
    /* Entry fetch context: equal contexts continue the stay without a
     * comparison, a different translation of P starts a new one. */
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    bundle(BASE, 0x11, nop, nop, validator_branch(BASE, BASE));
    restart(v, ENGINE_TIER0, BASE);
    uint64_t low, high, version;
    CHECK(code_source_read(v, BASE, &low, &high, &version).kind == IA64_STEP_COMPLETE);
    struct code_dependency d = {.invalidate = stay_retire, .mask = {1}};
    CHECK(code_dependency_publish(v, BASE, version, &d));
    code_dependency_attach(BASE, &d);
    const struct code_fetch f1 = {.psr = IA64_PSR_IT, .rr = 1, .pta = 2, .pte = 0x100661},
        f2 = {.psr = IA64_PSR_IT, .rr = 1, .pta = 2, .pte = 0x100663};
    struct code_validity_stats before, after;
    code_validity_get_stats(&before);
    CHECK(code_dependency_enter(v, &d, &f1) && code_dependency_enter(v, &d, &f1));
    code_validity_get_stats(&after); CHECK(after.comparisons == before.comparisons + 1);
    CHECK(code_dependency_enter(v, &d, &f2) && code_dependency_enter(v, &d, &f2));
    code_validity_get_stats(&before); CHECK(before.comparisons == after.comparisons + 1);
    /* An exact mask must never cover a bundle without a snapshot: such a
     * dependent is retired instead of skipping that bundle's comparison. */
    struct code_dependency broken = {.invalidate = stay_retire, .mask = {3}};
    CHECK(!code_dependency_publish(v, BASE, version, &broken));
    code_dependency_attach(BASE, &broken); stay_retired = 0;
    CHECK(!code_dependency_enter(v, &broken, &f2) && stay_retired == 1 && !broken.page);
    CHECK(d.page && code_dependency_enter(v, &d, &f2));
    code_dependency_attach(BASE, &broken); stay_retired = 0;
    CHECK(!code_dependency_enter(v, &broken, &f2) && stay_retired == 1 && !broken.page);
    code_dependency_remove(&d);
}
/* Page sets: a stay holds both pages of a two-page
 * region entry. */
static gpa_t stay_race_page;
static void stay_race_hook(gpa_t page, enum code_test_stage stage)
{
    /* A peer's store into another bundle of the page: its stay generation
     * moves between stay() and the comparison of this entry. */
    if (stage == CODE_TEST_STAY && page == stay_race_page) {
        stay_race_page = 0;
        code_notify_cpu_write(page + 16, 16);
    }
}
/* Stays begun or ended and pages admitted or dropped. */
static uint64_t stay_changes(void)
{
    struct code_stay_stats s;
    code_stay_get_stats(&s);
    return s.page + s.reset + s.event + s.epoch + s.fetch;
}
static void test_stay_page_set(struct vcpu *v)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    const gpa_t p0 = BASE, p1 = BASE + 4096, p2 = BASE + 2 * 4096;
    const struct code_fetch f = {.psr = IA64_PSR_IT, .rr = 1, .pta = 2, .pte = 0x100661};
    for (unsigned round = 0; round < 4; ++round) {
        bundle(p0, 0x11, nop, nop, validator_branch(p0, p1));
        bundle(p1, 0x11, nop, nop, validator_branch(p1, p0));
        bundle(p1 + 16, 0x11, nop, nop, validator_branch(p1 + 16, p0));
        bundle(p2, 0x11, nop, nop, validator_branch(p2, p2));
        restart(v, ENGINE_TIER0, p0);
        uint64_t low, high, g0, g1, g2;
        CHECK(code_source_read(v, p0, &low, &high, &g0).kind == IA64_STEP_COMPLETE);
        CHECK(code_source_read(v, p1, &low, &high, &g1).kind == IA64_STEP_COMPLETE);
        CHECK(code_source_read(v, p2, &low, &high, &g2).kind == IA64_STEP_COMPLETE);
        struct code_dependency set[2] = {{.invalidate = stay_retire, .mask = {1}},
                                         {.invalidate = stay_retire, .mask = {1}}};
        struct code_dependency other = {.invalidate = stay_retire, .mask = {1}};
        CHECK(code_dependency_publish(v, p0, g0, &set[0]) && code_dependency_publish(v, p1, g1, &set[1]) &&
              code_dependency_publish(v, p2, g2, &other));
        code_dependency_attach(p0, &set[0]); code_dependency_attach(p1, &set[1]); code_dependency_attach(p2, &other);
        stay_retired = 0;
        CHECK(code_dependencies_enter(v, set, 2, &f));
        struct code_validity_stats a, b;
        code_validity_get_stats(&a);
        uint64_t stays = stay_changes();
        if (round == 0) {
            /* (c) Tier0 and the interpreter on the non-entry page continue
             * the stay; the region's next entry compares nothing. */
            CHECK(code_dependency_enter(v, &set[1], &f));
            CHECK(code_page_enter(v, p1, false).kind == IA64_STEP_COMPLETE);
            CHECK(code_dependencies_enter(v, set, 2, &f));
            code_validity_get_stats(&b);
            CHECK(stay_changes() == stays && b.comparisons == a.comparisons && !stay_retired);
        } else if (round == 1) {
            /* (a) DMA into a compared bundle of the non-entry page while v
             * stays within the set: not seen; v enters p2 (it joins the
             * stay) and an instruction-side TC change ends the stay, so the
             * next entry compares it and retires the dependent. */
            dma_bundle(p1, 0x11, nop, add(8,10,0), validator_branch(p1, p0));
            CHECK(code_dependencies_enter(v, set, 2, &f) && !stay_retired);
            CHECK(code_dependency_enter(v, &other, &f) && stay_changes() == stays + 1);
            ia64_mmu_changed(v, IA64_MMU_TC, 0, 0);
            CHECK(!code_dependencies_enter(v, set, 2, &f) && stay_retired == 1 && !set[1].page);
        } else if (round == 2) {
            /* (b) A notified CPU or VMM store into the non-entry page ends
             * the stay; the next entry fails (dependent retired). */
            code_notify_vmm_write(p1, 16);
            CHECK(stay_retired == 1 && !set[1].page);
            CHECK(!code_dependencies_enter(v, set, 2, &f));
        } else {
            /* (d) A peer's event on the non-entry page between stay() and
             * the comparison: the entry uses the slots stay() returned,
             * and the next entry admits the page again. */
            stay_race_page = p1;
            code_validity_set_test_hook(stay_race_hook);
            code_notify_cpu_write(p0 + 16, 16);       /* p0 leaves the stay: the next entry admits it again */
            CHECK(code_dependencies_enter(v, set, 2, &f));
            code_validity_set_test_hook(NULL);
            CHECK(!stay_race_page && !stay_retired);
            uint64_t before = stay_changes();
            CHECK(code_dependencies_enter(v, set, 2, &f) && stay_changes() == before + 1 && !stay_retired);
        }
        code_dependency_remove(&set[0]); code_dependency_remove(&set[1]); code_dependency_remove(&other);
    }
}
/* Epoch mode, provisional source reads (formation before attach). A record's
 * validated epoch vouches for every fresh bundle, so a validation of one
 * dependent's bundles at a new epoch must also compare the provisional ones
 * (DMA case). Untracking the page lets inline stores bypass notification,
 * so it must also void provisional reads (eviction of the last dependent). */
static void test_epoch_provisional(struct vcpu *v)
{
    CHECK(code_validity_configure(CODE_VALIDITY_EPOCH_COMPARE, CODE_SNAPSHOT_DEFAULT_BYTES));
    bundle(DATA, 0x11, nop, nop, validator_branch(DATA, DATA));
    bundle(DATA + 16, 0x11, nop, nop, validator_branch(DATA + 16, DATA + 16));
    bundle(BASE, 0x11, store(3,9,10), nop, validator_branch(BASE, BASE));
    restart(v, ENGINE_TIER0, BASE);
    uint64_t low, high, v0, v1;
    struct code_dependency d0 = {.invalidate = stay_retire, .mask = {1}},
        d1 = {.invalidate = stay_retire, .mask = {2}};
    CHECK(code_source_read(v, DATA, &low, &high, &v0).kind == IA64_STEP_COMPLETE);
    CHECK(code_dependency_publish(v, DATA, v0, &d0));
    code_dependency_attach(DATA, &d0);
    CHECK(code_source_read(v, DATA + 16, &low, &high, &v1).kind == IA64_STEP_COMPLETE);
    dma_bundle(DATA + 16, 0x11, nop, add(8,10,0), validator_branch(DATA + 16, DATA + 16));
    code_epoch_bump(CODE_DEVICE_STORAGE);
    stay_retired = 0;
    CHECK(code_dependency_enter(v, &d0, &(struct code_fetch){0}) && d0.page && !stay_retired);
    CHECK(!code_dependency_publish(v, DATA + 16, v1, &d1));
    CHECK(code_source_read(v, DATA + 16, &low, &high, &v1).kind == IA64_STEP_COMPLETE);
    code_dependency_remove(&d0);
    /* A sealed Tier0 region stores inline, without notification, exactly
     * when code_page_has_code() is false (tier0_memory_pointer). */
    CHECK(!code_page_has_code(DATA));
    uint64_t inline_store = 0x1234;
    memcpy(ram + (DATA + 16 - BASE), &inline_store, 8);
    CHECK(!code_dependency_publish(v, DATA + 16, v1, &d1));
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
}
/* The vCPU-free source read returns
 * exactly what code_source_read returns for an unhooked vCPU, never goes
 * through a vCPU's memory hook, sees notified writes, and refuses what is
 * not code (unmapped, MMIO, unimplemented physical addresses). */
static const struct gm_region *no_region_hook(struct vcpu *v, gpa_t gpa, unsigned size)
{ (void)v; (void)gpa; (void)size; return NULL; }
static void test_source_read_physical(struct vcpu *v)
{
    for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS; mode <= CODE_VALIDITY_NONE; ++mode) {
        CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
        bundle(BASE + 32, 0x11, nop, add(8,9,0), validator_branch(BASE + 32, BASE + 32));
        restart(v, ENGINE_TIER0, BASE);
        uint64_t l0, h0, g0, l1, h1, g1;
        CHECK(code_source_read(v, BASE + 32, &l0, &h0, &g0).kind == IA64_STEP_COMPLETE);
        CHECK(code_source_read_physical(BASE + 32, &l1, &h1, &g1).kind == IA64_STEP_COMPLETE);
        CHECK(l1 == l0 && h1 == h0 && g1 == g0 && (mode == CODE_VALIDITY_NONE) == (g0 == 0));
        v->memory_hook = no_region_hook;
        CHECK(code_source_read(v, BASE + 32, &l1, &h1, &g1).kind == IA64_STEP_EXIT);
        CHECK(code_source_read_physical(BASE + 32, &l1, &h1, &g1).kind == IA64_STEP_COMPLETE);
        CHECK(l1 == l0 && h1 == h0 && g1 == g0);
        v->memory_hook = NULL; v->physical_region = NULL;
        bundle(BASE + 32, 0x11, nop, add(8,10,0), validator_branch(BASE + 32, BASE + 32));
        CHECK(code_source_read_physical(BASE + 32, &l1, &h1, &g1).kind == IA64_STEP_COMPLETE);
        CHECK((l1 != l0 || h1 != h0) && (mode == CODE_VALIDITY_NONE ? g1 == 0 : g1 != g0));
        CHECK(code_source_read_physical(DEVICE, &l1, &h1, &g1).kind == IA64_STEP_EXIT);
        CHECK(code_source_read_physical(BASE + sizeof(ram), &l1, &h1, &g1).kind == IA64_STEP_EXIT);
        CHECK(code_source_read_physical(BASE + sizeof(ram) - 8, &l1, &h1, &g1).kind == IA64_STEP_EXIT);
        CHECK(code_source_read_physical(UINT64_C(1) << (IA64_IMPL_PA_MSB + 1), &l1, &h1, &g1).kind ==
              IA64_STEP_EXIT);
    }
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
}
static void test_bundle_coherence(struct vcpu *v)
{
    uint64_t mask[4];
    code_bundle_mask(BASE, BASE + 63 * 16 + 15, 2, mask);
    CHECK(mask[0] == (UINT64_C(1) << 63) && mask[1] == 1 && !mask[2] && !mask[3]);
    code_bundle_mask(UINT64_MAX & ~UINT64_C(4095), UINT64_MAX - 15, UINT64_MAX, mask);
    CHECK(!mask[0] && !mask[1] && !mask[2] && mask[3] == (UINT64_C(1) << 63));
    code_bundle_mask(BASE, BASE, 0, mask); CHECK(!(mask[0] | mask[1] | mask[2] | mask[3]));
    for (enum code_validity_mode mode = CODE_VALIDITY_COMPARE_ALWAYS; mode <= CODE_VALIDITY_NONE; ++mode)
        for (enum engine_kind engine = ENGINE_INTERP; engine <= ENGINE_LOCKSTEP; ++engine) {
            CHECK(code_validity_configure(mode, CODE_SNAPSHOT_DEFAULT_BYTES));
            bundle(BASE, 0x11, nop, add(8,9,0), validator_branch(BASE, BASE));
            bundle(BASE + 256, 0x11, nop, add(11,9,0), validator_branch(BASE + 256, BASE + 256));
            bundle(BASE + 4096, 0x11, nop, nop, validator_branch(BASE + 4096, BASE));
            restart(v, engine, BASE); v->s.gr[9] = 11; v->s.gr[10] = 22;
            validator_run(v, 3); validator_run(v, 3);
            v->s.ip = BASE + 256; validator_run(v, 3); validator_run(v, 3);
            struct tier0_stats before, after; struct code_cache_stats cb, ca;
            tier0_get_stats(&before); code_cache_get_stats(&cb);
            code_notify_cpu_write(BASE + 512, 8); code_notify_vmm_write(BASE + 528, 8);
            v->s.ip = BASE; validator_run(v, 3);
            v->s.ip = BASE + 256; validator_run(v, 3);
            tier0_get_stats(&after); code_cache_get_stats(&ca);
            CHECK(after.translations == before.translations && after.invalidations == before.invalidations);
            CHECK(ca.decodes == cb.decodes && ca.invalidated_pages == cb.invalidated_pages);
            code_notify_cpu_write(BASE + 255, 2);
            dma_bundle(BASE + 256, 0x11, nop, add(11,10,0), validator_branch(BASE + 256, BASE + 256));
            v->s.ip = BASE; validator_run(v, 3); CHECK(v->s.gr[8] == 11);
            v->s.ip = BASE + 256; validator_run(v, 3); CHECK(v->s.gr[11] == 22);
            tier0_get_stats(&after);
            CHECK(after.translations == before.translations + (engine != ENGINE_INTERP));
            if (mode == CODE_VALIDITY_NONE) continue;
            /* Leave P, DMA into a second region, then validate only region A.
             * Region B's first entry in that stay must still compare its bytes. */
            v->s.ip = BASE + 4096; validator_run(v, 3);
            dma_bundle(BASE + 256, 0x11, nop, add(11,9,0), validator_branch(BASE + 256, BASE + 256));
            if (mode == CODE_VALIDITY_EPOCH_COMPARE) code_epoch_bump(CODE_DEVICE_STORAGE);
            validator_run(v, 3); CHECK(v->s.gr[8] == 11);
            v->s.ip = BASE + 256; validator_run(v, 3); CHECK(v->s.gr[11] == 11);
            /* An uncovered store ends the stay without retiring either region. */
            v->s.ip = BASE; validator_run(v, 3); tier0_get_stats(&before);
            code_notify_cpu_write(BASE + 512, 8);
            dma_bundle(BASE, 0x11, nop, add(8,10,0), validator_branch(BASE, BASE));
            if (mode == CODE_VALIDITY_EPOCH_COMPARE) code_epoch_bump(CODE_DEVICE_STORAGE);
            validator_run(v, 3); CHECK(v->s.gr[8] == 22);
            tier0_get_stats(&after);
            CHECK(after.translations == before.translations + (engine != ENGINE_INTERP));
        }
    /* Formation can abandon a source read without attaching a dependent.
     * Its snapshot must not survive an unnotified inline store in epoch mode. */
    CHECK(code_validity_configure(CODE_VALIDITY_EPOCH_COMPARE, CODE_SNAPSHOT_DEFAULT_BYTES));
    bundle(DATA, 0x11, nop, nop, validator_branch(DATA, DATA));
    bundle(BASE, 0x11, store(3,9,10), nop, validator_branch(BASE, BASE));
    restart(v, ENGINE_TIER0, BASE);
    uint64_t low, high, version;
    CHECK(code_source_read(v, DATA, &low, &high, &version).kind == IA64_STEP_COMPLETE);
    CHECK(code_page_is_tracked(DATA) && !code_page_has_dependencies(DATA));
    v->s.gr[9] = 0x1234; v->s.gr[10] = DATA;
    validator_run(v, 6);
    CHECK(code_source_read(v, DATA, &low, &high, &version).kind == IA64_STEP_COMPLETE);
    CHECK(low == 0x1234);
    test_stay_rules(v);
    test_epoch_provisional(v);
    test_stay_page_set(v);
    test_source_read_physical(v);
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
}
static void bench_bundle_stores(struct vcpu *v)
{
    for (unsigned tracked_page = 0; tracked_page < 2; ++tracked_page) {
        CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
        bundle(BASE, 0x11, store(0,9,10), nop, validator_branch(BASE, BASE));
        restart(v, ENGINE_TIER0, BASE);
        v->s.gr[9] = 42; v->s.gr[10] = tracked_page ? BASE + 512 : DATA;
        validator_run(v, 6);
        struct tier0_stats before, after; tier0_get_stats(&before);
        uint64_t start = hal_counter(); validator_run(v, 300000);
        uint64_t elapsed = hal_counter() - start; tier0_get_stats(&after);
        CHECK(after.translations == before.translations && after.invalidations == before.invalidations);
        printf("bundle-store tracked=%u stores=100000 ticks=%llu hz=%llu translations=%llu invalidations=%llu\n",
            tracked_page, (unsigned long long)elapsed, (unsigned long long)hal_counter_hz(),
            (unsigned long long)(after.translations - before.translations),
            (unsigned long long)(after.invalidations - before.invalidations));
    }
}
