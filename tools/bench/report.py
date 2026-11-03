#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Markdown tables from bench_engines.py results.

Per (workload, engine): cold and warm guest Minsn/s as min / median / max
over samples, spread (max - min) / median, load average range, and the
ratios of medians. Samples:
  harness  one process per sample; cold = first 4M instructions (4M
           segment run), warm = the next segment (40M segment run for
           gather/shladd_xor; speed's image allows only 4M); csuite: one whole run
  wrapper  one sample per (process, job): cold = the job's first run in
           the process (S), warm = median of its long runs (A in P1, B in
           P2); ITC ticks at the frequency the platform reports
Re-run rounds replace the original samples of the cells they re-ran;
both are in the JSON.

Usage: report.py RESULT.json [--md OUT.md]
"""

import argparse
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

ENGINE_ORDER = [
    "interp",
    "tier0",
    "tier1",
    "tier0+tier1",
    "tcg",
    "hosted-interp",
    "hosted-tier0",
    "hosted-tier1",
]
LABEL = {
    "interp": "interp (harness)",
    "tier0": "Tier0 (harness)",
    "tier1": "Tier1 v1 (harness)",
    "tier0+tier1": "Tier0+Tier1 (harness, product path)",
    "tcg": "QEMU TCG (wrapper)",
    "hosted-interp": "interp (vmm-hosted wrapper)",
    "hosted-tier0": "Tier0 (vmm-hosted wrapper)",
    "hosted-tier1": "Tier0+Tier1 (vmm-hosted wrapper)",
}
CSUITE_NAMES = ["crc32", "qsort", "matmul", "strsearch", "hash", "lz"]
FPSUITE_NAMES = [
    "fp-matmul",
    "fp-fft",
    "fp-nbody",
    "fp-trans",
    "fp-reduce",
    "fp-matmul-f",
    "fp-parallel",
    "fp-extended",
]


def job_name(program):
    if program.startswith("csuite:"):
        return CSUITE_NAMES[int(program.split(":")[1])]
    if program.startswith("fpsuite:"):
        return FPSUITE_NAMES[int(program.split(":")[1])]
    return program


def harness_workload(s):
    """The workload of a harness sample: the job, per FP mode when the
    sample has one (bench_engines.py run --fp-modes)."""
    return job_name(s["program"]) + (f" [{s['fp_mode']}]" if s.get("fp_mode") else "")


def rounds_for(data):
    """All rounds: the first set and the re-run set (re-run rounds hold only
    the cells that exceeded the spread limit); every sample counts."""
    rr = data.get("rerun_rounds", [])
    rerun_h = {(harness_workload(s), s["engine"]) for r in rr for s in r["harness"]}
    rerun_w = {w["platform"] for r in rr for w in r["wrap"]}
    out = []
    for i, r in enumerate(data["rounds"]):
        out.append(
            {
                "key": f"r{i}",
                "harness": r["harness"],
                "harness40": r.get("harness40", []),
                "wrap": r["wrap"],
                "t0": r.get("t0"),
                "t1": r.get("t1"),
            }
        )
    for i, r in enumerate(rr):
        out.append(
            {
                "key": f"rerun{i}",
                "harness": r["harness"],
                "harness40": r.get("harness40", []),
                "wrap": r["wrap"],
                "t0": r.get("t0"),
                "t1": r.get("t1"),
            }
        )
    return out, rerun_h, rerun_w


def cells(data):
    """{(workload, engine): {"cold": [...], "warm": [...], "load": [...], "t1": [...]}}"""
    rounds, rerun_h, rerun_w = rounds_for(data)
    c = {}

    def cell(w, e):
        return c.setdefault(
            (w, e),
            {
                "cold": [],
                "warm": [],
                "load": [],
                "t1": [],
                "wall": [],
                "round": {},
                "runs": {},
                "lw": [],
            },
        )

    def run_time(x, insns, seconds):
        x["runs"].setdefault(insns, []).append(seconds)

    def per_round(x, key, metric, value):
        x["round"].setdefault(key, {}).setdefault(metric, []).append(value)

    for r in rounds:
        key = r["key"]
        for s in r["harness"]:
            x = cell(harness_workload(s), s["engine"])
            x["load"].append(s["loadavg_before"][0])
            if "runs" in s:  # csuite: S then A x 3, one process
                if "cold" in s:
                    x["cold"].append(s["cold"] / 1e6)
                    x["warm"].append(s["warm"] / 1e6)
                    per_round(x, key, "cold", s["cold"] / 1e6)
                    per_round(x, key, "warm", s["warm"] / 1e6)
                    x["lw"].append((s["loadavg_before"][0], s["warm"] / 1e6))
                    for run in s["runs"][1:]:
                        run_time(x, run["retired"], run["ns"] / 1e9)
                x["ok"] = x.get("ok", True) and s.get("ok") == 1
                if "t1" in s:
                    t1 = dict(s["t1"])
                    t1["entries"] = sum(r.get("entries", 0) for r in s["runs"][1:])
                    x["t1"].append(t1)
            elif "cold" in s:  # bench.c sample
                x["cold"].append(s["cold"] / 1e6)
                per_round(x, key, "cold", s["cold"] / 1e6)
                if s["program"] == "speed":
                    x["warm"].append(s["warm"] / 1e6)
                    per_round(x, key, "warm", s["warm"] / 1e6)
                    x["lw"].append((s["loadavg_before"][0], s["warm"] / 1e6))
                    run_time(x, s["segment"], s["segment"] / s["warm"])
                if "t1" in s and s["program"] == "speed":
                    x["t1"].append(s["t1"])
        for s in r["harness40"]:
            x = cell(job_name(s["program"]), s["engine"])
            x["warm"].append(s["warm"] / 1e6)
            per_round(x, key, "warm", s["warm"] / 1e6)
            x["lw"].append((s["loadavg_before"][0], s["warm"] / 1e6))
            run_time(x, s["segment"], s["segment"] / s["warm"])
            if "t1" in s:
                x["t1"].append(s["t1"])
        for w in r["wrap"]:
            if not w.get("freq"):
                continue
            hz = w["freq"]["hz"]
            byjob = {}
            for s in w["steps"]:
                byjob.setdefault(s["job"], []).append(s)
            for job, steps in byjob.items():
                x = cell(job_name(job), w["platform"])
                rate = lambda s: s["retired"] / (s["itc"] / hz) / 1e6
                x["cold"].append(rate(steps[0]))
                x["warm"].append(statistics.median(rate(s) for s in steps[1:]))
                per_round(x, key, "cold", rate(steps[0]))
                per_round(x, key, "warm", statistics.median(rate(s) for s in steps[1:]))
                x["lw"].append(
                    (w["loadavg_before"][0], statistics.median(rate(s) for s in steps[1:]))
                )
                for s in steps[1:]:
                    run_time(x, s["retired"], s["itc"] / hz)
                x["load"].append(w["loadavg_before"][0])
                x["ok"] = x.get("ok", True) and all(s["ok"] == 1 for s in steps)
                x.setdefault("faults", set()).add(tuple(steps[0].get("faults", [])))
    return c, rerun_h, rerun_w


def fmt3(v):
    if not v:
        return "n/m", ""
    lo, med, hi, sp = common.spread(v)
    return f"{lo:.1f} / **{med:.1f}** / {hi:.1f}", f"{sp * 100:.0f}%"


def wall_check(data):
    """Per platform: (I_P2 - I_P1) / (wall_P2 - wall_P1) vs the ITC rate of
    the same difference, per round."""
    rounds, _, _ = rounds_for(data)
    out = {}
    for r in rounds:
        byp = {}
        for w in r["wrap"]:
            byp.setdefault(w["platform"], {})[w["plan_name"]] = w
        for pl, plans in byp.items():
            if set(plans) != {"p1", "p2"} or not all(p["ok"] for p in plans.values()):
                continue
            hz = plans["p1"]["freq"]["hz"]
            ins = {k: sum(s["retired"] for s in p["steps"]) for k, p in plans.items()}
            itc = {k: sum(s["itc"] for s in p["steps"]) for k, p in plans.items()}
            di = ins["p2"] - ins["p1"]
            wall_rate = di / (plans["p2"]["wall"] - plans["p1"]["wall"]) / 1e6
            itc_rate = di / ((itc["p2"] - itc["p1"]) / hz) / 1e6
            out.setdefault(pl, []).append(
                (wall_rate, itc_rate, plans["p2"]["wall"] - plans["p1"]["wall"])
            )
    return out


def report(data, title):
    c, rerun_h, rerun_w = cells(data)
    workloads = []
    for w, _e in c:
        if w not in workloads:
            workloads.append(w)
    lines = [
        f"### {title}",
        "",
        "| workload | engine | cold Minsn/s min / **median** / max | spread | "
        "warm Minsn/s min / **median** / max | spread | n cold/warm | load avg | checks |",
        "| --- | --- | --- | ---: | --- | ---: | ---: | --- | --- |",
    ]
    for w in workloads:
        for e in ENGINE_ORDER:
            x = c.get((w, e))
            if not x:
                continue
            cold, csp = fmt3(x["cold"])
            warm, wsp = fmt3(x["warm"])
            load = f"{min(x['load']):.1f}-{max(x['load']):.1f}" if x["load"] else ""
            rerun = " ¹" if (w, e) in rerun_h or e in rerun_w else ""
            ok = {True: "pass", False: "FAIL"}.get(
                x.get("ok"),
                "self-check" if e in ("interp", "tier0", "tier1", "tier0+tier1") else "",
            )
            lines.append(
                f"| {w} | {LABEL[e]}{rerun} | {cold} | {csp} | {warm} | {wsp} | "
                f"{len(x['cold'])}/{len(x['warm'])} | {load} | {ok} |"
            )
    if rerun_h or rerun_w:
        lines += [
            "",
            "¹ The cell's spread exceeded 10 % in the first set of rounds and was re-run; the "
            "row counts both sets (all samples are in the JSON).",
        ]
    lines += [
        "",
        "Wall time of one warm run (instructions per run: median seconds [min-max]; harness: "
        "one segment, wrapper: one long run timed by the ITC):",
        "",
        "| workload | engine | runs |",
        "| --- | --- | --- |",
    ]
    for w in workloads:
        for e in ENGINE_ORDER:
            x = c.get((w, e))
            if not x or not x["runs"]:
                continue
            parts = []
            for insns, secs in sorted(x["runs"].items()):
                parts.append(
                    f"{insns / 1e6:.1f}M: {statistics.median(secs):.4g} s "
                    f"[{min(secs):.3g}-{max(secs):.3g}] (n={len(secs)})"
                )
            lines.append(f"| {w} | {LABEL[e]} | {'; '.join(parts)} |")
    lines += [
        "",
        "Ratios of medians (warm; cold in parentheses):",
        "",
        "| workload | Tier1/Tier0 | Tier0/TCG | Tier1/TCG | interp/TCG | hosted Tier0/TCG | hosted interp/TCG |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]

    def med(w, e, m):
        x = c.get((w, e))
        return statistics.median(x[m]) if x and x[m] else None

    def ratio(w, a, b):
        parts = []
        for m in ("warm", "cold"):
            ma, mb = med(w, a, m), med(w, b, m)
            parts.append(f"{ma / mb:.3g}" if ma and mb else "n/m")
        return f"{parts[0]} ({parts[1]})"

    for w in workloads:
        lines.append(
            f"| {w} | {ratio(w, 'tier1', 'tier0')} | {ratio(w, 'tier0', 'tcg')} | "
            f"{ratio(w, 'tier1', 'tcg')} | {ratio(w, 'interp', 'tcg')} | "
            f"{ratio(w, 'hosted-tier0', 'tcg')} | {ratio(w, 'hosted-interp', 'tcg')} |"
        )

    def paired(w, a, b, m):
        xa, xb = c.get((w, a)), c.get((w, b))
        if not xa or not xb:
            return None
        rs = []
        for k in set(xa["round"]) & set(xb["round"]):
            va, vb = xa["round"][k].get(m), xb["round"][k].get(m)
            if va and vb:
                rs.append(statistics.median(va) / statistics.median(vb))
        return rs

    def pfmt(rs):
        if not rs:
            return "n/m"
        return f"{statistics.median(rs):.3g} [{min(rs):.3g}-{max(rs):.3g}]"

    lines += [
        "",
        "Ratios paired by round (each round runs every engine once, interleaved; median "
        "[min-max] over rounds of the per-round ratio), warm:",
        "",
        "| workload | Tier1/Tier0 | Tier0/TCG | Tier1/TCG | interp/TCG | hosted Tier0/TCG | hosted interp/TCG |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for w in workloads:
        lines.append(
            f"| {w} | "
            + " | ".join(
                pfmt(paired(w, a, b, "warm"))
                for a, b in (
                    ("tier1", "tier0"),
                    ("tier0", "tcg"),
                    ("tier1", "tcg"),
                    ("interp", "tcg"),
                    ("hosted-tier0", "tcg"),
                    ("hosted-interp", "tcg"),
                )
            )
            + " |"
        )
    lines += [
        "",
        "Same, cold:",
        "",
        "| workload | Tier1/Tier0 | Tier0/TCG | Tier1/TCG | interp/TCG | hosted Tier0/TCG | hosted interp/TCG |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for w in workloads:
        lines.append(
            f"| {w} | "
            + " | ".join(
                pfmt(paired(w, a, b, "cold"))
                for a, b in (
                    ("tier1", "tier0"),
                    ("tier0", "tcg"),
                    ("tier1", "tcg"),
                    ("interp", "tcg"),
                    ("hosted-tier0", "tcg"),
                    ("hosted-interp", "tcg"),
                )
            )
            + " |"
        )
    # The product path against TCG (target >= 1.0 on gather-phys and gather-virt)
    # and against Tier0 and v1.
    product = (
        ("tier0+tier1", "tcg"),
        ("tier0+tier1", "tier0"),
        ("tier0+tier1", "tier1"),
        ("hosted-tier1", "tcg"),
        ("hosted-tier1", "hosted-tier0"),
        ("hosted-tier1", "tier0+tier1"),
    )
    if any(c.get((w, e)) for w in workloads for e in ("tier0+tier1", "hosted-tier1")):
        head = (
            "| workload | Tier0+Tier1/TCG | Tier0+Tier1/Tier0 | Tier0+Tier1/Tier1 v1 | hosted Tier1/TCG | "
            "hosted Tier1/hosted Tier0 | hosted Tier1/harness Tier0+Tier1 |"
        )
        for m in ("warm", "cold"):
            lines += [
                "",
                f"Product path (Tier0+Tier1), ratios paired by round, {m}:",
                "",
                head,
                "| --- | --- | --- | --- | --- | --- | --- |",
            ]
            for w in workloads:
                lines.append(
                    f"| {w} | " + " | ".join(pfmt(paired(w, a, b, m)) for a, b in product) + " |"
                )
    lines += [
        "",
        "Cross-check, paired by round: wrapper on vmm-hosted vs the harness (same engine, "
        "same work), warm (cold):",
        "",
        "| workload | hosted interp / harness interp | hosted Tier0 / harness Tier0 |",
        "| --- | --- | --- |",
    ]
    for w in workloads:
        lines.append(
            f"| {w} | {pfmt(paired(w, 'hosted-interp', 'interp', 'warm'))} "
            f"({pfmt(paired(w, 'hosted-interp', 'interp', 'cold'))}) | "
            f"{pfmt(paired(w, 'hosted-tier0', 'tier0', 'warm'))} "
            f"({pfmt(paired(w, 'hosted-tier0', 'tier0', 'cold'))}) |"
        )
    # Load per round: 1-minute load average before each sample of the round.
    rounds, _, _ = rounds_for(data)
    rl = []
    for r in rounds:
        loads = [x["loadavg_before"][0] for x in r["harness"] + r["harness40"] + r["wrap"]]
        if not loads:
            continue
        when = "not recorded"
        if r.get("t0"):
            import time as _t

            when = (
                _t.strftime("%H:%M", _t.localtime(r["t0"]))
                + "-"
                + _t.strftime("%H:%M", _t.localtime(r["t1"]))
            )
        rl.append(
            f"| {r['key']} | {when} | {len(loads)} | {min(loads):.1f} / "
            f"{statistics.median(loads):.1f} / {max(loads):.1f} |"
        )
    if rl:
        lines += [
            "",
            "Load per round (1-minute load average read before each sample; rN = first set, "
            "rerunN = re-run set):",
            "",
            "| round | time (JST) | samples | load min / median / max |",
            "| --- | --- | ---: | --- |",
        ] + rl
    # Load sensitivity: warm samples taken at low vs high 1-minute load average.
    lo_t, hi_t = 7.0, 10.0
    rows = []
    for w in workloads:
        for e in ENGINE_ORDER:
            x = c.get((w, e))
            if not x or not x["lw"]:
                continue
            lo = [v for l, v in x["lw"] if l < lo_t]
            hi = [v for l, v in x["lw"] if l >= hi_t]
            if lo and hi:
                rows.append(
                    f"| {w} | {LABEL[e]} | {statistics.median(lo):.1f} (n={len(lo)}) | "
                    f"{statistics.median(hi):.1f} (n={len(hi)}) | {statistics.median(lo) / statistics.median(hi):.2f} |"
                )
    if rows:
        lines += [
            "",
            f"Load sensitivity: median warm Minsn/s of samples taken at load average < {lo_t:g} "
            f"and ≥ {hi_t:g} (1-minute, before the sample):",
            "",
            "| workload | engine | load < 7 | load ≥ 10 | low/high |",
            "| --- | --- | ---: | ---: | ---: |",
        ] + rows
    # Tier1 coverage.
    t1rows = []
    for w in workloads:
        x = c.get((w, "tier1"))
        if not x or not x["t1"]:
            continue
        shares = [
            t["tier1_retired"] / (t["tier1_retired"] + t["lower_retired"])
            for t in x["t1"]
            if t["tier1_retired"] + t["lower_retired"]
        ]
        if "compile_us" in x["t1"][0]:
            extra = (
                f"{statistics.median(t['compile_us'] for t in x['t1']):.0f} | "
                f"{statistics.median(t['host_per_guest'] for t in x['t1']):.2f} | "
                f"{statistics.median(t['entries'] for t in x['t1']):.0f} | "
                f"{statistics.median(t['tier1_retired'] / t['entries'] for t in x['t1'] if t['entries']):.0f} | "
            )
        else:
            per_entry = [t["tier1_retired"] / t["entries"] for t in x["t1"] if t.get("entries")]
            extra = (
                f" | | {statistics.median(t['entries'] for t in x['t1']):.0f} | "
                f"{statistics.median(per_entry):.1f} | "
                f"{statistics.median(t['compiles'] for t in x['t1']):.0f} / "
                f"{statistics.median(t['rejects'] for t in x['t1']):.0f}"
            )
        t1rows.append(
            f"| {w} | {min(shares) * 100:.2f}% / {statistics.median(shares) * 100:.2f}% / "
            f"{max(shares) * 100:.2f}% | {extra} |"
        )
    if t1rows:
        lines += [
            "",
            "Tier1 (harness, production regions): share of retired instructions executed by "
            "Tier1 code, the rest by the harness's stepping lower tier (min / median / max; asm: the "
            "warm segment, C: the three warm runs). asm also: compile time of the loop region and its "
            "host instructions per guest instruction (bench.c's report); region entries in the warm "
            "segment / runs; C: Tier1 instructions per region entry, regions compiled / rejected "
            "(whole process, medians):",
            "",
            "| workload | Tier1 share | compile µs | host/guest | entries | insns/entry | compiled / rejected |",
            "| --- | --- | ---: | ---: | ---: | ---: | --- |",
        ] + t1rows
    wc = wall_check(data)
    if wc:
        lines += [
            "",
            "Wall-clock cross-check (wrapper): per round, (I_P2 − I_P1) / (wall_P2 − wall_P1) "
            "against the ITC rate of the same difference:",
            "",
            "| platform | wall-based Minsn/s (median) | ITC-based Minsn/s (median) | wall/ITC per round | Δwall s |",
            "| --- | ---: | ---: | --- | --- |",
        ]
        for pl, rows in sorted(wc.items()):
            lines.append(
                f"| {pl} | {statistics.median(r[0] for r in rows):.1f} | "
                f"{statistics.median(r[1] for r in rows):.1f} | "
                f"{', '.join(f'{r[0] / r[1]:.3f}' for r in rows)} | "
                f"{', '.join(f'{r[2]:.2f}' for r in rows)} |"
            )
    # Only gather-virt's bank-0 registers are IVT counters (speed runs in bank 0).
    faults = {
        (w, e): x.get("faults")
        for (w, e), x in c.items()
        if x.get("faults") and w.startswith("gather_virt")
    }
    if any(f and f != {tuple([0] * 4)} for f in faults.values()):
        lines += [
            "",
            "asm IVT counters of each wrapper job's first run (bank-0 r20 Data TLB, r21 Alternate "
            "Data TLB, r22 Dirty-Bit, r29 Page Not Present):",
            "",
        ]
        for (w, e), f in sorted(faults.items()):
            if f and f != {tuple([0] * 4)}:
                lines.append(
                    f"- {w} on {e}: " + "; ".join(",".join(map(str, t)) for t in sorted(f))
                )
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json")
    ap.add_argument("--md")
    ap.add_argument("--title", default="Results")
    args = ap.parse_args()
    data = json.loads(Path(args.json).read_text())
    md = report(data, args.title)
    if args.md:
        Path(args.md).write_text(md)
    print(md)


if __name__ == "__main__":
    main()
