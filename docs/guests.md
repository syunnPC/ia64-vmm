<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Guest operating systems

Guests boot unmodified IA-64 installation media from assigned SATA (AHCI),
IDE, NVMe or USB devices. Use the native firmware (`--firmware new` in the
QEMU runner) for guest media.

## Status

"Not confirmed" means the stage has not been run to completion.

| Guest | Status | Notes |
| --- | --- | --- |
| Windows Server 2003 (build 3790 and later, Enterprise/Datacenter for Itanium) | Setup runs through text mode and continues into GUI-mode setup; a completed installation is not confirmed | Needs IDE: the setup has no AHCI driver. On QEMU use `--machine pc --cdrom-controller ide --disk-controller ide --memory 3G` |
| Windows Server 2008 for Itanium | The OS loader starts the kernel; setup is not confirmed | |
| Windows Server 2008 R2 for Itanium | Installation completes through both restarts to the desktop and normal shutdown | Setup needs a resolved AHCI INTx route; the VMM evaluates PCI configuration fields in the host's ACPI interrupt-link methods |
| Gentoo minimal installation CD (Linux 6.6) | Boots to the live system's console | Use at least 4 GiB of guest memory. Its kernel has no xHCI driver; USB devices need an EHCI controller |
| Debian 12 (debian-ports netinst) | The kernel and the installer start; loading kernel modules fails | Linux 6.3 moves CPU 0's per-CPU area without purging the TLB entry that maps it, so later writes go through a stale translation. This is a guest kernel defect; the VMM does not work around it |
| SUSE Linux Enterprise Server 11 SP4 | The installer kernel starts; installation is not confirmed | |
| FreeBSD 10, HP-UX 11i v3, OpenVMS I64, RHEL 5 | Firmware boot menus and loaders only; not tested further | |

Guests run much slower than on Itanium hardware; an installation can take
hours. Set `timeout_sec=0` (no limit) for long runs.

## Console and display

- The debug UART is off by default. `debug_serial=console` gives Linux and
  the firmware a serial console (it is listed in the firmware's console
  tables); Windows uses its Emergency Management Services only when its
  boot configuration enables them.
- With the GOP framebuffer the firmware and the OS loaders draw graphics.
  Windows boot applications draw graphics only at 1024x768 or 800x600; set
  the GOP mode with the boot loader (the USB boot kit uses 1024x768).
  Kernels without a framebuffer driver for the assigned GPU have no screen
  after ExitBootServices unless VGA text is available.
- Legacy VGA text works when the display adapter decodes legacy VGA and the
  path to it forwards VGA cycles. Linux uses it through the firmware's PCDP
  table (vgacon).
- `display=vbios` POSTs the card with its own legacy video BIOS before the
  guest starts, so that guests get VGA text and VBE graphics modes from the
  real card (configuration.md).

## Known limits

- An instruction or feature that is not implemented stops the vCPU with a
  message (exit code 2) instead of being skipped.
- `cpu_model=madison` supports native IA-32 execution with VMX or SVM.
  `br.ia` and `rfi` enter IA-32; JMPE returns to IA-64. Instruction, gate,
  lock, system-flag and software-interrupt events use the IA-64 interruption
  vectors. Descriptor queries, segment loads, privilege-changing far returns,
  real/VM86 execution, virtual interrupt flags, counters, ENTER and FP context
  transfers are supported. Device operands use checked accesses; separate
  instruction and data translations may map the same address differently.
  Execution requires code validation and uses one native instruction per
  entry. The native FP context covers x87, MMX and XMM registers; AVX guest
  state is not enabled. Recognized newer scalar and SSE instructions can
  execute on hosts that support them. Encodings without a complete access
  classification stop with a diagnostic.
- VT-d and AMD-Vi DMA protection have limited topology coverage. Interrupt
  remapping requires VT-d support (real-hardware.md).
- Defects of guest operating systems are not worked around; a guest that
  relies on behaviour the architecture does not guarantee can fail.
