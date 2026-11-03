/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Vol.2 §§3.3.4.2, 5.8.3.6: ITC/ITM equality and ITV masking.
 * A native admission may skip a poll only if that poll changes no state. */
#include <string.h>
#include "tests/review/review.h"
#include "ia64/interrupt.h"

static uint64_t noop_hook(const struct vcpu *v, uint64_t value)
{
    (void)v;
    return value;
}

static void noop_check(struct vcpu *v, bool expected)
{
    static struct vcpu before, polled;
    before = *v;
    CHECK(ia64_timer_poll_noop(v) == expected);
    CHECK(!memcmp(v, &before, sizeof(*v)));
    if (expected) {
        polled = *v;
        ia64_timer_poll(&polled);
        CHECK(!memcmp(&polled, &before, sizeof(polled)));
    }
}

static void timer_noop_tests(void)
{
    static struct vcpu v, polled;
    review_mock_clock = 1;
    uint64_t seed = 1;
    for (unsigned test = 0; test < 512; ++test) {
        memset(&v, 0, sizeof(v));
        seed = seed * UINT64_C(6364136223846793005) + 1;
        v.cfg.itc_hz = seed % 101 + 1;
        review_mock_hz = (seed >> 8) % 103 + 1;
        v.clock_origin = test & 1 ? UINT64_MAX - 100 : 0;
        v.clock_offset = test & 2 ? UINT64_MAX - 20 : 0;
        review_mock_counter = v.clock_origin + test % 31;
        v.s.cr[CR_ITV] = 0xef | (test & 4 ? UINT64_C(1) << 16 : 0);
        /* Independent integer conversion, including both unsigned wraps. */
        uint64_t itc =
            v.clock_offset + (uint64_t)((__uint128_t)(review_mock_counter - v.clock_origin) *
                                        v.cfg.itc_hz / review_mock_hz);
        v.s.cr[CR_ITM] = itc + 100;
        ia64_timer_program(&v);
        CHECK(v.timer_last == itc && !v.timer_cache.valid);
        noop_check(&v, false);
        ia64_timer_poll(&v);
        CHECK(v.timer_cache.valid && !v.timer_fired && v.timer_cache.span > 0);
        const uint64_t offsets[] = {0, v.timer_cache.span - 1, v.timer_cache.span,
                                    v.timer_cache.span + 1};
        for (unsigned k = 0; k < sizeof(offsets) / sizeof(offsets[0]); ++k) {
            review_mock_counter = v.timer_cache.host + offsets[k];
            noop_check(&v, k < 2);
            polled = v;
            ia64_timer_poll(&polled);
            uint64_t now =
                v.clock_offset + (uint64_t)((__uint128_t)(review_mock_counter - v.clock_origin) *
                                            v.cfg.itc_hz / review_mock_hz);
            bool match = now - v.timer_last >= v.s.cr[CR_ITM] - v.timer_last;
            CHECK(polled.timer_fired == match);
            CHECK(polled.s.cr[CR_IRR0 + 3] ==
                  (match && !(v.s.cr[CR_ITV] & (UINT64_C(1) << 16)) ? UINT64_C(1) << 47 : 0));
        }
    }

    memset(&v, 0, sizeof(v));
    review_mock_hz = v.cfg.itc_hz = 1;
    review_mock_counter = 1000;
    v.s.cr[CR_ITM] = 1100;
    v.s.cr[CR_ITV] = 0xef;
    ia64_timer_program(&v);
    ia64_timer_poll(&v);
    CHECK(v.timer_cache.valid);
    v.cfg.itc_icount = true;
    noop_check(&v, false);
    v.cfg.itc_icount = false;
    v.clock_hook = noop_hook;
    noop_check(&v, false);
    v.clock_hook = NULL;
    v.timer_cache.span = 0;
    noop_check(&v, false);
    v.timer_cache.span = 100;
    v.timer_cache.valid = false;
    noop_check(&v, false);
    v.timer_cache.valid = true;
    v.timer_armed = false;
    noop_check(&v, true);
    v.timer_armed = v.timer_fired = true;
    noop_check(&v, true);
    v.timer_fired = false;
    v.s.cr[CR_IRR0 + 2] = 4;
    v.irq_kick = 1;
    vcpu_request_exit(&v, VCPU_EXIT_DEBUG);
    noop_check(&v, true); /* The proof neither consumes nor delivers requests. */
    v.s.cr[CR_ITM] = 1001;
    ia64_timer_program(&v);
    noop_check(&v, false);
    ia64_timer_poll(&v);
    noop_check(&v, true);
    ia64_itc_write(&v, 1000);
    noop_check(&v, false);
    ia64_timer_poll(&v);
    noop_check(&v, true);
    v.s.cr[CR_ITM] = 999; /* A past ITM waits for unsigned wrap. */
    ia64_timer_program(&v);
    ia64_timer_poll(&v);
    CHECK(!v.timer_fired && !v.timer_cache.valid);
    noop_check(&v, false);
    /* An ITV update before match keeps the time proof, but equality must
     * resolve the old vector/mask before the control-register write. */
    for (unsigned test = 0; test < 8; ++test) {
        memset(&v, 0, sizeof(v));
        review_mock_hz = v.cfg.itc_hz = 1;
        review_mock_counter = 1000;
        v.s.cr[CR_ITM] = 1005;
        v.s.cr[CR_ITV] = 0xef | (test & 1 ? UINT64_C(1) << 16 : 0);
        ia64_timer_program(&v);
        ia64_timer_poll(&v);
        review_mock_counter = test & 4 ? 1005 : 1003;
        Ia64Instruction write = {.opcode = IA64_OP_MOV_GRCR};
        write.operands.system.source = CR_ITV;
        write.operands.system.destination = 2;
        v.s.gr[2] = 0xee | (test & 2 ? UINT64_C(1) << 16 : 0);
        CHECK(ia64_control(&v, &write).kind == IA64_STEP_COMPLETE);
        noop_check(&v, true);
        if (!(test & 4)) {
            review_mock_counter = 1005;
            noop_check(&v, false);
            ia64_timer_poll(&v);
        }
        CHECK(v.timer_fired);
        uint64_t expected =
            test & 4 ? (test & 1 ? 0 : UINT64_C(1) << 47) : (test & 2 ? 0 : UINT64_C(1) << 46);
        CHECK(v.s.cr[CR_IRR0 + 3] == expected);
    }
    review_mock_clock = 0;
}

REVIEW_SUITE("timer-noop", timer_noop_tests)
