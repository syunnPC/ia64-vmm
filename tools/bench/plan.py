#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Packs job records (build/bench-jobdump --dump) into a benchwrap job file.

Layout (tests/bench/job.h struct bj_file): header with job offsets and the
step list, then the job records, 64-byte aligned. Steps: for each job in
order, the variants of --pattern (default 0,1,1,1: variant 0 first, the
cold run, i.e. the first run of that code in the process, then three runs
of variant 1). Single-variant jobs (speed) run variant 0 at every position,
so two plans that differ only in the pattern's variants have the same
steps for them.

Usage: plan.py --out jobs.bin [--pattern 0,2,2,2] JOB.bin...
"""

import argparse
import struct
from pathlib import Path

MAGIC = 0x424F4A4234364149
VERSION = 2
MAX_JOBS = 16
MAX_STEPS = 256


def record_layout():
    """Byte offset of nvariant in struct bj_job: name[32], then u64 fields
    size; ip..ccv (11); iva, pta, dcr, pr, nstacked (5); gr, bank0, br
    (32 + 16 + 8); rr_mask, rr (1 + 8); ntr, tr (1 + 4 x 5); npurge,
    purge (1 + 64 x 3)."""
    words = 1 + 11 + 5 + 56 + 9 + 1 + 20 + 1 + 192
    return 32 + 8 * words


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--pattern", default="0,1,1,1")
    ap.add_argument("jobs", nargs="+")
    args = ap.parse_args()
    if len(args.jobs) > MAX_JOBS:
        raise SystemExit("plan: too many jobs")
    records = [Path(j).read_bytes() for j in args.jobs]
    nvar_at = record_layout()
    steps = []
    for index, rec in enumerate(records):
        (nvariant,) = struct.unpack_from("<Q", rec, nvar_at)
        if not 1 <= nvariant <= 4:
            raise SystemExit(f"plan: {args.jobs[index]}: bad variant count {nvariant} (layout?)")
        for v in (int(x) for x in args.pattern.split(",")):
            if v >= nvariant and nvariant > 1:
                raise SystemExit(f"plan: {args.jobs[index]} has no variant {v}")
            steps.append((index, v if nvariant > 1 else 0))
    if len(steps) > MAX_STEPS:
        raise SystemExit("plan: too many steps")
    header_size = 4 * 8 + MAX_JOBS * 8 + MAX_STEPS * 16
    offsets, blob, at = [], b"", (header_size + 63) & ~63
    for rec in records:
        offsets.append(at)
        pad = (-len(rec)) % 64
        blob += rec + b"\0" * pad
        at += len(rec) + pad
    header = struct.pack("<4Q", MAGIC, VERSION, len(records), len(steps))
    header += struct.pack(f"<{MAX_JOBS}Q", *(offsets + [0] * (MAX_JOBS - len(offsets))))
    flat = [x for s in steps for x in s] + [0] * (2 * (MAX_STEPS - len(steps)))
    header += struct.pack(f"<{2 * MAX_STEPS}Q", *flat)
    header += b"\0" * (((header_size + 63) & ~63) - header_size)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_bytes(header + blob)
    print(
        f"plan: {args.out}: {len(records)} jobs, {len(steps)} steps, {len(header) + len(blob)} bytes"
    )


if __name__ == "__main__":
    main()
