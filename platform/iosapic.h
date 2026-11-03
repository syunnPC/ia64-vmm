/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_PLATFORM_IOSAPIC_H
#define IA64VMM_PLATFORM_IOSAPIC_H
#include "ia64vmm/interrupts.h"
#include "platform/lock.h"
static inline void platform_iosapic_lock(struct ia64_iosapic *s)
{
#ifdef IA64VMM_TEST_IOSAPIC_UNLOCKED
    (void)s; /* Broken variant, built only by platform/tests/smp.py. */
#else
    platform_lock(&s->lock);
#endif
}
static inline void platform_iosapic_unlock(struct ia64_iosapic *s)
{
#ifdef IA64VMM_TEST_IOSAPIC_UNLOCKED
    (void)s;
#else
    platform_unlock(&s->lock);
#endif
}
/* Caller holds the IOSAPIC lock; an ISA pulse must keep both transitions
 * together, and INTx poll changes input and remote IRR in that same scope. */
bool platform_iosapic_set_pin_locked(struct ia64_iosapic *, unsigned, bool);
#endif
