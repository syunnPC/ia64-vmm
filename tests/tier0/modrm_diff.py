#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare actual original/current Tier0 encoders across register/buffer boundaries."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument(
        "--sanitize",
        action="store_true",
        help="Build both encoders and driver with ASan/UBSan instead of product flags.",
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    baseline = args.baseline.resolve()
    if (root / "engine/tier0/x86emit.h").read_bytes() != (
        baseline / "engine/tier0/x86emit.h"
    ).read_bytes():
        raise RuntimeError("encoder headers differ")
    with tempfile.TemporaryDirectory(prefix="tier0-modrm-diff-") as scratch:
        scratch = Path(scratch)
        objects, provenance = {}, {}
        for name, tree in (("baseline", baseline), ("current", root)):
            if args.sanitize:
                compiler = "gcc"
                flags = [
                    "-std=c11",
                    "-O1",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Iinclude",
                    "-I.",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",
                ]
            else:
                dry = subprocess.check_output(
                    [
                        "make",
                        "-Bn",
                        "SAN=0",
                        "TIER0_STATS=0",
                        "TIER1_STATS=0",
                        "build/bm/engine/tier0/x86emit.o",
                    ],
                    cwd=tree,
                    text=True,
                )
                commands = [
                    shlex.split(line)
                    for line in dry.replace("\\\n", " ").splitlines()
                    if line.strip()
                ]
                command = next(c for c in commands if "-c" in c and "engine/tier0/x86emit.c" in c)
                compiler = command[0]
                flags = [f for f in command[1 : command.index("-c")] if f not in ("-MMD", "-MP")]
            obj = scratch / (name + ".o")
            subprocess.run(
                [compiler, *flags, "-c", "engine/tier0/x86emit.c", "-o", str(obj)],
                cwd=tree,
                check=True,
            )
            objects[name] = obj
            sections = subprocess.check_output(["size", "-A", str(obj)], text=True)
            text_bytes = int(
                next(line.split()[1] for line in sections.splitlines() if line.startswith(".text "))
            )
            provenance[name] = {
                "source_sha256": sha(tree / "engine/tier0/x86emit.c"),
                "header_sha256": sha(tree / "engine/tier0/x86emit.h"),
                "object_sha256": sha(obj),
                "text_bytes": text_bytes,
                "flags": " ".join([compiler, *flags]),
            }
        if provenance["baseline"]["flags"] != provenance["current"]["flags"]:
            raise RuntimeError("encoder flags differ")
        symbols = subprocess.check_output(
            ["nm", "-g", "--defined-only", str(objects["baseline"])], text=True
        )
        mapping = scratch / "rename.txt"
        mapping.write_text(
            "".join(
                f"{name} {name}_baseline\n"
                for name in sorted(
                    line.split()[-1]
                    for line in symbols.splitlines()
                    if line.split()[-1].startswith("x86_")
                )
            )
        )
        old = scratch / "renamed.o"
        subprocess.run(
            ["objcopy", "--redefine-syms", str(mapping), str(objects["baseline"]), str(old)],
            check=True,
        )
        driver = Path(__file__).with_name("modrm-diff.c")
        program = scratch / "diff"
        driver_flags = [
            "-std=c11",
            "-O1" if args.sanitize else "-O2",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(root / "include"),
            "-I" + str(root),
            "-no-pie",
        ]
        if args.sanitize:
            driver_flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(
            [
                "gcc",
                *driver_flags,
                str(driver),
                str(old),
                str(objects["current"]),
                "-o",
                str(program),
            ],
            check=True,
        )
        env = dict(os.environ)
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1"
        result = json.loads(subprocess.check_output([str(program)], text=True, env=env))
    print(
        json.dumps(
            {
                "mode": "ASan/UBSan" if args.sanitize else "freestanding product encoders",
                "result": result,
                "provenance": provenance,
                "driver_sha256": sha(driver),
                "runner_sha256": sha(Path(__file__).resolve()),
                "driver_flags": driver_flags,
                "coverage": "All 16x16 register pairs for 15 changed register APIs and 9 memory controls; representative opcode/group/width/immediate/count combinations; capacities 0..17; starts 0,1,7,capacity+1,SIZE_MAX; real/measuring buffers; clear/sticky failure; VEX invalid registers; whole-buffer canaries.",
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
