/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compiled-C benchmark suite: see csuite.c. */
#ifndef IA64VMM_TESTS_BENCH_CSUITE_H
#define IA64VMM_TESTS_BENCH_CSUITE_H
#include <stddef.h>
#include <stdint.h>

#define CSUITE_KERNELS 6
/* Runs kernel 0..5 (crc32, qsort, matmul, strsearch, hash, lz) `iters`
 * times; returns its checksum (0 if a kernel's self-check failed). */
uint64_t csuite_main(uint64_t kernel, uint64_t iters);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
#endif
