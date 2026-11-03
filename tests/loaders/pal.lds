/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(pal_proc_entry)
PHDRS { image PT_LOAD FLAGS(7); }
SECTIONS {
    . = 0x100000;
    .pal : { *(.pal) } :image
    /DISCARD/ : { *(.comment) *(.note*) *(.IA_64.unwind*) }
}
