<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Running in a virtual machine

`tools/qemu/run.sh` builds the VMM (unless `--no-build`), stages a private
boot image and starts it in QEMU or VMware Workstation. The VMM boots
through UEFI and Limine inside the AMD64 virtual machine.

```sh
tools/qemu/run.sh --cmdline 'exit=isa-debug-exit engine=tier0'
tools/qemu/run.sh --firmware new --cpus 4 --cmdline 'exit=isa-debug-exit engine=tier1 vcpus=2'
tools/qemu/run.sh --firmware new --machine pc --memory 4G --timeout 900 \
  --cdrom install-ia64-minimal.iso --cmdline 'engine=tier0 timeout_sec=800'
```

Without guest media the image carries `hello.efi` as a boot module; the
firmware runs it and the VMM shuts down with the application's status.

## Backends

`--backend` selects the backend; the default is `VMM_BACKEND` from
`local.env`, otherwise `win-qemu` when it is fully configured and WSL
interop works, otherwise `wsl-qemu`.

| Backend | Settings (`local.env`) | Machine |
| --- | --- | --- |
| `wsl-qemu` | `QEMU_X86`, `OVMF_CODE`, `OVMF_VARS` | Linux QEMU, q35, KVM when usable, otherwise TCG |
| `win-qemu` | `WIN_QEMU`, `WIN_QEMU_FW_CODE`, `WIN_QEMU_FW_VARS`, `WIN_STAGING_DIR` | Windows QEMU from WSL, q35, WHPX; falls back to TCG when WHPX fails to start |
| `vmware` | `VMWARE_DIR`, `WIN_STAGING_DIR` | VMware Workstation through `vmrun`, EFI, Secure Boot off, LSI SCSI boot disk, SATA guest media |

If guest disk I/O stalls for seconds under KVM, try a QEMU build configured
with `--disable-linux-io-uring` to check whether io_uring is involved.

Windows paths may be given as `C:\...` or `/mnt/c/...`. `WIN_STAGING_DIR`
must be on a Windows drive. Each run copies the boot image and the firmware
variable store into a private directory (below `build/runs/` on Linux,
below `WIN_STAGING_DIR` on Windows) and removes it afterwards unless
`--keep` is given.

## The VMM's boot disk and command line

The VMM boots from its own disk on a separate controller that the IA-64
guest never sees: NVMe at `00:03.0` on QEMU, LSI SCSI at `00:10.0` on
VMware. The runner adds `vmm_media=auto` on QEMU, which uses the UEFI boot
path, and `vmm_media=00:10.0` on VMware. An explicit `vmm_media=` token in
`--cmdline` overrides these defaults. `--cmdline` gives the VMM command line
(configuration.md); `VMM_CMDLINE` in `local.env` is
the default (`exit=isa-debug-exit reserve_mib=128`). `VMM_OPTIONS_FILE`
adds an options file to the image, as on a USB stick.

## Guest media and devices

| Option | Device |
| --- | --- |
| `--cdrom ISO`, `--disk IMG` | SATA CD-ROM and disk on the q35 AHCI controller (`00:1f.2`); `--cdrom-controller ide`, `--disk-controller ide` use the PIIX IDE controller with `--machine pc` |
| `--machine pc` | i440FX instead of q35 for this run (needed for IDE media) |
| `--usb-disk IMG`, `--usb-cdrom ISO` | USB mass storage on `--usb-controller xhci` (default) or `ehci` |
| `--usb-keyboard` | USB HID keyboard instead of PS/2 |
| `--nvme IMG` | NVMe disk |
| `--pcie-switch`, `--pcie-switch-depth N` | AHCI behind PCIe switches |
| `--iommu [intel\|amd]` | Expose VT-d (default) or AMD-Vi to exercise DMA and interrupt remapping; AMD-Vi requires QEMU |
| `--vga std` or `cirrus` | QEMU display device; `--vga-root-port` puts it behind its own root port |

Relative media paths are taken from
`IA64_IMAGES_DIR`. Disks are private copies, so the guest never changes the
original image; `--keep` keeps the copy in the run directory. Use the new
firmware (`--firmware new`) for USB, NVMe and IDE media.

## CPUs, memory and firmware

| Option | Effect |
| --- | --- |
| `--cpus N` | Host logical CPUs of the virtual machine, 2 to 256 (default 4). One is the VMM's management CPU; `vcpus=` and `placement=` decide how many guest vCPUs run on the rest |
| `--memory MiB` or `NG` | Virtual machine memory (default 2 GiB) |
| `--firmware new` | Native IA-64 firmware (default) |
| `--firmware-image FILE` | Another firmware ELF for this run |
| `--cpu-flags FLAGS` | QEMU `-cpu` feature suffix, e.g. `-svm` |
| `--tcg` | Use QEMU software emulation with `-cpu max` |
| `--vmware-vhv` | Offer nested AMD-V/VT-x to the VMM on VMware |

## Screens, keys and logs

| Option | Effect |
| --- | --- |
| `--log FILE` | Serial log of the VMM (copied on success, failure and timeout) |
| `--timeout SECONDS` | Watchdog for the virtual machine (default 60) |
| `--screenshot FILE.png` | Halt at the end (`exit=halt`) and save the screen |
| `--screenshot-on REGEX`, `--screenshot-after SECONDS`, `--screenshot-delay` | Capture while the guest keeps running |
| `--screenshot-text TEXT` | Retry the capture until the text is visible |
| `--screenshot-every SECONDS --screenshot-dir DIR` | Periodic captures |
| `--sendkey KEY` (repeatable), `--sendkey-after REGEX`, `--sendkey-delay`, `--sendkey-interval` | Type keys after a log line appears |
| `--control-file FILE` | QEMU: execute `key NAME`, `shot FILE.png` and `hmp COMMAND` lines appended to FILE while running |
| `--debug-serial off/device/console`, `--debug-screen 0/1` | Override the VMM's debug policy (configuration.md) |
| `--gdb` | QEMU: wait for GDB on port 1234; VMware: debug stub on port 8864 |

With a debug UART (`debug_serial=device` or `console`) the guest's serial
output is written to `<log>.guest-serial.log`.

## Exit status

The runner returns the VMM's exit code: 0 when the guest shut down with
EFI_SUCCESS, 1 for a guest failure or fatal error, 2 for an unimplemented
instruction or feature, 3 for a bad guest access, 4 for a setup error and 5
when the VMM's `timeout_sec` expired. The runner itself returns 5 when its
watchdog expires, 1 when it or the virtual machine failed, and 130 when
interrupted. With `exit=isa-debug-exit` QEMU's exit status encodes the VMM
exit code as `code * 2 + 1`; VMware has no such device, so the runner uses
`exit=halt` there and reads the code from the log.
