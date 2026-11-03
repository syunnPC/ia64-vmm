#!/usr/bin/env python3
# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Read-only FPSWA discovery evidence from the configured Windows 2003 media.

Extracted proprietary bytes and disassembly stay under build/. This is a
version-pinned ABI inspection, not a Windows boot or kernel-assist test.
"""

import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from qemu.config import load_local_env, required

load_local_env()
media = json.loads(Path(required("IA64_MEDIA_MANIFEST")).read_text())["windows"]["iso"]
out = ROOT / "build/tests/fpswa/windows2003"
out.mkdir(parents=True, exist_ok=True)
archive = out / "ia64ldr.ef_"
archive.write_bytes(
    subprocess.check_output(
        ["7z", "e", "-so", str(Path(required("IA64_IMAGES_DIR")) / media), "ia64/ia64ldr.ef_"]
    )
)
subprocess.run(
    ["7z", "e", "-y", "-o" + str(out), str(archive)], check=True, stdout=subprocess.DEVNULL
)
loader = out / "ia64ldr.efi"
digest = hashlib.sha256(loader.read_bytes()).hexdigest()
if digest != "4fe63643e79e0cce2c7af416f929bff832b03ddf66f5f39777af0dc48b135c59":
    raise SystemExit("Uninspected Windows loader version: " + digest)
disassembly = subprocess.check_output(
    [required("CROSS_IA64") + "objdump", "-d", str(loader)], text=True
)
(out / "disassembly.txt").write_text(disassembly)
evidence = {
    "10a17e6": "movl r29=0xc41b6531",
    "10a18c0": "st2 [r94]=r25",  # GUID Data3 0x11d3
    "10a18c6": "st2 [r28]=r22",  # GUID Data2 0x97b9
    "10a2306": "mov r90=2",  # ByProtocol
    "10a22fc": "adds r91=80,r12",
    "10a2326": "adds r30=176,r95",  # EFI_BOOT_SERVICES.LocateHandle
    "10a234c": "br.call.dptk.many b0=b6",
    "10a236c": "adds r91=80,r12",
    "10a2376": "adds r31=152,r26",  # EFI_BOOT_SERVICES.HandleProtocol
    "10a23ac": "br.call.dptk.many b0=b7",
    "10a2d56": "st8 [r58]=r30",  # Hand off the retrieved interface.
}
for address, instruction in evidence.items():
    line = next(
        (line for line in disassembly.splitlines() if line.lstrip().startswith(address + ":")), None
    )
    if not line or instruction not in line:
        raise SystemExit("Loader evidence changed at " + address)
(out / "evidence.json").write_text(
    json.dumps(
        dict(
            media=media,
            sha256=digest,
            observations=evidence,
            limitation="Static discovery ABI only; no Windows kernel FPSWA execution",
        ),
        indent=2,
    )
    + "\n"
)
print(
    "Windows 2003 IA64 loader: FPSWA GUID, LocateHandle(ByProtocol), HandleProtocol and handoff verified; "
    + digest
)
