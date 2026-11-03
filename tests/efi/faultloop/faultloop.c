/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A guest whose entry and IVA are both unmapped. The
 * application rfi's into an unmapped region-2 address with PSR.it = 1 and
 * cr.iva in another unmapped page: the Alternate ITLB fault is delivered to
 * IVA + 0x0c00 with PSR.it kept (SDM Vol.2 §5.5) and faults again, forever,
 * without retiring an instruction. tests/qemu/apps.py expects the VMM to
 * stop the vCPU with a diagnostic exit and a state dump.
 */
#include <efi.h>
#include "../../../include/ia64vmm/platform.h"
static void say(const char *s)
{
    while (*s)
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_CONSOLE) = (unsigned char)*s++;
}
EFI_STATUS EFIAPI _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    (void)image;
    (void)st;
    say("fault loop: rfi to an unmapped entry with an unmapped IVA\n");
    UINT64 iva = 0x4000000056780000ULL, entry = 0x4000000012340000ULL;
    UINT64 ipsr = (1ULL << 44) | (1ULL << 36) | (1ULL << 13); /* bn, it, ic; cpl 0 */
    __asm__ volatile(
        "rsm 0x6000 ;;\n" /* psr.i, psr.ic: the interruption registers become writable */
        "srlz.d ;;\n"
        "mov cr.iva = %0\n"
        "mov cr.iip = %1\n"
        "mov cr.ipsr = %2\n"
        "mov cr.ifs = r0 ;;\n"
        "srlz.d ;;\n"
        "rfi ;;\n" ::"r"(iva),
        "r"(entry), "r"(ipsr)
        : "memory");
    say("fault loop: rfi returned\n");
    return EFI_ABORTED;
}
