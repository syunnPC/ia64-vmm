/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Four-page stays: which entries continue a stay, which pages
 * leave it alone, and the events that end it.
 */
/* A stay holds four pages. One engine_run call is one stay:
 * a cycle over four pages compares each page once; a cycle over five pages
 * finds the page it enters always to be the least recently entered one,
 * which left the stay, so every entry compares again. */
static uint64_t validator_cycle(struct vcpu *v, enum engine_kind engine, unsigned pages)
{
    for (unsigned i = 0; i < pages; ++i) {
        gpa_t at = BASE + i * 4096, next = BASE + (i + 1 == pages ? 0 : i + 1) * 4096;
        bundle(at, 0x11, nop, nop, validator_branch(at, next));
    }
    restart(v, engine, BASE);
    validator_run(v, 6 * pages); validator_run(v, 6 * pages);   /* translate, then seal */
    struct code_validity_stats before, after;
    struct code_stay_stats s_before, s_after;
    code_validity_get_stats(&before); code_stay_get_stats(&s_before);
    validator_run(v, 9 * pages);
    code_validity_get_stats(&after); code_stay_get_stats(&s_after);
    CHECK(v->s.ip == BASE && s_after.reset == s_before.reset + 1);
    CHECK(s_after.evicted - s_before.evicted == (pages > CODE_STAY_PAGES ? 3 * pages - CODE_STAY_PAGES : 0));
    return after.comparisons - before.comparisons;
}
static void validator_four_pages(struct vcpu *v)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    for (enum engine_kind engine = ENGINE_INTERP; engine <= ENGINE_TIER0; ++engine) {
        code_cache_flush();
        CHECK(validator_cycle(v, engine, 4) == 4);
        code_cache_flush();
        CHECK(validator_cycle(v, engine, 5) == 15);
    }
}
/* The whole stay's end. In one engine_run call P (BASE), Q
 * and R (+4 KiB, +8 KiB) run in turn; R's admission DMAs into P's compared
 * bundle (the validator's stay hook). An interruption, a device access or
 * an instruction-side translation change before P's next entry ends the
 * stay, so that entry compares and runs the new bundle (r8 = r10); without
 * one, P is still held and its old bundle runs (r8 = r9): the window. The
 * handlers return to P with rfi, so P's entry has its region's key. */
enum stay_end { STAY_END_NONE, STAY_END_EXTERNAL, STAY_END_FAULT, STAY_END_DEVICE_READ, STAY_END_DEVICE_WRITE, STAY_END_DIRECT_FP_STORE,
                STAY_END_ITC, STAY_END_RR, STAY_END_PKR, STAY_END_EPOCH, STAY_END_KINDS };
static struct vcpu *stay_end_vcpu;
static enum stay_end stay_end_kind;
static bool stay_end_armed;
static void stay_end_hook(gpa_t page, enum code_test_stage stage)
{
    if (!stay_end_armed || stage != CODE_TEST_STAY || page != BASE + 8192) return;
    stay_end_armed = false;
    dma_bundle(BASE, 0x11, nop, add(8,10,0), validator_branch(BASE, BASE + 4096));
    struct vcpu *v = stay_end_vcpu;
    if (stay_end_kind == STAY_END_EXTERNAL) vcpu_raise_external(v, 150);
    if (stay_end_kind == STAY_END_ITC) ia64_tc_insert(v, UINT64_C(0x8000000000), 0, (BASE + 0x5000) | 0x7e1, 12 << 2);
    if (stay_end_kind == STAY_END_PKR) { v->s.pkr[1] = (UINT64_C(0x4242) << 8) | 1; ia64_mmu_changed(v, IA64_MMU_PKR, 0, 1); }
    if (stay_end_kind == STAY_END_EPOCH) code_epoch_bump(CODE_DEVICE_STORAGE);
}
static void validator_stay_breaks(struct vcpu *v)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    const uint64_t iva = BASE + 0x18000;
    const uint64_t stf8 = UINT64_C(0xcc41604000), mov_rr = UINT64_C(0x2001730000), break_m = UINT64_C(0x40);
    const uint64_t to_iip = UINT64_C(0x216133c000), from_ivr = UINT64_C(0x21241007c0), rfi = UINT64_C(8) << 27;
    for (enum stay_end kind = STAY_END_NONE; kind < STAY_END_KINDS; ++kind) {
        code_cache_flush();
        for (unsigned offset = 0; offset < 0x8000; offset += 0x100) bundle(iva + offset, 1, nop, nop, nop);
        bundle(iva + 0x2c00, 0x11, to_iip, nop, rfi);     /* mov cr.iip=r30; rfi */
        bundle(iva + 0x3000, 0x11, from_ivr, nop, rfi);   /* mov r31=cr.ivr; rfi */
        bundle(BASE, 0x11, nop, add(8,9,0), validator_branch(BASE, BASE + 4096));
        bundle(BASE + 4096, 0x11, nop, nop, validator_branch(BASE + 4096, BASE + 8192));
        uint64_t m0 = nop, m1 = nop;
        if (kind == STAY_END_FAULT) m0 = break_m;
        if (kind == STAY_END_DEVICE_READ) m0 = load(3, 20, 21);
        if (kind == STAY_END_DEVICE_WRITE) m1 = store(3, 20, 21);
        if (kind == STAY_END_DIRECT_FP_STORE) m0 = stf8;
        if (kind == STAY_END_RR) m1 = mov_rr;
        bundle(BASE + 8192, 0x18, m0, m1, validator_branch(BASE + 8192, BASE));
        restart(v, ENGINE_TIER0, BASE);
        v->s.psr = IA64_PSR_IC | IA64_PSR_I; v->s.cr[CR_IVA] = iva;
        v->s.gr[9] = 11; v->s.gr[10] = 22; v->s.gr[21] = DEVICE; v->s.gr[22] = DEVICE + 4096;
        v->s.gr[23] = UINT64_C(3) << 61; v->s.gr[24] = (UINT64_C(0x777) << 8) | (12 << 2); v->s.gr[30] = BASE;
        validator_run(v, 9); validator_run(v, 9);   /* translate, then seal */
        v->s.ip = BASE; v->s.slot = 0; v->s.gr[8] = 0; v->s.psr = IA64_PSR_IC | IA64_PSR_I;
        stay_end_vcpu = v; stay_end_kind = kind; stay_end_armed = true;
        code_validity_set_test_hook(stay_end_hook);
        uint64_t budget = kind == STAY_END_EXTERNAL ? 15 : 12;
        CHECK(engine_run(v, budget).reason == VCPU_EXIT_BUDGET);
        code_validity_set_test_hook(NULL);
        CHECK(!stay_end_armed && v->s.ip == BASE + 4096);
        if (kind == STAY_END_NONE) CHECK(v->s.gr[8] == 11);
        else CHECK(v->s.gr[8] == 22);
        if (kind == STAY_END_EXTERNAL) CHECK(v->s.gr[31] == 150);
    }
}
/* A notified store into P makes P alone leave the stay; a region entry on P
 * with another fetch translation (another alias) too. The stay continues:
 * one stay per call, the other page admitted once. */
static void validator_page_leaves(struct vcpu *v)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    code_cache_flush();
    bundle(BASE, 0x11, nop, nop, validator_branch(BASE, BASE + 4096));
    bundle(BASE + 4096, 0x18, store(3, 20, 21), nop, validator_branch(BASE + 4096, BASE));
    restart(v, ENGINE_TIER0, BASE);
    v->s.gr[21] = BASE + 0x800; v->s.gr[20] = 0;
    validator_run(v, 6); validator_run(v, 6);
    struct code_stay_stats a, b;
    code_stay_get_stats(&a);
    validator_run(v, 12);
    code_stay_get_stats(&b);
    CHECK(b.reset == a.reset + 1 && b.event == a.event + 1 && b.page == a.page + 2 && b.fetch == a.fetch);
    /* Aliases: P through region 0 and region 1 (other RR), Q through region 0
     * only; P branches to Q through b7, Q to P's second alias through b6. */
    const uint64_t a1 = UINT64_C(0x2000000), a2 = (UINT64_C(1) << 61) | a1;
    const uint64_t br_b6 = UINT64_C(0x10000d000), br_b7 = UINT64_C(0x10000f000);
    code_cache_flush();
    bundle(BASE, 0x11, nop, nop, br_b7);
    bundle(BASE + 4096, 0x11, nop, nop, br_b6);
    restart(v, ENGINE_TIER0, a1); v->s.psr = IA64_PSR_IT | IA64_PSR_IC;
    v->s.rr[0] = 0x111100 | 48; v->s.rr[1] = 0x222200 | 48;
    virtual_map(v, 0, 0, a1, BASE, 12, FETCH_FLAGS);
    virtual_map(v, 0, 1, a2, BASE, 12, FETCH_FLAGS);
    virtual_map(v, 0, 2, a1 + 4096, BASE + 4096, 12, FETCH_FLAGS);
    ia64_mmu_changed(v, IA64_MMU_ALL, 0, 0);
    v->s.br[6] = a2; v->s.br[7] = a1 + 4096;
    validator_run(v, 12); validator_run(v, 12);
    v->s.ip = a1; v->s.slot = 0;
    code_stay_get_stats(&a);
    validator_run(v, 12);
    code_stay_get_stats(&b);
    CHECK(b.reset == a.reset + 1 && b.fetch == a.fetch + 1 && b.page == a.page + 2 && b.event == a.event);
}
/* A four-page entry (a Tier1 region's page set) while four other pages are
 * held: its pages join and the four others leave, never one of its own; the
 * next entry finds all four held and compares nothing. */
static unsigned page_set_retired;
static void page_set_retire(struct code_dependency *d) { (void)d; ++page_set_retired; }
static void validator_page_set_entry(struct vcpu *v)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    code_cache_flush();
    const struct code_fetch f = {.psr = IA64_PSR_IT, .rr = 1, .pta = 2, .pte = 0x100661};
    struct code_dependency deps[8];
    restart(v, ENGINE_TIER0, BASE);
    for (unsigned i = 0; i < 8; ++i) {
        gpa_t page = BASE + i * 4096;
        bundle(page, 0x11, nop, nop, validator_branch(page, page));
        uint64_t low, high, generation;
        CHECK(code_source_read(v, page, &low, &high, &generation).kind == IA64_STEP_COMPLETE);
        deps[i] = (struct code_dependency){.invalidate = page_set_retire, .mask = {1}};
        CHECK(code_dependency_publish(v, page, generation, &deps[i]));
        code_dependency_attach(page, &deps[i]);
    }
    page_set_retired = 0;
    for (unsigned i = 4; i < 8; ++i) CHECK(code_dependency_enter(v, &deps[i], &f));
    struct code_validity_stats a, b;
    struct code_stay_stats sa, sb;
    code_validity_get_stats(&a); code_stay_get_stats(&sa);
    CHECK(code_dependencies_enter(v, deps, 4, &f));
    CHECK(code_dependencies_enter(v, deps, 4, &f));
    code_validity_get_stats(&b); code_stay_get_stats(&sb);
    CHECK(b.comparisons == a.comparisons + 4 && sb.evicted == sa.evicted + 4 && sb.reset == sa.reset);
    CHECK(!page_set_retired);
    for (unsigned i = 0; i < 8; ++i) code_dependency_remove(&deps[i]);
}
/* A stay that ends clears its page records: a boundary-cache hint naming
 * the record a page held in the previous stay matches nothing in the next
 * one. In one engine_run call P0..P3 run in turn; P3's device read ends the
 * first stay. P0's admission to the second stay DMAs into P3, whose next
 * entry (a boundary-cache hit with the first stay's hint) compares it. */
static unsigned stale_record_admissions;
static void stale_record_hook(gpa_t page, enum code_test_stage stage)
{
    if (stage != CODE_TEST_STAY || page != BASE || ++stale_record_admissions != 2) return;
    dma_bundle(BASE + 3 * 4096, 0x18, load(3, 20, 21), add(8,10,0), validator_branch(BASE + 3 * 4096, BASE));
}
static void validator_stale_record(struct vcpu *v)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    code_cache_flush();
    for (unsigned i = 0; i < 3; ++i)
        bundle(BASE + i * 4096, 0x11, nop, nop, validator_branch(BASE + i * 4096, BASE + (i + 1) * 4096));
    bundle(BASE + 3 * 4096, 0x18, load(3, 20, 21), add(8,9,0), validator_branch(BASE + 3 * 4096, BASE));
    restart(v, ENGINE_TIER0, BASE);
    v->s.gr[9] = 11; v->s.gr[10] = 22; v->s.gr[21] = DEVICE;
    validator_run(v, 12); validator_run(v, 12); validator_run(v, 12);   /* translate, seal, fill */
    v->s.gr[8] = 0; stale_record_admissions = 0;
    code_validity_set_test_hook(stale_record_hook);
    validator_run(v, 24);
    code_validity_set_test_hook(NULL);
    CHECK(stale_record_admissions == 2 && v->s.ip == BASE && v->s.gr[8] == 22);
}
static void test_stays(struct vcpu *v)
{
    validator_four_pages(v);
    validator_stay_breaks(v);
    validator_page_leaves(v);
    validator_page_set_entry(v);
    validator_stale_record(v);
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
}
