/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The platform unit harness supplies an instruction-free vCPU stub. */
#include <ia64vmm/engine.h>
bool vcpu_halt_wake_pending(const struct vcpu *v)
{
    (void)v;
    return false;
}
bool vcpu_pal_interrupt_pending(const struct vcpu *v)
{
    (void)v;
    return false;
}
