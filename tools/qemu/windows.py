# SPDX-License-Identifier: GPL-2.0-or-later
"""WSL interop, private staging, and cleanup of owned Windows processes."""

from contextlib import contextmanager
import csv
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

from qemu.config import ROOT, required


def local_path(value):
    if re.match(r"^[A-Za-z]:[\\/]", value) or value.startswith("\\\\"):
        value = subprocess.check_output(["wslpath", "-u", value], text=True, timeout=10).strip()
    return Path(value).resolve()


def system_tool(name):
    directory = os.environ.get("WIN_SYSTEM32")
    if directory:
        path = local_path(directory)
        if name == "powershell.exe":
            path = path / "WindowsPowerShell/v1.0"
        executable = str(path / name)
    else:
        executable = name
    resolved = shutil.which(executable)
    if not resolved:
        raise RuntimeError(f"{name} not found; set WIN_SYSTEM32 or add it to PATH")
    return resolved


def system_cwd():
    return Path(system_tool("cmd.exe")).resolve().parent


def windows_path(path):
    return subprocess.check_output(
        ["wslpath", "-w", str(Path(path).resolve())], text=True, timeout=10
    ).strip()


def windows_available():
    if not shutil.which("wslpath"):
        return False
    try:
        result = subprocess.run(
            [system_tool("cmd.exe"), "/d", "/c", "exit 0"],
            cwd=system_cwd(),
            capture_output=True,
            timeout=10,
        )
        return result.returncode == 0
    except (OSError, RuntimeError, subprocess.SubprocessError):
        return False


@contextmanager
def run_directory(windows, keep):
    parent = local_path(required("WIN_STAGING_DIR")) if windows else ROOT / "build/runs"
    if windows and windows_path(parent).startswith("\\\\"):
        raise RuntimeError("WIN_STAGING_DIR must be on a Windows drive, not a WSL UNC path")
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
    print(f"run directory: {directory}", flush=True)
    try:
        yield directory
    finally:
        if keep:
            print(f"kept run directory: {directory}", file=sys.stderr)
        else:
            active_error = sys.exc_info()[1]
            for attempt in range(10):
                try:
                    shutil.rmtree(directory)
                    break
                except PermissionError as error:
                    # VMware can release Windows file handles after stop returns.
                    if attempt < 9:
                        time.sleep(0.2)
                        continue
                    print(
                        f"kept run directory after cleanup failure: {directory}: {error}",
                        file=sys.stderr,
                    )
                    if active_error is None:
                        raise


def copy_writable(source, dest):
    shutil.copyfile(source, dest)
    # DrvFS preserves the Program Files read-only attribute on some copies.
    dest.chmod(dest.stat().st_mode | 0o200)


def owned_windows_pids(image, directory):
    # WSL's Popen PID belongs to the interop relay, not the Windows process.
    # Match the private directory in CommandLine rather than killing by image.
    marker = (windows_path(directory).rstrip("\\") + "\\").replace("'", "''")
    name = image.replace("'", "''")
    script = (
        "$ErrorActionPreference='Stop'; "
        f"Get-CimInstance Win32_Process -Filter \"Name='{name}'\" | "
        f"Where-Object {{$_.CommandLine -and $_.CommandLine.IndexOf('{marker}', "
        "[StringComparison]::OrdinalIgnoreCase) -ge 0} | "
        "ForEach-Object {$_.ProcessId}"
    )
    output = subprocess.check_output(
        [system_tool("powershell.exe"), "-NoProfile", "-NonInteractive", "-Command", script],
        cwd=system_cwd(),
        timeout=10,
    )
    return [int(line) for line in output.decode().split()]


def pid_in_tasklist(pid):
    result = subprocess.run(
        [system_tool("tasklist.exe"), "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
        cwd=system_cwd(),
        capture_output=True,
        check=True,
        timeout=10,
    )
    for row in csv.reader(io.StringIO(result.stdout.decode(errors="replace"))):
        if len(row) > 1 and row[1] == str(pid):
            return True
    return False


def kill_windows_processes(image, directory):
    for pid in owned_windows_pids(image, directory):
        if pid_in_tasklist(pid):
            result = subprocess.run(
                [system_tool("taskkill.exe"), "/F", "/PID", str(pid)],
                cwd=system_cwd(),
                capture_output=True,
                timeout=10,
            )
            # A WHPX guest's emulator stays listed while it tears down after
            # a successful taskkill; only a process still listed after the
            # grace period failed to stop.
            deadline = time.monotonic() + 15
            while pid_in_tasklist(pid) and time.monotonic() < deadline:
                time.sleep(0.25)
            if pid_in_tasklist(pid):
                raise RuntimeError(
                    f"taskkill failed for {image} PID {pid}: "
                    f"{result.stdout!r} {result.stderr!r}; files: {directory}"
                )
            print(f"Windows cleanup: {image} PID {pid}; tasklist verified absent", file=sys.stderr)


def stop_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
