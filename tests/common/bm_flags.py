# SPDX-License-Identifier: GPL-2.0-or-later
"""The product's ISA flags (mk/config.mk BM_SIMD_CFLAGS) for scripts that
compile VMM sources outside make."""

from pathlib import Path
import re
import shlex


def bm_simd_cflags(root=Path(__file__).resolve().parents[2]):
    text = (Path(root) / "mk/config.mk").read_text()
    match = re.search(r"^BM_SIMD_CFLAGS := (.*)$", text, re.M)
    if not match:
        raise SystemExit("mk/config.mk: no BM_SIMD_CFLAGS")
    return shlex.split(match.group(1))
