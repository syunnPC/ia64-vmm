# SPDX-License-Identifier: GPL-2.0-or-later
# vmmcfg.efi: the "Configure VMM Options" UEFI application the stick's first
# Limine entry chainloads (tools/vmmcfg). x86-64 PE32+, built with a MinGW-w64
# cross compiler; MINGW_X64 is its triplet prefix (local.env.example). The
# editing logic (tools/vmmcfg/cfg.c) is also unit-tested hosted.
VMMCFG_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone \
    -fno-tree-loop-distribute-patterns -I$(GNU_EFI)/inc -I$(GNU_EFI)/inc/x86_64 -DGNU_EFI_USE_MS_ABI -Itools/vmmcfg
.PHONY: vmmcfg test-vmmcfg check-mingw
TEST_TARGETS += test-vmmcfg
vmmcfg: build/vmmcfg/vmmcfg.efi
check-mingw:
	@command -v $(MINGW_X64)gcc >/dev/null || { echo 'vmmcfg.efi needs a MinGW-w64 x86-64 compiler: $(MINGW_X64)gcc not found (install it or set MINGW_X64 to its prefix in local.env)' >&2; exit 1; }
build/vmmcfg/vmmcfg.efi: tools/vmmcfg/vmmcfg.c tools/vmmcfg/cfg.c tools/vmmcfg/cfg.h mk/vmmcfg.mk | check-mingw
	@mkdir -p $(@D)
	$(MINGW_X64)gcc $(VMMCFG_CFLAGS) tools/vmmcfg/vmmcfg.c tools/vmmcfg/cfg.c -nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main -Wl,--file-alignment,512 -s -o $@
build/tests/vmmcfg: tests/vmmcfg/test_cfg.c tools/vmmcfg/cfg.c tools/vmmcfg/cfg.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -I. $< tools/vmmcfg/cfg.c -o $@
test-vmmcfg: build/tests/vmmcfg build/vmmcfg/vmmcfg.efi
	./build/tests/vmmcfg
	python3 tests/vmmcfg/test_pe.py build/vmmcfg/vmmcfg.efi
