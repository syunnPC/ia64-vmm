# /* SPDX-License-Identifier: GPL-2.0-or-later */
.PHONY: firmware-new test-firmware-converter
TEST_TARGETS += test-firmware-converter test-firmware-hobs
# The debug image keeps EDK2's DEBUG/ASSERT output; build/firmware-new/release/ is the release build.
firmware-new: build/firmware-new/.edk2 build/firmware-new/ia64-firmware-new.elf build/firmware-new/hello.efi \
	build/firmware-new/release/ia64-firmware-new.elf
NEW_FW_EDK2_INPUTS := $(shell find firmware/edk2 firmware/new third_party/edk2-ipf -type f \( -name '*.[chS]' \
	-o -name '*.s' -o -name '*.i' -o -name '*.lds' -o -name '*.json' -o -name '*.patch' \) 2>/dev/null)
# CPU model reporting is also consumed inside the EDK2 firmware volume.
NEW_FW_EDK2_INPUTS += $(wildcard firmware/*.h) include/ia64vmm/platform.h include/ia64vmm/cpu_model.h
NEW_FW_FPSWA_INPUTS := $(wildcard ia64/fp*.c ia64/fp*.h ia64/decoder.c ia64/decode/decode.c) tests/firmware/build_fpswa.py tests/firmware/fpswa-guest.c tests/firmware/fpswa-guest.S tests/firmware/test_fpswa.py $(wildcard tests/fuzz/fp/*.py)
build/firmware-new/.edk2: $(NEW_FW_EDK2_INPUTS) $(NEW_FW_FPSWA_INPUTS) tools/firmware/build_edk2.py tools/firmware/elf_to_pe.py build/firmware-new/debug.o
	python3 tools/firmware/build_edk2.py
	@touch $@
build/firmware-new/release/.edk2: $(NEW_FW_EDK2_INPUTS) $(NEW_FW_FPSWA_INPUTS) tools/firmware/build_edk2.py tools/firmware/elf_to_pe.py | build/firmware-new/.edk2
	python3 tools/firmware/build_edk2.py --release
	@touch $@
build/firmware-new/debug.o: firmware/new/debug.c mk/firmware-new.mk
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(NEW_FW_CFLAGS) -MMD -MP -c $< -o $@
test-firmware-converter:
	python3 -m unittest discover -s tests/firmware -p 'test_*.py' -v
.PHONY: firmware-new-gate-esp test-firmware-hobs
test-firmware-hobs: build/tests/firmware-hobs
	./build/tests/firmware-hobs
build/tests/firmware-hobs: tests/firmware/hob.c firmware/new/hob.c firmware/new/hob.h firmware/new/legacy-handoff.h firmware/new/legacy-window.h include/ia64vmm/platform.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -D_EFI_P64 -I. -Iinclude -Ithird_party/edk2-ipf/MdePkg/Include -Ithird_party/edk2-ipf/MdePkg/Include/Ipf tests/firmware/hob.c firmware/new/hob.c -o $@
build/efi/gate-launcher.efi: build/firmware-new/.edk2
	@mkdir -p $(@D)
	cp build/firmware-new/GateLauncher.efi $@
build/firmware-new/hello.efi: build/efi/hello.efi
	@mkdir -p $(@D)
	cp $< $@
firmware-new-gate-esp: firmware-new
	+$(MAKE) esp BOOT_APP=gate-launcher

NEW_FW_CFLAGS = -std=c11 -Os -Wall -Wextra -Werror -fshort-wchar -ffreestanding -fno-builtin -fno-strict-aliasing -minline-int-divide-min-latency -D_EFI_P64 -Iinclude -Ithird_party/edk2-ipf/MdePkg/Include -Ithird_party/edk2-ipf/MdePkg/Include/Ipf
build/firmware-new/dxe.fv: build/firmware-new/.edk2 tools/firmware/make_dxe_fv.py tools/firmware/make_fv.py
	python3 tools/firmware/make_dxe_fv.py
build/firmware-new/%.o: firmware/new/%.c firmware/new/hob.h mk/firmware-new.mk
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(NEW_FW_CFLAGS) -ffile-prefix-map=$(CURDIR)=. -MMD -MP -c $< -o $@
	$(CROSS_IA64)objcopy --redefine-sym memcpy=CopyMem $@
build/firmware-new/entry.o: firmware/new/entry.S
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -c $< -o $@
build/firmware-new/fv-blob.o: firmware/new/fv_blob.S build/firmware-new/dxe.fv
	$(CROSS_IA64)gcc -DFW_FV='"build/firmware-new/dxe.fv"' -c $< -o $@
# Release build: the same sources with MDEPKG_NDEBUG, in build/firmware-new/release/.
build/firmware-new/release/dxe.fv: build/firmware-new/release/.edk2 tools/firmware/make_dxe_fv.py tools/firmware/make_fv.py
	python3 tools/firmware/make_dxe_fv.py --release
build/firmware-new/release/fv-blob.o: firmware/new/fv_blob.S build/firmware-new/release/dxe.fv
	$(CROSS_IA64)gcc -DFW_FV='"build/firmware-new/release/dxe.fv"' -c $< -o $@
build/firmware-new/release/%.o: firmware/new/%.c firmware/new/hob.h mk/firmware-new.mk
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(NEW_FW_CFLAGS) -DMDEPKG_NDEBUG -ffile-prefix-map=$(CURDIR)=. -MMD -MP -c $< -o $@
	$(CROSS_IA64)objcopy --redefine-sym memcpy=CopyMem $@
NEW_FW_IPL_LIBNAMES = BasePeCoffLib BaseMemoryLib BaseLib BasePrintLib
NEW_FW_IPL_LIBS_IN = $(foreach lib,$(NEW_FW_IPL_LIBNAMES),$(1)/edk2/VmmDxe/DEBUG_ELFGCC/IPF/MdePkg/Library/$(lib)/$(lib)/$(lib).a) $(1)/edk2/VmmDxe/DEBUG_ELFGCC/IPF/MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull/PeCoffExtraActionLibNull.a
NEW_FW_IPL_LIBS = $(call NEW_FW_IPL_LIBS_IN,build/firmware-new)
build/firmware-new/ia64-firmware-new.elf: build/firmware-new/entry.o build/firmware-new/pal-entry.o build/firmware-new/pale.o build/firmware-new/sal-entry.o build/firmware-new/ivt.o build/firmware-new/ap-entry.o build/firmware-new/ap.o build/firmware-new/unaligned.o build/firmware-new/ipl.o build/firmware-new/hob.o build/firmware-new/debug.o build/firmware-new/fv-blob.o firmware/new/firmware.lds
	$(CROSS_IA64)ld -static --emit-relocs --no-undefined --build-id=none -T firmware/new/firmware.lds -Map=$(@:.elf=.map) -o $@ $(filter %.o,$^) --start-group $(NEW_FW_IPL_LIBS) --end-group
NEW_FW_ASM_OBJS = $(addprefix build/firmware-new/,entry.o pal-entry.o pale.o sal-entry.o ivt.o ap-entry.o)
build/firmware-new/release/ia64-firmware-new.elf: $(NEW_FW_ASM_OBJS) $(addprefix build/firmware-new/release/,ap.o unaligned.o ipl.o hob.o debug.o fv-blob.o) firmware/new/firmware.lds build/firmware-new/release/.edk2
	$(CROSS_IA64)ld -static --emit-relocs --no-undefined --build-id=none -T firmware/new/firmware.lds -Map=$(@:.elf=.map) -o $@ $(filter %.o,$^) --start-group $(call NEW_FW_IPL_LIBS_IN,build/firmware-new/release) --end-group
-include $(wildcard build/firmware-new/*.d build/firmware-new/release/*.d)

build/firmware-new/ivt.o: firmware/new/ivt.S firmware/new/ivt-layout.h
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -c $< -o $@
build/firmware-new/pal-entry.o: firmware/new/pal/entry.S
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -c $< -o $@
build/firmware-new/pale.o: firmware/new/pal/pale.S
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -c $< -o $@
build/firmware-new/sal-entry.o: firmware/new/sal/entry.S firmware/new/sal/mc.h
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -c $< -o $@
build/firmware-new/ap-entry.o: firmware/new/ap.S firmware/new/ap.h
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -c $< -o $@

.PHONY: test-firmware-fvb
TEST_TARGETS += test-firmware-fvb
TEST_TARGETS += test-firmware-timing
TEST_TARGETS += test-firmware-console
TEST_TARGETS += test-firmware-runtime
TEST_TARGETS += test-firmware-tables
TEST_TARGETS += test-firmware-sal
TEST_TARGETS += test-firmware-unaligned
.PHONY: test-firmware-unaligned
test-firmware-unaligned: build/tests/firmware-unaligned
	./build/tests/firmware-unaligned
build/tests/firmware-unaligned: tests/firmware/unaligned.c firmware/new/unaligned.c firmware/new/unaligned.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -DUNALIGNED_HOST_TEST -I. $(filter %.c,$^) -o $@
.PHONY: test-firmware-sal
test-firmware-sal: build/tests/firmware-sal
	./build/tests/firmware-sal
build/tests/firmware-sal: firmware/new/boot-translation.h tests/firmware/sal.c firmware/new/sal/sal.c firmware/new/sal/table.c firmware/new/sal/sal.h firmware/new/sal/mc.h firmware/new/acpi/tables.h firmware/new/ap.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -I. -Iinclude $(filter %.c,$^) -o $@
.PHONY: test-firmware-tables
test-firmware-tables: build/tests/firmware-tables
	python3 tests/firmware/check_tables.py
build/tests/firmware-tables: tests/firmware/acpi-map.h tests/firmware/tables.c firmware/new/acpi/tables.c firmware/new/acpi/aml.c firmware/new/acpi/tables.h firmware/new/smbios/smbios.c firmware/new/smbios/smbios.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -D_EFI_P64 -I. -Iinclude -Ithird_party/edk2-ipf/MdePkg/Include -Ithird_party/edk2-ipf/MdePkg/Include/Ipf $(filter %.c,$^) -o $@
.PHONY: test-firmware-runtime
test-firmware-runtime: build/tests/firmware-runtime
	./build/tests/firmware-runtime
build/tests/firmware-runtime: tests/firmware/runtime.c firmware/new/dxe/runtime.c firmware/new/platform.h firmware/new/legacy-window.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -fshort-wchar -D_EFI_P64 -ffunction-sections -fdata-sections -I. -Iinclude -Ithird_party/edk2-ipf/MdePkg/Include -Ithird_party/edk2-ipf/MdePkg/Include/Ipf -Ithird_party/edk2-ipf/MdeModulePkg/Include $< -Wl,--gc-sections -o $@
.PHONY: test-firmware-console
test-firmware-console: build/tests/firmware-console
	./build/tests/firmware-console
build/tests/firmware-console: tests/firmware/console.c firmware/new/dxe/console.c firmware/new/platform.h firmware/new/vga-text.h firmware/new/legacy-bios.h firmware/new/legacy-handoff.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -D_EFI_P64 -ffunction-sections -fdata-sections -I. -Iinclude -Ithird_party/edk2-ipf/MdePkg/Include -Ithird_party/edk2-ipf/MdePkg/Include/Ipf -Ithird_party/edk2-ipf/MdeModulePkg/Include $< -Wl,--gc-sections -o $@
.PHONY: test-firmware-timing
test-firmware-timing: build/tests/firmware-timing
	./build/tests/firmware-timing
build/tests/firmware-timing: tests/firmware/timing.c firmware/new/timing.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -I. $< -o $@
test-firmware-fvb: build/tests/firmware-fvb
	./build/tests/firmware-fvb
build/tests/firmware-fvb: tests/firmware/fvb.c firmware/new/dxe/fvb.c firmware/new/platform.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -D_EFI_P64 -ffunction-sections -fdata-sections -I. -Iinclude -Ithird_party/edk2-ipf/MdePkg/Include -Ithird_party/edk2-ipf/MdePkg/Include/Ipf -Ithird_party/edk2-ipf/MdeModulePkg/Include $< -Wl,--gc-sections -o $@

.PHONY: test-firmware-ap
TEST_TARGETS += test-firmware-ap
test-firmware-ap: build/tests/firmware-ap
	./build/tests/firmware-ap
build/tests/firmware-ap: tests/firmware/ap.c firmware/new/sal/sal.c firmware/new/sal/runtime.c firmware/new/sal/sal.h firmware/new/ap.h firmware/new/ivt-layout.h firmware/new/acpi/tables.h firmware/new/platform.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -fshort-wchar -D_EFI_P64 -I. -Iinclude -Ithird_party/edk2-ipf/MdePkg/Include -Ithird_party/edk2-ipf/MdePkg/Include/Ipf -Ithird_party/edk2-ipf/MdeModulePkg/Include $< -o $@

.PHONY: test-firmware-cpu-tables
TEST_TARGETS += test-firmware-cpu-tables
test-firmware-cpu-tables: build/tests/firmware-cpu-tables
	./build/tests/firmware-cpu-tables
build/tests/firmware-cpu-tables: tests/firmware/acpi-map.h tests/firmware/cpu-tables.c firmware/new/acpi/tables.c firmware/new/acpi/aml.c firmware/new/acpi/tables.h firmware/new/smbios/smbios.c firmware/new/smbios/smbios.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -I. -Iinclude $(filter %.c,$^) -o $@

.PHONY: test-firmware-pci
TEST_TARGETS += test-firmware-pci
test-firmware-pci: build/tests/firmware-pci
	./build/tests/firmware-pci
build/tests/firmware-pci: tests/firmware/pci.c firmware/new/pci/root.c firmware/new/platform.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -D_EFI_P64 -ffunction-sections -fdata-sections -I. -Iinclude -Ithird_party/edk2-ipf/MdePkg/Include -Ithird_party/edk2-ipf/MdePkg/Include/Ipf -Ithird_party/edk2-ipf/MdeModulePkg/Include $< -Wl,--gc-sections -o $@

.PHONY: test-firmware-srat
TEST_TARGETS += test-firmware-srat
test-firmware-srat: build/tests/firmware-srat build/tests/firmware-acpi-map
	./build/tests/firmware-srat
	./build/tests/firmware-acpi-map
build/tests/firmware-srat: tests/host/firmware/srat.c firmware/new/acpi/tables.c firmware/new/acpi/aml.c firmware/new/acpi/tables.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -D_EFI_P64 -I. -Iinclude -Ithird_party/edk2-ipf/MdePkg/Include -Ithird_party/edk2-ipf/MdePkg/Include/Ipf $(filter %.c,$^) -o $@
build/tests/firmware-acpi-map: tests/host/firmware/acpi-map.c firmware/new/acpi/driver.c firmware/new/acpi/tables.c firmware/new/acpi/aml.c firmware/new/acpi/tables.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -D_EFI_P64 -ffunction-sections -fdata-sections -I. -Iinclude -Ithird_party/edk2-ipf/MdePkg/Include -Ithird_party/edk2-ipf/MdePkg/Include/Ipf -Ithird_party/edk2-ipf/MdeModulePkg/Include $(filter-out firmware/new/acpi/driver.c,$(filter %.c,$^)) -Wl,--gc-sections -o $@
