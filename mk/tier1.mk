# SPDX-License-Identifier: GPL-2.0-or-later
# Tier1 compiler and host-MMU windows: build glue.
#
# Tier1 and host windows are linked into the VMM; Linux tests use a hosted backend.
TIER1_STATS ?= 0
CPPFLAGS += -DIA64VMM_TIER1_STATS=$(TIER1_STATS)
# Diagnostic product build: replay Tier1 regions on the interpreter.
ifeq ($(TIER1_CHECKED),1)
CPPFLAGS += -DIA64VMM_TIER1_CHECKED=1
endif

T1_SOURCES := $(wildcard engine/tier1/*.c engine/hostwin/*.c)
# The module interfaces; ir_ops.def is compiled
# through rules.h and validated by rulelib.py.
T1_RULES_DIR := engine/tier1/rules
T1_PROVER_DIR := tests/tier1/rules
T1_RULE_INPUTS := $(T1_RULES_DIR)/isel.rules \
	$(T1_RULES_DIR)/fpfast.rules engine/tier1/x86sse.h \
	$(T1_RULES_DIR)/rulelib.py $(T1_RULES_DIR)/gen.py engine/tier1/ir_ops.def \
	engine/tier1/x86enc.h $(wildcard $(T1_PROVER_DIR)/*.py) $(T1_PROVER_DIR)/requirements.txt

# Harness and suites.
T1_HARNESS_SOURCES := tests/tier1/harness.c tests/tier1/snapshot.c tests/tier1/storelog.c \
	tests/tier1/schedule.c tests/tier1/reference.c tests/tier1/engines.c tests/tier1/results.c \
	tests/tier1/observe.c tests/tier1/programs.c tests/tier1/program_tests.c \
	tests/tier1/dispatch_tests.c tests/tier1/harness_tests.c tests/tier1/bench.c \
	tests/tier1/provider_real.c tests/tier1/provider_double.c tests/tier1/ledger.c \
	tests/tier1/restore_tests.c
# tests/tier1 (except the fuzz/rule runners), Tier1, hostwin.
T1_TEST_SOURCES := $(T1_SOURCES) tests/tier1/main.c tests/tier1/compare.c tests/tier1/compare_test.c \
	tests/tier1/unit_rules.c $(T1_HARNESS_SOURCES) \
	$(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c
# The hosted window backend (window) for the harness.
T1_TEST_SOURCES += tests/tier1/hostwin_linux.c
# Unit tests per file, the IR evaluator and its harness.
T1_TEST_SOURCES += tests/tier1/ia64_enc.c tests/tier1/um_machine.c tests/tier1/um_random.c tests/tier1/ir_eval.c \
	tests/tier1/unit_asm.c tests/tier1/unit_ir.c tests/tier1/unit_lift.c tests/tier1/unit_opt.c \
	tests/tier1/unit_eval.c tests/tier1/unit_x86enc.c tests/tier1/fp_tests.c
# Generated code under the real runtime.
T1_TEST_SOURCES += tests/tier1/host_isa.c
T1_TEST_SOURCES += tests/tier1/um_t1.c tests/tier1/unit_codegen.c tests/tier1/unit_runtime.c
# The Tier1 engine of harness's harness (engines.h): every suite that
# iterates the engines runs Tier1 too.
T1_TEST_SOURCES += tests/tier1/engine_tier1.c tests/tier1/unit_programs.c
# (product dispatch): the product-path engine `tier0+tier1`
# and its suite "upper".
T1_TEST_SOURCES += tests/tier1/engine_tier0t1.c tests/tier1/upper_tests.c tests/tier1/memory_tests.c
# store pairing, engine_compile_ticks, checked lockstep.
T1_TEST_SOURCES += tests/tier1/ver_tests.c
# (bundle coherence): suite "coherence" (and coh-bench).
T1_TEST_SOURCES += tests/tier1/coherence_tests.c tests/tier1/coverage_tests.c
# Region chaining: suite "chain".
T1_TEST_SOURCES += tests/tier1/chain_tests.c
T1_TEST_OBJECTS := $(addprefix build/hosted/,$(T1_TEST_SOURCES:.c=.o))
# The speed program is tests/ia64/speed.S as built by mk/ia64.mk.
T1_TEST_IMAGES := $(patsubst tests/tier1/programs/%.S,build/tests/tier1/%.bin,$(wildcard tests/tier1/programs/*.S)) \
	build/tests/ia64/speed.bin
# Own position-independent fragments (tests/tier1/frag_*.S).
T1_TEST_IMAGES += $(patsubst tests/tier1/%.S,build/tests/tier1/frag/%.bin,$(wildcard tests/tier1/frag_*.S))
# (product dispatch): the compiled-C suite (mk/bench.mk) for
# the arena tests of suite "upper".
T1_TEST_IMAGES += $(CSUITE_DIR)/csuite.bin
# The same objects in the shared layout: csuite.lds without the ALIGN(0x10000) before .data, so .bss
# follows .text and the LZ kernel's lz_head starts on the page that holds
# the end of its code (suite "coherence", --suite=coh-lz).
$(CSUITE_DIR)/csuite-shared.lds: tests/bench/csuite/csuite.lds
	@mkdir -p $(@D)
	sed '/ALIGN(0x10000)/d' $< >$@
$(CSUITE_DIR)/csuite-shared.elf: $(CSUITE_DIR)/start.o $(CSUITE_DIR)/csuite.o $(CSUITE_DIR)/csuite-shared.lds
	$(CROSS_IA64)ld --no-warn-rwx-segments -T $(CSUITE_DIR)/csuite-shared.lds --defsym CSUITE_BASE=0x100000 \
		$(CSUITE_DIR)/start.o $(CSUITE_DIR)/csuite.o -o $@
T1_TEST_IMAGES += $(CSUITE_DIR)/csuite-shared.bin
# The hostwin tests: their own binary (process-wide SIGSEGV handler,
# forked children); ia64_translate is wrapped so the tests see every call
# hostwin makes and its reference kind.
T1_HOSTWIN_TEST_SOURCES := tests/tier1/hostwin_test.c tests/tier1/hostwin_linux.c \
	$(wildcard engine/hostwin/*.c) $(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c
T1_HOSTWIN_TEST_OBJECTS := $(addprefix build/hosted/,$(T1_HOSTWIN_TEST_SOURCES:.c=.o))
# Under SAN=1 the previous SIGSEGV handler is ASan's.
T1_ASAN_OPTIONS = $(if $(filter 1,$(SAN)),ASAN_OPTIONS='$(ASAN_OPTIONS):allow_user_segv_handler=1')

.PHONY: test-tier1 test-tier1-rules test-tier1-rules-check

test-tier1:  build/tier1-test build/tier1-hostwin-test $(T1_TEST_IMAGES)
	@mkdir -p build/tests/tier1
	@$(T1_ASAN_OPTIONS) ./build/tier1-test $(T1_TEST_IMAGES) 2>build/tests/tier1/stderr || { cat build/tests/tier1/stderr >&2; exit 1; }
	@$(T1_ASAN_OPTIONS) ./build/tier1-hostwin-test

# baselines: interpreter and Tier0 on the guest loops,
# median of T1_BENCH_RUNS processes (cold = a fresh process), pinned to CPU
# T1_BENCH_CPU when taskset exists.
T1_BENCH_CPU ?= 1
T1_BENCH_RUNS ?= 5
# tier0+tier1: the product path (product dispatch). Benches run
# without the harness's MMU-log completeness check (T1H_HOSTWIN_CHECK=0,
# engine_tier1.c), which the product does not run.
T1_BENCH_ENGINES ?= interp tier0 tier1 tier0+tier1
T1_BENCH_PROGRAMS ?= speed gather_phys gather_virt shladd_xor
T1_BENCH_PIN = $(if $(shell command -v taskset 2>/dev/null),taskset -c $(T1_BENCH_CPU))
.PHONY: bench-tier1
bench-tier1: build/tier1-test $(T1_TEST_IMAGES)
	@mkdir -p build/tests/tier1
	@rm -f build/tests/tier1/bench.txt
	@for e in $(T1_BENCH_ENGINES); do for p in $(T1_BENCH_PROGRAMS); do \
		i=0; while [ $$i -lt $(T1_BENCH_RUNS) ]; do i=$$((i + 1)); \
		T1H_HOSTWIN_CHECK=0 $(T1_BENCH_PIN) ./build/tier1-test --bench=$$e:$$p $(T1_TEST_IMAGES) \
			>>build/tests/tier1/bench.txt || exit 1; \
	done; done; done
	python3 -I tests/tier1/bench_report.py <build/tests/tier1/bench.txt

# Isolated optimizer phase, with synthetic IR and no IA-64 image/toolchain.
T1_FOLD_BENCH_OBJECTS := $(filter-out build/hosted/tests/tier1/main.o,$(T1_TEST_OBJECTS)) \
	build/hosted/tests/tier1/bench_fold.o
build/tier1-fold-bench: $(T1_FOLD_BENCH_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(T1_FOLD_BENCH_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
.PHONY: bench-tier1-fold
bench-tier1-fold: build/tier1-fold-bench
	./build/tier1-fold-bench
-include build/hosted/tests/tier1/bench_fold.d

# Hot entry-value hits, including small indices; no IA-64 images needed.
include tests/tier1/bench_entry.mk

# Sparse loop propagation and a dense I1 control, without guest images.
include tests/tier1/bench_bool_fold.mk

# Known-bits propagation, including the loop fixed-point control.
include tests/tier1/bench_known.mk

# Random differential programs (harness):
# tests/tier1/t1fuzz.py generates v1-subset loops (tests/fuzz/fuzz.py's
# preamble), build/tier1-fuzz-runner compares every engine with the
# stepping reference. fuzz-tier1 runs T1_FUZZ_SEEDS (the full count),
# test-tier1 a smoke of T1_FUZZ_SMOKE.
T1_FUZZ_RUNNER_SOURCES := tests/tier1/fuzz_runner.c tests/tier1/harness.c tests/tier1/snapshot.c \
	tests/tier1/storelog.c tests/tier1/schedule.c tests/tier1/reference.c tests/tier1/engines.c \
	tests/tier1/results.c tests/tier1/observe.c tests/tier1/compare.c \
	tests/tier1/provider_real.c tests/tier1/provider_double.c tests/tier1/engine_tier1.c \
	tests/tier1/engine_tier0t1.c tests/tier1/hostwin_linux.c $(T1_SOURCES) $(IA64_SOURCES) core/log.c core/guest_mem.c \
	tests/hal/hal_linux.c
T1_FUZZ_RUNNER_OBJECTS := $(addprefix build/hosted/,$(T1_FUZZ_RUNNER_SOURCES:.c=.o))
T1_FUZZ_SEEDS ?= 10000
T1_FUZZ_SMOKE ?= 200
T1_FUZZ_SEED0 ?= 1
T1_FUZZ_JOBS ?= 8
T1_FUZZ = @test -n '$(CROSS_IA64)' || { echo '$@: CROSS_IA64 is not set (see local.env.example)' >&2; exit 1; }; \
	CROSS_IA64='$(CROSS_IA64)' $(T1_ASAN_OPTIONS) python3 -I tests/tier1/t1fuzz.py \
	--runner build/tier1-fuzz-runner --seed0 $(T1_FUZZ_SEED0) --jobs $(T1_FUZZ_JOBS)
.PHONY: fuzz-tier1 test-tier1-fuzz-smoke
build/tier1-fuzz-runner: $(T1_FUZZ_RUNNER_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(T1_FUZZ_RUNNER_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
fuzz-tier1: build/tier1-fuzz-runner
	$(T1_FUZZ) --seeds $(T1_FUZZ_SEEDS)
test-tier1-fuzz-smoke: build/tier1-fuzz-runner
	$(T1_FUZZ) --seeds $(T1_FUZZ_SMOKE)
test-tier1: test-tier1-fuzz-smoke
# FP-heavy programs (t1fuzz.py --fp), host FP results checked against the
# soft-float in every engine and the reference (T1H_FP=check).
T1_FUZZ_FP_SMOKE ?= 20
.PHONY: fuzz-tier1-fp test-tier1-fuzz-fp-smoke
fuzz-tier1-fp test-tier1-fuzz-fp-smoke: export T1H_FP = check
fuzz-tier1-fp: build/tier1-fuzz-runner
	$(T1_FUZZ) --fp --seeds $(T1_FUZZ_SEEDS)
test-tier1-fuzz-fp-smoke: build/tier1-fuzz-runner
	$(T1_FUZZ) --fp --seeds $(T1_FUZZ_FP_SMOKE)
test-tier1: test-tier1-fuzz-fp-smoke
-include $(T1_FUZZ_RUNNER_OBJECTS:.o=.d)

# Generated rule table: no z3; parses rule definitions.
build/tier1/isel_rules.h: $(T1_RULE_INPUTS)
	@mkdir -p $(@D)
	python3 -I $(T1_RULES_DIR)/gen.py --out $@
build/hosted/tests/tier1/unit_rules.o: build/tier1/isel_rules.h
# Experimental FP expansion table: its proof covers encoding and conditional semantics.
build/tier1/fpfast_rules.h: $(T1_RULE_INPUTS) engine/tier1/rules.h
	@mkdir -p $(@D)
	python3 -I $(T1_RULES_DIR)/gen.py --fpfast-out $@
build/hosted/engine/tier1/fpfast.o build/bm/engine/tier1/fpfast.o: build/tier1/fpfast_rules.h
# isel.c is the translation unit that instantiates the generated table.
build/hosted/engine/tier1/isel.o build/bm/engine/tier1/isel.o: build/tier1/isel_rules.h

# Proofs need z3 in the venv named by T1_Z3_VENV (local.env.example).
T1_Z3_PYTHON = $(T1_Z3_VENV)/bin/python
T1_REQUIRE_Z3 = @test -n '$(T1_Z3_VENV)' || { echo '$@: T1_Z3_VENV is not set (see local.env.example)' >&2; exit 1; }; \
	test -x '$(T1_Z3_PYTHON)' || { echo '$@: $(T1_Z3_PYTHON) not found (T1_Z3_VENV)' >&2; exit 1; }
# Prove instruction-selection and fold semantics.
test-tier1-rules:
	$(T1_REQUIRE_Z3)
	$(T1_Z3_PYTHON) -I $(T1_PROVER_DIR)/prove.py --selftest
	$(T1_Z3_PYTHON) -I $(T1_PROVER_DIR)/prove.py
	@$(MAKE) --no-print-directory test-tier1-conformance test-tier1-table-conformance
# Re-prove rules without changing build outputs.
test-tier1-rules-check:
	$(T1_REQUIRE_Z3)
	$(T1_Z3_PYTHON) -I $(T1_PROVER_DIR)/fp_ieee_test.py
	$(T1_Z3_PYTHON) -I $(T1_PROVER_DIR)/prove.py --selftest
	$(T1_Z3_PYTHON) -I $(T1_PROVER_DIR)/prove.py

build/tier1-test: $(T1_TEST_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(T1_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -Wl,--wrap=hal_counter -o $@
# Optional instruction-selection phase measurement; no IA-64 images needed.
T1_ISEL_BENCH_OBJECTS := $(filter-out build/hosted/tests/tier1/main.o,$(T1_TEST_OBJECTS)) \
	build/hosted/tests/tier1/bench_isel.o
.PHONY: bench-tier1-isel
bench-tier1-isel: build/tier1-isel-bench
	./build/tier1-isel-bench
build/tier1-isel-bench: $(T1_ISEL_BENCH_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(T1_ISEL_BENCH_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include build/hosted/tests/tier1/bench_isel.d
build/tier1-hostwin-test: $(T1_HOSTWIN_TEST_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(T1_HOSTWIN_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -Wl,--wrap=ia64_translate,--wrap=code_lock -o $@
# Lock order of window fills, with defects.
build/tests/tier1/frag/%.o: tests/tier1/%.S
	@mkdir -p $(@D)
	$(CROSS_IA64)as $< -o $@
build/tests/tier1/frag/%.elf: build/tests/tier1/frag/%.o
	$(CROSS_IA64)ld -Ttext=0x100000 -e _start $< -o $@
build/tests/tier1/frag/%.bin: build/tests/tier1/frag/%.elf
	$(CROSS_IA64)objcopy -O binary $< $@

# Shared pieces (gather body/driver, IVT) are .inc files, never assembled alone.
build/tests/tier1/%.o: tests/tier1/programs/%.S $(wildcard tests/tier1/programs/*.inc)
	@mkdir -p $(@D)
	$(CROSS_IA64)as -I $(<D) $< -o $@
build/tests/tier1/%.elf: build/tests/tier1/%.o
	$(CROSS_IA64)ld -Ttext=0x100000 -e _start $< -o $@
build/tests/tier1/%.bin: build/tests/tier1/%.elf
	$(CROSS_IA64)objcopy -O binary $< $@

-include $(T1_TEST_OBJECTS:.o=.d) $(T1_HOSTWIN_TEST_OBJECTS:.o=.d)

# Conformance of the prover's models.
# The runner executes IA-64 cases through the interpreter's decoder and
# ia64_execute() and x86 snippets natively; conformance.py (z3) generates
# the cases (IA-64 encodings from $(CROSS_IA64)as, x86 from the host as)
# and compares with sem_ia64/sem_amd64. test-tier1 runs the runner's
# smoke test, so it is built and run under SAN=1 too.
T1_RUNNER_SOURCES := tests/tier1/rules/rule_runner.c $(IA64_SOURCES) core/log.c core/guest_mem.c \
	tests/hal/hal_linux.c
T1_RUNNER_OBJECTS := $(addprefix build/hosted/,$(T1_RUNNER_SOURCES:.c=.o))
T1_CONFORMANCE_ARGS ?=
.PHONY: test-tier1-conformance test-tier1-runner
build/tier1-rule-runner: $(T1_RUNNER_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(T1_RUNNER_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
test-tier1-runner: build/tier1-rule-runner
	@$(T1_ASAN_OPTIONS) ./build/tier1-rule-runner selftest
test-tier1: test-tier1-runner
# FPI conformance exports the actual lifter IR through the Tier1 test binary.
test-tier1-conformance: build/tier1-rule-runner build/tier1-test
	$(T1_REQUIRE_Z3)
	@test -n '$(CROSS_IA64)' || { echo '$@: CROSS_IA64 is not set (see local.env.example)' >&2; exit 1; }
	$(T1_ASAN_OPTIONS) $(T1_Z3_PYTHON) -I $(T1_PROVER_DIR)/conformance.py --runner build/tier1-rule-runner \
		--cross '$(CROSS_IA64)' $(T1_CONFORMANCE_ARGS)
build/tier1/isel_rules.h: engine/tier1/rules.h
-include $(T1_RUNNER_OBJECTS:.o=.d)

# ---- Rule table (rules.h format 2) ----
# fold.rules: proved IR -> IR fold rules, generated into
# build/tier1/fold_rules.h (static tables: any translation unit may include
# it). The table runner executes the generated isel table through x86enc
# natively and instantiates the fold table; its selftest runs in
# test-tier1 (also SAN=1), its conformance against sem_ia64 at the end of
build/tier1/isel_rules.h: $(T1_RULES_DIR)/fold.rules
build/tier1/fold_rules.h: $(T1_RULE_INPUTS) $(T1_RULES_DIR)/fold.rules engine/tier1/rules.h
	@mkdir -p $(@D)
	python3 -I $(T1_RULES_DIR)/gen.py --fold-out $@
build/hosted/tests/tier1/unit_rules.o: build/tier1/fold_rules.h
# A parallel clean build must generate the table before opt.c includes it.
build/hosted/engine/tier1/opt.o build/bm/engine/tier1/opt.o: build/tier1/fold_rules.h
T1_TABLE_RUNNER_SOURCES := tests/tier1/rules/table_runner.c engine/tier1/x86enc.c engine/tier1/x86sse.c \
	engine/tier1/fpfast.c
T1_TABLE_RUNNER_OBJECTS := $(addprefix build/hosted/,$(T1_TABLE_RUNNER_SOURCES:.c=.o))
build/hosted/tests/tier1/rules/table_runner.o: build/tier1/isel_rules.h build/tier1/fold_rules.h build/tier1/fpfast_rules.h
build/tier1-table-runner: $(T1_TABLE_RUNNER_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(T1_TABLE_RUNNER_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
T1_TABLE_CONFORMANCE_ARGS ?=
.PHONY: test-tier1-table-runner test-tier1-table-conformance
test-tier1-table-runner: build/tier1-table-runner
	@$(T1_ASAN_OPTIONS) ./build/tier1-table-runner selftest
test-tier1: test-tier1-table-runner
test-tier1-table-conformance: build/tier1-table-runner
	$(T1_REQUIRE_Z3)
	$(T1_ASAN_OPTIONS) $(T1_Z3_PYTHON) -I $(T1_PROVER_DIR)/table_conformance.py \
		--runner build/tier1-table-runner $(T1_TABLE_CONFORMANCE_ARGS)
	$(T1_ASAN_OPTIONS) $(T1_Z3_PYTHON) -I $(T1_PROVER_DIR)/fpx_conformance.py \
		--runner build/tier1-table-runner
-include $(T1_TABLE_RUNNER_OBJECTS:.o=.d)
# ---- Tier1 MMU log: the MMU change log ----
# Writers through the real instruction semantics, replay = full diff,
# children). Own binary; run by test-tier1, also under SAN=1.
T1_MMU_LOG_TEST_SOURCES := tests/tier1/mmu_log_test.c tests/tier1/hostwin_linux.c \
	$(wildcard engine/hostwin/*.c) $(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c
T1_MMU_LOG_TEST_OBJECTS := $(addprefix build/hosted/,$(T1_MMU_LOG_TEST_SOURCES:.c=.o))
build/tier1-mmu-log-test: $(T1_MMU_LOG_TEST_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(T1_MMU_LOG_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
.PHONY: test-tier1-mmu-log
test-tier1-mmu-log: build/tier1-mmu-log-test
	@$(T1_ASAN_OPTIONS) ./build/tier1-mmu-log-test
test-tier1: test-tier1-mmu-log
-include $(T1_MMU_LOG_TEST_OBJECTS:.o=.d)
# Each writer family muted (T1H_MMU_MUTE, tests/tier1/engine_tier1.c) must
# end the programs and the dispatch suite in the completeness check's panic; the
# programs reach every family between two region entries (programs/mmu_log_writers.S; the
# walker and itc.d also through gather-virt). vcpu_reset (family reset) runs
# tier1-mmu-log-test's. Not part of test-tier1 (about 15 suite runs).
T1_MMU_FAMILIES := tc_insert_purge tc_insert insert_purge itc itr ptc_l ptc_g ptc_ga ptc_e \
	ptr_tc ptr_tr rr pkr pkr_dup
.PHONY: test-tier1-mmu-mutation
test-tier1-mmu-mutation: build/tier1-test $(T1_TEST_IMAGES)
	@mkdir -p build/tests/tier1/mutation
	@for f in $(T1_MMU_FAMILIES); do for s in programs dispatch; do \
		out=build/tests/tier1/mutation/$$f.$$s.out; \
		if T1H_MMU_MUTE=$$f $(T1_ASAN_OPTIONS) ./build/tier1-test --suite=$$s $(T1_TEST_IMAGES) >$$out 2>&1; then \
			echo "test-tier1-mmu-mutation: $$f muted, --suite=$$s passed: not detected" >&2; exit 1; fi; \
		grep -q '^\[panic\] hostwin: MMU log check: ' $$out || { tail -5 $$out >&2; exit 1; }; \
		echo "test-tier1-mmu-mutation: $$f/$$s: $$(grep -m1 -o 'MMU log check: .*' $$out)"; \
	done; done
# The same with the fuzz (tests/tier1/t1fuzz.py --mmu-writers): every
# virtual program's loop ends with a writer block; unmuted all must be
# equal, each muted family must fail some programs in the check's panic
# (tc_insert_purge and reset: no fuzz block; see above). Muted runs use one
# runner process per program, so "N failed" counts programs.
T1_MMU_FUZZ_SEEDS ?= 100
T1_MMU_FUZZ_FAMILIES := tc_insert insert_purge itc itr ptc_l ptc_g ptc_ga ptc_e ptr_tc ptr_tr rr pkr pkr_dup
.PHONY: fuzz-tier1-mmu-mutation
fuzz-tier1-mmu-mutation: build/tier1-fuzz-runner
	@mkdir -p build/tests/tier1/mutation
	$(T1_FUZZ) --seeds $(T1_MMU_FUZZ_SEEDS) --virtual-share 1 --mmu-writers random
	@for f in $(T1_MMU_FUZZ_FAMILIES); do out=build/tests/tier1/mutation/fuzz-$$f.out; \
		T1H_MMU_MUTE=$$f CROSS_IA64='$(CROSS_IA64)' $(T1_ASAN_OPTIONS) python3 -I tests/tier1/t1fuzz.py \
			--runner build/tier1-fuzz-runner --seed0 $(T1_FUZZ_SEED0) --jobs $(T1_FUZZ_JOBS) \
			--seeds $(T1_MMU_FUZZ_SEEDS) --virtual-share 1 --mmu-writers random --batch 1 >$$out 2>&1 && \
			{ echo "fuzz-tier1-mmu-mutation: $$f muted: no program failed" >&2; exit 1; }; \
		grep -q 'MMU log check: ' $$out || { tail -5 $$out >&2; exit 1; }; \
		echo "fuzz-tier1-mmu-mutation: $$f: $$(grep -m1 -o '[0-9]* failed' $$out), first: $$(grep -m1 -o 'MMU log check: [^(]*' $$out)"; \
	done

# ---- Mock region and trap client ----
# A mock region on struct t1_exec with the trap client: forced traps
# from the poison page, window-tags at the hostwin level, two instances, and
# teardown at the default vm.max_map_count. Own binary (process-wide
# SIGSEGV client); run by test-tier1, also under SAN=1.
T1_HOSTWIN_REGION_TEST_SOURCES := tests/tier1/hostwin_region_test.c tests/tier1/hostwin_linux.c \
	$(wildcard engine/hostwin/*.c) $(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c
T1_HOSTWIN_REGION_TEST_OBJECTS := $(addprefix build/hosted/,$(T1_HOSTWIN_REGION_TEST_SOURCES:.c=.o))
build/tier1-hostwin-region-test: $(T1_HOSTWIN_REGION_TEST_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(T1_HOSTWIN_REGION_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
.PHONY: test-tier1-hostwin-region
test-tier1-hostwin-region: build/tier1-hostwin-region-test
	@$(T1_ASAN_OPTIONS) ./build/tier1-hostwin-region-test
test-tier1: test-tier1-hostwin-region
-include $(T1_HOSTWIN_REGION_TEST_OBJECTS:.o=.d)





# Region chaining: the broken variants, and the suites
# with chaining off; test-tier1's main run has it on, and every run of either
# equals the stepping reference (the on/off differential).
.PHONY:  test-tier1-chain-off
T1_CHAIN_OFF_SUITES := dispatch upper restore coherence programs checked
test-tier1-chain-off: $(addprefix test-tier1-chain-off-,$(T1_CHAIN_OFF_SUITES))
test-tier1-chain-off-%: build/tier1-test $(T1_TEST_IMAGES)
	@mkdir -p build/tests/tier1
	@T1H_CHAIN=off $(T1_ASAN_OPTIONS) ./build/tier1-test --suite=$* $(T1_TEST_IMAGES) >/dev/null \
		2>build/tests/tier1/chain-off-$*.stderr || { cat build/tests/tier1/chain-off-$*.stderr >&2; exit 1; }
	@echo 'test-tier1-chain-off: suite $* equal to the reference with tier1_chain=off'
test-tier1:  test-tier1-chain-off

# Calls, returns and alloc in regions: the broken variants.

# Loop regions with several back-edges to the head.

# Sandy Bridge fallback acceptance: full instruction and compiler suites.
.PHONY: test-host-isa-baseline test-tier1-table-skips
test-tier1:  test-host-isa-baseline test-tier1-table-skips
test-host-isa-baseline: build/tier1-test build/ia64-test $(T1_TEST_IMAGES) $(IA64_TEST_IMAGES)
	@mkdir -p build/tests/ia64 build/tests/tier1
	@IA64VMM_HOST_ISA_MASK=0 $(T1_ASAN_OPTIONS) ./build/ia64-test --engine=tier0 $(IA64_TEST_IMAGES) >build/tests/ia64/mask0.stdout 2>build/tests/ia64/mask0.stderr || { cat build/tests/ia64/mask0.stderr >&2; exit 1; }
	@IA64VMM_HOST_ISA_MASK=0 $(T1_ASAN_OPTIONS) ./build/ia64-test --engine=lockstep $(IA64_TEST_IMAGES) >build/tests/ia64/mask0-lockstep.stdout 2>build/tests/ia64/mask0-lockstep.stderr || { cat build/tests/ia64/mask0-lockstep.stderr >&2; exit 1; }
	@IA64VMM_HOST_ISA_MASK=0 $(T1_ASAN_OPTIONS) ./build/tier1-test $(T1_TEST_IMAGES) >build/tests/tier1/mask0.stdout 2>build/tests/tier1/mask0.stderr || { cat build/tests/tier1/mask0.stderr >&2; exit 1; }
	@echo 'host-isa baseline: full Tier0, lockstep and Tier1 suites passed with mask 0'
test-tier1-table-skips: build/tier1-table-runner
	$(T1_ASAN_OPTIONS) python3 -I $(T1_PROVER_DIR)/table_skip_test.py build/tier1-table-runner



# One speed job for the benchmark wrapper's exit-path case (mk/bench.mk).
$(BENCH_QEMU_DIR)/jobs.bin: build/bench-jobdump tools/bench/plan.py $(T1_TEST_IMAGES)
	@mkdir -p $(@D)
	./build/bench-jobdump --dump=speed --insns=0 --out=$(@D)/speed.job $(T1_TEST_IMAGES) > /dev/null
	python3 -I tools/bench/plan.py --out $@ --pattern 0 $(@D)/speed.job
