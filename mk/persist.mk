# SPDX-License-Identifier: GPL-2.0-or-later
ifneq ($(AMD64_HOST_ONLY),1)
SHARED_SOURCES += $(wildcard persist/*.c)
endif
.PHONY: test-persist
test-host: test-persist
test-persist: build/tests/persist-fat32 build/tests/persist-protocol build/tests/persist-writer build/tests/persist-nvram build/tests/persist-smbios build/tests/persist-ring build/tests/persist-bot build/tests/persist-scsi build/tests/persist-xhci build/tests/persist-nvme build/tests/persist-ahci build/tests/persist-ehci
	./build/tests/persist-fat32
	./build/tests/persist-protocol
	./build/tests/persist-bot
	./build/tests/persist-scsi
	./build/tests/persist-xhci
	./build/tests/persist-nvme
	./build/tests/persist-ahci
	./build/tests/persist-ehci
	./build/tests/persist-writer
	./build/tests/persist-nvram
	./build/tests/persist-smbios
	./build/tests/persist-ring 2>build/tests/persist-ring.stderr
	python3 tests/persist/media.py
build/tests/persist-protocol: tests/persist/protocol.c persist/protocol.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -I. -Iinclude $< persist/protocol.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@

build/tests/persist-writer: tests/persist/writer.c tests/persist/fat32-fixture.h persist/log.c persist/media.c persist/fat32.c persist/fat32.h persist/protocol.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -I. -Iinclude $< persist/log.c persist/media.c persist/fat32.c persist/protocol.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@

build/tests/persist-smbios: tests/persist/smbios.c persist/smbios.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -I. -Iinclude $< persist/smbios.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@

build/tests/persist-nvram: tests/persist/nvram.c tests/persist/fat32-fixture.h persist/nvram.c persist/media.c persist/fat32.c persist/fat32.h persist/protocol.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -I. -Iinclude $< persist/nvram.c persist/media.c persist/fat32.c persist/protocol.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@

build/tests/persist-ring: tests/persist/ring.c core/log.c tests/hal/hal_linux.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< core/log.c tests/hal/hal_linux.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
.PHONY: test-persist-qemu
test-persist-qemu: vmm build/firmware.elf build/efi/hello.efi build/bootmedia/bootmedia.efi third_party/limine/BOOTX64.EFI \
	build/firmware-new/ia64-firmware-new.elf build/firmware-new/restart.efi build/firmware-new/hello.efi \
	build/efi/restart.efi build/firmware-new/ia64-firmware-new.elf
	python3 tests/persist/qemu_roundtrip.py
	python3 tests/persist/qemu_roundtrip.py --crash
	python3 tests/persist/qemu_roundtrip.py --discovery
	python3 tests/persist/qemu_roundtrip.py --nvram
	python3 tests/persist/qemu_roundtrip.py --nvme
	python3 tests/persist/qemu_roundtrip.py --nvme --crash
	python3 tests/persist/qemu_roundtrip.py --nvme --nvram
	python3 tests/persist/qemu_roundtrip.py --ahci
	python3 tests/persist/qemu_roundtrip.py --ahci --crash
	python3 tests/persist/qemu_roundtrip.py --ahci --nvram
	python3 tests/persist/qemu_roundtrip.py --ehci
	python3 tests/persist/qemu_roundtrip.py --ehci --crash
	python3 tests/persist/qemu_roundtrip.py --ehci --stop
	python3 tests/persist/qemu_roundtrip.py --ehci --hub
	python3 tests/persist/qemu_roundtrip.py --ehci --nvram

build/tests/persist-bot: tests/persist/bot.c persist/bot.c persist/protocol.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< persist/bot.c persist/protocol.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@

build/tests/persist-scsi: tests/persist/scsi.c persist/bot.c persist/protocol.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) $< persist/bot.c persist/protocol.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@

build/tests/persist-xhci: tests/persist/xhci.c host/amd64/plog_xhci.c persist/bot.c persist/sbc.c persist/protocol.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections $< persist/bot.c persist/sbc.c persist/protocol.c $(HOSTED_LDFLAGS) $(LDFLAGS) -Wl,--gc-sections -o $@

build/tests/persist-nvme: tests/persist/nvme.c host/amd64/plog_nvme.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections $< $(HOSTED_LDFLAGS) $(LDFLAGS) -Wl,--gc-sections -o $@

build/tests/persist-ahci: tests/persist/ahci.c host/amd64/plog_ahci.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections $< $(HOSTED_LDFLAGS) $(LDFLAGS) -Wl,--gc-sections -o $@

build/tests/persist-ehci: tests/persist/ehci.c host/amd64/plog_ehci.c persist/bot.c persist/sbc.c persist/protocol.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections $< persist/bot.c persist/sbc.c persist/protocol.c $(HOSTED_LDFLAGS) $(LDFLAGS) -Wl,--gc-sections -o $@

build/tests/persist-fat32: tests/persist/fat32.c persist/fat32.c persist/fat32.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -I. -Iinclude $< persist/fat32.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
