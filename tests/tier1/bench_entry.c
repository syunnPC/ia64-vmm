/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional hot CONST/LIVEIN lookup measurement, without new insertions
 * inside the timer. Report the checksum and check every returned value
 * against the identities established while building the IR. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "engine/tier1/ir.h"
#include "engine/tier1/memory.h"

static uint64_t now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}

static t1_value lookup(struct t1_ir *ir, unsigned mode, unsigned k)
{
    return mode == 1 || (mode == 2 && k % 2)
               ? t1_ir_livein(ir, T1_I64, T1_ELEMENT_GR(k + 1))
               : t1_ir_const(ir, T1_I64, UINT64_C(0x8000000000000000) + (uint64_t)k * 65537);
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 16;
    unsigned reps = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 2000000;
    if (!n || n > 127 || !reps)
        return 1;
    struct t1_ir *ir = calloc(1, sizeof(*ir));
    struct t1_workspace ws = {calloc(64, 4096), 64, 0};
    if (!ir || !ws.base)
        return 1;
    static const char *names[] = {"constant", "livein", "mixed"};
    for (unsigned mode = 0; mode < 3; ++mode) {
        t1_ir_init(ir, NULL);
        ir->ws = &ws;
        t1_ir_new_block(ir, 0);
        t1_ir_new_block(ir, 0);
        t1_ir_set_jump(ir, T1_ENTRY_BLOCK, T1_HEADER_BLOCK);
        t1_value values[127];
        for (unsigned k = 0; k < n; ++k)
            values[k] = lookup(ir, mode, k);
        uint32_t point = t1_ir_new_point(ir, T1_PT_EXIT, 0x5000, 0, T1_NO_INSN, T1_HEADER_BLOCK, 0);
        t1_ir_set_frame(ir, point, NULL, NULL, NULL, 0);
        t1_ir_set_exit(ir, T1_HEADER_BLOCK, point, T1_NO_INSN);
        char err[256];
        if (ir->failed || !t1_ir_verify(ir, 0, err, sizeof(err)))
            return 1;
        for (unsigned warm = 0; warm < 1000; ++warm)
            if (lookup(ir, mode, warm % n) != values[warm % n])
                return 1;
        unsigned before = ir->nops;
        uint64_t checksum = 0, begin = now();
        unsigned k = 0;
        for (unsigned r = 0; r < reps; ++r) {
            checksum += lookup(ir, mode, k);
            if (++k == n)
                k = 0;
        }
        uint64_t elapsed = now() - begin, expected = 0;
        for (unsigned j = 0; j < n; ++j)
            expected += (uint64_t)values[j] * (reps / n + (j < reps % n));
        if (ir->nops != before || ir->failed || checksum != expected)
            return 1;
        printf(
            "entry-bench mode=%s entries=%u repetitions=%u ns_per_lookup=%.3f checksum=%016" PRIx64
            "\n",
            names[mode], n, reps, (double)elapsed / reps, checksum);
    }
    free(ws.base);
    free(ir);
    return 0;
}
