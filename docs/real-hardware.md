<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Running on a physical machine

The VMM boots from a USB stick (or an NVMe disk) through UEFI and takes over
the whole machine. The IA-64 guest gets the machine's PCI devices and boots
from its own disks or optical drives.

## Safety

- Every PCI device except the controller of the VMM's own medium belongs to
  the guest. The guest firmware and operating system can read and write
  every disk in the machine. Use a machine whose disks hold nothing of
  value, or disconnect them.
- Supported Intel VT-d and AMD-Vi configurations confine guest memory DMA
  to guest RAM and required firmware-reserved ranges. Machines without an
  IOMMU, or with `iommu=off`, run with unrestricted identity DMA. Interrupt isolation requires
  supported VT-d or AMD-Vi remapping. DMA before protection is enabled and root-complex
  peer routing remain unprotected; this is not a complete device-isolation boundary.
- Booting the VMM needs Secure Boot off. If the machine uses disk
  encryption, have its recovery key before changing Secure Boot.
- A guest reset restarts the guest inside the VMM; it never resets the
  machine. At the end of a run the VMM powers the machine off through ACPI,
  or halts with its summary on the screen when the entry has `exit=halt`.

## Requirements

- An AMD64 CPU with SSE4.2, POPCNT, AVX, XSAVE and an invariant TSC.
  One CPU manages the VMM and each guest vCPU runs on a CPU of its own;
  the default placement keeps them on separate cores.
- UEFI boot with Secure Boot disabled (Limine is not signed).
- A controller for the VMM's medium that the guest does not need: an xHCI
  or EHCI USB controller (one USB port is enough, but every high-speed
  device on that controller is then the VMM's), an NVMe controller, or an
  AHCI controller with exactly one ATA disk. Persistent logs and EFI
  variables need one of these.
- A display the guest can use: the UEFI GOP framebuffer, or legacy VGA.
  `display=vbios` (the VMM runs the card's legacy video BIOS) needs the
  motherboard's CSM enabled, so that the card is initialized by its legacy
  BIOS and its frame buffer lies below 4 GiB, and AMD-V or VT-x enabled in
  the firmware setup.

## Preparing a USB stick

1. Copy `tools/usb-stick/local.env.example` to `usb-stick.local.env` at the
   repository root (or anywhere; `USB_STICK_CONFIG` in `local.env` names
   it). The menu is fixed; this file controls the optional settings seed
   and EFI variable slot count.
2. Set `USB_STICK_DIR` and, for a disk image, `USB_STICK_IMAGE` in `local.env`.
3. `make -j4 usb-stick` builds the VMM, native firmware, UEFI boot shim,
   `hello.efi` and `vmmcfg.efi` (needs `MINGW_X64`), then writes the ESP tree and, if set,
   a raw MBR disk image with one FAT32 EFI system partition. It never writes
   to a device.
4. Write the image to the stick with a raw image writer (`dd` on Linux;
   Rufus in DD mode on Windows). This erases the stick.

The generated tree can also be copied to an existing MBR FAT32 EFI system
partition (type `0xEF`).

`EFI/BOOT/BOOTX64.EFI` records the boot medium's PCI controller from UEFI,
then starts Limine at `EFI/vmm/limine.efi`. Keep both files when copying
the tree.

The same image works on any supported medium at least as large as the image;
extra space remains unallocated. No target capacity is needed. Before saving,
the VMM matches the boot-loaded manifest to the file on the reserved medium
and validates the FAT32 file chains. Log and EFI variable files may be fragmented.

### Boot menu

| Entry | Behavior |
| --- | --- |
| Configure VMM Options | Edit saved settings on the stick |
| Boot from guest media (using user config) | Native firmware with saved VMM settings |
| Boot from guest media (safe mode - default settings) | Native firmware with VMM defaults; retain saved media selectors |
| Diagnostics | Discovery, basic checks and individual tests |

Both guest entries open the firmware boot menu at 1024x768. Safe mode uses
the interpreter, one vCPU and GOP display; it ignores saved tuning and
self-test options. With the default `vmm_media=auto`, both entries reserve
the EHCI, xHCI, NVMe or AHCI controller used for the UEFI boot.

Diagnostics ignores saved settings except `vmm_media` and `guest_media`; Discovery ignores
the file entirely. Basic Tests runs memory, timers, SIMD/native x86 state
and host memory-mapping checks, then halts. IPI and idle checks run
automatically. Basic Tests and host memory-mapping tests need at least
three logical CPUs and reserve at least 520 MiB. Native x86 checks report
a skip when hardware virtualization is unavailable.

Individual tests cover those domains plus guest firmware startup.
Persistent Storage checks firmware startup with persistent logging enabled.
Fault Handling contains deliberate exception tests that must stop the VMM;
these are excluded from Basic Tests.

`USB_STICK_CONFIG` accepts only these literal `KEY=value` settings:

| Key | Meaning |
| --- | --- |
| `VMM_OPTIONS_FILE` | Path of a seed for `vmm-options.conf`, relative to the repository root or absolute |
| `NVRAM_PAIRS` | EFI variable slot pairs per firmware, one per machine the stick serves (1 to 32, default 8) |

Set runtime options, including `debug_serial`, in `vmm-options.conf`.

## Selecting the VMM controller

Automatic selection uses the UEFI boot path, so moving the medium to
another supported controller needs no saved configuration. The VMM reserves
the entire controller, including all its ports, disks and USB devices.
Keep guest media on a different controller.
If the boot controller cannot be identified or is unsupported, automatic
selection stops with an error before guest startup.

For an explicit override, choose Diagnostics → Discovery. The VMM lists
storage and USB controllers with their bus/device/function numbers, IDs
and bridges, then halts before it touches any device. Restart, open
Configure VMM Options, and add `vmm_media=BB:DD.F` to "extra tokens", then
save and reset. Discovery always lists devices even when an override is saved.
Invalid or ambiguous selections stop boot.

On Windows, `tools/find-vmm-controller.ps1 -DriveLetter E:` (or
`-DiskNumber N`, `-ListUsb`) follows a disk to its controller and prints a
candidate number. The VMM's own list is authoritative, because firmware can
number buses differently.

## Assigning devices

The guest owns everything except the VMM's controller and the bridges above
it: disks, optical drives, USB controllers with their keyboards, the display
and the legacy keyboard controller. Boot guest media with `bootmenu=1`, so
the firmware's boot menu offers every bootable device, and attach the guest's
media to a different controller from the stick.

DMA protection starts after memory reclaim and PCI inventory, before the
VMM media driver starts. Guest DMA maps the same page-aligned usable RAM as
the guest CPU; the trusted VMM controller has a separate identity domain
covering VMM memory. Unknown requesters have no mapping. Firmware RMRRs
must lie entirely in reserved memory and resolve to one owner; conflicting
or ambiguous scopes stop boot. These mappings persist across guest resets.

`iommu=auto` (default) uses the available remapper; no remapper is required.
Choose **IOMMU DMA protection → Disabled** (`iommu=off`) in Configure VMM
Options to run without DMA or interrupt isolation. This skips the protection
topology checks, including ACS requirements. Firmware remapping must still
be stopped before devices use identity DMA; if takeover fails, disable
VT-d/AMD-Vi in UEFI setup. Safe mode uses the default `iommu=auto`.

VT-d supports segment zero and legacy translation with register invalidation
(hardware major versions 1–5). Active firmware queued invalidation or interrupt
remapping and namespace DMA stop boot. AMD-Vi supports segment zero, legacy
unsegmented device tables, three/four-level host page tables, and hardware with
EFR and global invalidation support. IVHD select/range, alias and special-device
entries are validated; unknown sources retain valid deny entries. Alternate
IVHD encodings must agree. IVMD unity and permission constraints may add only
reserved or same-owner memory; exclusion bypasses, conflicting constraints,
HID device entries and active optional routing/virtualization engines stop boot.
AMD-Vi enables and locks a disabled firmware-declared PCI register aperture,
then uses private command/event rings with completion-store synchronization;
event interrupts remain disabled.

VT-d uses private interrupt tables and queued invalidation when every unit and
the firmware permit interrupt remapping. MSI/MSI-X and IOAPIC routes validate
their requester IDs. Route changes drain old deliveries before reusing host
vectors. AMD-Vi uses a private interrupt table per requester and the IVHD
special-device IOAPIC identities. It supports basic IRTEs and, when every unit
supports it, the 128-bit x2APIC format. Shared IVHD requester aliases cannot
authenticate individual MSI sources and are refused. Unknown sources and
unsupported interrupt types are blocked after activation; SMI handling remains
under the firmware's SMI filter policy. Cache invalidation and completion drain
old writes before host-vector retirement. If interrupt remapping is unavailable
for the host APIC mode, either backend retains direct routing with a warning.
After activation, unsupported routes are refused and hardware errors stop boot
or execution.

Both backends refuse enabled ATS/PRI/PASID or phantom functions, enabled SR-IOV,
and conventional bridge requester aliases. ARI forwarding retains the same
requester IDs; all functions on its secondary bus form one multifunction group. Shared
multifunction devices and downstream hierarchies require ACS isolation.
Supported ACS controls are enabled and read back. Mixed DMA ownership requires
ACS on populated root ports; shared hierarchies require isolated routing paths
to the root. Bridge configuration ownership does not
determine downstream DMA ownership. ACS settings are preserved across resets.
Unverified requester aliases remain rejected. Peer routing outside these ACS
paths and firmware DMA during takeover remain outside this protection.

## Configuring options on the stick

The menu entry "Configure VMM Options" starts `vmmcfg.efi`, which edits
`\vmm-options.conf` on the stick: Up/Down select a setting, Left/Right or
Enter change its value, Enter on "extra tokens" types other `key=value`
tokens, `S` saves, `R` saves and resets, `X` resets without saving. File
settings apply to the user-config boot entry; safe mode and diagnostics
read only media selectors (configuration.md).
The editor shows concrete normal-boot defaults for absent settings and saves
every displayed value. Boolean choices are shown as Enabled or Disabled.
"Guest vCPU count" sets `vcpus` (default 1); "Host CPU placement" sets
`placement`. Changing placement keeps the guest count unchanged. Logical-thread
placement can use at most the host's logical CPU count minus one, because CPU 0
runs management. Set both rows, save and reset, then select the user-config boot
entry. Existing custom counts in the options file are retained until changed.
The FP rows include a separate experimental switch, off by default;
FP execution explains its incomplete proof boundary.
The file may also be edited by hand (one token per line, `#` comments).
Rebuilding the stick replaces it with the seed or an empty default.

## Logs and EFI variables

The VMM keeps its log in RAM and shows a summary on the screen at the end
of a run (`pause_sec=` holds each page). With `media_log=1` it also writes
the log to two preallocated 2 MiB files on the stick every 30 seconds and at
the end of the run. After a VMM failure and a warm reset, the next boot first saves
the previous run's log, if it survived in RAM, in a separate crash slot. Read them on another machine without mounting
the stick:

```sh
python3 tools/usb-stick/read_logs.py /dev/sdX          # or the image file
python3 tools/usb-stick/read_logs.py --crash /dev/sdX
```

EFI variables (boot options and settings the guest firmware or OS stores)
are saved on the stick once when the guest run ends and loaded at the next
VMM boot. The VMM recognizes the machine by its SMBIOS system identity, so
one stick serves several machines. `nvram_slots=off` ignores the saved
variables for one run; `nvram_slots=fresh` starts with empty variables and
replaces the saved ones at the end.

Both use the controller of the VMM's medium:

- xHCI: one USB mass-storage stick on a root port, with 512-byte sectors.
- EHCI: one high-speed stick on a root port or behind one high-speed hub
  on that port, with 512-byte sectors. The hub may have at most 15 ports;
  a second hub level or another high-speed device is refused. Devices
  behind the hub remain on the VMM's controller. On controllers with
  companions, low-speed devices on other root ports go to the companions;
  full-speed devices go only after the medium works. A stick that links
  at full speed is refused and kept on the EHCI controller. The guest's
  keyboard needs another controller when no companion is available.
- NVMe: namespace 1 with 512-byte sectors.
- AHCI: exactly one port with an ATA disk (no ATAPI, no port multiplier)
  that supports 48-bit addresses and FLUSH CACHE EXT. Write the image to
  that disk instead of a stick.

A medium that cannot guarantee written data reaches it, or a
write-protected one, is refused before the first write. Without 64-bit DMA
(xHCI, EHCI, AHCI) the VMM reservation must lie below 4 GiB.
