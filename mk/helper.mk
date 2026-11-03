# SPDX-License-Identifier: GPL-2.0-or-later
HELPER_TEST_SOURCES = tests/host/helper/queue.c core/helper.c core/log.c tests/hal/hal_linux.c
HELPER_TEST_OBJECTS = $(addprefix build/hosted/,$(HELPER_TEST_SOURCES:.c=.o))
.PHONY: test-helper
test-host: test-helper
test-helper: build/tests/helper
	./build/tests/helper
build/tests/helper: $(HELPER_TEST_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HELPER_TEST_OBJECTS) -pthread -Wl,--wrap=hal_helper_rank,--wrap=hal_helper_count $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include $(HELPER_TEST_OBJECTS:.o=.d)

HELPER_T1_OBJECTS = $(filter-out build/hosted/tests/tier1/main.o,$(T1_TEST_OBJECTS)) \
	build/hosted/tests/host/helper/tier1.o build/hosted/core/helper.o build/hosted/core/smp.o \
	$(addprefix build/hosted/,$(patsubst %.c,%.o,$(wildcard platform/*.c) persist/log.c persist/media.c persist/fat32.c persist/nvram.c persist/protocol.c))
.PHONY: test-helper-tier1
test-tier1: test-helper-tier1
test-helper-tier1: build/tests/helper-tier1 $(T1_TEST_IMAGES)
	HELPER_CONTRACTS=yes ./build/tests/helper-tier1 helper $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 management $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 helper-icount $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 helper-concurrent $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 helper-parallel $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 helper-many $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 management-many $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 helper-delay-race $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 management-delay-race $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 helper-pressure $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 management-pressure $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 synchronous-pressure $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 helper-spans $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 management-spans $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 helper-spans-concurrent $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 helper-spans-parallel $(T1_TEST_IMAGES)
	./build/tests/helper-tier1 helper-spans-delay-race $(T1_TEST_IMAGES)
build/tests/helper-tier1: $(HELPER_T1_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HELPER_T1_OBJECTS) -pthread -Wl,--wrap=t1_compile_step -Wl,--wrap=hal_counter \
		-Wl,--wrap=code_page_tracked_fast -Wl,--wrap=t1_region_from_link \
		-Wl,--wrap=hal_code_set_writable $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include build/hosted/tests/host/helper/tier1.d

# Concurrent Tier1 executors share the translation cache and compiler helper.
SMP_T1_OBJECTS = $(filter-out build/hosted/tests/host/helper/tier1.o,$(HELPER_T1_OBJECTS)) \
	build/hosted/tests/host/smp/tier1.o build/hosted/tests/host/smp/tier1_probe.o
SMP_T1_MODES = shared xmod pair kick flag sync2 parked ptcg purge pin width chainxmod chainkick chainpurge probe checked checked-walk checked-partial checked-shared checked-long checked-long-be checked-sampled checked-fp
.PHONY: test-smp-tier1
test-tier1: test-smp-tier1
test-smp-tier1: build/tests/smp-tier1 $(T1_TEST_IMAGES)
	@for m in $(SMP_T1_MODES); do ./build/tests/smp-tier1 $$m $(T1_TEST_IMAGES) || exit 1; done
	@for m in load store drop gpa extra; do T1_CHECKED_MUTATION=$$m ./build/tests/smp-tier1 checked $(T1_TEST_IMAGES) || exit 1; done
	@T1_CHECKED_MUTATION=load ./build/tests/smp-tier1 checked-long $(T1_TEST_IMAGES)
build/tests/smp-tier1: $(SMP_T1_OBJECTS) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(SMP_T1_OBJECTS) -pthread -Wl,--wrap=hostwin_trap,--wrap=vcpu_kick,--wrap=hostwin_host_backend $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include build/hosted/tests/host/smp/tier1.d build/hosted/tests/host/smp/tier1_probe.d
