<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# VMM options

Set `key=value` tokens in a Limine entry's `cmdline:`, the QEMU runner's
`--cmdline`, or the USB stick's options file.

## How options are combined

The `options` module (normally `vmm-options.conf`) overrides matching keys
in the boot entry. Its tokens come first; `selftest` tokens from both
sources are kept. With no file tokens, the entry is used unchanged.

The USB safe-mode and diagnostic entries load the file as `options-media`:
the first `vmm_media=` and `guest_media=` tokens are used. Other settings
and malformed bytes outside those tokens are ignored. Discovery loads no options file.

- The file accepts printable ASCII and space, tab, CR and LF separators.
  A token starting with `#` comments out the rest of the line. Invalid bytes
  or multiple options modules (including mixed `options`/`options-media`)
  cause a panic.
- Most keys use their first occurrence. Duplicated `vcpus`, `placement` or
  `pause_sec` cause a panic; `reserve_mib`, `tier0_code_mib` and `nvram_slots`
  use the last occurrence. Exact tokens match anywhere in the line.
- Stick tools limit each source to 511 bytes; the VMM limits the merged
  line to 1023 bytes. An oversized merged line causes a panic.
- Keys are case-sensitive. Unknown tokens, including misspellings, are
  silently ignored. Exact-token options ignore other values. The merged
  line is logged when an options file is present.

In the tables, a **setup error** prevents the guest from starting; a
**panic** stops the VMM. Decimal values have no sign or suffix. Hex values
use lowercase digits without `0x` unless stated otherwise.

## Execution and engines

| Token | Values | Default | Meaning |
|---|---|---|---|
| `engine` | `interp`, `tier0`, `tier1`, `lockstep` | `interp` | Execution engine. `tier1` requires `code_validity` other than `none` and, with `vcpus>1`, `helpers=on`. `engine=tier1` raises `reserve_mib` as described below. Other values are a setup error. |
| `cpu_model` | `montecito`, `madison` | `montecito` | Guest CPU identity and topology. `madison` has one core per socket; `montecito` has two when `vcpus>1`. Other values are a setup error. |
| `code_validity` | `compare-always`, `epoch-compare`, `none`, `hw-dirty` | `compare-always` | How translated code is checked against guest memory that changed. `compare-always` compares the source bundles once per stay on their pages. `epoch-compare` compares only after a storage-interrupt epoch bump, so DMA completed by polling or by other devices is not detected. `none` does no comparison; it is a setup error with `vcpus>1` and with `engine=tier1`. `hw-dirty` is accepted but currently falls back to `compare-always` with a warning. Other values are a setup error. |
| `tier1_hot` | decimal 1..268435455 | `64` | Tier0 entries before compiling a loop head. Other region heads use 16 times this threshold. |
| `tier1_slice` | decimal 1..4294967296 | `16384` | Maximum instructions per Tier1 entry. |
| `tier1_regions` | decimal 1..1048575 | sized from the Tier1 pool | Number of Tier1 region records. |
| `tier1_min_insns` | decimal 1..256 | `16` | Minimum instructions per entry for a Tier1 region to stay promoted. |
| `tier1_verify` | `passes`, `final` | `passes` | Verify IR after each optimizer pass, or only after optimization. |
| `tier1_chain` | `on`, `off` | `on` | `off` limits each Tier1 entry to one region. |
| `fp_host` | `0`, `1` | `1` | Enable host FP helpers where operands, precision, results and exception flags match IA-64. `0` selects soft-float arithmetic in every engine and disables direct expansions. Other values are a setup error. |
| `fp_experimental` | `0`, `1` | `0` | Explicitly enable direct Tier1 FP expansions with incomplete guest exception proofs. FP modes describe the proof boundary. Other values are a setup error. |
| `fp_approx` | `0`, `1` | `0` | With `fp_experimental=1`, allow reduced FP precision and double rounding; numeric results and inexact status can differ. Without experimental mode it has no effect. Other values are a setup error. |
| `fp_check` | `0`, `1` | `0` | Compare accepted host FP helper results and FPSR against soft-float; a mismatch stops execution. Direct expansions are not checked by this option. Other values are a setup error. |
| `fp_stats` | `0`, `1` | `0` | Print per-vCPU host FP helper outcome counts. Direct expansions bypass these counters. Other values are a setup error. |

Tier1 options (`tier1_*`, `hostwin_check`) are read only when Tier1 is
configured: `engine=tier1`, or `engine=lockstep` with `tier1=on`. An invalid
value is a setup error. Tier1 sizes are in the Memory section.

## vCPUs and host placement

| Token | Values | Default | Meaning |
|---|---|---|---|
| `vcpus` | decimal 1..256 at parse; 1..32 accepted at guest setup | `1` | Number of guest vCPUs. Each runs on its own host CPU, in addition to the management CPU. Zero or a non-decimal value is a panic; a second `vcpus` token is a panic; a count above 32 is a setup error; too few host CPUs for the chosen `placement` is a panic. `vcpus>1` is a setup error with `code_validity=none`. SMP Tier1 execution requires background compilation (`helpers=off` is a setup error). |
| `placement` | `physical-core`, `logical-thread`, `core` | `physical-core` | Which host CPUs get vCPUs. The management CPU is CPU 0 in every mode. `physical-core`: one vCPU per physical core, and the management CPU's core is not used. `logical-thread`: any logical CPU except the management CPU, so SMT siblings may be used (also usable when the topology cannot be verified). `core`: one vCPU per core, and a sibling thread of the management CPU's core may take that core's vCPU. Other values and a second `placement` token are a panic. P-cores are used before other cores. |
| `helpers` | `on`, `off` | `on` | Whether idle host CPUs run background work (Tier1 compilation, code-cache reclamation). Up to two Tier1 compilers run concurrently. `off` compiles synchronously on the executor. With `on` and no idle helper CPUs, the management CPU runs background work in steps. With Tier1 enabled and `vcpus>1`, `off` is a setup error. Other values are a setup error. |
| `host_mwait` | exact token `host_mwait=off` | MONITOR/MWAIT when usable | `host_mwait=off` disables the MONITOR/MWAIT idle wait on the host CPUs; idle CPUs use `HLT` and are woken by IPI. Without it, MWAIT is used only if CPUID supports it and the boot-time MWAIT self-test passes; otherwise `HLT` is used. Any other value is ignored. |
| `host_isa_mask` | decimal 0..15 | `15` | Disable-only mask for generated code: bit 0 BMI1, bit 1 BMI2, bit 2 LZCNT, bit 3 MOVBE. Features must also exist on every executor CPU. An invalid or empty value is a panic. |

## Memory

| Token | Values | Default | Meaning |
|---|---|---|---|
| `reserve_mib` | decimal 1..16777215 | `128` | Requested VMM memory in MiB, capped at half of known physical RAM and the largest contiguous usable range; placed at the highest suitable address. `engine=tier1`, `selftest=hostwin` and `selftest=hostwin-fatal` raise it to at least `512 + 8*(vcpus-1)` MiB; boot fails if that minimum exceeds either cap. Invalid values cause a panic. |
| `code_snapshot_mib` | decimal, at least 1 | one sixteenth of allocatable VMM memory, clamped to 48..1024 MiB and limited to half the free reserve after other code-cache reservations | Code-validity snapshot budget. Zero, empty or invalid is a setup error. |
| `tier0_code_mib` | decimal 1..1024 | one quarter of allocatable VMM memory (one eighth with Tier1), rounded down to MiB and clamped to 32..1024 MiB | Tier0 code arena size. Allocatable memory excludes the 2 MiB crash log from `reserve_mib`. Used by every engine except `interp`. Invalid values are a setup error. |
| `tier1_code_mib` | decimal 1..1024 | one thirty-second of allocatable VMM memory, rounded down to MiB and clamped to 16..512 MiB | Tier1 code arena size. |

## Guest media and firmware

| Token | Values | Default | Meaning |
|---|---|---|---|
| `vmm_media` | `auto`, `BB:DD.F` (PCI bus/device/function), `hyperv:LUN` (decimal 0..63), `list` | `auto` | Automatically reserves the EHCI, xHCI, NVMe or AHCI controller identified by the UEFI boot path. The whole controller belongs to the VMM; guest media must use another controller. Hyper-V identifies its synthetic SCSI disk using the boot-loaded media identity. An explicit address overrides automatic selection. `list` prints devices and halts. Missing, unsupported, invalid or ambiguous selections stop boot. See device assignment. |
| `guest_media` | `auto`, `hyperv:LUN` (decimal 0..63) | `auto` | Hyper-V exposes up to 32 remaining media on its single SCSI controller as guest AHCI ports, ordered by SCSI address: 512/4096-byte disks and read-only 2048-byte DVDs. An explicit LUN restricts this to one medium; VMM media is excluded. |
| `iommu` | `auto`, `off` | `auto` | Use an available IOMMU for DMA protection. `off` disables DMA and interrupt remapping after firmware handoff, without requiring ACS. If an available remapper cannot be used, boot stops with configuration guidance. Other values cause a panic. |
| `bootmenu` | `0`, `1` | `0` | `1`: the firmware offers its boot menu and waits a few seconds for a choice before it boots the first option; otherwise it boots at once. Any other value counts as `0`. |

## Display

All `vbios_*` options below are checked even with `display=gop`; an invalid
value is a setup error.

| Token | Values | Default | Meaning |
|---|---|---|---|
| `display` | `gop`, `vbios` | `gop` | `gop`: use the inherited framebuffer. `vbios`: run the GPU's legacy VBIOS through AMD-V or VT-x, then give the guest its legacy VGA display. Missing prerequisites or failed initialization stop boot; there is no GOP fallback. Other values are a setup error. |
| `vbios_image` | `rom`, `host` | `rom` if the card has an expansion ROM BAR, else `host` | Image to run: the card's ROM BAR, or the host's legacy image at C0000 (PCI or classic option ROM). `rom` with `vbios_call=mode` is a setup error. Other values are a setup error. |
| `vbios_call` | `post`, `mode` | `post` for a ROM BAR or intact host ROM with an unset INT10 vector; otherwise `mode` | `post`: initialise the card, then set the mode. `mode`: set the mode on the live state only. Other values are a setup error. |
| `vbios_reset` | `sbr` | off | Secondary-bus-reset the GPU before running the VBIOS. Combining it with `vbios_call=mode` is a setup error. Other values are a setup error. |
| `vbios_post_ms` | decimal 1..600000 (milliseconds) | `30000`; `120000` when `vbios_trace_io` is set and this key is absent | Time bound of the VBIOS POST call. |
| `vbios_mode_ms` | decimal 1..600000 (milliseconds) | `10000` | Time bound of the VBIOS mode-set call. |

## Debug output and logging

| Token | Values | Default | Meaning |
|---|---|---|---|
| `debug_serial` | `off`, `device`, `console` | `off` | Debug serial port for the guest. `device` adds a plain serial device (PNP0501). `console` also makes it the guest's firmware console serial port. Any other value is a setup error. |
| `debug_screen` | `0`, `1` | `0` | `1` also shows the guest's serial output on the VMM's screen console. It has no effect while the guest owns a legacy VGA display. Any other value is a setup error. |

## Run control and timeouts

| Token | Values | Default | Meaning |
|---|---|---|---|
| `exit` | exact tokens `exit=halt`, `exit=isa-debug-exit` | ACPI power-off | `exit=halt`: after the run the VMM prints its final summary and halts instead of powering off or resetting. `exit=isa-debug-exit`: the VMM writes the exit code to I/O port 0xF4 (dword) before halting, for a QEMU `isa-debug-exit` device; ignored when `exit=halt` is also present. Any other value is ignored. |
| `pause_sec` | decimal 0..3600 (seconds) | `0` | Pause this many seconds at each boot checkpoint (inventory, self-tests, VBIOS result, guest exit) so the screen can be read; a non-zero value also prints the final summary on exit. A second `pause_sec` token, an empty value or a value above 3600 is a panic. |
| `timeout_sec` | decimal (seconds) | no limit (`0`) | Wall-clock limit for the guest run, counted from the first guest boot across restarts. On expiry the VMM logs the vCPU state and exits with its timeout code. Empty or non-decimal is a setup error. |
| `halt_retry_us` | decimal 0..1000000 (microseconds) | `1000` | Before ExitBootServices, vCPU 0 retries `PAL_HALT_LIGHT` after this interval instead of waiting only for an interrupt or timer. `0` disables the retry. Other values are a setup error. |
| `handoff_steps` | decimal, `0` to about 10^12 | `0` | Opt-in loader diagnostics. A positive value records pre-exit execution pages and bounds single-step observation after ExitBootServices up to the kernel entry. `0` skips recording and observation. At the bound, the VMM logs a warning and resumes normal execution budgets. Invalid values are a setup error. |
| `purge_panic_ms` | decimal 0..4294967295 (milliseconds) | `30000` | With `vcpus>1`, maximum wait for a global TLB purge acknowledgement or a `sync.i` wait before the VMM panics. `0` waits without a limit. Invalid values are a setup error. |

## Persistence

These options need the stick's persistence data and a `vmm_media` controller
that is xHCI, EHCI, NVMe or AHCI.

| Token | Values | Default | Meaning |
|---|---|---|---|
| `media_log` | `1` | off | `media_log=1` writes the VMM log to the persistence area of the `vmm_media` controller. If the backend or the persistence manifest is missing or invalid, an error is logged and only the RAM log is kept. Any other value is off. |
| `nvram_slots` | `on`, `off`, `fresh` | `on` | Requires an `nvram` module. `on` loads this host's newest record and saves variables at the end of the run; `off` neither loads nor saves; `fresh` starts empty and saves at the end. Other values are ignored. |

## Diagnostics and tests

These options are for bring-up, debugging and tests. They are not needed for
normal use.

| Token | Values | Default | Meaning |
|---|---|---|---|
| `break_ip` | `HEX[:N][+HEX[:N]...]`, at most 4 entries; `HEX` up to 16 hex digits, `N` decimal 1..9999 | off; `N` defaults to `1` | Logs the vCPU state the first `N` times slot 0 of the bundle at `HEX` is about to execute; the run goes on. The interpreter checks every instruction; Tier0 checks only at region entries. Invalid syntax is a setup error. |
| `break_mem` | hex guest physical address, up to 16 digits | off | Adds the 8 bytes at this guest physical address to each `break_ip` log line. No effect without `break_ip`. |
| `dump_va` | `HEXVA:BYTES[,...]`; `BYTES` decimal at most 65536 | off | At guest exit, prints guest memory through the BSP's translations. Bad ranges are skipped with a warning. |
| `guest_ram_fill` | one or two lowercase hex digits (a byte) | off | Before the firmware is loaded, fills all guest RAM with this byte. Other values are a setup error. |
| `hostwin_check` | `0`, `1` | `0` | Enables the host-window MMU-log completeness check. `1` is not available in the VMM image (setup error); it is for unit tests. Read only when Tier1 is active. |
| `hwb_fatal_outside` | exact token `hwb_fatal_outside=1` | off | With `selftest=hostwin-fatal`: the final deliberate fault is taken outside all host-window reservations (at address 0x1000) instead of inside one. |
| `intx_latency` | decimal 16..255 (vector) | off | Measures, per vCPU, the time from a host INTx interrupt to the guest's entry for this vector, and logs samples, mean and maximum at exit. Out-of-range or invalid values are silently ignored. |
| `irq_latency` | decimal 16..255 (vector) | off | Measures, per vCPU, the time from injection of this vector to the guest's entry for it, and logs samples, mean, minimum and maximum at exit. Out-of-range or invalid values are silently ignored. |
| `log_faults` | decimal 0..9999 | off | Logs the first N delivered guest faults other than routine ones (TLB and VHPT misses, access and dirty bit faults, unaligned references with PSR.it=0). Other values are a setup error. |
| `port_trace` | `N[:FIRST-LAST]`; `N` decimal at most 9999, ports hex at most 4 digits, `FIRST<=LAST` | off; ports default to all | Logs the first N legacy I/O port accesses in the port range made by OS code (not firmware or loaders), whether forwarded to a guest-owned host port or denied. Invalid syntax is a setup error. |
| `profile_hz` | decimal 1..100000 | off | Starts a statistical profile of the host instruction pointers sampled at this rate, dumped to the log every `profile_sec` seconds and at exit. Invalid values are a setup error. |
| `profile_sec` | decimal 1..86400 (seconds) | `30` | Dump interval of the profile. Used only with `profile_hz`. Invalid values are a setup error. |
| `sample_sec` | `off`, decimal 1..3600 (seconds) | `off` | Logs one progress line per vCPU every N seconds (position, privilege, retired instructions, idle time, faults, and the VMM reserve's use and peak). `off` disables sampling; zero or invalid is a setup error. |
| `selftest` | repeatable; exact tokens `selftest=timer`, `selftest=simd`, `selftest=host`, `selftest=hostwin`, `selftest=hostwin-fatal`, `selftest=ap-ud`, `selftest=pf`, `selftest=ud`, `selftest=gp` | none | Host self-tests run before guest execution. `timer`: per-CPU timer test. `simd`: per-CPU SIMD initialization and interrupt-preservation checks; with `hostwin`, also checks native guest state and page-fault preservation. `host`: host memory test. `hostwin`: host-window test; needs `vcpus>=2` (panic otherwise), raises `reserve_mib` as above, and shuts down with success when it passes instead of starting the guest. `hostwin-fatal`: as `hostwin`, then a deliberate fatal host fault that must stop the VMM (see `hwb_fatal_outside`). `ap-ud`: an undefined-opcode exception on a vCPU host CPU must reach the management CPU. `pf`, `ud`, `gp`: deliberately raise a page fault, undefined opcode or general-protection fault on the management CPU. Unknown values are ignored. |
| `smp_serialize` | `0`, `1` | `0` | `1` serialises guest execution with a VM-wide lock so that only one vCPU runs at a time. Other values are a setup error. |
| `stop_fault` | decimal 0..9999 (value of the internal fault-kind enumeration, `ia64/state.h`) | `0` (off) | Stops the run at the first delivery of this fault kind that `log_faults` would log, and logs the interruption state. Other values are a setup error. |
| `tier1` | `on`, `off` | off | Only with `engine=lockstep`: `on` runs Tier1 with every execution checked. Requires a build with Tier1 checking enabled (setup error otherwise). SMP checking replays ordered memory accesses. Any other use is a setup error. |
| `tier1_check_after` | decimal 0..2^60 | `0` | Retired-instruction count after which sampled Tier1 replay checking starts. Requires `tier1_check_every`. Requires a build with Tier1 checking enabled (setup error otherwise). |
| `tier1_check_every` | decimal 0..4294967295 | `0` (off) | Sampled Tier1 replay: checks one in every N eligible executions of each region across its vCPUs. Requires a build with Tier1 checking enabled (setup error otherwise). SMP checking replays ordered memory accesses. |
| `trace` | decimal 0..65536 | off | Keeps a ring of the last N executed instruction attempts per vCPU, printed with the vCPU state dump. Empty, non-decimal or above 65536 is a setup error. |
| `vbios_selftest` | `cli-loop`, `guest-stub`, `guest-trace` | off | Needs `display=vbios`. `cli-loop`: tests the time bound with a `cli; jmp $` loop, then stops without booting a guest. `guest-stub`: tests the guest's `int 1Ah` and `int 13h` stub. `guest-trace`: tests the guest display trace; needs `vbios_trace_guest=1`. Other values are a setup error. |
| `vbios_text_hold` | decimal 1..600 (seconds), optionally followed by `,keep` | off | With `vbios_call=mode`: shows a VGA text test page for N seconds, restores the GOP mode for diagnostics, then stops without booting a guest. With `,keep` text mode stays and boot continues. Ignored with a warning for `vbios_call=post`. |
| `vbios_trace_guest` | `0`, `1` | `0` | `1` counts and logs the guest's accesses to the card's legacy VGA memory and ports and its BIOS interrupt calls. Requires `display=vbios` (setup error otherwise). Other values are a setup error. |
| `vbios_trace_io` | decimal 1..1024 | off | Every access to the card's I/O BAR during the VBIOS run exits to the VMM, is forwarded unchanged, and is recorded (up to N events). Raises the default `vbios_post_ms`. Other values are a setup error. |
| `vga_probe` | decimal 0..3600 (seconds) | off (`0` is off) | The firmware enters VGA text mode on the assigned display early, holds a test page for N seconds (for a photograph), then restores the GOP state. Other values are a setup error. |
