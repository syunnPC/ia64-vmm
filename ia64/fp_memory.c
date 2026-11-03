/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/fp.h"
#include "ia64/memory.h"
#include "engine/interp/interp.h"
#include "ia64/mmu.h"
#include "ia64/debug.h"

struct fp_memory_pair {
    uint64_t low, high;
} __attribute__((may_alias));

/* AMD64 CMPXCHG16B supplies the architected atomic aligned fill/spill and
 * double-pair accesses without SIMD or a libatomic dependency. Extended
 * stores preserve the six padding bytes (SDM Vol.2 section 4.5). */
static struct fp_memory_pair atomic_read16(void *address)
{
    struct fp_memory_pair old = {0};
    __asm__ volatile("lock cmpxchg16b %0"
                     : "+m"(*(struct fp_memory_pair *)address), "+a"(old.low), "+d"(old.high)
                     : "b"(UINT64_C(0)), "c"(UINT64_C(0))
                     : "cc", "memory");
    return old;
}
static void atomic_write16(void *address, struct fp_memory_pair value, bool extended)
{
    struct fp_memory_pair old = atomic_read16(address);
    bool equal;
    do {
        uint64_t high =
            extended ? (old.high & ~UINT64_C(0xffff)) | (value.high & 0xffff) : value.high;
        __asm__ volatile("lock cmpxchg16b %1"
                         : "=@ccz"(equal), "+m"(*(struct fp_memory_pair *)address), "+a"(old.low),
                           "+d"(old.high)
                         : "b"(value.low), "c"(high)
                         : "memory");
    } while (!equal);
}
static struct ia64_result ram_access(struct vcpu *v, const struct gm_region *region, gpa_t gpa,
                                     unsigned size, bool store, struct fp_memory_pair *data)
{
    void *ptr = region->hva + (size_t)(gpa - region->base);
    unsigned width = size > 8 ? 16 : size;
    if ((uintptr_t)ptr & (width - 1))
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unaligned atomic FP backing");
    if (size == 10 && !ia64_physical_region(v, gpa, 16))
        return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, gpa, "atomic extended FP backing boundary");
    struct lockstep_access a;
    bool replay = lockstep_access_begin(v, &a, store ? LOCKSTEP_STORE : LOCKSTEP_LOAD, gpa, size);
    if (replay) {
        if (!store)
            *data = (struct fp_memory_pair){a.read[0], a.read[1]};
    } else if (store) {
        bool acquired = code_cpu_write_begin(gpa, size);
        if (width == 16)
            atomic_write16(ptr, *data, size == 10);
        else if (width == 8)
            __atomic_store_n((uint64_t *)ptr, data->low, __ATOMIC_RELAXED);
        else
            __atomic_store_n((uint32_t *)ptr, (uint32_t)data->low, __ATOMIC_RELAXED);
        if (acquired)
            code_cpu_write_end(true);
    } else {
        if (width == 16)
            *data = atomic_read16(ptr);
        else if (width == 8)
            data->low = __atomic_load_n((uint64_t *)ptr, __ATOMIC_RELAXED);
        else
            data->low = __atomic_load_n((uint32_t *)ptr, __ATOMIC_RELAXED);
    }
    lockstep_access_end(v, &a, store ? 0 : data->low, store ? 0 : data->high, store ? data->low : 0,
                        store ? data->high : 0, store);
    return ia64_complete();
}

/* A device (emulated or direct MMIO) FP reference wider than 8 bytes is
 * issued as two naturally aligned 8-byte transactions, lower address first:
 * AMD64 has no general-register 16-byte device store (CMPXCHG16B is a locked
 * read-modify-write, unsuitable for device registers). The whole range was
 * preflighted in one region, so the only failure precedes both halves. */
static struct ia64_result device_access(struct vcpu *v, gpa_t gpa, unsigned total, bool store,
                                        struct fp_memory_pair *data)
{
    unsigned first = total > 8 ? 8 : total;
    struct ia64_result r = store ? ia64_physical_write(v, gpa, first, data->low)
                                 : ia64_physical_read(v, gpa, first, &data->low);
    if (r.kind != IA64_STEP_COMPLETE || total <= 8)
        return r;
    return store ? ia64_physical_write(v, gpa + 8, total - 8, data->high)
                 : ia64_physical_read(v, gpa + 8, total - 8, &data->high);
}

/* UM.be = 1 reverses the double-extended and spill memory formats as a
 * whole (SDM Vol.1 Figure 5-10); data holds the memory image in
 * little-endian order. */
static struct fp_memory_pair reverse_bytes(struct fp_memory_pair data, unsigned size)
{
    uint8_t bytes[16], reversed[16] = {0};
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = data.low >> (8 * i);
        bytes[8 + i] = data.high >> (8 * i);
    }
    for (unsigned i = 0; i < size; ++i)
        reversed[i] = bytes[size - 1 - i];
    struct fp_memory_pair out = {0, 0};
    for (unsigned i = 0; i < 8; ++i) {
        out.low |= (uint64_t)reversed[i] << (8 * i);
        out.high |= (uint64_t)reversed[8 + i] << (8 * i);
    }
    return out;
}

struct ia64_result ia64_fp_memory(struct vcpu *v, const Ia64Instruction *insn)
{
    struct ia64_state *s = &v->s;
    const IA64MemoryOperands *op = &insn->operands.memory;
    bool store = insn->opcode == IA64_OP_STFS || insn->opcode == IA64_OP_STFD ||
                 insn->opcode == IA64_OP_STFE || insn->opcode == IA64_OP_STF8 ||
                 insn->opcode == IA64_OP_STF_SPILL;
    bool paired = insn->opcode == IA64_OP_LDFPS || insn->opcode == IA64_OP_LDFPD ||
                  insn->opcode == IA64_OP_LDFP8;
    bool single = insn->opcode == IA64_OP_LDFS || insn->opcode == IA64_OP_STFS ||
                  insn->opcode == IA64_OP_LDFPS;
    bool ext = insn->opcode == IA64_OP_LDFE || insn->opcode == IA64_OP_STFE;
    bool spill = insn->opcode == IA64_OP_LDF_FILL || insn->opcode == IA64_OP_STF_SPILL;
    bool integer = insn->opcode == IA64_OP_LDF8 || insn->opcode == IA64_OP_STF8 ||
                   insn->opcode == IA64_OP_LDFP8;
    bool update = insn->reg_base_update || insn->imm_base_update;
    bool big_endian = s->psr & IA64_PSR_BE;
    unsigned reg = store ? op->source : op->destination;
    unsigned size = spill ? 16 : ext ? 10 : single ? 4 : 8;
    unsigned total = paired ? size * 2 : size;
    unsigned align = ext ? 16 : total;
    if (paired && !((ia64_fr_index(s, reg) ^ ia64_fr_index(s, op->source)) & 1))
        return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
    if ((!store && (reg < 2 || (paired && op->source < 2))) ||
        (update && (!op->base || (op->base >= 32 && op->base - 32u >= (s->cfm & 127)))))
        return ia64_fault(IA64_FAULT_ILLEGAL, insn->raw);
    struct ia64_result result = ia64_fp_disabled(s, reg, paired ? op->source : 0, 0, 0);
    if (result.kind != IA64_STEP_COMPLETE) {
        /* SDM Vol.3 ldf/ldfp/stf pass the access type to the disabled
         * register fault; Vol.2 Table 3-7 defines ISR.r/w/sp. */
        result.fault_isr = UINT64_C(1) << (store ? 33 : 34);
        if (insn->fp_load_speculative)
            result.fault_isr |= UINT64_C(1) << 36;
        return result;
    }
    struct ia64_gr_value base = ia64_gr_read_value(s, op->base);
    uint64_t address = base.value;
    bool base_nat = base.nat, speculative = insn->fp_load_speculative;
    struct ia64_fr out[2] = {{0}, {0}};
    if (store)
        out[0] = ia64_fr_read(s, reg);
    if ((!speculative && base_nat) || (store && !spill && ia64_fr_is_nat(out[0])))
        return ia64_fault(IA64_FAULT_NAT, address);
    struct ia64_gr_value increment = insn->reg_base_update
                                         ? ia64_gr_read_value(s, op->source)
                                         : (struct ia64_gr_value){.value = (uint64_t)op->immediate};
    bool defer = speculative && (base_nat || (s->psr & (UINT64_C(1) << 43)));
    gpa_t gpa = address & ~(UINT64_C(1) << 63);
    const struct gm_region *region = NULL;
    bool non_spec = false;
    if (!defer) {
        unsigned ma = address >> 63 ? IA64_MA_UC : IA64_MA_WB;
        if (s->psr & IA64_PSR_DT) {
            struct ia64_translation translated;
            result = ia64_translate(v, address, IA64_REF_DATA,
                                    store ? IA64_ACCESS_WRITE : IA64_ACCESS_READ,
                                    (s->psr >> 32) & 3, speculative, &translated);
            if (result.kind != IA64_STEP_COMPLETE)
                return result;
            if (!translated.debug_precluded) {
                struct ia64_result debug = ia64_debug_data(
                    v, address, ext ? 16 : total, store ? IA64_ACCESS_WRITE : IA64_ACCESS_READ,
                    (s->psr >> 32) & 3, speculative, false);
                if (debug.kind != IA64_STEP_COMPLETE)
                    return debug;
                translated.deferred |= debug.info != 0;
            }
            if (!translated.debug_precluded && (address & (align - 1)) &&
                !(speculative && ia64_defer_fault(v, IA64_FAULT_UNALIGNED)))
                return ia64_fault(IA64_FAULT_UNALIGNED, address);
            gpa = translated.pa;
            ma = translated.ma;
            defer = translated.deferred;
        }
        /* An unimplemented address precludes alignment and always defers
         * on speculative loads (SDM Vol.2 Tables 5-3 and 5-4). */
        if (gpa >> (IA64_IMPL_PA_MSB + 1)) {
            if (speculative)
                defer = true;
            else
                return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unimplemented FP physical address");
        }
        if (!defer && !(s->psr & IA64_PSR_DT)) {
            struct ia64_result debug = ia64_debug_data(v, address, ext ? 16 : total,
                                                       store ? IA64_ACCESS_WRITE : IA64_ACCESS_READ,
                                                       (s->psr >> 32) & 3, speculative, false);
            if (debug.kind != IA64_STEP_COMPLETE)
                return debug;
            defer = debug.info != 0;
        }
        if (!defer && (gpa & (align - 1))) {
            if (speculative && ia64_defer_fault(v, IA64_FAULT_UNALIGNED))
                defer = true;
            else
                return ia64_fault(IA64_FAULT_UNALIGNED, address);
        }
        /* Preflight the entire access before touching either destination,
         * memory, or base. Wide FP MMIO transactions are optional in the SDM. */
        if (!defer) {
            region = ia64_physical_region(v, gpa, total);
            if (!region)
                return ia64_exit(VCPU_EXIT_BAD_ACCESS, gpa, "unmapped FP access");
            non_spec = !ia64_ma_speculative(ma) || region->kind == GM_EMULATED ||
                       region->kind == GM_DIRECT_MMIO;
            if (speculative && !(s->psr & IA64_PSR_DT))
                defer = true;
            if (!store && speculative && non_spec)
                defer = true;
            /* SDM Vol.2 §4.4.9: of the FP references only ldfe/stfe may lack
             * support for non-WB pages (optional; not supported here).
             * Spill/fill and paired loads are ordinary references there. */
            if (ext && non_spec && !defer && !(insn->fp_load_advanced && !store))
                return ia64_fault(IA64_FAULT_UNSUPPORTED_DATA, address);
        }
    }
    if (defer)
        out[0] = out[1] = ia64_fp_nat();
    else if (!store && insn->fp_load_advanced && non_spec) {
        if (integer)
            out[0].exponent = out[1].exponent = IA64_FP_INTEGER_EXP;
    } else if (store) {
        uint64_t low =
            integer || spill || ext ? out[0].significand : ia64_fp_to_ieee(out[0], single);
        uint64_t high = 0;
        if (spill)
            high = out[0].exponent | ((uint64_t)out[0].sign << 17);
        if (ext)
            high = (((out[0].exponent >> 2) & 0x4000) | (out[0].exponent & 0x3fff)) |
                   ((uint64_t)out[0].sign << 15);
        struct fp_memory_pair data = {low, high};
        /* Single, double and integer data have the integer byte order (Vol.1 §5.3.1). */
        if (big_endian)
            data = spill || ext ? reverse_bytes(data, size)
                                : (struct fp_memory_pair){ia64_byte_order(low, size, true), 0};
        if (region->kind == GM_RAM || region->kind == GM_FIRMWARE)
            result = ram_access(v, region, gpa, size, true, &data);
        else
            result = device_access(v, gpa, size, true, &data);
        if (result.kind != IA64_STEP_COMPLETE)
            return result;
    } else {
        struct fp_memory_pair data = {0};
        if (region->kind == GM_RAM || region->kind == GM_FIRMWARE)
            result = ram_access(v, region, gpa, total, false, &data);
        else
            result = device_access(v, gpa, total, false, &data);
        if (result.kind != IA64_STEP_COMPLETE)
            return result;
        if (big_endian && (spill || ext))
            data = reverse_bytes(data, size);
        for (unsigned i = 0; i < (paired ? 2u : 1u); ++i) {
            uint64_t low = paired ? (single ? (data.low >> (32 * i)) & UINT32_MAX
                                     : i    ? data.high
                                            : data.low)
                                  : data.low;
            uint64_t high = data.high;
            /* Each element of a pair has its own byte order (Vol.3 Table 3-1 mem_read_pair). */
            if (!spill && !ext)
                low = ia64_byte_order(low, size, big_endian);
            if (spill)
                out[i] = (struct ia64_fr){low, high & 0x1ffff, (high >> 17) & 1};
            else if (ext) {
                unsigned exp = high & 0x7fff;
                out[i] = (struct ia64_fr){low,
                                          exp == 0x7fff ? IA64_FP_SPECIAL_EXP
                                          : exp         ? exp + 0xc000
                                                        : 0,
                                          (high >> 15) & 1};
            } else if (integer)
                out[i] = (struct ia64_fr){low, IA64_FP_INTEGER_EXP, false};
            else
                out[i] = ia64_fp_from_ieee(low, single);
        }
    }
    if (!store) {
        ia64_fr_write(s, reg, out[0]);
        if (paired)
            ia64_fr_write(s, op->source, out[1]);
    }
    if (update)
        ia64_gr_write(s, op->base, address + increment.value, base_nat || increment.nat);
    return ia64_complete();
}
