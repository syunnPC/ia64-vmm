/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_AMD64_IA32_NATIVE_MODE_H
#define IA64VMM_AMD64_IA32_NATIVE_MODE_H
#include "ia64vmm/ia32.h"

#define AMD64_IA32_VM (UINT32_C(1) << 17)
/* The engine assists segment/table operations, flag instructions and all
 * privilege/mode changes. Hardware caches are private execution state;
 * none of their normalization writes a guest descriptor or semantic cache. */
struct amd64_ia32_segment_view {
    uint16_t cs, ss, semantic_cs, semantic_ss;
};
static inline const struct ia32_segment *amd64_ia32_segment(const struct ia32_native_state *s,
                                                            unsigned i)
{
    const struct ia32_segment *segments[] = {&s->es, &s->cs, &s->ss, &s->ds, &s->fs, &s->gs};
    return segments[i];
}
static inline uint16_t amd64_ia32_fp_selector(const struct ia32_fp_image *fp, unsigned off)
{
    return fp->bytes[off] | (uint16_t)fp->bytes[off + 1] << 8;
}
static inline uint16_t amd64_ia32_spare_selector(const struct ia32_native_state *s, unsigned which)
{
    unsigned free = 0;
    for (unsigned slot = 0; slot < 16; ++slot) {
        uint16_t selector = (uint16_t)((0xfff8u - 8 * slot) | s->cpl);
        bool used = selector == amd64_ia32_fp_selector(&s->fp, 12) ||
                    selector == amd64_ia32_fp_selector(&s->fp, 20);
        for (unsigned i = 0; i < 6; ++i)
            used |= selector == amd64_ia32_segment(s, i)->selector;
        if (!used && free++ == which)
            return selector;
    }
    return 0; /* At most eight imported selectors exclude sixteen choices. */
}
static inline struct amd64_ia32_segment_view
amd64_ia32_segment_view(const struct ia32_native_state *s)
{
    return (struct amd64_ia32_segment_view){
        .cs = (s->cs.selector & 3) == s->cpl ? s->cs.selector : amd64_ia32_spare_selector(s, 0),
        .ss = (s->ss.selector & 3) == s->cpl ? s->ss.selector : amd64_ia32_spare_selector(s, 1),
        .semantic_cs = s->cs.selector,
        .semantic_ss = s->ss.selector};
}
static inline struct ia32_segment amd64_ia32_private_segment(const struct ia32_native_state *s,
                                                             unsigned i)
{
    struct ia32_segment p = *amd64_ia32_segment(s, i);
    bool legacy = !(s->cflg & 1) || (s->eflag & AMD64_IA32_VM);
    uint16_t geometry = p.attrib & 0xc00; /* byte limit remains exact */
    uint16_t expand = (p.attrib & 8) ? 0 : p.attrib & 4;
    if (i == 1) {
        p.selector = amd64_ia32_segment_view(s).cs;
        p.attrib = geometry | 0x9b | (s->cpl << 5);
        p.unusable = false;
    } else if (i == 2) {
        p.selector = amd64_ia32_segment_view(s).ss;
        p.attrib = geometry | expand | 0x93 | (s->cpl << 5);
        p.unusable = false;
    } else if (!legacy && (p.unusable || !(p.attrib & 0x80))) {
        p.attrib = 0;
        p.unusable = true;
    } else {
        /* Permission/privilege faults use the semantic cache in the engine. */
        p.attrib = geometry | expand | 0xf3;
        p.unusable = false;
    }
    return p;
}
static inline uint32_t amd64_ia32_private_eflag(const struct ia32_native_state *s)
{
    return s->eflag & ~AMD64_IA32_VM;
}
static inline uint32_t amd64_ia32_semantic_eflag(const struct ia32_native_state *s, uint32_t raw)
{
    return (raw & ~AMD64_IA32_VM) | (s->eflag & AMD64_IA32_VM);
}
static inline void amd64_ia32_restore_fp_selectors(struct ia32_native_state *s,
                                                   struct amd64_ia32_segment_view view)
{
    for (unsigned off = 12; off <= 20; off += 8) {
        uint16_t selector = amd64_ia32_fp_selector(&s->fp, off);
        if (selector == view.cs && view.cs != view.semantic_cs)
            selector = view.semantic_cs;
        else if (selector == view.ss && view.ss != view.semantic_ss)
            selector = view.semantic_ss;
        s->fp.bytes[off] = selector;
        s->fp.bytes[off + 1] = selector >> 8;
    }
}
#endif
