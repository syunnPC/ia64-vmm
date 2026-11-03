/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Evidence for the generic display route: what
 * the host firmware left at physical C0000 and in the real-mode interrupt
 * vector table. Formats bytes the caller already read; nothing is mapped,
 * written or executed here.
 *  - Option ROM header: 55 AA, initialization size in 512-byte units at +2,
 *    PCI Data Structure offset at +0x18; "PCIR": vendor +4, device +6,
 *    class code +0xD..+0xF (programming interface, subclass, base class),
 *    image length in 512-byte units +0x10, code type +0x14 (0 = x86 PC-AT,
 *    3 = EFI), indicator +0x15 bit 7 = last image (PCI Firmware
 *    Specification 3.0 §5.1-5.2).
 *  - IVT entry n at 4*n: offset, then segment (int 10h at 0x40). */
#include "platform/platform.h"
#include "ia64vmm/log.h"

static unsigned le16(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}
struct text {
    char *out;
    size_t size, used;
};
static void add(struct text *t, const char *fmt, ...) IA64VMM_PRINTF(2, 3);
static void add(struct text *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vmm_vsnprintf(t->out + t->used, t->used < t->size ? t->size - t->used : 0, fmt, ap);
    va_end(ap);
    t->used += (size_t)n;
    if (t->used >= t->size)
        t->used = t->size ? t->size - 1 : 0;
}
size_t platform_legacy_rom_describe(char *out, size_t size, const uint8_t *rom, size_t rom_len,
                                    const uint8_t *ivt, size_t ivt_len)
{
    struct text t = {out, size, 0};
    if (size)
        out[0] = 0;
    add(&t, "host C0000:");
    if (!rom || rom_len < 3)
        add(&t, " not in the host memory map, not read");
    else {
        add(&t, " %02x %02x size=%02x", rom[0], rom[1], rom[2]);
        if (rom[0] != 0x55 || rom[1] != 0xaa)
            add(&t, " (no option ROM signature)");
        else {
            add(&t, " (%u KiB)", rom[2] / 2u);
            unsigned pcir = rom_len >= 0x1a ? le16(rom + 0x18) : 0;
            if (rom_len < 0x1a)
                add(&t, " PCIR offset beyond the readable %zx bytes", rom_len);
            else if (pcir > rom_len || rom_len - pcir < 0x18)
                add(&t, " PCIR@%04x beyond the readable %zx bytes", pcir, rom_len);
            else if (rom[pcir] != 'P' || rom[pcir + 1] != 'C' || rom[pcir + 2] != 'I' ||
                     rom[pcir + 3] != 'R')
                add(&t, " PCIR@%04x without the PCIR signature (%02x %02x %02x %02x)", pcir,
                    rom[pcir], rom[pcir + 1], rom[pcir + 2], rom[pcir + 3]);
            else {
                const uint8_t *d = rom + pcir;
                add(&t, " PCIR@%04x %04x:%04x class=%02x%02x%02x type=%02x%s len=%04x last=%u",
                    pcir, le16(d + 4), le16(d + 6), d[0xf], d[0xe], d[0xd], d[0x14],
                    d[0x14] == 0   ? "(x86)"
                    : d[0x14] == 3 ? "(EFI)"
                                   : "",
                    le16(d + 0x10), d[0x15] >> 7);
            }
        }
    }
    if (!ivt || ivt_len < 4)
        add(&t, "; IVT 0040 not in the host memory map, not read");
    else
        add(&t, "; IVT int10=%04x:%04x (raw)", le16(ivt + 2), le16(ivt));
    return t.used;
}

/* VMX: the host's BDA,
 * the IVT entries a VBIOS and its callers use, and the image's entry, from
 * the copies boot/limine.c takes at VMM start. Line 0: BDA fields (IBM PC
 * AT Technical Reference: 040Eh EBDA segment, 0413h base memory KiB,
 * 0449h video mode, 044Ah columns, 0463h CRTC base, 0484h rows - 1, 0485h
 * character height, 0487h/0488h EGA flags, 04A8h video save pointer).
 * Line 1: IVT 10h, 15h, 1Ah, 1Fh, 42h, 43h, 6Dh, each marked "image" when
 * it points into the C0000 image (byte 2 x 512). Line 2: the first 32
 * bytes at C0000 and the target of the jump at +3. */
size_t platform_legacy_snapshot_describe(char *out, size_t size, unsigned line, const uint8_t *low,
                                         size_t low_len, const uint8_t *option, size_t option_len)
{
    struct text t = {out, size, 0};
    if (size)
        out[0] = 0;
    bool image = option && option_len >= 3 && option[0] == 0x55 && option[1] == 0xaa;
    unsigned image_size = image ? option[2] * 512u : 0;
    if (line == 0) {
        if (!low || low_len < 0x500) {
            add(&t, "host BDA: not copied");
            return t.used;
        }
        const uint8_t *b = low + 0x400;
        add(&t,
            "host BDA: 40E=%04x%s 413=%u KiB 449=%02x 44A=%u 463=%04x 484=%02x 485=%04x 487=%02x 488=%02x "
            "4A8=%04x:%04x",
            le16(b + 0x0e), le16(b + 0x0e) ? " (EBDA)" : " (no EBDA)", le16(b + 0x13), b[0x49],
            le16(b + 0x4a), le16(b + 0x63), b[0x84], le16(b + 0x85), b[0x87], b[0x88],
            le16(b + 0xaa), le16(b + 0xa8));
    } else if (line == 1) {
        if (!low || low_len < 0x400) {
            add(&t, "host IVT: not copied");
            return t.used;
        }
        static const uint8_t vectors[] = {0x10, 0x15, 0x1a, 0x1f, 0x42, 0x43, 0x6d};
        add(&t, "host IVT:");
        for (unsigned i = 0; i < sizeof(vectors); ++i) {
            const uint8_t *e = low + vectors[i] * 4;
            unsigned target = le16(e + 2) * 16u + le16(e);
            bool inside = target >= 0xc0000 && target - 0xc0000 < image_size;
            add(&t, " %02X=%04x:%04x(%s)", vectors[i], le16(e + 2), le16(e),
                inside ? "image" : "redirected");
        }
    } else {
        if (!option || option_len < 32) {
            add(&t, "host C0000 bytes: not copied");
            return t.used;
        }
        add(&t, "host C0000 bytes:");
        for (unsigned i = 0; i < 32; ++i)
            add(&t, " %02x", option[i]);
        /* +3: E9 rel16 (near jump, next IP 6) or EB rel8 (short, next IP 5). */
        if (option[3] == 0xe9)
            add(&t, "; +3 jmp c000:%04x", (6u + le16(option + 4)) & 0xffffu);
        else if (option[3] == 0xeb)
            add(&t, "; +3 jmp c000:%04x", (5u + (unsigned)(int8_t)option[4]) & 0xffffu);
        else
            add(&t, "; +3 is not a jump (%02x)", option[3]);
    }
    return t.used;
}
