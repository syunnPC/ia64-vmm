#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build real GCC leaf sequences; the harness stops at their br.ret.

Inlining selects the same architectural algorithms used by libgcc and avoids
depending on the call/RSE implementation. Preserve the generated assembly
beside the image for diagnosing compiler-specific sequences.
"""

import os
from pathlib import Path
import subprocess
import sys

prefix = os.environ.get("CROSS_IA64")
if not prefix:
    sys.exit("CROSS_IA64 is not set (see local.env.example)")
out = Path(sys.argv[1])
out.parent.mkdir(parents=True, exist_ok=True)
base = out.with_suffix("")
asm, obj, image = base.with_suffix(".s"), base.with_suffix(".o"), base.with_suffix(".bin")
subprocess.run(
    [
        prefix + "gcc",
        "-O2",
        "-fno-math-errno",
        "-minline-int-divide-min-latency",
        "-minline-sqrt-min-latency",
        "-S",
        "tests/ia64/fp-guest.c",
        "-o",
        str(asm),
    ],
    check=True,
)
if "br.call" in asm.read_text():
    sys.exit("FP guest compiler emitted a call: leaf test needs a new inlining option")
subprocess.run([prefix + "as", str(asm), "-o", str(obj)], check=True)
subprocess.run(
    [prefix + "objcopy", "-O", "binary", "-j", ".text", str(obj), str(image)], check=True
)
symbols = subprocess.check_output([prefix + "nm", "--defined-only", str(obj)], text=True)
names = []
for line in symbols.splitlines():
    address, kind, name = line.split()
    if kind == "T":
        names.append((name, int(address, 16)))
data = image.read_bytes()
out.write_text(
    "/* SPDX-License-Identifier: GPL-2.0-or-later */\n"
    "/* Generated from tests/ia64/fp-guest.c by the exported toolchain. */\n"
    "static const unsigned char fp_guest_code[] = {\n"
    + "".join(
        "    " + ",".join(f"0x{x:02x}" for x in data[i : i + 16]) + ",\n"
        for i in range(0, len(data), 16)
    )
    + "};\nstatic const struct { const char *name; unsigned offset; } fp_guest_functions[] = {\n"
    + "".join(f'    {{"{name}", 0x{address:x}}},\n' for name, address in names)
    + "};\n"
)
