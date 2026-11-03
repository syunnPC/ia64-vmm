#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests for tools/vbios/vga_ports.py on synthetic port_trace logs.

Expected behaviour follows the IBM VGA register interface: index/data pairs
at 3C4/3C5 (sequencer), 3CE/3CF (graphics controller) and the CRTC at
3D4/3D5 or 3B4/3B5 selected by Miscellaneous Output bit 0, with CR11 bit 7
write-protecting CR0-7 except CR7 bit 4; the attribute controller's single
port 3C0 with an index/data flip-flop that an Input Status 1 read (3DAh in
colour, 3BAh in monochrome addressing) returns to index. A 16-bit OUT reaches
port and port + 1 as two byte cycles, low byte first.

The mode streams below are written here from the IBM VGA Technical
Reference's BIOS video parameter table entries for modes 03h, 12h and 13h
(sequencer 1-4, Miscellaneous Output, CRTC 0-18h, attribute 0-13h, graphics
0-8), in the table's byte order.
"""

import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vga_ports  # noqa: E402


def table(seq, misc, crtc, attr, gc):
    return {
        "seq": bytes.fromhex(seq),
        "misc": misc,
        "crtc": bytes.fromhex(crtc),
        "attr": bytes.fromhex(attr),
        "gc": bytes.fromhex(gc),
    }


PARAM_03 = table(
    "00 03 00 02",
    0x67,
    "5f 4f 50 82 55 81 bf 1f 00 4f 0d 0e 00 00 00 00 9c 8e 8f 28 1f 96 b9 a3 ff",
    "00 01 02 03 04 05 14 07 38 39 3a 3b 3c 3d 3e 3f 0c 00 0f 08",
    "00 00 00 00 00 10 0e 00 ff",
)
PARAM_12 = table(
    "01 0f 00 06",
    0xE3,
    "5f 4f 50 82 54 80 0b 3e 00 40 00 00 00 00 00 00 ea 8c df 28 00 e7 04 e3 ff",
    "00 01 02 03 04 05 14 07 38 39 3a 3b 3c 3d 3e 3f 01 00 0f 00",
    "00 00 00 00 00 00 05 0f ff",
)
PARAM_13 = table(
    "01 0f 00 0e",
    0x63,
    "5f 4f 50 82 54 80 bf 1f 00 41 00 00 00 00 00 00 9c 8e 8f 28 40 96 b9 a3 ff",
    "00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 41 00 0f 00",
    "00 00 00 00 00 40 05 0f ff",
)


def line(direction, port, size, value, ip="e000000088160000:0"):
    """One line in the VMM's port_trace format (platform/devices.c)."""
    return f"[warn] port_trace: {direction} port={port:04x} size={size} value={value:x} ip={ip}"


def program(mode):
    """A BIOS-style mode set with byte writes: sequencer held in reset,
    Misc, CR11 unlocked before CR0-18h, attributes behind a flip-flop reset."""
    crtc = 0x3D4 if mode["misc"] & 1 else 0x3B4
    out = [line("out", 0x3C4, 1, 0), line("out", 0x3C5, 1, 1)]
    for index, value in enumerate(mode["seq"], 1):
        out += [line("out", 0x3C4, 1, index), line("out", 0x3C5, 1, value)]
    out += [line("out", 0x3C2, 1, mode["misc"]), line("out", 0x3C4, 1, 0), line("out", 0x3C5, 1, 3)]
    out += [line("out", crtc, 1, 0x11), line("out", crtc + 1, 1, mode["crtc"][0x11] & 0x7F)]
    for index, value in enumerate(mode["crtc"]):
        out += [line("out", crtc, 1, index), line("out", crtc + 1, 1, value)]
    out.append(line("in", crtc + 6, 1, 0))
    for index, value in enumerate(mode["attr"]):
        out += [line("out", 0x3C0, 1, index), line("out", 0x3C0, 1, value)]
    for index, value in enumerate(mode["gc"]):
        out += [line("out", 0x3CE, 1, index), line("out", 0x3CF, 1, value)]
    out += [line("in", crtc + 6, 1, 0), line("out", 0x3C0, 1, 0x20)]
    return out


def replay(lines, **kw):
    state = vga_ports.VgaState()
    vga_ports.replay(state, vga_ports.parse(lines), **kw)
    return state


def unlocked(*lines):
    return [line("out", 0x3D4, 2, 0x0E11)] + list(lines)


class Registers(unittest.TestCase):
    def test_word_out_is_index_then_data(self):
        s = replay([line("out", 0x3C4, 2, 0x0F02), line("out", 0x3CE, 2, 0xFF08)])
        self.assertEqual((s.get("SR02"), s.seq_index), (0x0F, 2))
        self.assertEqual((s.get("GR08"), s.gc_index), (0xFF, 8))

    def test_crtc_base_follows_misc_bit0(self):
        s = replay(
            [
                line("out", 0x3C2, 1, 0x67),
                line("out", 0x3D4, 2, 0x5513),
                line("out", 0x3B4, 2, 0x2213),
            ]
        )
        self.assertEqual(s.get("CR13"), 0x55)  # 3B4 is not decoded in colour addressing
        s = replay(
            [
                line("out", 0x3C2, 1, 0x66),
                line("out", 0x3B4, 2, 0x2213),
                line("out", 0x3D4, 2, 0x5513),
            ]
        )
        self.assertEqual(s.get("CR13"), 0x22)

    def test_attribute_flip_flop(self):
        s = replay(
            [
                line("in", 0x3DA, 1, 0),
                line("out", 0x3C0, 1, 0x10),
                line("out", 0x3C0, 1, 0x41),
                line("out", 0x3C0, 1, 0x32),
                line("out", 0x3C0, 1, 0x05),
            ]
        )
        self.assertEqual((s.get("AR10"), s.get("AR12")), (0x41, 0x05))
        self.assertTrue(s.attr_pas)  # 0x32: index 12h with bit 5 set
        s = replay(
            [
                line("out", 0x3C0, 1, 0x10),
                line("in", 0x3DA, 1, 0),
                line("out", 0x3C0, 1, 0x10),
                line("out", 0x3C0, 1, 0x01),
            ]
        )
        self.assertEqual(s.get("AR10"), 0x01)
        # Without the reset the third write is an index: AR10 holds 10h, index 1.
        s = replay(
            [line("out", 0x3C0, 1, 0x10), line("out", 0x3C0, 1, 0x10), line("out", 0x3C0, 1, 0x01)]
        )
        self.assertEqual((s.get("AR10"), s.attr_index), (0x10, 1))
        # A word read covering 3DA resets the flip-flop too.
        s = replay(
            [
                line("out", 0x3C0, 1, 0x10),
                line("in", 0x3D9, 2, 0),
                line("out", 0x3C0, 1, 0x13),
                line("out", 0x3C0, 1, 0x08),
            ]
        )
        self.assertEqual(s.get("AR13"), 0x08)

    def test_status_read_only_at_the_crtc_base(self):
        # Colour addressing: 3BA is not Input Status 1, so the data write follows.
        s = replay(
            [
                line("out", 0x3C2, 1, 0x67),
                line("out", 0x3C0, 1, 0x10),
                line("in", 0x3BA, 1, 0),
                line("out", 0x3C0, 1, 0x01),
            ]
        )
        self.assertEqual(s.get("AR10"), 0x01)
        s = replay(
            [
                line("out", 0x3C2, 1, 0x66),
                line("out", 0x3C0, 1, 0x10),
                line("in", 0x3DA, 1, 0),
                line("out", 0x3C0, 1, 0x01),
            ]
        )
        self.assertEqual(s.get("AR10"), 0x01)
        s = replay(
            [
                line("out", 0x3C2, 1, 0x66),
                line("out", 0x3C0, 1, 0x10),
                line("in", 0x3BA, 1, 0),
                line("out", 0x3C0, 1, 0x10),
                line("out", 0x3C0, 1, 0x02),
            ]
        )
        self.assertEqual(s.get("AR10"), 0x02)

    def test_crtc_protect(self):
        s = replay(
            unlocked(
                line("out", 0x3D4, 2, 0x8011),
                line("out", 0x3D4, 2, 0x5F01),
                line("out", 0x3D4, 2, 0xFF07),
                line("out", 0x3D4, 2, 0x2813),
            )
        )
        self.assertEqual((s.get("CR01"), s.get("CR07"), s.get("CR13")), (0, 0x10, 0x28))
        # A blocked write teaches nothing; through CR7 only bit 4 becomes known.
        self.assertEqual((s.show("CR01"), s.show("CR07"), s.show("CR13")), ("--", "~10", "28"))
        s = replay(unlocked(line("out", 0x3D4, 2, 0x5F01)))
        self.assertEqual(s.show("CR01"), "5f")

    def test_protection_is_assumed_at_start(self):
        s = replay([line("out", 0x3D4, 2, 0x5F01), line("out", 0x3D4, 2, 0x2813)])
        self.assertEqual((s.show("CR01"), s.show("CR11"), s.show("CR13")), ("--", "--", "28"))

    def test_denied_accesses_do_not_reach_the_card(self):
        s = replay(
            [
                line("out-denied", 0x3C4, 2, 0x0F02),
                line("in-denied", 0x3DA, 1, 0xFF),
                line("out", 0x3C0, 1, 0x10),
                line("out", 0x3C0, 1, 0x01),
            ]
        )
        self.assertEqual(s.show("SR02"), "--")
        self.assertEqual(s.get("AR10"), 0x01)
        self.assertEqual(s.denied, 2)


class Modes(unittest.TestCase):
    def test_mode_12h(self):
        s = replay(program(PARAM_12))
        d = s.decode()
        self.assertEqual((d["kind"], d["width"], d["height"]), ("graphics", 640, 480))
        self.assertEqual((d["memory"], d["addressing"]), ("A0000-AFFFF", "planar"))
        self.assertEqual(
            (d["bytes_per_line"], d["scanlines_per_row"], d["address_mode"]), (80, 1, "byte")
        )
        self.assertEqual(d["write_mode"], 0)
        self.assertFalse(d["screen_off"])
        self.assertEqual(vga_ports.differences(s, vga_ports.MODE_TABLES["12"]), [])

    def test_mode_03h(self):
        s = replay(program(PARAM_03))
        d = s.decode()
        self.assertEqual((d["kind"], d["width"], d["height"]), ("text", 720, 400))
        self.assertEqual((d["columns"], d["rows"], d["memory"]), (80, 25, "B8000-BFFFF"))
        self.assertEqual((d["addressing"], d["bytes_per_line"]), ("odd/even", 160))
        self.assertEqual(vga_ports.differences(s, vga_ports.MODE_TABLES["03"]), [])

    def test_mode_13h(self):
        # 256 colours: AR10 bit 6 halves the width, CR09 repeats each line, dword addressing.
        d = replay(program(PARAM_13)).decode()
        self.assertEqual(
            (d["kind"], d["width"], d["height"], d["scanlines"]), ("graphics", 320, 200, 400)
        )
        self.assertEqual(
            (d["addressing"], d["address_mode"], d["bytes_per_line"]), ("chain-4", "dword", 320)
        )

    def test_double_scan(self):
        s = replay(program(PARAM_12) + unlocked(line("out", 0x3D4, 2, 0xC009)))
        self.assertEqual((s.decode()["height"], s.decode()["double_scan"]), (240, True))

    def test_partial_mode_set_is_reported(self):
        s = replay(program(PARAM_03) + program(PARAM_12)[:40])
        diff = {
            name: (actual, value)
            for name, actual, value in vga_ports.differences(s, vga_ports.MODE_TABLES["12"])
        }
        self.assertEqual(diff["CR12"], (0x8F, 0xDF))
        self.assertEqual(s.decode()["kind"], "text")

    def test_unwritten_registers_are_unknown(self):
        s = replay([line("out", 0x3C4, 2, 0x0F02)])
        diff = {
            name: actual
            for name, actual, _ in vga_ports.differences(s, vga_ports.MODE_TABLES["12"])
        }
        self.assertIsNone(diff["CR12"])
        self.assertIsNone(diff["misc"])
        self.assertNotIn("SR02", diff)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            vga_ports.print_state("t", s, True)
        self.assertIn("SR: -- -- 0f -- --", out.getvalue())
        self.assertIn("misc: --", out.getvalue())


class Log(unittest.TestCase):
    def test_until_stops_at_first_match(self):
        lines = (
            program(PARAM_03)
            + ["[info] INT10 cpu0 in  AX=0012 BX=0000 CX=0000 DX=0000 F=0000 seq=1"]
            + program(PARAM_12)
        )
        s = replay(lines, until=r"INT10 cpu\d+ in  AX=0012")
        self.assertEqual(s.decode()["kind"], "text")
        self.assertEqual(replay(lines).decode()["kind"], "graphics")

    def test_until_matches_the_port_line(self):
        stop = line("out", 0x3CE, 2, 0x0506, ip="e0000000deadbeef:1")
        lines = program(PARAM_03) + [stop] + program(PARAM_12)
        s = replay(lines, until="deadbeef")
        self.assertEqual((s.last[-1], s.decode()["kind"]), (stop, "text"))
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "stop.log"
            log.write_text("\n".join(lines) + "\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                vga_ports.main([str(log), "--until", r"port=03ce size=2 value=506 "])
            self.assertIn("stopped before: " + stop, out.getvalue())

    def test_other_lines_and_prefixes_are_ignored(self):
        lines = [
            "[guest] [warn] port_trace: out port=03c4 size=2 value=f02 ip=0:0",
            "garbage",
            "x [warn] port_trace: out port=03ce size=2 value=ff08 ip=e0000000:1",
        ]
        s = replay(lines)
        self.assertEqual(s.show("SR02"), "--")
        self.assertEqual(s.get("GR08"), 0xFF)

    def test_reference_compares_unknown_registers(self):
        partial, full = replay([line("out", 0x3C4, 2, 0x0F02)]), replay(program(PARAM_12))
        diff = {name: (a, b) for name, a, b in vga_ports.compare(partial, full)}
        self.assertEqual(diff["CR12"], (None, 0xDF))
        self.assertEqual(diff["GR00"], (None, 0))  # unknown, not equal to a written 0
        self.assertNotIn("SR02", diff)
        self.assertEqual(vga_ports.compare(full, replay(program(PARAM_12))), [])

    def test_cli_diff_against_reference(self):
        with tempfile.TemporaryDirectory() as tmp:
            good, bad = Path(tmp) / "good.log", Path(tmp) / "bad.log"
            good.write_text("\n".join(program(PARAM_12)) + "\n")
            bad.write_text("\n".join(program(PARAM_03) + program(PARAM_12)[:40]) + "\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = vga_ports.main([str(bad), "--reference", str(good)])
            self.assertEqual(rc, 0)
            self.assertIn("kind: text", out.getvalue())
            self.assertIn("CR12: 8f df", out.getvalue())

    def test_cli_tail_lists_the_last_accesses(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "stall.log"
            log.write_text("\n".join(program(PARAM_03) + [line("in", 0x3DA, 1, 0x09)] * 3) + "\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                vga_ports.main([str(log), "--tail", "4"])
            listed = out.getvalue().split("last 4 accesses:\n")[1].splitlines()
            self.assertEqual(
                listed, ["  " + line("out", 0x3C0, 1, 0x20)] + ["  " + line("in", 0x3DA, 1, 9)] * 3
            )


if __name__ == "__main__":
    unittest.main()
