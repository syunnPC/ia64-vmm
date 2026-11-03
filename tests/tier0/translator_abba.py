#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure complete baseline/current Tier0 translators and encoders in ABBA order.

All other production sources and headers must match. Source and object hashes,
each observation, and paired block summaries are retained in JSON output.
Native code and state addresses are shared; installation is outside timing.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument(
        "--case",
        choices=(
            "control",
            "branch-false",
            "same-constant",
            "same-identity",
            "same-forward",
            "cmp-reg",
            "cmp-imm",
            "cmp-wide",
            "cmp-same",
            "cmp-same-zero",
            "cmp-same-nat",
            "tbit",
            "tnat",
            "extensions",
            "extension-chain",
            "variable-shifts",
            "helper",
            "predicate-forward-first",
            "predicate-forward-second",
            "predicate-forward-both",
            "predicate-forward-immediate",
            "predicate-forward-tbit",
            "predicate-forward-tnat",
            "predicate-forward-nat",
            "forwarded-home-adds",
            "forwarded-home-first",
            "forwarded-home-second",
            "forwarded-home-both",
            "forwarded-home-extract",
            "forwarded-home-stacked-control",
        ),
        required=True,
    )
    parser.add_argument("--first-reg", type=int, choices=(8, 32, 48, 96), default=48)
    parser.add_argument("--mask", type=int, choices=range(16), default=0)
    parser.add_argument("--cpu", type=int, default=min(os.sched_getaffinity(0)))
    parser.add_argument("--blocks", type=int, default=9)
    parser.add_argument("--emit-count", type=int, default=3000)
    parser.add_argument("--run-count", type=int, default=1000000)
    parser.add_argument("--insns", type=int, choices=range(1, 49), default=48)
    parser.add_argument("--entry-offset", type=int, choices=(0, 16, 32, 48), default=0)
    parser.add_argument("--budget", type=int)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument(
        "--product",
        action="store_true",
        help="Compile both translator/encoder pairs with exact freestanding product flags; common support objects remain hosted.",
    )
    parser.add_argument(
        "--check-request-advance",
        action="store_true",
        help="Check request-advance private helper equivalence, including a vanished notification; use only with that optimization.",
    )
    args = parser.parse_args()
    if min(
        args.blocks, args.emit_count, args.run_count
    ) <= 0 or args.cpu not in os.sched_getaffinity(0):
        parser.error("counts must be positive and CPU must be in the affinity set")
    budget = args.insns if args.budget is None else args.budget
    if not 1 <= budget <= args.insns:
        parser.error("budget must be between one and the region length")
    root = Path(__file__).resolve().parents[2]
    baseline = args.baseline.resolve()
    varied = {"engine/tier0/translate.c", "engine/tier0/x86emit.c", "engine/tier0/x86emit.h"}
    shared = []
    for directory in ("core", "engine", "ia64", "include"):
        files = set(
            p.relative_to(root)
            for p in (root / directory).rglob("*")
            if p.suffix in (".c", ".h", ".inc", ".S")
        )
        files |= set(
            p.relative_to(baseline)
            for p in (baseline / directory).rglob("*")
            if p.suffix in (".c", ".h", ".inc", ".S")
        )
        for path in sorted(files):
            if str(path) in varied:
                continue
            a, b = root / path, baseline / path
            if not a.exists() or not b.exists() or a.read_bytes() != b.read_bytes():
                raise RuntimeError(f"shared production source differs: {path}")
            shared.append({"path": str(path), "sha256": sha(a)})
    flags = ["SAN=0", "TIER0_STATS=0", "TIER1_STATS=0"]
    if not args.no_build:
        for tree in (baseline, root):
            subprocess.run(
                ["make", "-j2", *flags, "build/tier0-test"],
                cwd=tree,
                check=True,
                stdout=subprocess.DEVNULL,
            )
    for tree in (baseline, root):
        stamp = (tree / "build/hosted/.flags").read_text()
        if (
            "IA64VMM_TIER0_STATS=0" not in stamp
            or "IA64VMM_TIER1_STATS=0" not in stamp
            or "-fsanitize" in stamp
        ):
            raise RuntimeError(f"unexpected hosted build flags in {tree}")
    if (baseline / "build/hosted/.flags").read_bytes() != (
        root / "build/hosted/.flags"
    ).read_bytes():
        raise RuntimeError("baseline and current hosted build flags differ")
    dry = subprocess.check_output(["make", "-Bn", *flags, "build/tier0-test"], cwd=root, text=True)
    commands = [shlex.split(line) for line in dry.replace("\\\n", " ").splitlines() if line.strip()]
    compile = next(c for c in commands if "-c" in c and "tests/tier0/main.c" in c)
    link = next(
        c for c in reversed(commands) if "-o" in c and c[c.index("-o") + 1] == "build/tier0-test"
    )
    cflags = [f for f in compile[1 : compile.index("-c")] if f not in ("-MMD", "-MP")]
    arguments = [
        arg
        for index, arg in enumerate(link[1:], 1)
        if arg != "-o"
        and link[index - 1] != "-o"
        and not arg.endswith("/tests/tier0/main.o")
        and not arg.startswith("-Wl,--wrap=")
    ]
    object_paths = [
        Path("build/hosted/engine/tier0") / name for name in ("translate.o", "x86emit.o")
    ]
    with tempfile.TemporaryDirectory(prefix="tier0-translator-abba-") as temp:
        temp = Path(temp)
        objects, provenance = {}, {}
        for name, tree in (("baseline", baseline), ("current", root)):
            variant_flags = (tree / "build/hosted/.flags").read_text()
            objects[name] = [tree / path for path in object_paths]
            if args.product:
                product_dry = subprocess.check_output(
                    [
                        "make",
                        "-Bn",
                        *flags,
                        "build/bm/engine/tier0/translate.o",
                        "build/bm/engine/tier0/x86emit.o",
                    ],
                    cwd=tree,
                    text=True,
                )
                product_commands = [
                    shlex.split(line)
                    for line in product_dry.replace("\\\n", " ").splitlines()
                    if line.strip()
                ]
                objects[name] = []
                recorded_flags = []
                for path in object_paths:
                    source = "engine/tier0/" + path.stem + ".c"
                    command = next(c for c in product_commands if "-c" in c and source in c)
                    target_flags = [
                        f for f in command[1 : command.index("-c")] if f not in ("-MMD", "-MP")
                    ]
                    obj = temp / (name + "-product-" + path.name)
                    subprocess.run(
                        [command[0], *target_flags, "-c", source, "-o", str(obj)],
                        cwd=tree,
                        check=True,
                    )
                    objects[name].append(obj)
                    recorded_flags.append(" ".join([command[0], *target_flags]))
                if len(set(recorded_flags)) != 1:
                    raise RuntimeError("product translator and encoder flags differ")
                variant_flags = recorded_flags[0]
            provenance[name] = {
                "sources": {path: sha(tree / path) for path in sorted(varied)},
                "objects": {
                    str(path).replace("build/hosted", "build/bm")
                    if args.product
                    else str(path): sha(obj)
                    for path, obj in zip(object_paths, objects[name])
                },
                "flags": variant_flags,
            }
        if provenance["baseline"]["flags"] != provenance["current"]["flags"]:
            raise RuntimeError("baseline and current target flags differ")
        symbols = set()
        for path in objects["baseline"]:
            output = subprocess.check_output(["nm", "-g", "--defined-only", str(path)], text=True)
            for line in output.splitlines():
                name = line.split()[-1]
                if name == "tier0_emit" or name.startswith("x86_"):
                    symbols.add(name)
        mapping = temp / "rename.txt"
        mapping.write_text(
            "".join(f"{name} {name}_baseline\n" for name in sorted(symbols))
            + "inline_requested inline_requested_baseline\n"
        )
        old = []
        for path in objects["baseline"]:
            obj = temp / ("renamed-" + path.name)
            exported = temp / ("exported-" + path.name)
            subprocess.run(
                ["objcopy", "--globalize-symbol", "inline_requested", str(path), str(exported)],
                check=True,
            )
            subprocess.run(
                ["objcopy", "--redefine-syms", str(mapping), str(exported), str(obj)], check=True
            )
            old.append(str(obj))
        current = temp / "current-translate.o"
        exported = temp / "exported-current-translate.o"
        subprocess.run(
            [
                "objcopy",
                "--globalize-symbol",
                "inline_requested",
                str(objects["current"][0]),
                str(exported),
            ],
            check=True,
        )
        subprocess.run(
            [
                "objcopy",
                "--redefine-sym",
                "inline_requested=inline_requested_current",
                str(exported),
                str(current),
            ],
            check=True,
        )
        arguments = [
            str(current)
            if arg == str(object_paths[0])
            else str(objects["current"][1])
            if arg == str(object_paths[1])
            else arg
            for arg in arguments
        ]
        if args.product:
            arguments.append("-no-pie")
        program = temp / "bench"
        driver = Path(__file__).resolve().with_name("translator-abba.c")
        subprocess.run(
            [link[0], *cflags, str(driver), *arguments, *old, "-o", str(program)],
            cwd=root,
            check=True,
        )
        os.sched_setaffinity(0, {args.cpu})
        raw = json.loads(
            subprocess.check_output(
                [
                    str(program),
                    args.case,
                    str(args.first_reg),
                    str(args.mask),
                    str(args.blocks),
                    str(args.emit_count),
                    str(args.run_count),
                    str(args.insns),
                    str(args.entry_offset),
                    str(budget),
                    str(int(args.check_request_advance)),
                ],
                text=True,
            )
        )
    keys = ("bytes", "emit_ns_per_region", "run_ns_per_instruction")
    summary = {}
    for name in ("baseline", "current"):
        rows = [sample for sample in raw["samples"] if sample["variant"] == name]
        if len({row["bytes"] for row in rows}) != 1:
            raise RuntimeError("code size changed across identical emissions")
        summary[name] = {key: statistics.median(row[key] for row in rows) for key in keys}
    paired = []
    for block in range(1, args.blocks + 1):
        values = {
            name: {
                key: statistics.mean(
                    row[key]
                    for row in raw["samples"]
                    if row["variant"] == name and row["block"] == block
                )
                for key in keys[1:]
            }
            for name in ("baseline", "current")
        }
        paired.append(
            {
                "block": block,
                **values,
                "percent_change": {
                    key: 100 * (values["current"][key] / values["baseline"][key] - 1)
                    for key in keys[1:]
                },
            }
        )
    paired_summary = {
        key: {
            "median_percent_change": statistics.median(
                row["percent_change"][key] for row in paired
            ),
            "min_percent_change": min(row["percent_change"][key] for row in paired),
            "max_percent_change": max(row["percent_change"][key] for row in paired),
            "improved_blocks": sum(row["percent_change"][key] < 0 for row in paired),
        }
        for key in keys[1:]
    }
    print(
        json.dumps(
            {
                "case": args.case,
                "first_reg": args.first_reg,
                "host_isa_mask": args.mask,
                "host_isa": raw["host_isa"],
                "cpu": args.cpu,
                "platform": platform.platform(),
                "insns": args.insns,
                "entry_offset": args.entry_offset,
                "budget": budget,
                "counts": {
                    "blocks": args.blocks,
                    "emits_per_sample": args.emit_count,
                    "calls_per_sample": args.run_count,
                },
                "method": "Same process; thread CPU time; one warmup block followed by alternating ABBA/BAAB. Native code, vCPU and execution-record addresses are identical for both variants; copying, W^X changes and per-sample warmup are outside timers.",
                "native_warmup_calls_per_sample": raw["native_warmup_calls_per_sample"],
                "request_callback_checks": raw["request_callback_checks"],
                "scope": "Buffered emission and prebuilt native regions; excludes decoding, lookup, dispatch and publication.",
                "target_objects": "freestanding product translator/encoder"
                if args.product
                else "hosted translator/encoder",
                "shared_support_flags": (root / "build/hosted/.flags").read_text(),
                "checksum": raw["checksum"],
                "same_bytes_except_callback_address": raw["same_bytes_except_callback_address"],
                "driver_sha256": sha(Path(__file__).resolve().with_name("translator-abba.c")),
                "runner_sha256": sha(Path(__file__).resolve()),
                "provenance": provenance,
                "shared_sources": shared,
                "summary": summary,
                "paired_summary": paired_summary,
                "pairs": paired,
                "samples": raw["samples"],
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
