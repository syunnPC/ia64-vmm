/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine/cache/validity.h"
#include "engine/cache/cache.h"
#include "engine/interp/interp.h"
#include "ia64/pal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

/* Exhaustion below the cap must evict, while an
 * empty cache must stop at the fetch without advancing architectural state. */
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            LOGE("%s:%u: %s", __FILE__, __LINE__, #x);                                             \
            hal_panic("snapshot reserve test failed");                                             \
        }                                                                                          \
    } while (0)
static _Alignas(16) uint8_t ram[65536];
static unsigned allocation_calls, fail_at;
static bool fail_all;
static bool fail_metadata;
void *__real_hal_alloc_pages(size_t pages, unsigned flags);
void *__wrap_hal_alloc_pages(size_t pages, unsigned flags)
{
    ++allocation_calls;
    if (fail_all || (fail_metadata && (flags & HAL_PAGE_ZERO)) ||
        (fail_at && allocation_calls == fail_at))
        return NULL;
    return __real_hal_alloc_pages(pages, flags);
}
static struct ia64_result read_page(unsigned page, uint64_t *version)
{
    uint64_t low = 0, high = 0;
    struct ia64_result r = code_source_read_physical(page * 4096, &low, &high, version);
    if (r.kind == IA64_STEP_COMPLETE)
        CHECK(low == ram[page * 4096] && high == 0);
    return r;
}
static unsigned invalidations;
static void invalidated(struct code_dependency *d)
{
    CHECK(!d->page);
    ++invalidations;
}
static void reset(void)
{
    fail_all = fail_metadata = false;
    fail_at = 0;
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, 48u << 20));
    allocation_calls = invalidations = 0;
}
static void empty_failure(unsigned at)
{
    reset();
    /* The bitmap is allocated at configure; next come the slab and page. */
    fail_at = at;
    uint64_t version = UINT64_MAX;
    struct ia64_result r = read_page(2, &version);
    CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_BAD_ACCESS);
    CHECK(r.info == 8192 && !strcmp(r.detail, "code snapshot allocation failed"));
    CHECK(version == UINT64_MAX && !code_page_has_snapshot(8192));
}
static void pressure(bool grace)
{
    reset();
    uint64_t version, old_version;
    CHECK(read_page(0, &old_version).kind == IA64_STEP_COMPLETE);
    CHECK(read_page(2, &version).kind == IA64_STEP_COMPLETE);
    CHECK(read_page(3, &version).kind == IA64_STEP_COMPLETE);
    struct code_dependency d = {.invalidate = invalidated, .mask = {1}};
    code_dependency_attach(0, &d);
    struct code_dependency d2 = {.invalidate = invalidated, .mask = {1}};
    code_dependency_attach(8192, &d2);
    struct vcpu v = {0};
    if (grace)
        code_register_vcpu(&v);
    fail_all = true;
    struct ia64_result r = read_page(1, &version);
    if (grace) {
        CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_BUDGET && r.info == 4096);
        CHECK(!code_page_has_snapshot(4096) && invalidations == 1);
        /* A second try cannot restamp the victim or retire other pages. */
        r = read_page(1, &version);
        CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_BUDGET);
        CHECK(invalidations == 1 && d2.page);
        code_quiescent(&v);
        r = read_page(1, &version);
    }
    CHECK(r.kind == IA64_STEP_COMPLETE && invalidations == 1 && !d.page);
    CHECK(!code_page_has_snapshot(0) && code_page_has_snapshot(4096));
    CHECK(!code_source_publish(&v, 0, old_version));
    struct code_validity_stats s;
    code_validity_get_stats(&s);
    CHECK(s.snapshot_bytes == 3 * 4096);
    code_dependency_remove(&d2);
    if (grace)
        code_unregister_vcpu(&v);
    fail_all = false;
}
static void budget(void)
{
    reset();
    CHECK(code_validity_reserve_budget(UINT64_MAX, 0) == (48u << 20));
    CHECK(code_validity_reserve_budget(64u << 20, 0) == (32u << 20));
    CHECK(code_validity_reserve_budget(UINT64_MAX, UINT64_C(16) << 30) == (UINT64_C(1) << 30));
    CHECK(code_validity_reserve_budget(UINT64_MAX, UINT64_C(1) << 30) == (64u << 20));
    CHECK(code_validity_reserve_budget(UINT64_C(128) << 20, UINT64_C(16) << 30) == (64u << 20));

    CHECK(code_validity_reserve_budget(32767, 0) == 12288);
    CHECK(code_validity_reserve_budget(0, 0) == 8192);
    CHECK(code_validity_reserve_budget(16384, 0) == 8192);
    uint64_t version;
    CHECK(read_page(0, &version).kind == IA64_STEP_COMPLETE);
    CHECK(read_page(1, &version).kind == IA64_STEP_COMPLETE);
    CHECK(read_page(2, &version).kind == IA64_STEP_COMPLETE);
    struct code_validity_stats s;
    code_validity_get_stats(&s);
    CHECK(s.snapshot_bytes == 4096);
    CHECK(s.metadata_bytes == 4096);
}
static void slab_pressure(void)
{
    reset();
    uint64_t version;
    CHECK(read_page(0, &version).kind == IA64_STEP_COMPLETE);
    fail_metadata = true;
    /* Fill the existing slab, then require eviction without a new slab. */
    for (unsigned i = 1; i < 16; ++i)
        CHECK(read_page(i, &version).kind == IA64_STEP_COMPLETE);
    struct code_validity_stats s;
    code_validity_get_stats(&s);
    CHECK(s.snapshot_bytes < sizeof(ram) && code_page_has_snapshot(15 * 4096));
    fail_metadata = false;
}
static void exact_stop(void)
{
    reset();
    struct vcpu *v = vcpu_create(&(struct vcpu_config){.engine = ENGINE_INTERP});
    CHECK(v);
    v->s.ip = 8192;
    v->s.slot = 1;
    v->retired = 17;
    v->s.gr[8] = 123;
    fail_all = true;
    struct vcpu_exit e = engine_run(v, 1);
    CHECK(e.reason == VCPU_EXIT_BAD_ACCESS && e.ip == 8192 && e.slot == 1 && e.info == 8192);
    CHECK(!strcmp(e.detail, "code snapshot allocation failed"));
    CHECK(v->s.ip == 8192 && v->s.slot == 1 && v->retired == 17 && v->s.gr[8] == 123);
    fail_all = false;
    vcpu_destroy(v);
}
/* A derived cap learns exhaustion once. An explicit cap remains authoritative. */
static void no_rescan(bool explicit, bool metadata)
{
    reset();
    if (!explicit)
        code_validity_reserve_budget(64u << 20, 0);
    uint64_t version;
    CHECK(read_page(0, &version).kind == IA64_STEP_COMPLETE);
    if (metadata) {
        CHECK(read_page(2, &version).kind == IA64_STEP_COMPLETE);
        CHECK(read_page(3, &version).kind == IA64_STEP_COMPLETE);
    }
    fail_metadata = metadata;
    fail_all = !metadata;
    CHECK(read_page(1, &version).kind == IA64_STEP_COMPLETE);
    unsigned calls = allocation_calls;
    for (unsigned i = 2; i < 16; ++i)
        CHECK(read_page(i, &version).kind == IA64_STEP_COMPLETE);
    if (explicit)
        CHECK(allocation_calls > calls);
    else
        CHECK(allocation_calls == calls);
    fail_all = fail_metadata = false;
}
/* Schedule both registered executors at the exact eviction boundary. The
 * decoder must yield before a quiescent point can satisfy the retirement. */
static void executors(void)
{
    reset();
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, 16384));
    struct vcpu *x = vcpu_create(&(struct vcpu_config){.engine = ENGINE_INTERP});
    struct vcpu *y = vcpu_create(&(struct vcpu_config){.index = 1, .engine = ENGINE_TIER0});
    CHECK(x && y);
    const struct ia64_bundle *bundle;
    CHECK(code_get_bundle(x, 0, &bundle).kind == IA64_STEP_COMPLETE);
    uint64_t version;
    CHECK(read_page(2, &version).kind == IA64_STEP_COMPLETE);
    CHECK(read_page(3, &version).kind == IA64_STEP_COMPLETE);
    code_register_vcpu(x);
    code_register_vcpu(y);
    /* Keep decoded storage available: only snapshot growth is forbidden. */
    struct ia64_result r = code_source_read(y, 4096, &version, &version, &version);
    CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_BUDGET);
    uint64_t retired = x->retired;
    r = code_get_bundle(x, 16, &bundle);
    CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_BUDGET);
    CHECK(!strcmp(r.detail, "code snapshot awaiting quiescence"));
    struct vcpu_exit e = interp_run(x, 1);
    CHECK(e.reason == VCPU_EXIT_BUDGET && x->retired == retired);
    code_quiescent(x);
    code_quiescent(y);
    /* The old reader cannot revive the victim ahead of its evictor. */
    r = code_get_bundle(x, 16, &bundle);
    CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_BUDGET);
    CHECK(read_page(1, &version).kind == IA64_STEP_COMPLETE);
    CHECK(!code_page_has_snapshot(0));
    r = code_get_bundle(x, 16, &bundle);
    CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_BUDGET);
    code_quiescent(x);
    code_quiescent(y);
    CHECK(code_get_bundle(x, 16, &bundle).kind == IA64_STEP_COMPLETE);
    code_unregister_vcpu(x);
    code_unregister_vcpu(y);
    vcpu_destroy(x);
    vcpu_destroy(y);
}
/* An interruption can redirect the retiring executor to its own victim
 * before it retries the original miss. It must still make progress. */
static void redirected_evictor(bool physical)
{
    reset();
    struct vcpu *v = vcpu_create(&(struct vcpu_config){.engine = ENGINE_INTERP});
    CHECK(v);
    const struct ia64_bundle *bundle;
    CHECK(code_get_bundle(v, 0, &bundle).kind == IA64_STEP_COMPLETE);
    code_register_vcpu(v);
    fail_all = true;
    uint64_t low, high, version;
    struct ia64_result r = physical ? code_source_read_physical(4096, &low, &high, &version)
                                    : code_source_read(v, 4096, &low, &high, &version);
    CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_BUDGET);
    code_quiescent(v);
    CHECK(code_get_bundle(v, 16, &bundle).kind == IA64_STEP_COMPLETE);
    fail_all = false;
    code_unregister_vcpu(v);
    vcpu_destroy(v);
}
/* A missed allocation must not strand its victim
 * when the evictor runs cached code and then halts instead of retrying. */
static void abandoned_eviction(bool parked)
{
    reset();
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, 16384));
    struct vcpu *x = vcpu_create(&(struct vcpu_config){.engine = ENGINE_INTERP});
    struct vcpu *y = vcpu_create(&(struct vcpu_config){.index = 1, .engine = ENGINE_INTERP});
    CHECK(x && y);
    const struct ia64_bundle *bundle;
    CHECK(code_get_bundle(x, 0, &bundle).kind == IA64_STEP_COMPLETE);
    CHECK(code_get_bundle(x, 8192, &bundle).kind == IA64_STEP_COMPLETE);
    uint64_t low, high, version;
    CHECK(read_page(3, &version).kind == IA64_STEP_COMPLETE);
    struct code_validity_stats before;
    code_validity_get_stats(&before);
    code_register_vcpu(x);
    code_register_vcpu(y);
    struct ia64_result r = code_source_read(x, 4096, &low, &high, &version);
    CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_BUDGET);
    r = code_source_read(y, 0, &low, &high, &version);
    CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_BUDGET);
    /* An interruption redirects X to cached C, never back to missing B. */
    CHECK(code_get_bundle(x, 8192, &bundle).kind == IA64_STEP_COMPLETE);
    if (parked) {
        x->s.gr[28] = PAL_HALT_LIGHT;
        r = ia64_pal_dispatch(x);
        CHECK(r.kind == IA64_STEP_COMPLETE && x->requested_exit == VCPU_EXIT_HALT + 1);
    }
    code_quiescent(x);
    code_quiescent(y);
    if (parked)
        __atomic_store_n(&x->parked, 1, __ATOMIC_SEQ_CST);
    /* Preserve the evictor's first retry, even with repeated peer polls. */
    for (unsigned i = 0; i < 8; ++i) {
        r = code_source_read(y, 0, &low, &high, &version);
        CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_BUDGET);
    }
    code_quiescent(y);
    if (!parked) {
        r = code_source_read(y, 0, &low, &high, &version);
        CHECK(r.kind == IA64_STEP_EXIT && r.exit == VCPU_EXIT_BUDGET);
        code_quiescent(x);
    }
    unsigned calls = allocation_calls;
    r = code_source_read(y, 0, &low, &high, &version);
    CHECK(r.kind == IA64_STEP_COMPLETE && low == ram[0] && high == 0);
    CHECK(allocation_calls == calls && !code_page_has_snapshot(4096));
    struct code_validity_stats s;
    code_validity_get_stats(&s);
    CHECK(s.snapshot_bytes == 3 * 4096 && s.evictions == before.evictions);
    code_unregister_vcpu(x);
    code_unregister_vcpu(y);
    vcpu_destroy(x);
    vcpu_destroy(y);
}
static struct vcpu clock_v;
static unsigned clock_visits;
static void hot_clock(gpa_t page, enum code_test_stage stage)
{
    if (stage != CODE_TEST_CLOCK)
        return;
    ++clock_visits;
    CHECK(clock_visits <= 6);
    CHECK(code_page_enter(&clock_v, page, false).kind == IA64_STEP_COMPLETE);
}
static void clock_bound(void)
{
    reset();
    uint64_t version;
    for (unsigned i = 0; i < 3; ++i)
        CHECK(read_page(i, &version).kind == IA64_STEP_COMPLETE);
    clock_visits = 0;
    code_validity_set_test_hook(hot_clock);
    fail_all = true;
    CHECK(read_page(3, &version).kind == IA64_STEP_COMPLETE);
    CHECK(clock_visits == 6);
    code_validity_set_test_hook(NULL);
    fail_all = false;
}
static void late_budget(void)
{
    reset();
    uint64_t empty = code_validity_reserve_budget(64u << 20, 0);
    struct vcpu *x = vcpu_create(&(struct vcpu_config){.engine = ENGINE_INTERP});
    struct vcpu *y = vcpu_create(&(struct vcpu_config){.index = 1, .engine = ENGINE_LOCKSTEP});
    CHECK(x && y);
    CHECK(code_cache_vcpu_init(x) && code_cache_vcpu_init(y));
    uint64_t occupied = code_validity_reserve_budget(64u << 20, 0);
    /* Two owners, 64 pages each; the default takes half after subtraction. */
    uint64_t decoded = 64u * ((256u * sizeof(struct ia64_bundle) + 4095) / 4096) * 4096;
    CHECK(occupied <= empty - decoded);
    vcpu_destroy(x);
    vcpu_destroy(y);
}
static void hash_capacity(void)
{
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, UINT64_C(1) << 30));
    struct code_validity_stats initial;
    code_validity_get_stats(&initial);
    CHECK(initial.metadata_bytes && !initial.snapshot_bytes);
    reset();
    uint64_t versions[16], addresses[16];
    for (unsigned i = 0; i < 16; ++i) {
        /* At the small hash, pages 0, 1024, 2048 and 3072 collide.
         * Growth must separate them and shrink must reunite them. */
        addresses[i] = (uint64_t)i * (UINT64_C(4) << 20);
        if (i)
            CHECK(gm_add_region(&(struct gm_region){
                .base = addresses[i], .size = 4096, .kind = GM_RAM, .hva = ram + i * 4096}));
        uint64_t low, high;
        CHECK(code_source_read_physical(addresses[i], &low, &high, &versions[i]).kind ==
              IA64_STEP_COMPLETE);
        CHECK(low == ram[i * 4096] && !high);
    }
    struct code_dependency d = {.invalidate = invalidated, .mask = {1}};
    code_dependency_attach(addresses[1], &d);
    struct code_validity_stats before, after;
    code_validity_get_stats(&before);
    struct vcpu v = {0};
    code_register_vcpu(&v);
    CHECK(code_validity_reserve_budget(UINT64_MAX, UINT64_C(16) << 30) == (UINT64_C(1) << 30));
    code_validity_get_stats(&after);
    CHECK(after.metadata_bytes > before.metadata_bytes);
    for (unsigned i = 0; i < 16; ++i)
        CHECK(code_source_publish(&v, addresses[i], versions[i]));
    CHECK(d.page && !invalidations);
    code_validity_invalidate(addresses[1], 16);
    CHECK(!d.page && invalidations == 1);
    CHECK(!code_source_publish(&v, addresses[1], versions[1]));
    uint64_t low, high;
    CHECK(code_source_read_physical(addresses[1], &low, &high, &versions[1]).kind ==
          IA64_STEP_COMPLETE);
    code_dependency_attach(addresses[1], &d);
    CHECK(code_validity_reserve_budget(UINT64_MAX, 0) == (48u << 20));
    code_validity_get_stats(&after);
    CHECK(after.metadata_bytes == before.metadata_bytes);
    for (unsigned i = 0; i < 16; ++i)
        CHECK(code_source_publish(&v, addresses[i], versions[i]));
    fail_all = true;
    CHECK(code_validity_reserve_budget(UINT64_MAX, UINT64_C(16) << 30) == (48u << 20));
    fail_all = false;
    CHECK(d.page && invalidations == 1);
    for (unsigned i = 0; i < 16; ++i)
        CHECK(code_source_publish(&v, addresses[i], versions[i]));
    code_validity_invalidate(addresses[1], 16);
    CHECK(!d.page && invalidations == 2);
    code_unregister_vcpu(&v);
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, UINT64_C(1) << 30));
    code_validity_get_stats(&after);
    CHECK(after.metadata_bytes && !after.snapshot_bytes);
    reset();
    code_validity_get_stats(&after);
    CHECK(!after.metadata_bytes && !after.snapshot_bytes);
}
int main(int argc, char **argv)
{
    CHECK(gm_add_region(&(struct gm_region){.size = sizeof(ram), .kind = GM_RAM, .hva = ram}));
    for (unsigned i = 0; i < 16; ++i)
        ram[i * 4096] = (uint8_t)(i + 1);
    if (argc == 1 || !strcmp(argv[1], "empty")) {
        empty_failure(1);
        empty_failure(2);
    }
    if (argc == 1 || !strcmp(argv[1], "pressure")) {
        pressure(false);
        pressure(true);
    }
    if (argc == 1 || !strcmp(argv[1], "budget"))
        budget();
    if (argc == 1 || !strcmp(argv[1], "slab"))
        slab_pressure();
    if (argc == 1 || !strcmp(argv[1], "exact"))
        exact_stop();
    if (argc == 1 || !strcmp(argv[1], "no-rescan")) {
        no_rescan(false, false);
        no_rescan(false, true);
    }
    if (argc == 1 || !strcmp(argv[1], "explicit")) {
        no_rescan(true, false);
        no_rescan(true, true);
    }
    if (argc == 1 || !strcmp(argv[1], "executors"))
        executors();
    if (argc == 1 || !strcmp(argv[1], "clock"))
        clock_bound();
    if (argc == 1 || !strcmp(argv[1], "redirected")) {
        redirected_evictor(false);
        redirected_evictor(true);
    }
    if (argc == 1 || !strcmp(argv[1], "abandoned")) {
        abandoned_eviction(true);
        abandoned_eviction(false);
    }
    if (argc == 1 || !strcmp(argv[1], "hash"))
        hash_capacity();
    if (argc == 1 || !strcmp(argv[1], "late"))
        late_budget();
    code_cache_flush();
    hal_debug_write("snapshot reserve: PASS\n", 23);
    return 0;
}
