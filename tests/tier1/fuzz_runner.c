/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * build/tier1-fuzz-runner: differential execution of t1fuzz.py programs.
 *   stdin:  IMAGE LOOP SEED (one per line; LOOP = t1fuzz_loop, hex)
 *   stdout: IMAGE ok | FAIL <why> | skip <why>
 *   exit:   0 if no program failed, 1 otherwise
 *
 * Compare each engine with the logged stepping reference for whole runs,
 * random budget splits and a forced verification exit (in place and at end).
 * Move stacked registers to another BOF, often wrapped; the original compiled
 * header region must serve it without recompiling. The product path also tests
 * a random entry kick and forced exit; T1H_CHAIN=off disables chaining.
 * T1H_FP selects FP modes (ia64/hostfp.h): check, soft, experimental, approx,
 * nofma3. In check mode each host result is checked against soft-float.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/log_internal.h"
#include "engine/cache/validity.h"
#include "tests/tier1/engines.h"
#include "tests/tier1/hostwin_linux.h"
#include "tests/tier1/observe.h"
#include "tests/tier1/provider.h"
#include "tests/tier1/reference.h"
#include "tests/tier1/results.h"
#include "tests/tier1/storelog.h"
#include "tests/tier1/bof.h"
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/engine_tier0t1.h"
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"
#include "ia64/hostfp.h"

#define ENTRY UINT64_C(0x100000)
#define BUDGET UINT64_C(2000000)
#define TO_LOOP UINT64_C(200000)
/* Capture every materialized verification-region exit during a forced run,
 * including regions compiled by hotness and exits at calls/returns. */
#define FORCED_CAPTURES 4096u

static struct t1h_logs ref_logs, logs;
static struct snapshot snap, moved;
static struct run_result reference, actual;
static struct observer obs;
static char why[512];
static const char *forced_what; /* the forced case of the program, for the ok line */

static uint64_t rnd(uint64_t *s)
{
    *s += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t x = *s;
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

/* The fork's raw-image reset state, as tests/fuzz/runner.c sets it. */
static void reset(struct vcpu *v)
{
    struct ia64_entry_regs regs = {
        .ip = ENTRY, .rsc = 3, .br0 = ENTRY, .bsp = 0x6e00000, .bspstore = 0x6e00000};
    regs.gr[1] = ENTRY;
    regs.gr[12] = 0x77ffff0;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = 0x10000;
    v->s.cr[CR_PTA] = 0x3c;
}

static bool equal(const char *label, bool stores)
{
    if (!result_compare(&reference, &actual, stores, label))
        return true;
    snprintf(why, sizeof(why), "%s differs from the reference", label);
    return false;
}

/* engine e on v from the snapshot: whole, then split at k1 < k2; with the
 * store log too unless max_stores is 0 (the checked engine: Tier0's
 * lockstep owns the memory hook the store log uses). */
static bool engine_runs(const struct t1h_engine *e, struct vcpu *v, uint64_t total, uint64_t *seed,
                        unsigned max_stores)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, e, v);
    bool ok = true;
    for (unsigned stores = 0; stores <= max_stores && ok; ++stores) {
        logs.stores_on = stores;
        snapshot_restore(&snap, v);
        t1h_engine_restored(&ev);
        struct vcpu_exit x = t1h_engine_run(&ev, BUDGET);
        result_take(&actual, &x, v);
        char label[64];
        snprintf(label, sizeof(label), "%s%s whole", e->name, stores ? "+stores" : "");
        ok = equal(label, stores);
        if (!ok || !total)
            break;
        uint64_t k1 = rnd(seed) % total, k2 = k1 + rnd(seed) % (total - k1 + 1);
        snapshot_restore(&snap, v);
        t1h_engine_restored(&ev);
        x = t1h_engine_run(&ev, k1);
        if (x.reason == VCPU_EXIT_BUDGET)
            x = t1h_engine_run(&ev, k2 - k1);
        if (x.reason == VCPU_EXIT_BUDGET)
            x = t1h_engine_run(&ev, BUDGET - k2);
        result_take(&actual, &x, v);
        snprintf(label, sizeof(label), "%s%s split %llu/%llu", e->name, stores ? "+stores" : "",
                 (unsigned long long)k1, (unsigned long long)k2);
        ok = equal(label, stores);
    }
    storelog_detach(v);
    t1h_engine_release(&ev);
    return ok;
}

static void on_restore(struct t1_vcpu *tv, const struct vcpu *v, const struct t1_restore_record *r,
                       void *arg)
{
    observe_t1_restore(tv, v, r, arg);
}

/* One random forced-exit case in a verification region at the loop header:
 * compiled from the first snapshot, run (and compared in place) from `run`
 * -- the first snapshot or the moved one. The case drawn from *seed is
 * returned in *pick / *act / *occ, or taken from there when *pick != -1. */
static bool forced_case(const struct t1h_provider *pv, struct vcpu *ref, struct vcpu *v,
                        const struct snapshot *run, uint64_t *seed, int *pick, unsigned *act,
                        uint64_t *occ)
{
    bool ok = true;
    forced_what = "no region at the loop header";
    snapshot_restore(&snap, v);
    void *ctx = pv->create(v);
    struct t1_compile_report rep;
    struct t1_compile_options opt = {.flags = T1_COMPILE_VERIFY | T1_COMPILE_STORE_LOG};
    if (pv->compile(ctx, &opt, &rep) == T1_COMPILED) {
        struct t1_point_info pts[T1_MAX_POINTS];
        unsigned n = pv->point_count(ctx, rep.region_id), candidates = 0;
        for (unsigned i = 0; i < n && i < T1_MAX_POINTS; ++i) {
            struct t1_point_info p;
            if (pv->point_get(ctx, rep.region_id, i, &p) &&
                (p.kind == T1_PT_SITE || p.kind == T1_PT_HELPER))
                pts[candidates++] = p;
        }
        if (candidates) {
            static const enum t1_force_action site[] = {T1_FORCE_TRAP_RETRY, T1_FORCE_TRAP_EXIT,
                                                        T1_FORCE_MISS_EXIT};
            if (*pick < 0) {
                *pick = (int)(rnd(seed) % candidates);
                const struct t1_point_info *p = &pts[*pick];
                *act = p->kind == T1_PT_SITE ? site[rnd(seed) % 3] : T1_FORCE_HELPER_EXIT;
                *occ = 1 + rnd(seed) % 2;
            }
            T1H_CHECK((unsigned)*pick < candidates);
            const struct t1_point_info *p = &pts[*pick];
            enum t1_force_action a = (enum t1_force_action) * act;
            static const char *const names[] = {"forced TRAP_RETRY", "forced TRAP_EXIT",
                                                "forced MISS_EXIT", "forced HELPER_EXIT"};
            forced_what = names[a];
            struct schedule sched;
            schedule_clear(&sched);
            observe_init(&obs, FORCED_CAPTURES, &sched);
            logs.stores_on = true;
            snapshot_restore(run, v);
            pv->restored(ctx);
            pv->set_restore_observer(ctx, on_restore, &obs);
            pv->set_store_observer(ctx, storelog_t1_store, &logs);
            T1H_CHECK(pv->force(ctx, rep.region_id, p->point_id, *occ, a, NULL, NULL));
            struct vcpu_exit x = pv->run(ctx, BUDGET);
            pv->set_restore_observer(ctx, NULL, NULL);
            pv->set_store_observer(ctx, NULL, NULL);
            result_take(&actual, &x, v);
            if (getenv("T1FUZZ_CAPTURES"))
                fprintf(stderr, "forced case: %u captures\n", obs.n);
            char label[64];
            snprintf(label, sizeof(label), "%s forced%s", pv->name,
                     run == &snap ? "" : " at the moved BOF");
            ok = equal(label, true);
            if (ok && observe_verify(&obs, run, ref, &sched, "tier1 forced in place")) {
                snprintf(why, sizeof(why), "tier1 forced point %u action %u differs in place%s",
                         p->point_id, a, run == &snap ? "" : " at the moved BOF");
                ok = false;
            }
            observe_free(&obs);
        }
    }
    pv->destroy(ctx);
    storelog_detach(v);
    return ok;
}

/* A kick at a random Tier1 entry of the product path: Tier0 yields at its
 * next boundary and the run goes on (the engine's run calls again). */
static struct {
    unsigned at, left;
    bool kicked;
} kick_at;
static void kick_hook(struct t1_vcpu *tv, uint32_t region, void *arg)
{
    (void)region;
    (void)arg;
    if (!kick_at.kicked && !--kick_at.left) {
        kick_at.kicked = true;
        vcpu_kick(tv->v);
    }
}

static bool kick_case(struct vcpu *v, uint64_t *seed)
{
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier0t1(), v);
    logs.stores_on = false;
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    kick_at.at = kick_at.left = 1 + (unsigned)(rnd(seed) % 8);
    kick_at.kicked = false;
    t1_test_set_entry_hook(kick_hook, NULL);
    struct vcpu_exit x = t1h_engine_run(&ev, BUDGET);
    t1_test_set_entry_hook(NULL, NULL);
    result_take(&actual, &x, v);
    bool ok = equal("tier0+tier1 kicked", false);
    t1h_engine_release(&ev);
    return ok;
}

/* The valid verification region (flags VERIFY, as the tier1 engine's runs
 * without the store log compile it) at the header with the snapshot's CFM,
 * or NULL. */
static const struct t1_region *header_region(uint64_t loop)
{
    for (uint32_t id = 1; id <= t1_cfg.max_regions; ++id) {
        const struct t1_region *r = t1_region_get(id);
        if (r && r->valid && r->c.ip == loop && r->c.slot == 0 && r->c.cfm == snap.v.s.cfm &&
            r->c.flags == T1_COMPILE_VERIFY)
            return r;
    }
    return NULL;
}

static uint64_t moved_loop;
static unsigned moved_compiles;
static void on_compile(const struct vcpu *v, enum t1_compile_status st,
                       const struct t1_compile_report *rep, void *arg)
{
    (void)rep;
    (void)arg;
    if (v->s.ip == moved_loop && v->s.slot == 0 && st == T1_COMPILED)
        ++moved_compiles;
}

static unsigned exits_from(struct t1_vcpu *tv, uint32_t id)
{
    unsigned n = 0;
    struct t1_exit_info xi;
    for (unsigned k = 0; k < t1_exit_count(tv); ++k)
        if (t1_exit_get(tv, k, &xi) && id && xi.region_id == id)
            ++n;
    return n;
}

/* Tier1 from the moved snapshot, whole, against the reference from there.
 * The header region of the first BOF (if hotness compiled one) must serve
 * the moved BOF exactly as the first: no compile at the header, the same
 * number of its exits as a control run from the first snapshot (the
 * region is not entered at all when its NaT guard always fails, e.g. on a
 * NaT sink). *reused: it was entered at both. */
static bool moved_run(struct vcpu *ref, struct vcpu *v, uint64_t loop, bool *reused)
{
    const struct t1_region *r = header_region(loop);
    uint32_t id = r ? r->id : 0;
    /* With the store log: the forced case at the moved BOF compares it. */
    ref_logs.stores_on = true;
    snapshot_restore(&moved, ref);
    struct vcpu_exit rx = ref_run(ref, BUDGET, NULL);
    if (!t1h_logs_ok(&ref_logs)) {
        snprintf(why, sizeof(why), "reference at the moved BOF: %s", ref_logs.error);
        return false;
    }
    result_take(&reference, &rx, ref);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, t1h_engine_tier1(), v);
    struct t1_vcpu *tv = t1h_tier1_vcpu(ev.ctx);
    logs.stores_on = false;
    snapshot_restore(&snap, v);
    t1h_engine_restored(&ev);
    (void)t1h_engine_run(&ev, BUDGET);
    unsigned control = exits_from(tv, id);
    snapshot_restore(&moved, v);
    t1h_engine_restored(&ev);
    moved_loop = loop;
    moved_compiles = 0;
    t1_test_set_compile_observer(on_compile, NULL);
    struct vcpu_exit x = t1h_engine_run(&ev, BUDGET);
    t1_test_set_compile_observer(NULL, NULL);
    result_take(&actual, &x, v);
    bool ok = equal("tier1 at the moved BOF", false);
    unsigned mine = exits_from(tv, id);
    *reused = id && control && mine == control && !moved_compiles;
    if (ok && id && (mine != control || moved_compiles)) {
        snprintf(why, sizeof(why),
                 "the header region %u of the first BOF: %u exits at the moved BOF, %u at the "
                 "first, %u compiles at the header",
                 id, mine, control, moved_compiles);
        ok = false;
    }
    storelog_detach(v);
    t1h_engine_release(&ev);
    return ok;
}

/* "ok", "FAIL", or "skip" with why. */
static const char *program(const char *image, uint64_t loop, uint64_t seed)
{
    static char *path;
    path = (char *)image;
    t1h_set_images(1, &path);
    t1h_load_image(image);
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true, .itc_hz = 1000000};
    struct vcpu *ref = vcpu_create(&cfg), *v = vcpu_create(&cfg);
    T1H_CHECK(ref && v);
    t1h_bind(ref, &ref_logs);
    t1h_bind(v, &logs);
    reset(ref);
    const char *status = "ok";
    uint64_t steps = 0;
    while (!(ref->s.ip == loop && ref->s.slot == 0)) {
        if (++steps > TO_LOOP || ref_run(ref, 1, NULL).reason != VCPU_EXIT_BUDGET) {
            snprintf(why, sizeof(why), "the loop header was not reached");
            status = "skip";
            goto out;
        }
    }
    snapshot_take(&snap, ref);
    ref_logs.stores_on = true;
    snapshot_restore(&snap, ref);
    struct vcpu_exit x = ref_run(ref, BUDGET, NULL);
    if (!t1h_logs_ok(&ref_logs)) {
        snprintf(why, sizeof(why), "reference: %s", ref_logs.error);
        status = "FAIL";
        goto out;
    }
    result_take(&reference, &x, ref);
    uint64_t total = ref->retired - snap.v.retired;
    struct t1_stats c0, c1;
    t1_get_stats(&c0);
    for (unsigned k = 0; k < t1h_engine_count(); ++k)
        if (!engine_runs(t1h_engine_at(k), v, total, &seed, 1)) {
            status = "FAIL";
            goto out;
        }
    /* Regions holding inner loops: a checkpoint resuming
     * at a header other than the head. */
    unsigned nests = 0, calls = 0, callee_pages = 0;
    for (uint32_t id = 1; id <= t1_cfg.max_regions; ++id) {
        const struct t1_region *r = t1_region_get(id);
        bool inner = false;
        for (unsigned k = 0; r && r->valid && k < r->npoints && !inner; ++k)
            inner = r->point[k].kind == T1_PT_CHECKPOINT &&
                    (r->point[k].ip != r->c.ip || r->point[k].slot != r->c.slot);
        nests += inner;
        /* Regions holding a call or a return. */
        calls += r && r->valid && r->calls;
        /* Regions holding a page other than the entry page and the pages
         * after it in order: a callee's. */
        bool callee = false;
        for (unsigned k = 1; r && r->valid && k < r->npages && !callee; ++k)
            callee = r->page[k].ip_base != r->page[0].ip_base + UINT64_C(4096) * k;
        callee_pages += callee;
    }
    /* Allocs that spilled in a region, under every engine. */
    struct t1_stats c2;
    t1_get_stats(&c2);
    uint64_t spills = c2.spills - c0.spills;
    /* Checked Tier1 lockstep (engine/tier1/checked.c) on
     * the same program; a divergence ends the run unequal. */
    struct t1_stats s0, s1;
    t1_get_stats(&s0);
    bool equal = engine_runs(t1h_engine_checked(), v, total, &seed, 0);
    t1_get_stats(&s1);
    if (!equal || s1.divergences != s0.divergences) {
        status = "FAIL";
        goto out;
    }
    uint64_t checked = s1.checked - s0.checked;
    int pick = -1, product_pick = -1;
    unsigned act = 0, product_act = 0;
    uint64_t occ = 0, product_occ = 0;
    if (!kick_case(v, &seed) || !forced_case(t1h_provider_tier0t1(), ref, v, &snap, &seed,
                                             &product_pick, &product_act, &product_occ)) {
        status = "FAIL";
        goto out;
    }
    const char *product_forced = forced_what;
    t1_get_stats(&c1);
    if (!forced_case(t1h_provider_real(), ref, v, &snap, &seed, &pick, &act, &occ)) {
        status = "FAIL";
        goto out;
    }
    /* The frame base moved. */
    unsigned sof = (unsigned)(snap.v.s.cfm & 127), from = snap.v.s.rse_bof % IA64_N_STACKED_PHYS;
    unsigned to = sof > 1 && rnd(&seed) % 2 ? 95 - (unsigned)(rnd(&seed) % (sof - 1))
                                            : (unsigned)(rnd(&seed) % 96);
    if (to == from)
        to = (to + 1) % 96;
    moved.v = snap.v;
    memcpy(moved.ram, snap.ram, T1H_RAM_SIZE);
    t1t_rebase_bof(&moved.v.s, to);
    bool reused = false;
    if (!moved_run(ref, v, loop, &reused) ||
        (pick >= 0 &&
         !forced_case(t1h_provider_real(), ref, v, &moved, &seed, &pick, &act, &occ))) {
        status = "FAIL";
        goto out;
    }
    snprintf(
        why, sizeof(why),
        "%llu instructions from the loop header, %u engines and checked lockstep "
        "(%llu Tier1 executions checked), %u regions with inner loops, %u regions with calls, %u regions with "
        "callee pages, %llu in-region spills, bof %u sof %u%s, moved to bof %u%s%s, %llu chain links, kick at "
        "entry %u%s, product path %s, %s",
        (unsigned long long)total, t1h_engine_count(), (unsigned long long)checked, nests, calls,
        callee_pages, (unsigned long long)spills, from, sof, from + sof > 96 ? " wraps" : "", to,
        to + sof > 96 ? " wraps" : "", reused ? ", region reused" : "",
        (unsigned long long)(c1.chain_links - c0.chain_links), kick_at.at,
        kick_at.kicked ? "" : " (not reached)", product_forced, forced_what);
out:
    storelog_detach(ref);
    storelog_detach(v);
    t1h_unbind(ref);
    t1h_unbind(v);
    vcpu_destroy(ref);
    vcpu_destroy(v);
    return status;
}

static void fp_mode(void)
{
    static const struct {
        const char *name;
        unsigned flag;
    } modes[] = {{"check", IA64_HOSTFP_CFG_CHECK},
                 {"soft", IA64_HOSTFP_CFG_SOFT},
                 {"experimental", IA64_HOSTFP_CFG_EXPERIMENTAL},
                 {"approx", IA64_HOSTFP_CFG_APPROX | IA64_HOSTFP_CFG_EXPERIMENTAL},
                 {"nofma3", IA64_HOSTFP_CFG_NOFMA3}};
    const char *env = getenv("T1H_FP");
    unsigned flags = 0;
    while (env && *env) {
        size_t n = strcspn(env, ",");
        unsigned k = 0;
        while (k < sizeof(modes) / sizeof(modes[0]) &&
               (strlen(modes[k].name) != n || strncmp(modes[k].name, env, n)))
            ++k;
        if (k == sizeof(modes) / sizeof(modes[0])) {
            fprintf(stderr, "T1H_FP: unknown mode %.*s\n", (int)n, env);
            exit(2);
        }
        flags |= modes[k].flag;
        env += n + (env[n] == ',');
    }
    ia64_hostfp_configure(flags);
}

int main(void)
{
    log_set_level(LOG_ERROR);
    fp_mode();
    t1h_set_ram_allocator(hostwin_linux_ram);
    if (!hostwin_linux_init()) {
        fprintf(stderr, "tier1-fuzz-runner: hostwin_linux_init failed\n");
        return 2;
    }
    code_validity_set_track_hook(hostwin_code_page_event);
    t1_test_isel_crosscheck = true;
    t1_test_workspace_poison = true;
    t1h_init();
    /* The fuzz data: .data words, the store window above them, and the
     * page below (decrementing load pointers). */
    static const struct storelog_range data[] = {{0x10f000, 0x112000}};
    storelog_set_ranges(data, 1);
    snapshot_init(&snap);
    snapshot_init(&moved);
    result_init(&reference);
    result_init(&actual);
    char line[4096], image[4000];
    unsigned long long loop, seed;
    int failed = 0;
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "%3999s %llx %llu", image, &loop, &seed) != 3) {
            fprintf(stderr, "tier1-fuzz-runner: bad line: %s", line);
            return 2;
        }
        why[0] = 0;
        const char *status = program(image, loop, seed);
        failed |= !strcmp(status, "FAIL");
        printf("%s %s%s%s\n", image, status, why[0] ? " " : "", why);
        fflush(stdout);
    }
    result_free(&reference);
    result_free(&actual);
    snapshot_free(&moved);
    snapshot_free(&snap);
    return failed;
}
