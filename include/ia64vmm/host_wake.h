/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_HOST_WAKE_H
#define IA64VMM_HOST_WAKE_H
#include "ia64vmm/host_isa.h"
#include "ia64vmm/types.h"

/* Host CPU mailbox and idle wake, shared by the
 * bare-metal HAL and the hosted shim. The owner idles either halted (HLT:
 * only an interrupt wakes it) or monitoring its mailbox word
 * (MONITOR/MWAIT: a store to the line wakes it). */
#define HOST_IDLE_RUNNING 0u
#define HOST_IDLE_HALT 1u
#define HOST_IDLE_MONITOR 2u
/* The mailbox has a line of its own: no other variable shares the bytes a
 * monitor covers (Intel SDM Vol.3 §11.10.5). */
#define HOST_MAILBOX_LINE 64u

struct host_mailbox {
    unsigned requests; /* HAL_CPU_* bits; the owner consumes them */
    unsigned idle;     /* HOST_IDLE_*; written by the owner only */
} IA64VMM_ALIGNED(HOST_MAILBOX_LINE);

/* Any context: publish bits. True when the owner must also be interrupted:
 * a bit is new and the owner halts. A monitoring owner wakes from this
 * store; a running one sees the bits at its next poll or idle check. Pairs
 * with host_idle_enter (sequentially consistent): either the owner's check
 * sees the bits or this load sees the state it published before the check.
 * Bits already pending need nothing: the owner has not consumed them, so
 * it does not sleep (or the post that set them interrupted it). */
static inline bool host_mailbox_post(struct host_mailbox *m, unsigned bits)
{
    unsigned old = __atomic_fetch_or(&m->requests, bits, __ATOMIC_SEQ_CST);
    return (old & bits) != bits && __atomic_load_n(&m->idle, __ATOMIC_SEQ_CST) == HOST_IDLE_HALT;
}

/* Owner, interrupts masked: publish state (HALT or MONITOR), arm the
 * monitor on the mailbox word when monitoring, then check the mailbox.
 * True when nothing is pending and the owner may sleep (HLT, or MWAIT on
 * the line arm armed: a store after arming ends it). host_idle_leave
 * follows either way. */
static inline bool host_idle_enter(struct host_mailbox *m, unsigned state,
                                   void (*arm)(const unsigned *))
{
    __atomic_store_n(&m->idle, state, __ATOMIC_SEQ_CST);
    if (state == HOST_IDLE_MONITOR)
        arm(&m->requests);
    return !__atomic_load_n(&m->requests, __ATOMIC_SEQ_CST);
}
static inline void host_idle_leave(struct host_mailbox *m)
{
    __atomic_store_n(&m->idle, HOST_IDLE_RUNNING, __ATOMIC_SEQ_CST);
}

/* Whether the owner can wait with MONITOR on its mailbox word and MWAIT in
 * C1 (EAX = 0, ECX = 0; Intel SDM Vol.2 MONITOR/MWAIT, Vol.1 CPUID leaf
 * 05H): NULL when it can, otherwise why not. Unsupported leaves are not
 * queried. */
static inline const char *host_mwait_unusable(host_isa_cpuid_fn cpuid)
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    uint32_t max = a;
    if (max < 1)
        return "no CPUID leaf 01H";
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 3)))
        return "CPUID.01H:ECX.MONITOR[3] = 0";
    if (max < 5)
        return "no CPUID leaf 05H";
    cpuid(5, 0, &a, &b, &c, &d);
    unsigned smallest = a & 0xffff, largest = b & 0xffff;
    if (smallest < sizeof(unsigned))
        return "monitor line smaller than the mailbox word";
    if (largest < smallest || largest > HOST_MAILBOX_LINE)
        return "monitor line larger than the mailbox line";
    /* With extensions enumerated, EAX = 0 targets C1 sub-state 0. */
    if ((c & 1u) && !(d & 0xf0u))
        return "no MWAIT C1 sub-state";
    return NULL;
}

#endif
