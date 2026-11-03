#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Runs benchwrap.efi on one platform and parses its BENCH lines.

Platform:
  tcg           $QEMU_IA64 -machine itanium2-vpc (TCG, the fork's firmware),
                the plan's FAT disk on the default SCSI controller

Every output line is timestamped on arrival (host monotonic clock), so a
step's wall time is the gap between its BENCH line and the previous one;
the process wall time is measured too. Returns a dict (see run()).

Usage: wrap_run.py PLATFORM PLAN_DIR [--cpu N] [--json OUT]
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

PLATFORMS = ("tcg",)


def command(platform, plan_dir):
    plan_dir = Path(plan_dir)
    qemu = common.load_env("QEMU_IA64")["QEMU_IA64"]
    return [
        qemu,
        "-machine",
        "itanium2-vpc,nvram=none",
        "-drive",
        f"file={plan_dir / 'disk.img'},format=raw,snapshot=on",
        "-display",
        "none",
        "-serial",
        "stdio",
        "-monitor",
        "none",
    ]


KV = re.compile(r"(\w+)=(\S+)")


def parse(lines):
    out = {"steps": [], "freq": None, "env": {}, "window": None, "done": None, "errors": []}
    for t, line in lines:
        i = line.find("BENCH")
        if i < 0:
            continue
        body = line[i:]
        kind = body.split()[0]
        kv = dict(KV.findall(body))
        if kind == "BENCH":
            step = {"t": t, "job": kv["job"]}
            for k, v in kv.items():
                if k == "f":
                    step["faults"] = [int(x) for x in v.split(",")]
                elif k != "job":
                    step[k] = int(v, 16) if v.startswith("0x") else int(v)
            out["steps"].append(step)
        elif kind == "BENCH-FREQ":
            out["freq"] = {k: int(v) for k, v in kv.items()}
        elif kind == "BENCH-ENV":
            out["env"].update(kv)
        elif kind == "BENCH-WINDOW":
            out["window"] = kv["base"]
        elif kind == "BENCH-DONE":
            out["done"] = {k: int(v) for k, v in kv.items()}
            out["t_done"] = t
        elif kind in ("BENCH-ERROR", "BENCH-CHECK"):
            out["errors"].append(body.strip())
        elif kind == "BENCH-START":
            out["t_start"] = t
    return out


def run(platform, plan_dir, cpu=None):
    cmd = common.pin(cpu) + command(platform, plan_dir)
    load_before = common.loadavg()
    t0 = time.monotonic()
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, bufsize=0
    )
    lines, buf = [], b""
    while True:
        chunk = (
            proc.stdout.read1(65536) if hasattr(proc.stdout, "read1") else proc.stdout.read(65536)
        )
        if not chunk:
            break
        t = time.monotonic() - t0
        buf += chunk
        *complete, buf = buf.split(b"\n")
        for raw in complete:
            lines.append((t, raw.decode("ascii", "replace").replace("\r", "")))
    if buf:
        lines.append((time.monotonic() - t0, buf.decode("ascii", "replace")))
    rc = proc.wait()
    wall = time.monotonic() - t0
    result = parse(lines)
    result.update(
        platform=platform,
        plan=str(plan_dir),
        cpu=cpu,
        rc=rc,
        wall=wall,
        loadavg_before=load_before,
        loadavg_after=common.loadavg(),
        command=cmd,
    )
    ok = (
        rc == 0
        and result["done"] is not None
        and result["done"]["failures"] == 0
        and all(s["ok"] == 1 for s in result["steps"])
    )
    result["ok"] = ok
    if not ok:
        result["log_tail"] = [l for _, l in lines[-60:]]
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("platform", choices=PLATFORMS)
    ap.add_argument("plan_dir")
    ap.add_argument("--cpu", type=int)
    ap.add_argument("--json")
    args = ap.parse_args()
    r = run(args.platform, args.plan_dir, args.cpu)
    hz = r["freq"]["hz"] if r["freq"] else None
    prev = r.get("t_start", 0)
    for s in r["steps"]:
        rate = s["retired"] / (s["itc"] / hz) / 1e6 if hz and s["itc"] else float("nan")
        print(
            f"{s['step']:3} {s['job']:16} var {s['var']} retired {s['retired']:>11} "
            f"itc {s['itc']:>12} {rate:9.1f} Minsn/s  wall-gap {s['t'] - prev:7.3f}s ok {s['ok']}"
        )
        prev = s["t"]
    print(f"loadavg {r['loadavg_before']} -> {r['loadavg_after']}")
    print(f"rc {r['rc']} wall {r['wall']:.2f}s ok {r['ok']}")
    if not r["ok"]:
        print("\n".join(r.get("log_tail", [])))
    if args.json:
        common.write_json(args.json, r)


if __name__ == "__main__":
    main()
