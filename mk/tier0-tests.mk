# SPDX-License-Identifier: GPL-2.0-or-later
# Tier0 fuzz runners.
TIER0_FUZZ_OBJECTS = $(filter-out build/hosted/tests/fuzz/runner.o,$(FUZZ_RUNNER_OBJECTS))
.PHONY: tier0-fuzz-runners
tier0-fuzz-runners: build/tier0-fuzz-runner build/lockstep-fuzz-runner fuzz-ia64-runner
build/tier0-fuzz-runner: build/hosted/tests/tier0/fuzz-runner.o $(TIER0_FUZZ_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(filter-out build/hosted/.flags,$^) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
build/hosted/tests/tier0/fuzz-lockstep.o: tests/tier0/fuzz-runner.c build/hosted/.flags
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -DTIER0_FUZZ_ENGINE=ENGINE_LOCKSTEP -MMD -MP -c $< -o $@
build/lockstep-fuzz-runner: build/hosted/tests/tier0/fuzz-lockstep.o $(TIER0_FUZZ_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(filter-out build/hosted/.flags,$^) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include build/hosted/tests/tier0/fuzz-runner.d build/hosted/tests/tier0/fuzz-lockstep.d
