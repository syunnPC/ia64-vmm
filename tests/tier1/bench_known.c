/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional known-bits phase benchmark. The loop control requires the
 * original fixed point; the straight input has no block parameters. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "engine/tier1/ir.h"

static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t))
        abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}

static void build(struct t1_ir *ir, unsigned n, bool loop)
{
    t1_ir_init(ir, NULL);
    t1_ir_new_block(ir, 0);
    t1_ir_new_block(ir, 0);
    t1_ir_set_jump(ir, T1_ENTRY_BLOCK, T1_HEADER_BLOCK);
    uint16_t e = T1_ELEMENT_GR(20);
    t1_elem_add(ir->frame_set, e);
    t1_value v = t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(14));
    if (loop) {
        uint32_t p = t1_ir_add_param(ir, T1_HEADER_BLOCK, T1_I64, e);
        t1_ir_edge_add_arg(ir, T1_ENTRY_BLOCK, 0, v, e);
        v = T1_VALUE(p, 0);
    }
    for (unsigned k = 0; k < n; ++k) {
        static const uint16_t ops[] = {T1_IR_ZXT, T1_IR_ADD, T1_IR_SHL_IMM, T1_IR_OR};
        t1_value a[2] = {v, t1_ir_const(ir, T1_I64, k % 23 + 1)};
        unsigned op = k % 4, nargs = op % 2 ? 2 : 1;
        int64_t imm = op == 0 ? 16 : op == 2 ? 1 : 0;
        v = T1_VALUE(t1_ir_append(ir, T1_HEADER_BLOCK, ops[op], T1_I64, imm, a, nargs, T1_NO_INSN),
                     0);
    }
    uint32_t pt = t1_ir_new_point(ir, loop ? T1_PT_CHECKPOINT : T1_PT_EXIT, 0x5000, 0, T1_NO_INSN,
                                  T1_HEADER_BLOCK, 0);
    t1_ir_set_frame(ir, pt, &e, &v, NULL, 1);
    if (loop) {
        t1_ir_set_backedge(ir, T1_HEADER_BLOCK, T1_HEADER_BLOCK, pt, T1_NO_INSN);
        t1_ir_edge_add_arg(ir, T1_HEADER_BLOCK, 0, v, e);
    } else
        t1_ir_set_exit(ir, T1_HEADER_BLOCK, pt, T1_NO_INSN);
}

static uint64_t hash_known(const struct t1_known *kb, unsigned n)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (unsigned o = 0; o < n; ++o) {
        h = (h ^ kb[o].zero) * UINT64_C(1099511628211);
        h = (h ^ kb[o].one) * UINT64_C(1099511628211);
    }
    return h;
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 256;
    unsigned reps = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 5000;
    if (!n || n > 8192 || !reps)
        return 1;
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    struct t1_known *kb = malloc(T1_IR_MAX_OPS * sizeof(*kb));
    if (!ir || !kb)
        return 1;
    for (unsigned mode = 0; mode < 2; ++mode) {
        build(ir, n, mode != 0);
        char err[256];
        if (ir->failed || !t1_ir_verify(ir, 0, err, sizeof(err)))
            return 1;
        uint64_t total = 0, checksum = 0;
        for (unsigned r = 0; r < reps + 8; ++r) {
            uint64_t begin = now();
            t1_known_bits(ir, kb);
            uint64_t elapsed = now() - begin;
            if (r >= 8)
                total += elapsed;
            if (!r || r + 1 == reps + 8) {
                uint64_t h = hash_known(kb, ir->nops);
                if (r && checksum != h)
                    return 1;
                checksum = h;
            }
        }
        printf(
            "known-bench mode=%s operations=%u repetitions=%u ns_per_analysis=%.1f checksum=%016" PRIx64
            " clock=thread-cpu\n",
            mode ? "loop" : "straight", n, reps, (double)total / reps, checksum);
    }
    free(kb);
    free(ir);
    return 0;
}
