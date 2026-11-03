#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""FP execution modes against the soft-float on the fpsuite kernels.

For each kernel, one cold run of an fpsuite job (variant 0 of the slow job,
bench_engines.py prepare --suite fpsuite) on the interpreter per mode of
bench-jobdump --fp: soft (the reference), exact and approx. The result words
of the last iteration (fpsuite_out, fpsuite_out_count in the ELF, dumped
with --dump-mem) and the final ar.fpsr and r8 of each run are compared:

  exact vs soft   outputs, r8, retired instructions and ar.fpsr must be
                  bit-identical
  approx vs soft  per value: differing values, ULP distance (max, mean over
                  all values; doubles for kernels 0-4, floats for 5 and 6,
                  64-bit-significand long doubles for 7),
                  max relative error, NaN/Inf class mismatches; final
                  ar.fpsr

and from the exact run's fpstats line the share of candidate scalar and
parallel operations executed on the host, their outcome breakdown and the
other FP arithmetic.

Usage: fp_error.py [--variant N] [--kernels 0,1,...] [--out FILE.json]
"""

import argparse
from fractions import Fraction
import math
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_engines  # noqa: E402
import common  # noqa: E402

SINGLE = {5, 6}
EXTENDED = {7}
OUT_DIR = common.REPO / "build/bench/fp-error"
ELF = common.REPO / "build/bench/fpsuite/fpsuite.elf"
MODES = ["soft", "exact", "approx"]


def symbols():
    nm = common.load_env("CROSS_IA64")["CROSS_IA64"] + "nm"
    r = subprocess.run([nm, str(ELF)], capture_output=True, text=True, check=True)
    sym = {}
    for line in r.stdout.splitlines():
        f = line.split()
        if len(f) == 3:
            sym[f[2]] = int(f[0], 16)
    return sym["fpsuite_out"], sym["fpsuite_out_count"]


def run_mode(kernel, mode, variant, out_addr, count_addr, words):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out_file = OUT_DIR / f"k{kernel}-{mode}.bin"
    count_file = OUT_DIR / f"k{kernel}-{mode}-count.bin"
    cmd = [
        str(bench_engines.JOBDUMP),
        "--run=interp",
        f"--job={bench_engines.job_file(f'fpsuite:{kernel}', 'slow')}",
        f"--variants={variant}",
        f"--fp={mode}",
        f"--dump-mem={out_addr:#x}:{words}:{out_file}",
        f"--dump-mem={count_addr:#x}:1:{count_file}",
    ]
    if mode == "exact":
        cmd.append("--fp-stats")
    # approx runs exit 1 when the soft-float's checks differ.
    r = subprocess.run(
        cmd + bench_engines.images(),
        cwd=common.REPO,
        capture_output=True,
        text=True,
        env=bench_engines.harness_bench.bench_env(),
    )
    res = {"mode": mode, "rc": r.returncode, "stats": None}
    for line in r.stdout.splitlines():
        f = line.split()
        if f and f[0] == "jobrun":
            res.update(
                retired=int(f[6]),
                ok=int(f[10]),
                fpsr=int(f[f.index("fpsr") + 1], 0),
                r8=int(f[f.index("r8") + 1], 0),
            )
        elif f and f[0] == "fpstats":
            res["stats"] = {k: int(v) for k, v in zip(f[3::2], f[4::2])}
    if "r8" not in res or not out_file.exists():
        raise SystemExit(f"fp_error: kernel {kernel} {mode}: no result\n{r.stdout}{r.stderr}")
    count = struct.unpack("<Q", count_file.read_bytes())[0]
    data = out_file.read_bytes()
    res["words"] = list(struct.unpack(f"<{count}Q", data[: 8 * count]))
    return res


def ordered(bits, width):
    """Unsigned key that increases with the value, so key distance is ULP distance."""
    sign = 1 << (width - 1)
    return (~bits) & (2 * sign - 1) if bits & sign else bits | sign


def decode(bits, width):
    if width == 32:
        return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]
    return struct.unpack("<d", struct.pack("<Q", bits))[0]


def compare(ref, test, single):
    width = 32 if single else 64
    mask = (1 << width) - 1
    ulps, rel, differ, class_mismatch = [], 0.0, 0, 0
    for a, b in zip(ref, test):
        a &= mask
        b &= mask
        x, y = decode(a, width), decode(b, width)
        if a != b:
            differ += 1
        if math.isfinite(x) != math.isfinite(y) or math.isnan(x) != math.isnan(y):
            class_mismatch += 1
            continue
        if not (math.isfinite(x) and math.isfinite(y)):
            continue
        ulps.append(0 if x == y else abs(ordered(a, width) - ordered(b, width)))
        if x != y:
            rel = max(rel, abs(y - x) / abs(x) if x else math.inf)
    return {
        "words": len(ref),
        "differ": differ,
        "max_ulp": max(ulps, default=0),
        "mean_ulp": sum(ulps) / len(ulps) if ulps else 0.0,
        "max_rel": rel,
        "nan_inf_mismatch": class_mismatch,
    }


def extended(sig, se):
    """The exact value of a double-extended (significand, sign/exponent) pair,
    None for an infinity or NaN; the ULP at 64-bit precision."""
    e = se & 0x7FFF
    if e == 0x7FFF:
        return None, None
    scale = Fraction(2) ** ((e if e else 1) - 16383 - 63)
    return (-1 if se & 0x8000 else 1) * sig * scale, scale


def compare_extended(ref, test):
    ulps, rel, differ, class_mismatch = [], 0.0, 0, 0
    for k in range(0, min(len(ref), len(test)) - 1, 2):
        (x, ulp), (y, _) = extended(ref[k], ref[k + 1]), extended(test[k], test[k + 1])
        if (ref[k], ref[k + 1]) != (test[k], test[k + 1]):
            differ += 1
        if (x is None) != (y is None):
            class_mismatch += 1
            continue
        if x is None:
            continue
        ulps.append(float(abs(y - x) / ulp))
        if x != y:
            rel = max(rel, float(abs(y - x) / abs(x)) if x else math.inf)
    return {
        "words": len(ref) // 2,
        "differ": differ,
        "max_ulp": max(ulps, default=0),
        "mean_ulp": sum(ulps) / len(ulps) if ulps else 0.0,
        "max_rel": rel,
        "nan_inf_mismatch": class_mismatch,
    }


def host_share(stats):
    out = {}
    for cls in ("scalar", "parallel"):
        reasons = {k[len(cls) + 1 :]: v for k, v in stats.items() if k.startswith(cls + "_")}
        total = sum(reasons.values())
        out[cls] = {
            "candidates": total,
            "host": reasons.get("host", 0),
            "host_share": reasons.get("host", 0) / total if total else None,
            "reasons": {k: v for k, v in reasons.items() if v},
        }
    out["other"] = stats.get("other", 0)
    return out


def kernel_result(kernel, variant, out_addr, count_addr):
    runs = {m: run_mode(kernel, m, variant, out_addr, count_addr, 1024) for m in MODES}
    single = kernel in SINGLE
    soft, exact, approx = runs["soft"], runs["exact"], runs["approx"]
    errors = (
        compare_extended(soft["words"], approx["words"])
        if kernel in EXTENDED
        else compare(soft["words"], approx["words"], single)
    )
    return {
        "kernel": kernel,
        "name": bench_engines.FPSUITE_NAMES[kernel],
        "format": "f80" if kernel in EXTENDED else "f32" if single else "f64",
        "words": len(soft["words"]),
        "retired": soft["retired"],
        "fpsr_soft": soft["fpsr"],
        "r8_soft": soft["r8"],
        "exact_equals_soft": {
            "outputs": exact["words"] == soft["words"],
            "r8": exact["r8"] == soft["r8"],
            "fpsr": exact["fpsr"] == soft["fpsr"],
            "retired": exact["retired"] == soft["retired"],
        },
        "approx_vs_soft": dict(
            errors,
            r8_equal=approx["r8"] == soft["r8"],
            fpsr_approx=approx["fpsr"],
            fpsr_equal=approx["fpsr"] == soft["fpsr"],
            retired_equal=approx["retired"] == soft["retired"],
        ),
        "host": host_share(exact["stats"]) if exact["stats"] else None,
    }


def print_tables(results):
    print("exact vs soft (bit-identical: outputs, r8, retired, ar.fpsr):")
    for r in results:
        e = r["exact_equals_soft"]
        print(
            f"  {r['kernel']} {r['name']:9} {'identical' if all(e.values()) else 'MISMATCH ' + str(e)}"
        )
    print("\napprox vs soft:")
    print(
        f"  {'kernel':11} {'words':>5} {'differ':>6} {'max ulp':>8} {'mean ulp':>9} {'max rel':>9} "
        f"{'nan/inf':>7} {'r8':>5} {'fpsr':>5}"
    )
    for r in results:
        a = r["approx_vs_soft"]
        print(
            f"  {r['kernel']} {r['name']:9} {a['words']:5} {a['differ']:6} {a['max_ulp']:8.6g} {a['mean_ulp']:9.4f} "
            f"{a['max_rel']:9.2e} {a['nan_inf_mismatch']:7} {'same' if a['r8_equal'] else 'diff':>5} "
            f"{'same' if a['fpsr_equal'] else 'diff':>5}"
        )
    print(
        "\nhost execution in an exact interp run (candidate operations; 'other' = remaining FP arithmetic):"
    )
    for r in results:
        h = r["host"]
        if not h:
            continue
        for cls in ("scalar", "parallel"):
            c = h[cls]
            share = f"{c['host_share'] * 100:6.2f}%" if c["host_share"] is not None else "   n/a "
            reasons = " ".join(f"{k}={v}" for k, v in c["reasons"].items() if k != "host")
            print(
                f"  {r['kernel']} {r['name']:9} {cls:8} candidates {c['candidates']:9} host {share}  {reasons}"
            )
        print(f"  {r['kernel']} {r['name']:9} other    {h['other']:9}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--variant", type=int, default=0, help="variant of the slow job (0: about 4M instructions)"
    )
    ap.add_argument("--kernels", default="0,1,2,3,4,5,6,7")
    ap.add_argument("--out", help="JSON result")
    args = ap.parse_args()
    out_addr, count_addr = symbols()
    results = [
        kernel_result(int(k), args.variant, out_addr, count_addr) for k in args.kernels.split(",")
    ]
    print_tables(results)
    if args.out:
        common.write_json(
            args.out,
            {
                "engine": "interp",
                "variant": args.variant,
                "repo": common.git_describe(common.REPO),
                "kernels": results,
            },
        )
    sys.exit(0 if all(all(r["exact_equals_soft"].values()) for r in results) else 1)


if __name__ == "__main__":
    main()
