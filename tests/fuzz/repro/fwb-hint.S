/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Vol.3 fwb (M24): a store-buffer flush hint with no interruptions
 * and no register effects; Windows Server 2003 ntoskrnl executes it. */
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    mov r5=7 ;;
    fwb ;;
    mov r6=8 ;;
(p0) fwb ;;
fuzz_checkpoint:
    break.i 0x1ffffe ;;
.data
    .zero 512
