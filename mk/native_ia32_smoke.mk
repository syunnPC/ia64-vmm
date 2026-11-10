# SPDX-License-Identifier: GPL-2.0-or-later
# Opt-in native hardware tests; neither target builds IA-64 firmware.
NATIVE_SMOKE_ROOT := build/native-smoke
NATIVE_IA32_QEMU_ARGS ?=
NATIVE_SMOKE_C := core/log.c core/guest_mem.c core/options.c core/string.c \
	host/amd64/tests/native_smoke.c $(wildcard boot/*.c host/amd64/*.c host/fb/*.c)
NATIVE_SMOKE_S := $(wildcard boot/*.S host/amd64/*.S)
NATIVE_SMOKE_O := $(addprefix $(NATIVE_SMOKE_ROOT)/,$(NATIVE_SMOKE_C:.c=.o) $(NATIVE_SMOKE_S:.S=.o))
NATIVE_ENGINE_SMOKE_O = $(filter-out build/bm/core/vm.o build/bm/boot/limine.o,$(BM_OBJECTS)) \
	$(NATIVE_SMOKE_ROOT)/boot/limine.o $(NATIVE_SMOKE_ROOT)/native_engine_smoke.o
.PHONY: native-ia32-smoke test-native-ia32-qemu test-native-ia32-engine-qemu
native-ia32-smoke: $(NATIVE_SMOKE_ROOT)/vmm.elf
$(NATIVE_SMOKE_ROOT)/.flags: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(HOSTCC) $(BM_CFLAGS) $(CPPFLAGS) $(CFLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
$(NATIVE_SMOKE_ROOT)/%.o: %.c $(NATIVE_SMOKE_ROOT)/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(BM_CFLAGS) $(CPPFLAGS) $(CFLAGS) -DIA64VMM_AMD64_HOST_ONLY=1 \
		-Ithird_party/limine -mgeneral-regs-only -ffunction-sections -fdata-sections \
		-fno-tree-loop-distribute-patterns -MMD -MP -c $< -o $@
$(NATIVE_SMOKE_ROOT)/%.o: %.S $(NATIVE_SMOKE_ROOT)/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(BM_CFLAGS) $(CPPFLAGS) $(CFLAGS) -DIA64VMM_AMD64_HOST_ONLY=1 \
		-Ithird_party/limine -ffunction-sections -fdata-sections -MMD -MP -c $< -o $@
$(NATIVE_SMOKE_ROOT)/native_engine_smoke.o: host/amd64/tests/native_engine_smoke.c $(NATIVE_SMOKE_ROOT)/.flags
	$(HOSTCC) $(BM_CFLAGS) $(CPPFLAGS) $(CFLAGS) -mgeneral-regs-only -MMD -MP -c $< -o $@
$(NATIVE_SMOKE_ROOT)/vmm.elf: $(NATIVE_SMOKE_O) host/amd64/linker.ld
	$(HOSTCC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,--gc-sections \
		-Wl,-T,host/amd64/linker.ld $(NATIVE_SMOKE_O) $(BM_LDFLAGS) -o $@
$(NATIVE_SMOKE_ROOT)/native-engine.elf: $(NATIVE_ENGINE_SMOKE_O) host/amd64/linker.ld
	$(HOSTCC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-T,host/amd64/linker.ld \
		$(NATIVE_ENGINE_SMOKE_O) $(BM_LDFLAGS) -o $@
test-native-ia32-qemu: $(NATIVE_SMOKE_ROOT)/vmm.elf third_party/limine/BOOTX64.EFI
	python3 -I host/amd64/tests/native_smoke.py $(NATIVE_IA32_QEMU_ARGS)
test-native-ia32-engine-qemu: $(NATIVE_SMOKE_ROOT)/native-engine.elf third_party/limine/BOOTX64.EFI
	python3 -I host/amd64/tests/native_smoke.py --engine $(NATIVE_IA32_QEMU_ARGS)
-include $(NATIVE_SMOKE_O:.o=.d) $(NATIVE_SMOKE_ROOT)/native_engine_smoke.d
