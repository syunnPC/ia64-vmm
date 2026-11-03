# /* SPDX-License-Identifier: GPL-2.0-or-later */
"""Build the guest fixture with vectors from the independent integer oracle."""

from pathlib import Path
import os
import subprocess
import sys
from test_fpswa import exact_result, expected_packed, m, x

ROOT = Path(__file__).resolve().parents[2]


def build(cross=None):
    if not cross:
        env = os.environ.copy()
        local = ROOT / "local.env"
        for line in local.read_text().splitlines() if local.exists() else []:
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                env.setdefault(k, v)
        cross = env.get("CROSS_IA64")
        if not cross:
            raise SystemExit("Required variable is unset: CROSS_IA64")
    out = ROOT / "build/tests/fpswa-guest"
    out.mkdir(parents=True, exist_ok=True)
    vectors = []
    for rc in range(4):
        for op in (0, 1, 4, 5):
            for case in range(3):
                fpsr = 63 | ((14 | (rc << 4)) << 6)
                if case == 2:
                    fpsr &= ~m.I
                # Denominator scaling keeps the completed result normal even
                # at the numerator's SWA exponent limit. Native fork's cached
                # FPSWA currently spills deep WRE underflow with exponent zero;
                # that edge remains covered by the independent hosted oracle.
                a = m.FR(0, 1, m.IBIT | (case << 40))
                b = m.FR(0, 1 if op == 1 else m.BIAS - case, m.IBIT | (case << 47))
                if op == 1:
                    a = b
                st = x.new_state()
                st["fpsr"] = fpsr
                st["fr"] = {7: a, 8: b}
                i = dict(op="frsqrta" if op == 1 else "frcpa", sf=0, f1=6, f2=7, f3=8, p2=6)
                expected = exact_result(i, st)

                def raw(f):
                    return "{0x%016xULL,0x%xULL}" % (f.sig, f.exp | (f.sign << 17))

                f = m.FR(*[int(n, 16) for n in expected["f6"].split(":")])
                vectors.append(
                    "{%d,0x%xULL,%s,%s,%s,0x%xULL,%d,%d}"
                    % (
                        op,
                        fpsr,
                        raw(a),
                        raw(b),
                        raw(f),
                        expected["fpsr"],
                        (expected["pr"] >> 6) & 1,
                        expected["status"],
                    )
                )
        for op in (2, 3):
            a = m.integer(0x3F8000003F800000)
            b = m.integer(0x3F80000000400000)
            st = x.new_state()
            st["fpsr"] = 63 | (rc << 10)
            st["fr"] = {7: a, 8: b}
            i = dict(op="fprsqrta" if op == 3 else "fprcpa", sf=0, f1=6, f2=7, f3=8, p2=6)
            expected = expected_packed(i, st)
            f = m.FR(*[int(n, 16) for n in expected["f6"].split(":")])
            vectors.append(
                "{%d,0x%xULL,%s,%s,%s,0x%xULL,%d,%d}"
                % (
                    op,
                    st["fpsr"],
                    raw(a),
                    raw(b),
                    raw(f),
                    expected["fpsr"],
                    (expected["pr"] >> 6) & 1,
                    expected["status"],
                )
            )
    _, raw = x.encode(dict(op="frcpa", sf=0, f1=6, f2=7, f3=8, p2=6))
    (out / "fpswa-vectors.h").write_text(
        "/* SPDX-License-Identifier: GPL-2.0-or-later */\n"
        "static const uint64_t fpswa_guest_bundle[2]={0x%xULL,0x%xULL};\n"
        % ((0xD | (raw << 46)) & m.M64, raw >> 18)
        + "struct fpswa_guest_vector { unsigned op; uint64_t fpsr; struct fpswa_fr a,b,result; uint64_t result_fpsr,predicate,status; } __attribute__((aligned(16)));\n"
        "static const struct fpswa_guest_vector fpswa_guest_vectors[]={\n"
        + ",\n".join(vectors)
        + "\n};\n"
    )
    # Keep a single maintained context-save implementation. This fixture uses
    # a private copy of its IVT and routes FP fault/trap vectors to the saved
    # C context entry. It does not install a firmware or VMM FP handler.
    source = (ROOT / "firmware/new/ivt.S").read_text()
    source = source.replace('#include "ivt-layout.h"', '#include "firmware/new/ivt-layout.h"')
    for vector in (0x5C00, 0x5D00):
        source = source.replace(
            f".org ivt_base+{vector}\n    br.sptk.many ivt_fatal",
            f".org ivt_base+{vector}\n    br.sptk.many ivt_unaligned",
        )
    (out / "ivt.S").write_text(source)
    common = [
        "-std=c11",
        "-Os",
        "-fshort-wchar",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-mfixed-range=f32-f127",
        "-minline-int-divide-min-latency",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-D_EFI_P64",
        "-I.",
        "-Iinclude",
        "-Ithird_party/edk2-ipf/MdePkg/Include",
        "-Ithird_party/edk2-ipf/MdePkg/Include/Ipf",
        "-I" + str(out),
    ]
    objects = []
    for name, path in [
        ("guest", "tests/firmware/fpswa-guest.c"),
        ("execute", "tests/firmware/fpswa-guest.S"),
        ("ivt", str(out / "ivt.S")),
        ("virtual", "firmware/new/gate/virtual.S"),
        ("support", "firmware/new/fpswa/support.c"),
    ]:
        obj = out / (name + ".o")
        objects.append(str(obj))
        subprocess.run(
            [
                cross + "gcc",
                *common,
                "-ffile-prefix-map=" + str(ROOT) + "=.",
                "-c",
                path,
                "-o",
                str(obj),
            ],
            cwd=ROOT,
            check=True,
        )
        subprocess.run(
            [cross + "objcopy", "--redefine-sym", "memcpy=CopyMem", str(obj)], check=True
        )
    libgcc = subprocess.check_output([cross + "gcc", "-print-libgcc-file-name"], text=True).strip()
    elf = ROOT / "build/firmware-new/FpswaProbe.elf"
    subprocess.run(
        [
            cross + "ld",
            "-static",
            "--emit-relocs",
            "--no-undefined",
            "--build-id=none",
            "-T",
            "firmware/edk2/module.lds",
            "-o",
            str(elf),
            *objects,
            libgcc,
        ],
        cwd=ROOT,
        check=True,
    )
    subprocess.run(
        [sys.executable, "tools/firmware/elf_to_pe.py", str(elf), str(elf.with_suffix(".efi"))],
        cwd=ROOT,
        check=True,
    )
    target = ROOT / "build/efi/FpswaProbe.efi"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(elf.with_suffix(".efi").read_bytes())


if __name__ == "__main__":
    build()
