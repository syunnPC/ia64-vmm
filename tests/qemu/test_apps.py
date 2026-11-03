#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reject product-runner false passes and mismatched guest profiles."""

import os
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch
from concurrent.futures import wait as futures_wait

sys.path.insert(0, str(Path(__file__).resolve().parent))
import apps

sys.path.insert(0, str(apps.ROOT / "tools"))
from qemu import run

STATS = (
    "[info] tier1 linked=1 compiles=0 rejects=0 entries=0 tier1_retired=0 probes=0 "
    "links=0 enters=0 ran=0 ran_pending=0 declines=0 hot=0 held=0 demotions=0 "
    "evictions=0 replacements=0 recompiles=0 invalidations=0 site_exits=0\n"
)
END = (
    STATS
    + (
        "[info] tier1 lockstep checked=0 checked_retired=0 unchecked=0 unchecked_retired=0 "
        "divergences=0 captures=0 undo_pre_traps=0\n"
        "[info] hostwin instances=0 fills=0 fills_ro=0 live=0 unbacked=0 not_ram=0 backend_failures=0\n"
    )
    + "[info] milestone exit retired=1 exec_ms=1 idle_ms=0 wall_ms=1\n"
)


class AppsTests(unittest.TestCase):
    def test_snapshot_reserve_budget(self):
        log = (
            "VMM reserve total_bytes=134217728 used_bytes=83886080 free_bytes=50331648\n"
            "code snapshot budget_bytes=25165824 policy=default\n"
        )
        check = apps.snapshot_budget_problems(None)
        self.assertEqual(check(log), [])
        self.assertTrue(check(log.replace("25165824", "50331648")))
        self.assertTrue(check(log.replace("free_bytes=", "missing=")))
        self.assertTrue(check(log.replace("policy=default", "policy=user")))
        user = log.replace("25165824 policy=default", "104857600 policy=user")
        self.assertEqual(apps.snapshot_budget_problems(100)(user), [])
        self.assertTrue(apps.snapshot_budget_problems(100)(log))
        late = log.replace(
            "code snapshot budget", "code snapshot late_reserve_bytes=8388608\ncode snapshot budget"
        )
        self.assertTrue(check(late))
        self.assertEqual(check(late.replace("25165824", "20971520")), [])
        tiny = (
            "VMM reserve total_bytes=4096 used_bytes=4096 free_bytes=0\n"
            "code snapshot late_reserve_bytes=8388608\n"
            "code snapshot budget_bytes=8192 policy=default\n"
        )
        self.assertEqual(check(tiny), [])

    def test_profile_count_and_consistent_failure(self):
        lines = ["SMP-APP BEGIN profile=ipi-itc"]
        exp = apps.smplog.expected("vmm", 3, "ipi-itc")
        lines += [f"{status} {name}" for name, (status, _, _) in exp.items()]
        lines += [f"SMP-APP END pass={len(exp)} fail=0 skip=0 cpus=3"]
        guest = "\n".join(lines)
        check = apps.smp_problems("ipi-itc", 3)
        self.assertEqual(check(guest), [])
        wrong_profile = (
            "SMP-APP BEGIN profile=tlb-purge\n"
            + "".join(f"PASS {name}\n" for name in apps.smplog.PROFILE_CASES["tlb-purge"])
            + "SMP-APP END pass=8 fail=0 skip=0 cpus=3"
        )
        parsed = apps.smplog.parse(wrong_profile.splitlines())
        # The old adaptive checker would accept this internally consistent
        # different app. Requested-profile validation must reject it.
        self.assertEqual(
            apps.smplog.check(parsed, apps.smplog.expected("vmm", 3, parsed["profile"])), []
        )
        self.assertTrue(check(wrong_profile))
        self.assertTrue(check(guest.replace("cpus=3", "cpus=2")))
        bad = guest.replace("PASS ap-start", "FAIL ap-start injected").replace(
            f"pass={len(exp)} fail=0", f"pass={len(exp) - 1} fail=1"
        )
        self.assertEqual(apps.smplog.parse(bad.splitlines())["summary"]["fail"], 1)
        self.assertTrue(check(bad))

    def test_wake_variant_has_exactly_its_bad_vector(self):
        good = (
            "SMP-APP BEGIN profile=broken-wake-vector\n"
            "FAIL sal-ap-wake vector=0x00000000000000ff\n"
            "PASS set-vectors-boot-rendez\nPASS madt-exact\n"
            "SMP-APP END pass=2 fail=1 skip=0 cpus=2"
        )
        check = apps.smp_problems("broken-wake-vector", 2, broken="wake-vector")
        self.assertEqual(check(good), [])
        errors = apps.ordinary_boot_problems(good)
        self.assertEqual(len(errors), 1)
        self.assertTrue(errors[0].startswith("sal-ap-wake: FAIL "))
        wrong = good.replace("PASS madt-exact", "FAIL madt-exact injected").replace(
            "pass=2 fail=1", "pass=1 fail=2"
        )
        self.assertTrue(check(wrong))

    def test_faultloop_bound_and_runner_seconds(self):
        case = apps.cases()[0]
        log = (
            "".join("[guest] " + line + "\n" for line in case.needs)
            + END
            + "[info] VMM shutdown kind=0 exit_code=3\n"
        )
        self.assertEqual(apps.problems(case, 3, log, 1), [])
        self.assertEqual(apps.problems(case, 3, log, 300), [])
        self.assertIn("timeout_sec=120", case.cmdline)
        self.assertIn("1048576 interruption deliveries without a retired instruction", case.needs)
        slow = log.replace("exec_ms=1 idle_ms=0 wall_ms=1", "exec_ms=60000 idle_ms=0 wall_ms=60000")
        self.assertIn(
            "faultloop executor wall_ms=60000, expected <60000", apps.problems(case, 3, slow, 300)
        )
        headroom = log.replace("wall_ms=1", "wall_ms=59999")
        self.assertEqual(apps.problems(case, 3, headroom, 300), [])
        self.assertEqual(apps.mutation_problems(case, log, 300), [])
        late = log.replace("exec_ms=1 idle_ms=0 wall_ms=1", "exec_ms=9000 idle_ms=0 wall_ms=9000")
        self.assertTrue(any("milestone" in p for p in apps.problems(case, 3, late, 1)))

    def test_only_unknown_is_an_error(self):
        result = subprocess.run(
            [sys.executable, str(apps.ROOT / "tests/qemu/apps.py"), "--only", "does-not-exist"],
            capture_output=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"unknown case", result.stderr)

    def test_backend_failure_clears_previous_log(self):
        with tempfile.TemporaryDirectory(dir=apps.ROOT / "build") as tmp:
            log = Path(tmp) / "stale.log"
            log.write_text("previous successful guest")
            with (
                patch.object(run, "load_local_env"),
                patch.object(
                    run, "validate_backend", side_effect=RuntimeError("deliberate backend failure")
                ),
                patch.object(sys, "argv", ["run.py", "--backend", "wsl-qemu", "--log", str(log)]),
            ):
                with self.assertRaisesRegex(RuntimeError, "deliberate backend failure"):
                    run.main()
            self.assertEqual(log.read_text(), "")

    def test_harness_timeout_terminates_exact_runner_and_allows_cleanup(self):
        case = apps.Case("timeout-unit", "hello", "engine=tier0", timeout=1)
        with tempfile.TemporaryDirectory(dir=apps.ROOT / "build") as tmp:
            with (
                patch.object(apps, "LOGS", Path(tmp)),
                patch.object(apps.subprocess, "Popen") as launch,
            ):
                process = launch.return_value
                process.communicate.side_effect = [
                    subprocess.TimeoutExpired("runner", 1),
                    (b"cleaned", None),
                ]
                process.pid = 12345
                process.returncode = 130
                result = apps.run(case)
                process.terminate.assert_called_once_with()
                process.kill.assert_not_called()
                self.assertEqual(result[0], -1)
                self.assertIn("cleaned", result[2])
                self.assertTrue(launch.call_args.kwargs["start_new_session"])

    def test_harness_timeout_escalates_only_its_session(self):
        case = apps.Case("timeout-kill-unit", "hello", "engine=tier0", timeout=1)
        with tempfile.TemporaryDirectory(dir=apps.ROOT / "build") as tmp:
            with (
                patch.object(apps, "LOGS", Path(tmp)),
                patch.object(apps.subprocess, "Popen") as launch,
                patch.object(apps.os, "killpg") as kill,
            ):
                process = launch.return_value
                process.communicate.side_effect = [
                    subprocess.TimeoutExpired("runner", 1),
                    subprocess.TimeoutExpired("runner", 30),
                    (b"killed", None),
                ]
                process.pid = 12345
                process.returncode = -9
                result = apps.run(case)
                kill.assert_called_once_with(12345, apps.signal.SIGKILL)
                self.assertEqual(
                    [c.kwargs["timeout"] for c in process.communicate.call_args_list], [121, 30, 30]
                )
                record = apps.json.loads(result[4].with_suffix(".json").read_text())
                self.assertTrue(record["timed_out"] and record["killed_session"])

    def test_harness_timeout_detached_pipe_is_bounded(self):
        case = apps.Case("timeout-pipe-unit", "hello", "engine=tier0", timeout=1)
        with tempfile.TemporaryDirectory(dir=apps.ROOT / "build") as tmp:
            with (
                patch.object(apps, "LOGS", Path(tmp)),
                patch.object(apps.subprocess, "Popen") as launch,
                patch.object(apps.os, "killpg"),
            ):
                process = launch.return_value
                process.communicate.side_effect = [
                    subprocess.TimeoutExpired("runner", 1),
                    subprocess.TimeoutExpired("runner", 30),
                    subprocess.TimeoutExpired("runner", 30, output=b"partial"),
                ]
                process.pid = 12345
                process.returncode = -9
                result = apps.run(case)
                process.stdout.close.assert_called_once_with()
                process.wait.assert_called_once_with(timeout=30)
                self.assertEqual(result[0], -1)
                self.assertIn("partial", result[2])

    def test_harness_timeout_unreapable_runner_cannot_hang(self):
        case = apps.Case("timeout-wait-unit", "hello", "engine=tier0", timeout=1)
        with tempfile.TemporaryDirectory(dir=apps.ROOT / "build") as tmp:
            with (
                patch.object(apps, "LOGS", Path(tmp)),
                patch.object(apps.subprocess, "Popen") as launch,
                patch.object(apps.os, "killpg"),
            ):
                process = launch.return_value
                process.communicate.side_effect = [subprocess.TimeoutExpired("runner", 1)] * 3
                process.wait.side_effect = subprocess.TimeoutExpired("runner", 30)
                process.pid = 12345
                process.returncode = None
                result = apps.run(case)
                self.assertEqual(result[0], -1)
                self.assertIn("could not be reaped", result[2])
                process.__exit__.assert_not_called()

    def test_behaviour_requires_high_alias(self):
        self.assertEqual(
            apps.behaviour_problems("UEFI behaviour: high runtime aliases pinned=1\n"), []
        )
        self.assertTrue(apps.behaviour_problems("UEFI behaviour: high runtime aliases pinned=0\n"))
        self.assertTrue(apps.behaviour_problems(""))

    def test_initpmi_entries(self):
        # One PALE_INIT, two PALE_PMI and two PALE_CHECK entries on vCPU0.
        good = "\n".join(
            f"[info] vCPU0 {kind} at ip=1 slot=0 psr=0"
            for kind in ("INIT", "PMI", "PMI", "machine check", "machine check")
        )
        self.assertEqual(apps.initpmi_problems(good), [])
        for line in good.splitlines():
            self.assertTrue(apps.initpmi_problems(good.replace(line, "", 1)))
        self.assertTrue(apps.initpmi_problems(good + "\n[info] vCPU0 INIT at ip=2 slot=0 psr=0"))
        self.assertTrue(apps.initpmi_problems(good.replace("vCPU0 machine", "vCPU1 machine")))
        self.assertTrue(
            {"initpmi-new-tier0", "initpmi-new-tier1"} <= {c.name for c in apps.cases()}
        )

    def test_clock_tests_precede_smoke_guest(self):
        result = subprocess.run(
            [
                "make",
                "--no-print-directory",
                "-n",
                "-o",
                "all",
                "-o",
                "build/tests/hal-smp",
                "-o",
                "build/tests/smp-exec",
                "test-qemu",
            ],
            cwd=apps.ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        for binary in ("hal-smp", "smp-exec"):
            self.assertLess(
                result.stdout.index(f"./build/tests/{binary} "),
                result.stdout.index("tools/qemu/run.sh"),
            )

    def test_worker_communication_error_cleans_up_its_runner(self):
        case = apps.Case("worker-error", "hello", "engine=tier0")
        with tempfile.TemporaryDirectory(dir=apps.ROOT / "build") as tmp:
            with (
                patch.object(apps, "LOGS", Path(tmp)),
                patch.object(apps.subprocess, "Popen") as launch,
                patch.object(apps.os, "killpg") as kill,
            ):
                process = launch.return_value
                process.pid = 12345
                process.communicate.side_effect = OSError("injected pipe error")
                with self.assertRaisesRegex(OSError, "injected pipe error"):
                    apps.run(case)
                process.terminate.assert_called_once_with()
                kill.assert_called_once_with(12345, apps.signal.SIGKILL)

    def test_parallel_runs_are_bounded_and_keep_every_result(self):
        selected = [apps.Case(str(i), "hello", "engine=tier0") for i in range(7)]
        barrier = threading.Barrier(3)
        lock = threading.Lock()
        active, peak = 0, 0

        def execute(case, runners):
            nonlocal active, peak
            with lock:
                active += 1
                peak = max(peak, active)
            if int(case.name) < 3:
                barrier.wait(timeout=5)
            with lock:
                active -= 1
            return (int(case.name) % 2, "", "", 1, None)

        with (
            patch.object(apps, "run", side_effect=execute),
            patch.object(apps.os, "sched_getaffinity", return_value=set(range(16))),
        ):
            results = list(apps.run_cases(selected, 3))
        self.assertEqual(peak, 3)
        self.assertEqual({case.name for case, _ in results}, {case.name for case in selected})
        for case, result in results:
            self.assertEqual(result, (int(case.name) % 2, "", "", 1, None))

    def test_timeout_stops_new_runs_and_drains_active_guests(self):
        selected = [apps.Case(str(i), "hello", "engine=tier0") for i in range(5)]
        started = threading.Barrier(2)
        release = threading.Event()

        def execute(case, runners):
            started.wait(timeout=5)
            if case.name == "0":
                return (-1, "", "", 1, None)
            self.assertTrue(release.wait(timeout=5))
            return (0, "", "", 1, None)

        with (
            patch.object(apps, "run", side_effect=execute),
            patch.object(apps.os, "sched_getaffinity", return_value=set(range(16))),
        ):
            results = apps.run_cases(selected, 2)
            first, result = next(results)
            self.assertEqual((first.name, result[0]), ("0", -1))
            release.set()
            self.assertEqual([case.name for case, _ in results], ["1"])

    def test_cpu_admission_and_oversized_guest_runs_alone(self):
        selected = [
            apps.Case(str(i), "hello", "engine=tier0", cpus=n)
            for i, n in enumerate((4, 4, 4, 16, 33, 4))
        ]
        active = {}
        lock = threading.Lock()
        first_pair = threading.Barrier(2)

        def execute(case, runners):
            with lock:
                active[case.name] = case.cpus
                self.assertTrue(sum(active.values()) <= 8 or len(active) == 1)
            if case.name in ("0", "1"):
                first_pair.wait(timeout=5)
            with lock:
                del active[case.name]
            return (int(case.name), "", "", 1, None)

        with (
            patch.object(apps.os, "sched_getaffinity", return_value=set(range(8))),
            patch.object(apps, "run", side_effect=execute),
        ):
            results = list(apps.run_cases(selected, 3))
        self.assertEqual(len(results), len(selected))
        for case, result in results:
            self.assertEqual(result, (int(case.name), "", "", 1, None))

    def test_timeout_in_completed_batch_prevents_refill(self):
        selected = [apps.Case(str(i), "hello", "engine=tier0") for i in range(4)]

        def execute(case, runners):
            return (-1 if case.name == "0" else int(case.name), "", "", 1, None)

        def batch(pending, **kwargs):
            return futures_wait(pending)

        with (
            patch.object(apps, "run", side_effect=execute) as run_case,
            patch.object(apps.os, "sched_getaffinity", return_value=set(range(16))),
            patch.object(apps, "wait", side_effect=batch),
        ):
            results = list(apps.run_cases(selected, 2))
        self.assertEqual(run_case.call_count, 2)
        self.assertEqual({case.name for case, _ in results}, {"0", "1"})
        for case, result in results:
            self.assertEqual(result[0], -1 if case.name == "0" else 1)

    def test_worker_exception_preserves_other_results_and_summary(self):
        selected = [apps.Case(str(i), "hello", "engine=tier0") for i in range(4)]

        def execute(case, runners):
            if case.name == "1":
                raise RuntimeError("injected worker error")
            return (int(case.name), "", "", 1, None)

        output = io.StringIO()
        with (
            patch.object(apps, "run", side_effect=execute),
            patch.object(apps, "cases", return_value=selected),
            patch.object(apps, "broken_guest_inputs", return_value=[]),
            patch.object(sys, "argv", ["apps.py", "--jobs", "2"]),
            patch.object(sys, "stdout", output),
        ):
            self.assertEqual(apps.main(), 1)
        text = output.getvalue()
        for case in selected:
            self.assertIn(f"qemu-apps: {case.name} (", text)
        self.assertIn("injected worker error", text)
        self.assertIn("4 failure(s)", text)
        with patch.object(apps, "run", side_effect=execute):
            results = list(apps.run_cases(selected, 2))
        self.assertEqual(len(results), len(selected))
        for case, result in results:
            if case.name == "1":
                self.assertEqual(result[0], -2)
                self.assertIn("injected worker error", result[2])
            else:
                self.assertEqual(result, (int(case.name), "", "", 1, None))

    def test_abort_terminates_each_runner_before_session_escalation(self):
        runners = apps.RunnerGroup()
        processes = [Mock(pid=12345), Mock(pid=12346)]
        events = []
        for process in processes:
            process.terminate.side_effect = lambda p=process: events.append(("term", p.pid))
            process.wait.side_effect = subprocess.TimeoutExpired("runner", 30)
        with (
            patch.object(apps.subprocess, "Popen", side_effect=processes),
            patch.object(
                apps.os, "killpg", side_effect=lambda pid, sig: events.append(("kill", pid))
            ),
        ):
            for _ in processes:
                runners.launch(["runner"], start_new_session=True)
            runners.abort()
            with self.assertRaises(RuntimeError):
                runners.launch(["must not start"])
        self.assertEqual(
            events, [("term", 12345), ("term", 12346), ("kill", 12345), ("kill", 12346)]
        )

    def test_interrupt_cleans_up_before_pool_shutdown_and_ignores_second_signal(self):
        case = apps.Case("interrupt", "hello", "engine=tier0")
        events = []

        def abort(runners):
            apps.signal.raise_signal(apps.signal.SIGINT)
            events.append("abort")

        original = apps.signal.getsignal(apps.signal.SIGINT)
        with (
            patch.object(apps, "run", return_value=(0, "", "", 1, None)),
            patch.object(apps, "wait", side_effect=KeyboardInterrupt),
            patch.object(apps.RunnerGroup, "abort", abort),
        ):
            with self.assertRaises(KeyboardInterrupt):
                list(apps.run_cases([case], 1))
        self.assertEqual(events, ["abort"])
        self.assertEqual(apps.signal.getsignal(apps.signal.SIGINT), original)

    def test_repeated_interrupt_reaps_real_runner_children(self):
        # Sleepers exercise the runner/session lifecycle without booting VMs.
        runner = """
import os, signal, subprocess, sys, time
from pathlib import Path
marker = Path(sys.argv[1])
child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
def stop(sig, frame):
    raise KeyboardInterrupt
signal.signal(signal.SIGTERM, stop)
try:
    marker.write_text(str(os.getpid()) + ' ' + str(child.pid))
    time.sleep(60)
except KeyboardInterrupt:
    marker.with_suffix('.cleanup').touch()
    time.sleep(.4)
finally:
    child.terminate()
    child.wait(timeout=5)
    marker.with_suffix('.done').touch()
"""
        controller = """
import subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path.cwd() / 'tests/qemu'))
import apps
apps.signal.signal(apps.signal.SIGINT, apps.interrupted)
apps.signal.signal(apps.signal.SIGTERM, apps.interrupted)
directory, script = sys.argv[1:]
def execute(case, runners):
    process = runners.launch([sys.executable, '-c', script, str(Path(directory) / case.name)],
                             start_new_session=True, stdout=subprocess.PIPE)
    try:
        process.communicate(timeout=10)
    finally:
        runners.discard(process)
    return (0, '', '', 1, None)
apps.run = execute
apps.cases = lambda: [apps.Case(str(i), 'hello', 'engine=tier0', cpus=1) for i in range(2)]
apps.broken_guest_inputs = lambda: []
sys.argv = ['apps.py', '--jobs', '2']
sys.exit(apps.main())
"""
        with tempfile.TemporaryDirectory(dir=apps.ROOT / "build") as tmp:
            # Background shells pass ignored SIGINT to their children.
            original = apps.signal.signal(apps.signal.SIGINT, apps.signal.SIG_IGN)
            try:
                process = subprocess.Popen(
                    [sys.executable, "-c", controller, tmp, runner],
                    cwd=apps.ROOT,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
            finally:
                apps.signal.signal(apps.signal.SIGINT, original)
            try:
                deadline = time.monotonic() + 5
                while (
                    not all((Path(tmp) / str(i)).exists() for i in range(2))
                    and time.monotonic() < deadline
                ):
                    time.sleep(0.01)
                self.assertTrue(all((Path(tmp) / str(i)).exists() for i in range(2)))
                process.send_signal(apps.signal.SIGINT)
                while not (Path(tmp) / "0.cleanup").exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue((Path(tmp) / "0.cleanup").exists())
                process.send_signal(apps.signal.SIGINT)
                output, _ = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 130, output)
                self.assertIn("stopped after 0 cases", output)
                for i in range(2):
                    self.assertTrue((Path(tmp) / f"{i}.done").exists(), output)
                    for pid in (Path(tmp) / str(i)).read_text().split():
                        with self.assertRaises(ProcessLookupError):
                            os.kill(int(pid), 0)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=5)

    def test_outer_make_cannot_override_case_selection(self):
        with tempfile.TemporaryDirectory(dir=apps.ROOT / "build") as tmp:
            root = Path(tmp)
            (root / "build").mkdir()
            (root / "build/esp.img").write_bytes(b"private ESP")
            environment = dict(
                os.environ,
                MAKEFLAGS="-- FIRMWARE=new BOOT_APP=wrong",
                MAKEOVERRIDES="FIRMWARE=new BOOT_APP=wrong",
                FIRMWARE="new",
                BOOT_APP="faultloop",
            )
            with patch.object(run.subprocess, "run") as calls:
                with run.staged_esp(root, environment, False, True, ""):
                    pass
            make_calls = [c for c in calls.call_args_list if c.args[0][0] == "make"]
            self.assertEqual(len(make_calls), 2)
            for call in make_calls:
                self.assertIn("FIRMWARE=new", call.args[0])
                self.assertIn("BOOT_APP=faultloop", call.args[0])
                self.assertEqual(call.kwargs["env"]["MAKEFLAGS"], "")
                self.assertEqual(call.kwargs["env"]["MAKEOVERRIDES"], "")


if __name__ == "__main__":
    unittest.main()
