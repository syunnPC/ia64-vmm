/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Legacy VGA: the check of the handoff's
 * legacy_* tail, used by native firmware and hosted tests. */
#ifndef VMM_LEGACY_HANDOFF_H
#define VMM_LEGACY_HANDOFF_H
#include <stdbool.h>
#include <stdint.h>
#include "ia64vmm/platform.h"

/* NULL when the tail is consistent, else the rule it breaks. Version 0 is
 * the default display path and carries no legacy field at all. */
static inline const char *legacy_handoff_check(const struct ia64vmm_handoff *h)
{
    const uint32_t known =
        IA64VMM_LEGACY_TEXT_CONSOLE | IA64VMM_LEGACY_IVT | IA64VMM_LEGACY_ROM | IA64VMM_LEGACY_STUB;
    uint32_t f = h->legacy_flags;
    if (!h->legacy_version)
        return f || h->legacy_ivt_base || h->legacy_ivt_size || h->legacy_rom_base ||
                       h->legacy_rom_size || h->legacy_stub_base || h->legacy_stub_size ||
                       h->legacy_bdf || h->legacy_reserved[0] || h->legacy_reserved[1] ||
                       h->legacy_reserved[2]
                   ? "legacy fields without legacy_version"
                   : NULL;
    if (h->legacy_version != IA64VMM_HANDOFF_LEGACY_VERSION)
        return "unknown legacy_version";
    if (f & ~known)
        return "unknown legacy_flags";
    if (!(f & IA64VMM_LEGACY_TEXT_CONSOLE))
        return "legacy state without the VGA text console";
    if (!(h->flags & IA64VMM_HF_LEGACY_VGA) || h->vga_bdf != h->legacy_bdf ||
        h->vga_base != IA64VMM_VGA_MEMORY_BASE || h->vga_size != IA64VMM_VGA_MEMORY_SIZE)
        return "VGA text console without the card's assigned legacy VGA";
    if (h->fb_format != IA64VMM_FB_NONE || h->fb_base || h->fb_size)
        return "a framebuffer under the VGA text console";
    if (f & IA64VMM_LEGACY_IVT ? h->legacy_ivt_base != IA64VMM_LEGACY_IVT_BASE ||
                                     h->legacy_ivt_size != IA64VMM_LEGACY_IVT_SIZE
                               : h->legacy_ivt_base || h->legacy_ivt_size)
        return "IVT/BDA location";
    if (f & IA64VMM_LEGACY_ROM
            ? h->legacy_rom_base != IA64VMM_LEGACY_ROM_BASE || !h->legacy_rom_size ||
                  h->legacy_rom_size % 512 || h->legacy_rom_size > IA64VMM_LEGACY_ROM_MAX
            : h->legacy_rom_base || h->legacy_rom_size)
        return "option ROM image location";
    if (f & IA64VMM_LEGACY_STUB ? h->legacy_stub_base != IA64VMM_LEGACY_STUB_BASE ||
                                      h->legacy_stub_size != IA64VMM_LEGACY_STUB_SIZE
                                : h->legacy_stub_base || h->legacy_stub_size)
        return "stub segment location";
    if (h->legacy_reserved[0] || h->legacy_reserved[1] || h->legacy_reserved[2])
        return "legacy reserved fields";
    /* Page 0 must be RAM the firmware keeps reserved for the IVT/BDA. */
    if (f & IA64VMM_LEGACY_IVT) {
        bool ram = false;
        for (uint32_t i = 0; i < h->ram_range_count && i < IA64VMM_HANDOFF_MAX_RAM; ++i)
            ram |= h->ram[i].base == 0 && h->ram[i].size >= 4096;
        if (!ram)
            return "IVT/BDA outside RAM page 0";
    }
    return NULL;
}
static inline bool legacy_handoff_text_console(const struct ia64vmm_handoff *h)
{
    return h->legacy_version == IA64VMM_HANDOFF_LEGACY_VERSION &&
           (h->legacy_flags & IA64VMM_LEGACY_TEXT_CONSOLE);
}

#endif
