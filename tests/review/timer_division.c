/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Check guarded division and timer conversion against integer references. */
#include <stdbool.h>
#include "tests/review/review.h"
#include "ia64/state.h"
#include "ia64/interrupt.h"
#include "host/amd64/udiv128.h"

/* Requires high < divisor. */
static uint64_t reference_divide(uint64_t high, uint64_t low, uint64_t divisor, uint64_t *remainder)
{
    uint64_t quotient = 0;
    for (unsigned i = 64; i--;) {
        bool carry = high >> 63;
        high = (high << 1) | ((low >> i) & 1);
        if (carry || high >= divisor) {
            high -= divisor;
            quotient |= UINT64_C(1) << i;
        }
    }
    *remainder = high;
    return quotient;
}
static uint64_t reference_fractional(uint64_t remainder, uint64_t frequency, uint64_t divisor)
{
    if (!frequency || remainder <= UINT64_MAX / frequency)
        return remainder * frequency / divisor;
    __uint128_t product = (__uint128_t)remainder * frequency;
    uint64_t high = product >> 64, low = (uint64_t)product, quotient = 0;
    for (unsigned i = 64; i; --i) {
        bool carry = high >> 63;
        high = (high << 1) | ((low >> (i - 1)) & 1);
        if (carry || high >= divisor) {
            high -= divisor;
            quotient |= UINT64_C(1) << (i - 1);
        }
    }
    return quotient;
}
static uint64_t reference_itc(uint64_t offset, uint64_t ticks, uint64_t hz, uint64_t itc_hz)
{
    return offset + (ticks / hz) * itc_hz + reference_fractional(ticks % hz, itc_hz, hz);
}
/* Exact-tick deadline reference; the sample has no fractional ITC phase. */
static uint64_t reference_deadline(uint64_t now, uint64_t hz, uint64_t itc, uint64_t itm,
                                   uint64_t timer_last, uint64_t itc_hz)
{
    uint64_t delta = itm - itc;
    if (itc - timer_last >= itm - timer_last)
        return now;
    __uint128_t product = (__uint128_t)delta * hz;
    uint64_t divisor = itc_hz, high = product >> 64, low = product, quotient = 0;
    if (high >= divisor)
        return UINT64_MAX;
    for (unsigned i = 64; i--;) {
        bool carry = high >> 63;
        high = (high << 1) | ((low >> i) & 1);
        if (carry || high >= divisor) {
            high -= divisor;
            quotient |= UINT64_C(1) << i;
        }
    }
    if (high) {
        if (quotient == UINT64_MAX)
            return UINT64_MAX;
        ++quotient;
    }
    return quotient > UINT64_MAX - now ? UINT64_MAX : now + quotient;
}

static uint64_t rng = UINT64_C(0x2545f4914f6cdd1d);
static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}
/* Mostly full-width values, sometimes small or near a power of two. */
static uint64_t shaped(void)
{
    uint64_t x = rnd();
    switch (rnd() & 7) {
    case 0:
        return x >> (rnd() & 63);
    case 1:
        return (UINT64_C(1) << (rnd() & 63)) + (x & 3) - 1;
    default:
        return x;
    }
}

static void divide_case(uint64_t high, uint64_t low, uint64_t divisor)
{
    uint64_t q = 0, r = 0;
    bool fits = amd64_udiv128(high, low, divisor, &q, &r);
    CHECK(fits == (high < divisor));
    if (!fits)
        return;
    uint64_t want_r, want_q = reference_divide(high, low, divisor, &want_r);
    if (q != want_q || r != want_r)
        fprintf(
            stderr,
            "review: %016llx:%016llx / %016llx = %016llx r %016llx, reference %016llx r %016llx\n",
            (unsigned long long)high, (unsigned long long)low, (unsigned long long)divisor,
            (unsigned long long)q, (unsigned long long)r, (unsigned long long)want_q,
            (unsigned long long)want_r);
    CHECK(q == want_q && r == want_r);
}

static struct vcpu *timer_vcpu(void)
{
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_hz = 400000000};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    return v;
}

static void itc_case(struct vcpu *v, uint64_t origin, uint64_t ticks, uint64_t hz, uint64_t itc_hz,
                     uint64_t offset)
{
    v->cfg.itc_icount = false;
    v->cfg.itc_hz = itc_hz;
    v->clock_origin = origin;
    v->clock_offset = offset;
    review_mock_counter = origin + ticks;
    review_mock_hz = hz;
    uint64_t want = reference_itc(offset, ticks, hz, itc_hz);
    CHECK(ia64_itc_read_unhooked(v) == want);
}

static void deadline_case(struct vcpu *v, uint64_t now, uint64_t hz, uint64_t itc_hz, uint64_t itc,
                          uint64_t itm, uint64_t timer_last)
{
    v->cfg.itc_icount = false;
    v->cfg.itc_hz = itc_hz;
    v->clock_origin = now;
    v->clock_offset = itc; /* ia64_itc_read(v) == itc */
    v->clock_hook = NULL;
    v->timer_armed = true;
    v->timer_fired = false;
    v->timer_last = timer_last;
    v->s.cr[CR_ITV] = 0xef;
    v->s.cr[CR_ITM] = itm;
    review_mock_counter = now;
    review_mock_hz = hz;
    uint64_t got = vcpu_next_timer_deadline(v),
             want = reference_deadline(now, hz, itc, itm, timer_last, itc_hz);
    if (got != want)
        fprintf(
            stderr,
            "review: deadline now=%llx hz=%llx itc_hz=%llx itc=%llx itm=%llx last=%llx: %llx, reference %llx\n",
            (unsigned long long)now, (unsigned long long)hz, (unsigned long long)itc_hz,
            (unsigned long long)itc, (unsigned long long)itm, (unsigned long long)timer_last,
            (unsigned long long)got, (unsigned long long)want);
    CHECK(got == want);
}

static void timer_phase_cases(struct vcpu *v)
{
    /* ITC == ITM raises the interrupt (SDM Vol.2 Part I s3.3.4.2).
     * Cache every host tick strictly before the first crossing. */
    static const struct {
        uint64_t hz, itc_hz, start, offset, itm, deadline;
    } cases[] = {
        {10, 3, 0, 0, 1, 4},
        {10, 3, 2, 0, 1, 4},
        {10, 3, 3, 0, 2, 7},
        {3, 10, 0, 0, 2, 1},
        {UINT64_MAX, UINT64_MAX - 1, 1, 0, 2, 3},
        {10, 3, 0, UINT64_MAX - 1, 1, 10},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        v->cfg.itc_icount = false;
        v->cfg.itc_hz = cases[i].itc_hz;
        v->clock_origin = 0;
        v->clock_offset = cases[i].offset;
        v->s.cr[CR_ITV] = 0xef;
        v->s.cr[CR_ITM] = cases[i].itm;
        review_mock_hz = cases[i].hz;
        review_mock_counter = cases[i].start;
        ia64_timer_program(v);
        uint64_t last =
            reference_itc(cases[i].offset, cases[i].start, cases[i].hz, cases[i].itc_hz);
        for (; review_mock_counter <= cases[i].deadline; ++review_mock_counter) {
            uint64_t now =
                reference_itc(cases[i].offset, review_mock_counter, cases[i].hz, cases[i].itc_hz);
            bool crossed = now - last >= cases[i].itm - last;
            CHECK(crossed == (review_mock_counter == cases[i].deadline));
            ia64_timer_poll(v);
            CHECK(v->timer_fired == crossed);
            if (!crossed) {
                CHECK(v->timer_cache.valid);
                CHECK(v->timer_cache.host + v->timer_cache.span == cases[i].deadline);
            }
            last = now;
        }
    }
}

static void review_timer_division(void)
{
    /* The guard: high >= divisor (including divisor 0) never divides. */
    divide_case(0, 0, 0);
    divide_case(5, 0, 5);
    divide_case(UINT64_MAX, UINT64_MAX, UINT64_MAX);
    static const uint64_t divisors[] = {
        1,
        2,
        3,
        7,
        1000000,
        400000000,
        UINT64_C(0xffffffff),
        UINT64_C(0x100000000),
        UINT64_C(0x100000001),
        UINT64_C(0x7fffffffffffffff),
        UINT64_C(0x8000000000000000),
        UINT64_C(0x8000000000000001),
        UINT64_MAX - 1,
        UINT64_MAX,
    };
    static const uint64_t lows[] = {0,         1, 2, UINT64_C(0x8000000000000000), UINT64_MAX - 1,
                                    UINT64_MAX};
    for (unsigned d = 0; d < sizeof(divisors) / sizeof(divisors[0]); ++d) {
        uint64_t div = divisors[d];
        const uint64_t highs[] = {0, 1, div / 2, div - 1, div, div + 1};
        for (unsigned h = 0; h < sizeof(highs) / sizeof(highs[0]); ++h)
            for (unsigned l = 0; l < sizeof(lows) / sizeof(lows[0]); ++l)
                divide_case(highs[h], lows[l], div);
    }
    for (unsigned i = 0; i < 1000000; ++i) {
        uint64_t div = shaped();
        if (!div)
            div = 1;
        uint64_t high = (i & 15) ? shaped() % div : shaped(); /* 1/16 exercise the guard */
        divide_case(high, rnd(), div);
    }

    struct vcpu *v = timer_vcpu(); /* before the mock: vcpu_create reads the real hz */
    review_mock_clock = 1;
    timer_phase_cases(v);
    /* ITC: the 128-bit fractional path needs (ticks % hz) * itc_hz >= 2^64. */
    itc_case(v, 0, 0, 1, 1, 0);
    itc_case(v, 5, UINT64_MAX - 5, UINT64_MAX, UINT64_MAX, 0);
    itc_case(v, 0, UINT64_MAX - 1, UINT64_MAX, UINT64_MAX, 7);
    itc_case(v, 0, UINT64_C(0x123456789abcdef), UINT64_C(1) << 62, UINT64_C(3) << 61, 0);
    for (unsigned i = 0; i < 200000; ++i)
        itc_case(v, rnd(), shaped(), shaped() | 1, shaped(), rnd());
    /* Deadline: ceiling, quotient overflow (high >= itc_hz), the
     * UINT64_MAX quotient with a remainder, and now + quotient saturation. */
    deadline_case(v, 1000, 1000000000, 400000000, 100, 500, 0);   /* ceil(400 * 2.5) */
    deadline_case(v, 1000, 1000000000, 400000000, 100, 500, 600); /* crossed: now */
    deadline_case(v, 1000, 400000000, 400000000, 0, 400, 0);      /* exact, no ceiling */
    deadline_case(v, 0, UINT64_MAX, 1, 0, UINT64_MAX, 0);         /* high >= itc_hz */
    deadline_case(v, 0, UINT64_MAX, 2, 0, UINT64_MAX, 0);         /* high >= itc_hz */
    /* 31 * 0x1084210842108421 = 2^65 - 1 = 2 * (2^64 - 1) + 1: the
     * quotient is UINT64_MAX with a remainder, so the ceiling saturates. */
    deadline_case(v, 0, UINT64_C(0x1084210842108421), 2, 0, 31, 0);
    deadline_case(v, 5, UINT64_C(0x1084210842108421), 3, 0, 31, 0); /* fits: now + ceil */
    deadline_case(v, UINT64_MAX - 10, 3, 2, 0, 100, 0);             /* now + quotient saturates */
    deadline_case(v, 7, 1, UINT64_MAX, 0, UINT64_MAX, 0);           /* exactly 1 */
    deadline_case(v, 7, 1, 2, 0, 1, 0);                             /* quotient 0, remainder: 1 */
    for (unsigned i = 0; i < 200000; ++i) {
        uint64_t itc = rnd(), itm = (i & 3) ? itc + shaped() : rnd();
        uint64_t last = (i & 1) ? itc - (shaped() & 0xffff) : rnd();
        deadline_case(v, rnd(), shaped() | 1, shaped() | 1, itc, itm, last);
    }
    vcpu_destroy(v);
    review_mock_clock = 0;
}
REVIEW_SUITE("timer-division", review_timer_division)
