# SPDX-License-Identifier: GPL-2.0-or-later
"""Parse smp.efi output and compare it with the expected outcome per target.

The application (tests/efi/smp/smp.c) prints, between "SMP-APP BEGIN" and
"SMP-APP END pass=P fail=F skip=S cpus=N", one line per case ("PASS <case>",
"FAIL <case> <detail>", "SKIP <case> <reason>") and "INFO <case> k=v ..."
measurements. Expectations are keyed on the target and on the CPU count the
application itself reports, never on what a runner requested, so a runner
needs no change when the VMM passes `--vcpus` reach the guest.
"""

import re

CASES = (
    "sal-ap-wake",
    "set-vectors-boot-rendez",
    "madt-exact",
    "ap-start",
    "ap-entry-state",
    "repeat-rendezvous",
    "ipi-ping-pong",
    "itc-skew",
    "mp",
    "sb",
    "sem-sb",
    "fetchadd",
    "cmpxchg-lock",
    "xchg-lock",
    "purge-remote",
    "purge-halted",
)
BOOT_CASES = CASES[:3]
AP_CASES = CASES[3:]
LITMUS_CASES = ("mp", "sb", "sem-sb", "fetchadd", "cmpxchg-lock", "xchg-lock")
PURGE_CASES = ("purge-remote", "purge-halted")
START_CASES = BOOT_CASES + ("ap-start", "ap-entry-state", "repeat-rendezvous")
# Cases each profile runs (smp.c SMP_CASES); prerequisites included.
PROFILE_CASES = {
    "all": CASES,
    "long": CASES,
    "quick": CASES,
    "ipi-itc": START_CASES + ("ipi-ping-pong", "itc-skew"),
    "no-purge": tuple(c for c in CASES if c not in PURGE_CASES),
    "tlb-purge": START_CASES + PURGE_CASES,
}
# itc-skew: the smallest common-release spread (ITC ticks) over 1000 rounds.
# A conservative 400-tick spread bound at virtual ITC 400 MHz. A constant
# offset appears even in the minimum spread and detects unsynchronized ITCs.
FORK_ITC_SPREAD_MIN_BOUND = 400

# Known differences of the fork (recorded, not failures of this suite).
FORK_KNOWN = {
    "madt-exact": (
        "FAIL",
        r"^\d+ disabled Local SAPIC entries$",
        "the fork lists 64 Local SAPIC entries (QEMU's limit); "
        "the suite wants exactly the allocated CPUs",
    ),
    "sem-sb": (
        "FAIL",
        r"^cmpxchg1\.rel: forbidden outcome o00 seen \d+ times in \d+"
        r"(; cmpxchg1\.acq: forbidden outcome o00 seen \d+ times in \d+)?$",
        "fork defect: a cmpxchg whose ar.ccv does "
        "not fit the operand takes a plain load (target/ia64/translate/"
        "translate.c load_only path) and IA64_TCG_MO_RELEASE has no ST_LD, so the "
        "earlier store passes the semaphore read on an x86 host",
    ),
}

LINE = re.compile(r"^(PASS|FAIL|SKIP|INFO) (\S+)(?: (.*))?$")
END = re.compile(r"^SMP-APP END pass=(\d+) fail=(\d+) skip=(\d+) cpus=(\d+)$")
BEGIN = re.compile(r"^SMP-APP BEGIN profile=(\S+)")


class LogError(Exception):
    pass


def guest_lines(raw, prefix=None):
    """Lines of guest text; `prefix` (e.g. b'[guest] ') selects and strips."""
    text = raw.decode("utf-8", "replace") if isinstance(raw, bytes) else raw
    out = []
    for line in text.replace("\r", "").split("\n"):
        if prefix is not None:
            if not line.startswith(prefix):
                continue
            line = line[len(prefix) :]
        out.append(line)
    return out


def parse(lines):
    """The first complete BEGIN..END block: cases, info and summary."""
    begin = next((i for i, l in enumerate(lines) if BEGIN.match(l)), None)
    if begin is None:
        raise LogError('no "SMP-APP BEGIN" line')
    result = {"profile": BEGIN.match(lines[begin])[1], "cases": {}, "info": {}, "order": []}
    for line in lines[begin + 1 :]:
        m = END.match(line)
        if m:
            result["summary"] = dict(zip(("pass", "fail", "skip", "cpus"), map(int, m.groups())))
            break
        m = LINE.match(line)
        if not m:
            continue
        kind, case, rest = m[1], m[2], m[3] or ""
        if kind == "INFO":
            result["info"].setdefault(case, []).append(
                dict(kv.split("=", 1) for kv in rest.split() if "=" in kv)
            )
            continue
        if case in result["cases"]:
            raise LogError(f"case {case} reported twice")
        result["cases"][case] = (kind, rest)
        result["order"].append(case)
    else:
        raise LogError('no "SMP-APP END" line')
    s = result["summary"]
    counts = [
        sum(1 for k, _ in result["cases"].values() if k == x) for x in ("PASS", "FAIL", "SKIP")
    ]
    if counts != [s["pass"], s["fail"], s["skip"]]:
        raise LogError(f"summary {s} disagrees with the case lines {counts}")
    return result


def expected(target, cpus, profile="all"):
    """{case: (status, detail regex or None, note)} for a correct system."""
    exp = {}
    for case in PROFILE_CASES.get(profile, CASES):
        if case in BOOT_CASES:
            exp[case] = ("PASS", None, "")
        elif cpus < 2:
            exp[case] = ("SKIP", r"^1 CPU$", "")
        elif case == "ipi-ping-pong" and cpus < 3:
            exp[case] = ("SKIP", rf"^{cpus} CPUs \(needs 3\)$", "")
        else:
            exp[case] = ("PASS", None, "")
    if target == "fork":
        for case, value in FORK_KNOWN.items():
            if case in exp and exp[case][0] != "SKIP":
                exp[case] = value
    return exp


# Negative variants (smp.c SMP_VARIANT_broken_*): the case each one breaks
# and the FAIL detail it must produce. Other cases are reported, not judged.
BROKEN = {
    "wake-vector": {"sal-ap-wake": r"^vector=0x0+ff$"},
    "set-vectors": {"set-vectors-boot-rendez": r"^status=-\d+$"},
    "smbios": {"madt-exact": r"^smbios=\d+ madt=\d+$"},
    "nocallin": {"ap-start": r"^no call-in cpu=\d+ lid=0x[0-9a-f]+$"},
    "lid": {"ap-start": r"^lid mismatch cpu=\d+ expected=0x[0-9a-f]+ reported=0x[0-9a-f]+"},
    "entry-gp": {"ap-entry-state": r"^gp=0x[0-9a-f]+ registered=0x[0-9a-f]+ cpu=\d+(;|$)"},
    "repeat": {"repeat-rendezvous": r"^round 2: \d+ of \d+ APs called in$"},
    "ipi": {"ipi-ping-pong": r"^lost IPI: "},
    "itc": {"itc-skew": r"^ITC not one time base: .* causality violations$"},
    "mp": {"mp": r"^st\.rel/ld\.acq: forbidden outcome o10 seen [1-9]\d* times"},
    "sb": {"sb": r"^st/mf/ld: forbidden outcome o00 seen [1-9]\d* times"},
    "sem-sb": {"sem-sb": r"^cmpxchg1\.rel: forbidden outcome o00 seen [1-9]\d* times"},
    "nonatomic": {
        c: r"^lost updates: [1-9]\d* of \d+$" for c in ("fetchadd", "cmpxchg-lock", "xchg-lock")
    },
    "purge-local": {
        c: r"^cpu=\d+ stale translation after ptc\.ga: access hit=1 tak=0x0+(;|$)"
        for c in PURGE_CASES
    },
}
# Minimum CPUs a negative variant needs to reach its case.
BROKEN_MIN_CPUS = {"wake-vector": 1, "set-vectors": 1, "smbios": 1, "ipi": 3}


def check(result, exp):
    """Mismatches between a parsed result and expectations, as strings."""
    problems = []
    for case, (status, detail_re, _note) in exp.items():
        got = result["cases"].get(case)
        if got is None:
            problems.append(f"{case}: missing (expected {status})")
            continue
        if got[0] != status:
            problems.append(f"{case}: {got[0]} {got[1]} (expected {status})")
        elif detail_re and not re.search(detail_re, got[1]):
            problems.append(f'{case}: {got[0]} "{got[1]}" does not match /{detail_re}/')
    for case in result["cases"]:
        if case not in CASES:
            problems.append(f"{case}: unknown case")
        elif case not in exp:
            problems.append(f"{case}: not in profile {result['profile']}")
    return problems


def check_broken(result, variant):
    problems = []
    for case, detail_re in BROKEN[variant].items():
        got = result["cases"].get(case)
        if got is None:
            problems.append(f"{case}: missing (expected FAIL)")
        elif got[0] != "FAIL" or not re.search(detail_re, got[1]):
            problems.append(f'{case}: {got[0]} "{got[1]}" (expected FAIL /{detail_re}/)')
    return problems


def histograms(result):
    """One short line per litmus/lock measurement, for runner summaries."""
    out = []
    for case in ("mp", "sb", "sem-sb"):
        for info in result["info"].get(case, []):
            out.append(
                f"{case} {info.get('form', '')}: o00={info.get('o00')} o01={info.get('o01')} "
                f"o10={info.get('o10')} o11={info.get('o11')} forbidden={info.get('forbidden')}"
            )
    for case in ("fetchadd", "cmpxchg-lock", "xchg-lock"):
        for info in result["info"].get(case, []):
            out.append(f"{case}: " + " ".join(f"{k}={v}" for k, v in info.items()))
    for case in ("ipi-ping-pong", "itc-skew", "ap-start", "purge-remote", "purge-halted"):
        for info in result["info"].get(case, []):
            out.append(f"{case}: " + " ".join(f"{k}={v}" for k, v in info.items()))
    return out


def itc_spread_min(result):
    for info in result["info"].get("itc-skew", []):
        if "spread_min" in info:
            return int(info["spread_min"])
    return None


def loadavg():
    try:
        with open("/proc/loadavg") as f:
            return " ".join(f.read().split()[:3])
    except OSError:
        return "unavailable"
