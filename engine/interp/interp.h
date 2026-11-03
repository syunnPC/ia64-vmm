/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_INTERP_H
#define IA64VMM_INTERP_H
#include "ia64/state.h"
/* Direct/checked stepping leaves non-INTERP kicks for the caller. */
struct vcpu_exit interp_run(struct vcpu *v, uint64_t budget);
/* Tier0-owned fallback can yield before retirement without consuming a
 * kick, including a fault loop. Active checked transactions remain intact. */
struct vcpu_exit interp_run_fallback(struct vcpu *v, uint64_t budget);

/* Lockstep access log (used by the undo log of checked Tier1 lockstep,
 * engine/tier1/checked.c): one record per guest data access to RAM or firmware
 * memory (GM_RAM, GM_FIRMWARE) made inside a checked region, in the order
 * the real execution made them. Device transactions and clock samples keep
 * their own logs (engine/tier0/lockstep.c check_bus, clocks). */
enum lockstep_kind {
    LOCKSTEP_LOAD = 1,  /* memory read; read[] = the value */
    LOCKSTEP_STORE = 2, /* memory written; written[] = the value */
    LOCKSTEP_RMW = 3,   /* semaphore: read[] = old value, written[] if WROTE */
    LOCKSTEP_PURGE = 4, /* a remote ptc.g/ptc.ga entry applied while
                                   this vCPU's own ptc.g/ptc.ga waited */
};
#define LOCKSTEP_WROTE 0x01u /* RMW: memory was written */
#define LOCKSTEP_PRE                                                                               \
    0x02u                        /* STORE: read[] holds the bytes the store
                                     replaced (Tier1 undo log) */
#define LOCKSTEP_PURGE_ALL 0x04u /* PURGE: ring overrun, whole TC purged */
struct lockstep_access {         /* 56 bytes */
    uint8_t kind;                /* enum lockstep_kind */
    uint8_t width;               /* bytes: 1, 2, 4, 8, 10 or 16; PURGE: page shift */
    uint8_t slot;                /* slot of the instruction that made the access */
    uint8_t flags;               /* LOCKSTEP_* */
    uint32_t rid;                /* PURGE: region ID; otherwise 0 */
    uint64_t ip;                 /* bundle address of that instruction */
    uint64_t gpa;                /* PURGE: the purged VA */
    uint64_t read[2];            /* little-endian, low word first, zero above width */
    uint64_t written[2];         /* same encoding */
};
/* CPU interception contract. The record layout above must not change.
 * begin is called only for an architecturally qualified RAM access;
 * end records its result or checks the shadow's independently computed write. */
static __attribute__((noinline, cold, unused)) bool
lockstep_access_begin_checked(struct vcpu *v, struct lockstep_access *a, unsigned kind, gpa_t gpa,
                              unsigned width)
{
    if (!v->access_hook)
        return false;
    *a = (struct lockstep_access){
        .kind = kind, .width = width, .ip = v->s.ip, .slot = v->s.slot, .gpa = gpa};
    return v->access_hook(v, a, false);
}
static __attribute__((noinline, cold, unused)) void
lockstep_access_end_checked(struct vcpu *v, struct lockstep_access *a, uint64_t low, uint64_t high,
                            uint64_t written_low, uint64_t written_high, bool wrote)
{
    if (!v->access_hook)
        return;
    unsigned low_bits = a->width < 8 ? a->width * 8 : 64;
    unsigned high_bits = a->width > 8 ? (a->width - 8) * 8 : 0;
    uint64_t low_mask = low_bits == 64 ? UINT64_MAX : (UINT64_C(1) << low_bits) - 1;
    uint64_t high_mask = high_bits == 64 ? UINT64_MAX : (UINT64_C(1) << high_bits) - 1;
    a->read[0] = low & low_mask;
    a->read[1] = high & high_mask;
    a->written[0] = written_low & low_mask;
    a->written[1] = written_high & high_mask;
    a->flags = a->kind == LOCKSTEP_RMW && wrote ? LOCKSTEP_WROTE : 0;
    (void)v->access_hook(v, a, true);
}
/* Normal RAM helpers pay only the hook test; record construction and width
 * masking stay out of their hot instruction stream when checking is off. */
static inline bool lockstep_access_begin(struct vcpu *v, struct lockstep_access *a, unsigned kind,
                                         gpa_t gpa, unsigned width)
{
    return v->access_hook && lockstep_access_begin_checked(v, a, kind, gpa, width);
}
static inline void lockstep_access_end(struct vcpu *v, struct lockstep_access *a, uint64_t low,
                                       uint64_t high, uint64_t written_low, uint64_t written_high,
                                       bool wrote)
{
    if (v->access_hook)
        lockstep_access_end_checked(v, a, low, high, written_low, written_high, wrote);
}
#endif
