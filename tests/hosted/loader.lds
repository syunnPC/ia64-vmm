/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(_start)
PHDRS { firmware PT_LOAD FLAGS(7); }
SECTIONS {
  . = 0x100000;
  .text : { *(.text) } :firmware
  .data : { *(.data) } :firmware
  .opd : { *(.opd) } :firmware
  .got : { *(.got) } :firmware
  __firmware_payload_end = .;
  /DISCARD/ : { *(.IA_64.unwind*) *(.comment) *(.note*) }
}
