/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Vol.2 §10.3.2 Table 10-3: at PL0 an Itanium write of ar.eflag
 * stores every bit, including ignored and reserved fields (the permitted
 * "write the value and return the last value written" option). Windows
 * Server 2003 saves/restores it in its trap frame. */
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    ssm psr.ic ;;
    srlz.d ;;
    movl r2=0xfedcba9876543fff ;;
    mov ar.eflag=r2 ;;
    mov r8=ar.eflag ;;
    movl r2=0x0000000000273202 ;;
    mov ar.eflag=r2 ;;
    mov r9=ar.eflag ;;
    mov ar.eflag=r0 ;;
    mov r10=ar.eflag ;;
fuzz_checkpoint:
    break.i 0x1ffffe ;;
.data
    .zero 512
