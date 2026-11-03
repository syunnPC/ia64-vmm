# SPDX-License-Identifier: GPL-2.0-or-later
"""Symbolic execution of a rule template on AMD64.

Registers are 64-bit z3 bit-vectors; 32-bit writes zero-extend, 8- and
16-bit writes preserve the other bits; shift counts are masked to 6 bits
for 64-bit operands (5 for 32-bit); lea computes base + index * scale +
disp without touching flags; jcc/cmovcc/setcc read flags through
flags.condition. Flags come from flags.py (undefined flags are havoc).
Control flow: a jcc forks the path; a path ends at a label (jmp, taken jcc)
or by falling off the end of the template.

For rule templates, run() follows t1_mir_effects: a write-only register
operand narrower than 32 bits gets fresh upper bits at every definition.
Allocation may change its physical register without carrying an earlier
definition across the lifetime hole. Read-write operands retain their
upper bits. step() alone models a physical instruction for native
conformance tests. Tied binders share a logical register (rules.h).
Memory: LOAD/STORE rules (rules.h format 2) make one
access to [$addr + $window]. The state holds the single memory word the
rule may touch, at the host address the window invariant gives it
(mem_addr = addr + delta, host_access = gva + delta);
every access adds the obligation that its address is exactly that one.
The window mapping itself is the invariant hostwin maintains and is tested,
not proved.
"""

import z3


class RuleSemanticsError(Exception):
    pass


class State:
    def __init__(self, regs, flags, mem_addr=None, mem_word=None, obligations=()):
        self.regs = dict(regs)  # register name -> BitVec 64
        self.flags = dict(flags)  # 'CF'.. -> Bool
        self.mem_addr = mem_addr  # host address of the one memory word, or None
        self.mem_word = mem_word  # its 64-bit little-endian image
        self.obligations = list(obligations)

    def copy(self):
        return State(self.regs, self.flags, self.mem_addr, self.mem_word, self.obligations)


def run(rule, regs, flags, alias, imm_value, flagmod, mem_addr=None, mem_word=None):
    """Executes rule.emit. regs: initial register values by register name;
    alias: binder -> register name; imm_value(binder, int or expression) ->
    BitVec 64; mem_addr/mem_word: the memory word of a LOAD/STORE rule.
    Returns a list of (path condition, exit label or None, State)."""
    paths = []
    work = [(z3.BoolVal(True), 0, State(regs, flags, mem_addr, mem_word))]
    while work:
        cond, pc, st = work.pop()
        while True:
            if pc == len(rule.emit):
                paths.append((cond, None, st))
                break
            insn = rule.emit[pc]
            # Match regalloc.c's operand-spec uses/defs. A pure byte (or
            # word) def does not read the binder's previous full value.
            for (kind, binder), spec in zip(insn.operands, insn.op.specs):
                if kind == "reg" and spec.startswith("R") and spec.endswith("_W"):
                    width = width_of(spec)
                    if width < 32:
                        reg = alias[binder]
                        upper = z3.FreshConst(z3.BitVecSort(64 - width), "upper")
                        st.regs[reg] = z3.Concat(upper, z3.Extract(width - 1, 0, st.regs[reg]))
            out = step(insn, st, alias, imm_value, flagmod)
            if out is None:
                pc += 1
                continue
            kind, label, taken = out
            if kind == "jmp":
                paths.append((cond, label, st))
                break
            # jcc: the taken side exits, the other continues.
            paths.append((z3.And(cond, taken), label, st.copy()))
            cond = z3.And(cond, z3.Not(taken))
            pc += 1
    return paths


def width_of(spec):
    return {"R64": 64, "R32": 32, "R16": 16, "R8": 8, "M64": 64, "M32": 32, "M16": 16, "M8": 8}[
        spec.split("_")[0]
    ]


def read(st, alias, binder, width):
    v = st.regs[alias[binder]]
    return v if width == 64 else z3.Extract(width - 1, 0, v)


def write(st, alias, binder, value, width):
    reg = alias[binder]
    if width == 64:
        st.regs[reg] = value
    elif width == 32:
        st.regs[reg] = z3.ZeroExt(32, value)
    else:
        st.regs[reg] = z3.Concat(z3.Extract(63, width, st.regs[reg]), value)


def address(mem, st, alias, imm_value):
    a = z3.BitVecVal(mem.disp & (2**64 - 1), 64)
    if getattr(mem, "disp_expr", None) is not None:
        a = a + imm_value(mem.disp_expr)
    if mem.base is not None:
        a = a + st.regs[alias[mem.base]]
    if mem.index is not None:
        if isinstance(mem.scale, str):
            shift = imm_value(mem.scale)
            a = a + (st.regs[alias[mem.index]] << shift)
        else:
            a = a + st.regs[alias[mem.index]] * mem.scale
    return a


def step(insn, st, alias, imm_value, flagmod):
    """Executes one line; returns None, or ('jmp'|'jcc', label, condition)."""
    op, ops = insn.op, insn.operands
    m = op.mnemonic
    specs = op.specs

    def src(i):
        kind, v = ops[i]
        w = width_of(specs[i]) if specs[i][0] == "R" else 64
        if kind in ("reg", "cl"):
            if kind == "cl":
                return z3.Extract(7, 0, st.regs[alias[v]])
            return read(st, alias, v, w)
        if kind == "imm":
            # The encoded immediate, extended to the operation width as the
            # hardware does (x86enc.h operand specs): I8 is a shift count
            # (masked below), I32 is sign-extended to 64 bits, I64 is full.
            val = imm_value(v)
            opw = width_of(specs[0])
            if specs[i] == "I8":
                return z3.ZeroExt(opw - 8, z3.Extract(7, 0, val))
            if specs[i] == "I32":
                v32 = z3.Extract(31, 0, val)
                return v32 if opw == 32 else z3.SignExt(opw - 32, v32)
            return val
        raise RuleSemanticsError("%s: operand %d of %s" % (insn.line, i, m))

    def dst_width():
        return width_of(specs[0])

    if op.cc is not None:
        c = flagmod.condition(op.cc, st.flags)
        if m.startswith("j"):
            return ("jcc", ops[0][1], c)
        if m.startswith("set"):
            write(st, alias, ops[0][1], z3.If(c, z3.BitVecVal(1, 8), z3.BitVecVal(0, 8)), 8)
            return None
        # cmov<cc>64 r64, r64: a 64-bit cmov writes its destination either way.
        write(st, alias, ops[0][1], z3.If(c, src(1), src(0)), 64)
        return None
    if m == "jmp" and ops[0][0] == "label":
        return ("jmp", ops[0][1], None)
    if m in ("mov64", "mov32", "movabs64"):
        write(st, alias, ops[0][1], src(1), dst_width())
        return None
    if m in ("movzx32_8", "movzx32_16"):
        write(st, alias, ops[0][1], z3.ZeroExt(32 - src(1).size(), src(1)), 32)
        return None
    if m in ("movsx64_8", "movsx64_16", "movsx64_32"):
        write(st, alias, ops[0][1], z3.SignExt(64 - src(1).size(), src(1)), 64)
        return None
    if m == "lea64":
        write(st, alias, ops[0][1], address(ops[1][1], st, alias, imm_value), 64)
        return None
    if m in ("load64", "load32", "load16zx", "load8zx", "store64", "store32", "store16", "store8"):
        load = m.startswith("load")
        mem = ops[1][1] if load else ops[0][1]
        if st.mem_addr is None:
            raise RuleSemanticsError("%s: %s outside a LOAD/STORE rule" % (insn.line, m))
        st.obligations.append(address(mem, st, alias, imm_value) == st.mem_addr)
        w = width_of(specs[1] if load else specs[0])
        if load:
            v = z3.Extract(w - 1, 0, st.mem_word)
            # 64- and 32-bit loads write the register whole (a 32-bit write
            # zero-extends); load16zx/load8zx are movzx to a 32-bit register.
            write(st, alias, ops[0][1], z3.ZeroExt(32 - w, v) if w < 32 else v, max(w, 32))
        else:
            if ops[1][0] != "reg":
                raise RuleSemanticsError("%s: %s stores only registers in rules" % (insn.line, m))
            v = src(1)
            st.mem_word = v if w == 64 else z3.Concat(z3.Extract(63, w, st.mem_word), v)
        return None
    if m == "xor32" and len(ops) == 1:
        write(st, alias, ops[0][1], z3.BitVecVal(0, 32), 32)
        st.flags = flagmod.zero_idiom()
        return None
    # Intel SDM Vol.2 scalar BMI/LZCNT operations. PF/AF are undefined
    # for BMI1 (unlike AND), and shifts/rotates preserve every flag.
    if m in ("shlx64", "shrx64", "sarx64", "rorx64"):
        a, count = src(0), src(1) & 63
        r = (
            a << count
            if m == "shlx64"
            else z3.LShR(a, count)
            if m == "shrx64"
            else a >> count
            if m == "sarx64"
            else z3.RotateRight(a, count)
        )
        write(st, alias, ops[0][1], r, 64)
        return None
    if m in ("andn64", "bextr64", "blsi64", "blsmsk64", "blsr64", "lzcnt64"):
        b = src(1)
        if m == "andn64":
            r = ~src(0) & b
        elif m == "bextr64":
            start, length = b & 255, z3.LShR(b, 8) & 255
            r = z3.LShR(src(0), start) & ((z3.BitVecVal(1, 64) << length) - 1)
        elif m == "lzcnt64":
            r = z3.BitVecVal(64, 64)
            for k in range(64):
                r = z3.If(z3.Extract(k, k, b) == 1, z3.BitVecVal(63 - k, 64), r)
        else:
            r = -b & b if m == "blsi64" else (b - 1) ^ b if m == "blsmsk64" else (b - 1) & b
        fs = {f: flagmod.havoc(f) for f in flagmod.FLAGS}
        fs["ZF"] = r == 0
        fs["CF"] = (
            b == 0
            if m in ("blsr64", "blsmsk64", "lzcnt64")
            else b != 0
            if m == "blsi64"
            else z3.BoolVal(False)
        )
        if m != "lzcnt64":
            fs["OF"] = z3.BoolVal(False)
        if m not in ("lzcnt64", "bextr64"):
            fs["SF"] = flagmod.msb(r, 64)
        st.flags = fs
        write(st, alias, ops[0][1], r, 64)
        return None
    if m in ("not64", "neg64", "inc64"):
        a = src(0)
        r = ~a if m == "not64" else -a if m == "neg64" else a + 1
        if m == "neg64":
            st.flags = flagmod.neg(a, r, 64)
        elif m == "inc64":
            st.flags = flagmod.inc(a, r, 64, st.flags)
        write(st, alias, ops[0][1], r, 64)
        return None
    if m in ("mulpair64", "imulpair64"):
        a, b = src(0), src(1)
        extend = z3.ZeroExt if m == "mulpair64" else z3.SignExt
        wide = extend(64, a) * extend(64, b)
        lo, hi = z3.Extract(63, 0, wide), z3.Extract(127, 64, wide)
        overflow = hi != 0 if m == "mulpair64" else wide != z3.SignExt(64, lo)
        write(st, alias, ops[0][1], lo, 64)
        write(st, alias, ops[1][1], hi, 64)
        st.flags = {f: overflow if f in ("CF", "OF") else flagmod.havoc(f) for f in flagmod.FLAGS}
        return None
    if m == "adc64":
        a, b = src(0), src(1)
        carry = z3.If(st.flags["CF"], z3.BitVecVal(1, 65), z3.BitVecVal(0, 65))
        wide = z3.ZeroExt(1, a) + z3.ZeroExt(1, b) + carry
        r = z3.Extract(63, 0, wide)
        fs = flagmod.add(a, b, r, 64)
        fs["CF"] = z3.Extract(64, 64, wide) == 1
        fs["OF"] = z3.Extract(64, 64, z3.SignExt(1, a) + z3.SignExt(1, b) + carry) != z3.Extract(
            63, 63, r
        )
        st.flags = fs
        write(st, alias, ops[0][1], r, 64)
        return None
    if m == "imul64":
        a, b = src(0), src(1)
        wide = z3.SignExt(64, a) * z3.SignExt(64, b)
        r = z3.Extract(63, 0, wide)
        overflow = wide != z3.SignExt(64, r)
        st.flags = {f: overflow if f in ("CF", "OF") else flagmod.havoc(f) for f in flagmod.FLAGS}
        write(st, alias, ops[0][1], r, 64)
        return None
    base = m.rstrip("0123456789")
    if base in ("add", "sub", "and", "or", "xor", "cmp", "test"):
        w = dst_width()
        a, b = src(0), src(1)
        if b.size() != w:
            b = z3.SignExt(w - b.size(), b) if b.size() < w else z3.Extract(w - 1, 0, b)
        r = {
            "add": a + b,
            "sub": a - b,
            "cmp": a - b,
            "and": a & b,
            "test": a & b,
            "or": a | b,
            "xor": a ^ b,
        }[base]
        if base == "add":
            st.flags = flagmod.add(a, b, r, w)
        elif base in ("sub", "cmp"):
            st.flags = flagmod.sub(a, b, r, w)
        else:
            st.flags = flagmod.logic(r, w)
        if base not in ("cmp", "test"):
            write(st, alias, ops[0][1], r, w)
        return None
    if base in ("shl", "shr", "sar"):
        w = dst_width()
        a = src(0)
        c = src(1)
        count = z3.ZeroExt(w - c.size(), c) if c.size() < w else c
        count = count & z3.BitVecVal(63 if w == 64 else 31, w)
        r = {"shl": a << count, "shr": z3.LShR(a, count), "sar": a >> count}[base]
        st.flags = flagmod.shift(base, a, count, r, w, st.flags)
        write(st, alias, ops[0][1], r, w)
        return None
    raise RuleSemanticsError("%s: %s is not modelled for rule templates" % (insn.line, m))


# ---------------------------------------------------------------- FP expansions


class ExpandState(State):
    """R11 ($s), the low 64 bits of xmm0-xmm3 and RFLAGS, the helper
    results written (slot -> 64-bit value)."""

    def __init__(self, regs, flags, xmm, outs):
        super().__init__(regs, flags)
        self.xmm = list(xmm)
        self.outs = dict(outs)

    def copy(self):
        return ExpandState(self.regs, self.flags, self.xmm, self.outs)


def _lanes32(x):
    return [z3.Extract(31, 0, x), z3.Extract(63, 32, x)]


def step_sse(insn, st, imm_value, fp_op):
    """One SSE op of x86sse.h on the low 64 bits of xmm0-xmm3 (Intel SDM
    Vol.2): no op reads bits 127:64 into bits 63:0, so the upper halves are
    not modelled. MOVQ to xmm zero-extends; the scalar ops keep the other
    bits of the low half; the packed single ops compute lanes 0 and 1. The
    IEEE operations are fp_op(name): sem_ia64's native FP expressions,
    or their conservative abstraction during the proof (round to nearest,
    all exceptions masked, FTZ and DAZ clear: the MXCSR the VMM keeps)."""
    name = insn.op.enum[len("T1_SSE_") :]
    ops = insn.operands
    x = st.xmm
    if name == "MOVQ_XR":
        x[ops[0][1]] = st.regs["r11"]
    elif name == "MOVQ_RX":
        st.regs["r11"] = x[ops[1][1]]
    elif name in ("POR", "PXOR"):
        a, b = ops[0][1], ops[1][1]
        x[a] = x[a] | x[b] if name == "POR" else x[a] ^ x[b]
    elif name == "PCMPEQD":
        a, b = ops[0][1], ops[1][1]
        la, lb = _lanes32(x[a]), _lanes32(x[b])
        eq = [
            z3.If(la[k] == lb[k], z3.BitVecVal(0xFFFFFFFF, 32), z3.BitVecVal(0, 32))
            for k in range(2)
        ]
        x[a] = z3.Concat(eq[1], eq[0])
    elif name == "PSLLQ":
        x[ops[0][1]] = x[ops[0][1]] << imm_value(ops[1][1])
    elif name in ("ADDSD", "MULSD"):
        a, b = ops[0][1], ops[1][1]
        x[a] = fp_op(name[:3].lower() + "_d")(x[a], x[b])
    elif name in ("ADDSS", "MULSS"):
        a, b = ops[0][1], ops[1][1]
        r = fp_op(name[:3].lower() + "_s")(z3.Extract(31, 0, x[a]), z3.Extract(31, 0, x[b]))
        x[a] = z3.Concat(z3.Extract(63, 32, x[a]), r)
    elif name == "MULPS":
        a, b = ops[0][1], ops[1][1]
        f = fp_op("mul_s")
        la, lb = _lanes32(x[a]), _lanes32(x[b])
        x[a] = z3.Concat(f(la[1], lb[1]), f(la[0], lb[0]))
    elif name in ("FMADD213SD", "FMADD213SS", "FMADD213PS"):
        # vfmadd213 dst, src2, src3: dst = dst * src2 + src3, one rounding.
        a, b, c = ops[0][1], ops[1][1], ops[2][1]
        if name.endswith("SD"):
            x[a] = fp_op("fma_d")(x[a], x[b], x[c])
        elif name.endswith("SS"):
            r = fp_op("fma_s")(
                z3.Extract(31, 0, x[a]), z3.Extract(31, 0, x[b]), z3.Extract(31, 0, x[c])
            )
            x[a] = z3.Concat(z3.Extract(63, 32, x[a]), r)
        else:
            la, lb, lc = _lanes32(x[a]), _lanes32(x[b]), _lanes32(x[c])
            f = fp_op("fma_s")
            x[a] = z3.Concat(f(la[1], lb[1], lc[1]), f(la[0], lb[0], lc[0]))
    else:
        raise RuleSemanticsError("%s: %s is not modelled" % (insn.line, insn.mnemonic))


def run_expand(rule, inputs, imm_value, fp_op, flagmod):
    """Executes an expansion (rulelib "expand"). inputs: input name ->
    BitVec 64. Returns (path condition, exit '$BAIL' | '$DONE', state) for
    every path; labels are forward, so every path ends."""
    at = {insn.label: i for i, insn in enumerate(rule.emit) if insn.kind == "label"}
    alias = {"$s": "r11"}
    start = ExpandState(
        {"r11": z3.FreshConst(z3.BitVecSort(64), "r11")},
        {f: flagmod.havoc(f) for f in flagmod.FLAGS},
        [z3.FreshConst(z3.BitVecSort(64), "xmm%d" % k) for k in range(4)],
        {},
    )
    paths, work = [], [(z3.BoolVal(True), 0, start)]
    while work:
        cond, pc, st = work.pop()
        while True:
            if pc == len(rule.emit):
                raise RuleSemanticsError("expand %s: a path falls off the end" % rule.name)
            insn = rule.emit[pc]
            if insn.kind == "label":
                pc += 1
                continue
            if insn.kind == "in":
                st.regs["r11"] = inputs[insn.input]
                pc += 1
                continue
            if insn.kind == "out":
                k, v = insn.source
                st.outs[insn.slot] = (
                    st.regs["r11"]
                    if k == "reg"
                    else st.xmm[v]
                    if k == "xmm"
                    else z3.SignExt(32, z3.Extract(31, 0, imm_value(v)))
                )
                pc += 1
                continue
            if insn.kind == "sse":
                step_sse(insn, st, imm_value, fp_op)
                pc += 1
                continue
            out = step(insn, st, alias, imm_value, flagmod)
            if out is None:
                pc += 1
                continue
            kind, label, taken = out
            exit_ = label if label in ("$BAIL", "$DONE") else None
            if kind == "jmp":
                if exit_:
                    paths.append((cond, exit_, st))
                    break
                pc = at[label]
                continue
            if exit_:
                paths.append((z3.And(cond, taken), exit_, st.copy()))
            else:
                work.append((z3.And(cond, taken), at[label], st.copy()))
            cond = z3.And(cond, z3.Not(taken))
            pc += 1
    return paths
