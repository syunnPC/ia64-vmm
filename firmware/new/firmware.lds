/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(_start)
PHDRS { image PT_LOAD FLAGS(7); }
SECTIONS
{
    . = 0x100000;
    .pal : ALIGN(0x100000) { KEEP(*(.pal)) } :image
    ASSERT(SIZEOF(.pal) == 32768, "PAL copy size mismatch")
    .text : { *(.text*) } :image
    __firmware_text_end = .;
    ASSERT(__firmware_native_pal == ADDR(.pal), "PAL must start the firmware code page")
    ASSERT(__firmware_text_end - __firmware_native_pal <= 0x100000,
           "PAL and native handlers exceed a 1 MiB ITR")
    ASSERT(ivt_base >= __firmware_native_pal && ivt_base + 0x8000 <= __firmware_text_end,
           "IVT must be inside the firmware code span")
    .rodata : ALIGN(4096) { *(.rodata*) } :image
    .opd : ALIGN(16) { *(.opd) } :image
    .got : ALIGN(16) { __gp = .; *(.got*) } :image
    .data : ALIGN(16) { *(.data*) *(.sdata*) } :image
    .bss : ALIGN(16) { *(.bss*) *(.sbss*) *(COMMON) } :image
    /* IPF runtime descriptors require 8 KiB boundaries (UEFI 2.7 §7.2.3). */
    . = ALIGN(8192);
    .tail : { QUAD(0) . = ALIGN(8192); } :image
    __firmware_payload_end = .;
    ASSERT(__firmware_payload_end >= __firmware_native_pal + 0x100000,
           "firmware allocation must contain the code page")
    /DISCARD/ : { *(.comment) *(.note*) *(.IA_64.unwind*) }
    ASSERT(__firmware_payload_end - 0x100000 < 0x2000000, "firmware image exceeds 32 MiB")
}
