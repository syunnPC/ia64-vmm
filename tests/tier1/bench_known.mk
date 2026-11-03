# SPDX-License-Identifier: GPL-2.0-or-later
# Build the same driver against either production source tree.
T1_KNOWN_BENCH_SOURCE ?= tests/tier1/bench_known.c
T1_KNOWN_BENCH_OBJECTS := $(filter-out build/hosted/tests/tier1/main.o,$(T1_TEST_OBJECTS))
build/tier1-known-bench: $(T1_KNOWN_BENCH_SOURCE) $(T1_KNOWN_BENCH_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< $(T1_KNOWN_BENCH_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
