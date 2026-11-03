#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Replay a VMM log's port_trace lines into the standard VGA register file.

Reads the `port_trace` lines of a serial log or PLOG text (platform/devices.c
format) in order and applies them as the IBM VGA register interface
defines: index/data pairs at 3C4/3C5 and 3CE/3CF, the CRTC at 3D4/3D5 or
3B4/3B5 by Miscellaneous Output bit 0 with CR11 bit 7 protecting CR0-7
(except CR7 bit 4), and the attribute controller's 3C0 index/data
flip-flop that an Input Status 1 read (the CRTC base + 6) returns to index.
A wider access is a sequence of byte cycles at port, port + 1, ... (low
byte first). Denied accesses never reached the card and are only counted.
The DAC is not modelled.

It prints the register state and the display mode it describes (text or
graphics, resolution, memory window, chain-4/odd-even/planar addressing),
the registers that differ from the IBM VGA parameter table of mode 03h or
12h (`--expect`), and the difference to a reference log's final state
(`--reference`). `--until REGEX` stops before the first log line that
matches, for example the first INT10 return; `--tail N` lists the last
accesses replayed. A register is known only once the trace writes it;
unknown registers print as `--`. Vendor extension registers are kept by
index but not decoded. The trace holds only the first N accesses
(port_trace=N) and no display memory contents.
"""

import argparse
from collections import deque
import json
import re
import sys

LINE = re.compile(
    r"\[warn\] port_trace: (in|out|in-denied|out-denied) port=([0-9a-f]+) "
    r"size=(\d+) value=([0-9a-f]+) ip=([0-9a-f]+:\d+)"
)

# IBM VGA video parameter table entries (BIOS mode set): SR1-4, Misc,
# CR0-18h, AR0-13h, GR0-8. SR0 and AR14h are not in the table.
MODE_TABLES = {
    "03": {
        "misc": 0x67,
        "seq": [0x00, 0x03, 0x00, 0x02],
        "crtc": [
            0x5F,
            0x4F,
            0x50,
            0x82,
            0x55,
            0x81,
            0xBF,
            0x1F,
            0x00,
            0x4F,
            0x0D,
            0x0E,
            0x00,
            0x00,
            0x00,
            0x00,
            0x9C,
            0x8E,
            0x8F,
            0x28,
            0x1F,
            0x96,
            0xB9,
            0xA3,
            0xFF,
        ],
        "attr": [
            0x00,
            0x01,
            0x02,
            0x03,
            0x04,
            0x05,
            0x14,
            0x07,
            0x38,
            0x39,
            0x3A,
            0x3B,
            0x3C,
            0x3D,
            0x3E,
            0x3F,
            0x0C,
            0x00,
            0x0F,
            0x08,
        ],
        "gc": [0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF],
    },
    "12": {
        "misc": 0xE3,
        "seq": [0x01, 0x0F, 0x00, 0x06],
        "crtc": [
            0x5F,
            0x4F,
            0x50,
            0x82,
            0x54,
            0x80,
            0x0B,
            0x3E,
            0x00,
            0x40,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0xEA,
            0x8C,
            0xDF,
            0x28,
            0x00,
            0xE7,
            0x04,
            0xE3,
            0xFF,
        ],
        "attr": [
            0x00,
            0x01,
            0x02,
            0x03,
            0x04,
            0x05,
            0x14,
            0x07,
            0x38,
            0x39,
            0x3A,
            0x3B,
            0x3C,
            0x3D,
            0x3E,
            0x3F,
            0x01,
            0x00,
            0x0F,
            0x00,
        ],
        "gc": [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0xFF],
    },
}
# Cursor shape/location and start address are moved by text output, not by a mode set.
SKIP_CRTC = {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F}
# The registers printed and compared.
NAMES = (
    ["misc"]
    + [f"SR{i:02X}" for i in range(5)]
    + [f"CR{i:02X}" for i in range(0x19)]
    + [f"GR{i:02X}" for i in range(9)]
    + [f"AR{i:02X}" for i in range(0x15)]
)
# Accesses kept for --tail: where a stalled guest last touched the card.
RECENT = 4096


class VgaState:
    """Register file of one VGA as far as a trace shows it. A register is
    known only from the bits the trace writes; unknown bits read as 0, except
    Misc bit 0 (CRTC at 3D4h, as mode 03h leaves it) and CR11 bit 7 (CR0-7
    protected, as every BIOS mode set leaves it), which start set."""

    def __init__(self):
        self.value = {"misc": 0x01, "CR11": 0x80}
        self.known = {}
        self.seq_index = self.gc_index = self.crtc_index = self.attr_index = 0
        self.attr_data_next = False
        self.attr_pas = False
        self.denied = self.accesses = 0
        self.last = None
        self.recent = deque(maxlen=RECENT)

    def get(self, name):
        return self.value.get(name, 0)

    def set(self, name, value, mask=0xFF):
        self.value[name] = (self.get(name) & ~mask) | (value & mask)
        self.known[name] = self.known.get(name, 0) | mask

    def crtc_base(self):
        return 0x3D4 if self.get("misc") & 1 else 0x3B4

    def out(self, port, value):
        crtc = self.crtc_base()
        if port == 0x3C2:
            self.set("misc", value)
        elif port == 0x3C4:
            self.seq_index = value
        elif port == 0x3C5:
            self.set(f"SR{self.seq_index:02X}", value)
        elif port == 0x3CE:
            self.gc_index = value
        elif port == 0x3CF:
            self.set(f"GR{self.gc_index:02X}", value)
        elif port == crtc:
            self.crtc_index = value
        elif port == crtc + 1:
            index = self.crtc_index
            if index >= 8 or not self.get("CR11") & 0x80:
                self.set(f"CR{index:02X}", value)
            elif index == 7:
                self.set("CR07", value, 0x10)
        elif port == 0x3C0:
            if self.attr_data_next:
                self.set(f"AR{self.attr_index:02X}", value)
            else:
                self.attr_index, self.attr_pas = value & 0x1F, bool(value & 0x20)
            self.attr_data_next = not self.attr_data_next

    def read(self, port):
        if port == self.crtc_base() + 6:
            self.attr_data_next = False

    def decode(self):
        g = self.get
        gr_graphics, ar_graphics = bool(g("GR06") & 1), bool(g("AR10") & 1)
        kind = (
            "graphics"
            if gr_graphics and ar_graphics
            else "text"
            if not gr_graphics and not ar_graphics
            else "mixed (GR06 %s, AR10 %s)"
            % ("graphics" if gr_graphics else "text", "graphics" if ar_graphics else "text")
        )
        columns = g("CR01") + 1
        scanlines = (g("CR12") | (g("CR07") & 2) << 7 | (g("CR07") & 0x40) << 3) + 1
        scan = (g("CR09") & 0x1F) + 1
        double = bool(g("CR09") & 0x80)
        repeat = scan * (2 if double else 1)
        address = "dword" if g("CR14") & 0x40 else "byte" if g("CR17") & 0x40 else "word"
        if g("SR04") & 8:
            addressing = "chain-4"
        elif not g("SR04") & 4 or g("GR05") & 0x10:
            addressing = "odd/even"
        else:
            addressing = "planar"
        if kind == "graphics":
            width, height, rows = (
                columns * 8 // (2 if g("AR10") & 0x40 else 1),
                scanlines // repeat,
                None,
            )
        else:
            width, height, rows = (
                columns * (8 if g("SR01") & 1 else 9),
                scanlines,
                scanlines // repeat,
            )
        return {
            "kind": kind,
            "width": width,
            "height": height,
            "scanlines": scanlines,
            "columns": columns,
            "rows": rows,
            "scanlines_per_row": scan,
            "double_scan": double,
            "bytes_per_line": g("CR13") * 2 * {"byte": 1, "word": 2, "dword": 4}[address],
            "address_mode": address,
            "start_address": g("CR0C") << 8 | g("CR0D"),
            "memory": ["A0000-BFFFF", "A0000-AFFFF", "B0000-B7FFF", "B8000-BFFFF"][
                g("GR06") >> 2 & 3
            ],
            "addressing": addressing,
            "write_mode": g("GR05") & 3,
            "read_mode": g("GR05") >> 3 & 1,
            "shift256": bool(g("GR05") & 0x40),
            "map_mask": g("SR02") & 0xF,
            "bit_mask": g("GR08"),
            "screen_off": bool(g("SR01") & 0x20),
            "palette_enabled": self.attr_pas,
            "crtc_port": "%x" % self.crtc_base(),
        }

    def show(self, name):
        """hh known, ~hh partly known (unknown bits as 0), -- unknown."""
        mask = self.known.get(name, 0)
        return (
            "--"
            if not mask
            else "%02x" % self.get(name)
            if mask == 0xFF
            else "~%02x" % self.get(name)
        )


def parse(lines):
    """('port', direction, port, size, value, ip, line) per port_trace line, ('line', line) otherwise."""
    for text in lines:
        text = text.rstrip("\n")
        m = LINE.search(text)
        if m and "[guest]" not in text[: m.start()]:
            yield ("port", m[1], int(m[2], 16), int(m[3]), int(m[4], 16), m[5], text)
        else:
            yield ("line", text)


def replay(state, entries, until=None):
    pattern = re.compile(until) if until else None
    for entry in entries:
        if pattern and pattern.search(entry[-1]):
            state.last = entry
            return False
        if entry[0] != "port":
            continue
        _, direction, port, size, value, ip, _ = entry
        state.accesses += 1
        state.recent.append(entry)
        if direction.endswith("denied"):
            state.denied += 1
            continue
        for i in range(size):
            if direction == "out":
                state.out(port + i, value >> (8 * i) & 0xFF)
            else:
                state.read(port + i)
    return True


def differences(state, table):
    """(register, actual or None when unknown, expected) for every table
    register whose known bits differ or that the trace never wrote."""
    expected = {"misc": table["misc"]}
    for i, v in enumerate(table["seq"]):
        expected[f"SR{i + 1:02X}"] = v
    for i, v in enumerate(table["crtc"]):
        if i not in SKIP_CRTC:
            expected[f"CR{i:02X}"] = v
    for i, v in enumerate(table["attr"]):
        expected[f"AR{i:02X}"] = v
    for i, v in enumerate(table["gc"]):
        expected[f"GR{i:02X}"] = v
    out = []
    for name, value in expected.items():
        mask = state.known.get(name, 0)
        if not mask:
            out.append((name, None, value))
        elif (state.get(name) ^ value) & mask:
            out.append((name, state.get(name), value))
    return out


def compare(a, b):
    """(register, a, b) for registers whose commonly known bits differ or that
    only one trace wrote; None marks unknown."""
    out = []
    for name in NAMES:
        ma, mb = a.known.get(name, 0), b.known.get(name, 0)
        if (ma or mb) and (ma != mb or (a.get(name) ^ b.get(name)) & ma):
            out.append((name, a.get(name) if ma else None, b.get(name) if mb else None))
    return out


def load(path, until):
    state = VgaState()
    with open(path, errors="replace") as f:
        complete = replay(state, parse(f), until)
    return state, complete


def print_state(title, state, complete):
    print(
        f"{title}: {state.accesses} port accesses replayed ({state.denied} denied)"
        + ("" if complete else f"; stopped before: {state.last[-1]}")
    )
    for key, value in state.decode().items():
        print(f"  {key}: {value}")
    print(f"  misc: {state.show('misc')}")
    for prefix in ["SR", "CR", "GR", "AR"]:
        print(f"  {prefix}: " + " ".join(state.show(n) for n in NAMES if n.startswith(prefix)))
    if any(state.show(n)[0] in "-~" for n in NAMES):
        print("  (--: not written in the trace, ~: partly written; unknown bits decode as 0)")


def hexa(value):
    return "--" if value is None else f"{value:02x}"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("log", help="VMM serial log or PLOG text with port_trace lines")
    parser.add_argument("--until", metavar="REGEX", help="stop before the first matching log line")
    parser.add_argument(
        "--reference", metavar="LOG", help="compare with the final state of another log"
    )
    parser.add_argument("--reference-until", metavar="REGEX", help="--until for the reference log")
    parser.add_argument(
        "--expect", choices=sorted(MODE_TABLES), help="list registers that differ from this mode"
    )
    parser.add_argument(
        "--tail",
        type=int,
        default=0,
        metavar="N",
        help=f"also list the last N (at most {RECENT}) replayed accesses",
    )
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args(argv)
    state, complete = load(args.log, args.until)
    result = {
        "decode": state.decode(),
        "registers": {n: state.show(n) for n in NAMES},
        "complete": complete,
        "stopped_before": None if complete else state.last[-1],
        "accesses": state.accesses,
        "denied": state.denied,
    }
    if args.expect:
        result["expect"] = differences(state, MODE_TABLES[args.expect])
    if args.reference:
        ref, ref_complete = load(args.reference, args.reference_until)
        result["reference"] = {"decode": ref.decode(), "differences": compare(state, ref)}
    if args.tail:
        result["tail"] = [e[-1] for e in list(state.recent)[-args.tail :]]
    if args.json:
        print(json.dumps(result, indent=2))
        return 0
    print_state(args.log, state, complete)
    if args.tail:
        print(f"last {len(result['tail'])} accesses:")
        for line in result["tail"]:
            print(f"  {line}")
    if args.expect:
        print(f"differences from mode {args.expect} (register: actual expected):")
        for name, actual, value in result["expect"]:
            print(f"  {name}: {hexa(actual)} {value:02x}")
    if args.reference:
        print_state(args.reference, ref, ref_complete)
        print("differences (register: this reference):")
        for name, actual, value in result["reference"]["differences"]:
            print(f"  {name}: {hexa(actual)} {hexa(value)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
