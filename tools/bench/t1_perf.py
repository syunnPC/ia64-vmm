# SPDX-License-Identifier: GPL-2.0-or-later
"""Where Tier1's time goes on a asm loop.

Runs `build/tier1-test --bench=ENGINE:PROGRAM:SEGMENT` pinned to one CPU
under `perf record -e cycles:u --call-graph dwarf` and attributes every
sample to a category. Prints the shares and /proc/loadavg before and after.
Needs perf with user-space cycles.

--engine tier1 (default): production regions with the harness's stepping
reference as the lower tier (v1's dispatcher). A sample goes to the first
category found on its call chain (category order): generated code (samples
whose IP perf cannot resolve: the anonymous code arena), the lower-tier
step, window misses, the per-entry MMU-context diff (hostwin_enter), region
lookup and fetch guard (find), materialization, code-page validation, the
dispatch loop's per-iteration checks, compiles, the harness set-up.

--engine tier0+tier1: the product path (Tier0 the outer loop, Tier1 its
upper tier). The bench prints the code arenas and the
live regions (`bench-arena`, `bench-region`); a sample whose leaf IP lies in
an arena is classified by address (Tier1 stub page; a loop region's code
before its innermost loop body = prologue and LIVEIN loads, the innermost
loop body (loop_start/loop_end), the code after it = exit
blocks and cold stubs; in a loop nest the outer loops' code counts before
or after the innermost body; a trace region as a whole; Tier0 code). Any
other sample goes to the
category of the first frame, from the leaf up, that names one, with shared
source-file fallbacks used only when no caller identifies the work (frames are
keyed by function and source file, so Tier0's and Tier1's static `enter`,
`run` and `probe` differ; lines of Tier0's `run` inside the upper-tier hook
count as the hook). The bench runs with T1H_HOSTWIN_CHECK=0: the harness
otherwise runs the MMU-log completeness check at every entry. Other hosted
checks remain enabled and have a separate category. Per-entry cycles =
samples x period / Tier1 entries of the whole run. The entry/exit C runtime
is a partial attribution, not a proof of the <= 50-cycle entry/exit or
<= 200-cycle total targets: shared helpers with no caller, trace regions,
retired regions and outer-loop work cannot be fully partitioned. Generated
restore routines and frame-in images have separate categories when the dump
includes their bounds; older metadata leaves them in the mixed code
category. No fixed per-region prologue size is assumed.

--job FILE (with --variants, engine tier0+tier1): a compiled-C job instead
of a asm program, run by build/bench-jobdump --regions, which lists the
arenas and regions the same way.

Without precise sampling (perf's :pp), a sample can land after the
instruction that stalled, often at the next serializing instruction.
Attribution can move between categories in either direction; shares are
estimates, not upper bounds. Confirm a gain with paired A/B runs. When perf
resolves no symbols or call chains (a perf built without symbol or
unwinding support), only the leaf of each sample is known: its function
and source file come from addr2line through the binary's mappings, and
the categories match that leaf frame alone. Shared code validity stays
separate when no Tier0/Tier1 caller is available. Source-line attribution
requires the profiled binary to match the current source tree. Hosted
implementation samples cannot predict the cost of a production build.

    python3 -I tools/bench/t1_perf.py [--engine tier1|tier0+tier1] [--program gather_phys]
                                      [--job FILE --variants 0,2,2] [--segment 100000000]
                                      [--period N] [--cpu N]
"""

import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

CATS = [
    ("lower-tier step (interp_run(v, 1) + stepping reference)", {"lower", "ref_lower", "ref_run"}),
    ("window miss (t1_window_miss, hostwin_bind)", {"t1_window_miss"}),
    ("entry: MMU-context diff (hostwin_enter)", {"hostwin_enter", "hw_context_sync"}),
    ("entry: region lookup + fetch guard (find)", {"find", "t1_region_lookup", "fetch_guard"}),
    ("exit: materialize (t1_materialize)", {"t1_materialize"}),
    (
        "entry: code-page validation (code_page_enter/publish)",
        {"code_page_enter", "code_source_publish"},
    ),
    (
        "dispatch loop checks (needs_lower, timer poll, reclaim, hot note)",
        {
            "needs_lower",
            "ia64_timer_poll",
            "t1_reclaim",
            "t1_hot_note",
            "ia64_external_pending",
            "vcpu_next_timer_deadline",
            "hal_counter",
        },
    ),
    ("compile", {"t1_compile_at", "compile_once"}),
    ("harness set-up (to the loop header)", {"t1h_run_to_loop_header", "guest_setup"}),
]
FRAME = re.compile(r"\s+[0-9a-f]+\s+(.*?)\s+\((.*)\)\s*$")


def categorize(script_text):
    counts = collections.Counter()
    total = 0
    frames, started = [], False

    def flush():
        nonlocal total
        if not started:
            return
        total += 1
        if not frames:  # no frame resolved: the IP lies in the anonymous code arena
            counts["generated code (regions and stubs)"] += 1
            return
        names = [f for f in frames]
        for name, syms in CATS:
            if any(s in syms for s in names):
                counts[name] += 1
                return
        counts[
            "dispatcher, other (t1_run/run, budget, exit log)"
            if any(s in ("t1_run", "run") for s in names)
            else "other"
        ] += 1

    for line in script_text.splitlines():
        if not line.strip() or line[0] not in " \t":
            flush()
            frames, started = [], bool(line.strip()) and "cycles" in line
            continue
        m = FRAME.match(line)
        if m:
            frames.append(re.sub(r"\+0x[0-9a-f]+$", "", m.group(1)).replace(" (inlined)", ""))
    flush()
    return counts, total


# ---- the product path (--engine tier0+tier1) -------------------------------

# Groups of the per-entry account.
OVERHEAD, MATERIALIZE, CODE_ENTRY, GUEST, TIER0, HOSTED, SHARED, OTHER = (
    "attributed production entry/exit C runtime",
    "materialization",
    "Tier1 code outside the innermost loop (mixed runtime and guest work)",
    "guest work in Tier1",
    "Tier0 (outer loop, its code and helpers)",
    "hosted implementation and optional checking",
    "shared runtime (caller unavailable)",
    "compile, harness, other",
)

# (category, group, keys): a key is "function" or "function@file" (the
# basename perf's srcline reports) or "@file" (any function of that file).
PRODUCT_CATS = [
    (
        "hosted/checked source lines, TLS current-context implementation and verification hooks",
        HOSTED,
        {"@hosted", "t1_set_current", "@vhooks.c", "@checked.c"},
    ),
    (
        "hook: Tier0's upper-tier lines in run, upper_sync, upper_due, the engine.c wrapper",
        OVERHEAD,
        {
            "upper_sync@tier0.c",
            "upper_due@tier0.c",
            "upper_enter@engine.c",
            "upper_reclaim@engine.c",
            "t1_upper_reclaim",
            "t1_reclaim",
            "@hook",
        },
    ),
    (
        "enter: link liveness and contract (t1_region_from_link, t1_region_matches)",
        OVERHEAD,
        {"t1_region_from_link", "t1_region_matches"},
    ),
    (
        "enter: guards (lower-tier PSR, rse, budget, NaT guard, fetch guard of other pages)",
        OVERHEAD,
        {
            "lower_tier_psr@dispatch.c",
            "entry_budget@dispatch.c",
            "t1_nat_guard_holds",
            "fetch_guard@dispatch.c",
            "fetch_guard_cached@dispatch.c",
            "rse_incomplete@dispatch.c",
        },
    ),
    (
        "enter: code validation (validate_entry, entry_fetch)",
        OVERHEAD,
        {"validate_entry@dispatch.c", "entry_fetch@dispatch.c"},
    ),
    (
        "enter: MMU context (hostwin_enter: fast path, log replay, full diff)",
        OVERHEAD,
        {"hostwin_enter", "hw_context_sync", "hw_context_update", "@context.c"},
    ),
    ("enter: entry image (t1_frame_in_fill)", OVERHEAD, {"t1_frame_in_fill"}),
    (
        "enter/exit bookkeeping (t1_upper_enter, enter, execute, exit log, demotion windows)",
        OVERHEAD,
        {
            "t1_upper_enter",
            "enter@dispatch.c",
            "execute@dispatch.c",
            "log_exit@dispatch.c",
            "t1_demote_after_exit",
            "t1_demote_after_nat_guard",
            "t1_point_consumed",
            "hostwin_bind_epoch",
            "t1_arena_seal_for_entry",
        },
    ),
    (
        "entry/exit: remaining dispatch runtime (publication, preparation, chain handoff, finish)",
        OVERHEAD,
        {"@dispatch.c"},
    ),
    ("exit: t1_materialize and restore helpers", MATERIALIZE, {"t1_materialize", "@materialize.c"}),
    ("exit: generated restore routines", MATERIALIZE, {"@jit-restore"}),
    ("entry: generated frame-in image", CODE_ENTRY, {"@jit-frame-in"}),
    (
        "Tier1 code: before innermost body (entry code and outer-loop work)",
        CODE_ENTRY,
        {"@jit-entry"},
    ),
    (
        "Tier1 code: after innermost body (exit/restore code and outer-loop work)",
        CODE_ENTRY,
        {"@jit-exit"},
    ),
    (
        "Tier1 code: stub page (exit_save/exit_nosave, epilogue; miss, helper, trap stubs)",
        CODE_ENTRY,
        {"@jit-stubs"},
    ),
    ("Tier1 code: innermost loop bodies", GUEST, {"@jit-loop"}),
    ("Tier1 code: trace regions (whole: prologue, body, exits)", GUEST, {"@jit-trace"}),
    ("Tier1 code: no live region (retired since)", GUEST, {"@jit-gone"}),
    (
        "Tier1 runtime called from code (t1_window_miss, t1_helper_call, t1_trap_resolve)",
        GUEST,
        {"t1_window_miss", "t1_helper_call", "t1_trap_resolve"},
    ),
    ("Tier0 code", TIER0, {"@jit-tier0"}),
    (
        "Tier0 helpers (tier0_helper, tier0_memory_pointer and callees)",
        TIER0,
        {"tier0_helper", "tier0_breakpoint_helper", "tier0_memory_pointer"},
    ),
    ("Tier0 interpreter steps (interp_run)", TIER0, {"interp_run", "interpreter_step@tier0.c"}),
    (
        "Tier0 outer loop (run, lookup, fetch translation, timer and interrupt polls, region call)",
        TIER0,
        {
            "run@tier0.c",
            "tier0_run",
            "lookup@tier0.c",
            "lookup_attempt@tier0.c",
            "needs_interpreter@tier0.c",
            "entry_key@tier0.c",
            "key_equal@tier0.c",
            "key_hash@tier0.c",
            "execute_region@tier0.c",
            "enter@tier0.c",
            "homes_for@tier0.c",
            "resolve_homes@tier0.c",
            "advance@tier0.c",
        },
    ),
    ("shared code validity (Tier0/Tier1 caller unavailable)", SHARED, {"@validity.c"}),
    (
        "compiles, probes, hot() (Tier0 translation, Tier1 compile)",
        OTHER,
        {
            "t1_upper_probe",
            "t1_upper_hot",
            "upper_probe@engine.c",
            "upper_hot@engine.c",
            "t1_compile_hot",
            "t1_compile_at",
            "@translate.c",
            "@x86emit.c",
        },
    ),
    (
        "harness: run loop around tier0_run, set-up",
        OTHER,
        {
            "t1h_tier0t1_slice",
            "t1h_tier0t1_run",
            "t1h_engine_run",
            "guest_setup",
            "t1h_load_image",
            "t1h_engine_bind",
            "vcpu_create",
            "hostwin_create",
            "t1h_run_to_loop_header",
        },
    ),
]
SRC = re.compile(r"^\s+(\S*?):(\d+)( \(inlined\))?\s*$")
PFRAME = re.compile(r"^\t\s*([0-9a-f]+)\s+(.*?)\s*$")
KERNEL = 0xFFFF800000000000


def tier0_hook_lines(tier0_c):
    """Lines of tier0.c's run that belong to the upper-tier hook: the hit
    block from `bool skipped` to the close of its upper-tier condition,
    the loop-top reclaim and the loop-head note."""
    lines = Path(tier0_c).read_text().splitlines()
    hook = set()
    start = next(
        (i for i, l in enumerate(lines) if re.search(r"\bbool\s+skipped\s*=\s*skip\b", l)), None
    )
    opened = next(
        (
            i
            for i in range(start or 0, len(lines))
            if re.search(r"\bif\s*\(\s*upper\s*&&\s*!skipped\b", lines[i])
        ),
        None,
    )
    if start is None or opened is None:
        raise SystemExit("t1_perf: cannot locate Tier0 upper-tier hook; update source attribution")
    depth = 0
    for i in range(opened, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if depth == 0 and "}" in lines[i]:
            hook.update(range(start + 1, i + 2))
            break
    else:
        raise SystemExit("t1_perf: unterminated Tier0 upper-tier hook")
    for i, l in enumerate(lines):
        if "upper->reclaim()" in l or l.strip().startswith("loop_head ="):
            hook.add(i + 1)
    return hook


def hosted_source_lines(paths):
    """Source locations in hosted/checked branches; unknown conditions stay shared.

    This locates observed checks, not the cycles a production build would save.
    Build and source must agree, as for Tier0 hook source attribution.
    """
    result = {}
    for path in paths:
        stack, selected = [], set()
        for number, line in enumerate(Path(path).read_text().splitlines(), 1):
            directive = re.match(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", line)
            if directive:
                kind, condition = directive.groups()
                optional = bool(re.search(r"\bIA64VMM_(?:HOSTED|TIER1_CHECKED)\b", condition))
                # Only positive conditions establish a checking-only branch.
                positive = optional and "!" not in condition and kind != "ifndef"
                if kind in ("if", "ifdef", "ifndef"):
                    stack.append(positive)
                elif kind == "elif" and stack:
                    stack[-1] = positive
                elif kind == "else" and stack:
                    stack[-1] = False
                elif kind == "endif" and stack:
                    stack.pop()
            elif any(stack):
                selected.add(number)
        result[Path(path).name] = selected
    return result


def parse_bench(out):
    """Arenas and live regions from tier1-test's bench-arena/bench-region
    lines or bench-jobdump's jobdump-arena/jobdump-region lines (whose
    entries come from the jobrun lines)."""
    arena, regions, job_entries = None, [], 0
    for line in out.splitlines():
        f = line.split()
        if line.startswith(("bench-arena ", "jobdump-arena ")):
            t1, t0 = f.index("tier1"), f.index("tier0")
            arena = {
                "t1": (int(f[t1 + 1], 16), int(f[t1 + 2], 16)),
                "t0": (int(f[t0 + 1], 16), int(f[t0 + 2], 16)),
                "entries": int(f[f.index("entries") + 1]) if "entries" in f else None,
            }
        elif line.startswith(("bench-region ", "jobdump-region ")):
            loop = f.index("loop")
            code, size = int(f[f.index("code") + 1], 16), int(f[f.index("size") + 1])
            ls, le = int(f[loop + 1]), int(f[loop + 2])
            if not 0 <= ls <= le <= size:
                raise SystemExit("t1_perf: invalid region loop bounds")
            region = (code, size, ls, le)
            if "restore" in f:
                restore = f.index("restore")
                rs, re_ = int(f[restore + 1]), int(f[restore + 2])
                if not 0 <= rs <= re_ <= size:
                    raise SystemExit("t1_perf: invalid region restore bounds")
                region += (rs, re_)
            if "frame-in" in f:
                frame_in = f.index("frame-in")
                fs, fe = int(f[frame_in + 1]), int(f[frame_in + 2])
                if not 0 <= fs <= fe <= size:
                    raise SystemExit("t1_perf: invalid region frame-in bounds")
                if len(region) == 4:
                    region += (0, 0)
                region += (fs, fe)
            regions.append(region)
        elif f and f[0] == "jobrun" and "entries" in f:
            job_entries += int(f[f.index("entries") + 1])
    if not arena:
        raise SystemExit("t1_perf: no bench-arena/jobdump-arena line (engine tier0+tier1 only)")
    if arena["entries"] is None:
        arena["entries"] = job_entries
    return arena, sorted(regions)


MMAP2 = re.compile(
    r"PERF_RECORD_MMAP2 .*\[(0x[0-9a-f]+)\((0x[0-9a-f]+)\) @ (0x[0-9a-f]+|0) .*\]: r-xp (\S+)$"
)


def leaf_frames(data, binary, ips):
    """One frame per sample, the leaf: [ip, function, source file basename,
    line], resolved with addr2line through the binary's executable mappings
    (perf's PERF_RECORD_MMAP2 events); other IPs get no name."""
    mmaps = []
    events = subprocess.run(
        ["perf", "script", "-i", str(data), "--show-mmap-events", "-F", "ip"],
        capture_output=True,
        text=True,
    ).stdout
    for line in events.splitlines():
        m = MMAP2.search(line)
        if m and Path(m.group(4)).resolve() == Path(binary).resolve():
            mmaps.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)))
    offsets = {}
    for ip in ips:
        for start, size, pgoff in mmaps:
            if start <= ip < start + size:
                offsets[ip] = ip - start + pgoff
    names = {}
    unique = sorted(set(offsets.values()))
    for i in range(0, len(unique), 2000):
        chunk = unique[i : i + 2000]
        out = subprocess.run(
            ["addr2line", "-f", "-e", str(binary)] + [hex(o) for o in chunk],
            capture_output=True,
            text=True,
        ).stdout.splitlines()
        for k, off in enumerate(chunk):
            sym, where = out[2 * k], out[2 * k + 1]
            src, _, line = where.rpartition(":")
            line = line.split()[0] if line else ""
            names[off] = (
                sym,
                Path(src).name if src and src != "??" else None,
                int(line) if line.isdigit() else 0,
            )
    samples = []
    for ip in ips:
        sym, src, line = names.get(offsets.get(ip), ("[unknown]", None, 0))
        samples.append([[ip, sym, src, line]] if src or sym != "[unknown]" else [])
    return samples


def jit_key(ip, arena, regions):
    t1, t0 = arena["t1"], arena["t0"]
    if t1[0] <= ip < t1[1]:
        if ip < t1[0] + 4096:
            return "@jit-stubs"
        for region in regions:
            code, size, ls, le = region[:4]
            if code <= ip < code + size:
                off = ip - code
                if len(region) > 4 and region[4] <= off < region[5]:
                    return "@jit-restore"
                if len(region) > 6 and region[6] <= off < region[7]:
                    return "@jit-frame-in"
                if le == 0:
                    return "@jit-trace"
                return "@jit-entry" if off < ls else "@jit-loop" if off < le else "@jit-exit"
        return "@jit-gone"
    if t0[0] <= ip < t0[1]:
        return "@jit-tier0"
    return None


def product_categorize(
    script_text, ips, arena, regions, hook_lines, detail=None, leaf=None, hosted=None
):
    index = {}
    for name, _group, keys in PRODUCT_CATS:
        for k in keys:
            index[k] = name
    counts, total = collections.Counter(), 0
    samples, cur = [], None
    if leaf is not None:
        samples, script_text = leaf, ""
    # perf script: a header line per sample, then per frame "\t ip sym" and,
    # when resolved, "  file:line[ (inlined)]".
    for line in script_text.splitlines():
        if not line.strip():
            continue
        if line[0] not in " \t":
            cur = [] if "cycles" in line else None
            if cur is not None:
                samples.append(cur)
            continue
        if cur is None:
            continue
        m = PFRAME.match(line)
        if m:
            cur.append([int(m.group(1), 16), m.group(2), None, 0])
            continue
        m = SRC.match(line)
        if m and cur:
            cur[-1][2], cur[-1][3] = Path(m.group(1)).name, int(m.group(2))
    # A sample whose IP perf cannot map (generated code) prints no frame at
    # all, so the leaf comes from the call-graph-less listing (same order).
    if len(samples) != len(ips):
        raise SystemExit(f"t1_perf: {len(samples)} samples with call graphs, {len(ips)} sample IPs")
    for frames, leaf in zip(samples, ips):
        total += 1
        user = [f for f in frames if f[0] < KERNEL]
        name = None
        k = jit_key(leaf, arena, regions)
        if k:
            name = index[k]
        if name is None and hosted and any(line in hosted.get(src, ()) for _, _, src, line in user):
            name = index["@hosted"]
        if name is None:
            fallback = None
            for ip, sym, src, line in user:
                if jit_key(ip, arena, regions):
                    continue
                sym = re.sub(r"(?:\.(?:constprop|isra|part)\.\d+)?(?:\+0x[0-9a-f]+)?$", "", sym)
                if sym == "run" and src == "tier0.c" and line in hook_lines:
                    name = index["@hook"]
                elif src and f"{sym}@{src}" in index:
                    name = index[f"{sym}@{src}"]
                elif sym in index:
                    name = index[sym]
                elif src in ("vhooks.c", "checked.c", "materialize.c"):
                    name = index[f"@{src}"]
                # Shared helpers inherit a resolved caller's category. A
                # source-file fallback is useful when only the leaf exists.
                elif fallback is None and src and f"@{src}" in index:
                    fallback = index[f"@{src}"]
                if name:
                    break
            name = name or fallback
        counts[name or "other"] += 1
        if detail is not None:
            leaf = next(
                (
                    (f"{sym}@{src}:{line}" if src else sym)
                    for ip, sym, src, line in user
                    if not jit_key(ip, arena, regions)
                ),
                f"{leaf:#x}",
            )
            detail[name or "other"][leaf] += 1
    return counts, total


def run_perf(
    engine, program, segment, period, cpu, fields, job=None, variants="0,2,2", binary=None
):
    images = sorted(str(p) for p in (common.REPO / "build/tests/tier1").glob("*.bin"))
    images.append(str(common.REPO / "build/tests/ia64/speed.bin"))
    if job:
        images.append(str(common.REPO / "build/bench/csuite/csuite.bin"))
        binary = Path(binary) if binary else common.REPO / "build/bench-jobdump"
        argv = [
            str(binary),
            f"--run={engine}",
            f"--job={job}",
            f"--variants={variants}",
            "--regions",
        ] + images
    else:
        binary = Path(binary) if binary else common.REPO / "build/tier1-test"
        argv = [str(binary), f"--bench={engine}:{program}:{segment}"] + images
    with tempfile.TemporaryDirectory() as tmp:
        data = Path(tmp) / "perf.data"
        before = common.loadavg()
        # The product path without the harness's MMU-log completeness check
        # (a hosted test of MMU log; hostwin_check=0 is the product's
        # default): tests/tier1/engine_tier1.c reads T1H_HOSTWIN_CHECK.
        env = dict(os.environ, T1H_HOSTWIN_CHECK="0")
        r = subprocess.run(
            [
                "taskset",
                "-c",
                str(cpu),
                "perf",
                "record",
                "-q",
                "-e",
                "cycles:u",
                "-c",
                str(period),
                "--call-graph",
                "dwarf,8192",
                "-o",
                str(data),
                "--",
            ]
            + argv,
            cwd=common.REPO,
            capture_output=True,
            text=True,
            env=env,
        )
        after = common.loadavg()
        if r.returncode:
            raise SystemExit(f"t1_perf: perf record failed:\n{r.stderr}")
        script = subprocess.run(
            ["perf", "script", "-i", str(data), "-F", fields], capture_output=True, text=True
        ).stdout
        listing = subprocess.run(
            ["perf", "script", "-i", str(data), "-F", "ip", "--hide-call-graph"],
            capture_output=True,
            text=True,
        ).stdout
        ips = [int(line.split()[0], 16) for line in listing.splitlines() if line.strip()]
        # The kernel throttles a sampling rate above perf_event_max_sample_rate
        # (dwarf stacks lower it): throttled intervals are not sampled.
        stats = subprocess.run(
            ["perf", "report", "-i", str(data), "--stats"], capture_output=True, text=True
        ).stdout
        m = re.search(r"THROTTLE events:\s+(\d+)", stats)
        throttles = int(m.group(1)) if m else 0
        # A perf without symbol or unwinding support prints no named frame.
        leaf = None
        if not re.search(r"^\t\s*[0-9a-f]+\s+(?!\[unknown\])\S", script, re.M):
            leaf = leaf_frames(data, binary, ips)
    return r.stdout, script, ips, before, after, throttles, leaf


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--engine", default="tier1", choices=["tier1", "tier0+tier1"])
    ap.add_argument("--program", default="gather_phys")
    ap.add_argument(
        "--segment",
        type=int,
        help="guest instructions per bench segment (default 10^8; 4 x 10^9 for tier0+tier1)",
    )
    ap.add_argument(
        "--period",
        type=int,
        help="cycles per sample (default 100000; 4000000 for tier0+tier1, "
        "below the kernel's sampling-rate limit with dwarf stacks)",
    )
    ap.add_argument("--cpu", type=int)
    ap.add_argument(
        "--job", help="a compiled-C job (bench-jobdump) instead of --program; engine tier0+tier1"
    )
    ap.add_argument(
        "--variants", default="0,2,2", help="--job: the variants to run (default 0,2,2)"
    )
    ap.add_argument(
        "--binary", type=Path, help="override the benchmark binary (must match current sources)"
    )
    ap.add_argument(
        "--detail",
        type=int,
        default=0,
        metavar="N",
        help="tier0+tier1: also print each category's N most frequent leaf frames",
    )
    a = ap.parse_args()
    cpu = a.cpu if a.cpu is not None else common.pick_cpu()[0]
    if a.job and a.engine != "tier0+tier1":
        raise SystemExit("t1_perf: --job needs --engine tier0+tier1")
    product = a.engine == "tier0+tier1"
    period = a.period or (4000000 if product else 100000)
    segment = a.segment or (4000000000 if product else 100000000)
    fields = "comm,tid,event,ip,sym,srcline" if product else "comm,tid,event,ip,sym,dso"
    out, script, ips, before, after, throttles, leaf = run_perf(
        a.engine, a.program, segment, period, cpu, fields, a.job, a.variants, a.binary
    )
    print(
        "\n".join(
            l
            for l in out.strip().splitlines()
            if not l.startswith(("bench-region ", "jobdump-region "))
        )
    )
    print(
        f"cpu {cpu}; period {period} cycles; load average before {before}, after {after}; "
        f"throttle events {throttles}"
    )
    print(
        "note: sampling skid can shift attribution between categories; shares are estimates, "
        "confirm gains with paired A/B runs"
    )
    if leaf is not None:
        print("note: perf resolved no symbols or call chains: leaf frames from addr2line only")
    if not product:
        if leaf is not None:
            script = "\n".join(
                f"{a.engine} cycles:" + (f"\n\t{s[0][0]:x} {s[0][1]} (x)" if s else "")
                for s in leaf
            )
        counts, total = categorize(script)
        for name, n in counts.most_common():
            print(f"{100.0 * n / total:6.1f} %  {n:6d}  {name}")
        print(f"total samples {total}")
        return
    arena, regions = parse_bench(out)
    hook = tier0_hook_lines(common.REPO / "engine/tier0/tier0.c")
    hosted = hosted_source_lines(
        common.REPO / f"engine/tier1/{name}" for name in ("dispatch.c", "materialize.c", "region.h")
    )
    detail = collections.defaultdict(collections.Counter) if a.detail else None
    counts, total = product_categorize(script, ips, arena, regions, hook, detail, leaf, hosted)
    if not total:
        raise SystemExit("t1_perf: no cycle samples; increase the workload or reduce --period")
    entries = arena["entries"]
    print(f"Tier1 entries in the run {entries}; {len(regions)} live regions; total samples {total}")
    sums = collections.Counter()
    print(
        "note: source-line attribution requires matching sources and binary; hosted/checked samples "
        "are separate and do not predict production-build cost"
    )
    print(
        "note: only explicit restore/frame-in spans separate generated routines; outer-loop work, "
        "traces, retired code and shared helpers make entry-cost attribution partial"
    )
    for group in (OVERHEAD, MATERIALIZE, CODE_ENTRY, GUEST, TIER0, HOSTED, SHARED, OTHER):
        print(f"-- {group}")
        names = [n for n, g, _ in PRODUCT_CATS if g == group] + (
            ["other"] if group == OTHER else []
        )
        for name in names:
            n = counts.get(name, 0)
            cyc = n * period / entries if entries else float("nan")
            sums[group] += n
            print(f"{100.0 * n / total:6.1f} %  {n:7d}  {cyc:8.1f} cycles/entry  {name}")
            if detail is not None:
                for leaf, k in detail[name].most_common(a.detail):
                    print(f"{'':28s}{k:7d}  {leaf}")
    per = {g: sums[g] * period / entries for g in sums} if entries else {}
    if entries:
        overhead = per.get(OVERHEAD, 0.0)
        total_entry = overhead + per.get(MATERIALIZE, 0.0) + per.get(CODE_ENTRY, 0.0)
        print(f"attributed production entry/exit C runtime {overhead:.1f} cycles per entry")
        print(
            f"runtime + materialization + mixed code {total_entry:.1f} cycles per entry "
            f"({overhead:.1f} + {per.get(MATERIALIZE, 0.0):.1f} + {per.get(CODE_ENTRY, 0.0):.1f})"
        )
        print(
            f"hosted/checked {per.get(HOSTED, 0.0):.1f}; shared runtime {per.get(SHARED, 0.0):.1f} "
            f"cycles per entry; partial attribution cannot establish the <=50/<=200 targets"
        )
        print(
            f"for scale: Tier0 {per.get(TIER0, 0.0):.1f} and guest work in Tier1 {per.get(GUEST, 0.0):.1f} "
            f"cycles per entry"
        )


if __name__ == "__main__":
    main()
