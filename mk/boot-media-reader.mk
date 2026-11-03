# SPDX-License-Identifier: GPL-2.0-or-later
TEST_TARGETS += test-boot-media
.PHONY: test-boot-media
test-boot-media: build/tests/boot-media
	./build/tests/boot-media
build/tests/boot-media: tests/boot/media.c boot/media.c boot/media.h include/ia64vmm/bootmedia.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) tests/boot/media.c boot/media.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
