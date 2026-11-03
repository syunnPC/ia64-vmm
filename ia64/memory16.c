/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/memory.h"
#include "engine/interp/interp.h"
#include "ia64/mmu.h"
#include "ia64/debug.h"

struct pair {
    uint64_t lo, hi;
} __attribute__((may_alias));

static bool host_has_cmpxchg16b(void)
{
    static unsigned cached;
    unsigned value = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (value)
        return value == 2;
    unsigned a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    bool available = (c & (1u << 13)) != 0;
    __atomic_store_n(&cached, available ? 2 : 1, __ATOMIC_RELAXED);
    return available;
}

/* SDM Vol.3 ld16/st16/cmp8xchg16: one indivisible 16-byte reference,
 * including when the comparison selects the upper 8-byte word. AMD64's
 * integer-only cmpxchg16b avoids libatomic and host SIMD in bare metal. */
static struct pair compare_exchange(struct pair *p, struct pair expected, struct pair desired)
{
    __asm__ volatile("lock cmpxchg16b %0"
                     : "+m"(*p), "+a"(expected.lo), "+d"(expected.hi)
                     : "b"(desired.lo), "c"(desired.hi)
                     : "cc", "memory");
    return expected;
}
/* One indivisible 16-byte read, for ld16 and for the VMM's own reads of
 * guest code (engine/cache/cache.c, the snapshots of engine/cache/
 * validity.c). Contract: ptr is 16-byte aligned ordinary RAM in a writable
 * host mapping -- the VMM's read-write backing alias (gm_region.hva), never
 * a read-only alias such as a hostwin window protected for code or a
 * future read-only guest shadow. LOCK CMPXCHG16B always performs a write
 * cycle, storing the old value back when the comparison fails (Intel SDM
 * Vol.2 CMPXCHG8B/CMPXCHG16B), so a read-only mapping faults and a
 * write-tracking one marks the page dirty, although the value does not
 * change. Guest stores never come through here. */
bool ia64_atomic_read16(void *ptr, uint64_t *low, uint64_t *high)
{
    if (!host_has_cmpxchg16b())
        return false;
    struct pair old = compare_exchange(ptr, (struct pair){0}, (struct pair){0});
    *low = old.lo;
    *high = old.hi;
    return true;
}

/* 16 bytes on a device page mapped WB: two 8-byte transactions, lower
 * address first; cmp8xchg16 reads only the compared word and writes both
 * when it matches (Vol.3 Table 3-1). The device RMW lock keeps them one
 * reference for other device semaphores and 16-byte references. desired
 * holds the memory images to store; *read receives the images read (for
 * cmp8xchg16 the compared word in read->lo). */
static struct ia64_result device16(struct vcpu *v, const struct gm_region *r, gpa_t gpa,
                                   const Ia64Instruction *insn, struct pair desired,
                                   struct pair *read)
{
    bool load = insn->opcode == IA64_OP_LD16, compare = insn->opcode == IA64_OP_CMP8XCHG16;
    gpa_t base = gpa & ~UINT64_C(15);
    if (r->kind == GM_EMULATED &&
        ((!load && !r->ops->write) || ((load || compare) && !r->ops->read)))
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, base, "16-byte device access unavailable");
    ia64_device_rmw_begin();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    struct ia64_result result;
    if (load) {
        result = ia64_physical_read_ordered(v, base, 8, &read->lo, insn->mem_acquire);
        if (result.kind == IA64_STEP_COMPLETE)
            result = ia64_physical_read_ordered(v, base + 8, 8, &read->hi, insn->mem_acquire);
    } else {
        result = compare ? ia64_physical_read_ordered(v, gpa, 8, &read->lo, true) : ia64_complete();
        if (result.kind == IA64_STEP_COMPLETE &&
            (!compare || ia64_byte_order(read->lo, 8, v->s.psr & IA64_PSR_BE) == v->s.ar[AR_CCV])) {
            result = ia64_physical_write_ordered(v, base, 8, desired.lo, true);
            if (result.kind == IA64_STEP_COMPLETE)
                result = ia64_physical_write_ordered(v, base + 8, 8, desired.hi, true);
        }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ia64_device_rmw_end();
    return result;
}

struct ia64_result ia64_memory16(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64MemoryOperands *op = &insn->operands.memory;
    bool load = insn->opcode == IA64_OP_LD16;
    bool compare = insn->opcode == IA64_OP_CMP8XCHG16;
    struct ia64_gr_value base_value = ia64_gr_read_value(s, op->base);
    uint64_t address = base_value.value;
    if (base_value.nat || (!load && ia64_nat_read(s, op->source)))
        return ia64_fault(IA64_FAULT_NAT, address);
    struct ia64_translation translated;
    unsigned rights = load      ? IA64_ACCESS_READ
                      : compare ? IA64_ACCESS_READ | IA64_ACCESS_WRITE
                                : IA64_ACCESS_WRITE;
    struct ia64_result result =
        ia64_translate(v, address, IA64_REF_DATA, rights, (s->psr >> 32) & 3, false, &translated);
    if (result.kind != IA64_STEP_COMPLETE)
        return result;
    result = ia64_debug_data(v, compare ? address & ~UINT64_C(15) : address, 16, rights,
                             (s->psr >> 32) & 3, false, false);
    if (result.kind != IA64_STEP_COMPLETE)
        return result;
    gpa_t gpa = translated.pa;
    if (gpa & (compare ? 7 : 15))
        return ia64_fault(IA64_FAULT_UNALIGNED, address);
    gpa_t base = gpa & ~UINT64_C(15);
    /* SDM Vol.2 §4.4.9: non-WB 16-byte references are unsupported here
     * (optional for ld16/st16, required for cmp8xchg16), before any reference. */
    if (translated.ma != IA64_MA_WB)
        return ia64_fault(IA64_FAULT_UNSUPPORTED_DATA, address);
    const struct gm_region *r = ia64_physical_region(v, base, 16);
    if (!r)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, base, "unmapped 16-byte access");
    bool big_endian = s->psr & IA64_PSR_BE;
    /* Vol.3 Table 3-1: each 8-byte half has the data byte order, GR at the
     * lower address (mem_read_pair, mem_write16, mem_xchg16_cond). */
    struct pair desired = {ia64_byte_order(ia64_gr_read(s, op->source), 8, big_endian),
                           ia64_byte_order(s->ar[AR_CSD], 8, big_endian)};
    if (r->kind == GM_EMULATED || r->kind == GM_DIRECT_MMIO) {
        struct pair read = {0, 0};
        result = device16(v, r, gpa, insn, desired, &read);
        if (result.kind != IA64_STEP_COMPLETE)
            return result;
        if (load || compare)
            ia64_gr_write(s, op->destination, ia64_byte_order(read.lo, 8, big_endian), false);
        if (load)
            s->ar[AR_CSD] = ia64_byte_order(read.hi, 8, big_endian);
        return ia64_complete();
    }
    struct pair *p = (struct pair *)(r->hva + (size_t)(base - r->base));
    if ((uintptr_t)p & 15)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, base, "unaligned 16-byte backing");
    if (!host_has_cmpxchg16b())
        return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, insn->raw, "host cmpxchg16b unavailable");
    struct lockstep_access a;
    bool replay = lockstep_access_begin(v, &a,
                                        load      ? LOCKSTEP_LOAD
                                        : compare ? LOCKSTEP_RMW
                                                  : LOCKSTEP_STORE,
                                        gpa, 16);
    struct pair old = replay ? (struct pair){a.read[0], a.read[1]}
                             : compare_exchange(p, (struct pair){0}, (struct pair){0});
    uint64_t selected = (gpa & 8) ? old.hi : old.lo;
    if (!load && !replay) {
        /* Notify before the first possible write, so a concurrent decoded
         * reader cannot use stale instructions after the store commits. */
        bool acquired = code_cpu_write_begin(base, 16);
        for (;;) {
            selected = (gpa & 8) ? old.hi : old.lo;
            if (compare && ia64_byte_order(selected, 8, big_endian) != s->ar[AR_CCV])
                break;
            struct pair seen = compare_exchange(p, old, desired);
            if (seen.lo == old.lo && seen.hi == old.hi)
                break;
            old = seen;
        }
        if (acquired)
            code_cpu_write_end(true);
    }
    selected = ia64_byte_order(selected, 8, big_endian);
    bool wrote = !load && (!compare || selected == s->ar[AR_CCV]);
    lockstep_access_end(v, &a, load || compare ? old.lo : 0, load || compare ? old.hi : 0,
                        wrote ? desired.lo : 0, wrote ? desired.hi : 0, wrote);
    if (load) {
        ia64_gr_write(s, op->destination, ia64_byte_order(old.lo, 8, big_endian), false);
        s->ar[AR_CSD] = ia64_byte_order(old.hi, 8, big_endian);
    } else if (compare) {
        ia64_gr_write(s, op->destination, selected, false);
    }
    return ia64_complete();
}
