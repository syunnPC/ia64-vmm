/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .rodata,"a"
.balign 8
.global gate_hello_pe, gate_hello_pe_end
gate_hello_pe:
.incbin "build/firmware-new/GateHello.efi"
gate_hello_pe_end:
