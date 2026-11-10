# SPDX-License-Identifier: GPL-2.0-or-later
IA32_IO_TEST_SOURCES = tests/host/ia32/io.c $(wildcard ia64/*.c ia64/decode/*.c ia64/ia32/*.c engine/interp/*.c engine/cache/*.c engine/tier0/*.c) engine/engine.c core/log.c core/guest_mem.c tests/hal/hal_linux.c
IA32_IO_TEST_OBJECTS = $(addprefix build/hosted/,$(IA32_IO_TEST_SOURCES:.c=.o))
TEST_TARGETS += test-ia32-io
.PHONY: test-ia32-io
test-ia32-io: build/tests/ia32-io
	./build/tests/ia32-io
build/tests/ia32-io: $(IA32_IO_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(IA32_IO_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -lm -o $@
-include $(IA32_IO_TEST_OBJECTS:.o=.d)
