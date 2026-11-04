# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes engine/tier1/rules/fpfast.rules, the FP helper expansions
(docs/floating-point.md). The rules file is what gen.py emits and prove.py proves;
this script only spares writing the variants by hand.

    python3 -I engine/tier1/rules/fpfast_gen.py [--out FILE]

Registers: $s (R11) for integer work, x1 = f3 (b), x2 = f4 (c), x0 = f2 (a),
x3 a temporary. Operands are packed from the register format into the
format's IEEE image only when they are zeros or normals exactly
representable in it; results are unpacked only when normal above the min
normal, or zero where the zero is exact.
"""

import argparse
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FMT = {
    # width, fraction bits, register exponent of exponent 0, IEEE 1.0
    "s": (32, 23, 0xFF80, 0x3F800000),
    "d": (64, 52, 0xFC00, 0x3FF0000000000000),
}
SF_SHIFT = "(add 6 (shl #sf 3) (shl #sf 2) #sf)"  # 6 + 13 * sf


def guard(mask, want):
    return [
        "in $s, qp",
        "test64 $s, $s",
        "jz .skip",
        "in $s, fpsr",
        "test64 $s, 32",
        "jz $BAIL",  # traps.id clear: an inexact result traps
        "shr64 $s, %s" % SF_SHIFT,
        "and64 $s, 0x%x" % mask,
        "cmp64 $s, 0x%x" % want,
        "jne $BAIL",
    ]


def pack(role, xmm, neg, fmt):
    """xN = the IEEE image of FR role, sign flipped when neg."""
    width, frac, base, _ = FMT[fmt]
    ebits = width - frac - 1
    top = width - 1
    sign = (
        ["in $s, %s.meta" % role, "shr64 $s, 17"]
        + (["xor64 $s, 1"] if neg else [])
        + ["shl64 $s, %d" % top]
    )
    out = [
        "in $s, %s.meta" % role,
        "and64 $s, 0x1ffff",
        "sub64 $s, 0x%x" % (base + 1),
        "cmp64 $s, 0x%x" % ((1 << ebits) - 3),
        "ja .%sz" % role,
        "add64 $s, 1",
        "shl64 $s, %d" % frac,
        "movq x%d, $s" % xmm,
        "in $s, %s.sig" % role,
        "test64 $s, $s",
        "jns $BAIL",
    ]  # integer bit clear
    if fmt == "d":
        out += ["test64 $s, 0x7ff", "jnz $BAIL"]  # more than 53 bits
    else:
        out += ["shl64 $s, 24", "jnz $BAIL", "in $s, %s.sig" % role]  # more than 24 bits
    out += ["shl64 $s, 1", "shr64 $s, %d" % (64 - frac), "movq x3, $s", "por x%d, x3" % xmm]
    out += sign + ["movq x3, $s", "por x%d, x3" % xmm, "jmp .%sp" % role]
    out += [
        ".%sz:" % role,
        "in $s, %s.meta" % role,
        "and64 $s, 0x1ffff",
        "jnz $BAIL",
        "in $s, %s.sig" % role,
        "test64 $s, $s",
        "jnz $BAIL",
    ]  # +-0 only
    out += sign + ["movq x%d, $s" % xmm, ".%sp:" % role]
    return out


def const(xmm, value):
    return ["movabs64 $s, 0x%x" % value, "movq x%d, $s" % xmm]


def zero_exact(roles):
    """A zero result is exact when a factor is zero (else an underflow)."""
    if roles is None:
        return []
    out = []
    for i, r in enumerate(roles):
        last = i == len(roles) - 1
        out += ["in $s, %s.sig" % r, "test64 $s, $s", "jnz $BAIL" if last else "jz .rzok"]
    return out + ([".rzok:"] if len(roles) > 1 else [])


def result(fmt, zero_roles):
    width, frac, base, _ = FMT[fmt]
    ebits = width - frac - 1
    lead = 64 - width + 1  # shift that drops the bits above the exponent
    exp = ["movq $s, x1", "shl64 $s, %d" % lead, "shr64 $s, %d" % (64 - ebits)]
    sign = (
        ["movq $s, x1"]
        + (["shl64 $s, 32"] if fmt == "s" else [])
        + ["shr64 $s, 63", "shl64 $s, 17"]
    )
    out = (
        [
            "movq $s, x1",
            "shl64 $s, %d" % lead,
            "jz .rz",
            "shr64 $s, %d" % (64 - ebits),
            "sub64 $s, 1",
            "cmp64 $s, 0x%x" % ((1 << ebits) - 3),
            "ja $BAIL",  # denormal, infinity or NaN
            "test64 $s, $s",
            "jnz .rn",
            "movq $s, x1",
            "shl64 $s, %d" % (64 - frac),
            "jz $BAIL",  # the min normal
            ".rn:",
        ]
        + exp
        + ["add64 $s, 0x%x" % base, "movq x2, $s"]
        + sign
        + [
            "movq x0, $s",
            "por x2, x0",
            "out d.meta, x2",
            "psllq x1, %d" % (63 - frac),
            "pcmpeqd x3, x3",
            "psllq x3, 63",
            "por x1, x3",
            "out d.sig, x1",
            "out mf, #mf",
            "jmp $DONE",
            ".rz:",
        ]
        + zero_exact(zero_roles)
        + ["out d.sig, 0"]
        + sign
        + ["out d.meta, $s", "out mf, #mf", "jmp $DONE"]
    )
    return out


SKIP = [
    ".skip:",
    "in $s, d.sig",
    "out d.sig, $s",
    "in $s, d.meta",
    "out d.meta, $s",
    "out mf, 0",
    "jmp $DONE",
]


def scalar(name, insn, fmt, prec, addend, host, mode, mask, want):
    """One scalar rule (sem_ia64.fp_scalar names the operation)."""
    width, frac, base, one = FMT[fmt]
    sfx = "sd" if fmt == "d" else "ss"
    body = guard(mask, want)
    fused = insn in ("FMA", "FMS", "FNMA") and addend == "reg"
    if insn in ("FMA", "FMS", "FNMA"):
        body += pack("b", 1, insn == "FNMA", fmt) + pack("c", 2, False, fmt)
        if fused:
            body += pack("a", 0, insn == "FMS", fmt)
            body += (
                ["vfmadd213%s x1, x2, x0" % sfx]
                if host == "fma3"
                else ["mul%s x1, x2" % sfx, "add%s x1, x0" % sfx]
            )
        else:
            body += ["mul%s x1, x2" % sfx]
        zero = ("b", "c")
    elif insn == "FMPY":
        body += pack("a", 1, False, fmt) + pack("b", 2, False, fmt) + ["mul%s x1, x2" % sfx]
        zero = ("a", "b")
    elif insn == "FADD":
        body += pack("b", 1, False, fmt) + pack("a", 0, False, fmt) + ["add%s x1, x0" % sfx]
        zero = None
    elif insn == "FSUB":
        body += pack("a", 1, False, fmt) + pack("b", 0, True, fmt) + ["add%s x1, x0" % sfx]
        zero = None
    else:  # FNORM
        body += pack("b", 1, False, fmt) + const(2, one) + ["mul%s x1, x2" % sfx]
        zero = ("b",)
    body += result(fmt, zero) + SKIP
    return rule(name, insn, prec, addend, host, mode, body)


def packed(name, insn, addend, host):
    """fpma, fpms, fpnma on both single lanes."""
    roles = [("b", 1, insn == "FPNMA"), ("c", 2, False)] + (
        [("a", 0, insn == "FPMS")] if addend == "reg" else []
    )
    body = guard(0x1030, 0x1000)
    for role, xmm, neg in roles:
        body += [
            "in $s, %s.meta" % role,
            "cmp64 $s, 0x1fffe",
            "je $BAIL",  # NaTVal
            "in $s, %s.sig" % role,
            "shl64 $s, 33",
            "shr64 $s, 56",
            "cmp64 $s, 0xff",
            "je $BAIL",
            "test64 $s, $s",
            "jnz .%sl0" % role,
            "in $s, %s.sig" % role,
            "shl64 $s, 41",
            "jnz $BAIL",  # denormal lane 0
            ".%sl0:" % role,
            "in $s, %s.sig" % role,
            "shl64 $s, 1",
            "shr64 $s, 56",
            "cmp64 $s, 0xff",
            "je $BAIL",
            "test64 $s, $s",
            "jnz .%sl1" % role,
            "in $s, %s.sig" % role,
            "shl64 $s, 9",
            "shr64 $s, 41",
            "jnz $BAIL",  # denormal lane 1
            ".%sl1:" % role,
            "in $s, %s.sig" % role,
            "movq x%d, $s" % xmm,
        ]
        if neg:
            body += ["movabs64 $s, 0x8000000080000000", "movq x3, $s", "pxor x%d, x3" % xmm]
    body += ["vfmadd213ps x1, x2, x0"] if addend == "reg" else ["mulps x1, x2"]
    for lane, (lead, fshift) in enumerate(((33, 41), (1, 9))):
        frac = ["movq $s, x1", "shl64 $s, %d" % fshift] + (["shr64 $s, 41"] if lane else [])
        factor = []
        for i, r in enumerate(("b", "c")):
            factor += (
                ["in $s, %s.sig" % r, "shl64 $s, %d" % lead]
                + (["shr64 $s, 33"] if lane else [])
                + (["jnz $BAIL"] if i else ["jz .r%d" % lane])
            )
        body += (
            [
                "movq $s, x1",
                "shl64 $s, %d" % lead,
                "shr64 $s, 56",
                "cmp64 $s, 0xff",
                "je $BAIL",
                "cmp64 $s, 1",
                "ja .r%d" % lane,
                "je .r%dm" % lane,
            ]
            + frac
            + ["jnz $BAIL"]
            + factor
            + ["jmp .r%d" % lane, ".r%dm:" % lane]
            + frac
            + ["jz $BAIL", ".r%d:" % lane]
        )
    body += ["out d.sig, x1", "out d.meta, 0x1003e", "out mf, #mf", "jmp $DONE"] + SKIP
    return rule(name, insn, "s", addend, host, "exact", body)


def rule(name, insn, prec, addend, host, mode, body):
    lines = [
        "expand %s" % name,
        "  insn      %s" % insn,
        "  precision %s" % prec,
        "  addend    %s" % addend,
        "  host      %s" % host,
        "  mode      %s" % mode,
    ]
    for i, b in enumerate(body):
        lines.append(("  emit      " if i == 0 else "            ") + b)
    return "\n".join(lines + ["end"])


def rules():
    out = []
    exact = (0x1032, 0x1000)  # rc nearest, i set, wre clear
    approx = (0x1030, 0x1000)  # rc nearest, i set
    double_pc = (0x103C, 0x1008)  # pc 10 only (precision none)
    for fmt, prec in (("s", "s"), ("d", "d")):
        for insn in ("FMA", "FMS", "FNMA"):
            out.append(
                scalar(
                    "%s_%s_fused" % (insn.lower(), fmt),
                    insn,
                    fmt,
                    prec,
                    "reg",
                    "fma3",
                    "exact",
                    *exact,
                )
            )
            out.append(
                scalar(
                    "%s_%s_f0" % (insn.lower(), fmt), insn, fmt, prec, "f0", "base", "exact", *exact
                )
            )
        for insn in ("FMPY", "FADD", "FSUB", "FNORM"):
            out.append(
                scalar(
                    "%s_%s" % (insn.lower(), fmt), insn, fmt, prec, "any", "base", "exact", *exact
                )
            )
    for insn in ("FPMA", "FPMS", "FPNMA"):
        out.append(packed("%s_fused" % insn.lower(), insn, "reg", "fma3"))
        out.append(packed("%s_f0" % insn.lower(), insn, "f0", "base"))
    # fp_approx: fused forms without FMA3 as two roundings; pc 10 on double.
    for fmt, prec in (("s", "s"), ("d", "d")):
        for insn in ("FMA", "FMS", "FNMA"):
            out.append(
                scalar(
                    "%s_%s_approx" % (insn.lower(), fmt),
                    insn,
                    fmt,
                    prec,
                    "reg",
                    "base",
                    "approx",
                    *approx,
                )
            )
    for insn in ("FMA", "FMS", "FNMA"):
        out.append(
            scalar(
                "%s_x_fused" % insn.lower(), insn, "d", "none", "reg", "fma3", "approx", *double_pc
            )
        )
        out.append(
            scalar(
                "%s_x_approx" % insn.lower(), insn, "d", "none", "reg", "base", "approx", *double_pc
            )
        )
        out.append(
            scalar("%s_x_f0" % insn.lower(), insn, "d", "none", "f0", "base", "approx", *double_pc)
        )
    for insn in ("FMPY", "FADD", "FSUB", "FNORM"):
        out.append(
            scalar("%s_x" % insn.lower(), insn, "d", "none", "any", "base", "approx", *double_pc)
        )
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "fpfast.rules"))
    args = ap.parse_args()
    head = [
        "# SPDX-License-Identifier: GPL-2.0-or-later",
        '# FP helper expansions (docs/floating-point.md; rulelib.py "expand"), written by',
        "# fpfast_gen.py. Rules are tried in order; the first whose insn,",
        "# precision, addend and host match and whose mode is enabled expands.",
        "",
    ]
    with open(args.out, "w") as f:
        f.write("\n".join(head) + "\n" + "\n\n".join(rules()) + "\n")


if __name__ == "__main__":
    main()
