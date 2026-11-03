#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("usb_builder", Path(__file__).with_name("build.py"))
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
from qemu import mkesp

USER_BOOT = "Boot from guest media (using user config)"
SAFE_BOOT = "Boot from guest media (safe mode - default settings)"
DIAGNOSTIC_BASE = [
    "exit=halt",
    "vmm_media=auto",
    "engine=interp",
    "reserve_mib=128",
    "placement=logical-thread",
    "pause_sec=5",
]


def entry(text, title, depth=1):
    lines = text.split(f"{'/' * depth}{title}\n", 1)[1].splitlines()
    end = next((i for i, line in enumerate(lines) if not line or line.startswith("/")), len(lines))
    return "\n".join(lines[:end])


def cmdline(block):
    return next(l.split("cmdline: ", 1)[1] for l in block.splitlines() if "cmdline:" in l)


class UsbTests(unittest.TestCase):
    def test_fixed_menu_hierarchy(self):
        text = builder.entries({})
        # Limine v11.x CONFIG.md: each additional slash is one submenu level.
        self.assertTrue(text.startswith("timeout: 10\ndefault_entry: 2\n"))
        titles = [line for line in text.splitlines() if line.startswith("/")]
        self.assertEqual(
            [title for title in titles if not title.startswith("//")],
            ["/Configure VMM Options", "/" + USER_BOOT, "/" + SAFE_BOOT, "/Diagnostics"],
        )
        self.assertEqual(
            [title for title in titles if title.startswith("//") and not title.startswith("///")],
            [
                "//Discovery",
                "//Basic Tests",
                "//Memory",
                "//Timers",
                "//SIMD and Native x86 State",
                "//Host Memory Mapping",
                "//Guest Firmware",
                "//Persistent Storage",
                "//Fault Handling (expected halt)",
            ],
        )
        self.assertEqual(entry(text, "Diagnostics"), "")
        self.assertEqual(entry(text, "Fault Handling (expected halt)", 2), "")
        self.assertEqual(
            entry(text, "Configure VMM Options").splitlines(),
            ["    protocol: efi", "    path: boot():/EFI/vmm/vmmcfg.efi"],
        )

    def test_guest_boot_defaults_and_options_isolation(self):
        text = builder.entries({})
        for title, module in [(USER_BOOT, "options"), (SAFE_BOOT, "options-media")]:
            block = entry(text, title)
            self.assertEqual(cmdline(block), "exit=halt bootmenu=1 vmm_media=auto")
            self.assertIn("    resolution: 1024x768", block)
            self.assertIn(
                "    module_path: boot():/firmware-new.elf\n    module_string: firmware", block
            )
            self.assertIn(
                f"    module_path: boot():/vmm-options.conf\n    module_string: {module}\n",
                block + "\n",
            )
            self.assertNotIn("bootdisk", block)
        self.assertNotIn("module_string: options\n", entry(text, SAFE_BOOT) + "\n")
        discovery = entry(text, "Discovery", 2)
        self.assertEqual(
            cmdline(discovery).split(),
            [token.replace("vmm_media=auto", "vmm_media=list") for token in DIAGNOSTIC_BASE],
        )
        self.assertNotIn("vmm-options.conf", discovery)
        self.assertNotIn("module_string: media-id", discovery)
        for title in [
            "Basic Tests",
            "Memory",
            "Timers",
            "SIMD and Native x86 State",
            "Host Memory Mapping",
            "Guest Firmware",
        ]:
            block = entry(text, title, 2)
            self.assertEqual(cmdline(block).split()[: len(DIAGNOSTIC_BASE)], DIAGNOSTIC_BASE)
            self.assertIn("module_string: options-media", block)
            self.assertNotIn("module_string: options\n", block + "\n")
            self.assertLessEqual(len(cmdline(block).encode()), builder.ENTRY_CMDLINE_MAX)

    def test_basic_and_individual_diagnostics(self):
        text = builder.entries({})
        basic = entry(text, "Basic Tests", 2)
        self.assertEqual(
            cmdline(basic).split()[len(DIAGNOSTIC_BASE) :],
            ["selftest=host", "selftest=timer", "selftest=simd", "selftest=hostwin", "vcpus=2"],
        )
        self.assertNotIn("bootdisk", basic)  # hostwin ends a successful run before guest execution.
        for title, extra, hello in [
            ("Memory", ["selftest=host"], True),
            ("Timers", ["selftest=timer"], True),
            ("SIMD and Native x86 State", ["selftest=simd"], True),
            ("Host Memory Mapping", ["selftest=hostwin", "vcpus=2"], False),
            ("Guest Firmware", [], True),
        ]:
            with self.subTest(title=title):
                block = entry(text, title, 2)
                self.assertEqual(cmdline(block).split()[len(DIAGNOSTIC_BASE) :], extra)
                self.assertEqual("module_string: bootdisk" in block, hello)
                self.assertIn(
                    "module_path: boot():/firmware-new.elf\n    module_string: firmware", block
                )

    def test_expected_faults_are_separate(self):
        text = builder.entries({})
        faults = [
            ("Page Fault", ["selftest=pf"]),
            ("Invalid Opcode", ["selftest=ud"]),
            ("General Protection", ["selftest=gp"]),
            ("AP Exception", ["selftest=ap-ud"]),
            ("Host-window Fatal", ["selftest=hostwin-fatal", "vcpus=2"]),
            (
                "Host-window Outside Fatal",
                ["selftest=hostwin-fatal", "vcpus=2", "hwb_fatal_outside=1"],
            ),
        ]
        self.assertEqual(
            [line for line in text.splitlines() if line.startswith("///")],
            ["///" + title for title, _ in faults],
        )
        for title, extra in faults:
            block = entry(text, title, 3)
            self.assertEqual(cmdline(block).split(), DIAGNOSTIC_BASE + extra)
            self.assertIn("module_string: options-media", block)
            self.assertNotIn("bootdisk", block)

    def test_optional_persistence_modules(self):
        plain = builder.entries({}, nvram=False)
        self.assertNotIn("Persistent Storage", plain)
        for module in ["persistence", "log-a", "log-b", "nvram"]:
            self.assertNotIn("module_string: " + module, plain)
        text = builder.entries({}, nvram=True)
        user = entry(text, USER_BOOT)
        safe = entry(text, SAFE_BOOT)
        storage = entry(text, "Persistent Storage", 2)
        self.assertEqual(cmdline(storage).split(), DIAGNOSTIC_BASE + ["media_log=1"])
        self.assertIn("module_string: bootdisk", storage)
        for block in [user, safe]:
            self.assertIn("module_path: boot():/nvram-new.bin\n    module_string: nvram", block)
        for block in [user, storage]:
            for filename, module in [
                ("persistence-manifest.bin", "persistence"),
                ("log-a.bin", "log-a"),
                ("log-b.bin", "log-b"),
            ]:
                self.assertIn(
                    f"module_path: boot():/{filename}\n    module_string: {module}", block
                )
        self.assertIn("module_string: options-media", storage)
        self.assertNotIn("module_string: nvram", storage)
        self.assertNotIn("module_string: persistence", safe)
        for block in text.split("\n\n"):
            if block.startswith("//") and not block.startswith("//Persistent Storage\n"):
                for module in ["persistence", "log-a", "log-b", "nvram"]:
                    self.assertNotIn("module_string: " + module, block)

    def test_automatic_media_identity(self):
        for nvram in (False, True):
            for block in builder.entries({}, nvram=nvram).split("\n\n"):
                if "protocol: limine" not in block or block.startswith("//Discovery\n"):
                    continue
                # Identification is independent of logging and variable persistence.
                self.assertIn("module_path: boot():/persistence-manifest.bin", block)
                tags = [line.strip() for line in block.splitlines() if "module_string:" in line]
                self.assertEqual(
                    sum(
                        tag in ("module_string: media-id", "module_string: persistence")
                        for tag in tags
                    ),
                    1,
                )

    def test_removed_and_unknown_settings_rejected(self):
        for key in [
            "ENTRY_1_TITLE",
            "ENTRY_1_CMDLINE",
            "DEBUG_SERIAL",
            "DEBUG_SCREEN",
            "MACHINE_MEDIA",
        ]:
            with (
                self.subTest(key=key),
                self.assertRaisesRegex(RuntimeError, "unknown setting " + key),
            ):
                builder.entries({key: "unused"})
        builder.entries({"VMM_OPTIONS_FILE": "seed.conf", "NVRAM_PAIRS": "8"})

    def test_example_configuration_builds(self):
        example = Path(__file__).with_name("local.env.example")
        settings = builder.parse_config(example)
        self.assertLessEqual(set(settings), {"VMM_OPTIONS_FILE", "NVRAM_PAIRS"})
        self.assertIn("//Persistent Storage", builder.entries(settings, nvram=True))

    def test_options_file(self):
        default = builder.options_file({}).decode()
        self.assertEqual(builder.options_tokens(default), [])  # comments only, no tokens
        self.assertIn("Configure VMM Options", default)
        with tempfile.TemporaryDirectory(dir=builder.ROOT / "build") as temp:
            seed = Path(temp) / "seed.conf"
            seed.write_bytes(b"# my seed\r\nengine=interp pause_sec=5\r\nbootmenu=1\n")
            self.assertEqual(
                builder.options_file({"VMM_OPTIONS_FILE": str(seed)}), seed.read_bytes()
            )
            self.assertEqual(
                builder.options_tokens(seed.read_text()),
                ["engine=interp", "pause_sec=5", "bootmenu=1"],
            )
            seed.write_bytes(b"# ok\n" + b"k=" + b"v" * 520 + b"\n")  # over the 511-byte budget
            with self.assertRaises(RuntimeError):
                builder.options_file({"VMM_OPTIONS_FILE": str(seed)})
            seed.write_bytes(b"engine=interp\n" + "café=1\n".encode())  # non-ASCII
            with self.assertRaises(RuntimeError):
                builder.options_file({"VMM_OPTIONS_FILE": str(seed)})
            seed.write_bytes(b"x" * 511)  # exactly the budget
            builder.options_file({"VMM_OPTIONS_FILE": str(seed)})
        with self.assertRaises(RuntimeError):
            builder.options_file({"VMM_OPTIONS_FILE": "/nonexistent/seed.conf"})

    def test_mbr_fat_payload(self):
        with tempfile.TemporaryDirectory(dir=builder.ROOT / "build") as temp:
            path = Path(temp)
            tree = path / "tree"
            tree.mkdir()
            (tree / "hello.efi").write_bytes(b"hello-test")
            dest = path / "usb.img"
            builder.disk_image(tree, dest)
            with dest.open("rb") as image:
                mbr = image.read(512)
                self.assertEqual(mbr[510:], b"\x55\xaa")
                self.assertEqual(mbr[450], 0xEF)
                first, count = struct.unpack_from("<II", mbr, 454)
                self.assertEqual(first, 2048)
                self.assertGreaterEqual(count, 64 * 2048)
                self.assertEqual(dest.stat().st_size, (first + count) * 512)
                self.assertEqual(mbr[462:510], bytes(48))
                self.assertNotEqual(image.read(8), b"EFI PART")
                image.seek(first * 512)
                self.assertEqual(image.read(512)[82:90], b"FAT32   ")
            got = subprocess.run(
                ["mtype", "-i", f"{dest}@@1048576", "::/hello.efi"], check=True, capture_output=True
            ).stdout
            self.assertEqual(got, b"hello-test")

    def build_tree(self, settings, image=True):
        """Run main() on a fake repository; returns (tree, image path, ROOT)."""
        temp = tempfile.TemporaryDirectory(dir=builder.ROOT / "build")
        self.addCleanup(temp.cleanup)
        path = Path(temp.name)
        root = path / "repo"
        tree = path / "tree"
        dest = path / "usb.img"
        notices = self.notices()
        for name in notices:
            source = root / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes((builder.ROOT / name).read_bytes())
        for name in [
            "third_party/limine/BOOTX64.EFI",
            "build/bootmedia/bootmedia.efi",
            "build/vmm.elf",
            "build/efi/hello.efi",
            "build/vmmcfg/vmmcfg.efi",
            "build/firmware-new/release/ia64-firmware-new.elf",
        ]:
            source = root / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(name.encode())
        config = path / "stick.conf"
        config.write_text("".join(f"{key}={value}\n" for key, value in settings.items()))
        env = {"USB_STICK_CONFIG": str(config), "USB_STICK_DIR": str(tree)}
        if image:
            env["USB_STICK_IMAGE"] = str(dest)
        tree.mkdir()
        (tree / "firmware.elf").write_bytes(b"obsolete reference firmware")
        with (
            mock.patch.object(builder, "ROOT", root),
            mock.patch.object(builder, "load_local_env"),
            mock.patch.dict(os.environ, env, clear=True),
        ):
            builder.main()
        return tree, dest, root

    def notices(self):
        names = [
            "NOTICE",
            "third_party/limine/LICENSE",
            "third_party/limine/LICENSE.protocol",
            "docs/limine-dependencies.md",
            "third_party/limine/NOTICE.dependencies",
            "third_party/gnu-efi/LICENSE.sources",
            "third_party/gnu-efi/LICENSE.efilib",
            "third_party/gnu-efi/LICENSE.BSD-3-Clause",
            "third_party/edk2-ipf/License.txt",
        ]
        return names + [
            str(p.relative_to(builder.ROOT))
            for directory in ["LICENSES", "third_party/gnu-efi/licenses"]
            for p in (builder.ROOT / directory).rglob("*")
            if p.is_file()
        ]

    def test_distribution_notices_in_tree_and_image(self):
        # NOTICE requires the full terms and the bootloader's embedded notices.
        tree, dest, _ = self.build_tree({})
        for name in self.notices():
            with self.subTest(name=name):
                data = (builder.ROOT / name).read_bytes()
                self.assertEqual((tree / name).read_bytes(), data)
                got = subprocess.run(
                    ["mtype", "-i", f"{dest}@@1048576", f"::/{name}"],
                    check=True,
                    capture_output=True,
                ).stdout
                self.assertEqual(got, data)

    def test_tree_firmware_copies(self):
        # USB builds require only the native firmware.
        tree, _, _ = self.build_tree({}, image=False)
        self.assertEqual(
            (tree / "firmware-new.elf").read_bytes(),
            b"build/firmware-new/release/ia64-firmware-new.elf",
        )
        self.assertFalse((tree / "firmware-new-debug.elf").exists())
        self.assertFalse((tree / "firmware.elf").exists())
        self.assertIn("/" + USER_BOOT + "\n", (tree / "limine.conf").read_text())
        self.assertIn("//Persistent Storage", (tree / "limine.conf").read_text())

    def test_boot_shim_and_limine_in_tree_and_image(self):
        tree, dest, _ = self.build_tree({})
        for source, name in [
            ("build/bootmedia/bootmedia.efi", "EFI/BOOT/BOOTX64.EFI"),
            ("third_party/limine/BOOTX64.EFI", "EFI/vmm/limine.efi"),
        ]:
            with self.subTest(name=name):
                self.assertEqual((tree / name).read_bytes(), source.encode())
                got = subprocess.run(
                    ["mtype", "-i", f"{dest}@@1048576", f"::/{name}"],
                    check=True,
                    capture_output=True,
                ).stdout
                self.assertEqual(got, source.encode())

    def test_qemu_esp_contains_boot_shim_and_limine(self):
        _, _, root = self.build_tree({}, image=False)
        with (
            mock.patch.object(mkesp, "ROOT", root),
            mock.patch.object(mkesp, "load_local_env"),
            mock.patch.dict(os.environ, {}, clear=True),
        ):
            mkesp.main()
        for source, name in [
            ("build/bootmedia/bootmedia.efi", "EFI/BOOT/BOOTX64.EFI"),
            ("third_party/limine/BOOTX64.EFI", "EFI/vmm/limine.efi"),
        ]:
            got = subprocess.run(
                ["mtype", "-i", str(root / "build/esp.img"), f"::/{name}"],
                check=True,
                capture_output=True,
            ).stdout
            self.assertEqual(got, source.encode())

    def test_hyperv_esp_contains_boot_shim_and_limine(self):
        _, _, root = self.build_tree({}, image=False)
        spec = importlib.util.spec_from_file_location(
            "hyperv_builder", builder.ROOT / "tools/hyperv/build.py"
        )
        hyperv = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(hyperv)

        def check_tree(tree, disk):
            self.assertEqual(
                (tree / "EFI/BOOT/BOOTX64.EFI").read_bytes(), b"build/bootmedia/bootmedia.efi"
            )
            self.assertEqual(
                (tree / "EFI/vmm/limine.efi").read_bytes(), b"third_party/limine/BOOTX64.EFI"
            )
            self.assertIn("vmm_media=auto", (tree / "limine.conf").read_text())

        with (
            mock.patch.object(hyperv, "ROOT", root),
            mock.patch.object(hyperv.usb, "copy_notices"),
            mock.patch.object(hyperv.usb, "disk_image", side_effect=check_tree) as disk_image,
            mock.patch.object(hyperv, "convert"),
            mock.patch.object(hyperv.subprocess, "run"),
        ):
            hyperv.build(root / "output")
        disk_image.assert_called_once()

    def test_ordinary_image_menu(self):
        tree, dest, _ = self.build_tree({"NVRAM_PAIRS": "1"})
        expected = builder.entries({"NVRAM_PAIRS": "1"}, nvram=True)
        self.assertEqual((tree / "limine.conf").read_text(), expected)
        got = subprocess.run(
            ["mtype", "-i", f"{dest}@@1048576", "::/limine.conf"], check=True, capture_output=True
        ).stdout
        self.assertEqual(got.decode(), expected)
        for filename in ["nvram-new.bin", "persistence-manifest.bin"]:
            got = subprocess.run(
                ["mtype", "-i", f"{dest}@@1048576", "::/" + filename],
                check=True,
                capture_output=True,
            ).stdout
            self.assertEqual(got, (tree / filename).read_bytes())
            self.assertTrue(any(got))

    def test_config_file_syntax(self):
        with tempfile.TemporaryDirectory(dir=builder.ROOT / "build") as temp:
            config = Path(temp) / "stick.conf"
            config.write_text("# comment\n\nNVRAM_PAIRS=8\nVMM_OPTIONS_FILE=seed.conf\n")
            self.assertEqual(
                builder.parse_config(config), {"NVRAM_PAIRS": "8", "VMM_OPTIONS_FILE": "seed.conf"}
            )
            for bad in ["NVRAM_PAIRS\n", "nvram_pairs=8\n", "NVRAM_PAIRS=8\nNVRAM_PAIRS=2\n"]:
                config.write_text(bad)
                with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                    builder.parse_config(config)


if __name__ == "__main__":
    unittest.main()
