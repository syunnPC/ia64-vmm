# SPDX-License-Identifier: GPL-2.0-or-later
# A baseline can use this driver without changing its production sources:
# make -f Makefile -f ../candidate/tests/tier1/bench_entry.mk \
#   T1_ENTRY_BENCH_SOURCE=../candidate/tests/tier1/bench_entry.c build/tier1-entry-bench
T1_ENTRY_BENCH_SOURCE ?= tests/tier1/bench_entry.c
T1_ENTRY_BENCH_OBJECTS := $(filter-out build/hosted/tests/tier1/main.o,$(T1_TEST_OBJECTS))
build/tier1-entry-bench: $(T1_ENTRY_BENCH_SOURCE) $(T1_ENTRY_BENCH_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< $(T1_ENTRY_BENCH_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
.PHONY: bench-tier1-entry
bench-tier1-entry: build/tier1-entry-bench
	./build/tier1-entry-bench
