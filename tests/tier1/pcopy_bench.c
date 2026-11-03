/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Parallel-copy correctness and emitter timing. Long dependency chains,
 * cycles, fan-out, identity moves and constants use physical MIR locations;
 * emitted bytes must equal the baseline before native execution is checked. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tests/tier1/um_t1.h"
#include "engine/tier1/codegen.h"
#include "engine/tier1/region.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/hal.h"

bool t1_emit_reference(const struct t1_mir *, struct t1_emitted *, char *, size_t);

static const uint8_t regs[] = {T1_RAX, T1_RCX, T1_RDX, T1_RBX, T1_RBP, T1_RSI,
                               T1_RDI, T1_R8,  T1_R9,  T1_R10, T1_R15};
enum { NREG = sizeof(regs), NVALUES = NREG + T1_SPILL_SCRATCH };

static struct t1_loc location(unsigned k)
{
    return k < NREG ? (struct t1_loc){.kind = T1_LOC_REG, .reg = regs[k]}
                    : (struct t1_loc){.kind = T1_LOC_SPILL, .slot = (uint16_t)(k - NREG)};
}

static uint64_t value(unsigned k)
{
    return UINT64_C(0x3141592600000000) + k;
}

static uint64_t now(void)
{
    struct timespec ts;
    UM_CHECK(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts));
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static unsigned random_step(unsigned *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void run_case(struct t1_workspace *ws, unsigned kind, unsigned n, unsigned seed,
                     unsigned repeat)
{
    static const char *names[] = {"identity",  "reverse-chain", "reverse-cycle", "fanout",
                                  "constants", "permutation",   "mixed",         "duplicate-cycle"};
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    UM_CHECK(ir != NULL);
    t1_ir_init(ir, NULL);
    ir->ws = ws;
    ir->npoints = 1;
    ir->point[0].kind = T1_PT_EXIT;
    ir->point[0].frame_count = ir->nuses = NREG;
    struct t1_mir *m = t1_mir_create(ir);
    UM_CHECK(m != NULL);
    m->nframe = m->point[0].frame_count = NREG;
    for (unsigned k = 0; k < NREG; ++k) {
        m->frame[k] = (struct t1_mir_frame){T1_ELEMENT_GR(14 + k), T1_MF_VREG, k, 0};
        m->frame_loc[k] = (struct t1_loc){.kind = T1_LOC_REG, .reg = regs[k]};
    }
    m->nblocks = 1;
    for (unsigned k = 0; k < NREG; ++k) {
        struct t1_mir_insn *in = t1_mir_append(m, T1_MIR_LIVEIN);
        UM_CHECK(in != NULL);
        in->nopnd = 2;
        in->opnd[0] = (struct t1_mir_opnd){T1_MO_VREG, regs[k], k, 0};
        in->opnd[1] = (struct t1_mir_opnd){.kind = T1_MO_IMM, .imm = T1_ELEMENT_GR(14 + k)};
    }
    uint64_t expected[NVALUES];
    unsigned permutation[NVALUES];
    for (unsigned k = 0; k < NVALUES; ++k)
        expected[k] = value(k), permutation[k] = k;
    if (kind == 5)
        for (unsigned k = n; k > 1; --k) {
            unsigned q = random_step(&seed) % k;
            unsigned tmp = permutation[k - 1];
            permutation[k - 1] = permutation[q];
            permutation[q] = tmp;
        }
    unsigned first = t1_mir_new_copies(m, n);
    UM_CHECK(first != T1_MIR_NONE);
    for (unsigned k = 0; k < n; ++k) {
        unsigned d = kind == 1 || kind == 2 ? n - 1 - k
                     : kind == 3            ? 1 + k % (NVALUES - 1)
                                            : k % NVALUES;
        struct t1_loc src;
        uint64_t answer;
        if (kind == 1 && d == n - 1)
            src = (struct t1_loc){.kind = T1_LOC_CONST, .value = 99}, answer = 99;
        else if (kind == 4 || (kind == 6 && k % 3 == 0))
            src = (struct t1_loc){.kind = T1_LOC_CONST, .value = ~value(k)}, answer = ~value(k);
        else {
            unsigned from = kind == 1   ? d + 1
                            : kind == 2 ? (d + 1) % n
                            : kind == 3 ? 0
                            : kind == 5 ? permutation[k]
                            : kind == 6 ? random_step(&seed) % NVALUES
                            : kind == 7 ? (d + 1) % (n < NVALUES ? n : NVALUES)
                                        : d;
            src = location(from);
            answer = value(from);
        }
        m->copy[first + k] = (struct t1_mir_copy){.dloc = location(d), .sloc = src};
        expected[d] = answer;
    }
    struct t1_mir_insn *copy = t1_mir_append(m, T1_MIR_PCOPY);
    UM_CHECK(copy != NULL);
    copy->copies_first = first;
    copy->ncopies = n;
    m->block[0].term = m->ninsns;
    m->point[0].insn = m->ninsns;
    struct t1_mir_insn *exit = t1_mir_append(m, T1_MIR_EXIT);
    UM_CHECK(exit != NULL);
    exit->point = 0;
    m->block[0].count = m->ninsns;
    char err[200];
    struct t1_emitted reference, candidate;
    if (n > 256) {
        char reference_err[200];
        UM_CHECK(!t1_emit_reference(m, &reference, reference_err, sizeof(reference_err)));
        UM_CHECK(!t1_emit(m, &candidate, err, sizeof(err)));
        UM_CHECK(!strcmp(err, reference_err) && strstr(err, "parallel copy too large"));
        UM_CHECK(!reference.buf && !reference.pt && !candidate.buf && !candidate.pt);
        printf("rejection,%u,%s\n", n, err);
        t1_mir_destroy(m);
        UM_CHECK(ws->top == 0);
        free(ir);
        return;
    }
    UM_CHECK(t1_emit_reference(m, &reference, err, sizeof(err)));
    UM_CHECK(t1_emit(m, &candidate, err, sizeof(err)));
    UM_CHECK(reference.size == candidate.size &&
             !memcmp(reference.buf, candidate.buf, reference.size));
    UM_CHECK(reference.nabs == candidate.nabs &&
             !memcmp(reference.abs_fix, candidate.abs_fix,
                     reference.nabs * sizeof(*reference.abs_fix)) &&
             !memcmp(reference.abs_target, candidate.abs_target,
                     reference.nabs * sizeof(*reference.abs_target)));
    UM_CHECK(!memcmp(reference.pt, candidate.pt, sizeof(*reference.pt)));
    size_t pages = (candidate.size + 4095) / 4096;
    uint8_t *code = t1_arena_alloc(pages);
    UM_CHECK(code != NULL);
    t1_emit_place(&candidate, code);
    hal_code_set_executable(code, pages);
    struct vcpu *v = um_vcpu(UM_CODE);
    for (unsigned k = 0; k < NREG; ++k)
        um_gr(v, 14 + k, value(k), false);
    static struct t1_exec exec;
    memset(&exec, 0, sizeof(exec));
    for (unsigned k = NREG; k < NVALUES; ++k)
        exec.spill[k - NREG] = value(k);
    t1_entry_fn fn = (t1_entry_fn)(uintptr_t)code;
    UM_CHECK(fn(v, 99, &exec) == T1_RET_POINT && exec.point == 0 && exec.raw_r13 == 99);
    for (unsigned k = 0; k < NVALUES; ++k)
        UM_CHECK((k < NREG ? exec.regs[regs[k]] : exec.spill[k - NREG]) == expected[k]);
    printf("correctness,%s,%u,%zu,identical-native-pass\n", names[kind], n, candidate.size);
    vcpu_destroy(v);
    t1_arena_free(code, pages);
    t1_emit_free(&candidate);
    t1_emit_free(&reference);
    t1_test_workspace_poison = false;
    uint64_t cpu[2] = {0};
    unsigned samples[2] = {0};
    for (unsigned k = 0; k < repeat; ++k) {
        unsigned version = (k ^ (k >> 1)) & 1;
        struct t1_emitted out;
        uint64_t start = now();
        bool ok = version ? t1_emit(m, &out, err, sizeof(err))
                          : t1_emit_reference(m, &out, err, sizeof(err));
        cpu[version] += now() - start;
        UM_CHECK(ok);
        ++samples[version];
        t1_emit_free(&out);
    }
    for (unsigned version = 0; version < 2; ++version)
        if (samples[version])
            printf("paired,%s-%u,%s,%u,%.2f\n", names[kind], n, version ? "new" : "base",
                   samples[version], (double)cpu[version] / samples[version]);
    t1_test_workspace_poison = true;
    t1_mir_destroy(m);
    UM_CHECK(ws->top == 0);
    free(ir);
}

int main(int argc, char **argv)
{
    unsigned repeat = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 2000;
    umt_init();
    struct t1_workspace ws = {0};
    UM_CHECK(t1_ws_acquire(&ws));
    t1_test_workspace_poison = true;
    static const unsigned lengths[] = {0, 1, 2, 4, 8, 16, 32, 64, NVALUES};
    for (unsigned kind = 0; kind < 8; ++kind)
        for (unsigned k = 0; k < sizeof(lengths) / sizeof(lengths[0]); ++k)
            run_case(&ws, kind, lengths[k], 20261020, repeat);
    run_case(&ws, 0, 256, 0, repeat);
    run_case(&ws, 3, 256, 0, repeat);
    run_case(&ws, 4, 256, 0, repeat);
    run_case(&ws, 7, 256, 0, repeat);
    for (unsigned seed = 1; seed <= 32; ++seed)
        run_case(&ws, 5, NVALUES, seed, 0);
    for (unsigned seed = 1; seed <= 32; ++seed)
        run_case(&ws, 6, NVALUES, seed, 0);
    run_case(&ws, 0, 257, 0, 0);
    t1_ws_release(&ws);
    return 0;
}
