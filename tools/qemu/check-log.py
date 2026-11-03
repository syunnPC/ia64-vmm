#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the bare-metal boot contract independently of the guest outcome."""

from pathlib import Path
import sys

text = Path(sys.argv[1]).read_text(errors="replace")
for expected in [
    "AMD64 Limine entry",
    "IA-64 VMM on amd64-limine",
    "host mem ",
    "VMM reservation ",
    "guest region ram ",
    "guest region gop-fb ",
    "TSC frequency=",
    "source=",
    "VMM shutdown ",
]:
    if expected not in text:
        raise SystemExit(f"boot log missing: {expected}")
if "panic:" in text or "AMD64 exception" in text:
    raise SystemExit("boot log contains a host failure")
print("QEMU boot log: banner, memory map, reservation, framebuffer, TSC and shutdown verified")
if len(sys.argv) > 2 and sys.argv[2] == "0":
    guest = "".join(
        line[len("[guest] ") :]
        for line in text.splitlines(keepends=True)
        if line.startswith("[guest] ")
    )
    name = "hello-uefi27.golden"
    golden = Path("tests/efi/hello", name).read_text()
    if golden not in guest:
        raise SystemExit("guest console missing contiguous hello.golden")
    if "EFI ResetSystem type=2 status=0" not in text:
        raise SystemExit("guest did not call ResetSystem(shutdown, EFI_SUCCESS)")
    print("QEMU guest: contiguous hello.golden and EFI_SUCCESS ResetSystem verified")
    # The executor's milestone lines: hello.efi never calls
    # ExitBootServices, so exit is its one milestone.
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests/hosted"))
    import milestones

    try:
        records = milestones.check(text, required=("exit",))
    except milestones.MilestoneError as error:
        raise SystemExit(f"QEMU boot log: milestone lines: {error}")
    print(f"QEMU guest: milestones {milestones.summary(records)}")
