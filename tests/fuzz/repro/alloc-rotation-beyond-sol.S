/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A rotating region may extend beyond SOL into the output registers. */
.text
.global _start
_start:
    alloc r8=ar.pfs,0,0,16,8 ;;
    .align 16
    /* Restore an empty frame before the differential end trap. */
    alloc r9=ar.pfs,0,0,0,0 ;;
    .align 16
fuzz_checkpoint:
    break.m 0x1ffffe ;;
.data
    .zero 512
