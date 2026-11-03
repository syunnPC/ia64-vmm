# SPDX-License-Identifier: GPL-2.0-or-later
# The executor tests with
# threads and no guest (tests/host/smp/executors.c).

# Executors, stop-all, reset-hold, park bracket, run lock, per-CPU entry layout. The
# real CPU model, routes and threaded HAL; engine_run is the test's
# (--wrap), so no guest code runs.
SMP_EXEC_TEST_SOURCES = tests/host/smp/executors.c tests/host/smp/entry_abi.c core/smp.c core/helper.c \
	$(wildcard platform/*.c) persist/log.c persist/media.c persist/fat32.c persist/nvram.c persist/protocol.c $(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c
SMP_EXEC_TEST_OBJECTS = $(addprefix build/hosted/,$(SMP_EXEC_TEST_SOURCES:.c=.o))
.PHONY: test-smp-exec
test-host: test-smp-exec
test-smp-exec: build/tests/smp-exec
	@./build/tests/smp-exec 2>build/tests/smp-exec.stderr || { tail -40 build/tests/smp-exec.stderr >&2; exit 1; }
build/tests/smp-exec: $(SMP_EXEC_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(SMP_EXEC_TEST_OBJECTS) -pthread -Wl,--wrap=engine_run -Wl,--wrap=hal_idle_until -Wl,--wrap=platform_reset_restarts $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include $(SMP_EXEC_TEST_OBJECTS:.o=.d)

# Actual cache/engine executor threads, including RX quarantine,
# run plain and under ThreadSanitizer.
.PHONY: test-smp-engines
SMP_ENGINE_TEST_TIMEOUT ?= 60
test-host: test-smp-engines
test-smp-engines:
	python3 -I tests/host/smp/engines.py --timeout $(SMP_ENGINE_TEST_TIMEOUT)
