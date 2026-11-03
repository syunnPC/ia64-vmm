# SPDX-License-Identifier: GPL-2.0-or-later
# Preserve environment settings when including literal KEY=value lines.
LOCAL_CONFIG_VARS := CROSS_IA64 QEMU_X86 OVMF_CODE OVMF_VARS QEMU_IA64 IA64_FORK_DIR VMM_BACKEND WIN_QEMU WIN_QEMU_FW_CODE WIN_QEMU_FW_VARS WIN_STAGING_DIR VMWARE_DIR WIN_SYSTEM32 VMM_CMDLINE QEMU_EXPECT_EXIT IA64_IMAGES_DIR IA64_MEDIA_MANIFEST USB_STICK_CONFIG USB_STICK_DIR USB_STICK_IMAGE
# Python venv with z3-solver for the Tier1 rule prover (make test-tier1-rules).
LOCAL_CONFIG_VARS += T1_Z3_VENV
# Disable-only scalar ISA policy in Linux test shims.
LOCAL_CONFIG_VARS += IA64VMM_HOST_ISA_MASK
# ACPI compiler and recorded host tables for the AML routing tests.
LOCAL_CONFIG_VARS += IASL ACPI_TABLE_DIRS
# Host objdump for the VBIOS stub decode (tools/vbios/test_stubs.py).
LOCAL_CONFIG_VARS += OBJDUMP
# MinGW-w64 x86-64 prefix for vmmcfg.efi (mk/vmmcfg.mk).
LOCAL_CONFIG_VARS += MINGW_X64
# Python 2.7 for EDK2 BaseTools (tools/firmware/regenerate_edk2.py).
LOCAL_CONFIG_VARS += EDK2_PYTHON2
LOCAL_ENV_VARS := $(foreach v,$(LOCAL_CONFIG_VARS),$(if $(findstring environment,$(origin $(v))),$(v)))
$(foreach v,$(LOCAL_ENV_VARS),$(eval LOCAL_SAVED_$(v) := $($(v))))
-include local.env
$(foreach v,$(LOCAL_ENV_VARS),$(eval $(v) := $(LOCAL_SAVED_$(v))))
IA64VMM_HOST_ISA_MASK ?= 15
IASL ?= iasl
MINGW_X64 ?= x86_64-w64-mingw32-
HOSTCC ?= gcc
HOSTNM ?= nm
CROSS_IA64 ?= ia64-linux-gnu-
QEMU_X86 ?= qemu-system-x86_64
VMM_CMDLINE ?= exit=isa-debug-exit reserve_mib=128
QEMU_EXPECT_EXIT ?= 0
export $(LOCAL_CONFIG_VARS)
HOSTED_CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude -I. -DIA64VMM_HOSTED=1
# Host SIMD baseline; AVX2 and FMA3 require hal_host_simd().
# Compiler-chosen vectors and inline copies stay 128 bits wide.
BM_SIMD_CFLAGS := -msse4.2 -mpopcnt -mavx -mprefer-vector-width=128 -mmove-max=128 -mstore-max=128
BM_CFLAGS := -std=c11 -O2 -g -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel $(BM_SIMD_CFLAGS) -Wall -Wextra -Werror -Iinclude -I.
# General-registers-only sources:
# code that runs before its CPU enables SIMD state, in interrupt, exception,
# NMI or panic context, and everything those paths call. The flag comes last:
# it must follow every -m ISA option. mk/check-gpr-only.py checks the image.
BM_GPR_ONLY_SOURCES := $(wildcard boot/*.c host/amd64/*.c host/fb/*.c) host/amd64/tests/hwb.c \
	core/log.c core/profile.c core/string.c persist/bot.c persist/log.c persist/media.c persist/fat32.c persist/nvram.c persist/protocol.c persist/sbc.c \
	platform/interrupts.c platform/intx.c platform/msi.c engine/cache/coherence.c engine/cache/validity.c \
	engine/hostwin/registry.c engine/hostwin/trap.c engine/tier1/trap.c ia64/interrupt.c
BM_GPR_ONLY_CFLAGS = $(if $(filter $(1),$(BM_GPR_ONLY_SOURCES)),-mgeneral-regs-only)
# No product frame may consume more than 1/4 of a 64 KiB host stack.
BM_CFLAGS += -Wstack-usage=16384 -Werror=stack-usage=16384
ifeq ($(SAN),1)
HOSTED_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
HOSTED_LDFLAGS += -fsanitize=address,undefined
endif
# ThreadSanitizer for hosted objects and tests.
# Known limits: code generated at run time (Tier0/Tier1 AMD64 output) is not
# instrumented, so accesses made by it are invisible to TSan; only the C
# helpers it calls are checked. Stand-alone fences (__atomic_thread_fence,
# the guest's mf/acquire/release paths in ia64/) are not modelled: GCC warns
# at each one (-Wtsan, kept visible but not fatal) and TSan may report races
# that only such a fence orders. Raw futex waits are not modelled either; the
# HAL's ordering comes from the atomics on the futex words.
ifeq ($(TSAN),1)
ifeq ($(SAN),1)
$(error SAN=1 and TSAN=1 are mutually exclusive)
endif
HOSTED_CFLAGS += -fsanitize=thread -fno-omit-frame-pointer -Wno-error=tsan
HOSTED_LDFLAGS += -fsanitize=thread
endif
BOOT_APP ?= hello
