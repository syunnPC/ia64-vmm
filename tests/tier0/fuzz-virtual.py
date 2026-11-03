#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the existing generator (virtual programs, or physical with
--physical) against interp, Tier0 and lockstep. Tier0 and lockstep run every
program twice from the same state (runner --repeat): the first run executes
new regions through their staged helpers, the second their sealed native
code; the second run is compared with the interpreter."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ia64_fuzz", ROOT / "tests/fuzz/fuzz.py")
fuzz = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fuzz)


def debug_setup():
    """PSR.db = 1 with IBR/DBR pairs that match every region 0 and region 1
    address (mask 0 compares only bits 63:56) at PL1-3 only (SDM Vol.2
    §7.1.1): CPL 0 code stays in Tier0, while RSE references at RSC.pl > 0
    raise Data Debug faults from Tier0 helpers."""
    lines = []
    for bank, enable in (("ibr", 1 << 63), ("dbr", 3 << 62)):
        for pair, top in enumerate((0, 1 << 61)):
            lines += [
                f"movl r2=0x{top:x} ;;",
                f"mov r3={2 * pair} ;;",
                f"mov {bank}[r3]=r2 ;;",
                f"movl r2=0x{enable | 14 << 56:x} ;;",
                f"mov r3={2 * pair + 1} ;;",
                f"mov {bank}[r3]=r2 ;;",
            ]
    return lines + [
        "srlz.d ;;",
        "mov r2=psr ;;",
        f"movl r3=0x{1 << 24:x} ;;",
        "or r2=r2,r3 ;;",
        "mov psr.l=r2 ;;",
        "srlz.i ;;",
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seeds", type=int, default=1000)
    parser.add_argument("--seed0", type=int, default=1)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--workdir", type=Path, default=ROOT / "build/tests/tier0/virtual-fuzz")
    parser.add_argument(
        "--debug", action="store_true", help="append debug_setup() to every program"
    )
    parser.add_argument("--physical", action="store_true", help="physical-mode programs")
    args = parser.parse_args()
    if min(args.seeds, args.jobs) < 1:
        parser.error("--seeds and --jobs must be positive")
    env = fuzz.load_env()
    runners = [
        ROOT / "build/fuzz/ia64-fuzz-runner",
        ROOT / "build/tier0-fuzz-runner",
        ROOT / "build/lockstep-fuzz-runner",
    ]
    tools = [
        fuzz.Tools(env, runner, args.workdir, () if runner == runners[0] else ("--repeat",))
        for runner in runners
    ]

    def evaluate(seed):
        program = fuzz.Gen(seed, 40, virtual=not args.physical).program(0.1)
        if args.debug:
            program.setup += debug_setup()
        _, _, image, checkpoint = tools[0].build(program, f"s{seed}")
        expected = tools[0].run_interp(image, checkpoint)
        for tool in tools[1:]:
            actual = tool.run_interp(image, checkpoint)
            if actual != expected:
                raise RuntimeError(f"seed {seed}: {tool.runner} differs from interp; {image}")
        if expected["term"]["kind"] != "end":
            raise RuntimeError(f"seed {seed}: incomplete execution {expected['term']}; {image}")
        return len(expected["trace"])

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        bundles = sum(pool.map(evaluate, range(args.seed0, args.seed0 + args.seeds)))
    print(
        f"{'physical' if args.physical else 'virtual'} fuzz{' (PSR.db)' if args.debug else ''}: "
        f"{args.seeds} seeds, {bundles} bundle states, Tier0/lockstep (second run) == interp"
    )


if __name__ == "__main__":
    main()
