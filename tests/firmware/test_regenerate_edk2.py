# SPDX-License-Identifier: GPL-2.0-or-later
"""A regenerated manifest must still supply every firmware volume driver."""

import contextlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/firmware"))
import make_dxe_fv as pack
import snapshot_edk2 as freeze


class RegenerationTests(unittest.TestCase):
    def test_snapshot_retains_fpswa_and_packs_volume(self):
        previous = json.loads((ROOT / "firmware/edk2/manifest.json").read_text())
        fpswa_key, fpswa = next(
            (k, r) for k, r in previous["recipes"].items() if r["name"] == "VmmFpswa"
        )
        memory_key = fpswa["libraries"][0]
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode="w") as stream:
            license = tarfile.TarInfo("License.txt")
            license.size = 4
            stream.addfile(license, io.BytesIO(b"test"))
        with tempfile.TemporaryDirectory(prefix="edk2-snapshot-test-") as directory:
            repo = Path(directory) / "repo"
            workspace = Path(directory) / "workspace"
            manifest = repo / "firmware/edk2/manifest.json"
            manifest.parent.mkdir(parents=True)
            previous["recipes"]["obsolete"] = dict(fpswa, name="ObsoleteDriver")
            manifest.write_text(json.dumps(previous))
            modules = [k for k in previous["roots"] if k != fpswa_key] + [memory_key]
            for key in modules:
                recipe = previous["recipes"][key]
                inf = Path(recipe["inf"])
                build = workspace / "Build" / key
                (build / "DEBUG").mkdir(parents=True)
                (build / "DEBUG/AutoGen.c").write_text("")
                # Synthetic genmake output; FPSWA supplies its own metadata and
                # has no GNUmakefile in the regeneration workspace.
                library = (
                    ("$(BIN_DIR)/" + memory_key.split("/IPF/")[1] + "/OUTPUT/BaseMemoryLib.lib")
                    if recipe["name"] == "DxeCore"
                    else ""
                )
                (build / "GNUmakefile").write_text(
                    f"MODULE_NAME = {recipe['name']}\n"
                    f"MODULE_RELATIVE_DIR = {inf.parent}\n"
                    f"MODULE_FILE = {inf.name}\n"
                    f"MODULE_TYPE = {recipe['kind']}\n"
                    f"STATIC_LIBRARY_FILES = {library}\n"
                    f'\t"$(CC)" -c {workspace}/firmware/new/stub.c\n'
                )
                target = (
                    repo
                    / ("" if str(inf).startswith("firmware/") else "third_party/edk2-ipf")
                    / inf
                )
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(
                    "FILE_GUID = 6931222a-74ae-4d44-bde8-fc5ba42b7ac4\n[Depex]\nTRUE\n"
                )
            target = repo / fpswa["inf"]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text((ROOT / fpswa["inf"]).read_text())
            (target.parent / "metadata.c").write_text("")
            frozen = None
            for _ in range(2):
                with (
                    patch.object(freeze, "REPO", repo),
                    patch.object(
                        freeze.subprocess,
                        "check_output",
                        side_effect=["License.txt\n", archive.getvalue()],
                    ),
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    freeze.snapshot(workspace, workspace)
                if frozen is not None:
                    self.assertEqual(manifest.read_bytes(), frozen)
                frozen = manifest.read_bytes()
            regenerated = json.loads(manifest.read_text())
            self.assertIn(fpswa_key, regenerated["recipes"])
            self.assertEqual(regenerated["recipes"][fpswa_key], fpswa)
            self.assertEqual(regenerated["roots"].count(fpswa_key), 1)
            self.assertIn(memory_key, regenerated["recipes"])
            self.assertNotIn("obsolete", regenerated["recipes"])
            names = {regenerated["recipes"][k]["name"] for k in regenerated["roots"]}
            self.assertTrue({"DxeCore", *pack.NAMES} <= names)
            dec = repo / "third_party/edk2-ipf/MdePkg/MdePkg.dec"
            dec.parent.mkdir(parents=True, exist_ok=True)
            dec.write_text(
                "\n".join(
                    name + " = {0,0,0,{0,0,0,0,0,0,0,0}}"
                    for name in (
                        "gEfiFirmwareVolumeBlockProtocolGuid",
                        "gEfiFaultTolerantWriteProtocolGuid",
                    )
                )
            )
            for release in (False, True):
                out = repo / "build/firmware-new" / ("release" if release else "")
                out.mkdir(parents=True, exist_ok=True)
                for name in ("DxeCore", *pack.NAMES):
                    (out / (name + ".efi")).write_bytes(b"PE fixture")
                with patch.object(pack, "ROOT", repo), contextlib.redirect_stdout(io.StringIO()):
                    pack.build(release)
                self.assertTrue((out / "dxe.fv").is_file())


if __name__ == "__main__":
    unittest.main()
