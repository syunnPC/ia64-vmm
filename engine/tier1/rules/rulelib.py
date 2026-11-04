# SPDX-License-Identifier: GPL-2.0-or-later
"""The one parser and canonicalizer of Tier1 instruction-selection rules.

gen.py (C tables) and
tests/tier1/rules/prove.py (SMT proofs) both read rules only through this
module.

Inputs: engine/tier1/rules/isel.rules (instruction-selection rules),
engine/tier1/rules/fold.rules (IR -> IR fold rules, §5.1),
engine/tier1/rules/fpfast.rules (FP helper expansions, docs/floating-point.md),
engine/tier1/ir_ops.def (IR ops, operand/result names and types, immediate
layouts) and the T1_X86_OPS table of engine/tier1/x86enc.h (mnemonics,
operand specs, implicit registers, flag columns). Every check here is
static; semantics are the prover's business.

Rule format (format 2 additions marked +):

    rule <name> cost <n>
      match   <pattern>            # (OP attr... operand...)@result; $x values, #k immediates
      when    <predicate>          # (in S v...) | (range S lo hi) | (and p...)
      outputs $x = <IR result>, ...
      temps   $t0 ...
      tie     $out = $in, ...
      fixed   $v = <reg>, ...
      window  $w                   # + LOAD/STORE rules: the site's window delta register
      labels  $T $F
      emit    <mnemonic> <operand>, ...   # one AMD64 instruction per line
              ...
      flags   preserved | clobbered | undefined | dead-after
    end

    fold <name>                    # + fold.rules: a proved IR -> IR rewrite
      match   <pattern>
      when    <predicate>
      result  <IR tree>            # leaves $x; (CONST e); op immediates are expressions
    end

+ Immediate expressions (rules.h, format 2): `#k`, an integer, or
  (add e e...) (sub e e...) (and e e...) (or e e...) (xor e e...)
  (shl e e) (shru e e) (shr e e) (eq e e) (lt e e) (ltu e e), with the sugar
  (pos #f) (len #f) (bytes #m) (neg e) (not e) (ne e e). Shift counts are
  taken modulo 64; comparisons give 0/1. They appear as template
  immediates, as lea displacements ([$a + #k], [$a + (sub 0 #k)]), as op
  immediates and CONST values in fold results, and as the subject S of a
  `when` predicate: a predicate over a pattern binder #k restricts that
  pattern node (t1_pat imm_min/imm_max/imm_set, as in format 1); any other
  subject becomes a relation check (struct t1_icheck) that the matcher
  evaluates after binding.
+ A FIELD node may bind its parts: (EXTRU #p #l $a) binds the packed
  pos | len << 8 to one binder and makes #p = (pos ...) and #l = (len ...)
  expressions over it.
+ LOAD/STORE roots (isel only): (LOAD #m $a)@value, (STORE #m $a $v); MEM
  operands are never written in patterns. The template's one memory access
  is [$a + $w * 1] with $w declared by `window`.

    expand <name>                  # fpfast.rules: a proved FP helper expansion
      insn      <OPCODE>           # FMA FMS FNMA FMPY FADD FSUB FNORM FPMA FPMS FPNMA
      precision s | d | none       # the completer (none: fp_approx, pc 10 only)
      addend    f0 | reg | any     # the f2 field is f0 (a multiply) or another FR
      host      fma3 | base        # host FMA3 required (dispatch honours it)
      mode      exact | approx     # the promise proved (approx: fp_approx only)
      emit      <line>             # straight-line code on $s (R11) and x0-x3
              ...                  # (xmm0-xmm3); .name: defines a forward label
    end

  Expansion lines: x86 instructions of T1_X86_OPS on $s and immediates;
  the SSE ops of x86sse.h (T1_SSE_OPS) on x0-x3 and $s; `in $s, <input>`
  (qp, fpsr, a.sig, a.meta, b.sig, b.meta, c.sig, c.meta, d.sig, d.meta:
  R11 = the value, f0/f1 as their register images); `out <slot>, $s|xN|imm`
  (d.sig, d.meta, mf: the helper's results); jcc/jmp to a later label or
  to $BAIL (the helper call) or $DONE (after it). Immediates may use #sf
  (the status field) and #mf (PSR.mfl or PSR.mfh).

`#` followed by white space or the end of the line starts a comment.
"""

import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
RULES_PATH = os.path.join(ROOT, "engine", "tier1", "rules", "isel.rules")
FOLD_PATH = os.path.join(ROOT, "engine", "tier1", "rules", "fold.rules")
FPFAST_PATH = os.path.join(ROOT, "engine", "tier1", "rules", "fpfast.rules")
X86SSE_PATH = os.path.join(ROOT, "engine", "tier1", "x86sse.h")
IR_OPS_PATH = os.path.join(ROOT, "engine", "tier1", "ir_ops.def")
X86ENC_PATH = os.path.join(ROOT, "engine", "tier1", "x86enc.h")
RULES_FORMAT = 2  # rules.h T1_RULES_FORMAT

X86_REGS = [
    "rax",
    "rcx",
    "rdx",
    "rbx",
    "rsp",
    "rbp",
    "rsi",
    "rdi",
    "r8",
    "r9",
    "r10",
    "r11",
    "r12",
    "r13",
    "r14",
    "r15",
]
# Rules may fix operands only to allocatable registers.
ALLOCATABLE = {"rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "rbx", "rbp", "r15"}
CC_NAMES = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"]
CC_ALIASES = {"z": "e", "nz": "ne", "c": "b", "nc": "ae"}
CMP_RELS = {"eq": 0, "lt": 1, "ltu": 2}  # enum t1_cmp_rel
FLAG_KINDS = {
    "preserved": "T1_RULE_PRESERVED",
    "clobbered": "T1_RULE_CLOBBERED",
    "undefined": "T1_RULE_UNDEFINED",
    "dead-after": "T1_RULE_DEAD_AFTER",
}
KIND_IN, KIND_OUT, KIND_TEMP, KIND_IMM, KIND_LABEL, KIND_WINDOW = (
    "IN",
    "OUT",
    "TEMP",
    "IMM",
    "LABEL",
    "WINDOW",
)
KIND_CODE = {KIND_IN: 0, KIND_OUT: 1, KIND_TEMP: 2, KIND_IMM: 3, KIND_LABEL: 4, KIND_WINDOW: 6}
MASK64 = 2**64 - 1


class RuleError(Exception):
    pass


# ---------------------------------------------------------------- tables


class IROp:
    def __init__(self, name, imm, effect, nopnd, nres):
        self.name, self.imm, self.effect = name, imm, effect
        self.nopnd, self.nres = nopnd, nres
        self.operands, self.results = [], []


def load_ir_ops(path=IR_OPS_PATH):
    ops = {}
    row = re.compile(r"^(T1_IR_OP|T1_IR_OPERAND|T1_IR_RESULT)\(([^()]*)\)\s*$")
    with open(path) as f:
        for line_no, line in enumerate(f, 1):
            m = row.match(line.strip())
            if not m:
                continue
            args = [a.strip() for a in m.group(2).split(",")]
            if m.group(1) == "T1_IR_OP":
                name, imm, effect, nopnd, nres = args
                if name in ops:
                    raise RuleError("ir_ops.def:%d: duplicate op %s" % (line_no, name))
                ops[name] = IROp(name, imm, effect, int(nopnd), int(nres))
            else:
                op, index, name, typ = args
                if op not in ops:
                    raise RuleError("ir_ops.def:%d: row for unknown op %s" % (line_no, op))
                lst = ops[op].operands if m.group(1) == "T1_IR_OPERAND" else ops[op].results
                if int(index) != len(lst):
                    raise RuleError(
                        "ir_ops.def:%d: %s index %s out of order" % (line_no, op, index)
                    )
                lst.append((name, typ))
    for op in ops.values():
        if len(op.operands) != op.nopnd or len(op.results) != op.nres or op.nres > 4:
            raise RuleError(
                "ir_ops.def: %s declares %d operands/%d results, rows give %d/%d"
                % (op.name, op.nopnd, op.nres, len(op.operands), len(op.results))
            )
        if any(t == "VAR" for _, t in op.operands[:-1]):
            raise RuleError("ir_ops.def: %s: VAR operand must be last" % op.name)
    return ops


def pattern_operands(op):
    """The operands a pattern writes: MEM tokens and VAR tails are implicit."""
    return [o for o in op.operands if o[1] not in ("VAR", "MEM")]


class X86Op:
    def __init__(self, enum, mnemonic, specs, implicit, fread, fdef, fundef, attrs, cc):
        self.enum, self.mnemonic, self.specs = enum, mnemonic, specs
        self.implicit, self.fread, self.fdef, self.fundef = implicit, fread, fdef, fundef
        self.attrs, self.cc = attrs, cc


def _eval_flags(expr, defines):
    value = 0
    for tok in expr.split("|"):
        tok = tok.strip().strip("()")
        if tok in defines:
            value |= _eval_flags(defines[tok], defines)
        elif re.match(r"^\(?1u << \d+\)?$", tok):
            value |= 1 << int(tok.split("<<")[1].strip(" )"))
        elif tok in ("0", "0u"):
            pass
        else:
            m = re.match(r"^1u << (\d+)$", tok)
            if not m:
                raise RuleError("x86enc.h: cannot evaluate flag expression %r" % tok)
            value |= 1 << int(m.group(1))
    return value


def _macro_rows(text, name, filename):
    # C removes backslash-newline pairs before parsing macro definitions.
    text = text.replace("\\\n", "")
    macro = re.search(r"^[ \t]*#[ \t]*define[ \t]+" + name + r"\([^()\n]*\)[ \t]*(.*)$", text, re.M)
    if not macro:
        raise RuleError("%s: missing %s definition" % (filename, name))
    body, pos = macro.group(1), 0
    while body[pos:].strip():
        row = re.match(r"\s*(X|XCC)\s*\(", body[pos:])
        if not row:
            raise RuleError("%s: unparsable %s row: %r" % (filename, name, body[pos:]))
        start = pos + row.end()
        depth = 1
        for token in re.finditer(r'"(?:\\.|[^"\\])*"|[()]|[^()"]+', body[start:]):
            if token.group() == "(":
                depth += 1
            elif token.group() == ")":
                depth -= 1
                if depth == 0:
                    end = start + token.start()
                    yield row.group(1), body[start:end]
                    pos = end + 1
                    break
        else:
            raise RuleError("%s: unterminated %s row: %r" % (filename, name, body[pos:]))


def load_x86_ops(path=X86ENC_PATH):
    """Returns (list of X86Op in enum order, {mnemonic: [X86Op]}, flag bits)."""
    with open(path) as f:
        text = f.read()
    defines = {}
    for m in re.finditer(r"^#define (T1_XF_\w+) (.*)$", text, re.M):
        defines[m.group(1)] = m.group(2).strip()
    flag_bits = {
        name[6:]: _eval_flags(name, defines)
        for name in ("T1_XF_CF", "T1_XF_PF", "T1_XF_AF", "T1_XF_ZF", "T1_XF_SF", "T1_XF_OF")
    }
    ops, by_name = [], {}
    for kind, row in _macro_rows(text, "T1_X86_OPS", "x86enc.h"):
        args = [a.strip() for a in row.split(",")]
        if kind == "X":
            enum, mnem, nopnd, o0, o1, iu, idf, fr, fd, fu, attr = args
            rows = [(enum, mnem.strip('"'), None, _eval_flags(fr, defines))]
        else:
            enum, pattern, nopnd, o0, o1, iu, idf, fd, fu, attr = args
            rows = [(enum, pattern.strip('"').replace("<cc>", cc), cc, None) for cc in CC_NAMES]
        specs = [s for s in (o0, o1) if s != "T1_XO_NONE"][: int(nopnd)]
        if len(specs) != int(nopnd):
            raise RuleError("x86enc.h: %s: operand count mismatch" % enum)
        attrs = set(a.strip() for a in attr.split("|")) - {"T1_XA_NONE"}
        implicit = (iu != "0") or (idf != "0")
        for enum_name, mnem, cc, fread in rows:
            if cc is not None:
                fread = cc_reads(cc, flag_bits)
                expr = "T1_X86_%s + T1_CC_%s" % (enum_name, cc.upper())
            else:
                expr = "T1_X86_%s" % enum_name
            op = X86Op(
                expr,
                mnem,
                [s[6:] for s in specs],
                implicit,
                fread,
                _eval_flags(fd, defines),
                _eval_flags(fu, defines),
                attrs,
                cc,
            )
            ops.append(op)
            by_name.setdefault(mnem, []).append(op)
    return ops, by_name, flag_bits


def cc_reads(cc, bits):
    """RFLAGS bits read by a condition (SDM Vol.1 Table B-1)."""
    i = CC_NAMES.index(cc)
    groups = [
        ("OF",),
        ("CF",),
        ("ZF",),
        ("CF", "ZF"),
        ("SF",),
        ("PF",),
        ("SF", "OF"),
        ("ZF", "SF", "OF"),
    ]
    return sum(bits[f] for f in groups[i // 2])


# ---------------------------------------------------------------- lexing

COMMENT = re.compile(r"(^|\s)#(\s|$).*$")


def strip_comment(line):
    return COMMENT.sub("", line).rstrip()


def tokenize_sexpr(text):
    return re.findall(r"\(|\)|@\w+|[$#]?[\w.\-]+", text)


class Node:
    """A pattern or result expression node."""

    def __init__(self):
        self.op = None  # IR op name, or None for a $x leaf
        self.binder = None  # leaf: '$x'
        self.attrs = []  # literal attribute tokens, '#k', or expression tuples
        self.kids = []
        self.result = None  # '@name' given, or None
        self.index = None  # preorder index (patterns)

    def shape(self):
        """The expression without its own @result (kids keep theirs)."""
        if self.op is None:
            return self.binder
        return "(%s %s)" % (
            self.op,
            " ".join(
                [attr_text(a) for a in self.attrs]
                + [k.shape() + ("@" + k.result if k.result else "") for k in self.kids]
            ),
        )


def attr_text(a):
    return a if isinstance(a, str) else expr_text(a)


def parse_sexpr(tokens, pos, where):
    if pos >= len(tokens):
        raise RuleError("%s: unexpected end of expression" % where)
    tok = tokens[pos]
    n = Node()
    if tok.startswith("$"):
        n.binder = tok
        return n, pos + 1
    if tok != "(":
        raise RuleError("%s: expected ( or $value, got %r" % (where, tok))
    pos += 1
    n.op = tokens[pos]
    pos += 1
    while tokens[pos] != ")":
        if tokens[pos] == "(" and pos + 1 < len(tokens) and re.match(r"^[a-z]", tokens[pos + 1]):
            # A lowercase head is an immediate expression (format 2), an
            # attribute of the enclosing op.
            e, pos = parse_expr(tokens, pos, where)
            n.attrs.append(e)
        elif tokens[pos] == "(" or tokens[pos].startswith("$"):
            kid, pos = parse_sexpr(tokens, pos, where)
            n.kids.append(kid)
        else:
            n.attrs.append(tokens[pos])
            pos += 1
        if pos >= len(tokens):
            raise RuleError("%s: unbalanced parentheses" % where)
    pos += 1
    if pos < len(tokens) and tokens[pos].startswith("@"):
        n.result = tokens[pos][1:]
        pos += 1
    return n, pos


# ---------------------------------------------------------------- expressions
# Tuples: ('lit', int) | ('imm', '#k') | (op, e1, e2) with op in EXPR_OPS.

EXPR_OPS = {
    "add": "T1_IX_ADD",
    "sub": "T1_IX_SUB",
    "and": "T1_IX_AND",
    "or": "T1_IX_OR",
    "xor": "T1_IX_XOR",
    "shl": "T1_IX_SHL",
    "shru": "T1_IX_SHRU",
    "shr": "T1_IX_SHR",
    "eq": "T1_IX_EQ",
    "lt": "T1_IX_LT",
    "ltu": "T1_IX_LTU",
}
NARY = ("add", "sub", "and", "or", "xor")


def lit(v):
    return ("lit", to_signed(v))


def to_signed(v):
    v &= MASK64
    return v - 2**64 if v >> 63 else v


def parse_expr(tokens, pos, where):
    """One immediate expression at tokens[pos]; returns (tuple, next pos)."""
    tok = tokens[pos]
    if tok.startswith("#"):
        return ("imm", tok), pos + 1
    if re.match(r"^-?(0x[0-9a-fA-F]+|\d+)$", tok):
        return lit(int(tok, 0)), pos + 1
    if tok != "(":
        raise RuleError("%s: bad immediate expression at %r" % (where, tok))
    op = tokens[pos + 1]
    pos += 2
    args = []
    while pos < len(tokens) and tokens[pos] != ")":
        e, pos = parse_expr(tokens, pos, where)
        args.append(e)
    if pos >= len(tokens):
        raise RuleError("%s: unbalanced expression" % where)
    pos += 1
    if op in ("pos", "bytes") and len(args) == 1:
        return ("and", args[0], lit(255)), pos
    if op == "len" and len(args) == 1:
        return ("shru", args[0], lit(8)), pos
    if op == "neg" and len(args) == 1:
        return ("sub", lit(0), args[0]), pos
    if op == "not" and len(args) == 1:
        return ("xor", args[0], lit(-1)), pos
    if op == "ne" and len(args) == 2:
        return ("xor", ("eq", args[0], args[1]), lit(1)), pos
    if op in NARY and len(args) >= 2:
        e = args[0]
        for a in args[1:]:
            e = (op, e, a)
        return e, pos
    if op in EXPR_OPS and len(args) == 2:
        return (op, args[0], args[1]), pos
    raise RuleError("%s: unknown expression (%s ...) with %d arguments" % (where, op, len(args)))


def parse_expr_text(text, where):
    tokens = tokenize_sexpr(text)
    e, pos = parse_expr(tokens, 0, where)
    if pos != len(tokens):
        raise RuleError("%s: trailing tokens after %r" % (where, text))
    return e


def expr_text(e):
    if e[0] == "lit":
        return str(e[1])
    if e[0] == "imm":
        return e[1]
    return "(%s %s %s)" % (e[0], expr_text(e[1]), expr_text(e[2]))


def expr_binders(e):
    if e[0] == "lit":
        return set()
    if e[0] == "imm":
        return {e[1]}
    return expr_binders(e[1]) | expr_binders(e[2])


def subst_expr(e, sub):
    """Replaces ('imm', #k) leaves that name FIELD parts by their expressions."""
    if e[0] == "lit":
        return e
    if e[0] == "imm":
        return sub.get(e[1], e)
    return (e[0], subst_expr(e[1], sub), subst_expr(e[2], sub))


def eval_expr(e, env):
    """Concrete value (signed int64) of an expression; env: #k -> int. The
    definition of rules.h t1_iexpr_eval."""
    if e[0] == "lit":
        return to_signed(e[1])
    if e[0] == "imm":
        return to_signed(env[e[1]])
    a, b = eval_expr(e[1], env) & MASK64, eval_expr(e[2], env) & MASK64
    op = e[0]
    if op == "add":
        r = a + b
    elif op == "sub":
        r = a - b
    elif op == "and":
        r = a & b
    elif op == "or":
        r = a | b
    elif op == "xor":
        r = a ^ b
    elif op == "shl":
        r = a << (b & 63)
    elif op == "shru":
        r = a >> (b & 63)
    elif op == "shr":
        r = to_signed(a) >> (b & 63)
    elif op == "eq":
        r = int(a == b)
    elif op == "lt":
        r = int(to_signed(a) < to_signed(b))
    elif op == "ltu":
        r = int(a < b)
    else:
        raise RuleError("unknown expression op %s" % op)
    return to_signed(r)


# ---------------------------------------------------------------- rules


class Rule:
    def __init__(self, name, cost, line, fold=False):
        self.name, self.cost, self.line, self.fold = name, cost, line, fold
        self.match = None
        self.when = {}  # pattern '#k' -> sorted list of allowed values or (lo, hi)
        self.preds = []  # raw: (kind, subject expression, args, where)
        self.checks = []  # (expression, lo, hi): relation checks (format 2)
        self.outputs = []  # (binder, Node, result name)
        self.temps, self.ties, self.labels, self.window = [], [], [], []
        self.fixed = {}  # binder -> register name
        self.emit = []  # Insn
        self.flags = None
        self.result = None  # fold rules: the result tree (Node)
        # Filled by resolve():
        self.pat = []  # preorder Nodes
        self.ins, self.imms = [], []
        self.types = {}  # IN binder -> type
        self.output_node = {}  # OUT binder -> (pattern Node, result name)
        self.imm_layout = {}  # IMM binder -> ir_ops.def immediate layout
        self.parts = {}  # FIELD part binder -> expression over the packed binder
        self.match_labels = []

    def binder_kind(self, b):
        if b in self.ins:
            return KIND_IN
        if b in [o[0] for o in self.outputs]:
            return KIND_OUT
        if b in self.temps:
            return KIND_TEMP
        if b in self.labels:
            return KIND_LABEL
        if b in self.window:
            return KIND_WINDOW
        if b in self.imms:
            return KIND_IMM
        return None

    def binder_index(self, b):
        kind = self.binder_kind(b)
        lst = {
            KIND_IN: self.ins,
            KIND_OUT: [o[0] for o in self.outputs],
            KIND_TEMP: self.temps,
            KIND_LABEL: self.labels,
            KIND_IMM: self.imms,
            KIND_WINDOW: self.window,
        }[kind]
        return lst.index(b)

    def is_branch(self):
        return self.match.op == "BRANCH"

    def is_memory(self):
        return self.match.op in ("LOAD", "STORE")

    def expr(self, e):
        """A parsed expression with FIELD parts substituted."""
        return subst_expr(e, self.parts)


class Insn:
    def __init__(self, text, line):
        self.text, self.line = text, line
        self.mnemonic = None
        self.operands = []  # ('reg', '$x') ('imm', int|'#k'|expr) ('label', '$T') ('mem', Mem) ('cl', '$c')
        self.op = None  # X86Op


class Mem:
    def __init__(self):
        self.base = self.index = None
        self.scale = 1  # int, or '#k' meaning 1 << #k
        self.disp = 0
        self.disp_expr = None  # format 2: a derived displacement added to disp


KEYWORDS = (
    "match",
    "when",
    "outputs",
    "temps",
    "tie",
    "fixed",
    "window",
    "labels",
    "emit",
    "flags",
)
FOLD_KEYWORDS = ("match", "when", "result")


def parse_rules(text, path="isel.rules", fold=False):
    rules, rule, key = [], None, None
    head = "fold" if fold else "rule"
    for line_no, raw in enumerate(text.split("\n"), 1):
        line = strip_comment(raw)
        if not line.strip():
            continue
        words = line.split()
        where = "%s:%d" % (path, line_no)
        if rule is None:
            if fold:
                if words[0] != "fold" or len(words) != 2:
                    raise RuleError('%s: expected "fold <name>"' % where)
            elif words[0] != "rule" or len(words) != 4 or words[2] != "cost":
                raise RuleError('%s: expected "rule <name> cost <n>"' % where)
            if not re.match(r"^[a-z][a-z0-9_]*$", words[1]):
                raise RuleError("%s: bad %s name %r" % (where, head, words[1]))
            rule = Rule(words[1], 0 if fold else int(words[3]), line_no, fold)
            key = None
            continue
        if words[0] == "end":
            if len(words) != 1:
                raise RuleError("%s: text after end" % where)
            rules.append(rule)
            rule = None
            continue
        if words[0] in (FOLD_KEYWORDS if fold else KEYWORDS):
            key = words[0]
            rest = line.split(None, 1)[1] if len(words) > 1 else ""
        elif key == "emit":
            rest = line.strip()  # continuation of the template
        else:
            raise RuleError("%s: unknown clause %r" % (where, words[0]))
        clause(rule, key, rest.strip(), where)
    if rule is not None:
        raise RuleError("%s: %s %s has no end" % (path, head, rule.name))
    names = [r.name for r in rules]
    for n in names:
        if names.count(n) > 1:
            raise RuleError("duplicate %s %s" % (head, n))
    return rules


def clause(rule, key, rest, where):
    if key == "match":
        if rule.match is not None:
            raise RuleError("%s: second match" % where)
        tokens = tokenize_sexpr(rest)
        rule.match, pos = parse_sexpr(tokens, 0, where)
        if pos != len(tokens):
            raise RuleError("%s: trailing tokens after the pattern" % where)
        rule.match_labels = []
        if rule.match.op == "BRANCH":
            # (BRANCH c $T $F): the last two operands are the block labels.
            kids = rule.match.kids
            if len(kids) != 3 or kids[1].op is not None or kids[2].op is not None:
                raise RuleError("%s: BRANCH pattern is (BRANCH <cond> $T $F)" % where)
            rule.match_labels = [kids[1].binder, kids[2].binder]
            rule.match.kids = kids[:1]
    elif key == "when":
        parse_when(rule, tokenize_sexpr(rest), where)
    elif key == "result":
        if rule.result is not None:
            raise RuleError("%s: second result" % where)
        tokens = tokenize_sexpr(rest)
        rule.result, pos = parse_sexpr(tokens, 0, where)
        if pos != len(tokens):
            raise RuleError("%s: trailing tokens after the result" % where)
    elif key == "outputs":
        for part in split_top(rest, ","):
            b, _, expr = part.partition("=")
            b = b.strip()
            if not b.startswith("$"):
                raise RuleError("%s: output must be a $binder" % where)
            node, pos = parse_sexpr(tokenize_sexpr(expr), 0, where)
            if node.op is None:
                raise RuleError("%s: output %s must name an IR result" % (where, b))
            rule.outputs.append((b, node, node.result))
    elif key == "temps":
        rule.temps += rest.split()
    elif key == "labels":
        rule.labels += rest.split()
    elif key == "window":
        rule.window += rest.split()
        if len(rule.window) != 1 or not rule.window[0].startswith("$"):
            raise RuleError("%s: window declares exactly one $binder" % where)
    elif key in ("tie", "fixed"):
        for part in split_top(rest, ","):
            a, eq, b = part.partition("=")
            a = a.strip()
            alts = [x.strip() for x in b.split("|")]
            if not eq or not a.startswith("$"):
                raise RuleError("%s: expected $x = ..." % where)
            if key == "tie":
                rule.ties.append((a, alts[0]))
            else:
                if len(alts) != 1:
                    raise RuleError("%s: fixed takes exactly one register" % where)
                rule.fixed[a] = alts[0]
    elif key == "emit":
        insn = Insn(rest, where)
        parse_insn(insn)
        rule.emit.append(insn)
    elif key == "flags":
        if rest not in FLAG_KINDS:
            raise RuleError("%s: flags must be one of %s" % (where, ", ".join(FLAG_KINDS)))
        rule.flags = rest


def split_top(text, sep):
    parts, depth, cur = [], 0, ""
    for ch in text:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return [p.strip() for p in parts]


def parse_when(rule, tokens, where):
    def pred(pos):
        if tokens[pos] != "(":
            raise RuleError("%s: predicate must be parenthesized" % where)
        kind = tokens[pos + 1]
        pos += 2
        if kind == "and":
            while tokens[pos] != ")":
                pos = pred(pos)
            return pos + 1
        subject, pos = parse_expr(tokens, pos, where)
        args = []
        while tokens[pos] != ")":
            args.append(int(tokens[pos], 0))
            pos += 1
        if kind not in ("in", "range") or (kind == "range" and len(args) != 2) or not args:
            raise RuleError("%s: unknown predicate %s" % (where, kind))
        rule.preds.append((kind, subject, args, where))
        return pos + 1

    if pred(0) != len(tokens):
        raise RuleError("%s: trailing tokens in when" % where)


def parse_insn(insn):
    mnem, _, rest = insn.text.partition(" ")
    insn.mnemonic = mnem.strip()
    for part in split_top(rest.strip(), ",") if rest.strip() else []:
        if part.startswith("["):
            if not part.endswith("]"):
                raise RuleError("%s: bad memory operand %r" % (insn.line, part))
            insn.operands.append(("mem", parse_mem(part[1:-1], insn.line)))
        elif part == "cl":
            insn.operands.append(("cl", None))
        elif part.startswith("$"):
            insn.operands.append(("reg", part))
        elif part.startswith("#"):
            insn.operands.append(("imm", part))
        elif part.startswith("("):
            insn.operands.append(("imm", parse_expr_text(part, insn.line)))
        elif re.match(r"^-?(0x[0-9a-fA-F]+|\d+)$", part):
            insn.operands.append(("imm", int(part, 0)))
        else:
            raise RuleError("%s: bad operand %r" % (insn.line, part))


def split_terms(text):
    """Splits a memory operand at top-level + and - (a - keeps its sign)."""
    terms, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if depth == 0 and ch in "+-":
            if cur.strip():
                terms.append(cur.strip())
            cur = "-" if ch == "-" else ""
        else:
            cur += ch
    if cur.strip():
        terms.append(cur.strip())
    return terms


def parse_mem(text, where):
    mem = Mem()
    for term in split_terms(text):
        if "*" in term and not term.startswith("("):
            reg, _, scale = term.partition("*")
            reg, scale = reg.strip(), scale.strip()
            if mem.index is not None or not reg.startswith("$"):
                raise RuleError("%s: bad index term %r" % (where, term))
            mem.index = reg
            m = re.match(r"^\(1 << (#\w+)\)$", scale)
            if m:
                mem.scale = m.group(1)
            elif scale in ("1", "2", "4", "8"):
                mem.scale = int(scale)
            else:
                raise RuleError("%s: scale must be 1/2/4/8 or (1 << #k)" % where)
        elif term.startswith("$"):
            if mem.base is not None:
                raise RuleError("%s: two base registers" % where)
            mem.base = term
        elif term.startswith("#") or term.startswith("("):
            if mem.disp_expr is not None:
                raise RuleError("%s: two derived displacements" % where)
            mem.disp_expr = ("imm", term) if term.startswith("#") else parse_expr_text(term, where)
        else:
            mem.disp += int(term.replace(" ", ""), 0)
    if not -(2**31) <= mem.disp < 2**31:
        raise RuleError("%s: disp32 out of range" % where)
    return mem


# ---------------------------------------------------------------- resolution

FIELD_MIN, FIELD_MAX = 0 | 1 << 8, 63 | 64 << 8
ACCESS_MIN, ACCESS_MAX = (
    1,
    8 | 1 << 8 | 63 << 16,
)  # bytes | ordered << 8 | site << 16 (T1_MAX_SITES)


def imm_domain(layout):
    """Default value range of a packed immediate (ir_ops.def layouts). A
    FIELD binder ranges over the packed pos | len << 8 and an ACCESS binder
    over bytes | ordered << 8 | site << 16; the prover adds the exact
    per-field bounds (pos 0..63, len 1..64; bytes 1/2/4/8, ordered 0/1),
    which the matcher gets from the IR's own invariant (ir_ops.def)."""
    return {
        "COUNT": (0, 63),
        "WIDTH": (8, 32),
        "VALUE": (-(2**63), 2**63 - 1),
        "FIELD": (FIELD_MIN, FIELD_MAX),
        "ACCESS": (ACCESS_MIN, ACCESS_MAX),
    }.get(layout)


def pack_attrs(op, attrs, where):
    """Literal attributes -> packed immediate (ir_ops.def), or None if bound
    (a #binder, a FIELD part pair) or derived (an expression, results only)."""
    if op.imm == "NONE":
        if attrs:
            raise RuleError("%s: %s takes no attributes" % (where, op.name))
        return None
    if op.imm == "CMP":
        if len(attrs) != 2 or attrs[0] not in CMP_RELS or attrs[1] not in ("32", "64"):
            raise RuleError("%s: CMP needs <eq|lt|ltu> <32|64>" % where)
        return CMP_RELS[attrs[0]] | int(attrs[1]) << 8
    if op.imm in ("COUNT", "WIDTH", "VALUE", "ACCESS"):
        if len(attrs) != 1:
            raise RuleError("%s: %s takes one attribute" % (where, op.name))
        if not isinstance(attrs[0], str) or attrs[0].startswith("#"):
            return None
        if op.imm == "ACCESS":
            raise RuleError("%s: %s binds its access immediate (#m)" % (where, op.name))
        return int(attrs[0], 0)
    if op.imm == "FIELD":
        # (EXTRU pos len $a) with literals, (EXTRU #f $a) binding the packed
        # pos | len << 8, or (EXTRU #p #l $a) binding it and naming its parts.
        if len(attrs) == 1 and (not isinstance(attrs[0], str) or attrs[0].startswith("#")):
            return None
        if len(attrs) == 2 and all(isinstance(a, str) and a.startswith("#") for a in attrs):
            return None
        if len(attrs) != 2 or any(not isinstance(a, str) or a.startswith("#") for a in attrs):
            raise RuleError(
                "%s: %s takes <pos> <len> literals, #pos #len or one #binder" % (where, op.name)
            )
        pos, length = int(attrs[0], 0), int(attrs[1], 0)
        if not (0 <= pos <= 63 and 1 <= length <= 64):
            raise RuleError("%s: %s field pos %d len %d" % (where, op.name, pos, length))
        return pos | length << 8
    raise RuleError(
        "%s: patterns over %s (layout %s) are not supported yet" % (where, op.name, op.imm)
    )


def walk_pattern(rule, ir_ops, where, allow_root_memory):
    """Preorder walk of rule.match: binders, immediates, types."""

    def walk(n, want_type, is_root):
        n.index = len(rule.pat)
        rule.pat.append(n)
        if n.op is None:
            if n.binder in rule.types and rule.types[n.binder] != want_type:
                raise RuleError(
                    "%s: %s used as %s and %s" % (where, n.binder, rule.types[n.binder], want_type)
                )
            rule.types[n.binder] = want_type
            if n.binder not in rule.ins:
                rule.ins.append(n.binder)
            return
        if n.op not in ir_ops:
            raise RuleError("%s: unknown IR op %s" % (where, n.op))
        op = ir_ops[n.op]
        if n.op == "BRANCH":
            if not is_root:
                raise RuleError("%s: BRANCH only as the root" % where)
        elif op.effect in ("LOAD", "STORE"):
            if not (is_root and allow_root_memory):
                raise RuleError("%s: %s only as the root of an isel rule" % (where, n.op))
        elif op.effect != "PURE" or n.op == "LIVEIN":
            # CONST is a pattern leaf matched on its value (immediate forms);
            # LIVEIN values are entry loads, matched as plain $x leaves.
            raise RuleError("%s: %s cannot appear in a pattern" % (where, n.op))
        if any(not isinstance(a, str) for a in n.attrs):
            raise RuleError("%s: expressions are not allowed in patterns" % where)
        n.packed = pack_attrs(op, n.attrs, where)
        n.imm_binder = None
        if n.packed is None and n.attrs:
            if len(n.attrs) == 2:
                # FIELD parts: one hidden binder for the packed immediate.
                p, l = n.attrs
                n.imm_binder = "#field(%s,%s)" % (p[1:], l[1:])
                for part, e in (
                    (p, ("and", ("imm", n.imm_binder), lit(255))),
                    (l, ("shru", ("imm", n.imm_binder), lit(8))),
                ):
                    if part in rule.parts or part in rule.imms:
                        raise RuleError("%s: %s bound twice" % (where, part))
                    rule.parts[part] = e
            else:
                n.imm_binder = n.attrs[0]
        if n.imm_binder is not None:
            if rule.imm_layout.get(n.imm_binder, op.imm) != op.imm:
                raise RuleError("%s: %s binds immediates of two layouts" % (where, n.imm_binder))
            rule.imm_layout[n.imm_binder] = op.imm
            if n.imm_binder not in rule.imms:
                rule.imms.append(n.imm_binder)
        operands = pattern_operands(op)
        if len(n.kids) != len(operands):
            raise RuleError("%s: %s takes %d operands" % (where, n.op, len(operands)))
        if n.result is not None and n.result not in [r[0] for r in op.results]:
            raise RuleError("%s: %s has no result %s" % (where, n.op, n.result))
        if n.result is None and len(op.results) > 1 and not is_root:
            raise RuleError("%s: name the result of %s with @" % (where, n.op))
        for kid, (oname, otype) in zip(n.kids, operands):
            if kid.op is not None:
                kop = ir_ops.get(kid.op)
                res = kid.result or (kop.results[0][0] if kop and kop.results else None)
                ktype = dict(kop.results)[res] if kop else None
                if otype != "T" and ktype not in (otype, "T"):
                    raise RuleError("%s: %s operand %s wants %s" % (where, n.op, oname, otype))
            walk(kid, otype if otype != "T" else "I64", False)

    walk(rule.match, None, True)


def resolve_preds(rule, where):
    """when: predicates over a pattern binder restrict its node (format 1);
    all others become relation checks (format 2)."""
    for kind, subject, args, pwhere in rule.preds:
        if subject[0] == "imm" and subject[1] in rule.imms and subject[1] not in rule.parts:
            k = subject[1]
            if k in rule.when:
                raise RuleError("%s: %s constrained twice" % (pwhere, k))
            rule.when[k] = sorted(set(args)) if kind == "in" else (args[0], args[1])
            continue
        e = rule.expr(subject)
        for b in expr_binders(e):
            if b not in rule.imms:
                raise RuleError("%s: when constrains unbound %s" % (pwhere, b))
        if kind == "range":
            rule.checks.append((e, args[0], args[1]))
        else:
            vals = sorted(set(args))
            if vals == list(range(vals[0], vals[-1] + 1)):
                rule.checks.append((e, vals[0], vals[-1]))
            else:
                any_eq = None
                for v in vals:
                    t = ("eq", e, lit(v))
                    any_eq = t if any_eq is None else ("or", any_eq, t)
                rule.checks.append((any_eq, 1, 1))
    for k in rule.when:
        if k not in rule.imms:
            raise RuleError("%s: when constrains unbound %s" % (where, k))


def binder_domain(rule, k):
    """Python-side domain of an immediate binder: list or (lo, hi)."""
    dom = rule.when.get(k) or imm_domain(rule.imm_layout.get(k))
    if dom is None:
        raise RuleError("rule %s: no domain for %s" % (rule.name, k))
    return dom


def binder_values(rule, k):
    """The IR-valid values of binder k, if few enough to enumerate, else None."""
    dom = binder_domain(rule, k)
    layout = rule.imm_layout.get(k)
    if layout == "FIELD":
        vals = [p | l << 8 for p in range(64) for l in range(1, 65)]
    elif layout == "ACCESS":
        vals = [b | o << 8 for b in (1, 2, 4, 8) for o in (0, 1)]  # the site is never relevant
    elif isinstance(dom, list):
        return list(dom)
    elif dom[1] - dom[0] <= 4096:
        return list(range(dom[0], dom[1] + 1))
    else:
        return None
    if isinstance(dom, list):
        return [v for v in vals if v in dom]
    return [v for v in vals if dom[0] <= v <= dom[1]]


def expr_range(rule, e):
    """(lo, hi) of a derived immediate over every immediate the rule
    accepts: by enumeration when the binders' domains are small (checks
    over those binders applied), else by interval arithmetic on add/sub."""
    binders = sorted(expr_binders(e))
    domains = [binder_values(rule, b) for b in binders]
    if all(d is not None for d in domains):
        total = 1
        for d in domains:
            total *= len(d)
        if total <= 1 << 16:
            checks = [c for c in rule.checks if expr_binders(c[0]) <= set(binders)]
            vals = []

            def rec(i, env):
                if i == len(binders):
                    if all(lo <= eval_expr(c, env) <= hi for c, lo, hi in checks):
                        vals.append(eval_expr(e, env))
                    return
                for v in domains[i]:
                    env[binders[i]] = v
                    rec(i + 1, env)

            rec(0, {})
            if not vals:
                raise RuleError(
                    "rule %s: no immediate satisfies the checks of %s" % (rule.name, expr_text(e))
                )
            return min(vals), max(vals)

    def interval(x):
        if x[0] == "lit":
            return x[1], x[1]
        if x[0] == "imm":
            dom = binder_domain(rule, x[1])
            return (min(dom), max(dom)) if isinstance(dom, list) else dom
        if x[0] in ("add", "sub"):
            (a0, a1), (b0, b1) = interval(x[1]), interval(x[2])
            return (a0 + b0, a1 + b1) if x[0] == "add" else (a0 - b1, a1 - b0)
        raise RuleError("rule %s: cannot bound %s statically" % (rule.name, expr_text(e)))

    lo, hi = interval(e)
    if lo < -(2**63) or hi >= 2**63:
        raise RuleError("rule %s: %s may wrap" % (rule.name, expr_text(e)))
    return lo, hi


def resolve(rule, ir_ops, x86_by_name):
    where = "rule %s" % rule.name
    if rule.match is None or rule.flags is None or not rule.emit:
        raise RuleError("%s: match, emit and flags are required" % where)
    if rule.is_branch():
        if rule.match_labels != ["$T", "$F"] or rule.labels != ["$T", "$F"]:
            raise RuleError("%s: a BRANCH rule matches and declares labels $T $F" % where)
    elif rule.labels or rule.match_labels:
        raise RuleError("%s: only BRANCH rules have labels" % where)
    walk_pattern(rule, ir_ops, where, True)
    if rule.match.op not in ("BRANCH", "STORE") and not rule.outputs:
        raise RuleError("%s: a value rule declares its outputs" % where)
    if rule.is_memory() != bool(rule.window):
        raise RuleError("%s: LOAD/STORE rules, and only they, declare a window" % where)
    resolve_preds(rule, where)
    for b, node, res in rule.outputs:
        target = [p for p in rule.pat if p.op is not None and p.shape() == node.shape()]
        if not target:
            raise RuleError("%s: output %s is not a result of the pattern" % (where, b))
        rule.output_node[b] = (target[0], res or ir_ops[node.op].results[0][0])
    all_binders = rule.ins + [o[0] for o in rule.outputs] + rule.temps + rule.labels + rule.window
    if len(set(all_binders)) != len(all_binders):
        raise RuleError("%s: a binder is declared twice" % where)
    for out, inp in rule.ties:
        if rule.binder_kind(out) != KIND_OUT or rule.binder_kind(inp) != KIND_IN:
            raise RuleError("%s: tie joins an output to an input" % where)
    for b, reg in rule.fixed.items():
        if rule.binder_kind(b) not in (KIND_IN, KIND_OUT, KIND_TEMP) or reg not in ALLOCATABLE:
            raise RuleError("%s: fixed %s = %s" % (where, b, reg))
    if (
        len(rule.ins) > 31
        or len(rule.temps) > 31
        or len(rule.outputs) > 31
        or len(rule.pat) > 255
        or len(rule.imms) > 31
    ):
        raise RuleError("%s: too many binders" % where)

    tied_out = {o for o, _ in rule.ties}
    written = set(tied_out)
    accesses = 0
    for insn in rule.emit:
        resolve_insn(rule, insn, x86_by_name)
        if insn.mnemonic in ("mulpair64", "imulpair64"):
            regs = [rule.fixed.get(v) for kind, v in insn.operands]
            if regs != ["rax", "rdx"]:
                raise RuleError("%s: wide multiply needs explicit fixed rax, rdx" % insn.line)
        reads, writes = insn_access(insn)
        for b in reads:
            kind = rule.binder_kind(b)
            if kind in (KIND_OUT, KIND_TEMP) and b not in written:
                raise RuleError(
                    "%s: %s reads %s before it is written" % (insn.line, insn.mnemonic, b)
                )
        for b in writes:
            kind = rule.binder_kind(b)
            if kind not in (KIND_OUT, KIND_TEMP):
                raise RuleError(
                    "%s: %s writes %s, which is not an output or temp"
                    % (insn.line, insn.mnemonic, b)
                )
            written.add(b)
        mem = [v for k, v in insn.operands if k == "mem"]
        if mem and any(s.startswith("M") and s != "MA" for s in insn.op.specs):
            accesses += 1
            m = mem[0]
            if (
                not rule.is_memory()
                or m.base != rule.match.kids[0].binder
                or m.index != rule.window[0]
                or m.scale != 1
                or m.disp != 0
                or m.disp_expr is not None
            ):
                raise RuleError(
                    "%s: the access of a LOAD/STORE rule is [$addr + %s * 1]"
                    % (insn.line, rule.window[0] if rule.window else "$window")
                )
        elif mem and rule.window and rule.window[0] in (mem[0].base, mem[0].index):
            raise RuleError("%s: the window register is used only by the access" % insn.line)
    if rule.is_memory() and accesses != 1:
        raise RuleError("%s: a LOAD/STORE rule makes exactly one memory access" % where)
    for b in [o[0] for o in rule.outputs]:
        if b not in written:
            raise RuleError("%s: output %s is never written" % (where, b))
    last = rule.emit[-1].op
    if rule.is_branch() and "T1_XA_NOFALL" not in last.attrs:
        raise RuleError("%s: a BRANCH rule ends in jmp" % where)
    if not rule.is_branch() and any("T1_XA_BRANCH" in i.op.attrs for i in rule.emit):
        raise RuleError("%s: only BRANCH rules branch" % where)
    return rule


def resolve_fold(rule, ir_ops):
    """A fold rule: the pattern as for isel (no memory roots), the result a
    tree of IR ops over the pattern's value binders whose immediates are
    expressions over its immediate binders."""
    where = "fold %s" % rule.name
    if rule.match is None or rule.result is None:
        raise RuleError("%s: match and result are required" % where)
    if rule.match.op == "BRANCH":
        raise RuleError("%s: fold rules rewrite values, not terminators" % where)
    walk_pattern(rule, ir_ops, where, False)
    resolve_preds(rule, where)
    root_op = ir_ops[rule.match.op]
    if rule.match.result is None and len(root_op.results) > 1:
        raise RuleError("%s: name the replaced result of %s with @" % (where, rule.match.op))
    rule.root_result = rule.match.result or root_op.results[0][0]
    rule.root_type = dict(root_op.results)[rule.root_result]
    rule.res = []

    def walk(n, want_type, is_root):
        n.index = len(rule.res)
        rule.res.append(n)
        n.imm_expr = None
        if n.op is None:
            if n.binder not in rule.ins:
                raise RuleError(
                    "%s: result uses %s, which the pattern does not bind" % (where, n.binder)
                )
            if want_type not in (rule.types[n.binder], "T") and rule.types[n.binder] != "T":
                raise RuleError(
                    "%s: %s is %s, used as %s" % (where, n.binder, rule.types[n.binder], want_type)
                )
            return
        if n.op not in ir_ops or ir_ops[n.op].effect != "PURE" or n.op == "LIVEIN":
            raise RuleError("%s: a result builds only pure ops (%s)" % (where, n.op))
        op = ir_ops[n.op]
        if op.imm == "NONE":
            if n.attrs:
                raise RuleError("%s: %s takes no immediate" % (where, n.op))
        elif op.imm == "CMP":
            n.imm_expr = lit(pack_attrs(op, n.attrs, where))
        elif op.imm == "FIELD" and len(n.attrs) == 2:
            p, l = [a if not isinstance(a, str) else parse_expr_text(a, where) for a in n.attrs]
            n.imm_expr = rule.expr(("or", p, ("shl", l, lit(8))))
        elif len(n.attrs) == 1:
            a = n.attrs[0]
            n.imm_expr = rule.expr(a if not isinstance(a, str) else parse_expr_text(a, where))
        else:
            raise RuleError("%s: %s takes one immediate expression" % (where, n.op))
        if n.imm_expr is not None:
            for b in expr_binders(n.imm_expr):
                if b not in rule.imms:
                    raise RuleError("%s: result uses unbound %s" % (where, b))
        if n.result is not None and n.result not in [r[0] for r in op.results]:
            raise RuleError("%s: %s has no result %s" % (where, n.op, n.result))
        if n.result is None and len(op.results) > 1:
            raise RuleError("%s: name the result of %s with @" % (where, n.op))
        res = n.result or op.results[0][0]
        rtype = dict(op.results)[res]
        if rtype != "T" and want_type not in (rtype, "T"):
            raise RuleError("%s: %s gives %s where %s is needed" % (where, n.op, rtype, want_type))
        operands = pattern_operands(op)
        if len(n.kids) != len(operands):
            raise RuleError("%s: %s takes %d operands" % (where, n.op, len(operands)))
        for kid, (oname, otype) in zip(n.kids, operands):
            walk(kid, otype if otype != "T" else want_type, False)

    walk(rule.result, rule.root_type, True)
    if len(rule.res) > 255 or len(rule.ins) > 31 or len(rule.imms) > 31:
        raise RuleError("%s: too large" % where)
    return rule


def operand_kind(spec):
    if spec in ("REL", "CL"):
        return {"REL": "label", "CL": "cl"}[spec]
    if spec.startswith("R"):
        return "reg"
    if spec.startswith("M"):
        return "mem"
    if spec.startswith("I"):
        return "imm"
    raise RuleError("unknown operand spec %s" % spec)


def resolve_insn(rule, insn, x86_by_name):
    name = insn.mnemonic
    for alias, cc in CC_ALIASES.items():
        for form in ("j%s", "set%s8", "cmov%s64"):
            if name == form % alias:
                name = form % cc
    cands = x86_by_name.get(name)
    if not cands:
        raise RuleError("%s: unknown mnemonic %s" % (insn.line, insn.mnemonic))
    kinds = [k for k, _ in insn.operands]
    regs = [v for k, v in insn.operands]
    zero = [c for c in cands if "T1_XA_ZERO_IDIOM" in c.attrs]
    if zero and kinds == ["reg", "reg"] and regs[0] == regs[1]:
        insn.op = zero[0]
        insn.operands = insn.operands[:1]
    else:
        for c in cands:
            if "T1_XA_ZERO_IDIOM" not in c.attrs and [operand_kind(s) for s in c.specs] == [
                ("label" if k == "reg" and v in rule.labels else k) for k, v in insn.operands
            ]:
                insn.op = c
                break
        else:
            raise RuleError(
                "%s: no form of %s takes (%s)" % (insn.line, insn.mnemonic, ", ".join(kinds))
            )
    if insn.op.implicit:
        raise RuleError(
            "%s: %s has implicit registers; not allowed in rules" % (insn.line, insn.mnemonic)
        )
    ops = []
    for (k, v), spec in zip(insn.operands, insn.op.specs):
        if operand_kind(spec) == "label":
            if v not in rule.labels:
                raise RuleError("%s: %s is not a label" % (insn.line, v))
            ops.append(("label", v))
        elif k == "cl":
            fixed = [b for b, r in rule.fixed.items() if r == "rcx"]
            if len(fixed) != 1:
                raise RuleError("%s: cl needs exactly one binder fixed to rcx" % insn.line)
            ops.append(("cl", fixed[0]))
        elif k == "reg":
            if rule.binder_kind(v) not in (KIND_IN, KIND_OUT, KIND_TEMP):
                raise RuleError("%s: unknown register binder %s" % (insn.line, v))
            ops.append((k, v))
        elif k == "imm":
            if isinstance(v, str) and v in rule.parts:
                v = rule.parts[v]  # a FIELD part: derived
            elif isinstance(v, tuple):
                v = rule.expr(v)
            check_imm(rule, insn, v, spec)
            ops.append((k, v))
        else:
            for b in (v.base, v.index):
                if b is not None and rule.binder_kind(b) not in (
                    KIND_IN,
                    KIND_TEMP,
                    KIND_OUT,
                    KIND_WINDOW,
                ):
                    raise RuleError("%s: memory operand register %s" % (insn.line, b))
            if isinstance(v.scale, str):
                dom = rule.when.get(v.scale)
                vals = (
                    dom if isinstance(dom, list) else list(range(dom[0], dom[1] + 1)) if dom else []
                )
                if not vals or not all(0 <= x <= 3 for x in vals):
                    raise RuleError(
                        "%s: scale (1 << %s) needs when restricting it to 0..3"
                        % (insn.line, v.scale)
                    )
            if v.disp_expr is not None:
                v.disp_expr = rule.expr(v.disp_expr)
                for b in expr_binders(v.disp_expr):
                    if b not in rule.imms:
                        raise RuleError("%s: unbound displacement %s" % (insn.line, b))
                lo, hi = expr_range(rule, v.disp_expr)
                if lo + v.disp < -(2**31) or hi + v.disp >= 2**31:
                    raise RuleError(
                        "%s: displacement %s out of range for disp32"
                        % (insn.line, expr_text(v.disp_expr))
                    )
            ops.append((k, v))
    insn.operands = ops


def imm_spec_range(insn, spec):
    lo, hi = {"I8": (0, 63), "I32": (-(2**31), 2**31 - 1), "I64": (-(2**63), 2**64 - 1)}[spec]
    if spec == "I32" and insn.op.specs[0].startswith("R32"):
        # A 32-bit operation takes all 32 immediate bits as its operand
        # (mov32 zero-extends the result): 0x80000000..0xffffffff are the
        # same encodings as the negative imm32 values. The encoder emits
        # the low 32 bits (x86enc.h, I32 of mov r32).
        hi = 2**32 - 1
    return lo, hi


def check_imm(rule, insn, v, spec):
    lo, hi = imm_spec_range(insn, spec)
    if isinstance(v, int):
        vals = (v, v)
    elif isinstance(v, tuple):
        for b in expr_binders(v):
            if b not in rule.imms:
                raise RuleError("%s: unbound immediate %s" % (insn.line, b))
        vals = expr_range(rule, v)
    else:
        if v not in rule.imms:
            raise RuleError("%s: unbound immediate %s" % (insn.line, v))
        dom = rule.when.get(v) or imm_domain(rule.imm_layout.get(v))
        if dom is None:
            raise RuleError("%s: %s used as an immediate needs a when range" % (insn.line, v))
        vals = (min(dom), max(dom)) if isinstance(dom, list) else dom
    if not (lo <= vals[0] and vals[1] <= hi):
        raise RuleError(
            "%s: immediate %s out of range for %s"
            % (insn.line, attr_text(v) if not isinstance(v, int) else v, spec)
        )


def insn_access(insn):
    """Binders read and written by one template line (x86 operand access)."""
    reads, writes = [], []
    for (k, v), spec in zip(insn.operands, insn.op.specs):
        if k in ("reg", "cl"):
            if spec.endswith("_R") or spec.endswith("_RW") or k == "cl":
                reads.append(v)
            if spec.endswith("_W") or spec.endswith("_RW"):
                writes.append(v)
        elif k == "mem":
            reads += [b for b in (v.base, v.index) if b is not None]
    return reads, writes


def load_rules(rules_path=RULES_PATH, ir_path=IR_OPS_PATH, x86_path=X86ENC_PATH):
    ir_ops = load_ir_ops(ir_path)
    _, by_name, _ = load_x86_ops(x86_path)
    with open(rules_path) as f:
        rules = parse_rules(f.read(), os.path.relpath(rules_path, ROOT))
    for r in rules:
        resolve(r, ir_ops, by_name)
    return rules, ir_ops


def load_fold_rules(fold_path=FOLD_PATH, ir_path=IR_OPS_PATH):
    ir_ops = load_ir_ops(ir_path)
    with open(fold_path) as f:
        rules = parse_rules(f.read(), os.path.relpath(fold_path, ROOT), fold=True)
    for r in rules:
        resolve_fold(r, ir_ops)
    return rules, ir_ops


def load_all(rules_path=RULES_PATH, fold_path=FOLD_PATH):
    """Read both rule files with unique names."""
    rules, ir_ops = load_rules(rules_path)
    folds, _ = load_fold_rules(fold_path)
    names = [r.name for r in rules + folds]
    for n in names:
        if names.count(n) > 1:
            raise RuleError("rule name %s used in isel.rules and fold.rules" % n)
    return rules, folds, ir_ops


# ---------------------------------------------------------------- FP expansions

EXPAND_KEYWORDS = ("insn", "precision", "addend", "host", "mode", "emit")
EXPAND_OPCODES = ("FMA", "FMS", "FNMA", "FMPY", "FADD", "FSUB", "FNORM", "FPMA", "FPMS", "FPNMA")
EXPAND_INPUTS = (
    "qp",
    "fpsr",
    "a.sig",
    "a.meta",
    "b.sig",
    "b.meta",
    "c.sig",
    "c.meta",
    "d.sig",
    "d.meta",
)
EXPAND_SLOTS = ("d.sig", "d.meta", "mf")
EXPAND_IMMS = {"#sf": [0, 1, 2, 3], "#mf": [16, 32]}  # PSR.mfl bit 4, PSR.mfh bit 5
EXPAND_EXITS = ("$BAIL", "$DONE")
# The x86 rows an expansion may use: R11 and immediates only, no memory.
EXPAND_X86 = (
    "test64",
    "and64",
    "or64",
    "xor64",
    "add64",
    "sub64",
    "cmp64",
    "shl64",
    "shr64",
    "movabs64",
    "jmp",
) + tuple("j" + cc for cc in CC_NAMES)
XMM = ("x0", "x1", "x2", "x3")


class SSEOp:
    def __init__(self, enum, mnemonic, form):
        self.enum, self.mnemonic, self.form = enum, mnemonic, form


def load_sse_ops(path=X86SSE_PATH):
    with open(path) as f:
        text = f.read()
    ops = []
    for kind, row in _macro_rows(text, "T1_SSE_OPS", "x86sse.h"):
        m = re.fullmatch(r'\s*(\w+)\s*,\s*"(\w+)"\s*,\s*(\w+)\s*', row)
        if kind != "X" or not m:
            raise RuleError("x86sse.h: unparsable T1_SSE_OPS row: %r" % row)
        ops.append(SSEOp("T1_SSE_" + m.group(1), m.group(2), m.group(3)))
    return ops


class ExpInsn:
    def __init__(self, text, line):
        self.text, self.line = text, line
        self.kind = None  # 'x86' 'sse' 'in' 'out' 'label'
        self.mnemonic = None
        self.op = None  # X86Op | SSEOp
        self.operands = []  # x86: ('reg', '$s') ('imm', v) ('label', l); sse: ('xmm', k) ('reg', '$s') ('imm', v)
        self.input = self.slot = self.label = None
        self.source = None  # out: ('reg', '$s') | ('xmm', k) | ('imm', v)


def parse_expand(text, path="fpfast.rules"):
    rules, rule, key = [], None, None
    for line_no, raw in enumerate(text.split("\n"), 1):
        line = strip_comment(raw)
        if not line.strip():
            continue
        words = line.split()
        where = "%s:%d" % (path, line_no)
        if rule is None:
            if (
                words[0] != "expand"
                or len(words) != 2
                or not re.match(r"^[a-z][a-z0-9_]*$", words[1])
            ):
                raise RuleError('%s: expected "expand <name>"' % where)
            rule = Rule(words[1], 0, line_no)
            rule.expand = {}
            rule.imms = list(EXPAND_IMMS)
            rule.when = dict(EXPAND_IMMS)
            key = None
            continue
        if words[0] == "end":
            if len(words) != 1:
                raise RuleError("%s: text after end" % where)
            rules.append(rule)
            rule = None
            continue
        if words[0] in EXPAND_KEYWORDS:
            key = words[0]
            rest = line.split(None, 1)[1] if len(words) > 1 else ""
        elif key == "emit":
            rest = line.strip()
        else:
            raise RuleError("%s: unknown clause %r" % (where, words[0]))
        if key == "emit":
            if rest:
                rule.emit.append(ExpInsn(rest.strip(), where))
            continue
        if key in rule.expand:
            raise RuleError("%s: second %s" % (where, key))
        choices = {
            "insn": EXPAND_OPCODES,
            "precision": ("s", "d", "none"),
            "addend": ("f0", "reg", "any"),
            "host": ("fma3", "base"),
            "mode": ("exact", "approx"),
        }[key]
        if rest.strip() not in choices:
            raise RuleError("%s: %s must be one of %s" % (where, key, " ".join(choices)))
        rule.expand[key] = rest.strip()
    if rule is not None:
        raise RuleError("%s: expand %s has no end" % (path, rule.name))
    names = [r.name for r in rules]
    for n in names:
        if names.count(n) > 1:
            raise RuleError("duplicate expand %s" % n)
    return rules


def parse_exp_operand(part, where):
    if part == "$s":
        return ("reg", "$s")
    if part in XMM:
        return ("xmm", XMM.index(part))
    if part in EXPAND_EXITS or re.match(r"^\.[a-z][a-z0-9_]*$", part):
        return ("label", part)
    if part.startswith("#"):
        return ("imm", part)
    if part.startswith("("):
        return ("imm", parse_expr_text(part, where))
    if re.match(r"^-?(0x[0-9a-fA-F]+|\d+)$", part):
        return ("imm", int(part, 0))
    raise RuleError("%s: bad operand %r" % (where, part))


def resolve_expand(rule, x86_by_name, sse_ops):
    """Static checks of an expansion (the prover checks the semantics)."""
    for k in EXPAND_KEYWORDS[:-1]:
        if k not in rule.expand:
            raise RuleError("expand %s: no %s clause" % (rule.name, k))
    x = rule.expand
    if x["insn"] in ("FPMA", "FPMS", "FPNMA") and x["precision"] != "s":
        raise RuleError("expand %s: the parallel forms are single precision" % rule.name)
    if x["addend"] != "any" and x["insn"] not in ("FMA", "FMS", "FNMA", "FPMA", "FPMS", "FPNMA"):
        raise RuleError("expand %s: addend applies to the fma and fpma forms only" % rule.name)
    if x["addend"] == "any" and x["insn"] in ("FMA", "FMS", "FNMA", "FPMA", "FPMS", "FPNMA"):
        raise RuleError("expand %s: the fma and fpma forms need addend f0 or reg" % rule.name)
    if x["precision"] == "none" and x["mode"] != "approx":
        raise RuleError("expand %s: precision none is fp_approx only" % rule.name)
    defined, used = [], []
    for insn in rule.emit:
        t = insn.text
        if re.match(r"^\.[a-z][a-z0-9_]*:$", t):
            insn.kind, insn.label = "label", t[:-1]
            if insn.label in defined:
                raise RuleError("%s: label %s defined twice" % (insn.line, insn.label))
            defined.append(insn.label)
            continue
        mnem, _, rest = t.partition(" ")
        insn.mnemonic = mnem
        parts = [p.strip() for p in split_top(rest.strip(), ",")] if rest.strip() else []
        if mnem == "in":
            if len(parts) != 2 or parts[0] != "$s" or parts[1] not in EXPAND_INPUTS:
                raise RuleError("%s: in $s, <input>" % insn.line)
            insn.kind, insn.input = "in", parts[1]
            continue
        if mnem == "out":
            if len(parts) != 2 or parts[0] not in EXPAND_SLOTS:
                raise RuleError("%s: out <slot>, $s|xN|imm" % insn.line)
            insn.kind, insn.slot = "out", parts[0]
            insn.source = parse_exp_operand(parts[1], insn.line)
            if insn.source[0] == "imm":
                check_exp_imm(rule, insn, insn.source[1], "I32")
            elif insn.source[0] not in ("reg", "xmm"):
                raise RuleError("%s: bad out source" % insn.line)
            continue
        ops = [parse_exp_operand(p, insn.line) for p in parts]
        sse = [o for o in sse_ops if o.mnemonic == mnem]
        if sse:
            form = "".join({"xmm": "X", "reg": "R", "imm": "I"}.get(k, "?") for k, _ in ops)
            match = [o for o in sse if o.form == form]
            if not match:
                raise RuleError("%s: no form of %s takes %s" % (insn.line, mnem, form))
            insn.kind, insn.op, insn.operands = "sse", match[0], ops
            if match[0].mnemonic.startswith("vfmadd") and x["host"] != "fma3":
                raise RuleError("%s: %s needs host fma3" % (insn.line, mnem))
            if form == "XI":
                check_exp_imm(rule, insn, ops[1][1], "I8")
            continue
        name = mnem
        for alias, cc in CC_ALIASES.items():
            if name == "j" + alias:
                name = "j" + cc
        if name not in EXPAND_X86:
            raise RuleError("%s: %s is not allowed in an expansion" % (insn.line, mnem))
        kinds = [k for k, _ in ops]
        for c in x86_by_name.get(name, []):
            if (
                [operand_kind(sp) for sp in c.specs] == kinds
                and not c.implicit
                and all(operand_kind(sp) != "reg" or sp.startswith("R64") for sp in c.specs)
            ):
                insn.op = c
                break
        else:
            raise RuleError("%s: no form of %s takes (%s)" % (insn.line, mnem, ", ".join(kinds)))
        insn.kind, insn.operands = "x86", ops
        for (k, v), spec in zip(ops, insn.op.specs):
            if k == "imm":
                check_exp_imm(rule, insn, v, spec)
            elif k == "label":
                used.append((insn.line, v, len(defined)))
    for line, label, at in used:
        if label in EXPAND_EXITS:
            continue
        if label not in defined:
            raise RuleError("%s: label %s is not defined" % (line, label))
        if defined.index(label) < at:
            raise RuleError("%s: label %s is not later (labels are forward only)" % (line, label))
    rule.labels = list(EXPAND_EXITS) + defined


def check_exp_imm(rule, insn, v, spec):
    """An immediate of an expansion: a literal or an expression over #sf
    and #mf whose every value fits spec."""
    lo, hi = {"I8": (0, 63), "I32": (-(2**31), 2**31 - 1), "I64": (-(2**63), 2**64 - 1)}[spec]
    if isinstance(v, int):
        vals = (v, v)
    else:
        e = ("imm", v) if isinstance(v, str) else v
        for b in expr_binders(e):
            if b not in EXPAND_IMMS:
                raise RuleError("%s: unbound immediate %s" % (insn.line, b))
        values = []
        names = sorted(expr_binders(e))

        def rec(i, env):
            if i == len(names):
                values.append(eval_expr(e, env))
                return
            for val in EXPAND_IMMS[names[i]]:
                env[names[i]] = val
                rec(i + 1, env)

        rec(0, {})
        vals = (min(values), max(values))
    if not (lo <= vals[0] and vals[1] <= hi):
        raise RuleError("%s: immediate out of range for %s" % (insn.line, spec))


def load_expand_rules(path=FPFAST_PATH, x86_path=X86ENC_PATH, sse_path=X86SSE_PATH):
    _, by_name, _ = load_x86_ops(x86_path)
    sse = load_sse_ops(sse_path)
    with open(path) as f:
        rules = parse_expand(f.read(), os.path.relpath(path, ROOT))
    for r in rules:
        resolve_expand(r, by_name, sse)
    return rules
