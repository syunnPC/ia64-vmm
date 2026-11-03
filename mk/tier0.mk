# SPDX-License-Identifier: GPL-2.0-or-later
# Per-instruction diagnostics are opt-in; the flag stamp rebuilds both HALs.
TIER0_STATS ?= 0
CPPFLAGS += -DIA64VMM_TIER0_STATS=$(TIER0_STATS)
TIER0_TEST_SOURCES = tests/tier0/main.c $(IA64_SOURCES) core/log.c core/guest_mem.c tests/hal/hal_linux.c
TIER0_TEST_OBJECTS = $(addprefix build/hosted/,$(TIER0_TEST_SOURCES:.c=.o))
TEST_TARGETS += test-tier0
# staging must wait for both modules, including parallel `make esp`.
build/esp.img: build/firmware.elf build/bootdisk.img
.PHONY: test-tier0
test-tier0: build/tier0-test test-snapshot-reserve
	@mkdir -p build/tests/tier0
	@./build/tier0-test 2>build/tests/tier0/stderr || { cat build/tests/tier0/stderr >&2; exit 1; }
	@tail -1 build/tests/tier0/stderr
# --wrap counts exact-helper calls and translation-cache fills from
# generated code (tests/tier0/native.inc) and drives the host counter of the
# timer polls (tests/tier0/timer.inc) in binaries built from main.c.
TIER0_TEST_WRAP = -Wl,--wrap=tier0_helper -Wl,--wrap=tier0_data_pointer \
	-Wl,--wrap=tier0_store_tracked -Wl,--wrap=hal_counter -Wl,--wrap=ia64_timer_poll
build/tier0-test: $(TIER0_TEST_OBJECTS) build/hosted/.flags
	$(HOSTCC) $(TIER0_TEST_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) $(TIER0_TEST_WRAP) -o $@
-include $(TIER0_TEST_OBJECTS:.o=.d)

.PHONY: test-snapshot-reserve
test-snapshot-reserve: build/tests/snapshot-reserve
	./build/tests/snapshot-reserve
build/tests/snapshot-reserve: tests/tier0/reserve.c $(filter-out build/hosted/tests/tier0/main.o,$(TIER0_TEST_OBJECTS)) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< $(filter %.o,$^) -Wl,--wrap=hal_alloc_pages $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
