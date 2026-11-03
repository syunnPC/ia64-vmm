#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The `tier1 linked= compiles= entries= ...` line a VMM run prints for
every engine (engine/engine.c engine_dump_stats; product dispatch).

check(log, engine) returns the parsed counters or raises Tier1StatsError:
the line must be there; with any engine but tier1, no Tier1 code ran
(compiles=0, entries=0: only engine=tier1 registers the upper tier); with
engine=tier1, Tier1 is linked and compiled and entered regions.
summary(counters, retired) is the report: Tier1 share of the retired
instructions (retired: the run's total, e.g. the exit milestone's), entries,
compiles, declines.

The `hostwin instances= fills= ... unbacked= backend_failures=` line next to
it describes the host windows of the vCPUs that
ran Tier1. Guest RAM that the windows cannot alias (anonymous memory instead
of hostwin_linux_alloc) does not stop Tier1: its sites exit as unbacked and
the Tier1 share drops, which no share threshold can tell from a workload
with less Tier1 code. So a run that ran Tier1 fails on any unbacked exit or
backend failure, and must have mapped window pages; without Tier1 the line
shows no instance.

check(log, engine, checked=True) is a run of engine=lockstep tier1=on
(checked execution): Tier1 runs, checked, so the run must
show it like engine=tier1, and its `tier1 lockstep checked= ...` line must
show every Tier1 execution checked (checked equal to the entries, none
unchecked) and no divergence.
"""

import re

LINE = re.compile(
    rb"tier1 linked=(?P<linked>\d) compiles=(?P<compiles>\d+) rejects=(?P<rejects>\d+) "
    rb"entries=(?P<entries>\d+) tier1_retired=(?P<tier1_retired>\d+) probes=(?P<probes>\d+) "
    rb"links=(?P<links>\d+) enters=(?P<enters>\d+) ran=(?P<ran>\d+) ran_pending=(?P<ran_pending>\d+) "
    rb"declines=(?P<declines>\d+) hot=(?P<hot>\d+) held=(?P<held>\d+) demotions=(?P<demotions>\d+) "
    rb"evictions=(?P<evictions>\d+) replacements=(?P<replacements>\d+) recompiles=(?P<recompiles>\d+) "
    rb"invalidations=(?P<invalidations>\d+) site_exits=(?P<site_exits>\d+)"
)


LOCKSTEP = re.compile(
    rb"tier1 lockstep checked=(?P<checked>\d+) checked_retired=(?P<checked_retired>\d+) "
    rb"unchecked=(?P<unchecked>\d+) unchecked_retired=(?P<unchecked_retired>\d+) "
    rb"divergences=(?P<divergences>\d+) captures=(?P<captures>\d+) "
    rb"undo_pre_traps=(?P<undo_pre_traps>\d+)"
)
WINDOWS = re.compile(
    rb"hostwin instances=(?P<instances>\d+) fills=(?P<fills>\d+) fills_ro=(?P<fills_ro>\d+) "
    rb"live=(?P<live>\d+) unbacked=(?P<unbacked>\d+) not_ram=(?P<not_ram>\d+) "
    rb"backend_failures=(?P<backend_failures>\d+)"
)


class Tier1StatsError(Exception):
    pass


def last(regex, log):
    match = None
    for m in regex.finditer(log):
        match = m
    return match


def check(log, engine, checked=False):
    if checked and engine != "lockstep":
        raise Tier1StatsError(f"checked Tier1 is engine=lockstep tier1=on, not engine={engine}")
    runs_tier1 = engine == "tier1" or checked
    match = None
    for m in LINE.finditer(log):
        match = m  # the last line: the totals at the end of the run
    if match is None:
        raise Tier1StatsError('no "tier1 linked= compiles= ..." statistics line')
    c = {k: int(v) for k, v in match.groupdict().items()}
    if not runs_tier1 and (c["compiles"] or c["entries"] or c["rejects"] or c["enters"]):
        raise Tier1StatsError(f"engine={engine} ran Tier1 code: {match.group(0).decode()}")
    if runs_tier1 and (c["linked"] != 1 or not c["compiles"] or not c["entries"]):
        raise Tier1StatsError(
            f"engine={engine} did not compile and enter Tier1 regions: {match.group(0).decode()}"
        )
    lock = last(LOCKSTEP, log)
    if lock is None:
        raise Tier1StatsError('no "tier1 lockstep checked= ..." line')
    k = {k: int(v) for k, v in lock.groupdict().items()}
    if k["divergences"]:
        raise Tier1StatsError(f"Tier1 lockstep divergence: {lock.group(0).decode()}")
    if checked and (k["checked"] != c["entries"] or k["unchecked"] or not k["checked"]):
        raise Tier1StatsError(
            f"not every Tier1 execution was checked ({c['entries']} entries): "
            f"{lock.group(0).decode()}"
        )
    if not checked and (k["checked"] or k["unchecked"]):
        raise Tier1StatsError(
            f"checked Tier1 executions without tier1=on: {lock.group(0).decode()}"
        )
    c.update({f"lockstep_{k2}": v for k2, v in k.items()})
    windows = None
    for m in WINDOWS.finditer(log):
        windows = m
    if windows is None:
        raise Tier1StatsError('no "hostwin instances= fills= ..." line')
    w = {k: int(v) for k, v in windows.groupdict().items()}
    if not runs_tier1 and w["instances"]:
        raise Tier1StatsError(f"engine={engine} created host windows: {windows.group(0).decode()}")
    if w["unbacked"] or w["backend_failures"]:
        raise Tier1StatsError(
            f"host windows do not alias guest RAM (unbacked exits) or the backend failed: "
            f"{windows.group(0).decode()}"
        )
    if runs_tier1 and (not w["instances"] or not w["fills"]):
        raise Tier1StatsError(f"engine={engine} mapped no window page: {windows.group(0).decode()}")
    c.update({f"hostwin_{k}": v for k, v in w.items()})
    return c


def summary(c, retired=None):
    share = (
        f"{100.0 * c['tier1_retired'] / retired:.1f} % of {retired} retired, " if retired else ""
    )
    checked = (
        f"; checked lockstep: {c['lockstep_checked']} Tier1 executions checked "
        f"({c['lockstep_checked_retired']} instructions), {c['lockstep_unchecked']} unchecked, "
        f"{c['lockstep_divergences']} divergences, {c['lockstep_captures']} C-side captures"
        if c["lockstep_checked"] or c["lockstep_unchecked"]
        else ""
    )
    return (
        f"tier1 {share}{c['tier1_retired']} retired in Tier1, {c['entries']} entries, {c['compiles']} "
        f"compiles, {c['rejects']} rejects, {c['declines']} declines, {c['demotions']} demotions; "
        f"windows {c['hostwin_fills']} pages mapped ({c['hostwin_live']} live), "
        f"{c['hostwin_unbacked']} unbacked, {c['hostwin_not_ram']} not RAM, "
        f"{c['hostwin_backend_failures']} backend failures{checked}"
    )


def self_test():
    """Broken logs are rejected, good ones accepted (make test-milestones)."""

    def tier1(compiles=0, entries=0, linked=1):
        return (
            f"[info] tier1 linked={linked} compiles={compiles} rejects=0 entries={entries} tier1_retired=7 "
            f"probes=1 links=1 enters={entries} ran={entries} ran_pending=0 declines=0 hot=1 held=0 "
            f"demotions=0 evictions=0 replacements=0 recompiles=0 invalidations=0 site_exits=0\n"
        ).encode()

    def lockstep(checked=0, unchecked=0, divergences=0):
        return (
            f"[info] tier1 lockstep checked={checked} checked_retired={7 * checked} unchecked={unchecked} "
            f"unchecked_retired={unchecked} divergences={divergences} captures=0 undo_pre_traps=0\n"
        ).encode()

    def windows(instances=0, fills=0, unbacked=0, failures=0):
        return (
            f"[info] hostwin instances={instances} fills={fills} fills_ro=0 live={fills} "
            f"unbacked={unbacked} not_ram=3 backend_failures={failures}\n"
        ).encode()

    L = lockstep()
    good = [
        ("interp", tier1() + L + windows(), False),
        ("tier0", tier1() + L + windows(), False),
        ("lockstep", tier1() + L + windows(), False),
        ("tier1", tier1(3, 9) + L + windows(1, 40), False),
        # The last lines are the totals.
        ("tier1", tier1(1, 1) + L + windows(1, 1) + tier1(3, 9) + L + windows(1, 40), False),
        ("lockstep", tier1(3, 9) + lockstep(9) + windows(1, 40), True),
    ]
    bad = [
        ("interp", L + windows(), 'no "tier1 linked=', False),
        ("interp", tier1() + L, 'no "hostwin instances=', False),
        ("interp", tier1() + windows(), 'no "tier1 lockstep', False),
        ("interp", tier1(2, 2) + L + windows(), "ran Tier1 code", False),
        ("tier0", tier1() + L + windows(1, 0), "created host windows", False),
        ("tier1", tier1(0, 0) + L + windows(1, 4), "did not compile and enter", False),
        ("tier1", tier1(3, 9) + L + windows(0, 0), "mapped no window page", False),
        ("tier1", tier1(3, 9) + L + windows(1, 0), "mapped no window page", False),
        ("tier1", tier1(3, 9) + L + windows(1, 40, unbacked=3021), "do not alias guest RAM", False),
        ("tier1", tier1(3, 9) + L + windows(1, 40, failures=1), "backend failed", False),
        (
            "tier1",
            tier1(3, 9) + L + windows(1, 40) + windows(1, 41, unbacked=1),
            "do not alias guest RAM",
            False,
        ),
        ("tier1", tier1(3, 9) + L + windows(1, 40), "not engine=tier1", True),
        ("lockstep", tier1(3, 9) + L + windows(1, 40), "ran Tier1 code", False),
        ("lockstep", tier1(0, 0) + L + windows(), "did not compile and enter", True),
        (
            "lockstep",
            tier1(3, 9) + lockstep(8) + windows(1, 40),
            "not every Tier1 execution was checked",
            True,
        ),
        (
            "lockstep",
            tier1(3, 9) + lockstep(9, unchecked=1) + windows(1, 40),
            "not every Tier1 execution",
            True,
        ),
        ("lockstep", tier1(3, 9) + lockstep(9, divergences=1) + windows(1, 40), "divergence", True),
        ("lockstep", tier1() + lockstep(2) + windows(), "without tier1=on", False),
    ]
    for engine, log, checked in good:
        check(log, engine, checked)
    for engine, log, expected, checked in bad:
        try:
            check(log, engine, checked)
        except Tier1StatsError as error:
            if expected not in str(error):
                raise SystemExit(f"tier1stats self-test: {engine}: wrong reason: {error}")
            continue
        raise SystemExit(
            f"tier1stats self-test: {engine}: accepted a log that should fail ({expected})"
        )
    print(f"tier1stats self-test: {len(bad)} broken logs rejected, {len(good)} good logs accepted")


if __name__ == "__main__":
    import sys

    if sys.argv[1:] != ["--self-test"]:
        sys.exit("usage: tier1stats.py --self-test")
    self_test()
