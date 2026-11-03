/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Regression: legal PSR.db with reset (disabled) IBR/DBRs.
 * Both the VMM and fork continue to the checkpoint. */
.text
.global _start
_start:
    mov r2 = psr ;;
    movl r3 = 1 << 24 ;;
    or r2 = r2, r3 ;;
    mov psr.l = r2 ;;
    srlz.i ;;
fuzz_checkpoint:
    break.i 0x1ffffe ;;
.data
    .zero 512
