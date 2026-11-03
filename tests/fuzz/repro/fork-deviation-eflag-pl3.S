/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fork deviation: SDM Vol.2 Table 10-3, at PL3 (> EFLAG.iopl=0) Itanium
 * writes of IOPL, VM and IF are silently ignored; the fork stores them. */
.text
.global _start
_start:
    movl r2=0x118000 ;;
    mov cr.iva=r2 ;;
    srlz.i ;;
    mov ar.eflag=r0 ;;
    rsm psr.ic ;;
    srlz.d ;;
    movl r2=0x100200 ;;
    mov cr.iip=r2 ;;
    movl r2=(1 << 13) | (3 << 32) ;;
    mov cr.ipsr=r2 ;;
    mov r2=0 ;;
    mov cr.ifs=r2 ;;
    rfi ;;
.org 0x200
    movl r2=0x0000000000023201 ;;
    mov ar.eflag=r2 ;;
    mov r8=ar.eflag ;;
fuzz_checkpoint:
    break.i 0x1ffffe ;;
.data
    .zero 512
