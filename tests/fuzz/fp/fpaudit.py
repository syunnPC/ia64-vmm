#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Differential FP audit: hosted harness (real decoder + ia64_execute) versus
the independent exact model (fpmodel.py/fpexec.py).

  python3 tests/fuzz/fp/fpaudit.py --harness build/fpaudit-harness [--count N]

Divergences are grouped by signature; the simplest cases of each group are
saved to --cases-dir/<signature>.txt as replayable harness input lines with
the expected/actual fields in comments (build/fpaudit-harness < file replays
them). Groups explained by an SDM ambiguity knob (fpmodel.Knobs) are labelled
as such; groups reproduced by a confirmed-bug emulation are labelled BUGn.
Exit status is 1 when any divergence is not explained by an ambiguity knob."""

import argparse
import collections
import multiprocessing
import os
import random
import subprocess
import sys
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fpmodel import Knobs, AMBIGUITY  # noqa: E402
from fpexec import execute, to_line, snapshot  # noqa: E402
import fpgen  # noqa: E402

FIELDS = (
    "kind",
    "fault",
    "info",
    "exit",
    "fpsr",
    "psr",
    "pr",
    "f6",
    "f7",
    "f8",
    "f9",
    "f32",
    "r8",
    "n8",
    "r20",
    "n20",
    "m0",
    "m1",
)
HEX = ("info", "fpsr", "psr", "pr", "r8", "r20", "m0", "m1")
EXIT_UNIMPLEMENTED = 3


def parse(line):
    d = {}
    for tok in line.split():
        k, _, v = tok.partition("=")
        d[k] = v
    for k in HEX:
        d[k] = int(d.get(k, "0"), 16)
    for k in ("exit", "n8", "n20"):
        d[k] = int(d.get(k, "0"))
    return d


def norm_expected(o):
    return {k: o[k] for k in FIELDS}


def diff(exp, act, skip):
    if "*" in skip:
        return []
    return [k for k in FIELDS if k not in skip and exp[k] != act[k]]


def describe(insn):
    s = insn["op"]
    for k in ("form", "rel", "pc"):
        if insn.get(k):
            s += "." + str(insn[k])
    if insn["op"] in ("fma", "fms", "fnma", "fpma", "fpms", "fpnma"):
        if insn["f2"] == 0 and insn["f4"] == 1:
            s += "[fnorm]"
        elif insn["f2"] == 0:
            s += "[mpy]"
        elif insn["f4"] == 1:
            s += "[add]"
    if insn.get("qp"):
        s += "[qp]"
    return s


ALT_KNOBS = [
    ("u_trap_sets_i", Knobs(u_trap_sets_i=True)),
    ("fms_nan_flip", Knobs(fms_nan_flip=True)),
    ("fxu_neg_round_to_zero_valid", Knobs(fxu_neg_round_to_zero_valid=False)),
    ("unsupported_unordered", Knobs(unsupported_unordered=False)),
    ("parallel-wrap-exponent", Knobs(bug_parallel_wrap_regbits=True)),
]


def worker(args):
    harness, family, seed, count = args[:4]
    base = Knobs(**args[4]) if len(args) > 4 else Knobs()
    rng = random.Random(seed)
    cases = fpgen.FAMILIES[family](rng, count)
    lines = [to_line(i, s) for i, s in cases]
    proc = subprocess.run(
        [harness], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True
    )
    outs = proc.stdout.splitlines()
    assert len(outs) == len(cases), (len(outs), len(cases), proc.stderr[-500:])
    stats = collections.Counter()
    notes = collections.Counter()
    bad = []
    for (insn, st), line, out in zip(cases, lines, outs):
        act = parse(out)
        res = execute(insn, st, base)
        exp = norm_expected(res.output(st))
        stats["cases"] += 1
        stats["op:" + describe(insn)] += 1
        for n in res.notes:
            notes[n] += 1
        if (
            "fprcpa-recip-out-of-single-range" in res.notes
            and act["kind"] == "complete"
            and (act["pr"] >> insn["p2"]) & 1
        ):
            notes["obs:fprcpa-p2=1-with-out-of-range-reciprocal"] += 1
        if act["kind"] == "fault" and act["fault"] == "fp":
            if act["info"] in (8, 128, 136) and res.swa_ok:
                # An SDM Vol.2 FP software-assist fault leaves every effect
                # undone (FR, PR, FPSR, PSR.mfl/mfh, GRs, memory).
                untouched = norm_expected(snapshot(st))
                d = [
                    k
                    for k in FIELDS
                    if k not in ("kind", "fault", "info", "exit") and untouched[k] != act[k]
                ]
                if not d:
                    stats["swa-fault"] += 1
                    stats["swa-fault:" + insn["op"]] += 1
                    continue
                bad.append(
                    (
                        "%s|swa-fault-leaked:%s|" % (describe(insn), ",".join(d)),
                        None,
                        insn,
                        st,
                        line,
                        untouched,
                        act,
                    )
                )
                continue
        skip = set(res.unspecified)
        if exp["fault"] in ("illegal", "nat"):
            skip.add("info")  # Illegal Operation ISR detail is not modelled
        d = diff(exp, act, skip)
        if not d:
            stats["match"] += 1
            stats["kind:%s/%s" % (exp["kind"], exp["fault"])] += 1
            continue
        explained = None
        for name, kn in ALT_KNOBS:
            alt = norm_expected(execute(insn, st, kn).output(st))
            if not diff(alt, act, skip):
                explained = name
                break
        sig = "%s|%s|%s" % (describe(insn), ",".join(d), explained or "")
        bad.append((sig, explained, insn, st, line, exp, act))
    return stats, notes, bad


def complexity(insn, st):
    c = bin(st["fpsr"]).count("1")
    for v in st["fr"].values():
        c += bin(v.sig).count("1") + bin(v.exp).count("1")
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--harness", default="build/fpaudit-harness")
    ap.add_argument("--count", type=int, default=200000, help="cases per family")
    ap.add_argument("--seed", type=int, default=20261008)
    ap.add_argument("--families", default=",".join(fpgen.FAMILIES))
    ap.add_argument("--chunk", type=int, default=5000)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument(
        "--cases-dir",
        default="build/fpaudit-cases",
        help="where divergence groups are dumped (curated minimized cases live "
        "in tests/fuzz/fp/cases/)",
    )
    ap.add_argument("--show", type=int, default=2, help="examples printed per signature")
    ap.add_argument(
        "--knob",
        action="append",
        default=[],
        help="replay an alternative model hypothesis instead of the specification default, "
        "e.g. --knob u_trap_sets_i",
    )
    a = ap.parse_args()
    knob_over = {}
    for k in a.knob:
        knob_over[k] = not getattr(Knobs(), k)
    jobs = []
    for fam in a.families.split(","):
        n = a.count if fam not in ("status", "misc", "bits") else max(a.count // 4, 1000)
        for k in range(0, n, a.chunk):
            seed = zlib.crc32(("%d/%s/%d" % (a.seed, fam, k)).encode())  # reproducible
            jobs.append((a.harness, fam, seed, min(a.chunk, n - k), knob_over))
    stats = collections.Counter()
    notes = collections.Counter()
    groups = collections.defaultdict(list)
    # Python 3.14 defaults to a forkserver Unix socket. These Linux-hosted
    # pure workers need only fork/pipes, also usable in restricted sandboxes.
    with multiprocessing.get_context("fork").Pool(a.jobs) as pool:
        for st, nt, bad in pool.imap_unordered(worker, jobs):
            stats.update(st)
            notes.update(nt)
            for b in bad:
                groups[b[0]].append(b)
    print(
        "cases %d  match %d  swa-fault %d  divergent %d"
        % (stats["cases"], stats["match"], stats["swa-fault"], sum(len(v) for v in groups.values()))
    )
    print(
        "-- outcome kinds:",
        ", ".join("%s=%d" % (k[5:], v) for k, v in sorted(stats.items()) if k.startswith("kind:")),
    )
    print(
        "-- ops:",
        ", ".join("%s=%d" % (k[3:], v) for k, v in sorted(stats.items()) if k.startswith("op:")),
    )
    print(
        "-- swa faults:",
        ", ".join(
            "%s=%d" % (k[9:], v) for k, v in sorted(stats.items()) if k.startswith("swa-fault:")
        ),
    )
    print("-- divergence signatures:")
    for sig, items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print("   %6d  %s" % (len(items), sig))
    os.makedirs(a.cases_dir, exist_ok=True)
    for sig, items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        items.sort(key=lambda b: complexity(b[2], b[3]))
        expl = items[0][1]
        print(
            "\n## %s  (%d cases)%s"
            % (sig, len(items), "  [explained by: %s]" % AMBIGUITY.get(expl, expl) if expl else "")
        )
        fname = "".join(c if c.isalnum() or c in ".-_" else "_" for c in sig)[:120] + ".txt"
        with open(os.path.join(a.cases_dir, fname), "w") as f:
            for _, _, insn, st, line, exp, act in items[:5]:
                f.write("# insn %r\n" % (insn,))
                f.write(
                    "# expected "
                    + " ".join("%s=%s" % (k, hex(exp[k]) if k in HEX else exp[k]) for k in FIELDS)
                    + "\n"
                )
                f.write(
                    "# actual   "
                    + " ".join("%s=%s" % (k, hex(act[k]) if k in HEX else act[k]) for k in FIELDS)
                    + "\n"
                )
                f.write(line + "\n")
        for _, _, insn, st, line, exp, act in items[: a.show]:
            print("  insn", insn)
            print("  in  ", line)
            keys = [k for k in FIELDS if exp[k] != act[k]]
            print(
                "  exp ", " ".join("%s=%s" % (k, hex(exp[k]) if k in HEX else exp[k]) for k in keys)
            )
            print(
                "  act ", " ".join("%s=%s" % (k, hex(act[k]) if k in HEX else act[k]) for k in keys)
            )
    print("\n-- coverage notes:")
    for k, v in sorted(notes.items()):
        print("   %8d  %s" % (v, k))
    unexplained = sum(len(v) for k, v in groups.items() if not v[0][1] or v[0][1].startswith("BUG"))
    return 1 if unexplained else 0


if __name__ == "__main__":
    sys.exit(main())
