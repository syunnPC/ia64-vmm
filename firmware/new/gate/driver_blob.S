/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .rodata,"a"
.balign 8
.global gate_driver_pe, gate_driver_pe_end
gate_driver_pe:
.incbin "build/firmware-new/GateDriver.efi"
gate_driver_pe_end:
