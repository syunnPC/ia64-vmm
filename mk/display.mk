# SPDX-License-Identifier: GPL-2.0-or-later
# Register-model test of the shared GOP->VGA text sequences in
# firmware/new/vga-text.h, and the sweep showing each check catches a broken
# header variant.
.PHONY: test-vga-igd
TEST_TARGETS += test-vga-igd
test-vga-igd: build/tests/vga-igd
	./build/tests/vga-igd
build/tests/vga-igd: tests/firmware/vga_igd.c firmware/new/vga-text.h include/ia64vmm/platform.h firmware/include/ui/vgafont.c core/log_internal.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -I. -Iinclude $< -o $@
