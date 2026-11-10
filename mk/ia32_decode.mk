# SPDX-License-Identifier: GPL-2.0-or-later
IA32_DECODE_TEST_SOURCES = tests/host/ia32/decode.c ia64/ia32/decode.c
IA32_DECODE_TEST_OBJECTS = $(addprefix build/hosted/,$(IA32_DECODE_TEST_SOURCES:.c=.o))
TEST_TARGETS += test-ia32-decode
.PHONY: test-ia32-decode
test-ia32-decode: build/tests/ia32-decode
	./build/tests/ia32-decode
build/tests/ia32-decode: $(IA32_DECODE_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(IA32_DECODE_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include $(IA32_DECODE_TEST_OBJECTS:.o=.d)
