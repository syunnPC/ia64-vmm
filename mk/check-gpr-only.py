#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the general-registers-only paths of the linked VMM image.

Every function reachable from the
roots below, through direct calls, jumps, fall-through into assembly labels
and the declared indirect call sites, must lie in an object listed on the
command line (BM_GPR_ONLY_SOURCES and the assembly objects) and must not
touch x87, MMX, SSE, AVX or AVX-512 state. Every function of those objects
is checked the same way. The rest of the image must use SIMD registers, so
the check cannot pass on an image built without the baseline flags.

Usage: check-gpr-only.py OBJDUMP ELF MAP OBJECT...
"""

import bisect
import collections
import fnmatch
import re
import subprocess
import sys

# Vector entry stubs and their common paths, the SIMD enable and its stop,
# and the log and panic paths, which every context may take.
ROOTS = [
    "exception_common",
    "interrupt_common",
    "amd64_simd_enable",
    "simd_baseline_stop",
    "vmm_log",
    "hal_panic",
]
# What a CPU runs before amd64_simd_enable: each function of a chain is read
# in address order up to its first call of a later one, and every call before
# that point is a root. The boot CPU enters at _start, an AP at boot_ap, whose
# amd64_enter_cpu reaches amd64_cpu_loop through its one indirect call.
CHAINS = [
    ["boot/entry.S:_start", "boot/limine.c:boot_main", "host/amd64/cpu.c:amd64_simd_enable"],
    [
        "boot/limine.c:boot_ap",
        "boot/entry.S:amd64_enter_cpu",
        "host/amd64/smp.c:amd64_cpu_loop",
        "host/amd64/smp.c:amd64_smp_online",
        "host/amd64/cpu.c:amd64_cpu_init_index",
        "host/amd64/cpu.c:amd64_simd_enable",
    ],
]
CHAIN_INDIRECT = {"boot/entry.S:amd64_enter_cpu"}
ROOT_PATTERN = re.compile(r"(exception|interrupt)_\d+")
# Indirect call sites on those paths and every function each can reach.
# Targets may be patterns (fnmatch): every block backend and USB transport.
CONSOLE = ["host/fb/fb_console.c:console_write"]
BLOCK = [
    "host/amd64/plog_*.c:read_blocks",
    "host/amd64/plog_*.c:write_blocks",
    "host/amd64/plog_*.c:flush",
]
INDIRECT = {
    "engine/hostwin/trap.c:hostwin_trap": [
        "engine/tier1/trap.c:t1_trap_client",
        "host/amd64/tests/hwb.c:trap",
    ],
    "core/log.c:mirrors": CONSOLE,
    "core/log.c:serial_drain": CONSOLE,
    "core/log.c:vmm_guest_serial_show_tail": CONSOLE,
    "persist/log.c:commit": BLOCK,
    "persist/log.c:read": BLOCK,
    "persist/log.c:write": BLOCK,
    "persist/log.c:flush": BLOCK,
    "persist/nvram.c:read": BLOCK,
    "persist/nvram.c:write": BLOCK,
    "persist/nvram.c:flush": BLOCK,
    "persist/fat32.c:rd": ["persist/log.c:read", "persist/nvram.c:read"],
    "persist/fat32.c:wr": ["persist/log.c:write", "persist/nvram.c:write"],
    "persist/fat32.c:metadata": ["persist/log.c:read", "persist/nvram.c:read"],
    "persist/fat32.c:fat32_flush": ["persist/log.c:flush", "persist/nvram.c:flush"],
    "persist/bot.c:bot_execute": [
        "host/amd64/plog_*.c:bot_transfer",
        "host/amd64/plog_*.c:clear_halt",
        "host/amd64/plog_*.c:reset_bot",
    ],
    "persist/bot.c:scsi_execute": ["host/amd64/plog_*.c:pause_ms"],
    "platform/intx.c:platform_intx_interrupt": ["host/amd64/intx.c:mask"],
    "host/amd64/simd.c:amd64_simd_missing": ["host/amd64/cpu.c:amd64_cpuid"],
    # External Hyper-V call page: TLFS memory-based hypercalls preserve SIMD
    # state. These exact adapters never use the optional XMM calling convention.
    "host/amd64/hyperv.c:amd64_hyperv_post": [],
    "host/amd64/hyperv.c:amd64_hyperv_signal": [],
}
# The only functions of the checked objects that may manage SIMD state.
STATE_SETUP = {
    "host/amd64/cpu.c:amd64_simd_enable",
    "host/amd64/xstate.S:amd64_xstate_switch",
    "host/amd64/xstate.S:amd64_xstate_init_x87",
    "host/amd64/ia32_fp_switch.S:amd64_ia32_fp_enter",
    "host/amd64/ia32_fp_switch.S:amd64_ia32_fp_leave",
    "host/amd64/ia32_fp_switch.S:amd64_ia32_fp_mxcsr_mask",
}

SIMD_OPERAND = re.compile(r"%([xyzt]mm\d+|mm\d|st\b|k[0-7]\b)")
SIMD_MNEMONIC = re.compile(
    r"(ldmxcsr|stmxcsr|vldmxcsr|vstmxcsr|vzeroupper|vzeroall|emms|femms|xsetbv|"
    r"xsave\w*|xrstor\w*|tile\w*|ldtilecfg|sttilecfg|f(?!s$)\w*)$"
)
PREFIXES = {
    "lock",
    "rep",
    "repz",
    "repnz",
    "repe",
    "repne",
    "notrack",
    "bnd",
    "data16",
    "addr32",
    "cs",
    "ds",
    "es",
    "ss",
    "fs",
    "gs",
}
CLONE = re.compile(r"\.(isra|part|constprop|cold|lto_priv)(\.\d+)?")
HEADER = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
INSN = re.compile(r"^\s*([0-9a-f]+):\s+(.*)$")
DIRECT = re.compile(r"^(call|jmp|j[a-z]+|loop\w*)\s+([0-9a-f]+) <")
SWITCH = re.compile(r"^jmp\s+\*(-?0x[0-9a-f]+)\(,%\w+,8\)$")
NO_FALLTHROUGH = re.compile(r"^(ret|jmp|iretq|ud2|sysretq)\b")
MASK = (1 << 64) - 1


def base(name):
    while True:
        stripped = CLONE.sub("", name)
        if stripped == name:
            return name
        name = stripped


def stem(path):
    for prefix in ("build/checked/bm/", "build/bm/"):
        if path.startswith(prefix):
            return path[len(prefix) : -2]
    return path.rsplit(".", 1)[0]


def declared(text):
    path, _, name = text.partition(":")
    return stem(path) + ":" + name


def load_map(path):
    """(start, end, object) for every .text* input section."""
    ranges, pending = [], False
    for line in open(path, errors="replace"):
        fields = line.split()
        if pending:
            pending = False
            if len(fields) == 3 and fields[0].startswith("0x"):
                fields = [".text"] + fields
            else:
                continue
        elif not line.startswith(" .text"):
            continue
        if len(fields) == 1:
            pending = True
        elif len(fields) == 4 and fields[1].startswith("0x") and int(fields[2], 16):
            start = int(fields[1], 16)
            ranges.append((start, start + int(fields[2], 16), fields[3]))
    return sorted(ranges)


def run(*command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def analyse(objdump, elf, mapfile):
    """Functions of the image: name, object, direct callees, indirect sites, SIMD use."""
    ranges = load_map(mapfile)
    starts = [r[0] for r in ranges]

    def owner(address):
        i = bisect.bisect_right(starts, address) - 1
        return ranges[i][2] if i >= 0 and address < ranges[i][1] else None

    functions, current = {}, None
    for line in run(objdump, "-d", "--no-show-raw-insn", elf).splitlines():
        match = HEADER.match(line)
        if match:
            current = int(match.group(1), 16)
            functions[current] = {
                "name": match.group(2),
                "object": owner(current),
                "calls": set(),
                "indirect": [],
                "simd": [],
                "last": "",
                "events": [],
            }
            continue
        match = INSN.match(line)
        if not match or current is None:
            continue
        text = match.group(2).strip()
        words = text.split()
        while words and words[0] in PREFIXES:
            words = words[1:]
        bare = " ".join(words)
        f = functions[current]
        f["last"] = bare
        if SIMD_OPERAND.search(text) or SIMD_MNEMONIC.match(words[0] if words else ""):
            f["simd"].append(match.group(1) + ": " + text)
        direct = DIRECT.match(bare)
        if direct:
            f["calls"].add(int(direct.group(2), 16))
            f["events"].append(int(direct.group(2), 16))
        elif re.match(r"(call|jmp)\s+\*", bare):
            f["indirect"].append((match.group(1), bare))
            f["events"].append(None)
    order = sorted(functions)
    for i, address in enumerate(order):
        functions[address]["end"] = order[i + 1] if i + 1 < len(order) else MASK

    def containing(address):
        i = bisect.bisect_right(order, address) - 1
        return order[i] if i >= 0 else None

    for address, f in functions.items():
        f["calls"] = {
            c for c in (containing(t) for t in f["calls"]) if c is not None and c != address
        }
        f["events"] = [e if e is None else containing(e) for e in f["events"]]
        f["key"] = stem(f["object"] or "?") + ":" + base(f["name"])

    typed = set()
    for line in run(objdump, "-t", elf).splitlines():
        fields = line.split()
        if " F .text" in line and re.fullmatch(r"[0-9a-f]{16}", fields[0]):
            typed.add(int(fields[0], 16))
    # Fall-through only into an untyped assembly label of the same object.
    for previous, following in zip(order, order[1:]):
        p, n = functions[previous], functions[following]
        if (
            following not in typed
            and p["object"]
            and p["object"] == n["object"]
            and not NO_FALLTHROUGH.match(p["last"])
        ):
            p["calls"].add(following)

    rodata = {}
    for line in run(objdump, "-s", "-j", ".rodata", elf).splitlines():
        fields = line.split()
        if len(fields) >= 2 and re.fullmatch(r"[0-9a-f]{16}", fields[0]):
            words = [w for w in fields[1:5] if re.fullmatch(r"[0-9a-f]{8}", w)]
            rodata[int(fields[0], 16)] = bytes.fromhex("".join(words))

    def quad(address):
        line = address & ~15
        chunk = rodata.get(line, b"") + rodata.get(line + 16, b"")
        offset = address - line
        return (
            int.from_bytes(chunk[offset : offset + 8], "little")
            if len(chunk) >= offset + 8
            else None
        )

    # A switch dispatch: the table's first entry lies in the same function.
    for address, f in functions.items():
        kept = []
        for site, bare in f["indirect"]:
            table = SWITCH.match(bare)
            entry = quad(int(table.group(1), 16) & MASK) if table else None
            if entry is None or not address <= entry < f["end"]:
                kept.append(site + ": " + bare)
        f["indirect"] = kept

    return functions


def check(functions, allowed, indirect, extra=()):
    """Problems of the image for the given objects and indirect-site table."""
    by_key = collections.defaultdict(list)
    for address, f in functions.items():
        by_key[f["key"]].append(address)
    problems = []

    def resolve(text):
        pattern = declared(text)
        found = [a for key in fnmatch.filter(by_key, pattern) for a in by_key[key]]
        if not found:
            problems.append(f"declared function {text} is not in the image")
        return found

    roots = [a for a, f in functions.items() if ROOT_PATTERN.fullmatch(f["name"])]
    by_name = {f["name"]: a for a, f in functions.items()}
    for name in ROOTS:
        if name in by_name:
            roots.append(by_name[name])
        else:
            problems.append(f"root {name} is not in the image")
    for chain in CHAINS:
        elements = [resolve(name) for name in chain]
        if not all(elements):
            continue
        position = 0
        while position < len(chain) - 1:
            f = functions[elements[position][0]]
            later = {a: i for i in range(position + 1, len(chain)) for a in elements[i]}
            following = None
            for event in f["events"]:
                if event is None:
                    if chain[position] in CHAIN_INDIRECT:
                        following = position + 1
                        break
                    problems.append(f"{chain[position]}: indirect call before amd64_simd_enable")
                elif event in later:
                    following = later[event]
                    break
                elif event != elements[position][0]:
                    roots.append(event)
            if following is None:
                problems.append(f"{chain[position]} does not reach {chain[position + 1]}")
                break
            position = following
    sites = {}
    for site, targets in indirect.items():
        # A site the image does not link (AMD64_HOST_ONLY) needs no targets.
        if by_key.get(declared(site)):
            sites[declared(site)] = [a for t in targets for a in resolve(t)]
    setup = {declared(name) for name in STATE_SETUP}

    reached, queue = {}, collections.deque((a, None) for a in roots)
    edges = collections.defaultdict(set)
    for caller, callee in extra:
        edges[caller].add(callee)
    while queue:
        address, parent = queue.popleft()
        if address in reached:
            continue
        reached[address] = parent
        f = functions[address]
        queue.extend((callee, address) for callee in f["calls"] | edges[address])
        if f["indirect"]:
            targets = sites.get(f["key"])
            if targets is None:
                via = functions[parent]["name"] if parent is not None else "a root"
                problems.append(
                    f"{f['key']}: undeclared indirect call on a general-registers-only path "
                    f"({f['indirect'][0]}; reached from {via})"
                )
            else:
                queue.extend((t, address) for t in targets)

    def via(address):
        parent = reached.get(address)
        return f" (reached from {functions[parent]['name']})" if parent is not None else ""

    for address in sorted(reached):
        f = functions[address]
        if f["object"] not in allowed:
            problems.append(
                f"{f['key']}{via(address)} is not compiled general-registers-only "
                f"(object {f['object']})"
            )
    checked = {a for a, f in functions.items() if f["object"] in allowed} | set(reached)
    for address in sorted(checked):
        f = functions[address]
        if f["simd"] and f["key"] not in setup:
            problems.append(f"{f['key']}{via(address)} touches SIMD/x87 state: {f['simd'][0]}")
    elsewhere = sum(
        1
        for f in functions.values()
        if f["object"] not in allowed and any(SIMD_OPERAND.search(i) for i in f["simd"])
    )
    if not elsewhere:
        problems.append(
            "no function outside the general-registers-only objects uses SIMD registers: "
            "the image was not built with the SIMD baseline"
        )
    return (
        problems,
        f"{len(reached)} functions on general-registers-only paths, {len(checked)} checked, "
        f"{elsewhere} others use SIMD",
    )


# Each variant must fail: the check itself is tested against this image.
MUTANT_OBJECTS = ["core/log", "core/string", "host/amd64/apic", "engine/tier1/trap", "platform/msi"]


def mutants(functions, allowed):
    """Variants of this image's own inputs; those it does not link are skipped."""
    failures = []
    linked = {stem(f["object"]) for f in functions.values() if f["object"]}
    for name in MUTANT_OBJECTS:
        reduced = {o for o in allowed if stem(o) != name}
        if name in linked and (reduced == allowed or not check(functions, reduced, INDIRECT)[0]):
            failures.append(f"{name} compiled with SIMD")
    reduced = {
        site: targets for site, targets in INDIRECT.items() if not site.endswith(":hostwin_trap")
    }
    if "engine/hostwin/trap" in linked and not check(functions, allowed, reduced)[0]:
        failures.append("undeclared hostwin_trap client call")
    by_name = {f["name"]: a for a, f in functions.items()}
    simd = next(
        (
            a
            for a, f in sorted(functions.items())
            if f["object"] not in allowed and any(SIMD_OPERAND.search(i) for i in f["simd"])
        ),
        None,
    )
    # A SIMD function called on the AP path before amd64_simd_enable.
    entry = next((f for f in functions.values() if f["key"] == declared(CHAINS[1][0])), None)
    if entry is None or simd is None:
        failures.append("AP chain entry or a SIMD function missing")
    else:
        entry["events"].insert(0, simd)
        if not check(functions, allowed, INDIRECT)[0]:
            failures.append("SIMD function called from boot_ap before amd64_simd_enable")
        del entry["events"][0]
    if (
        simd is None
        or not check(
            functions,
            allowed | {functions[simd]["object"]},
            INDIRECT,
            [(by_name["amd64_interrupt"], simd)],
        )[0]
    ):
        failures.append("SIMD function called from amd64_interrupt")
    return failures


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    objdump, elf, mapfile, allowed = sys.argv[1], sys.argv[2], sys.argv[3], set(sys.argv[4:])
    functions = analyse(objdump, elf, mapfile)
    problems, summary = check(functions, allowed, INDIRECT)
    if not problems:
        problems = [f"variant not caught: {m}" for m in mutants(functions, allowed)]
    for problem in problems:
        print("check-gpr-only: " + problem, file=sys.stderr)
    if problems:
        sys.exit(1)
    print(f"check-gpr-only: passed ({summary}; broken variants caught)")


if __name__ == "__main__":
    main()
