/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Compiled-C benchmark suite (see docs/testing.md): small
 * freestanding kernels compiled by the IA-64 cross GCC at -O2 and run as
 * a flat image (start.S) in the hosted Tier1 harness and, through
 * benchwrap.efi, on the QEMU fork's TCG (once also on vmm-hosted). The same
 * source compiled for the host (native.c) gives each kernel's checksum
 * independently of every IA-64 engine.
 *
 * Every kernel generates its own input from a fixed seed on each call and
 * keeps no state between calls, so repeated runs do identical work. No
 * function pointers, switch tables or pointer-valued initialisers: the
 * image must run unrelocated at any address (start.S, -mauto-pic).
 */
#include <stddef.h>
#include <stdint.h>
#include "csuite.h"

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

static uint64_t xorshift(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

static uint64_t fold(uint64_t h, uint64_t v)
{
    return (h ^ v) * UINT64_C(0x100000001b3) + (h >> 29);
}

/* ---- 0: CRC-32 (IEEE, reflected), table built per call ------------------ */

#define CRC_BYTES 8192
static uint8_t crc_buf[CRC_BYTES];
static uint32_t crc_table[256];

static uint64_t k_crc32(uint64_t iters)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    uint64_t seed = 0x1234567, h = 0;
    for (uint64_t it = 0; it < iters; it++) {
        for (size_t i = 0; i < CRC_BYTES; i += 8) {
            uint64_t r = xorshift(&seed);
            for (int k = 0; k < 8; k++)
                crc_buf[i + k] = (uint8_t)(r >> (8 * k));
        }
        uint32_t crc = 0xffffffffu;
        for (size_t i = 0; i < CRC_BYTES; i++)
            crc = crc_table[(crc ^ crc_buf[i]) & 0xff] ^ (crc >> 8);
        h = fold(h, crc ^ 0xffffffffu);
    }
    return h;
}

/* ---- 1: quicksort (median of three, insertion sort below 16) ------------ */

#define QS_N 2048
static uint32_t qs_a[QS_N];

static void insertion(uint32_t *a, long lo, long hi)
{
    for (long i = lo + 1; i <= hi; i++) {
        uint32_t v = a[i];
        long j = i - 1;
        while (j >= lo && a[j] > v) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}

static void quicksort(uint32_t *a, long lo, long hi)
{
    while (hi - lo > 16) {
        long mid = lo + (hi - lo) / 2;
        if (a[mid] < a[lo]) {
            uint32_t t = a[mid];
            a[mid] = a[lo];
            a[lo] = t;
        }
        if (a[hi] < a[lo]) {
            uint32_t t = a[hi];
            a[hi] = a[lo];
            a[lo] = t;
        }
        if (a[hi] < a[mid]) {
            uint32_t t = a[hi];
            a[hi] = a[mid];
            a[mid] = t;
        }
        uint32_t pivot = a[mid];
        long i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot)
                i++;
            while (a[j] > pivot)
                j--;
            if (i <= j) {
                uint32_t t = a[i];
                a[i] = a[j];
                a[j] = t;
                i++;
                j--;
            }
        }
        /* Recurse into the smaller part, loop on the larger. */
        if (j - lo < hi - i) {
            quicksort(a, lo, j);
            lo = i;
        } else {
            quicksort(a, i, hi);
            hi = j;
        }
    }
    insertion(a, lo, hi);
}

static uint64_t k_qsort(uint64_t iters)
{
    uint64_t seed = 0x9e3779b97f4a7c15u, h = 0;
    for (uint64_t it = 0; it < iters; it++) {
        for (long i = 0; i < QS_N; i++)
            qs_a[i] = (uint32_t)(xorshift(&seed) >> 7);
        quicksort(qs_a, 0, QS_N - 1);
        for (long i = 0; i < QS_N; i += 64)
            h = fold(h, qs_a[i]);
        for (long i = 1; i < QS_N; i++)
            if (qs_a[i - 1] > qs_a[i])
                return 0; /* unsorted: wrong checksum */
    }
    return h;
}

/* ---- 2: integer matrix multiply ---------------------------------------- */

#define MM_N 32
static int64_t mm_a[MM_N][MM_N], mm_b[MM_N][MM_N], mm_c[MM_N][MM_N];

static uint64_t k_matmul(uint64_t iters)
{
    uint64_t seed = 0xdeadbeefcafef00du, h = 0;
    for (uint64_t it = 0; it < iters; it++) {
        for (int i = 0; i < MM_N; i++)
            for (int j = 0; j < MM_N; j++) {
                mm_a[i][j] = (int64_t)(xorshift(&seed) % 2001) - 1000;
                mm_b[i][j] = (int64_t)(xorshift(&seed) % 2001) - 1000;
            }
        for (int i = 0; i < MM_N; i++)
            for (int j = 0; j < MM_N; j++) {
                int64_t s = 0;
                for (int k = 0; k < MM_N; k++)
                    s += mm_a[i][k] * mm_b[k][j];
                mm_c[i][j] = s;
            }
        for (int i = 0; i < MM_N; i++)
            h = fold(h, (uint64_t)mm_c[i][(i * 7) % MM_N]);
    }
    return h;
}

/* ---- 3: string search (Horspool), small alphabet ------------------------ */

#define SS_TEXT 16384
#define SS_PATS 8
static uint8_t ss_text[SS_TEXT];

static uint64_t horspool(const uint8_t *t, size_t n, const uint8_t *p, size_t m)
{
    size_t shift[256];
    for (int i = 0; i < 256; i++)
        shift[i] = m;
    for (size_t i = 0; i + 1 < m; i++)
        shift[p[i]] = m - 1 - i;
    uint64_t count = 0, posh = 0;
    for (size_t i = 0; i + m <= n;) {
        size_t k = m;
        while (k > 0 && t[i + k - 1] == p[k - 1])
            k--;
        if (!k) {
            count++;
            posh = posh * 31 + i;
        }
        i += shift[t[i + m - 1]];
    }
    return count ^ (posh << 20);
}

static uint64_t k_strsearch(uint64_t iters)
{
    uint64_t seed = 0x0123456789abcdefu, h = 0;
    for (uint64_t it = 0; it < iters; it++) {
        for (size_t i = 0; i < SS_TEXT; i++)
            ss_text[i] = (uint8_t)('a' + xorshift(&seed) % 4);
        for (int k = 0; k < SS_PATS; k++) {
            uint8_t pat[12];
            size_t m = 4 + (size_t)k;
            size_t at = (size_t)(xorshift(&seed) % (SS_TEXT - m));
            for (size_t i = 0; i < m; i++)
                pat[i] = ss_text[at + i];
            h = fold(h, horspool(ss_text, SS_TEXT, pat, m));
        }
    }
    return h;
}

/* ---- 4: open-addressing hash table -------------------------------------- */

#define HT_SLOTS 4096
#define HT_KEYS 3000
static uint64_t ht_key[HT_SLOTS], ht_val[HT_SLOTS];

static uint64_t k_hash(uint64_t iters)
{
    uint64_t seed = 0x5555aaaa3333ccccu, h = 0;
    for (uint64_t it = 0; it < iters; it++) {
        for (int i = 0; i < HT_SLOTS; i++)
            ht_key[i] = ht_val[i] = 0;
        uint64_t base = seed;
        for (int i = 0; i < HT_KEYS; i++) {
            uint64_t k = (xorshift(&seed) | 1), v = (uint64_t)i;
            uint64_t s = (k * UINT64_C(0x9e3779b97f4a7c15)) >> 52;
            while (ht_key[s] && ht_key[s] != k)
                s = (s + 1) & (HT_SLOTS - 1);
            ht_key[s] = k;
            ht_val[s] += v;
        }
        /* Look up the inserted keys again plus as many absent ones. */
        uint64_t again = base, found = 0, sum = 0;
        for (int i = 0; i < 2 * HT_KEYS; i++) {
            uint64_t k =
                i < HT_KEYS ? (xorshift(&again) | 1) : (xorshift(&again) & ~UINT64_C(1)) + 2;
            uint64_t s = (k * UINT64_C(0x9e3779b97f4a7c15)) >> 52;
            while (ht_key[s] && ht_key[s] != k)
                s = (s + 1) & (HT_SLOTS - 1);
            if (ht_key[s] == k) {
                found++;
                sum += ht_val[s];
            }
        }
        h = fold(fold(h, found), sum);
    }
    return h;
}

/* ---- 5: LZ77-style compressor and decompressor -------------------------- */

#define LZ_IN 8192
#define LZ_HASH 4096
static uint8_t lz_in[LZ_IN], lz_out[2 * LZ_IN + 16], lz_back[LZ_IN];
static uint32_t lz_head[LZ_HASH];

/* Tokens: 0x00 len(1) bytes... literal run (len <= 255); 0x01 len(1)
 * off(2) match (len 3..258, off 1..65535). */
static size_t lz_compress(const uint8_t *in, size_t n, uint8_t *out)
{
    for (int i = 0; i < LZ_HASH; i++)
        lz_head[i] = 0xffffffffu;
    size_t o = 0, lit = 0, i = 0;
    while (i < n) {
        size_t best = 0, off = 0;
        if (i + 3 <= n) {
            uint32_t hv =
                ((uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2]) * 2654435761u >> 20;
            uint32_t cand = lz_head[hv];
            lz_head[hv] = (uint32_t)i;
            if (cand != 0xffffffffu && i - cand < 65536) {
                size_t l = 0;
                while (i + l < n && l < 258 && in[cand + l] == in[i + l])
                    l++;
                if (l >= 3) {
                    best = l;
                    off = i - cand;
                }
            }
        }
        if (best) {
            if (lit) {
                out[o++] = 0;
                out[o++] = (uint8_t)lit;
                memcpy(out + o, in + i - lit, lit);
                o += lit;
                lit = 0;
            }
            out[o++] = 1;
            out[o++] = (uint8_t)(best - 3);
            out[o++] = (uint8_t)off;
            out[o++] = (uint8_t)(off >> 8);
            i += best;
        } else {
            i++;
            if (++lit == 255) {
                out[o++] = 0;
                out[o++] = (uint8_t)lit;
                memcpy(out + o, in + i - lit, lit);
                o += lit;
                lit = 0;
            }
        }
    }
    if (lit) {
        out[o++] = 0;
        out[o++] = (uint8_t)lit;
        memcpy(out + o, in + n - lit, lit);
        o += lit;
    }
    return o;
}

static size_t lz_decompress(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t i = 0, o = 0;
    while (i < n) {
        if (in[i] == 0) {
            size_t l = in[i + 1];
            memcpy(out + o, in + i + 2, l);
            o += l;
            i += 2 + l;
        } else {
            size_t l = (size_t)in[i + 1] + 3, off = in[i + 2] | (size_t)in[i + 3] << 8;
            for (size_t k = 0; k < l; k++, o++)
                out[o] = out[o - off];
            i += 4;
        }
    }
    return o;
}

static uint64_t k_lz(uint64_t iters)
{
    uint64_t seed = 0x2468ace013579bdfu, h = 0;
    for (uint64_t it = 0; it < iters; it++) {
        /* Text-like input: words from a small vocabulary with noise. */
        size_t n = 0;
        while (n < LZ_IN) {
            uint64_t r = xorshift(&seed);
            size_t w = 3 + r % 6, base = (r >> 8) % 24;
            for (size_t k = 0; k < w && n < LZ_IN; k++)
                lz_in[n++] = (uint8_t)('a' + (base + k * (r >> 40 & 3)) % 26);
            if (n < LZ_IN)
                lz_in[n++] = (r >> 20) % 7 ? ' ' : (uint8_t)('0' + (r >> 24) % 10);
        }
        size_t c = lz_compress(lz_in, LZ_IN, lz_out);
        size_t d = lz_decompress(lz_out, c, lz_back);
        uint64_t same = d == LZ_IN;
        for (size_t i = 0; i < LZ_IN && same; i++)
            same = lz_back[i] == lz_in[i];
        if (!same)
            return 0;
        uint64_t s = 0;
        for (size_t i = 0; i < c; i++)
            s = s * 131 + lz_out[i];
        h = fold(fold(h, c), s);
    }
    return h;
}

uint64_t csuite_main(uint64_t kernel, uint64_t iters)
{
    if (kernel == 0)
        return k_crc32(iters);
    if (kernel == 1)
        return k_qsort(iters);
    if (kernel == 2)
        return k_matmul(iters);
    if (kernel == 3)
        return k_strsearch(iters);
    if (kernel == 4)
        return k_hash(iters);
    if (kernel == 5)
        return k_lz(iters);
    return 0;
}
