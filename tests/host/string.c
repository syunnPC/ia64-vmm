/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * core/string.c against the C library's definitions of memcpy, memset,
 * memmove, memcmp, strlen, strcmp and strncmp (C11 §7.24): every length
 * 0..300 and a few large ones, every source/destination alignment 0..15,
 * overlapping moves both ways, and the bytes around the destination left
 * as they were. core/string.c is linked under renamed symbols
 * (build/hosted/tests/host/vmm_string.o), so the C library stays in use.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "test.h"

void *vmm_memcpy(void *restrict dst, const void *restrict src, size_t n);
void *vmm_memset(void *dst, int c, size_t n);
void *vmm_memmove(void *dst, const void *src, size_t n);
int vmm_memcmp(const void *a, const void *b, size_t n);
size_t vmm_strlen(const char *s);
int vmm_strcmp(const char *a, const char *b);
int vmm_strncmp(const char *a, const char *b, size_t n);

#define AREA 80000u
#define GUARD 64u
static unsigned char got[AREA], want[AREA], src[AREA];

static uint64_t state = 0x9e3779b97f4a7c15u;
static unsigned char rnd(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return (unsigned char)state;
}
static void fill(unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        p[i] = rnd();
}

static int sign(int x)
{
    return (x > 0) - (x < 0);
}

static void lengths(size_t *list, unsigned *count)
{
    unsigned n = 0;
    for (size_t len = 0; len <= 300; ++len)
        list[n++] = len;
    static const size_t big[] = {511, 512, 513, 4096, 4097, 70000};
    for (unsigned i = 0; i < sizeof(big) / sizeof(big[0]); ++i)
        list[n++] = big[i];
    *count = n;
}

static void memmove_bounds(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *mapping = mmap(NULL, 6 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED);
    unsigned char *a = mapping + page, *b = mapping + 4 * page;
    CHECK(!mprotect(a, page, PROT_READ | PROT_WRITE));
    CHECK(!mprotect(b, page, PROT_READ | PROT_WRITE));
    for (size_t n = 0; n <= 300; ++n) {
        for (unsigned end = 0; end < 2; ++end) {
            unsigned char *src_at = end ? a + page - n : a;
            unsigned char *dst_at = end ? b + page - n : b;
            fill(src_at, n);
            CHECK(vmm_memmove(dst_at, src_at, n) == dst_at);
            CHECK(!memcmp(dst_at, src_at, n));
            CHECK(vmm_memmove(src_at, src_at, n) == src_at);
            /* Opposite disjoint address order uses the same exact bounds. */
            CHECK(vmm_memmove(src_at, dst_at, n) == src_at);
        }
        for (unsigned shift = 1; shift <= 16; ++shift) {
            for (unsigned end = 0; end < 2; ++end) {
                unsigned char *lo = end ? a + page - n - shift : a;
                for (unsigned backward = 0; backward < 2; ++backward) {
                    fill(lo, n + shift);
                    memcpy(want, lo, n + shift);
                    size_t dst = backward ? shift : 0, src_at = backward ? 0 : shift;
                    memmove(want + dst, want + src_at, n);
                    CHECK(vmm_memmove(lo + dst, lo + src_at, n) == lo + dst);
                    CHECK(!memcmp(lo, want, n + shift));
                }
            }
        }
    }
    CHECK(!munmap(mapping, 6 * page));
}

static void memcmp_bounds(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *mapping = mmap(NULL, 6 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED);
    unsigned char *a = mapping + page, *b = mapping + 4 * page;
    CHECK(!mprotect(a, page, PROT_READ | PROT_WRITE));
    CHECK(!mprotect(b, page, PROT_READ | PROT_WRITE));
    for (size_t n = 0; n <= 300; ++n) {
        for (unsigned align = 0; align < 16; ++align) {
            /* One range touches the protected page, the other has each
             * possible unaligned starting address. Exercise both orders. */
            unsigned char *x = a + page - n, *y = b + align;
            fill(x, n);
            memcpy(y, x, n);
            CHECK(vmm_memcmp(x, y, n) == 0);
            CHECK(vmm_memcmp(y, x, n) == 0);
            for (size_t at = 0; at < n; ++at) {
                unsigned char keep = y[at];
                y[at] ^= 0x80;
                CHECK(sign(vmm_memcmp(x, y, n)) == sign(memcmp(x, y, n)));
                CHECK(sign(vmm_memcmp(y, x, n)) == sign(memcmp(y, x, n)));
                y[at] = keep;
            }
        }
    }
    /* First unequal byte decides, even when a later byte orders opposite. */
    for (unsigned first = 0; first < 16; ++first)
        for (unsigned later = first + 1; later < 24; ++later) {
            memset(a, 0, 24);
            memset(b, 0, 24);
            b[first] = 1;
            a[later] = 255;
            CHECK(vmm_memcmp(a, b, 24) < 0 && vmm_memcmp(b, a, 24) > 0);
        }
    CHECK(!munmap(mapping, 6 * page));
}

void test_string(void)
{
    memmove_bounds();
    memcmp_bounds();
    static size_t len[320];
    unsigned nlen;
    lengths(len, &nlen);
    fill(src, AREA);
    for (unsigned l = 0; l < nlen; ++l) {
        size_t n = len[l];
        if (n > AREA - 2 * GUARD - 512)
            continue; /* bounds for the reference calls too */
        unsigned max_align = n > 300 ? 3 : 16;
        for (unsigned da = 0; da < max_align; ++da) {
            for (unsigned sa = 0; sa < max_align; ++sa) {
                /* memcpy: n bytes from src + sa to GUARD + da. */
                fill(got, n + 2 * GUARD + 16);
                memcpy(want, got, n + 2 * GUARD + 16);
                memcpy(want + GUARD + da, src + sa, n);
                CHECK(vmm_memcpy(got + GUARD + da, src + sa, n) == got + GUARD + da);
                CHECK(!memcmp(got, want, n + 2 * GUARD + 16));
            }
            static const int values[] = {0, 0xa5, 0x1ff, -1, -0x80};
            for (unsigned c = 0; c < sizeof(values) / sizeof(values[0]); ++c) {
                fill(got, n + 2 * GUARD + 16);
                memcpy(want, got, n + 2 * GUARD + 16);
                memset(want + GUARD + da, values[c], n);
                CHECK(vmm_memset(got + GUARD + da, values[c], n) == got + GUARD + da);
                CHECK(!memcmp(got, want, n + 2 * GUARD + 16));
            }
        }
        /* memmove within one buffer, destination below and above the source. */
        static const size_t shifts[] = {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 64, 255, 256, 257};
        for (unsigned k = 0; k < sizeof(shifts) / sizeof(shifts[0]); ++k)
            for (unsigned dir = 0; dir < 2; ++dir) {
                size_t lo = GUARD, hi = GUARD + shifts[k];
                size_t d = dir ? hi : lo, s = dir ? lo : hi;
                fill(got, n + shifts[k] + 2 * GUARD);
                memcpy(want, got, n + shifts[k] + 2 * GUARD);
                memmove(want + d, want + s, n);
                CHECK(vmm_memmove(got + d, got + s, n) == got + d);
                CHECK(!memcmp(got, want, n + shifts[k] + 2 * GUARD));
            }
        /* memcmp: equal, then one byte differing at each end and inside. */
        if (n && n <= 300) {
            fill(got, n);
            memcpy(want, got, n);
            CHECK(vmm_memcmp(got, want, n) == 0);
            static const size_t at_list[] = {0, 1, 7, 8};
            for (unsigned a = 0; a < 5; ++a) {
                size_t at = a < 4 ? at_list[a] : n - 1;
                if (at >= n)
                    continue;
                unsigned char keep = want[at];
                want[at] = (unsigned char)(keep + 1 + (rnd() % 254));
                CHECK(sign(vmm_memcmp(got, want, n)) == sign(memcmp(got, want, n)));
                CHECK(sign(vmm_memcmp(want, got, n)) == sign(memcmp(want, got, n)));
                want[at] = keep;
            }
        }
    }
    static const char *const strs[] = {"", "a", "ab", "abc", "abd", "ab\xff", "abcdefghijklmnop"};
    for (unsigned i = 0; i < sizeof(strs) / sizeof(strs[0]); ++i) {
        CHECK(vmm_strlen(strs[i]) == strlen(strs[i]));
        for (unsigned j = 0; j < sizeof(strs) / sizeof(strs[0]); ++j) {
            CHECK(sign(vmm_strcmp(strs[i], strs[j])) == sign(strcmp(strs[i], strs[j])));
            for (size_t n = 0; n < 6; ++n)
                CHECK(sign(vmm_strncmp(strs[i], strs[j], n)) == sign(strncmp(strs[i], strs[j], n)));
        }
    }
}
