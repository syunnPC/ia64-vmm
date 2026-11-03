# SPDX-License-Identifier: GPL-2.0-or-later
# Engine benchmarks (see docs/testing.md): the asm kernels and
# a compiled-C suite on interp, Tier0, Tier1 (hosted Tier1 harness) and on
# the QEMU fork's TCG through benchwrap.efi; a floating-point suite in the
# harness only.
#
#   make bench-engines            all measurements, report in build/bench/
#   make bench-wrap PLAN=name     build/bench/plans/<name>/{benchwrap.efi,disk.img}
#
# Job files (tests/bench/job.h) come from build/bench-jobdump, which links
# the Tier1 harness objects (tests/tier1) without changing them.

BENCH_DIR := build/bench
# BENCH_WRAP_STEPS=1: the wrapper prints each step's BENCH line as it
# finishes instead of at the end. Own object directory and plan name
# (make bench-wrap PLAN=asm-fast-steps, with jobs.bin copied from asm-fast).
BENCH_WRAP_DIR := $(BENCH_DIR)/wrap$(if $(BENCH_WRAP_STEPS),-steps)
BENCH_WRAP_CFLAGS = $(EFI_CFLAGS) -I. -Itests/bench/wrap $(if $(BENCH_WRAP_STEPS),-DBENCH_FLUSH_EACH_STEP=1)
BENCH_WRAP_OBJECTS := $(BENCH_WRAP_DIR)/benchwrap.o $(BENCH_WRAP_DIR)/enter.o

$(BENCH_WRAP_DIR)/benchwrap.o: tests/bench/wrap/benchwrap.c tests/bench/job.h tests/bench/wrap/ctx.h
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(BENCH_WRAP_CFLAGS) -c $< -o $@
$(BENCH_WRAP_DIR)/enter.o: tests/bench/wrap/enter.S tests/bench/wrap/ctx.h
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc -Itests/bench/wrap -c $< -o $@

# A plan directory holds jobs.bin (written by tools/bench/plan.py); the
# wrapper embeds it in .data.
$(BENCH_DIR)/plans/%/jobs.o: $(BENCH_DIR)/plans/%/jobs.bin
	printf '\t.section .data\n\t.align 64\n\t.global bench_jobs, bench_jobs_end\nbench_jobs:\n\t.incbin "%s"\nbench_jobs_end:\n' '$<' > $(@D)/jobs.S
	$(CROSS_IA64)gcc -c $(@D)/jobs.S -o $@
$(BENCH_DIR)/plans/%/benchwrap.so: $(BENCH_DIR)/plans/%/jobs.o $(BENCH_WRAP_OBJECTS) build/efi/crt0.o build/efi/reloc.o third_party/gnu-efi/elf_ia64_efi.lds
	$(CROSS_IA64)ld -nostdlib -shared -Bsymbolic -z nocombreloc --no-undefined -T $(GNU_EFI)/elf_ia64_efi.lds build/efi/crt0.o build/efi/reloc.o $(BENCH_WRAP_OBJECTS) $< $$($(CROSS_IA64)gcc -print-libgcc-file-name) -o $@
$(BENCH_DIR)/plans/%/benchwrap.efi: $(BENCH_DIR)/plans/%/benchwrap.so
	$(CROSS_IA64)objcopy -j .text -j .sdata -j .data -j .dynamic -j .rela -j .rela.plt -j .reloc -j .dynsym -j .dynstr -O pei-ia64 --image-base=0 --subsystem=10 $< $@
# The fork boots \EFI\BOOT\BOOTIA64.EFI from a FAT disk (its default
# LSI53C895A SCSI disk on itanium2-vpc).
$(BENCH_DIR)/plans/%/disk.img: $(BENCH_DIR)/plans/%/benchwrap.efi
	rm -f $@.tmp
	truncate -s 16M $@.tmp
	mkfs.fat -F 16 $@.tmp >/dev/null
	mmd -i $@.tmp ::/EFI ::/EFI/BOOT
	mcopy -i $@.tmp $< ::/EFI/BOOT/BOOTIA64.EFI
	mv $@.tmp $@

# The wrapper's exit path under the new firmware (tests/qemu/apps.py): one speed
# job, and the same wrapper with BENCH_WRAP_KEEP_PSR_I, which returns to the
# firmware with PSR.i set before ar.k1 is restored. Both wait for the pending
# timer interrupt before the KRs are restored (BENCH_WRAP_WAIT_IRQ). The
# jobs.bin rule is in mk/tier1.mk, after T1_TEST_IMAGES is defined.
BENCH_QEMU_DIR := $(BENCH_DIR)/qemu-app
define BENCH_QEMU_APP
$$(BENCH_QEMU_DIR)/$(1)/benchwrap.o: tests/bench/wrap/benchwrap.c tests/bench/job.h tests/bench/wrap/ctx.h mk/bench.mk
	@mkdir -p $$(@D)
	$$(CROSS_IA64)gcc $$(EFI_CFLAGS) -I. -Itests/bench/wrap $(2) -c $$< -o $$@
$$(BENCH_QEMU_DIR)/$(1)/enter.o: tests/bench/wrap/enter.S tests/bench/wrap/ctx.h mk/bench.mk
	@mkdir -p $$(@D)
	$$(CROSS_IA64)gcc -Itests/bench/wrap $(2) -c $$< -o $$@
$$(BENCH_QEMU_DIR)/$(1)/jobs.o: $$(BENCH_QEMU_DIR)/jobs.bin
	printf '\t.section .data\n\t.align 64\n\t.global bench_jobs, bench_jobs_end\nbench_jobs:\n\t.incbin "%s"\nbench_jobs_end:\n' '$$<' > $$(@D)/jobs.S
	$$(CROSS_IA64)gcc -c $$(@D)/jobs.S -o $$@
build/firmware-new/$(3).efi: $$(BENCH_QEMU_DIR)/$(1)/benchwrap.o $$(BENCH_QEMU_DIR)/$(1)/enter.o $$(BENCH_QEMU_DIR)/$(1)/jobs.o build/efi/crt0.o build/efi/reloc.o third_party/gnu-efi/elf_ia64_efi.lds
	@mkdir -p $$(@D)
	$$(CROSS_IA64)ld -nostdlib -shared -Bsymbolic -z nocombreloc --no-undefined -T $$(GNU_EFI)/elf_ia64_efi.lds build/efi/crt0.o build/efi/reloc.o $$(wordlist 1,3,$$^) $$$$($$(CROSS_IA64)gcc -print-libgcc-file-name) -o $$(BENCH_QEMU_DIR)/$(1)/benchwrap.so
	$$(CROSS_IA64)objcopy -j .text -j .sdata -j .data -j .dynamic -j .rela -j .rela.plt -j .reloc -j .dynsym -j .dynstr -O pei-ia64 --image-base=0 --subsystem=10 $$(BENCH_QEMU_DIR)/$(1)/benchwrap.so $$@
endef
$(eval $(call BENCH_QEMU_APP,ok,-DBENCH_WRAP_WAIT_IRQ,benchwrap))
$(eval $(call BENCH_QEMU_APP,keep-psr-i,-DBENCH_WRAP_WAIT_IRQ -DBENCH_WRAP_KEEP_PSR_I,benchwrap-keep-psr-i))

.PHONY: bench-wrap
bench-wrap: $(BENCH_DIR)/plans/$(PLAN)/benchwrap.efi $(BENCH_DIR)/plans/$(PLAN)/disk.img
.SECONDARY:

# Harness-side job tool: the tier1-test objects without their main().
# T1_TEST_OBJECTS (mk/tier1.mk, included after this file) is expanded in
# the recipe; build/tier1-test as a prerequisite builds every object.
build/bench-jobdump: build/tier1-test build/hosted/tests/bench/jobdump.o
	$(HOSTCC) $(filter-out build/hosted/tests/tier1/main.o,$(T1_TEST_OBJECTS)) \
		build/hosted/tests/bench/jobdump.o $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
-include build/hosted/tests/bench/jobdump.d

# Compiled-C suite (tests/bench/csuite): a flat image linked at the harness
# RAM base, and the same objects linked 16 MiB higher; the two binaries
# must be identical (the image runs unrelocated in the wrapper's window).
CSUITE_CFLAGS := -std=c11 -O2 -ffreestanding -fno-builtin -fno-jump-tables -mauto-pic \
	-minline-int-divide-max-throughput -Wall -Wextra -Werror
CSUITE_DIR := $(BENCH_DIR)/csuite
$(CSUITE_DIR)/csuite.o: tests/bench/csuite/csuite.c tests/bench/csuite/csuite.h
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(CSUITE_CFLAGS) -c $< -o $@
$(CSUITE_DIR)/start.o: tests/bench/csuite/start.S tests/tier1/programs/ivt.inc
	@mkdir -p $(@D)
	$(CROSS_IA64)as -mauto-pic -I tests/tier1/programs $< -o $@
$(CSUITE_DIR)/csuite.elf: $(CSUITE_DIR)/start.o $(CSUITE_DIR)/csuite.o tests/bench/csuite/csuite.lds
	$(CROSS_IA64)ld --no-warn-rwx-segments -T tests/bench/csuite/csuite.lds --defsym CSUITE_BASE=0x100000 $(CSUITE_DIR)/start.o $(CSUITE_DIR)/csuite.o -o $@
$(CSUITE_DIR)/csuite-alt.elf: $(CSUITE_DIR)/start.o $(CSUITE_DIR)/csuite.o tests/bench/csuite/csuite.lds
	$(CROSS_IA64)ld --no-warn-rwx-segments -T tests/bench/csuite/csuite.lds --defsym CSUITE_BASE=0x1100000 $(CSUITE_DIR)/start.o $(CSUITE_DIR)/csuite.o -o $@
$(CSUITE_DIR)/%.bin: $(CSUITE_DIR)/%.elf
	$(CROSS_IA64)objcopy -O binary $< $@
$(CSUITE_DIR)/csuite-native: tests/bench/csuite/native.c tests/bench/csuite/csuite.c tests/bench/csuite/csuite.h
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -fno-builtin -o $@ tests/bench/csuite/native.c tests/bench/csuite/csuite.c
.PHONY: csuite
csuite: $(CSUITE_DIR)/csuite.bin $(CSUITE_DIR)/csuite-alt.bin $(CSUITE_DIR)/csuite-native
	@cmp $(CSUITE_DIR)/csuite.bin $(CSUITE_DIR)/csuite-alt.bin && \
		echo 'csuite: image is position independent ($(CSUITE_DIR)/csuite.bin)'

# Floating-point suite (tests/bench/fpsuite): the same image layout and link
# script; contraction is explicit because -std=c11 turns it off, square root
# and division are the compiler's inline frsqrta/frcpa sequences, and the
# vectorizer is off so only kernel 6 uses the parallel (v2sf) instructions.
FPSUITE_CFLAGS := $(CSUITE_CFLAGS) -ffp-contract=fast -fno-tree-vectorize -fno-math-errno \
	-minline-sqrt-max-throughput -minline-float-divide-max-throughput
FPSUITE_DIR := $(BENCH_DIR)/fpsuite
$(FPSUITE_DIR)/fpsuite.o: tests/bench/fpsuite/fpsuite.c tests/bench/fpsuite/fpsuite.h
	@mkdir -p $(@D)
	$(CROSS_IA64)gcc $(FPSUITE_CFLAGS) -c $< -o $@
$(FPSUITE_DIR)/start.o: tests/bench/fpsuite/start.S tests/tier1/programs/ivt.inc
	@mkdir -p $(@D)
	$(CROSS_IA64)as -mauto-pic -I tests/tier1/programs $< -o $@
$(FPSUITE_DIR)/fpsuite.elf: $(FPSUITE_DIR)/start.o $(FPSUITE_DIR)/fpsuite.o tests/bench/csuite/csuite.lds
	$(CROSS_IA64)ld --no-warn-rwx-segments -T tests/bench/csuite/csuite.lds --defsym CSUITE_BASE=0x100000 $(FPSUITE_DIR)/start.o $(FPSUITE_DIR)/fpsuite.o -o $@
$(FPSUITE_DIR)/fpsuite-alt.elf: $(FPSUITE_DIR)/start.o $(FPSUITE_DIR)/fpsuite.o tests/bench/csuite/csuite.lds
	$(CROSS_IA64)ld --no-warn-rwx-segments -T tests/bench/csuite/csuite.lds --defsym CSUITE_BASE=0x1100000 $(FPSUITE_DIR)/start.o $(FPSUITE_DIR)/fpsuite.o -o $@
$(FPSUITE_DIR)/%.bin: $(FPSUITE_DIR)/%.elf
	$(CROSS_IA64)objcopy -O binary $< $@
.PHONY: fpsuite
fpsuite: $(FPSUITE_DIR)/fpsuite.bin $(FPSUITE_DIR)/fpsuite-alt.bin
	@cmp $(FPSUITE_DIR)/fpsuite.bin $(FPSUITE_DIR)/fpsuite-alt.bin && \
		echo 'fpsuite: image is position independent ($(FPSUITE_DIR)/fpsuite.bin)'

# The whole measurement (see docs/testing.md).
# BENCH_CPU pins every sample to that CPU (default: the least busy one).
BENCH_ROUNDS ?= 5
BENCH_PY = python3 -I tools/bench
.PHONY: bench-engines bench-engines-prepare
bench-engines-prepare: build/tier1-test build/bench-jobdump firmware efi csuite
	$(MAKE) $(T1_TEST_IMAGES)
	$(BENCH_PY)/bench_engines.py prepare --suite asm
	$(BENCH_PY)/bench_engines.py prepare --suite csuite
	$(BENCH_PY)/csuite_verify.py
bench-engines: bench-engines-prepare
	$(BENCH_PY)/bench_engines.py run --suite asm --rounds $(BENCH_ROUNDS) --out $(BENCH_DIR)/asm.json
	$(BENCH_PY)/bench_engines.py run --suite csuite --rounds $(BENCH_ROUNDS) --no-segment40 \
		--out $(BENCH_DIR)/csuite.json
	$(BENCH_PY)/bench_engines.py wallcheck --suite asm --platform tcg --out $(BENCH_DIR)/wall-asm-tcg.json
	$(BENCH_PY)/bench_engines.py wallcheck --suite csuite --platform tcg --out $(BENCH_DIR)/wall-csuite-tcg.json
	$(BENCH_PY)/report.py $(BENCH_DIR)/asm.json --title 'asm kernels' --md $(BENCH_DIR)/asm.md
	$(BENCH_PY)/report.py $(BENCH_DIR)/csuite.json --title 'Compiled C' --md $(BENCH_DIR)/csuite.md
	$(BENCH_PY)/report.py $(BENCH_DIR)/wall-asm-tcg.json --title 'TCG wall check, asm'
	$(BENCH_PY)/report.py $(BENCH_DIR)/wall-csuite-tcg.json --title 'TCG wall check, compiled C'
