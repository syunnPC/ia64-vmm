/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Hosted compiler-stage microbenchmark. Inputs use the existing IA-64
 * test encoder, then the real formation, lifting and optimization passes.
 * Setup, selection and cleanup are excluded from the named stage's time.
 * A retained compile workspace models the normal compiler allocation path.
 * The correctness pass poisons nonzero allocations before hashing MIR,
 * emitted bytes, point/frame tables and helper read/write mappings. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tests/tier1/um_t1.h"
#include "engine/tier1/codegen.h"
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"

static clockid_t timer_clock = CLOCK_MONOTONIC;
static uint64_t now(void)
{
    struct timespec ts;
    UM_CHECK(!clock_gettime(timer_clock, &ts));
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void hash_bytes(uint64_t *h, const void *data, size_t n)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < n; ++i)
        *h = (*h ^ p[i]) * UINT64_C(1099511628211);
}
static void hash_line(void *arg, const char *s)
{
    hash_bytes(arg, s, strlen(s));
}

static void make_program(unsigned kind, unsigned n)
{
    struct ienc_bundle b[90];
    UM_CHECK(n + 2 <= sizeof(b) / sizeof(b[0]));
    unsigned width = !kind && getenv("T1_BENCH_WIDE") ? 48 : 12;
    for (unsigned k = 0; k < n; ++k) {
        unsigned dst = 14 + k % width, src = 14 + (k + 1) % width;
        uint64_t m = kind == 1   ? ienc_ld(0, 8, false, dst, 30)
                     : kind == 2 ? ienc_ld(6, 8, false, dst, 30)
                                 : ienc_nop_m();
        uint64_t x =
            kind == 3 ? ienc_tnat(0, 0, 0, 0, 6, 7, 31) : ienc_alu(0, IENC_XOR, dst, dst, src);
        b[k] = (struct ienc_bundle){IENC_STOP(IENC_MII), {m, x, ienc_adds(0, src, 1, src)}};
    }
    b[n] = (struct ienc_bundle){IENC_STOP(IENC_MIB),
                                {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(-(int64_t)(16 * n))}};
    b[n + 1] =
        (struct ienc_bundle){IENC_STOP(IENC_MII), {ienc_break_m(0), ienc_nop_i(), ienc_nop_i()}};
    um_emit(UM_CODE, b, n + 2);
}

static struct t1_mir *selected(struct t1_ir *ir)
{
    struct t1_mir *m = t1_mir_create(ir);
    UM_CHECK(m != NULL);
    m->window_bits = 40;
    UM_CHECK(t1_isel(m, 0));
    return m;
}

static uint64_t output_hash(struct t1_mir *m)
{
    char err[200];
    UM_CHECK(t1_raverify(m, err, sizeof(err)));
    uint64_t h = UINT64_C(1469598103934665603);
    t1_mir_print(m, true, hash_line, &h);
    struct t1_emitted e;
    UM_CHECK(t1_emit(m, &e, err, sizeof(err)));
    hash_bytes(&h, e.buf, e.size);
    struct t1_region r = {0};
    memcpy(r.nat_guard, m->ir->nat_guard, sizeof(r.nat_guard));
    UM_CHECK(t1_frame_build(&r, m, &e));
    hash_bytes(&h, r.point, r.npoints * sizeof(*r.point));
    hash_bytes(&h, r.frame, r.nframe * sizeof(*r.frame));
    hash_bytes(&h, r.rip, r.nrip * sizeof(*r.rip));
    for (unsigned k = 0; k < r.nhelper; ++k) {
        const struct t1_helper_info *helper = &r.helper[k];
        hash_bytes(&h, helper->read_frame, helper->nreads * sizeof(*helper->read_frame));
        hash_bytes(&h, helper->writes, helper->nwrites * sizeof(*helper->writes));
        hash_bytes(&h, helper->wr, sizeof(helper->wr));
        hash_bytes(&h, helper->def_reg, sizeof(helper->def_reg));
    }
    printf("shape,%u,%u,%u,%u,%zu,%u,%u\n", m->nblocks, m->ninsns, m->nvregs, m->nframe, e.size,
           m->total_spills, m->spill_slots);
    t1_memory_free(r.meta, r.meta_pages);
    t1_emit_free(&e);
    return h;
}

int main(int argc, char **argv)
{
    unsigned repeat = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 1000;
    const char *stage = argc > 2 ? argv[2] : "regalloc";
    if (!repeat || (strcmp(stage, "regalloc") && strcmp(stage, "raverify") &&
                    strcmp(stage, "frames") && strcmp(stage, "emit") && strcmp(stage, "irverify") &&
                    strcmp(stage, "mircreate") && strcmp(stage, "lift") && strcmp(stage, "isel"))) {
        fprintf(
            stderr,
            "usage: %s [positive iterations] [regalloc|raverify|frames|emit|irverify|mircreate|lift|isel]\n",
            argv[0]);
        return 2;
    }
    if (getenv("T1_BENCH_CPU"))
        timer_clock = CLOCK_THREAD_CPUTIME_ID;
    umt_init();
    /* Emission is inspected, never executed here. Canonical stub
     * addresses make embedded wrapper pointers independent of ASLR. */
    t1_stub = (struct t1_stub_addrs){0x10000000, 0x10000100, 0x10000200, 0x10000300, 0x10000400};
    struct t1_workspace ws = {0};
    if (!getenv("T1_BENCH_POOL"))
        UM_CHECK(t1_ws_acquire(&ws));
    struct t1_form *f = calloc(1, sizeof(*f));
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    UM_CHECK(f && ir);
    ir->ws = &ws;
    const char *names[] = {"arithmetic", "memory", "predicated", "helpers"};
    unsigned lengths[] = {64, 40, 12, 40};
    if (getenv("T1_BENCH_SHORT"))
        lengths[0] = lengths[1] = lengths[2] = lengths[3] = 1;
    for (unsigned kind = 0; kind < 4; ++kind) {
        make_program(kind, lengths[kind]);
        struct vcpu *v = um_vcpu(UM_CODE);
        if (!kind && getenv("T1_BENCH_WIDE"))
            v->s.cfm = um_cfm(96, 96, 0, 0, 0);
        um_gr(v, 30, UM_DATA, false);
        um_gr(v, 31, 0, true);
        v->s.ar[AR_LC] = 100;
        um_pr(v, 6, true);
        const char *reason = NULL;
        unsigned fail;
        char err[200];
        UM_CHECK(t1_form(v, 0, T1_MAX_INSNS, f, &reason) == T1_COMPILED);
        t1_test_workspace_poison = true;
        UM_CHECK(t1_lift(f, ir, &fail));
        UM_CHECK(t1_optimize(ir, true, err, sizeof(err)));
        struct t1_mir *m = selected(ir);
        UM_CHECK(t1_regalloc(m));
        uint64_t hash = output_hash(m);
        printf("correctness,%s,%016llx\n", names[kind], (unsigned long long)hash);
        t1_mir_destroy(m);
        t1_test_workspace_poison = false;
        uint64_t total = 0;
        if (!strcmp(stage, "lift")) {
            uint64_t start = now();
            for (unsigned i = 0; i < repeat; ++i)
                UM_CHECK(t1_lift(f, ir, &fail));
            total = now() - start;
            UM_CHECK(t1_ir_verify(ir, 0, err, sizeof(err)));
        } else if (!strcmp(stage, "isel")) {
            for (unsigned i = 0; i < repeat; ++i) {
                m = t1_mir_create(ir);
                UM_CHECK(m != NULL);
                m->window_bits = 40;
                uint64_t start = now();
                UM_CHECK(t1_isel(m, 0));
                total += now() - start;
                t1_mir_destroy(m);
            }
        } else if (!strcmp(stage, "mircreate")) {
            for (unsigned i = 0; i < repeat; ++i) {
                uint64_t start = now();
                m = t1_mir_create(ir);
                total += now() - start;
                UM_CHECK(m != NULL);
                t1_mir_destroy(m);
            }
        } else if (!strcmp(stage, "regalloc")) {
            for (unsigned i = 0; i < repeat; ++i) {
                m = selected(ir);
                uint64_t start = now();
                UM_CHECK(t1_regalloc(m));
                total += now() - start;
                if (!(i % 97))
                    UM_CHECK(t1_raverify(m, err, sizeof(err)));
                t1_mir_destroy(m);
            }
        } else {
            m = selected(ir);
            UM_CHECK(t1_regalloc(m));
            if (!strcmp(stage, "irverify")) {
                uint64_t start = now();
                for (unsigned i = 0; i < repeat; ++i)
                    UM_CHECK(t1_ir_verify(ir, 0, err, sizeof(err)));
                total = now() - start;
            } else if (!strcmp(stage, "raverify")) {
                uint64_t start = now();
                for (unsigned i = 0; i < repeat; ++i)
                    UM_CHECK(t1_raverify(m, err, sizeof(err)));
                total = now() - start;
            } else if (!strcmp(stage, "emit")) {
                for (unsigned i = 0; i < repeat; ++i) {
                    struct t1_emitted e;
                    uint64_t start = now();
                    UM_CHECK(t1_emit(m, &e, err, sizeof(err)));
                    total += now() - start;
                    t1_emit_free(&e);
                }
            } else if (!strcmp(stage, "frames")) {
                struct t1_emitted e;
                UM_CHECK(t1_emit(m, &e, err, sizeof(err)));
                for (unsigned i = 0; i < repeat; ++i) {
                    struct t1_region r = {0};
                    memcpy(r.nat_guard, ir->nat_guard, sizeof(r.nat_guard));
                    uint64_t start = now();
                    UM_CHECK(t1_frame_build(&r, m, &e));
                    total += now() - start;
                    t1_memory_free(r.meta, r.meta_pages);
                }
                t1_emit_free(&e);
            } else
                UM_CHECK(!"unknown stage");
            t1_mir_destroy(m);
        }
        printf("bench,%s,%s,%u,%.2f\n", stage, names[kind], repeat, (double)total / repeat);
        UM_CHECK(ws.top == 0);
        vcpu_destroy(v);
    }
    free(ir);
    free(f);
    t1_ws_release(&ws);
    return 0;
}
