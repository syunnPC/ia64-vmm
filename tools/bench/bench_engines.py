#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Engine benchmarks: interp, Tier0, Tier1 (hosted Tier1 harness) and the
QEMU fork's TCG (benchwrap.efi), interleaved.

  bench_engines.py prepare --suite asm|csuite|fpsuite
      job records (build/bench-jobdump), plans and benchwrap images
      (fpsuite: job records only, dumped with --fp=soft)
  bench_engines.py run --suite asm|csuite|fpsuite --rounds 5 --out FILE.json
      rounds of: harness samples (each program x engine, one process
      each), then each wrapper platform with plan P1 and plan P2 (order
      alternating by round); all pinned to one CPU, loadavg per sample;
      cells whose spread exceeds 10% get --reruns extra rounds
  bench_engines.py run --suite csuite|fpsuite --fp-modes soft,exact,experimental,approx
      each harness sample once per FP mode (bench-jobdump --fp=MODE), the
      modes adjacent and in alternating order per round; "fp_mode" in the
      sample. fpsuite has no wrapper samples. Default: fpsuite exact,
      csuite none.
  bench_engines.py report FILE.json [--md OUT]

Plans (same steps, same printed text, different instruction counts):
  P1 = per job [S, A, A, A], P2 = per job [S, B, B, B]
S is about 4M instructions (cold); A and B are warm runs. TCG uses
A ~ 100M+, B ~ 300M+; harness samples use the slow jobs (A ~ 20M+, B ~ 60M+).
Gather variants round up to whole passes.
"""

import argparse
import statistics
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402
import harness_bench  # noqa: E402
import wrap_run  # noqa: E402

BUILD = common.REPO / "build/bench"
JOBDUMP = common.REPO / "build/bench-jobdump"

SUITES = {
    # name: (programs, fast insns, slow insns, harness programs, harness engines)
    "asm": (
        ["speed", "gather_phys", "gather_virt", "shladd_xor"],
        "4000000,100000000,300000000",
        "4000000,20000000,60000000",
    ),
    "csuite": (
        [f"csuite:{k}" for k in range(6)],
        "4000000,100000000,300000000",
        "4000000,20000000,60000000",
    ),
    "fpsuite": (
        [f"fpsuite:{k}" for k in range(8)],
        "4000000,100000000,300000000",
        "4000000,20000000,60000000",
    ),
}
CSUITE_NAMES = ["crc32", "qsort", "matmul", "strsearch", "hash", "lz"]
FPSUITE_NAMES = ["matmul", "fft", "nbody", "trans", "reduce", "matmul-f", "parallel", "extended"]
FP_MODES = ["soft", "exact", "experimental", "approx"]
WRAP_PLATFORMS = [("tcg", "fast")]
WRAP_SUITES = ("asm", "csuite")


def images():
    found = harness_bench.images()
    for suite in ("csuite", "fpsuite"):
        image = common.REPO / f"build/bench/{suite}/{suite}.bin"
        if image.exists():
            found.append(str(image))
    return found


def job_file(program, size):
    name = program.replace(":", "-")
    if program == "speed":
        return BUILD / "jobs" / "speed.bin"
    return BUILD / "jobs" / f"{name}-{size}.bin"


def prepare(suite):
    programs, fast, slow = SUITES[suite]
    targets = [("make", ["make", "-j4", "build/bench-jobdump", "build/tier1-test"])]
    for name, cmd in targets:
        subprocess.run(cmd, cwd=common.REPO, check=True, stdout=subprocess.DEVNULL)
    jobs = []
    for p in programs:
        for size, insns in (("fast", fast), ("slow", slow)):
            out = job_file(p, size)
            if p == "speed":
                insns = "0"
            jobs.append((p, insns, out))
    jobs = list({str(o): (p, i, o) for p, i, o in jobs}.values())
    (BUILD / "jobs").mkdir(parents=True, exist_ok=True)

    image = BUILD / suite / f"{suite}.bin"
    newest = max([JOBDUMP.stat().st_mtime] + ([image.stat().st_mtime] if image.exists() else []))

    def dump(job):
        p, insns, out = job
        if out.exists() and out.stat().st_mtime > newest:
            return f"{out.name}: up to date"
        # The recorded checks are the soft-float's.
        fp = ["--fp=soft"] if suite == "fpsuite" else []
        r = subprocess.run(
            [str(JOBDUMP), f"--dump={p}", f"--insns={insns}", f"--out={out}"] + fp + images(),
            cwd=common.REPO,
            capture_output=True,
            text=True,
        )
        if r.returncode:
            raise SystemExit(f"jobdump {p}: {r.stderr}")
        out.with_suffix(".log").write_text(r.stdout)
        return r.stdout.strip()

    with ThreadPoolExecutor(max_workers=4) as pool:
        for line in pool.map(dump, jobs):
            print(line)
    if suite not in WRAP_SUITES:
        return
    plans = [
        (size, plan, pattern)
        for size in ("fast", "slow")
        for plan, pattern in (("p1", "0,1,1,1"), ("p2", "0,2,2,2"))
    ]
    # Wall-clock check plans (wallcheck): [S, A x 10] / [S, B x 10].
    plans += [("fast", "wall-p1", "0" + ",1" * 10), ("fast", "wall-p2", "0" + ",2" * 10)]
    for size, plan, pattern in plans:
        files = [str(job_file(p, size)) for p in programs]
        name = f"{suite}-{size}-{plan}" if not plan.startswith("wall") else f"{suite}-{plan}"
        subprocess.run(
            [
                sys.executable,
                "-I",
                str(common.REPO / "tools/bench/plan.py"),
                "--out",
                str(BUILD / "plans" / name / "jobs.bin"),
                "--pattern",
                pattern,
            ]
            + files,
            check=True,
        )
        subprocess.run(
            ["make", f"PLAN={name}", "bench-wrap"],
            cwd=common.REPO,
            check=True,
            stdout=subprocess.DEVNULL,
        )


def wrap_sample(platform, size, suite, plan, cpu):
    r = wrap_run.run(platform, BUILD / "plans" / f"{suite}-{size}-{plan}", cpu)
    r["plan_name"] = plan
    if not r["ok"]:
        print(f"  !! {platform} {plan} failed: rc {r['rc']} errors {r['errors'][:4]}", flush=True)
    return r


def number(text):
    """An integer (decimal or 0x hexadecimal), else the text."""
    try:
        return int(text, 0)
    except ValueError:
        return text


def harness_csuite_sample(engine, program, cpu, fp_mode=None):
    """One csuite or fpsuite job in the harness, one process: S (cold) then
    A three times (warm), as a wrapper process runs it. fp_mode: the
    bench-jobdump --fp mode."""
    before = common.loadavg()
    fp = [f"--fp={fp_mode}"] if fp_mode else []
    r = subprocess.run(
        common.pin(cpu)
        + [
            str(JOBDUMP),
            f"--run={engine}",
            f"--job={job_file(program, 'slow')}",
            "--variants=0,1,1,1",
        ]
        + fp
        + images(),
        cwd=common.REPO,
        capture_output=True,
        text=True,
        env=harness_bench.bench_env(),
    )
    out = {
        "engine": engine,
        "program": program,
        "loadavg_before": before,
        "loadavg_after": common.loadavg(),
        "rc": r.returncode,
        "runs": [],
    }
    if fp_mode:
        out["fp_mode"] = fp_mode
    for line in r.stdout.splitlines():
        f = line.split()
        if f and f[0] == "jobrun":
            run = {"variant": int(f[4]), "retired": int(f[6]), "ns": int(f[8]), "ok": int(f[10])}
            run.update({k: number(v) for k, v in zip(f[11::2], f[12::2])})
            out["runs"].append(run)
    if len(out["runs"]) == 4:
        rate = lambda x: x["retired"] / x["ns"] * 1e3
        out["cold"] = rate(out["runs"][0]) * 1e6
        out["warm"] = statistics.median(rate(x) for x in out["runs"][1:]) * 1e6
        out["ok"] = int(all(x["ok"] for x in out["runs"]))
        if engine in ("tier1", "tier0+tier1"):
            out["t1"] = {
                "tier1_retired": sum(x["tier1_retired"] for x in out["runs"][1:]),
                "lower_retired": sum(x["lower_retired"] for x in out["runs"][1:]),
                "compiles": sum(x["compiles"] for x in out["runs"]),
                "rejects": sum(x["rejects"] for x in out["runs"]),
            }
    else:
        out["stderr"] = r.stderr[-2000:]
        print(f"  !! harness {engine} {program} failed: {r.stderr[-300:]}", flush=True)
    return out


def run_round(suite, rnd, cpu, harness_cells=None, wrap_cells=None, segment40=True, fp_modes=None):
    """harness_cells: (program, engine, fp_mode) cells to run (None: all).
    fp_modes: the harness samples of csuite and fpsuite per FP mode, the
    modes of a cell adjacent, in reverse order in odd rounds."""
    programs = SUITES[suite][0]
    data = {"round": rnd, "harness": [], "harness40": [], "wrap": [], "t0": time.time()}
    engines = harness_bench.ENGINES
    if suite == "asm":
        for program in programs:
            for engine in engines:
                if harness_cells is not None and (program, engine, None) not in harness_cells:
                    continue
                s = harness_bench.sample(
                    str(common.REPO / "build/tier1-test"), cpu, engine, program, 0
                )
                data["harness"].append(s)
                if segment40 and program != "speed":
                    s = harness_bench.sample(
                        str(common.REPO / "build/tier1-test"), cpu, engine, program, 40000000
                    )
                    data["harness40"].append(s)
    else:
        modes = fp_modes or [None]
        if rnd % 2:
            modes = modes[::-1]
        for program in programs:
            for engine in engines:
                for mode in modes:
                    if harness_cells is not None and (program, engine, mode) not in harness_cells:
                        continue
                    data["harness"].append(harness_csuite_sample(engine, program, cpu, mode))
    order = ("p1", "p2") if rnd % 2 == 0 else ("p2", "p1")
    for platform, size in WRAP_PLATFORMS:
        if suite not in WRAP_SUITES or (wrap_cells is not None and platform not in wrap_cells):
            continue
        for plan in order:
            data["wrap"].append(wrap_sample(platform, size, suite, plan, cpu))
    data["t1"] = time.time()
    return data


def summarize_wrap(rounds):
    """Per (platform, job): per-process medians of cold/warm ITC rates."""
    cells = {}
    for rd in rounds:
        for r in rd["wrap"]:
            if not r.get("freq"):
                continue
            hz = r["freq"]["hz"]
            byjob = {}
            for s in r["steps"]:
                byjob.setdefault(s["job"], []).append(s)
            for job, steps in byjob.items():
                c = cells.setdefault((r["platform"], job), {"cold": [], "warm": [], "plan": []})
                rate = lambda s: s["retired"] / (s["itc"] / hz) / 1e6
                c["cold"].append(rate(steps[0]))
                c["warm"].append(statistics.median(rate(s) for s in steps[1:]))
                c["plan"].append(r["plan_name"])
    return cells


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--suite", default="asm", choices=SUITES)
    w = sub.add_parser("wallcheck")
    w.add_argument("--suite", default="asm", choices=WRAP_SUITES)
    w.add_argument("--platform", default="tcg", choices=wrap_run.PLATFORMS)
    w.add_argument("--rounds", type=int, default=5)
    w.add_argument("--out", required=True)
    r = sub.add_parser("run")
    r.add_argument("--suite", default="asm", choices=SUITES)
    r.add_argument("--rounds", type=int, default=5)
    r.add_argument("--reruns", type=int, default=1)
    r.add_argument("--max-spread", type=float, default=0.10)
    r.add_argument("--no-segment40", action="store_true")
    r.add_argument(
        "--fp-modes",
        help="csuite and fpsuite: comma list of bench-jobdump --fp modes "
        f"({','.join(FP_MODES)}); default: fpsuite exact, csuite none",
    )
    r.add_argument("--out", required=True)
    args = ap.parse_args()
    if args.cmd == "prepare":
        prepare(args.suite)
        return
    if args.cmd == "wallcheck":
        # Longer wall-clock differencing for a fast platform: plans
        # <suite>-wall-p1/p2 run each job [S, A x 10] / [S, B x 10].
        cpu, _ = common.pick_cpu()
        data = {
            "suite": args.suite,
            "cpu": cpu,
            "host": common.host_info(),
            "repo": common.git_describe(common.REPO),
            "rounds": [],
            "rerun_rounds": [],
        }
        for rnd in range(args.rounds):
            rd = {"round": rnd, "harness": [], "harness40": [], "wrap": [], "t0": time.time()}
            for plan in ("p1", "p2") if rnd % 2 == 0 else ("p2", "p1"):
                r = wrap_run.run(args.platform, BUILD / "plans" / f"{args.suite}-wall-{plan}", cpu)
                r["plan_name"] = plan
                rd["wrap"].append(r)
            rd["t1"] = time.time()
            data["rounds"].append(rd)
            common.write_json(args.out, data)
            print(
                f"wallcheck round {rnd}: walls "
                + ", ".join(f"{w['plan_name']} {w['wall']:.2f}s ok {w['ok']}" for w in rd["wrap"]),
                flush=True,
            )
        return
    fp_modes = (
        args.fp_modes.split(",")
        if args.fp_modes
        else ["exact"]
        if args.suite == "fpsuite"
        else None
    )
    if fp_modes and (args.suite not in ("csuite", "fpsuite") or not set(fp_modes) <= set(FP_MODES)):
        raise SystemExit(
            f"bench_engines: --fp-modes is for csuite and fpsuite, from {','.join(FP_MODES)}"
        )
    cpu, busy = common.pick_cpu()
    data = {
        "suite": args.suite,
        "cpu": cpu,
        "cpu_busy_at_start": busy,
        "host": common.host_info(),
        "repo": common.git_describe(common.REPO),
        "max_spread": args.max_spread,
        "rounds": [],
        "rerun_rounds": [],
    }
    if fp_modes:
        data["fp_modes"] = fp_modes
    fork = common.load_env("IA64_FORK_DIR", required=False)["IA64_FORK_DIR"]
    if fork:
        data["fork"] = common.git_describe(fork)
    print(f"bench_engines: {args.suite}, pinned to CPU {cpu}", flush=True)
    for rnd in range(args.rounds):
        rd = run_round(args.suite, rnd, cpu, segment40=not args.no_segment40, fp_modes=fp_modes)
        data["rounds"].append(rd)
        common.write_json(args.out, data)
        cells = summarize_wrap([rd])
        print(
            f"round {rnd}: load {common.loadavg()[0]:.1f}; "
            + "; ".join(
                f"{pl}/{job} warm {statistics.median(c['warm']):.0f}"
                for (pl, job), c in sorted(cells.items())
            ),
            flush=True,
        )
        if fp_modes:
            for s in rd["harness"]:
                print(
                    f"  {s['program']:10} {s['engine']:12} {s.get('fp_mode', '-'):6} "
                    + (
                        f"cold {s['cold'] / 1e6:7.1f} warm {s['warm'] / 1e6:7.1f} Minsn/s ok {s['ok']}"
                        if "warm" in s
                        else "failed"
                    ),
                    flush=True,
                )
    # Re-runs: cells whose cold or warm spread over the rounds exceeds max_spread.
    for rr in range(args.reruns):
        bad_h, bad_w = set(), set()
        hs = {}
        for rd in data["rounds"]:
            for s in rd["harness"]:
                hs.setdefault((s["program"], s["engine"], s.get("fp_mode")), []).append(s)
        for key, rows in hs.items():
            for metric in ("cold", "warm", "ns"):
                vals = [x[metric] for x in rows if metric in x]
                if len(vals) > 1 and common.spread(vals)[3] > args.max_spread:
                    bad_h.add(key)
        for (pl, job), c in summarize_wrap(data["rounds"]).items():
            for metric in ("cold", "warm"):
                if common.spread(c[metric])[3] > args.max_spread:
                    bad_w.add(pl)
        if not bad_h and not bad_w:
            break
        print(f"bench_engines: re-run {sorted(bad_h)} {sorted(bad_w)}", flush=True)
        for rnd in range(args.rounds):
            rd = run_round(
                args.suite,
                rnd,
                cpu,
                harness_cells=bad_h,
                wrap_cells=bad_w,
                segment40=not args.no_segment40,
                fp_modes=fp_modes,
            )
            rd["rerun"] = rr + 1
            data["rerun_rounds"].append(rd)
            common.write_json(args.out, data)
    common.write_json(args.out, data)
    print(f"bench_engines: wrote {args.out}")


if __name__ == "__main__":
    main()
