# ia64-vmm

**ia64-vmm** is an experimental Type-1 hypervisor that runs IA-64 (Itanium) guest operating systems on AMD64 hosts, powered by original instruction set translators.

## System Requirements

- **CPU:** Intel Sandy Bridge or AMD Zen 2 (or newer) with hardware virtualization (Intel VT-x / AMD-V) enabled in the firmware.
  - *Recommended:* 4 or more physical cores.
  - *Optional:* Intel VT-d / AMD-Vi for IOMMU support (currently experimental).
- **Memory:** Sufficient RAM for both the guest OS and the VMM's reserved memory.
  - *Recommended:* Guest memory allocation + 1–2 GiB.
- **GPU:** A graphics adapter with Legacy BIOS / CSM support.
- **Boot Storage:** A dedicated EHCI/xHCI (USB), NVMe, or AHCI controller for the VMM boot medium.
  - *Note:* The controller hosting the VMM boot device is hidden from the guest, so guest boot media must be attached to a separate storage controller.
- **System Firmware:** UEFI with Secure Boot disabled. CSM may also need to be enabled depending on your system's graphics initialization requirements.

## Known Limitations

As this is an experimental project, please be aware of the following limitations:

- **Performance:** Emulation and translation overhead currently result in very low performance.
- **Guest Compatibility:** Some guest operating systems may not work properly depending on the host configuration.
- **Host Compatibility:** This program is not tested enough with real machines.
- **Nested Virtualization:** Running inside other hypervisors (such as VMware Workstation or VirtualBox) is generally unsupported and incompatible. Hyper-V support is in progress with GOP output only.
- **IA-32 Execution:** Currently not implemented.

## License & Disclaimer

This program is licensed under the **GPL-3.0-or-later**. Individual files and third-party components retain their respective licenses. Please refer to `NOTICE` and the `LICENSES/` directory for full attribution and license texts.

This experimental software is provided "as is", without warranty of any kind.

Itanium is a trademark of Intel Corporation or its subsidiaries in the United Stated and other countries.