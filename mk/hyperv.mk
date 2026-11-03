# SPDX-License-Identifier: GPL-2.0-or-later
.PHONY: hyperv-image test-hyperv
hyperv-image: vmm firmware-new build/efi/hello.efi build/vmmcfg/vmmcfg.efi build/bootmedia/bootmedia.efi third_party/limine/BOOTX64.EFI
	python3 tools/hyperv/build.py

test-host: test-hyperv
test-hyperv: build/tests/hyperv-synic build/tests/hyperv-vmbus build/tests/hyperv-session build/tests/hyperv-storvsc build/tests/hyperv-ahci build/tests/hyperv-media build/tests/hyperv-keyboard build/tests/hyperv-i8042
	./build/tests/hyperv-synic
	./build/tests/hyperv-vmbus
	./build/tests/hyperv-session
	./build/tests/hyperv-storvsc
	./build/tests/hyperv-ahci
	./build/tests/hyperv-media
	./build/tests/hyperv-keyboard
	./build/tests/hyperv-i8042

build/tests/hyperv-synic: tests/hyperv/synic.c host/amd64/hyperv.c host/amd64/hyperv.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
build/tests/hyperv-vmbus: tests/hyperv/vmbus.c host/amd64/vmbus.c host/amd64/vmbus.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -DIA64VMM_VMBUS_WIRE_ONLY $< host/amd64/vmbus.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
build/tests/hyperv-session: tests/hyperv/vmbus_session.c host/amd64/vmbus.c host/amd64/vmbus.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
build/tests/hyperv-storvsc: tests/persist/storvsc.c host/amd64/storvsc.c host/amd64/storvsc.h host/amd64/vmbus.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
build/tests/hyperv-ahci: tests/hyperv/ahci.c platform/ahci.c platform/ahci.h include/ia64vmm/block.h core/guest_mem.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< platform/ahci.c core/guest_mem.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@

build/tests/hyperv-media: tests/hyperv/media.c host/amd64/plog_hyperv.c host/amd64/plog_hyperv.h host/amd64/storvsc.h core/options.c platform/handoff.c persist/media.c persist/fat32.c persist/protocol.c tests/persist/fat32-fixture.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections $< core/options.c platform/handoff.c persist/media.c persist/fat32.c persist/protocol.c -Wl,--gc-sections $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@

build/tests/hyperv-keyboard: tests/hyperv/keyboard.c host/amd64/hyperv_keyboard.c host/amd64/hyperv_keyboard.h host/amd64/vmbus.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
build/tests/hyperv-i8042: tests/hyperv/i8042.c platform/i8042.c platform/i8042.h platform/lock.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< platform/i8042.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
