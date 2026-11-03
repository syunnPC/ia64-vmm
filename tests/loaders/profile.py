#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize opt-in loader PC samples and cumulative firmware storage counters."""

import argparse
from bisect import bisect_right
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from qemu.config import load_local_env, required


def summarize(serial, symbols):
    samples = [
        dict(
            (k, int(v, 16 if k in ("ip", "pa") else 10))
            for k, v in re.findall(r"(\w+)=([0-9a-f]+)", line)
        )
        for line in serial.splitlines()
        if "loader sample ip=" in line
    ]
    addresses = [entry[0] for entry in symbols]
    histogram = Counter()
    for sample in samples:
        pc = sample["pa"] if sample["mapped"] else sample["ip"]
        at = bisect_right(addresses, pc) - 1
        name = symbols[at][2] if at >= 0 and pc < symbols[at][0] + symbols[at][1] else f"{pc:x}"
        histogram[name] += 1
    records = [
        dict((k, int(v, 16)) for k, v in re.findall(r"(\w+)=([0-9a-fA-F]+)", line))
        for line in serial.splitlines()
        if "storage profile hz=" in line
    ]
    result = dict(samples=len(samples), histogram=histogram.most_common(30))
    if len(samples) > 1:
        elapsed = (samples[-1]["counter"] - samples[0]["counter"]) / samples[-1]["hz"]
        result["sample_seconds"] = elapsed
        result["retired_per_second"] = (
            (samples[-1]["retired"] - samples[0]["retired"]) / elapsed if elapsed else None
        )
    if records:
        first, last = records[0], records[-1]
        hz = last["hz"]
        elapsed = (last["itc"] - first["itc"]) / hz
        result["storage"] = dict(last)
        # Read time includes nested/bridged BlockIo only once. AHCI command
        # time and its polling intervals are subsets, not additional time.
        for key in ["read_ticks", "ahci_ticks", "tfd_ticks", "ci_ticks"]:
            result["storage"][key.replace("_ticks", "_seconds")] = last[key] / hz
        result["storage"].update(
            observed_seconds=elapsed,
            bytes_per_second=(last["bytes"] - first["bytes"]) / elapsed if elapsed else None,
            reads_per_second=(last["reads"] - first["reads"]) / elapsed if elapsed else None,
            poll_seconds=(last["tfd_ticks"] + last["ci_ticks"]) / hz,
        )
        sizes = re.findall(r"storage profile .* sizes=([0-9a-fA-F,]+)", serial)
        result["storage"]["request_bins"] = dict(
            zip(
                ["<2K", "2K..<4K", "4K..<8K", "8K..<16K", "16K..<32K", "32K..<64K", ">=64K"],
                [int(n, 16) for n in sizes[-1].split(",")],
            )
        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument(
        "--firmware-elf", type=Path, default=ROOT / "build/firmware-new/ia64-firmware-new.elf"
    )
    args = parser.parse_args()
    load_local_env()
    output = subprocess.check_output(
        [required("CROSS_IA64") + "nm", "-n", "-S", str(args.firmware_elf)], text=True
    )
    symbols = sorted(
        (int(address, 16), int(size, 16), name)
        for address, size, kind, name in re.findall(
            r"^([0-9a-fA-F]+) ([0-9a-fA-F]+) ([Tt]) (\S+)$", output, re.M
        )
    )
    print(json.dumps(summarize(args.log.read_text(errors="replace"), symbols), indent=2))


if __name__ == "__main__":
    main()
