<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Testing

Most tests link VMM modules against `tests/hal/hal_linux.c` and run on
Linux; QEMU tests boot the VMM image. Run make from the repository root
with the build tools. Use `-j4` or less on a shared machine
and run QEMU tests one guest at a time with
`QEMU_APPS_ARGS='--jobs 1'`.

## Main targets

| Command | What it runs | Needs |
| --- | --- | --- |
| `make -j4 test QEMU_APPS_ARGS='--jobs 1'` | Unit, IA-64 instruction, Tier0, firmware, platform tests; QEMU smoke and application tests | AMD64 GCC, IA-64 cross toolchain, Python, `iasl`, MinGW-w64, `IA64_FORK_DIR` (QEMU's recorded q35 and pc ACPI tables), a QEMU or VMware backend |
| `make -j4 test-tier1` | Tier1 compiler, optimizer, allocator, host-window, region and rule-table tests; random-program smoke test | AMD64 GCC, IA-64 cross toolchain, Python |
| `make -j4 check-freestanding` | The VMM's objects use no hosted library symbols | |
| `make test-hyperv` | SynIC, VMBus, synthetic SCSI/keyboard, media selection and guest AHCI/ATAPI/i8042 protocols | AMD64 GCC |

`make test-host`, `make test-ia64` and `make test-qemu` run parts of
`make test`. `SAN=1` builds the unit tests with ASan and UBSan.

## QEMU application tests

`tests/qemu/apps.py` boots IA-64 applications from `tests/efi/` with the native
firmware and checks log markers, statistics and shutdown status.
`make test-qemu-apps-full QEMU_APPS_ARGS='--jobs 1'` adds longer SMP,
CPU-model, interrupt, PAL/SAL, FPSWA, UART and firmware cases.
List or select cases with `python3 -I tests/qemu/apps.py --full --list` and
`--only NAME`.

## Guest media

`make test-loaders` boots the installation media of the guest operating
systems (guests.md) on assigned controllers and checks the
boot menus on screen, consumed key presses, ExitBootServices and the entry
into the media's kernel or loader. The images are not part of the tree:

- `IA64_IMAGES_DIR` is the directory holding them.
- `IA64_MEDIA_MANIFEST` names the JSON file describing images and expected
  boot milestones; see `tests/loaders/media.example.json` for the format.

```sh
make test-loaders LOADER_ARGS='--target gentoo --engine tier0 --scenario scripted'
python3 tests/loaders/run.py --help
```

`make test-persist-qemu` checks persistent logs and EFI variables under
QEMU on each backend: USB behind xHCI and EHCI, NVMe and AHCI.

## Differential and random tests

| Command | What it compares | Needs |
| --- | --- | --- |
| `make test-ia64-oracle` | IA-64 instruction tests against the qemu-system-ia64 fork | `QEMU_IA64` |
| `make fuzz-ia64 SEEDS=N [SEED0=S FUZZ_JOBS=J FUZZ_ARGS=...]` | Random IA-64 programs on the interpreter and on the fork, compared bundle by bundle; divergences are minimized into `tests/fuzz/repro/` | `QEMU_IA64` |
| `make fuzz-tier1 [T1_FUZZ_SEEDS=N]` | Random loops on every engine against a stepping reference | |
| `make fpaudit` | Floating-point arithmetic against an exact Python model, and the SDM divide and square-root sequences | |
| `make test-tier1-rules T1_Z3_VENV=DIR` | Proves instruction-selection rule semantics; `test-tier1-rules-check` also verifies the IEEE model | z3-solver in the venv (`tests/tier1/rules/requirements.txt`) |

`python3 tests/fuzz/fuzz.py --help` lists the generator's options (virtual
memory, floating point, big-endian and extra exception-boundary cases).

`make test-audit` runs the bounded concurrent-memory harness in
`tests/audit/repro/tear.c`. Other reproducers there are guest assembly:
assemble with the IA-64 cross toolchain, link at `0x100000`, convert to
raw binary, and pass the image to `build/ia64-test`.

## Hardware-virtualization and real-hardware checks

`make test-hostwin-qemu` and `make test-hostwin-vmware` run the host-window
self-test of the bare-metal Tier1 backend in a virtual machine.
`make test-real-prep` checks controller discovery, IOMMU takeover and clock
calibration against synthetic tables and builds test USB images; it is part
of `make test`. `make test-vtd` independently walks DMA mappings and checks
denied private-memory holes, DRHD/RMRR ownership, unsupported features,
table visibility and register-command completion/failure. It is also part
of `make test`; these synthetic checks do not qualify physical routing.
`make test-amdvi` checks architectural DTE/page-table permissions, denied
unknown sources, IVHD aliases/ranges/alternate encodings, IVMD constraints and
command/event-ring activation failures. It also independently decodes basic and
x2APIC interrupt entries, checks MSI blocks and IOAPIC sources across units,
and exercises update/revoke draining and completion failures.
`make test-vtd-irq` checks source IDs,
interrupt-table publication and queued-invalidation completion. Both are part
of `make test`. QEMU runs with `--iommu` enable DMA and interrupt remapping.
Physical machines are tested with the USB boot kit
(real-hardware.md).

## Benchmarks

`make bench-engines` measures the interpreter, Tier0, Tier1 and the
combined Tier0+Tier1 path in the Linux test harness, including hosted
verification overhead, and compares them with QEMU's IA-64 TCG. It uses
assembly kernels and a compiled C suite, writing tables to `build/bench/`.
`make bench-tier1` and `make bench-ia64` time the Linux test builds.
Measure on an idle machine and compare runs made the same way.
