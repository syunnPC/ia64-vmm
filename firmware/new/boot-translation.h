/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Ia64/Memory/InitialTranslationInstaller.S
 * (ITR insertion and serialization sequence).
 */
#ifndef VMM_BOOT_TRANSLATION_H
#define VMM_BOOT_TRANSLATION_H
#include <stdbool.h>
#include <stdint.h>

struct ipl_boot_translation {
    uint64_t address, itir;
};

/* SAL 245359-005 §§3.2.5, 3.3.1–2: only ITR(0) belongs to firmware.
 * Cover the relocated PAL and native handler code with one identity page.
 * Limit the page to 1 MiB; the ELF load alignment and code-span assertions
 * keep it inside the image's RAM allocation (SDM Vol.2 §4.4 attributes).
 */
static inline bool ipl_boot_translation(uint64_t first, uint64_t end,
                                        struct ipl_boot_translation *tr)
{
    if (!first || end <= first)
        return false;
    for (unsigned ps = 12; ps <= 20; ps += 2) {
        uint64_t mask = (UINT64_C(1) << ps) - 1;
        if ((first & ~mask) == ((end - 1) & ~mask)) {
            *tr = (struct ipl_boot_translation){first & ~mask, ps << 2};
            return true;
        }
    }
    return false;
}

static inline bool ipl_install_boot_translation(struct ipl_boot_translation *tr)
{
    uint64_t first, end, psr;
    __asm__ volatile("movl %0=__firmware_native_pal;; movl %1=__firmware_text_end;;"
                     : "=r"(first), "=r"(end));
    if (!ipl_boot_translation(first, end, tr))
        return false;
    uint64_t pte = tr->address | UINT64_C(0x661); /* present, WB, A/D, PL0 RWX */
    /* SDM Vol.3 itr: collection must be off. Vol.2 Table 4-1 requires a
     * purge even when replacing the same TR slot. Restore the caller's
     * mask; leave PSR.it/dt/rt and the RRs alone. */
    __asm__ volatile("mov %0=psr;; rsm 0x6000;; srlz.d;;"
                     "ptr.i %1,%2;; srlz.i;;"
                     "mov cr.ifa=%1; mov cr.itir=%2;;"
                     "itr.i itr[r0]=%3;; srlz.i;;"
                     "mov psr.l=%0;; srlz.d;;"
                     : "=&r"(psr)
                     : "r"(tr->address), "r"(tr->itir), "r"(pte)
                     : "memory");
    return true;
}
#endif
