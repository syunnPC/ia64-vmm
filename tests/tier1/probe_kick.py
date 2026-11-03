#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare the existing kick-delivery check with built baseline/candidate objects.

Only the dispatch suite's entry point is replaced in a temporary source copy.
The kicker, retry sequence, required coverage, delivery bound and watchdog are
unchanged. No production source or object in either checkout is modified.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shlex
import subprocess
import time


SOURCE = "tests/tier1/dispatch_tests.c"
TARGET = "build/tier1-test"
DISPATCH_OBJECT = "build/hosted/tests/tier1/dispatch_tests.o"
DRIVER = r"""
void dispatch_tests(void);
void dispatch_tests(void)
{
    snapshot_init(&m.snap);
    result_init(&m.reference);
    result_init(&m.actual);
    const struct guest_program *p = guest_program("gather_phys");
    const struct t1h_engine *engine = t1h_engine_named("@ENGINE@");
    T1H_CHECK(engine != NULL);
    machine_open(p, guest_completion_params(p));
    reference_whole(NULL, true);
    m.total = m.reference.v.retired;
    printf("kick-probe: engine=%s total=%llu trials=24 required=12 watchdog=%us\n",
        engine->name, (unsigned long long)m.total, (unsigned)T1H_WATCHDOG_SECONDS);
    fflush(stdout);
    kick_delivery(engine, 24);
    machine_close();
    result_free(&m.reference);
    result_free(&m.actual);
    snapshot_free(&m.snap);
}
"""


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(work, *args):
    return subprocess.check_output(["git", *args], cwd=work, text=True).strip()


def build_probe(work, output, variant, source, make_args):
    # Use the same dry-run recipe discovery as kick_mutants.py. The actual
    # commands below compile only the copied fixture and link existing objects.
    plan = subprocess.check_output(
        ["make", "--no-print-directory", "-Bn", *make_args, TARGET], cwd=work, text=True
    )
    (output / (variant + "-make-plan.txt")).write_text(plan)
    commands = []
    for line in plan.replace("\\\n", " ").splitlines():
        if SOURCE not in line and TARGET not in line:
            continue
        try:
            commands.append(shlex.split(line))
        except ValueError:
            continue
    compile_cmd = next(
        c[:]
        for c in commands
        if "-c" in c and SOURCE in c and "-o" in c and c[c.index("-o") + 1] == DISPATCH_OBJECT
    )
    link = next(
        c[:]
        for c in reversed(commands)
        if "-c" not in c and "-o" in c and c[c.index("-o") + 1] == TARGET
    )
    original_objects = [work / arg for arg in link if arg.endswith(".o")]
    missing = [str(path) for path in original_objects if not path.is_file()]
    if missing:
        raise RuntimeError("build the normal tier1-test objects first: " + ", ".join(missing))
    if DISPATCH_OBJECT not in link:
        raise RuntimeError("the link recipe does not contain the dispatch object")
    obj = output / (variant + "-dispatch.o")
    binary = output / (variant + "-kick")
    compile_cmd[compile_cmd.index(SOURCE)] = str(source)
    compile_cmd[compile_cmd.index("-o") + 1] = str(obj)
    link_cmd = [
        str(obj) if arg == DISPATCH_OBJECT else str(work / arg) if arg.endswith(".o") else arg
        for arg in link
    ]
    link_cmd[link_cmd.index("-o") + 1] = str(binary)
    with (output / (variant + "-build.log")).open("w") as log:
        subprocess.run(compile_cmd, cwd=work, check=True, stdout=log, stderr=log)
        subprocess.run(link_cmd, cwd=work, check=True, stdout=log, stderr=log)
    return binary, dict(
        checkout=str(work),
        commit=git(work, "rev-parse", "HEAD"),
        source_tree=git(work, "rev-parse", "HEAD^{tree}"),
        tracked_changes=git(work, "status", "--porcelain", "--untracked-files=no"),
        binary_sha256=digest(binary),
        compile=compile_cmd,
        link=link_cmd,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True, help="built baseline checkout")
    parser.add_argument("--candidate", type=Path, required=True, help="built candidate checkout")
    parser.add_argument(
        "--engine", choices=("interp", "tier0", "tier1", "tier0+tier1"), default="interp"
    )
    parser.add_argument(
        "--rounds", type=int, default=1, help="fresh processes per version (default: 1)"
    )
    parser.add_argument(
        "--cpu", help="CPU list/ranges, e.g. 0 or 0-3,7; default: inherited affinity"
    )
    parser.add_argument(
        "--image", type=Path, help="shared gather_phys.bin; default: baseline build image"
    )
    parser.add_argument(
        "--output", type=Path, required=True, help="new directory for binaries, logs and JSON"
    )
    parser.add_argument(
        "--timeout", type=float, default=300, help="outer process timeout in seconds (default: 300)"
    )
    parser.add_argument(
        "--make-arg",
        action="append",
        default=[],
        help="extra make variable argument; repeat as needed",
    )
    args = parser.parse_args()
    if args.rounds < 1 or args.timeout <= 0:
        parser.error("--rounds and --timeout must be positive")
    allowed = set(os.sched_getaffinity(0))
    affinity = set(allowed)
    if args.cpu:
        affinity = set()
        try:
            for part in args.cpu.split(","):
                ends = [int(x) for x in part.split("-")]
                if len(ends) == 1:
                    affinity.add(ends[0])
                elif len(ends) == 2 and ends[0] <= ends[1]:
                    affinity.update(range(ends[0], ends[1] + 1))
                else:
                    raise ValueError()
        except ValueError:
            parser.error("invalid --cpu list")
        if not affinity or not affinity <= allowed:
            parser.error("--cpu must be a nonempty subset of the inherited CPU affinity")
    work = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    image = (args.image or work["baseline"] / "build/tests/tier1/gather_phys.bin").resolve()
    if not image.is_file():
        parser.error("missing shared image: " + str(image))
    baseline_source = (work["baseline"] / SOURCE).read_text()
    if baseline_source != (work["candidate"] / SOURCE).read_text():
        parser.error("baseline and candidate must have identical dispatch_tests.c fixtures")
    if baseline_source.count("void dispatch_tests(void)") != 2:
        parser.error("unrecognized dispatch suite entry point")
    output = args.output.resolve()
    if output.exists():
        parser.error("--output must name a new directory to preserve previous evidence")
    output.mkdir(parents=True)
    source = output / "dispatch_kick_only.c"
    source.write_text(
        baseline_source.replace("void dispatch_tests(void)", "void dispatch_full_tests(void)")
        + DRIVER.replace("@ENGINE@", args.engine)
    )
    record = dict(
        engine=args.engine,
        rounds=args.rounds,
        affinity=sorted(affinity),
        cpu_option=args.cpu,
        image=str(image),
        image_sha256=digest(image),
        original_dispatch_sha256=hashlib.sha256(baseline_source.encode()).hexdigest(),
        probe_source_sha256=digest(source),
        variants={},
        runs=[],
    )
    binaries = {}
    for variant, checkout in work.items():
        binaries[variant], record["variants"][variant] = build_probe(
            checkout, output, variant, source, args.make_arg
        )

    def child_setup():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        if args.cpu:
            os.sched_setaffinity(0, affinity)

    for sample in range(args.rounds):
        order = ("baseline", "candidate") if sample % 2 == 0 else ("candidate", "baseline")
        for variant in order:
            stem = output / ("%03d-%s" % (sample, variant))
            command = [str(binaries[variant]), "--suite=dispatch", str(image)]
            started = time.monotonic()
            with (
                stem.with_suffix(".stdout").open("w") as out,
                stem.with_suffix(".stderr").open("w") as err,
            ):
                try:
                    result = subprocess.run(
                        command,
                        cwd=work[variant],
                        stdout=out,
                        stderr=err,
                        preexec_fn=child_setup,
                        timeout=args.timeout,
                    )
                    status = result.returncode
                except subprocess.TimeoutExpired:
                    status = "outer timeout after %g seconds" % args.timeout
            row = dict(
                variant=variant,
                sample=sample,
                command=command,
                exit_code=status,
                seconds=time.monotonic() - started,
                stdout=stem.with_suffix(".stdout").read_text(),
                stderr=stem.with_suffix(".stderr").read_text(),
            )
            record["runs"].append(row)
            (output / "results.json").write_text(json.dumps(record, indent=2) + "\n")
            print(
                "%s round %d: exit=%s %.3fs" % (variant, sample, status, row["seconds"]), flush=True
            )
            if row["stderr"]:
                print(row["stderr"], end="", flush=True)
    return int(any(row["exit_code"] != 0 for row in record["runs"]))


if __name__ == "__main__":
    raise SystemExit(main())
