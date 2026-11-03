# SPDX-License-Identifier: GPL-2.0-or-later
# Guest SMP test application and its runners.
#
# build/efi/smp.efi is efi.mk's wildcard application with one extra object,
# the OS_BOOT_RENDEZ entry stub (tests/efi/smp/entry.S), so its link rule is
# here. Variants are the same source built with -DSMP_VARIANT_<name>:
#   profiles  long (10^6 iterations), quick (10^3), ipi-itc (start-up, IPI and ITC
#             cases only: what the executor, interrupt and firmware-layout
#             tests run under smp_serialize=1), restart (ipi-itc twice with an
#             in-VMM restart between);
#   broken-*  negative variants, each breaking one check on purpose.
# Neither is part of `make efi`; `make smp-app` builds the profiles,
# `make smp-broken` the negative variants.
.PHONY: smp-app smp-broken
SMP_PROFILES := long quick ipi-itc no-purge tlb-purge restart
SMP_BROKEN := wake-vector set-vectors smbios nocallin lid entry-gp repeat ipi itc mp sb sem-sb nonatomic purge-local
SMP_EFI_OBJS := build/efi/crt0.o build/efi/reloc.o build/efi/smp_entry.o
SMP_LINK = $(CROSS_IA64)ld -nostdlib -shared -Bsymbolic -z nocombreloc --no-undefined \
	-T $(GNU_EFI)/elf_ia64_efi.lds $(SMP_EFI_OBJS) $< $$($(CROSS_IA64)gcc -print-libgcc-file-name) -o $@

build/efi/smp_entry.o: tests/efi/smp/entry.S tests/efi/smp/smp.h mk/smp.mk
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -c $< -o $@
build/efi/smp.so: build/efi/smp.o $(SMP_EFI_OBJS) $(GNU_EFI)/elf_ia64_efi.lds
	$(SMP_LINK)
# Static pattern rules: only the listed variants are buildable, so make's
# built-in `%: %.o` chain never turns an included .d file into a target.
SMP_VARIANTS := $(SMP_PROFILES) $(SMP_BROKEN:%=broken-%)
$(SMP_VARIANTS:%=build/efi/smp-%.o): build/efi/smp-%.o: tests/efi/smp/smp.c mk/smp.mk
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(EFI_CFLAGS) -DSMP_VARIANT_$(subst -,_,$*) -MMD -MP -c $< -o $@
$(SMP_VARIANTS:%=build/efi/smp-%.so): build/efi/smp-%.so: build/efi/smp-%.o $(SMP_EFI_OBJS) $(GNU_EFI)/elf_ia64_efi.lds
	$(SMP_LINK)
# The new firmware boots the same PE file (as mk/firmware-new.mk does for hello).
build/firmware-new/smp.efi: build/efi/smp.efi
	@mkdir -p $(@D)
	cp $< $@
$(SMP_VARIANTS:%=build/firmware-new/smp-%.efi): build/firmware-new/smp-%.efi: build/efi/smp-%.efi
	@mkdir -p $(@D)
	cp $< $@

smp-app: build/efi/smp.efi build/firmware-new/smp.efi \
	$(SMP_PROFILES:%=build/efi/smp-%.efi) $(SMP_PROFILES:%=build/firmware-new/smp-%.efi)
smp-broken: $(SMP_BROKEN:%=build/efi/smp-broken-%.efi) $(SMP_BROKEN:%=build/firmware-new/smp-broken-%.efi)
