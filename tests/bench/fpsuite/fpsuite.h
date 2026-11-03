/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Floating-point benchmark suite: see fpsuite.c. */
#ifndef IA64VMM_TESTS_BENCH_FPSUITE_H
#define IA64VMM_TESTS_BENCH_FPSUITE_H
#include <stdint.h>

#define FPSUITE_KERNELS 8
#define FPSUITE_OUT_MAX 1024
/* Runs kernel 0..7 (matmul, fft, nbody, transcendental, reduction,
 * matmul-single, parallel-single, extended) `iters` times; returns its
 * checksum. */
uint64_t fpsuite_main(uint64_t kernel, uint64_t iters);
/* Result words of the last iteration of the last call and their count: a
 * double as its 64 bits, a float as its 32 bits zero-extended, a long
 * double as its significand word and its sign/exponent word. */
extern uint64_t fpsuite_out[FPSUITE_OUT_MAX];
extern uint64_t fpsuite_out_count;
#endif
