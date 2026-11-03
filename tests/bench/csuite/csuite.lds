/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Flat image of the compiled-C suite: start.S (entry + IVT, 64 KiB) at
 * the link address, then text, read-only data, small data and data; gp
 * addressing reaches all of it (-mauto-pic: gprel22, +-2 MiB). */
OUTPUT_FORMAT("elf64-ia64-little")
OUTPUT_ARCH(ia64)
ENTRY(_start)
SECTIONS
{
  . = CSUITE_BASE;
  .text : {
    *start.o(.text)
    *(.text .text.*)
  }
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
  /* Writable data on its own pages, as an ELF executable's RW segment:
   * stores never hit a page that holds code (which would invalidate its
   * translations in every engine). */
  . = ALIGN(0x10000);
  .data : { *(.data .data.* .sdata .sdata.*) }
  __gp = ALIGN(16) + 0x100000;
  gp_offset = __gp - _start;
  .bss : { *(.sbss .sbss.* .scommon .bss .bss.* COMMON) }
  _end = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.IA_64.unwind*) *(.IA_64.unwind_info*) }
}
