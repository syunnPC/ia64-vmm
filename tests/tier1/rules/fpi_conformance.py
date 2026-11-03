# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual lift.c IR, evaluated with sem_ia64, versus decoded ia64_execute.

Run with $T1_Z3_VENV/bin/python -I tests/tier1/rules/fpi_conformance.py
--runner build/tier1-rule-runner --image build/tests/tier1/frag/frag_fpi.bin.
"""

import argparse
import importlib.util
import json
import pathlib
import random
import struct
import subprocess
import z3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runner", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--lifter", default="build/tier1-test")
    ap.add_argument("--cases", type=int, default=100000)
    a = ap.parse_args()
    here = pathlib.Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location("conformance", here / "conformance.py")
    C = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(C)
    S = C.S
    # The C runner forms and lifts one decoded instruction at a time, before
    # optimization/entry guards. Compose the exported SSA graph and frame,
    # so a wrong operand, extraction width or shift in lift.c fails here.
    p = subprocess.run([a.lifter, "--suite=fpi-ir", a.image], capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f"lifter export failed ({p.returncode}): {p.stderr}")
    graphs = json.loads(p.stdout)
    elements = [41, 40, 169, 168, 262, 451, 333, 462, 334, 463, 335, 464, 336, 465]
    inputs = [z3.BitVec(f"e{e}", 64) for e in elements]
    live = dict(zip(elements, inputs))
    exprs, fn = [], []
    for graph in graphs:
        memo = {}

        def value(v):
            assert v % 4 == 0, ("unexpected multi-result FPI op", v)
            o = v // 4
            if o not in memo:
                name, imm, uses = graph["ops"][o]
                if name == "LIVEIN":
                    memo[o] = live[imm]
                elif name == "CONST":
                    memo[o] = S.bv(imm)
                else:
                    args = [value(u) for u in uses]
                    memo[o] = S.evaluate(name, imm, args)[0]
            return memo[o]

        outputs = [
            value(graph["frame"][str(e)]) if str(e) in graph["frame"] else live[e]
            for e in (40, 168, 333, 462, 451)
        ]
        exprs.append(outputs)
        fn.append(C.compile_fn(inputs, outputs))
    names = [
        "SETF_SIG",
        "SETF_EXP",
        "GETF_SIG",
        "GETF_EXP",
        "XMA_L",
        "XMA_H",
        "XMA_HU",
        "XMPY_HU",
        "FAND",
        "FOR",
        "FXOR",
        "FANDCM",
        "FMIX_LR",
        "FMIX_L",
        "FMIX_R",
        "FSXT_L",
        "FSXT_R",
    ]
    blob = pathlib.Path(a.image).read_bytes()
    rng = random.Random("FPI conformance")
    for op, name in enumerate(names):
        lo, hi = struct.unpack_from("<QQ", blob, op * 512)
        lines, expected = [], []
        for sample in range(a.cases):
            g, oldg = C.value(rng), C.value(rng)
            ng, oldn = bool(sample & 2), bool(sample & 4)
            qp = bool(sample & 1)
            pr = 1 | (qp << 6)
            psr = orig_psr = rng.choice([0, 16, 32, 48, 1 << 18, 1 << 19])
            fpsr = rng.getrandbits(64)
            fr = [(C.value(rng), rng.randrange(1 << 18)) for _ in range(4)]
            for k in range(4):
                if sample & (1 << (k + 3)):
                    fr[k] = (0, 0x1FFFE)
            sig, meta = fr[0]
            outg, outn = oldg, oldn
            disabled = qp and bool(psr & (1 << 18))
            vals = [g, oldg, int(ng), int(oldn), int(qp), orig_psr & 48]
            vals.extend(x for pair in fr for x in pair)
            if sample < 24:
                assert C.z3_eval(inputs, exprs[op], vals) == fn[op](vals)
            if not disabled:
                outg, outn, sig, meta, mf = fn[op](vals)
                psr = (orig_psr & ~48) | mf
            expected.append(
                ("fault" if disabled else "complete", name, outg, int(outn), sig, meta, psr, fpsr)
            )
            fields = [lo, hi, 0 if op < 4 else 1, pr, g, oldg, int(ng), int(oldn), orig_psr, fpsr]
            fields.extend(x for pair in fr for x in pair)
            fields.append(0)
            lines.append(" ".join(format(x, "x") for x in fields))
        p = subprocess.run(
            [a.runner, "fpi"],
            input="\n".join(lines) + "\n",
            capture_output=True,
            text=True,
            check=True,
        )
        actual = p.stdout.splitlines()
        assert len(actual) == a.cases
        for n, (line, exp) in enumerate(zip(actual, expected)):
            words = line.split()
            got = (words[0], words[1]) + tuple(int(x, 16) for x in words[2:])
            assert got == exp, (name, n, got, exp)
        print(f"fpi-conformance: {name} {a.cases} cases passed", flush=True)


if __name__ == "__main__":
    main()
