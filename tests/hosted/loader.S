/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.align 16
.global _start, pal_proc_entry, pale_init, pale_check
_start:
pale_init:
    movl r14 = pointer
    ;;
    br.sptk.many pal_proc_entry
    ;;
pal_proc_entry:
pale_check:
    .global __firmware_native_pal
__firmware_native_pal:
    break.m 0x100000
    br.many b0
    ;;
.section .data,"aw",@progbits
.align 8
.global __gp
__gp:
pointer:
    data8 _start
.section .opd,"aw",@progbits
.align 16
descriptor:
    data8 _start, __gp
.section .got,"aw",@progbits
    data8 descriptor
