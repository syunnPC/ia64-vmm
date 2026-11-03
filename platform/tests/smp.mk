# SPDX-License-Identifier: GPL-2.0-or-later
# SMP threaded units use the existing Linux HAL shim; no guest VM.
SMP_PLATFORM_OBJECTS = build/hosted/core/smp.o build/hosted/core/helper.o build/hosted/platform/tests/smp.o $(filter-out build/hosted/platform/tests/interrupts.o,$(IRQ_TEST_OBJECTS))
SMP_PLATFORM_WRAP = -pthread -Wl,--wrap=platform_irq_inject,--wrap=hal_cpu_request,--wrap=hal_port_in,--wrap=engine_run
TEST_TARGETS += test-smp-platform
.PHONY: test-smp-platform test-smp-platform-mutants
build/tests/smp-platform: $(SMP_PLATFORM_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(SMP_PLATFORM_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) $(SMP_PLATFORM_WRAP) -o $@
test-smp-platform: build/tests/smp-platform
	@for case in iosapic latch ownership pci-race iosapic-intx teardown-1 teardown-2 teardown-3 teardown-4; do ./build/tests/smp-platform $$case || exit 1; done
.PHONY: test-smp-platform-watchdog
test-smp-platform-watchdog: build/tests/smp-platform
	python3 -I platform/tests/smp-watchdog.py --cc '$(HOSTCC)' \
		--cflags '$(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS)' \
		--ldflags '$(HOSTED_LDFLAGS) $(LDFLAGS)' $(SMP_PLATFORM_OBJECTS)
build/tests/smp-iosapic-unlocked.o: platform/iosapic.c build/hosted/.flags
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -DIA64VMM_TEST_IOSAPIC_UNLOCKED=1 -c $< -o $@
build/tests/smp-iosapic-unlocked: build/tests/smp-iosapic-unlocked.o $(filter-out build/hosted/platform/iosapic.o,$(SMP_PLATFORM_OBJECTS)) build/hosted/.flags
	$(HOSTCC) $(filter %.o,$^) $(HOSTED_LDFLAGS) $(LDFLAGS) $(SMP_PLATFORM_WRAP) -o $@
test-smp-platform-mutants: build/tests/smp-iosapic-unlocked build/tests/smp-shared-latch
	python3 platform/tests/smp.py
-include build/hosted/platform/tests/smp.d

.PHONY: irq-smp-efi
irq-smp-efi: build/firmware-new/interrupts-smp.efi
build/efi/interrupts-smp.o: platform/tests/efi.c
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(NEW_FW_CFLAGS) -fpic -mno-sdata -mfixed-range=f32-f127 -DIA64VMM_IRQ_SMP=1 -MMD -MP -c $< -o $@
build/firmware-new/interrupts-smp.efi: build/efi/interrupts-smp.efi
	@mkdir -p $(@D)
	cp $< $@

build/tests/smp-shared-latch.c: platform/devices.c platform/tests/shared-latch.py
	@mkdir -p $(@D)
	python3 platform/tests/shared-latch.py
build/tests/smp-shared-latch.o: build/tests/smp-shared-latch.c build/hosted/.flags
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
build/tests/smp-shared-latch: build/tests/smp-shared-latch.o $(filter-out build/hosted/platform/devices.o,$(SMP_PLATFORM_OBJECTS)) build/hosted/.flags
	$(HOSTCC) $(filter %.o,$^) $(HOSTED_LDFLAGS) $(LDFLAGS) $(SMP_PLATFORM_WRAP) -o $@

build/tests/smp-stopped-owner.c build/tests/smp-delayed-command.c &: platform/intx.c platform/tests/intx-mutants.py
	@mkdir -p build/tests
	python3 platform/tests/intx-mutants.py
build/tests/smp-stopped-owner.o build/tests/smp-delayed-command.o: build/tests/%.o: build/tests/%.c build/hosted/.flags
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
build/tests/smp-stopped-owner build/tests/smp-delayed-command: build/tests/%: build/tests/%.o $(filter-out build/hosted/platform/intx.o,$(SMP_PLATFORM_OBJECTS)) build/hosted/.flags
	$(HOSTCC) $(filter %.o,$^) $(HOSTED_LDFLAGS) $(LDFLAGS) $(SMP_PLATFORM_WRAP) -o $@
test-smp-platform-mutants: build/tests/smp-stopped-owner build/tests/smp-delayed-command

build/tests/smp-platform-cost: build/hosted/platform/tests/cost.o $(filter-out build/hosted/platform/tests/interrupts.o,$(IRQ_TEST_OBJECTS)) build/hosted/.flags
	$(HOSTCC) $(filter %.o,$^) $(HOSTED_LDFLAGS) $(LDFLAGS) -pthread -o $@
-include build/hosted/platform/tests/cost.d
