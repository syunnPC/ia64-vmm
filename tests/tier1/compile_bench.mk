# SPDX-License-Identifier: GPL-2.0-or-later
# make -f Makefile -f tests/tier1/compile_bench.mk build/tier1-compile-bench
# taskset -c 0 ./build/tier1-compile-bench 1000 regalloc
# Other stages: raverify, frames. No guest assembler or image is required.
build/tier1-compile-bench: tests/tier1/compile_bench.c $(filter-out build/hosted/tests/tier1/main.o,$(T1_TEST_OBJECTS)) build/hosted/.flags
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< $(filter-out build/hosted/tests/tier1/main.o,$(T1_TEST_OBJECTS)) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
