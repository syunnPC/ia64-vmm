/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Floating-point benchmark suite (docs/bench/engines-2026-10-09.md): eight
 * freestanding kernels compiled by the IA-64 cross GCC at -O2 with the
 * default FP contraction (fma.d, fma.s, fpma, fpms, fpnma) and run as a
 * flat image (start.S) with the Linux default FPSR. Division and square
 * root are the compiler's inline frcpa/frsqrta sequences, which run in
 * status field 1.
 *
 * Every kernel regenerates its inputs from a fixed seed on each iteration
 * (values with full significands and random signs), keeps no state between
 * calls and has no branch on a floating-point value, so repeated runs
 * retire the same instructions. Each iteration stores its result words to
 * fpsuite_out and folds them into the checksum. No function pointers,
 * switch tables or pointer-valued initialisers: the image must run
 * unrelocated at any address (start.S, -mauto-pic).
 */
#include <stddef.h>
#include <stdint.h>
#include "fpsuite.h"

uint64_t fpsuite_out[FPSUITE_OUT_MAX];
uint64_t fpsuite_out_count;

union dbits {
    double d;
    uint64_t u;
};
union fbits {
    float f;
    uint32_t u;
};
typedef float v2sf __attribute__((vector_size(8)));
union vbits {
    v2sf v;
    uint32_t u[2];
};

static double d_from(uint64_t u)
{
    union dbits x = {.u = u};
    return x.d;
}
static uint64_t d_bits(double d)
{
    union dbits x = {.d = d};
    return x.u;
}
static float f_from(uint32_t u)
{
    union fbits x = {.u = u};
    return x.f;
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

/* Random sign, exponent 2^-1 .. 2^-4 and 52 random fraction bits: a double
 * with a full 53-bit significand, 1/16 <= |x| < 1. */
static double rnd_d(uint64_t *s)
{
    uint64_t r = xorshift(s);
    return d_from((r & (UINT64_C(1) << 63)) | (uint64_t)(0x3fe - ((r >> 52) & 3)) << 52 |
                  (r & ((UINT64_C(1) << 52) - 1)));
}

/* The same for a float: 24-bit significand, 1/16 <= |x| < 1. */
static float rnd_f(uint64_t *s)
{
    uint64_t r = xorshift(s);
    return f_from((uint32_t)(r >> 63) << 31 | (uint32_t)(0x7e - ((r >> 32) & 3)) << 23 |
                  ((uint32_t)r & 0x7fffff));
}

static uint64_t emit_d(uint64_t h, size_t at, double v)
{
    uint64_t b = d_bits(v);
    fpsuite_out[at] = b;
    return fold(h, b);
}

static uint64_t emit_f(uint64_t h, size_t at, float v)
{
    union fbits x = {.f = v};
    fpsuite_out[at] = x.u;
    return fold(h, x.u);
}

/* ---- 0: dense matrix multiply, double ----------------------------------- */

#define MM_N 32
static double mm_a[MM_N][MM_N], mm_b[MM_N][MM_N], mm_c[MM_N][MM_N];

static uint64_t k_matmul(uint64_t iters)
{
    uint64_t seed = 0x9e3779b97f4a7c15u, h = 0;
    fpsuite_out_count = MM_N * MM_N;
    for (uint64_t it = 0; it < iters; it++) {
        for (int i = 0; i < MM_N; i++)
            for (int j = 0; j < MM_N; j++) {
                mm_a[i][j] = rnd_d(&seed);
                mm_b[i][j] = rnd_d(&seed);
            }
        for (int i = 0; i < MM_N; i++)
            for (int j = 0; j < MM_N; j++) {
                double acc = 0.0;
                for (int k = 0; k < MM_N; k++)
                    acc += mm_a[i][k] * mm_b[k][j];
                mm_c[i][j] = acc;
            }
        for (int i = 0; i < MM_N; i++)
            for (int j = 0; j < MM_N; j++)
                h = emit_d(h, i * MM_N + j, mm_c[i][j]);
    }
    return h;
}

/* ---- 3 (shared): sin, cos and exp by range reduction and polynomials ---- */

/* x * c + 1.5 * 2^52 rounds x * c to an integer in the low mantissa bits. */
#define ROUND_MAGIC 6755399441055744.0
#define TWO_OVER_PI 0x1.45f306dc9c883p-1
#define PIO2_1 0x1.921fb544p+0 /* pi/2 in three pieces of 33 bits */
#define PIO2_2 0x1.0b4611a6p-34
#define PIO2_3 0x1.3198a2ep-69
#define LOG2E 0x1.71547652b82fep+0
#define LN2_HI 0x1.62e42fefp-1
#define LN2_LO 0x1.473de6af278edp-34

/* Coefficients from the highest degree: Taylor series in r^2 (sin(r) / r to
 * r^14, cos(r) to r^16, |r| <= pi/4) and in r (exp(r) to r^13, |r| < 0.35). */
static const double sin_c[] = {
    -1.0 / 1307674368000.0, 1.0 / 6227020800.0, -1.0 / 39916800.0, 1.0 / 362880.0,
    -1.0 / 5040.0,          1.0 / 120.0,        -1.0 / 6.0,        1.0};
static const double cos_c[] = {1.0 / 20922789888000.0,
                               -1.0 / 87178291200.0,
                               1.0 / 479001600.0,
                               -1.0 / 3628800.0,
                               1.0 / 40320.0,
                               -1.0 / 720.0,
                               1.0 / 24.0,
                               -1.0 / 2.0,
                               1.0};
static const double exp_c[] = {1.0 / 6227020800.0,
                               1.0 / 479001600.0,
                               1.0 / 39916800.0,
                               1.0 / 3628800.0,
                               1.0 / 362880.0,
                               1.0 / 40320.0,
                               1.0 / 5040.0,
                               1.0 / 720.0,
                               1.0 / 120.0,
                               1.0 / 24.0,
                               1.0 / 6.0,
                               1.0 / 2.0,
                               1.0,
                               1.0};

static double horner(const double *c, int n, double x)
{
    double p = c[0];
    for (int i = 1; i < n; i++)
        p = p * x + c[i];
    return p;
}

/* sin of x + shift * pi/2 for |x| < 2^20: both polynomials are evaluated
 * and selected by the quadrant with integer masks. */
static double trig(double x, int64_t shift)
{
    double t = x * TWO_OVER_PI + ROUND_MAGIC;
    double kd = t - ROUND_MAGIC;
    int64_t q = (int32_t)d_bits(t) + shift;
    double r = x - kd * PIO2_1;
    r = r - kd * PIO2_2;
    r = r - kd * PIO2_3;
    double r2 = r * r;
    double s = r * horner(sin_c, 8, r2);
    double c = horner(cos_c, 9, r2);
    uint64_t m = -(uint64_t)(q & 1);
    uint64_t bits = (d_bits(c) & m) | (d_bits(s) & ~m);
    return d_from(bits ^ (uint64_t)(q & 2) << 62);
}

static double p_sin(double x)
{
    return trig(x, 0);
}
static double p_cos(double x)
{
    return trig(x, 1);
}

/* exp(x) for |x| < 700: x = k * ln2 + r, scaled by 2^k through the exponent
 * field. */
static double p_exp(double x)
{
    double t = x * LOG2E + ROUND_MAGIC;
    double kd = t - ROUND_MAGIC;
    int64_t k = (int32_t)d_bits(t);
    double r = x - kd * LN2_HI;
    r = r - kd * LN2_LO;
    return d_from(d_bits(horner(exp_c, 14, r)) + ((uint64_t)k << 52));
}

/* ---- 1: radix-2 iterative FFT, complex double --------------------------- */

#define FFT_N 256
#define FFT_LOG 8
static double fft_re[FFT_N], fft_im[FFT_N], fft_wr[FFT_N / 2], fft_wi[FFT_N / 2];
static uint16_t fft_rev[FFT_N];

static uint64_t k_fft(uint64_t iters)
{
    uint64_t seed = 0x243f6a8885a308d3u, h = 0;
    fpsuite_out_count = 2 * FFT_N;
    for (int k = 0; k < FFT_N / 2; k++) {
        double a = (double)k * (-0x1.921fb54442d18p+2 / FFT_N); /* -2 pi k / N */
        fft_wr[k] = p_cos(a);
        fft_wi[k] = p_sin(a);
    }
    for (int i = 0; i < FFT_N; i++) {
        unsigned r = 0;
        for (int b = 0; b < FFT_LOG; b++)
            r |= (unsigned)(i >> b & 1) << (FFT_LOG - 1 - b);
        fft_rev[i] = (uint16_t)r;
    }
    for (uint64_t it = 0; it < iters; it++) {
        for (int i = 0; i < FFT_N; i++) {
            fft_re[fft_rev[i]] = rnd_d(&seed);
            fft_im[fft_rev[i]] = rnd_d(&seed);
        }
        for (int half = 1, step = FFT_N / 2; half < FFT_N; half <<= 1, step >>= 1) {
            int len = half << 1;
            for (int i = 0; i < FFT_N; i += len)
                for (int j = 0; j < half; j++) {
                    double wr = fft_wr[j * step], wi = fft_wi[j * step];
                    int u = i + j, v = u + half;
                    double tr = fft_re[v] * wr - fft_im[v] * wi;
                    double ti = fft_re[v] * wi + fft_im[v] * wr;
                    fft_re[v] = fft_re[u] - tr;
                    fft_im[v] = fft_im[u] - ti;
                    fft_re[u] = fft_re[u] + tr;
                    fft_im[u] = fft_im[u] + ti;
                }
        }
        for (int i = 0; i < FFT_N; i++) {
            h = emit_d(h, 2 * i, fft_re[i]);
            h = emit_d(h, 2 * i + 1, fft_im[i]);
        }
    }
    return h;
}

/* ---- 2: n-body, softened gravity ---------------------------------------- */

#define NB_N 16
#define NB_STEPS 8
#define NB_DT 0x1p-7
#define NB_EPS2 0x1p-6
static double nb_p[NB_N][3], nb_v[NB_N][3], nb_a[NB_N][3], nb_m[NB_N];

static void nb_step(void)
{
    for (int i = 0; i < NB_N; i++)
        for (int d = 0; d < 3; d++)
            nb_a[i][d] = 0.0;
    for (int i = 0; i < NB_N; i++)
        for (int j = i + 1; j < NB_N; j++) {
            double dx = nb_p[j][0] - nb_p[i][0];
            double dy = nb_p[j][1] - nb_p[i][1];
            double dz = nb_p[j][2] - nb_p[i][2];
            double r2 = dx * dx + dy * dy + dz * dz + NB_EPS2;
            double inv = 1.0 / (r2 * __builtin_sqrt(r2));
            double fi = nb_m[j] * inv, fj = nb_m[i] * inv;
            nb_a[i][0] += dx * fi;
            nb_a[i][1] += dy * fi;
            nb_a[i][2] += dz * fi;
            nb_a[j][0] -= dx * fj;
            nb_a[j][1] -= dy * fj;
            nb_a[j][2] -= dz * fj;
        }
    for (int i = 0; i < NB_N; i++)
        for (int d = 0; d < 3; d++) {
            nb_v[i][d] += nb_a[i][d] * NB_DT;
            nb_p[i][d] += nb_v[i][d] * NB_DT;
        }
}

static uint64_t k_nbody(uint64_t iters)
{
    uint64_t seed = 0x13198a2e03707344u, h = 0;
    fpsuite_out_count = NB_N * 6;
    for (uint64_t it = 0; it < iters; it++) {
        for (int i = 0; i < NB_N; i++) {
            for (int d = 0; d < 3; d++) {
                nb_p[i][d] = rnd_d(&seed) * 2.0;
                nb_v[i][d] = rnd_d(&seed) * 0.125;
            }
            nb_m[i] = d_from(d_bits(rnd_d(&seed)) & ~(UINT64_C(1) << 63)); /* 1/16 <= m < 1 */
        }
        for (int s = 0; s < NB_STEPS; s++)
            nb_step();
        for (int i = 0; i < NB_N; i++)
            for (int d = 0; d < 3; d++) {
                h = emit_d(h, i * 6 + d, nb_p[i][d]);
                h = emit_d(h, i * 6 + 3 + d, nb_v[i][d]);
            }
    }
    return h;
}

/* ---- 3: exp and sin of an array ------------------------------------------ */

#define TR_N 512
static double tr_x[TR_N];

static uint64_t k_trans(uint64_t iters)
{
    uint64_t seed = 0xa4093822299f31d0u, h = 0;
    fpsuite_out_count = 2 * TR_N;
    for (uint64_t it = 0; it < iters; it++) {
        for (int i = 0; i < TR_N; i++)
            tr_x[i] = rnd_d(&seed) * 32.0;
        for (int i = 0; i < TR_N; i++) {
            h = emit_d(h, i, p_exp(tr_x[i]));
            h = emit_d(h, TR_N + i, p_sin(tr_x[i]));
        }
    }
    return h;
}

/* ---- 4: reductions: dot product and sum of squares ----------------------- */

#define RD_N 4096
static double rd_x[RD_N], rd_y[RD_N];

/* The arrays are filled once per call; each iteration replaces one element
 * of each, so no iteration repeats another's operands. */
static uint64_t k_reduce(uint64_t iters)
{
    uint64_t seed = 0x082efa98ec4e6c89u, h = 0;
    fpsuite_out_count = 10;
    for (int i = 0; i < RD_N; i++) {
        rd_x[i] = rnd_d(&seed);
        rd_y[i] = rnd_d(&seed);
    }
    for (uint64_t it = 0; it < iters; it++) {
        size_t at = (size_t)(it * 37 % RD_N);
        rd_x[at] = rnd_d(&seed);
        rd_y[at] = rnd_d(&seed);
        double d0 = 0, d1 = 0, d2 = 0, d3 = 0, s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (int i = 0; i < RD_N; i += 4) {
            d0 += rd_x[i] * rd_y[i];
            d1 += rd_x[i + 1] * rd_y[i + 1];
            d2 += rd_x[i + 2] * rd_y[i + 2];
            d3 += rd_x[i + 3] * rd_y[i + 3];
            s0 += rd_x[i] * rd_x[i];
            s1 += rd_x[i + 1] * rd_x[i + 1];
            s2 += rd_x[i + 2] * rd_x[i + 2];
            s3 += rd_x[i + 3] * rd_x[i + 3];
        }
        h = emit_d(h, 0, d0);
        h = emit_d(h, 1, d1);
        h = emit_d(h, 2, d2);
        h = emit_d(h, 3, d3);
        h = emit_d(h, 4, s0);
        h = emit_d(h, 5, s1);
        h = emit_d(h, 6, s2);
        h = emit_d(h, 7, s3);
        h = emit_d(h, 8, (d0 + d1) + (d2 + d3));
        h = emit_d(h, 9, (s0 + s1) + (s2 + s3));
    }
    return h;
}

/* ---- 5: dense matrix multiply, single ------------------------------------ */

static float fm_a[MM_N][MM_N], fm_b[MM_N][MM_N], fm_c[MM_N][MM_N];

static uint64_t k_matmul_f(uint64_t iters)
{
    uint64_t seed = 0x452821e638d01377u, h = 0;
    fpsuite_out_count = MM_N * MM_N;
    for (uint64_t it = 0; it < iters; it++) {
        for (int i = 0; i < MM_N; i++)
            for (int j = 0; j < MM_N; j++) {
                fm_a[i][j] = rnd_f(&seed);
                fm_b[i][j] = rnd_f(&seed);
            }
        for (int i = 0; i < MM_N; i++)
            for (int j = 0; j < MM_N; j++) {
                float acc = 0.0f;
                for (int k = 0; k < MM_N; k++)
                    acc += fm_a[i][k] * fm_b[k][j];
                fm_c[i][j] = acc;
            }
        for (int i = 0; i < MM_N; i++)
            for (int j = 0; j < MM_N; j++)
                h = emit_f(h, i * MM_N + j, fm_c[i][j]);
    }
    return h;
}

/* ---- 6: parallel singles (fpma, fpms, fpnma, fpmpy) --------------------- */

#define PS_H (MM_N / 2)
static v2sf ps_a[MM_N][MM_N]; /* each A element in both lanes */
static v2sf ps_b[MM_N][PS_H], ps_bias[PS_H], ps_c[MM_N][PS_H];

/* C = scale * (A * B) - bias on even rows and bias - scale * (A * B) on odd
 * rows; B, C and the bias hold two columns per element. */
static uint64_t k_parallel(uint64_t iters)
{
    uint64_t seed = 0xbe5466cf34e90c6cu, h = 0;
    const v2sf scale = {0.75f, 0.75f};
    fpsuite_out_count = MM_N * MM_N;
    for (uint64_t it = 0; it < iters; it++) {
        for (int i = 0; i < MM_N; i++)
            for (int k = 0; k < MM_N; k++) {
                float a = rnd_f(&seed);
                ps_a[i][k] = (v2sf){a, a};
            }
        for (int k = 0; k < MM_N; k++)
            for (int j = 0; j < PS_H; j++) {
                float x = rnd_f(&seed);
                ps_b[k][j] = (v2sf){x, rnd_f(&seed)};
            }
        for (int j = 0; j < PS_H; j++) {
            float x = rnd_f(&seed);
            ps_bias[j] = (v2sf){x, rnd_f(&seed)};
        }
        for (int i = 0; i < MM_N; i++)
            for (int j = 0; j < PS_H; j++) {
                v2sf acc = ps_a[i][0] * ps_b[0][j];
                for (int k = 1; k < MM_N; k++)
                    acc += ps_a[i][k] * ps_b[k][j];
                ps_c[i][j] = i & 1 ? ps_bias[j] - acc * scale : acc * scale - ps_bias[j];
            }
        for (int i = 0; i < MM_N; i++)
            for (int j = 0; j < PS_H; j++) {
                union vbits x = {.v = ps_c[i][j]};
                h = fold(h, x.u[0]);
                h = fold(h, x.u[1]);
                fpsuite_out[i * MM_N + 2 * j] = x.u[0];
                fpsuite_out[i * MM_N + 2 * j + 1] = x.u[1];
            }
    }
    return h;
}

/* ---- 7: extended precision (long double) --------------------------------- */

/* Register-precision arithmetic without a precision completer (64 bits
 * with the Linux FPSR): exp by Horner and a running sum of products. Each
 * result is stored as its significand word and its sign/exponent word. */
#define XP_N 256
static long double xp_x[XP_N];
union xbits {
    long double x;
    struct {
        uint64_t significand;
        uint16_t sign_exponent;
    } u;
};

static uint64_t emit_x(uint64_t h, size_t at, long double v)
{
    union xbits b = {.x = v};
    fpsuite_out[2 * at] = b.u.significand;
    fpsuite_out[2 * at + 1] = b.u.sign_exponent;
    return fold(fold(h, b.u.significand), b.u.sign_exponent);
}

static long double horner_x(long double x)
{
    long double p = 1.0L / 20922789888000.0L;
    for (int k = 15; k >= 1; k--)
        p = p * x + 1.0L / (long double)k;
    return p * x + 1.0L;
}

static uint64_t k_extended(uint64_t iters)
{
    uint64_t seed = 0x3f84d5b5b5470917u, h = 0;
    fpsuite_out_count = 2 * XP_N;
    for (uint64_t it = 0; it < iters; it++) {
        for (int i = 0; i < XP_N; i++)
            xp_x[i] = (long double)rnd_d(&seed) * 3.0L;
        long double acc = 0.0L;
        for (int i = 0; i < XP_N; i++) {
            long double p = horner_x(xp_x[i]);
            acc = acc + p * xp_x[i];
            h = emit_x(h, i, i & 1 ? acc : p);
        }
    }
    return h;
}

uint64_t fpsuite_main(uint64_t kernel, uint64_t iters)
{
    if (kernel == 0)
        return k_matmul(iters);
    if (kernel == 1)
        return k_fft(iters);
    if (kernel == 2)
        return k_nbody(iters);
    if (kernel == 3)
        return k_trans(iters);
    if (kernel == 4)
        return k_reduce(iters);
    if (kernel == 5)
        return k_matmul_f(iters);
    if (kernel == 6)
        return k_parallel(iters);
    if (kernel == 7)
        return k_extended(iters);
    return 0;
}
