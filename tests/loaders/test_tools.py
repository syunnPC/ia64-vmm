#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reject blank/diagnostic screenshots and incomplete keyboard/boot evidence."""

from pathlib import Path
import struct
import os
import shlex
import subprocess
import tempfile
import unittest
import zlib

from run import (
    accepted,
    classify,
    consumed_keys,
    observe_milestones,
    stop_state,
    tier1_statistics,
)
from screen import firmware_font, menu_matches
from profile import summarize

FONT = Path(__file__).resolve().parents[2] / "firmware/include/ui/vgafont.c"


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def text_png(path, lines, scale=1, rgba=False, size=None):
    font = firmware_font(FONT)
    channels = 4 if rgba else 3
    background = bytes([0, 0, 128] + ([255] if rgba else []))
    foreground = bytes([255] * channels)
    width, height = size or (640 * scale, 400 * scale)
    rows = []
    for y in range(height):
        cell_y = ((y + 1) * 25 - 1) // height
        y0, y1 = cell_y * height // 25, (cell_y + 1) * height // 25
        glyph_y = (y - y0) * 16 // (y1 - y0)
        line = lines[cell_y] if cell_y < len(lines) else ""
        row = bytearray()
        for x in range(width):
            cell_x = ((x + 1) * 80 - 1) // width
            x0, x1 = cell_x * width // 80, (cell_x + 1) * width // 80
            char = ord(line[cell_x]) if cell_x < len(line) else 32
            glyph_x = (x - x0) * 8 // (x1 - x0)
            row.extend(foreground if font[char * 16 + glyph_y] & (128 >> glyph_x) else background)
        rows.append(b"\0" + row)
    header = struct.pack(">IIBBBBB", width, height, 8, 6 if rgba else 2, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(b"".join(rows)))
        + png_chunk(b"IEND", b"")
    )


class LoaderToolsTests(unittest.TestCase):
    def test_profile_units_and_nested_read_accounting(self):
        text = (
            "loader sample ip=1000 slot=0 pa=2000 mapped=1 retired=10 counter=100 hz=100\n"
            "loader sample ip=1000 slot=0 pa=2000 mapped=1 retired=210 counter=300 hz=100\n"
        )
        fields = dict(
            hz=100,
            itc=100,
            reads=1,
            bytes=2048,
            failed=0,
            read_ticks=20,
            min=2048,
            max=2048,
            ahci_commands=1,
            ahci_ticks=10,
            tfd_ticks=1,
            ci_ticks=4,
            tfd_polls=0,
            ci_polls=2,
        )

        def line():
            return (
                "storage profile "
                + " ".join(f"{k}={v:x}" for k, v in fields.items())
                + " sizes=0,1,0,0,0,0,0\n"
            )

        text += line()
        fields.update(
            itc=300, reads=3, bytes=6144, read_ticks=60, ahci_ticks=30, tfd_ticks=3, ci_ticks=12
        )
        text += line()
        result = summarize(text, [(0x2000, 16, "blk_read")])
        self.assertEqual(result["histogram"], [("blk_read", 2)])
        self.assertEqual(result["retired_per_second"], 100)
        storage = result["storage"]
        self.assertEqual(storage["bytes_per_second"], 2048)
        self.assertEqual(storage["reads_per_second"], 1)
        self.assertEqual(storage["read_seconds"], 0.6)
        self.assertEqual(storage["poll_seconds"], 0.15)

    def test_milestones_are_first_observations(self):
        milestones = {}
        observe_milestones("EFI ExitBootServices succeeded", 3.5, milestones)
        observe_milestones(
            "EFI ExitBootServices succeeded\nEFI loader done\nLinux version 6.1", 5, milestones
        )
        self.assertEqual(milestones, dict(ebs=3.5, kernel_entry=5, linux_banner=5))
        observe_milestones(
            "console [ttyS0] enabled\nmanagement watchdog stopped guest", 7, milestones
        )
        self.assertEqual(milestones["serial_console"], 7)
        self.assertEqual(milestones["stop"], 7)

    def test_unclaimed_physical_bus_policy(self):
        root = FONT.parents[3]
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / "physical")
            command = shlex.split(os.environ.get("HOSTCC", "cc")) + [
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Iinclude",
                "-I.",
                "tests/loaders/physical.c",
                "platform/physical.c",
                "platform/uart.c",
                "ia64/memory.c",
                "core/guest_mem.c",
                "-o",
                binary,
            ]
            subprocess.run(command, cwd=root, check=True)
            subprocess.run([binary], cwd=root, check=True)

    def test_menu_pixels_at_both_gop_sizes_and_colors(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "menu.png"
            for scale, rgba in [(1, False), (2, True)]:
                text_png(
                    path,
                    ["GNU GRUB  version 2.12", "Boot LiveCD", "Special console options"],
                    scale,
                    rgba,
                )
                match, _ = menu_matches(path, ["GNU GRUB version 2.12", "Boot LiveCD"], FONT)
                self.assertTrue(match)
            text_png(path, ["GNU GRUB  version 2.12", "Boot LiveCD"], size=(1024, 768))
            self.assertTrue(menu_matches(path, ["GNU GRUB version 2.12", "Boot LiveCD"], FONT)[0])
            for lines in [[], ["VMM halted exit_code=5"]]:
                text_png(path, lines)
                self.assertFalse(menu_matches(path, ["GNU GRUB", "Boot LiveCD"], FONT)[0])
            path.write_bytes(b"\x89PNG\r\n\x1a\n")
            self.assertFalse(menu_matches(path, ["GNU GRUB"], FONT)[0])

    def test_input_requires_consumed_order_not_just_injection(self):
        keys = ["down", "up", "ret"]
        text = "EFI input key scan=0002 unicode=0000\nEFI input key scan=0001 unicode=0000\n"
        self.assertFalse(consumed_keys(text, keys)[0])
        self.assertTrue(consumed_keys(text + "EFI input key scan=0000 unicode=000d", keys)[0])
        result = dict(
            menu=True,
            ebs=True,
            loader_done=True,
            screenshot_reviewed=True,
            scenario="scripted",
            keys_sent=keys,
        )
        self.assertFalse(accepted(result))
        result["input_consumed"] = True
        self.assertTrue(accepted(result))

    def test_stop_classification_preserves_firmware_fatal(self):
        text = "firmware debug 0000000000001000\nfirmware panic 0000000000001000\n"
        text += "guest exit reason=1 ip=110080 slot=2 info=0\nvcpu0 IP=110080"
        result = classify(text, "", 1)
        self.assertEqual(result["classification"], "firmware-exception")
        self.assertEqual(result["ip"], "110080")
        result = classify(
            "guest exit reason=3 ip=1234 slot=1 info=112 PAL stacked return: RSE not built", "", 2
        )
        self.assertEqual(result["classification"], "cpu-unimplemented")
        result = classify("", "QEMU timeout after 155s; serial log: example.log", 5)
        self.assertEqual(result["classification"], "timeout")
        self.assertEqual(
            classify("management watchdog stopped guest at ip=1234", "", 5)["classification"],
            "timeout",
        )
        state = stop_state(
            "[info] instruction BR_RET raw=00108001100\n"
            "[info] tier0 interpreter reason=single-instruction instructions=0\n"
        )
        self.assertEqual(state["instruction"], "instruction BR_RET raw=00108001100")

    def test_restarted_run_is_classified_by_its_last_boot(self):
        # A reset request of an earlier boot restarted the
        # guest; it neither ends the run nor hides how the last boot ended.
        first = "[info] EFI ResetSystem type=1 status=0 ip=1234 slot=0\n[info] guest restart 1 source=efi type=warm status=0\n"
        self.assertEqual(
            classify(first + "management watchdog stopped guest at ip=1234", "", 5)[
                "classification"
            ],
            "timeout",
        )
        result = classify(first + "guest exit reason=3 ip=4321 slot=0 info=0\nvcpu0 IP=4321", "", 2)
        self.assertEqual((result["classification"], result["ip"]), ("cpu-unimplemented", "4321"))
        self.assertEqual(
            classify(first + "[info] EFI ResetSystem type=2 status=0 ip=1 slot=0", "", 0)[
                "classification"
            ],
            "firmware-return",
        )
        self.assertEqual(classify(first, "runner status 9", 9)["classification"], "runner")

    def test_host_failure_overrides_guest_timeout_for_every_engine(self):
        for engine in ["interp", "tier0", "tier1"]:
            for host in ["panic: host window flush timed out", "host exc: vector=14 #PF"]:
                serial = "management watchdog stopped guest\nguest timeout\n" + host
                self.assertEqual(
                    classify(serial, "QEMU timeout after 155s", 5)["classification"], "host-failure"
                )
                result = dict(
                    engine=engine,
                    menu=True,
                    ebs=True,
                    loader_done=True,
                    screenshot_reviewed=True,
                    scenario="auto",
                    tier1_ran=1,
                    hostwin_fills=1,
                    backend_failures=0,
                    **classify(serial, "", 5),
                )
                self.assertFalse(accepted(result))
                self.assertEqual(
                    classify("guest timeout", host, 5)["classification"], "host-failure"
                )

    def test_tier1_requires_product_statistics(self):
        result = dict(
            engine="tier1",
            menu=True,
            ebs=True,
            loader_done=True,
            screenshot_reviewed=True,
            scenario="auto",
        )
        self.assertFalse(accepted(result))
        serial = "[info] tier1 linked=2 ran=123\n[info] hostwin instances=1 fills=45 backend_failures=0\n"
        result.update(tier1_statistics(serial))
        self.assertTrue(accepted(result))
        result.update(
            tier1_statistics(serial + "hostwin instances=1 fills=45 backend_failures=1\n")
        )
        self.assertFalse(accepted(result))


if __name__ == "__main__":
    unittest.main()
