# SPDX-License-Identifier: GPL-2.0-or-later
TEST_TARGETS += test-boot-reserve
.PHONY: test-boot-reserve
test-boot-reserve: build/tests/boot-reserve build/tests/boot-allocator
	./build/tests/boot-reserve
	./build/tests/boot-allocator
build/tests/boot-reserve: tests/boot/reserve.c boot/ranges.c boot/ranges.h include/ia64vmm/memory_policy.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) tests/boot/reserve.c boot/ranges.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@

build/tests/boot-allocator: tests/boot/allocator.c host/amd64/memory.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections $< $(HOSTED_LDFLAGS) $(LDFLAGS) -Wl,--gc-sections -o $@
