# SPDX-License-Identifier: GPL-2.0-or-later
# Offline PCI expansion ROM parser (tools/vbios); synthetic images only.
.PHONY: test-pci-rom
test-host: test-pci-rom
test-pci-rom:
	@python3 tools/vbios/test_pci_rom.py

# Offline replay of port_trace logs into the VGA register file (tools/vbios).
.PHONY: test-vga-ports
test-host: test-vga-ports
test-vga-ports:
	@python3 tools/vbios/test_vga_ports.py

# Hosted checks of the
# SVM and VMX containers' structures and of the VBIOS environment, each with
# broken variants. The VBIOS test replaces the HAL's container with a script.
.PHONY: test-vbios test-vbios-qemu
test-host: test-vbios
# legacy VGA: broken variants of the VMM's guest side (platform/vbios.c) must
# VMX: broken variants of the VMX backend's pure parts and of
SVM_TEST_SOURCES := host/amd64/tests/svm.c host/amd64/svm_layout.c
SVM_TEST_OBJECTS := $(addprefix build/hosted/,$(SVM_TEST_SOURCES:.c=.o))
VMX_TEST_SOURCES := host/amd64/tests/vmx.c host/amd64/vmx_layout.c
VMX_TEST_OBJECTS := $(addprefix build/hosted/,$(VMX_TEST_SOURCES:.c=.o))
VBIOS_TEST_SOURCES = platform/tests/vbios.c $(wildcard platform/*.c) $(IA64_SOURCES) persist/log.c persist/media.c persist/fat32.c persist/nvram.c persist/protocol.c core/log.c core/guest_mem.c tests/hal/hal_linux.c
VBIOS_TEST_OBJECTS = $(addprefix build/hosted/,$(VBIOS_TEST_SOURCES:.c=.o))
VBIOS_TEST_WRAPS := hal_x86rm_probe hal_x86rm_info hal_x86rm_create hal_x86rm_reclaim_create hal_x86rm_run hal_x86rm_destroy hal_console_vga_text hal_counter hal_idle_until hal_port_in hal_port_out hal_irq_save hal_irq_restore hal_checkpoint
build/tests/svm: $(SVM_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(SVM_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
build/tests/vmx: $(VMX_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(VMX_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
build/tests/vbios: $(VBIOS_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(VBIOS_TEST_OBJECTS) $(foreach w,$(VBIOS_TEST_WRAPS),-Wl,--wrap=$(w)) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
test-vbios: build/tests/svm build/tests/vmx build/tests/vbios
	@./build/tests/svm
	@./build/tests/vmx
	@./build/tests/vbios 2>build/tests/vbios.stderr || { cat build/tests/vbios.stderr >&2; exit 1; }
	@./build/tests/vbios stubs build/tests/vbios-stubs.bin
	@./build/tests/vbios stubs-vmx build/tests/vbios-stubs-vmx.bin
	@python3 tools/vbios/test_stubs.py build/tests/vbios-stubs.bin build/tests/vbios-stubs-vmx.bin
-include $(SVM_TEST_OBJECTS:.o=.d) $(VMX_TEST_OBJECTS:.o=.d) build/hosted/platform/tests/vbios.d

# QEMU TCG -cpu max (wsl-qemu, --machine pc): POST the std-vga SeaVGABIOS and
# the Cirrus one, the cli-loop bound proof, and the no-SVM setup failure. Not part
# of `make test` (about four VM-minutes under TCG).
test-vbios-qemu: all esp
	python3 tools/vbios/qemu_check.py $(VBIOS_QEMU_ARGS)

# legacy VGA: register-model test of the new firmware's display=vbios code
# (the tail check both firmwares use, the text console core, VmmConsole) and
# the sweep showing broken variants of it and of hob.c fail their tests.
.PHONY: test-firmware-legacy-bios
TEST_TARGETS += test-firmware-legacy-bios
test-firmware-legacy-bios: build/tests/firmware-legacy-bios
	@./build/tests/firmware-legacy-bios
build/tests/firmware-legacy-bios: tests/firmware/legacy_bios.c firmware/new/dxe/console.c firmware/new/legacy-bios.h firmware/new/legacy-handoff.h firmware/new/vga-text.h include/ia64vmm/platform.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -fshort-wchar -D_EFI_P64 -ffunction-sections -fdata-sections -I. -Iinclude -Ithird_party/edk2-ipf/MdePkg/Include -Ithird_party/edk2-ipf/MdePkg/Include/Ipf -Ithird_party/edk2-ipf/MdeModulePkg/Include $< -Wl,--gc-sections -o $@

.PHONY: test-virtualization
test-host: test-virtualization
build/tests/virtualization: host/amd64/tests/virtualization.c host/amd64/virtualization.c host/amd64/virtualization_hw.h host/amd64/ia32.h
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude -I. host/amd64/tests/virtualization.c host/amd64/virtualization.c -o $@
test-virtualization: build/tests/virtualization
	@./build/tests/virtualization
