# SPDX-License-Identifier: GPL-2.0-or-later
"""SMT proofs of the Tier1 instruction-selection and fold rules
(rules.h format 2).

For each isel rule, with z3 bit-vectors: inputs are one 64-bit vector per
value binder (I1 inputs are 0 or 1) and per immediate binder (restricted by
`when`, the immediate's domain and the IR invariant of its layout), under
the rule's relation checks; incoming flags are free; outputs and temps
start with free values (havoc), tied outputs share their input's logical
register. Every write-only operand narrower than 32 bits gets fresh upper
bits, matching t1_mir_effects even across allocation splits.
The IA-64 side evaluates the pattern with sem_ia64.py; the AMD64 side
executes the template with sem_amd64.py, forking at each jcc. The
obligation, for every path: each declared output equals its IA-64 result
(an I1 result as 0/1), the exit label equals the IA-64 branch decision
(BRANCH rules) or the path falls through (value rules), untied inputs are
unchanged, `preserved` rules leave every flag unchanged, every derived
immediate and displacement is encodable (I8 counts 0..63, imm32, disp32),
and a LOAD/STORE rule's one access is at [address + window delta] and loads
or stores the value ld/st defines (sem_ia64 LOAD_VALUE/STORE_VALUE). z3
must answer unsat for the negation.

For each fold rule: the pattern and the result, both evaluated with
sem_ia64.py (result immediates by the expression semantics below, the
definition rules.h t1_iexpr_eval implements), are equal for all inputs and
immediates satisfying the checks.

For each FP expansion (fpfast.rules): inputs are the values `in` reads
(qp 0 or 1; meta values below 2^18, the FR metadata elements' range),
#sf in 0..3 and #mf PSR.mfl or PSR.mfh; R11, xmm0-xmm3 and the flags start
free. On every path to $DONE the helper results written (FR significand,
metadata and the PSR.mfl/mfh bit) are those of the instruction:
unchanged with no modified bit for qp = 0, else sem_ia64.FP_EXPAND's under
its conditions (the exact or approx promise). A path to $BAIL writes no
result (the helper computes them). The specification uses Z3 IEEE
Float32/Float64 add, mul and fused FMA with RNE. The proof conservatively
abstracts each named operation by an arbitrary function on encodings:
UNSAT for all such functions implies UNSAT for the IEEE interpretation.
The separate status theorem proves the MXCSR/FPSR mapping preserves FPSR
and cannot trap under the guard when the only possible exception is I.
Neither theorem verifies the C soft-float arithmetic implementation;
native/rational and soft-float conformance tests cover that boundary.

    $T1_Z3_VENV/bin/python -I tests/tier1/rules/prove.py
        [--selftest] [--rules FILE] [--fold FILE] [--fpfast FILE]
"""

import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def load(name, path):
    # -I drops the script directory from sys.path: import by path.
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


try:
    import z3
except ImportError:
    sys.stderr.write(
        "prove.py: z3 is not importable; run it with $T1_Z3_VENV/bin/python "
        "(see local.env.example)\n"
    )
    sys.exit(1)
rulelib = load("rulelib", os.path.join(ROOT, "engine", "tier1", "rules", "rulelib.py"))
flags = load("flags", os.path.join(HERE, "flags.py"))
sem_ia64 = load("sem_ia64", os.path.join(HERE, "sem_ia64.py"))
sem_amd64 = load("sem_amd64", os.path.join(HERE, "sem_amd64.py"))

TIMEOUT_MS = 120000
MASK64 = 2**64 - 1


def zexpr(e, imm):
    """An immediate expression (rulelib tuples) as a 64-bit z3 term: the
    definition of rules.h t1_iexpr_eval (wrapping arithmetic, shift counts
    modulo 64, comparisons 0/1, lt signed, ltu unsigned)."""
    if e[0] == "lit":
        return z3.BitVecVal(e[1] & MASK64, 64)
    if e[0] == "imm":
        return imm[e[1]]
    a, b = zexpr(e[1], imm), zexpr(e[2], imm)
    one, zero = z3.BitVecVal(1, 64), z3.BitVecVal(0, 64)
    count = b & 63
    return {
        "add": lambda: a + b,
        "sub": lambda: a - b,
        "and": lambda: a & b,
        "or": lambda: a | b,
        "xor": lambda: a ^ b,
        "shl": lambda: a << count,
        "shru": lambda: z3.LShR(a, count),
        "shr": lambda: a >> count,
        "eq": lambda: z3.If(a == b, one, zero),
        "lt": lambda: z3.If(a < b, one, zero),
        "ltu": lambda: z3.If(z3.ULT(a, b), one, zero),
    }[e[0]]()


def signed_in(v, lo, hi):
    return z3.And(v >= z3.BitVecVal(lo & MASK64, 64), v <= z3.BitVecVal(hi & MASK64, 64))


def immediates(rule, ir_ops):
    """The immediate binders as z3 variables with their preconditions: the
    `when` domain or the layout's domain, the IR invariant of the layout
    (ir_ops.def), and the rule's relation checks."""
    pre, imm = [], {}
    for k in rule.imms:
        v = z3.BitVec("imm_%s_%s" % (rule.name, k[1:]), 64)
        imm[k] = v
        node = [p for p in rule.pat if getattr(p, "imm_binder", None) == k][0]
        layout = ir_ops[node.op].imm
        dom = rule.when.get(k) or rulelib.imm_domain(layout)
        if dom is None:
            raise rulelib.RuleError("rule %s: no domain for %s" % (rule.name, k))
        if isinstance(dom, list):
            pre.append(z3.Or([v == x for x in dom]))
        else:
            pre.append(z3.And(v >= dom[0], v <= dom[1]))
        if layout == "FIELD":
            # ir_ops.def FIELD: pos 0..63, len 1..64 (the IR invariant the
            # matcher relies on; the packed range alone admits more).
            pos, length = v & 255, z3.LShR(v, 8)
            pre.append(z3.And(z3.ULE(pos, 63), z3.UGE(length, 1), z3.ULE(length, 64)))
        elif layout == "ACCESS":
            # bytes 1/2/4/8 | ordered << 8 | site << 16, site < T1_MAX_SITES.
            pre.append(
                z3.And(
                    z3.Or([v & 255 == b for b in (1, 2, 4, 8)]),
                    z3.ULE(z3.LShR(v, 8) & 255, 1),
                    z3.ULE(z3.LShR(v, 16), 63),
                )
            )
    for e, lo, hi in rule.checks:
        pre.append(signed_in(zexpr(e, imm), lo, hi))
    return pre, imm


def encode(rule, ir_ops, share=None):
    """Builds (precondition, obligation) for one resolved rule. share maps an
    input binder to another whose register (and value) it shares; it exists
    only for the self-test that shows why isel must copy a tied input whose
    value is also read through another binder (rules.h register model)."""
    pre, imm = immediates(rule, ir_ops)
    ins = {b: z3.BitVec("in_%s_%s" % (rule.name, b[1:]), 64) for b in rule.ins}
    for b, t in rule.types.items():
        if t == "I1":
            pre.append(z3.ULE(ins[b], 1))
    memory = rule.is_memory()
    word = z3.BitVec("mem_%s" % rule.name, 64) if memory else None

    # IA-64 side.
    results = {}

    def value(node):
        if node.op is None:
            return ins[node.binder]
        if node.index not in results:
            kids = [value(k) for k in node.kids]
            packed = node.packed if node.packed is not None else imm.get(node.imm_binder, 0)
            if node.op == "BRANCH":
                results[node.index] = (kids[0],)
            elif node.op == "LOAD":
                results[node.index] = (sem_ia64.LOAD_VALUE(packed & 255, word), None)
            elif node.op == "STORE":
                results[node.index] = (sem_ia64.STORE_VALUE(packed & 255, word, kids[1]),)
            else:
                results[node.index] = sem_ia64.evaluate(node.op, packed, kids)
        res = node.result or (ir_ops[node.op].results[0][0] if ir_ops[node.op].results else None)
        names = [r[0] for r in ir_ops[node.op].results] or [None]
        return results[node.index][names.index(res)]

    # A BRANCH on a leaf $c (an I1 register) decides on c != 0.
    decision = sem_ia64.b1(value(rule.match)) if rule.is_branch() else None
    if not rule.is_branch():
        value(rule.match)

    # AMD64 side: logical registers per binder; run havocs the upper bits
    # of every pure sub-32-bit definition, regardless of earlier writes.
    alias, regs = {}, {}
    for b in rule.ins:
        alias[b] = "r_" + b[1:]
        regs[alias[b]] = ins[b]
    mem_addr = None
    if memory:
        # The window delta register; host address of the guest word at the
        # access address (host_access = gva + delta).
        delta = z3.BitVec("window_%s" % rule.name, 64)
        alias[rule.window[0]] = "r_window"
        regs["r_window"] = delta
        mem_addr = ins[rule.match.kids[0].binder] + delta
    for b, other in (share or {}).items():
        pre.append(ins[b] == ins[other])
        alias[b] = alias[other]
    tied = dict(rule.ties)
    for b in [o[0] for o in rule.outputs] + rule.temps:
        if b in tied:
            alias[b] = alias[tied[b]]
        else:
            alias[b] = "r_" + b[1:]
            regs[alias[b]] = z3.BitVec("init_%s_%s" % (rule.name, b[1:]), 64)
    flags_in = {f: z3.Bool("flag_in_%s_%s" % (rule.name, f)) for f in flags.FLAGS}

    def imm_value(x):
        if isinstance(x, int):
            return z3.BitVecVal(x & MASK64, 64)
        return zexpr(x, imm) if isinstance(x, tuple) else imm[x]

    paths = sem_amd64.run(rule, regs, flags_in, alias, imm_value, flags, mem_addr, word)

    # Encodability of every immediate operand and displacement (format 2
    # derives them; the matcher hands them to the encoder unchecked).
    encodable = []
    for insn in rule.emit:
        for (k, v), spec in zip(insn.operands, insn.op.specs):
            if k == "imm" and not isinstance(v, int):
                lo, hi = rulelib.imm_spec_range(insn, spec)
                if hi < 2**63:
                    encodable.append(signed_in(imm_value(v), lo, hi))
            elif k == "mem" and v.disp_expr is not None:
                encodable.append(signed_in(imm_value(v.disp_expr) + v.disp, -(2**31), 2**31 - 1))

    untied_ins = [b for b in rule.ins if b not in tied.values()]
    full = []
    for cond, label, st in paths:
        ob = []
        if rule.is_branch():
            if label is None:
                raise rulelib.RuleError("rule %s: a path falls off the template" % rule.name)
            ob.append(decision if label == "$T" else z3.Not(decision))
        elif label is not None:
            raise rulelib.RuleError("rule %s: value rule leaves through %s" % (rule.name, label))
        for b, _, _ in rule.outputs:
            node, res = rule.output_node[b]
            names = [r[0] for r in ir_ops[node.op].results]
            ia = results[node.index][names.index(res)]
            got = st.regs[alias[b]]
            if z3.is_bool(ia):
                ob.append(got == z3.If(ia, z3.BitVecVal(1, 64), z3.BitVecVal(0, 64)))
            else:
                ob.append(got == ia)
        for b in untied_ins:
            ob.append(st.regs[alias[b]] == ins[b])
        if rule.flags == "preserved":
            ob += [st.flags[f] == flags_in[f] for f in flags.FLAGS]
        if memory:
            ob += st.obligations
            if rule.match.op == "STORE":
                ob.append(st.mem_word == results[rule.match.index][0])
            else:
                ob.append(st.mem_word == word)
        full.append(z3.Implies(cond, z3.And(ob)))
    return z3.And(pre), z3.And(full + encodable)


def encode_fold(rule, ir_ops):
    """(precondition, obligation) of a fold rule: pattern == result."""
    pre, imm = immediates(rule, ir_ops)
    ins = {b: z3.BitVec("in_%s_%s" % (rule.name, b[1:]), 64) for b in rule.ins}
    for b, t in rule.types.items():
        if t == "I1":
            pre.append(z3.ULE(ins[b], 1))

    def result_of(op, res, values):
        names = [r[0] for r in ir_ops[op].results]
        return values[names.index(res)]

    def pattern(node):
        if node.op is None:
            return ins[node.binder]
        kids = [pattern(k) for k in node.kids]
        packed = node.packed if node.packed is not None else imm.get(node.imm_binder, 0)
        res = node.result or ir_ops[node.op].results[0][0]
        return result_of(node.op, res, sem_ia64.evaluate(node.op, packed, kids))

    def build(node):
        if node.op is None:
            return ins[node.binder]
        kids = [build(k) for k in node.kids]
        e = node.imm_expr
        packed = 0 if e is None else e[1] if e[0] == "lit" else zexpr(e, imm)
        if node.op == "CONST":
            return sem_ia64.CONST(packed)
        res = node.result or ir_ops[node.op].results[0][0]
        return result_of(node.op, res, sem_ia64.evaluate(node.op, packed, kids))

    lhs, rhs = pattern(rule.match), build(rule.result)
    if rule.root_type == "I1":
        l = sem_ia64.b1(lhs)
        if z3.is_bool(rhs):
            ob = l == rhs
        else:
            ob = z3.And(z3.ULE(rhs, 1), l == (rhs == 1))
    else:
        ob = lhs == sem_ia64.as_bv(rhs)
    return z3.And(pre), ob


def prove(rule, ir_ops):
    """Returns (True, None) when proved, else (False, counterexample text)."""
    pre, ob = encode_fold(rule, ir_ops) if rule.fold else encode(rule, ir_ops)
    s = z3.Solver()
    s.set("timeout", TIMEOUT_MS)
    # An unsatisfiable precondition (e.g. a when range outside the
    # immediate's domain) would make every obligation hold vacuously.
    s.add(pre)
    if s.check() != z3.sat:
        return False, "vacuous: the precondition has no model"
    # A wide multiply has the same product on both sides. Quantifying a
    # fresh 128-bit product proves the carry identity for every product,
    # avoiding bit-blasting a 64x64 multiplier. This strengthens the
    # obligation: only an UNSAT result is accepted; otherwise prove the
    # original expression and retain its actual counterexample.
    if not rule.fold and any(i.mnemonic in ("mulpair64", "imulpair64") for i in rule.emit):
        term = ob
        products, seen = [], set()
        work = [term]
        while work:
            node = work.pop()
            if node.get_id() in seen:
                continue
            seen.add(node.get_id())
            if node.decl().kind() == z3.Z3_OP_BMUL:
                products.append((node, z3.FreshConst(node.sort(), "product")))
            else:
                work.extend(node.children())
        if products:
            s.push()
            s.add(z3.Not(z3.substitute(term, *products)))
            abstract = s.check()
            s.pop()
            if abstract == z3.unsat:
                return True, None
    s.add(z3.Not(ob))
    r = s.check()
    if r == z3.unsat:
        return True, None
    if r == z3.sat:
        m = s.model()
        cex = ", ".join(
            "%s=%s" % (d.name(), m[d])
            for d in sorted(m.decls(), key=lambda d: d.name())
            if d.name().startswith(("in_", "imm_", "mem_"))
        )
        return False, cex
    return False, "z3 returned %s" % r


def load_rules(path):
    rules, ir_ops = rulelib.load_rules(path)
    return rules, ir_ops


def expand_kind(rule):
    x = rule.expand
    return {
        "opcode": x["insn"],
        "precision": x["precision"],
        "addend": x["addend"],
        "mode": x["mode"],
        "fused": x["host"] == "fma3",
    }


def abstract_fp_op(name):
    """Conservative abstraction of sem_ia64.fp_op's deterministic operation.

    Keeping the arguments retains congruence when the two sides pack
    encodings differently. Every IEEE interpretation is one of these
    arbitrary functions; no arithmetic property is assumed by the proof.
    """
    sem_ia64.fp_op(name)  # Reject names without an IEEE interpretation.
    width = 32 if name.endswith("_s") else 64
    args = [z3.BitVecSort(width)] * (3 if name.startswith("fma") else 2)
    return z3.Function("abstract_ieee_" + name, *args, z3.BitVecSort(width))


def encode_expand(rule, fp_ops=sem_ia64.fp_op):
    """(precondition, negated obligation) of an expansion."""
    inp = {
        n: z3.BitVec("in_%s_%s" % (rule.name, n.replace(".", "_")), 64)
        for n in rulelib.EXPAND_INPUTS
    }
    imm = {k: z3.BitVec("imm_%s_%s" % (rule.name, k[1:]), 64) for k in rulelib.EXPAND_IMMS}
    pre = [z3.ULE(inp["qp"], 1)] + [z3.ULT(inp[n], 1 << 18) for n in inp if n.endswith(".meta")]
    pre += [z3.Or([imm[k] == v for v in vals]) for k, vals in rulelib.EXPAND_IMMS.items()]

    def imm_value(v):
        if isinstance(v, int):
            return z3.BitVecVal(v & MASK64, 64)
        if isinstance(v, str):
            return imm[v]
        return zexpr(v, imm)

    paths = sem_amd64.run_expand(rule, inp, imm_value, fp_ops, flags)
    spec = {k.replace(".", "_"): v for k, v in inp.items()}
    defined, sig, meta, mf = sem_ia64.FP_EXPAND(
        expand_kind(rule), spec, imm["#sf"], imm["#mf"], fp_ops
    )
    bad = []
    for cond, exit_, st in paths:
        if exit_ == "$BAIL":
            if st.outs:
                bad.append(cond)
            continue
        if set(st.outs) != set(rulelib.EXPAND_SLOTS):
            raise sem_amd64.RuleSemanticsError(
                "expand %s: a path to $DONE writes %s" % (rule.name, sorted(st.outs))
            )
        o = st.outs
        skip = z3.And(o["d.sig"] == inp["d.sig"], o["d.meta"] == inp["d.meta"], o["mf"] == 0)
        exe = z3.And(defined, o["d.sig"] == sig, o["d.meta"] == meta, o["mf"] == mf)
        bad.append(z3.And(cond, z3.Not(z3.If(inp["qp"] == 0, skip, exe))))
    return z3.And(pre), z3.Or(bad) if bad else z3.BoolVal(False), len(paths)


def prove_expand(rule):
    pre, neg, npaths = encode_expand(rule, abstract_fp_op)
    s = z3.Solver()
    s.set("timeout", TIMEOUT_MS)
    s.add(pre)
    if s.check() != z3.sat:
        return False, "vacuous: the precondition has no model", npaths
    s.add(neg)
    r = s.check()
    if r == z3.unsat:
        return True, None, npaths
    if r == z3.sat:
        m = s.model()
        cex = ", ".join(
            "%s=%s" % (d.name(), m[d])
            for d in sorted(m.decls(), key=lambda d: d.name())
            if d.name().startswith(("in_", "imm_"))
        )
        return False, cex, npaths
    return False, "z3 returned %s" % r, npaths


def prove_fp_status():
    """Status mapping, conditional on no exception other than inexact.

    This does not derive the exception set from the arithmetic guard.
    The exclusion of invalid/denormal/overflow/underflow remains a
    semantic premise of FP_EXPAND, checked by conformance tests.
    """
    fpsr, sf, mxcsr = z3.BitVecs("fp_fpsr fp_sf fp_mxcsr", 64)
    s = z3.Solver()
    s.set("timeout", TIMEOUT_MS)
    s.add(z3.ULE(sf, 3), sem_ia64.fp_controls(fpsr, sf, False), mxcsr & 31 == 0)
    if s.check() != z3.sat:
        return False
    s.add(
        z3.Or(
            sem_ia64.fp_apply_flags(fpsr, sf, mxcsr) != fpsr,
            sem_ia64.fp_traps(fpsr, sf, mxcsr),
            sem_ia64.fp_mxcsr(fpsr, sf) != 0x1F80,
        )
    )
    return s.check() == z3.unsat


def prove_fp_finite():
    """IEEE add/mul/FMA of normal or zero operands never produce NaN.

    Use the native FP expressions, without arithmetic abstraction. This
    proves the invalid-result exclusion; it does not prove the underflow
    bound or correspondence with the soft-float rounding implementation.
    """
    for fmt, sort, width in [("s", z3.Float32(), 32), ("d", z3.Float64(), 64)]:
        for op in ("add", "mul", "fma"):
            args = [
                z3.BitVec("finite_%s_%s_%d" % (op, fmt, k), width)
                for k in range(3 if op == "fma" else 2)
            ]
            values = [z3.fpBVToFP(x, sort) for x in args]
            result = z3.fpBVToFP(sem_ia64.fp_op(op + "_" + fmt)(*args), sort)
            s = z3.Solver()
            s.set("timeout", TIMEOUT_MS)
            s.add(*[z3.Or(z3.fpIsNormal(x), z3.fpIsZero(x)) for x in values])
            if s.check() != z3.sat:
                return False
            s.add(z3.fpIsNaN(result))
            if s.check() != z3.unsat:
                return False
    return True


def prove_fp_round_normal():
    """The soft normal-rounding model equals IEEE RNE for any significand.

    Low 64 bits witness the exact fraction summarized by raw.tail. For
    positive shifts only zero/nonzero matters, so this covers both tail
    values. A wider exponent range prevents overflow from hiding a wrong
    rounded significand. No claim about exponent-range handling or the
    C implementation's production of raw.mant/tail is made here.
    """
    for precision in (24, 53):
        raw = z3.BitVec("round_raw_%d" % precision, 128)
        mant, low = z3.Extract(127, 64, raw), z3.Extract(63, 0, raw)
        rounded, inexact, up = sem_ia64.fp_round_normal(mant, low != 0, precision)
        magnitude = z3.ZeroExt(1, rounded) << (128 - precision)
        sort = z3.FPSort(17, precision)
        expected = z3.fpToFPUnsigned(z3.RNE(), raw, sort)
        actual = z3.fpToFPUnsigned(z3.RNE(), magnitude, sort)
        s = z3.Solver()
        s.set("timeout", TIMEOUT_MS)
        s.add(z3.Extract(127, 127, raw) == 1)
        if s.check() != z3.sat:
            return False
        s.add(
            z3.Or(
                actual != expected,
                inexact != (z3.Extract(127 - precision, 0, raw) != 0),
                up != z3.UGT(magnitude, z3.ZeroExt(1, raw)),
            )
        )
        if s.check() != z3.unsat:
            return False
    return True


def prove_expansions(rules):
    ok = prove_fp_status()
    print("prove.py: FP status mapping (only inexact) %s" % ("unsat (proved)" if ok else "FAILED"))
    finite = prove_fp_finite()
    print("prove.py: IEEE finite add/mul/fma (s/d) %s" % ("unsat (proved)" if finite else "FAILED"))
    ok &= finite
    rounding = prove_fp_round_normal()
    print(
        "prove.py: soft normal RNE significand (24/53 bits) %s"
        % ("unsat (proved)" if rounding else "FAILED")
    )
    ok &= rounding
    for r in rules:
        good, cex, npaths = prove_expand(r)
        print(
            "prove.py: expand %-18s %s (%d paths)"
            % (r.name, "unsat (conditional mapping)" if good else "REFUTED: " + cex, npaths)
        )
        ok &= good
    return ok


def prove_all(rules, ir_ops):
    ok = True
    for r in rules:
        good, cex = prove(r, ir_ops)
        print(
            "prove.py: %s %-22s %s"
            % (
                "fold" if r.fold else "rule",
                r.name,
                "unsat (proved)" if good else "REFUTED: " + cex,
            )
        )
        ok &= good
    return ok


def _st(name, body, why):
    return name, ("rule %s cost 1\n%s\nend" % (name, body.strip("\n")), why)


# One broken variant per rule family: each must be
# refuted, and where `why` is given the counterexample must show the reason
# the variant is wrong.
SELFTEST = dict(
    [
        # x86 masks the count; IA-64 shl gives 0 for counts >= 64.
        _st(
            "shl_masked",
            """
  match   (SHL $a $c)
  outputs $d = (SHL $a $c)@value
  tie     $d = $a
  fixed   $c = rcx
  emit    shl64 $d, cl
  flags   clobbered""",
            lambda m, v: m.eval(z3.UGE(v["in_shl_masked_c"], 64)),
        ),
        # sub lc,1 ; jae T for br.cloop is wrong for LC = 0 (LC must stay 0).
        _st(
            "cloop_sub_jae",
            """
  match   (BRANCH (CLOOP $lc)@taken $T $F)
  outputs $n = (CLOOP $lc)@next
  tie     $n = $lc
  labels  $T $F
  emit    sub64 $n, 1
          jae   $T
          jmp   $F
  flags   dead-after""",
            lambda m, v: m.eval(v["in_cloop_sub_jae_lc"] == 0),
        ),
        # OF after shl by 2 is undefined; reading it is unprovable.
        _st(
            "of_after_shl2",
            """
  match   (SHL_IMM $a #c)
  when    (in #c 2)
  outputs $d = (SHL_IMM $a #c)@value
  temps   $t0
  emit    mov64   $d, $a
          xor32   $t0, $t0
          shl64   $d, 2
          cmovo64 $d, $t0
  flags   clobbered""",
            None,
        ),
        # integer: andcm complements the wrong operand.
        _st(
            "andcm_swapped",
            """
  match   (ANDCM $a $b)
  outputs $d = (ANDCM $a $b)@value
  emit    mov64 $d, $a
          not64 $d
          and64 $d, $b
  flags   clobbered""",
            lambda m, v: m.eval(v["in_andcm_swapped_a"] != v["in_andcm_swapped_b"]),
        ),
        # integer: sub r1 = imm8, r3 computed as r3 - imm.
        _st(
            "sub_ir_reversed",
            """
  match   (SUB (CONST #k) $b)
  when    (range #k -128 127)
  outputs $d = (SUB (CONST #k) $b)@value
  tie     $d = $b
  emit    sub64 $d, #k
  flags   clobbered""",
            None,
        ),
        # integer: a flag-writing template claimed `preserved`.
        _st(
            "add_claims_preserved",
            """
  match   (ADD $a $b)
  outputs $d = (ADD $a $b)@value
  tie     $d = $a
  emit    add64 $d, $b
  flags   preserved""",
            None,
        ),
        # shifts: IA-64 shr by > 63 is the sign fill, not 0.
        _st(
            "shr_var_zero_fill",
            """
  match   (SHR $a $c)
  outputs $d = (SHR $a $c)@value
  temps   $t0
  tie     $d = $a
  fixed   $c = rcx
  emit    xor32   $t0, $t0
          sar64   $d, cl
          cmp64   $c, 63
          cmova64 $d, $t0
  flags   clobbered""",
            lambda m, v: m.eval(
                z3.And(z3.UGT(v["in_shr_var_zero_fill_c"], 63), v["in_shr_var_zero_fill_a"] < 0)
            ),
        ),
        # bitfields: extr with a logical first shift loses the sign when the
        # field runs past bit 63 (effective length 64 - pos).
        _st(
            "extr_any_shr_first",
            """
  match   (EXTR #f $a)
  outputs $d = (EXTR #f $a)@value
  temps   $t0
  tie     $d = $a
  fixed   $t0 = rcx
  emit    mov32 $t0, #f
          shr64 $d, cl
          shr64 $t0, 8
          neg64 $t0
          shl64 $d, cl
          sar64 $d, cl
  flags   clobbered""",
            lambda m, v: m.eval(
                z3.UGT(
                    (v["imm_extr_any_shr_first_f"] & 255)
                    + z3.LShR(v["imm_extr_any_shr_first_f"], 8),
                    64,
                )
            ),
        ),
        # bitfields: sxt4 by a zero extension.
        _st(
            "sxt4_zero",
            """
  match   (SXT 32 $a)
  outputs $d = (SXT 32 $a)@value
  emit    mov32 $d, $a
  flags   preserved""",
            lambda m, v: m.eval(z3.Extract(31, 31, v["in_sxt4_zero_a"]) == 1),
        ),
        # bitfields: zxt4 by a 64-bit move keeps bits 63:32.
        _st(
            "zxt4_mov64",
            """
  match   (ZXT 32 $a)
  outputs $d = (ZXT 32 $a)@value
  emit    mov64 $d, $a
  flags   preserved""",
            lambda m, v: m.eval(z3.UGE(v["in_zxt4_mov64_a"], 2**32)),
        ),
        # constants: mov32 zero-extends, so it cannot load a negative imm32.
        _st(
            "const_neg_mov32",
            """
  match   (CONST #k)
  when    (range #k -2147483648 -1)
  outputs $d = (CONST #k)@value
  emit    mov32 $d, #k
  flags   preserved""",
            None,
        ),
        # compares: cmp4 compared on 64 bits.
        _st(
            "cmp4_lt_cmp64",
            """
  match   (CMP lt 32 $a $b)
  outputs $p = (CMP lt 32 $a $b)@value
  emit    cmp64     $a, $b
          setl8     $p
          movzx32_8 $p, $p
  flags   clobbered""",
            None,
        ),
        # compares: ltu with a signed condition.
        _st(
            "cmp_ltu_signed",
            """
  match   (CMP ltu 64 $a $b)
  outputs $p = (CMP ltu 64 $a $b)@value
  emit    cmp64     $a, $b
          setl8     $p
          movzx32_8 $p, $p
  flags   clobbered""",
            None,
        ),
        # compares: the immediate form k < b without swapping the condition.
        _st(
            "cmp_lt_ir_unswapped",
            """
  match   (CMP lt 64 (CONST #k) $b)
  when    (range #k -128 127)
  outputs $p = (CMP lt 64 (CONST #k) $b)@value
  emit    cmp64     $b, #k
          setl8     $p
          movzx32_8 $p, $p
  flags   clobbered""",
            None,
        ),
        # compares: set<cc>8 alone leaves bits 63:8 of the I1 register stale.
        _st(
            "cmp_eq_no_movzx",
            """
  match   (CMP eq 64 $a $b)
  outputs $p = (CMP eq 64 $a $b)@value
  emit    cmp64 $a, $b
          sete8 $p
  flags   clobbered""",
            None,
        ),
        # A pure byte definition can start in a different physical register:
        # clearing the binder before SETcc cannot constrain its upper bits.
        _st(
            "frnattest_zero_before_set",
            """
  match (FRNATTEST $sig $meta)
  outputs $d = (FRNATTEST $sig $meta)@value
  temps $tag
  emit xor32 $d, $d
       xor32 $tag, $tag
       test64 $sig, $sig
       sete8 $d
       cmp64 $meta, 131070
       sete8 $tag
       and64 $d, $tag
  flags clobbered""",
            None,
        ),
        _st(
            "cloop_taken_zero_before_set",
            """
  match (CLOOP $lc)@taken
  outputs $p = (CLOOP $lc)@taken
  emit xor32 $p, $p
       test64 $lc, $lc
       setne8 $p
  flags clobbered""",
            None,
        ),
        # branches: a compare branch on the wrong width.
        _st(
            "cmp4_ltu_branch_cmp64",
            """
  match   (BRANCH (CMP ltu 32 $a $b)@value $T $F)
  labels  $T $F
  emit    cmp64 $a, $b
          jb    $T
          jmp   $F
  flags   dead-after""",
            None,
        ),
        # branches: br on NOT1 c with the condition not inverted.
        _st(
            "br_not_jnz",
            """
  match   (BRANCH (NOT1 $c)@value $T $F)
  labels  $T $F
  emit    test64 $c, $c
          jnz    $T
          jmp    $F
  flags   dead-after""",
            None,
        ),
        # predicates: not64 of an I1 is not 0/1.
        _st(
            "not1_not64",
            """
  match   (NOT1 $a)
  outputs $d = (NOT1 $a)@value
  tie     $d = $a
  emit    not64 $d
  flags   preserved""",
            None,
        ),
        # predicates: SELECT with the arms exchanged.
        _st(
            "select_swapped",
            """
  match   (SELECT $c $t $f)
  outputs $d = (SELECT $c $t $f)@value
  tie     $d = $f
  emit    test64  $c, $c
          cmove64 $d, $t
  flags   clobbered""",
            lambda m, v: m.eval(v["in_select_swapped_t"] != v["in_select_swapped_f"]),
        ),
        # format 2, derived immediates: extr.u's second count off by one.
        _st(
            "extru_field_count",
            """
  match   (EXTRU #p #l $a)
  when    (range (add #p #l) 1 63)
  outputs $d = (EXTRU #p #l $a)@value
  tie     $d = $a
  emit    shl64 $d, (sub 64 #p #l)
          shr64 $d, (sub 63 #l)
  flags   clobbered""",
            None,
        ),
        # format 2, relations: the one-shift form of dep.z without its relation
        # (pos + len >= 64) is wrong for inner fields.
        _st(
            "depz_top_no_relation",
            """
  match   (DEPZ #p #l $a)
  outputs $d = (DEPZ #p #l $a)@value
  tie     $d = $a
  emit    shl64 $d, #p
  flags   clobbered""",
            lambda m, v: m.eval(
                z3.ULT(
                    (v["imm_depz_top_no_relation_field(p,l)"] & 255)
                    + z3.LShR(v["imm_depz_top_no_relation_field(p,l)"], 8),
                    64,
                )
            ),
        ),
        # format 2, parts: the count taken from the wrong part of the field.
        _st(
            "extru_top_len",
            """
  match   (EXTRU #p #l $a)
  when    (range (add #p #l) 64 127)
  outputs $d = (EXTRU #p #l $a)@value
  tie     $d = $a
  emit    shr64 $d, (sub 64 #l)
  flags   clobbered""",
            None,
        ),
        # format 2, lea displacement: a - k computed as a + k.
        _st(
            "sub_ri_lea_sign",
            """
  match   (SUB $a (CONST #k))
  when    (range #k -2147483647 2147483647)
  outputs $d = (SUB $a (CONST #k))@value
  emit    lea64 $d, [$a + #k]
  flags   preserved""",
            lambda m, v: m.eval(v["imm_sub_ri_lea_sign_k"] != 0),
        ),
        # memory: ld4 by a 64-bit load keeps bytes 7:4 of the word.
        _st(
            "load4_load64",
            """
  match   (LOAD #m $a)
  when    (in (bytes #m) 4)
  outputs $d = (LOAD #m $a)@value
  window  $w
  emit    load64 $d, [$a + $w * 1]
  flags   preserved""",
            lambda m, v: m.eval(z3.UGE(v["mem_load4_load64"], 2**32)),
        ),
        # memory: st2 by a 32-bit store overwrites bytes 3:2.
        _st(
            "store2_store32",
            """
  match   (STORE #m $a $v)
  when    (in (bytes #m) 2)
  window  $w
  emit    store32 [$a + $w * 1], $v
  flags   preserved""",
            None,
        ),
        # memory: ld1 is a zero extension, not a sign extension.
        _st(
            "load1_sign_extended",
            """
  match   (LOAD #m $a)
  when    (in (bytes #m) 1)
  outputs $d = (LOAD #m $a)@value
  window  $w
  emit    load8zx $d, [$a + $w * 1]
          movsx64_8 $d, $d
  flags   preserved""",
            None,
        ),
        # selects: (qp) adds r = 2, r with scale 4.
        _st(
            "select_add2_scale4",
            """
  match   (SELECT $c (ADD $f (CONST 2))@value $f)
  outputs $d = (SELECT $c (ADD $f (CONST 2))@value $f)@value
  emit    lea64 $d, [$f + $c * 4]
  flags   preserved""",
            lambda m, v: m.eval(v["in_select_add2_scale4_c"] == 1),
        ),
        # branches on predicates: AND1(a, NOT1 b) with the unsigned order reversed.
        _st(
            "br_and1_not_jb",
            """
  match   (BRANCH (AND1 $a (NOT1 $b)@value)@value $T $F)
  labels  $T $F
  emit    cmp64  $a, $b
          jb     $T
          jmp    $F
  flags   dead-after""",
            None,
        ),
    ]
)

# Fold rules that must be refuted (format 2).
FOLD_SELFTEST = [
    # br.cloop with LC = 0 leaves LC at 0.
    (
        "cloop_next_always_dec",
        """fold cloop_next_always_dec
  match   (CLOOP (CONST #a))@next
  result  (CONST (sub #a 1))
end""",
        lambda m, v: m.eval(v["imm_cloop_next_always_dec_a"] == 0),
    ),
    # cmp4 compares bits 31:0 only.
    (
        "cmp4_lt_full",
        """fold cmp4_lt_full
  match   (CMP lt 32 (CONST #a) (CONST #b))
  result  (CONST (lt #a #b))
end""",
        None,
    ),
    # shr by >= 64 is the sign fill, not 0.
    (
        "shr_big_zero",
        """fold shr_big_zero
  match   (SHR (CONST #a) (CONST #c))
  when    (in (ltu #c 64) 0)
  result  (CONST 0)
end""",
        lambda m, v: m.eval(v["imm_shr_big_zero_a"] < 0),
    ),
    # the expression's shift count is taken modulo 64: no guard, no fold.
    (
        "shl_unguarded",
        """fold shl_unguarded
  match   (SHL (CONST #a) (CONST #c))
  result  (CONST (shl #a #c))
end""",
        lambda m, v: m.eval(z3.UGE(v["imm_shl_unguarded_c"], 64)),
    ),
    # a FIELD rewrite without its relation.
    (
        "extru_shru_no_relation",
        """fold extru_shru_no_relation
  match   (EXTRU #p #l $a)
  result  (SHRU $a (CONST #p))
end""",
        None,
    ),
]

# A fold rule whose checks no immediate satisfies proves nothing.
VACUOUS_FOLD = """fold vacuous
  match   (EXTRU #p #l $a)
  when    (range (add #p #l) 200 300)
  result  (CONST 0)
end"""

# The encodability obligation itself (format 2): with the static range check
# bypassed, a derived count that can be 64 must be refuted by z3.
ENCODABLE_ONLY = """rule count_reaches_64 cost 1
  match   (EXTRU #p #l $a)
  when    (range (add #p #l) 64 127)
  outputs $d = (EXTRU #p #l $a)@value
  tie     $d = $a
  emit    shr64 $d, (add #p 1)
  flags   clobbered
end"""

# Rules the static checks of rulelib.resolve must reject (no proof runs).
STATIC_REJECT = [
    # an imm32 of a 64-bit operation is sign-extended: 2^31.. is not one.
    (
        "add_ri_u32",
        """rule add_ri_u32 cost 1
  match   (ADD $a (CONST #k))
  when    (range #k 0 4294967295)
  outputs $d = (ADD $a (CONST #k))@value
  tie     $d = $a
  emit    add64 $d, #k
  flags   clobbered
end""",
        "out of range",
    ),
    # an untied input is never written.
    (
        "sub_writes_input",
        """rule sub_writes_input cost 1
  match   (SUB $a $b)
  outputs $d = (SUB $a $b)@value
  emit    sub64 $a, $b
          mov64 $d, $a
  flags   clobbered
end""",
        "not an output or temp",
    ),
    # a full-range CONST binder cannot be an imm32.
    (
        "const_any_mov64",
        """rule const_any_mov64 cost 1
  match   (CONST #k)
  outputs $d = (CONST #k)@value
  emit    mov64 $d, #k
  flags   preserved
end""",
        "out of range",
    ),
    # format 2: a derived shift count that reaches 64 (pos = 0).
    (
        "shift_count_64",
        """rule shift_count_64 cost 1
  match   (EXTRU #p #l $a)
  outputs $d = (EXTRU #p #l $a)@value
  tie     $d = $a
  emit    shl64 $d, (sub 64 #p)
  flags   clobbered
end""",
        "out of range",
    ),
    # format 2: -k is not a disp32 for k = -2^31.
    (
        "sub_ri_disp_min",
        """rule sub_ri_disp_min cost 1
  match   (SUB $a (CONST #k))
  when    (range #k -2147483648 2147483647)
  outputs $d = (SUB $a (CONST #k))@value
  emit    lea64 $d, [$a + (neg #k)]
  flags   preserved
end""",
        "out of range for disp32",
    ),
    # memory: the access is exactly [address + window].
    (
        "load8_scaled_window",
        """rule load8_scaled_window cost 1
  match   (LOAD #m $a)
  when    (in (bytes #m) 8)
  outputs $d = (LOAD #m $a)@value
  window  $w
  emit    load64 $d, [$a + $w * 2]
  flags   preserved
end""",
        "the access of a LOAD/STORE rule",
    ),
    # memory: a LOAD rule names its window register.
    (
        "load8_no_window",
        """rule load8_no_window cost 1
  match   (LOAD #m $a)
  when    (in (bytes #m) 8)
  outputs $d = (LOAD #m $a)@value
  emit    load64 $d, [$a * 1]
  flags   preserved
end""",
        "declare a window",
    ),
    # memory: one access per site.
    (
        "store8_twice",
        """rule store8_twice cost 1
  match   (STORE #m $a $v)
  when    (in (bytes #m) 8)
  window  $w
  emit    store64 [$a + $w * 1], $v
          store64 [$a + $w * 1], $v
  flags   preserved
end""",
        "exactly one memory access",
    ),
]


# shl_var with the cmp placed before the shl (the order the prover rejects).
SHL_VAR_CMP_FIRST = """rule shl_var_cmp_first cost 4
  match   (SHL $a $c)
  outputs $d = (SHL $a $c)@value
  temps   $t0
  tie     $d = $a
  fixed   $c = rcx
  emit    xor32   $t0, $t0
          cmp64   $c, 63
          shl64   $d, cl
          cmova64 $d, $t0
  flags   clobbered
end"""


# FP expansions with one defect each (fpfast.rules text, old -> new): the
# prover must refute every one.
EXPAND_SELFTEST = [
    ("fma_d_fused", "jns $BAIL", "jo $BAIL", "an operand without the integer bit (unnormal)"),
    ("fma_d_fused", "test64 $s, 0x7ff", "test64 $s, 0x3ff", "an operand with 54 significant bits"),
    ("fma_d_fused", "and64 $s, 0x1032", "and64 $s, 0x1030", "wre set: the 17-bit exponent range"),
    ("fma_s_fused", "test64 $s, 32", "test64 $s, 16", "the inexact trap enabled"),
    ("fmpy_d", "add64 $s, 0xfc00", "add64 $s, 0xfc01", "a wrong result exponent"),
    (
        "fadd_s",
        "shl64 $s, 41\n            jz $BAIL",
        "shl64 $s, 41\n            jo $BAIL",
        "the min normal accepted (it may be a tiny result rounded up)",
    ),
    (
        "fma_s_f0",
        "test64 $s, $s\n            jnz $BAIL\n            .rzok:",
        "test64 $s, $s\n            jo $BAIL\n            .rzok:",
        "a zero product of nonzero factors",
    ),
    ("fnma_d_f0", "xor64 $s, 1", "xor64 $s, 0", "fnma without the negation"),
    ("fsub_d", "out mf, 0", "out mf, #mf", "a false qp marking the bank modified"),
    ("fpma_fused", "cmp64 $s, 0x1fffe", "cmp64 $s, 0x1ffff", "a NaTVal source"),
    (
        "fpnma_f0",
        "shl64 $s, 9\n            shr64 $s, 41\n            jnz $BAIL",
        "shl64 $s, 9\n            shr64 $s, 41\n            jo $BAIL",
        "a denormal lane 1 source",
    ),
]


def selftest_expand():
    _, by_name, _ = rulelib.load_x86_ops()
    sse = rulelib.load_sse_ops()
    with open(rulelib.FPFAST_PATH) as f:
        text = f.read()
    ok = True
    for name, old, new, why in EXPAND_SELFTEST:
        start = text.index("expand %s\n" % name)
        body = text[start : text.index("\nend", start) + 4]
        if body.count(old) < 1:
            print("prove.py --selftest: expand %s has no %r" % (name, old))
            ok = False
            continue
        rule = rulelib.parse_expand(body.replace(old, new, 1), "selftest")[0]
        rulelib.resolve_expand(rule, by_name, sse)
        good, _, _ = prove_expand(rule)
        print(
            "prove.py --selftest: expand %s with %s %s"
            % (name, why, "was NOT rejected" if good else "rejected as required")
        )
        ok &= not good
    return ok


def selftest():
    ir_ops = rulelib.load_ir_ops()
    _, by_name, _ = rulelib.load_x86_ops()
    ok = True
    for name, text, want in STATIC_REJECT:
        try:
            rulelib.resolve(rulelib.parse_rules(text, "selftest")[0], ir_ops, by_name)
            print("prove.py --selftest: %s was NOT rejected statically" % name)
            ok = False
        except rulelib.RuleError as e:
            if want not in str(e):
                print("prove.py --selftest: %s rejected for an unexpected reason: %s" % (name, e))
                ok = False
            else:
                print("prove.py --selftest: %s rejected statically as required" % name)
    for name, (text, why) in SELFTEST.items():
        rule = rulelib.resolve(rulelib.parse_rules(text, "selftest")[0], ir_ops, by_name)
        pre, ob = encode(rule, ir_ops)
        s = z3.Solver()
        s.add(pre, z3.Not(ob))
        if s.check() != z3.sat:
            print("prove.py --selftest: %s was NOT rejected" % name)
            ok = False
            continue
        m = s.model()
        if why is not None:
            names = {d.name(): d() for d in m.decls()}
            if not z3.is_true(why(m, names)):
                print("prove.py --selftest: %s rejected for an unexpected reason" % name)
                ok = False
                continue
        print("prove.py --selftest: %s rejected as required" % name)
    for name, text, why in FOLD_SELFTEST:
        rule = rulelib.resolve_fold(rulelib.parse_rules(text, "selftest", fold=True)[0], ir_ops)
        pre, ob = encode_fold(rule, ir_ops)
        s = z3.Solver()
        s.add(pre, z3.Not(ob))
        if s.check() != z3.sat:
            print("prove.py --selftest: fold %s was NOT rejected" % name)
            ok = False
            continue
        names = {d.name(): d() for d in s.model().decls()}
        if why is not None and not z3.is_true(why(s.model(), names)):
            print("prove.py --selftest: fold %s rejected for an unexpected reason" % name)
            ok = False
            continue
        print("prove.py --selftest: fold %s rejected as required" % name)
    vac = rulelib.resolve_fold(rulelib.parse_rules(VACUOUS_FOLD, "selftest", fold=True)[0], ir_ops)
    good, why = prove(vac, ir_ops)
    if good or "vacuous" not in why:
        print("prove.py --selftest: a vacuous fold rule was NOT rejected")
        ok = False
    else:
        print("prove.py --selftest: vacuous rejected as required")
    saved = rulelib.expr_range
    rulelib.expr_range = lambda rule, e: (0, 0)
    try:
        enc = rulelib.resolve(rulelib.parse_rules(ENCODABLE_ONLY, "selftest")[0], ir_ops, by_name)
    finally:
        rulelib.expr_range = saved
    pre, ob = encode(enc, ir_ops)
    s = z3.Solver()
    s.add(pre, z3.Not(ob))
    if s.check() != z3.sat:
        print("prove.py --selftest: count_reaches_64 was NOT rejected")
        ok = False
    else:
        print("prove.py --selftest: count_reaches_64 rejected as required (encodability)")
    # The cmp must follow the shl (shl by a non-zero count
    # changes the flags, by zero leaves them unchanged).
    rules, _ = load_rules(rulelib.RULES_PATH)
    shl_var = [r for r in rules if r.name == "shl_var"][0]
    order = rulelib.parse_rules(SHL_VAR_CMP_FIRST, "selftest")[0]
    order = rulelib.resolve(order, ir_ops, by_name)
    for name, (pre, ob) in (
        ("shl_var_cmp_first", encode(order, ir_ops)),
        ("shl_var_shared_register", encode(shl_var, ir_ops, {"$c": "$a"})),
    ):
        s = z3.Solver()
        s.add(pre, z3.Not(ob))
        if s.check() != z3.sat:
            print("prove.py --selftest: %s was NOT rejected" % name)
            ok = False
        else:
            print("prove.py --selftest: %s rejected as required" % name)
    # High multiply must propagate low-half carry, and signedness must
    # affect both multiplicands. Concrete witnesses keep negative tests
    # fast without relying on the proof's product abstraction.
    import copy

    all_rules, _ = load_rules(rulelib.RULES_PATH)
    for family, a, b, c in (("xma_hu", 1, 2**64 - 1, 1), ("xma_h", 2**64 - 1, 1, 0)):
        original = next(r for r in all_rules if r.name == family)
        broken = copy.deepcopy(original)
        if family == "xma_hu":
            broken.emit = [i for i in broken.emit if i.mnemonic != "adc64"]
        else:
            insn = next(i for i in broken.emit if i.mnemonic == "imulpair64")
            insn.mnemonic = "mulpair64"
            insn.op = by_name["mulpair64"][0]
        pre, ob = encode(broken, ir_ops)
        solver = z3.Solver()
        solver.add(pre, z3.Not(ob))
        for binder, value in zip(("a", "b", "c"), (a, b, c)):
            solver.add(z3.BitVec("in_%s_%s" % (family, binder), 64) == value)
        if solver.check() != z3.sat:
            print("prove.py --selftest: %s broken high multiply was NOT rejected" % family)
            ok = False
        else:
            print("prove.py --selftest: %s broken high multiply rejected as required" % family)
    # The havoc model itself: OF after shl by 2 is not a function of the inputs.
    a, old = z3.BitVec("a", 64), {f: z3.Bool("o_" + f) for f in flags.FLAGS}
    count = z3.BitVecVal(2, 64)
    f1 = flags.shift("shl", a, count, a << count, 64, old)
    f2 = flags.shift("shl", a, count, a << count, 64, old)
    s = z3.Solver()
    s.add(f1["OF"] != f2["OF"])
    if s.check() != z3.sat:
        print("prove.py --selftest: OF after shl by 2 is determined (havoc broken)")
        ok = False
    s = z3.Solver()
    s.add(f1["CF"] != f2["CF"])
    if s.check() != z3.unsat:
        print("prove.py --selftest: CF after shl by 2 is not determined")
        ok = False
    ok &= selftest_expand()
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--rules", default=rulelib.RULES_PATH)
    ap.add_argument("--fold", default=rulelib.FOLD_PATH)
    ap.add_argument("--fpfast", default=rulelib.FPFAST_PATH)
    args = ap.parse_args()
    _, _, flag_bits = rulelib.load_x86_ops()
    x86_ops = rulelib.load_x86_ops()[0]
    problems = flags.cross_check(x86_ops, flag_bits)
    if problems:
        for p in problems:
            print("prove.py: flag table mismatch: %s" % p)
        return 1
    if args.selftest:
        return 0 if selftest() else 1
    try:
        rules, folds, ir_ops = rulelib.load_all(args.rules, args.fold)
        expands = rulelib.load_expand_rules(args.fpfast)
        names = [r.name for r in rules + folds]
        for e in expands:
            if e.name in names:
                raise rulelib.RuleError(
                    "expand %s: the name is used by an isel or fold rule" % e.name
                )
        ok = prove_all(rules + folds, ir_ops)
        ok &= prove_expansions(expands)
    except (rulelib.RuleError, sem_amd64.RuleSemanticsError, NotImplementedError) as e:
        print("prove.py: %s" % e)
        return 1
    if not ok:
        print("prove.py: some rules are not proved")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
