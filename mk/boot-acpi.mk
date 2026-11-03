# SPDX-License-Identifier: GPL-2.0-or-later
TEST_TARGETS += test-boot-acpi
.PHONY: test-boot-acpi
test-boot-acpi: build/tests/boot-acpi
	./build/tests/boot-acpi
build/tests/boot-acpi: tests/boot/acpi.c boot/acpi.c boot/acpi.h boot/ranges.c boot/ranges.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) tests/boot/acpi.c boot/acpi.c boot/ranges.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
