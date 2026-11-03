/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The loop top's host-clock timer poll (engine/tier0/tier0.c run): an
 * ITC/ITM match (SDM Vol.2 §3.3.4.2) is observed within TIER0_TIMER_SKIP + 1
 * boundaries, at the first boundary of every engine_run call, and at the
 * boundary after an ITM or ITC write.
 * tier0-test links with --wrap=hal_counter and --wrap=ia64_timer_poll
 * (mk/tier0.mk): the host clock advances at a selected poll or after
 * the first region retires, independently of polling. */
static bool timer_mocked;
static uint64_t timer_now, timer_after, timer_polls, timer_due_poll;
static const struct vcpu *timer_retiring;
uint64_t __real_hal_counter(void);
uint64_t __wrap_hal_counter(void);
uint64_t __wrap_hal_counter(void)
{
    if (!timer_mocked) return __real_hal_counter();
    if (timer_retiring && timer_retiring->retired >= 3) timer_now = timer_after;
    return timer_now;
}
void __real_ia64_timer_poll(struct vcpu *v);
void __wrap_ia64_timer_poll(struct vcpu *v);
void __wrap_ia64_timer_poll(struct vcpu *v)
{
    if (timer_mocked && ++timer_polls >= timer_due_poll) timer_now = timer_after;
    __real_ia64_timer_poll(v);
}
/* M32 mov cr[cr3] = r2 (SDM Vol.3: opcode 1, x3 0, x6 0x2c). */
static uint64_t move_to_cr(unsigned cr, unsigned source)
{ return (UINT64_C(1) << 37) | (UINT64_C(0x2c) << 27) | ((uint64_t)cr << 20) | ((uint64_t)source << 13); }
/* M29 mov ar3 = r2 (SDM Vol.3: opcode 1, x3 0, x6 0x2a). */
static uint64_t move_to_ar(unsigned ar, unsigned source)
{ return (UINT64_C(1) << 37) | (UINT64_C(0x2a) << 27) | ((uint64_t)ar << 20) | ((uint64_t)source << 13); }
/* Host clock, ITM 1000 ITC ticks ahead; the later counter value lies one
 * second beyond it. The ITV vector is unmasked, PSR.i is 0: pended only. */
static void timer_start(struct vcpu *v, uint64_t due_poll)
{
    code_cache_flush(); restart(v, ENGINE_TIER0, BASE);
    v->cfg.itc_icount = false;
    v->s.cr[CR_ITV] = 150;
    timer_mocked = true; timer_polls = 0; timer_due_poll = due_poll;
    timer_retiring = NULL;
    timer_now = UINT64_C(1) << 40; timer_after = timer_now + hal_counter_hz();
    v->s.cr[CR_ITM] = ia64_itc_read(v) + 1000;
    ia64_timer_program(v);
}
static bool timer_pending(const struct vcpu *v)
{ return (v->s.cr[CR_IRR0 + 150 / 64] >> (150 % 64)) & 1; }
/* SDM Vol.2 §§3.3.4.2, 5.8.3.5–6: equality raises a request with the
 * vector/mask at that occurrence; later timer writes do not withdraw it. */
static void test_timer_writes(struct vcpu *v)
{
    bool icount = v->cfg.itc_icount;
    const struct { unsigned cr; bool itc, masked; uint64_t value; unsigned vector; } cases[] = {
        {CR_ITM, false, false, 0, 150},
        {0, true, false, 0, 150},
        {CR_ITV, false, false, 151, 150},
        {CR_ITV, false, false, (1u << 16) | 150, 150},
        {CR_ITV, false, true, 150, 15}
    };
    for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(cases); ++n) {
        bundle(BASE, 0x11, nop, nop, validator_branch(BASE, BASE + 16));
        bundle(BASE + 16, 0x11, cases[n].itc ? move_to_ar(AR_ITC, 10) :
               move_to_cr(cases[n].cr, 10), nop, validator_branch(BASE + 16, BASE + 32));
        /* M24 srlz.d (SDM Vol.3): opcode 0, x3 0, x6 0x30. */
        bundle(BASE + 32, 0x11, UINT64_C(0x30) << 27, nop,
               validator_branch(BASE + 32, BASE + 32));
        timer_start(v, UINT64_MAX);
        if (cases[n].masked) v->s.cr[CR_ITV] |= 1u << 16;
        v->s.gr[10] = cases[n].itc ? v->s.cr[CR_ITM] - 10 : cases[n].cr == CR_ITM ?
            ia64_itc_read_host(v, timer_after, hal_counter_hz(), NULL) + 1000 : cases[n].value;
        /* Time crosses the old match after the first region, independently
         * of whether the next boundary polls before executing the write. */
        timer_retiring = v;
        CHECK(engine_run(v, 7).reason == VCPU_EXIT_BUDGET && v->retired == 7);
        CHECK(ia64_ivr_read(v) == cases[n].vector);
        ia64_eoi(v);
        CHECK(ia64_ivr_read(v) == 15);
        timer_retiring = NULL;
    }
    timer_mocked = false;
    v->cfg.itc_icount = icount;
    restart(v, ENGINE_TIER0, BASE);
}
static void test_timer_poll(struct vcpu *v)
{
    /* One region per boundary: nop, add r8 = r8, r9 and a branch to itself. */
    bundle(BASE, 0x11, nop, add(8,8,9), validator_branch(BASE, BASE));
    /* The first boundary polls; the next 15 do not read the counter. */
    timer_start(v, 2);
    CHECK(engine_run(v, 3 * 16).reason == VCPU_EXIT_BUDGET);
    CHECK(timer_polls == 1 && !v->timer_fired && !timer_pending(v));
    /* The 17th boundary polls again and observes the match. */
    timer_start(v, 2);
    CHECK(engine_run(v, 3 * 17).reason == VCPU_EXIT_BUDGET);
    CHECK(timer_polls == 2 && v->timer_fired && timer_pending(v));
    /* The skip window ends with its engine_run call: the next call polls
     * at its first boundary. */
    timer_start(v, 2);
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && timer_polls == 1 && !v->timer_fired);
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET);
    CHECK(timer_polls == 2 && v->timer_fired && timer_pending(v));
    /* mov cr.itm = r10 ends its region; the next boundary polls although
     * the first boundary's skip window is still open. */
    bundle(BASE, 0x11, move_to_cr(CR_ITM, 10), nop, validator_branch(BASE, BASE));
    timer_start(v, 3);
    v->s.gr[10] = ia64_itc_read(v) + 10;
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->retired == 3);
    CHECK(v->s.cr[CR_ITM] == v->s.gr[10] && v->timer_fired && timer_pending(v));
    /* mov ar.itc = r10, ten ticks before ITM, ends its region; the next
     * boundary polls (at the later counter value) and observes the match. */
    bundle(BASE, 0x11, move_to_ar(AR_ITC, 10), nop, validator_branch(BASE, BASE));
    timer_start(v, 3);
    v->s.gr[10] = v->s.cr[CR_ITM] - 10;
    CHECK(engine_run(v, 3).reason == VCPU_EXIT_BUDGET && v->retired == 3);
    CHECK(timer_polls == 3 && v->timer_fired && timer_pending(v));
    /* A deterministic ITC polls at every boundary. */
    timer_mocked = false;
    bundle(BASE, 0x11, nop, add(8,8,9), validator_branch(BASE, BASE));
    code_cache_flush(); restart(v, ENGINE_TIER0, BASE);
    v->cfg.itc_icount = true; v->s.cr[CR_ITV] = 150;
    v->s.cr[CR_ITM] = ia64_itc_read(v) + 4; ia64_timer_program(v);
    CHECK(engine_run(v, 6).reason == VCPU_EXIT_BUDGET && v->timer_fired);
    restart(v, ENGINE_TIER0, BASE);
}
