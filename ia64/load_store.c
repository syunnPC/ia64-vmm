/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/smp.h"
#include "ia64/memory.h"
#include "engine/interp/interp.h"
#include "ia64/tlb.h"
#include "ia64/fp.h"
#include "ia64/mmu.h"
#include "ia64/debug.h"

static unsigned memory_size(Ia64Opcode opcode)
{
    switch (opcode) {
    case IA64_OP_LD1S:
    case IA64_OP_LD1SA:
        return 1;
    case IA64_OP_LD2S:
    case IA64_OP_LD2SA:
        return 2;
    case IA64_OP_LD4S:
    case IA64_OP_LD4SA:
        return 4;
    case IA64_OP_LD8S:
    case IA64_OP_LD8SA:
        return 8;
    case IA64_OP_LD1:
    case IA64_OP_LD1A:
    case IA64_OP_LD1C_CLR:
    case IA64_OP_LD1C_NC:
    case IA64_OP_ST1:
    case IA64_OP_ST1REL:
    case IA64_OP_XCHG1:
    case IA64_OP_CMPXCHG1:
        return 1;
    case IA64_OP_LD2:
    case IA64_OP_LD2A:
    case IA64_OP_LD2C_CLR:
    case IA64_OP_LD2C_NC:
    case IA64_OP_ST2:
    case IA64_OP_ST2REL:
    case IA64_OP_XCHG2:
    case IA64_OP_CMPXCHG2:
        return 2;
    case IA64_OP_LD4:
    case IA64_OP_LD4A:
    case IA64_OP_LD4C_CLR:
    case IA64_OP_LD4C_NC:
    case IA64_OP_ST4:
    case IA64_OP_ST4REL:
    case IA64_OP_XCHG4:
    case IA64_OP_CMPXCHG4:
    case IA64_OP_FETCHADD4:
        return 4;
    case IA64_OP_LD8:
    case IA64_OP_LD8A:
    case IA64_OP_LD8C_CLR:
    case IA64_OP_LD8C_NC:
    case IA64_OP_ST8:
    case IA64_OP_ST8REL:
    case IA64_OP_LD8FILL:
    case IA64_OP_ST8SPILL:
    case IA64_OP_XCHG8:
    case IA64_OP_CMPXCHG8:
    case IA64_OP_FETCHADD8:
        return 8;
    default:
        return 0;
    }
}

/* SDM Vol.3 cmpxchg zero-extends the memory value and compares it with all
 * 64 bits of AR[CCV]: a ccv wider than the operand never matches, so the
 * instruction writes nothing and returns the value read. That read is
 * still a semaphore access, .acq or .rel (Vol.1 §4.4.7, Vol.2 Table 4-15),
 * and a semaphore read is never satisfied from the local store buffer
 * (Vol.2 Part II §2.2.1.10). An AMD64 load may be forwarded from the store
 * buffer and may pass earlier stores, and a C seq_cst load emits no fence,
 * so MFENCE drains every earlier store first (write-coalesced ones too,
 * Vol.2 §4.4.5); AMD64 keeps later loads and stores after the load, which
 * covers .acq. ar.ccv is never truncated and memory is never written.
 * Out of line and unclonable so tests/review/check_codegen.py finds the
 * fence in front of the load. */
static __attribute__((noinline, noclone)) uint64_t semaphore_read_unequal(const void *ptr,
                                                                          unsigned size)
{
    __asm__ volatile("mfence" ::: "memory");
    switch (size) { /* 1, 2 or 4: every 64-bit ccv fits cmpxchg8 */
    case 1:
        return __atomic_load_n((const uint8_t *)ptr, __ATOMIC_ACQUIRE);
    case 2:
        return __atomic_load_n((const uint16_t *)ptr, __ATOMIC_ACQUIRE);
    default:
        return __atomic_load_n((const uint32_t *)ptr, __ATOMIC_ACQUIRE);
    }
}

/* GCC lowers these width-specific builtins to AMD64 locked instructions,
 * without libatomic. A locked read-modify-write is a full fence and serves
 * both the acquire and the release forms. value and compare are the
 * architectural operands; the result is the memory image of the old datum.
 * A big-endian fetchadd adds to the byte-reversed datum in a locked
 * compare-exchange loop. */
static uint64_t atomic_access(void *ptr, unsigned size, Ia64Opcode opcode, uint64_t value,
                              uint64_t compare, bool big_endian)
{
    bool exchange = opcode >= IA64_OP_XCHG1 && opcode <= IA64_OP_XCHG8;
    bool fetchadd = opcode == IA64_OP_FETCHADD4 || opcode == IA64_OP_FETCHADD8;
    uint64_t image = ia64_byte_order(value, size, big_endian);
#define ATOMIC_CASE(bytes, type)                                                                   \
    case bytes: {                                                                                  \
        type *p = ptr, old = (type)ia64_byte_order(compare, bytes, big_endian);                    \
        if (exchange)                                                                              \
            return __atomic_exchange_n(p, (type)image, __ATOMIC_SEQ_CST);                          \
        if (fetchadd && !big_endian)                                                               \
            return __atomic_fetch_add(p, (type)value, __ATOMIC_SEQ_CST);                           \
        if (fetchadd) {                                                                            \
            old = __atomic_load_n(p, __ATOMIC_RELAXED);                                            \
            while (!__atomic_compare_exchange_n(                                                   \
                p, &old,                                                                           \
                (type)ia64_byte_order(ia64_byte_order(old, bytes, true) + value, bytes, true),     \
                false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {                                      \
            }                                                                                      \
            return old;                                                                            \
        }                                                                                          \
        if (compare != (type)compare)                                                              \
            return semaphore_read_unequal(p, bytes);                                               \
        __atomic_compare_exchange_n(p, &old, (type)image, false, __ATOMIC_SEQ_CST,                 \
                                    __ATOMIC_SEQ_CST);                                             \
        return old;                                                                                \
    }
    switch (size) {
        ATOMIC_CASE(1, uint8_t)
        ATOMIC_CASE(2, uint16_t)
        ATOMIC_CASE(4, uint32_t)
        ATOMIC_CASE(8, uint64_t)
    default:
        return 0; /* only called after size qualification */
    }
#undef ATOMIC_CASE
}

/* A semaphore on a device page mapped WB reads the datum once and writes
 * it at most once (Vol.3 Table 3-1 mem_xchg, mem_xchg_add, mem_xchg_cond),
 * fenced like the locked RAM forms. The device RMW lock keeps the pair one
 * reference for other device semaphores and 16-byte references. Returns
 * the architectural old datum. */
static struct ia64_result device_semaphore(struct vcpu *v, const struct gm_region *r, gpa_t gpa,
                                           unsigned size, Ia64Opcode opcode, uint64_t value,
                                           uint64_t compare, bool big_endian, uint64_t *old)
{
    if (r->kind == GM_EMULATED && (!r->ops->read || !r->ops->write))
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "device semaphore unavailable");
    bool exchange = opcode >= IA64_OP_XCHG1 && opcode <= IA64_OP_XCHG8;
    bool fetchadd = opcode == IA64_OP_FETCHADD4 || opcode == IA64_OP_FETCHADD8;
    uint64_t image;
    ia64_device_rmw_begin();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    struct ia64_result result = ia64_physical_read_ordered(v, gpa, size, &image, true);
    if (result.kind == IA64_STEP_COMPLETE) {
        *old = ia64_byte_order(image, size, big_endian);
        if (exchange || fetchadd || *old == compare)
            result = ia64_physical_write_ordered(
                v, gpa, size, ia64_byte_order(fetchadd ? *old + value : value, size, big_endian),
                true);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ia64_device_rmw_end();
    return result;
}

struct ia64_result ia64_memory(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64MemoryOperands *op = &insn->operands.memory;
    switch (insn->opcode) {
    case IA64_OP_PROBE_R:
    case IA64_OP_PROBE_W:
    case IA64_OP_PROBE_RW:
        return ia64_tlb(v, insn);
    case IA64_OP_LD16:
    case IA64_OP_ST16:
    case IA64_OP_CMP8XCHG16:
        return ia64_memory16(v, insn);
    case IA64_OP_LFETCH:
    case IA64_OP_LFETCH_COUNT:
    case IA64_OP_LFETCH_FAULT:
    case IA64_OP_FC:
    case IA64_OP_FC_I:
        return ia64_memory_hint(v, insn);
    case IA64_OP_LDFS:
    case IA64_OP_LDFD:
    case IA64_OP_LDFE:
    case IA64_OP_LDF8:
    case IA64_OP_LDF_FILL:
    case IA64_OP_LDFPS:
    case IA64_OP_LDFPD:
    case IA64_OP_LDFP8:
    case IA64_OP_STFS:
    case IA64_OP_STFD:
    case IA64_OP_STFE:
    case IA64_OP_STF8:
    case IA64_OP_STF_SPILL:
        return ia64_fp_memory(v, insn);
    case IA64_OP_INVALA:
    case IA64_OP_INVALAT:
        return ia64_complete();
    case IA64_OP_CHK_A:
    case IA64_OP_CHK_A_CLR:
        /* A zero-entry ALAT always takes the recovery branch. */
        return (struct ia64_result){.kind = IA64_STEP_BRANCH,
                                    .info = insn->address + (uint64_t)op->immediate};
    case IA64_OP_CHK_S:
        if (insn->check_fp) {
            struct ia64_result r = ia64_fp_disabled(s, op->source, 0, 0, 0);
            if (r.kind != IA64_STEP_COMPLETE)
                return r;
        }
        if (insn->check_fp ? ia64_fr_is_nat(ia64_fr_read(s, op->source))
                           : ia64_nat_read(s, op->source))
            return (struct ia64_result){.kind = IA64_STEP_BRANCH,
                                        .info = insn->address + (uint64_t)op->immediate};
        return ia64_complete();
    case IA64_OP_MF:
    case IA64_OP_MF_A:
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return ia64_complete();
    case IA64_OP_SYNC_I:
        /* Every CPU/VMM write already invalidates the decoded page; other
         * vCPUs leave retired translated code first (SDM Vol.2 Part II
         * §2.5.2). The wait acknowledges
         * purges; a lockstep shadow replays them, as at ptc.g. */
        if (!ia64_smp_replay_purges(v))
            ia64_smp_sync_i(v);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return ia64_complete();
    default:
        break;
    }
    unsigned size = memory_size(insn->opcode);
    if (!size)
        return ia64_unimplemented(insn);
    bool store = ia64_opcode_is_store(insn->opcode);
    bool speculative = (insn->opcode >= IA64_OP_LD1S && insn->opcode <= IA64_OP_LD8S) ||
                       (insn->opcode >= IA64_OP_LD1SA && insn->opcode <= IA64_OP_LD8SA);
    bool atomic = (insn->opcode >= IA64_OP_XCHG1 && insn->opcode <= IA64_OP_FETCHADD8);
    bool spill = insn->opcode == IA64_OP_ST8SPILL;
    bool advanced = (insn->opcode >= IA64_OP_LD1A && insn->opcode <= IA64_OP_LD8A) ||
                    (insn->opcode >= IA64_OP_LD1SA && insn->opcode <= IA64_OP_LD8SA);
    bool fetchadd = insn->opcode == IA64_OP_FETCHADD4 || insn->opcode == IA64_OP_FETCHADD8;
    bool update = insn->reg_base_update || insn->imm_base_update;
    bool big_endian = s->psr & IA64_PSR_BE;
    if (update && (!op->base || (op->base >= 32 && op->base - 32u >= (s->cfm & 127))))
        return ia64_fault(IA64_FAULT_ILLEGAL, op->base);
    if (!store && update && op->base == op->destination)
        return ia64_fault(IA64_FAULT_ILLEGAL, op->base);
    struct ia64_gr_value base = ia64_gr_read_value(s, op->base);
    struct ia64_gr_value source = ia64_gr_read_value(s, op->source);
    if ((!speculative && base.nat) ||
        ((store || atomic) && !spill && !(fetchadd && op->immediate) && source.nat))
        return ia64_fault(IA64_FAULT_NAT, base.value);
    uint64_t address = base.value;
    gpa_t gpa = address & ~(UINT64_C(1) << 63);
    bool deferred = speculative && (base.nat || (s->psr & IA64_PSR_ED));
    unsigned ma = address >> 63 ? IA64_MA_UC : IA64_MA_WB;
    if (!deferred && (s->psr & IA64_PSR_DT)) {
        struct ia64_translation translated;
        unsigned rights = atomic  ? IA64_ACCESS_READ | IA64_ACCESS_WRITE
                          : store ? IA64_ACCESS_WRITE
                                  : IA64_ACCESS_READ;
        struct ia64_result r = ia64_translate(v, address, IA64_REF_DATA, rights, (s->psr >> 32) & 3,
                                              speculative, &translated);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        gpa = translated.pa;
        ma = translated.ma;
        deferred = translated.deferred;
        /* Disabled or suppressed DBRs need no aggregate-return helper. */
        if (!translated.debug_precluded && (s->psr & (IA64_PSR_DB | IA64_PSR_DD)) == IA64_PSR_DB) {
            struct ia64_result debug =
                ia64_debug_data(v, address, size, rights, (s->psr >> 32) & 3, speculative, false);
            if (debug.kind != IA64_STEP_COMPLETE)
                return debug;
            deferred |= debug.info != 0;
        }
        /* An alignment exception can coexist with a deferred translation
         * exception (Vol.2 Table 5-3); it uses the original address. */
        if (!translated.debug_precluded && (address & (size - 1)) &&
            !(speculative && ia64_defer_fault(v, IA64_FAULT_UNALIGNED)))
            return ia64_fault(IA64_FAULT_UNALIGNED, address);
    }
    /* SDM Vol.2 §5.5: unimplemented PA precludes alignment; speculative
     * address faults always defer. Virtual fault qualification is above. */
    if (!deferred && (gpa >> (IA64_IMPL_PA_MSB + 1))) {
        if (speculative)
            deferred = true;
        else
            return ia64_fault(IA64_FAULT_UNIMPL_DATA, address);
    }
    if (!deferred && (s->psr & (IA64_PSR_DT | IA64_PSR_DB | IA64_PSR_DD)) == IA64_PSR_DB) {
        unsigned rights = atomic  ? IA64_ACCESS_READ | IA64_ACCESS_WRITE
                          : store ? IA64_ACCESS_WRITE
                                  : IA64_ACCESS_READ;
        struct ia64_result debug =
            ia64_debug_data(v, address, size, rights, (s->psr >> 32) & 3, speculative, false);
        if (debug.kind != IA64_STEP_COMPLETE)
            return debug;
        deferred = debug.info != 0;
    }
    if (!deferred && (gpa & (size - 1))) {
        if (speculative && ia64_defer_fault(v, IA64_FAULT_UNALIGNED))
            deferred = true;
        else
            return ia64_fault(IA64_FAULT_UNALIGNED, address);
    }
    /* SDM Vol.2 §4.4.6 Table 4-12: physical WB uses limited speculation,
     * and physical UC uses non-speculative memory. Both abort control
     * speculation after fault qualification, without reading any device. */
    if (speculative && (!(s->psr & IA64_PSR_DT) || !ia64_ma_speculative(ma)))
        deferred = true;
    uint64_t increment = insn->reg_base_update ? source.value : (uint64_t)op->immediate;
    bool increment_nat = base.nat || (insn->reg_base_update && source.nat);
    uint64_t value = source.value, loaded = 0;
    /* SDM Vol.3 st: choose the unconditional zero representation for a
     * spilled NaT, independent of otherwise undefined GR data bits. */
    if (spill && source.nat)
        value = 0;
    bool load_nat = false;
    struct ia64_result result;
    if (atomic) {
        /* SDM Vol.2 §4.4.9: semaphores require WB (fetchadd to UCE needs an
         * exported fetchadd, which these models lack); the fault follows
         * Unaligned (Table 5-6) and precedes any reference. */
        if (ma != IA64_MA_WB)
            return ia64_fault(IA64_FAULT_UNSUPPORTED_DATA, address);
        const struct gm_region *r = ia64_physical_region(v, gpa, size);
        if (!r)
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unmapped atomic access");
        if (fetchadd && op->immediate)
            value = (uint64_t)op->immediate;
        if (r->kind == GM_EMULATED || r->kind == GM_DIRECT_MMIO) {
            result = device_semaphore(v, r, gpa, size, insn->opcode, value, s->ar[AR_CCV],
                                      big_endian, &loaded);
            if (result.kind != IA64_STEP_COMPLETE)
                return result;
        } else {
            void *ptr = r->hva + (size_t)(gpa - r->base);
            if ((uintptr_t)ptr & (size - 1))
                return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unaligned atomic backing");
            struct lockstep_access a;
            uint64_t image;
            if (lockstep_access_begin(v, &a, LOCKSTEP_RMW, gpa, size))
                image = a.read[0];
            else {
                bool acquired = code_cpu_write_begin(gpa, size);
                image = atomic_access(ptr, size, insn->opcode, value, s->ar[AR_CCV], big_endian);
                if (acquired)
                    code_cpu_write_end(true);
            }
            loaded = ia64_byte_order(image, size, big_endian);
            bool exchange = insn->opcode >= IA64_OP_XCHG1 && insn->opcode <= IA64_OP_XCHG8;
            bool wrote = exchange || fetchadd || loaded == s->ar[AR_CCV];
            uint64_t written = ia64_byte_order(fetchadd ? loaded + value : value, size, big_endian);
            lockstep_access_end(v, &a, image, 0, wrote ? written : 0, 0, wrote);
            result = ia64_complete();
        }
    } else if (store) {
        result = ia64_physical_write_ordered(v, gpa, size, ia64_byte_order(value, size, big_endian),
                                             insn->mem_release);
        if (result.kind != IA64_STEP_COMPLETE)
            return result;
        if (spill) {
            uint64_t mask = UINT64_C(1) << ((address >> 3) & 63);
            s->ar[AR_UNAT] = (s->ar[AR_UNAT] & ~mask) | (source.nat ? mask : 0);
        }
    } else {
        const struct gm_region *r = deferred ? NULL : ia64_physical_region(v, gpa, size);
        if (!deferred && !r)
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unmapped physical load");
        bool nonspeculative = !deferred && (!ia64_ma_speculative(ma) || r->kind == GM_EMULATED ||
                                            r->kind == GM_DIRECT_MMIO);
        if (deferred || ((advanced || speculative) && nonspeculative)) {
            /* SDM Vol.3 ld: advanced loads of nonspeculative memory return
             * zero and no ALAT entry, without reading the device. */
            result = ia64_complete();
            load_nat = speculative;
        } else {
            result = ia64_physical_read_ordered(v, gpa, size, &loaded, insn->mem_acquire);
            if (result.kind != IA64_STEP_COMPLETE)
                return result;
            loaded = ia64_byte_order(loaded, size, big_endian);
        }
        if (insn->opcode == IA64_OP_LD8FILL)
            load_nat = (s->ar[AR_UNAT] >> ((address >> 3) & 63)) & 1;
    }
    if (!store) {
        ia64_gr_write(s, op->destination, loaded, load_nat);
    }
    if (update)
        ia64_gr_write(s, op->base, address + increment, increment_nat);
    return result;
}
