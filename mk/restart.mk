# SPDX-License-Identifier: GPL-2.0-or-later
# Restart of the assigned functions: hosted checks
# against a fake PCIe topology with reset semantics.
RESTART_TEST_SOURCES = platform/tests/restart.c $(wildcard platform/*.c) $(IA64_SOURCES) persist/log.c persist/media.c persist/fat32.c persist/nvram.c \
	persist/protocol.c core/log.c core/guest_mem.c tests/hal/hal_linux.c
RESTART_TEST_OBJECTS = $(addprefix build/hosted/,$(RESTART_TEST_SOURCES:.c=.o))
.PHONY: test-restart
test-host: test-restart
test-restart: build/tests/restart
	@./build/tests/restart 2>build/tests/restart.stderr || { cat build/tests/restart.stderr >&2; exit 1; }
build/tests/restart: $(RESTART_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(RESTART_TEST_OBJECTS) -Wl,--wrap=hal_counter -Wl,--wrap=hal_idle_until $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include $(RESTART_TEST_OBJECTS:.o=.d)
