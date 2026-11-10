# SPDX-License-Identifier: GPL-2.0-or-later
IA32_EXTENDED_TEST_SOURCES = tests/host/ia32/extended.c $(wildcard ia64/*.c ia64/decode/*.c ia64/ia32/*.c engine/interp/*.c engine/cache/*.c engine/tier0/*.c) engine/engine.c core/log.c core/guest_mem.c tests/hal/hal_linux.c
IA32_EXTENDED_TEST_OBJECTS = $(addprefix build/hosted/,$(IA32_EXTENDED_TEST_SOURCES:.c=.o))
TEST_TARGETS += test-ia32-extended
.PHONY: test-ia32-extended
test-ia32-extended: build/tests/ia32-extended
	./build/tests/ia32-extended
build/tests/ia32-extended: $(IA32_EXTENDED_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(IA32_EXTENDED_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -lm -o $@
-include $(IA32_EXTENDED_TEST_OBJECTS:.o=.d)
