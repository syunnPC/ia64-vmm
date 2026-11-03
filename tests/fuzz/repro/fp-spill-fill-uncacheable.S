/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Vol.2 §4.4.9: of the FP references only ldfe/stfe may be
 * unsupported on non-WB pages; stf.spill, ldf.fill and ldfp to an
 * uncacheable page (physical addressing, bit 63 = UC) are ordinary
 * references. Windows Server 2003 zeroes non-cached memory with stf.spill. */
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    movl r3=0x8000000000000001 ;;
    setf.sig f6=r3 ;;
    movl r4=0x8000000000110040 ;;
    stf.spill [r4]=f6 ;;
    ldf.fill f7=[r4] ;;
    movl r5=0x110040 ;;
    ld8 r8=[r5],8 ;;
    ld8 r9=[r5] ;;
    ldfp8 f8,f9=[r4] ;;
    adds r6=16,r4 ;;
    stf.spill [r6]=f8 ;;
    movl r5=0x110050 ;;
    ld8 r10=[r5],8 ;;
    ld8 r11=[r5] ;;
    getf.sig r12=f7 ;;
    getf.exp r13=f7 ;;
    getf.sig r14=f9 ;;
fuzz_checkpoint:
    break.i 0x1ffffe ;;
.data
    .zero 512
