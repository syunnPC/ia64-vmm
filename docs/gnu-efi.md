<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# GNU-EFI sources

`third_party/gnu-efi` contains IA-64 startup code (`crt0-efi-ia64.S`),
relocation code (`reloc_ia64.S`), the linker script (`elf_ia64_efi.lds`)
and EFI headers. The recorded upstream revision is
`f56c54402de23349bd658dc91eb508d7732a9970` from ncroxon's GNU-EFI repository.

The IA-64 startup and relocation files retain Hewlett-Packard's 1999
BSD-3-Clause notice. EFI Toolkit headers retain Intel's permissive notice;
individual headers also include MIT, BSD-2-Clause-Patent and GPL alternatives.
`inc/subst/elf.h` carries LGPL-2.1-or-later. Full upstream notices are retained
in `third_party/gnu-efi/LICENSE.sources`, `LICENSE.efilib`,
`LICENSE.BSD-3-Clause`, `licenses/`, and each source file.

Unused BSD-4-Clause headers `efidbg.h` and `ia64/eficontext.h` are omitted.
IA-64 test applications use the bundled startup, relocator and headers;
`make efi` requires the IA-64 GCC/binutils toolchain.
