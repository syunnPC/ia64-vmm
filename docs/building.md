<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Building

## Tools

| Tool | Used for |
| --- | --- |
| GNU make, GCC and binutils for AMD64 | The VMM (`build/vmm.elf`) and the Linux unit-test programs |
| IA-64 cross GCC and binutils (`ia64-linux-gnu-`) | IA-64 firmware, the EFI test applications and IA-64 test programs |
| Python 3.11 or later | Build helpers, image builders and test drivers (standard library only) |
| mtools, dosfstools | FAT images for the ESP and the USB boot kit |
| ACPICA `iasl` | AML routing tests (`make test`) |
| MinGW-w64 x86-64 GCC (`x86_64-w64-mingw32-`) | `vmmcfg.efi`, the option editor on the USB stick (also built by `make test`) |
| QEMU (x86-64) with OVMF, or VMware Workstation | Running the VMM in a virtual machine (running-qemu.md) |
| qemu-img (optional) | Building Hyper-V disks |
| clang-format 21 and Ruff | C and Python formatting; Ruff static checks |
| z3-solver in a Python venv (optional) | Re-proving the Tier1 instruction-selection rules |

Limine, GNU-EFI, selected EDK2 sources and generated EDK2 build files are
included in the tree. Normal firmware builds need no EDK2 BaseTools.

## Configuration

Machine-specific locations go into `local.env` at the repository root,
which git ignores:

```sh
cp local.env.example local.env
```

The file holds literal `KEY=value` lines without quotes, shell expansion or
inline comments. Environment variables override it; assignments on the
make command line override both. Tool names default to the commands on
`PATH`; give a full prefix such as `CROSS_IA64=/opt/cross/bin/ia64-linux-gnu-`
when needed. Run make and the tools from the repository root.

## Targets

| Command | Result |
| --- | --- |
| `make -j4 all` | `build/vmm.elf`, native firmware `build/firmware-new/ia64-firmware-new.elf` and EFI test applications `build/efi/*.efi` |
| `make -j4 firmware-new` | The native firmware: debug build `build/firmware-new/ia64-firmware-new.elf` (EDK2 DEBUG output and assertions) and release build `build/firmware-new/release/ia64-firmware-new.elf` |
| `make esp` | The QEMU boot image `build/esp.img` (Limine, VMM, firmware and boot module) |
| `make vmmcfg` | `build/vmmcfg/vmmcfg.efi` |
| `make usb-stick` | The USB boot kit (real-hardware.md) |
| `make hyperv-image` | Separate VMM and guest-test VHDX disks |
| `make check-freestanding` | Checks that the VMM objects use no hosted library symbols |
| `make format` / `make check-format` | Formats or checks project C with clang-format 21 and Python with Ruff; imported sources are excluded |
| `make lint` | Checks Python syntax and undefined names |
| `make clean` | Removes `build/` |

The VMM is freestanding C11. Host C code uses SSE4.2, POPCNT and AVX;
boot and interrupt paths use general registers only. Additional host
instructions are used after feature checks. See architecture.md.

## Build options

| Variable | Effect |
| --- | --- |
| `SAN=1` | Unit-test programs with AddressSanitizer and UndefinedBehaviorSanitizer |
| `TSAN=1` | Unit-test programs with ThreadSanitizer (the SMP and helper tests) |
| `TIER0_STATS=1`, `TIER1_STATS=1` | Time and count Tier0/Tier1 events; the counts appear in the VMM's statistics lines |
| `BOOTDISK_KIND=pe` or `fat` | `build/bootdisk.img` as the plain hello.efi image (default) or a FAT image |

Changing `SAN`, `TSAN` or a statistics flag rebuilds the affected objects.

## Regenerating the EDK2 build files

After changing selected EDK2 modules or build descriptions, run
`tools/firmware/regenerate_edk2.py` against the pinned checkout named by
`EDK2_IPF_DIR` in `local.env`. It needs Python 2.7 (`EDK2_PYTHON2`, or
`python2` on `PATH`). It updates `firmware/edk2/generated` and
`firmware/edk2/manifest.json`, preserving the hand-maintained `VmmFpswa`
recipe. See firmware.md for source provenance.
