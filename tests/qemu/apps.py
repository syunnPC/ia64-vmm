#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot EFI test applications through the VMM on QEMU.

Check application results, executor milestones and Tier1 statistics. Deliberately
broken guests and log copies must fail their corresponding checks. Guest RAM
starts filled with ff. See docs/testing.md for coverage and prerequisites.

Use --list to list cases, --only to select them and --full for extended coverage.
--jobs bounds concurrent guests; their QEMU CPUs must fit the host affinity,
except that a larger case may run alone. Use --jobs 1 on a shared machine.
"""

import argparse
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from contextlib import closing
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/hosted"))
sys.path.insert(0, str(ROOT / "tests/smp"))
import milestones  # noqa: E402
import smplog  # noqa: E402
import tier1stats  # noqa: E402

LOGS = ROOT / "build/tests/qemu-apps"
BASE = "exit=isa-debug-exit reserve_mib=128 guest_ram_fill=ff"
SHUTDOWN = "EFI ResetSystem type=2 status=0"
RELEASE_FIRMWARE = ROOT / "build/firmware-new/release/ia64-firmware-new.elf"
# EDK2 DEBUG output of the debug build; the release build compiles it out.
EDK2_DEBUG_LINE = "CoreInitializeMemoryServices:"
# The stall guard stops after 2^20 deliveries (ia64/interrupt.c), not a
# timer. Require that work to finish within 60 seconds,
# while the independent 120 s watchdog must never be what stops this case.
FAULTLOOP_WALL_MS = 60000

APP_IDENTITIES = {
    "hello": "IA64-VMM: hello from IA-64 EFI",
    "faultloop": "fault loop: rfi to an unmapped entry",
    "ebsloop": "ebsloop:",
    "cpudesc-1": "CPUDESC BEGIN expected=1",
    "cpudesc-2": "CPUDESC BEGIN expected=2",
    "legacymap": "LEGACYMAP PASS GetMemoryMap",
    "sratmap": "SRATMAP BEGIN",
    "sratmap-broken-hole": "SRATMAP BEGIN",
    "smp-ipi-itc": "SMP-APP BEGIN profile=ipi-itc",
    "smp-tlb-purge": "SMP-APP BEGIN profile=tlb-purge",
    "smp-long": "SMP-APP BEGIN profile=long",
    "smp-quick": "SMP-APP BEGIN profile=quick",
    "smp-broken-wake-vector": "SMP-APP BEGIN profile=broken-wake-vector",
    "UefiBehaviour": "UEFI behaviour: RTC, variables, authenticated rejection, ITM events, Stall, PAL idle passed",
    "PalProbe": "PAL probe:",
    "FpswaProbe": "FPSWA probe: protocol located",
    "GateHello": "gate EDK2 HelloWorld",
    "GateLauncher": "gate EDK2 HelloWorld",
    "debugserial": "debugserial mode=",
    "benchwrap": "BENCH-START",
    "benchwrap-keep-psr-i": "BENCH-START",
    "initpmi": "INITPMI BEGIN",
    "initpmi-broken-resume": "INITPMI BEGIN",
}
# initpmi.efi: every case passes, and the VMM entered
# PALE_INIT, PALE_PMI twice and PALE_CHECK twice on the BSP.
INITPMI_CASES = ("init", "pmi-sal", "pmi-pal", "mca", "purge")


class Case:
    def __init__(
        self,
        name,
        app,
        cmdline,
        rc=0,
        firmware="new",
        needs=(),
        forbids=(),
        milestone_names=("exit",),
        check=None,
        expect_problems=False,
        timeout=120,
        must_reject=None,
        memory=None,
        cpus=4,
        firmware_image=None,
        setup=False,
        log_check=None,
        release=False,
        runner_args=(),
    ):
        self.name, self.app, self.firmware, self.rc = name, app, firmware, rc
        self.cmdline = f"{BASE} {cmdline}".strip()
        self.needs, self.forbids, self.milestone_names = list(needs), list(forbids), milestone_names
        self.check_extra, self.expect_problems, self.timeout = check, expect_problems, timeout
        self.must_reject, self.memory = must_reject, memory
        self.cpus, self.firmware_image, self.setup, self.log_check = (
            cpus,
            firmware_image,
            setup,
            log_check,
        )
        # The release build of the new firmware (make firmware-new) instead of the debug build.
        self.release_image = RELEASE_FIRMWARE if release else None
        self.runner_args = list(runner_args)
        if firmware_image:
            self.needs.append(f"SMP AP fixture {firmware_image.stem} BEGIN")
        elif not setup:
            self.needs.append("IPL: validated handoff")
            identity = APP_IDENTITIES.get(app)
            if app.startswith("cpudesc-"):
                identity = f"CPUDESC BEGIN expected={app.split('-')[-1]}"
            if identity:
                self.needs.append(identity)


def snapshot_budget_problems(user_mib):
    def check(log):
        reserve = re.search(r"VMM reserve total_bytes=(\d+) used_bytes=(\d+) free_bytes=(\d+)", log)
        budget = re.search(r"code snapshot budget_bytes=(\d+) policy=(user|default)", log)
        if not reserve or not budget:
            return ["snapshot reserve accounting missing"]
        total, used, free = map(int, reserve.groups())
        late = re.search(r"code snapshot late_reserve_bytes=(\d+)", log)
        remaining = max(0, free - (int(late[1]) if late else 0))
        expected = (
            user_mib << 20 if user_mib else max(8192, min(48 << 20, (remaining // 2) & ~4095))
        )
        policy = "user" if user_mib else "default"
        if total != used + free or int(budget[1]) != expected or budget[2] != policy:
            return ["snapshot reserve budget differs from policy"]
        return []

    return check


def guest_text(log):
    return "".join(
        line[len("[guest] ") :] + "\n"
        for line in log.replace("\r", "").split("\n")
        if line.startswith("[guest] ")
    )


def problems(case, rc, log, elapsed=None):
    found = []
    if rc != case.rc:
        found.append(f"runner status {rc}, expected {case.rc}")
    statuses = re.findall(r"^\[info\] VMM shutdown.*?exit_code=(\d+)", log.replace("\r", ""), re.M)
    if statuses != [str(case.rc)]:
        found.append(f"guest shutdown status {statuses}, expected [{case.rc}]")
    guest = guest_text(log)
    for needle in case.needs:
        if needle not in log and needle not in guest:
            found.append(f"missing: {needle}")
    for needle in case.forbids:
        if needle in log or needle in guest:
            found.append(f"present: {needle}")
    try:
        if not case.setup:
            records = milestones.check(
                log.replace("\r", ""), required=case.milestone_names, runner_seconds=elapsed
            )
            if case.name.startswith("faultloop-") and records[-1]["wall_ms"] >= FAULTLOOP_WALL_MS:
                found.append(
                    f"faultloop executor wall_ms={records[-1]['wall_ms']}, expected <{FAULTLOOP_WALL_MS}"
                )
    except milestones.MilestoneError as error:
        found.append(f"milestone lines: {error}")
    # Only engine=tier1 runs Tier1 code.
    engine = re.search(r"\bengine=(\w+)", case.cmdline)[1]
    try:
        if not case.setup:
            tier1stats.check(log.encode(), engine)
    except tier1stats.Tier1StatsError as error:
        found.append(f"tier1 statistics: {error}")
    if engine == "lockstep" and not case.setup:
        coverage = re.findall(
            r"^\[info\] tier0 regions=.*? checked=(\d+) unchecked=(\d+)", log, re.M
        )
        if not coverage or any(
            int(checked) == 0 or int(unchecked) != 0 for checked, unchecked in coverage
        ):
            found.append("lockstep coverage: require checked>0 and unchecked==0")
    if case.check_extra:
        found += case.check_extra(guest)
    if case.log_check:
        found += case.log_check(log)
    return found


def smp_problems(profile, cpus, broken=None):
    def check(guest):
        try:
            result = smplog.parse(guest.split("\n"))
        except smplog.LogError as error:
            return [f"smp.efi output: {error}"]
        if result["profile"] != profile:
            return [f"smp.efi profile {result['profile']}, expected {profile}"]
        if result["summary"]["cpus"] != cpus:
            return [f"smp.efi saw {result['summary']['cpus']} CPUs, expected {cpus}"]
        if broken:
            errors = smplog.check_broken(result, broken)
            ordinary = smplog.expected("vmm", cpus, "ipi-itc")
            expected = {
                name: value for name, value in ordinary.items() if name in smplog.BOOT_CASES
            }
            for name in smplog.BROKEN[broken]:
                expected[name] = ("FAIL", smplog.BROKEN[broken][name], "")
            return errors + smplog.check(result, expected)
        return smplog.check(result, smplog.expected("vmm", cpus, profile))

    return check


RESTART_LINE = re.compile(r"^\[info\] guest restart \d+ [^\n]*$", re.M)


def boots(log):
    """The log of each guest boot: split at the restart lines."""
    return RESTART_LINE.split(log.replace("\r", ""))


def restart_problems(count, smp_cpus=None, tier1=False):
    """In-VMM restarts: count guest boots, every reset
    source of restart.efi or the SMP cases on every boot, the same VMM
    reserve use at each boot's start (no leak), and Tier1 compiling and
    entering regions again in every boot."""

    def check(log):
        found = []
        parts = boots(log)
        if len(parts) != count:
            return [f"restart: {len(parts)} guest boots, expected {count}"]
        reserve = re.findall(
            r"^\[info\] VMM reserve total_bytes=\d+ used_bytes=(\d+) free_bytes=\d+$",
            log.replace("\r", ""),
            re.M,
        )
        if len(reserve) != count or len(set(reserve)) != 1:
            found.append(f"restart: VMM reserve use at boot start differs: {reserve}")
        previous = (0, 0)
        for at, part in enumerate(parts, 1):
            if smp_cpus:
                found += [
                    f"boot {at}: {error}"
                    for error in smp_problems("ipi-itc", smp_cpus)(guest_text(part))
                ]
            if tier1:
                lines = re.findall(
                    r"^\[info\] tier1 linked=1 compiles=(\d+) rejects=\d+ entries=(\d+)", part, re.M
                )
                now = tuple(map(int, lines[-1])) if lines else previous
                if now[0] <= previous[0] or now[1] <= previous[1]:
                    found.append(
                        f"restart: boot {at} compiled or entered no Tier1 region ({previous} -> {now})"
                    )
                previous = now
        if not smp_cpus:
            sources = [
                "source=efi type=cold",
                "source=efi type=warm",
                "source=efi type=platform status=8000000000000015",
                "source=acpi type=cold",
                "source=cf9 type=cold",
                "source=i8042 type=cold",
            ]
            lines = RESTART_LINE.findall(log.replace("\r", ""))
            if [s for s in sources if not any(s in line for line in lines)] or len(lines) != len(
                sources
            ):
                found.append(f"restart: reset sources {lines}")
            for phase in range(1, count):
                if f"RESTART read back phase={phase}" not in guest_text(parts[phase]):
                    found.append(
                        f"restart: boot {phase + 1} did not read back the variable of boot {phase}"
                    )
        return found

    return check


def initpmi_problems(log):
    """One PALE_INIT, two PALE_PMI and two PALE_CHECK entries on vCPU0."""
    counts = {
        kind: len(re.findall(rf"^\[info\] vCPU0 {kind} at ip=", log, re.M))
        for kind in ("INIT", "PMI", "machine check")
    }
    expected = {"INIT": 1, "PMI": 2, "machine check": 2}
    return (
        []
        if counts == expected
        else [f"initpmi: PAL-based interruptions {counts}, expected {expected}"]
    )


def cpumodel_cases(counts):
    """Keep the same PAL/SMBIOS checks in the default and extended budgets."""
    out = []
    for firmware in ("new",):
        for model in ("montecito", "madison"):
            for count in counts:
                app = f"cpumodel-{model}-{count}"
                name = f"{app}-{firmware}" if count != 1 else f"cpumodel-{model}-{firmware}"
                out.append(
                    Case(
                        name,
                        app,
                        f"engine=tier0 cpu_model={model} vcpus={count} smp_serialize=1",
                        firmware=firmware,
                        needs=[
                            "CPU MODEL PASSED:",
                            "CPU MODEL SMBIOS PASSED",
                            "CPU MODEL PAL TOPOLOGY PASSED",
                            SHUTDOWN,
                        ],
                        forbids=["CPU MODEL FAILED", "CPU MODEL PAL TOPOLOGY FAILED"],
                        cpus=count + 1,
                    )
                )
    return out


def budget_cases(full=False):
    out = []
    for engine in ("interp", "tier0", "lockstep"):
        out.append(
            Case(
                f"faultloop-{engine}",
                "faultloop",
                f"engine={engine} timeout_sec=120",
                rc=3,
                needs=[
                    "fault loop: rfi to an unmapped entry",
                    "1048576 interruption deliveries without a retired instruction",
                    "guest exit reason=4 ip=4000000056780c00",
                    "vcpu0 IP=4000000056780c00",
                ],
            )
        )
    out.append(
        Case(
            "ebsloop",
            "ebsloop",
            "engine=interp handoff_steps=1000",
            milestone_names=("ebs", "loader_resumed", "exit"),
            forbids=["EFI loader done"],
            needs=[
                "EFI ExitBootServices succeeded",
                "EFI kernel entry not recognised within handoff_steps=1000: observation "
                "stopped, normal budgets resume",
                "EFI handoff observation stopped at its bound: 1000 single-step slices in ",
                "ebsloop: done",
                SHUTDOWN,
            ],
        )
    )
    units = {
        "compare-always": "unit=bundle, once per stay on its pages",
        "epoch-compare": "unit=bundle, only after a storage-interrupt epoch bump: DMA completed by "
        "polling or by other devices is not detected",
        "none": "no comparison: DMA into code is not detected",
    }
    for option, mode, mib in (
        (None, "compare-always", 48),
        ("epoch-compare", "epoch-compare", 48),
        ("none", "none", 48),
        ("hw-dirty", "compare-always", 8),
    ):
        extra = ("" if option is None else f" code_validity={option}") + (
            f" code_snapshot_mib={mib}" if mib != 48 else ""
        )
        out.append(
            Case(
                f"validity-{option or 'default'}",
                "hello",
                f"engine=tier0{extra}",
                needs=[f"code validity: mode={mode}, {units[mode]}; snapshot budget ", SHUTDOWN],
                log_check=snapshot_budget_problems(mib if mib != 48 else None),
            )
        )
    out.append(
        Case(
            "cpudesc-1",
            "cpudesc-1",
            "engine=tier0",
            needs=["CPUDESC PASSED"],
            forbids=["CPUDESC FAIL"],
        )
    )
    out.append(
        Case(
            "cpudesc-2",
            "cpudesc-2",
            "engine=tier0 vcpus=2 smp_serialize=1",
            needs=["CPUDESC PASSED"],
            forbids=["CPUDESC FAIL"],
        )
    )
    out.append(
        Case(
            "legacymap",
            "legacymap",
            "engine=tier0",
            needs=["LEGACYMAP PASSED", "LEGACYMAP PASS RAM granules whole 8 KiB"],
            forbids=["LEGACYMAP FAILED"],
        )
    )
    out.extend(cpumodel_cases((1,)))
    out.append(
        Case(
            "sratmap-new",
            "sratmap",
            "engine=tier0",
            firmware="new",
            needs=["SRATMAP PASSED EFI RAM and SRAT affinity unions agree"],
            forbids=["SRATMAP FAILED"],
        )
    )
    # benchwrap.efi overwrites ar.k1-k4 at the end of a job and restores them
    # before it takes an interrupt again; ar.k1 is the firmware's interrupt
    # context pointer.
    out.append(
        Case(
            "benchwrap-new",
            "benchwrap",
            "engine=interp",
            firmware="new",
            needs=[SHUTDOWN, "BENCH step=000 job=speed var=0 ", "BENCH-DONE steps=1 failures=0"],
            forbids=["firmware panic", "BENCH-ERROR", "BENCH-CHECK"],
        )
    )
    out.append(
        Case(
            "legacymap-new",
            "legacymap",
            "engine=tier0",
            firmware="new",
            needs=[
                "LEGACYMAP PASSED",
                "LEGACYMAP PASS RAM granules whole 8 KiB",
                "LEGACYMAP descriptor 00000000000a0000 0000000000000020 "
                "000000000000000b 8000000000000001 0000000000000000",
                "LEGACYMAP descriptor 00000000000c0000 0000000000000040 "
                "000000000000000b 0000000000000001 0000000000000000",
            ],
            forbids=["LEGACYMAP FAILED"],
        )
    )
    # The release build keeps the platform's own status and failure lines and
    # drops EDK2's DEBUG/ASSERT output; the applications behave the same.
    out.append(
        Case(
            "hello-new-release",
            "hello",
            "engine=tier0 halt_retry_us=0",
            firmware="new",
            release=True,
            needs=[
                "IPL: entering DxeCore descriptor=",
                "BDS: StartImage",
                (ROOT / "tests/efi/hello/hello-uefi27.golden").read_text(),
                SHUTDOWN,
            ],
            forbids=["IPL failure", EDK2_DEBUG_LINE],
        )
    )
    out.append(
        Case(
            "legacymap-new-release",
            "legacymap",
            "engine=tier0",
            firmware="new",
            release=True,
            needs=["LEGACYMAP PASSED", "LEGACYMAP PASS RAM granules whole 8 KiB"],
            forbids=["LEGACYMAP FAILED", EDK2_DEBUG_LINE],
        )
    )
    for engine in ("interp", "tier0"):
        out.append(
            Case(
                f"smp-tlb-purge-new-{engine}",
                "smp-tlb-purge",
                f"engine={engine} vcpus=2 smp_serialize=1",
                firmware="new",
                needs=[SHUTDOWN, "PASS purge-remote", "PASS purge-halted"],
                check=smp_problems("tlb-purge", 2),
                timeout=240,
            )
        )
    out.append(
        Case(
            "smp-ipi-itc-new",
            "smp-ipi-itc",
            "engine=tier0 vcpus=3 smp_serialize=1",
            firmware="new",
            needs=[SHUTDOWN, "PASS ipi-ping-pong"],
            check=smp_problems("ipi-itc", 3),
            timeout=600,
        )
    )
    # The full matrix retains the serialized rows above as diagnostics.
    for firmware in ("new",):
        for engine in ("interp", "tier0"):
            for count in (2, 3):
                for profile in ("ipi-itc", "tlb-purge"):
                    out.append(
                        Case(
                            f"smp-concurrent-{firmware}-{engine}-{count}-{profile}",
                            f"smp-{profile}",
                            f"engine={engine} vcpus={count}",
                            firmware=firmware,
                            needs=[SHUTDOWN, f"guest SMP: {count} vCPUs, smp_serialize=0"]
                            + (
                                ["PASS ipi-ping-pong"]
                                if profile == "ipi-itc" and count >= 3
                                else []
                            ),
                            check=smp_problems(profile, count),
                            timeout=600,
                        )
                    )
        for profile in ("ipi-itc", "tlb-purge"):
            out.append(
                Case(
                    f"smp-concurrent-{firmware}-lockstep-2-{profile}",
                    f"smp-{profile}",
                    "engine=lockstep vcpus=2",
                    firmware=firmware,
                    needs=[SHUTDOWN, "guest SMP: 2 vCPUs, smp_serialize=0"],
                    check=smp_problems(profile, 2),
                    timeout=900,
                )
            )
    out.append(
        Case(
            "smp-concurrent-new-tier1-2-tlb-purge",
            "smp-tlb-purge",
            "engine=tier1 vcpus=2",
            firmware="new",
            cpus=5,
            needs=[
                SHUTDOWN,
                "guest SMP: 2 vCPUs, smp_serialize=0",
                "helper CPUs=2; background work",
            ],
            forbids=["purge wait vCPU", "sync.i wait vCPU"],
            check=smp_problems("tlb-purge", 2),
            timeout=600,
        )
    )
    out.append(
        Case(
            "placement-core-smp-tlb-purge",
            "smp-tlb-purge",
            "engine=tier0 vcpus=2 placement=core",
            firmware="new",
            needs=[SHUTDOWN, "placement=core", "guest SMP: 2 vCPUs, smp_serialize=0"],
            check=smp_problems("tlb-purge", 2),
            timeout=600,
        )
    )
    # In-VMM restart: every reset source,
    # Tier1, and APs that run before a restart and start reset-held after it.
    for firmware in ("new",):
        out.append(
            Case(
                f"restart-{firmware}",
                "restart",
                "engine=tier0",
                firmware=firmware,
                needs=["RESTART PASS boots=7", SHUTDOWN],
                forbids=["RESTART FAIL", "RESTART i8042 absent"],
                log_check=restart_problems(7),
                timeout=300,
            )
        )
    out.append(
        Case(
            "restart-new-tier1",
            "restart",
            "engine=tier1",
            firmware="new",
            needs=["RESTART PASS boots=7", SHUTDOWN],
            forbids=["RESTART FAIL", "RESTART i8042 absent"],
            log_check=restart_problems(7, tier1=True),
            timeout=600,
        )
    )
    # Two AHCI functions behind a PCIe switch's downstream ports: a Secondary
    # Bus Reset on each port at every restart.
    out.append(
        Case(
            "restart-new-sbr",
            "restart",
            "engine=tier0",
            firmware="new",
            runner_args=["--pcie-switch"],
            needs=[
                "RESTART PASS boots=7",
                SHUTDOWN,
                "restart: Secondary Bus Reset on bridge 02:00.0",
                "restart: Secondary Bus Reset on bridge 02:01.0",
            ],
            forbids=["RESTART FAIL", "RESTART i8042 absent", "a function reset failed"],
            log_check=restart_problems(7),
            timeout=300,
        )
    )
    out.append(
        Case(
            "restart-new-smp",
            "smp-restart",
            "engine=tier0 vcpus=2",
            firmware="new",
            needs=["SMP-APP RESTART aps_spinning=1", SHUTDOWN],
            log_check=restart_problems(2, smp_cpus=2),
            cpus=3,
            timeout=600,
        )
    )
    # INIT from another vCPU, PMI with SAL and PAL vectors and the TR-overlap
    # machine checks of itr.d and ptc.l on the native firmware.
    for engine in ("tier0", "tier1"):
        out.append(
            Case(
                f"initpmi-new-{engine}",
                "initpmi",
                f"engine={engine} vcpus=2",
                firmware="new",
                cpus=3,
                needs=[f"INITPMI PASS {name}" for name in INITPMI_CASES]
                + ["vCPU0 INIT at ip=", "vCPU0 PMI at ip=", "vCPU0 machine check at ip=", SHUTDOWN],
                forbids=["INITPMI FAIL", "firmware panic"],
                log_check=initpmi_problems,
                timeout=300,
            )
        )
    out.append(
        Case(
            "behaviour-new",
            "UefiBehaviour",
            "engine=interp halt_retry_us=0 handoff_steps=33554432",
            firmware="new",
            memory=2304,
            milestone_names=("ebs", "loader_resumed", "exit"),
            needs=[
                "UEFI behaviour: RTC, variables, authenticated rejection, ITM events, Stall, "
                "PAL idle passed",
                "UEFI behaviour: map key, ExitBootServices retry, nonidentity virtual map and "
                "runtime calls passed",
                "BDS: StartImage",
                SHUTDOWN,
            ],
            check=behaviour_problems,
        )
    )
    # Host SIMD state: every CPU
    # enabled it and keeps it across interrupts, then a guest runs on the
    # SIMD build. The native container (SVM/VMX) and the hostwin #PF path
    # run on wsl-qemu, whose KVM or TCG CPU offers SVM; the CPU without AVX
    # stops before its first SIMD instruction.
    simd_cpus = [
        f"selftest=simd: CPU{cpu} state XCR0 = boot mask, MXCSR=1f80 FCW=037f, interrupts preserved "
        "YMM0-15 MXCSR x87"
        for cpu in range(4)
    ]
    out.append(
        Case(
            "simd-selftest",
            "hello",
            "engine=tier0 selftest=simd",
            needs=simd_cpus + ["selftest=simd passed"],
        )
    )
    out.append(
        Case(
            "simd-native",
            "hello",
            "engine=tier0 vcpus=2 selftest=simd selftest=hostwin",
            setup=True,
            runner_args=["--backend", "wsl-qemu"],
            needs=simd_cpus
            + [
                "host YMM0-15 MXCSR x87 preserved; guest starts from the initial state, "
                "guest state kept across exits; guest XSETBV intercepted, host XCR0 unchanged",
                "selftest=simd passed",
                "HWB selftest passed: fills>0 backend_failures=0",
            ]
            + [
                f"HWB local passed CPU{cpu}: coherence, #PF RO retry, #PF with live SIMD state"
                for cpu in (1, 2)
            ],
        )
    )
    out.append(
        Case(
            "simd-baseline-missing",
            "hello",
            "engine=tier0",
            rc=1,
            setup=True,
            runner_args=["--backend", "wsl-qemu", "--cpu-flags=-avx"],
            needs=[
                "panic: host CPU lacks the SIMD baseline (SSE4.2, POPCNT, AVX, XSAVE of x87/SSE/AVX "
                "state): missing AVX"
            ],
            forbids=["host scalar ISA="],
        )
    )
    default_smp = {
        "smp-tlb-purge-new-tier0",
        "smp-concurrent-new-interp-2-ipi-itc",
        "smp-concurrent-new-tier0-3-ipi-itc",
        "smp-concurrent-new-tier0-2-tlb-purge",
        "smp-concurrent-new-tier1-2-tlb-purge",
        "placement-core-smp-tlb-purge",
    }
    return [
        case
        for case in out
        if (case.app in ("smp-ipi-itc", "smp-tlb-purge") and case.name not in default_smp) == full
    ]


def cases():
    replay_options = [
        Case(
            f"replay-option-{engine}-{option}",
            "hello",
            f"engine={engine} {option}=0",
            rc=4,
            setup=True,
            needs=["Tier1 replay options require a TIER1_CHECKED=1 build"],
            forbids=["engine executor"],
        )
        for engine in ("interp", "tier0", "tier1", "lockstep")
        for option in ("tier1_check_every", "tier1_check_after")
    ]
    replay_smp = Case(
        "smp-tier1-replay",
        "hello",
        "engine=lockstep tier1=on vcpus=2",
        rc=4,
        setup=True,
        needs=["engine=lockstep tier1=on requires a TIER1_CHECKED=1 build"],
        forbids=["engine executor"],
    )
    return budget_cases() + replay_options + [replay_smp]


def behaviour_problems(guest):
    counts = re.findall(r"^UEFI behaviour: high runtime aliases pinned=(\d+)$", guest, re.M)
    return (
        []
        if len(counts) == 1 and int(counts[0]) > 0
        else ["behaviour: require at least one high runtime alias"]
    )


def gate_problems(relocated):
    def check(guest):
        errors = []
        driver = re.search(r"gate DXE driver: preferred=4000000 actual=([0-9a-fA-F]+)", guest)
        if not driver or int(driver[1], 16) == 0x4000000:
            errors.append("gate: driver relocation not proved")
        if relocated:
            actual = re.search(r"preferred=4000000 actual=([0-9a-fA-F]+)", guest)
            loaded = re.search(r"gate pointers=42, loaded=([0-9a-fA-F]+)", guest)
            if (
                not actual
                or not loaded
                or int(actual[1], 16) == 0x4000000
                or actual[1] != loaded[1]
            ):
                errors.append("gate: application relocation not proved")
        return errors

    return check


def serial_problems(mode):
    def check(log):
        frames = re.findall(
            r"^\[vmm-serial\] ([0-9a-f]{16}) ([0-9a-f]{4}) ([0-9a-f]+)\r?$", log, re.M
        )
        data, end = b"", 0
        for position, count, payload in frames:
            position, count = int(position, 16), int(count, 16)
            if position != end or len(payload) != count * 2:
                return ["debug serial: lost or malformed frame"]
            end += count
            data += bytes.fromhex(payload)
        if len(frames) != len(re.findall(r"^\[vmm-serial\] ", log, re.M)):
            return ["debug serial: malformed frame"]
        checkpoints = re.findall(r"^\[vmm-serial-end\] ([0-9a-f]{16})\r?$", log, re.M)
        if (
            not checkpoints
            or int(checkpoints[-1], 16) != end
            or re.search(r"\[vmm-serial-lost\] [1-9]", log)
        ):
            return ["debug serial: incomplete drain"]
        expected = b"DS:\0\r\n\xff\x1b[m"
        if bool(data) if mode == "off" else expected not in data:
            return ["debug serial: byte-exact payload disagrees with mode"]
        return []

    return check


def full_cases():
    """The remaining matrix and longer cases ported from hosted scripts."""
    out = budget_cases(full=True)
    out.extend(cpumodel_cases((2, 4)))
    # Four guests require the BSP plus four executors.
    for engine in ("interp", "tier0"):
        for count in (2, 4):
            out.append(
                Case(
                    f"smp-concurrent-long-{engine}-{count}",
                    "smp-long",
                    f"engine={engine} vcpus={count}",
                    firmware="new",
                    cpus=count + 1,
                    needs=[SHUTDOWN, f"guest SMP: {count} vCPUs, smp_serialize=0"],
                    check=smp_problems("long", count),
                    timeout=7200,
                )
            )
    out.append(
        Case(
            "smp-concurrent-quick-lockstep-2",
            "smp-quick",
            "engine=lockstep vcpus=2",
            firmware="new",
            needs=[SHUTDOWN, "guest SMP: 2 vCPUs, smp_serialize=0"],
            check=smp_problems("quick", 2),
            timeout=1800,
        )
    )

    for firmware in ("new",):
        out.append(
            Case(
                f"smp-ipi-itc-{firmware}-interp",
                "smp-ipi-itc",
                "engine=interp vcpus=3 smp_serialize=1",
                firmware=firmware,
                needs=[SHUTDOWN],
                check=smp_problems("ipi-itc", 3),
                timeout=600,
            )
        )
        out.append(
            Case(
                f"smp-tlb-purge-{firmware}-3",
                "smp-tlb-purge",
                "engine=tier0 vcpus=3 smp_serialize=1",
                firmware=firmware,
                needs=[SHUTDOWN, "PASS purge-remote", "PASS purge-halted"],
                check=smp_problems("tlb-purge", 3),
                timeout=240,
            )
        )
        for mode in ("off", "device", "console"):
            out.append(
                Case(
                    f"debugserial-{firmware}-{mode}",
                    "debugserial",
                    f"engine=tier0 debug_serial={mode}",
                    firmware=firmware,
                    needs=[f"debugserial mode={mode}", "debugserial behaviour PASSED", SHUTDOWN],
                    forbids=["debugserial FAIL"],
                    log_check=serial_problems(mode),
                    timeout=180,
                )
            )
    out.append(
        Case(
            "pal-probe-new",
            "PalProbe",
            "engine=tier0 halt_retry_us=0",
            firmware="new",
            needs=[
                "PAL probe: " + marker
                for marker in (
                    "native PAL static/stacked vectors, buffers and copied entry passed",
                    "raw static PAL GR/PR/BR and rotating frame preservation passed",
                    "published ACPI and SMBIOS tables passed",
                    "SAL table, procedure ABI, vectors and empty logs passed",
                    "physical static-GR unaligned IVT accesses passed",
                    "stacked GR, speculative NaT and FP unaligned IVT accesses passed",
                    "nonidentity virtual PAL and runtime SAL calls passed",
                )
            ]
            + [SHUTDOWN],
            forbids=["PAL probe FAIL"],
            timeout=300,
        )
    )
    for firmware in ("new",):
        out.append(
            Case(
                f"fpswa-{firmware}",
                "FpswaProbe",
                "engine=tier0 halt_retry_us=0",
                firmware=firmware,
                needs=[
                    "FPSWA probe: " + marker
                    for marker in (
                        "protocol located",
                        "IVT scalar/packed results, flags and predicates passed",
                        "saved-state call and live FPU controls passed",
                        "runtime memory types passed",
                        "ExitBootServices/nonidentity virtual calls passed",
                    )
                ]
                + [SHUTDOWN],
                forbids=["FPSWA probe FAIL"],
                timeout=300,
            )
        )
    for app in ("GateHello", "GateLauncher"):
        out.append(
            Case(
                f"gate-{app}",
                app,
                "engine=tier0",
                needs=[
                    "gate pointers=42, loaded=",
                    "gate DXE driver: entry reached, pointers=73",
                    "gate app+driver EFI_SUCCESS",
                    SHUTDOWN,
                ]
                + (["gate relocated app EFI_SUCCESS"] if app == "GateLauncher" else []),
                check=gate_problems(app == "GateLauncher"),
            )
        )
    # IPL/DXE must not assert: the protocols exist. Prove
    # handoff validation, DXE dispatch, BDS and the app's golden output.
    out.append(
        Case(
            "ipl-dxe-new",
            "hello",
            "engine=tier0 halt_retry_us=0",
            firmware="new",
            needs=[
                "IPL: entering DxeCore descriptor=",
                "CoreInitializeMemoryServices:",
                "BDS: StartImage",
                (ROOT / "tests/efi/hello/hello-uefi27.golden").read_text(),
                SHUTDOWN,
            ],
            forbids=["IPL failure", "ASSERT_EFI_ERROR"],
        )
    )
    # The largest unoversubscribed host topology: management plus N vCPUs,
    # respecting the product's 32-vCPU limit rather than hard-coding 32.
    host_cpus = min(33, len(os.sched_getaffinity(0)))
    largest = host_cpus - 1
    # DSP0134 socket counts: full/even and last-partial/odd on both models.
    for firmware in ("new",):
        for model in ("montecito", "madison"):
            for count in (1, 2, 3, 4):
                out.append(
                    Case(
                        f"cpudesc-{firmware}-{model}-{count}",
                        f"cpudesc-{count}",
                        f"engine=tier0 cpu_model={model} vcpus={count} smp_serialize=1",
                        firmware=firmware,
                        cpus=max(4, count + 1),
                        needs=["CPUDESC PASSED", SHUTDOWN],
                        forbids=["CPUDESC FAIL"],
                    )
                )
    for scenario, engines, count, status, needs, forbids in (
        ("main", ("interp", "tier0"), 3, 0, [SHUTDOWN], ["firmware panic"]),
        (
            "nmi",
            ("interp", "tier0"),
            2,
            0,
            [
                "vCPU1 leaves reset-hold",
                "IRQ vCPU1 vector=2 injections=1",
                "firmware debug 00000000534d0101",
                SHUTDOWN,
            ],
            ["firmware panic"],
        ),
        (
            "init",
            ("interp",),
            2,
            2,
            [
                "vCPU1 INIT/PMI in reset-hold: no firmware context",
                "requested exit in reset-hold",
                "vCPU1 guest exit reason=3",
                "vCPU0 stopped: vCPU1 ended the VM",
            ],
            ["vCPU1 leaves reset-hold", "firmware panic"],
        ),
        (
            "ptcg",
            ("interp", "tier0"),
            2,
            0,
            ["SMP AP purge ptc.g completed", SHUTDOWN, "vCPU1 stopped: vCPU0 ended the VM"],
            ["firmware panic"],
        ),
        (
            "ptcg",
            ("interp", "tier0"),
            1,
            0,
            [SHUTDOWN],
            ["global purge", "firmware panic", "[info] guest SMP"],
        ),
        (
            "ptcga",
            ("interp", "tier0"),
            2,
            0,
            ["SMP AP purge ptc.ga completed cpu=1", SHUTDOWN, "vCPU1 stopped: vCPU0 ended the VM"],
            ["firmware panic"],
        ),
        # On a four-CPU host the largest count is the fixed 3 above.
        *(
            (("main", ("interp",), largest, 0, [SHUTDOWN], ["firmware panic"]),)
            if largest != 3
            else ()
        ),
    ):
        for engine in engines:
            markers = list(needs)
            if count > 1:
                markers += [f"guest SMP: {count} vCPUs, smp_serialize=1"]
                markers += [f"vCPU{cpu} engine executor host CPU{cpu + 1}" for cpu in range(count)]
            if scenario == "main":
                for cpu in range(1, count):
                    markers += [
                        f"vCPU{cpu} leaves reset-hold",
                        f"vCPU{cpu} stopped: vCPU0 ended the VM",
                        f"firmware debug {0x534D0100 | cpu:016x}",
                        f"firmware debug {0x534D0200 | cpu:016x}",
                        f"IRQ vCPU{cpu} vector=255 injections=2",
                        f"IRQ vCPU0 vector={0xD0 + cpu} injections=2",
                        f"vCPU{cpu} executor stopped",
                    ]
            out.append(
                Case(
                    f"ap-{scenario}-{engine}-{count}",
                    "hello",
                    f"engine={engine} vcpus={count} smp_serialize=1",
                    rc=status,
                    firmware="new",
                    firmware_image=ROOT / f"build/tests/qemu-ap/{scenario}.elf",
                    needs=markers,
                    forbids=forbids,
                    cpus=max(4, count + 1),
                    timeout=180,
                )
            )
    for engine in ("interp", "tier0"):
        for count in (2, 4):
            markers = [SHUTDOWN, f"guest SMP: {count} vCPUs, smp_serialize=0"]
            markers += [f"vCPU{cpu} engine executor host CPU{cpu + 1}" for cpu in range(count)]
            for cpu in range(1, count):
                markers += [
                    f"vCPU{cpu} leaves reset-hold",
                    f"vCPU{cpu} stopped: vCPU0 ended the VM",
                    f"firmware debug {0x534D0100 | cpu:016x}",
                    f"firmware debug {0x534D0200 | cpu:016x}",
                    f"IRQ vCPU{cpu} vector=255 injections=2",
                    f"IRQ vCPU0 vector={0xD0 + cpu} injections=2",
                    f"vCPU{cpu} executor stopped",
                ]
            out.append(
                Case(
                    f"smp-concurrent-ap-main-{engine}-{count}",
                    "hello",
                    f"engine={engine} vcpus={count}",
                    firmware="new",
                    firmware_image=ROOT / "build/tests/qemu-ap/main.elf",
                    needs=markers,
                    forbids=["firmware panic"],
                    cpus=max(4, count + 1),
                    timeout=300,
                )
            )
    for original in list(out):
        if original.name.startswith("ap-") and "vcpus=2 " in original.cmdline:
            out.append(
                Case(
                    "smp-concurrent-" + original.name,
                    original.app,
                    original.cmdline.replace(BASE, "").replace("smp_serialize=1", ""),
                    rc=original.rc,
                    firmware=original.firmware,
                    firmware_image=original.firmware_image,
                    needs=[
                        x.replace("smp_serialize=1", "smp_serialize=0")
                        for x in original.needs
                        if not x.startswith("SMP AP fixture")
                    ],
                    forbids=original.forbids,
                    cpus=original.cpus,
                    timeout=original.timeout,
                )
            )
    for firmware in ("new",):
        out.append(
            Case(
                f"over-limit-{firmware}",
                "hello",
                "engine=interp vcpus=33 smp_serialize=1",
                rc=4,
                firmware=firmware,
                cpus=34,
                setup=True,
                needs=[f"vcpus=33: the {firmware} firmware supports 1..32 vCPUs"],
                forbids=["engine executor", "IA64-VMM: hello from IA-64 EFI"],
            )
        )
    for name, option, engine, message in (
        (
            "smp-none",
            "code_validity=none",
            "interp",
            "code_validity=none with multiple vCPUs is a setup error",
        ),
        ("bad-serialize", "smp_serialize=2", "interp", "smp_serialize must be 0 or 1"),
        (
            "smp-tier1-helpers-off",
            "helpers=off",
            "tier1",
            "engine=tier1 with vcpus=2 needs background compilation: helpers=off is a setup error",
        ),
        (
            "smp-tier1-lockstep-helpers-off",
            "tier1=on helpers=off",
            "lockstep",
            "engine=lockstep tier1=on with vcpus=2 needs background compilation: helpers=off is a setup error",
        ),
    ):
        out.append(
            Case(
                name,
                "hello",
                f"engine={engine} vcpus=2 {option}",
                rc=4,
                setup=True,
                needs=[message],
                forbids=["engine executor"],
            )
        )
    # Retain the removed scripts' default interpreter runs as well as Tier0.
    for case in list(out):
        if case.name in (
            "pal-probe-new",
            "fpswa-new",
            "gate-GateHello",
            "gate-GateLauncher",
            "ipl-dxe-new",
        ):
            interp = Case(
                case.name + "-interp",
                case.app,
                case.cmdline.removeprefix(BASE).replace("engine=tier0", "engine=interp"),
                firmware=case.firmware,
                forbids=case.forbids,
                check=case.check_extra,
                timeout=600,
            )
            interp.needs = list(case.needs)
            out.append(interp)
            if case.name != "ipl-dxe-new":
                lockstep = Case(
                    case.name + "-lockstep",
                    case.app,
                    case.cmdline.removeprefix(BASE).replace("engine=tier0", "engine=lockstep"),
                    firmware=case.firmware,
                    forbids=case.forbids,
                    check=case.check_extra,
                    timeout=600,
                )
                lockstep.needs = list(case.needs)
                out.append(lockstep)
    return out


def ordinary_boot_problems(guest):
    """Apply ordinary PASS rules to the negative app's three boot cases."""
    try:
        result = smplog.parse(guest.splitlines())
    except smplog.LogError as error:
        return [str(error)]
    expected = smplog.expected("vmm", 2, "ipi-itc")
    return smplog.check(result, {name: expected[name] for name in smplog.BOOT_CASES})


def broken_guest_inputs():
    """Real runs the checks must reject: the app's own result is the failure."""
    return [
        Case(
            "broken-sratmap-hole-new",
            "sratmap-broken-hole",
            "engine=tier0",
            firmware="new",
            needs=[
                "SRATMAP inserted affinity over VGA hole",
                "SRATMAP FAILED EFI RAM and SRAT affinity unions differ",
            ],
            forbids=["SRATMAP PASSED"],
        ),
        # Remove the descriptor containing C0000 from the new firmware's map.
        Case(
            "broken-legacymap-new",
            "legacymap-broken-descriptor",
            "engine=tier0",
            firmware="new",
            needs=[
                "LEGACYMAP removed descriptor containing C0000",
                "LEGACYMAP FAIL legacy window granule",
                "LEGACYMAP FAILED",
            ],
            forbids=["LEGACYMAP PASSED"],
        ),
        Case(
            "broken-legacymap-page0-new",
            "legacymap-broken-page0",
            "engine=tier0",
            firmware="new",
            needs=[
                "LEGACYMAP added conventional page 0 overlap",
                "LEGACYMAP FAIL no conventional page 0 overlap",
                "LEGACYMAP FAILED",
            ],
            forbids=["LEGACYMAP PASSED", "LEGACYMAP FAIL legacy window granule"],
        ),
        # The wrapper that returns to the firmware with PSR.i set while ar.k1
        # still holds the job's IPSR: the pending timer interrupt reaches the
        # IVT with a bad context pointer (its fatal code).
        Case(
            "broken-benchwrap-keep-psr-i-new",
            "benchwrap-keep-psr-i",
            "engine=interp",
            firmware="new",
            rc=1,
            needs=[
                "firmware panic 0000000047324956",
                # ar.k5 = the wrapper's exit PSR, ar.k1 = the job's IPSR (0 for the speed job):
                # the interrupt was taken between the break stub and the KR restore.
                "ar5=0000100000000000",
                "ar1=0000000000000000",
            ],
            forbids=["BENCH-DONE"],
        ),
        # A defect still fails on the release build: the application's result
        # and the platform's fatal code are not DEBUG output.
        Case(
            "broken-legacymap-new-release",
            "legacymap-broken-descriptor",
            "engine=tier0",
            firmware="new",
            release=True,
            needs=["LEGACYMAP FAIL legacy window granule", "LEGACYMAP FAILED"],
            forbids=["LEGACYMAP PASSED", EDK2_DEBUG_LINE],
        ),
        Case(
            "broken-benchwrap-keep-psr-i-new-release",
            "benchwrap-keep-psr-i",
            "engine=interp",
            firmware="new",
            release=True,
            rc=1,
            needs=["firmware panic 0000000047324956"],
            forbids=["BENCH-DONE", EDK2_DEBUG_LINE],
        ),
        # OS_INIT claims a new context although it resumes the interrupted
        # one: PAL_MC_RESUME restores no BR1, and init must say so.
        Case(
            "broken-initpmi-resume",
            "initpmi-broken-resume",
            "engine=tier0 vcpus=2",
            firmware="new",
            cpus=3,
            rc=1,
            needs=[
                "INITPMI FAIL init registers or timeout 0x0000000002000000",
                "INITPMI PASS mca",
                "INITPMI PASS purge",
                "EFI ResetSystem type=2 status=8000000000000015",
            ],
            forbids=["INITPMI PASS init", "firmware panic"],
            timeout=300,
        ),
        # cpudesc-2.efi expects two processors; the VMM gives it one.
        Case(
            "broken-cpudesc-count",
            "cpudesc-2",
            "engine=tier0",
            rc=1,
            needs=[
                "CPUDESC PASSED",
                "CPUDESC FAIL MADT enabled Local SAPIC entries = 1",
                "CPUDESC FAILED",
                "EFI ResetSystem type=2 status=8000000000000015",
            ],
            forbids=["CPUDESC FAIL"],
            expect_problems=True,
            log_check=cpudesc_defect,
        ),
        # The smp.efi variant with a wrong wake vector: it must show exactly
        # its defect (sal-ap-wake FAIL) and the ordinary expectations must
        # reject it.
        Case(
            "broken-smp-wake-vector",
            "smp-broken-wake-vector",
            "engine=tier0 vcpus=2 smp_serialize=1",
            rc=1,
            needs=["EFI ResetSystem type=2 status=8000000000000007"],
            check=smp_problems("broken-wake-vector", 2, broken="wake-vector"),
            must_reject=ordinary_boot_problems,
            timeout=240,
        ),
    ]


class RunnerGroup:
    """Keep launches atomic with abort so a late worker cannot escape cleanup."""

    def __init__(self):
        self.lock = threading.Lock()
        self.processes = {}
        self.stopping = False

    def launch(self, *args, **kwargs):
        with self.lock:
            if self.stopping:
                raise RuntimeError("runner group aborted")
            process = subprocess.Popen(*args, **kwargs)
            self.processes[process.pid] = process
            return process

    def discard(self, process):
        with self.lock:
            self.processes.pop(process.pid, None)

    def abort(self):
        with self.lock:
            self.stopping = True
            processes = list(self.processes.values())
        stop_runners(processes)


def stop_runners(processes):
    # Give every runner its cleanup signal before waiting on any one.
    for process in processes:
        try:
            process.terminate()
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + 30
    for process in processes:
        try:
            process.wait(timeout=max(0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            pass
    for process in processes:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    for process in processes:
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            pass


def run(case, runners=None):
    LOGS.mkdir(parents=True, exist_ok=True)
    log = LOGS / f"{case.name}.log"
    environment = dict(os.environ, BOOT_APP=case.app)
    command = [
        sys.executable,
        str(ROOT / "tools/qemu/run.py"),
        "--no-build",
        "--firmware",
        case.firmware,
        "--timeout",
        str(case.timeout),
        "--cmdline",
        case.cmdline,
        "--log",
        str(log),
    ]
    if case.memory:
        command += ["--memory", str(case.memory)]
    command += ["--cpus", str(case.cpus)]
    if case.firmware_image or case.release_image:
        command += ["--firmware-image", str(case.firmware_image or case.release_image)]
    command += case.runner_args
    log.unlink(missing_ok=True)
    load_before = smplog.loadavg()
    started = time.monotonic()
    timed_out, killed_session = False, False
    runners = runners if runners is not None else RunnerGroup()
    process = runners.launch(
        command,
        cwd=ROOT,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    try:
        try:
            output, _ = process.communicate(timeout=case.timeout + 120)
        except subprocess.TimeoutExpired:
            # SIGTERM to the exact runner PID lets its finally blocks stop its VM.
            timed_out = True
            process.terminate()
            try:
                output, _ = process.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                killed_session = True
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass  # The exact session exited between the timeout and kill.
                try:
                    output, _ = process.communicate(timeout=30)
                except subprocess.TimeoutExpired as error:
                    # A detached descendant may retain the pipe after the
                    # session died. Never wait indefinitely for that pipe.
                    output = error.output or b""
                    process.stdout.close()
                    try:
                        process.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        output += b"\nrunner could not be reaped after session SIGKILL"
            output += b"\nrunner timeout"
    except BaseException:
        stop_runners([process])
        raise
    finally:
        runners.discard(process)
    runner_rc = process.returncode
    rc = -1 if timed_out else runner_rc
    elapsed = time.monotonic() - started
    text = log.read_text(errors="replace") if log.is_file() else ""
    record = dict(
        command=command,
        runner_pid=process.pid,
        exit=rc,
        runner_exit=runner_rc,
        elapsed=elapsed,
        load_before=load_before,
        load_after=smplog.loadavg(),
        timed_out=timed_out,
        killed_session=killed_session,
    )
    ram = re.findall(r"guest RAM[^\n]*", text)
    record["guest_ram"] = ram
    log.with_suffix(".json").write_text(json.dumps(record, indent=2) + "\n")
    return rc, text, output.decode(errors="replace"), elapsed, log


CPUDESC_COUNT_FAILURES = {
    "CPUDESC FAIL MADT enabled Local SAPIC entries = 1",
    "CPUDESC FAIL SRAT enabled processor affinity entries = 1",
    "CPUDESC FAIL MADT Local SAPICs not (ACPI id i, id i, eid 0) = 2",
    "CPUDESC FAIL SRAT processors not (id i, eid 0) = 2",
    "CPUDESC FAIL ACPI Processor objects = 1",
    "CPUDESC FAIL Processor object ids not exactly 0 .. N-1 = 1",
    "CPUDESC FAIL SMBIOS enabled cores = 1",
    "CPUDESC FAIL SMBIOS core count (no spare capacity) = 1",
    "CPUDESC FAIL SMBIOS thread count = 1",
}


def cpudesc_defect(log):
    lines = re.findall(r"^CPUDESC FAIL .+$", guest_text(log), re.M)
    return (
        []
        if set(lines) == CPUDESC_COUNT_FAILURES and len(lines) == len(CPUDESC_COUNT_FAILURES)
        else [f"cpudesc: unexpected FAIL lines {lines}"]
    )


def expected_defect(case, found):
    return case.expect_problems and set(found) == {
        "missing: CPUDESC PASSED",
        "present: CPUDESC FAIL",
    }


def mutations(case, log):
    """(description, rc, log, expected reason prefix) independently broken copies."""
    log = log.replace("\r", "")
    out = [("exit status changed", case.rc + 1, log, "runner status ")]
    out.append(
        (
            "guest shutdown status changed",
            case.rc,
            re.sub(r"(VMM shutdown[^\n]*exit_code=)\d+", rf"\g<1>{case.rc + 1}", log),
            "guest shutdown status ",
        )
    )
    for needle in case.needs:
        if needle not in log and needle not in guest_text(log):
            continue  # The negative cpudesc deliberately lacks PASSED.
        piece = needle.splitlines()[1] if len(needle.splitlines()) > 1 else needle
        out.append(
            (
                f'"{piece[:40]}" removed',
                case.rc,
                log.replace(piece, "REMOVED"),
                f"missing: {needle}",
            )
        )
    for needle in case.forbids:
        if needle in log or needle in guest_text(log):
            continue
        out.append(
            (f'"{needle}" present', case.rc, log + f"\n[guest] {needle}\n", f"present: {needle}")
        )
    if not case.setup:
        out.append(
            (
                "no Tier1 statistics line",
                case.rc,
                re.sub(r"^\[info\] tier1 linked=.*$", "", log, flags=re.M),
                "tier1 statistics: ",
            )
        )
        out.append(
            (
                "no exit milestone",
                case.rc,
                re.sub(r"^\[info\] milestone exit .*$", "", log, flags=re.M),
                "milestone lines: milestone exit missing",
            )
        )
    if case.name.startswith("faultloop-"):
        out.append(
            (
                "executor exceeds faultloop bound",
                case.rc,
                re.sub(r"(milestone exit[^\n]*wall_ms=)\d+", rf"\g<1>{FAULTLOOP_WALL_MS}", log),
                f"faultloop executor wall_ms={FAULTLOOP_WALL_MS}, expected <{FAULTLOOP_WALL_MS}",
            )
        )
    if case.name == "behaviour-new":
        out.append(
            (
                "no high aliases",
                case.rc,
                re.sub(r"(high runtime aliases pinned=)\d+", r"\g<1>0", log),
                "behaviour: require at least one high runtime alias",
            )
        )
    if case.name == "broken-cpudesc-count":
        out.append(
            (
                "unrelated cpudesc failure",
                case.rc,
                log + "\n[guest] CPUDESC FAIL XSDT checksum = 0\n",
                "cpudesc: unexpected FAIL lines",
            )
        )
    if "engine=lockstep" in case.cmdline and not case.setup:
        for field, value in (("checked", 0), ("unchecked", 1)):
            out.append(
                (
                    f"lockstep {field} invalid",
                    case.rc,
                    re.sub(
                        r"(^\[info\] tier0 regions=[^\n]*?\b" + field + r"=)\d+",
                        rf"\g<1>{value}",
                        log,
                        flags=re.M,
                    ),
                    "lockstep coverage: ",
                )
            )
    if case.app in (
        "smp-ipi-itc",
        "smp-tlb-purge",
        "smp-long",
        "smp-quick",
    ) and "SMP-APP END" in guest_text(log):
        bad = log.replace("PASS ap-start", "FAIL ap-start injected")
        bad = re.sub(
            r"(SMP-APP END pass=)(\d+)( fail=)(\d+)",
            lambda m: f"{m[1]}{int(m[2]) - 1}{m[3]}{int(m[4]) + 1}",
            bad,
        )
        out.append(("consistent ap-start FAIL and END counts", case.rc, bad, "ap-start: FAIL "))
        out.append(
            (
                "wrong END cpus",
                case.rc,
                re.sub(r"(SMP-APP END[^\n]* cpus=)\d+", r"\g<1>1", log),
                "smp.efi saw 1 CPUs, expected ",
            )
        )
        wrong_profile = "ipi-itc" if case.app == "smp-quick" else "quick"
        out.append(
            (
                "wrong profile",
                case.rc,
                re.sub(r"(SMP-APP BEGIN profile=)[\w-]+", rf"\g<1>{wrong_profile}", log),
                f"smp.efi profile {wrong_profile}, expected ",
            )
        )
        out.append(
            (
                "SMP-APP END removed",
                case.rc,
                re.sub(r"^\[guest\] SMP-APP END.*$", "", log, flags=re.M),
                "smp.efi output: ",
            )
        )
    if case.app in ("restart", "smp-restart"):
        out.append(
            (
                "restart line removed",
                case.rc,
                RESTART_LINE.sub("", log.replace("\r", ""), count=1),
                "restart: ",
            )
        )
        out.append(
            (
                "VMM reserve leaked",
                case.rc,
                re.sub(
                    r"(VMM reserve total_bytes=\d+ used_bytes=)(\d+)( free_bytes=\d+\n(?:.*\n)*?.*guest restart)",
                    lambda m: m[1] + str(int(m[2]) + 4096) + m[3],
                    log.replace("\r", ""),
                    count=1,
                ),
                "restart: VMM reserve use at boot start differs",
            )
        )
    if case.app == "debugserial":
        out.append(
            (
                "UART payload corrupted",
                case.rc,
                re.sub(
                    r"^(\[vmm-serial\] [0-9a-f]{16} [0-9a-f]{4} )([0-9a-f]+)",
                    lambda m: m[1] + "00" * (len(m[2]) // 2),
                    log,
                    flags=re.M,
                )
                if "debug_serial=off" not in case.cmdline
                else log
                + "\n[vmm-serial] 0000000000000000 0001 00\n[vmm-serial-end] 0000000000000001\n",
                "debug serial: byte-exact payload disagrees with mode",
            )
        )
    return out


def mutation_problems(case, log, elapsed):
    found = []
    for description, mrc, mlog, reason in mutations(case, log):
        # The executor-bound mutant must reach that bound check independently
        # of the real runner duration. All other copies retain the measured time.
        seconds = (
            max(FAULTLOOP_WALL_MS / 1000, elapsed)
            if description == "executor exceeds faultloop bound"
            else elapsed
        )
        errors = problems(case, mrc, mlog, seconds)
        if not any(error.startswith(reason) for error in errors):
            found.append(f"mutation {description}: expected {reason!r}, got {errors}")
        if case.expect_problems and expected_defect(case, errors):
            found.append(f"mutation {description}: accepted as the expected defect")
    return found


def run_cases(selected, jobs):
    """Bound runners and QEMU CPUs; drain without new boots after a timeout."""
    remaining = list(selected)
    host_cpus = len(os.sched_getaffinity(0))
    stopped = False
    runners = RunnerGroup()
    pool = ThreadPoolExecutor(max_workers=jobs)
    try:
        pending = {}
        while True:
            while not stopped and remaining and len(pending) < jobs:
                case = remaining[0]
                if pending and sum(c.cpus for c in pending.values()) + case.cpus > host_cpus:
                    break
                remaining.pop(0)
                pending[pool.submit(run, case, runners)] = case
            if not pending:
                return
            done, _ = wait(pending, return_when=FIRST_COMPLETED)
            completed = []
            for future in done:
                case = pending.pop(future)
                try:
                    result = future.result()
                except Exception as error:
                    result = (-2, "", f"worker exception: {type(error).__name__}: {error}", 0, None)
                completed.append((case, result))
            stopped |= any(result[0] == -1 for _, result in completed)
            yield from completed
    finally:
        # A repeated Ctrl-C must not interrupt cleanup before the pool joins.
        handlers = {
            sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)
        }
        try:
            runners.abort()
            pool.shutdown(wait=True, cancel_futures=True)
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(3, max(1, len(os.sched_getaffinity(0)) // 4)),
        choices=[1, 2, 3],
        help="concurrent guests (default: up to three, by host CPU count)",
    )
    parser.add_argument("--only", help="comma-separated case names (including the broken-* ones)")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--full", action="store_true", help="include extended product coverage")
    parser.add_argument(
        "--extended-only", action="store_true", help="run only the --full extension"
    )
    args = parser.parse_args()
    if args.extended_only and not args.full:
        parser.error("--extended-only requires --full")
    positive = ([] if args.extended_only else cases()) + (full_cases() if args.full else [])
    negative = [] if args.extended_only else broken_guest_inputs()
    if args.list:
        print(" ".join(c.name for c in positive + negative))
        return 0
    if args.only:
        wanted = set(args.only.split(","))
        unknown = wanted - {c.name for c in positive + negative}
        if unknown:
            parser.error("unknown case(s): " + ", ".join(sorted(unknown)))
        positive, negative = (
            [c for c in group if c.name in wanted] for group in (positive, negative)
        )
    started = time.monotonic()
    failures = 0
    rejected = 0
    checked = 0
    aborted = False
    try:
        with closing(run_cases(positive + negative, args.jobs)) as results:
            for case, result in results:
                rc, log, output, elapsed, path = result
                found = problems(case, rc, log, elapsed)
                if case.expect_problems:
                    ok = expected_defect(case, found)
                    status = "rejected as designed" if ok else "NOT REJECTED"
                else:
                    ok = not found
                    status = "ok" if ok else "FAILED"
                    if ok and case.must_reject:
                        defects = case.must_reject(guest_text(log))
                        if len(defects) != 1 or not defects[0].startswith("sal-ap-wake: FAIL "):
                            ok, status = False, "NOT REJECTED for exactly the bad wake vector"
                            found += defects
                if ok:
                    errors = mutation_problems(case, log, elapsed)
                    rejected += len(mutations(case, log))
                    if errors:
                        ok, status = False, "FAILED mutation self-check"
                        found += errors
                checked += 1
                failures += not ok
                record = json.loads(path.with_suffix(".json").read_text()) if path else {}
                load = record.get("load_before", smplog.loadavg())
                print(
                    f"qemu-apps: {case.name} ({case.firmware}, {elapsed:.1f} s, load {load}): {status}",
                    flush=True,
                )
                if not ok:
                    for line in found:
                        print(f"    {line}")
                    if rc == -1 or not log:
                        print(output[-2000:])
                    print(f"    log: {path}")
                if record.get("timed_out"):
                    print(
                        "qemu-apps: no new boots after runner timeout; draining active guests",
                        flush=True,
                    )
    except KeyboardInterrupt:
        aborted = True
        print("qemu-apps: interrupted; runners terminated", flush=True)
    total = time.monotonic() - started
    print(
        f"qemu-apps: {len(positive)} runs and {len(negative)} broken-guest runs, {args.jobs} worker(s), "
        f"{rejected} broken log copies rejected, {failures} failure(s), {total:.0f} s"
        + (f" (stopped after {checked} cases)" if checked != len(positive) + len(negative) else "")
    )
    return 130 if aborted else (1 if failures else 0)


def interrupted(signum, frame):
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, signal.SIG_IGN)
    raise KeyboardInterrupt


if __name__ == "__main__":
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    sys.exit(main())
