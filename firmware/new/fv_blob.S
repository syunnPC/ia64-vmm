/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FW_FV: quoted path of the firmware volume (-DFW_FV). */
.section .rodata,"a"
.balign 4096
.global dxe_fv, dxe_fv_end
dxe_fv:
.incbin FW_FV
dxe_fv_end:
