/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native microbenchmark, linked to the freestanding string implementation
 * under renamed symbols so the harness continues to use the host libc. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void *vmm_memmove(void *, const void *, size_t);
int vmm_memcmp(const void *, const void *, size_t);
static unsigned char area[262144], other[262144];

static uint64_t now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        abort();
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

int main(int argc, char **argv)
{
    if (argc != 7)
        return 2;
    int compare = !strcmp(argv[1], "memcmp");
    if (!compare && strcmp(argv[1], "memmove"))
        return 2;
    size_t n = strtoull(argv[2], NULL, 0), align = strtoull(argv[4], NULL, 0);
    long shift = strtol(argv[3], NULL, 0);
    uint64_t iterations = strtoull(argv[5], NULL, 0), sum = 0;
    if (n > 65536 || align > 15 || shift < -65552 || shift > 65552 || !iterations)
        return 2;
    for (size_t i = 0; i < sizeof(area); ++i)
        area[i] = (unsigned char)(i * 37 + i / 251);
    unsigned char *src = area + 65568 + align, *dst = src + shift;
    if (compare) {
        dst = other + 64 + align;
        memcpy(dst, src, n);
        if (n && !strcmp(argv[6], "first"))
            dst[0] ^= 0x80;
        else if (n && !strcmp(argv[6], "last"))
            dst[n - 1] ^= 0x80;
        else if (strcmp(argv[6], "equal"))
            return 2;
        int want = memcmp(src, dst, n), got = vmm_memcmp(src, dst, n);
        if ((want > 0) - (want < 0) != (got > 0) - (got < 0))
            abort();
    }
    /* Fixed warmup is identical in both builds and is outside the timer. */
    for (unsigned i = 0; i < 200; ++i) {
        if (compare)
            sum += (uint64_t)vmm_memcmp(src, dst, n);
        else if (vmm_memmove(dst, src, n) != dst)
            abort();
    }
    uint64_t start = now();
    if (compare)
        for (uint64_t i = 0; i < iterations; ++i)
            sum += (uint64_t)vmm_memcmp(src, dst, n);
    else
        for (uint64_t i = 0; i < iterations; ++i)
            vmm_memmove(dst, src, n);
    uint64_t elapsed = now() - start;
    for (size_t i = 0; i < n; ++i)
        sum = (sum ^ dst[i]) * UINT64_C(1099511628211);
    printf("{\"ns_per_call\":%.6f,\"checksum\":\"%016" PRIx64 "\"}\n", (double)elapsed / iterations,
           sum);
    return 0;
}
