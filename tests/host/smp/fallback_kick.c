/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "ia64/interrupt.h"
#include "engine/interp/interp.h"
#include "engine/tier0/internal.h"
#include "core/log_internal.h"

static unsigned checks, deliveries;
static bool request_fatal;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%u: %s\n", __LINE__, #x);                                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
void __real_ia64_deliver_fault(struct vcpu *, const Ia64Instruction *, struct ia64_result);
void __wrap_ia64_deliver_fault(struct vcpu *v, const Ia64Instruction *i, struct ia64_result r)
{
    __real_ia64_deliver_fault(v, i, r);
    if (++deliveries == 1) {
        vcpu_kick(v);
        if (request_fatal)
            vcpu_request_exit(v, VCPU_EXIT_SHUTDOWN);
    } else
        vcpu_request_exit(v, VCPU_EXIT_DEBUG); /* Bound repeated fault delivery. */
}
static void run(enum engine_kind kind, bool fatal)
{
    struct vcpu_config cfg = {.engine = kind};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    /* BE forces lower-tier fallback; neither the source nor IVT fetch is mapped. */
    v->s.psr = IA64_PSR_BE | IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
    v->s.ip = 0x1000;
    deliveries = 0;
    request_fatal = fatal;
    struct vcpu_exit out = engine_run(v, 1);
    CHECK(out.reason == (fatal ? VCPU_EXIT_SHUTDOWN : VCPU_EXIT_BUDGET));
    CHECK(deliveries == 1 && v->retired == 0 && v->s.ip == 0xc00 && v->s.slot == 0);
    CHECK(fatal || !v->irq_kick); /* The Tier0 checkpoint owns consumption. */
    vcpu_destroy(v);
}
static void direct_step(void)
{
    struct vcpu_config cfg = {.engine = ENGINE_TIER1};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    v->s.psr = IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_IC;
    v->s.ip = 0x1000;
    vcpu_kick(v);
    deliveries = 0;
    request_fatal = true;
    struct vcpu_exit out = interp_run(v, 1);
    CHECK(out.reason == VCPU_EXIT_SHUTDOWN && deliveries == 1);
    CHECK(!v->retired && v->irq_kick == 1 && !v->requested_exit);
    vcpu_destroy(v);
}
static void checked_boundary(void)
{
    struct vcpu_config cfg = {.engine = ENGINE_LOCKSTEP};
    struct vcpu *v = vcpu_create(&cfg);
    CHECK(v != NULL);
    v->s.psr = IA64_PSR_IS;
    v->irq_kick = 1;
    bool checked = false;
    struct vcpu_exit out = tier0_check_debug_boundary(v, &checked);
    CHECK(checked && out.reason == VCPU_EXIT_UNIMPLEMENTED);
    CHECK(v->irq_kick == 1 && !v->retired && !v->check_context);
    vcpu_destroy(v);
}
int main(void)
{
    log_set_level(LOG_ERROR);
    direct_step();
    run(ENGINE_TIER0, false);
    run(ENGINE_TIER1, false);
    run(ENGINE_TIER0, true);
    checked_boundary();
    tier0_destroy();
    printf("interpreter fallback kick: %u checks passed\n", checks);
    return 0;
}
