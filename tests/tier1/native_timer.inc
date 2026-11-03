/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Vol.2 §§3.3.4.2, 5.8.3.6: match equality, unsigned wrap and ITV.m.
 * Mock the HAL counter so these native admissions depend on its exact
 * domain, independently of the host CPU's TSC. */
static struct {
    bool active;
    uint64_t now, step, reads;
    struct ia64_timer_cache cache;
    uint64_t last;
    bool armed, fired;
} native_timer;

uint64_t __real_hal_counter(void) __attribute__((weak));
/* Shared harness objects also link into suites with their own clock. */
uint64_t __wrap_hal_counter(void) __attribute__((weak));
uint64_t __wrap_hal_counter(void)
{
    if (!native_timer.active)
        return __real_hal_counter ? __real_hal_counter() : hal_counter();
    uint64_t now = native_timer.now;
    native_timer.now += native_timer.step;
    ++native_timer.reads;
    return now;
}

static uint64_t native_timer_hook(const struct vcpu *v, uint64_t value)
{
    (void)v;
    return value;
}

static void native_timer_case(struct vcpu *v, unsigned test, unsigned *hops)
{
    if (test < 37 || test > 53)
        return;
    native_timer.active = false;
    native_timer.now = 1000;
    native_timer.step = native_timer.reads = 0;
    v->timer_armed = true;
    v->timer_last = 200;
    v->s.cr[CR_ITM] = 210;
    v->s.cr[CR_ITV] = 0xef;
    v->timer_cache = (struct ia64_timer_cache){.host = 1000, .span = 10, .valid = true};
    switch (test) {
    case 37: native_timer.now = 1009; *hops = 32; break;
    case 38: native_timer.now = 1010; break;
    case 39: native_timer.now = UINT64_C(0xabcdef1234); break;
    case 40: v->timer_cache.valid = false; break;
    case 41: v->timer_cache.span = 0; break;
    case 42:
    case 43:
        v->timer_cache.host = UINT64_MAX - 3;
        native_timer.now = test == 42 ? 5 : 6;
        *hops = test == 42 ? 32 : 0;
        break;
    case 44: v->cfg.itc_icount = true; break;
    case 45: v->clock_hook = native_timer_hook; break;
    case 46:
    case 47:
        v->s.cr[CR_ITV] |= UINT64_C(1) << 16;
        native_timer.now = test == 46 ? 1009 : 1010;
        *hops = test == 46 ? 32 : 0;
        break;
    case 48: v->timer_armed = false; *hops = 32; break;
    case 49: v->timer_fired = true; *hops = 32; break;
    case 50: v->s.cr[CR_IRR0 + 3] = UINT64_C(1) << 47; break;
    case 51: vcpu_request_exit(v, VCPU_EXIT_DEBUG); break;
    case 52: vcpu_kick(v); break;
    case 53: v->timer_cache.span = 3; native_timer.step = 1; *hops = 3; break;
    }
    native_timer.cache = v->timer_cache;
    native_timer.last = v->timer_last;
    native_timer.armed = v->timer_armed;
    native_timer.fired = v->timer_fired;
}

static void native_timer_check(const struct vcpu *v, unsigned test, unsigned hops)
{
    if (test < 37 || test > 53)
        return;
    native_timer.active = false;
    CHECK(v->timer_cache.host == native_timer.cache.host &&
          v->timer_cache.span == native_timer.cache.span &&
          v->timer_cache.valid == native_timer.cache.valid);
    CHECK(v->timer_last == native_timer.last && v->timer_armed == native_timer.armed &&
          v->timer_fired == native_timer.fired);
    CHECK(v->s.cr[CR_IRR0 + 3] == (test == 50 ? UINT64_C(1) << 47 : 0));
    CHECK(v->requested_exit == (test == 51 ? VCPU_EXIT_DEBUG + 1 : 0));
    CHECK(v->irq_kick == (test == 52));
    if (test == 40 || test == 44 || test == 45 || (test >= 48 && test <= 52))
        CHECK(native_timer.reads == 0);
    else
        CHECK(native_timer.reads == (hops == 32 ? hops : hops + 1));
}
