# SPDX-License-Identifier: GPL-2.0-or-later
"""Vendor overlays must apply exactly at their reviewed line positions."""

import difflib
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("build_edk2", ROOT / "tools/firmware/build_edk2.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class OverlayPatch(unittest.TestCase):
    def check_application(self, apply):
        original = "".join(f"line {i}\n" for i in range(20))
        modified = original.replace("line 10\n", "replacement\n")
        diff = "".join(
            difflib.unified_diff(
                original.splitlines(True),
                modified.splitlines(True),
                fromfile="a/source.c",
                tofile="b/source.c",
            )
        )
        with tempfile.TemporaryDirectory(prefix="overlay-patch-") as directory:
            temp = Path(directory)
            source, patch, dest = (temp / name for name in ("source.c", "fix.patch", "output.c"))
            patch.write_text(diff)
            for name, text in [
                ("exact", original),
                ("offset", "extra\n" + original),
                ("fuzz", original.replace("line 7\n", "changed context\n")),
            ]:
                source.write_text(text)
                if name == "exact":
                    apply(source, patch, dest)
                    self.assertEqual(dest.read_text(), modified)
                else:
                    with self.assertRaises((RuntimeError, subprocess.CalledProcessError)):
                        apply(source, patch, dest)

    def test_exact_application(self):
        self.check_application(builder.apply_overlay)

    def test_pinned_overlays(self):
        with tempfile.TemporaryDirectory(prefix="pinned-overlays-") as directory:
            for source, overlay in builder.PATCHES.items():
                with self.subTest(overlay=overlay):
                    upstream = Path(directory) / "upstream.c"
                    upstream.write_text((ROOT / source).read_text())
                    builder.apply_overlay(upstream, ROOT / overlay, Path(directory) / "patched.c")


if __name__ == "__main__":
    unittest.main()
