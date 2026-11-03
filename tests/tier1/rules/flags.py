# SPDX-License-Identifier: GPL-2.0-or-later
"""AMD64 status-flag model for the rule prover.

Per instruction class: the flags written with defined values (as z3
expressions) and the flags left architecturally undefined, which are fresh
unconstrained variables ("havoc"). AF is havoc after
every flag-writing instruction (no rule may depend on it). Authority: Intel
SDM Vol.2 instruction pages and Vol.1 Appendix A (EFLAGS cross-reference).

cross_check() compares the written/undefined sets below with the flag
columns of engine/tier1/x86enc.h's T1_X86_OPS, so the allocator's clobber
sets and the prover's model cannot drift apart.
"""

import itertools
import z3

FLAGS = ("CF", "PF", "AF", "ZF", "SF", "OF")
_fresh = itertools.count()


def havoc(name):
    return z3.Bool("havoc_%s_%d" % (name, next(_fresh)))


def parity_even(byte):
    """PF: 1 when the low byte has an even number of set bits."""
    bits = [z3.Extract(i, i, byte) for i in range(8)]
    x = bits[0]
    for b in bits[1:]:
        x = x ^ b
    return x == 0


def szp(result, width):
    return {
        "SF": z3.Extract(width - 1, width - 1, result) == 1,
        "ZF": result == 0,
        "PF": parity_even(z3.Extract(7, 0, result)),
    }


def msb(x, width):
    return z3.Extract(width - 1, width - 1, x) == 1


def add(a, b, r, width):
    f = szp(r, width)
    f["CF"] = z3.ULT(r, a)
    f["OF"] = z3.And(msb(a, width) == msb(b, width), msb(r, width) != msb(a, width))
    f["AF"] = havoc("AF")
    return f


def sub(a, b, r, width):
    """sub and cmp: r = a - b."""
    f = szp(r, width)
    f["CF"] = z3.ULT(a, b)
    f["OF"] = z3.And(msb(a, width) != msb(b, width), msb(r, width) != msb(a, width))
    f["AF"] = havoc("AF")
    return f


def logic(r, width):
    """and, or, xor, test: CF = OF = 0."""
    f = szp(r, width)
    f["CF"] = z3.BoolVal(False)
    f["OF"] = z3.BoolVal(False)
    f["AF"] = havoc("AF")
    return f


def inc(a, r, width, old):
    f = szp(r, width)
    f["CF"] = old["CF"]  # inc/dec leave CF unchanged
    f["OF"] = z3.And(z3.Not(msb(a, width)), msb(r, width))
    f["AF"] = havoc("AF")
    return f


def neg(a, r, width):
    f = szp(r, width)
    f["CF"] = a != 0
    f["OF"] = a == z3.BitVecVal(1 << (width - 1), width)
    f["AF"] = havoc("AF")
    return f


def shift(kind, a, count, r, width, old):
    """shl/shr/sar by a masked count (0..63 for 64-bit operands).

    count 0: every flag unchanged. Otherwise CF = last bit shifted out, SF,
    ZF, PF from the result, AF undefined, OF defined only for count 1
    (shl: MSB(r) xor CF; shr: MSB(a); sar: 0) and undefined otherwise."""
    one = z3.BitVecVal(1, width)
    if kind == "shl":
        cf = z3.Extract(0, 0, z3.LShR(a, z3.BitVecVal(width, width) - count)) == 1
        of1 = z3.Xor(msb(r, width), cf)
    elif kind == "shr":
        cf = z3.Extract(0, 0, z3.LShR(a, count - one)) == 1
        of1 = msb(a, width)
    else:
        cf = z3.Extract(0, 0, a >> (count - one)) == 1
        of1 = z3.BoolVal(False)
    new = szp(r, width)
    new["CF"] = cf
    new["OF"] = z3.If(count == one, of1, havoc("OF"))
    new["AF"] = havoc("AF")
    zero = count == 0
    return {f: z3.If(zero, old[f], new[f]) for f in FLAGS}


def zero_idiom():
    """xor r32, r32 of one register."""
    return {
        "CF": z3.BoolVal(False),
        "OF": z3.BoolVal(False),
        "SF": z3.BoolVal(False),
        "ZF": z3.BoolVal(True),
        "PF": z3.BoolVal(True),
        "AF": havoc("AF"),
    }


def condition(cc, f):
    """Jcc/SETcc/CMOVcc condition (SDM Vol.1 Table B-1)."""
    return {
        "o": f["OF"],
        "no": z3.Not(f["OF"]),
        "b": f["CF"],
        "ae": z3.Not(f["CF"]),
        "e": f["ZF"],
        "ne": z3.Not(f["ZF"]),
        "be": z3.Or(f["CF"], f["ZF"]),
        "a": z3.And(z3.Not(f["CF"]), z3.Not(f["ZF"])),
        "s": f["SF"],
        "ns": z3.Not(f["SF"]),
        "p": f["PF"],
        "np": z3.Not(f["PF"]),
        "l": f["SF"] != f["OF"],
        "ge": f["SF"] == f["OF"],
        "le": z3.Or(f["ZF"], f["SF"] != f["OF"]),
        "g": z3.And(z3.Not(f["ZF"]), f["SF"] == f["OF"]),
    }[cc]


# Written (defined) and undefined flags per x86enc.h op class, as this
# model implements them; cross-checked against the header.
DEF_ALL = {"CF", "PF", "ZF", "SF", "OF"}
TABLE = {
    "arith": (DEF_ALL, {"AF"}),  # add sub cmp and or xor test neg
    "inc": ({"PF", "ZF", "SF", "OF"}, {"AF"}),
    "shift": ({"CF", "PF", "ZF", "SF"}, {"OF", "AF"}),
    "zero": (DEF_ALL, {"AF"}),
    "mul": ({"CF", "OF"}, {"SF", "ZF", "AF", "PF"}),
    "bmi1": ({"SF", "ZF", "CF", "OF"}, {"PF", "AF"}),
    "bextr": ({"ZF", "CF", "OF"}, {"SF", "PF", "AF"}),
    "lzcnt": ({"CF", "ZF"}, {"SF", "OF", "PF", "AF"}),
    "none": (set(), set()),
}
CLASS_OF = {
    "andn64": "bmi1",
    "blsi64": "bmi1",
    "blsmsk64": "bmi1",
    "blsr64": "bmi1",
    "bextr64": "bextr",
    "lzcnt64": "lzcnt",
    "add64": "arith",
    "sub64": "arith",
    "and64": "arith",
    "or64": "arith",
    "or32": "arith",
    "xor64": "arith",
    "neg64": "arith",
    "cmp64": "arith",
    "cmp32": "arith",
    "test64": "arith",
    "test32": "arith",
    "inc64": "inc",
    "dec64": "inc",
    "shl64": "shift",
    "shr64": "shift",
    "sar64": "shift",
    "xor32": "zero",
    "imul64": "mul",
    "mulpair64": "mul",
    "imulpair64": "mul",
    "adc64": "arith",
}


def cross_check(x86_ops, flag_bits):
    """Returns a list of mismatches between TABLE and x86enc.h."""
    problems = []
    names = {v: k for k, v in flag_bits.items()}

    def to_set(mask):
        return {names[b] for b in names if mask & b}

    for op in x86_ops:
        if op.mnemonic in ("call", "popfq"):
            continue  # never in rules; whole-RFLAGS effects
        cls = CLASS_OF.get(op.mnemonic, "none")
        want_def, want_undef = TABLE[cls]
        if to_set(op.fdef) != want_def or to_set(op.fundef) != want_undef:
            problems.append(
                "%s (%s): x86enc.h def %s undef %s, prover %s / %s"
                % (
                    op.mnemonic,
                    op.enum,
                    sorted(to_set(op.fdef)),
                    sorted(to_set(op.fundef)),
                    sorted(want_def),
                    sorted(want_undef),
                )
            )
        if cls == "shift" and "T1_XA_COUNT0_KEEPS_FLAGS" not in op.attrs:
            problems.append("%s: shifts keep flags for count 0" % op.mnemonic)
    return problems
