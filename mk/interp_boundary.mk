# SPDX-License-Identifier: GPL-2.0-or-later
INTERP_BOUNDARY_TEST_SOURCES = tests/host/smp/fallback_kick.c $(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c
INTERP_BOUNDARY_TEST_OBJECTS = $(addprefix build/hosted/,$(INTERP_BOUNDARY_TEST_SOURCES:.c=.o))
TEST_TARGETS += test-interp-boundary
.PHONY: test-interp-boundary
test-interp-boundary: build/tests/interp-boundary
	./build/tests/interp-boundary
build/tests/interp-boundary: $(INTERP_BOUNDARY_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(INTERP_BOUNDARY_TEST_OBJECTS) -Wl,--wrap=ia64_deliver_fault $(HOSTED_LDFLAGS) $(LDFLAGS) -lm -o $@
-include $(INTERP_BOUNDARY_TEST_OBJECTS:.o=.d)
