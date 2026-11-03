#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record exact command, owned PID, elapsed time and load for SMP runs."""

import json
import os
from pathlib import Path
import subprocess
import sys
import time
import threading

record = Path(sys.argv[1])
command = sys.argv[2:]
start = time.monotonic()
load_before = os.getloadavg()
process = subprocess.Popen(command)
print(f"SMP PID={process.pid} load_before={load_before} command={command}", flush=True)
samples = []
finished = threading.Event()


def sample_load():
    while not finished.wait(5):
        samples.append(dict(seconds=time.monotonic() - start, load=os.getloadavg()))


sampler = threading.Thread(target=sample_load)
sampler.start()
status = process.wait()
finished.set()
sampler.join()
result = dict(
    command=command,
    pid=process.pid,
    status=status,
    elapsed=time.monotonic() - start,
    load_before=load_before,
    load_after=os.getloadavg(),
    load_samples=samples,
)
record.parent.mkdir(parents=True, exist_ok=True)
record.write_text(json.dumps(result, indent=2) + "\n")
print(
    "SMP timing: " + json.dumps({k: v for k, v in result.items() if k != "load_samples"}),
    flush=True,
)
raise SystemExit(status)
