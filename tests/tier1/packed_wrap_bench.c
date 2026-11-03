/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual whole-compiler and dispatched execution comparison. The baseline
 * form/lift/natguard objects are renamed together; all other objects are shared.
 * Native timing uses one fixed code address and the same vCPU/execution
 * context. The normal compiler/arena is used for full-compile timing. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tests/tier1/um_t1.h"
#include "engine/tier0/tier0.h"
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"

enum t1_compile_status baseline_t1_form_request(const struct t1_compile_request *,
                                                struct ia64_state *, unsigned, struct t1_form *,
                                                const char **);
enum t1_compile_status __real_t1_form_request(const struct t1_compile_request *,
                                              struct ia64_state *, unsigned, struct t1_form *,
                                              const char **);
bool baseline_t1_lift(const struct t1_form *, struct t1_ir *, unsigned *);
bool __real_t1_lift(const struct t1_form *, struct t1_ir *, unsigned *);
unsigned baseline_t1_natguard(struct t1_ir *);
unsigned __real_t1_natguard(struct t1_ir *);
bool __real_t1_arena_place(struct t1_region *, const struct t1_emitted *);
void __real_t1_arena_release(struct t1_region *);
static bool baseline, fixed_placement;
static unsigned forms, lifts;
static uint8_t *fixed_code;
static struct t1_region *fixed_owner;

enum t1_compile_status __wrap_t1_form_request(const struct t1_compile_request *r,
                                              struct ia64_state *s, unsigned n, struct t1_form *f,
                                              const char **why);
enum t1_compile_status __wrap_t1_form_request(const struct t1_compile_request *r,
                                              struct ia64_state *s, unsigned n, struct t1_form *f,
                                              const char **why)
{
    ++forms;
    return (baseline ? baseline_t1_form_request : __real_t1_form_request)(r, s, n, f, why);
}
bool __wrap_t1_lift(const struct t1_form *f, struct t1_ir *ir, unsigned *fail);
bool __wrap_t1_lift(const struct t1_form *f, struct t1_ir *ir, unsigned *fail)
{
    ++lifts;
    return (baseline ? baseline_t1_lift : __real_t1_lift)(f, ir, fail);
}
unsigned __wrap_t1_natguard(struct t1_ir *ir);
unsigned __wrap_t1_natguard(struct t1_ir *ir)
{
    return (baseline ? baseline_t1_natguard : __real_t1_natguard)(ir);
}

/* Test-only placement into an arena RAW chunk, using the production copy
 * and relocation routine. The chunk has no normal region-list owner and
 * is released once at the end. Exactly one region may use it at a time.
 * Every permission change and compilation is outside native timing. */
bool __wrap_t1_arena_place(struct t1_region *r, const struct t1_emitted *e);
bool __wrap_t1_arena_place(struct t1_region *r, const struct t1_emitted *e)
{
    if (!fixed_placement)
        return __real_t1_arena_place(r, e);
    UM_CHECK(fixed_code && !fixed_owner && e->size <= T1_CHUNK_BYTES);
    hal_code_set_writable(fixed_code, T1_CHUNK_BYTES / 4096);
    t1_emit_place(e, fixed_code);
    r->code = fixed_code;
    r->code_size = e->size;
    r->entry = (t1_entry_fn)(uintptr_t)fixed_code;
    hal_code_set_executable(fixed_code, T1_CHUNK_BYTES / 4096);
    r->sealed = true;
    fixed_owner = r;
    return true;
}
void __wrap_t1_arena_release(struct t1_region *r);
void __wrap_t1_arena_release(struct t1_region *r)
{
    if (r != fixed_owner) {
        __real_t1_arena_release(r);
        return;
    }
    UM_CHECK(r->code == fixed_code);
    r->code = NULL;
    r->entry = NULL;
    fixed_owner = NULL;
}

static uint64_t now(void)
{
    struct timespec t;
    UM_CHECK(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t));
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}

static void program(unsigned kind, unsigned n, bool stacked)
{
    struct ienc_bundle b[20];
    UM_CHECK(n + 3 <= sizeof(b) / sizeof(b[0]));
    for (unsigned k = 0; k < n; ++k) {
        unsigned d = (stacked ? 32 : 14) + k % 4;
        unsigned s = (stacked ? 40 : 18) + k % 4;
        unsigned bits = kind < 6 ? 8u << (kind / 2) : kind == 6 ? 8 : 16;
        uint64_t op = kind == 8 ? ienc_alu(0, IENC_XOR, d, d, s)
                                : ienc_packed(0, kind < 6 ? kind & 1 : kind == 7, bits,
                                              kind < 6    ? 0
                                              : kind == 6 ? 1
                                                          : 2,
                                              d, d, s);
        b[k] = (struct ienc_bundle){IENC_STOP(IENC_MII), {op, ienc_adds(0, s, 1, s), ienc_nop_i()}};
    }
    b[n] = (struct ienc_bundle){IENC_STOP(IENC_MIB),
                                {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(-(int64_t)n * 16)}};
    b[n + 1] =
        (struct ienc_bundle){IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}};
    /* Bound formation lookahead before bytes from an earlier longer case. */
    b[n + 2] =
        (struct ienc_bundle){IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}};
    um_emit(UM_CODE, b, n + 3);
}

static uint32_t compile(struct vcpu *v, struct t1_compile_report *report)
{
    struct t1_compile_options opt = {0};
    forms = lifts = 0;
    enum t1_compile_status status = t1_compile(umt_tv(v), &opt, report);
    if (status != T1_COMPILED)
        fprintf(stderr, "compile failure: %u %s\n", status, report->reject_reason);
    UM_CHECK(status == T1_COMPILED && forms == 1 && lifts == 1);
    return report->region_id;
}

static void shape(const char *name, unsigned version, uint32_t id,
                  const struct t1_compile_report *p)
{
    const struct t1_region *r = t1_region_get(id);
    UM_CHECK(r);
    printf("shape,%s,%u,%u,%u,%u,%u,%zu,%u,%u,%u,%u,%p\n", name, version, p->insns, p->blocks,
           p->points, r->nhelper, p->code_bytes, p->loop_host_insns, p->loop_value_spills,
           p->restore_only_spills, p->nat_guards, (void *)r->code);
}

static void one(const char *name, unsigned kind, unsigned n, bool stacked, const char *stage,
                unsigned repeat, unsigned pairs, bool source_nat)
{
    tier0_flush();
    program(kind, n, stacked);
    struct vcpu *v = um_vcpu(UM_CODE), *ref = um_vcpu(UM_CODE);
    v->s.cfm = um_cfm(24, 24, 16, 5, 0);
    v->s.rse_bof = stacked ? 95 : 0;
    for (unsigned r = 1; r < 56; ++r)
        um_gr(v, r, UINT64_C(0x0123456789abcdef) * r + (r & 1 ? UINT32_MAX : 0), false);
    if (source_nat) {
        unsigned r = stacked ? 40 : 18;
        um_gr(v, r, ia64_gr_read(&v->s, r), true);
    }
    v->s.ar[AR_LC] = UINT64_C(1000000);
    struct um_snapshot *snap = calloc(1, sizeof(*snap));
    UM_CHECK(snap);
    um_snapshot_take(snap, v);
    struct t1_vcpu *tv = umt_tv(v);
    struct t1_compile_report reports[2];
    uint8_t control[T1_CHUNK_BYTES];
    size_t control_size = 0;
    fixed_placement = true;
    for (unsigned version = 0; version < 2; ++version) {
        t1_flush();
        baseline = version == 0;
        um_snapshot_restore(snap, v);
        t1_vcpu_state_restored(tv);
        uint32_t id = compile(v, &reports[version]);
        UM_CHECK(reports[version].insns == 3 * (n + 1));
        const struct t1_region *r = t1_region_get(id);
        UM_CHECK(r && r->nhelper == (kind < 6 && version ? 0 : kind == 8 ? 0 : n));
        shape(name, version, id, &reports[version]);
        if (!version) {
            control_size = r->code_size;
            memcpy(control, r->code, control_size);
        } else if (kind >= 6)
            UM_CHECK(control_size == r->code_size && !memcmp(control, r->code, control_size));
        static const unsigned budgets[] = {0, 1, 2, 3, 5, 6, 27, 64, 4096};
        for (unsigned b = 0; b < sizeof(budgets) / sizeof(budgets[0]); ++b) {
            struct umt_run run;
            UM_CHECK(!umt_case(v, ref, snap, 0, budgets[b], id, NULL, &run, false, name));
            if (budgets[b] == 4096) {
                if (!source_nat || kind < 8)
                    UM_CHECK(run.run.entries > 0);
            }
        }
    }
    /* Independently check the Tier0 comparison before timing it. */
    for (unsigned b = 0; b < 9; ++b) {
        static const unsigned budgets[] = {0, 1, 2, 3, 5, 6, 27, 64, 4096};
        struct um_run run = {0};
        um_snapshot_restore(snap, v);
        tier0_vcpu_state_restored(v);
        struct vcpu_exit e = tier0_run(v, budgets[b]);
        UM_CHECK(!um_compare(snap, ref, &run, v, &e, budgets[b], name));
    }
    t1_flush();
    fixed_placement = !strcmp(stage, "run");
    t1_test_set_restore_observer(NULL, NULL);
    uint64_t sums[3] = {0};
    for (unsigned pair = 0; pair < pairs; ++pair) {
        uint64_t ns[3] = {0};
        for (unsigned turn = 0; turn < (fixed_placement ? 6u : 4u); ++turn) {
            /* Rotated ABC/CBA for native old Tier1/new Tier1/Tier0;
             * ABBA/BAAB remains the two-compiler timing order. */
            unsigned version = fixed_placement ? ((turn < 3 ? turn : 5 - turn) + pair) % 3
                                               : ((turn ^ (turn >> 1)) & 1) ^ (pair & 1);
            baseline = version == 0;
            t1_flush();
            um_snapshot_restore(snap, v);
            t1_vcpu_state_restored(tv);
            tier0_vcpu_state_restored(v);
            if (fixed_placement && version < 2) {
                struct t1_compile_report p;
                (void)compile(v, &p);
            }
            for (unsigned k = 0; k < repeat + 1; ++k) {
                struct t1_compile_report p;
                um_snapshot_restore(snap, v);
                t1_vcpu_state_restored(tv);
                tier0_vcpu_state_restored(v);
                if (!fixed_placement)
                    t1_flush();
                uint64_t start = now();
                if (fixed_placement) {
                    struct vcpu_exit e = version == 2 ? tier0_run(v, 4096) : t1_run(tv, 4096);
                    uint64_t elapsed = now() - start;
                    UM_CHECK(e.reason == VCPU_EXIT_BUDGET && v->retired - snap->v.retired == 4096);
                    if (k)
                        ns[version] += elapsed;
                    sums[version] += v->s.gr[14] + v->s.gr[32] + v->s.ar[AR_LC];
                } else {
                    (void)compile(v, &p);
                    uint64_t elapsed = now() - start;
                    if (k)
                        ns[version] += elapsed;
                    UM_CHECK(p.code_bytes == reports[version].code_bytes);
                }
            }
        }
        printf("pair,%s,%s,%u,%u,%llu,%llu,%llu\n", stage, name, pair, 2 * repeat,
               (unsigned long long)ns[0], (unsigned long long)ns[1], (unsigned long long)ns[2]);
    }
    UM_CHECK(sums[0] == sums[1]);
    if (fixed_placement)
        UM_CHECK(sums[0] == sums[2]);
    printf("checksum,%s,%llu,%llu\n", name, (unsigned long long)sums[0],
           (unsigned long long)sums[1]);
    t1_flush();
    UM_CHECK(!fixed_owner);
    umt_release(v);
    vcpu_destroy(v);
    vcpu_destroy(ref);
    um_snapshot_free(snap);
    free(snap);
}

int main(int argc, char **argv)
{
    const char *stage = argc > 1 ? argv[1] : "run";
    unsigned repeat = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 100;
    unsigned pairs = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 9;
    bool source_nat = argc > 4 && !strcmp(argv[4], "nat");
    UM_CHECK(repeat && pairs && (!strcmp(stage, "run") || !strcmp(stage, "compile")));
    umt_init();
    tier0_set_upper(NULL);
    tier0_test_set_boundary_mode(TIER0_BOUNDARY_ON);
    fixed_code = t1_arena_alloc(T1_CHUNK_BYTES / 4096);
    UM_CHECK(fixed_code);
    static const char *names[] = {"padd1",
                                  "psub1",
                                  "padd2",
                                  "psub2",
                                  "padd4",
                                  "psub4",
                                  "padd1.sss-control",
                                  "psub2.uuu-control",
                                  "xor-control"};
    for (unsigned kind = 0; kind < 9; ++kind)
        for (unsigned shape = 0; shape < 3; ++shape) {
            char name[100];
            snprintf(name, sizeof(name), "%s-%s", names[kind],
                     shape == 0   ? "short"
                     : shape == 1 ? "static"
                                  : "rotating");
            one(name, kind, shape ? 8 : 1, shape == 2, stage, repeat, pairs, source_nat);
        }
    t1_arena_free(fixed_code, T1_CHUNK_BYTES / 4096);
    return 0;
}
