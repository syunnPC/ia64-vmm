/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/ia32/run.h"
#include "ia64/ia32/access.h"
#include "ia64/ia32/segment.h"
#include "ia64/ia32/extended.h"
#include "ia64/ia32/clock.h"
#include "ia64/memory.h"
#include "ia64/mmu.h"
#include "ia64/pmu.h"

static struct ia64_result stop(const char *detail)
{
    return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, 0, detail);
}
static uint32_t linear(const struct ia32_native_state *n)
{
    return (uint32_t)n->cs.base + n->eip.value;
}
const struct ia32_segment *ia32_segment(const struct ia32_native_state *n, unsigned id)
{
    switch (id) {
    case IA32_SEG_ES:
        return &n->es;
    case IA32_SEG_CS:
        return &n->cs;
    case IA32_SEG_SS:
        return &n->ss;
    case IA32_SEG_DS:
        return &n->ds;
    case IA32_SEG_FS:
        return &n->fs;
    case IA32_SEG_GS:
        return &n->gs;
    default:
        return NULL;
    }
}
struct ia64_result ia32_exception(const struct ia32_native_state *n, unsigned vector, unsigned code,
                                  struct ia32_interruption *r)
{
    struct ia32_record_input in = {.kind = IA32_RECORD_EXCEPTION,
                                   .phase = IA32_PHASE_BEFORE,
                                   .source = {linear(n)},
                                   .vector = vector,
                                   .code = code};
    return ia32_make_record(&in, r) == IA32_STATE_OK
               ? ia64_complete()
               : stop("IA-32 exception cannot be represented");
}
/* Validate the entire range before issuing an access, including page splits.
 * Descriptor accesses retain PSR.cpl (SDM Vol.2 10.6.6). */
static struct ia64_result memory_access(struct vcpu *v, uint64_t address, unsigned size, bool write,
                                        unsigned level, uint64_t *value, bool perform)
{
    if (!size || size > 8)
        return stop("IA-32 assist memory width");
    gpa_t pa[8];
    for (unsigned i = 0; i < size; ++i) {
        struct ia64_translation t;
        struct ia64_result r =
            ia64_translate(v, (uint32_t)(address + i), IA64_REF_DATA,
                           write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ, level, false, &t);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        pa[i] = t.pa;
        const struct gm_region *region = ia64_physical_region(v, t.pa, 1);
        if (!region ||
            (region->kind == GM_EMULATED &&
             (!region->ops || (write ? !region->ops->write : !region->ops->read))))
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, t.pa, "IA-32 assist unmapped backing");
    }
    if (!perform)
        return ia64_complete();
    uint64_t out = 0;
    for (unsigned i = 0; i < size;) {
        unsigned width = 1;
        while (width < 8 && i + width * 2 <= size && !(pa[i] & (width * 2 - 1)) &&
               ia64_physical_region(v, pa[i], width * 2)) {
            bool contiguous = true;
            for (unsigned j = width; j < width * 2; ++j)
                contiguous &= pa[i + j] == pa[i] + j;
            if (!contiguous)
                break;
            width *= 2;
        }
        uint64_t part = *value >> (8 * i);
        struct ia64_result r = write ? ia64_physical_write(v, pa[i], width, part)
                                     : ia64_physical_read(v, pa[i], width, &part);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        out |= (part & ia64_low_mask(8 * width)) << (8 * i);
        i += width;
    }
    if (!write)
        *value = out;
    return ia64_complete();
}
static struct ia64_result memory(struct vcpu *v, uint64_t address, unsigned size, bool write,
                                 unsigned level, uint64_t *value)
{
    return memory_access(v, address, size, write, level, value, true);
}
struct ia64_result ia32_operand(struct vcpu *v, const struct ia32_native_state *n, unsigned id,
                                uint32_t offset, unsigned size, bool write, uint64_t *value,
                                struct ia32_interruption *record)
{
    if (!size || size > 8)
        return stop("IA-32 scalar operand width");
    struct ia64_result result =
        ia32_operand_prepare(v, n, id, offset, size, write, size == 6 ? 4 : size, record);
    if (result.kind != IA64_STEP_COMPLETE || record->valid)
        return result;
    uint8_t bytes[8];
    for (unsigned i = 0; i < size; ++i)
        bytes[i] = *value >> (8 * i);
    result = ia32_operand_bytes(v, n, id, offset, size, write, bytes, record);
    if (!write && result.kind == IA64_STEP_COMPLETE && !record->valid) {
        *value = 0;
        for (unsigned i = 0; i < size; ++i)
            *value |= (uint64_t)bytes[i] << (8 * i);
    }
    return result;
}

static unsigned trap_code(const struct vcpu *v, const struct ia32_native_state *n)
{
    return ((v->s.psr & IA64_PSR_SS) || (n->eflag & 0x100)) ? 8 : 0;
}
static struct ia64_result intercept(const struct ia32_native_state *n, const struct ia32_decoded *d,
                                    struct ia32_interruption *r)
{
    struct ia32_record_input in = {.kind = IA32_RECORD_INSTRUCTION,
                                   .phase = IA32_PHASE_BEFORE,
                                   .source = {linear(n)},
                                   .code = d->intercept_code,
                                   .iim = d->iim};
    return ia32_make_record(&in, r) == IA32_STATE_OK ? ia64_complete()
                                                     : stop("IA-32 instruction record");
}
static uint32_t stack_offset(const struct ia32_native_state *n, int delta)
{
    uint32_t sp = n->gpr[IA32_ESP] + (uint32_t)delta;
    return (n->ss.attrib & 0x400) ? sp : (uint16_t)sp;
}
static void stack_advance(struct ia32_native_state *n, int delta)
{
    uint32_t sp = stack_offset(n, delta);
    n->gpr[IA32_ESP] = (n->ss.attrib & 0x400) ? sp : (n->gpr[IA32_ESP] & 0xffff0000) | sp;
}
static struct ia64_result descriptor(struct vcpu *v, const struct ia32_native_state *n,
                                     uint16_t selector, uint64_t *raw, struct ia32_segment *out,
                                     struct ia32_interruption *r)
{
    const struct ia32_segment *table = (selector & 4) ? &n->ldtr : &n->gdtr;
    unsigned off = selector & 0xfff8;
    unsigned id = (selector & 4) ? IA32_SEG_LDT : IA32_SEG_GDT;
    if (!ia32_segment_valid(n, id, IA32_SEGMENT_READ) || off + 7 > table->limit)
        return ia32_exception(n, 13, selector & 0xfffc, r);
    if (table->base > UINT32_MAX)
        return stop("IA-32 descriptor table base");
    struct ia64_result result = memory(v, (uint32_t)table->base + off, 8, false, n->cpl, raw);
    if (result.kind != IA64_STEP_COMPLETE) {
        result.fault_isr |= UINT64_C(1) << 40;
        return result;
    }
    uint32_t base =
        ((*raw >> 16) & 0xffff) | ((*raw >> 16) & 0xff0000) | ((*raw >> 32) & 0xff000000);
    unsigned limit = (*raw & 0xffff) | ((*raw >> 32) & 0xf0000);
    unsigned attrib = ((*raw >> 40) & 0xff) | ((*raw >> 44) & 0xf00);
    *out = (struct ia32_segment){.base = base,
                                 .limit = (attrib & 0x800) ? (limit << 12) | 4095 : limit,
                                 .attrib = attrib & ~0x300,
                                 .selector = selector};
    return ia64_complete();
}
static struct ia64_result load_segment(struct vcpu *v, struct ia32_native_state *n, unsigned id,
                                       uint16_t selector, unsigned *loaded,
                                       struct ia32_interruption *r)
{
    struct ia32_segment *s = (struct ia32_segment *)ia32_segment(n, id);
    if (!s || id == IA32_SEG_CS)
        return stop("IA-32 segment load target");
    struct ia32_segment replacement = *s;
    if (!(n->cflg & 1) || (n->eflag & 0x20000)) {
        replacement.selector = selector;
        replacement.base = (uint32_t)selector << 4;
        replacement.unusable = false;
        if (n->eflag & 0x20000) {
            replacement.limit = 0xffff;
            replacement.attrib = 0xf3;
        }
        *s = replacement;
        *loaded |= 1u << id;
        return ia64_complete();
    }
    if (!(selector & 0xfffc)) {
        if (id == IA32_SEG_SS)
            return ia32_exception(n, 13, 0, r);
        replacement.selector = selector;
        replacement.unusable = true;
    } else {
        uint64_t raw;
        struct ia64_result result = descriptor(v, n, selector, &raw, &replacement, r);
        if (result.kind != IA64_STEP_COMPLETE || r->valid)
            return result;
        unsigned a = replacement.attrib, dpl = (a >> 5) & 3;
        if (!(a & 0x10) || ((a & 8) && !(a & 2)) ||
            (id == IA32_SEG_SS ? (a & 0xa) != 2 || dpl != n->cpl || (selector & 3) != n->cpl
                               : (!(a & 8) || !(a & 4)) && (dpl < n->cpl || dpl < (selector & 3))))
            return ia32_exception(n, 13, selector & 0xfffc, r);
        if (!(a & 0x80))
            return ia32_exception(n, id == IA32_SEG_SS ? 12 : 11, selector & 0xfffc, r);
        if (!(a & 1)) {
            const struct ia32_segment *table = (selector & 4) ? &n->ldtr : &n->gdtr;
            uint64_t accessed = ((raw >> 40) & 255) | 1;
            result = memory(v, (uint32_t)table->base + (selector & 0xfff8) + 5, 1, true, n->cpl,
                            &accessed);
            if (result.kind != IA64_STEP_COMPLETE) {
                result.fault_isr |= UINT64_C(1) << 40;
                return result;
            }
        }
        replacement.attrib |= 1;
    }
    *s = replacement;
    *loaded |= 1u << id;
    return ia64_complete();
}
static uint32_t immediate(const uint8_t *p, unsigned size)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (uint32_t)p[i] << (i * 8);
    return value;
}
static struct ia64_result query_descriptor(struct vcpu *v, struct ia32_native_state *n,
                                           const struct ia32_decoded *d,
                                           struct ia32_interruption *r)
{
    if (!(n->cflg & 1) || (n->eflag & 0x20000))
        return intercept(n, d, r);
    uint64_t value = n->gpr[d->modrm & 7];
    if (d->operand_memory) {
        struct ia64_result result = ia32_operand(v, n, d->segment, d->offset, 2, false, &value, r);
        if (result.kind != IA64_STEP_COMPLETE || r->valid)
            return result;
    }
    uint16_t selector = value;
    const struct ia32_segment *table = (selector & 4) ? &n->ldtr : &n->gdtr;
    bool valid = (selector & 0xfffc) && (selector & 0xfff8) + 7u <= table->limit;
    if (valid) {
        uint64_t raw;
        struct ia32_segment seg;
        struct ia64_result result = descriptor(v, n, selector, &raw, &seg, r);
        if (result.kind != IA64_STEP_COMPLETE || r->valid)
            return result;
        unsigned type = seg.attrib & 31, dpl = (seg.attrib >> 5) & 3;
        bool conforming = (type & 28) == 28;
        valid = conforming || (n->cpl <= dpl && (selector & 3) <= dpl);
        if (d->opcode == 0x0f02) {
            valid &= type >= 16 || type == 1 || type == 2 || type == 3 || type == 4 || type == 5 ||
                     type == 9 || type == 11 || type == 12;
            value = (raw >> 32) & 0xf0ff00;
        } else if (d->opcode == 0x0f03) {
            valid &= type >= 16 || type == 1 || type == 2 || type == 3 || type == 9 || type == 11;
            value = seg.limit;
        } else if (((d->modrm >> 3) & 7) == 4)
            valid &= type >= 16 && (!(type & 8) || (type & 2));
        else
            valid &= (type & 26) == 18;
    }
    n->eflag = (n->eflag & ~0x40u) | (valid ? 0x40 : 0);
    if (valid && d->opcode != 0x0f00) {
        unsigned reg = (d->modrm >> 3) & 7;
        n->gpr[reg] =
            d->operand_bits == 16 ? (n->gpr[reg] & 0xffff0000) | (value & 0xffff) : (uint32_t)value;
    }
    return ia64_complete();
}
static struct ia64_result far_transfer(struct vcpu *v, struct ia32_native_state *n,
                                       const struct ia32_decoded *d, const uint8_t *bytes,
                                       unsigned *loaded, struct ia32_interruption *record)
{
    unsigned op = d->opcode, width = d->operand_bits / 8;
    bool call = op == 0x9a || (op == 0xff && ((d->modrm >> 3) & 7) == 3);
    bool ret = op == 0xca || op == 0xcb;
    uint64_t pointer = 0, raw;
    struct ia64_result r;
    if (ret) {
        uint64_t selector_value = 0;
        r = ia32_operand(v, n, IA32_SEG_SS, stack_offset(n, 0), width, false, &pointer, record);
        if (r.kind != IA64_STEP_COMPLETE || record->valid)
            return r;
        r = ia32_operand(v, n, IA32_SEG_SS, stack_offset(n, width), width, false, &selector_value,
                         record);
        if (r.kind != IA64_STEP_COMPLETE || record->valid)
            return r;
        pointer |= selector_value << (width * 8);
    } else if (d->operand_memory) {
        r = ia32_operand(v, n, d->segment, d->offset, width + 2, false, &pointer, record);
        if (r.kind != IA64_STEP_COMPLETE || record->valid)
            return r;
    } else {
        pointer = immediate(bytes + d->prefix_length + 1, width);
        pointer |= (uint64_t)immediate(bytes + d->prefix_length + 1 + width, 2) << (width * 8);
    }
    uint16_t selector = pointer >> (width * 8);
    uint32_t target = pointer & (width == 2 ? 0xffff : UINT32_MAX);
    bool protected = (n->cflg & 1) && !(n->eflag & 0x20000);
    unsigned extra = ret && op == 0xca ? immediate(bytes + d->prefix_length + 1, 2) : 0;
    struct ia32_segment cs = n->cs, ss = n->ss;
    unsigned new_cpl = n->cpl;
    uint32_t new_sp = 0;
    uint64_t ss_raw = 0;
    if (protected) {
        if (!(selector & 0xfffc))
            return ia32_exception(n, 13, 0, record);
        r = descriptor(v, n, selector, &raw, &cs, record);
        if (r.kind != IA64_STEP_COMPLETE || record->valid)
            return r;
        unsigned type = cs.attrib & 15, dpl = (cs.attrib >> 5) & 3;
        if (!(cs.attrib & 0x10)) {
            if (ret || (type != 1 && type != 4 && type != 5 && type != 9 && type != 12) ||
                ((type == 1 || type == 9) && (selector & 4)) || dpl < n->cpl ||
                dpl < (selector & 3))
                return ia32_exception(n, 13, selector & 0xfffc, record);
            if (!(cs.attrib & 0x80))
                return ia32_exception(n, 11, selector & 0xfffc, record);
            struct ia32_record_input in = {.kind = IA32_RECORD_GATE,
                                           .phase = IA32_PHASE_COMPLETED,
                                           .source = {linear(n)},
                                           .next = {linear(n) + d->length},
                                           .next_valid = true,
                                           .code = (call ? 0 : 0x4000) | trap_code(v, n) |
                                                   ((v->s.psr & IA64_PSR_TB) ? 4 : 0),
                                           .ifa = selector,
                                           .ifa_valid = true,
                                           .iim = raw};
            if (ia32_make_record(&in, record) != IA32_STATE_OK)
                return stop("IA-32 gate intercept record");
            n->eip.value += d->length;
            n->eflag &= ~0x10000u;
            return ia64_complete();
        }
        new_cpl = ret ? selector & 3 : n->cpl;
        if (!(type & 8) || new_cpl < n->cpl || ((type & 4) ? dpl > new_cpl : dpl != new_cpl) ||
            (!ret && (selector & 3) > n->cpl))
            return ia32_exception(n, 13, selector & 0xfffc, record);
        if (!(cs.attrib & 0x80))
            return ia32_exception(n, 11, selector & 0xfffc, record);
        if (new_cpl != n->cpl) {
            uint64_t sp_value = 0, ss_value = 0;
            r = ia32_operand(v, n, IA32_SEG_SS, stack_offset(n, 2 * width + extra), width, false,
                             &sp_value, record);
            if (r.kind != IA64_STEP_COMPLETE || record->valid)
                return r;
            r = ia32_operand(v, n, IA32_SEG_SS, stack_offset(n, 3 * width + extra), width, false,
                             &ss_value, record);
            if (r.kind != IA64_STEP_COMPLETE || record->valid)
                return r;
            uint16_t ss_selector = ss_value;
            if (!(ss_selector & 0xfffc))
                return ia32_exception(n, 13, 0, record);
            r = descriptor(v, n, ss_selector, &ss_raw, &ss, record);
            if (r.kind != IA64_STEP_COMPLETE || record->valid)
                return r;
            if ((ss.attrib & 0x1a) != 0x12 || ((ss.attrib >> 5) & 3) != new_cpl ||
                (ss_selector & 3) != new_cpl)
                return ia32_exception(n, 13, ss_selector & 0xfffc, record);
            if (!(ss.attrib & 0x80))
                return ia32_exception(n, 12, ss_selector & 0xfffc, record);
            new_sp = sp_value;
        }
    } else {
        cs.selector = selector;
        cs.base = (uint32_t)selector << 4;
        cs.unusable = false;
        if (n->eflag & 0x20000) {
            cs.limit = 0xffff;
            cs.attrib = 0xfb;
        }
    }
    if (target > cs.limit)
        return ia32_exception(n, 13, 0, record);
    if (protected) {
        struct ia32_segment *segments[] = {&cs, &ss};
        uint64_t raws[] = {raw, ss_raw};
        unsigned count = new_cpl != n->cpl ? 2 : 1;
        /* Validate both descriptor writes before changing either cache. */
        for (unsigned i = 0; i < count; ++i) {
            struct ia32_segment *seg = segments[i];
            if (seg->attrib & 1)
                continue;
            const struct ia32_segment *table = (seg->selector & 4) ? &n->ldtr : &n->gdtr;
            uint64_t value = (raws[i] >> 40) | 1;
            r = memory_access(v, (uint32_t)table->base + (seg->selector & 0xfff8) + 5, 1, true,
                              n->cpl, &value, false);
            if (r.kind != IA64_STEP_COMPLETE) {
                r.fault_isr |= UINT64_C(1) << 40;
                return r;
            }
        }
        for (unsigned i = 0; i < count; ++i) {
            struct ia32_segment *seg = segments[i];
            if (seg->attrib & 1)
                continue;
            const struct ia32_segment *table = (seg->selector & 4) ? &n->ldtr : &n->gdtr;
            uint64_t value = ((raws[i] >> 40) & 255) | 1;
            r = memory(v, (uint32_t)table->base + (seg->selector & 0xfff8) + 5, 1, true, n->cpl,
                       &value);
            if (r.kind != IA64_STEP_COMPLETE) {
                r.fault_isr |= UINT64_C(1) << 40;
                return r;
            }
            seg->attrib |= 1;
        }
    }
    if (call) {
        uint32_t offset = stack_offset(n, -(int)(2 * width));
        r = ia32_operand_prepare(v, n, IA32_SEG_SS, offset, 2 * width, true, width, record);
        if (r.kind != IA64_STEP_COMPLETE || record->valid)
            return r;
        uint64_t value = (n->eip.value + d->length) & (width == 2 ? 0xffffu : UINT32_MAX);
        value |= (uint64_t)n->cs.selector << (width * 8);
        r = memory_access(v, (uint32_t)n->ss.base + offset, 2 * width, true, n->cpl, &value, false);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        /* Each push has operand-size alignment, not the combined size. */
        uint64_t low = (uint32_t)value;
        r = ia32_operand(v, n, IA32_SEG_SS, offset, width, true, &low, record);
        if (r.kind != IA64_STEP_COMPLETE || record->valid)
            return r;
        low = value >> (width * 8);
        r = ia32_operand(v, n, IA32_SEG_SS, offset + width, width, true, &low, record);
        if (r.kind != IA64_STEP_COMPLETE || record->valid)
            return r;
        stack_advance(n, -(int)(2 * width));
    } else if (ret) {
        if (new_cpl != n->cpl) {
            n->ss = ss;
            n->gpr[IA32_ESP] =
                width == 2 ? (n->gpr[IA32_ESP] & 0xffff0000) | (uint16_t)new_sp : new_sp;
            stack_advance(n, extra);
            *loaded |= 1u << IA32_SEG_SS;
            n->cpl = new_cpl;
            const unsigned data[] = {IA32_SEG_ES, IA32_SEG_DS, IA32_SEG_FS, IA32_SEG_GS};
            for (unsigned i = 0; i < IA64VMM_ARRAY_SIZE(data); ++i) {
                struct ia32_segment *seg = (struct ia32_segment *)ia32_segment(n, data[i]);
                unsigned a = seg->attrib;
                if ((!(a & 8) || !(a & 4)) && ((a >> 5) & 3) < new_cpl) {
                    seg->selector = 0;
                    seg->unusable = true;
                    *loaded |= 1u << data[i];
                }
            }
        } else
            stack_advance(n, 2 * width + extra);
    }
    cs.selector = protected ? (selector & ~3u) | n->cpl : selector;
    cs.attrib |= 1;
    n->cs = cs;
    n->eip.value = target;
    n->eflag &= ~0x10000u;
    *loaded |= 1u << IA32_SEG_CS;
    return ia64_complete();
}
struct ia64_result ia32_assist(struct vcpu *v, struct ia32_native_state *n,
                               const struct ia32_decoded *d, const uint8_t *bytes, bool *handled,
                               unsigned *loaded, struct ia32_interruption *r)
{
    *handled = true;
    if (d->kind == IA32_INSN_INTERCEPT)
        return intercept(n, d, r);
    uint32_t source = linear(n), next = n->eip.value + d->length, old_flags = n->eflag;
    unsigned width = d->operand_bits / 8, op = d->opcode;
    uint64_t value = 0;
    struct ia64_result result = ia64_complete();
    if (d->kind != IA32_INSN_INTERCEPT) {
        result = ia32_extended(v, n, d, bytes, handled, r);
        if (*handled || result.kind != IA64_STEP_COMPLETE || r->valid)
            return result;
        *handled = true;
    }
    if (op == 0x0f02 || op == 0x0f03 ||
        (op == 0x0f00 && ((d->modrm >> 3) & 7) >= 4 && ((d->modrm >> 3) & 7) <= 5)) {
        result = query_descriptor(v, n, d, r);
        if (result.kind != IA64_STEP_COMPLETE || r->valid)
            return result;
        goto complete;
    }
    if (op == 0x63 && (!(n->cflg & 1) || (n->eflag & 0x20000)))
        return intercept(n, d, r);
    if (op == 0x8c || op == 0x06 || op == 0x0e || op == 0x16 || op == 0x1e || op == 0x0fa0 ||
        op == 0x0fa8) {
        unsigned id = op == 0x8c     ? (d->modrm >> 3) & 7
                      : op == 0x06   ? IA32_SEG_ES
                      : op == 0x0e   ? IA32_SEG_CS
                      : op == 0x16   ? IA32_SEG_SS
                      : op == 0x1e   ? IA32_SEG_DS
                      : op == 0x0fa0 ? IA32_SEG_FS
                                     : IA32_SEG_GS;
        const struct ia32_segment *seg = ia32_segment(n, id);
        if (!seg || id > IA32_SEG_GS)
            return intercept(n, d, r);
        value = seg->selector;
        if (op != 0x8c || d->operand_memory) {
            result = ia32_operand(v, n, op == 0x8c ? d->segment : IA32_SEG_SS,
                                  op == 0x8c ? d->offset : stack_offset(n, -(int)width),
                                  op == 0x8c ? 2 : width, true, &value, r);
            if (result.kind != IA64_STEP_COMPLETE || r->valid)
                return result;
            if (op != 0x8c)
                stack_advance(n, -(int)width);
        } else {
            unsigned reg = d->modrm & 7;
            n->gpr[reg] = width == 2 ? (n->gpr[reg] & 0xffff0000) | value : value;
        }
        goto complete;
    }
    if (op == 0x0f18)
        goto complete; /* Prefetch hints may be ignored without an operand access. */
    switch (d->kind) {
    case IA32_INSN_INTERCEPT:
        return intercept(n, d, r);
    case IA32_INSN_JMPE: {
        enum ia32_state_status status = ia32_state_jmpe_check(&v->s, n);
        if (status == IA32_STATE_DISABLED)
            return ia64_fault(IA64_FAULT_DISABLED_ISA, source);
        if (status == IA32_STATE_FP_PENDING)
            return ia32_exception(n, 16, 0, r);
        if (status != IA32_STATE_OK)
            return stop("IA-32 JMPE state");
        value = d->operand;
        if (d->operand_memory) {
            result = ia32_operand(v, n, d->segment, d->offset, width, false, &value, r);
            if (result.kind != IA64_STEP_COMPLETE || r->valid)
                return result;
        }
        status = ia32_state_jmpe(&v->s, n, (uint32_t)value, (ia32_eip_t){next},
                                 ia32_operand_breakpoints(v), r);
        return status == IA32_STATE_OK ? ia64_complete() : stop("IA-32 JMPE materialization");
    }
    case IA32_INSN_INT:
    case IA32_INSN_INT3:
    case IA32_INSN_INTO: {
        if (d->kind == IA32_INSN_INTO && !(old_flags & 0x800))
            break;
        struct ia32_record_input in = {.kind = d->kind == IA32_INSN_INT ? IA32_RECORD_SOFTWARE_INT
                                                                        : IA32_RECORD_EXCEPTION,
                                       .phase = IA32_PHASE_COMPLETED,
                                       .source = {source},
                                       .next = {(uint32_t)n->cs.base + next},
                                       .next_valid = true,
                                       .vector = d->kind == IA32_INSN_INT    ? d->vector
                                                 : d->kind == IA32_INSN_INT3 ? 3
                                                                             : 4,
                                       .code = trap_code(v, n)};
        if (ia32_make_record(&in, r) != IA32_STATE_OK)
            return stop("IA-32 software interrupt record");
        break;
    }
    case IA32_INSN_CONTROL_READ: {
        if (n->cpl)
            return ia32_exception(n, 13, 0, r);
        unsigned cr = (d->modrm >> 3) & 7;
        switch (cr) {
        case 0:
            value = ((uint32_t)n->cflg & ~0x1c0u) | 0x10;
            break;
        case 2:
            value = n->kr2 >> 32;
            break;
        case 3:
            value = (uint32_t)n->kr2;
            break;
        case 4:
            value = n->cflg >> 32;
            break;
        default:
            return intercept(n, d, r);
        }
        n->gpr[d->modrm & 7] = (uint32_t)value;
        break;
    }
    case IA32_INSN_CPUID:
        /* Virtual Pentium III feature set; host CPUID never names host
         * topology, VMX, long mode or an unsaved extended xstate layout. */
        value = n->gpr[IA32_EAX];
        n->gpr[IA32_EAX] = n->gpr[IA32_EBX] = n->gpr[IA32_ECX] = n->gpr[IA32_EDX] = 0;
        if (value == 0) {
            n->gpr[IA32_EAX] = 1;
            n->gpr[IA32_EBX] = 0x756e6547;
            n->gpr[IA32_EDX] = 0x49656e69;
            n->gpr[IA32_ECX] = 0x6c65746e;
        } else if (value == 1) {
            n->gpr[IA32_EAX] = 0x673;
            n->gpr[IA32_EDX] = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) |
                               (1u << 24) | (1u << 25);
        }
        break;
    case IA32_INSN_CLOCK:
        result = ia32_clock(v, n,
                            op == 0x0f31   ? IA32_CLOCK_RDTSC
                            : op == 0x0f33 ? IA32_CLOCK_RDPMC
                                           : IA32_CLOCK_RDTSCP,
                            r);
        if (result.kind != IA64_STEP_COMPLETE || r->valid)
            return result;
        break;
    case IA32_INSN_SYSTEM_FLAG:
        if (op == 0x8e || op == 0x17 || op == 0x0fb2)
            goto segment_load;
        if (op == 0xfa || op == 0xfb) {
            bool vm = (old_flags & 0x20000) != 0;
            unsigned iopl = (old_flags >> 12) & 3;
            unsigned bit = 0x200;
            if ((n->cflg & 1) && (vm ? iopl != 3 : n->cpl > iopl)) {
                bool virtual = vm ? (n->cflg & (UINT64_C(1) << 32)) != 0
                                  : n->cpl == 3 && (n->cflg & (UINT64_C(1) << 33));
                if (!virtual || (op == 0xfb && (old_flags & 0x100000)))
                    return ia32_exception(n, 13, 0, r);
                bit = 0x80000;
            }
            n->eflag = op == 0xfb ? n->eflag | bit : n->eflag & ~bit;
        } else if (op == 0x9d) {
            bool virtual = (old_flags & 0x20000) && ((old_flags >> 12) & 3) < 3;
            if (virtual && (width == 4 || !(n->cflg & (UINT64_C(1) << 32))))
                return ia32_exception(n, 13, 0, r);
            result = ia32_operand(v, n, IA32_SEG_SS, stack_offset(n, 0), width, false, &value, r);
            if (result.kind != IA64_STEP_COMPLETE || r->valid)
                return result;
            uint32_t mask = width == 2 ? 0x7fd5 : 0x247fd5;
            if (virtual) {
                if ((value & 0x100) || ((value & 0x200) && (old_flags & 0x100000)))
                    return ia32_exception(n, 13, 0, r);
                mask &= ~0x3200u;
                n->eflag = (old_flags & ~(mask | 0x80000u)) | ((uint32_t)value & mask) |
                           ((value & 0x200) << 10) | 2;
            } else {
                if ((n->cflg & 1) && n->cpl)
                    mask &= ~0x3000u;
                if ((n->cflg & 1) && n->cpl > ((old_flags >> 12) & 3))
                    mask &= ~0x200u;
                n->eflag = (old_flags & ~mask) | ((uint32_t)value & mask) | 2;
            }
            stack_advance(n, width);
        } else
            return stop("IA-32 system flag instruction");
        if (((n->eflag ^ old_flags) & 0x40100) ||
            ((n->cflg & 0x100) && ((n->eflag ^ old_flags) & 0x200))) {
            struct ia32_record_input in = {
                .kind = IA32_RECORD_SYSTEM_FLAG,
                .phase = IA32_PHASE_COMPLETED,
                .source = {source},
                .next = {(uint32_t)n->cs.base + next},
                .next_valid = true,
                .code = ((op == 0xfa   ? 0
                          : op == 0xfb ? 1
                                       : 2)
                         << 14) |
                        (((v->s.psr & IA64_PSR_SS) || (old_flags & 0x100)) ? 8 : 0),
                .iim = old_flags};
            if (ia32_make_record(&in, r) != IA32_STATE_OK)
                return stop("IA-32 system flag record");
        }
        break;
    case IA32_INSN_SEGMENT: {
    segment_load:;
        bool pointer = op == 0xc4 || op == 0xc5 || op == 0x0fb2 || op == 0x0fb4 || op == 0x0fb5;
        unsigned id = op == 0x8e     ? (d->modrm >> 3) & 7
                      : op == 0x07   ? IA32_SEG_ES
                      : op == 0x17   ? IA32_SEG_SS
                      : op == 0x1f   ? IA32_SEG_DS
                      : op == 0x0fa1 ? IA32_SEG_FS
                      : op == 0x0fa9 ? IA32_SEG_GS
                      : op == 0xc4   ? IA32_SEG_ES
                      : op == 0xc5   ? IA32_SEG_DS
                      : op == 0x0fb2 ? IA32_SEG_SS
                      : op == 0x0fb4 ? IA32_SEG_FS
                      : op == 0x0fb5 ? IA32_SEG_GS
                                     : IA32_SEG_COUNT;
        bool pop = op != 0x8e && !pointer;
        if (id >= IA32_SEG_COUNT)
            return stop("IA-32 far pointer segment load");
        if (pop || d->operand_memory) {
            result = ia32_operand(v, n, pop ? IA32_SEG_SS : d->segment,
                                  pop ? stack_offset(n, 0) : d->offset,
                                  pointer ? width + 2
                                  : pop   ? width
                                          : 2,
                                  false, &value, r);
            if (result.kind != IA64_STEP_COMPLETE || r->valid)
                return result;
        } else
            value = n->gpr[d->modrm & 7];
        result = load_segment(v, n, id, pointer ? value >> (width * 8) : value, loaded, r);
        if (result.kind != IA64_STEP_COMPLETE || r->valid)
            return result;
        if (pointer) {
            unsigned reg = (d->modrm >> 3) & 7;
            n->gpr[reg] =
                width == 2 ? (n->gpr[reg] & 0xffff0000) | (value & 0xffff) : (uint32_t)value;
        }
        if (pop)
            stack_advance(n, width);
        if (id == IA32_SEG_SS && (op == 0x8e || op == 0x17)) {
            struct ia32_record_input in = {.kind = IA32_RECORD_SYSTEM_FLAG,
                                           .phase = IA32_PHASE_COMPLETED,
                                           .source = {source},
                                           .next = {(uint32_t)n->cs.base + next},
                                           .next_valid = true,
                                           .code = 0xc000 | trap_code(v, n),
                                           .iim = old_flags};
            if (ia32_make_record(&in, r) != IA32_STATE_OK)
                return stop("IA-32 SS load record");
        }
        break;
    }
    case IA32_INSN_FAR:
        return far_transfer(v, n, d, bytes, loaded, r);
    case IA32_INSN_IO:
        return ia32_io(v, n, d, bytes, r);
    default:
        if (op == 0x9c) {
            value = n->eflag & ~0x30000u;
            if ((n->eflag & 0x20000) && ((n->eflag >> 12) & 3) < 3) {
                if (width == 4 || !(n->cflg & (UINT64_C(1) << 32)))
                    return ia32_exception(n, 13, 0, r);
                value = (value & ~0x200u) | 0x3000 | ((n->eflag & 0x80000) >> 10);
            }
            result = ia32_operand(v, n, IA32_SEG_SS, stack_offset(n, -(int)width), width, true,
                                  &value, r);
            if (result.kind != IA64_STEP_COMPLETE || r->valid)
                return result;
            stack_advance(n, -(int)width);
            break;
        }
        (void)bytes;
        *handled = false;
        return ia64_complete();
    }
complete:
    n->eip.value = next;
    n->eflag &= ~0x10000u;
    return result;
}
