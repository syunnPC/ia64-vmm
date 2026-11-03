#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Product PLOG round trip on a builder MBR/FAT32 image and qemu-xhci/usb-storage.

--nvram: EFI variable slots on the same
kind of image with the native firmware and restart.efi.
Run 1 goes through every reset source as in-VMM restarts and ends with one
slot write; run 2 on the same image starts at the last phase, so its
variables came from the stick. A second machine (another SMBIOS identity)
then gets its own pair and the first machine still finds its variables
with native firmware.

--nvme: the same image as the QEMU runner's VMM ESP, NVMe at 00:03.0.
--ahci: the image on ide-hd behind ich9-ahci at 00:05.0. --ehci: on
usb-storage behind ich9-usb-ehci1 at 00:05.7, with the guest's UHCI
companions at 00:05.0-2 and a USB keyboard on another port; QEMU's port
routing trace shows the keyboard going to a companion and the stick never
reaching one during the VMM run. --ehci --stop: the stick is unplugged
during the guest run and plugged into another port; the backend stops and
the stick still stays with the EHCI controller. --ehci --hub: the stick
behind usb-hub on that port; QEMU's usb-hub is a full-speed hub, so the
backend refuses it, writes nothing, and neither the hub nor the keyboard
reaches a companion during the VMM run."""

import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import socket
import json
import hashlib
import re
import uuid
from datetime import datetime

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from qemu.config import ROOT, load_local_env, required
from qemu.run import ESP_DEVICE

sys.path.insert(0, str(ROOT / "tools/usb-stick"))
import persistence as p

spec = importlib.util.spec_from_file_location("usb_builder", ROOT / "tools/usb-stick/build.py")
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)


NVME = "--nvme" in sys.argv
BACKEND = next((name for name in ("nvme", "ahci", "ehci") if "--" + name in sys.argv), "xhci")
SUFFIX = "" if BACKEND == "xhci" else "-" + BACKEND
STOP = "--stop" in sys.argv
HUB = "--hub" in sys.argv
REFUSED = "PLOG EHCI port 1: a full-speed device; the medium must link at high speed"


def media(disk):
    """vmm_media BDF and QEMU arguments for the image."""
    if NVME:
        return "00:03.0", [
            "-drive",
            f"if=none,id=vmm-esp,format=raw,file={disk}",
            "-device",
            ESP_DEVICE,
        ]
    if BACKEND == "ehci":
        # The stick as a block node, so that it survives an unplug (--stop).
        stick = ["-device", "usb-storage,drive=stick,bus=plog.0,port=1,id=stick-dev,bootindex=1"]
        if HUB:
            stick = [
                "-device",
                "usb-hub,bus=plog.0,port=1",
                "-device",
                "usb-storage,drive=stick,bus=plog.0,port=1.1,id=stick-dev,bootindex=1",
            ]
        return "00:05.7", [
            "-device",
            "ich9-usb-ehci1,id=plog,addr=05.7",
            "-device",
            "ich9-usb-uhci1,masterbus=plog.0,firstport=0,addr=05.0,multifunction=on",
            "-device",
            "ich9-usb-uhci2,masterbus=plog.0,firstport=2,addr=05.1",
            "-device",
            "ich9-usb-uhci3,masterbus=plog.0,firstport=4,addr=05.2",
            "-blockdev",
            f"driver=file,filename={disk},node-name=stick-file",
            "-blockdev",
            "driver=raw,file=stick-file,node-name=stick",
            *stick,
            "-device",
            "usb-kbd,usb_version=1,bus=plog.0,port=3",
        ]
    controller, drive = {"xhci": ("qemu-xhci", "usb-storage"), "ahci": ("ich9-ahci", "ide-hd")}[
        BACKEND
    ]
    return "00:05.0", [
        "-device",
        f"{controller},id=plog,addr=05.0",
        "-drive",
        f"if=none,id=stick,format=raw,file={disk}",
        "-device",
        f"{drive},drive=stick,bus=plog.0,bootindex=1",
    ]


def backend_lines(who):
    """Start line (in the persistent log) and serial lines of the backend."""
    return {
        "xhci": (f"{who} xHCI BOT root-port=", []),
        "nvme": (f"{who} NVMe '", [f"{who} NVMe shutdown complete"]),
        "ahci": (f"{who} AHCI port=", [f"{who} AHCI standby complete"]),
        "ehci": (f"{who} EHCI BOT root-port=", []),
    }[BACKEND]


def qmp(root, *requests):
    """Run QMP commands on the VM's socket."""
    sock = socket.socket(socket.AF_UNIX)
    sock.connect(str(root / "qmp.sock"))
    stream = sock.makefile("rwb")
    json.loads(stream.readline())
    for request in [{"execute": "qmp_capabilities"}, *requests]:
        stream.write(json.dumps(request).encode() + b"\n")
        stream.flush()
        while True:
            response = json.loads(stream.readline())
            if "error" in response:
                raise RuntimeError(str(response))
            if "return" in response:
                break
    stream.close()
    sock.close()


def watched(proc, log, timeout, marks, act=None):
    """Wait for QEMU, noting the wall time at which each mark first shows in
    the serial log; act(text) runs at every look."""
    seen, deadline = {}, time.monotonic() + timeout
    while True:
        running = proc.poll() is None
        text = log.read_text(errors="replace") if log.exists() else ""
        for mark in marks:
            if mark not in seen and mark in text:
                seen[mark] = time.time()
        if not running:
            break
        if time.monotonic() > deadline:
            raise subprocess.TimeoutExpired(proc.args, timeout)
        if act:
            act(text)
        time.sleep(0.05)
    return seen, proc.stderr.read()


TRACE = [
    "-trace",
    "enable=usb_ehci_port_attach",
    "-trace",
    "enable=usb_ehci_opreg_write",
    "-msg",
    "timestamp=on",
]


def routing(trace):
    """(wall time, port index, owner, device) of each EHCI port attach and the
    times of CONFIGFLAG 0 writes, from QEMU's trace."""
    attaches, cleared = [], []
    for line in trace.read_text(errors="replace").splitlines():
        stamp, _, rest = line.partition(" ")
        try:
            at = datetime.fromisoformat(stamp.replace("Z", "+00:00")).timestamp()
        except ValueError:
            continue
        found = re.search(r"attach port #(\d+), owner (\w+), device (.*)$", rest)
        if found:
            attaches.append((at, int(found[1]), found[2], found[3]))
        if "usb_ehci_opreg_write" in rest and "[CONFIGFLAG]" in rest and rest.endswith("= 0x0"):
            cleared.append(at)
    return attaches, cleared


def portable_medium(disk, fragment=()):
    """Copying this image to larger media preserves its FAT32 partition.
    Move one interior cluster per selected file to exercise FAT traversal."""
    with disk.open("r+b") as f:
        f.seek(0, 2)
        f.truncate(f.tell() + 32 * 1024 * 1024)
        for name in fragment:
            metadata = {}
            item = p.fat_files(f, metadata)[name]
            fat_lba, fat_size, data_lba, spc, fats = metadata["geometry"]
            f.seek(fat_lba * 512)
            fat = f.read(fat_size * 512)
            free = next(
                i for i in range(2, len(fat) // 4) if struct.unpack_from("<I", fat, i * 4)[0] == 0
            )
            old = item.clusters[1]
            f.seek((data_lba + (old - 2) * spc) * 512)
            data = f.read(spc * 512)
            f.seek((data_lba + (free - 2) * spc) * 512)
            f.write(data)
            for copy in range(fats):
                for cluster, value in [
                    (item.clusters[0], free),
                    (free, item.clusters[2]),
                    (old, 0),
                ]:
                    f.seek((fat_lba + copy * fat_size) * 512 + cluster * 4)
                    f.write(struct.pack("<I", value))
            assert len(p.fat_files(f)[name].extents) > 1, "fixture was not fragmented"


def outside(disk, extents):
    """Digest of every byte of the image outside the given (LBA, count) extents."""
    digest = hashlib.sha256()
    with disk.open("rb") as f:
        at = 0
        for lba, count in sorted(extents):
            f.seek(at)
            digest.update(f.read(lba * 512 - at))
            at = (lba + count) * 512
        f.seek(at)
        digest.update(f.read())
    return digest.digest()


NVRAM_FIRMWARE = {
    "new": (
        "build/firmware-new/ia64-firmware-new.elf",
        "build/firmware-new/restart.efi",
        "build/firmware-new/hello.efi",
        "native",
    ),
}
UUID_A = "6a7f3c51-2d19-4c8e-9b51-3f0e72a4d618"
HOST_A = ["-smbios", f"type=1,uuid={UUID_A}"]
# An unset UUID: the identity is the SMBIOS strings.
HOST_B = [
    "-smbios",
    "type=1,manufacturer=VMMTest,product=NVRAM,serial=B-1,uuid=00000000-0000-0000-0000-000000000000",
]


def nvram_roundtrip(fmt):
    image, restart, hello, name = NVRAM_FIRMWARE[fmt]
    root = ROOT / "build/plog-qemu" / (f"nvram-{fmt}" + SUFFIX)
    tree = root / "tree"
    b.stage_bootmedia(ROOT, tree)
    for source, dest in [
        ("build/vmm.elf", "vmm.elf"),
        (image, "firmware.elf"),
        (restart, "restart.efi"),
        (hello, "hello.efi"),
    ]:
        shutil.copyfile(ROOT / source, tree / dest)
    disk = root / "stick.img"
    bdf, devices = media(disk)
    config = ["timeout: 0", "default_entry: 1", ""]
    for title, app in [("RESTART", "restart.efi"), ("HELLO", "hello.efi")]:
        config += [
            f"/{title}",
            "    protocol: limine",
            "    path: boot():/vmm.elf",
            f"    cmdline: exit=isa-debug-exit reserve_mib=128 engine=tier0 vmm_media={bdf} timeout_sec=1700",
        ]
        for file, module in [
            ("firmware.elf", "firmware"),
            (app, "bootdisk"),
            (f"nvram-{fmt}.bin", "nvram"),
        ]:
            config += [f"    module_path: boot():/{file}", f"    module_string: {module}"]
        config.append("")
    (tree / "limine.conf").write_text("\n".join(config))
    b.disk_image(tree, disk, pairs=3)
    portable_medium(
        disk, [p.nvram_slot_file(fmt, 0, side) for side in (0, 1)] if BACKEND == "xhci" else ()
    )
    with disk.open("rb") as f:
        manifest, _ = p.read_nvram(f, fmt)
    slots = [extent for pair in manifest["slots"] for file in pair for extent in file.extents]
    shutil.copyfile(required("OVMF_VARS"), root / "vars.fd")
    # Host A twice (the second run starts at the last phase), host B, host A
    # again. The native firmware's MonotonicCounter
    # changes a variable at every boot, so each of its runs writes; the
    # reference run 2 writes nothing.
    runs = [(1, "RESTART", HOST_A), (2, "RESTART", HOST_A)]
    runs += [(3, "HELLO", HOST_B), (4, "RESTART", HOST_A)]
    for run, entry, identity in runs:
        with disk.open("r+b") as f:
            files = p.fat_files(f)
            raw = p.read_file(f, files["limine.conf"])
            new = raw.replace(
                b"default_entry: 1", b"default_entry: " + (b"1" if entry == "RESTART" else b"2"), 1
            )
            new = new.replace(
                b"default_entry: 2", b"default_entry: " + (b"1" if entry == "RESTART" else b"2"), 1
            )
            p.write_file(f, files["limine.conf"], new)
        original = outside(disk, slots)
        with disk.open("rb") as f:
            before = {(q, s): r for q, s, r in p.read_nvram(f, fmt)[1]}
        log = root / f"serial-{run}.log"
        log.unlink(missing_ok=True)
        command = [
            os.environ.get("QEMU_X86", "qemu-system-x86_64"),
            "-machine",
            "q35",
            "-accel",
            "tcg",
            "-cpu",
            "max",
            "-m",
            "2048",
            "-smp",
            "4",
            *identity,
            "-drive",
            f"if=pflash,format=raw,readonly=on,file={required('OVMF_CODE')}",
            "-drive",
            f"if=pflash,format=raw,file={root / 'vars.fd'}",
            *devices,
            "-device",
            "isa-debug-exit,iobase=0xf4,iosize=0x04",
            "-no-reboot",
            "-serial",
            f"file:{log}",
            "-display",
            "none",
        ]
        started = time.monotonic()
        proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            _, error = proc.communicate(timeout=1800)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.communicate(timeout=15)
            raise RuntimeError(
                f"NVRAM {fmt} run {run}: QEMU timed out; see {log.relative_to(ROOT)}"
            )
        text = log.read_text(errors="replace")
        print(
            f"NVRAM QEMU {fmt} run {run} rc={proc.returncode} seconds={time.monotonic() - started:.1f}"
        )
        if error:
            print(error.decode(errors="replace"), file=sys.stderr)
        done = "RESTART PASS boots=7" if entry == "RESTART" else "hello from IA-64 EFI"
        if proc.returncode != 1 or done not in text:
            raise RuntimeError(
                f"NVRAM {fmt} run {run}: QEMU exit={proc.returncode}; see {log.relative_to(ROOT)}"
            )
        started, closed = backend_lines("NVRAM stick" if BACKEND in ("xhci", "ehci") else "NVRAM")
        assert started in text and all(line in text for line in closed), BACKEND + " backend"
        # Only this format's slots change on the stick.
        assert outside(disk, slots) == original, "write outside the NVRAM slots"
        with disk.open("rb") as f:
            after = {(q, s): r for q, s, r in p.read_nvram(f, fmt)[1]}
        written = re.findall(
            rf"NVRAM {name} pair (\d+) slot ([ab]): generation (\d+) written \(([^)]*)\)", text
        )
        changed = {k for k in after if before.get(k) != after[k]}
        if run == 1 or (identity == HOST_B and entry == "RESTART"):
            # Every reset source restarted inside the run; one write at its end.
            pair = 0 if identity == HOST_A else 1
            if identity == HOST_A:
                assert f"host identity: SMBIOS UUID {UUID_A}" in text, "host A identity"
            else:
                assert "host identity: SMBIOS strings 'VMMTest' 'NVRAM' 'B-1'" in text
            assert len(re.findall(r"\[info\] guest restart \d+ ", text)) == 6, "restarts in run 1"
            assert f"NVRAM {name} slots: no record for this host" in text
            assert written == [(str(pair), "a", "1", "a free pair")] and changed == {(pair, 0)}, (
                written,
                changed,
            )
            assert text.index("guest restart 6 ") < text.index("generation 1 written"), (
                "written before the last boot"
            )
            r = after[pair, 0]
            if identity == HOST_A:
                assert (r["kind"], r["identity"], r["sequence"]) == (
                    1,
                    uuid.UUID(UUID_A).bytes_le,
                    1,
                )
            else:
                assert r["kind"] == 2 and r["identity"].startswith(b"VMMTest\0NVRAM\0B-1\0")
        elif entry == "RESTART":
            # Host A continues where its last run ended.
            loaded = re.search(rf"NVRAM {name} pair 0 slot ([ab]): generation (\d+) loaded", text)
            assert (
                loaded and "RESTART boot phase=6" in text and "RESTART boot phase=0" not in text
            ), run
            assert "guest restart" not in text, "run restarted"
            assert (
                len(written) == 1 and written[0][0] == "0" and written[0][3] == "this host's pair"
            ), written
            assert written[0][1] != loaded.group(1) and changed == {(0, "ab".index(written[0][1]))}
        else:
            # Host B: another machine, its own free pair; host A's records stay.
            assert "host identity: SMBIOS strings 'VMMTest' 'NVRAM' 'B-1'" in text
            assert f"NVRAM {name} slots: no record for this host" in text
            assert written == [("1", "a", "1", "a free pair")] and changed == {(1, 0)}, (
                written,
                changed,
            )
            assert after[1, 0]["kind"] == 2 and after[1, 0]["identity"].startswith(
                b"VMMTest\0NVRAM\0B-1\0"
            )
    print(f"NVRAM QEMU {fmt}{'' if BACKEND == 'xhci' else ' ' + BACKEND} round trip passed")


def main():
    load_local_env()
    if "--nvram" in sys.argv:
        nvram_roundtrip("new")
        return
    crash = "--crash" in sys.argv
    discovery = "--discovery" in sys.argv
    if crash + discovery + STOP + HUB > 1 or ((STOP or HUB) and BACKEND != "ehci"):
        raise ValueError("choose one scenario")
    scenario = (
        "crash"
        if crash
        else "discovery"
        if discovery
        else "stop"
        if STOP
        else "hub"
        if HUB
        else "normal"
    )
    root = ROOT / "build/plog-qemu" / (scenario + SUFFIX)
    root.mkdir(parents=True, exist_ok=True)
    tree = root / "tree"
    b.stage_bootmedia(ROOT, tree)
    for source, dest in [
        ("build/vmm.elf", "vmm.elf"),
        ("build/firmware.elf", "firmware.elf"),
        ("build/efi/hello.efi", "hello.efi"),
    ]:
        shutil.copyfile(ROOT / source, tree / dest)
    modules = [
        ("firmware.elf", "firmware"),
        ("hello.efi", "bootdisk"),
        ("persistence-manifest.bin", "persistence"),
        ("log-a.bin", "log-a"),
        ("log-b.bin", "log-b"),
    ]
    disk = root / "stick.img"
    bdf, devices = media(disk)
    config = [
        "timeout: 0",
        "/PLOG",
        "    protocol: limine",
        "    path: boot():/vmm.elf",
        f"    cmdline: exit=isa-debug-exit reserve_mib=128 engine=tier0 vmm_media={bdf} media_log=1",
    ]
    for file, name in modules:
        config += [f"    module_path: boot():/{file}", f"    module_string: {name}"]
    if discovery:
        config[4] = config[4].replace(f"vmm_media={bdf}", "vmm_media=list")
    if crash:
        config[4] = config[4].replace("exit=isa-debug-exit", "exit=halt         ") + " selftest=ud"
    (tree / "limine.conf").write_text("\n".join(config) + "\n")
    b.disk_image(tree, disk)
    portable_medium(
        disk, ["log-a.bin", "log-b.bin"] if scenario == "normal" and BACKEND == "xhci" else ()
    )

    def outside_slots():
        with disk.open("rb") as f:
            m = p.read_manifest(f)
        return outside(
            disk, [extent for file in m["slots"] + [m["crash"]] for extent in file.extents]
        )

    original = outside_slots()
    whole = hashlib.sha256(disk.read_bytes()).digest()
    shutil.copyfile(required("OVMF_VARS"), root / "vars.fd")
    log = root / "serial.log"
    log.unlink(missing_ok=True)
    command = [
        os.environ.get("QEMU_X86", "qemu-system-x86_64"),
        "-machine",
        "q35",
        "-accel",
        "tcg",
        "-cpu",
        "max",
        "-m",
        "512",
        "-smp",
        "4",
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={required('OVMF_CODE')}",
        "-drive",
        f"if=pflash,format=raw,file={root / 'vars.fd'}",
        *devices,
        "-device",
        "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-serial",
        f"file:{log}",
        "-display",
        "none",
        "-qmp",
        f"unix:{root / 'qmp.sock'},server=on,wait=off",
    ]
    trace = root / "trace.log"
    trace.unlink(missing_ok=True)
    if BACKEND == "ehci":
        command += [*TRACE, "-D", str(trace)]
    (root / "qmp.sock").unlink(missing_ok=True)
    started_line = REFUSED if HUB else backend_lines("PLOG")[0]
    replugged = []

    def replug(text):
        # During the guest run: the stick out of port 1, into port 2.
        if STOP and not replugged and "[guest] " in text:
            qmp(root, {"execute": "device_del", "arguments": {"id": "stick-dev"}})
            time.sleep(0.5)
            qmp(
                root,
                {
                    "execute": "device_add",
                    "arguments": {
                        "driver": "usb-storage",
                        "drive": "stick",
                        "bus": "plog.0",
                        "port": "2",
                        "id": "stick-dev2",
                    },
                },
            )
            replugged.append(time.time())

    started = time.monotonic()
    load = os.getloadavg()
    proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        if crash:
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                if log.exists() and "VMM halted exit_code=1" in log.read_text(errors="replace"):
                    break
                if proc.poll() is not None:
                    raise RuntimeError("panic VM exited before warm reset")
                time.sleep(0.1)
            else:
                raise RuntimeError("panic did not halt")
            # CPU is halted and the media sink issued no failure-path writes.
            with disk.open("r+b", buffering=0) as f:
                assert p.read_logs(f) is None and p.read_logs(f, True) is None, "panic wrote media"
                files = p.fat_files(f)
                raw = p.read_file(f, files["limine.conf"])
                new = raw.replace(b"exit=halt         ", b"exit=isa-debug-exit").replace(
                    b"selftest=ud", b"          "
                )
                assert len(raw) == len(new)
                p.write_file(f, files["limine.conf"], new)
                os.fsync(f.fileno())
            original = outside_slots()
            qmp(root, {"execute": "system_reset"})
        seen, error = watched(proc, log, 180, [started_line, "VMM shutdown kind="], replug)
    except subprocess.TimeoutExpired:
        proc.terminate()
        _, error = proc.communicate(timeout=15)
        raise RuntimeError("QEMU timed out; retry under lower machine load")
    except Exception:
        if proc.poll() is None:
            proc.terminate()
            proc.communicate(timeout=15)
        raise
    print(
        f"PLOG QEMU rc={proc.returncode} seconds={time.monotonic() - started:.3f} load={load} end_load={os.getloadavg()}"
    )
    if error:
        print(error.decode(errors="replace"), file=sys.stderr)
    if proc.returncode != 1:
        raise RuntimeError(f"QEMU exit={proc.returncode}; see {log.relative_to(ROOT)}")
    assert outside_slots() == original, "write outside log files"
    if HUB:
        # Refused and kept: nothing reaches a companion or the stick after the refusal.
        text = log.read_text(errors="replace")
        attaches, cleared = routing(trace)
        begun = seen[started_line]
        assert (
            "PLOG unavailable" in text
            and "EHCI ports returned" not in text
            and "hello from IA-64 EFI" in text
        )
        assert not [c for c in cleared if c >= begun], "CONFIGFLAG 0 after the refusal"
        moved = [
            (port, device)
            for at, port, owner, device in attaches
            if at >= begun and owner == "comp"
        ]
        assert not moved, f"reached a companion: {moved}"
        assert hashlib.sha256(disk.read_bytes()).digest() == whole, "the stick was written"
        print(
            "PLOG QEMU EHCI hub passed: the full-speed hub was refused and stayed with the EHCI controller"
        )
        return
    if BACKEND == "ehci" and not crash:
        text = log.read_text(errors="replace")
        attaches, cleared = routing(trace)
        begun = seen[started_line]
        keyboard = [
            owner
            for at, _, owner, device in attaches
            if device == "QEMU USB Keyboard" and at < begun
        ]
        assert keyboard and keyboard[-1] == "comp", "keyboard not released to its companion"
        assert not [
            at
            for at, _, owner, device in attaches
            if device == "QEMU USB Keyboard" and at >= begun and owner == "ehci"
        ], "keyboard taken back from its companion"
        # The stick reaches a companion only after the shutdown hook's CONFIGFLAG 0.
        for at, port, owner, device in attaches:
            if device == "QEMU USB MSD" and at >= begun and owner == "comp":
                assert any(begun <= c <= at for c in cleared), f"stick on a companion port {port}"
        if STOP:
            assert replugged and "PLOG save/flush failed" in text, "backend did not stop"
            assert not [c for c in cleared if c >= begun] and "EHCI ports returned" not in text
            assert any(
                at >= replugged[0] and device == "QEMU USB MSD" and owner == "ehci" and port == 1
                for at, port, owner, device in attaches
            ), "replugged stick not seen"
            print("PLOG QEMU EHCI forced stop passed: the stick stayed with the EHCI controller")
            return
        assert [
            c for c in cleared if c >= begun
        ] and "PLOG EHCI ports returned to the companion controllers" in text
    with disk.open("rb") as f:
        result = p.read_logs(f)
    if discovery:
        assert result is None and "Controller discovery complete" in log.read_text()
        assert not any(
            f"PLOG {name}" in log.read_text() for name in ("xHCI", "NVMe", "AHCI", "EHCI")
        )
        print("PLOG discovery passed; no persistent record written")
        return
    if (
        not result
        or b"hello from IA-64 EFI" not in result[2]
        or b"VMM shutdown kind=" not in result[2]
    ):
        raise RuntimeError("missing guest/final text in persistent log")
    started, closed = backend_lines("PLOG")
    assert started.encode() in result[2] and all(line in log.read_text() for line in closed), (
        BACKEND + " backend"
    )
    for line in (ROOT / "tests/efi/hello/hello.golden").read_bytes().splitlines():
        assert b"[guest] " + line + b"\n" in result[2], "missing golden line: " + repr(line)
    if crash:
        with disk.open("rb") as f:
            manifest = p.read_manifest(f)
            records = [p.newest([p.read_file(f, manifest["crash"])], manifest["disk_id"])]
        assert any(r and r[1] and b"host exc:" in r[2] for r in records), "crash record missing"
    reader = [sys.executable, str(ROOT / "tools/usb-stick/read_logs.py"), str(disk)]
    if crash:
        reader.append("--crash")
    cli = subprocess.run(reader, check=True, capture_output=True)
    assert b"host exc:" in cli.stdout if crash else cli.stdout == result[2]
    print(f"PLOG QEMU round trip passed: generation={result[0]} bytes={len(result[2])}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as e:
        sys.exit(f"PLOG QEMU: {e}")
