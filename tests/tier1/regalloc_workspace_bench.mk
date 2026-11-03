# SPDX-License-Identifier: GPL-2.0-or-later
# Include regalloc_pair_bench.mk before this file to build the reference.
build/tier1-regalloc-workspace-bench: tests/tier1/compile_bench.c tests/tier1/regalloc_workspace_bench.c build/tier1-regalloc-reference.o $(filter-out build/hosted/tests/tier1/main.o,$(T1_TEST_OBJECTS)) build/hosted/.flags
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) tests/tier1/compile_bench.c tests/tier1/regalloc_workspace_bench.c build/tier1-regalloc-reference.o $(filter-out build/hosted/tests/tier1/main.o,$(T1_TEST_OBJECTS)) $(HOSTED_LDFLAGS) $(LDFLAGS) -Wl,--wrap=t1_regalloc -Wl,--wrap=t1_ws_alloc -o $@
