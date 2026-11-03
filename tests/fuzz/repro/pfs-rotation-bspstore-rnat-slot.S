/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Legal PFS rotation beyond SOL, and an RNAT-slot BSPSTORE. */
.text
.global _start
_start:
    mov ar.rsc=0 ;;
    movl r9=0x4010 ;;
    mov ar.pfs=r9 ;;
    mov r8=ar.pfs ;;
    movl r7=0x6e001f8 ;;
    mov ar.bspstore=r7 ;;
    mov r6=ar.bsp ;;
fuzz_checkpoint:
    break.i 0x1ffffe ;;
.data
    .zero 512
