/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/ia32/string.h"
#include "ia64/ia32/access.h"

static struct ia64_result stop(unsigned opcode, const char *detail)
{
    return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, opcode, detail);
}
static struct ia64_result failure(struct ia32_native_state *n, bool before_final,
                                  struct ia64_result result, const struct ia32_interruption *record)
{
    if (before_final && (result.kind == IA64_STEP_FAULT || record->valid))
        n->eflag |= 0x10000;
    return result;
}
static unsigned debug(const struct vcpu *v, const struct ia32_native_state *n, unsigned segment,
                      uint32_t offset, unsigned width, bool write)
{
    if (!(v->s.psr & IA64_PSR_DB) || (v->s.psr & IA64_PSR_DD))
        return 0;
    const struct ia32_segment *s = ia32_segment(n, segment);
    uint32_t address = (uint32_t)s->base + offset;
    unsigned matches = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t control = v->s.dbr[2 * i + 1];
        if (!(control & (UINT64_C(1) << (56 + n->cpl))) ||
            !(control & (UINT64_C(1) << (write ? 62 : 63))))
            continue;
        uint64_t mask = UINT64_C(0xff00000000000000) | (control & UINT64_C(0x00ffffffffffffff));
        for (unsigned b = 0; b < width; ++b)
            if (!(((uint32_t)(address + b) ^ v->s.dbr[2 * i]) & mask))
                matches |= 1u << i;
    }
    return matches;
}
static uint32_t subtract_flags(uint64_t left, uint64_t right, unsigned width)
{
    uint64_t mask = (UINT64_C(1) << (width * 8)) - 1;
    uint64_t sign = UINT64_C(1) << (width * 8 - 1);
    left &= mask;
    right &= mask;
    uint64_t result = (left - right) & mask;
    unsigned flags = (left < right ? 1u : 0) | (((left ^ right ^ result) & 0x10) ? 0x10u : 0) |
                     (!result ? 0x40u : 0) | ((result & sign) ? 0x80u : 0) |
                     (((left ^ right) & (left ^ result) & sign) ? 0x800u : 0);
    if (!__builtin_parity((unsigned)result & 0xff))
        flags |= 4;
    return flags;
}
static void advance(struct ia32_native_state *n, unsigned reg, unsigned width, uint32_t mask)
{
    uint32_t delta = n->eflag & 0x400 ? 0u - width : width;
    n->gpr[reg] = (n->gpr[reg] & ~mask) | ((n->gpr[reg] + delta) & mask);
}
struct ia64_result ia32_string(struct vcpu *v, struct ia32_native_state *n,
                               const struct ia32_decoded *d, struct ia32_interruption *record)
{
    unsigned op = d->opcode;
    bool move = op == 0xa4 || op == 0xa5;
    bool compare = op == 0xa6 || op == 0xa7;
    bool store = op == 0xaa || op == 0xab;
    bool load = op == 0xac || op == 0xad;
    bool scan = op == 0xae || op == 0xaf;
    unsigned width = op & 1 ? d->operand_bits / 8 : 1;
    if (!(move || compare || store || load || scan) || (width != 1 && width != 2 && width != 4) ||
        (d->address_bits != 16 && d->address_bits != 32) || !d->length || d->length > 15 ||
        (d->flags & IA32_INSN_LOCK))
        return stop(op, "IA-32 string encoding is unimplemented");
    bool repeat = (d->flags & IA32_INSN_REP) != 0;
    uint32_t mask = d->address_bits == 32 ? UINT32_MAX : 0xffff;
    if (repeat && !(n->gpr[IA32_ECX] & mask)) {
        n->eip.value += d->length;
        n->eflag &= ~0x10000u;
        return ia64_complete();
    }
    bool before_final = repeat && (n->gpr[IA32_ECX] & mask) > 1;
    bool source = move || compare || load;
    bool destination = move || compare || store || scan;
    bool write = move || store;
    uint32_t si = n->gpr[IA32_ESI] & mask, di = n->gpr[IA32_EDI] & mask;
    struct ia64_result result;
    /* Preflight both operands before a source MMIO read can be consumed. */
    if (source) {
        result = ia32_operand_prepare(v, n, d->segment, si, width, false, width, record);
        if (result.kind != IA64_STEP_COMPLETE || record->valid)
            return failure(n, before_final, result, record);
    }
    if (destination) {
        result = ia32_operand_prepare(v, n, IA32_SEG_ES, di, width, write, width, record);
        if (result.kind != IA64_STEP_COMPLETE || record->valid)
            return failure(n, before_final, result, record);
    }
    uint64_t left = n->gpr[IA32_EAX], right = 0;
    unsigned breakpoints = 0;
    if (source) {
        result = ia32_operand(v, n, d->segment, si, width, false, &left, record);
        if (result.kind != IA64_STEP_COMPLETE || record->valid)
            return failure(n, before_final, result, record);
        breakpoints |= debug(v, n, d->segment, si, width, false);
    }
    if (destination) {
        uint64_t value = write ? left : 0;
        result = ia32_operand(v, n, IA32_SEG_ES, di, width, write, &value, record);
        if (result.kind != IA64_STEP_COMPLETE || record->valid)
            return failure(n, before_final, result, record);
        right = value;
        breakpoints |= debug(v, n, IA32_SEG_ES, di, width, write);
    }
    /* All effects of this iteration committed. Publish its register state
     * together, retaining each earlier REP iteration on a later fault. */
    if (load) {
        uint32_t operand_mask = (uint32_t)((UINT64_C(1) << (width * 8)) - 1);
        n->gpr[IA32_EAX] = (n->gpr[IA32_EAX] & ~operand_mask) | ((uint32_t)left & operand_mask);
    }
    if (compare || scan)
        n->eflag = (n->eflag & ~0x8d5u) | subtract_flags(left, right, width);
    if (source)
        advance(n, IA32_ESI, width, mask);
    if (destination)
        advance(n, IA32_EDI, width, mask);
    if (repeat)
        n->gpr[IA32_ECX] = (n->gpr[IA32_ECX] & ~mask) | ((n->gpr[IA32_ECX] - 1) & mask);
    bool partial = repeat && (n->gpr[IA32_ECX] & mask);
    if (partial && (compare || scan)) {
        bool equal = (n->eflag & 0x40) != 0;
        bool repne = (d->intercept_code & 0x20) != 0;
        partial = repne ? !equal : equal;
    }
    uint32_t source_ip = (uint32_t)n->cs.base + n->eip.value;
    if (partial)
        n->eflag |= 0x10000;
    else {
        n->eip.value += d->length;
        n->eflag &= ~0x10000u;
    }
    bool step = (v->s.psr & IA64_PSR_SS) || (n->eflag & 0x100);
    if (breakpoints || (partial && step)) {
        struct ia32_record_input trap = {.kind = IA32_RECORD_EXCEPTION,
                                         .phase = IA32_PHASE_COMPLETED,
                                         .source = {source_ip},
                                         .next = {(uint32_t)n->cs.base + n->eip.value},
                                         .next_valid = true,
                                         .vector = 1,
                                         .code = (breakpoints << 4) | (step ? 8 : 0)};
        if (ia32_make_record(&trap, record) != IA32_STATE_OK)
            return stop(op, "IA-32 string debug record is unrepresentable");
    }
    return ia64_complete();
}
