# SPDX-License-Identifier: GPL-2.0-or-later
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
include mk/config.mk

# Serialize an explicit clean with all other requested goals even under -j.
ifneq ($(and $(filter clean,$(MAKECMDGOALS)),$(filter-out clean,$(MAKECMDGOALS))),)
.PHONY: $(MAKECMDGOALS) clean-then-build
$(MAKECMDGOALS): clean-then-build
clean-then-build:
	rm -rf build
	+$(MAKE) $(filter-out clean,$(MAKECMDGOALS))
else
SHARED_SOURCES = $(filter-out core/string.c,$(wildcard core/*.c platform/*.c ia64/*.c engine/*/*.c))
BM_SOURCES = $(SHARED_SOURCES) $(wildcard core/string.c boot/*.c host/amd64/*.c host/amd64/*.S host/fb/*.c)
BM_OBJECTS = $(addprefix build/bm/,$(patsubst %.S,%.o,$(BM_SOURCES:.c=.o)))
CORE_BM_OBJECTS = $(addprefix build/bm/,$(SHARED_SOURCES:.c=.o)) build/bm/core/string.o
TEST_HOST_SOURCES = $(wildcard tests/host/*.c) core/log.c core/guest_mem.c core/options.c tests/hal/hal_linux.c
TEST_HOST_OBJECTS = $(addprefix build/hosted/,$(TEST_HOST_SOURCES:.c=.o)) build/hosted/tests/host/vmm_string.o

# Modules may add double-colon test recipes, or register prerequisite targets
# in TEST_*_TARGETS. Set AVAILABLE_TESTS to suppress a placeholder recipe.
include $(filter-out mk/config.mk mk/tier1.mk mk/helper.mk,$(wildcard mk/*.mk))
include mk/tier1.mk
include mk/helper.mk

.PHONY: all vmm efi bootdisk test test-host test-ia64 test-qemu check-freestanding clean FORCE
all: vmm firmware efi

# A flag stamp invalidates the unit-test objects when switching SAN or TSAN in either direction.
build/hosted/.flags: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(HOSTED_LDFLAGS) $(LDFLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
build/bm/.flags: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(HOSTCC) $(BM_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(BM_GPR_ONLY_SOURCES)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
# core/string.c implements memcpy and friends: GCC must not turn its loops
# back into calls to themselves (infinite recursion when linked into a unit test).
build/hosted/core/string.o build/bm/core/string.o: CFLAGS += -fno-builtin -fno-tree-loop-distribute-patterns
# The VMM's core/string.c for tests/host/string.c: its product ISA flags,
# every function renamed vmm_* beside the C library's.
VMM_STRING_RENAMES = $(foreach f,memcpy memset memmove memcmp strlen strcmp strncmp,-D$(f)=vmm_$(f))
build/hosted/tests/host/vmm_string.o: core/string.c include/ia64vmm/string.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude -I. -ffreestanding -fno-builtin \
		-fno-tree-loop-distribute-patterns $(BM_SIMD_CFLAGS) -fno-stack-protector -U_FORTIFY_SOURCE \
		$(VMM_STRING_RENAMES) $(call BM_GPR_ONLY_CFLAGS,core/string.c) -c $< -o $@
build/hosted/%.o: %.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
build/bm/%.o: %.c build/bm/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(BM_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(if $(filter boot/%,$<),-Ithird_party/limine) $(call BM_GPR_ONLY_CFLAGS,$<) -MMD -MP -c $< -o $@
build/bm/%.o: %.S build/bm/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(BM_CFLAGS) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

ifneq ($(wildcard host/amd64/linker.ld),)
vmm: build/vmm.elf
build/vmm.elf: $(BM_OBJECTS) host/amd64/linker.ld
	$(HOSTCC) -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-T,host/amd64/linker.ld -Wl,-Map,$@.map $(BM_OBJECTS) $(BM_LDFLAGS) -o $@
else
vmm:
	@echo 'vmm: host/amd64/linker.ld not available yet'
endif
ifeq ($(wildcard mk/firmware.mk),)
.PHONY: firmware
firmware:
	@echo 'firmware: not available yet'
endif

test: test-host test-ia64 test-qemu test-ia32-contracts $(TEST_TARGETS)
.PHONY: test-ia32-contracts
test-ia32-contracts:
	@HOSTCC='$(HOSTCC)' BM_SIMD_CFLAGS='$(BM_SIMD_CFLAGS)' host/amd64/tests/check-ia32-contracts.sh
test-host: build/tests/host
	@./build/tests/host 2>build/tests/host.stderr || { cat build/tests/host.stderr >&2; exit 1; }
build/tests/host: $(TEST_HOST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(TEST_HOST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
# The threaded hosted HAL mode is
# process-wide, so its test is a binary of its own.
HAL_SMP_TEST_SOURCES = tests/host/smp/hal.c core/log.c tests/hal/hal_linux.c
HAL_SMP_TEST_OBJECTS = $(addprefix build/hosted/,$(HAL_SMP_TEST_SOURCES:.c=.o))
.PHONY: test-hal-smp
test-host: test-hal-smp
test-hal-smp: build/tests/hal-smp
	@IA64VMM_HOSTED_IDLE=halt ./build/tests/hal-smp 2>build/tests/hal-smp.stderr || { cat build/tests/hal-smp.stderr >&2; exit 1; }
	@IA64VMM_HOSTED_IDLE=monitor ./build/tests/hal-smp 2>build/tests/hal-smp.stderr || { cat build/tests/hal-smp.stderr >&2; exit 1; }
build/tests/hal-smp: $(HAL_SMP_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HAL_SMP_TEST_OBJECTS) -pthread -Wl,--wrap=syscall -Wl,--wrap=clock_gettime $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
test-ia64:: $(TEST_IA64_TARGETS)
	@$(if $(filter ia64,$(AVAILABLE_TESTS))$(TEST_IA64_TARGETS),:,echo 'test-ia64: not available yet')
test-qemu:: $(TEST_QEMU_TARGETS)
	@$(if $(filter qemu,$(AVAILABLE_TESTS))$(TEST_QEMU_TARGETS),:,echo 'test-qemu: not available yet')
check-freestanding: $(CORE_BM_OBJECTS) check-gpr-only
	@sh mk/check-freestanding.sh '$(HOSTNM)' $(CORE_BM_OBJECTS)
# General-registers-only paths of the linked image (mk/check-gpr-only.py).
# Assembly is checked too; the selftest's SIMD harness is not on these paths.
GPR_ONLY_OBJECTS = $(addprefix build/bm/,$(BM_GPR_ONLY_SOURCES:.c=.o) \
	$(patsubst %.S,%.o,$(filter-out host/amd64/tests/simd_access.S,$(filter %.S,$(BM_SOURCES)))))
.PHONY: check-gpr-only
check-gpr-only: build/vmm.elf
	@python3 -I mk/check-gpr-only.py '$(HOSTOBJDUMP)' build/vmm.elf build/vmm.elf.map $(GPR_ONLY_OBJECTS)
clean:
	rm -rf build
FORCE:
-include $(BM_OBJECTS:.o=.d) $(TEST_HOST_OBJECTS:.o=.d) $(HAL_SMP_TEST_OBJECTS:.o=.d)
endif
