/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64vmm/string.h"
#include <stdint.h>

/* Below STRING_REP_MIN bytes, 8-byte moves and a byte tail; from there
 * `rep movsq`/`rep stosq` for the 8-byte words, whose start-up cost is
 * amortized, and the same byte tail. Quadword strings are fast on every
 * supported host, ERMSB (byte strings) only from Ivy Bridge. The ABI and
 * every VMM entry path keep DF clear. */
#define STRING_REP_MIN 256u
typedef uint64_t __attribute__((may_alias, aligned(1))) string_word;

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
#if defined(__x86_64__)
    if (n >= STRING_REP_MIN) {
        size_t words = n / 8;
        __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(words) : : "memory");
        n %= 8;
    }
#endif
    for (; n >= 8; n -= 8, d += 8, s += 8)
        *(string_word *)d = *(const string_word *)s;
    for (; n; --n)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    uint64_t word = UINT64_C(0x0101010101010101) * (unsigned char)c;
#if defined(__x86_64__)
    if (n >= STRING_REP_MIN) {
        size_t words = n / 8;
        __asm__ volatile("rep stosq" : "+D"(d), "+c"(words) : "a"(word) : "memory");
        n %= 8;
    }
#endif
    for (; n >= 8; n -= 8, d += 8)
        *(string_word *)d = word;
    for (; n; --n)
        *d++ = (unsigned char)c;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || !n)
        return dst;
    /* Integer comparison also works for unrelated address ranges. */
    if ((uintptr_t)d < (uintptr_t)s) {
        if ((uintptr_t)s - (uintptr_t)d >= n)
            return memcpy(dst, src, n);
        /* Load each complete word before its overlapping store. Never
         * pass an overlapping range to memcpy's restrict-qualified API. */
        for (; n >= 8; n -= 8, d += 8, s += 8) {
            string_word word = *(const string_word *)s;
            *(string_word *)d = word;
        }
        for (; n; --n)
            *d++ = *s++;
    } else {
        if ((uintptr_t)d - (uintptr_t)s >= n)
            return memcpy(dst, src, n);
        while (n >= 8) {
            n -= 8;
            string_word word = *(const string_word *)(s + n);
            *(string_word *)(d + n) = word;
        }
        while (n) {
            --n;
            d[n] = s[n];
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    /* Compare only complete words inside the requested ranges. When a
     * word differs, retain the unsigned-byte lexicographic order below;
     * integer word order would be wrong on little-endian hosts. */
    for (; n >= 8; n -= 8, x += 8, y += 8)
        if (*(const string_word *)x != *(const string_word *)y)
            break;
    for (size_t i = 0; i < n; ++i)
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i] || !a[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}
