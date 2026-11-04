<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Floating-point execution

The default uses host SSE2 and, when available, FMA3 through
`ia64/hostfp.c`. Each helper reads the host exception flags before accepting
its result. Unsupported operands, precision, exceptions and traps fall back
to the existing IA-64 soft-float implementation. Tier1 also lowers
`getf.s/.d`, `setf.s/.d`, FP merges and ordinary single/double/integer FP
loads and stores to native bit operations and guarded memory accesses.

## Modes

Set these in VMM options, or use the corresponding rows
in Configure VMM Options on the USB stick.

| Option | Default | Effect |
| --- | --- | --- |
| `fp_host` | `1` | Enable checked host arithmetic helpers. Set to `0` for soft-float; native bit transfers remain available. |
| `fp_experimental` | `0` | Enable direct Tier1 arithmetic expansions whose guest exception proofs are incomplete. |
| `fp_approx` | `0` | With experimental mode, also allow reduced precision and double rounding. Without experimental mode this has no effect. |
| `fp_check` | `0` | Check accepted host helper results against soft-float; a mismatch stops execution. This does not check direct expansions. |
| `fp_stats` | `0` | Print per-vCPU helper outcome counts. Direct expansions bypass these counters. |

Experimental mode alone preserves the requested precision. Approximation
requires both `fp_experimental=1 fp_approx=1`; numeric results and FPSR
inexact status can differ for single/double operations. Requested 64-bit
significand precision retains IA-64 operand precision and fused rounding,
including in approximate mode: integer division and remainder algorithms
depend on it. These operations use checked helpers or soft-float.
Safe mode and diagnostics
ignore saved FP settings and retain only saved media selectors.

## Helper acceptance

Operands must be canonical zeros or normals exactly representable in the
host format. Scalar precision is single or double, or the corresponding
FPSR precision; 64-bit significand precision accepts only exact host results.
A fused product uses FMA3 or a separate multiply proven exact by its
significand and exponent bounds. Host rounding follows the selected FPSR
rounding mode. NaNs, infinities, NaTVal, unnormalized operands, denormal
results and any host exception other than inexact use soft-float. An inexact
result also requires its guest trap to be disabled. Packed operations apply
these conditions to both single-precision lanes.

The architecture rules are Intel Itanium SDM Rev. 2.3, Vol. 1, sections
5.2–5.4 and Tables 5-2–5-6; host MXCSR behavior follows Intel SDM Vol. 1,
sections 4.8–4.9 and 10.2.3. Builds without SIMD in C use soft-float.

## Experimental proof boundary

`engine/tier1/rules/fpfast.rules` describes the direct expansions.
`tests/tier1/rules/prove.py` verifies their encoding, operand/result mapping
and guards against a conditional IEEE arithmetic model. Its status theorem
assumes that no exception except inexact occurs; it does not derive that
assumption from the emitted guards. The emitter does not read MXCSR flags.
Consequently these proofs do not establish complete guest exception
correctness. Differential and rational arithmetic tests cover sampled
cases, including every generated expansion, and do not replace that proof.

## Validation and benchmarks

```sh
make -j4 test-ia64 test-tier1
make test-tier1-rules-check
make fuzz-tier1-fp T1_FUZZ_SEEDS=1000
make fpsuite build/bench-jobdump
python3 -I tools/bench/bench_engines.py prepare --suite fpsuite
python3 -I tools/bench/bench_engines.py run --suite fpsuite --fp-modes soft,exact,experimental,approx --out build/bench/fp.json
```

The benchmark modes select soft-float, default helpers, direct experimental
expansions, and experimental approximation respectively. `tools/bench/fp_error.py`
compares kernel output words, FPSR and approximation error against soft-float.
