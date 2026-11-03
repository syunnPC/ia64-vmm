# SPDX-License-Identifier: GPL-2.0-or-later
"""Paired single-vCPU loop-program and compiled-C measurements against a chosen revision.

Private objects and binaries only; never changes the checkout. Build
build/tier1-test, build/bench-jobdump and build/tests/smp-purge-exec-bench
first. Each sample records load and pins to one CPU from the process's
current affinity.
"""

import argparse
import json
import os
from pathlib import Path
import shlex
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]


def make_var(name):
    command = ["make", "-s", "--eval", f"purge-vars:;@echo $({name})", "purge-vars"]
    return shlex.split(subprocess.check_output(command, cwd=ROOT, text=True))


def baseline(out, revision):
    cc = make_var("HOSTCC")
    flags = make_var("HOSTED_CFLAGS") + make_var("CPPFLAGS") + make_var("CFLAGS")
    ldflags = make_var("HOSTED_LDFLAGS") + make_var("LDFLAGS")
    replacements = {}
    for name in ("mmu", "tlb", "smp_default"):
        source = out / f"{name}-baseline.c"
        source.write_bytes(
            subprocess.check_output(["git", "show", f"{revision}:ia64/{name}.c"], cwd=ROOT)
        )
        obj = source.with_suffix(".o")
        subprocess.run(cc + flags + ["-c", str(source), "-o", str(obj)], cwd=ROOT, check=True)
        replacements[f"build/hosted/ia64/{name}.o"] = str(obj)
    objects = make_var("T1_TEST_OBJECTS")
    jobs = [obj for obj in objects if obj != "build/hosted/tests/tier1/main.o"]
    jobs.append("build/hosted/tests/bench/jobdump.o")
    for name, inputs in (("tier1-baseline", objects), ("jobdump-baseline", jobs)):
        subprocess.run(
            cc + [replacements.get(obj, obj) for obj in inputs] + ldflags + ["-o", str(out / name)],
            cwd=ROOT,
            check=True,
        )

    source = out / "smp-baseline.c"
    source.write_bytes(subprocess.check_output(["git", "show", f"{revision}:core/smp.c"], cwd=ROOT))
    obj = source.with_suffix(".o")
    subprocess.run(cc + flags + ["-c", str(source), "-o", str(obj)], cwd=ROOT, check=True)
    inputs = make_var("SMP_PURGE_BENCH_OBJECTS")
    replacements["build/hosted/core/smp.o"] = str(obj)
    subprocess.run(
        cc
        + [replacements.get(item, item) for item in inputs]
        + ["-pthread", "-Wl,--wrap=engine_run"]
        + ldflags
        + ["-o", str(out / "executor-baseline")],
        cwd=ROOT,
        check=True,
    )


def cpu_ticks():
    result = {}
    for line in Path("/proc/stat").read_text().splitlines():
        fields = line.split()
        if fields[0].startswith("cpu") and fields[0] != "cpu":
            ticks = list(map(int, fields[1:]))
            result[int(fields[0][3:])] = (sum(ticks), ticks[3] + ticks[4])
    return result


def choose_cpu():
    before = cpu_ticks()
    time.sleep(1)
    after = cpu_ticks()
    return min(
        os.sched_getaffinity(0),
        key=lambda cpu: (
            1 - (after[cpu][1] - before[cpu][1]) / max(1, after[cpu][0] - before[cpu][0])
        ),
    )


def sample(command, cpu, env):
    before = os.getloadavg()
    done = subprocess.run(
        ["taskset", "-c", str(cpu)] + command,
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        check=True,
    )
    return done.stdout, {"cpu": cpu, "load_before": before, "load_after": os.getloadavg()}


def order(round_number):
    return ("baseline", "current") if round_number % 2 == 0 else ("current", "baseline")


def print_comparison(label, engine, cell, metric):
    old = statistics.median(row[metric] for row in cell if row["flavor"] == "baseline")
    new = statistics.median(row[metric] for row in cell if row["flavor"] == "current")
    print(
        f"{label} {engine}: baseline={old:.3f} current={new:.3f} "
        f"{metric} delta={(new / old - 1) * 100:.3f}%",
        flush=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-revision", help="baseline revision (default: merge-base with main)")
    parser.add_argument("--executor-slices", type=int, default=1000000)
    parser.add_argument("--executor-only", action="store_true")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--segment", type=int, default=4000000)
    parser.add_argument("--out", type=Path, default=Path("build/purge-bench-final"))
    args = parser.parse_args()
    if args.rounds < 1 or args.segment < 1 or args.executor_slices < 1:
        parser.error("rounds, segment and executor-slices must be positive")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    revision = (
        args.base_revision
        or subprocess.check_output(
            ["git", "merge-base", "HEAD", "main"], cwd=ROOT, text=True
        ).strip()
    )
    revision = subprocess.check_output(["git", "rev-parse", revision], cwd=ROOT, text=True).strip()
    (out / "baseline.json").write_text(
        json.dumps({"revision": revision, "load": os.getloadavg()}, indent=2)
    )
    baseline(out, revision)
    images = make_var("T1_TEST_IMAGES")
    cpu = choose_cpu()
    env = dict(os.environ, T1H_HOSTWIN_CHECK="0")
    records = []
    for rnd in range(args.rounds):
        for flavor in order(rnd):
            binary = (
                out / "executor-baseline"
                if flavor == "baseline"
                else ROOT / "build/tests/smp-purge-exec-bench"
            )
            stdout, record = sample([str(binary), str(args.executor_slices)], cpu, env)
            fields = dict(item.split("=") for item in stdout.strip().split()[1:])
            assert int(fields["slices"]) == args.executor_slices
            record.update(round=rnd, flavor=flavor, ns=int(fields["ns"]))
            records.append(record)
            (out / "executor.json").write_text(json.dumps(records, indent=2))
    print_comparison("executor", "single-vcpu", records, "ns")
    if args.executor_only:
        return
    records = []
    for rnd in range(args.rounds):
        for program in ("speed", "gather_phys", "gather_virt", "shladd_xor"):
            for engine in ("interp", "tier0"):
                for flavor in order(rnd):
                    binary = (
                        out / "tier1-baseline"
                        if flavor == "baseline"
                        else ROOT / "build/tier1-test"
                    )
                    stdout, record = sample(
                        [str(binary), f"--bench={engine}:{program}:{args.segment}"] + images,
                        cpu,
                        env,
                    )
                    line = next(line for line in stdout.splitlines() if line.startswith("bench "))
                    fields = line.split()
                    record.update(
                        round=rnd,
                        program=program,
                        engine=engine,
                        flavor=flavor,
                        line=line,
                        cold=float(fields[6]),
                        warm=float(fields[8]),
                    )
                    records.append(record)
                    (out / "asm.json").write_text(json.dumps(records, indent=2))
    for program in ("speed", "gather_phys", "gather_virt", "shladd_xor"):
        for engine in ("interp", "tier0"):
            cell = [row for row in records if row["program"] == program and row["engine"] == engine]
            print_comparison(program, engine, cell, "warm")

    # Snapshot each C kernel once; both timed binaries use that same job.
    records = []
    for kernel in range(6):
        job = out / f"csuite-{kernel}.job"
        subprocess.run(
            [
                str(ROOT / "build/bench-jobdump"),
                f"--dump=csuite:{kernel}",
                f"--insns={args.segment},{args.segment}",
                f"--out={job}",
            ]
            + images,
            cwd=ROOT,
            env=env,
            check=True,
            capture_output=True,
        )
        for rnd in range(args.rounds):
            for engine in ("interp", "tier0"):
                for flavor in order(rnd):
                    binary = (
                        out / "jobdump-baseline"
                        if flavor == "baseline"
                        else ROOT / "build/bench-jobdump"
                    )
                    stdout, record = sample(
                        [str(binary), f"--run={engine}", f"--job={job}", "--variants=0,1,1,1"]
                        + images,
                        cpu,
                        env,
                    )
                    lines = [line for line in stdout.splitlines() if line.startswith("jobrun ")]
                    assert len(lines) == 4 and all("ok 1" in line for line in lines)
                    ns = [int(line.split()[line.split().index("ns") + 1]) for line in lines[1:]]
                    record.update(
                        round=rnd,
                        kernel=kernel,
                        engine=engine,
                        flavor=flavor,
                        lines=lines,
                        warm_ns=sum(ns) / len(ns),
                    )
                    records.append(record)
                    (out / "csuite.json").write_text(json.dumps(records, indent=2))
        for engine in ("interp", "tier0"):
            cell = [row for row in records if row["kernel"] == kernel and row["engine"] == engine]
            print_comparison(f"csuite {kernel}", engine, cell, "warm_ns")


if __name__ == "__main__":
    main()
