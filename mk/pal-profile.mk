# SPDX-License-Identifier: GPL-2.0-or-later
TEST_TARGETS += test-pal-profile test-pal-events
# INIT, PMI and machine check paths of the native PAL/SAL.
.PHONY: test-pal-events
test-pal-events: build/tests/pal-events build/tests/pal-events-native.elf
	@for model in montecito madison; do for engine in interp tier0 lockstep; do \
	    ./build/tests/pal-events build/tests/pal-events-native.elf $$engine $$model || exit 1; \
	done; done
build/tests/pal-events-native.elf: build/firmware-new/pal-entry.o build/firmware-new/pale.o build/firmware-new/sal-entry.o tests/firmware/pal-events.lds
	@mkdir -p $(@D)
	$(CROSS_IA64)ld -T tests/firmware/pal-events.lds $(filter %.o,$^) -o $@
build/tests/pal-events: tests/firmware/pal-events.c firmware/new/sal/mc.h $(LOADER_IVT_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< $(LOADER_IVT_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
.PHONY: test-pal-profile
test-pal-profile: build/tests/pal-profile build/tests/pal-profile-native.elf
	./build/tests/pal-profile build/tests/pal-profile-native.elf
build/tests/pal-profile-native.elf: build/firmware-new/pal-entry.o tests/loaders/pal.lds
	@mkdir -p $(@D)
	$(CROSS_IA64)ld -T tests/loaders/pal.lds $< -o $@
build/tests/pal-profile: tests/firmware/pal-profile.c $(LOADER_IVT_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< $(LOADER_IVT_OBJECTS) $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
.PHONY: test-pal-profile-oracle
# Boots the configured IA-64 fork for every vector (minutes); not in make test.
test-pal-profile-oracle:
	python3 -I tests/firmware/pal_oracle.py
