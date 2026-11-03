/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional, image-free measurement of t1_opt_fold, not guest execution.
 * Three synthetic I64 workloads: live inputs, one third constant pairs,
 * and all constant pairs. Copying the input and warming it are outside
 * the timed interval. Reuse the normal workspace allocator, verify both
 * IRs, and print the final IR checksum when comparing compiler changes.
 *
 * make bench-tier1-fold
 * build/tier1-fold-bench [arithmetic-ops (1..4096) [repetitions]]
 */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "engine/tier1/ir.h"
#include "engine/tier1/memory.h"

static uint64_t now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        abort();
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static uint64_t hash_ir(const struct t1_ir *ir)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned b = 0; b < ir->nblocks; ++b)
        for (uint32_t o = ir->block[b].first; o != T1_NO_OP; o = ir->op[o].next) {
            hash = (hash ^ ir->op[o].code) * UINT64_C(1099511628211);
            hash = (hash ^ (uint64_t)ir->op[o].imm) * UINT64_C(1099511628211);
            for (unsigned k = 0; k < ir->op[o].nuse; ++k)
                hash = (hash ^ ir->use[ir->op[o].use_first + k].value) * UINT64_C(1099511628211);
        }
    return hash;
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 512;
    unsigned reps = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 2000;
    if (!n || n > 4096 || !reps)
        return 1;
    struct t1_ir *src = calloc(1, sizeof(*src)), *ir = calloc(1, sizeof(*ir));
    struct t1_workspace ws = {calloc(64, 4096), 64, 0};
    if (!src || !ir || !ws.base)
        return 1;
    static const uint16_t ops[] = {T1_IR_ADD,   T1_IR_SUB, T1_IR_AND,
                                   T1_IR_ANDCM, T1_IR_OR,  T1_IR_XOR};
    static const char *names[] = {"live", "mixed", "constant"};
    for (unsigned mode = 0; mode < 3; ++mode) {
        t1_ir_init(src, NULL);
        t1_ir_new_block(src, 0);
        t1_ir_new_block(src, 0);
        t1_ir_set_jump(src, T1_ENTRY_BLOCK, T1_HEADER_BLOCK);
        t1_value live = t1_ir_livein(src, T1_I64, T1_ELEMENT_GR(14)), result = live;
        for (unsigned k = 0; k < n; ++k) {
            t1_value a[2] = {mode == 2 || (mode == 1 && k % 3 == 0)
                                 ? t1_ir_const(src, T1_I64, (uint64_t)k * 17 + 5)
                                 : live,
                             t1_ir_const(src, T1_I64, k % 23 + 1)};
            result = T1_VALUE(
                t1_ir_append(src, T1_HEADER_BLOCK, ops[k % 6], T1_I64, 0, a, 2, T1_NO_INSN), 0);
        }
        uint16_t element = T1_ELEMENT_GR(20);
        t1_elem_add(src->frame_set, element);
        uint32_t point =
            t1_ir_new_point(src, T1_PT_EXIT, 0x5000, 0, T1_NO_INSN, T1_HEADER_BLOCK, 0);
        t1_ir_set_frame(src, point, &element, &result, NULL, 1);
        t1_ir_set_exit(src, T1_HEADER_BLOCK, point, T1_NO_INSN);
        char err[256];
        if (src->failed || !t1_ir_verify(src, 0, err, sizeof(err))) {
            fprintf(stderr, "input: %s\n", src->failed ? src->fail_reason : err);
            return 1;
        }
        uint64_t total = 0;
        for (unsigned i = 0; i < reps + 8; ++i) {
            memcpy(ir, src, sizeof(*ir));
            ws.top = 0;
            ir->ws = &ws;
            uint64_t begin = now();
            t1_opt_fold(ir);
            uint64_t end = now();
            if (ir->failed) {
                fprintf(stderr, "%s\n", ir->fail_reason);
                return 1;
            }
            if (i >= 8)
                total += end - begin;
        }
        if (!t1_ir_verify(ir, 0, err, sizeof(err))) {
            fprintf(stderr, "output: %s\n", err);
            return 1;
        }
        printf(
            "fold-bench mode=%s arithmetic_ops=%u repetitions=%u ns_per_fold=%.1f checksum=%016" PRIx64
            "\n",
            names[mode], n, reps, (double)total / reps, hash_ir(ir));
    }
    free(ws.base);
    free(ir);
    free(src);
    return 0;
}
