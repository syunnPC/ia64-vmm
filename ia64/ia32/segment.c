/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ia64/ia32/segment.h"
#include "ia64/ia32/run.h"

bool ia32_segment_valid(const struct ia32_native_state *n, unsigned id,
                         enum ia32_segment_reference reference)
{
    const struct ia32_segment *s = id == IA32_SEG_GDT   ? &n->gdtr
                                   : id == IA32_SEG_LDT ? &n->ldtr
                                   : id == IA32_SEG_TSS ? &n->tr
                                                       : ia32_segment(n, id);
    if (!s || s->unusable)
        return false;
    unsigned a = s->attrib, type = a & 15;
    if (id >= IA32_SEG_LDT)
        return reference != IA32_SEGMENT_FETCH && (a & 0x90) == 0x80;
    if ((a & 0x91) != 0x91)
        return false;
    bool vm = (n->eflag & 0x20000) != 0, pe = (n->cflg & 1) != 0;
    bool expand = !(type & 8) && (type & 4);
    if (vm && ((a & 0x400) || expand))
        return false;
    if (reference == IA32_SEGMENT_FETCH) {
        if (id != IA32_SEG_CS || expand || (!pe && n->cpl) ||
            (vm && (!pe || n->cpl != 3 || ((a >> 5) & 3) != 3)))
            return false;
        bool virtual = (n->cflg & (UINT64_C(1) << 33)) ||
                       (vm && (n->cflg & (UINT64_C(1) << 32)));
        return !((n->eflag & 0x180000) == 0x180000 && pe && n->cpl == 3 && virtual);
    }
    if (id == IA32_SEG_SS && ((a >> 5) & 3) != n->cpl)
        return false;
    if (id == IA32_SEG_CS && (!pe || vm))
        return true;
    return reference == IA32_SEGMENT_WRITE ? !(type & 8) && (type & 2)
                                           : !(type & 8) || (type & 2);
}
