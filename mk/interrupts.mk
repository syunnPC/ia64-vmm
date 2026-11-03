# SPDX-License-Identifier: GPL-2.0-or-later
# Interrupt-controller and MSI unit suite; part of `make test`.
TEST_TARGETS += test-interrupts
TEST_TARGETS += test-aml-routing
IRQ_TEST_SOURCES = platform/tests/interrupts.c $(wildcard platform/*.c) persist/log.c persist/media.c persist/fat32.c persist/nvram.c persist/protocol.c $(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c
IRQ_TEST_OBJECTS = $(addprefix build/hosted/,$(IRQ_TEST_SOURCES:.c=.o))
.PHONY: test-interrupts
test-interrupts: build/tests/interrupts
	./build/tests/interrupts
build/tests/interrupts: $(IRQ_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(IRQ_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -pthread -Wl,--wrap=hal_cpu_request -Wl,--wrap=vcpu_kick -o $@
-include $(IRQ_TEST_OBJECTS:.o=.d)
-include build/hosted/platform/tests/aml.d
.PHONY: test-aml-routing
build/tests/aml-core.aml: platform/tests/aml-core.asl
	@mkdir -p $(@D)
	"$(IASL)" -p build/tests/aml-core $<
build/tests/aml-routing: build/hosted/platform/tests/aml.o $(filter-out build/hosted/platform/tests/interrupts.o,$(IRQ_TEST_OBJECTS)) build/hosted/.flags
	$(HOSTCC) $(filter %.o,$^) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
test-aml-routing: build/tests/aml-routing build/tests/aml-core.aml
	./build/tests/aml-routing build/tests/aml-core.aml
	python3 platform/tests/aml-fixtures.py
.PHONY: irq-efi
irq-efi: build/firmware-new/interrupts.efi
build/efi/interrupts.o: platform/tests/efi.c
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(NEW_FW_CFLAGS) -fpic -mno-sdata -mfixed-range=f32-f127 -MMD -MP -c $< -o $@
build/firmware-new/interrupts.efi: build/efi/interrupts.efi
	@mkdir -p $(@D)
	cp $< $@

include platform/tests/smp.mk
