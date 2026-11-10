<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Architecture

The VMM is a freestanding C11 AMD64 program, loaded by Limine through UEFI.
It executes IA-64 firmware and operating systems and assigns host devices
to the guest.

## Host and memory

One host CPU manages logging, display, persistence and global requests.
Each vCPU runs on a separate host CPU; additional CPUs can compile code
in the background. Guest RAM uses host physical addresses. The VMM reserves
memory for itself and excludes that memory from the guest map.
The reservation cannot exceed half the known physical RAM or the largest
contiguous usable range. Executable host pages use separate writable and
executable aliases.

Host code requires SSE4.2, POPCNT, AVX and XSAVE. Boot and interrupt paths
use general registers only. Native x86 guests have separate SIMD state.

## Execution

All engines share the instruction semantics in `ia64/`. Unsupported
instructions stop the vCPU with architectural state for diagnosis.

- Interpreter: executes decoded IA-64 bundles.
- Tier0: translates straight-line regions to AMD64, with interpreter fallback.
- Tier1: optimizes hot regions using SSA, retaining NaT bits, predicates,
  floating-point status, ordering and precise faults. Host page-table
  windows map ordinary guest RAM; device accesses return to the runtime.
- Lockstep: compares translated execution with the interpreter.

IA-32 execution on Madison uses a separate VMX/SVM container on the vCPU's
host CPU. The dispatcher checks original instruction bytes and IA-64
translations before each bounded native step. A private instruction buffer
separates instruction and data mappings. Data leaves expose authorized guest
RAM or private buffers for device operands; device reads survive pre-instruction
host exits without repetition, and writes commit only after proven execution.
Scalar device RMW and string iterations use synchronous assists. Code writes
participate in the shared code validator. JMPE, sensitive instructions,
segment and flag changes, FP context transfers and virtual I/O return through
the IA-64 state and interruption machinery. Host interrupts
and deadlines preserve IA-32 restart state without changing instruction sets.
Shared segment checks enforce Itanium runtime integrity in every IA-32 mode.
Instruction encodings and required host features are checked before device
operands are consumed. INS validates and freezes its destination mapping
before reading the port, including when the destination is MMIO.
The native container shares host virtualization ownership with VBIOS and
restores host floating-point and debug state before servicing host events.

Instruction and interruption behavior follows Intel's
[Itanium SDM, Volume 2, chapters 9–10](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf).

Translated code is validated against guest bytes before reuse because
assigned devices can modify RAM through DMA. Tier1 instruction-selection
rules have offline z3 proofs. Direct floating-point expansions are
experimental and disabled by default because guest exception proofs are
incomplete. Supported host FP helpers fall back to software arithmetic.

A VM supports 1–32 vCPUs. Interrupt mailboxes and host IPIs notify
executors. Global TLB purges complete on every vCPU before the issuer
continues. Translation caches are shared; code admission is per vCPU.

## Devices and firmware

The host PCI hierarchy is inventoried at boot. The VMM reserves its boot
medium's controller and assigns other devices. PCI configuration is
mediated; assigned BARs retain host addresses. Host INTx, MSI and MSI-X
interrupts become guest IOSAPIC or local SAPIC interrupts.
Supported VT-d and AMD-Vi configurations restrict DMA and remap interrupts;
unsupported or disabled remappers can leave direct routing and identity DMA.

Native IA-64 PAL/SAL and EDK2 drivers provide guest firmware services.
The display uses the UEFI GOP framebuffer or legacy VGA. Optional VBIOS
execution requires AMD-V or VT-x. A polling 16550 UART provides diagnostics.

Guest reset restarts assigned devices and firmware within the VMM, retaining
RAM and EFI variables. EHCI, xHCI, NVMe or AHCI drivers can save logs and
EFI variables to preallocated files on the VMM boot medium.
