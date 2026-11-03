# SPDX-License-Identifier: GPL-2.0-or-later
FIRMWARE ?= new
ifneq ($(FIRMWARE),new)
$(error FIRMWARE must be new)
endif
.PHONY: firmware
firmware: firmware-new

build/firmware.elf: build/firmware-new/ia64-firmware-new.elf FORCE
	cp $< $@
esp: build/firmware.elf build/bootdisk.img
