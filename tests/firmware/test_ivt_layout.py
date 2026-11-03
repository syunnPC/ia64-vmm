# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Check the assembly context offsets against the upstream IPF ABI."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class InterruptLayoutTests(unittest.TestCase):
    def test_context_offsets(self):
        header = (ROOT / "firmware/new/ivt-layout.h").read_text()
        fields = re.findall(r"#define IPF_CONTEXT_(\w+) (\d+)", header)
        self.assertGreater(len(fields), 100)
        source = "#include <stddef.h>\n#undef NULL\n#include <Uefi.h>\n#include <Protocol/DebugSupport.h>\n"
        source += "\n".join(
            f'_Static_assert(offsetof(EFI_SYSTEM_CONTEXT_IPF,{field})+16=={offset},"{field}");'
            for field, offset in fields
        )
        source += "\nint main(void) { return 0; }\n"
        with tempfile.TemporaryDirectory(prefix="ivt-layout-") as directory:
            path = Path(directory) / "layout.c"
            path.write_text(source)
            subprocess.run(
                [
                    os.environ.get("HOSTCC", "gcc"),
                    "-std=c11",
                    "-Werror",
                    "-D_EFI_P64",
                    "-I" + str(ROOT / "third_party/edk2-ipf/MdePkg/Include"),
                    "-I" + str(ROOT / "third_party/edk2-ipf/MdePkg/Include/Ipf"),
                    "-c",
                    str(path),
                    "-o",
                    str(path.with_suffix(".o")),
                ],
                check=True,
                capture_output=True,
            )
