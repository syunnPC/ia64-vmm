# SPDX-License-Identifier: GPL-2.0-or-later
# The foundation wildcard includes engine/*/*.c, but not the dispatcher.
SHARED_SOURCES += engine/engine.c
AVAILABLE_TESTS += ia64
IA64_SOURCES = $(wildcard ia64/*.c ia64/decode/*.c ia64/ia32/*.c engine/interp/*.c engine/cache/*.c engine/tier0/*.c) engine/engine.c
SHARED_SOURCES += $(wildcard ia64/decode/*.c)
IA64_TEST_SOURCES = tests/ia64/main.c tests/ia64/fp-test.c tests/ia64/hostfp-test.c $(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c
IA64_TEST_OBJECTS = $(addprefix build/hosted/,$(IA64_TEST_SOURCES:.c=.o))
IA64_TEST_ASM = $(filter-out tests/ia64/model-absent.S,$(wildcard tests/ia64/*.S))
IA64_TEST_IMAGES = $(patsubst tests/ia64/%.S,build/tests/ia64/%.bin,$(IA64_TEST_ASM))

test-ia64:: build/ia64-test $(IA64_TEST_IMAGES)
	@mkdir -p build/tests/ia64
	@./build/ia64-test $(IA64_TEST_IMAGES) 2>build/tests/ia64/stderr || { cat build/tests/ia64/stderr >&2; exit 1; }
	@./build/ia64-test --engine=tier0 $(IA64_TEST_IMAGES) 2>build/tests/ia64/tier0.stderr || { cat build/tests/ia64/tier0.stderr >&2; exit 1; }
	@./build/ia64-test --engine=lockstep $(IA64_TEST_IMAGES) 2>build/tests/ia64/lockstep.stderr || { cat build/tests/ia64/lockstep.stderr >&2; exit 1; }
build/ia64-test: $(IA64_TEST_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(IA64_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -Wl,--wrap=hal_counter,--wrap=hal_counter_hz,--wrap=hal_host_simd -lm -o $@
build/hosted/tests/ia64/fp-test.o: CFLAGS += -frounding-math
build/hosted/tests/ia64/fp-test.o: build/tests/ia64/fp-compiler.h
build/tests/ia64/fp-compiler.h: tests/ia64/fp-guest.c tests/ia64/fp-build.py
	CROSS_IA64='$(CROSS_IA64)' python3 tests/ia64/fp-build.py $@
build/tests/ia64/%.o: tests/ia64/%.S
	@mkdir -p $(@D)
	$(CROSS_IA64)as $< -o $@
build/tests/ia64/%.elf: build/tests/ia64/%.o
	$(CROSS_IA64)ld -Ttext=0x100000 -e _start $< -o $@
build/tests/ia64/%.bin: build/tests/ia64/%.elf
	$(CROSS_IA64)objcopy -O binary $< $@

.PHONY: bench-ia64 ia64-coverage test-ia64-oracle
test-ia64-oracle: build/ia64-test $(IA64_TEST_IMAGES)
	QEMU_IA64='$(QEMU_IA64)' sh tests/ia64/oracle.sh
bench-ia64: build/ia64-test build/tests/ia64/speed.bin
	./build/ia64-test --code-validity=$(if $(CODE_VALIDITY),$(CODE_VALIDITY),compare-always) --bench build/tests/ia64/speed.bin
	./build/ia64-test --engine=tier0 --code-validity=$(if $(CODE_VALIDITY),$(CODE_VALIDITY),compare-always) --bench build/tests/ia64/speed.bin
ia64-coverage: build/ia64-test
	./build/ia64-test --coverage
-include $(IA64_TEST_OBJECTS:.o=.d)

# CPU model tests: link the production Tier1 glue and Linux window shim for the
# same profile/negative-instruction suite under all three engines.
TEST_TARGETS += test-ia32-model
IA32_MODEL_OBJECTS = build/hosted/tests/ia64/model-tier1.o $(patsubst %.c,build/hosted/%.o,$(wildcard engine/tier1/*.c engine/hostwin/*.c)) build/hosted/tests/tier1/hostwin_linux.o
build/ia32-model-test: $(IA64_TEST_OBJECTS) $(IA32_MODEL_OBJECTS)
	$(HOSTCC) $^ $(HOSTED_LDFLAGS) $(LDFLAGS) -Wl,--wrap=hal_counter,--wrap=hal_counter_hz,--wrap=hal_host_simd -lm -o $@
.PHONY: test-ia32-model
test-ia32-model: build/ia32-model-test build/tests/ia64/model-absent.bin
	@for engine in interp tier0 tier1; do ./build/ia32-model-test --engine=$$engine --model-tests build/tests/ia64/model-absent.bin || exit 1; done
# The model fixture must rebuild with the private vCPU layout too.
-include $(IA32_MODEL_OBJECTS:.o=.d)
