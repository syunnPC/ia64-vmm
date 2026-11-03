# SPDX-License-Identifier: GPL-2.0-or-later
.PHONY: test-loaders test-loader-tools test-cpudesc-sockets
TEST_TARGETS += test-loader-tools test-cpudesc-sockets
LOADER_IVT_OBJECTS = $(addprefix build/hosted/,$(patsubst %.c,%.o,$(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c))
test-loader-tools:
	python3 tests/loaders/test_tools.py
test-loaders: vmm build/firmware.elf test-loader-tools
	python3 tests/loaders/run.py $(LOADER_ARGS)
test-cpudesc-sockets: build/tests/cpudesc-sockets
	./build/tests/cpudesc-sockets
build/tests/cpudesc-sockets: tests/firmware/cpudesc-sockets.c tests/efi/cpudesc/sockets.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -I. $< -o $@
