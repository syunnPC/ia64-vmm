/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Usage: tier1-test --bench=ENGINE:PROGRAM[:SEGMENT] IMAGES...
 * Measure cold and warm guest instructions/second in one process per
 * (engine, program). Cold includes decoding, translation, validator snapshots
 * and TLB fills; warm uses the next segment. The store log is disabled to
 * exercise Tier0's inline physical accesses.
 *
 * make bench-tier1 runs five CPU-pinned trials and reports medians through
 * bench_report.py. Tier1 uses production compile flags (0); bench-t1 reports
 * explicit header-region compilation, static code/spills and warm counter
 * deltas for retired instructions, entries and window operations.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tests/tier1/engines.h"
#include "tests/tier1/programs.h"
#include "tests/tier1/provider.h"
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/engine_tier0t1.h"
#include "engine/tier0/internal.h"
#include "engine/tier1/region.h"

#define DEFAULT_SEGMENT UINT64_C(4000000)
#define START UINT64_C(10000)

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t thread_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* Parameters giving at least 2 * segment instructions before the end. */
static struct guest_params bench_params(const struct guest_program *p, uint64_t segment)
{
    struct guest_params q = guest_completion_params(p);
    if (p->kind == GUEST_SHLADD_XOR) {
        q.n = (unsigned)(2 * segment / 6 + 1000); /* 6 instructions per iteration */
    } else if (p->kind == GUEST_GATHER) {
        /* Gather: one table page per iteration, 48 pages at most. */
        q.n = (p->flags & GUEST_STRIDE) ? 40 : 64;
        q.bench = true;                                      /* output restarts every pass */
        q.passes = (unsigned)(2 * segment / (q.n * 12) + 2); /* >= 12 per iteration */
    }
    return q;
}

int bench_main(const char *spec)
{
    char engine_name[32], program_name[64];
    unsigned long long segment = DEFAULT_SEGMENT;
    if (sscanf(spec, "%31[^:]:%63[^:]:%llu", engine_name, program_name, &segment) < 2 || !segment) {
        fprintf(stderr, "bench: expected ENGINE:PROGRAM[:SEGMENT], got %s\n", spec);
        return 2;
    }
    const struct t1h_engine *engine = t1h_engine_named(engine_name);
    const struct guest_program *p = guest_program(program_name);
    if (!engine || !p) {
        fprintf(stderr, "bench: unknown engine or program in %s\n", spec);
        return 2;
    }
    if (p->kind == GUEST_SPEED && segment * 2 > 8000000)
        segment = 4000000; /* speed.S: 9M */
    struct guest_params q = bench_params(p, segment);
    /* The product's boundary cache, without the hosted full-path check. */
    tier0_test_set_boundary_mode(TIER0_BOUNDARY_ON);
    struct vcpu_config cfg = guest_vcpu_config(p);
    struct vcpu *v = vcpu_create(&cfg);
    T1H_CHECK(v != NULL);
    static struct t1h_logs logs;
    t1h_bind(v, &logs);
    guest_setup(p, &q, v);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, engine, v);
    const struct t1h_provider *pv = !strcmp(engine->name, "tier1") ? t1h_provider_real() : NULL;
    if (pv)
        t1h_tier1_set_flags(0); /* production-shaped regions */
    /* The product path: production regions compiled by
     * its own hot(), Tier0 as the outer loop; the counters of the warm
     * segment are reported like Tier1's. */
    bool product = !strcmp(engine->name, "tier0+tier1");
    if (product)
        t1h_tier0t1_set_flags(0);
    struct t1_compile_report report = {0};
    uint64_t compile_ns = 0;
    if (pv) {
        /* The loop's region, compiled explicitly for its report (compile
         * time, code quality, spills); the run then enters it. */
        uint64_t header;
        T1H_CHECK(t1h_run_to_loop_header(v, 1000000, &header));
        uint64_t t0 = now_ns();
        struct t1_compile_options opt = {0};
        if (pv->compile(ev.ctx, &opt, &report) != T1_COMPILED)
            report.region_id = UINT32_MAX;
        compile_ns = now_ns() - t0;
    }

    /* [0, START): start-up; [0, segment): cold; [segment, 2 segment): warm. */
    uint64_t start_len = segment < START ? segment : START;
    uint64_t lengths[3] = {start_len, segment - start_len, segment}, ns[3], cpu_ns[3];
    struct t1_stats warm0 = {0}, warm1 = {0};
    struct hostwin_stats hw0 = {0}, hw1 = {0};
    static struct tier0_stats z0, z1;
    for (unsigned i = 0; i < 3; ++i) {
        if ((pv || product) && i == 2) {
            tier0_get_stats(&z0);
            t1_get_stats(&warm0);
            hostwin_get_stats(t1h_tier1_hostwin(ev.ctx), &hw0);
        }
        uint64_t cpu0 = thread_ns(), t0 = now_ns(), before = v->retired;
        struct vcpu_exit e = t1h_engine_run(&ev, lengths[i]);
        ns[i] = now_ns() - t0;
        cpu_ns[i] = thread_ns() - cpu0;
        if (e.reason != VCPU_EXIT_BUDGET || v->retired - before != lengths[i]) {
            fprintf(stderr, "bench: %s on %s stopped early (exit %u at %llu)\n", engine->name,
                    p->name, e.reason, (unsigned long long)v->retired);
            return 1;
        }
    }
    printf("bench %s %s start %.0f cold %.0f warm %.0f segment %llu\n", engine->name, p->name,
           start_len * 1e9 / (double)ns[0], segment * 1e9 / (double)(ns[0] + ns[1]),
           segment * 1e9 / (double)ns[2], segment);
    printf("bench-cpu %s %s cold_ns %llu warm_ns %llu\n", engine->name, p->name,
           (unsigned long long)(cpu_ns[0] + cpu_ns[1]), (unsigned long long)cpu_ns[2]);
    if (pv || product) {
        tier0_get_stats(&z1);
        t1_get_stats(&warm1);
        hostwin_get_stats(t1h_tier1_hostwin(ev.ctx), &hw1);
        double per_guest = report.insns ? (double)report.loop_host_insns / report.insns : 0;
        printf(
            "bench-t1 %s %s tier1_retired %llu lower_retired %llu compiled %d compile_us %.1f "
            "host_per_guest %.2f loop_value_spills %u restore_only_spills %u entries %llu "
            "fills %llu rebinds %llu window_misses %llu misses_same_region %llu misses_other_region %llu "
            "traps %llu live_mappings %llu translate_calls %llu "
            "probe_calls %llu lower_steps %llu context_unchanged %llu context_replays %llu "
            "context_full_diffs %llu check_diffs %llu\n",
            engine->name, p->name, (unsigned long long)(warm1.tier1_retired - warm0.tier1_retired),
            (unsigned long long)(warm1.lower_retired - warm0.lower_retired),
            pv ? report.region_id != UINT32_MAX : -1, compile_ns / 1e3, per_guest,
            report.loop_value_spills, report.restore_only_spills,
            (unsigned long long)(warm1.entries - warm0.entries),
            (unsigned long long)(hw1.fills - hw0.fills),
            (unsigned long long)(hw1.rebinds - hw0.rebinds),
            (unsigned long long)(warm1.window_misses - warm0.window_misses),
            (unsigned long long)(warm1.window_misses_same_region - warm0.window_misses_same_region),
            (unsigned long long)(warm1.window_misses_other_region -
                                 warm0.window_misses_other_region),
            (unsigned long long)(warm1.traps - warm0.traps), (unsigned long long)hw1.live_mappings,
            (unsigned long long)(hw1.translate_calls - hw0.translate_calls),
            (unsigned long long)(hw1.probe_calls - hw0.probe_calls),
            (unsigned long long)(warm1.lower_steps - warm0.lower_steps),
            (unsigned long long)(hw1.context_unchanged - hw0.context_unchanged),
            (unsigned long long)(hw1.context_replays - hw0.context_replays),
            (unsigned long long)(hw1.context_full_diffs - hw0.context_full_diffs),
            (unsigned long long)(hw1.check_diffs - hw0.check_diffs));
    } else {
        printf("bench-t1 %s %s n/a (not Tier1)\n", engine->name, p->name);
    }
    if (product) {
        struct t1_stats d = warm1;
#define DELTA(f) (unsigned long long)(d.f - warm0.f)
        printf(
            "bench-t0t1 %s %s upper_enters %llu ran %llu ran_pending %llu declines link/budget/rse/nat/"
            "fetch/validate/retired/lower %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu hot %llu compiles %llu "
            "tier0_hits %llu tier0_executed %llu fetch_guards %llu fetch_cache_hits %llu\n",
            engine->name, p->name, DELTA(upper_enters), DELTA(upper_ran), DELTA(upper_ran_pending),
            DELTA(decline_link), DELTA(decline_budget), DELTA(decline_rse), DELTA(decline_nat),
            DELTA(decline_fetch), DELTA(decline_validate), DELTA(decline_retired),
            DELTA(decline_lower), DELTA(hot_calls), DELTA(compiles),
            (unsigned long long)(z1.hits - z0.hits),
            (unsigned long long)(z1.executed_instructions - z0.executed_instructions),
            DELTA(fetch_guards), DELTA(fetch_cache_hits));
#undef DELTA
        /* For tools/bench/t1_perf.py: the code arenas (samples there have no
         * symbol) and the whole run's region entries. */
        printf("bench-arena tier1 %#llx %#llx tier0 %#llx %#llx entries %llu\n",
               (unsigned long long)(uintptr_t)t1_arena_stub_page(),
               (unsigned long long)((uintptr_t)t1_arena_stub_page() + t1_cfg.code_bytes),
               (unsigned long long)(uintptr_t)tier0_arena.code,
               (unsigned long long)((uintptr_t)tier0_arena.code + tier0_arena.code_pages * 4096),
               (unsigned long long)warm1.entries);
        /* The live regions' code: [code, + loop_start) is the prologue and
         * the entry block's LIVEIN loads, [loop_start, loop_end) the loop
         * body, the rest the exit blocks and cold stubs (emit.c). */
        for (uint32_t id = 1; id <= t1_cfg.max_regions; ++id) {
            const struct t1_region *r = t1_region_get(id);
            if (!r || !r->valid)
                continue;
            unsigned restore = 0;
            for (unsigned p = 0; p < r->npoints; ++p)
                if (r->point[p].restore_offset &&
                    (!restore || r->point[p].restore_offset < restore))
                    restore = r->point[p].restore_offset;
            printf("bench-region %u code %#llx size %zu loop %u %u entries %llu restore %u %zu "
                   "frame-in %u %zu\n",
                   id, (unsigned long long)(uintptr_t)r->code, r->code_size, r->loop_start,
                   r->loop_end, (unsigned long long)r->entries, restore, restore ? r->code_size : 0,
                   r->frame_in_offset, r->frame_in_offset ? (restore ? restore : r->code_size) : 0);
        }
        t1h_tier0t1_set_flags(T1_COMPILE_VERIFY);
    }
    t1h_engine_release(&ev);
    t1h_unbind(v);
    vcpu_destroy(v);
    return 0;
}
