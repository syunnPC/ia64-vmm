# SPDX-License-Identifier: GPL-2.0-or-later
TEST_TARGETS += test-boot-diagnostics
.PHONY: test-boot-diagnostics
test-boot-diagnostics: build/tests/boot-diagnostics
	./build/tests/boot-diagnostics
build/tests/boot-diagnostics: tests/boot/diagnostics.c host/amd64/boot_walk.c host/amd64/boot_walk.h host/amd64/boot_probe.S build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) tests/boot/diagnostics.c host/amd64/boot_walk.c host/amd64/boot_probe.S $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
