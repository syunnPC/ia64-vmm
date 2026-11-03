/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Vol.1 §6.2.2.5, Vol.2 §10.3: IA-32 save/restore in native code. */
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    ssm psr.ic ;;
    srlz.d ;;
    movl r2=0x123456789abcdef ;;
    mov ar.fcr=r2 ;;
    mov ar.ssd=r2 ;;
    mov ar.fsr=r2 ;;
    mov ar.fir=r2 ;;
    mov ar.fdr=r2 ;;
    movl r2=0x000007ffe00501ff ;;
    mov ar.cflg=r2 ;;
    mov r8=ar.fcr ;;
    mov r9=ar.ssd ;;
    mov r10=ar.fsr ;;
    mov r11=ar.fir ;;
    mov r12=ar.fdr ;;
    mov r13=ar.cflg ;;
fuzz_checkpoint:
    break.i 0x1ffffe ;;
.data
    .zero 512
