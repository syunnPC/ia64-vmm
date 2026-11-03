/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VMM <-> IA-64 firmware platform contract.
 *
 * This header is shared with the IA-64 firmware build (firmware/ includes it
 * with its own freestanding types), so it uses only fixed-width integer
 * types, no host pointers, and explicit little-endian packed layouts.
 *
 * Address-space rule: ordinary guest RAM is identity-mapped
 * (GPA == HPA). Except for the architectural PIB, emulation lives just below
 * 2^IA64VMM_PHYS_ADDR_BITS, an area no supported AMD64 host uses for RAM or
 * MMIO, so emulated ranges can never shadow a host range that the guest
 * owns directly. core/ verifies at boot that no host range overlaps it.
 */
#ifndef IA64VMM_PLATFORM_H
#define IA64VMM_PLATFORM_H

#include "cpu_model.h"
#include <stdint.h>

/* --- clocks ------------------------------------------------------------ */

/* PAL_FREQ_BASE / SAL_FREQ_BASE. The firmware derives the ITC frequency as
 * base x PAL_FREQ_RATIOS ITC ratio and halts unless the result is a
 * multiple of 10 MHz and >= 10 MHz. */
#define IA64VMM_PLATFORM_BASE_HZ 100000000ULL
#define IA64VMM_ITC_RATIO_NUM 4u
#define IA64VMM_ITC_RATIO_DEN 1u
#define IA64VMM_ITC_HZ (IA64VMM_PLATFORM_BASE_HZ * IA64VMM_ITC_RATIO_NUM / IA64VMM_ITC_RATIO_DEN)
/* Reported processor core ratio (PAL_FREQ_RATIOS proc): 1.6 GHz, the
 * Montecito 9050 the CPU profile follows. Informational only. */
#define IA64VMM_PROC_RATIO_NUM 16u
#define IA64VMM_PROC_RATIO_DEN 1u
#define IA64VMM_BUS_RATIO_NUM 4u
#define IA64VMM_BUS_RATIO_DEN 1u

/* --- guest physical address map ------------------------------------------ */

/* IMPL_PA_MSB + 1 of the CPU profile (Montecito-class). */
#define IA64VMM_PHYS_ADDR_BITS 50u

#define IA64VMM_WINDOW_BASE 0x0003ffff00000000ULL /* 4 GiB, ends at 2^50 */
#define IA64VMM_WINDOW_SIZE 0x0000000100000000ULL

/* PCI configuration (ECAM layout, segment 0, buses 0..255). The VMM mediates the
 * real host configuration space; the hosted harness retains an empty bus. */
#define IA64VMM_ECAM_BASE 0x0003ffffe0000000ULL
#define IA64VMM_ECAM_SIZE 0x0000000010000000ULL

/* VMM service window (service registers below). */
#define IA64VMM_SERVICE_BASE 0x0003fffff8000000ULL
#define IA64VMM_SERVICE_SIZE 0x0000000000010000ULL
/* In-RAM NVRAM store, VMM-backed, not identity RAM. Same
 * format as the fork's VPC store ("IVARSTOR"; commit = 8-byte write of
 * "NVCOMMIT" at offset IA64VMM_NVRAM_SIZE - 8). */
#define IA64VMM_NVRAM_BASE 0x0003fffff8010000ULL
#define IA64VMM_NVRAM_SIZE 0x0000000000010000ULL
/* Virtual I/O SAPIC; the model provides redirection, remote IRR and EOI. */
/* Controller geometry, shared by the model and firmware handoff. */
#define IA64VMM_IOSAPIC_GSI_BASE 0u
#define IA64VMM_IOSAPIC_GSI_COUNT 24u
#define IA64VMM_SCI_GSI 9u
#define IA64VMM_IOSAPIC_BASE 0x0003fffff8020000ULL
#define IA64VMM_IOSAPIC_SIZE 0x0000000000001000ULL
/* Optional debug 16550 (stride 1); absent by default. Output is retained
 * byte-for-byte in the serial ring, independently of line-oriented logs. */
#define IA64VMM_UART_BASE 0x0003fffff8030000ULL
#define IA64VMM_UART_SIZE 0x0000000000001000ULL
/* IA-64 EFI runtime descriptors need an 8 KiB span, including reserved padding. */
#define IA64VMM_UART_RUNTIME_SIZE 0x0000000000002000ULL
/* SDM Vol.2 §5.8.4 default PIB. MADT's 32-bit address and 64-bit override
 * describe the same base.
 * AMD64 reserves this LAPIC range; never expose its physical backing. */
#define IA64VMM_PIB_BASE 0x00000000fee00000ULL
#define IA64VMM_PIB_SIZE 0x0000000000200000ULL
/* Legacy I/O port space, IA-64 sparse encoding:
 *   GPA = base + ((port >> 2) << 12 | (port & 0xfff))
 * Accesses are mediated by the port bridge (platform/): VMM-captured ports
 * are emulated, allow-listed ports are forwarded to the host, the rest read
 * all-ones and ignore writes (logged once per port). */
#define IA64VMM_LEGACY_IO_BASE 0x0003fffffc000000ULL
#define IA64VMM_LEGACY_IO_SIZE 0x0000000004000000ULL
/* ACPI PM block in port space, captured by the VMM: same port
 * layout as the fork's VPC machine so the firmware's FADT code is reused.
 * PM1_CNT SLP_EN -> shutdown request, reset register (0x200c, value 1) ->
 * reset request. */
#define IA64VMM_ACPI_PM_PORT_BASE 0x2000u
#define IA64VMM_ACPI_PM_PORT_SIZE 0x10u

/* --- firmware entry ------------------------------------------------------ */

/* The VMM starts the BSP at the firmware ELF entry with PSR = 0 (physical
 * mode, interruption collection and interrupts off), cr.lid = 0, and:
 *   r8  = GPA of struct ia64vmm_handoff (in guest RAM, inside fw_data)
 *   r9  = sizeof(struct ia64vmm_handoff)
 *   r10 = IA64VMM_PLATFORM_ID
 *   r1  = firmware __gp, r12 = stack top inside fw_data (16-byte aligned),
 *   ar.bsp = ar.bspstore = backing-store base inside fw_data, ar.rsc = 0,
 *   ar.k0 = IA64VMM_LEGACY_IO_BASE, b0 = entry.
 * Every other register has its power-on value. */
#define IA64VMM_PLATFORM_ID 0x00564d4du /* "VMM" */

#define IA64VMM_HANDOFF_MAGIC 0x3146464f484d4d56ULL /* "VMMHOFF1" */
#define IA64VMM_HANDOFF_VERSION 1u
#define IA64VMM_HANDOFF_MAX_RAM 64u
#define IA64VMM_HANDOFF_MAX_PCI_MMIO 64u
#define IA64VMM_HANDOFF_IRQ_VERSION 1u
#define IA64VMM_HANDOFF_MAX_IRQ_ROUTES 256u
struct __attribute__((packed)) ia64vmm_irq_route {
    uint16_t bdf, parent;     /* 0xffff parent = PCI root */
    uint8_t pin, reserved[3]; /* pin 0..3 (_PRT numbering) */
    uint32_t gsi;
};
struct __attribute__((packed)) ia64vmm_irq_bridge {
    uint16_t bdf, parent;
    uint8_t secondary, reserved[3];
};

/* handoff.flags */
#define IA64VMM_HF_SHUTDOWN_ON_BOOT_RETURN                                                         \
    (1u << 0) /* boot image returned -> ResetSystem(shutdown, status) */
#define IA64VMM_HF_NO_BOOT_MENU_WAIT (1u << 1) /* skip the hotkey window */
#define IA64VMM_HF_UART_PRESENT (1u << 2)      /* uart_base is valid */
/* PRESENT alone is a plain PNP0501 device; CONSOLE opts into EMS/PCDP. */
#define IA64VMM_HF_UART_CONSOLE (1u << 3)

/* Guest-visible physical or emulated i8042. */
#define IA64VMM_HF_I8042_ENABLED (1u << 4)
/* Real legacy decode of the inherited GOP's assigned PCI function. */
#define IA64VMM_HF_LEGACY_VGA (1u << 5)
#define IA64VMM_HF_DEBUG_SCREEN (1u << 6)
#define IA64VMM_HF_I8042_KEYBOARD_ONLY (1u << 7)
#define IA64VMM_VGA_MEMORY_BASE 0xa0000ULL
#define IA64VMM_VGA_MEMORY_SIZE 0x20000ULL
#define IA64VMM_VGA_GENERIC 0u
#define IA64VMM_VGA_BOCHS 1u
#define IA64VMM_VGA_VMWARE 2u
/* Sandy Bridge (gen6) IGD 8086:0102/0106/010a/0112/
 * 0116/0122/0126 at 00:02.0. vga_control_base/size is its GTTMMADR (BAR0). */
#define IA64VMM_VGA_INTEL_GEN6 3u

/* handoff.fb_format */
#define IA64VMM_FB_NONE 0u
#define IA64VMM_FB_BGRX8888 1u /* EFI PixelBlueGreenRedReserved8BitPerColor */
#define IA64VMM_FB_RGBX8888 2u /* EFI PixelRedGreenBlueReserved8BitPerColor */
#define IA64VMM_FB_BITMASK 3u  /* 32 bpp with the masks below */

/* handoff.boot_image_kind */
#define IA64VMM_BOOT_NONE 0u
#define IA64VMM_BOOT_PE_IMAGE 1u   /* an EFI application: LoadImage(buffer) + StartImage */
#define IA64VMM_BOOT_DISK_IMAGE 2u /* raw disk (MBR/GPT or FAT superfloppy): RAM-disk Block I/O */

struct __attribute__((packed)) ia64vmm_ram_range {
    uint64_t base;
    uint64_t size;
};

struct __attribute__((packed)) ia64vmm_handoff {
    uint64_t magic;
    uint32_t version;
    uint32_t size; /* sizeof(struct ia64vmm_handoff) */
    uint32_t flags;
    uint32_t cpu_count;
    uint64_t itc_hz; /* == IA64VMM_ITC_HZ */

    /* Guest RAM the firmware may use, identity-mapped, sorted, disjoint,
     * 4 KiB granular. Holes between ranges are not RAM. The firmware image,
     * fw_data and the boot image lie inside these ranges and must be
     * reported with their own EFI memory types. */
    uint32_t ram_range_count;
    uint32_t reserved0;
    struct ia64vmm_ram_range ram[IA64VMM_HANDOFF_MAX_RAM];

    uint64_t firmware_base; /* loaded firmware image span */
    uint64_t firmware_size;
    uint64_t fw_data_base; /* firmware-private RAM: stack, backing store, tables; */
    uint64_t fw_data_size; /* >= 32 MiB, 64 KiB aligned */

    uint64_t legacy_io_base, legacy_io_size;
    uint64_t service_base, service_size;
    uint64_t nvram_base, nvram_size;
    uint64_t ecam_base, ecam_size;
    uint32_t ecam_bus_first, ecam_bus_last;
    uint64_t pci_mmio_base, pci_mmio_size; /* host BAR aperture below 4 GiB */
    uint64_t iosapic_base;
    uint32_t iosapic_gsi_base, iosapic_gsi_count;
    uint64_t pib_base;
    uint64_t uart_base; /* valid with IA64VMM_HF_UART_PRESENT */
    uint32_t acpi_pm_port_base, acpi_pm_port_size;

    /* Inherited GOP framebuffer: direct guest MMIO. */
    uint64_t fb_base, fb_size;
    uint32_t fb_width, fb_height;
    uint32_t fb_pitch;  /* bytes per scan line */
    uint32_t fb_format; /* IA64VMM_FB_* */
    uint32_t fb_red_mask, fb_green_mask, fb_blue_mask, fb_reserved_mask;

    /* Boot media handed over as a module. */
    uint64_t boot_image_base, boot_image_size;
    uint32_t boot_image_kind; /* IA64VMM_BOOT_* */
    uint32_t reserved1;

    /* Root apertures, identity translation. The existing pair
     * describes memory below 4 GiB; this pair describes memory above it. */
    uint64_t pci_mmio64_base, pci_mmio64_size;
    uint64_t pci_io_base, pci_io_size;
    /* Page-rounded, merged assigned BARs for the EFI memory map. Root _CRS
     * apertures can contain holes and are not EFI memory descriptors. */
    uint32_t pci_mmio_range_count, pci_reserved;
    struct ia64vmm_ram_range pci_mmio[IA64VMM_HANDOFF_MAX_PCI_MMIO];
    /* Legacy memory is identity UC device memory. I/O is
     * forwarded through legacy_io_base; never backed by an emulated VGA. */
    uint64_t vga_base, vga_size;
    uint16_t vga_bdf, vga_vendor, vga_device, vga_kind;
    uint64_t vga_control_base, vga_control_size; /* Bochs BAR2 / VMware BAR0 */
    /* Version zero has no INTx facts. Each bus's
     * _PRT and bridge _ADR are generated from these assigned-function facts. */
    uint32_t irq_version, irq_route_count, irq_bridge_count, irq_reserved;
    struct ia64vmm_irq_route irq_routes[IA64VMM_HANDOFF_MAX_IRQ_ROUTES];
    struct ia64vmm_irq_bridge irq_bridges[IA64VMM_HANDOFF_MAX_IRQ_ROUTES];
    /* Nonzero (VMM option vga_probe=N): the firmware
     * enters VGA text on the assigned display early, holds N seconds for a
     * photograph, then restores the GOP state it saved. Zero: no probe. */
    uint32_t vga_probe_sec, vga_probe_reserved;
    /* Legacy VGA tail.
     * legacy_version 0: no legacy BIOS state, every other legacy_* field is
     * zero (the default display path). IA64VMM_HANDOFF_LEGACY_VERSION: the
     * VMM POSTed the assigned VGA's legacy VBIOS and set text mode 3
     * (display=vbios); the firmware installs no GOP (fb_format is NONE),
     * its console is VGA text at B8000, and it runs no GOP-to-VGA leave
     * sequence. Guest physical memory then holds, at the fixed addresses
     * below: the IVT and BDA POST left in guest RAM page 0, the post-POST
     * option ROM image at C0000 (legacy_rom_size bytes, read-only) and the
     * x86 stub segment at F0000 (read-only). Both are VMM-backed, not RAM:
     * the EFI map keeps C0000-FFFFF as UC MMIO. legacy_bdf is the card. */
    uint32_t legacy_version, legacy_flags;
    uint64_t legacy_ivt_base, legacy_ivt_size;
    uint64_t legacy_rom_base, legacy_rom_size;
    uint64_t legacy_stub_base, legacy_stub_size;
    uint16_t legacy_bdf, legacy_reserved[3];
    /* Independently versioned model report. Existing offsets
     * are unchanged. Firmware checks the outer length before reading this tail. */
    struct {
        uint32_t version, size, model, reserved;
        uint64_t cpuid3, cpuid4;
        char brand[64];
    } cpu_report;
};
/* Version-1 handoff without the model report; an absent tail preserves Montecito. */
#define IA64VMM_HANDOFF_BASE_SIZE offsetof(struct ia64vmm_handoff, cpu_report)
#define IA64VMM_CPU_REPORT_VERSION 1u
static inline void ia64vmm_handoff_cpu_model(struct ia64vmm_handoff *h, unsigned model)
{
    h->cpu_report.version = IA64VMM_CPU_REPORT_VERSION;
    h->cpu_report.size = sizeof(h->cpu_report);
    h->cpu_report.model = model;
    h->cpu_report.reserved = 0;
    h->cpu_report.cpuid3 = ia64_model_cpuid(model, 3);
    h->cpu_report.cpuid4 = ia64_model_cpuid(model, 4);
    const char *brand = ia64_model_brand(model);
    unsigned i = 0;
    for (; brand[i]; ++i)
        h->cpu_report.brand[i] = brand[i];
    for (; i < sizeof(h->cpu_report.brand); ++i)
        h->cpu_report.brand[i] = 0;
}
static inline bool ia64vmm_handoff_cpu_valid(const struct ia64vmm_handoff *h)
{
    if (h->size == IA64VMM_HANDOFF_BASE_SIZE)
        return true;
    if (h->size < sizeof(*h) || h->cpu_report.version != IA64VMM_CPU_REPORT_VERSION ||
        h->cpu_report.size < sizeof(h->cpu_report) ||
        h->cpu_report.size > h->size - IA64VMM_HANDOFF_BASE_SIZE || h->cpu_report.reserved ||
        !ia64_cpu_model_valid(h->cpu_report.model) ||
        h->cpu_report.cpuid3 != ia64_model_cpuid(h->cpu_report.model, 3) ||
        h->cpu_report.cpuid4 != ia64_model_cpuid(h->cpu_report.model, 4))
        return false;
    const char *brand = ia64_model_brand(h->cpu_report.model);
    unsigned i = 0;
    for (; brand[i]; ++i)
        if (h->cpu_report.brand[i] != brand[i])
            return false;
    return !h->cpu_report.brand[i];
}
/* Copy only bytes the producer supplied. Consumers keep a complete local
 * handoff so old producers need not map or initialize the new tail. */
static inline void ia64vmm_handoff_copy(struct ia64vmm_handoff *to,
                                        const struct ia64vmm_handoff *from)
{
    unsigned char *dst = (unsigned char *)to;
    const unsigned char *src = (const unsigned char *)from;
    size_t length = from->size < sizeof(*to) ? from->size : sizeof(*to);
    for (size_t i = 0; i < length; ++i)
        dst[i] = src[i];
    for (size_t i = length; i < sizeof(*to); ++i)
        dst[i] = 0;
    if (length == IA64VMM_HANDOFF_BASE_SIZE)
        ia64vmm_handoff_cpu_model(to, IA64_CPU_MONTECITO);
    to->cpu_report.size = sizeof(to->cpu_report);
    to->size = sizeof(*to);
}
#define IA64VMM_HANDOFF_LEGACY_VERSION 1u
#define IA64VMM_LEGACY_TEXT_CONSOLE (1u << 0) /* VGA text console, no GOP */
#define IA64VMM_LEGACY_IVT (1u << 1)          /* page 0 holds the IVT/BDA */
#define IA64VMM_LEGACY_ROM (1u << 2)          /* C0000 holds the image */
#define IA64VMM_LEGACY_STUB (1u << 3)         /* F0000 holds the stubs */
#define IA64VMM_LEGACY_IVT_BASE 0x0ULL
#define IA64VMM_LEGACY_IVT_SIZE 0x500ULL /* IVT 0-3FF, BDA 400-4FF */
#define IA64VMM_LEGACY_ROM_BASE 0xc0000ULL
#define IA64VMM_LEGACY_ROM_MAX 0x20000ULL /* C0000-DFFFF option ROM area */
#define IA64VMM_LEGACY_STUB_BASE 0xf0000ULL
#define IA64VMM_LEGACY_STUB_SIZE 0x10000ULL
#define IA64VMM_LEGACY_AREA_BASE 0xc0000ULL /* UC MMIO in the EFI map */
#define IA64VMM_LEGACY_AREA_SIZE 0x40000ULL

/* --- VMM service window (offsets from service_base) ------------------------ *
 * All registers are 64-bit, naturally aligned; reads of write-only registers
 * return 0, writes to read-only registers are ignored and logged. */
#define IA64VMM_SVC_MAGIC 0x000u   /* R: "VMMSVC01" */
#define IA64VMM_SVC_VERSION 0x008u /* R: 1 */
#define IA64VMM_SVC_CONSOLE                                                                        \
    0x010u /* W: low byte = one character of firmware console text (any width) */
#define IA64VMM_SVC_RTC 0x018u /* R: current time, Unix seconds UTC */
#define IA64VMM_SVC_WATCHDOG                                                                       \
    0x020u /* W: EFI watchdog timeout in seconds, 0 = disarm (logged only) */
#define IA64VMM_SVC_RESET_STATUS                                                                   \
    0x028u /* W: EFI_STATUS passed to ResetSystem; write before RESET */
#define IA64VMM_SVC_RESET                                                                          \
    0x030u /* W: EFI_RESET_TYPE (0 cold, 1 warm, 2 shutdown, 3 platform) -> vCPU exits */
#define IA64VMM_SVC_HALT 0x038u   /* W: idle hint, same as PAL_HALT_LIGHT */
#define IA64VMM_SVC_PANIC 0x040u  /* W: firmware fatal error code: VMM dumps vCPU state and fails */
#define IA64VMM_SVC_DEBUG 0x048u  /* W: value logged in hex (firmware debugging aid) */
#define IA64VMM_SVC_ITC_HZ 0x050u /* R: guest ITC frequency */
/* Raw PI FVB update boundary. No bytes in NVRAM are reserved
 * for a commit marker. This records a dirty generation in RAM; persistence
 * must serialize the entire FV/FTW window. */
#define IA64VMM_SVC_NVRAM_COMMIT 0x058u /* W: completed FVB mutation, value ignored */

/* Diagnostics: final map byte count, accepted key, then loader ranges.
 * Range writes are BASE, SIZE, TYPE (EFI 1=LoaderCode, 2=LoaderData).
 * EXIT_BOOT_SERVICES asks for a diagnostic exit at the next instruction boundary. */
#define IA64VMM_SVC_MAP_SIZE 0x060u
#define IA64VMM_SVC_EXIT_BOOT_SERVICES 0x068u
#define IA64VMM_SVC_LOAD_BASE 0x078u
#define IA64VMM_SVC_LOAD_SIZE 0x080u
#define IA64VMM_SVC_LOAD_TYPE 0x088u
/* Loaded PE images are excluded from the kernel-continuation check. */
#define IA64VMM_SVC_IMAGE_BASE 0x090u
#define IA64VMM_SVC_IMAGE_SIZE 0x098u
#define IA64VMM_SVC_INPUT_KEY                                                                      \
    0x0a0u /* W8: ConIn key read/event accepted, scan:31..16 / Unicode:15..0 */
#define IA64VMM_INPUT_KEY_EVENT (UINT64_C(1) << 32)
#define IA64VMM_SVC_CONSOLE_FLUSH 0x0a8u /* W8: flush a newline-free prompt before ConIn polling */
/* Optional ownership hint: preserve the guest GOP on successful shutdown,
 * including the VMM's final diagnostics. The reference firmware leaves it 0. */
#define IA64VMM_SVC_GOP_OWNED 0x0b0u /* W: nonzero = firmware owns framebuffer */
/* Applications can inspect the display/debug policy without probing absent MMIO. */
#define IA64VMM_SVC_DEBUG_FLAGS 0x0d8u /* R: handoff debug/display flags */
/* EFI PCI I/O may request an x86 word/dword port at an unaligned address.
 * IA-64 cannot issue that as an aligned MMIO instruction. These registers
 * route the same allow-listed host operation without decomposing its width. */
#define IA64VMM_SVC_PORT_SELECT 0x0e0u /* W8: port[15:0], width (1/2/4)[18:16] */
#define IA64VMM_SVC_PORT_DATA 0x0e8u   /* R/W8: selected real host port */

/* TR snapshot, per processor. SELECT = (is_dtr << 8) | index.
 * READ takes an 8-byte-aligned physical RAM buffer of 32 bytes (UC alias
 * permitted). STATUS is signed PAL status; VALID is PAL_VM_TR_READ's
 * four-bit field-valid mask. Invalid buffers are rejected before any write. */
#define IA64VMM_SVC_TR_SELECT 0x0b8u
#define IA64VMM_SVC_TR_READ 0x0c0u
#define IA64VMM_SVC_TR_STATUS 0x0c8u
#define IA64VMM_SVC_TR_VALID 0x0d0u
struct vcpu;
void ia64_pal_tr_select(struct vcpu *vcpu, uint64_t selector);
void ia64_pal_tr_read(struct vcpu *vcpu, uint64_t buffer);
uint64_t ia64_pal_tr_status(const struct vcpu *vcpu);
uint64_t ia64_pal_tr_valid(const struct vcpu *vcpu);
/* Native PAL hardware resources, per processor, all 0
 * after reset. XR0 is the min-state save area address PAL_MC_REGISTER_MEM
 * records; EXPECTED the PAL_MC_EXPECTED indicator (0 or 1); PALE_PMI the
 * PALE_PMI entrypoint, set by PAL_COPY_PAL;
 * SALE_PMI the SAL PMI entrypoint of PAL_PMI_ENTRYPOINT. EVENT describes
 * the INIT or machine check last entered, PMI_VECTOR the vector of the PMI
 * last entered; both are read only. */
#define IA64VMM_SVC_PAL_XR0 0x0f0u        /* R/W8 */
#define IA64VMM_SVC_PAL_EXPECTED 0x0f8u   /* R/W8 */
#define IA64VMM_SVC_PAL_PALE_PMI 0x100u   /* R/W8 */
#define IA64VMM_SVC_PAL_SALE_PMI 0x108u   /* R/W8 */
#define IA64VMM_SVC_PAL_EVENT 0x110u      /* R8 */
#define IA64VMM_SVC_PAL_PMI_VECTOR 0x118u /* R8 */
/* VMM-private PAL operations, completed after the requesting instruction
 * leaves the engine. SAVE writes Figure 11-2 and covers the frame; RESUME
 * restores it and transfers control. RESUME bit 0 is new_context; remaining
 * bits are the UC min-state address. SAL may use offsets 0x200 through 0x3ff.
 * These services do not expose Intel-private XR registers or rfi encodings. */
#define IA64VMM_SVC_PAL_STATE_SAVE 0x120u   /* W8 */
#define IA64VMM_SVC_PAL_STATE_RESUME 0x128u /* W8 */
/* EVENT bits 7:0: the SALE_ENTRY function (SDM Vol.2 Table 11-11).
 * COLLECTED: PSR.ic was 1, so IIP, IPSR and IFS describe the interrupted
 * context. TLB: the machine check is a TLB check (Table 4-1 TR overlap).
 * TLB_PURGE: that TLB check follows a purge that completed; without it the
 * overlapping insert had no effect. */
#define IA64VMM_PAL_EVENT_MCA 1u
#define IA64VMM_PAL_EVENT_INIT 2u
#define IA64VMM_PAL_EVENT_COLLECTED (1u << 8)
#define IA64VMM_PAL_EVENT_TLB (1u << 9)
#define IA64VMM_PAL_EVENT_TLB_PURGE (1u << 10)
uint64_t ia64_pal_service_read(const struct vcpu *vcpu, unsigned offset);
void ia64_pal_service_write(struct vcpu *vcpu, unsigned offset, uint64_t value);
/* Complete queued PAL operations after engine_run, before the next instruction.
 * This keeps state transfers outside lockstep's CPU memory transaction. */
void ia64_pal_complete_services(struct vcpu *vcpu);

/* LID convention shared by SAL, MADT and the CPU reset path: id=i, eid=0. */
#define IA64VMM_MAX_CPUS 256u
#define IA64VMM_CPU_LID(index) ((uint64_t)(index) << 24)

/* Entry stack and early backing store of CPU i inside fw_data;
 * i = 0 is the BSP. Equal to the firmware ABI's IA64_FW_BOOTSTRAP_STACK_TOP_OFFSET, IA64_FW_CPU_STACK_SIZE,
 * IA64_FW_EARLY_RSE_OFFSET and IA64_FW_EARLY_RSE_SIZE. */
#define IA64VMM_ENTRY_STACK_TOP_OFFSET 0x1000000u /* r12 = fw_data + this - 16 - i * stride */
#define IA64VMM_ENTRY_STACK_STRIDE 0x20000u
#define IA64VMM_ENTRY_RSE_OFFSET 0x600000u /* bsp = bspstore = fw_data + this + i * stride */
#define IA64VMM_ENTRY_RSE_STRIDE 0x8000u
/* Supported vCPU counts: the product limit,
 * which is the new firmware's, and the reference firmware's own limit (its
 * FW_MAX_CPUS). Setup rejects a count above the selected firmware's
 * limit before creating anything; the array bounds (IA64VMM_MAX_CPUS) stay. */
#define IA64VMM_VCPU_LIMIT 32u
/* The entry register set of CPU i, which
 * vcpu_reset gives AP i before its reset-hold (platform/handoff.c);
 * platform_handoff returns CPU 0's. */
struct ia64_entry_regs;
void platform_entry_regs(const struct ia64vmm_handoff *h, uint64_t handoff_gpa, uint64_t entry,
                         uint64_t gp, unsigned cpu, struct ia64_entry_regs *regs);

#define IA64VMM_SVC_MAGIC_VALUE 0x31304356534d4d56ULL /* "VMMSVC01" */

/* CPU bus fallback for an unclaimed load/store in a PCI/legacy aperture.
 * Called only after gm_lookup fails, with PA width/alignment validated.
 * Does not add RAM/device backing to gm_*; standalone CPU tests use NULL. */
struct gm_region;
const struct gm_region *platform_unclaimed_region(uint64_t address, unsigned size);

/* Diagnostic observer after a completed native MMIO load/store of
 * 1/2/4/8 bytes. Receives the transaction's value; must not access the
 * device again. Standalone CPU fixtures provide a weak no-op. */
void platform_display_memory_access(struct vcpu *vcpu, uint64_t address, unsigned size, bool write,
                                    uint64_t value);

#endif
