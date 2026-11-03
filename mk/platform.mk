# SPDX-License-Identifier: GPL-2.0-or-later
PLATFORM_SOURCES := $(wildcard platform/*.c)
PLATFORM_TEST_SOURCES := tests/host/smp/platform_purge_stub.c tests/firmware/halt_stub.c tests/hosted/platform.c $(PLATFORM_SOURCES) persist/log.c persist/media.c persist/fat32.c persist/nvram.c persist/protocol.c core/vm.c core/smp.c core/helper.c core/log.c core/guest_mem.c tests/hal/hal_linux.c
PLATFORM_TEST_OBJECTS := $(addprefix build/hosted/,$(PLATFORM_TEST_SOURCES:.c=.o))
.PHONY: test-platform check-platform-freestanding
test-host: test-platform
test-platform: build/tests/platform build/tests/platform-loader.elf build/tests/platform-rtc
	@mkdir -p build/tests
	@for suite in loader handoff granule presence window devices pci rtc vm; do ./build/tests/platform $$suite build/tests/platform-loader.elf 2>build/tests/platform-$$suite.stderr || { cat build/tests/platform-$$suite.stderr >&2; exit 1; }; done
	@./build/tests/platform-rtc
build/tests/platform: $(PLATFORM_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(PLATFORM_TEST_OBJECTS) -Wl,--wrap=hal_counter,--wrap=hal_idle_until,--wrap=hal_port_in,--wrap=hal_port_out,--wrap=hal_memory_get_stats,--wrap=nvram_load,--wrap=nvram_save,--wrap=platform_pci_reset_functions $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
build/tests/platform-loader.o: tests/hosted/loader.S
	@mkdir -p $(@D)
	$(CROSS_IA64)as $< -o $@
build/tests/platform-loader.elf: build/tests/platform-loader.o tests/hosted/loader.lds
	$(CROSS_IA64)ld --emit-relocs -T tests/hosted/loader.lds $< -o $@
build/tests/platform-rtc: tests/hosted/rtc.c build/hosted/platform/rtc.o build/hosted/.flags
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< build/hosted/platform/rtc.o $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
check-platform-freestanding: check-freestanding
-include $(PLATFORM_TEST_OBJECTS:.o=.d)
