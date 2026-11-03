/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The region registers and PTA of the SAL hand-off state (SAL 245359-005
 * §3.2.5), as the reference firmware establishes them
 * (firmware/firmware.c fw_prepare_sal_handoff_registers): region n gets
 * RID 0x1000+n, a 4 KiB page size and the VHPT walker off; PTA 0x3c (walker
 * off, 32 KiB). Reset leaves the RRs undefined (zero here, and page size 0
 * is unsupported). An Alternate TLB miss delivers ITIR.ps = RR[region].ps,
 * which must be valid for the handler's translation insert. Assembler
 * only; ra and rb are scratch. */
#ifndef IA64VMM_FIRMWARE_NEW_SAL_HANDOFF_H
#define IA64VMM_FIRMWARE_NEW_SAL_HANDOFF_H
.macro SAL_HANDOFF_RR va, value, ra, rb
    movl \ra = \va
    movl \rb = \value
    ;;
    mov rr[\ra] = \rb
    ;;
.endm
.macro SAL_HANDOFF_REGS ra, rb
    SAL_HANDOFF_RR 0x0000000000000000, 0x100030, \ra, \rb
    SAL_HANDOFF_RR 0x2000000000000000, 0x100130, \ra, \rb
    SAL_HANDOFF_RR 0x4000000000000000, 0x100230, \ra, \rb
    SAL_HANDOFF_RR 0x6000000000000000, 0x100330, \ra, \rb
    SAL_HANDOFF_RR 0x8000000000000000, 0x100430, \ra, \rb
    SAL_HANDOFF_RR 0xa000000000000000, 0x100530, \ra, \rb
    SAL_HANDOFF_RR 0xc000000000000000, 0x100630, \ra, \rb
    SAL_HANDOFF_RR 0xe000000000000000, 0x100730, \ra, \rb
    movl \ra = 0x3c
    ;;
    mov cr.pta = \ra
    ;;
    srlz.d
    ;;
    srlz.i
    ;;
.endm
#endif
