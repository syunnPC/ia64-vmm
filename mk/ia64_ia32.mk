# SPDX-License-Identifier: GPL-2.0-or-later
SHARED_SOURCES += $(wildcard ia64/ia32/*.c)
IA32_STATE_TEST_SOURCES = tests/host/ia32/state.c $(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c
IA32_STATE_TEST_OBJECTS = $(addprefix build/hosted/,$(IA32_STATE_TEST_SOURCES:.c=.o))
TEST_TARGETS += test-ia32-state
.PHONY: test-ia32-state
test-ia32-state: build/tests/ia32-state
	./build/tests/ia32-state
build/tests/ia32-state: $(IA32_STATE_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(IA32_STATE_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -lm -o $@
-include $(IA32_STATE_TEST_OBJECTS:.o=.d)
