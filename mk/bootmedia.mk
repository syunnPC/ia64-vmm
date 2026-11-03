# SPDX-License-Identifier: GPL-2.0-or-later
BOOTMEDIA_CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone \
    -fno-tree-loop-distribute-patterns -I$(GNU_EFI)/inc -I$(GNU_EFI)/inc/x86_64 -DGNU_EFI_USE_MS_ABI -Iinclude
.PHONY: bootmedia test-bootmedia
TEST_TARGETS += test-bootmedia
bootmedia: build/bootmedia/bootmedia.efi
build/bootmedia/bootmedia.efi: tools/bootmedia/bootmedia.c include/ia64vmm/bootmedia.h mk/bootmedia.mk | check-mingw
	@mkdir -p $(@D)
	$(MINGW_X64)gcc $(BOOTMEDIA_CFLAGS) $< -nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main -Wl,--file-alignment,512 -s -o $@
build/tests/bootmedia: tests/bootmedia/uefi.c tools/bootmedia/bootmedia.c include/ia64vmm/bootmedia.h mk/bootmedia.mk
	@mkdir -p $(@D)
	$(HOSTCC) $(BOOTMEDIA_CFLAGS) tests/bootmedia/uefi.c tools/bootmedia/bootmedia.c -o $@
test-bootmedia: build/tests/bootmedia build/bootmedia/bootmedia.efi
	./build/tests/bootmedia
	python3 tests/vmmcfg/test_pe.py build/bootmedia/bootmedia.efi --allow-no-relocations
