# SPDX-License-Identifier: GPL-2.0-or-later
.PHONY: usb-stick test-real-prep
usb-stick: vmm firmware-new build/efi/hello.efi build/vmmcfg/vmmcfg.efi build/bootmedia/bootmedia.efi third_party/limine/BOOTX64.EFI
	python3 tools/usb-stick/build.py
REAL_PREP_SOURCES := host/amd64/tests/prep.c host/amd64/iommu_tables.c host/amd64/calibration.c boot/ranges.c platform/handoff.c platform/pci.c core/guest_mem.c core/log.c tests/hal/hal_linux.c
build/tests/real-prep: $(REAL_PREP_SOURCES) build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections $(REAL_PREP_SOURCES) -Wl,--gc-sections $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
test-real-prep: build/tests/real-prep
	./build/tests/real-prep
	python3 tools/usb-stick/test_build.py
test-host: test-real-prep

.PHONY: test-vtd
test-vtd: build/tests/vtd
	./build/tests/vtd
build/tests/vtd: host/amd64/tests/vtd.c host/amd64/vtd.c host/amd64/iommu_dma.c host/amd64/vtd.h host/amd64/iommu_dma.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) host/amd64/tests/vtd.c host/amd64/vtd.c host/amd64/iommu_dma.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
test-host: test-vtd

.PHONY: test-amdvi
test-amdvi: build/tests/amdvi
	./build/tests/amdvi
build/tests/amdvi: host/amd64/tests/amdvi.c host/amd64/amdvi.c host/amd64/amdvi.h host/amd64/iommu_dma.h host/amd64/iommu_dma.c host/amd64/iommu_tables.c build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) host/amd64/tests/amdvi.c host/amd64/amdvi.c host/amd64/iommu_dma.c host/amd64/iommu_tables.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
test-host: test-amdvi

.PHONY: test-vtd-irq
test-vtd-irq: build/tests/vtd-irq
	./build/tests/vtd-irq
build/tests/vtd-irq: host/amd64/tests/vtd_irq.c host/amd64/vtd.c host/amd64/vtd_irq.c host/amd64/iommu_dma.c host/amd64/vtd.h host/amd64/iommu_dma.h build/hosted/.flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTED_CFLAGS) $(CPPFLAGS) $(CFLAGS) host/amd64/tests/vtd_irq.c host/amd64/vtd.c host/amd64/vtd_irq.c host/amd64/iommu_dma.c $(HOSTED_LDFLAGS) $(LDFLAGS) -o $@
test-host: test-vtd-irq
