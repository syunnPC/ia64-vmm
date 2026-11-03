<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Firmware

`firmware/new` implements the IA-64 firmware with native PAL/SAL, interrupt
handlers and an EDK2 DXE phase. It consumes the VMM handoff containing the
RAM map, vCPU count, assigned devices and display configuration.
The guest ITC runs at 400 MHz. Console diagnostics, reset, RTC and watchdog
services use the VMM service window. EFI variables persist when the VMM
boot medium supports persistent storage.

## Building

Run `make -j4 firmware` with GNU make, Python 3.11 or later, and an IA-64
GCC/binutils cross toolchain (`CROSS_IA64=ia64-linux-gnu-` by default).
The debug image is `build/firmware-new/ia64-firmware-new.elf`;
the release image is `build/firmware-new/release/ia64-firmware-new.elf`.
The release build disables EDK2 debug output and assertions.
`make esp` stages the debug image as `build/firmware.elf`.

## Source licences

EDK2 sources are pinned to vUDK2018 commit
`3e72ffe8afdd03f1f89eba65c921cbdcb004cfee` and retain BSD-2-Clause
notices. Generated BaseTools templates retain Intel's BSD-2-Clause notice.
Patches to EDK2 build copies carry GPL-2.0-or-later AND BSD-2-Clause.
Asteria-derived PAL/SAL, PCI and translation code retains Apache-2.0
attribution alongside GPL-3.0-or-later. FPSWA arithmetic includes
GPL-2.0-or-later code adapted from qemu-system-ia64 commit
`5f67e29d0fb6bea1f3bbae86d2af2674036f1b9c`.

The historical QEMU firmware implementation is not included. The ELF loader
requires the native PAL marker. Selected EDK2 sources and generated build
files are bundled; ordinary builds do not require BaseTools. To regenerate,
set `EDK2_IPF_DIR` to the pinned EDK2 checkout and `EDK2_PYTHON2` to Python
2.7, then run `python3 tools/firmware/regenerate_edk2.py`.
