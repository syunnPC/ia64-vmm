# SPDX-License-Identifier: GPL-2.0-or-later
# Opt-in hardware acceptance; the shared runners retain their normal policy.
.PHONY: test-hostwin-qemu test-hostwin-vmware
test-hostwin-qemu:
	python3 tools/qemu/hwb.py --backend win-qemu
test-hostwin-vmware:
	python3 tools/qemu/hwb.py --backend vmware
# Test-only build knob: the host-window test additionally checks real table exhaustion with a pool of 3 or 4
# tables per executor (e.g. 6 or 8 with vcpus=2).
# Restore a normal build after using it; never set this on a physical boot image.
ifneq ($(HWB_TEST_SHADOW_TABLES),)
BM_CFLAGS += -DHWB_TEST_SHADOW_TABLES=$(HWB_TEST_SHADOW_TABLES)
endif
