/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Builder-only control and PARAM-prefix measurement. Full initialization,
 * op allocation, edge arguments and final frame construction are timed.
 * Verification and a semantic IR print checksum are outside the timer. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "engine/tier1/ir.h"
#include "engine/tier1/memory.h"

#ifndef T1_BUILD_ONLY
static uint64_t cpu_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts))
        abort();
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}
static void hash_line(void *arg, const char *s)
{
    uint64_t *hash = arg;
    for (; *s; ++s)
        *hash = (*hash ^ (unsigned char)*s) * UINT64_C(1099511628211);
}
#endif
#ifdef T1_BUILD_ONLY
void T1_BUILD_ONLY(struct t1_ir *ir, unsigned count, bool parameters);
void T1_BUILD_ONLY(struct t1_ir *ir, unsigned count, bool parameters)
#else
static void build(struct t1_ir *ir, unsigned count, bool parameters)
#endif
{
    t1_ir_init(ir, NULL);
    t1_ir_new_block(ir, 0);
    t1_ir_new_block(ir, 0);
    t1_ir_set_jump(ir, T1_ENTRY_BLOCK, T1_HEADER_BLOCK);
    uint16_t e = T1_ELEMENT_GR(14);
    t1_value x = t1_ir_livein(ir, T1_I64, e), c = t1_ir_const(ir, T1_I64, 7);
    t1_value result = x;
    uint32_t point = t1_ir_new_point(ir, T1_PT_EXIT, 0x5000, 0, T1_NO_INSN, T1_HEADER_BLOCK, 0);
    t1_ir_set_exit(ir, T1_HEADER_BLOCK, point, T1_NO_INSN);
    for (unsigned k = 0; k < count; ++k) {
        if (parameters) {
            result = T1_VALUE(t1_ir_add_param(ir, T1_HEADER_BLOCK, T1_I64, 0), 0);
            t1_ir_edge_add_arg(ir, T1_ENTRY_BLOCK, 0, k & 1 ? c : x, 0);
        } else {
            t1_value operands[] = {result, c};
            result = T1_VALUE(
                t1_ir_append(ir, T1_HEADER_BLOCK, T1_IR_ADD, 0, 0, operands, 2, T1_NO_INSN), 0);
        }
    }
    t1_elem_add(ir->frame_set, e);
    t1_ir_set_frame(ir, point, &e, &result, NULL, 1);
}

#ifndef T1_BUILD_ONLY
int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 32;
    unsigned repeat = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 10000;
    if (!n || n > 8192 || !repeat)
        return 1;
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    struct t1_workspace ws = {calloc(128, 4096), 128, 0};
    if (!ir || !ws.base)
        return 1;
    ir->ws = &ws;
    for (unsigned mode = 0; mode < 2; ++mode) {
        build(ir, n, mode != 0);
        char err[256];
        if (ir->failed || !t1_ir_verify(ir, 0, err, sizeof(err)))
            return 2;
        for (unsigned warm = 0; warm < 8; ++warm)
            build(ir, n, mode != 0);
        uint64_t start = cpu_ns();
        for (unsigned k = 0; k < repeat; ++k)
            build(ir, n, mode != 0);
        uint64_t elapsed = cpu_ns() - start;
        if (ir->failed || !t1_ir_verify(ir, 0, err, sizeof(err)))
            return 2;
        uint64_t hash = UINT64_C(1469598103934665603);
        t1_ir_print(ir, hash_line, &hash);
        printf("ir-build-bench mode=%s n=%u repetitions=%u ns_per_build=%.3f checksum=%016" PRIx64
               " block_bytes=%zu ir_bytes=%zu\n",
               mode ? "parameters" : "append", n, repeat, (double)elapsed / repeat, hash,
               sizeof(struct t1_block), sizeof(struct t1_ir));
    }
    free(ws.base);
    free(ir);
    return 0;
}
#endif
