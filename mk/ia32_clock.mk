# SPDX-License-Identifier: GPL-2.0-or-later
IA32_CLOCK_TEST_SOURCES = tests/host/ia32/clock.c ia64/ia32/clock.c ia64/ia32/records.c
IA32_CLOCK_TEST_OBJECTS = $(addprefix build/hosted/,$(IA32_CLOCK_TEST_SOURCES:.c=.o))
TEST_TARGETS += test-ia32-clock
.PHONY: test-ia32-clock
test-ia32-clock: build/tests/ia32-clock
	./build/tests/ia32-clock
build/tests/ia32-clock: $(IA32_CLOCK_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(IA32_CLOCK_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include $(IA32_CLOCK_TEST_OBJECTS:.o=.d)
