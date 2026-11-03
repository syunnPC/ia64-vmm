/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * build/tier1-test: test binary entry and suite selection.
 * Usage: tier1-test [--suite=NAME] IMAGE.bin...
 *        tier1-test --bench=ENGINE:PROGRAM IMAGE.bin...   (one measurement)
 * Images are found by basename (build/tests/tier1/<name>.bin,
 * build/tests/ia64/speed.bin).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/log_internal.h"
#include "engine/cache/validity.h"
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"
#include "engine/tier1/tier1.h"
#include "tests/tier1/harness.h"
#include "tests/tier1/hostwin_linux.h"

/* Suites (one file each). */
void unit_rules_test(void);
void host_isa_tests(void);
void compare_test(void);
void harness_tests(void);
void program_tests(void);
void region_property_tests(void);
void dispatch_tests(void);
void restore_tests(void);
void memory_tests(void);
void arena_occupancy_tests(void);
void arena_span_tests(void);
void upper_memory_tests(void);
void fetch_cache_tests(void);
void upper_tests(void);         /* product dispatch */
void pairing_tests_suite(void); /* checked execution */
void compile_ticks_suite(void); /* checked execution */
void undo_suite(void);          /* checked execution */
void ordered_suite(void);
void checked_suite(void); /* checked execution */
void replay_review_tests(void);
void coverage_tests(const char *image);
void fpi_tests(const char *image);
void fparith_tests(const char *image);
void fpi_ir_dump(const char *image);
void coherence_tests(void); /* bundle coherence */
void chain_tests(void);     /* region chaining */
void native_virtual_test(void);
void native_chka_test(void);
void native_fr_test(void);
void native_gr_test(void);
void coherence_bench(void); /* bundle coherence: --suite=coh-bench only */
void coherence_lz(void);    /* bundle coherence: --suite=coh-lz only */
int bench_main(const char *spec);
void unit_asm_test(void);                  /* compiler */
void unit_ir_test(void);                   /* compiler */
void unit_lift_test(const char *image);    /* compiler */
void unit_opt_test(const char *image);     /* compiler */
void unit_eval_test(const char *image);    /* compiler */
void unit_x86enc_test(void);               /* compiler */
void unit_codegen_test(const char *image); /* compiler, */
void unit_programs_tests(void);            /* compiler, */
void unit_runtime_test(const char *image); /* compiler, */

static const char *image(int argc, char **argv, const char *name)
{
    size_t n = strlen(name);
    for (int i = 1; i < argc; ++i) {
        size_t len = strlen(argv[i]);
        if (len >= n && !strcmp(argv[i] + len - n, name) &&
            (len == n || argv[i][len - n - 1] == '/'))
            return argv[i];
    }
    fprintf(stderr, "tier1-test: image %s not given\n", name);
    exit(1);
}

int main(int argc, char **argv)
{
    log_set_level(LOG_ERROR);
    /* The test RAM is the hosted window
     * backend's memfd RAM and the backend's SIGSEGV handler and code-page
     * hook are installed, for every engine alike, before the harness
     * registers its RAM; Tier1's engine glue (engine_tier1.c) adds its
     * trap client when the first Tier1 vCPU is created. */
    t1h_set_ram_allocator(hostwin_linux_ram);
    if (!hostwin_linux_init()) {
        fprintf(stderr, "tier1-test: hostwin_linux_init failed\n");
        return 1;
    }
    code_validity_set_track_hook(hostwin_code_page_event);
    const char *suite = NULL, *bench = NULL;
    if (argc > 1 && !strncmp(argv[1], "--suite=", 8)) {
        suite = argv[1] + 8;
        --argc;
        ++argv;
    } else if (argc > 1 && !strncmp(argv[1], "--bench=", 8)) {
        bench = argv[1] + 8;
        --argc;
        ++argv;
    }
    t1h_set_images(argc - 1, argv + 1);
    if (bench)
        return bench_main(bench);
    t1_test_isel_crosscheck = true;
    t1_test_workspace_poison = true;
    static const struct {
        const char *name;
        void (*run)(void);
    } suites[] = {
        {"rules", unit_rules_test},
        {"compare", compare_test},
        {"harness", harness_tests},
        {"programs", program_tests},
        {"regionprops", region_property_tests},
        {"dispatch", dispatch_tests},
        {"restore", restore_tests},
        {"memory", memory_tests},
        {"arena-occupancy", arena_occupancy_tests},
        {"arena-spans", arena_span_tests},
        {"memory-pressure", upper_memory_tests},
        {"upper", upper_tests},
        {"fetch-cache", fetch_cache_tests},
        {"replay-review", replay_review_tests},
        {"pairing", pairing_tests_suite},
        {"compile_ticks", compile_ticks_suite},
        {"undo", undo_suite},
        {"checked", checked_suite},
        {"ordered", ordered_suite},
        {"coherence", coherence_tests},
        {"chain", chain_tests},
        {"native-virtual", native_virtual_test},
        {"native-chka", native_chka_test},
        {"native-fr", native_fr_test},
        {"native-gr", native_gr_test},
    };
    bool any = false;
    /* A measurement, not a test: only when named. */
    if (suite && !strcmp(suite, "coh-bench")) {
        coherence_bench();
        return 0;
    }
    if (suite && !strcmp(suite, "coh-lz")) {
        coherence_lz();
        return 0;
    }
    for (unsigned i = 0; i < sizeof(suites) / sizeof(suites[0]); ++i)
        if (!suite || !strcmp(suite, suites[i].name)) {
            suites[i].run();
            any = true;
        }
    if (suite && !strcmp(suite, "fpi-ir")) {
        if (argc != 2) {
            fprintf(stderr, "tier1-test --suite=fpi-ir: expected one image path\n");
            return 1;
        }
        fpi_ir_dump(argv[1]);
        return 0;
    }
    if (!suite || !strcmp(suite, "fpi")) {
        fpi_tests(image(argc, argv, "frag_fpi.bin"));
        any = true;
    }
    if (!suite || !strcmp(suite, "fparith")) {
        fparith_tests(image(argc, argv, "frag_fparith.bin"));
        any = true;
    }
    if (!suite || !strcmp(suite, "coverage")) {
        coverage_tests(image(argc, argv, "frag_coverage.bin"));
        any = true;
    }
    /* The unit suites take their program image by name. */
    if (!suite || !strcmp(suite, "asm")) {
        unit_asm_test();
        any = true;
    }
    if (!suite || !strcmp(suite, "ir")) {
        unit_ir_test();
        any = true;
    }
    if (!suite || !strcmp(suite, "lift")) {
        unit_lift_test(image(argc, argv, "frag_loops.bin"));
        any = true;
    }
    if (!suite || !strcmp(suite, "opt")) {
        unit_opt_test(image(argc, argv, "frag_loops.bin"));
        any = true;
    }
    if (!suite || !strcmp(suite, "eval")) {
        unit_eval_test(image(argc, argv, "frag_loops.bin"));
        any = true;
    }
    if (!suite || !strcmp(suite, "x86enc")) {
        unit_x86enc_test();
        any = true;
    }
    if (!suite || !strcmp(suite, "unit_programs")) {
        unit_programs_tests();
        any = true;
    }
    if (!suite || !strcmp(suite, "runtime")) {
        unit_runtime_test(image(argc, argv, "frag_loops.bin"));
        any = true;
    }
    if (!suite || !strcmp(suite, "codegen")) {
        unit_codegen_test(image(argc, argv, "frag_loops.bin"));
        any = true;
    }
    if (!suite || !strcmp(suite, "host-isa")) {
        host_isa_tests();
        any = true;
    }
    if (!any) {
        fprintf(stderr, "tier1-test: unknown suite %s\n", suite);
        return 1;
    }
    /* Region chaining: on unless T1H_CHAIN=off. */
    struct t1_stats stats;
    t1_get_stats(&stats);
    const char *chain = getenv("T1H_CHAIN");
    printf("tier1-test: %llu regions entered at chain points (tier1_chain=%s)\n",
           (unsigned long long)stats.chain_links, chain && !strcmp(chain, "off") ? "off" : "on");
    printf("tier1-test: passed\n");
    return 0;
}
