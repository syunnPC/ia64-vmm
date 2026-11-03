# SPDX-License-Identifier: GPL-2.0-or-later
"""Conformance of the generated rule tables (rules.h format 2), with build/tier1-table-runner.

(3) isel: every entry of build/tier1/isel_rules.h, instantiated from the
    table (registers by its ties and fixed registers, immediates and
    displacements by rules.h t1_iexpr_eval), encoded by engine/tier1/x86enc.c
    and executed natively, against sem_ia64 on random and edge inputs:
    declared outputs, the branch decision, untied inputs unchanged, all
    status flags unchanged for `preserved` rules, and the memory word of
    LOAD/STORE rules. The table's acceptance of immediates (pattern node
    ranges/sets, relation checks) must equal the rule text's `when`.
(4) fold: every entry of build/tier1/fold_rules.h: acceptance as above; the
    result tree in the table equals the rule text's, its immediates
    evaluated by t1_iexpr_eval equal the prover's expression semantics, and
    the result equals the pattern under sem_ia64 on concrete inputs.
(5) iexpr: rules.h t1_iexpr_eval for every operator against the prover's
    definition (prove.zexpr) on random and edge operands.

With conformance.py (sem_ia64 against ia64_execute) this closes
ia64_execute = sem_ia64 = proved rule = generated table = native code.

    $T1_Z3_VENV/bin/python -I tests/tier1/rules/table_conformance.py
        --runner build/tier1-table-runner [--cases N] [--seed S] [--jobs J] [--only REGEX]
"""

import argparse
import importlib.util
import multiprocessing
import os
import random
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


rulelib = load("rulelib", os.path.join(ROOT, "engine", "tier1", "rules", "rulelib.py"))
MASK64 = 2**64 - 1
STATUS = {"CF": 0, "PF": 2, "AF": 4, "ZF": 6, "SF": 7, "OF": 11}
STATUS_MASK = sum(1 << b for b in STATUS.values())
EDGE_IMMS = [
    0,
    1,
    2,
    7,
    8,
    31,
    32,
    63,
    64,
    65,
    127,
    128,
    255,
    -1,
    -2,
    -63,
    -64,
    2**31 - 1,
    -(2**31),
    2**31,
    2**32 - 1,
    2**63 - 1,
    -(2**63),
]


# ---------------------------------------------------------------- immediates


def accepted(rule, env):
    """The rule text's acceptance: each pattern binder within its `when`
    domain or its layout's domain (the table's imm_min/imm_max/imm_set),
    and every relation check."""
    for k in rule.imms:
        dom = rule.when.get(k) or rulelib.imm_domain(rule.imm_layout[k])
        v = env[k]
        if isinstance(dom, list):
            if v not in dom:
                return False
        elif not dom[0] <= v <= dom[1]:
            return False
    return all(lo <= rulelib.eval_expr(e, env) <= hi for e, lo, hi in rule.checks)


def ir_valid(rule, k, rng):
    """A random immediate of binder k that the IR can carry (ir_ops.def
    layout invariant), not necessarily accepted by the rule."""
    layout = rule.imm_layout[k]
    dom = rule.when.get(k)
    if layout == "FIELD":
        if rng.random() < 0.4:  # extensions and fields at the edges
            pos = rng.choice((0, 0, 0, 1, 8, 31, 32, 63))
            length = rng.choice((8, 16, 32, 64 - pos, 65 - pos, 63 - pos, 1, 64))
            if 1 <= length <= 64:
                return pos | length << 8
        return rng.randrange(64) | rng.randrange(1, 65) << 8
    if layout == "ACCESS":
        return rng.choice((1, 2, 4, 8)) | rng.randrange(2) << 8 | rng.randrange(64) << 16
    if layout == "COUNT":
        return rng.randrange(64)
    if layout == "WIDTH":
        return rng.choice((8, 16, 32))
    if layout == "VALUE":
        if dom is not None and rng.random() < 0.7:
            if isinstance(dom, list):
                return rng.choice(dom)
            lo, hi = dom
            r = rng.random()
            if r < 0.3:
                return rng.choice(
                    [x for x in (lo, hi, lo + 1, hi - 1, 0, 1, -1) if lo <= x <= hi] or [lo]
                )
            return rng.randrange(lo, hi + 1)
        if rng.random() < 0.5:
            return rng.choice(EDGE_IMMS)
        return rulelib.to_signed(rng.getrandbits(64))
    raise SystemExit("table_conformance.py: no generator for layout %s" % layout)


def immediates(rule, rng, want_accepted, ir_ops):
    """An environment the rule accepts (rejection sampling), or any IR-valid
    one."""
    for _ in range(64):
        env = {k: ir_valid(rule, k, rng) for k in rule.imms}
        if not want_accepted or accepted(rule, env):
            return env
    # Coupled fields (e.g. complementary rotation counts) are sparse.
    # Solve the existing architectural preconditions, pinning one sampled
    # binder when possible to keep exercising different legal fields.
    pre, variables = prove.immediates(rule, ir_ops)
    solver = z3.Solver()
    solver.add(*pre)
    if rule.imms:
        key = rng.choice(rule.imms)
        solver.push()
        solver.add(variables[key] == ir_valid(rule, key, rng))
        if solver.check() != z3.sat:
            solver.pop()
    if solver.check() != z3.sat:
        raise SystemExit("table_conformance.py: %s: no accepted immediates exist" % rule.name)
    model = solver.model()
    return {
        k: rulelib.to_signed(model.eval(v, model_completion=True).as_long())
        for k, v in variables.items()
    }


# ---------------------------------------------------------------- isel


def isel_model(rule, ir_ops):
    """z3 inputs (ins, imms, memory word) and expected values: the outputs
    (0/1 for I1), the branch decision, the memory word after the rule."""
    imm = {k: z3.BitVec("imm_" + k[1:], 64) for k in rule.imms}
    ins = {b: z3.BitVec("in_" + b[1:], 64) for b in rule.ins}
    word = z3.BitVec("mem", 64)
    results = {}

    def val(node):
        if node.op is None:
            return ins[node.binder]
        if node.index not in results:
            kids = [val(k) for k in node.kids]
            packed = node.packed if node.packed is not None else imm.get(node.imm_binder, 0)
            if node.op == "BRANCH":
                results[node.index] = (kids[0],)
            elif node.op == "LOAD":
                results[node.index] = (sem_ia64.LOAD_VALUE(packed & 255, word), None)
            elif node.op == "STORE":
                results[node.index] = (sem_ia64.STORE_VALUE(packed & 255, word, kids[1]),)
            else:
                results[node.index] = sem_ia64.evaluate(node.op, packed, kids)
        names = [r[0] for r in ir_ops[node.op].results] or [None]
        res = node.result or names[0]
        return results[node.index][names.index(res)]

    root = val(rule.match)
    outs = []
    for b, _, _ in rule.outputs:
        node, res = rule.output_node[b]
        names = [r[0] for r in ir_ops[node.op].results]
        outs.append(sem_ia64.as_bv(results[node.index][names.index(res)]))
    decision = sem_ia64.b1(root) if rule.is_branch() else z3.BoolVal(False)
    after = root if rule.match.op == "STORE" else word
    inputs = [ins[b] for b in rule.ins] + [imm[k] for k in rule.imms] + [word]
    return inputs, outs + [decision, after]


def isel_worker(job):
    names, runner, ncases, seed = job
    rules, folds, ir_ops = rulelib.load_all()
    by_name = {r.name: (i, r) for i, r in enumerate(rules)}
    out = []
    for name in names:
        idx, rule = by_name[name]
        rng = random.Random("%s:isel:%s" % (seed, name))
        inputs, exprs = isel_model(rule, ir_ops)
        fn = compile_fn(inputs, exprs)
        tied = {i for _, i in rule.ties}
        lines, cases = [], []
        for i in range(ncases):
            env = immediates(rule, rng, rng.random() < 0.9, ir_ops)
            ins = []
            for b in rule.ins:
                ins.append(rng.randrange(2) if rule.types[b] == "I1" else value(rng))
            if len(ins) > 1 and rng.random() < 0.1:
                ins[1] = ins[0]  # equal operands
            status = sum(1 << bit for bit in STATUS.values() if rng.random() < 0.5)
            mem = value(rng)
            lines.append(
                "%x %x %s %x %s %x %x\n"
                % (
                    idx,
                    len(rule.imms),
                    " ".join("%x" % (env[k] & MASK64) for k in rule.imms),
                    len(ins),
                    " ".join("%x" % v for v in ins),
                    status,
                    mem,
                )
            )
            cases.append((env, ins, status, mem))
        r = subprocess.run(
            [runner, "isel"],
            input="".join(lines),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if r.returncode != 0:
            out.append((name, 0, 0, ["runner failed: %s" % r.stderr[:2000]]))
            continue
        got = r.stdout.split("\n")
        fails, executed = [], 0
        for i, (env, ins, status, mem) in enumerate(cases):
            g = got[i].split()
            want_acc = accepted(rule, env)
            if (g[0] == "1") != want_acc:
                fails.append(
                    "acceptance: table %s, rule text %s for %s" % (g[0], int(want_acc), env)
                )
                continue
            if g[0] == "0":
                continue
            executed += 1
            exp = fn(ins + [env[k] & MASK64 for k in rule.imms] + [mem])
            vals = [int(x, 16) for x in g[1:]]
            label, flags_after = vals[0], vals[1]
            nout = len(rule.outputs)
            outs, ins_after, mem_after = (
                vals[2 : 2 + nout],
                vals[2 + nout : 2 + nout + len(ins)],
                vals[-1],
            )
            bad = []
            for j, (b, _, _) in enumerate(rule.outputs):
                if outs[j] != exp[j]:
                    bad.append("%s want %x got %x" % (b, exp[j], outs[j]))
            if rule.is_branch():
                want = 1 if exp[nout] else 2
                if label != want:
                    bad.append("label want %d got %d" % (want, label))
            elif label != 0:
                bad.append("value rule left through label %d" % label)
            for j, b in enumerate(rule.ins):
                if b not in tied and ins_after[j] != ins[j]:
                    bad.append("input %s changed %x -> %x" % (b, ins[j], ins_after[j]))
            if rule.flags == "preserved" and flags_after != status:
                bad.append("flags %x -> %x in a preserved rule" % (status, flags_after))
            if mem_after != exp[nout + 1]:
                bad.append("memory want %x got %x" % (exp[nout + 1], mem_after))
            if bad and len(fails) < 6:
                fails.append("%s; imm %s in %s" % ("; ".join(bad), env, ["%x" % v for v in ins]))
            elif bad:
                fails.append(None)
        out.append((name, ncases, executed, fails))
    return out


# ---------------------------------------------------------------- fold


def fold_model(rule, ir_ops):
    imm = {k: z3.BitVec("imm_" + k[1:], 64) for k in rule.imms}
    ins = {b: z3.BitVec("in_" + b[1:], 64) for b in rule.ins}

    def result_of(op, res, vals):
        return vals[[r[0] for r in ir_ops[op].results].index(res)]

    def pattern(node):
        if node.op is None:
            return ins[node.binder]
        kids = [pattern(k) for k in node.kids]
        packed = node.packed if node.packed is not None else imm.get(node.imm_binder, 0)
        return result_of(
            node.op,
            node.result or ir_ops[node.op].results[0][0],
            sem_ia64.evaluate(node.op, packed, kids),
        )

    def build(node):
        if node.op is None:
            return ins[node.binder]
        kids = [build(k) for k in node.kids]
        e = node.imm_expr
        packed = 0 if e is None else e[1] if e[0] == "lit" else prove.zexpr(e, imm)
        if node.op == "CONST":
            return sem_ia64.CONST(packed)
        return result_of(
            node.op,
            node.result or ir_ops[node.op].results[0][0],
            sem_ia64.evaluate(node.op, packed, kids),
        )

    exprs = [sem_ia64.as_bv(pattern(rule.match)), sem_ia64.as_bv(build(rule.result))]
    exprs += [
        prove.zexpr(n.imm_expr, imm)
        for n in rule.res
        if n.op is not None and n.imm_expr is not None
    ]
    inputs = [ins[b] for b in rule.ins] + [imm[k] for k in rule.imms]
    return inputs, exprs


def fold_worker(job):
    names, runner, ncases, seed = job
    rules, folds, ir_ops = rulelib.load_all()
    by_name = {r.name: (i, r) for i, r in enumerate(folds)}
    op_ids = {name: i for i, name in enumerate(ir_ops)}
    out = []
    for name in names:
        idx, rule = by_name[name]
        rng = random.Random("%s:fold:%s" % (seed, name))
        inputs, exprs = fold_model(rule, ir_ops)
        fn = compile_fn(inputs, exprs)
        lines, cases = [], []
        for i in range(ncases):
            env = immediates(rule, rng, rng.random() < 0.9, ir_ops)
            ins = [rng.randrange(2) if rule.types[b] == "I1" else value(rng) for b in rule.ins]
            lines.append(
                "%x %x %s\n"
                % (idx, len(rule.imms), " ".join("%x" % (env[k] & MASK64) for k in rule.imms))
            )
            cases.append((env, ins))
        r = subprocess.run(
            [runner, "fold"],
            input="".join(lines),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if r.returncode != 0:
            out.append((name, 0, 0, ["runner failed: %s" % r.stderr[:2000]]))
            continue
        # The table's tree must be the rule text's (op, result, kids, bind).
        want_nodes = []
        for n in rule.res:
            if n.op is None:
                want_nodes.append((0xFFFF, 0, 0, (0, 0, 0), rule.ins.index(n.binder), 0))
            else:
                res = n.result or ir_ops[n.op].results[0][0]
                kids = ([k.index for k in n.kids] + [0, 0, 0])[:3]
                want_nodes.append(
                    (
                        op_ids[n.op],
                        [x[0] for x in ir_ops[n.op].results].index(res),
                        len(n.kids),
                        tuple(kids),
                        0xFF,
                        int(n.imm_expr is not None),
                    )
                )
        got = r.stdout.split("\n")
        fails, executed = [], 0
        for i, (env, ins) in enumerate(cases):
            g = got[i].split()
            want_acc = accepted(rule, env)
            if (g[0] == "1") != want_acc:
                fails.append(
                    "acceptance: table %s, rule text %s for %s" % (g[0], int(want_acc), env)
                )
                continue
            if g[0] == "0":
                continue
            executed += 1
            vals = [int(x, 16) for x in g[1:]]
            nres = vals[0]
            nodes = [vals[1 + 9 * j : 10 + 9 * j] for j in range(nres)]
            exp = fn(ins + [env[k] & MASK64 for k in rule.imms])
            bad = []
            if nres != len(want_nodes):
                bad.append("%d result nodes, rule text %d" % (nres, len(want_nodes)))
            else:
                imms = iter(exp[2:])
                for j, (node, want) in enumerate(zip(nodes, want_nodes)):
                    op, res, nkids, k0, k1, k2, bind, has, imm = node
                    if (op, res, nkids, (k0, k1, k2), bind, has) != want:
                        bad.append("node %d is %s, rule text %s" % (j, node[:8], want))
                    elif has:
                        w = next(imms)
                        if imm != w or (rulelib.eval_expr(rule.res[j].imm_expr, env) & MASK64) != w:
                            bad.append("node %d immediate %x, prover %x" % (j, imm, w))
            if exp[0] != exp[1]:
                bad.append("pattern %x != result %x" % (exp[0], exp[1]))
            if bad and len(fails) < 6:
                fails.append("%s; imm %s in %s" % ("; ".join(bad), env, ["%x" % v for v in ins]))
            elif bad:
                fails.append(None)
        out.append((name, ncases, executed, fails))
    return out


# ---------------------------------------------------------------- iexpr


def iexpr_check(runner, ncases, seed):
    ops = ["add", "sub", "and", "or", "xor", "shl", "shru", "shr", "eq", "lt", "ltu"]
    a, b = z3.BitVec("a", 64), z3.BitVec("b", 64)
    imm = {"#a": a, "#b": b}
    fns = {
        op: compile_fn([a, b], [prove.zexpr((op, ("imm", "#a"), ("imm", "#b")), imm)]) for op in ops
    }
    rng = random.Random("%s:iexpr" % seed)
    lines, cases = [], []
    for i in range(ncases):
        op = ops[i % len(ops)]
        x = value(rng)
        y = rng.randrange(0, 200) if op.startswith("sh") and rng.random() < 0.7 else value(rng)
        if rng.random() < 0.1:
            y = x
        # enum t1_iexpr_op: LIT, IMM, then the operators in EXPR_OPS order.
        code = 2 + list(rulelib.EXPR_OPS).index(op)
        lines.append("%x %x %x\n" % (code, x, y))
        cases.append((op, x, y))
    r = subprocess.run(
        [runner, "iexpr"],
        input="".join(lines),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if r.returncode != 0:
        return ["runner failed: %s" % r.stderr[:2000]]
    got = r.stdout.split("\n")
    fails = []
    for i, (op, x, y) in enumerate(cases):
        want = fns[op]([x, y])[0]
        twin = rulelib.eval_expr((op, ("lit", x), ("lit", y)), {}) & MASK64
        if int(got[i], 16) != want or twin != want:
            fails.append("%s %x %x: C %s, prover %x, rulelib %x" % (op, x, y, got[i], want, twin))
    return fails


# ---------------------------------------------------------------- coverage


def coverage(rules, ir_ops, rng):
    """Every op the lifter emits that isel must cover (pure ops except
    LIVEIN, the BRANCH terminator, LOAD/STORE sites), for every IR-valid
    immediate, is matched by a rule whose pattern is that one node over $x
    leaves -- the fallback when maximal munch cuts every operand
    (otherwise the region would end before the instruction)."""
    missing = []
    singles = {}
    for r in rules:
        if all(k.op is None for k in r.match.kids):
            singles.setdefault(r.match.op, []).append(r)
    for name, op in ir_ops.items():
        # isel.c lowers these home/entry-image snapshots directly to MIR
        # (like LIVEIN); their semantics are checked by the compiler and
        # restoration suites, not an isel template over operand leaves.
        if name in ("LIVEIN", "HREAD", "NATIN") or (
            op.effect != "PURE" and name not in ("BRANCH", "LOAD", "STORE")
        ):
            continue
        if op.imm == "FIELD":
            values = [p | l << 8 for p in range(64) for l in range(1, 65)]
        elif op.imm == "COUNT":
            values = list(range(64))
        elif op.imm == "WIDTH":
            values = [8, 16, 32]
        elif op.imm == "CMP":
            values = [rel | w << 8 for rel in (0, 1, 2) for w in (32, 64)]
        elif op.imm == "ACCESS":
            values = [b | o << 8 | s << 16 for b in (1, 2, 4, 8) for o in (0, 1) for s in (0, 63)]
        elif op.imm == "VALUE":
            values = EDGE_IMMS + [rulelib.to_signed(rng.getrandbits(64)) for _ in range(200)]
        else:
            values = [None]
        for v in values:
            hit = False
            for r in singles.get(name, []):
                node = r.match
                if node.packed is not None:
                    hit = node.packed == v
                elif node.imm_binder is None:
                    hit = True
                else:
                    hit = accepted(r, {node.imm_binder: v})
                if hit:
                    break
            if not hit:
                missing.append("%s imm %s" % (name, v))
    return missing


# ---------------------------------------------------------------- driver


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runner", required=True)
    ap.add_argument("--cases", type=int, default=3000, help="cases per isel and fold rule")
    ap.add_argument("--seed", default="tier1-table-conformance")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only", help="regex on rule names")
    args = ap.parse_args()
    runner = os.path.abspath(args.runner)
    rules, folds, _ = rulelib.load_all()
    if list(rulelib.EXPR_OPS) != [
        "add",
        "sub",
        "and",
        "or",
        "xor",
        "shl",
        "shru",
        "shr",
        "eq",
        "lt",
        "ltu",
    ]:
        raise SystemExit("table_conformance.py: EXPR_OPS order differs from enum t1_iexpr_op")
    pick = (lambda n: re.search(args.only, n)) if args.only else (lambda n: True)
    supported = set(subprocess.check_output([runner, "supported"], text=True).splitlines())
    inames = [r.name for r in rules if pick(r.name) and r.name in supported]
    fnames = [r.name for r in folds if pick(r.name)]
    skipped = [r.name for r in rules if pick(r.name) and r.name not in supported]
    for name in skipped:
        print("table-conformance: isel %-24s SKIPPED (host ISA unavailable)" % name)
    print("table-conformance: %d isel rules skipped" % len(skipped))
    if not inames and not fnames:
        print("table-conformance: no selected rules executed")
        return 0
    # An all-skipped selection needs no solver and must not claim a pass.
    global z3, sem_ia64, conformance, prove, compile_fn, value
    try:
        import z3
    except ImportError:
        sys.stderr.write(
            "table_conformance.py: z3 is not importable; run it with $T1_Z3_VENV/bin/python\n"
        )
        return 1
    sem_ia64 = load("sem_ia64", os.path.join(HERE, "sem_ia64.py"))
    conformance = load("conformance", os.path.join(HERE, "conformance.py"))
    prove = load("prove", os.path.join(HERE, "prove.py"))
    compile_fn, value = conformance.compile_fn, conformance.value
    chunks = max(1, args.jobs)
    ijobs = [
        (inames[i::chunks], runner, args.cases, args.seed)
        for i in range(chunks)
        if inames[i::chunks]
    ]
    fjobs = [
        (fnames[i::chunks], runner, args.cases, args.seed)
        for i in range(chunks)
        if fnames[i::chunks]
    ]
    ok = True
    with ProcessPoolExecutor(
        max_workers=args.jobs, mp_context=multiprocessing.get_context("fork")
    ) as pool:
        ires = [r for rs in pool.map(isel_worker, ijobs) for r in rs]
        fres = [r for rs in pool.map(fold_worker, fjobs) for r in rs]
    totals = {"isel": [0, 0], "fold": [0, 0]}
    for kind, res in (("isel", ires), ("fold", fres)):
        for name, n, executed, fails in sorted(res):
            totals[kind][0] += n
            totals[kind][1] += executed
            status = "ok" if not fails else "FAILED (%d)" % len(fails)
            if fails or executed < n // 2:
                ok = False
                if not fails:
                    status = "FAILED (only %d of %d cases accepted)" % (executed, n)
            print(
                "table-conformance: %s %-24s %6d cases %6d executed %s"
                % (kind, name, n, executed, status)
            )
            for x in fails[:6]:
                if x:
                    print("    " + x)
    missing = coverage(rules, rulelib.load_ir_ops(), random.Random(args.seed + ":coverage"))
    print(
        "table-conformance: coverage: every op and IR-valid immediate has a single-node rule: %s"
        % ("ok" if not missing else "MISSING %d: %s" % (len(missing), ", ".join(missing[:12])))
    )
    ok &= not missing
    ifails = iexpr_check(runner, 20000, args.seed)
    print(
        "table-conformance: iexpr t1_iexpr_eval vs prover: 20000 cases %s"
        % ("ok" if not ifails else "FAILED (%d)" % len(ifails))
    )
    for x in ifails[:8]:
        print("    " + x)
    ok &= not ifails
    print(
        "table-conformance: %d isel rules (%d cases, %d executed natively), %d fold rules (%d cases, "
        "%d instantiated), %d isel rules skipped: %s"
        % (
            len(ires),
            totals["isel"][0],
            totals["isel"][1],
            len(fres),
            totals["fold"][0],
            totals["fold"][1],
            len(skipped),
            "passed" if ok else "FAILED",
        )
    )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
