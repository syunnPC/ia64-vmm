#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot AMD64 Limine/OVMF, decode debug-exit, and capture halted GOP output."""

import argparse
from contextlib import contextmanager
import fcntl
import json
import importlib.util
import math
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qemu.config import ROOT, load_local_env, required
from qemu.windows import (
    copy_writable,
    kill_windows_processes,
    local_path,
    run_directory,
    stop_process,
    system_tool,
    windows_available,
    windows_path,
)


class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        self.sock.connect(str(path))
        self.file = self.sock.makefile("rwb")
        greeting = json.loads(self.file.readline())
        if "QMP" not in greeting:
            raise RuntimeError("invalid QMP greeting")
        self.command("qmp_capabilities")

    def command(self, name, arguments=None):
        request = {"execute": name}
        if arguments is not None:
            request["arguments"] = arguments
        self.file.write(json.dumps(request).encode() + b"\n")
        self.file.flush()
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError("QMP disconnected")
            response = json.loads(line)
            if "error" in response:
                raise RuntimeError(f"QMP {name}: {response['error']}")
            if "return" in response:
                return response["return"]

    def close(self):
        self.file.close()
        self.sock.close()


def ppm_to_png(source, dest, quiet=False):
    # QEMU's P6 screendump is usable even in builds without PNG support.
    data = source.read_bytes()
    match = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not match:
        raise RuntimeError("unexpected QEMU PPM format")
    width, height = map(int, match.groups())
    pixels = data[match.end() :]
    if len(pixels) != width * height * 3:
        raise RuntimeError("truncated QEMU screendump")

    def chunk(kind, body):
        return (
            struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
        )

    scanlines = b"".join(b"\0" + pixels[y * width * 3 : (y + 1) * width * 3] for y in range(height))
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(scanlines))
        + chunk(b"IEND", b"")
    )
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(png)
    if not quiet:
        print(f"screenshot: {dest} ({width}x{height})")


def screenshot_matches(path, args):
    words = getattr(args, "screenshot_text", [])
    if not words:
        return True
    spec = importlib.util.spec_from_file_location("loader_screen", ROOT / "tests/loaders/screen.py")
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    return reader.edk2_menu_matches(
        path,
        words,
        ROOT / "third_party/edk2-ipf/MdeModulePkg/Universal/Console/GraphicsConsoleDxe/LaffStd.c",
    )[0]


def memory_mib(value):
    match = re.fullmatch(r"([1-9][0-9]*)([mMgG]?)", str(value))
    if not match:
        raise argparse.ArgumentTypeError("memory must be positive MiB or NG")
    mib = int(match[1]) * (1024 if match[2].lower() == "g" else 1)
    if mib < 256 or mib > 1048576:
        raise argparse.ArgumentTypeError("memory must be between 256 MiB and 1 TiB")
    return mib


class LiveActions:
    """One-shot screenshot and paced keys; a regex gate starts their delays."""

    def __init__(self, args, started):
        self.args, self.started = args, started
        self.screen_gate = self.key_gate = None
        self.captured = self.late_captured = False
        self.next_periodic = (
            started + getattr(args, "screenshot_every", 0)
            if getattr(args, "screenshot_every", 0)
            else None
        )
        self.key_index = 0
        self.next_key = None

    def screenshot_due(self, serial, now):
        if not self.args.screenshot or self.captured:
            return False
        pattern = getattr(self.args, "screenshot_on", None)
        after = getattr(self.args, "screenshot_after", None)
        if pattern is None and after is None:
            return bool(re.search(r"VMM halted exit_code=\d+", serial))
        if pattern is not None:
            if self.screen_gate is None and re.search(pattern, serial):
                self.screen_gate = now + self.args.screenshot_delay
            return self.screen_gate is not None and now >= self.screen_gate
        return now - self.started >= after

    def late_screenshot_due(self, now):
        # One extra live capture at a fixed time (e.g. just before the
        # guest deadline), independent of the gated menu screenshot.
        late = getattr(self.args, "late_screenshot", None)
        return (
            late is not None
            and not self.late_captured
            and now - self.started >= self.args.late_screenshot_after
        )

    def periodic_due(self, now):
        # legacy VGA: live captures every N seconds for screens whose time is
        # not known in advance (bootvid, setup's first page).
        if self.next_periodic is None or now < self.next_periodic:
            return None
        elapsed = int(round(now - self.started))
        self.next_periodic = now + self.args.screenshot_every
        return elapsed

    def key_due(self, serial, now):
        if getattr(self.args, "screenshot_text", []) and not self.captured:
            return None
        keys = getattr(self.args, "sendkey", [])
        if self.key_index >= len(keys):
            return None
        if self.key_gate is None and re.search(self.args.sendkey_after, serial):
            self.key_gate = now + self.args.sendkey_delay
            self.next_key = self.key_gate
        if self.next_key is None or now < self.next_key:
            return None
        key = keys[self.key_index]
        self.key_index += 1
        self.next_key = now + self.args.sendkey_interval
        return key


def live_screenshot(args):
    return (
        getattr(args, "screenshot_after", None) is not None
        or getattr(args, "screenshot_on", None) is not None
    )


def guest_serial_file(log):
    """Decode debug UART frames; ordinary guest/firmware log lines are separate."""
    data = log.read_bytes()
    frames = re.findall(
        rb"^\[vmm-serial\] ([0-9a-f]{16}) ([0-9a-f]{4}) ([0-9a-f]+)\r?$", data, re.M
    )
    decoded = []
    end = missing = 0
    for position, count, frame in frames:
        position, count = int(position, 16), int(count, 16)
        if len(frame) != count * 2 or position < end:
            raise RuntimeError("invalid guest-serial frame length or sequence")
        missing += position - end
        end = position + count
        decoded.append(bytes.fromhex(frame.decode("ascii")))
    malformed = len(re.findall(rb"^\[vmm-serial\] ", data, re.M)) != len(frames)
    checkpoints = re.findall(rb"^\[vmm-serial-end\] ([0-9a-f]{16})\r?$", data, re.M)
    drained = bool(checkpoints) and int(checkpoints[-1], 16) == end
    dest = Path(str(log) + ".guest-serial.log")
    dest.write_bytes(b"".join(decoded))
    lost = sum(map(int, re.findall(rb"^\[vmm-serial-lost\] (\d+)\r?$", data, re.M)))
    dest.with_suffix(dest.suffix + ".json").write_text(
        json.dumps(
            {
                "bytes": dest.stat().st_size,
                "lost_bytes": max(lost, missing),
                "complete": drained and not (lost or missing or malformed),
            },
            indent=2,
        )
        + "\n"
    )
    if malformed:
        raise RuntimeError("truncated or invalid guest-serial frame")
    if lost:
        print(f"guest serial ring overrun: {lost} bytes lost", file=sys.stderr)
    return dest


def linux_accelerator():
    try:
        fd = os.open("/dev/kvm", os.O_RDWR)
        try:
            # linux/kvm.h: opening the device alone does not prove that
            # virtualization is available to this process.
            if fcntl.ioctl(fd, 0xAE00, 0) != 12:
                raise OSError("unsupported KVM API version")
            vm_fd = fcntl.ioctl(fd, 0xAE01, 0)
            os.close(vm_fd)
        finally:
            os.close(fd)
        return "kvm", "host"
    except OSError:
        print("QEMU: /dev/kvm unusable; falling back to TCG (-cpu max)", file=sys.stderr)
        return "tcg", "max"


def storage_arguments(args, temp, windows):
    path = windows_path if windows else str
    result = []
    controller = "ide"
    if getattr(args, "pcie_switch", False):
        # Root port -> upstream switch -> two downstream ports. Keep the
        # reserved ESP on the root bus; one AHCI is deliberately empty.
        # --pcie-switch-depth 2 chains a second switch behind the media port,
        # modeling chained switches with AHCI at the downstream endpoints.
        depth = getattr(args, "pcie_switch_depth", 1)
        result += [
            "-device",
            "pcie-root-port,id=r0-root,addr=04.0",
            "-device",
            "x3130-upstream,id=r0-switch,bus=r0-root",
            "-device",
            "xio3130-downstream,id=r0-empty-port,bus=r0-switch,addr=00.0,chassis=2",
            "-device",
            "xio3130-downstream,id=r0-media-port,bus=r0-switch,addr=01.0,chassis=3",
        ]
        parent = "r0-media-port"
        for level in range(2, depth + 1):
            result += [
                "-device",
                f"x3130-upstream,id=r0-switch{level},bus={parent}",
                "-device",
                f"xio3130-downstream,id=r0-media-port{level},bus=r0-switch{level},"
                f"addr=00.0,chassis={2 + level}",
            ]
            parent = f"r0-media-port{level}"
        result += [
            "-device",
            "ich9-ahci,id=r0-empty,bus=r0-empty-port",
            "-device",
            f"ich9-ahci,id=r0-ahci,bus={parent}",
        ]
        controller = "r0-ahci"
    if getattr(args, "machine", "q35") == "pc" and any(
        getattr(args, n, None) for n in ["cdrom", "disk"]
    ):
        result += ["-device", "ich9-ahci,id=guest-ahci,addr=04.0"]
        controller = "guest-ahci"
    ide_cdrom = getattr(args, "cdrom_controller", "sata") == "ide"
    ide_disk = getattr(args, "disk_controller", "sata") == "ide"
    if (ide_cdrom and getattr(args, "cdrom", None) is not None) or (
        ide_disk and getattr(args, "disk", None) is not None
    ):
        # Compatibility IDE (1F0/3F6, ISA IRQ14) serves guests without an
        # AHCI driver. pc supplies PIIX3; q35 needs a PIIX4 function.
        # The x86 QEMU builds lack native-mode PCI IDE (cmd646/via-ide).
        if getattr(args, "machine", "q35") == "pc":
            ide_bus = "ide.0"  # i440FX's own PIIX3 IDE primary channel
        else:
            result += ["-device", "piix4-ide,id=guest-ide,addr=07.0"]
            ide_bus = "guest-ide.0"
    for name, port in [("cdrom", 0), ("disk", 1)]:
        source = getattr(args, name, None)
        if source is None:
            continue
        if name == "cdrom" and ide_cdrom:
            dest = temp / "guest.iso"
            copy_writable(source, dest)
            result += [
                "-drive",
                f"if=none,id=guest-cdrom,format=raw,file={path(dest)},readonly=on,media=cdrom",
                "-device",
                f"ide-cd,drive=guest-cdrom,bus={ide_bus},unit=0",
            ]
            continue
        # Private disk copies protect installation media and make retries clean.
        dest = temp / ("guest.iso" if name == "cdrom" else "guest.img")
        copy_writable(source, dest)
        drive = f"if=none,id=guest-{name},format=raw,file={path(dest)}" + (
            ",readonly=on,media=cdrom" if name == "cdrom" else ""
        )
        if name == "disk" and ide_disk:
            result += [
                "-drive",
                drive,
                "-device",
                f"ide-hd,drive=guest-disk,bus={ide_bus.removesuffix('.0')}.1,unit=0",
            ]
            continue
        result += [
            "-drive",
            drive,
            "-device",
            f"{'ide-cd' if name == 'cdrom' else 'ide-hd'},drive=guest-{name},bus={controller}.{port}",
        ]
    usb = any(getattr(args, name, None) for name in ["usb_disk", "usb_cdrom"]) or getattr(
        args, "usb_keyboard", False
    )
    if usb:
        if getattr(args, "usb_controller", "xhci") == "xhci":
            result += ["-device", "qemu-xhci,id=guest-usb-host,addr=05.0"]
        else:
            result += [
                "-device",
                "ich9-usb-ehci1,id=guest-usb-host,addr=05.7",
                "-device",
                "ich9-usb-uhci1,masterbus=guest-usb-host.0,firstport=0,addr=05.0,multifunction=on",
                "-device",
                "ich9-usb-uhci2,masterbus=guest-usb-host.0,firstport=2,addr=05.1",
                "-device",
                "ich9-usb-uhci3,masterbus=guest-usb-host.0,firstport=4,addr=05.2",
            ]
        if getattr(args, "usb_keyboard", False):
            result += ["-device", "usb-kbd,bus=guest-usb-host.0"]
    for name in ["usb_disk", "usb_cdrom", "nvme"]:
        source = getattr(args, name, None)
        if source is None:
            continue
        ident = "guest-" + name.replace("_", "-")
        dest = temp / (ident + (".iso" if name == "usb_cdrom" else ".img"))
        copy_writable(source, dest)
        drive = f"if=none,id={ident},format=raw,file={path(dest)}"
        if name == "usb_cdrom":
            result += [
                "-drive",
                drive + ",readonly=on,media=cdrom",
                "-device",
                "usb-bot,id=guest-usb-cd,bus=guest-usb-host.0",
                "-device",
                f"scsi-cd,drive={ident},bus=guest-usb-cd.0",
            ]
        else:
            result += [
                "-drive",
                drive,
                "-device",
                f"usb-storage,drive={ident},bus=guest-usb-host.0"
                if name == "usb_disk"
                else f"nvme,drive={ident},serial=GUESTNVME,addr=06.0",
            ]
    return result


# std-vga is the Bochs VGA whose
# ROM BAR holds SeaVGABIOS; cirrus-vga keeps all its mode state in the VGA
# ports 3B0-3DF, so its own VBIOS can leave OVMF's GOP mode under the
# container's IOPM.
VGA_DEVICES = {"std": "VGA", "cirrus": "cirrus-vga"}
# The VMM ESP controller at 00:03.0.
ESP_DEVICE = "nvme,drive=vmm-esp,serial=VMMESP,addr=03.0,bootindex=1"


def whpx_init_failed(stderr):
    return bool(
        re.search(
            r"(?i)(failed to (?:initialize whpx|create.*partition)|"
            r"whpx[^\n]*(?:failed|not available|not supported|invalid accelerator)|"
            r"(?:invalid|unavailable) accelerator[^\n]*whpx)",
            stderr,
        )
    )


def run_qemu(args, root, log, cmdline):
    windows = args.backend == "win-qemu"
    if windows:
        qemu = str(local_path(required("WIN_QEMU")))
        code = local_path(required("WIN_QEMU_FW_CODE"))
        variables = local_path(required("WIN_QEMU_FW_VARS"))
        accel, cpu = ("tcg", "max") if getattr(args, "tcg", False) else ("whpx", None)
    else:
        qemu = os.environ.get("QEMU_X86", "qemu-system-x86_64")
        code = Path(required("OVMF_CODE"))
        variables = Path(required("OVMF_VARS"))
        accel, cpu = ("tcg", "max") if getattr(args, "tcg", False) else linux_accelerator()
    with run_directory(windows, args.keep) as temp:
        copy_writable(code, temp / "code.fd")
        serial_path = temp / "serial.log"
        path = windows_path if windows else str
        deadline = time.monotonic() + args.timeout
        while True:
            # A failed accelerator must not leave firmware or disk state for the retry.
            copy_writable(variables, temp / "vars.fd")
            copy_writable(getattr(args, "esp_image", root / "build/esp.img"), temp / "esp.img")
            serial_path.write_text("")
            # With a USB keyboard, disable PS/2 so sendkey validates USB input.
            machine = getattr(args, "machine", "q35")
            if getattr(args, "usb_keyboard", False):
                machine += ",i8042=off"
            if getattr(args, "iommu", False) and accel == "kvm":
                machine += ",kernel-irqchip=split"
            command = [
                qemu,
                "-machine",
                machine,
                "-m",
                str(getattr(args, "memory", 2048)),
                "-accel",
                accel,
                "-smp",
                f"{args.cpus},sockets=1,cores={args.cpus},threads=1",
                "-drive",
                f"if=pflash,format=raw,readonly=on,file={path(temp / 'code.fd')}",
                "-drive",
                f"if=pflash,format=raw,file={path(temp / 'vars.fd')}",
                "-drive",
                f"if=none,id=vmm-esp,format=raw,file={path(temp / 'esp.img')}",
                "-device",
                ESP_DEVICE,
                "-device",
                "isa-debug-exit,iobase=0xf4,iosize=0x04",
                "-serial",
                f"file:{path(serial_path)}",
                "-display",
                "none",
                "-nodefaults",
                "-device",
                VGA_DEVICES[getattr(args, "vga", "std")],
                "-no-reboot",
            ]
            # PCI reset fixture: the display device behind its own PCIe root port (q35),
            # as a discrete GPU sits behind 00:01.1, so that a Secondary Bus Reset
            # resets nothing else (vbios_reset=sbr).
            if getattr(args, "vga_root_port", False):
                at = command.index("-nodefaults") + 1
                command[at : at + 2] = [
                    "-device",
                    "pcie-root-port,id=guest-vga-port,chassis=9,addr=06.0",
                    "-device",
                    VGA_DEVICES[getattr(args, "vga", "std")] + ",bus=guest-vga-port",
                ]
            command += storage_arguments(args, temp, windows)
            if getattr(args, "iommu", False):
                device = (
                    "amd-iommu,dma-remap=on,intremap=on,xtsup=on"
                    if args.iommu == "amd"
                    else "intel-iommu,intremap=on,eim=on"
                )
                command += ["-device", device]
            if cpu:
                # E.g. '-svm' removes AMD-V from TCG's -cpu max.
                flags = getattr(args, "cpu_flags", None)
                command += ["-cpu", f"{cpu},{flags}" if flags else cpu]
            if windows:
                command += [
                    "-monitor",
                    "stdio"
                    if args.screenshot
                    or getattr(args, "sendkey", [])
                    or getattr(args, "control_file", None)
                    or getattr(args, "late_screenshot", None)
                    or getattr(args, "screenshot_every", 0)
                    else "none",
                ]
            else:
                command += ["-qmp", f"unix:{temp / 'qmp.sock'},server=on,wait=off"]
            if args.gdb:
                command += ["-s", "-S"]
                print("QEMU paused; connect GDB to localhost:1234, then continue", flush=True)
            print(f"{args.backend}: accelerator={accel}", flush=True)
            status, stderr, captured, timed_out = qemu_attempt(
                command, args, temp, serial_path, log, deadline, windows
            )
            if timed_out:
                return 5
            serial = log.read_text(errors="replace")
            if (
                windows
                and accel == "whpx"
                and status != 0
                and "VMM shutdown " not in serial
                and whpx_init_failed(stderr)
            ):
                print(
                    "QEMU: WHPX failed to initialise; falling back to Windows TCG (-cpu max)",
                    file=sys.stderr,
                )
                accel, cpu = "tcg", "max"
                continue
            matches = re.findall(r"VMM shutdown kind=\d+ exit_code=(\d+)", serial)
            if not matches:
                print(
                    f"QEMU exited {status} without a VMM shutdown; serial log: {log}",
                    file=sys.stderr,
                )
                return 1
            exit_code = int(matches[-1])
            if args.screenshot:
                if not captured or (not live_screenshot(args) and status != 0):
                    raise RuntimeError(
                        f"screenshot capture did not complete (QEMU status {status})"
                    )
            if (
                not args.screenshot or live_screenshot(args)
            ) and "exit=isa-debug-exit" in cmdline.split():
                if status != (((exit_code << 1) | 1) & 0xFF):
                    raise RuntimeError(f"QEMU status {status} does not match VMM exit {exit_code}")
            elif (not args.screenshot or live_screenshot(args)) and status != 0:
                raise RuntimeError(f"QEMU failed during host shutdown (status {status})")
            return exit_code


class ControlFile:
    """Whole lines appended to a file after the runner started.

    peek() returns the next complete line without consuming it, so a command
    that cannot run yet is retried. A shrunken or replaced file restarts at 0."""

    def __init__(self, path):
        self.path = path
        try:
            st = path.stat()
            self.offset, self.inode = st.st_size, st.st_ino
        except OSError:
            self.offset, self.inode = 0, None

    def peek(self):
        try:
            with self.path.open("rb") as handle:
                st = os.fstat(handle.fileno())
                if st.st_ino != self.inode or st.st_size < self.offset:
                    self.offset, self.inode = 0, st.st_ino
                handle.seek(self.offset)
                data = handle.read()
        except OSError:
            return None
        end = data.find(b"\n")
        if end < 0:
            return None
        self.length = end + 1
        return data[:end].decode(errors="replace").rstrip("\r")

    def consume(self):
        self.offset += self.length


def parse_control_line(line):
    """('key', NAME), ('hmp', COMMAND) or ('shot', FILE.png); None if invalid."""
    verb, _, arg = line.strip().partition(" ")
    arg = arg.strip()
    if verb == "key" and re.fullmatch(r"[A-Za-z0-9_-]+", arg):
        return verb, arg
    if verb == "hmp" and arg:
        return verb, arg
    if verb == "shot" and arg.endswith(".png"):
        return verb, arg
    return None


def save_qemu_screenshots(args, temp, captured, strict):
    shots = []
    if captured and (temp / "screen.ppm").is_file():
        shots.append((temp / "screen.ppm", args.screenshot, True))
    if getattr(args, "late_screenshot", None) is not None and (temp / "late.ppm").is_file():
        shots.append((temp / "late.ppm", args.late_screenshot, False))
    if getattr(args, "screenshot_every", 0):
        shots.extend(
            (shot, args.screenshot_dir / (shot.stem.replace("periodic-", "t") + ".png"), False)
            for shot in sorted(temp.glob("periodic-*.ppm"))
        )
    failure = None
    for source, destination, primary in shots:
        try:
            ppm_to_png(source, destination.resolve())
        except (OSError, RuntimeError) as error:
            print(f"screenshot {source.name} unusable: {error}", file=sys.stderr)
            if primary and strict:
                failure = error
    if failure is not None:
        raise failure


def qemu_attempt(command, args, temp, serial_path, log, deadline, windows):
    captured = False
    timed_out = False
    qmp = None
    process = None
    boot_reported = False
    control = ControlFile(args.control_file) if getattr(args, "control_file", None) else None
    paused = False
    pending_screen, next_screen = False, 0
    match_screen = bool(getattr(args, "screenshot_text", []))
    stderr_path = temp / "qemu.stderr"
    # File-backed diagnostics cannot fill a pipe while the watchdog is polling.
    with stderr_path.open("wb") as errors, (temp / "qemu.stdout").open("wb") as output:
        try:
            started = time.monotonic()
            actions = LiveActions(args, started)
            process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE
                if args.screenshot
                or getattr(args, "sendkey", [])
                or getattr(args, "control_file", None)
                or getattr(args, "late_screenshot", None)
                or getattr(args, "screenshot_every", 0)
                else subprocess.DEVNULL,
                stdout=output,
                stderr=errors,
                cwd=temp,
            )

            def monitor(text):
                if windows:
                    process.stdin.write(f"{text}\n".encode())
                    process.stdin.flush()
                else:
                    reply = qmp.command("human-monitor-command", {"command-line": text})
                    output.write(f"(qemu) {text}\n{reply or ''}".encode())
                    output.flush()

            while process.poll() is None:
                if time.monotonic() >= deadline:
                    timed_out = True
                    print(
                        f"QEMU timeout after {args.timeout:g}s; serial log: {log}", file=sys.stderr
                    )
                    break
                serial = serial_path.read_text(errors="replace")
                if not boot_reported and "VMM shutdown " in serial:
                    print(
                        f"{args.backend}: VMM shutdown after {time.monotonic() - started:.3f}s",
                        flush=True,
                    )
                    boot_reported = True
                now = time.monotonic()
                if pending_screen:
                    try:
                        ppm_to_png(temp / "screen.ppm", temp / "candidate.png", quiet=True)
                    except (OSError, RuntimeError):
                        pass  # The Windows monitor writes asynchronously.
                    else:
                        actions.captured = captured = screenshot_matches(
                            temp / "candidate.png", args
                        )
                        pending_screen, next_screen = False, now + 0.5
                        if windows:
                            process.stdin.write(b"cont\n")
                            process.stdin.flush()
                        else:
                            qmp.command("cont")
                        if captured:
                            # Keep the confirmed frame even if a milestone ends the runner.
                            destination = args.screenshot.resolve()
                            destination.parent.mkdir(parents=True, exist_ok=True)
                            shutil.copyfile(temp / "candidate.png", destination)
                            print(
                                f"{args.backend}: screenshot text matched after {now - started:.3f}s",
                                flush=True,
                            )
                if (
                    not pending_screen
                    and now >= next_screen
                    and actions.screenshot_due(serial, now)
                ):
                    (temp / "screen.ppm").unlink(missing_ok=True)
                    if windows:
                        filename = json.dumps(windows_path(temp / "screen.ppm"), ensure_ascii=False)
                        process.stdin.write(f"stop\nscreendump {filename}\n".encode())
                        if not match_screen:
                            process.stdin.write(b"cont\n" if live_screenshot(args) else b"quit\n")
                        process.stdin.flush()
                    else:
                        if qmp is None:
                            qmp = QMP(temp / "qmp.sock")
                        qmp.command("stop")
                        qmp.command("screendump", {"filename": str(temp / "screen.ppm")})
                        if not match_screen:
                            qmp.command("cont" if live_screenshot(args) else "quit")
                    if match_screen:
                        pending_screen = True
                    else:
                        actions.captured = captured = True
                        print(
                            f"{args.backend}: screenshot requested after {now - started:.3f}s",
                            flush=True,
                        )
                if (
                    not pending_screen
                    and actions.late_screenshot_due(now)
                    and (windows or qmp is not None or (temp / "qmp.sock").exists())
                ):
                    late = temp / "late.ppm"
                    if windows:
                        filename = json.dumps(windows_path(late), ensure_ascii=False)
                        process.stdin.write(f"stop\nscreendump {filename}\ncont\n".encode())
                        process.stdin.flush()
                    else:
                        if qmp is None:
                            qmp = QMP(temp / "qmp.sock")
                        qmp.command("stop")
                        qmp.command("screendump", {"filename": str(late)})
                        qmp.command("cont")
                    actions.late_captured = True
                    print(
                        f"{args.backend}: late screenshot requested after {now - started:.3f}s",
                        flush=True,
                    )
                elapsed = (
                    actions.periodic_due(now)
                    if not pending_screen
                    and (windows or qmp is not None or (temp / "qmp.sock").exists())
                    else None
                )
                if elapsed is not None:
                    shot = temp / f"periodic-{elapsed:05d}.ppm"
                    if windows:
                        filename = json.dumps(windows_path(shot), ensure_ascii=False)
                        process.stdin.write(f"stop\nscreendump {filename}\ncont\n".encode())
                        process.stdin.flush()
                    else:
                        if qmp is None:
                            qmp = QMP(temp / "qmp.sock")
                        qmp.command("stop")
                        qmp.command("screendump", {"filename": str(shot)})
                        qmp.command("cont")
                while control is not None and not pending_screen:
                    line = control.peek()
                    if line is None:
                        break
                    command_line = parse_control_line(line)
                    if command_line is None:
                        print(f"control file: ignored {line!r}", file=sys.stderr)
                        control.consume()
                        continue
                    if not windows and qmp is None:
                        if not (temp / "qmp.sock").exists():
                            break  # retried next pass
                        qmp = QMP(temp / "qmp.sock")
                    verb, arg = command_line
                    try:
                        if verb == "key":
                            monitor(f"sendkey {arg} 100")
                            time.sleep(args.sendkey_interval)
                        elif verb == "hmp":
                            word = arg.split()[0]
                            if word == "stop":
                                paused = True
                            elif word in ("cont", "c"):
                                paused = False
                            if qmp:
                                qmp.sock.settimeout(max(1, min(600, deadline - time.monotonic())))
                            monitor(arg)
                        else:
                            ppm = temp / "control.ppm"
                            ppm.unlink(missing_ok=True)
                            if not paused:
                                monitor("stop")
                            monitor(
                                f"screendump {json.dumps(windows_path(ppm) if windows else str(ppm), ensure_ascii=False)}"
                            )
                            if not paused:
                                monitor("cont")
                            destination = (
                                Path(arg) if Path(arg).is_absolute() else args.caller_dir / arg
                            )
                            for _ in range(100):
                                try:
                                    ppm_to_png(ppm, destination, quiet=True)
                                    break
                                except (OSError, RuntimeError):
                                    time.sleep(0.1)
                            else:
                                print(
                                    f"control file: no screenshot written for {arg}",
                                    file=sys.stderr,
                                )
                    except (OSError, RuntimeError) as error:
                        print(f"control file: {line!r} failed: {error}", file=sys.stderr)
                    finally:
                        if qmp:
                            qmp.sock.settimeout(5)
                    control.consume()
                key = actions.key_due(serial, now) if not pending_screen else None
                if key is not None:
                    if windows:
                        process.stdin.write(f"sendkey {key} 100\n".encode())
                        process.stdin.flush()
                    else:
                        if qmp is None:
                            qmp = QMP(temp / "qmp.sock")
                        qmp.command("human-monitor-command", {"command-line": f"sendkey {key} 100"})
                    print(f"{args.backend}: sendkey {key} after {now - started:.3f}s", flush=True)
                time.sleep(0.05)
            if not boot_reported and "VMM shutdown " in serial_path.read_text(errors="replace"):
                print(
                    f"{args.backend}: VMM shutdown after {time.monotonic() - started:.3f}s",
                    flush=True,
                )
        finally:
            try:
                try:
                    if qmp:
                        qmp.close()
                finally:
                    if process:
                        try:
                            # Killing only the WSL relay can orphan the Windows emulator.
                            if windows:
                                kill_windows_processes(Path(command[0]).name, temp)
                        finally:
                            stop_process(process)
            finally:
                try:
                    if process and process.stdin:
                        process.stdin.close()
                    if serial_path.is_file():
                        shutil.copyfile(serial_path, log)
                finally:
                    save_qemu_screenshots(
                        args, temp, captured, strict=not timed_out and sys.exc_info()[0] is None
                    )
    stderr = stderr_path.read_text(errors="replace")
    if stderr:
        print(stderr, file=sys.stderr, end="")
    return process.returncode, stderr, captured, timed_out


def nested_make_environment(environment):
    # A command-line assignment in the outer make must not override this run.
    clean = dict(environment)
    clean["MAKEFLAGS"] = clean["MAKEOVERRIDES"] = ""
    return clean


@contextmanager
def staged_esp(root, environment, physical_media, no_build, cmdline, firmware_image=None):
    # Different invocations share mkesp's output. Lock generation and snapshot
    # it before release so later staging or accelerator retry cannot mix VMs.
    environment = nested_make_environment(environment)
    selection = [
        f"FIRMWARE={environment.get('FIRMWARE', 'new')}",
        f"BOOT_APP={environment.get('BOOT_APP', 'hello')}",
    ]
    build = root / "build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="esp-snapshot-", dir=build) as temp:
        snapshot = Path(temp) / "esp.img"
        with (build / ".runner-esp.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            if no_build or physical_media:
                # Re-select the firmware image (FIRMWARE=new) before packing.
                subprocess.run(
                    [
                        "make",
                        "build/firmware.elf",
                        "build/bootmedia/bootmedia.efi",
                        "third_party/limine/BOOTX64.EFI",
                        *selection,
                    ],
                    cwd=root,
                    env=environment,
                    check=True,
                )
                if not physical_media:
                    subprocess.run(
                        ["make", "bootdisk", *selection], cwd=root, env=environment, check=True
                    )
                subprocess.run(
                    [sys.executable, str(root / "tools/qemu/mkesp.py")], env=environment, check=True
                )
            else:
                subprocess.run(
                    ["make", "esp", *selection, f"VMM_CMDLINE={cmdline}"],
                    cwd=root,
                    env=environment,
                    check=True,
                )
            shutil.copyfile(build / "esp.img", snapshot)
            if firmware_image is not None:
                # Override only this private snapshot; shared build images
                # and later users retain their selected default firmware.
                subprocess.run(
                    ["mcopy", "-o", "-i", str(snapshot), str(firmware_image), "::/firmware.elf"],
                    check=True,
                )
        yield snapshot


def main():
    load_local_env()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--backend",
        choices=["wsl-qemu", "win-qemu", "vmware"],
        default=os.environ.get("VMM_BACKEND") or None,
    )
    parser.add_argument("--cdrom", type=Path, metavar="ISO", help="real guest SATA installation CD")
    parser.add_argument(
        "--cdrom-controller",
        choices=["sata", "ide"],
        default="sata",
        help="attach --cdrom to SATA/AHCI (default) or to a PIIX IDE primary master",
    )
    parser.add_argument(
        "--disk", type=Path, metavar="IMG", help="real guest raw disk (private copy)"
    )
    parser.add_argument(
        "--disk-controller",
        choices=["sata", "ide"],
        default="sata",
        help="QEMU target disk: AHCI or PIIX IDE secondary master",
    )
    parser.add_argument(
        "--usb-disk", type=Path, metavar="IMG", help="guest USB raw disk (private copy)"
    )
    parser.add_argument("--usb-cdrom", type=Path, metavar="ISO", help="guest USB BOT/SCSI CD-ROM")
    parser.add_argument("--usb-controller", choices=["xhci", "ehci"], default="xhci")
    parser.add_argument(
        "--usb-keyboard",
        action="store_true",
        help="attach a USB HID keyboard to the guest controller",
    )
    parser.add_argument(
        "--nvme", type=Path, metavar="IMG", help="guest NVMe raw disk (private copy)"
    )
    parser.add_argument(
        "--pcie-switch",
        action="store_true",
        help="PCIe bridge hierarchy with two AHCI controllers (one empty)",
    )
    parser.add_argument(
        "--pcie-switch-depth",
        type=int,
        choices=[1, 2, 3],
        default=1,
        help="switches chained in front of the media AHCI (two switches)",
    )
    parser.add_argument(
        "--iommu",
        nargs="?",
        const="intel",
        choices=["intel", "amd"],
        help="expose VT-d (default) or AMD-Vi for DMA and interrupt-remapping validation",
    )
    parser.add_argument("--keep", action="store_true", help="keep the private run directory")
    parser.add_argument("--memory", type=memory_mib, default=2048, metavar="MiB|NG")
    parser.add_argument("--screenshot", type=Path, metavar="FILE.png")
    screen_gate = parser.add_mutually_exclusive_group()
    screen_gate.add_argument(
        "--screenshot-after",
        type=float,
        metavar="SECONDS",
        help="capture while running after this delay",
    )
    screen_gate.add_argument("--screenshot-on", metavar="REGEX", help="capture when serial matches")
    parser.add_argument(
        "--screenshot-text",
        action="append",
        default=[],
        metavar="TEXT",
        help="retry captures until all text is visible; defer keys until captured",
    )
    parser.add_argument(
        "--screenshot-delay",
        type=float,
        default=0.25,
        help="delay after a screenshot log match (default .25s)",
    )
    parser.add_argument(
        "--late-screenshot",
        type=Path,
        metavar="FILE.png",
        help="one additional live capture at --late-screenshot-after seconds",
    )
    parser.add_argument("--late-screenshot-after", type=float, default=0, metavar="SECONDS")
    parser.add_argument(
        "--screenshot-every",
        type=float,
        default=0,
        metavar="SECONDS",
        help="live captures every SECONDS into --screenshot-dir as tNNNNN.png (seconds since start)",
    )
    parser.add_argument("--screenshot-dir", type=Path, metavar="DIR")
    parser.add_argument(
        "--sendkey",
        action="append",
        default=[],
        metavar="KEY",
        help="paced PS/2 HMP key names; VMware uses MKS scancodes",
    )
    parser.add_argument("--sendkey-after", default="GRUB", metavar="REGEX")
    parser.add_argument(
        "--control-file",
        type=Path,
        metavar="FILE",
        help='QEMU: execute lines appended to FILE after start: "key NAME", '
        '"shot FILE.png", "hmp COMMAND"',
    )
    parser.add_argument("--sendkey-delay", type=float, default=0.75)
    parser.add_argument("--sendkey-interval", type=float, default=0.4)
    parser.add_argument(
        "--no-build", action="store_true", help="stage the existing vmm.elf without compiling"
    )
    parser.add_argument(
        "--firmware", choices=["new"], default="new", help="select IA-64 firmware (default: new)"
    )
    parser.add_argument(
        "--firmware-image",
        type=Path,
        help="override the firmware module in this private ESP snapshot",
    )
    parser.add_argument(
        "--gdb", action="store_true", help="QEMU: pause on :1234; VMware: guest64 stub on :8864"
    )
    parser.add_argument(
        "--cpus",
        type=int,
        default=4,
        metavar="N",
        help="host logical CPUs, one thread per core (default: 4)",
    )
    parser.add_argument(
        "--machine",
        choices=["q35", "pc"],
        default="q35",
        help="QEMU chipset for this invocation (default: q35)",
    )
    parser.add_argument("--timeout", type=float, default=60, metavar="SECONDS")
    parser.add_argument(
        "--vga",
        choices=sorted(VGA_DEVICES),
        default="std",
        help="QEMU display device (default: std, the Bochs std-vga)",
    )
    parser.add_argument(
        "--vga-root-port",
        action="store_true",
        help="q35: the display device behind its own pcie-root-port (PCI reset vbios_reset=sbr)",
    )
    parser.add_argument(
        "--vmware-vhv",
        action="store_true",
        help="VMware: offer nested AMD-V/VT-x to the VMM (vhv.enable)",
    )
    parser.add_argument(
        "--cpu-flags",
        metavar="FLAGS",
        help="QEMU -cpu feature suffix for TCG/KVM, e.g. '-svm' (no-SVM setup failure test)",
    )
    parser.add_argument(
        "--tcg", action="store_true", help="QEMU: use software emulation with -cpu max"
    )
    parser.add_argument(
        "--cmdline", default=os.environ.get("VMM_CMDLINE", "exit=isa-debug-exit reserve_mib=128")
    )
    parser.add_argument(
        "--debug-serial",
        choices=["off", "device", "console"],
        help="override debug_serial= (VMM default: off)",
    )
    parser.add_argument(
        "--debug-screen", choices=["0", "1"], help="override debug_screen= (VMM default: 0)"
    )
    parser.add_argument("--log", type=Path, default=Path("build/qemu.log"))
    args = parser.parse_args()
    args.caller_dir = Path.cwd()
    if args.control_file is not None:
        args.control_file = args.control_file.resolve()
        if not args.control_file.is_file():
            parser.error(f"--control-file not found: {args.control_file}")
    args.log = (ROOT / args.log).resolve()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.log.write_text("")
    if not 2 <= args.cpus <= 256:
        parser.error("--cpus must be between 2 and 256 (management needs a separate CPU)")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive")
    for name in [
        "screenshot_after",
        "screenshot_delay",
        "sendkey_delay",
        "sendkey_interval",
        "late_screenshot_after",
    ]:
        value = getattr(args, name)
        if value is not None and (not math.isfinite(value) or value < 0):
            parser.error("--" + name.replace("_", "-") + " must be finite and nonnegative")
    if args.screenshot_text and (
        not args.screenshot or not live_screenshot(args) or args.backend == "vmware"
    ):
        parser.error("--screenshot-text requires a live QEMU screenshot")
    if live_screenshot(args) and not args.screenshot:
        parser.error("--screenshot-on/--screenshot-after requires --screenshot")
    if not math.isfinite(args.screenshot_every) or args.screenshot_every < 0:
        parser.error("--screenshot-every must be finite and nonnegative")
    if args.screenshot_every and (args.screenshot_dir is None or args.backend == "vmware"):
        parser.error("--screenshot-every needs --screenshot-dir and a QEMU backend")
    for pattern in [args.screenshot_on, args.sendkey_after]:
        if pattern is not None:
            try:
                re.compile(pattern)
            except re.error as error:
                parser.error(f"invalid log regex: {error}")
    if any(not re.fullmatch(r"[A-Za-z0-9_-]+", key) for key in args.sendkey):
        parser.error("--sendkey accepts key names/combinations only")
    if args.backend is not None and args.backend not in ["wsl-qemu", "win-qemu", "vmware"]:
        parser.error("VMM_BACKEND must be wsl-qemu, win-qemu or vmware")
    if args.backend is None:
        try:
            validate_backend("win-qemu")
            args.backend = "win-qemu"
        except (OSError, RuntimeError, subprocess.SubprocessError):
            args.backend = "wsl-qemu"
    validate_backend(args.backend)
    if args.backend == "vmware" and args.iommu == "amd":
        parser.error("--iommu amd requires a QEMU backend")
    if args.machine != "q35" and (args.backend == "vmware" or args.iommu or args.pcie_switch):
        parser.error("--machine pc requires QEMU without --iommu or --pcie-switch")
    if args.backend == "vmware" and (args.vga != "std" or args.cpu_flags or args.tcg):
        parser.error("--vga, --cpu-flags and --tcg apply to the QEMU backends")
    if args.vmware_vhv and args.backend != "vmware":
        parser.error("--vmware-vhv applies to the VMware backend")
    if args.vga_root_port and (args.backend == "vmware" or args.machine != "q35"):
        parser.error("--vga-root-port needs a QEMU backend with --machine q35")
    if args.cpu_flags is not None and not re.fullmatch(
        r"[+-]?[a-z0-9_-]+(,[+-]?[a-z0-9_-]+)*", args.cpu_flags
    ):
        parser.error("--cpu-flags takes comma-separated QEMU CPU feature switches")
    root = ROOT
    os.chdir(root)
    log = args.log.resolve()
    if args.control_file is not None and args.backend == "vmware":
        parser.error("--control-file applies to the QEMU backends")
    if args.disk_controller == "ide" and args.backend == "vmware":
        parser.error("--disk-controller ide requires a QEMU backend")
    if args.backend == "vmware" and (args.usb_disk or args.usb_cdrom or args.usb_keyboard):
        parser.error(
            "VMware USB needs manual host-device pass-through; its CLI cannot attach USB image files or a virtual USB keyboard"
        )
    for name in ["cdrom", "disk", "usb_disk", "usb_cdrom", "nvme"]:
        value = getattr(args, name)
        if value is not None:
            if not value.is_absolute():
                value = Path(required("IA64_IMAGES_DIR")) / value
            value = value.resolve()
            if not value.is_file():
                parser.error(f"--{name} file not found: {value}")
            if name in ["disk", "usb_disk", "nvme"] and (
                not value.stat().st_size or value.stat().st_size % 512
            ):
                parser.error(
                    "--"
                    + name.replace("_", "-")
                    + " must be a nonempty raw image of whole 512-byte sectors"
                )
            setattr(args, name, value)
    if args.firmware_image is not None:
        args.firmware_image = args.firmware_image.resolve()
        if not args.firmware_image.is_file():
            parser.error("--firmware-image must name an existing ELF file")
    cmdline = args.cmdline
    for option, value in [("debug_serial", args.debug_serial), ("debug_screen", args.debug_screen)]:
        if value is not None:
            cmdline = " ".join(t for t in cmdline.split() if not t.startswith(option + "="))
            cmdline += f" {option}={value}"
    if not any(token.startswith("vmm_media=") for token in cmdline.split()):
        cmdline += " vmm_media=" + ("00:10.0" if args.backend == "vmware" else "auto")
    if "\n" in cmdline or "\r" in cmdline or len(cmdline.encode()) > 511:
        parser.error("--cmdline must be one line of at most 511 bytes")
    if args.screenshot and not live_screenshot(args):
        cmdline = (
            " ".join(token for token in cmdline.split() if not token.startswith("exit="))
            + " exit=halt"
        )
    elif not any(token.startswith("exit=") for token in cmdline.split()):
        cmdline += " exit=isa-debug-exit"
    if len(cmdline.encode()) > 511:
        parser.error("--cmdline exceeds 511 bytes after adding the exit option")
    # VMware has no isa-debug-exit. Halt after logging so vmrun can stop it.
    explicit_halt = "exit=halt" in cmdline.split() and not args.screenshot
    if args.backend == "vmware":
        cmdline = (
            " ".join(token for token in cmdline.split() if not token.startswith("exit="))
            + " exit=halt"
        )
        if len(cmdline.encode()) > 511:
            parser.error("--cmdline exceeds 511 bytes after adding the exit option")
    physical_media = any([args.cdrom, args.disk, args.usb_disk, args.usb_cdrom, args.nvme])
    environment = nested_make_environment(
        dict(
            os.environ,
            VMM_CMDLINE=cmdline,
            FIRMWARE=args.firmware,
            VMM_PHYSICAL_MEDIA="1" if physical_media else "0",
        )
    )
    if not args.no_build and physical_media:
        # Building the smoke bootdisk would overwrite --disk build/bootdisk.img.
        # Physical runs need the VMM, firmware and boot chain, then a fresh ESP.
        subprocess.run(
            [
                "make",
                "vmm",
                "build/firmware.elf",
                "third_party/limine/BOOTX64.EFI",
                "build/bootmedia/bootmedia.efi",
                f"VMM_CMDLINE={cmdline}",
                f"FIRMWARE={args.firmware}",
                f"BOOT_APP={environment.get('BOOT_APP', 'hello')}",
            ],
            cwd=root,
            env=environment,
            check=True,
        )
    try:
        with staged_esp(
            root, environment, physical_media, args.no_build, cmdline, args.firmware_image
        ) as image:
            args.esp_image = image
            if args.backend == "vmware":
                from vmware.backend import run_vmware

                result = run_vmware(args, root, log, explicit_halt)
            else:
                result = run_qemu(args, root, log, cmdline)
    finally:
        guest_serial_file(log)
    print(f"VMM runner status: {result}; serial log: {log}")
    return result


def validate_backend(backend):
    if backend == "wsl-qemu":
        qemu = os.environ.get("QEMU_X86", "qemu-system-x86_64")
        if not shutil.which(qemu):
            raise RuntimeError(f"QEMU_X86 executable not found: {qemu}")
        files = ["OVMF_CODE", "OVMF_VARS"]
    else:
        files = (
            ["WIN_QEMU", "WIN_QEMU_FW_CODE", "WIN_QEMU_FW_VARS"]
            if backend == "win-qemu"
            else ["VMWARE_DIR"]
        )
        # Check configuration before interop so explicit selection names missing variables.
        for name in [*files, "WIN_STAGING_DIR"]:
            required(name)
        if not windows_available():
            raise RuntimeError(
                f"{backend} requires working WSL Windows interop (WIN_SYSTEM32/PATH)"
            )
        for name in ["powershell.exe", "tasklist.exe", "taskkill.exe"]:
            system_tool(name)
    for name in files:
        path = local_path(required(name)) if backend != "wsl-qemu" else Path(required(name))
        if name == "VMWARE_DIR":
            path = path / "vmrun.exe"
        if not path.is_file():
            raise RuntimeError(f"{name} file not found: {path}")


def interrupted(signum, frame):
    raise KeyboardInterrupt


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"run.sh: {error}", file=sys.stderr)
        sys.exit(1)
