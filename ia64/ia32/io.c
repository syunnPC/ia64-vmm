/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/ia32/run.h"
#include "ia64/ia32/access.h"
#include "ia64/ia32/segment.h"
#include "ia64/memory.h"
#include "ia64/mmu.h"

struct port_transaction {
    gpa_t pa;
    unsigned width, shift;
};
static struct ia64_result stop(uint64_t address, const char *detail)
{
    return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, address, detail);
}
static struct ia64_result failure(struct ia32_native_state *n, bool before_final,
                                  struct ia64_result r, const struct ia32_interruption *record)
{
    if (before_final && (r.kind == IA64_STEP_FAULT || record->valid))
        n->eflag |= 0x10000;
    return r;
}
static unsigned debug(const struct vcpu *v, const struct ia32_native_state *n, uint64_t address,
                      unsigned width, bool write)
{
    if (!(v->s.psr & IA64_PSR_DB) || (v->s.psr & IA64_PSR_DD))
        return 0;
    unsigned matches = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t control = v->s.dbr[2 * i + 1];
        if (!(control & (UINT64_C(1) << (56 + n->cpl))) ||
            !(control & (UINT64_C(1) << (write ? 62 : 63))))
            continue;
        uint64_t mask = UINT64_C(0xff00000000000000) | (control & UINT64_C(0x00ffffffffffffff));
        for (unsigned b = 0; b < width; ++b)
            if (!(((address + b) ^ v->s.dbr[2 * i]) & mask))
                matches |= 1u << i;
    }
    return matches;
}
static struct ia64_result system_read(struct vcpu *v, uint32_t address, unsigned width,
                                      unsigned level, uint64_t *value)
{
    gpa_t physical[2];
    for (unsigned i = 0; i < width; ++i) {
        struct ia64_translation t;
        struct ia64_result r = ia64_translate(v, (uint32_t)(address + i), IA64_REF_DATA,
                                              IA64_ACCESS_READ, level, false, &t);
        if (r.kind != IA64_STEP_COMPLETE) {
            r.fault_isr |= UINT64_C(1) << 40;
            return r;
        }
        const struct gm_region *region = ia64_physical_region(v, t.pa, 1);
        if (!region || (region->kind != GM_RAM && region->kind != GM_FIRMWARE))
            return stop(t.pa, "IA-32 I/O bitmap requires ordinary backing");
        physical[i] = t.pa;
    }
    *value = 0;
    for (unsigned i = 0; i < width; ++i) {
        uint64_t b;
        struct ia64_result r = ia64_physical_read(v, physical[i], 1, &b);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        *value |= b << (8 * i);
    }
    return ia64_complete();
}
static struct ia64_result permission(struct vcpu *v, const struct ia32_native_state *n,
                                     unsigned port, unsigned width,
                                     struct ia32_interruption *record)
{
    /* The normative algorithm in Vol.2 10.7.3 takes precedence over the
     * conflicting CFLG.io description in Table 10-4. */
    if (n->cpl <= ((n->eflag >> 12) & 3))
        return ia64_complete();
    if (!(n->cflg & 0x40))
        return ia32_exception(n, 13, 0, record);
    const struct ia32_segment *tss = &n->tr;
    if (tss->base > UINT32_MAX)
        return stop(tss->base, "IA-32 I/O bitmap TSS base");
    if (!ia32_segment_valid(n, IA32_SEG_TSS, IA32_SEGMENT_READ) || tss->limit < 0x67)
        return ia32_exception(n, 13, 0, record);
    uint64_t bitmap;
    struct ia64_result r = system_read(v, (uint32_t)tss->base + 0x66, 2, n->cpl, &bitmap);
    if (r.kind != IA64_STEP_COMPLETE)
        return r;
    unsigned bytes = ((port & 7) + width + 7) / 8;
    uint32_t offset = (uint32_t)bitmap + (port >> 3);
    if ((uint64_t)offset + bytes - 1 > tss->limit)
        return ia32_exception(n, 13, 0, record);
    r = system_read(v, (uint32_t)tss->base + offset, bytes, n->cpl, &bitmap);
    if (r.kind != IA64_STEP_COMPLETE)
        return r;
    if ((bitmap >> (port & 7)) & ((1u << width) - 1))
        return ia32_exception(n, 13, 0, record);
    return ia64_complete();
}
static uint64_t port_address(uint64_t base, unsigned port)
{
    /* Addition also addresses the three excess bytes above port 0xffff,
     * which the operating system may map back to the start of port space. */
    return base + ((uint64_t)(port >> 2) << 12) + (port & 0xfff);
}
static struct ia64_result ports(struct vcpu *v, const struct ia32_native_state *n, unsigned port,
                                unsigned width, bool write, struct port_transaction transactions[4],
                                unsigned *count, unsigned *breakpoints,
                                struct ia32_interruption *record)
{
    if (!(v->s.psr & IA64_PSR_DT))
        return ia32_exception(n, 13, 0, record);
    if (n->kr0 & 0x3ffffff)
        return stop(n->kr0, "IA-32 I/O base is not aligned to 64 MiB");
    *count = 0;
    for (unsigned i = 0; i < width;) {
        unsigned p = port + i, chunk = 4 - (p & 3);
        if (chunk > width - i)
            chunk = width - i;
        if (chunk == 3)
            chunk = 2;
        uint64_t address = port_address(n->kr0, p);
        if (address < n->kr0)
            return ia32_exception(n, 13, 0, record);
        gpa_t first = 0;
        for (unsigned b = 0; b < chunk; ++b) {
            struct ia64_translation t;
            struct ia64_result r =
                ia64_translate(v, address + b, IA64_REF_DATA,
                               write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ, n->cpl, false, &t);
            if (r.kind != IA64_STEP_COMPLETE)
                return r.kind == IA64_STEP_FAULT && r.fault == IA64_FAULT_UNIMPL_DATA
                           ? ia32_exception(n, 13, 0, record)
                           : r;
            if (t.ma != IA64_MA_UC)
                return stop(address + b, "IA-32 I/O port mapping requires UC");
            if (!b)
                first = t.pa;
            else if (t.pa != first + b)
                return stop(address + b, "IA-32 I/O transaction has split physical backing");
        }
        const struct gm_region *region = ia64_physical_region(v, first, chunk);
        if (!region ||
            (region->kind == GM_EMULATED &&
             (!region->ops || (write ? !region->ops->write : !region->ops->read))) ||
            (region->kind != GM_EMULATED && ((first | (uintptr_t)region->hva) & (chunk - 1))))
            return stop(first, "IA-32 I/O transaction backing is not representable");
        transactions[(*count)++] = (struct port_transaction){first, chunk, i * 8};
        *breakpoints |= debug(v, n, address, chunk, write);
        i += chunk;
    }
    uint64_t address = port_address(n->kr0, port);
    if ((v->s.psr & (UINT64_C(1) << 3)) && (port & (width - 1))) {
        struct ia32_record_input input = {.kind = IA32_RECORD_EXCEPTION,
                                          .phase = IA32_PHASE_BEFORE,
                                          .source = {(uint32_t)n->cs.base + n->eip.value},
                                          .vector = 17,
                                          .ifa_valid = true,
                                          .ifa = address};
        return ia32_make_record(&input, record) == IA32_STATE_OK
                   ? ia64_complete()
                   : stop(address, "IA-32 I/O alignment record");
    }
    return ia64_complete();
}
/* Freeze the complete physical reference before consuming a device input. */
static struct ia64_result string_check(struct vcpu *v, const struct ia32_native_state *n,
                                       unsigned segment, uint32_t offset, unsigned width,
                                       bool write, struct port_transaction transactions[4],
                                       unsigned *count, unsigned *breakpoints,
                                       struct ia32_interruption *record)
{
    struct ia64_result r = ia32_operand_prepare(v, n, segment, offset, width, write, width, record);
    if (r.kind != IA64_STEP_COMPLETE || record->valid)
        return r;
    const struct ia32_segment *s = ia32_segment(n, segment);
    uint32_t address = (uint32_t)s->base + offset;
    gpa_t physical[4];
    for (unsigned i = 0; i < width; ++i) {
        struct ia64_translation t;
        r = ia64_translate(v, (uint32_t)(address + i), IA64_REF_DATA,
                            write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ, n->cpl, false, &t);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        physical[i] = t.pa;
    }
    *count = 0;
    for (unsigned i = 0; i < width;) {
        unsigned chunk = 1;
        while (i + 2 * chunk <= width && !(physical[i] & (2 * chunk - 1)) &&
               ia64_physical_region(v, physical[i], 2 * chunk)) {
            bool contiguous = true;
            for (unsigned j = chunk; j < 2 * chunk; ++j)
                contiguous &= physical[i + j] == physical[i] + j;
            if (!contiguous)
                break;
            chunk *= 2;
        }
        const struct gm_region *region = ia64_physical_region(v, physical[i], chunk);
        if (!region ||
            (region->kind == GM_EMULATED &&
             (!region->ops || (write ? !region->ops->write : !region->ops->read))) ||
            (region->kind != GM_EMULATED &&
             ((uintptr_t)(region->hva + (physical[i] - region->base)) & (chunk - 1))))
            return stop(physical[i], "IA-32 I/O string backing is not representable");
        transactions[(*count)++] = (struct port_transaction){physical[i], chunk, i * 8};
        i += chunk;
    }
    *breakpoints |= debug(v, n, address, width, write);
    return ia64_complete();
}
static struct ia64_result string_access(struct vcpu *v, const struct ia32_native_state *n,
                                        unsigned segment, uint32_t offset, unsigned width,
                                        const struct port_transaction *transactions, unsigned count,
                                        bool write, uint64_t *value)
{
    uint64_t out = 0;
    for (unsigned i = 0; i < count; ++i) {
        const struct port_transaction *t = &transactions[i];
        uint64_t part = (*value >> t->shift) & ia64_low_mask(t->width * 8);
        struct ia64_result r = write ? ia64_physical_write(v, t->pa, t->width, part)
                                     : ia64_physical_read(v, t->pa, t->width, &part);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        out |= part << t->shift;
    }
    if (!write)
        *value = out;
    ia32_note_operand(v, (uint32_t)ia32_segment(n, segment)->base + offset, width, write);
    return ia64_complete();
}
struct ia64_result ia32_io(struct vcpu *v, struct ia32_native_state *n,
                           const struct ia32_decoded *d, const uint8_t *bytes,
                           struct ia32_interruption *record)
{
    unsigned op = d->opcode, width = op & 1 ? d->operand_bits / 8 : 1;
    bool string = op >= 0x6c && op <= 0x6f;
    bool input = !(op & 2);
    bool repeat = string && (d->flags & IA32_INSN_REP);
    uint32_t mask = d->address_bits == 32 ? UINT32_MAX : 0xffff;
    unsigned port = op >= 0xe4 && op <= 0xe7 ? bytes[d->length - 1] : n->gpr[IA32_EDX] & 0xffff;
    if (width != 1 && width != 2 && width != 4)
        return stop(op, "IA-32 I/O operand width");
    if (repeat && !(n->gpr[IA32_ECX] & mask)) {
        n->eip.value += d->length;
        n->eflag &= ~0x10000u;
        return ia64_complete();
    }
    bool before_final = repeat && (n->gpr[IA32_ECX] & mask) > 1;
    struct ia64_result r = permission(v, n, port, width, record);
    if (r.kind != IA64_STEP_COMPLETE || record->valid)
        return failure(n, before_final, r, record);
    unsigned breakpoints = 0, count = 0;
    struct port_transaction transactions[4];
    r = ports(v, n, port, width, !input, transactions, &count, &breakpoints, record);
    if (r.kind != IA64_STEP_COMPLETE || record->valid)
        return failure(n, before_final, r, record);
    unsigned index = input ? IA32_EDI : IA32_ESI;
    uint32_t offset = n->gpr[index] & mask;
    unsigned segment = input ? IA32_SEG_ES : d->segment;
    uint64_t value = n->gpr[IA32_EAX];
    struct port_transaction string_transactions[4];
    unsigned string_count = 0;
    if (string) {
        r = string_check(v, n, segment, offset, width, input, string_transactions, &string_count,
                         &breakpoints, record);
        if (r.kind != IA64_STEP_COMPLETE || record->valid)
            return failure(n, before_final, r, record);
        if (!input) {
            r = string_access(v, n, segment, offset, width, string_transactions, string_count,
                              false, &value);
            if (r.kind != IA64_STEP_COMPLETE || record->valid)
                return failure(n, before_final, r, record);
        }
    }
    if (input)
        value = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < count; ++i) {
        const struct port_transaction *t = &transactions[i];
        uint64_t part = (value >> t->shift) & ia64_low_mask(t->width * 8);
        r = input ? ia64_physical_read_ordered(v, t->pa, t->width, &part, true)
                  : ia64_physical_write_ordered(v, t->pa, t->width, part, true);
        if (r.kind != IA64_STEP_COMPLETE)
            return failure(n, before_final, r, record);
        if (input)
            value |= part << t->shift;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (string) {
        if (input) {
            r = string_access(v, n, segment, offset, width, string_transactions, string_count,
                              true, &value);
            if (r.kind != IA64_STEP_COMPLETE || record->valid)
                return failure(n, before_final, r, record);
        }
        int delta = n->eflag & 0x400 ? -(int)width : (int)width;
        n->gpr[index] = (n->gpr[index] & ~mask) | ((offset + delta) & mask);
        if (repeat)
            n->gpr[IA32_ECX] = (n->gpr[IA32_ECX] & ~mask) | ((n->gpr[IA32_ECX] - 1) & mask);
    } else if (input) {
        uint32_t operand_mask = (uint32_t)ia64_low_mask(width * 8);
        n->gpr[IA32_EAX] = (n->gpr[IA32_EAX] & ~operand_mask) | (uint32_t)value;
    }
    uint32_t source = (uint32_t)n->cs.base + n->eip.value;
    bool partial = repeat && (n->gpr[IA32_ECX] & mask);
    if (!partial) {
        n->eip.value += d->length;
        n->eflag &= ~0x10000u;
    } else
        n->eflag |= 0x10000; /* Vol.2 Table 10-3: REP resumes after an interruption with RF set. */
    if (breakpoints || (partial && ((v->s.psr & IA64_PSR_SS) || (n->eflag & 0x100)))) {
        struct ia32_record_input trap = {
            .kind = IA32_RECORD_EXCEPTION,
            .phase = IA32_PHASE_COMPLETED,
            .source = {source},
            .next = {(uint32_t)n->cs.base + n->eip.value},
            .next_valid = true,
            .vector = 1,
            .code = breakpoints << 4 | (((v->s.psr & IA64_PSR_SS) || (n->eflag & 0x100)) ? 8 : 0)};
        if (ia32_make_record(&trap, record) != IA32_STATE_OK)
            return stop(source, "IA-32 I/O completion debug record");
    }
    return ia64_complete();
}
