/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sal-handoff.h"
.text
.align 16
.global _start
.proc _start
_start:
    /* Every CPU: the SAL hand-off RRs and PTA (r8-r10 carry the hand-off). */
    SAL_HANDOFF_REGS r2, r3
    /* Only LID 0 runs the IPL. An AP branches before bsw, alloc or any
     * memory access. */
    mov r14 = cr.lid
    ;;
    cmp.ne p6,p0 = r14,r0
    ;;
(p6) br.cond.spnt.many ap_start
    ;;
    bsw.1
    ;;
    /* SDM Vol.2 6.5: BSPSTORE/RNAT access requires enforced lazy mode.
     * UDK2018 InternalLongJump reads BSPSTORE before changing RSC. */
    mov ar.rsc = 0
    ;;
    alloc r2 = ar.pfs, 0, 0, 3, 0
    mov out0 = r8
    mov out1 = r9
    mov out2 = r10
    ;;
    br.call.sptk.many b0 = ipl_main
    ;;
    br.sptk.many ipl_fatal
.endp _start

.proc ipl_fatal
ipl_fatal:
    /* Service ABI, include/ia64vmm/platform.h (physical mode). */
    movl r14 = 0x0003fffff8000040
    movl r15 = 0x47314956
    ;;
    st8 [r14] = r15
    ;;
    br.sptk.many ipl_fatal
.endp ipl_fatal
