# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared helpers of the engine benchmarks (see docs/testing.md).

Environment: variables come from the environment or the untracked local.env
at the repository root (KEY=value lines, see local.env.example), never
from paths written here.
"""

import json
import os
import re
import statistics
import subprocess
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def load_env(*keys, required=True):
    """KEY values from the environment, else local.env; exit naming a missing one."""
    file_env = {}
    path = REPO / "local.env"
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            m = re.match(r"([A-Za-z_][A-Za-z0-9_]*)=(.*)$", line)
            if m:
                file_env[m.group(1)] = m.group(2)
    out = {}
    for key in keys:
        value = os.environ.get(key, file_env.get(key))
        if not value and required:
            raise SystemExit(
                f"bench: {key} is not set (environment or local.env, see local.env.example)"
            )
        out[key] = value
    return out


def loadavg():
    return [float(x) for x in Path("/proc/loadavg").read_text().split()[:3]]


def cpu_busy(seconds=2.0):
    """Per-CPU busy fraction over `seconds` (from /proc/stat)."""

    def snap():
        d = {}
        for line in Path("/proc/stat").read_text().splitlines():
            if line.startswith("cpu") and line[3].isdigit():
                f = line.split()
                v = [int(x) for x in f[1:]]
                d[int(f[0][3:])] = (sum(v), v[3] + v[4])
        return d

    a = snap()
    time.sleep(seconds)
    b = snap()
    return {c: 1 - (b[c][1] - a[c][1]) / max(1, b[c][0] - a[c][0]) for c in a}


def pick_cpu():
    """BENCH_CPU if set, else the least busy CPU over a 2 s sample."""
    env = os.environ.get("BENCH_CPU")
    if env:
        return int(env), None
    busy = cpu_busy()
    cpu = min(busy, key=lambda c: busy[c])
    return cpu, busy


def pin(cpu):
    return ["taskset", "-c", str(cpu)] if cpu is not None else []


def spread(values):
    """(min, median, max, (max - min) / median)."""
    lo, med, hi = min(values), statistics.median(values), max(values)
    return lo, med, hi, (hi - lo) / med if med else float("inf")


def git_describe(path):
    def run(*args):
        r = subprocess.run(["git", "-C", str(path), *args], capture_output=True, text=True)
        return r.stdout.strip()

    return {
        "head": run("rev-parse", "HEAD"),
        "describe": run("log", "-1", "--format=%h %s"),
        "dirty": bool(run("status", "--porcelain", "--untracked-files=no")),
    }


def host_info():
    model = ""
    for line in Path("/proc/cpuinfo").read_text().splitlines():
        if line.startswith("model name"):
            model = line.split(":", 1)[1].strip()
            break
    return {"cpu": model, "kernel": os.uname().release, "nproc": os.cpu_count()}


def write_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=1, sort_keys=True) + "\n")
    tmp.replace(path)
