# SPDX-License-Identifier: GPL-2.0-or-later
"""Median report of `make bench-tier1` samples.

Reads the `bench ENGINE PROGRAM start S cold X warm Y segment N` lines that
`build/tier1-test --bench=...` prints (one process per sample, so every
start and cold number is a fresh process) and prints one Markdown table
row per (program, engine): sample count and the median start-up, cold and
warm guest instructions per second, in millions.

Input: the `bench-t1 ENGINE PROGRAM key value ...` lines (Tier1's
counters of the warm segment and its compile report) become a second table
of medians, one row per (program, engine) that has them; engines without
them ("n/a") are listed as such. Skip lines (`bench tier1 ... skipped`) are
passed through.
"""

import statistics
import sys


def main():
    samples = {}
    order = []
    counters, counter_order, skipped = {}, [], []
    for line in sys.stdin:
        fields = line.split()
        if len(fields) > 3 and fields[0] == "bench" and fields[3] == "skipped:":
            if line.strip() not in skipped:
                skipped.append(line.strip())
            continue
        if fields and fields[0] == "bench-t1":
            key = (fields[2], fields[1])
            if key not in counters:
                counters[key] = []
                counter_order.append(key)
            if fields[3] != "n/a":
                counters[key].append(dict(zip(fields[3::2], (float(x) for x in fields[4::2]))))
            continue
        if len(fields) != 11 or fields[0] != "bench":
            continue
        _, engine, program, _, start, _, cold, _, warm, _, segment = fields
        key = (program, engine)
        if key not in samples:
            samples[key] = []
            order.append(key)
        samples[key].append((float(start), float(cold), float(warm), int(segment)))
    if not samples:
        sys.exit("bench_report: no samples")
    print("| program | engine | runs | segment | start Minsn/s | cold Minsn/s | warm Minsn/s |")
    print("| --- | --- | ---: | ---: | ---: | ---: | ---: |")
    for program, engine in sorted(order):
        rows = samples[(program, engine)]
        start, cold, warm = (statistics.median(r[i] for r in rows) / 1e6 for i in range(3))
        print(
            f"| {program} | {engine} | {len(rows)} | {rows[0][3]} | {start:.1f} | "
            f"{cold:.1f} | {warm:.1f} |"
        )
    for line in skipped:
        print(f"\n{line}")
    names = []
    for key in counter_order:
        for row in counters[key]:
            names += [n for n in row if n not in names]
    if names:
        print("\nTier1 counters (warm segment deltas and the compile report, medians):\n")
        print("| program | engine | " + " | ".join(names) + " |")
        print("| --- | --- |" + " ---: |" * len(names))
        for program, engine in sorted(counter_order):
            rows = counters[(program, engine)]
            if not rows:
                print(f"| {program} | {engine} |" + " n/a |" * len(names))
                continue
            cells = []
            for n in names:
                values = [r[n] for r in rows if n in r]
                cells.append(f"{statistics.median(values):g}" if values else "n/a")
            print(f"| {program} | {engine} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
