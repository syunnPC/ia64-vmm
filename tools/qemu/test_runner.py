#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regression checks for configuration precedence, fallback and watchdog cleanup."""

from contextlib import nullcontext
import argparse
import json
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qemu import config, run, windows
from vmware import backend


class RunnerTests(unittest.TestCase):
    def test_control_file_ignores_existing_lines_and_partial_appends(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "ctl"
            path.write_bytes(b"hmp quit\n")
            ctl = run.ControlFile(path)
            self.assertIsNone(ctl.peek())
            with path.open("ab") as f:
                f.write(b"key ret\nshot a.png\nkey sp")
            self.assertEqual(ctl.peek(), "key ret")
            self.assertEqual(ctl.peek(), "key ret")  # not consumed until asked
            ctl.consume()
            self.assertEqual(ctl.peek(), "shot a.png")
            ctl.consume()
            self.assertIsNone(ctl.peek())
            with path.open("ab") as f:
                f.write(b"c\n")
            self.assertEqual(ctl.peek(), "key spc")

    def test_control_file_restarts_after_truncation_or_replacement(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "ctl"
            path.write_bytes(b"")
            ctl = run.ControlFile(path)
            with path.open("ab") as f:
                f.write(b"key ret\n")
            ctl.peek()
            ctl.consume()
            path.write_bytes(b"key a\n")  # truncated, shorter than the offset
            self.assertEqual(ctl.peek(), "key a")
            ctl.consume()
            path.unlink()
            path.write_bytes(b"key b\nkey c\n")  # replaced
            self.assertEqual(ctl.peek(), "key b")
        self.assertIsNone(run.ControlFile(Path(tmp) / "missing").peek())

    def test_control_line_parsing(self):
        self.assertEqual(run.parse_control_line("key ret"), ("key", "ret"))
        self.assertEqual(run.parse_control_line("hmp info status"), ("hmp", "info status"))
        self.assertEqual(run.parse_control_line("shot x/y.png"), ("shot", "x/y.png"))
        for bad in ["key", "key a b", "key ;", "hmp", "shot x.ppm", "quit", ""]:
            self.assertIsNone(run.parse_control_line(bad), bad)

    def test_guest_serial_transport_preserves_bytes_and_reports_loss(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "serial.log"
            raw = b"A\r\n\0\xff\x1b[m"
            log.write_bytes(
                b"[guest] unrelated text\r\n[vmm-serial] 0000000000000000 0008 "
                + raw.hex().encode()
                + b"\r\n[vmm-serial-end] 0000000000000008\n"
            )
            dest = run.guest_serial_file(log)
            self.assertEqual(dest.read_bytes(), raw)
            meta = json.loads(dest.with_suffix(".log.json").read_text())
            self.assertTrue(meta["complete"])
            log.write_bytes(b"[vmm-serial-lost] 3\n[vmm-serial] 0000000000000003 0001 00\n")
            dest = run.guest_serial_file(log)
            meta = json.loads(dest.with_suffix(".log.json").read_text())
            self.assertEqual(meta["lost_bytes"], 3)
            self.assertFalse(meta["complete"])
            log.write_bytes(b"[vmm-serial] 0000000000000000 0002 00\n")
            with self.assertRaises(RuntimeError):
                run.guest_serial_file(log)
            log.write_bytes(b"[vmm-serial] 0000000000000000 0002 001")
            with self.assertRaises(RuntimeError):
                run.guest_serial_file(log)

    def setUp(self):
        (config.ROOT / "build").mkdir(exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(dir=config.ROOT / "build")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "build").mkdir()
        self.run_dir = self.root / "run"
        self.run_dir.mkdir()
        for name in ["code", "vars"]:
            (self.root / name).write_bytes(name.encode())
        (self.root / "build/esp.img").write_bytes(b"original disk")
        self.log = self.root / "serial.log"
        self.args = SimpleNamespace(
            backend="win-qemu", keep=False, timeout=10, screenshot=None, gdb=False, cpus=4
        )
        settings = {
            "WIN_QEMU": str(self.root / "qemu.exe"),
            "WIN_QEMU_FW_CODE": str(self.root / "code"),
            "WIN_QEMU_FW_VARS": str(self.root / "vars"),
            "WIN_STAGING_DIR": str(self.run_dir),
        }
        environment = patch.dict(os.environ, settings, clear=True)
        environment.start()
        self.addCleanup(environment.stop)

    def test_pc_media_uses_assigned_ahci_and_default_q35_stays_unchanged(self):
        media = self.root / "test.iso"
        media.write_bytes(b"private optical medium")
        args = SimpleNamespace(machine="pc", cdrom=media, disk=None)
        devices = run.storage_arguments(args, self.run_dir, False)
        self.assertIn("ich9-ahci,id=guest-ahci,addr=04.0", devices)
        self.assertIn("ide-cd,drive=guest-cdrom,bus=guest-ahci.0", devices)
        args.machine = "q35"
        devices = run.storage_arguments(args, self.run_dir, False)
        self.assertFalse(any("id=guest-ahci" in d for d in devices))
        self.assertIn("ide-cd,drive=guest-cdrom,bus=ide.0", devices)

    def test_ide_install_disk_uses_separate_channel_and_private_copy(self):
        media = self.root / "test.iso"
        media.write_bytes(b"optical medium")
        disk = self.root / "target.img"
        disk.write_bytes(bytes(512))
        for machine, bus in [("pc", "ide"), ("q35", "guest-ide")]:
            args = SimpleNamespace(
                machine=machine,
                cdrom=media,
                disk=disk,
                cdrom_controller="ide",
                disk_controller="ide",
            )
            devices = run.storage_arguments(args, self.run_dir, False)
            self.assertIn(f"ide-cd,drive=guest-cdrom,bus={bus}.0,unit=0", devices)
            self.assertIn(f"ide-hd,drive=guest-disk,bus={bus}.1,unit=0", devices)
            self.assertEqual((self.run_dir / "guest.img").read_bytes(), disk.read_bytes())
            (self.run_dir / "guest.img").write_bytes(b"changed")
            self.assertEqual(disk.read_bytes(), bytes(512))

    def test_r0_switch_and_iommu_config(self):
        args = SimpleNamespace(pcie_switch=True, disk=None, cdrom=None)
        devices = run.storage_arguments(args, self.run_dir, False)
        self.assertIn("ich9-ahci,id=r0-empty,bus=r0-empty-port", devices)
        self.assertIn("ich9-ahci,id=r0-ahci,bus=r0-media-port", devices)
        self.assertIn("x3130-upstream,id=r0-switch,bus=r0-root", devices)
        vmx = backend.create_vm(self.root, self.run_dir, False, pcie_switch=True, iommu=True)
        settings = vmx.read_text()
        self.assertIn('sata0.pciSlotNumber = "37"', settings)
        self.assertIn('sata1.pciSlotNumber = "38"', settings)
        self.assertIn('vvtd.enable = "TRUE"', settings)

    def test_local_env_preserves_empty_and_nonempty_environment(self):
        (self.root / "local.env").write_text(
            "# comment\nWIN_QEMU=C:\\Program Files\\qemu\\qemu.exe\n"
            "OVMF_CODE=file-template\nOVMF_VARS=file-vars\n"
        )
        with (
            patch.object(config, "ROOT", self.root),
            patch.dict(os.environ, {"OVMF_CODE": "environment", "OVMF_VARS": ""}, clear=True),
        ):
            config.load_local_env()
            self.assertEqual(os.environ["WIN_QEMU"], r"C:\Program Files\qemu\qemu.exe")
            self.assertEqual(os.environ["OVMF_CODE"], "environment")
            with self.assertRaisesRegex(RuntimeError, "OVMF_VARS is unset"):
                config.required("OVMF_VARS")

    def test_make_environment_and_command_line_precedence(self):
        (self.root / "local.env").write_text("WIN_QEMU=from-file\n")
        makefile = f'include {config.ROOT / "mk/config.mk"}\nprobe:\n\t@echo "$(WIN_QEMU)"\n'
        for arguments, expected in [
            ([], "from-environment"),
            (["WIN_QEMU=from-command"], "from-command"),
        ]:
            result = subprocess.run(
                ["make", "--no-print-directory", "-f", "-", *arguments],
                input=makefile,
                text=True,
                capture_output=True,
                cwd=self.root,
                env={"PATH": os.defpath, "WIN_QEMU": "from-environment"},
                check=True,
            )
            self.assertEqual(result.stdout.strip(), expected)

    def test_make_tool_defaults_without_local_env(self):
        makefile = (
            f"include {config.ROOT / 'mk/config.mk'}\nprobe:\n"
            '\t@printf "%s\\n" "$(CROSS_IA64)" "$$CROSS_IA64" '
            '"$(QEMU_X86)" "$$QEMU_X86" '
            '"$(VMM_CMDLINE)" "$$VMM_CMDLINE" '
            '"$(QEMU_EXPECT_EXIT)" "$$QEMU_EXPECT_EXIT"\n'
        )
        result = subprocess.run(
            ["make", "--no-print-directory", "-f", "-"],
            input=makefile,
            text=True,
            capture_output=True,
            cwd=self.root,
            env={"PATH": os.defpath},
            check=True,
        )
        self.assertEqual(
            result.stdout.splitlines(),
            [
                "ia64-linux-gnu-",
                "ia64-linux-gnu-",
                "qemu-system-x86_64",
                "qemu-system-x86_64",
                "exit=isa-debug-exit reserve_mib=128",
                "exit=isa-debug-exit reserve_mib=128",
                "0",
                "0",
            ],
        )

    def test_missing_backend_variables_fail_before_interop(self):
        for choice, variable in [
            ("win-qemu", "WIN_QEMU"),
            ("win-qemu", "WIN_QEMU_FW_CODE"),
            ("win-qemu", "WIN_QEMU_FW_VARS"),
            ("win-qemu", "WIN_STAGING_DIR"),
            ("vmware", "VMWARE_DIR"),
        ]:
            with self.subTest(variable=variable), patch.dict(os.environ, {variable: ""}):
                with self.assertRaisesRegex(RuntimeError, f"{variable} is unset"):
                    run.validate_backend(choice)

    def test_auto_selection_skips_unconfigured_windows(self):
        with (
            patch.object(run, "load_local_env"),
            patch.object(run, "validate_backend") as validate,
            patch.object(run, "run_qemu", return_value=4) as execute,
            patch.object(run, "ROOT", self.root),
            patch.object(run.subprocess, "run"),
            patch.object(run.os, "chdir"),
            patch.object(sys, "argv", ["run.py", "--no-build", "--log", str(self.log)]),
        ):
            validate.side_effect = [RuntimeError("WIN_QEMU is unset"), None]
            self.assertEqual(run.main(), 4)
            self.assertEqual(execute.call_args.args[0].backend, "wsl-qemu")

    def test_windows_paths_are_converted_and_wsl_paths_preserved(self):
        with patch.object(
            windows.subprocess, "check_output", return_value="/mnt/example/tool\n"
        ) as convert:
            self.assertEqual(windows.local_path(r"D:\Tools\tool"), Path("/mnt/example/tool"))
            self.assertEqual(convert.call_args.args[0], ["wslpath", "-u", r"D:\Tools\tool"])
            convert.reset_mock()
            self.assertEqual(windows.local_path(str(self.root)), self.root)
            convert.assert_not_called()

    def test_memory_units_and_bounds(self):
        self.assertEqual(run.memory_mib("4G"), 4096)
        self.assertEqual(run.memory_mib("3072"), 3072)
        for value in ["0", "4GB", "255", "-1", "999999999G"]:
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                run.memory_mib(value)

    def test_live_capture_and_paced_input_use_independent_gates(self):
        args = SimpleNamespace(
            screenshot=Path("menu.png"),
            screenshot_on=r"GNU GRUB.*2.12",
            screenshot_after=None,
            screenshot_delay=0.5,
            sendkey=["down", "up", "ret"],
            sendkey_after="Install",
            sendkey_delay=1,
            sendkey_interval=0.4,
        )
        actions = run.LiveActions(args, 10)
        self.assertFalse(actions.screenshot_due("GNU GRUB version 2.12", 11))
        self.assertTrue(actions.screenshot_due("GNU GRUB version 2.12", 11.5))
        actions.captured = True
        self.assertFalse(actions.screenshot_due("GNU GRUB version 2.12", 12))
        self.assertIsNone(actions.key_due("Install", 11))
        self.assertEqual(actions.key_due("Install", 12), "down")
        self.assertIsNone(actions.key_due("Install", 12.1))
        self.assertEqual(actions.key_due("Install", 12.5), "up")
        self.assertEqual(actions.key_due("Install", 13), "ret")
        self.assertIsNone(actions.key_due("Install", 14))
        args.screenshot_on = None
        args.screenshot_after = 3
        actions = run.LiveActions(args, 10)
        self.assertFalse(actions.screenshot_due("", 12.9))
        self.assertTrue(actions.screenshot_due("", 13))

    def test_live_screenshot_preserves_debug_exit_and_memory(self):
        with (
            patch.object(run, "load_local_env"),
            patch.object(run, "validate_backend"),
            patch.object(run, "run_qemu", return_value=2) as execute,
            patch.object(run, "ROOT", self.root),
            patch.object(run.subprocess, "run"),
            patch.object(run.os, "chdir"),
            patch.object(
                sys,
                "argv",
                [
                    "run.py",
                    "--backend",
                    "win-qemu",
                    "--no-build",
                    "--memory",
                    "4G",
                    "--screenshot",
                    str(self.root / "menu.png"),
                    "--screenshot-on",
                    "GRUB",
                    "--log",
                    str(self.log),
                ],
            ),
        ):
            self.assertEqual(run.main(), 2)
        self.assertEqual(execute.call_args.args[0].memory, 4096)
        self.assertIn("exit=isa-debug-exit", execute.call_args.args[3].split())
        self.assertNotIn("exit=halt", execute.call_args.args[3].split())

    def test_vmware_memory_and_keyboard_without_guest_tools(self):
        with patch.object(backend, "windows_path", side_effect=str):
            vmx = backend.create_vm(self.root, self.run_dir, False, memory=4096)
        self.assertIn('memsize = "4096"', vmx.read_text())
        with (
            patch.dict(os.environ, {"VMWARE_DIR": str(self.root)}),
            patch.object(
                backend.subprocess, "run", return_value=SimpleNamespace(returncode=0)
            ) as call,
        ):
            backend.send_key(self.run_dir, "private.vmx", "ret", 9999999999)
        self.assertEqual(call.call_args.args[0][-4:], ["MKS", "sendKeyEvent", str(0x280007), "0"])

    def test_cleanup_waits_for_a_killed_emulator_to_leave_the_task_list(self):
        listed = iter([True, True, True, False, False])
        with (
            patch.object(windows, "owned_windows_pids", return_value=[4242]),
            patch.object(windows, "pid_in_tasklist", side_effect=lambda pid: next(listed)),
            patch.object(
                windows.subprocess, "run", return_value=SimpleNamespace(stdout=b"", stderr=b"")
            ),
            patch.object(windows.time, "sleep"),
            patch.object(windows, "system_tool", side_effect=lambda name: name),
            patch.object(windows, "system_cwd", return_value=None),
        ):
            windows.kill_windows_processes("qemu-system-x86_64.exe", self.root)
        with (
            patch.object(windows, "owned_windows_pids", return_value=[4242]),
            patch.object(windows, "pid_in_tasklist", return_value=True),
            patch.object(
                windows.subprocess, "run", return_value=SimpleNamespace(stdout=b"", stderr=b"")
            ),
            patch.object(windows.time, "sleep"),
            patch.object(windows.time, "monotonic", side_effect=[0.0, 0.0, 16.0]),
            patch.object(windows, "system_tool", side_effect=lambda name: name),
            patch.object(windows, "system_cwd", return_value=None),
        ):
            with self.assertRaises(RuntimeError):
                windows.kill_windows_processes("qemu-system-x86_64.exe", self.root)

    def test_each_run_keeps_its_esp_after_the_shared_image_changes(self):
        with (
            patch.object(run.subprocess, "run"),
            run.staged_esp(self.root, {}, True, True, "") as image,
        ):
            (self.root / "build/esp.img").write_bytes(b"different invocation")
            self.assertEqual(image.read_bytes(), b"original disk")
            with patch.object(backend, "windows_path", side_effect=str):
                backend.create_vm(self.root, self.run_dir, False, esp_image=image)
            self.assertEqual((self.run_dir / "esp-flat.img").read_bytes(), b"original disk")
        self.assertFalse(image.exists())

    def test_vmware_inventory_retries_only_the_transient_read(self):
        failed = SimpleNamespace(
            returncode=1, stdout=b"", stderr=b"UtilAcceptVsock: accept4 failed 110"
        )
        ok = SimpleNamespace(returncode=0, stdout=b"Total running VMs: 0", stderr=b"")
        with patch.object(backend.subprocess, "run", side_effect=[failed, ok]) as call:
            self.assertEqual(
                backend.vmrun("vmrun.exe", self.run_dir, ["list"], 10), "Total running VMs: 0"
            )
            self.assertEqual(call.call_count, 2)
        with patch.object(backend.subprocess, "run", return_value=failed) as call:
            with self.assertRaisesRegex(RuntimeError, "UtilAcceptVsock"):
                backend.vmrun("vmrun.exe", self.run_dir, ["start", "private.vmx", "nogui"], 10)
            self.assertEqual(call.call_count, 1)

    def test_windows_cleanup_retries_late_handles_and_preserves_original_error(self):
        with (
            patch.object(windows, "local_path", return_value=self.root),
            patch.object(windows, "windows_path", side_effect=str),
            patch.object(windows.time, "sleep"),
            patch.object(
                windows.shutil, "rmtree", side_effect=[PermissionError("locked"), None]
            ) as remove,
        ):
            with windows.run_directory(True, False):
                pass
            self.assertEqual(remove.call_count, 2)
        with (
            patch.object(windows, "local_path", return_value=self.root),
            patch.object(windows, "windows_path", side_effect=str),
            patch.object(windows.time, "sleep"),
            patch.object(windows.shutil, "rmtree", side_effect=PermissionError("locked")),
        ):
            with self.assertRaisesRegex(RuntimeError, "original inventory error"):
                with windows.run_directory(True, False):
                    raise RuntimeError("original inventory error")

    def test_cpus_option_rejects_missing_management_capacity(self):
        for cpus in ["0", "1", "257", "-1"]:
            with (
                self.subTest(cpus=cpus),
                patch.object(run, "load_local_env"),
                patch.object(sys, "argv", ["run.py", "--cpus", cpus]),
            ):
                with self.assertRaises(SystemExit) as error:
                    run.main()
                self.assertEqual(error.exception.code, 2)

    def test_vmware_cpus_are_one_thread_per_core(self):
        with patch.object(backend, "windows_path", side_effect=str):
            vmx = backend.create_vm(self.root, self.run_dir, False, cpus=2)
        text = vmx.read_text()
        self.assertIn('numvcpus = "2"', text)
        self.assertIn('cpuid.coresPerSocket = "2"', text)

    def qemu_context(self):
        return patch.object(run, "run_directory", return_value=nullcontext(self.run_dir))

    def test_whpx_retry_resets_private_disk_and_vars_and_shares_deadline(self):
        commands, deadlines = [], []

        def attempt(command, args, temp, serial_path, log, deadline, is_windows):
            commands.append(command)
            deadlines.append(deadline)
            self.assertTrue(is_windows)
            self.assertEqual((temp / "esp.img").read_bytes(), b"original disk")
            self.assertEqual((temp / "vars.fd").read_bytes(), b"vars")
            (temp / "esp.img").write_bytes(b"dirty disk")
            (temp / "vars.fd").write_bytes(b"dirty vars")
            if len(commands) == 1:
                log.write_text("")
                return 1, "WHPX: Failed to create partition", False, False
            log.write_text("VMM shutdown kind=0 exit_code=4\n")
            return 9, "", False, False

        with (
            self.qemu_context(),
            patch.object(run, "windows_path", side_effect=lambda p: f"W:\\{p.name}"),
            patch.object(run, "qemu_attempt", side_effect=attempt),
        ):
            self.assertEqual(run.run_qemu(self.args, self.root, self.log, "exit=isa-debug-exit"), 4)
        self.assertEqual([c[c.index("-accel") + 1] for c in commands], ["whpx", "tcg"])
        self.assertIn("max", commands[1])
        self.assertNotIn("-qmp", commands[0])
        self.assertEqual(
            commands[0][commands[0].index("-smp") + 1], "4,sockets=1,cores=4,threads=1"
        )
        self.assertEqual(
            commands[1][commands[1].index("-smp") + 1], "4,sockets=1,cores=4,threads=1"
        )
        self.assertEqual(deadlines[0], deadlines[1])
        self.assertEqual((self.root / "vars").read_bytes(), b"vars")
        self.assertEqual((self.root / "build/esp.img").read_bytes(), b"original disk")

    def test_media_uses_ahci_with_independent_nvme_esp(self):
        self.args.cdrom = self.root / "installation.iso"
        self.args.disk = self.root / "installation.img"
        self.args.cdrom.write_bytes(b"iso source")
        self.args.disk.write_bytes(b"raw source")
        captured = []

        def attempt(command, *unused):
            captured.extend(command)
            self.assertEqual((self.run_dir / "guest.iso").read_bytes(), b"iso source")
            self.assertEqual((self.run_dir / "guest.img").read_bytes(), b"raw source")
            (self.run_dir / "guest.img").write_bytes(b"changed by guest")
            self.log.write_text("VMM shutdown kind=0 exit_code=0\n")
            return 1, "", False, False

        with (
            self.qemu_context(),
            patch.object(run, "windows_path", side_effect=str),
            patch.object(run, "qemu_attempt", side_effect=attempt),
        ):
            self.assertEqual(run.run_qemu(self.args, self.root, self.log, "exit=isa-debug-exit"), 0)
        self.assertIn("nvme,drive=vmm-esp,serial=VMMESP,addr=03.0,bootindex=1", captured)
        self.assertIn("ide-cd,drive=guest-cdrom,bus=ide.0", captured)
        self.assertIn("ide-hd,drive=guest-disk,bus=ide.1", captured)
        self.assertIn("-nodefaults", captured)
        self.assertEqual(self.args.disk.read_bytes(), b"raw source")

    def test_esp_controller_is_nvme_on_every_backend(self):
        # The ESP is the NVMe controller at 00:03.0, so vmm_media=auto holds
        # on WHPX and KVM alike.
        firmware = {"OVMF_CODE": str(self.root / "code"), "OVMF_VARS": str(self.root / "vars")}
        for backend_name, accel in [("win-qemu", "whpx"), ("wsl-qemu", "kvm")]:
            captured = []

            def attempt(command, *unused):
                captured.extend(command)
                self.log.write_text("VMM shutdown kind=0 exit_code=0\n")
                return 1, "", False, False

            with (
                self.qemu_context(),
                patch.dict(os.environ, firmware),
                patch.object(run, "windows_path", side_effect=str),
                patch.object(run, "linux_accelerator", return_value=(accel, "host")),
                patch.object(run, "qemu_attempt", side_effect=attempt),
            ):
                self.args.backend = backend_name
                self.assertEqual(
                    run.run_qemu(self.args, self.root, self.log, "exit=isa-debug-exit"), 0
                )
            esp = [c for c in captured if "drive=vmm-esp" in c]
            self.assertEqual(esp, ["nvme,drive=vmm-esp,serial=VMMESP,addr=03.0,bootindex=1"])
            self.assertNotIn("virtio-blk-pci", [c.split(",")[0] for c in captured])

    def test_explicit_tcg_ignores_available_accelerators(self):
        firmware = {"OVMF_CODE": str(self.root / "code"), "OVMF_VARS": str(self.root / "vars")}
        self.args.tcg, self.args.cpu_flags = True, "-svm"
        for name in ["wsl-qemu", "win-qemu"]:
            captured = []

            def attempt(command, *unused):
                captured.extend(command)
                self.log.write_text("VMM shutdown kind=0 exit_code=0\n")
                return 1, "", False, False

            with (
                self.qemu_context(),
                patch.dict(os.environ, firmware),
                patch.object(run, "windows_path", side_effect=str),
                patch.object(run, "linux_accelerator", return_value=("kvm", "host")) as probe,
                patch.object(run, "qemu_attempt", side_effect=attempt),
            ):
                self.args.backend = name
                self.assertEqual(
                    run.run_qemu(self.args, self.root, self.log, "exit=isa-debug-exit"), 0
                )
                probe.assert_not_called()
            self.assertEqual(captured[captured.index("-accel") + 1], "tcg")
            self.assertEqual(captured[captured.index("-cpu") + 1], "max,-svm")

    def test_vga_device_and_cpu_flags(self):
        # --vga cirrus selects the Cirrus VGA; --cpu-flags reaches -cpu
        # only where QEMU takes a CPU model (TCG/KVM), the default stays.
        captured = []

        def attempt(command, *unused):
            captured.append(command)
            self.log.write_text("VMM shutdown kind=0 exit_code=0\n")
            return 1, "", False, False

        firmware = {"OVMF_CODE": str(self.root / "code"), "OVMF_VARS": str(self.root / "vars")}
        with (
            self.qemu_context(),
            patch.dict(os.environ, firmware),
            patch.object(run, "linux_accelerator", return_value=("tcg", "max")),
            patch.object(run, "qemu_attempt", side_effect=attempt),
        ):
            self.args.backend = "wsl-qemu"
            self.assertEqual(run.run_qemu(self.args, self.root, self.log, "exit=isa-debug-exit"), 0)
            self.args.vga, self.args.cpu_flags = "cirrus", "-svm"
            self.assertEqual(run.run_qemu(self.args, self.root, self.log, "exit=isa-debug-exit"), 0)
            # PCI reset fixture: --vga-root-port puts the display device behind its own root port.
            self.args.vga_root_port = True
            self.assertEqual(run.run_qemu(self.args, self.root, self.log, "exit=isa-debug-exit"), 0)
        default, changed, behind = captured
        self.assertEqual(default[default.index("-nodefaults") + 2], "VGA")
        self.assertEqual(default[default.index("-cpu") + 1], "max")
        self.assertEqual(changed[changed.index("-nodefaults") + 2], "cirrus-vga")
        self.assertEqual(changed[changed.index("-cpu") + 1], "max,-svm")
        self.assertNotIn("pcie-root-port,id=guest-vga-port,chassis=9,addr=06.0", changed)
        at = behind.index("-nodefaults")
        self.assertEqual(
            behind[at + 1 : at + 5],
            [
                "-device",
                "pcie-root-port,id=guest-vga-port,chassis=9,addr=06.0",
                "-device",
                "cirrus-vga,bus=guest-vga-port",
            ],
        )
        self.assertEqual(
            behind.count("cirrus-vga,bus=guest-vga-port") + behind.count("cirrus-vga"), 1
        )

    def test_usb_keyboard_disables_ps2(self):
        self.args.usb_keyboard = True
        captured = []

        def attempt(command, *unused):
            captured.extend(command)
            self.log.write_text("VMM shutdown kind=0 exit_code=0\n")
            return 1, "", False, False

        with (
            self.qemu_context(),
            patch.object(run, "windows_path", side_effect=str),
            patch.object(run, "qemu_attempt", side_effect=attempt),
        ):
            self.assertEqual(run.run_qemu(self.args, self.root, self.log, "exit=isa-debug-exit"), 0)
        self.assertEqual(captured[captured.index("-machine") + 1], "q35,i8042=off")
        self.assertIn("usb-kbd,bus=guest-usb-host.0", captured)

    def test_usb_and_nvme_private_attachments(self):
        media = self.root / "media.img"
        media.write_bytes(bytes(1024))
        for controller in ["xhci", "ehci"]:
            args = SimpleNamespace(
                usb_disk=media,
                usb_cdrom=media,
                nvme=media,
                usb_controller=controller,
                usb_keyboard=True,
            )
            command = run.storage_arguments(args, self.run_dir, False)
            self.assertIn("usb-storage,drive=guest-usb-disk,bus=guest-usb-host.0", command)
            self.assertIn("scsi-cd,drive=guest-usb-cdrom,bus=guest-usb-cd.0", command)
            self.assertIn("nvme,drive=guest-nvme,serial=GUESTNVME,addr=06.0", command)
            self.assertIn("usb-kbd,bus=guest-usb-host.0", command)
            self.assertIn(
                "qemu-xhci,id=guest-usb-host,addr=05.0"
                if controller == "xhci"
                else "ich9-usb-ehci1,id=guest-usb-host,addr=05.7",
                command,
            )
            (self.run_dir / "guest-usb-disk.img").write_bytes(b"guest write")
            self.assertEqual(media.read_bytes(), bytes(1024))
        with patch.object(backend, "windows_path", side_effect=str):
            vmx = backend.create_vm(self.root, self.run_dir, False, nvme=media)
        self.assertIn('nvme0:0.fileName = "nvme.vmdk"', vmx.read_text())
        self.assertIn('pciBridge4.virtualDev = "pcieRootPort"', vmx.read_text())
        self.assertNotIn('nvme0.pciSlotNumber = "38"', vmx.read_text())
        self.assertIn('RW 2 FLAT "nvme-flat.img"', (self.run_dir / "nvme.vmdk").read_text())

    def test_physical_build_preserves_bootdisk_input(self):
        disk = self.root / "build/bootdisk.img"
        disk.write_bytes(b"FAT disk" + bytes(504))

        def build(command, **kwargs):
            # The normal smoke ESP prerequisite replaces this exact filename.
            if command[0] == "make" and "esp" in command:
                disk.write_bytes(b"smoke PE module")
            return SimpleNamespace(returncode=0)

        with (
            patch.object(run, "ROOT", self.root),
            patch.object(run, "load_local_env"),
            patch.object(run, "validate_backend"),
            patch.object(run.os, "chdir"),
            patch.object(run.subprocess, "run", side_effect=build),
            patch.object(run, "run_qemu", return_value=0) as execute,
            patch.object(
                sys,
                "argv",
                ["run.py", "--backend", "win-qemu", "--disk", str(disk), "--log", str(self.log)],
            ),
        ):
            self.assertEqual(run.main(), 0)
        self.assertEqual(disk.read_bytes(), b"FAT disk" + bytes(504))
        self.assertEqual(execute.call_args.args[0].disk, disk)
        self.assertIn("vmm_media=auto", execute.call_args.args[3].split())

    def test_vmware_uses_scsi_esp_and_sata_guest_media(self):
        iso, disk = self.root / "install.iso", self.root / "disk.img"
        iso.write_bytes(b"iso")
        disk.write_bytes(bytes(1024))
        with patch.object(backend, "windows_path", side_effect=str):
            vmx = backend.create_vm(self.root, self.run_dir, False, iso, disk)
        settings = vmx.read_text()
        self.assertIn('scsi0:0.fileName = "esp.vmdk"', settings)
        self.assertIn('scsi0.pciSlotNumber = "16"', settings)
        self.assertIn('sata0:0.deviceType = "cdrom-image"', settings)
        self.assertIn('sata0:1.fileName = "guest.vmdk"', settings)
        self.assertIn('RW 2 FLAT "guest-flat.img"', (self.run_dir / "guest.vmdk").read_text())

    def test_startup_error_cannot_masquerade_as_vmm_success(self):
        self.log.write_text("")
        with (
            self.qemu_context(),
            patch.object(run, "windows_path", side_effect=str),
            patch.object(run, "qemu_attempt", return_value=(1, "", False, False)),
        ):
            self.assertEqual(run.run_qemu(self.args, self.root, self.log, "exit=isa-debug-exit"), 1)

    def test_debug_exit_status_must_agree_with_serial(self):
        self.log.write_text("VMM shutdown kind=0 exit_code=4\n")
        with (
            self.qemu_context(),
            patch.object(run, "windows_path", side_effect=str),
            patch.object(run, "qemu_attempt", return_value=(1, "", False, False)),
        ):
            with self.assertRaisesRegex(RuntimeError, "does not match VMM exit 4"):
                run.run_qemu(self.args, self.root, self.log, "exit=isa-debug-exit")

    def test_confirmed_capture_survives_milestone_termination(self):
        serial = self.run_dir / "serial.log"
        serial.write_text("prompt")
        self.args.screenshot = self.root / "capture.png"
        self.args.screenshot_on = "prompt"
        self.args.screenshot_delay = 0
        self.args.screenshot_text = ["prompt"]
        process = SimpleNamespace(
            poll=Mock(side_effect=[None, None, KeyboardInterrupt]),
            stdin=io.BytesIO(),
            returncode=None,
        )

        def monitor(command, arguments=None):
            if command == "screendump":
                Path(arguments["filename"]).write_bytes(b"P6\n1 1\n255\n\0\0\0")

        qmp = SimpleNamespace(command=monitor, close=lambda: None)
        with (
            patch.object(run.subprocess, "Popen", return_value=process),
            patch.object(run, "QMP", return_value=qmp),
            patch.object(run, "screenshot_matches", return_value=True),
            patch.object(run, "stop_process"),
            patch.object(run.time, "sleep"),
            patch.object(run.time, "monotonic", return_value=0),
        ):
            with self.assertRaises(KeyboardInterrupt):
                run.qemu_attempt(["qemu"], self.args, self.run_dir, serial, self.log, 100, False)
        self.assertTrue(self.args.screenshot.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(self.log.read_text(), "prompt")

    def capture_attempt(self, outcome, primary_complete=True):
        serial = self.run_dir / "serial.log"
        serial.write_text("prompt")
        self.args.screenshot = self.root / "capture.png"
        self.args.screenshot_after = 0
        self.args.late_screenshot = self.root / "late.png"
        self.args.late_screenshot_after = 0
        self.args.screenshot_every = 1
        self.args.screenshot_dir = self.root / "periodic"
        process = SimpleNamespace(
            poll=Mock(side_effect=[None, outcome]), stdin=io.BytesIO(), returncode=0
        )

        def monitor(command, arguments=None):
            if command == "screendump":
                path = Path(arguments["filename"])
                pixels = b"\xff\0\0" if primary_complete or path.name != "screen.ppm" else b""
                path.write_bytes(b"P6\n1 1\n255\n" + pixels)

        qmp = SimpleNamespace(command=monitor, close=lambda: None)
        with (
            patch.object(run.subprocess, "Popen", return_value=process),
            patch.object(run, "QMP", return_value=qmp),
            patch.object(run, "stop_process"),
            patch.object(run.time, "sleep"),
            patch.object(run.time, "monotonic", side_effect=[0, 1, 1, 101]),
        ):
            return run.qemu_attempt(["qemu"], self.args, self.run_dir, serial, self.log, 100, False)

    def test_requested_captures_survive_interruption_and_error(self):
        for error in [KeyboardInterrupt(), RuntimeError("QMP disconnected")]:
            with self.subTest(error=type(error).__name__):
                with self.assertRaises(type(error)) as raised:
                    self.capture_attempt(error)
                self.assertIs(raised.exception, error)
                for path in [
                    self.args.screenshot,
                    self.args.late_screenshot,
                    self.args.screenshot_dir / "t00001.png",
                ]:
                    self.assertTrue(path.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"))
                    path.unlink()
                self.assertEqual(self.log.read_text(), "prompt")

    def test_partial_primary_capture_does_not_replace_interruption(self):
        with self.assertRaises(KeyboardInterrupt):
            self.capture_attempt(KeyboardInterrupt(), primary_complete=False)
        self.assertFalse(self.args.screenshot.exists())
        self.assertTrue(self.args.late_screenshot.is_file())
        self.assertTrue((self.args.screenshot_dir / "t00001.png").is_file())

    def test_capture_destination_error_does_not_replace_interruption(self):
        (self.root / "periodic").write_text("occupied")
        error = KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt) as raised:
            self.capture_attempt(error)
        self.assertIs(raised.exception, error)
        self.assertTrue(self.args.screenshot.is_file())
        self.assertTrue(self.args.late_screenshot.is_file())

    def test_timeout_preserves_completed_captures_after_partial_primary(self):
        result = self.capture_attempt(None, primary_complete=False)
        self.assertTrue(result[-1])
        self.assertTrue(self.args.late_screenshot.is_file())
        self.assertTrue((self.args.screenshot_dir / "t00001.png").is_file())

    def test_partial_primary_capture_rejects_normal_completion(self):
        with self.assertRaisesRegex(RuntimeError, "truncated QEMU screendump"):
            self.capture_attempt(0, primary_complete=False)
        self.assertTrue(self.args.late_screenshot.is_file())
        self.assertTrue((self.args.screenshot_dir / "t00001.png").is_file())

    def test_linux_control_hmp_output_is_recorded(self):
        serial = self.run_dir / "serial.log"
        serial.write_text("")
        (self.run_dir / "qmp.sock").write_text("")
        control = self.root / "ctl"
        control.write_text("")
        self.args.control_file = control
        self.args.sendkey_interval = 0

        def poll():
            if not control.read_text():
                control.write_text("hmp info status\n")
                return None
            raise KeyboardInterrupt

        process = SimpleNamespace(poll=poll, stdin=io.BytesIO(), returncode=None)

        def monitor(command, arguments=None):
            if command == "human-monitor-command":
                return f"reply to {arguments['command-line']}\r\n"

        qmp = SimpleNamespace(command=monitor, close=lambda: None, sock=Mock())
        with (
            patch.object(run.subprocess, "Popen", return_value=process),
            patch.object(run, "QMP", return_value=qmp),
            patch.object(run, "stop_process"),
            patch.object(run.time, "sleep"),
            patch.object(run.time, "monotonic", return_value=0),
        ):
            with self.assertRaises(KeyboardInterrupt):
                run.qemu_attempt(["qemu"], self.args, self.run_dir, serial, self.log, 100, False)
        self.assertIn("reply to info status", (self.run_dir / "qemu.stdout").read_text())

    def test_timeout_kills_windows_process_before_relay_and_preserves_log(self):
        serial = self.run_dir / "serial.log"
        serial.write_text("VMM shutdown kind=0 exit_code=4\nVMM halted exit_code=4\n")
        process = SimpleNamespace(poll=lambda: None, stdin=None, returncode=None)
        cleanup = []
        with (
            patch.object(run.subprocess, "Popen", return_value=process),
            patch.object(
                run, "kill_windows_processes", side_effect=lambda *a: cleanup.append("Windows")
            ),
            patch.object(run, "stop_process", side_effect=lambda *a: cleanup.append("relay")),
        ):
            result = run.qemu_attempt(
                ["qemu.exe"], self.args, self.run_dir, serial, self.log, 0, True
            )
        self.assertTrue(result[-1])
        self.assertEqual(cleanup, ["Windows", "relay"])
        self.assertIn("VMM halted exit_code=4", self.log.read_text())

    def test_vmware_start_timeout_stops_late_vm_and_preserves_serial(self):
        self.args.backend = "vmware"
        calls = []

        def vmrun(executable, temp, arguments, timeout):
            calls.append(arguments)
            if arguments[0] == "start":
                (temp / "serial.log").write_text("late start\n")
                raise subprocess.TimeoutExpired(executable, timeout)
            if arguments[0] == "list":
                return str(temp / "vmm.vmx")
            return ""

        with (
            patch.dict(os.environ, {"VMWARE_DIR": str(self.root)}),
            patch.object(backend, "run_directory", return_value=nullcontext(self.run_dir)),
            patch.object(backend, "windows_path", side_effect=str),
            patch.object(backend, "vmrun", side_effect=vmrun),
            patch.object(backend, "kill_windows_processes") as kill,
        ):
            self.assertEqual(backend.run_vmware(self.args, self.root, self.log, False), 5)
        self.assertEqual(calls[-1], ["stop", str(self.run_dir / "vmm.vmx"), "hard"])
        self.assertEqual(kill.call_args.args[0], "vmware-vmx.exe")
        self.assertEqual(self.log.read_text(), "late start\n")


if __name__ == "__main__":
    unittest.main()
