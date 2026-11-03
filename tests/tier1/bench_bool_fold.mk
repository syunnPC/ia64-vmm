# SPDX-License-Identifier: GPL-2.0-or-later
# A baseline may use this driver via an absolute or relative source path.
T1_BOOL_FOLD_BENCH_SOURCE ?= tests/tier1/bench_bool_fold.c
T1_BOOL_FOLD_BENCH_OBJECTS := $(filter-out build/hosted/tests/tier1/main.o,$(T1_TEST_OBJECTS))
build/tier1-bool-fold-bench: $(T1_BOOL_FOLD_BENCH_SOURCE) $(T1_BOOL_FOLD_BENCH_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< $(T1_BOOL_FOLD_BENCH_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
.PHONY: bench-tier1-bool-fold
bench-tier1-bool-fold: build/tier1-bool-fold-bench
	./build/tier1-bool-fold-bench
