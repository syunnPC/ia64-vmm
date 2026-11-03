# SPDX-License-Identifier: GPL-2.0-or-later
"""Run a private EFI VM with a reserved SCSI ESP and assigned SATA media."""

import re
import shutil
import subprocess
import sys
import time

from qemu.config import required
from qemu.windows import (
    copy_writable,
    kill_windows_processes,
    local_path,
    run_directory,
    windows_path,
)


def create_vm(
    root,
    temp,
    gdb,
    cdrom=None,
    guest_disk=None,
    memory=2048,
    esp_image=None,
    cpus=4,
    pcie_switch=False,
    iommu=False,
    nvme=None,
    cdrom_controller="sata",
    nested_virtualization=False,
):
    disk = temp / "esp-flat.img"
    copy_writable(esp_image or root / "build/esp.img", disk)
    sectors = disk.stat().st_size // 512
    (temp / "esp.vmdk").write_text(
        "# Disk DescriptorFile\nversion=1\nCID=fffffffe\nparentCID=ffffffff\n"
        'createType="monolithicFlat"\n\n'
        f'RW {sectors} FLAT "esp-flat.img" 0\n\n'
        'ddb.adapterType = "lsilogic"\n'
        f'ddb.geometry.cylinders = "{max(1, sectors // (64 * 32))}"\n'
        'ddb.geometry.heads = "64"\nddb.geometry.sectors = "32"\n'
    )
    settings = {
        ".encoding": "UTF-8",
        "config.version": "8",
        "virtualHW.version": "19",
        "displayName": "ia64-vmm smoke",
        "guestOS": "other-64",
        "firmware": "efi",
        "efi.secureBoot.enabled": "FALSE",
        "memsize": str(memory),
        "numvcpus": str(cpus),
        "cpuid.coresPerSocket": str(cpus),
        "nvram": "vars.nvram",
        "scsi0.present": "TRUE",
        "scsi0.virtualDev": "lsilogic",
        "scsi0.pciSlotNumber": "16",
        "scsi0:0.present": "TRUE",
        "scsi0:0.fileName": "esp.vmdk",
        "scsi0:0.deviceType": "disk",
        "sata0.present": "TRUE",
        "sata0.pciSlotNumber": "37",
        "serial0.present": "TRUE",
        "serial0.fileType": "file",
        "serial0.fileName": windows_path(temp / "serial.log"),
        "serial0.startConnected": "TRUE",
        "serial0.yieldOnMsrRead": "TRUE",
        "svga.present": "TRUE",
        "mks.enable3d": "FALSE",
        "ethernet0.present": "FALSE",
        "sound.present": "FALSE",
        "usb.present": "FALSE",
        "ehci.present": "FALSE",
        "usb_xhci.present": "FALSE",
        "floppy0.present": "FALSE",
        "vmci0.present": "FALSE",
    }
    if pcie_switch:
        # Workstation SATA is conventional PCI: put both functions on its
        # supported PCI bridge. It rewrites PCIe/nested placements to root
        # slots, so the QEMU fixture is the true PCIe-switch acceptance.
        settings.update(
            {
                "pciBridge0.present": "TRUE",
                "pciBridge0.pciSlotNumber": "17",
                "sata0.pciSlotNumber": "37",
                "sata1.present": "TRUE",
                "sata1.pciSlotNumber": "38",
            }
        )
    if iommu:
        settings["vvtd.enable"] = "TRUE"
    if nested_virtualization:
        # "Virtualize Intel VT-x/EPT or AMD-V/RVI" (vhv.enable).
        settings["vhv.enable"] = "TRUE"
    if cdrom is not None:
        copy_writable(cdrom, temp / "guest.iso")
        # 'ide' uses the chipset's PIIX4 IDE primary master (00:07.1).
        unit = "ide0:0" if cdrom_controller == "ide" else "sata0:0"
        settings.update(
            {
                f"{unit}.present": "TRUE",
                f"{unit}.fileName": "guest.iso",
                f"{unit}.deviceType": "cdrom-image",
                f"{unit}.startConnected": "TRUE",
            }
        )
    if guest_disk is not None:
        copy_writable(guest_disk, temp / "guest-flat.img")
        sectors = (temp / "guest-flat.img").stat().st_size // 512
        (temp / "guest.vmdk").write_text(
            "# Disk DescriptorFile\nversion=1\nCID=fffffffe\nparentCID=ffffffff\n"
            'createType="monolithicFlat"\n\n'
            f'RW {sectors} FLAT "guest-flat.img" 0\n\nddb.adapterType = "lsilogic"\n'
        )
        settings.update(
            {
                "sata0:1.present": "TRUE",
                "sata0:1.fileName": "guest.vmdk",
                "sata0:1.deviceType": "disk",
            }
        )
    if nvme is not None:
        copy_writable(nvme, temp / "nvme-flat.img")
        sectors = (temp / "nvme-flat.img").stat().st_size // 512
        (temp / "nvme.vmdk").write_text(
            "# Disk DescriptorFile\nversion=1\nCID=fffffffe\nparentCID=ffffffff\n"
            'createType="monolithicFlat"\n\n'
            f'RW {sectors} FLAT "nvme-flat.img" 0\n\nddb.adapterType = "lsilogic"\n'
        )
        # NVMe requires PCIe. Let Workstation place it under this root port;
        # conventional slot 38 fails with msg.pci.noslotavail before boot.
        settings.update(
            {
                "pciBridge4.present": "TRUE",
                "pciBridge4.virtualDev": "pcieRootPort",
                "pciBridge4.functions": "8",
                "pciBridge4.pciSlotNumber": "21",
                "nvme0.present": "TRUE",
                "nvme0:0.present": "TRUE",
                "nvme0:0.fileName": "nvme.vmdk",
                "nvme0:0.deviceType": "disk",
            }
        )
    if gdb:
        settings["debugStub.listen.guest64"] = "TRUE"
        print("VMware GDB stub enabled; connect to Windows host port 8864", flush=True)
    vmx = temp / "vmm.vmx"
    vmx.write_text("".join(f'{key} = "{value}"\n' for key, value in settings.items()))
    return vmx


def vmrun(executable, temp, arguments, timeout):
    deadline = time.monotonic() + timeout
    for attempt in range(3):
        result = subprocess.run(
            [executable, "-T", "ws", *arguments],
            cwd=temp,
            capture_output=True,
            timeout=max(0.01, deadline - time.monotonic()),
        )
        # A WSL relay can fail before launching vmrun. Retry read-only inventory,
        # retaining the original overall deadline; never repeat key injection.
        transient = b"UtilAcceptVsock" in result.stdout + result.stderr
        if result.returncode and arguments[0] == "list" and transient and attempt < 2:
            print("vmware: retrying inventory after WSL interop failure", file=sys.stderr)
            continue
        break
    if result.returncode:
        detail = (result.stdout + result.stderr).decode(errors="replace").strip()
        raise RuntimeError(f"vmrun {arguments[0]} exited {result.returncode}: {detail}")
    return result.stdout.decode(errors="replace")


def capture_screen(executable, temp, win_vmx, screen, deadline):
    try:
        vmrun(
            executable,
            temp,
            ["captureScreen", win_vmx, windows_path(screen)],
            max(0.01, deadline - time.monotonic()),
        )
    except RuntimeError as error:
        # VIX capture requires a guest login on current Workstation. This bare-metal
        # guest has no VMware Tools; its MKS CLI can capture without a login.
        diagnostic = temp / "vmware.log"
        if (
            not diagnostic.is_file()
            or "VIX_E_ANON_GUEST_OPERATIONS_PROHIBITED"
            not in diagnostic.read_text(errors="replace")
        ):
            raise
        vmcli = local_path(required("VMWARE_DIR")) / "vmcli.exe"
        if not vmcli.is_file():
            raise RuntimeError(
                f"{error}; VMWARE_DIR has no vmcli.exe for bare-metal capture"
            ) from error
        print(
            f"{error}; using vmcli MKS captureScreenshot for the bare-metal guest", file=sys.stderr
        )
        result = subprocess.run(
            [str(vmcli), win_vmx, "MKS", "captureScreenshot", windows_path(screen)],
            cwd=temp,
            capture_output=True,
            timeout=max(0.01, deadline - time.monotonic()),
        )
        if result.returncode:
            detail = (result.stdout + result.stderr).decode(errors="replace").strip()
            raise RuntimeError(f"vmcli captureScreenshot exited {result.returncode}: {detail}")
    if not screen.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"):
        raise RuntimeError("VMware screen capture did not produce a PNG")


# MKS HID codes pack the usage ID in the high word and keyboard page 0x07
# in the low word (VMware govmomi cli/vm/keystrokes.go:intToHidCode).
# bare usage IDs are silently accepted but do not inject keyboard events.
# VMware vmcli MKS sendKeyEvent <hidcode> <modifier> (local --help).
HID_KEYS = {
    "ret": 40,
    "esc": 41,
    "spc": 44,
    "up": 82,
    "down": 81,
    "left": 80,
    "right": 79,
    "home": 74,
    "end": 77,
    "pgup": 75,
    "pgdn": 78,
}
HID_KEYS.update({chr(97 + i): 4 + i for i in range(26)})
HID_KEYS.update({str(i): 29 + i for i in range(1, 10)})
HID_KEYS["0"] = 39
HID_KEYS.update({f"f{i}": 57 + i for i in range(1, 13)})


def send_key(temp, win_vmx, key, deadline):
    if key not in HID_KEYS:
        raise RuntimeError(f"VMware --sendkey unsupported key: {key}")
    vmcli = local_path(required("VMWARE_DIR")) / "vmcli.exe"
    result = subprocess.run(
        [str(vmcli), win_vmx, "MKS", "sendKeyEvent", str((HID_KEYS[key] << 16) | 7), "0"],
        cwd=temp,
        capture_output=True,
        timeout=max(0.01, deadline - time.monotonic()),
    )
    if result.returncode:
        raise RuntimeError(
            "vmcli sendKeyEvent: " + (result.stdout + result.stderr).decode(errors="replace")
        )


def run_vmware(args, root, log, explicit_halt):
    executable = str(local_path(required("VMWARE_DIR")) / "vmrun.exe")
    with run_directory(True, args.keep) as temp:
        vmx = create_vm(
            root,
            temp,
            args.gdb,
            getattr(args, "cdrom", None),
            getattr(args, "disk", None),
            getattr(args, "memory", 2048),
            getattr(args, "esp_image", None),
            getattr(args, "cpus", 4),
            getattr(args, "pcie_switch", False),
            getattr(args, "iommu", False),
            getattr(args, "nvme", None),
            getattr(args, "cdrom_controller", "sata"),
            getattr(args, "vmware_vhv", False),
        )
        win_vmx = windows_path(vmx)
        serial = temp / "serial.log"
        serial.write_text("")
        deadline = time.monotonic() + args.timeout
        from qemu.run import LiveActions, live_screenshot

        actions = LiveActions(args, time.monotonic())
        attempted_start = False
        boot_reported = False
        try:
            attempted_start = True
            print("vmware: vmrun -T ws start <private vmm.vmx> nogui", flush=True)
            vmrun(executable, temp, ["start", win_vmx, "nogui"], args.timeout)
            next_check = time.monotonic() + 1
            while True:
                if time.monotonic() >= deadline:
                    print(
                        f"VMware timeout after {args.timeout:g}s; serial log: {log}",
                        file=sys.stderr,
                    )
                    return 5
                text = serial.read_text(errors="replace")
                now = time.monotonic()
                if actions.screenshot_due(text, now):
                    screen = temp / "screen.png"
                    capture_screen(executable, temp, win_vmx, screen, deadline)
                    args.screenshot.resolve().parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(screen, args.screenshot.resolve())
                    actions.captured = True
                    print(
                        f"vmware: screenshot captured after {now - actions.started:.3f}s",
                        flush=True,
                    )
                if actions.late_screenshot_due(now):
                    screen = temp / "late.png"
                    try:
                        capture_screen(executable, temp, win_vmx, screen, deadline)
                        args.late_screenshot.resolve().parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(screen, args.late_screenshot.resolve())
                    except (OSError, RuntimeError) as error:
                        print(f"vmware: late screenshot unusable: {error}", file=sys.stderr)
                    actions.late_captured = True
                    print(f"vmware: late screenshot after {now - actions.started:.3f}s", flush=True)
                key = actions.key_due(text, now)
                if key is not None:
                    send_key(temp, win_vmx, key, deadline)
                    print(f"vmware: sendkey {key} after {now - actions.started:.3f}s", flush=True)
                matches = re.findall(r"VMM shutdown kind=\d+ exit_code=(\d+)", text)
                halted = re.findall(r"VMM halted exit_code=(\d+)", text)
                if matches and not boot_reported:
                    started = deadline - args.timeout
                    print(
                        f"vmware: VMM shutdown after {time.monotonic() - started:.3f}s", flush=True
                    )
                    boot_reported = True
                if matches and halted and not explicit_halt:
                    exit_code = int(matches[-1])
                    if exit_code != int(halted[-1]):
                        raise RuntimeError("VMware shutdown and halt exit codes disagree")
                    if args.screenshot and not actions.captured:
                        if live_screenshot(args):
                            raise RuntimeError("screenshot trigger was not reached")
                        screen = temp / "screen.png"
                        capture_screen(executable, temp, win_vmx, screen, deadline)
                        args.screenshot.resolve().parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(screen, args.screenshot.resolve())
                        print(f"screenshot: {args.screenshot.resolve()}")
                    return exit_code
                if time.monotonic() >= next_check:
                    running = vmrun(
                        executable, temp, ["list"], max(0.01, deadline - time.monotonic())
                    )
                    if win_vmx.casefold() not in running.casefold():
                        raise RuntimeError(
                            f"VMware stopped without a VMM shutdown/halt; serial log: {log}"
                        )
                    next_check = time.monotonic() + 1
                time.sleep(0.05)
        except subprocess.TimeoutExpired:
            print(
                f"VMware timeout after {args.timeout:g}s (vmrun); serial log: {log}",
                file=sys.stderr,
            )
            return 5
        finally:
            try:
                if attempted_start:
                    # Stop orphaned interop commands before looking for a VM;
                    # an interrupted vmrun start can otherwise power it on late.
                    kill_windows_processes("vmrun.exe", temp)
                    if args.screenshot or getattr(args, "sendkey", []):
                        kill_windows_processes("vmcli.exe", temp)
                    running = vmrun(executable, temp, ["list"], 10)
                    if win_vmx.casefold() in running.casefold():
                        vmrun(executable, temp, ["stop", win_vmx, "hard"], 10)
            finally:
                # A failed/timed-out start can still spawn vmware-vmx after vmrun exits.
                try:
                    kill_windows_processes("vmware-vmx.exe", temp)
                finally:
                    shutil.copyfile(serial, log)
