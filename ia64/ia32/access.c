/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/ia32/access.h"
#include "ia64/ia32/segment.h"
#include "ia64/mmu.h"
#include "ia64/memory.h"

static unsigned access_width(struct vcpu *v, const gpa_t *physical, unsigned size, unsigned i)
{
    unsigned width = 1;
    while (width < 8 && i + 2 * width <= size && !(physical[i] & (2 * width - 1)) &&
           ia64_physical_region(v, physical[i], 2 * width)) {
        bool contiguous = true;
        for (unsigned j = width; j < 2 * width; ++j)
            contiguous &= physical[i + j] == physical[i] + j;
        if (!contiguous)
            break;
        width *= 2;
    }
    return width;
}
static struct ia64_result prepare(struct vcpu *v, const struct ia32_native_state *n, unsigned id,
                                  uint32_t offset, unsigned size, bool write, unsigned alignment,
                                  gpa_t *physical, struct ia32_interruption *record)
{
    const struct ia32_segment *s = ia32_segment(n, id);
    if (!s || !size || size > 512 || s->base > UINT32_MAX)
        return ia64_exit(VCPU_EXIT_BAD_ACCESS, offset, "invalid IA-32 operand range");
    unsigned type = s->attrib & 15;
    uint64_t last = (uint64_t)offset + size - 1;
    bool expand = !(type & 8) && (type & 4);
    bool bound = expand ? offset > s->limit && last <= ((s->attrib & 0x400) ? UINT32_MAX : 0xffff)
                        : last <= s->limit;
    if (!bound || !ia32_segment_valid(n, id, write ? IA32_SEGMENT_WRITE : IA32_SEGMENT_READ))
        return ia32_exception(n, id == IA32_SEG_SS ? 12 : 13, 0, record);
    uint32_t address = (uint32_t)s->base + offset;
    for (unsigned i = 0; i < size; ++i) {
        struct ia64_translation t;
        struct ia64_result r =
            ia64_translate(v, (uint32_t)(address + i), IA64_REF_DATA,
                           write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ, n->cpl, false, &t);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        const struct gm_region *region = ia64_physical_region(v, t.pa, 1);
        if (!region ||
            (region->kind == GM_EMULATED &&
             (!region->ops || (write ? !region->ops->write : !region->ops->read))))
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, t.pa, "IA-32 operand has no backing operation");
        physical[i] = t.pa;
    }
    for (unsigned i = 0; i < size;) {
        unsigned width = access_width(v, physical, size, i);
        const struct gm_region *region = ia64_physical_region(v, physical[i], width);
        if (!region ||
            (region->kind != GM_EMULATED &&
             ((uintptr_t)(region->hva + (physical[i] - region->base)) & (width - 1))))
            return ia64_exit(VCPU_EXIT_BAD_ACCESS, physical[i], "unaligned IA-32 operand backing");
        i += width;
    }
    if (alignment > 1 && (address & (alignment - 1)) &&
        ((v->s.psr & (UINT64_C(1) << 3)) ||
         (n->cpl == 3 && (n->eflag & 0x40000) && (n->cflg & 0x40000)))) {
        struct ia32_record_input in = {.kind = IA32_RECORD_EXCEPTION,
                                       .phase = IA32_PHASE_BEFORE,
                                       .source = {(uint32_t)n->cs.base + n->eip.value},
                                       .vector = 17,
                                       .ifa_valid = true,
                                       .ifa = address};
        ia32_make_record(&in, record);
    }
    return ia64_complete();
}
struct ia64_result ia32_operand_prepare(struct vcpu *v, const struct ia32_native_state *n,
                                        unsigned id, uint32_t offset, unsigned size, bool write,
                                        unsigned alignment, struct ia32_interruption *record)
{
    gpa_t physical[512];
    return prepare(v, n, id, offset, size, write, alignment, physical, record);
}
struct ia64_result ia32_operand_bytes(struct vcpu *v, const struct ia32_native_state *n,
                                      unsigned id, uint32_t offset, unsigned size, bool write,
                                      uint8_t *bytes, struct ia32_interruption *record)
{
    gpa_t physical[512];
    struct ia64_result r = prepare(v, n, id, offset, size, write, 1, physical, record);
    if (r.kind != IA64_STEP_COMPLETE || record->valid)
        return r;
    for (unsigned i = 0; i < size;) {
        unsigned width = access_width(v, physical, size, i);
        uint64_t value = 0;
        if (write)
            for (unsigned j = 0; j < width; ++j)
                value |= (uint64_t)bytes[i + j] << (8 * j);
        r = write ? ia64_physical_write(v, physical[i], width, value)
                  : ia64_physical_read(v, physical[i], width, &value);
        if (r.kind != IA64_STEP_COMPLETE)
            return r;
        if (!write)
            for (unsigned j = 0; j < width; ++j)
                bytes[i + j] = value >> (8 * j);
        i += width;
    }
    ia32_note_operand(v, (uint32_t)ia32_segment(n, id)->base + offset, size, write);
    return ia64_complete();
}
