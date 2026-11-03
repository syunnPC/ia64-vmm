/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * BootInfo: everything core/ needs from the boot
 * environment, copied out of bootloader-owned memory. Produced by boot/
 * (Limine, bare metal) or by a unit test (synthetic layout); consumed by
 * vmm_main(). No pointer in it refers to memory the boot path later frees.
 */
#ifndef IA64VMM_BOOTINFO_H
#define IA64VMM_BOOTINFO_H

#include "ia64vmm/types.h"
#include "ia64vmm/bootmedia.h"

#define BOOT_INFO_VERSION 1u

enum boot_mem_type {
    BOOT_MEM_USABLE,   /* free RAM: candidate for guest RAM or VMM reservation */
    BOOT_MEM_RESERVED, /* firmware/MMIO/unknown: never touched */
    BOOT_MEM_ACPI_RECLAIMABLE,
    BOOT_MEM_ACPI_NVS,
    BOOT_MEM_BAD,
    BOOT_MEM_BOOTLOADER, /* bootloader-reclaimable: holds BootInfo sources until copied */
    BOOT_MEM_VMM,        /* VMM image and modules: part of the VMM reservation */
    BOOT_MEM_FRAMEBUFFER,
};

struct boot_mem_range {
    hpa_t base;
    uint64_t size;
    enum boot_mem_type type;
    uint8_t *hva; /* host pointer for base when mapped, else NULL */
};

#define BOOT_MAX_MEM_RANGES 256
#define BOOT_MAX_MODULES 16
#define BOOT_MAX_CPUS 256

/* CPUID-derived identifiers, not indices inferred from MP ordering. Core
 * identifies a physical core within a package (including die bits). */
struct boot_cpu_info {
    uint32_t processor_id, apic_id, package_id, core_id, smt_id;
    uint32_t topology_leaf;
    bool topology_valid, management;
    unsigned vcpu_index; /* UINT32_MAX = idle helper */
    uint8_t core_type;   /* CPUID.1AH EAX[31:24]: 40=P, 20=E, 0=unknown */
};

/* Module names (Limine module "string"/cmdline, or hosted command line):
 *   "firmware"  IA-64 firmware ELF (firmware/)
 *   "bootdisk"  raw disk image (FAT ESP with \EFI\BOOT\BOOTIA64.EFI) exposed
 *               to the guest firmware as a RAM disk
 *   "config"    optional key=value configuration text
 *   "media-id"  boot-media identity for automatic storage selection
 *   "options"   the stick's vmm-options.conf; boot/limine.c merges its tokens
 *               into cmdline (core/options.h) and does not keep it here */
struct boot_module {
    char name[32];
    hpa_t hpa;
    uint8_t *hva;
    uint64_t size;
};

enum boot_fb_format { BOOT_FB_NONE, BOOT_FB_RGB_MASKS };

struct boot_framebuffer {
    enum boot_fb_format format;
    hpa_t base;
    uint8_t *hva;
    uint64_t size;
    uint32_t width, height, pitch; /* pitch in bytes */
    uint16_t bpp;
    uint8_t red_size, red_shift, green_size, green_shift, blue_size, blue_shift;
};

/* What the host firmware left
 * in conventional memory and the option ROM area, copied into VMM pages at
 * the earliest point (boot/limine.c legacy_rom_probe), before anything can
 * change it. NULL/0 when not taken (the hosted HAL). *_uc: read through the
 * UC host MMIO map because the BootInfo map leaves the range out. Bytes of
 * a page the map covers only in part are read only where mapped; the rest
 * of the copy stays zero (*_unread counts them). */
struct boot_legacy {
    const uint8_t *low; /* host 0-9FFFFh: IVT, BDA, EBDA, ... */
    uint64_t low_size;
    const uint8_t *option; /* host C0000-DFFFFh: the POSTed VBIOS image */
    uint64_t option_size;
    bool low_uc, option_uc;
    uint64_t low_unread, option_unread;
};

/* The host machine's identity from its SMBIOS tables (DSP0134), for the
 * EFI variable slots: the Type 1 System UUID, or,
 * when that is unset or absent, the Type 1 and Type 2 manufacturer,
 * product and serial strings, each NUL-terminated. */
enum boot_identity_kind { BOOT_IDENTITY_NONE, BOOT_IDENTITY_UUID, BOOT_IDENTITY_STRINGS };
#define BOOT_IDENTITY_MAX 384u
struct boot_identity {
    uint32_t kind, length;
    uint8_t bytes[BOOT_IDENTITY_MAX];
};

struct boot_info {
    uint32_t version;
    const char *host_name; /* "amd64-limine" or "linux-hosted" */
    unsigned mem_range_count;
    struct boot_mem_range mem[BOOT_MAX_MEM_RANGES];
    unsigned module_count;
    struct boot_module modules[BOOT_MAX_MODULES];
    struct boot_framebuffer fb;
    hpa_t acpi_rsdp; /* 0 if absent */
    struct boot_identity identity;
    struct boot_media boot_media;
    unsigned host_cpu_count;
    char cmdline[1024]; /* options module tokens + entry cmdline (core/options.h) */
    struct boot_cpu_info cpus[BOOT_MAX_CPUS];
    unsigned management_cpu, vcpu_count;
    struct boot_legacy legacy;
};

/* Implemented by core/vm.c. Takes a stable copy of *bi, never returns:
 * ends through hal_shutdown(). */
IA64VMM_NORETURN void vmm_main(const struct boot_info *bi);

#endif
