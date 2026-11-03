#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Serial real-SATA-CD loader matrix; preserve every command, log and outcome."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from qemu.config import load_local_env, required
from qemu.run import memory_mib
from screen import edk2_menu_matches


def match_menu(path, words, firmware):
    return edk2_menu_matches(
        path,
        words,
        ROOT / "third_party/edk2-ipf/MdeModulePkg/Universal/Console/GraphicsConsoleDxe/LaffStd.c",
    )


def classify(serial, runner, status):
    # Only the last guest boot ends the run; earlier boots ended in a restart.
    restarts = list(re.finditer(r"^\[info\] guest restart \d+ [^\r\n]*", serial, re.M))
    if restarts:
        serial = serial[restarts[-1].end() :]
    stop = re.findall(r"guest exit reason=\d+ ip=\S+ slot=\d+ info=\S+[^\r\n]*", serial)
    ip = re.findall(r"vcpu\d+ IP=(\S+)", serial)
    host = re.findall(r"(?:panic:|host exc:)[^\r\n]*", serial + "\n" + runner)
    if host:
        detail, category = host[-1], "host-failure"
    elif "firmware panic" in serial:
        debug = re.findall(r"firmware debug ([0-9a-f]+)", serial)
        detail = "firmware IVT fatal: " + ", ".join(debug[-5:])
        category = "firmware-exception"
    elif stop:
        detail = stop[-1]
        category = "cpu-unimplemented" if "reason=3" in detail else "platform-access"
    elif (
        "guest timeout" in serial
        or "management watchdog stopped guest" in serial
        or "result=timeout" in serial
    ):
        detail, category = "guest execution deadline", "timeout"
    elif re.search(r"(?:QEMU|VMware) timeout after [^\r\n]+", runner):
        detail = re.findall(r"(?:QEMU|VMware) timeout after [^\r\n]+", runner)[-1]
        category = "timeout"
    elif "EFI ResetSystem" in serial:
        detail, category = "EFI image returned/reset", "firmware-return"
    else:
        detail, category = (
            runner.strip().splitlines()[-1] if runner.strip() else f"runner status {status}",
            "runner",
        )
    return {"classification": category, "stop": detail, "ip": ip[-1] if ip else None}


KEY_VALUES = {"down": (2, 0), "up": (1, 0), "ret": (0, 13), "spc": (0, 32)}


def observe_milestones(serial, elapsed, milestones):
    """Observed host wall seconds since row launch (includes staging)."""
    patterns = {
        "image_start": "Boot Manager: +trying Boot0000",
        "ebs": "EFI ExitBootServices succeeded",
        "kernel_entry": "EFI loader done",
        "linux_banner": "Linux version ",
        "serial_console": r"console \[ttyS0\] enabled",
        "sci_missing": "No IOSAPIC for GSI 9",
        "sci_allocation_failed": r"SCI .*allocation failed",
        "ahci_irq_missing": r"ahci .*no GSI",
        "ahci_completion_timeout": r"ata\d+\.\d+: qc timeout",
        "init": "Run /init as init process",
        "kernel_panic": "Kernel panic - not syncing",
        "stop": r"guest exit reason=|firmware panic|guest timeout|management watchdog stopped guest|result=timeout|panic:|host exc:",
    }
    for name, pattern in patterns.items():
        if name not in milestones and re.search(pattern, serial):
            milestones[name] = round(elapsed, 3)


def run_observed(command, runner_path, started, stop_at=None):
    """Read the runner's private serial file; never alter the shared runner.
    With stop_at, end the owned runner (SIGTERM: it closes only its own VM)
    a few seconds after that milestone, for matrices that only need it."""
    milestones = {}
    stop_deadline = None
    with runner_path.open("w") as runner:
        process = subprocess.Popen(command, cwd=ROOT, stdout=runner, stderr=subprocess.STDOUT)
        while process.poll() is None:
            output = runner_path.read_text(errors="replace")
            directories = re.findall(r"^run directory: (.+)$", output, re.M)
            if directories:
                live = Path(directories[-1]) / "serial.log"
                try:
                    observe_milestones(
                        live.read_text(errors="replace"), time.monotonic() - started, milestones
                    )
                except OSError:
                    pass  # Private staging/cleanup can race this observation.
            if stop_at and stop_at in milestones and stop_deadline is None:
                stop_deadline = time.monotonic() + 5
            if stop_deadline is not None and time.monotonic() >= stop_deadline:
                process.terminate()
                milestones["stopped_after"] = stop_at
                process.wait()
                break
            time.sleep(0.1)
    return process.returncode, milestones


def stop_state(serial):
    states = re.findall(r"vcpu\d+ IP=[^\r\n]+", serial)
    instructions = re.findall(r"^\[info\] (instruction [^\r\n]+)", serial, re.M)
    faults = re.findall(r"last [^\r\n]+ fault at [^\r\n]+", serial)
    return dict(
        cpu=states[-1] if states else None,
        instruction=instructions[-1] if instructions else None,
        last_fault=faults[-1] if faults else None,
        console=re.findall(r"^\[guest\] (.*)$", serial, re.M)[-12:],
    )


def consumed_keys(serial, keys):
    observed = [
        (int(scan, 16), int(char, 16))
        for scan, char in re.findall(r"EFI input key scan=([0-9a-f]+) unicode=([0-9a-f]+)", serial)
    ]
    at = 0
    for key in observed:
        if at < len(keys) and key == KEY_VALUES.get(keys[at]):
            at += 1
    return at == len(keys), observed


def tier1_statistics(serial):
    def last(pattern):
        matches = re.findall(pattern, serial)
        return int(matches[-1]) if matches else None

    failures = re.findall(r"\bbackend_failures=(\d+)", serial)
    return dict(
        tier1_ran=last(r"\btier1 linked=[^\r\n]*\bran=(\d+)"),
        hostwin_fills=last(r"\bhostwin instances=[^\r\n]*\bfills=(\d+)"),
        backend_failures=max(map(int, failures)) if failures else None,
    )


def accepted(result):
    tier1_ok = result.get("engine") != "tier1" or (
        (result.get("tier1_ran") or 0) > 0
        and (result.get("hostwin_fills") or 0) > 0
        and result.get("backend_failures") == 0
    )
    return (
        tier1_ok
        and result.get("classification") != "host-failure"
        and not result.get("binaries_changed", False)
        and result["menu"]
        and result["ebs"]
        and result["loader_done"]
        and result["screenshot_reviewed"]
        and (result["scenario"] != "scripted" or result.get("input_consumed", False))
    )


def kernel_entered(serial, media):
    if "EFI loader done" not in serial:
        return False
    entry = media.get("kernel_entry")
    return not entry or bool(re.search(r"EFI loader done ip=" + entry + r" slot=0\b", serial))


def png_present(path):
    if not path.is_file():
        return False
    data = path.read_bytes()
    return (
        len(data) >= 33
        and data.startswith(b"\x89PNG\r\n\x1a\n")
        and all(struct.unpack(">II", data[16:24]))
    )


def review_recorded(result, media):
    """Recheck saved artifacts; old booleans must not accept a GRUB module."""
    log = ROOT / result["log"]
    screen = ROOT / result["screenshot"]
    serial = log.read_text(errors="replace") if log.is_file() else ""
    visual, _ = match_menu(screen, media["menu"], result.get("firmware", "new"))
    keys_ok, keys_seen = consumed_keys(serial, media["keys"])
    result.update(
        menu=all(word in serial for word in media["menu"]),
        ebs="EFI ExitBootServices succeeded" in serial,
        loader_done=kernel_entered(serial, media),
        kernel_entry_expected=media.get("kernel_entry"),
        screenshot_present=png_present(screen),
        screenshot_reviewed=visual,
        input_consumed=keys_ok,
        input_keys=keys_seen,
    )
    runner_log = log.with_suffix(".runner.log")
    runner = runner_log.read_text(errors="replace") if runner_log.exists() else ""
    result.update(
        state=stop_state(serial),
        **tier1_statistics(serial),
        **classify(serial, runner, result.get("status")),
    )


def write_report(output, results):
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    lines = [
        "<!-- SPDX-License-Identifier: GPL-2.0-or-later -->",
        "# Real media loader results",
        "",
        "PNG menu acceptance requires the golden text in actual framebuffer pixels. "
        "Scripted input requires consumed ConIn keys in order.",
        "",
        "| Target/backend/engine/scenario | Console menu | EBS | Kernel entry | PNG menu | Keys consumed | Seconds | Stop |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for r in results:
        name = "/".join(r[k] for k in ["target", "backend", "engine", "scenario"])
        lines.append(
            f"| {name} | {r.get('menu', False)} | {r.get('ebs', False)} | "
            f"{r.get('loader_done', False)} | {r.get('screenshot_reviewed', False)} | "
            f"{r.get('input_consumed', 'unrecorded') if r['scenario'] == 'scripted' else 'n/a'} | "
            f"{r['seconds']:.1f} | {r['classification']}: {r['stop'].replace('|', '/')} |"
        )
    for r in results:
        lines += [
            "",
            f"`{shlex.join(r['command'])}`",
            "",
            f"Log: {r.get('log', 'missing')}; screenshot: {r.get('screenshot', 'missing')}.",
            "",
            f"EBS: {r.get('ebs_record')}; first PC: {r.get('first_pc')}; "
            f"kernel allocation entry: {r.get('kernel_entry')}.",
        ]
        if r.get("note"):
            lines += ["", r["note"]]
        if r.get("milestones"):
            lines += ["", f"Observed wall milestones (seconds): {r['milestones']}."]
        if r.get("load"):
            lines += ["", f"Load average (1/5/15): {r['load']}."]
        if r.get("engine") == "tier1":
            lines += [
                "",
                f"Tier1 ran={r.get('tier1_ran')}; hostwin fills={r.get('hostwin_fills')}; "
                f"backend_failures={r.get('backend_failures')}.",
            ]
        if r.get("state"):
            lines += ["", f"Final state: {r['state']}."]
    (output / "report.md").write_text("\n".join(lines) + "\n")


def main():
    load_local_env()
    media = json.loads(Path(required("IA64_MEDIA_MANIFEST")).read_text())
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--target", action="append", choices=list(media))
    p.add_argument("--backend", action="append", choices=["win-qemu", "vmware", "wsl-qemu"])
    p.add_argument("--engine", action="append", choices=["interp", "tier0", "tier1"])
    p.add_argument("--firmware", choices=["new"], default="new")
    p.add_argument(
        "--firmware-image",
        type=Path,
        help="diagnostic firmware ELF, staged only in the private ESP",
    )
    p.add_argument("--transport", choices=["sata-cd", "usb-disk", "usb-cdrom"], default="sata-cd")
    p.add_argument(
        "--machine",
        choices=["q35", "pc"],
        default="q35",
        help="win-qemu machine; pc shows VGA output that q35 without SMM hides",
    )
    p.add_argument(
        "--vga",
        choices=["std", "cirrus"],
        default="std",
        help="QEMU display device; cirrus shows VGA text after display=vbios",
    )
    p.add_argument(
        "--cdrom-controller",
        choices=["sata", "ide"],
        default="sata",
        help="sata-cd transport: AHCI (default) or an assigned PIIX4 IDE primary master",
    )
    p.add_argument(
        "--usb-image", type=Path, help="prepared raw stick image (one target, --transport usb-disk)"
    )
    p.add_argument("--usb-controller", choices=["xhci", "ehci"], default="xhci")
    p.add_argument(
        "--pcie-switch-depth",
        type=int,
        choices=[1, 2, 3],
        help="q35 sata-cd: the media AHCI behind N PCIe switches (four Pci() nodes = depth 1)",
    )
    p.add_argument("--scenario", choices=["auto", "scripted", "both"], default="both")
    p.add_argument("--guest-timeout", type=int, default=400)
    p.add_argument(
        "--key-gate",
        help="scripted: serial regex that starts the keys and the screenshot "
        "(default: the media gate); a GOP-drawn prompt such as Windows' cdboot.efi one "
        "never reaches the serial log, so use its load line, e.g. 'cdboot\\.efi'",
    )
    p.add_argument(
        "--debug-serial",
        choices=["off", "device", "console"],
        help="override row policy (Linux: console; Windows: off)",
    )
    p.add_argument("--memory", type=memory_mib, default=4096)
    p.add_argument(
        "--cpus", type=int, default=4, help="QEMU host CPUs: management, executors and helpers"
    )
    p.add_argument(
        "--cmdline-extra",
        default="",
        help="append guest diagnostics, e.g. trace=1 for loader PC samples",
    )
    p.add_argument(
        "--stop-at",
        choices=["ebs", "kernel_entry", "linux_banner", "serial_console", "init"],
        help="end each row five seconds after this observed milestone (owned VM only)",
    )
    p.add_argument(
        "--late-screenshot",
        action="store_true",
        help="also capture <log>.late.png ten seconds before the guest deadline",
    )
    p.add_argument(
        "--late-screenshot-after",
        type=float,
        metavar="SECONDS",
        help="with --late-screenshot: capture at this many seconds instead (e.g. before "
        "Linux's 600 s console blanking)",
    )
    p.add_argument(
        "--screenshot-every",
        type=float,
        metavar="SECONDS",
        help="QEMU: also capture every SECONDS into <log>.shots/tNNNNN.png (e.g. Windows bootvid and setup pages)",
    )
    p.add_argument("--output", type=Path, default=ROOT / "build/loaders")
    p.add_argument(
        "--report-only", action="store_true", help="record failures without claiming acceptance"
    )
    p.add_argument(
        "--resume", action="store_true", help="skip combinations already in results.json"
    )
    args = p.parse_args()
    if args.guest_timeout <= 0:
        p.error("--guest-timeout must be positive")
    if args.usb_image and (
        args.transport != "usb-disk" or not args.target or len(args.target) != 1
    ):
        p.error("--usb-image requires one --target and --transport usb-disk")
    if (
        args.transport == "usb-disk"
        and not args.usb_image
        and any(name != "gentoo" for name in (args.target or list(media)))
    ):
        p.error(
            "--transport usb-disk needs --target gentoo (hybrid GPT ISO) or --usb-image for a prepared stick"
        )
    if args.pcie_switch_depth and (
        args.transport != "sata-cd" or args.machine != "q35" or args.cdrom_controller != "sata"
    ):
        p.error("--pcie-switch-depth applies to the q35 sata-cd transport")
    if args.cdrom_controller != "sata" and args.transport != "sata-cd":
        p.error("--cdrom-controller applies to --transport sata-cd")
    if args.firmware_image:
        args.firmware_image = args.firmware_image.resolve()
        if not args.firmware_image.is_file() or not args.firmware_image.is_relative_to(ROOT):
            p.error("--firmware-image must be an existing ELF inside the repository")
    images = Path(required("IA64_IMAGES_DIR"))
    output = args.output.resolve()
    if not output.is_relative_to(ROOT):
        p.error("--output must be inside the repository")
    output.mkdir(parents=True, exist_ok=True)
    results = (
        json.loads((output / "results.json").read_text())
        if args.resume and (output / "results.json").exists()
        else []
    )
    for result in results:
        review_recorded(result, media[result["target"]])
    if results:
        write_report(output, results)
    binaries = {
        str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in [
            ROOT / "build/vmm.elf",
            args.firmware_image
            or ROOT
            / (
                "build/firmware-new/ia64-firmware-new.elf"
                if args.firmware == "new"
                else "build/firmware-new/ia64-firmware-new.elf"
            ),
        ]
    }

    def identity(r):
        return tuple(
            r.get(key, default)
            for key, default in [
                ("target", None),
                ("backend", None),
                ("engine", None),
                ("scenario", None),
                ("firmware", "new"),
                ("transport", "sata-cd"),
                ("usb_controller", "xhci"),
                ("cmdline_extra", ""),
                ("firmware_image", ""),
                ("cdrom_controller", "sata"),
                ("machine", "q35"),
                ("vga", "std"),
                ("cpus", 4),
            ]
        )

    firmware_image = str(args.firmware_image.relative_to(ROOT)) if args.firmware_image else ""
    extra_identity = (
        args.firmware,
        args.transport,
        args.usb_controller,
        args.cmdline_extra,
        firmware_image,
        args.cdrom_controller,
        args.machine,
        args.vga,
        args.cpus,
    )
    done = {identity(r): r for r in results}
    for name in args.target or list(media):
        m = media[name]
        debug_serial = args.debug_serial or m["debug_serial"]
        source = args.usb_image.resolve() if args.usb_image else images / m["iso"]
        with source.open("rb") as stream:
            media_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
        scenarios = (
            ["auto", "scripted"]
            if args.scenario == "both" and m["required"]
            else ["auto" if args.scenario == "both" else args.scenario]
        )
        for backend in args.backend or ["win-qemu", "vmware"]:
            for engine in args.engine or ["interp", "tier0"]:
                for scenario in scenarios:
                    ident = (name, backend, engine, scenario)
                    previous = done.get((*ident, *extra_identity))
                    if (
                        previous
                        and previous.get("debug_serial") == debug_serial
                        and accepted(previous)
                        and previous.get("binaries") == binaries
                        and previous.get("media_sha256") == media_sha256
                    ):
                        continue
                    prefix = output / "-".join(ident)
                    if args.transport != "sata-cd":
                        prefix = prefix.with_name(
                            prefix.name
                            + "-"
                            + args.firmware
                            + "-"
                            + args.transport
                            + "-"
                            + args.usb_controller
                        )
                    if args.cdrom_controller != "sata":
                        prefix = prefix.with_name(prefix.name + "-" + args.cdrom_controller)
                    if args.machine != "q35":
                        prefix = prefix.with_name(prefix.name + "-" + args.machine)
                    if args.vga != "std":
                        prefix = prefix.with_name(prefix.name + "-" + args.vga)
                    if args.cpus != 4:
                        prefix = prefix.with_name(prefix.name + f"-cpus{args.cpus}")
                    if args.firmware_image:
                        prefix = prefix.with_name(
                            prefix.name
                            + "-"
                            + args.firmware_image.parent.name
                            + "-"
                            + hashlib.sha256(firmware_image.encode()).hexdigest()[:8]
                        )
                    if args.cmdline_extra:
                        prefix = prefix.with_name(
                            prefix.name
                            + "-diag-"
                            + hashlib.sha256(args.cmdline_extra.encode()).hexdigest()[:8]
                        )
                    attempt = 1 + sum(identity(r) == (*ident, *extra_identity) for r in results)
                    prefix = prefix.with_name(prefix.name + f"-a{attempt}")
                    log, screen = prefix.with_suffix(".log"), prefix.with_suffix(".png")
                    for artifact in [
                        log,
                        screen,
                        prefix.with_suffix(".screen.txt"),
                        prefix.with_suffix(".late.png"),
                    ]:
                        artifact.unlink(missing_ok=True)
                    timeout = (
                        min(args.guest_timeout, m.get("auto_timeout", args.guest_timeout))
                        if scenario == "auto"
                        else args.guest_timeout
                    )
                    command = [
                        str(ROOT / "tools/qemu/run.sh"),
                        "--no-build",
                        "--backend",
                        backend,
                        {
                            "sata-cd": "--cdrom",
                            "usb-disk": "--usb-disk",
                            "usb-cdrom": "--usb-cdrom",
                        }[args.transport],
                        str(source),
                        "--firmware",
                        args.firmware,
                        "--usb-controller",
                        args.usb_controller,
                        "--memory",
                        str(args.memory),
                        "--cpus",
                        str(args.cpus),
                        "--cmdline",
                        f"reserve_mib={512 if engine == 'tier1' else 128} engine={engine} timeout_sec={timeout} code_validity=compare-always debug_serial={debug_serial}"
                        + (" " + args.cmdline_extra if args.cmdline_extra else "")
                        + (
                            ""
                            if any(
                                token.startswith("handoff_steps=")
                                for token in args.cmdline_extra.split()
                            )
                            else " handoff_steps=33554432"
                        ),
                        "--timeout",
                        str(timeout + 120),
                        "--log",
                        str(log),
                        "--screenshot",
                        str(screen),
                        "--screenshot-on",
                        args.key_gate or m["gate"],
                        "--screenshot-delay",
                        str(m.get("capture_delay", 2)),
                    ]
                    if m.get("capture_text") and backend != "vmware":
                        for text in m["menu"]:
                            command += ["--screenshot-text", text]
                    if args.firmware_image:
                        command += ["--firmware-image", str(args.firmware_image)]
                    if args.pcie_switch_depth:
                        command += [
                            "--pcie-switch",
                            "--pcie-switch-depth",
                            str(args.pcie_switch_depth),
                        ]
                    if args.late_screenshot:
                        # Shortly before the guest deadline: the post-EBS screen.
                        command += [
                            "--late-screenshot",
                            str(prefix.with_suffix(".late.png")),
                            "--late-screenshot-after",
                            str(args.late_screenshot_after or max(1, timeout - 10)),
                        ]
                    if args.screenshot_every:
                        command += [
                            "--screenshot-every",
                            str(args.screenshot_every),
                            "--screenshot-dir",
                            str(prefix.with_suffix(".shots")),
                        ]
                    if args.cdrom_controller != "sata":
                        command += ["--cdrom-controller", args.cdrom_controller]
                    if args.machine != "q35":
                        command += ["--machine", args.machine]
                    if args.vga != "std":
                        command += ["--vga", args.vga]
                    if scenario == "scripted":
                        command += [
                            "--sendkey-after",
                            args.key_gate or m["gate"],
                            "--sendkey-delay",
                            str(m.get("input_delay", 4)),
                        ]
                        for key in m["keys"]:
                            command += ["--sendkey", key]
                    print("RUN " + "/".join(ident), flush=True)
                    load = os.getloadavg()
                    print(f"load average (1/5/15): {load}", flush=True)
                    started = time.monotonic()
                    binaries = {
                        str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in [
                            ROOT / "build/vmm.elf",
                            args.firmware_image
                            or ROOT
                            / (
                                "build/firmware-new/ia64-firmware-new.elf"
                                if args.firmware == "new"
                                else "build/firmware-new/ia64-firmware-new.elf"
                            ),
                        ]
                    }
                    status, milestones = run_observed(
                        command, prefix.with_suffix(".runner.log"), started, args.stop_at
                    )
                    serial = log.read_text(errors="replace") if log.exists() else ""
                    runner = prefix.with_suffix(".runner.log").read_text(errors="replace")
                    visual_match, screen_text = match_menu(screen, m["menu"], args.firmware)
                    prefix.with_suffix(".screen.txt").write_text(screen_text + "\n")
                    result = dict(zip(["target", "backend", "engine", "scenario"], ident))
                    keys_ok, keys_seen = consumed_keys(serial, m["keys"])
                    result.update(
                        firmware_image=firmware_image,
                        cmdline_extra=args.cmdline_extra,
                        cdrom_controller=args.cdrom_controller,
                        machine=args.machine,
                        vga=args.vga,
                        cpus=args.cpus,
                        firmware=args.firmware,
                        transport=args.transport,
                        usb_controller=args.usb_controller,
                        command=command,
                        seconds=time.monotonic() - started,
                        status=status,
                        debug_serial=debug_serial,
                        guest_serial=str(Path(str(log) + ".guest-serial.log").relative_to(ROOT)),
                        menu=all(x in serial for x in m["menu"]),
                        ebs="EFI ExitBootServices succeeded" in serial,
                        loader_done=kernel_entered(serial, m),
                        kernel_entry_expected=m.get("kernel_entry"),
                        screenshot_present=png_present(screen),
                        screenshot_reviewed=visual_match,
                        keys_sent=re.findall(
                            r"(?:win-qemu|wsl-qemu|vmware): sendkey (\S+)", runner
                        ),
                        input_consumed=keys_ok,
                        input_keys=keys_seen,
                        binaries=binaries,
                        media_sha256=media_sha256,
                        ebs_record=re.findall(r"EFI ExitBootServices succeeded[^\r\n]*", serial),
                        first_pc=re.findall(r"EFI first IP after ExitBootServices[^\r\n]*", serial),
                        kernel_entry=re.findall(r"EFI loader done[^\r\n]*", serial),
                        log=str(log.relative_to(ROOT)),
                        screenshot=str(screen.relative_to(ROOT)),
                        milestones=milestones,
                        state=stop_state(serial),
                        load=load,
                        **tier1_statistics(serial),
                        **classify(serial, runner, status),
                    )
                    result["binaries_changed"] = any(
                        hashlib.sha256((ROOT / path).read_bytes()).hexdigest() != digest
                        for path, digest in binaries.items()
                    )
                    if prefix.with_suffix(".late.png").is_file():
                        result["late_screenshot"] = str(
                            prefix.with_suffix(".late.png").relative_to(ROOT)
                        )
                    if "note" in m:
                        result["note"] = m["note"]
                    results.append(result)
                    write_report(output, results)
                    print(
                        json.dumps(
                            {
                                k: result[k]
                                for k in [
                                    "target",
                                    "backend",
                                    "engine",
                                    "scenario",
                                    "seconds",
                                    "menu",
                                    "ebs",
                                    "loader_done",
                                    "stop",
                                ]
                            }
                        ),
                        flush=True,
                    )
    latest = {identity(r): r for r in results}
    failures = [
        r
        for r in latest.values()
        if media[r["target"]]["required"]
        and r.get("binaries") == binaries
        and r.get("firmware", "new") == args.firmware
        and r.get("transport", "sata-cd") == args.transport
        and (not args.target or r["target"] in args.target)
        and (not args.backend or r["backend"] in args.backend)
        and (not args.engine or r["engine"] in args.engine)
        and not accepted(r)
    ]
    print(
        f"{len(results)} recorded runs; {len(failures)} required rows lack full acceptance; {output / 'report.md'}"
    )
    return 0 if args.report_only or not failures else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"test-loaders: {error}", file=sys.stderr)
        sys.exit(1)
