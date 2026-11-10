# SPDX-License-Identifier: GPL-2.0-or-later
TEST_TARGETS += test-native-ia32
test-host: test-native-ia32
.PHONY: test-native-ia32 test-native-svm test-native-vmx
NATIVE_IA32_TEST_FLAGS = -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude -I.
NATIVE_IA32_HEADERS = host/amd64/ia32.h host/amd64/ia32_native.h host/amd64/ia32_native_mode.h host/amd64/ia32_svm.h \
	host/amd64/svm.h host/amd64/simd.h include/ia64vmm/ia32.h
build/tests/native-svm-layout: host/amd64/tests/ia32_svm_layout.c host/amd64/ia32_svm_layout.c $(NATIVE_IA32_HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) $(NATIVE_IA32_TEST_FLAGS) $(filter %.c,$^) -o $@
build/tests/native-fp-layout: host/amd64/tests/ia32_fp.c host/amd64/ia32_fp_layout.c $(NATIVE_IA32_HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) $(NATIVE_IA32_TEST_FLAGS) $(filter %.c,$^) -o $@
build/tests/native-svm: host/amd64/tests/ia32_svm.c host/amd64/ia32_svm.c host/amd64/ia32_svm_layout.c host/amd64/virtualization.c host/amd64/ia32_fp.c host/amd64/ia32_fp_layout.c host/amd64/simd.c $(NATIVE_IA32_HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) $(NATIVE_IA32_TEST_FLAGS) $(filter %.c,$^) -o $@
build/tests/native-fp-switch: host/amd64/tests/ia32_fp_switch.c host/amd64/tests/ia32_fp_switch.S host/amd64/ia32_fp.c host/amd64/ia32_fp_layout.c host/amd64/ia32_fp_switch.S host/amd64/simd.c host/amd64/xstate.S $(NATIVE_IA32_HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) $(NATIVE_IA32_TEST_FLAGS) -mgeneral-regs-only $(filter %.c %.S,$^) -o $@
build/tests/native-map-layout: host/amd64/tests/ia32_native.c host/amd64/ia32_native_layout.c host/amd64/ia32_native.c $(NATIVE_IA32_HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) $(NATIVE_IA32_TEST_FLAGS) $(filter %.c,$^) -o $@
build/tests/native-vmx-layout: host/amd64/tests/vmx_ia32.c host/amd64/vmx_ia32_layout.c host/amd64/vmx_ia32.h host/amd64/vmx.h $(NATIVE_IA32_HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) $(NATIVE_IA32_TEST_FLAGS) $(filter %.c,$^) -o $@
build/tests/native-vmx: host/amd64/tests/vmx_ia32_lifecycle.c host/amd64/vmx_ia32.inc host/amd64/vmx_ia32_layout.c host/amd64/vmx_layout.c host/amd64/ia32_native.c host/amd64/ia32_native_layout.c host/amd64/vmx_ia32.h host/amd64/vmx.h $(NATIVE_IA32_HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) $(NATIVE_IA32_TEST_FLAGS) -ffunction-sections -fdata-sections $(filter %.c,$^) -Wl,--gc-sections -o $@
test-native-svm: build/tests/native-svm-layout build/tests/native-fp-layout build/tests/native-svm build/tests/native-fp-switch
	@./build/tests/native-svm-layout
	@./build/tests/native-fp-layout
	@./build/tests/native-svm
	@./build/tests/native-fp-switch
test-native-vmx: build/tests/native-vmx-layout build/tests/native-vmx
	@./build/tests/native-vmx-layout
	@./build/tests/native-vmx
test-native-ia32: test-native-svm test-native-vmx build/tests/native-map-layout
	@./build/tests/native-map-layout
