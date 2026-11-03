# SPDX-License-Identifier: GPL-2.0-or-later
# Discover host assembly; the entry assembly is owned by boot/.
BM_SOURCES += $(wildcard boot/*.S)
ifneq ($(AMD64_HOST_ONLY),1)
BM_SOURCES += host/amd64/tests/hwb.c host/amd64/tests/hwb_access.S host/amd64/tests/simd.c host/amd64/tests/simd_access.S
endif
AVAILABLE_TESTS += qemu
# Opt-in smoke image only; normal builds always use the shared lifecycle.
ifeq ($(AMD64_HOST_ONLY),1)
override SHARED_SOURCES := core/log.c core/guest_mem.c core/options.c host/amd64/tests/vm_stub.c
BM_CFLAGS += -DIA64VMM_AMD64_HOST_ONLY=1
endif

.PHONY: esp
esp: build/esp.img
build/esp.img: build/vmm.elf build/bootmedia/bootmedia.efi third_party/limine/BOOTX64.EFI tools/qemu/mkesp.py tools/qemu/bootmedia.py FORCE
	python3 tools/qemu/mkesp.py

test-qemu:: all | test-hal-smp test-smp-exec
	@status=0; tools/qemu/run.sh --firmware '$(FIRMWARE)' || status=$$?; \
	 test "$$status" = '$(QEMU_EXPECT_EXIT)' || { echo "test-qemu: expected $(QEMU_EXPECT_EXIT), got $$status" >&2; exit 1; }; \
	 FIRMWARE='$(FIRMWARE)' python3 tools/qemu/check-log.py build/qemu.log '$(QEMU_EXPECT_EXIT)' || exit $$?; \
	 echo 'test-qemu: passed (exit $(QEMU_EXPECT_EXIT))'

.PHONY: test-fb check-amd64-freestanding
FB_TEST_SOURCES := host/fb/tests/console.c host/fb/fb_console.c host/fb/vga_text.c core/string.c
FB_TEST_OBJECTS := $(addprefix build/hosted/,$(FB_TEST_SOURCES:.c=.o))
build/tests/fb: $(FB_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(FB_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
test-fb: build/tests/fb
	@./build/tests/fb
	@echo 'test-fb: 16/24/32-bpp masks, glyphs, cursor, scroll, handover and bounds passed'
test-host: test-fb
# Include the entire linked image to allow cross-module boot/host symbols.
check-amd64-freestanding: build/vmm.elf
	@sh mk/check-freestanding.sh '$(HOSTNM)' build/vmm.elf
-include $(FB_TEST_OBJECTS:.o=.d)

# Switching the optional smoke harness must relink even if every object
# already exists; Make does not itself track changes to prerequisite lists.
build/bm/.amd64-inputs: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(BM_OBJECTS) $(BM_LDFLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
build/vmm.elf: build/bm/.amd64-inputs

# Pure CPUID fixtures and threaded-HAL races exercise the host-foundation contracts.
FOUNDATION_TEST_SOURCES := host/amd64/tests/foundation.c host/amd64/topology.c host/amd64/simd.c core/log.c tests/hal/hal_linux.c
FOUNDATION_TEST_OBJECTS := $(addprefix build/hosted/,$(FOUNDATION_TEST_SOURCES:.c=.o))
.PHONY: test-foundation test-qemu-runner
test-host: test-foundation test-qemu-runner
test-foundation: build/tests/foundation
	@./build/tests/foundation 2>build/tests/foundation.stderr
build/tests/foundation: $(FOUNDATION_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(FOUNDATION_TEST_OBJECTS) -pthread $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
test-qemu-runner:
	@python3 tools/qemu/test_runner.py
-include $(FOUNDATION_TEST_OBJECTS:.o=.d)

.PHONY: test-apic-timer
test-host: test-apic-timer
test-apic-timer: build/tests/apic-timer
	@./build/tests/apic-timer
build/tests/apic-timer: host/amd64/tests/apic_timer.c host/amd64/apic.c host/amd64/smp.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections -MMD -MP -MF $@.d $< -Wl,--gc-sections $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include build/tests/apic-timer.d
