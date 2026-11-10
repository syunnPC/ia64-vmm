# SPDX-License-Identifier: GPL-2.0-or-later
# Random differential tester: hosted interpreter vs. the IA-64 QEMU fork.
# See docs/testing.md. mk/*.mk is included alphabetically, so this
# file cannot use ia64.mk's variables in prerequisite lists.
FUZZ_RUNNER_SOURCES = tests/fuzz/runner.c \
	$(wildcard ia64/*.c ia64/decode/*.c ia64/ia32/*.c engine/interp/*.c engine/cache/*.c engine/tier0/*.c) \
	engine/engine.c core/log.c core/guest_mem.c tests/hal/hal_linux.c
FUZZ_RUNNER_OBJECTS = $(addprefix build/hosted/,$(FUZZ_RUNNER_SOURCES:.c=.o))
SEEDS ?= 200
SEED0 ?= 1
FUZZ_JOBS ?= 8
FUZZ_ARGS ?=

.PHONY: fuzz-ia64 fuzz-ia64-runner
fuzz-ia64-runner: build/fuzz/ia64-fuzz-runner
build/fuzz/ia64-fuzz-runner: $(FUZZ_RUNNER_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(FUZZ_RUNNER_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
# Paths: the environment/command line wins, then local.env (read by fuzz.py),
# then mk/config.mk's defaults as a last resort.
fuzz_env = $(if $(filter environment% command%,$(origin $(1))),$(1)='$($(1))')
fuzz-ia64: build/fuzz/ia64-fuzz-runner
	$(call fuzz_env,CROSS_IA64) $(call fuzz_env,QEMU_IA64) python3 tests/fuzz/fuzz.py \
		--default-cross '$(CROSS_IA64)' --default-qemu '$(QEMU_IA64)' \
		--runner build/fuzz/ia64-fuzz-runner --seeds $(SEEDS) --seed0 $(SEED0) \
		--jobs $(FUZZ_JOBS) $(FUZZ_ARGS)
-include build/hosted/tests/fuzz/runner.d
