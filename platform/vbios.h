/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VBIOS environment: the pieces of
 * platform/vbios.c that are pure functions of their inputs, exposed for
 * platform/tests/vbios.c. Guest addresses are real-mode physical.
 */
#ifndef VMM_PLATFORM_VBIOS_H
#define VMM_PLATFORM_VBIOS_H
#include "ia64vmm/pci.h"
#include "ia64vmm/x86rm.h"

#define VBIOS_LOW_SIZE 0xa0000u /* conventional memory, no EBDA */
#define VBIOS_VGA_BASE 0xa0000u
#define VBIOS_VGA_SIZE 0x20000u
#define VBIOS_SHADOW_BASE 0xc0000u
#define VBIOS_SHADOW_MAX 0x20000u /* C0000-DFFFFh option ROM area */
#define VBIOS_STUB_BASE 0xf0000u
#define VBIOS_STUB_SIZE 0x10000u
#define VBIOS_STUB_SEGMENT 0xf000u
/* Stub offsets in segment F000h. *_DONE is the hypercall. */
#define VBIOS_POST_IP 0xe000u
#define VBIOS_POST_DONE 0xe005u
#define VBIOS_MODE_IP 0xe010u
#define VBIOS_MODE_DONE 0xe015u
#define VBIOS_CLI_IP 0xe020u
#define VBIOS_CALL10_IP 0xe028u /* int 10h; hypercall: the live-state calls */
#define VBIOS_CALL10_DONE 0xe02au
/* The container's entry for vector n at F000:D000 + 4n, each
 * "hypercall; iret"; the IVT of vbios_build_low points every vector but
 * 10h and 1Ah there. */
#define VBIOS_VECTOR_TABLE 0xd000u
#define VBIOS_VECTOR_ENTRY(n) (VBIOS_VECTOR_TABLE + 4u * (n))
#define VBIOS_INT10_IP 0xf065u
#define VBIOS_INT1A_IP 0xfe6eu
#define VBIOS_IRET_IP 0xff53u
#define VBIOS_STACK_TOP 0x7c00u
#define VBIOS_TICKS_PER_DAY 0x1800b0u /* PC BIOS IRQ0 ticks in a day */
#define VBIOS_MAX_MAPS (4u + 6u + 1u) /* fixed, card BARs, an IGD OpRegion */
#define VBIOS_MAX_BRIDGES 16u

/* A legacy executable image. PCI metadata is zero for classic platform ROMs. */
struct vbios_image {
    uint64_t offset;    /* image start in the snapshot */
    uint32_t length;    /* PCI image length, or classic header length */
    uint32_t init_size; /* byte 2 x 512: what is shadowed */
    uint16_t vendor, device;
    uint8_t revision;    /* PCIR revision */
    bool checksum_ok;    /* 8-bit sum of the init size is zero */
    uint32_t class_code; /* PCIR +0Dh..+0Fh */
    uint8_t checksum;    /* the 8-bit sum itself */
};
bool vbios_find_legacy(const uint8_t *rom, uint64_t size, struct vbios_image *out);

/* The C0000 image independent of initialized IVT state: a PCI image of
 * the card, or a bounded classic option ROM with a valid checksum. */
const char *vbios_host_rom(const uint8_t *option, uint64_t option_size,
                           const struct pci_device *card, struct vbios_image *out);

/* Host image: the C0000 snapshot accepted as the
 * POSTed BIOS of the card: a code type 0 image at offset 0 whose PCIR
 * vendor is the card's and class 03xxxxh, with the snapshot's int 10h
 * vector inside its runtime size (byte 2 x 512, the image's init_size).
 * NULL when accepted, else the refusal. */
const char *vbios_host_image(const uint8_t *option, uint64_t option_size, const uint8_t *low,
                             uint64_t low_size, const struct pci_device *card,
                             struct vbios_image *out);
/* In a copy of the host's IVT, every entry outside
 * [C0000, C0000 + image_size) is pointed at the container's entry for its
 * vector. Returns the number of entries changed. */
unsigned vbios_host_ivt(uint8_t *ivt, uint32_t image_size);

/* The card and its upstream bridges: the only functions the ROM reaches
 * through CF8h/CFCh and int 1Ah. */
struct vbios_card {
    uint16_t bdf;
    uint16_t bridges[VBIOS_MAX_BRIDGES]; /* nearest first */
    unsigned bridge_count;
    uint8_t last_bus;
    /* For an Intel IGD at 00:02.0, the host bridge 00:00.0 and the
     * LPC bridge 00:1F.0: readable, never written. */
    uint16_t readonly[2];
    unsigned readonly_count;
};
bool vbios_card_from_inventory(uint16_t bdf, struct vbios_card *card);
bool vbios_visible(void *card, uint16_t bdf);
/* The functions a configuration write of the VBIOS reaches: the card and
 * its bridges, not the read-only functions. */
bool vbios_writable(void *card, uint16_t bdf);
/* A vendor-8086 class-0300 function at 00:02.0. */
bool vbios_intel_igd(const struct pci_device *d);

/* Guest physical map and the I/O pass-through bitmap. */
struct vbios_layout {
    struct x86rm_mapping map[VBIOS_MAX_MAPS];
    unsigned map_count;
    uint8_t io_pass[8192];
};
bool vbios_build_layout(const struct pci_device *card, uint8_t *low, uint8_t *shadow,
                        uint32_t shadow_size, uint8_t *stub, struct vbios_layout *out);
/* The IGD OpRegion at ASLS, 8 KiB, widened to whole pages, when
 * the BootInfo map types every page of it ACPI NVS or reserved. NULL with
 * *base and *size set when it may be mapped, else the reason. */
struct boot_info;
const char *vbios_opregion_range(const struct boot_info *bi, uint32_t asls, uint64_t *base,
                                 uint64_t *size);
/* One more identity mapping (read/write, write-back): false when the map
 * is full or the range overlaps one already there. */
bool vbios_add_mapping(struct vbios_layout *l, uint64_t base, uint64_t size, unsigned flags);

/* vbios_trace_io=N: the card's I/O
 * BAR ports exit instead of passing; each access is forwarded to the card
 * unchanged and recorded with the VBIOS CS:IP. An access at a port p with
 * p & 7 == 4 is the data port of the index written last at p - 4 (NVIDIA's
 * BAR5 window: index at +8, data at +0Ch; +10h/+14h; the key at +0/+4). */
#define VBIOS_TRACE_MAX 1024u
#define VBIOS_TRACE_REGS 64u
#define VBIOS_TRACE_LATCHES 16u
struct vbios_io_event {
    uint64_t seq;
    uint32_t value, index;
    uint16_t cs, ip, port;
    uint8_t size;
    bool write, paired;
};
struct vbios_io_reg {
    uint64_t count;
    uint32_t index, first_value, last_value;
    uint16_t port, last_cs, last_ip;
    bool write, paired, changed;
};
struct vbios_io_trace {
    unsigned limit;
    uint64_t count, regs_lost;
    struct vbios_io_event first[VBIOS_TRACE_MAX], last[VBIOS_TRACE_MAX];
    struct {
        uint16_t port;
        uint32_t value;
    } latches[VBIOS_TRACE_LATCHES];
    unsigned latch_count, reg_count;
    struct vbios_io_reg regs[VBIOS_TRACE_REGS];
};
void vbios_trace_start(struct vbios_io_trace *t, unsigned limit);
void vbios_trace_record(struct vbios_io_trace *t, uint16_t cs, uint16_t ip, uint16_t port,
                        unsigned size, bool write, uint32_t value);
/* The events kept, oldest first: the first `limit`, then those of the last
 * `limit` not among them. Returns how many were stored in out. */
unsigned vbios_trace_kept(const struct vbios_io_trace *t, const struct vbios_io_event **out,
                          unsigned max);
/* The registers (paired index, else port) by access count, most first. */
unsigned vbios_trace_top(const struct vbios_io_trace *t, const struct vbios_io_reg **out,
                         unsigned max);
/* Clear the IOPM pass bits of the card's I/O BARs (they exit to be traced). */
void vbios_trace_ports(const struct pci_device *card, struct vbios_layout *l);

/* Contents: the stub segment (64 KiB), and the IVT/BDA of a PC BIOS in the
 * first page of conventional memory. */
void vbios_build_stubs(uint8_t *segment, const uint8_t hypercall[3]);
/* The guest's stub segment with the last PCI bus for B101h. */
void vbios_build_guest_stub(uint8_t *segment, uint8_t last_bus);
/* Guest display diagnostic veneer appended after the runtime image. Returns its
 * extended, 512-byte-aligned size, or zero without changing anything. */
uint32_t vbios_trace_install(uint8_t *low, uint8_t *image, uint32_t runtime, uint32_t capacity,
                             uint8_t *stub);
/* Point the IVT entries the container's stub served at the guest stub;
 * returns the entries changed, *odd the F000h vectors without a guest
 * counterpart (sent to the default iret). */
unsigned vbios_redirect_ivt(uint8_t *ivt, unsigned *odd);
void vbios_build_low(uint8_t *low, uint32_t ticks);

/* 18.2 Hz BIOS ticks in `elapsed` counter units at `hz`, exact. */
uint64_t vbios_ticks(uint64_t elapsed, uint64_t hz);
/* Counter units from the start until tick `tick` begins (rounded up). */
uint64_t vbios_tick_time(uint64_t tick, uint64_t hz);
/* 0040:006C = (start + elapsed) wrapped at a day; the midnight flag at
 * 0040:0070 is set once for each day boundary passed (*days counts them). */
void vbios_store_ticks(uint8_t *low, uint32_t start, uint64_t elapsed_ticks, uint64_t *days);
uint32_t vbios_ticks_of_day(uint64_t seconds);

/* Length of the instruction an exit names, prefixes included. */
bool vbios_insn_length(const uint8_t *code, unsigned avail, enum x86rm_exit_reason reason,
                       unsigned *length);

/* The answer to a software INT n that reached the container's
 * entry for vector n (or F065h for 10h). Returns 0: registers and FLAGS
 * unchanged, 1: handled with CF clear, 2: with CF set. post_hooks: the
 * int 15h 5Fxx answers of vbios_call=post (coreboot's Intel GMA hooks);
 * without it every 5Fxx is unhandled. low: the container's BDA. */
unsigned vbios_vector(unsigned vector, bool post_hooks, const uint8_t *low, struct x86rm_regs *r);

/* int 1Ah PCI BIOS. Returns 0: registers untouched (not B1h),
 * 1: handled with CF clear, 2: handled with CF set. */
unsigned vbios_int1a(const struct vbios_card *card, struct x86rm_regs *r);
/* CF8h-CFFh through the shared mediation, restricted to the card. */
uint64_t vbios_config_read(struct vbios_card *card, uint32_t *latch, uint16_t port, unsigned size);
void vbios_config_write(struct vbios_card *card, uint32_t *latch, uint16_t port, uint64_t value,
                        unsigned size);
#endif
