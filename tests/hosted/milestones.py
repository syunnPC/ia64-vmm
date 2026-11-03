#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks the executor's milestone lines in a
VMM log (QEMU serial log):

    milestone NAME retired=N exec_ms=N idle_ms=N [compile_ms=N] wall_ms=N

NAME is ebs, loader_resumed or kernel_entry (after the executor's EFI line
of that milestone) or exit (once, when the executor stops). Every value is
a total since the executor started. Rules: every line is well formed; the
EFI milestones appear at most once each and in their order; each pairs
with its EFI line: that line appears exactly once in the log, after the
previous milestone line and before this one (other lines may come in
between: threaded runs write direct-sink frames such as [vmm-serial]
between ring batches, and with guest SMP other executors log too); an EFI
line without its milestone line fails; exit appears exactly once and last; retired, exec_ms,
idle_ms, compile_ms and wall_ms never decrease; exec_ms + idle_ms <=
wall_ms on every line (they are disjoint intervals of the executor's
wall time); compile_ms (translation inside engine_run, statistics builds
only) is on every line or on none and never exceeds exec_ms; the last
wall_ms is within the runner's own wall time. A run with in-VMM restarts
logs `[info] guest restart N ...` after each guest boot
but the last: every boot is checked on its own by these rules, and the
required milestones apply to the last boot.

Imported by the QEMU runners (tests/qemu/apps.py, tools/qemu/check-log.py);
`milestones.py --self-test` checks the checker against broken logs (part of `make test`); `milestones.py LOG
[NAME...]` checks a saved log.
"""

import re
import sys

LINE = re.compile(
    r"^\[info\] milestone (?P<name>[a-z_]+) retired=(?P<retired>\d+) "
    r"exec_ms=(?P<exec_ms>\d+) idle_ms=(?P<idle_ms>\d+)"
    r"(?: compile_ms=(?P<compile_ms>\d+))? wall_ms=(?P<wall_ms>\d+)$"
)
# A VMM log line (any level) whose message is a milestone; guest text is not.
CANDIDATE = re.compile(r"^\[(?:error|warn|info|debug)\] milestone\b")
# The executor's EFI line that each milestone line follows (core/vm.c).
SOURCES = {
    "ebs": "[info] EFI first IP after ExitBootServices ip=",
    "loader_resumed": "[info] EFI loader resumed ip=",
    "kernel_entry": "[info] EFI loader done ip=",
}
ORDER = ["ebs", "loader_resumed", "kernel_entry", "exit"]
COUNTERS = ("retired", "exec_ms", "idle_ms", "compile_ms", "wall_ms")


class MilestoneError(Exception):
    pass


def parse(log):
    """Milestone records in log order: dicts of name and integer counters."""
    if isinstance(log, bytes):
        log = log.decode(errors="replace")
    lines = log.splitlines()
    records = []
    since = 0
    for at, line in enumerate(lines):
        if not CANDIDATE.match(line):
            continue
        match = LINE.match(line)
        if not match:
            raise MilestoneError(f"malformed milestone line: {line!r}")
        # The lines between the previous milestone line and this one.
        record = {"name": match["name"], "line": line, "window": lines[since:at]}
        since = at + 1
        for key in COUNTERS:
            record[key] = None if match[key] is None else int(match[key])
        records.append(record)
    return records


RESTART = re.compile(r"^\[info\] guest restart \d+ ")


def check(log, required=("exit",), runner_seconds=None):
    """Returns the records of every guest boot; raises MilestoneError on the
    first violation."""
    text = log.decode(errors="replace") if isinstance(log, bytes) else log
    boots, current = [], []
    for line in text.splitlines():
        if RESTART.match(line):
            boots.append(current)
            current = []
        else:
            current.append(line)
    boots.append(current)
    records = []
    for at, lines in enumerate(boots):
        try:
            records += check_boot(
                "\n".join(lines), required if at == len(boots) - 1 else ("exit",), runner_seconds
            )
        except MilestoneError as error:
            raise MilestoneError(
                f"{error} in guest boot {at + 1}" if len(boots) > 1 else str(error)
            ) from None
    return records


def check_boot(log, required, runner_seconds):
    records = parse(log)
    text = log
    names = [r["name"] for r in records]
    for name in names:
        if name not in ORDER:
            raise MilestoneError(f"unknown milestone {name!r}")
    for name in required:
        if name not in names:
            raise MilestoneError(f"milestone {name} missing (logged: {names})")
    if names.count("exit") != 1 or names[-1] != "exit":
        raise MilestoneError(f"exit must appear exactly once, last (logged: {names})")
    positions = [ORDER.index(n) for n in names]
    if positions != sorted(positions) or len(set(names)) != len(names):
        raise MilestoneError(f"milestones out of order or repeated: {names}")
    for name, source in SOURCES.items():
        logged = sum(line.startswith(source) for line in text.splitlines())
        if logged != names.count(name):
            raise MilestoneError(
                f'{logged} "{source}" lines but {names.count(name)} milestone {name} lines'
            )
    for r in records:
        source = SOURCES.get(r["name"])
        if source and not any(line.startswith(source) for line in r["window"]):
            raise MilestoneError(
                f"milestone {r['name']} without its EFI line since the previous milestone line"
            )
        if r["exec_ms"] + r["idle_ms"] > r["wall_ms"]:
            raise MilestoneError(f"exec_ms + idle_ms exceeds wall_ms: {r['line']!r}")
        if r["compile_ms"] is not None and r["compile_ms"] > r["exec_ms"]:
            raise MilestoneError(f"compile_ms exceeds exec_ms: {r['line']!r}")
    if len({r["compile_ms"] is None for r in records}) > 1:
        raise MilestoneError("compile_ms on some milestone lines only")
    for before, after in zip(records, records[1:]):
        for key in COUNTERS:
            if before[key] is not None and after[key] < before[key]:
                raise MilestoneError(
                    f"{key} decreases from {before['name']} to {after['name']}: "
                    f"{before[key]} -> {after[key]}"
                )
    if records[-1]["retired"] == 0:
        raise MilestoneError("exit milestone with no retired instruction")
    if runner_seconds is not None and records[-1]["wall_ms"] > runner_seconds * 1000:
        raise MilestoneError(
            f"executor wall_ms {records[-1]['wall_ms']} exceeds the runner's "
            f"{runner_seconds * 1000:.0f} ms"
        )
    return records


def summary(records):
    return ", ".join(
        f"{r['name']} {r['retired']}/{r['exec_ms']}/{r['idle_ms']}"
        f"{'' if r['compile_ms'] is None else '/' + str(r['compile_ms'])}/{r['wall_ms']}"
        for r in records
    )


def self_test():
    ebs = "[info] EFI first IP after ExitBootServices ip=1df1f0 slot=1"
    resumed = "[info] EFI loader resumed ip=1df313a0 slot=0"
    done = "[info] EFI loader done ip=4000 slot=0 allocation=4000+1000"

    def m(name, retired, exec_ms, idle_ms, wall_ms, compile_ms=None):
        compile = "" if compile_ms is None else f" compile_ms={compile_ms}"
        return (
            f"[info] milestone {name} retired={retired} exec_ms={exec_ms} "
            f"idle_ms={idle_ms}{compile} wall_ms={wall_ms}"
        )

    good = [
        ebs,
        m("ebs", 100, 10, 2, 13),
        resumed,
        m("loader_resumed", 101, 10, 2, 14),
        "[guest] text",
        done,
        m("kernel_entry", 200, 20, 5, 30),
        "[info] EFI ResetSystem type=2 status=0 ip=13af80 slot=0",
        m("exit", 900, 50, 9, 60),
    ]
    check("\n".join(good), required=ORDER)
    # Threaded hosted runs: direct-sink frames land between ring batches.
    check(
        "\n".join(
            good[:5]
            + [
                done,
                "",
                "[vmm-serial] 0000000000000000 0002 0d0a",
                "[guest] x",
                m("kernel_entry", 200, 20, 5, 30),
            ]
            + good[7:]
        ),
        required=ORDER,
    )
    check("\n".join(good).replace(" wall_ms=", " compile_ms=1 wall_ms="), required=ORDER)
    check(m("exit", 5, 0, 0, 0), runner_seconds=0.001)
    restart = "[info] guest restart 1 source=efi type=warm status=0"
    restarted = [m("exit", 900, 50, 9, 60), restart] + good
    check("\n".join(restarted), required=ORDER)

    def broken(lines, **kw):
        try:
            check("\n".join(lines), **kw)
        except MilestoneError as error:
            return str(error)
        raise AssertionError(f"accepted a broken log: {lines}")

    cases = {
        "retired decreases": good[:3] + [m("loader_resumed", 99, 10, 2, 14)] + good[4:],
        "exec decreases": good[:6] + [m("kernel_entry", 200, 9, 5, 30)] + good[7:],
        "idle decreases": good[:8] + [m("exit", 900, 50, 1, 60)],
        "wall decreases": good[:8] + [m("exit", 900, 20, 9, 29)],
        "exec + idle beyond wall": good[:6] + [m("kernel_entry", 200, 26, 5, 30)] + good[7:],
        "compile beyond exec": [m("exit", 900, 50, 9, 60, compile_ms=51)],
        "compile decreases": [ebs, m("ebs", 1, 5, 0, 5, 4), m("exit", 2, 6, 0, 6, 3)],
        "compile on some lines": [ebs, m("ebs", 1, 5, 0, 5, 4), m("exit", 2, 6, 0, 6)],
        "malformed": good[:8] + ["[info] milestone exit retired=900 exec_ms=50 wall_ms=60"],
        "other log level": good[:8]
        + ["[warn] milestone exit retired=900 exec_ms=50 idle_ms=9 wall_ms=60"],
        "negative": good[:8]
        + ["[info] milestone exit retired=900 exec_ms=-1 idle_ms=9 wall_ms=60"],
        "unknown name": [m("image_start", 1, 0, 0, 0), m("exit", 2, 0, 0, 0)],
        "exit missing": good[:7],
        "exit twice": good + [m("exit", 901, 50, 9, 61)],
        "exit not last": good[:2] + [m("exit", 100, 10, 2, 13)] + good[2:7],
        "repeated": good[:2] + [ebs, m("ebs", 100, 10, 2, 13)] + good[2:],
        "out of order": [
            resumed,
            m("loader_resumed", 1, 0, 0, 0),
            ebs,
            m("ebs", 2, 0, 0, 0),
            m("exit", 3, 0, 0, 0),
        ],
        "EFI line missing": good[:5] + [m("kernel_entry", 200, 20, 5, 30)] + good[7:],
        "EFI line after its milestone": good[:5]
        + [m("kernel_entry", 200, 20, 5, 30), done]
        + good[7:],
        "EFI line without a milestone": good[:7] + [done] + good[7:],
        "EFI line before the previous milestone": [
            ebs,
            resumed,
            m("ebs", 100, 10, 2, 13),
            m("loader_resumed", 101, 10, 2, 14),
        ]
        + good[7:],
        "no retired instruction": [m("exit", 0, 0, 0, 0)],
        "restart without the first exit": [ebs, m("ebs", 1, 0, 0, 0), restart] + good,
        "exit missing after a restart": restarted[:-1],
        "exit before the restart line repeated": [
            m("exit", 9, 0, 0, 0),
            m("exit", 9, 0, 0, 0),
            restart,
        ]
        + good,
    }
    for what, lines in cases.items():
        print(f"milestones self-test: {what}: rejected ({broken(lines)})")
    print(
        f"milestones self-test: required kernel_entry: rejected "
        f"({broken([ebs, m('ebs', 1, 0, 0, 0), m('exit', 2, 0, 0, 0)], required=('kernel_entry',))})"
    )
    print(
        f"milestones self-test: beyond the runner's wall time: rejected "
        f"({broken([m('exit', 5, 0, 0, 1500)], runner_seconds=1.4)})"
    )
    print(
        f"milestones self-test: a later boot beyond the runner's wall time: rejected "
        f"({broken(restarted[:-1] + [m('exit', 900, 50, 9, 2000)], runner_seconds=1.4)})"
    )
    print(f"milestones self-test: {len(cases) + 3} broken logs rejected, 5 good logs accepted")


def main():
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        return
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        log = f.read()
    try:
        records = check(log, required=sys.argv[2:] or ("exit",))
    except MilestoneError as error:
        sys.exit(f"milestones: {sys.argv[1]}: {error}")
    print(f"milestones: {sys.argv[1]}: {summary(records)}")


if __name__ == "__main__":
    main()
