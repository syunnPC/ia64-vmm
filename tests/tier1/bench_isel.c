/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional, image-free measurement of t1_isel on a live I64 chain.
 * Report the first selection separately from repeated warm selections.
 * Build the IR, allocate MIR storage and touch workspace pages outside
 * the timer. Check the printed MIR checksum across repetitions.
 *
 * make bench-tier1-isel
 * build/tier1-isel-bench [arithmetic-ops (1..2048) [repetitions]]
 */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "engine/tier1/codegen.h"
#include "engine/tier1/memory.h"
#include "engine/tier1/region.h"

bool t1_isel_reference(struct t1_mir *, unsigned) __attribute__((weak));
static clockid_t timer_clock = CLOCK_MONOTONIC;

static uint64_t now(void)
{
    struct timespec ts;
    if (clock_gettime(timer_clock, &ts))
        abort();
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static void hash_line(void *arg, const char *line)
{
    uint64_t *hash = arg;
    do {
        *hash = (*hash ^ (unsigned char)*line) * UINT64_C(1099511628211);
    } while (*line++);
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 256;
    unsigned reps = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 1000;
    if (!n || n > 2048 || !reps)
        return 1;
    if (getenv("T1_BENCH_CPU"))
        timer_clock = CLOCK_THREAD_CPUTIME_ID;
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    size_t pages = (t1_compile_workspace_bytes() + 4095) / 4096;
    struct t1_workspace ws = {calloc(pages, 4096), pages, 0};
    if (!ir || !ws.base)
        return 1;
    for (size_t p = 0; p < pages; ++p)
        ((uint8_t *)ws.base)[p * 4096] = (uint8_t)p;
    t1_ir_init(ir, NULL);
    ir->ws = &ws;
    t1_ir_new_block(ir, 0);
    t1_ir_new_block(ir, 0);
    t1_ir_set_jump(ir, T1_ENTRY_BLOCK, T1_HEADER_BLOCK);
    t1_value value = t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(14));
    t1_value other = t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(15));
    static const uint16_t ops[] = {T1_IR_ADD,   T1_IR_SUB, T1_IR_AND,
                                   T1_IR_ANDCM, T1_IR_OR,  T1_IR_XOR};
    for (unsigned k = 0; k < n; ++k) {
        t1_value args[2] = {value, other};
        value = T1_VALUE(
            t1_ir_append(ir, T1_HEADER_BLOCK, ops[k % 6], T1_I64, 0, args, 2, T1_NO_INSN), 0);
    }
    uint16_t element = T1_ELEMENT_GR(20);
    t1_elem_add(ir->frame_set, element);
    uint32_t point = t1_ir_new_point(ir, T1_PT_EXIT, 0x5000, 0, T1_NO_INSN, T1_HEADER_BLOCK, 0);
    t1_ir_set_frame(ir, point, &element, &value, NULL, 1);
    t1_ir_set_exit(ir, T1_HEADER_BLOCK, point, T1_NO_INSN);
    char err[256];
    if (ir->failed || !t1_ir_verify(ir, 0, err, sizeof(err))) {
        fprintf(stderr, "input: %s\n", ir->failed ? ir->fail_reason : err);
        return 1;
    }
    uint64_t first = 0, total = 0, checksum = 0;
    uint64_t paired_time[2] = {0, 0};
    unsigned paired_samples[2] = {0, 0};
    bool paired = t1_isel_reference != NULL;
    unsigned insns = 0;
    for (unsigned i = 0; i < reps + 8; ++i) {
        ws.top = 0;
        struct t1_mir *m = t1_mir_create(ir);
        if (!m)
            return 1;
        unsigned version = paired ? (i ^ (i >> 1)) & 1 : 1;
        uint64_t begin = now();
        bool ok = version ? t1_isel(m, 0) : t1_isel_reference(m, 0);
        uint64_t elapsed = now() - begin;
        if (!ok) {
            fprintf(stderr, "%s\n", m->fail_reason);
            return 1;
        }
        if (!i)
            first = elapsed;
        if (i >= 8) {
            total += elapsed;
            paired_time[version] += elapsed;
            ++paired_samples[version];
        }
        if (i < 4 || i + 4 >= reps + 8) {
            uint64_t hash = UINT64_C(14695981039346656037);
            t1_mir_print(m, false, hash_line, &hash);
            if (i && hash != checksum) {
                fprintf(stderr, "MIR changed across repetitions\n");
                return 1;
            }
            checksum = hash;
        }
        insns = m->ninsns;
        t1_mir_destroy(m);
    }
    printf("isel-bench arithmetic_ops=%u repetitions=%u first_ns=%" PRIu64
           " warm_ns=%.1f mir_insns=%u checksum=%016" PRIx64 "\n",
           n, reps, first, (double)total / reps, insns, checksum);
    if (paired)
        for (unsigned v = 0; v < 2; ++v)
            printf("isel-paired,chain,%s,%u,%.3f,%016" PRIx64 "\n", v ? "candidate" : "baseline",
                   paired_samples[v], (double)paired_time[v] / paired_samples[v], checksum);
    free(ws.base);
    free(ir);
    return 0;
}
