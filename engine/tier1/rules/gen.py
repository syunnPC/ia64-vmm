# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate Tier1 instruction selection, fold and floating-point rule tables."""

import argparse
import importlib.util
import os
import sys


def load_rulelib():
    # -I drops the script directory from sys.path: import by path.
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rulelib.py")
    spec = importlib.util.spec_from_file_location("rulelib", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


rulelib = load_rulelib()


class Exprs:
    """A rule's expression array (rules.h struct t1_iexpr): nodes in
    evaluation order, shared subexpressions emitted once."""

    def __init__(self, rule):
        self.rule, self.nodes, self.memo = rule, [], {}

    def add(self, e):
        if e in self.memo:
            return self.memo[e]
        if e[0] == "lit":
            node = ("T1_IX_LIT", 0, 0, e[1])
        elif e[0] == "imm":
            node = ("T1_IX_IMM", self.rule.imms.index(e[1]), 0, 0)
        else:
            a, b = self.add(e[1]), self.add(e[2])
            node = (rulelib.EXPR_OPS[e[0]], a, b, 0)
        if len(self.nodes) >= 32:
            raise rulelib.RuleError("rule %s: more than 32 expression nodes" % self.rule.name)
        self.nodes.append(node)
        self.memo[e] = len(self.nodes) - 1
        return self.memo[e]

    def c(self, name):
        return "static const struct t1_iexpr %s[] = {\n    %s\n};" % (
            name,
            ",\n    ".join(
                "{%s, %d, %d, %s}" % (op, a, b, c_i64(imm)) for op, a, b, imm in self.nodes
            ),
        )


def c_pat(rule, node, nodes_out, root_result="0"):
    """Preorder t1_pat initializers for the pattern."""
    if node.op is None:
        nodes_out.append(
            "{T1_PAT_VALUE, 0, 0, {0, 0, 0}, %d, 0, 0, 0}" % rule.ins.index(node.binder)
        )
        return
    kids_at = []
    me = len(nodes_out)
    nodes_out.append(None)
    for kid in node.kids:
        kids_at.append(len(nodes_out))
        c_pat(rule, kid, nodes_out)
    kid_txt = ", ".join(str(k) for k in (kids_at + [0, 0, 0])[:3])
    result = root_result if node is rule.match else result_index(rule, node)
    bind = rule.imms.index(node.imm_binder) if node.imm_binder else "T1_BINDER_NONE"
    if node.packed is not None:
        lo = hi = node.packed
        imm_set = 0
    elif node.imm_binder:
        dom = rule.when.get(node.imm_binder) or rulelib.imm_domain(rule.ir_ops[node.op].imm)
        if isinstance(dom, list):
            lo, hi = min(dom), max(dom)
            if hi > 31:
                raise rulelib.RuleError("rule %s: imm set beyond 31" % rule.name)
            imm_set = sum(1 << v for v in dom)
        else:
            lo, hi = dom
            imm_set = 0
    else:
        lo, hi, imm_set = 0, 0, 0
    nodes_out[me] = "{T1_IR_%s, %s, %d, {%s}, %s, %s, %s, UINT32_C(%d)}" % (
        node.op,
        result,
        len(node.kids),
        kid_txt,
        bind,
        c_i64(lo),
        c_i64(hi),
        imm_set,
    )


def c_i64(v):
    """An int64_t constant; INT64_C(-9223372036854775808) is not one (the
    literal overflows before the minus applies)."""
    if not -(2**63) <= v < 2**63:
        raise rulelib.RuleError("immediate %d does not fit int64_t" % v)
    return "(-INT64_C(9223372036854775807) - 1)" if v == -(2**63) else "INT64_C(%d)" % v


def result_index(rule, node):
    """The result index of node as consumed by its parent (0 for the root)."""
    if node is rule.match or node.op == "BRANCH":
        return "0"
    op = rule.ir_ops[node.op]
    name = node.result or op.results[0][0]
    return "T1_RES_%s_%s" % (node.op, name)


def binder_id(rule, b):
    if b is None:
        return "T1_BINDER_NONE"
    kind = rule.binder_kind(b)
    return "T1_BINDER_ID(%s, %d)" % (
        {"IN": "T1_OP_IN", "OUT": "T1_OP_OUT", "TEMP": "T1_OP_TEMP", "WINDOW": "T1_OP_WINDOW"}[
            kind
        ],
        rule.binder_index(b),
    )


def c_opnd(rule, kind, value, exprs):
    if kind in ("reg", "cl"):
        k = rule.binder_kind(value)
        return "{T1_OP_%s, %d, 0}" % (k, rule.binder_index(value))
    if kind == "label":
        return "{T1_OP_LABEL, %d, 0}" % rule.labels.index(value)
    if kind == "imm":
        if isinstance(value, int):
            return "{T1_OP_IMM, T1_BINDER_NONE, %s}" % c_i64(value)
        if isinstance(value, tuple):
            return "{T1_OP_IMM, T1_IMM_EXPR(%d), 0}" % exprs.add(value)
        return "{T1_OP_IMM, %d, 0}" % rule.imms.index(value)
    return "{T1_OP_MEM, 0, 0}"


def c_tmpl(rule, insn, exprs):
    opnds = [c_opnd(rule, k, v, exprs) for k, v in insn.operands]
    mem = [v for k, v in insn.operands if k == "mem"]
    if mem:
        m = mem[0]
        scale = (
            "T1_SCALE_FROM_IMM(%d)" % rule.imms.index(m.scale)
            if isinstance(m.scale, str)
            else str({1: 0, 2: 1, 4: 2, 8: 3}[m.scale])
        )
        disp_expr = (
            "T1_IMM_EXPR(%d)" % exprs.add(m.disp_expr)
            if m.disp_expr is not None
            else "T1_TMPL_DISP_NONE"
        )
        mem_txt = "%s, %s, %s, %d, %s" % (
            binder_id(rule, m.base),
            binder_id(rule, m.index),
            scale,
            m.disp,
            disp_expr,
        )
    else:
        mem_txt = "T1_BINDER_NONE, T1_BINDER_NONE, 0, 0, T1_TMPL_DISP_NONE"
    return "{%s, %d, {%s}, %s}" % (
        insn.op.enum,
        len(opnds),
        ", ".join(opnds) or "{0, 0, 0}",
        mem_txt,
    )


def c_checks(rule, exprs, name):
    items = ["{%d, %s, %s}" % (exprs.add(e), c_i64(lo), c_i64(hi)) for e, lo, hi in rule.checks]
    if not items:
        return None
    return "static const struct t1_icheck %s[] = { %s };" % (name, ", ".join(items))


HEADER = [
    '#include "engine/tier1/rules.h"',
    "#define T1_RULES_FORMAT_GENERATED %d" % rulelib.RULES_FORMAT,
    "_Static_assert(T1_RULES_FORMAT_GENERATED == T1_RULES_FORMAT,",
    '               "rules.h and the generated rule table disagree on the format");',
]


def generate(rules):
    out = (
        [
            "/* Generated by engine/tier1/rules/gen.py from engine/tier1/rules/isel.rules;",
            " * Do not edit generated tables. */",
            "#ifndef IA64VMM_TIER1_ISEL_RULES_H",
            "#define IA64VMM_TIER1_ISEL_RULES_H",
        ]
        + HEADER
        + ['#include "engine/tier1/x86enc.h"', ""]
    )
    table = []
    for i, r in enumerate(rules):
        nodes = []
        c_pat(r, r.match, nodes)
        exprs = Exprs(r)
        out.append("/* %s */" % r.name)
        out.append(
            "static const struct t1_pat t1_rule%d_pat[] = {\n    %s\n};"
            % (i, ",\n    ".join(nodes))
        )
        tmpl = ",\n    ".join(c_tmpl(r, insn, exprs) for insn in r.emit)
        checks = c_checks(r, exprs, "t1_rule%d_check" % i)
        out.append("static const struct t1_tmpl t1_rule%d_tmpl[] = {\n    %s\n};" % (i, tmpl))
        outs = []
        for b, _, _ in r.outputs:
            node, res = r.output_node[b]
            outs.append(
                "{T1_IR_%s, T1_RES_%s_%s, 0, {0, 0, 0}, %d, 0, 0, 0}"
                % (node.op, node.op, res, node.index)
            )
        ties = ["{%d, %d}" % (r.binder_index(o), r.binder_index(n)) for o, n in r.ties]
        fixed = ["{%s, T1_%s}" % (binder_id(r, b), reg.upper()) for b, reg in r.fixed.items()]
        for name, items, typ in (
            ("out", outs, "t1_pat"),
            ("tie", ties, "t1_tie"),
            ("fixed", fixed, "t1_fixed"),
        ):
            if items:
                out.append(
                    "static const struct %s t1_rule%d_%s[] = { %s };"
                    % (typ, i, name, ", ".join(items))
                )
        if exprs.nodes:
            out.append(exprs.c("t1_rule%d_expr" % i))
        if checks:
            out.append(checks)
        roots = [
            "(1u << T1_RES_%s_%s)" % (node.op, res)
            for node, res in r.output_node.values()
            if node.index == 0
        ]
        required_isa = ["T1_X86_REQUIRED_ISA(%s)" % insn.op.enum for insn in r.emit]
        table.append(
            '    {"%s", %d, %d, %d, %d, %d, %d, %d, %d, %s, %d, %s, %s,\n     t1_rule%d_pat, t1_rule%d_tmpl, %s, %s, %s,\n'
            "     %d, %d, %s, %s}"
            % (
                r.name,
                r.cost,
                len(nodes),
                len(r.emit),
                len(r.outputs),
                len(r.temps),
                len(r.labels),
                len(r.ties),
                len(r.fixed),
                rulelib.FLAG_KINDS[r.flags],
                len(r.ins),
                " | ".join(roots) or "0",
                " | ".join(required_isa) or "0",
                i,
                i,
                "t1_rule%d_out" % i if outs else "NULL",
                "t1_rule%d_tie" % i if ties else "NULL",
                "t1_rule%d_fixed" % i if fixed else "NULL",
                len(exprs.nodes),
                len(r.checks),
                "t1_rule%d_expr" % i if exprs.nodes else "NULL",
                "t1_rule%d_check" % i if checks else "NULL",
            )
        )
        out.append("")
    out.append("const struct t1_rule t1_rules[] = {\n%s\n};" % ",\n".join(table))
    out.append("const unsigned t1_rule_count = %d;" % len(rules))
    # Covers use exact roots or a value binder, preserving table order.
    # LOAD/STORE rules are selected separately for sites, not as covers.
    ids, ranges = [], []
    for op in rules[0].ir_ops if rules else []:
        group = [
            i
            for i, r in enumerate(rules)
            if r.match.op not in ("LOAD", "STORE") and r.match.op in (None, op)
        ]
        ranges.append("        [T1_IR_%s] = {%d, %d}" % (op, len(ids), len(group)))
        ids.extend(group)
    if len(rules) > 65535 or len(ids) > 65535:
        raise rulelib.RuleError("too many isel candidates for the opcode index")
    out += [
        "",
        "/* Immutable cover candidates, in the same order as t1_rules. */",
        "static inline const uint16_t *t1_isel_candidates(unsigned op, unsigned *count)",
        "{",
        "    static const uint16_t rules[] = { %s };" % ", ".join(map(str, ids or [0])),
        "    static const uint16_t ranges[T1_IR_OP_COUNT][2] = {",
        ",\n".join(ranges) if ranges else "        {0, 0}",
        "    };",
        "    if (op >= T1_IR_OP_COUNT || !t1_rule_count) { *count = 0; return NULL; }",
        "    *count = ranges[op][1];",
        "    return rules + ranges[op][0];",
        "}",
    ]
    out.append("#endif")
    return "\n".join(out) + "\n"


def c_res(rule, exprs, out_nodes):
    """Preorder t1_fold_node initializers of the result tree."""
    for n in rule.res:
        if n.op is None:
            out_nodes.append(
                "{T1_PAT_VALUE, 0, 0, {0, 0, 0}, %d, T1_BINDER_NONE}" % rule.ins.index(n.binder)
            )
            continue
        op = rule.ir_ops[n.op]
        res = "T1_RES_%s_%s" % (n.op, n.result or op.results[0][0])
        kids = ", ".join(str(k) for k in ([k.index for k in n.kids] + [0, 0, 0])[:3])
        imm = (
            "T1_IMM_EXPR(%d)" % exprs.add(n.imm_expr)
            if n.imm_expr is not None
            else "T1_BINDER_NONE"
        )
        out_nodes.append(
            "{T1_IR_%s, %s, %d, {%s}, T1_BINDER_NONE, %s}" % (n.op, res, len(n.kids), kids, imm)
        )


def generate_fold(folds):
    out = (
        [
            "/* Generated by engine/tier1/rules/gen.py from engine/tier1/rules/fold.rules;",
            " * Do not edit generated tables.",
            " * The tables are static: any number of translation units may include this. */",
            "#ifndef IA64VMM_TIER1_FOLD_RULES_H",
            "#define IA64VMM_TIER1_FOLD_RULES_H",
        ]
        + HEADER
        + [""]
    )
    table = []
    for i, r in enumerate(folds):
        nodes = []
        c_pat(r, r.match, nodes, "T1_RES_%s_%s" % (r.match.op, r.root_result))
        exprs = Exprs(r)
        res = []
        c_res(r, exprs, res)
        checks = c_checks(r, exprs, "t1_fold%d_check" % i)
        out.append("/* %s */" % r.name)
        out.append(
            "static const struct t1_pat t1_fold%d_pat[] = {\n    %s\n};"
            % (i, ",\n    ".join(nodes))
        )
        out.append(
            "static const struct t1_fold_node t1_fold%d_res[] = {\n    %s\n};"
            % (i, ",\n    ".join(res))
        )
        if exprs.nodes:
            out.append(exprs.c("t1_fold%d_expr" % i))
        if checks:
            out.append(checks)
        table.append(
            '    {"%s", %d, %d, %d, %d, t1_fold%d_pat, t1_fold%d_res, %s, %s}'
            % (
                r.name,
                len(nodes),
                len(res),
                len(exprs.nodes),
                len(r.checks),
                i,
                i,
                "t1_fold%d_expr" % i if exprs.nodes else "NULL",
                "t1_fold%d_check" % i if checks else "NULL",
            )
        )
        out.append("")
    out.append("static const struct t1_fold_rule t1_fold_rules[] = {\n%s\n};" % ",\n".join(table))
    out.append("static const unsigned t1_fold_rule_count = %d;" % len(folds))
    # opt.c implements only leaf replacements. Group exactly those rules
    # by their pattern root, preserving their original first-match order.
    # Keep the index local to an inline accessor so other table consumers
    # need not retain it, and no runtime initialization is needed.
    if len(folds) > 65535:
        raise rulelib.RuleError("too many fold rules for the leaf index")
    groups = {}
    for i, r in enumerate(folds):
        if len(r.res) == 1 and r.res[0].op in (None, "CONST"):
            groups.setdefault(r.match.op, []).append(i)
    ids, ranges = [], []
    for op, group in groups.items():
        ranges.append("        [T1_IR_%s] = {%d, %d}" % (op, len(ids), len(group)))
        ids.extend(group)
    out += [
        "",
        "/* Leaf fold candidates, in the same order as t1_fold_rules. */",
        "static inline const uint16_t *t1_fold_leaf_candidates(unsigned op, unsigned *count)",
        "{",
        "    static const uint16_t rules[] = { %s };" % ", ".join(map(str, ids or [0])),
        "    static const uint16_t ranges[T1_IR_OP_COUNT][2] = {",
        ",\n".join(ranges) if ranges else "        {0, 0}",
        "    };",
        "    if (op >= T1_IR_OP_COUNT || !t1_fold_rule_count) { *count = 0; return NULL; }",
        "    *count = ranges[op][1];",
        "    return rules + ranges[op][0];",
        "}",
    ]
    out.append("#endif")
    return "\n".join(out) + "\n"


PRECISION = {"s": 1, "d": 2, "none": 0}
ADDEND = {"any": "T1_FPX_ANY", "f0": "T1_FPX_F0", "reg": "T1_FPX_REG"}


def fpx_imm(exprs, v):
    """(expr index text, literal) of an expansion immediate."""
    if isinstance(v, int):
        return "T1_FPX_NO_EXPR", c_i64(rulelib.to_signed(v))
    e = ("imm", v) if isinstance(v, str) else v
    return str(exprs.add(e)), "INT64_C(0)"


def fpx_insn(rule, insn, exprs):
    none = ("T1_FPX_NO_EXPR", "INT64_C(0)")
    if insn.kind == "label":
        return "{T1_FPX_LABEL, 0, 0, 0, 0, T1_FPX_NO_EXPR, %d, INT64_C(0)}" % rule.labels.index(
            insn.label
        )
    if insn.kind == "in":
        return (
            "{T1_FPX_IN, 0, %d, 0, 0, T1_FPX_NO_EXPR, 0, INT64_C(0)}"
            % rulelib.EXPAND_INPUTS.index(insn.input)
        )
    if insn.kind == "out":
        slot = rulelib.EXPAND_SLOTS.index(insn.slot)
        k, v = insn.source
        if k == "reg":
            return "{T1_FPX_OUT_R, 0, %d, 0, 0, T1_FPX_NO_EXPR, 0, INT64_C(0)}" % slot
        if k == "xmm":
            return "{T1_FPX_OUT_X, 0, %d, %d, 0, T1_FPX_NO_EXPR, 0, INT64_C(0)}" % (slot, v)
        e, imm = fpx_imm(exprs, v)
        return "{T1_FPX_OUT_I, 0, %d, 0, 0, %s, 0, %s}" % (slot, e, imm)
    if insn.kind == "sse":
        xmm = [v for k, v in insn.operands if k == "xmm"] + [0, 0, 0]
        if insn.op.form == "RX":
            xmm = [0] + xmm
        imms = [v for k, v in insn.operands if k == "imm"]
        e, imm = fpx_imm(exprs, imms[0]) if imms else none
        return "{T1_FPX_SSE, %s, %d, %d, %d, %s, 0, %s}" % (
            insn.op.enum,
            xmm[0],
            xmm[1],
            xmm[2],
            e,
            imm,
        )
    label = [v for k, v in insn.operands if k == "label"]
    imms = [v for k, v in insn.operands if k == "imm"]
    e, imm = fpx_imm(exprs, imms[0]) if imms else none
    return "{T1_FPX_X86, %s, 0, 0, 0, %s, %d, %s}" % (
        insn.op.enum,
        e,
        rule.labels.index(label[0]) if label else 0,
        imm,
    )


def generate_fpfast(expands):
    out = [
        "/* Generated by engine/tier1/rules/gen.py from fpfast.rules. Do not edit:",
        " * Generated from rule definitions. */",
        "#ifndef IA64VMM_TIER1_FPFAST_RULES_H",
        "#define IA64VMM_TIER1_FPFAST_RULES_H",
        '#include "engine/tier1/fpfast.h"',
    ]
    rows = []
    for r in expands:
        exprs = Exprs(r)
        insns = [fpx_insn(r, i, exprs) for i in r.emit]
        out.append(
            "static const struct t1_fpx_insn t1_fpx_insn_%s[] = {\n    %s\n};"
            % (r.name, ",\n    ".join(insns))
        )
        if exprs.nodes:
            out.append(exprs.c("t1_fpx_expr_%s" % r.name))
        if len(r.labels) > 32 or len(r.emit) > 65535:
            raise rulelib.RuleError("expand %s: more than 32 labels" % r.name)
        x = r.expand
        rows.append(
            '{"%s", IA64_OP_%s, %d, %s, %d, %s, %d, %d, t1_fpx_insn_%s, %s}'
            % (
                r.name,
                x["insn"],
                PRECISION[x["precision"]],
                ADDEND[x["addend"]],
                int(x["host"] == "fma3"),
                "T1_FPX_EXACT" if x["mode"] == "exact" else "T1_FPX_APPROX",
                len(r.labels),
                len(r.emit),
                r.name,
                "t1_fpx_expr_%s" % r.name if exprs.nodes else "NULL",
            )
        )
    out.append("const struct t1_fpx_rule t1_fpx_rules[] = {\n    %s\n};" % ",\n    ".join(rows))
    out.append("const unsigned t1_fpx_rule_count = %d;" % len(rows))
    out.append("#endif")
    return "\n".join(out) + "\n"


def write(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("--fold-out")
    ap.add_argument("--fpfast-out")
    ap.add_argument("--fpfast", default=rulelib.FPFAST_PATH)
    ap.add_argument("--rules", default=rulelib.RULES_PATH)
    ap.add_argument("--fold", default=rulelib.FOLD_PATH)
    args = ap.parse_args()
    try:
        rules, folds, ir_ops = rulelib.load_all(args.rules, args.fold)
        expands = rulelib.load_expand_rules(args.fpfast)
        for r in rules + folds:
            r.ir_ops = ir_ops
        text = generate(rules)
        fold_text = generate_fold(folds)
        fpfast_text = generate_fpfast(expands)
    except rulelib.RuleError as e:
        sys.stderr.write("gen.py: %s\n" % e)
        return 1
    if not args.verify:
        if args.out:
            write(args.out, text)
        if args.fold_out:
            write(args.fold_out, fold_text)
        if args.fpfast_out:
            write(args.fpfast_out, fpfast_text)
    print(
        "gen.py: %d rules, %d fold rules and %d expansions, generated"
        % (len(rules), len(folds), len(expands))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
