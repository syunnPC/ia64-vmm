# SPDX-License-Identifier: GPL-2.0-or-later
# Threaded purge protocol and deterministic VHPT read races.
SMP_PURGE_TEST_SOURCES = tests/host/smp/purge.c core/smp.c core/helper.c \
	$(wildcard platform/*.c) persist/log.c persist/media.c persist/fat32.c persist/nvram.c persist/protocol.c $(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c
SMP_PURGE_TEST_OBJECTS = $(addprefix build/hosted/,$(SMP_PURGE_TEST_SOURCES:.c=.o))
.PHONY: test-smp-purge
test-host: test-smp-purge
test-smp-purge: build/tests/smp-purge
	@./build/tests/smp-purge 2>build/tests/smp-purge.stderr || { tail -40 build/tests/smp-purge.stderr >&2; exit 1; }
build/tests/smp-purge: $(SMP_PURGE_TEST_OBJECTS) build/hosted/.flags build/tests/ia64/smp-boundary.bin
	@mkdir -p $(@D)
	$(HOSTCC) $(SMP_PURGE_TEST_OBJECTS) -pthread -Wl,--wrap=ia64_smp_purge_hook -Wl,--wrap=engine_run -Wl,--wrap=ia64_physical_read -Wl,--wrap=ia64_smp_global_purge -Wl,--wrap=hal_idle_until $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include $(SMP_PURGE_TEST_OBJECTS:.o=.d)
# Separate sample so core/smp.c can link without the purge test APIs.
SMP_PURGE_BENCH_OBJECTS = $(filter-out build/hosted/tests/host/smp/purge.o,$(SMP_PURGE_TEST_OBJECTS)) \
	build/hosted/tests/host/smp/purge_exec_bench.o
build/tests/smp-purge-exec-bench: $(SMP_PURGE_BENCH_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(SMP_PURGE_BENCH_OBJECTS) -pthread -Wl,--wrap=engine_run $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include build/hosted/tests/host/smp/purge_exec_bench.d
