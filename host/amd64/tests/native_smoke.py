#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot an isolated native IA-32 smoke image with a prebuilt UEFI firmware."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def existing(setting, defaults):
    candidates = [os.environ[setting]] if os.environ.get(setting) else defaults
    for value in candidates:
        path = Path(value)
        if path.is_file():
            return path
    raise SystemExit(f"Set {setting} to an existing prebuilt OVMF image")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", action="store_true")
    parser.add_argument("--accel", choices=("auto", "kvm", "tcg"), default="auto")
    parser.add_argument("--qemu", default=os.environ.get("QEMU_X86", "qemu-system-x86_64"))
    parser.add_argument("--cpu", default=os.environ.get("NATIVE_QEMU_CPU"))
    parser.add_argument("--timeout", type=int, default=45)
    args = parser.parse_args()
    os.chdir(ROOT)
    output = ROOT / "build/native-smoke"
    image = output / ("native-engine.elf" if args.engine else "vmm.elf")
    if not image.is_file():
        raise SystemExit(f"Build {image.relative_to(ROOT)} first")
    accel = args.accel
    if accel == "auto":
        try:
            fd = os.open("/dev/kvm", os.O_RDWR)
            os.close(fd)
            accel = "kvm"
        except OSError:
            accel = "tcg"
    code = existing("NATIVE_OVMF_CODE", ["/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_CODE.fd"])
    variables = existing("NATIVE_OVMF_VARS", ["/usr/share/OVMF/OVMF_VARS_4M.fd", "/usr/share/OVMF/OVMF_VARS.fd"])
    mode = "engine" if args.engine else "transport"
    work = output / f"{mode}-{accel}"
    esp = work / "esp"
    (esp / "EFI/BOOT").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / "third_party/limine/BOOTX64.EFI", esp / "EFI/BOOT/BOOTX64.EFI")
    shutil.copyfile(image, esp / "vmm.elf")
    shutil.copyfile(variables, work / "OVMF_VARS.fd")
    (esp / "limine.conf").write_text(
        "timeout: 0\n\n/Native IA-32 smoke\n    protocol: limine\n"
        "    path: boot():/vmm.elf\n"
        "    cmdline: exit=isa-debug-exit reserve_mib=128 host_mwait=off\n"
    )
    disk = work / "esp.img"
    with disk.open("wb") as stream:
        stream.truncate(64 * 1024 * 1024)
    subprocess.run(["mformat", "-i", str(disk), "-F", "::"], check=True)
    subprocess.run(["mcopy", "-i", str(disk), "-s", str(esp / "EFI"), "::/"], check=True)
    for filename in ("vmm.elf", "limine.conf"):
        subprocess.run(["mcopy", "-i", str(disk), str(esp / filename), "::/"], check=True)
    log = work / "serial.log"
    command = [
        args.qemu, "-accel", accel, "-cpu", args.cpu or ("host" if accel == "kvm" else "max"),
        "-machine", "q35", "-m", "512M", "-smp", "2,sockets=1,cores=2,threads=1",
        "-boot", "order=c,menu=off",
        "-display", "none", "-serial", f"file:{log}", "-monitor", "none", "-net", "none",
        "-no-reboot", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
        "-drive", f"if=pflash,format=raw,file={work / 'OVMF_VARS.fd'}",
        "-drive", f"if=none,id=native-esp,format=raw,file={disk}",
        "-device", "virtio-blk-pci,drive=native-esp,bootindex=1",
    ]
    log.unlink(missing_ok=True)
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=args.timeout, text=True)
    except subprocess.TimeoutExpired:
        print(f"native {mode} smoke timed out; see {log.relative_to(ROOT)}", file=sys.stderr)
        return 1
    transcript = log.read_text(errors="replace") if log.exists() else ""
    marker = "native engine smoke PASSED" if args.engine else "native smoke PASSED"
    if result.returncode != 1 or marker not in transcript:
        print(result.stdout, file=sys.stderr)
        print(transcript[-12000:], file=sys.stderr)
        return 1
    print(f"native {mode} smoke passed with {accel}; log {log.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
