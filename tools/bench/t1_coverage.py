# SPDX-License-Identifier: GPL-2.0-or-later
"""Where Tier1's coverage goes on the compiled-C suite.

For each kernel: one harness process runs the job's variant 0 (about 4M
instructions, cold: regions compiled by hotness) and variant 1 (about 20M,
the measured one) under Tier1 with production regions and
`bench-jobdump --profile`, which observes every lower-tier step and every
compile attempt (tests/bench/jobdump.c). Prints, for variant 1:

- coverage bounds as shares of all retired instructions: Tier1 today;
  plus the lower-tier instructions already in the v1 subset (what region
  formation alone could reach: every subset instruction inside a region);
  plus call/return (br.call, br.ret, alloc, mov to/from BR); plus AR moves
  (mov to/from AR: ar.lc, ar.pfs, ...); plus addp4; plus integer multiply
  through the FP unit (setf.sig, getf.sig, xma, xmpy); the rest;
- the runs of consecutive lower-tier steps between region executions;
- the compiled regions: count, distinct headers, contracts per header,
  instructions per region, loops, the instruction that ended formation.

Not timed. Jobs: build/bench/jobs/csuite-K-prof.bin (made here with
bench-jobdump --dump when missing: variants of about 4M and 20M
instructions).

    python3 -I tools/bench/t1_coverage.py [--kernels 0,1,2,3,4,5] [--raw DIR]
"""

import argparse
import collections
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

NAMES = ["crc32", "qsort", "matmul", "strsearch", "hash", "lz"]
JOBDUMP = common.REPO / "build/bench-jobdump"
GROUPS = [
    (
        "call/return",
        lambda op: op in ("BR_CALL", "BR_CALL_INDIRECT", "BR_RET", "ALLOC", "MOV_BRGR", "MOV_GRBR"),
    ),
    ("AR moves", lambda op: op in ("MOV_GRAR", "MOV_ARGR", "MOV_IMMAR")),
    ("addp4", lambda op: op == "ADDP4"),
    ("FP-unit int multiply", lambda op: op.startswith(("SETF_SIG", "GETF_SIG", "XMA", "XMPY"))),
]


def images():
    found = sorted(str(p) for p in (common.REPO / "build/tests/tier1").glob("*.bin"))
    found.append(str(common.REPO / "build/tests/ia64/speed.bin"))
    found.append(str(common.REPO / "build/bench/csuite/csuite.bin"))
    return found


def ensure_job(k):
    job = common.REPO / f"build/bench/jobs/csuite-{k}-prof.bin"
    if not job.exists():
        job.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run(
            [str(JOBDUMP), f"--dump=csuite:{k}", "--insns=4000000,20000000", f"--out={job}"]
            + images(),
            cwd=common.REPO,
            capture_output=True,
            text=True,
        )
        if r.returncode:
            raise SystemExit(f"t1_coverage: dump csuite:{k} failed:\n{r.stderr}")
    return job


def profile(k, raw):
    job = ensure_job(k)
    r = subprocess.run(
        [str(JOBDUMP), "--run=tier1", f"--job={job}", "--variants=0,1", "--profile"] + images(),
        cwd=common.REPO,
        capture_output=True,
        text=True,
    )
    if raw:
        (Path(raw) / f"prof-{k}.log").write_text(r.stdout + r.stderr)
    if r.returncode:
        raise SystemExit(f"t1_coverage: csuite:{k} failed ({r.returncode}):\n{r.stderr}")
    out = {"ops": {}, "runs": [], "regions": [], "rejects": collections.Counter()}
    for line in r.stdout.splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == "jobrun" and f[4] == "1":
            out["ok"] = f[10] == "1"
        if f[0] == "profile" and f[3] == "1":
            out.update({key: int(val) for key, val in zip(f[4::2], f[5::2])})
        elif f[0] == "profile-op" and f[3] == "1":
            out["ops"][f[4]] = (int(f[5]), f[7] == "1")
        elif f[0] == "profile-run" and f[3] == "1":
            out["runs"].append((f[5], int(f[7]), int(f[9])))
        elif f[0] == "profile-reject":
            out["rejects"][" ".join(f[6:])] += int(f[5])
        elif f[0] == "profile-region" and f[3] == "1":
            e = f.index("end")
            d = dict(zip(f[4:e:2], f[5:e:2]))
            d["end"] = " ".join(f[e + 1 : -2])
            d["at"] = f[-1]
            out["regions"].append(d)
    return out


def pct(n, d):
    return f"{100.0 * n / d:.1f} %" if d else "-"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--kernels", default="0,1,2,3,4,5")
    ap.add_argument("--raw", help="directory for the raw bench-jobdump output")
    a = ap.parse_args()
    if not JOBDUMP.exists():
        raise SystemExit(
            "t1_coverage: build/bench-jobdump missing (make build/bench-jobdump csuite)"
        )
    kernels = [int(k) for k in a.kernels.split(",")]
    load = common.loadavg()
    data = {k: profile(k, a.raw) for k in kernels}
    print(
        f"load average before: {load[0]:.2f} {load[1]:.2f} {load[2]:.2f}; after: "
        + " ".join(f"{x:.2f}" for x in common.loadavg())
    )
    print(
        "\nCoverage bounds (variant 1, shares of all retired instructions; cumulative left to right):\n"
    )
    head = (
        ["kernel", "retired", "Tier1 today", "+ subset in lower tier (formation)"]
        + [f"+ {g}" for g, _ in GROUPS]
        + ["rest", "checks"]
    )
    print("| " + " | ".join(head) + " |")
    print("|" + " --- |" * len(head))
    for k, d in data.items():
        total, t1 = d["retired"], d["tier1"]
        row = [NAMES[k], f"{total:,}", pct(t1, total)]
        acc = t1 + d["subset_lower"]
        row.append(pct(acc, total))
        listed = 0
        for _, pred in GROUPS:
            n = sum(c for op, (c, sub) in d["ops"].items() if not sub and pred(op))
            listed += n
            acc += n
            row.append(pct(acc, total))
        rest = d["lower_steps"] - d["subset_lower"] - listed
        row.append(pct(rest, total))
        row.append(
            "ok"
            if d.get("ok") and d["tier1"] + d["lower"] == total == t1 + d["lower_steps"]
            else "MISMATCH"
        )
        print("| " + " | ".join(row) + " |")
    print("\nNon-subset opcodes in the lower tier (variant 1, >= 0.1 % of its steps):\n")
    for k, d in data.items():
        ops = sorted(((c, op) for op, (c, sub) in d["ops"].items() if not sub), reverse=True)
        print(f"- {NAMES[k]}: " + ", ".join(f"{op} {c:,}" for c, op in ops))
    print("\nRuns of consecutive lower-tier steps between region executions (variant 1):\n")
    print("| kernel | entries | lower steps | runs | mean run | steps in runs >= 64 |")
    print("| --- | ---: | ---: | ---: | ---: | ---: |")
    for k, d in data.items():
        runs = sum(r for _, r, _ in d["runs"])
        long_steps = sum(i for b, _, i in d["runs"] if int(b.split("-")[0]) >= 64)
        print(
            f"| {NAMES[k]} | {d['entries']:,} | {d['lower_steps']:,} | {runs:,} | "
            f"{d['lower_steps'] / max(1, runs):.1f} | {pct(long_steps, d['lower_steps'])} |"
        )
    print("\nRegions entered in variant 1 (compiled by hotness in either variant):\n")
    print(
        "| kernel | regions | distinct headers | max contracts per header | insns per region (median, max) "
        "| loops | Tier1 insns per entry | formation ended at (regions) | rejected compiles (whole run) |"
    )
    print("| --- | ---: | ---: | ---: | --- | ---: | ---: | --- | --- |")
    for k, d in data.items():
        regs = d["regions"]
        heads = collections.Counter(r["ip"] for r in regs)
        sizes = sorted(int(r["insns"]) for r in regs)
        loops = sum(r["end"] == "back-edge" for r in regs)
        ends = collections.Counter(r["at"] for r in regs if r["end"] != "back-edge")
        print(
            f"| {NAMES[k]} | {len(regs)} | {len(heads)} | {max(heads.values()) if heads else 0} | "
            f"{sizes[len(sizes) // 2] if sizes else 0}, {sizes[-1] if sizes else 0} | {loops} | "
            f"{d['tier1'] / max(1, d['entries']):.1f} | "
            + ", ".join(f"{op} {n}" for op, n in ends.most_common())
            + " | "
            + ", ".join(f"{n} {why}" for why, n in d["rejects"].most_common())
            + " |"
        )


if __name__ == "__main__":
    main()
