#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""QEMU TCG checks of display=vbios.

QEMU `-machine pc`, TCG `-cpu max` (AMD SVM with NPT, no NRIPS), two VMs
at a time by default (--jobs 1 serializes), with load recorded per batch:

  std     std-vga: SeaVGABIOS from the ROM BAR, its DISPI ports forwarded, mode 3
  cirrus  cirrus-vga: the same with a card whose mode state lives in
          3B0-3DFh; live screenshot of the VMM log in VGA text
  cli     vbios_selftest=cli-loop: `cli; jmp $` aborted by the bound
  nosvm   -cpu max,-svm: the no-SVM line, then setup stops
  gueststub  vbios_selftest=guest-stub: the guest's
          int 1Ah/13h stub run on the CPU, 15 calls compared with the
          VMM's int 1Ah
  reset   vbios_reset=sbr: -machine q35, Cirrus behind its
          own pcie-root-port; Secondary Bus Reset, restore, then POST
  rootbus vbios_reset=sbr with Cirrus on the root bus (-machine pc): no
          bridge and no FLR, so the reset is refused and nothing POSTs
  trace   vbios_trace_io=8 on Cirrus: no I/O BAR, nothing to trace, POST

--broken builds the VMM without V_INTR_MASKING (test-only define) and runs
the cli case, which must hang until the runner's timeout; --broken
gueststub builds it with a broken find-device frame offset in the guest
stub, and the selftest must report the difference. The normal VMM is
rebuilt afterwards. Logs and screenshots go to build/vbios-qemu/.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/vbios-qemu"
BASE = "exit=isa-debug-exit reserve_mib=128 display=vbios"
CASES = {
    "guesttrace": ["--cmdline", BASE + " vbios_trace_guest=1 vbios_selftest=guest-trace"],
    "std": ["--cmdline", BASE, "--screenshot", "build/vbios-qemu/vbios-std.png"],
    "cirrus": [
        "--vga",
        "cirrus",
        "--cmdline",
        BASE,
        "--screenshot",
        "build/vbios-qemu/vbios-cirrus.png",
        "--screenshot-on",
        "VMM console: VGA text",
        "--screenshot-delay",
        "0.3",
    ],
    "cli": ["--cmdline", BASE + " vbios_selftest=cli-loop vbios_post_ms=2000"],
    "nosvm": ["--cpu-flags=-svm", "--cmdline", BASE],
    "gueststub": ["--vga", "cirrus", "--cmdline", BASE + " vbios_selftest=guest-stub"],
    "reset": [
        "--machine",
        "q35",
        "--vga",
        "cirrus",
        "--vga-root-port",
        "--cmdline",
        BASE + " vbios_reset=sbr",
        "--screenshot",
        "build/vbios-qemu/vbios-reset.png",
        "--screenshot-on",
        "VMM console: VGA text",
        "--screenshot-delay",
        "0.3",
    ],
    "rootbus": ["--vga", "cirrus", "--cmdline", BASE + " vbios_reset=sbr"],
    "trace": ["--vga", "cirrus", "--cmdline", BASE + " vbios_trace_io=8"],
}


def load():
    return Path("/proc/loadavg").read_text().split()[0]


def run(name, timeout=300):
    log = OUT / f"vbios-{name}.log"
    machine = [] if "--machine" in CASES[name] else ["--machine", "pc"]
    command = (
        [str(ROOT / "tools/qemu/run.sh"), "--no-build", "--backend", "wsl-qemu", "--tcg"]
        + machine
        + ["--timeout", str(timeout), "--log", str(log)]
        + CASES[name]
    )
    started = time.monotonic()
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    (OUT / f"vbios-{name}.runner").write_text(result.stdout + result.stderr)
    return (
        name,
        result.returncode,
        log.read_text(errors="replace") if log.exists() else "",
        time.monotonic() - started,
    )


def need(text, pattern, what, problems):
    match = re.search(pattern, text, re.M)
    if not match:
        problems.append(f"missing: {what}")
    return match


def check(name, status, text):
    problems = []
    if name == "reset":
        for pattern, what in [
            (
                r"VBIOS reset: Secondary Bus Reset on bridge (\S+) \(bus (\w+)\): only the GPU",
                "SBR on the root port",
            ),
            (
                r"VBIOS reset: bridge \S+ control \w+ -> \w+ \(Secondary Bus Reset\) for \d+ ms, released",
                "assert and release",
            ),
            (
                r"VBIOS reset: (link active \d+ ms after release|no Link Active reporting)",
                "link wait",
            ),
            (r"VBIOS reset: \S+ answers 1013:00b8 \d+ ms after release", "Cirrus answers"),
            (r"VBIOS reset: \S+ restored \d+ register\(s\).*IDs and BARs as saved", "restored"),
            (r"VBIOS mode set returned in \d+ ms: BDA video mode 03", "mode 3 after the reset"),
            (r"VMM console: VGA text 80x25 at b8000", "console switch"),
        ]:
            match = need(text, pattern, what, problems)
            if match:
                print(f"  reset: {match.group(0)}")
        for line in re.findall(r"^\[\w+\] VBIOS reset: .*$", text, re.M):
            print(f"  {line}")
    elif name == "rootbus":
        need(
            text,
            r"VBIOS reset: refused: \S+ is on a root bus, no bridge above it, and \S+ does not advertise "
            r"Function Level Reset",
            "refusal",
            problems,
        )
        need(text, r"vbios_reset=sbr did not reset \S+; no POST", "no POST", problems)
        if "VBIOS POST: call far" in text:
            problems.append("POSTed without the reset")
    elif name == "trace":
        need(
            text, r"VBIOS trace: the card has no I/O BAR; nothing to trace", "no I/O BAR", problems
        )
        need(text, r"VBIOS mode set returned in \d+ ms: BDA video mode 03", "mode 3", problems)
    elif name in ("std", "cirrus"):
        post = need(text, r"VBIOS POST returned in (\d+) ms", "POST returned", problems)
        mode = need(
            text, r"VBIOS mode set returned in (\d+) ms: BDA video mode 03", "mode 3", problems
        )
        need(text, r"VMM console: VGA text 80x25 at b8000", "console switch", problems)
        need(
            text,
            r"SVM container closed: EFER=\w+ VM_HSAVE_PA=\w+ restored",
            "MSRs restored",
            problems,
        )
        need(
            text,
            r"VBIOS config read 00:02\.0 reg 00 size 2",
            "card config read via CF8/CFC",
            problems,
        )
        if name == "std":
            if re.search(r"VBIOS port 01c[ef].*denied", text):
                problems.append("the assigned Bochs VGA VBE ports were denied")
        if re.search(r"nested page fault|VBIOS (POST|mode set) failed", text):
            problems.append("POST or mode set failed")
        if post and mode:
            print(f"  {name}: POST {post.group(1)} ms, mode set {mode.group(1)} ms")
        for line in re.findall(r"^\[info\] VBIOS (?:exits:|port ).*$", text, re.M):
            print(f"  {name}: {line[7:]}")
    elif name == "cli":
        aborted = need(
            text,
            r"VBIOS selftest cli-loop: aborted by the 2000 ms bound after \d+ exits \((\d+) INTR\)",
            "cli-loop aborted by the bound",
            problems,
        )
        if aborted and int(aborted.group(1)) < 50:
            problems.append(f"only {aborted.group(1)} INTR exits in 2 s")
        if aborted:
            print(f"  cli: {aborted.group(0)}")
    elif name == "guesttrace":
        need(
            text,
            r"VBIOS selftest guest-trace: 10 calls preserve registers, FLAGS and stack",
            "trace wrapper matches direct BIOS calls",
            problems,
        )
        need(
            text,
            r"INT10 container0 in  AX=4f02 BX=1234 CX=5678 DX=9abc F=0603",
            "input registers and caller FLAGS observed",
            problems,
        )
        need(
            text,
            r"INT10 container0 out AX=004f BX=1234 CX=5678 DX=9abc F=0202",
            "output registers and returned FLAGS observed",
            problems,
        )
        need(
            text,
            r"VBE 0140 640x480x32 pitch=2560 attr=009b fb=e0000000",
            "ModeInfoBlock fields captured from the BIOS buffer",
            problems,
        )
        live = text.split("VBIOS selftest guest-trace:", 1)[0]
        sequences = [
            int(n) for n in re.findall(r"INT10 container0 (?:in |out) AX=[^\n]* seq=(\d+)", live)
        ]
        if not sequences or sequences != list(range(1, len(sequences) + 1)):
            problems.append("packet sequence does not advance once per emitted packet")
    elif name == "gueststub":
        need(
            text,
            r"VBIOS selftest guest-stub: 15 calls on the real CPU match the PCI BIOS specification",
            "all 15 guest stub calls match",
            problems,
        )
        for line in re.findall(r"^\[\w+\] VBIOS guest stub .*$", text, re.M):
            print(f"  gueststub: {line}")
    elif name == "nosvm":
        need(
            text,
            r"display=vbios: no SVM: CPUID 8000_0001h ECX\.SVM is clear",
            "no-SVM line",
            problems,
        )
        if "SVM container" in text or "VBIOS card" in text:
            problems.append("acted without SVM")
    expected_status = 4 if name in ("cli", "nosvm", "rootbus") else 0
    need(
        text,
        rf"VMM shutdown kind=\d+ exit_code={expected_status}(?:\s|$)",
        f"VMM exit {expected_status}",
        problems,
    )
    if expected_status == 4:
        if "EFI ResetSystem" in text:
            problems.append("guest ran after VBIOS setup stopped")
        if "VMM console: VGA text" in text:
            problems.append("switched console after VBIOS setup stopped")
    else:
        need(text, r"EFI ResetSystem type=2 status=0", "guest ran to EFI_SUCCESS", problems)
    if status != expected_status:
        problems.append(f"runner status {status}, expected {expected_status}")
    return problems


def broken_guest_stub():
    """The guest stub's find device reads the vendor from the wrong frame
    slot: the selftest must name the B102h call that differs."""
    subprocess.run(
        ["make", "vmm", "CFLAGS=-DIA64VMM_VBIOS_TEST_BROKEN_GUEST_STUB"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    try:
        print(f"load before: {load()}")
        name, status, text, seconds = run("gueststub", timeout=300)
        print(f"load after: {load()}")
    finally:
        subprocess.run(["make", "vmm"], cwd=ROOT, check=True, capture_output=True)
    differs = re.findall(r"^\[error\] VBIOS guest stub int 1Ah AX=(\w+): .* DIFFERS", text, re.M)
    caught = "VBIOS selftest guest-stub FAILED" in text and differs
    print(
        f"broken gueststub: {seconds:.0f} s, differing calls {differs}: "
        f"{'caught' if caught else 'NOT caught'}"
    )
    return 0 if caught else 1


def broken():
    """V_INTR_MASKING clear: the guest's CLI masks host interrupts, the
    cli loop never exits, and the bound can never fire."""
    subprocess.run(
        ["make", "vmm", "CFLAGS=-DIA64VMM_SVM_TEST_NO_VINTR_MASKING"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    try:
        print(f"load before: {load()}")
        name, status, text, seconds = run("cli", timeout=150)
        print(f"load after: {load()}")
    finally:
        subprocess.run(["make", "vmm"], cwd=ROOT, check=True, capture_output=True)
    hung = "SVM container:" in text and "aborted by the" not in text and "VMM shutdown" not in text
    print(
        f"broken cli: runner status {status} after {seconds:.0f} s, "
        f"{'hung inside the container as expected' if hung else 'did NOT hang'}"
    )
    return 0 if hung and status != 0 else 1


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--jobs", type=int, choices=[1, 2], default=2, help="concurrent guests (default: 2)"
    )
    parser.add_argument(
        "--broken", action="store_true", help="run the V_INTR_MASKING broken variant instead"
    )
    parser.add_argument("cases", nargs="*", metavar="CASE", help=f"subset of {', '.join(CASES)}")
    args = parser.parse_args()
    if any(case not in CASES for case in args.cases):
        parser.error(f"cases are {', '.join(CASES)}")
    OUT.mkdir(parents=True, exist_ok=True)
    if args.broken:
        return broken_guest_stub() if args.cases == ["gueststub"] else broken()
    failed = False
    cases = args.cases or list(CASES)
    for pair in (cases[i : i + args.jobs] for i in range(0, len(cases), args.jobs)):
        print(f"load before {pair}: {load()}")
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(run, pair))
        print(f"load after {pair}: {load()}")
        for name, status, text, seconds in results:
            problems = check(name, status, text)
            print(f"{name}: {'ok' if not problems else 'FAILED'} ({seconds:.0f} s wall)")
            for problem in problems:
                print(f"  {problem}")
            failed |= bool(problems)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
