# SPDX-License-Identifier: GPL-2.0-or-later
"""Each isolated regression must fail its corresponding normal assertion."""

import subprocess
from pathlib import Path

for name, case in [
    ("smp-iosapic-unlocked", "iosapic"),
    ("smp-iosapic-unlocked", "iosapic-intx"),
    ("smp-shared-latch", "latch"),
    ("smp-stopped-owner", "teardown-1"),
    ("smp-stopped-owner", "teardown-2"),
    ("smp-stopped-owner", "teardown-3"),
    ("smp-stopped-owner", "teardown-4"),
    ("smp-delayed-command", "pci-race"),
]:
    binary = Path("build/tests") / name
    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=120)
    binary.with_name(name + "-" + case + ".log").write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end="")
    reason = {
        ("smp-iosapic-unlocked", "iosapic"): "IA64_IOSAPIC_REMOTE_IRR",
        ("smp-iosapic-unlocked", "iosapic-intx"): "LOAD(&deliveries) == n + 1",
        ("smp-shared-latch", "latch"): "IA64VMM_SVC_PORT_DATA",
        ("smp-delayed-command", "pci-race"): (
            "hal_cpu_index() == (bdf ? 0 : 2)",
            "LOAD(&host_command) == 6",
            "!LOAD(&host_command)",
        ),
    }.get((name, case), "[panic] INTx teardown owner unavailable")
    reasons = reason if isinstance(reason, tuple) else (reason,)
    if result.returncode != 1 or not any(item in result.stderr for item in reasons):
        raise SystemExit(f"{name} not rejected by an assertion (status={result.returncode})")
    print(f"{name} rejected by the expected regression assertion/panic: PASS")
