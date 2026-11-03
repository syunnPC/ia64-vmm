# SPDX-License-Identifier: GPL-2.0-or-later
# CPU ordering, timing and generated-code regression tests.
# Part of `make test`. mk/*.mk is
# included in sorted order, so IA64_SOURCES (mk/ia64.mk) is defined here.
HOSTOBJDUMP ?= objdump
REVIEW_TEST_SOURCES = $(wildcard tests/review/*.c) $(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c
REVIEW_TEST_OBJECTS = $(addprefix build/hosted/,$(REVIEW_TEST_SOURCES:.c=.o))
TEST_TARGETS += test-review
.PHONY: test-review
test-review: build/tests/review
	@mkdir -p build/tests
	@./build/tests/review 2>build/tests/review.stderr || { tail -50 build/tests/review.stderr >&2; exit 1; }
build/tests/review: $(REVIEW_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(REVIEW_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -Wl,--wrap=hal_counter,--wrap=hal_counter_hz,--wrap=ia64_translate -o $@
-include $(REVIEW_TEST_OBJECTS:.o=.d)
