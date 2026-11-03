/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Ia64/Uefi/Application.lds
 */
/* Keeping .opd and .got separate lets the converter audit linker-created cells.
 */
ENTRY(__entry_descriptor)
PHDRS { image PT_LOAD FLAGS(7); }
SECTIONS
{
    __image_base = DEFINED(__configured_image_base) ? __configured_image_base : 0x04000000;
    . = __image_base + 0x1000;
    .text : { *(.text*) } :image
    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) *(.data.rel.ro*) } :image
    . = ALIGN(0x1000);
    .opd : {
        __entry_descriptor = .;
        QUAD(_ModuleEntryPoint) QUAD(__gp)
        *(.opd)
    } :image
    . = ALIGN(0x1000);
    .got : { *(.got*) } :image
    __gp = ADDR(.got);
    . = ALIGN(0x1000);
    .data : { *(.data*) *(.sdata*) *(.ctors*) } :image
    . = ALIGN(0x1000);
    .bss : { *(.bss*) *(.sbss*) *(COMMON) } :image
    /DISCARD/ : { *(.comment) *(.note*) *(.IA_64.unwind*) }
}
