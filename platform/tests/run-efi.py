#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Interrupt fixture: existing runner plus private AHCI/NVMe scratch disks.

Call with the ordinary runner arguments, --firmware new --no-build.
The runner still owns staging, deadlines, accelerator choice and cleanup.
"""

from pathlib import Path
import os
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from qemu import run
from qemu.windows import copy_writable, windows_path
from vmware import backend

scratch = ROOT / "build/tests/irq-scratch.img"
scratch.parent.mkdir(parents=True, exist_ok=True)
if not scratch.exists():
    with scratch.open("wb") as stream:
        stream.truncate(16 * 1024 * 1024)

original_storage = run.storage_arguments
original_create_vm = backend.create_vm


def storage(args, temp, windows):
    result = original_storage(args, temp, windows)
    path = windows_path if windows else str
    for name in ["irq-ahci", "irq-nvme"]:
        copy_writable(scratch, temp / (name + ".img"))
        result += ["-drive", f"if=none,id={name},format=raw,file={path(temp / (name + '.img'))}"]
    result += [
        "-device",
        "ide-hd,drive=irq-ahci,bus=ide.0",
        "-device",
        "nvme,drive=irq-nvme,serial=IRQTEST,addr=05.0",
    ]
    return result


def create_vm(root, temp, *args, **kwargs):
    vmx = original_create_vm(root, temp, *args, **kwargs)
    settings = vmx.read_text()
    for name, controller in [("irq-ahci", "sata0:1"), ("irq-nvme", "nvme0:0")]:
        copy_writable(scratch, temp / (name + ".img"))
        sectors = scratch.stat().st_size // 512
        (temp / (name + ".vmdk")).write_text(
            "# Disk DescriptorFile\nversion=1\nCID=fffffffe\nparentCID=ffffffff\n"
            'createType="monolithicFlat"\n\n'
            f'RW {sectors} FLAT "{name}.img" 0\n\nddb.adapterType = "lsilogic"\n'
        )
        settings += f'{controller}.present = "TRUE"\n{controller}.fileName = "{name}.vmdk"\n{controller}.deviceType = "disk"\n'
    settings += 'pciBridge4.present = "TRUE"\npciBridge4.virtualDev = "pcieRootPort"\npciBridge4.functions = "8"\npciBridge4.pciSlotNumber = "21"\n'
    settings += 'nvme0.present = "TRUE"\nnvme0.pciSlotNumber = "160"\n'
    vmx.write_text(settings)
    return vmx


smp = "--smp" in sys.argv
if smp:
    sys.argv.remove("--smp")
os.environ["BOOT_APP"] = "interrupts-smp" if smp else "interrupts"
run.storage_arguments = storage
backend.create_vm = create_vm
try:
    status = run.main()
except (OSError, RuntimeError) as error:
    print(f"interrupt runner: {error}", file=sys.stderr)
    status = 1
if not status:
    log_arg = sys.argv[sys.argv.index("--log") + 1] if "--log" in sys.argv else "build/qemu.log"
    log = Path(log_arg).read_text(errors="replace")
    if smp and "SMP MSI MSI-X INTx target vCPU1 PASS" not in log:
        print("SMP missing target-vCPU1 acceptance marker", file=sys.stderr)
        status = 1
    for marker in [
        "PIB self IPI",
        "AHCI MSI",
        "AHCI INTx",
        "IPI ITC ticks mean",
        "NVMe MSI-X",
        "interrupts PASS",
    ]:
        if marker not in log:
            print("interrupts: missing acceptance marker: " + marker, file=sys.stderr)
            status = 1
sys.exit(status)
