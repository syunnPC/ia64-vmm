/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The native PAL block, PALE_INIT/PALE_CHECK and SALE_ENTRY for
 * tests/firmware/pal-events.c, in one segment at the firmware's layout. */
ENTRY(pal_proc_entry)
PHDRS { image PT_LOAD FLAGS(7); }
SECTIONS {
    . = 0x100000;
    .pal : { KEEP(*(.pal)) } :image
    .text : { *(.text*) } :image
    .data : ALIGN(16) { *(.data*) *(.sdata*) } :image
    .bss : ALIGN(16) { *(.bss*) *(.sbss*) *(COMMON) } :image
    /DISCARD/ : { *(.comment) *(.note*) *(.IA_64.unwind*) }
}
