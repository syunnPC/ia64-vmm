/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_PLATFORM_INTERNAL_H
#define VMM_PLATFORM_INTERNAL_H
#include "ia64vmm/bootinfo.h"
#include "ia64vmm/engine.h"
#include "ia64vmm/guest_mem.h"
#include "ia64vmm/platform.h"
#include "ia64vmm/interrupts.h"
#include "platform/lock.h"

/* pale_init, pale_check: the native firmware's fixed PALE_INIT and
 * PALE_CHECK, 0 when the image has none. */
struct fw_image {
    uint64_t base, size, alignment, entry, global_pointer, pal_entry, pale_init, pale_check;
};
struct platform_state;
/* A nonzero base is useful for relocation tests; zero chooses guest RAM. */
bool fw_load(const void *elf, size_t size, uint64_t base, struct fw_image *out);
bool platform_ram_contains(uint64_t base, uint64_t size);
bool platform_ram_write(uint64_t base, const void *src, uint64_t size);
bool platform_find_ram(uint64_t size, uint64_t alignment, uint64_t limit,
                       const struct ia64vmm_ram_range *avoid, unsigned count, bool highest,
                       uint64_t *base);
bool platform_check_window(const struct boot_info *bi);
bool platform_handoff_ram_append(struct ia64vmm_handoff *h, uint64_t base, uint64_t size);
bool platform_handoff_ram(struct ia64vmm_handoff *h);
bool platform_handoff(const struct boot_info *bi, const struct fw_image *fw,
                      struct ia64vmm_handoff *h, uint64_t *gpa, struct ia64_entry_regs *regs);
/* CPU model: selected profile supplied before constructing/writing the handoff. */
bool platform_handoff_model(const struct boot_info *bi, const struct fw_image *fw,
                            enum ia64_cpu_model model, struct ia64vmm_handoff *h, uint64_t *gpa,
                            struct ia64_entry_regs *regs);
bool platform_cmdline_value(const char *line, const char *key, const char **value, size_t *length);
/* Debug policy is established before registering devices, also in hosted tests. */
bool platform_debug_policy(const char *cmdline, struct ia64vmm_handoff *h);
bool platform_debug_uart_present(void);
uint32_t platform_debug_flags(void);
void platform_debug_display_policy(const struct ia64vmm_handoff *h);
/* Sandy Bridge (gen6) IGD at 00:02.0, and whether it decodes legacy VGA
 * (memory decode on, GGC.IVD clear; GGC only read). */
bool platform_vga_intel_gen6(const struct pci_device *d);
bool platform_vga_intel_gen6_decodes(const struct pci_device *d, uint16_t *ggc);
/* Fixed DISPI ownership; VMSVGA is probed only with verified VGA routing. */
bool platform_vga_has_dispi(const struct pci_device *d);
bool platform_vga_handoff(const struct boot_framebuffer *, struct ia64vmm_handoff *);
bool platform_vga_map(struct platform_state *);
/* Generic-route evidence: format the host's C0000 option ROM header (rom
 * NULL: not readable) and IVT int 10h vector (ivt = bytes at 0x40, NULL: not
 * readable) as one log line. Returns the formatted length. */
size_t platform_legacy_rom_describe(char *out, size_t size, const uint8_t *rom, size_t rom_len,
                                    const uint8_t *ivt, size_t ivt_len);
/* VMX: line 0-2 of the host snapshot description. */
size_t platform_legacy_snapshot_describe(char *out, size_t size, unsigned line, const uint8_t *low,
                                         size_t low_len, const uint8_t *option, size_t option_len);

struct platform_abort_range {
    uint64_t base;
    struct platform_state *state;
    struct gm_region region;
};
#define PLATFORM_LOADER_RANGES 256u
#define PLATFORM_IMAGE_RANGES 64u
/* Where a guest reset request came from. */
enum platform_reset_source {
    PLATFORM_RESET_EFI,
    PLATFORM_RESET_ACPI,
    PLATFORM_RESET_CF9,
    PLATFORM_RESET_I8042
};
struct platform_state {
    unsigned service_lock, nvram_lock, pm_lock, uart_lock, port_lock, abort_lock;
    uint8_t *nvram;
    _Atomic uint64_t reset_type, reset_status; /* Executor terminal-status readers. */
    uint64_t pending_reset_status;             /* SVC_RESET_STATUS until SVC_RESET (service_lock) */
    unsigned reset_source;                     /* enum platform_reset_source */
    bool i8042_output_pending; /* D1h written at 64h; the next 60h byte is the output port */
    uint64_t panic_code, watchdog, commits;
    _Atomic bool reset_requested, panic;
    bool iosapic_logged, pib_logged;
    uint64_t map_size, map_key, load_base, load_size;
    _Atomic bool boot_services_exited; /* Other executors observe BSP EBS. */
    bool handoff_pending, loader_resumed, ebs_exit_requested;
    unsigned loader_range_count, image_range_count;
    bool preboot_overflow;
    uint64_t image_base;
    /* Loaded images, every pre-exit code allocation and the EBS caller. */
    struct ia64vmm_ram_range image_ranges[PLATFORM_IMAGE_RANGES + PLATFORM_LOADER_RANGES + 1];
    struct ia64vmm_ram_range loader_ranges[PLATFORM_LOADER_RANGES];
    uint8_t denied_ports[65536 / 8], allowed_ports[65536 / 8];
    uint16_t pm_status, pm_enable, pm_control;
    uint8_t uart_lcr;
    uint8_t uart_ier, uart_mcr, uart_scr, uart_dll, uart_dlm, uart_fcr, uart_rx;
    bool uart_rx_ready, uart_thre_pending;
    /* Subtractive decode diagnostics are sparse, including 64-bit PCI holes. */
    struct platform_abort_log *abort_log;
    /* Every maximal gap of the guest physical space below
     * 2^IA64VMM_PHYS_ADDR_BITS: n sorted, disjoint gm regions leave at most
     * n + 1 gaps, and n <= GM_MAX_REGIONS. */
    struct platform_abort_range abort_ranges[GM_MAX_REGIONS + 1];
    unsigned abort_range_count, abort_regions_seen;
    uint32_t pci_config_address;  /* Guest CF8 latch; never the host selector. */
    struct ia64_iosapic iosapic;  /* I1; GSI range matches the handoff/MADT. */
    _Atomic bool guest_gop_owned; /* Optional firmware service hint; reset at guest init. */
    /* Opt-in port_trace=N[:FIRST-LAST]: OS-phase port accesses left to log
     * and the (hex) port range they are counted in. */
    unsigned port_trace;
    uint16_t port_trace_first, port_trace_last;
};
/* PCI bus/configuration lock also serializes MSI-X pages with guest config. */
void platform_pci_lock(void);
void platform_pci_unlock(void);
bool platform_devices_init(struct platform_state *s);
/* Next guest boot of a restart: every per-boot field
 * takes its power-on value; NVRAM, the port allow-list, the master-abort
 * ranges and every registered region stay. Executors are stopped. */
void platform_devices_restart(struct platform_state *s);
/* A reset request that restarts the guest: cold, warm or platform-specific,
 * without a firmware panic. */
bool platform_reset_restarts(const struct platform_state *s);
const char *platform_reset_source_name(unsigned source);
bool platform_abort_init(struct platform_state *s);
void platform_abort_cleanup(struct platform_state *s);
extern const struct gm_mmio_ops platform_uart_ops;
void platform_devices_cleanup(struct platform_state *s);
/* Explicit host-port forwarding; the default is an empty allow-list. */
bool platform_allow_ports(struct platform_state *s, uint16_t first, unsigned count);
bool platform_decode_port(uint64_t offset, uint16_t *port);
uint64_t platform_rtc_seconds(void);
/* CF8h/CFCh mechanism #1 over the mediated configuration space (the guest's
 * port bridge and the VBIOS container). visible, when set, restricts the
 * reachable functions: others read all-ones and drop writes. */
typedef bool (*platform_config_filter)(void *ctx, uint16_t bdf);
bool platform_config_ports(uint16_t port, unsigned size);
uint64_t platform_config_port_read(uint32_t *latch, uint16_t port, unsigned size,
                                   platform_config_filter visible, void *ctx);
void platform_config_port_write(uint32_t *latch, uint16_t port, uint64_t value, unsigned size,
                                platform_config_filter visible, void *ctx);
/* display=vbios: POST the GPU's legacy
 * VBIOS in the x86 real-mode container and set text mode 3, once, before
 * the guest. False only for an invalid option (setup failure); every other
 * problem is logged and the VMM continues on the GOP framebuffer. */
bool platform_vbios_post(const struct boot_info *bi);
/* Guest restart, executors stopped: what VMM start
 * did for the configured options when it reached text mode. vbios_call=post
 * POSTs the image again and sets mode 3; vbios_call=mode sets mode 3 on the
 * state the guest left, with its BDA video state. False only when a
 * configured vbios_reset left the GPU unlike the inventory. */
bool platform_vbios_restart(void);
/* Record the boot GOP aperture before either default or VBIOS ROM snapshots. */
void platform_pci_boot_framebuffer(const struct boot_framebuffer *fb);
/* State the POST left, kept for the guest handoff. */
struct platform_vbios_state {
    bool posted; /* POST and mode 3 succeeded */
    uint16_t bdf;
    uint32_t runtime_size; /* post-POST byte 2 x 512 of the shadow */
    const uint8_t *low;    /* guest 0-9FFFFh: IVT at 0, BDA at 400h */
    const uint8_t *shadow; /* guest C0000h, shadow_size bytes */
    uint32_t shadow_size;
    const uint8_t *stub; /* the container's F0000-FFFFFh */
    uint8_t last_bus;    /* B101h CL */
};
const struct platform_vbios_state *platform_vbios_state(void);
/* legacy VGA: after a successful POST, give
 * the guest what POST left (handoff legacy_* tail, fb_format NONE, page 0
 * IVT/BDA with the IVT redirected to the guest stub segment). True without
 * a POST: the default path stays as it is. */
bool platform_vbios_handoff(struct ia64vmm_handoff *h);
/* Register the read-only C0000 image and F0000 stub regions (before the
 * master-abort ranges are built), and log their guest accesses at cleanup. */
bool platform_vbios_map(struct platform_state *s);
void platform_vbios_cleanup(void);

#endif
