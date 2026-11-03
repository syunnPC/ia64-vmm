/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional propagation benchmark: two sparse loops among unrelated I64
 * arithmetic, plus dense boolean and parameter controls. IR copying, workspace acquisition,
 * verification and output hashing are outside the folding interval. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "engine/tier1/ir.h"
#include "engine/tier1/memory.h"

static clockid_t bench_clock = CLOCK_MONOTONIC;

static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(bench_clock, &t))
        abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}
static t1_value op(struct t1_ir *ir, uint16_t code, uint8_t type, t1_value a, t1_value b,
                   t1_value c, unsigned n)
{
    t1_value v[3] = {a, b, c};
    return T1_VALUE(t1_ir_append(ir, T1_HEADER_BLOCK, code, type, 0, v, n, T1_NO_INSN), 0);
}
static uint64_t hash_ir(const struct t1_ir *ir)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (unsigned b = 0; b < ir->nblocks; ++b)
        for (uint32_t o = ir->block[b].first; o != T1_NO_OP; o = ir->op[o].next) {
            h = (h ^ ir->op[o].code) * UINT64_C(1099511628211);
            h = (h ^ (uint64_t)ir->op[o].imm) * UINT64_C(1099511628211);
            for (unsigned k = 0; k < ir->op[o].nuse; ++k)
                h = (h ^ ir->use[ir->op[o].use_first + k].value) * UINT64_C(1099511628211);
        }
    for (unsigned p = 0; p < ir->npoints; ++p)
        for (unsigned k = 0; k < ir->point[p].frame_count; ++k)
            h = (h ^ ir->use[ir->point[p].frame_first + k].value) * UINT64_C(1099511628211);
    return h;
}
static void build(struct t1_ir *ir, unsigned n, unsigned mode)
{
    t1_ir_init(ir, NULL);
    t1_ir_new_block(ir, 0);
    t1_ir_new_block(ir, 0);
    t1_ir_set_jump(ir, T1_ENTRY_BLOCK, T1_HEADER_BLOCK);
    uint16_t e[2] = {T1_ELEMENT_PR(6), T1_ELEMENT_LC};
    t1_elem_add(ir->frame_set, e[0]);
    t1_elem_add(ir->frame_set, e[1]);
    uint32_t lc = t1_ir_add_param(ir, T1_HEADER_BLOCK, T1_I64, e[1]);
    uint32_t p = t1_ir_add_param(ir, T1_HEADER_BLOCK, T1_I1, e[0]);
    t1_ir_edge_add_arg(ir, T1_ENTRY_BLOCK, 0, t1_ir_livein(ir, T1_I64, e[1]), e[1]);
    t1_ir_edge_add_arg(ir, T1_ENTRY_BLOCK, 0,
                       mode >= 2 ? t1_ir_livein(ir, T1_I1, e[0]) : t1_ir_const(ir, T1_I1, 0), e[0]);
    t1_value x = t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(14));
    t1_value y = t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(15));
    t1_value q = t1_ir_livein(ir, T1_I1, T1_ELEMENT_PR(7));
    t1_value next = T1_VALUE(p, 0);
    uint32_t params[8192];
    for (unsigned k = 0; k < n; ++k) {
        if (mode == 3) {
            params[k] = t1_ir_add_param(ir, T1_HEADER_BLOCK, T1_I1, 0);
            t1_ir_edge_add_arg(ir, T1_ENTRY_BLOCK, 0, q, 0);
        } else if (mode == 2)
            next = op(ir, k % 2 ? T1_IR_AND1 : T1_IR_OR1, T1_I1, next, q, 0, 2);
        else
            op(ir, T1_IR_ADD, T1_I64, x, y, 0, 2);
    }
    if (mode == 3)
        next = T1_VALUE(params[0], 0);
    if (mode < 2)
        next = mode ? op(ir, T1_IR_NOT1, T1_I1, next, 0, 0, 1)
                    : op(ir, T1_IR_OR1, T1_I1, next, t1_ir_const(ir, T1_I1, 0), 0, 2);
    t1_value cl = op(ir, T1_IR_CLOOP, T1_I64, T1_VALUE(lc, 0), 0, 0, 1);
    t1_value taken = T1_VALUE(T1_VALUE_OP(cl), 1);
    next = op(ir, T1_IR_SELECT, T1_I1, taken, next, T1_VALUE(p, 0), 3);
    uint16_t back = t1_ir_new_block(ir, 0), out = t1_ir_new_block(ir, 0);
    t1_ir_set_branch(ir, T1_HEADER_BLOCK, taken, back, out, T1_NO_INSN);
    t1_value fv[2] = {next, cl};
    uint32_t ck = t1_ir_new_point(ir, T1_PT_CHECKPOINT, 0x5000, 0, T1_NO_INSN, back, 0);
    t1_ir_set_frame(ir, ck, e, fv, NULL, 2);
    t1_ir_set_backedge(ir, back, T1_HEADER_BLOCK, ck, T1_NO_INSN);
    t1_ir_edge_add_arg(ir, back, 0, cl, e[1]);
    t1_ir_edge_add_arg(ir, back, 0, next, e[0]);
    if (mode == 3)
        for (unsigned k = 0; k < n; ++k)
            t1_ir_edge_add_arg(ir, back, 0, T1_VALUE(params[(k + 1) % n], 0), 0);
    uint32_t point = t1_ir_new_point(ir, T1_PT_EXIT, 0x5000, 0, T1_NO_INSN, out, 0);
    t1_ir_set_frame(ir, point, e, fv, NULL, 2);
    t1_ir_set_exit(ir, out, point, T1_NO_INSN);
}
int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 256;
    unsigned reps = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 1000;
    if (!n || n > 8192 || !reps)
        return 1;
    if (getenv("T1_BENCH_CPU_TIME"))
        bench_clock = CLOCK_THREAD_CPUTIME_ID;
    struct t1_ir *src = calloc(1, sizeof(*src)), *ir = calloc(1, sizeof(*ir));
    struct t1_workspace ws = {calloc(64, 4096), 64, 0};
    if (!src || !ir || !ws.base)
        return 1;
    static const char *names[] = {"constant-loop", "alternating-loop", "dense-bool",
                                  "dense-params"};
    for (unsigned mode = 0; mode < 4; ++mode) {
        build(src, n, mode);
        char err[256];
        if (src->failed || !t1_ir_verify(src, 0, err, sizeof(err)))
            return 1;
        uint64_t total = 0, checksum = 0;
        for (unsigned r = 0; r < reps + 8; ++r) {
            memcpy(ir, src, sizeof(*ir));
            ws.top = 0;
            ir->ws = &ws;
            uint64_t begin = now();
            t1_opt_fold(ir);
            uint64_t elapsed = now() - begin;
            if (ir->failed)
                return 1;
            if (r >= 8)
                total += elapsed;
            if (!r || r + 1 == reps + 8) {
                if (!t1_ir_verify(ir, 0, err, sizeof(err)))
                    return 1;
                uint64_t hash = hash_ir(ir);
                if (r && checksum != hash)
                    return 1;
                checksum = hash;
            }
        }
        printf(
            "bool-fold-bench mode=%s clock=%s operations=%u repetitions=%u ns_per_fold=%.1f checksum=%016" PRIx64
            "\n",
            names[mode], bench_clock == CLOCK_MONOTONIC ? "monotonic" : "thread-cpu", n, reps,
            (double)total / reps, checksum);
    }
    free(ws.base);
    free(ir);
    free(src);
    return 0;
}
