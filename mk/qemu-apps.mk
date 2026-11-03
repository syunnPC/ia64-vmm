# SPDX-License-Identifier: GPL-2.0-or-later
# Boot EFI test applications on the product VMM and check their serial logs.
TEST_TARGETS += test-qemu-apps
# cpudesc.efi with CPUDESC_EXPECTED vCPUs (tests/efi/cpudesc/cpudesc.c).
QEMU_MAX_VCPUS := $(shell python3 -c 'import os; print(min(32, len(os.sched_getaffinity(0)) - 1))')
CPUDESC_COUNTS := $(sort 1 2 3 4 $(QEMU_MAX_VCPUS))
$(CPUDESC_COUNTS:%=build/efi/cpudesc-%.o): build/efi/cpudesc-%.o: tests/efi/cpudesc/cpudesc.c tests/efi/cpudesc/sockets.h mk/qemu-apps.mk
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(EFI_CFLAGS) -DCPUDESC_EXPECTED=$* -MMD -MP -c $< -o $@
QEMU_APP_IMAGES := $(addprefix build/efi/,faultloop ebsloop hello cpumodel cpudesc-1 cpudesc-2 legacymap smp-ipi-itc \
	smp-broken-wake-vector legacymap-broken-descriptor legacymap-broken-page0)
build/efi/legacymap-broken-descriptor.o: tests/efi/legacymap/legacymap.c mk/qemu-apps.mk
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(EFI_CFLAGS) -DLEGACYMAP_REMOVE_DESCRIPTOR -MMD -MP -c $< -o $@
build/efi/legacymap-broken-page0.o: tests/efi/legacymap/legacymap.c mk/qemu-apps.mk
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(EFI_CFLAGS) -DLEGACYMAP_ADD_CONVENTIONAL -MMD -MP -c $< -o $@
build/firmware-new/legacymap.efi build/firmware-new/legacymap-broken-descriptor.efi build/firmware-new/legacymap-broken-page0.efi: build/firmware-new/%.efi: build/efi/%.efi
	@mkdir -p $(@D)
	cp $< $@
build/efi/sratmap-broken-hole.o: tests/efi/sratmap/sratmap.c mk/qemu-apps.mk
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(EFI_CFLAGS) -DSRATMAP_BROKEN_HOLE -MMD -MP -c $< -o $@
build/firmware-new/sratmap.efi build/firmware-new/sratmap-broken-hole.efi: build/firmware-new/%.efi: build/efi/%.efi
	@mkdir -p $(@D)
	cp $< $@
QEMU_APP_IMAGES += build/efi/sratmap build/efi/sratmap-broken-hole
QEMU_APP_IMAGES := $(QEMU_APP_IMAGES:%=%.efi) build/firmware-new/smp-ipi-itc.efi \
	build/firmware-new/UefiBehaviour.efi build/firmware-new/cpumodel.efi build/firmware-new/PalProbe.efi \
	build/firmware-new/benchwrap.efi build/firmware-new/benchwrap-keep-psr-i.efi \
	build/efi/smp-tlb-purge.efi build/firmware-new/smp-tlb-purge.efi \
	build/efi/restart.efi build/firmware-new/restart.efi build/firmware-new/smp-restart.efi
# restart.efi boots on the native firmware.
build/firmware-new/restart.efi: build/efi/restart.efi
	@mkdir -p $(@D)
	cp $< $@
# initpmi.efi: the application, its OS_INIT/OS_MCA/SAL PMI
# handlers and the PAL call stub; the broken variant's OS_INIT claims a new context.
QEMU_APP_IMAGES += build/firmware-new/initpmi.efi build/firmware-new/initpmi-broken-resume.efi
build/efi/initpmi-entry.o: tests/efi/initpmi/entry.S tests/efi/initpmi/initpmi.h mk/qemu-apps.mk
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -c $< -o $@
build/efi/initpmi-broken-resume-entry.o: tests/efi/initpmi/entry.S tests/efi/initpmi/initpmi.h mk/qemu-apps.mk
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -DINITPMI_BROKEN_RESUME=1 -c $< -o $@
build/efi/initpmi.so build/efi/initpmi-broken-resume.so: build/efi/%.so: build/efi/%-entry.o build/efi/initpmi.o build/efi/cpumodel-pal.o build/efi/crt0.o build/efi/reloc.o $(GNU_EFI)/elf_ia64_efi.lds
	$(CROSS_IA64)ld -nostdlib -shared -Bsymbolic -z nocombreloc --no-undefined -T $(GNU_EFI)/elf_ia64_efi.lds build/efi/crt0.o build/efi/reloc.o build/efi/initpmi.o $< build/efi/cpumodel-pal.o $$($(CROSS_IA64)gcc -print-libgcc-file-name) -o $@
build/firmware-new/initpmi.efi build/firmware-new/initpmi-broken-resume.efi: build/firmware-new/%.efi: build/efi/%.efi
	@mkdir -p $(@D)
	cp $< $@
# Finish the standalone smoke VM before the application's three VM workers.
# The threaded tests have real-clock bounds and must finish before any guest.
test-qemu-apps: | test-hal-smp test-smp-exec
ifneq ($(filter test,$(MAKECMDGOALS)),)
test-qemu-apps: | test-qemu
endif
.PHONY: test-qemu-apps
test-qemu-apps: build/vmm.elf firmware-new build/bootmedia/bootmedia.efi third_party/limine/BOOTX64.EFI \
		$(QEMU_APP_IMAGES)
	python3 -I tests/qemu/apps.py $(QEMU_APPS_ARGS)

# EDK2 produces this application as a side effect of its stamp.
build/firmware-new/UefiBehaviour.efi: build/firmware-new/.edk2
	@test -f $@

QEMU_EXTRA_EDK2 := PalProbe FpswaProbe GateHello GateLauncher
$(QEMU_EXTRA_EDK2:%=build/firmware-new/%.efi): build/firmware-new/.edk2
	@test -f $@
$(QEMU_EXTRA_EDK2:%=build/efi/%.efi): build/efi/%.efi: build/firmware-new/%.efi
	@mkdir -p $(@D)
	cp $< $@
build/firmware-new/debugserial.efi: build/efi/debugserial.efi
	@mkdir -p $(@D)
	cp $< $@
QEMU_AP_SCENARIOS := main nmi init ptcg ptcga
QEMU_AP_FW := $(addprefix build/firmware-new/,entry.o pal-entry.o pale.o sal-entry.o ivt.o ap-entry.o ap.o unaligned.o)
QEMU_AP_IMAGES := $(QEMU_AP_SCENARIOS:%=build/tests/qemu-ap/%.elf)
build/tests/qemu-ap/smp-ap-asm.o: tests/qemu/smp-ap.S
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -c $< -o $@
build/tests/qemu-ap/%.o: tests/qemu/smp-ap.c firmware/new/ap.h include/ia64vmm/platform.h
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(NEW_FW_CFLAGS) -I. -DSMP_AP_SCENARIO_$* -DSMP_AP_NAME='"$*"' -ffile-prefix-map=$(CURDIR)=. -c $< -o $@
build/tests/qemu-ap/%.elf: build/tests/qemu-ap/%.o build/tests/qemu-ap/smp-ap-asm.o $(QEMU_AP_FW) firmware/new/firmware.lds
	$(CROSS_IA64)ld -static --emit-relocs --no-undefined --build-id=none -T firmware/new/firmware.lds \
		-e smp_test_entry -o $@ $(QEMU_AP_FW) build/tests/qemu-ap/smp-ap-asm.o $<
.PHONY: test-qemu-apps-full
test-qemu-apps-full: test-qemu-apps smp-app build/efi/cpudesc-$(QEMU_MAX_VCPUS).efi $(QEMU_AP_IMAGES) $(QEMU_EXTRA_EDK2:%=build/firmware-new/%.efi) \
	$(QEMU_EXTRA_EDK2:%=build/efi/%.efi) build/efi/debugserial.efi build/firmware-new/debugserial.efi \
	build/efi/smp-tlb-purge.efi build/firmware-new/smp-tlb-purge.efi
	python3 -I tests/qemu/apps.py --full --extended-only $(QEMU_APPS_ARGS)

.PHONY: test-qemu-apps-checker
test-host: test-qemu-apps-checker

# Restore debug_serial.py's native ACPI pre-check before any serial-mode guest.
build/tests/qemu-apps/acpi-tables: tests/firmware/acpi-map.h firmware/new/acpi/tables.h tests/loaders/acpi_tables.c firmware/new/acpi/tables.c firmware/new/acpi/aml.c
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -I. -Iinclude $(filter %.c,$^) -o $@
.PHONY: test-qemu-acpi-tables
test-qemu-acpi-tables: build/tests/qemu-apps/acpi-tables
	./build/tests/qemu-apps/acpi-tables
test-qemu-apps-full: test-qemu-acpi-tables

build/tests/qemu-runtime-alias: tests/qemu/runtime-alias.c firmware/new/gate/runtime-alias.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -I. $< -o $@
test-qemu-apps-checker: build/tests/qemu-runtime-alias
	./build/tests/qemu-runtime-alias
	python3 -I tests/qemu/test_apps.py

# GNU-EFI CPU model payload.
build/firmware-new/cpumodel.efi: build/efi/cpumodel.efi
	@mkdir -p $(@D)
	cp $< $@

# One payload per CPU model and vCPU count.
CPU_MODEL_DEFAULT_APPS = $(foreach model,montecito madison,cpumodel-$(model)-1)
CPU_MODEL_FULL_APPS = $(foreach model,montecito madison,$(foreach count,2 4,cpumodel-$(model)-$(count)))
CPU_MODEL_APPS = $(CPU_MODEL_DEFAULT_APPS) $(CPU_MODEL_FULL_APPS)
test-qemu-apps: $(CPU_MODEL_DEFAULT_APPS:%=build/efi/%.efi) $(CPU_MODEL_DEFAULT_APPS:%=build/firmware-new/%.efi)
test-qemu-apps-full: $(CPU_MODEL_FULL_APPS:%=build/efi/%.efi) $(CPU_MODEL_FULL_APPS:%=build/firmware-new/%.efi)
build/efi/cpumodel-pal.o: tests/efi/cpumodel/pal.S
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -fpic -c $< -o $@
$(CPU_MODEL_APPS:%=build/efi/%.o): build/efi/cpumodel-%.o: tests/efi/cpumodel/cpumodel.c mk/qemu-apps.mk
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(EFI_CFLAGS) -DCPU_MODEL=$(if $(filter madison,$(word 1,$(subst -, ,$*))),1,0) -DCPU_COUNT=$(word 2,$(subst -, ,$*)) -MMD -MP -c $< -o $@
$(CPU_MODEL_APPS:%=build/efi/%.so): build/efi/cpumodel-%.so: build/efi/cpumodel-%.o build/efi/cpumodel-pal.o build/efi/crt0.o build/efi/reloc.o $(GNU_EFI)/elf_ia64_efi.lds
	$(CROSS_IA64)ld -nostdlib -shared -Bsymbolic -z nocombreloc --no-undefined -T $(GNU_EFI)/elf_ia64_efi.lds $(filter %.o,$^) $$($(CROSS_IA64)gcc -print-libgcc-file-name) -o $@
build/firmware-new/cpumodel-%.efi: build/efi/cpumodel-%.efi
	@mkdir -p $(@D)
	cp $< $@
build/efi/cpumodel.so: build/efi/cpumodel.o build/efi/cpumodel-pal.o build/efi/crt0.o build/efi/reloc.o $(GNU_EFI)/elf_ia64_efi.lds
	$(CROSS_IA64)ld -nostdlib -shared -Bsymbolic -z nocombreloc --no-undefined -T $(GNU_EFI)/elf_ia64_efi.lds $(filter %.o,$^) $$($(CROSS_IA64)gcc -print-libgcc-file-name) -o $@

# Both socket widths and firmware table builders, outside the smoke budget.
test-qemu-apps-full: $(foreach n,3 4,build/efi/cpudesc-$(n).efi) $(foreach n,1 2 3 4,build/firmware-new/cpudesc-$(n).efi)
build/firmware-new/cpudesc-%.efi: build/efi/cpudesc-%.efi
	@mkdir -p $(@D)
	cp $< $@
