# SPDX-License-Identifier: GPL-2.0-or-later
GNU_EFI := third_party/gnu-efi
EFI_APPS := $(foreach dir,$(wildcard tests/efi/*),$(if $(wildcard $(dir)/$(notdir $(dir)).c),$(notdir $(dir))))
EFI_CFLAGS := -std=c11 -O2 -fpic -ffreestanding -fno-builtin -fno-stack-protector -fno-common -fshort-wchar -mno-sdata -mfixed-range=f32-f127 -Wall -Wextra -I$(GNU_EFI)/inc -I$(GNU_EFI)/inc/ia64
efi: $(addprefix build/efi/,$(addsuffix .efi,$(EFI_APPS)))
build/efi/crt0.o: $(GNU_EFI)/crt0-efi-ia64.S
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -fpic -c $< -o $@
build/efi/reloc.o: $(GNU_EFI)/reloc_ia64.S
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -fpic -c $< -o $@
define EFI_APP_RULE
build/efi/$(1).o: tests/efi/$(1)/$(1).c
	@mkdir -p $$(@D)
	$$(CROSS_IA64)gcc $$(EFI_CFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach app,$(EFI_APPS),$(eval $(call EFI_APP_RULE,$(app))))
build/efi/%.so: build/efi/%.o build/efi/crt0.o build/efi/reloc.o $(GNU_EFI)/elf_ia64_efi.lds
	$(CROSS_IA64)ld -nostdlib -shared -Bsymbolic -z nocombreloc --no-undefined -T $(GNU_EFI)/elf_ia64_efi.lds build/efi/crt0.o build/efi/reloc.o $< $$($(CROSS_IA64)gcc -print-libgcc-file-name) -o $@
build/efi/%.efi: build/efi/%.so mk/efi.mk
	$(CROSS_IA64)objcopy -j .text -j .sdata -j .data -j .dynamic -j .rela -j .rela.plt -j .reloc -j .dynsym -j .dynstr -O pei-ia64 --image-base=0 --subsystem=10 $< $@
# FORCE also makes a BOOT_APP change replace the selected application.
BOOTDISK_KIND ?= pe
bootdisk: build/bootdisk.img
# A PE copy is used with GNU-EFI's overlapping ELF hash sections removed.
EFI_BOOT_IMAGE = $(if $(filter new,$(FIRMWARE)),build/firmware-new/$(BOOT_APP).efi,build/efi/$(BOOT_APP).efi)
build/bootdisk.img: $(EFI_BOOT_IMAGE) FORCE
	@mkdir -p $(@D)
ifeq ($(BOOTDISK_KIND),pe)
	cp $< $@.tmp
else ifeq ($(BOOTDISK_KIND),fat)
	truncate -s 16M $@.tmp
	mkfs.fat -F 16 $@.tmp
	mmd -i $@.tmp ::/EFI ::/EFI/BOOT
	mcopy -i $@.tmp $< ::/EFI/BOOT/BOOTIA64.EFI
else
	$(error unknown BOOTDISK_KIND '$(BOOTDISK_KIND)': use pe or fat)
endif
	mv $@.tmp $@
.SECONDARY:
-include $(wildcard build/efi/*.d)
