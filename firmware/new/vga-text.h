/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VGA text/font sequences extracted from firmware/firmware.c, whose origin
 * is the IA-64 QEMU reference firmware. Both firmwares program the
 * assigned controller through its real sparse I/O and memory apertures.
 * It also holds the Sandy Bridge (gen6) IGD leave/restore sequence below. */
#ifndef VMM_VGA_TEXT_H
#define VMM_VGA_TEXT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ia64vmm/platform.h"
#define LEGACY_VGA_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
/* Every device access and the time source go through these accessors. The
 * register-model test (tests/firmware/vga_igd.c) defines LEGACY_VGA_HOOKS and
 * supplies its own definitions to record the exact access order. */
#ifndef LEGACY_VGA_HOOKS
static inline uint8_t legacy_vga_rd8(uint64_t a)
{
    return *(volatile uint8_t *)(uintptr_t)a;
}
static inline uint16_t legacy_vga_rd16(uint64_t a)
{
    return *(volatile uint16_t *)(uintptr_t)a;
}
static inline uint32_t legacy_vga_rd32(uint64_t a)
{
    return *(volatile uint32_t *)(uintptr_t)a;
}
static inline void legacy_vga_wr8(uint64_t a, uint8_t v)
{
    *(volatile uint8_t *)(uintptr_t)a = v;
}
static inline void legacy_vga_wr16(uint64_t a, uint16_t v)
{
    *(volatile uint16_t *)(uintptr_t)a = v;
}
static inline void legacy_vga_wr32(uint64_t a, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)a = v;
}
static inline void legacy_vga_wr64(uint64_t a, uint64_t v)
{
    *(volatile uint64_t *)(uintptr_t)a = v;
}
#if defined(__ia64__)
static inline uint64_t legacy_vga_ticks(void)
{
    uint64_t value;
    __asm__ volatile("mov %0=ar.itc" : "=r"(value));
    return value;
}
#else
#error "vga-text.h: non-IA-64 builds must define LEGACY_VGA_HOOKS and the accessors"
#endif
#endif
static inline uint64_t legacy_vga_port_address(uint64_t base, unsigned port)
{
    return base + (((uint64_t)port >> 2) << 12 | (port & 0xfff));
}
static inline uint8_t legacy_vga_io_read(uint64_t base, unsigned port)
{
    return legacy_vga_rd8(legacy_vga_port_address(base, port));
}
static inline void legacy_vga_io_write(uint64_t base, unsigned port, uint8_t value)
{
    legacy_vga_wr8(legacy_vga_port_address(base, port), value);
}
static inline void legacy_vga_indexed_write(uint64_t base, unsigned index_port, unsigned data_port,
                                            uint8_t index, uint8_t value)
{
    legacy_vga_io_write(base, index_port, index);
    legacy_vga_io_write(base, data_port, value);
}
static inline uint8_t legacy_vga_indexed_read(uint64_t base, unsigned index_port,
                                              unsigned data_port, uint8_t index)
{
    legacy_vga_io_write(base, index_port, index);
    return legacy_vga_io_read(base, data_port);
}
static inline void legacy_vga_enable_attribute_output(uint64_t base)
{
    legacy_vga_io_write(base, 0x3c2, 0x67);
    (void)legacy_vga_io_read(base, 0x3da);
    legacy_vga_io_write(base, 0x3c0, 0x20);
}
static inline bool legacy_vga_can_leave_gop(const struct ia64vmm_handoff *h)
{
    return h->vga_kind == IA64VMM_VGA_BOCHS || h->vga_kind == IA64VMM_VGA_VMWARE ||
           h->vga_kind == IA64VMM_VGA_INTEL_GEN6;
}
static inline void legacy_vga_port_write(const struct ia64vmm_handoff *h, unsigned port,
                                         unsigned width, uint64_t value)
{
    /* SVGA's unaligned BAR0+1 dword cannot be represented by an IA-64 MMIO
     * instruction. Preserve the real transaction width through the service. */
    legacy_vga_wr64(h->service_base + IA64VMM_SVC_PORT_SELECT, port | (width << 16));
    legacy_vga_wr64(h->service_base + IA64VMM_SVC_PORT_DATA, value);
}

/* --- Transition context and the "vga: " trace ----------------------------
 * Each firmware passes a line sink (its diagnostic console, which the VMM
 * keeps as [guest] lines in the debug ring and lists in its final summary). */
struct legacy_vga_igd_pipe {
    uint32_t conf, src, htotal, vtotal, dspcntr, dspsurf, pf_ctl, pf_pos, pf_sz;
    uint32_t curcntr, curbase, dvscntr, dvssurf, frame;
};
struct legacy_vga_igd {
    struct legacy_vga_igd_pipe pipe[2];
    uint32_t vgacntrl, lvds, deiir, deisr, palette[2][256];
    uint16_t command, ggc;
    uint8_t msr, sr01, index; /* index: the panel pipe (0 = A, 1 = B) */
    uint8_t gr10, gr11;       /* VGA paging as the GOP/VBIOS left it */
    uint8_t diag_paged, diag_planar, diag_text, diag_font; /* probe: 0 not run, 1 ok, 2 bad */
    uint16_t applied;                                      /* IGD_DID_* steps to undo on restore */
};
struct legacy_vga_device {
    const struct ia64vmm_handoff *h;
    void (*trace)(const char *line); /* one line without newline; may be NULL */
    struct legacy_vga_igd igd;
};
struct legacy_vga_line {
    char text[176];
    unsigned used;
};
static inline void legacy_vga_line_str(struct legacy_vga_line *l, const char *s)
{
    while (*s && l->used < sizeof(l->text) - 1)
        l->text[l->used++] = *s++;
    l->text[l->used] = 0;
}
static inline void legacy_vga_line_start(struct legacy_vga_line *l, const char *s)
{
    l->used = 0;
    legacy_vga_line_str(l, "vga: ");
    legacy_vga_line_str(l, s);
}
static inline void legacy_vga_line_hex(struct legacy_vga_line *l, uint64_t value, unsigned digits)
{
    char text[17];
    for (unsigned i = 0; i < digits && i < 16; ++i)
        text[i] = "0123456789abcdef"[(value >> ((digits - 1 - i) * 4)) & 15];
    text[digits < 16 ? digits : 16] = 0;
    legacy_vga_line_str(l, text);
}
static inline void legacy_vga_line_dec(struct legacy_vga_line *l, uint64_t value)
{
    char text[21];
    unsigned at = sizeof(text) - 1;
    text[at] = 0;
    do {
        text[--at] = (char)('0' + value % 10);
        value /= 10;
    } while (value && at);
    legacy_vga_line_str(l, text + at);
}
static inline void legacy_vga_line_emit(const struct legacy_vga_device *v,
                                        struct legacy_vga_line *l)
{
    if (v->trace)
        v->trace(l->text);
}
static inline void legacy_vga_note(const struct legacy_vga_device *v, const char *text)
{
    struct legacy_vga_line l;
    legacy_vga_line_start(&l, text);
    legacy_vga_line_emit(v, &l);
}
static inline void legacy_vga_delay_us(const struct legacy_vga_device *v, uint64_t us)
{
    uint64_t hz = v->h->itc_hz ? v->h->itc_hz : IA64VMM_ITC_HZ;
    uint64_t start = legacy_vga_ticks(), wait = hz / 1000000u * us;
    while (legacy_vga_ticks() - start < wait) {
    }
}
/* Traced indexed VGA register write: " NAME old->new=readback". */
static inline void legacy_vga_set_indexed(const struct legacy_vga_device *v,
                                          struct legacy_vga_line *l, const char *name,
                                          unsigned index_port, unsigned data_port, uint8_t index,
                                          uint8_t value)
{
    uint64_t io = v->h->legacy_io_base;
    uint8_t old = legacy_vga_indexed_read(io, index_port, data_port, index);
    legacy_vga_indexed_write(io, index_port, data_port, index, value);
    uint8_t now = legacy_vga_indexed_read(io, index_port, data_port, index);
    legacy_vga_line_str(l, " ");
    legacy_vga_line_str(l, name);
    legacy_vga_line_str(l, " ");
    legacy_vga_line_hex(l, old, 2);
    legacy_vga_line_str(l, "->");
    legacy_vga_line_hex(l, value, 2);
    legacy_vga_line_str(l, "=");
    legacy_vga_line_hex(l, now, 2);
}
/* VGA MSR: write 3C2, read 3CC (SNB PRM Vol 3 Part 1 §1.1.4). */
static inline void legacy_vga_set_msr(const struct legacy_vga_device *v, struct legacy_vga_line *l,
                                      uint8_t value)
{
    uint64_t io = v->h->legacy_io_base;
    uint8_t old = legacy_vga_io_read(io, 0x3cc);
    legacy_vga_io_write(io, 0x3c2, value);
    uint8_t now = legacy_vga_io_read(io, 0x3cc);
    legacy_vga_line_str(l, " MSR ");
    legacy_vga_line_hex(l, old, 2);
    legacy_vga_line_str(l, "->");
    legacy_vga_line_hex(l, value, 2);
    legacy_vga_line_str(l, "=");
    legacy_vga_line_hex(l, now, 2);
}

/* --- Sandy Bridge (gen6) IGD: legacy VGA plane on the panel pipe ---------
 * Sources (cited per step below):
 *  [PRM] Intel OpenSource HD Graphics PRM, 2011 Intel Core processor family
 *        (Sandy Bridge), May 2011, Vol 3 Part 1 (VGA registers), Vol 3 Part 2
 *        (CPU display registers), Vol 3 Part 3 (PCH display registers).
 *  [i915] Linux drivers/gpu/drm/i915: display/intel_vga.c (intel_vga_disable,
 *        intel_vga_get, intel_vga_decode_is_enabled), display/intel_pfit.c
 *        (ilk_pfit_enable/disable), display/intel_display.c
 *        (intel_pipe_fastset), intel_uncore.c (NEEDS_FORCE_WAKE: display
 *        registers 0x40000..0x115fff need no GT forcewake).
 *  [gfxinit] coreboot libgfxinit common/hw-gfx-gma-pipe_setup.adb
 *        (Setup_Display VGA branch, Setup_Ironlake_Panel_Fitter,
 *        Legacy_VGA_Off) and coreboot drivers/intel/gma/text_fb: the VGA
 *        plane as a 640x400 pipe source scaled by the panel fitter.
 * The pipe, FDI, PCH transcoder and LVDS port keep the GOP's native panel
 * timing; only planes, panel fitter, pipe source and VGA state change. */
#define IGD_VGACNTRL 0x41000u                       /* PRM Vol3 Pt2 §2.1 */
#define IGD_DEISR 0x44000u                          /* §2.5.2 */
#define IGD_DEIIR 0x44008u                          /* §2.5.4 */
#define IGD_PALETTE(p) (0x4a000u + (p) * 0x800u)    /* §2.12.1/2 */
#define IGD_HTOTAL(p) (0x60000u + (p) * 0x1000u)    /* §3.1.1 */
#define IGD_VTOTAL(p) (0x6000cu + (p) * 0x1000u)    /* §3.1.4 */
#define IGD_PIPESRC(p) (0x6001cu + (p) * 0x1000u)   /* §3.1.7 */
#define IGD_PF_WIN_POS(p) (0x68070u + (p) * 0x800u) /* §3.5.1 */
#define IGD_PF_WIN_SZ(p) (0x68074u + (p) * 0x800u)  /* §3.5.2 */
#define IGD_PF_CTL(p) (0x68080u + (p) * 0x800u)     /* §3.5.3 */
#define IGD_PIPECONF(p) (0x70008u + (p) * 0x1000u)  /* §4.1.3 */
#define IGD_FRMCOUNT(p) (0x70040u + (p) * 0x1000u)  /* §4.2.1 */
#define IGD_CURCNTR(p) (0x70080u + (p) * 0x40u)     /* §4.6.1/4.7.1 */
#define IGD_CURBASE(p) (0x70084u + (p) * 0x40u)     /* §4.6.2/4.7.2 */
#define IGD_DSPCNTR(p) (0x70180u + (p) * 0x1000u)   /* §4.8.1 */
#define IGD_DSPSURF(p) (0x7019cu + (p) * 0x1000u)   /* §4.8.4 */
#define IGD_DVSCNTR(p) (0x72180u + (p) * 0x1000u)   /* §4.10.1 */
#define IGD_DVSSURF(p) (0x7219cu + (p) * 0x1000u)   /* §4.10.8 */
#define IGD_PCH_LVDS 0xe1180u                       /* Vol3 Pt3 §3.7.1; i915 intel_lvds_regs.h */
#define IGD_ENABLE 0x80000000u         /* bit 31 of PIPECONF/DSPCNTR/DVSCNTR/PF_CTL/LVDS */
#define IGD_PIPE_STATE 0x40000000u     /* PIPECONF bit 30 */
#define IGD_PIPE_PALETTE 0x03000000u   /* PIPECONF 25:24, 00 = 8-bit legacy */
#define IGD_PIPE_INTERLACE 0x00e00000u /* PIPECONF 23:21 */
#define IGD_CURSOR_MODE 0x00000027u    /* CURxCNTR mode select; 0 = off */
#define IGD_PF_FILTER_MED 0x00800000u  /* PF_CTL 24:23 = 01 */
#define IGD_VGA_DISABLE 0x80000000u
#define IGD_VGA_SOURCE 0x027f018fu /* 640x400, (w-1)<<16 | (h-1) */
#define IGD_DID_SR01 0x0001u
#define IGD_DID_CURSOR 0x0002u
#define IGD_DID_SPRITE 0x0004u
#define IGD_DID_PRIMARY 0x0008u
#define IGD_DID_CONF 0x0010u
#define IGD_DID_PF 0x0020u
#define IGD_DID_SRC 0x0040u
#define IGD_DID_MSR 0x0080u
#define IGD_DID_DAC 0x0100u
#define IGD_DID_VGA 0x0200u
#define IGD_DID_COMMAND 0x0400u
#define IGD_DID_PAGING 0x0800u
static inline uint32_t igd_rd(const struct legacy_vga_device *v, uint32_t offset)
{
    return legacy_vga_rd32(v->h->vga_control_base + offset);
}
static inline void igd_wr(const struct legacy_vga_device *v, uint32_t offset, uint32_t value)
{
    legacy_vga_wr32(v->h->vga_control_base + offset, value);
}
/* Traced MMIO write: " NAME old->new=readback". Double-buffered registers
 * read back the armed value (PRM Vol3 Pt2 §1.1.1). */
static inline void igd_set(const struct legacy_vga_device *v, struct legacy_vga_line *l,
                           const char *name, uint32_t offset, uint32_t value)
{
    uint32_t old = igd_rd(v, offset);
    igd_wr(v, offset, value);
    uint32_t now = igd_rd(v, offset);
    legacy_vga_line_str(l, " ");
    legacy_vga_line_str(l, name);
    legacy_vga_line_str(l, " ");
    legacy_vga_line_hex(l, old, 8);
    legacy_vga_line_str(l, "->");
    legacy_vga_line_hex(l, value, 8);
    legacy_vga_line_str(l, "=");
    legacy_vga_line_hex(l, now, 8);
}
static inline void igd_field(struct legacy_vga_line *l, const char *name, uint32_t value)
{
    legacy_vga_line_str(l, " ");
    legacy_vga_line_str(l, name);
    legacy_vga_line_str(l, "=");
    legacy_vga_line_hex(l, value, 8);
}
/* Double-buffered display registers latch at the start of vertical blank;
 * the frame counter increments there (PRM Vol3 Pt2 §4.2.1). 100 ms bound.
 * Appends " vbl old->new" or " vbl TIMEOUT old" to the step's line. */
static inline bool igd_vblank(const struct legacy_vga_device *v, struct legacy_vga_line *l)
{
    unsigned p = v->igd.index;
    uint64_t hz = v->h->itc_hz ? v->h->itc_hz : IA64VMM_ITC_HZ;
    uint32_t start = igd_rd(v, IGD_FRMCOUNT(p)), now = start;
    uint64_t begin = legacy_vga_ticks();
    bool ok = false;
    while (legacy_vga_ticks() - begin < hz / 10) {
        now = igd_rd(v, IGD_FRMCOUNT(p));
        if (now != start) {
            ok = true;
            break;
        }
    }
    legacy_vga_line_str(l, ok ? " vbl " : " vbl TIMEOUT ");
    legacy_vga_line_hex(l, start, 8);
    if (ok) {
        legacy_vga_line_str(l, "->");
        legacy_vga_line_hex(l, now, 8);
    }
    return ok;
}
static inline void igd_snapshot_pipe(struct legacy_vga_device *v, unsigned p)
{
    struct legacy_vga_igd_pipe *s = &v->igd.pipe[p];
    s->conf = igd_rd(v, IGD_PIPECONF(p));
    s->src = igd_rd(v, IGD_PIPESRC(p));
    s->htotal = igd_rd(v, IGD_HTOTAL(p));
    s->vtotal = igd_rd(v, IGD_VTOTAL(p));
    s->dspcntr = igd_rd(v, IGD_DSPCNTR(p));
    s->dspsurf = igd_rd(v, IGD_DSPSURF(p));
    s->pf_ctl = igd_rd(v, IGD_PF_CTL(p));
    s->pf_pos = igd_rd(v, IGD_PF_WIN_POS(p));
    s->pf_sz = igd_rd(v, IGD_PF_WIN_SZ(p));
    s->curcntr = igd_rd(v, IGD_CURCNTR(p));
    s->curbase = igd_rd(v, IGD_CURBASE(p));
    s->dvscntr = igd_rd(v, IGD_DVSCNTR(p));
    s->dvssurf = igd_rd(v, IGD_DVSSURF(p));
    s->frame = igd_rd(v, IGD_FRMCOUNT(p));
    struct legacy_vga_line l;
    legacy_vga_line_start(&l, p ? "igd B" : "igd A");
    igd_field(&l, "CONF", s->conf);
    igd_field(&l, "SRC", s->src);
    igd_field(&l, "HT", s->htotal);
    igd_field(&l, "VT", s->vtotal);
    igd_field(&l, "FRM", s->frame);
    if (!(s->conf & IGD_ENABLE) && !(s->dspcntr & IGD_ENABLE) && !(s->pf_ctl & IGD_ENABLE)) {
        legacy_vga_line_str(&l, " off");
        legacy_vga_line_emit(v, &l);
        return;
    }
    legacy_vga_line_emit(v, &l);
    legacy_vga_line_start(&l, p ? "igd B" : "igd A");
    igd_field(&l, "DSP", s->dspcntr);
    igd_field(&l, "SURF", s->dspsurf);
    igd_field(&l, "PF", s->pf_ctl);
    igd_field(&l, "POS", s->pf_pos);
    igd_field(&l, "SZ", s->pf_sz);
    igd_field(&l, "CUR", s->curcntr);
    igd_field(&l, "DVS", s->dvscntr);
    legacy_vga_line_emit(v, &l);
}
static inline bool igd_gen6_restore(struct legacy_vga_device *v);
/* Steps before the shared font/text programming. False: GOP unchanged. */
static inline bool igd_gen6_leave(struct legacy_vga_device *v)
{
    const struct ia64vmm_handoff *h = v->h;
    struct legacy_vga_igd *s = &v->igd;
    uint64_t config = h->ecam_base + ((uint64_t)h->vga_bdf << 12);
    struct legacy_vga_line l;
    s->applied = 0;
    s->index = 0;
    s->gr10 = s->gr11 = 0;
    s->diag_paged = s->diag_planar = s->diag_text = s->diag_font = 0;
    s->ggc = legacy_vga_rd16(config + 0x50);
    s->command = legacy_vga_rd16(config + 4);
    legacy_vga_line_start(&l, "igd ");
    legacy_vga_line_hex(&l, h->vga_vendor, 4);
    legacy_vga_line_str(&l, ":");
    legacy_vga_line_hex(&l, h->vga_device, 4);
    legacy_vga_line_str(&l, " bar0=");
    legacy_vga_line_hex(&l, h->vga_control_base, 12);
    legacy_vga_line_str(&l, "/");
    legacy_vga_line_hex(&l, h->vga_control_size, 8);
    legacy_vga_line_str(&l, " GGC=");
    legacy_vga_line_hex(&l, s->ggc, 4);
    if (h->vga_control_size < 0x100000u) {
        legacy_vga_line_emit(v, &l);
        legacy_vga_note(v, "igd FAIL: GTTMMADR (BAR0) smaller than the display range");
        return false;
    }
    /* GGC.IVD=1: the host bridge does not decode legacy VGA for the IGD and
     * the register is locked ([i915] intel_vga_decode_is_enabled). */
    if (s->ggc & 2) {
        legacy_vga_line_emit(v, &l);
        legacy_vga_note(v, "igd FAIL: GGC.IVD=1, legacy VGA decode disabled by platform firmware");
        return false;
    }
    legacy_vga_line_str(&l, " CMD=");
    legacy_vga_line_hex(&l, s->command, 4);
    legacy_vga_line_emit(v, &l);
    /* Display MMIO reads only until every precondition holds: a refusal
     * leaves the device exactly as the GOP left it. */
    s->vgacntrl = igd_rd(v, IGD_VGACNTRL);
    s->lvds = igd_rd(v, IGD_PCH_LVDS);
    s->deiir = igd_rd(v, IGD_DEIIR);
    s->deisr = igd_rd(v, IGD_DEISR);
    legacy_vga_line_start(&l, "igd");
    igd_field(&l, "VGACNTRL", s->vgacntrl);
    igd_field(&l, "LVDS", s->lvds);
    igd_field(&l, "DEIIR", s->deiir);
    igd_field(&l, "DEISR", s->deisr);
    legacy_vga_line_emit(v, &l);
    igd_snapshot_pipe(v, 0);
    igd_snapshot_pipe(v, 1);
    /* The panel pipe: LVDS port enable and its CPT transcoder select (PRM
     * Vol3 Pt3 §3.7.1, bits 31 and 30:29); else the only running pipe with
     * an enabled primary plane. Transcoder N is fed by CPU pipe N via FDI. */
    const char *source = "LVDS";
    unsigned p = 2;
    if (s->lvds & IGD_ENABLE)
        p = (s->lvds >> 29) & 3;
    else {
        source = "active";
        for (unsigned i = 0; i < 2; ++i) {
            const struct legacy_vga_igd_pipe *q = &s->pipe[i];
            if ((q->conf & IGD_ENABLE) && (q->dspcntr & IGD_ENABLE))
                p = p == 2 ? i : 3;
        }
    }
    const struct legacy_vga_igd_pipe *q = &s->pipe[p < 2 ? p : 0];
    uint32_t width = (q->htotal & 0xfff) + 1, height = (q->vtotal & 0xfff) + 1;
    legacy_vga_line_start(&l, "igd pipe ");
    legacy_vga_line_str(&l, p == 0 ? "A" : p == 1 ? "B" : "none");
    legacy_vga_line_str(&l, " (");
    legacy_vga_line_str(&l, source);
    legacy_vga_line_str(&l, ") panel ");
    legacy_vga_line_dec(&l, width);
    legacy_vga_line_str(&l, "x");
    legacy_vga_line_dec(&l, height);
    legacy_vga_line_emit(v, &l);
    const char *problem =
        p > 1 ? "no single panel pipe"
        : (q->conf & (IGD_ENABLE | IGD_PIPE_STATE)) != (IGD_ENABLE | IGD_PIPE_STATE)
            ? "panel pipe not running (no mode programming here)"
        : (q->conf & IGD_PIPE_INTERLACE) ? "interlaced pipe (VGA needs progressive fetch)"
        : width < 640 || height < 400    ? "panel smaller than 640x400"
                                         : NULL;
    if (problem) {
        legacy_vga_line_start(&l, "igd FAIL: ");
        legacy_vga_line_str(&l, problem);
        legacy_vga_line_emit(v, &l);
        return false;
    }
    /* [i915] intel_vga_get: the IGD wins VGA I/O once its I/O decode is on. */
    legacy_vga_line_start(&l, "igd");
    legacy_vga_wr16(config + 4, (uint16_t)(s->command | 3));
    legacy_vga_line_str(&l, " CMD ");
    legacy_vga_line_hex(&l, s->command, 4);
    legacy_vga_line_str(&l, "->");
    legacy_vga_line_hex(&l, s->command | 3, 4);
    legacy_vga_line_str(&l, "=");
    legacy_vga_line_hex(&l, legacy_vga_rd16(config + 4), 4);
    s->applied |= IGD_DID_COMMAND;
    s->msr = legacy_vga_io_read(h->legacy_io_base, 0x3cc);
    s->sr01 = legacy_vga_indexed_read(h->legacy_io_base, 0x3c4, 0x3c5, 1);
    legacy_vga_line_str(&l, " MSR=");
    legacy_vga_line_hex(&l, s->msr, 2);
    legacy_vga_line_str(&l, " SR01=");
    legacy_vga_line_hex(&l, s->sr01, 2);
    legacy_vga_line_emit(v, &l);
    s->index = (uint8_t)p;
    /* With VGACNTRL bits 22/21 at their GOP value the VGA DAC writes reach
     * both pipes' legacy palettes (PRM §2.1); keep both for a restore. */
    for (unsigned i = 0; i < 2 * 256; ++i)
        s->palette[i >> 8][i & 255] = igd_rd(v, IGD_PALETTE(i >> 8) + (i & 255) * 4);
    /* SR01 screen off around every VGA plane enable/disable (PRM Vol3 Pt2
     * §2.1 VGACNTRL bit 31 note; [i915] intel_vga_disable). */
    legacy_vga_line_start(&l, "igd");
    legacy_vga_set_indexed(v, &l, "SR01", 0x3c4, 0x3c5, 1, (uint8_t)(s->sr01 | 0x20));
    s->applied |= IGD_DID_SR01;
    /* Only the VGA plane may be enabled with VGA (PRM §2.1). Each control
     * write is armed by its surface/base write and latches at vblank (§4.6,
     * §4.8, §4.10; [gfxinit] Planes_Off). The pipe keeps running. */
    if (q->curcntr & IGD_CURSOR_MODE) {
        igd_set(v, &l, "CURCNTR", IGD_CURCNTR(p), q->curcntr & ~IGD_CURSOR_MODE);
        igd_set(v, &l, "CURBASE", IGD_CURBASE(p), q->curbase);
        s->applied |= IGD_DID_CURSOR;
    }
    if (q->dvscntr & IGD_ENABLE) {
        igd_set(v, &l, "DVSCNTR", IGD_DVSCNTR(p), q->dvscntr & ~IGD_ENABLE);
        igd_set(v, &l, "DVSSURF", IGD_DVSSURF(p), q->dvssurf);
        s->applied |= IGD_DID_SPRITE;
    }
    if (s->applied & (IGD_DID_CURSOR | IGD_DID_SPRITE)) {
        legacy_vga_line_emit(v, &l);
        legacy_vga_line_start(&l, "igd");
    }
    if (q->dspcntr & IGD_ENABLE) {
        igd_set(v, &l, "DSPCNTR", IGD_DSPCNTR(p), q->dspcntr & ~IGD_ENABLE);
        igd_set(v, &l, "DSPSURF", IGD_DSPSURF(p), q->dspsurf);
        s->applied |= IGD_DID_PRIMARY;
    }
    /* Indexed VGA pixels need the 8-bit legacy palette mode (PRM §2.12,
     * §4.1.3 bits 25:24); this field is double-buffered at vblank too. */
    if (q->conf & IGD_PIPE_PALETTE) {
        igd_set(v, &l, "PIPECONF", IGD_PIPECONF(p), q->conf & ~IGD_PIPE_PALETTE);
        s->applied |= IGD_DID_CONF;
    }
    bool ok = igd_vblank(v, &l);
    legacy_vga_line_emit(v, &l);
    if (!ok) {
        igd_gen6_restore(v);
        return false;
    }
    /* "VGA requires panel fitting to be enabled" (PRM §2.1). SNB has one
     * fitter per pipe without a pipe select (bits 30:29 are IVB+); writing
     * WIN_SZ arms CTL/POS (§3.5). [i915] ilk_pfit_enable writes exactly
     * PF_ENABLE|PF_FILTER_MED_3x3, POS, SZ on SNB; intel_pipe_fastset does
     * so on a running pipe. Full panel: [gfxinit] notes pillarbox quirks. */
    legacy_vga_line_start(&l, "igd");
    igd_set(v, &l, "PF_CTL", IGD_PF_CTL(p), IGD_ENABLE | IGD_PF_FILTER_MED);
    igd_set(v, &l, "PF_POS", IGD_PF_WIN_POS(p), 0);
    igd_set(v, &l, "PF_SZ", IGD_PF_WIN_SZ(p), width << 16 | height);
    s->applied |= IGD_DID_PF;
    ok = igd_vblank(v, &l);
    legacy_vga_line_emit(v, &l);
    if (!ok) {
        igd_gen6_restore(v);
        return false;
    }
    /* PIPESRC is the only timing register writable while the pipe runs and
     * may change on the fly with panel fitting enabled (PRM §3.1.7, §1.1.2
     * "Pipe timings change"). VGA is a 640x400 source ([gfxinit]
     * Setup_Display; coreboot text_fb). */
    legacy_vga_line_start(&l, "igd");
    igd_set(v, &l, "PIPESRC", IGD_PIPESRC(p), IGD_VGA_SOURCE);
    s->applied |= IGD_DID_SRC;
    ok = igd_vblank(v, &l);
    legacy_vga_line_emit(v, &l);
    if (!ok) {
        igd_gen6_restore(v);
        return false;
    }
    /* MSR reset value 00: A0000-BFFFF CPU access blocked and CRTC at 3Bx
     * (PRM Vol3 Pt1 §1.1.4; [i915] intel_vga_disable clears both bits).
     * Enable both before any CRTC, font or text access. */
    legacy_vga_line_start(&l, "igd");
    legacy_vga_set_msr(v, &l, 0x67);
    s->applied |= IGD_DID_MSR | IGD_DID_DAC;
    /* GR10 bit 0 (Page Mapping Enable) with GR11 maps A0000-AFFFF straight
     * onto stolen memory with "no VGA memory address swizzle" whenever GR06
     * selects A0000-AFFFF; VGA paging "should not be enabled when using
     * basic VGA modes" (PRM Vol3 Pt1 §1.3.11, §1.3.12). A VBIOS-built GOP
     * can leave it on, exposing raw interleaved planes through planar reads. I/O access only, as the PRM requires for GR10/GR11. */
    s->gr10 = legacy_vga_indexed_read(h->legacy_io_base, 0x3ce, 0x3cf, 0x10);
    s->gr11 = legacy_vga_indexed_read(h->legacy_io_base, 0x3ce, 0x3cf, 0x11);
    legacy_vga_set_indexed(v, &l, "GR10", 0x3ce, 0x3cf, 0x10, 0x00);
    legacy_vga_set_indexed(v, &l, "GR11", 0x3ce, 0x3cf, 0x11, 0x00);
    s->applied |= IGD_DID_PAGING;
    legacy_vga_line_emit(v, &l);
    return true;
}
/* Font/text memory and register readback after the shared programming. */
static inline bool igd_gen6_check(struct legacy_vga_device *v, const uint8_t glyphs[256 * 16])
{
    uint64_t io = v->h->legacy_io_base, memory = v->h->vga_base;
    unsigned font_bad = 0, text_bad = 0;
    /* Read plane 2 back in the configuration the font was written with,
     * which is also Linux vgacon_do_font_op's read-back: SR04=07, GR04=02,
     * GR05=00, GR06=00 (A0000-BFFFF). Then the text-mode values again. */
    legacy_vga_indexed_write(io, 0x3c4, 0x3c5, 0x04, 0x07);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x04, 0x02);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x05, 0x00);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x06, 0x00);
    for (unsigned at = 0; at < 256u * 32u; ++at) {
        uint8_t want = (at & 31) < 16 ? glyphs[(at >> 5) * 16 + (at & 31)] : 0;
        if (legacy_vga_rd8(memory + at) != want)
            ++font_bad;
    }
    legacy_vga_indexed_write(io, 0x3c4, 0x3c5, 0x04, 0x02);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x04, 0x00);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x05, 0x10);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x06, 0x0e);
    static const uint16_t cells[] = {0x1f56, 0x1f47, 0x1f41, 0x1f21}; /* "VGA!" */
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t at = memory + 0x18000u + (80u * 24u + 76u + i) * 2u;
        legacy_vga_wr16(at, cells[i]);
        if (legacy_vga_rd16(at) != cells[i])
            ++text_bad;
    }
    struct legacy_vga_line l;
    legacy_vga_line_start(&l, "igd check font_bad=");
    legacy_vga_line_dec(&l, font_bad);
    legacy_vga_line_str(&l, "/8192 text_bad=");
    legacy_vga_line_dec(&l, text_bad);
    legacy_vga_line_str(&l, "/4");
    static const struct {
        const char *name;
        uint16_t port;
        uint8_t index;
    } regs[] = {
        {"SR01", 0x3c4, 1}, {"SR02", 0x3c4, 2},    {"SR04", 0x3c4, 4},
        {"GR05", 0x3ce, 5}, {"GR06", 0x3ce, 6},    {"GR10", 0x3ce, 0x10},
        {"CR09", 0x3d4, 9}, {"CR11", 0x3d4, 0x11}, {"CR17", 0x3d4, 0x17},
    };
    for (unsigned i = 0; i < LEGACY_VGA_ARRAY_SIZE(regs); ++i) {
        legacy_vga_line_str(&l, " ");
        legacy_vga_line_str(&l, regs[i].name);
        legacy_vga_line_str(&l, "=");
        legacy_vga_line_hex(
            &l, legacy_vga_indexed_read(io, regs[i].port, regs[i].port + 1u, regs[i].index), 2);
    }
    legacy_vga_line_str(&l, " MSR=");
    legacy_vga_line_hex(&l, legacy_vga_io_read(io, 0x3cc), 2);
    legacy_vga_line_emit(v, &l);
    return !font_bad && !text_bad;
}
static inline bool igd_gen6_finish(struct legacy_vga_device *v, const uint8_t glyphs[256 * 16])
{
    struct legacy_vga_igd *s = &v->igd;
    unsigned p = s->index;
    struct legacy_vga_line l;
    if (!igd_gen6_check(v, glyphs)) {
        legacy_vga_note(v, "igd FAIL: VGA memory readback (A0000-BFFFF decode)");
        igd_gen6_restore(v);
        return false;
    }
    /* Screen off, then the VGA plane: [i915] intel_vga_disable waits 300 us
     * between SR01 and VGACNTRL ([gfxinit] Legacy_VGA_Off: PRM 100 us). */
    legacy_vga_line_start(&l, "igd");
    legacy_vga_set_indexed(
        v, &l, "SR01", 0x3c4, 0x3c5, 1,
        (uint8_t)(legacy_vga_indexed_read(v->h->legacy_io_base, 0x3c4, 0x3c5, 1) | 0x20));
    legacy_vga_delay_us(v, 300);
    /* VGACNTRL (PRM §2.1): enable (bit 31 = 0), pipe select 29 (set during
     * the enabling write), no border 26 or pipe CSC 24, palette read 23 from
     * the VGA pipe, writes 22/21 only to that pipe's palette, 6-bit DAC 20,
     * blink 50% duty and 30 VSYNCs per cycle ([gfxinit] Setup_Display).
     * Reserved/PBC bits 30, 28:27, 25, 19:8 keep their value. */
    uint32_t vga = (s->vgacntrl & ~0xa5f000ffu) | (uint32_t)p << 29 | (uint32_t)p << 23 |
                   (p ? 0x00400000u : 0x00200000u) | 0x80u | (30u / 2u - 1u);
    igd_set(v, &l, "VGACNTRL", IGD_VGACNTRL, vga);
    s->applied |= IGD_DID_VGA;
    bool ok = igd_vblank(v, &l);
    legacy_vga_line_emit(v, &l);
    if (!ok) {
        igd_gen6_restore(v);
        return false;
    }
    legacy_vga_line_start(&l, "igd");
    legacy_vga_set_indexed(
        v, &l, "SR01", 0x3c4, 0x3c5, 1,
        (uint8_t)(legacy_vga_indexed_read(v->h->legacy_io_base, 0x3c4, 0x3c5, 1) & ~0x20u));
    legacy_vga_line_emit(v, &l);
    legacy_vga_line_start(&l, "igd live");
    igd_field(&l, "VGACNTRL", igd_rd(v, IGD_VGACNTRL));
    igd_field(&l, "CONF", igd_rd(v, IGD_PIPECONF(p)));
    igd_field(&l, "SRC", igd_rd(v, IGD_PIPESRC(p)));
    igd_field(&l, "PF", igd_rd(v, IGD_PF_CTL(p)));
    igd_field(&l, "SZ", igd_rd(v, IGD_PF_WIN_SZ(p)));
    igd_field(&l, "DSP", igd_rd(v, IGD_DSPCNTR(p)));
    igd_field(&l, "DEIIR", igd_rd(v, IGD_DEIIR));
    igd_field(&l, "DEISR", igd_rd(v, IGD_DEISR));
    ok = igd_vblank(v, &l);
    legacy_vga_line_emit(v, &l);
    return ok;
}
/* Undo every applied step in the disable order of [i915] intel_vga_disable,
 * ilk_pfit_disable/fastset and plane enable. True when all vblanks came. */
static inline bool igd_gen6_restore(struct legacy_vga_device *v)
{
    struct legacy_vga_igd *s = &v->igd;
    unsigned p = s->index;
    const struct legacy_vga_igd_pipe *q = &s->pipe[p];
    uint64_t config = v->h->ecam_base + ((uint64_t)v->h->vga_bdf << 12);
    bool ok = true;
    struct legacy_vga_line l;
    legacy_vga_line_start(&l, "igd restore");
    if (s->applied & (IGD_DID_SR01 | IGD_DID_VGA)) {
        legacy_vga_set_indexed(
            v, &l, "SR01", 0x3c4, 0x3c5, 1,
            (uint8_t)(legacy_vga_indexed_read(v->h->legacy_io_base, 0x3c4, 0x3c5, 1) | 0x20));
        legacy_vga_delay_us(v, 300);
    }
    /* "program 1b even if it already reads as 1b" (PRM §2.1). */
    if (s->applied & IGD_DID_VGA) {
        igd_set(v, &l, "VGACNTRL", IGD_VGACNTRL, s->vgacntrl | IGD_VGA_DISABLE);
        ok = igd_vblank(v, &l) && ok;
    }
    legacy_vga_line_emit(v, &l);
    if (s->applied & (IGD_DID_SRC | IGD_DID_PF)) {
        legacy_vga_line_start(&l, "igd restore");
        if (s->applied & IGD_DID_SRC)
            igd_set(v, &l, "PIPESRC", IGD_PIPESRC(p), q->src);
        if (s->applied & IGD_DID_PF)
            igd_set(v, &l, "PF_CTL", IGD_PF_CTL(p), q->pf_ctl);
        legacy_vga_line_emit(v, &l);
    }
    if (s->applied & (IGD_DID_SRC | IGD_DID_PF | IGD_DID_CONF)) {
        legacy_vga_line_start(&l, "igd restore");
        if (s->applied & IGD_DID_PF) {
            igd_set(v, &l, "PF_POS", IGD_PF_WIN_POS(p), q->pf_pos);
            igd_set(v, &l, "PF_SZ", IGD_PF_WIN_SZ(p), q->pf_sz);
        }
        if (s->applied & IGD_DID_CONF)
            igd_set(v, &l, "PIPECONF", IGD_PIPECONF(p), q->conf);
        ok = igd_vblank(v, &l) && ok;
        legacy_vga_line_emit(v, &l);
    }
    /* The VGA DAC wrote the legacy palettes (PRM §2.12: one per pipe,
     * reachable through 3C8/3C9 and MMIO, DWORD access only). */
    if (s->applied & IGD_DID_DAC)
        for (unsigned i = 0; i < 2 * 256; ++i)
            igd_wr(v, IGD_PALETTE(i >> 8) + (i & 255) * 4, s->palette[i >> 8][i & 255]);
    legacy_vga_line_start(&l, "igd restore");
    if (s->applied & IGD_DID_DAC)
        legacy_vga_line_str(&l, " palettes=2x256");
    if (s->applied & IGD_DID_PRIMARY) {
        igd_set(v, &l, "DSPCNTR", IGD_DSPCNTR(p), q->dspcntr);
        igd_set(v, &l, "DSPSURF", IGD_DSPSURF(p), q->dspsurf);
    }
    if (s->applied & IGD_DID_SPRITE) {
        igd_set(v, &l, "DVSCNTR", IGD_DVSCNTR(p), q->dvscntr);
        igd_set(v, &l, "DVSSURF", IGD_DVSSURF(p), q->dvssurf);
    }
    if (s->applied & IGD_DID_CURSOR) {
        igd_set(v, &l, "CURCNTR", IGD_CURCNTR(p), q->curcntr);
        igd_set(v, &l, "CURBASE", IGD_CURBASE(p), q->curbase);
    }
    if (s->applied & (IGD_DID_PRIMARY | IGD_DID_SPRITE | IGD_DID_CURSOR))
        ok = igd_vblank(v, &l) && ok;
    legacy_vga_line_emit(v, &l);
    legacy_vga_line_start(&l, "igd restore");
    if (s->applied & IGD_DID_PAGING) {
        legacy_vga_set_indexed(v, &l, "GR10", 0x3ce, 0x3cf, 0x10, s->gr10);
        legacy_vga_set_indexed(v, &l, "GR11", 0x3ce, 0x3cf, 0x11, s->gr11);
    }
    if (s->applied & IGD_DID_MSR)
        legacy_vga_set_msr(v, &l, s->msr);
    if (s->applied & (IGD_DID_SR01 | IGD_DID_VGA))
        legacy_vga_set_indexed(v, &l, "SR01", 0x3c4, 0x3c5, 1, s->sr01);
    if (s->applied & IGD_DID_COMMAND) {
        uint16_t command = legacy_vga_rd16(config + 4);
        legacy_vga_wr16(config + 4, s->command);
        legacy_vga_line_str(&l, " CMD ");
        legacy_vga_line_hex(&l, command, 4);
        legacy_vga_line_str(&l, "->");
        legacy_vga_line_hex(&l, s->command, 4);
        legacy_vga_line_str(&l, "=");
        legacy_vga_line_hex(&l, legacy_vga_rd16(config + 4), 4);
    }
    legacy_vga_line_str(&l, ok ? " ok" : " VBLANK-MISSING");
    legacy_vga_line_emit(v, &l);
    s->applied = 0;
    return ok;
}

/* --- Generic leave / enter / restore --------------------------------------
 * False from leave or enter: the display was left (or put back) in GOP. */
static inline bool legacy_vga_leave_gop(struct legacy_vga_device *v)
{
    const struct ia64vmm_handoff *h = v->h;
    if (h->vga_kind == IA64VMM_VGA_INTEL_GEN6)
        return igd_gen6_leave(v);
    /* PCI enumeration may disable I/O on an all-MMIO BAR device, but its
     * fixed legacy VGA ports still require the endpoint's I/O Enable bit. */
    uint64_t command = h->ecam_base + ((uint64_t)h->vga_bdf << 12) + 4;
    legacy_vga_wr16(command, (uint16_t)(legacy_vga_rd16(command) | 3));
    if (h->vga_kind == IA64VMM_VGA_BOCHS) {
        /* QEMU docs/specs/standard-vga.rst: assigned BAR2+0x500 exposes VBE;
         * use the real x86 ports when BAR2 is absent (no fork-only alias). */
        if (h->vga_control_size >= 0x516)
            legacy_vga_wr16(h->vga_control_base + 0x508, 0);
        else {
            legacy_vga_port_write(h, 0x1ce, 2, 4);
            legacy_vga_port_write(h, 0x1cf, 2, 0);
        }
    } else if (h->vga_kind == IA64VMM_VGA_VMWARE) {
        /* Linux vmwgfx_drv.c vmw_device_init and commit ba723fe8b7e8:
         * ENABLE|HIDE retains device operation; TRACES enables VGA repaint. */
        unsigned port = (unsigned)h->vga_control_base;
        legacy_vga_port_write(h, port, 4, 1);
        legacy_vga_port_write(h, port + 1, 4, 3);
        legacy_vga_port_write(h, port, 4, 45);
        legacy_vga_port_write(h, port + 1, 4, 1);
    }
    return true;
}
static inline void legacy_vga_load_text_font(uint64_t io_base, volatile uint8_t *font,
                                             const uint8_t glyphs[256 * 16])
{
    size_t ch;

    /* The inherited GOP may leave CRTC doubleword addressing enabled.
     * Font-plane byte addresses must be consecutive (VGA CRTC 14/17). */
    legacy_vga_indexed_write(io_base, 0x3d4, 0x3d5, 0x14, 0x00);
    legacy_vga_indexed_write(io_base, 0x3d4, 0x3d5, 0x17, 0xe3);
    legacy_vga_indexed_write(io_base, 0x3c4, 0x3c5, 0x00, 0x01);
    legacy_vga_indexed_write(io_base, 0x3c4, 0x3c5, 0x02, 0x04);
    legacy_vga_indexed_write(io_base, 0x3c4, 0x3c5, 0x04, 0x07);
    legacy_vga_indexed_write(io_base, 0x3c4, 0x3c5, 0x00, 0x03);
    legacy_vga_indexed_write(io_base, 0x3ce, 0x3cf, 0x00, 0x00);
    legacy_vga_indexed_write(io_base, 0x3ce, 0x3cf, 0x01, 0x00);
    legacy_vga_indexed_write(io_base, 0x3ce, 0x3cf, 0x03, 0x00);
    legacy_vga_indexed_write(io_base, 0x3ce, 0x3cf, 0x04, 0x02);
    legacy_vga_indexed_write(io_base, 0x3ce, 0x3cf, 0x05, 0x00);
    legacy_vga_indexed_write(io_base, 0x3ce, 0x3cf, 0x06, 0x00);
    legacy_vga_indexed_write(io_base, 0x3ce, 0x3cf, 0x07, 0x00);
    legacy_vga_indexed_write(io_base, 0x3ce, 0x3cf, 0x08, 0xff);

    for (ch = 0; ch < 256U; ch++) {
        size_t row;

        for (row = 0; row < 32U; row++) {
            uint8_t bits = row < 16 ? glyphs[ch * 16 + row] : 0;

            legacy_vga_wr8((uint64_t)(uintptr_t)(font + ch * 32U + row), bits);
        }
    }
}

static inline void legacy_vga_program_text_mode(uint64_t io_base)
{
    /* 640x400, 80x25 text with native 8x16 glyphs. */
    static const uint8_t seq[] = {0x03, 0x01, 0x03, 0x00, 0x02};
    static const uint8_t crtc[] = {
        0x5f, 0x4f, 0x50, 0x82, 0x55, 0x81, 0xbf, 0x1f, 0x00, 0x4f, 0x0d, 0x0e, 0x00,
        0x00, 0x00, 0x00, 0x9c, 0x8e, 0x8f, 0x28, 0x1f, 0x96, 0xb9, 0xa3, 0xff,
    };
    static const uint8_t attr[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3a,
        0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x0c, 0x00, 0x0f, 0x08, 0x00,
    };
    static const uint8_t gfx[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0e, 0x00, 0xff,
    };
    static const uint8_t dac[][3] = {
        {0x00, 0x00, 0x00}, {0x00, 0x00, 0x2a}, {0x00, 0x2a, 0x00}, {0x00, 0x2a, 0x2a},
        {0x2a, 0x00, 0x00}, {0x2a, 0x00, 0x2a}, {0x2a, 0x15, 0x00}, {0x2a, 0x2a, 0x2a},
        {0x15, 0x15, 0x15}, {0x15, 0x15, 0x3f}, {0x15, 0x3f, 0x15}, {0x15, 0x3f, 0x3f},
        {0x3f, 0x15, 0x15}, {0x3f, 0x15, 0x3f}, {0x3f, 0x3f, 0x15}, {0x3f, 0x3f, 0x3f},
    };
    size_t i;

    legacy_vga_io_write(io_base, 0x3c2, 0x67);
    for (i = 0; i < LEGACY_VGA_ARRAY_SIZE(seq); i++) {
        legacy_vga_indexed_write(io_base, 0x3c4, 0x3c5, (uint8_t)i, seq[i]);
    }

    legacy_vga_indexed_write(io_base, 0x3d4, 0x3d5, 0x11, (uint8_t)(crtc[0x11] & ~0x80U));
    for (i = 0; i < LEGACY_VGA_ARRAY_SIZE(crtc); i++) {
        legacy_vga_indexed_write(io_base, 0x3d4, 0x3d5, (uint8_t)i, crtc[i]);
    }
    for (i = 0; i < LEGACY_VGA_ARRAY_SIZE(gfx); i++) {
        legacy_vga_indexed_write(io_base, 0x3ce, 0x3cf, (uint8_t)i, gfx[i]);
    }
    for (i = 0; i < LEGACY_VGA_ARRAY_SIZE(attr); i++) {
        (void)legacy_vga_io_read(io_base, 0x3da);
        legacy_vga_io_write(io_base, 0x3c0, (uint8_t)i);
        legacy_vga_io_write(io_base, 0x3c0, attr[i]);
    }
    legacy_vga_io_write(io_base, 0x3c6, 0xff);
    for (i = 0; i < LEGACY_VGA_ARRAY_SIZE(dac); i++) {
        /* Attribute palette entries address DAC 14/38..3f in text mode;
         * inherited GOP palette/color-select state is not the BIOS default. */
        legacy_vga_io_write(io_base, 0x3c8, attr[i]);
        legacy_vga_io_write(io_base, 0x3c9, dac[i][0]);
        legacy_vga_io_write(io_base, 0x3c9, dac[i][1]);
        legacy_vga_io_write(io_base, 0x3c9, dac[i][2]);
    }

    legacy_vga_enable_attribute_output(io_base);
}
/* After the shared font/text programming. Bochs and VMware need nothing. */
static inline bool legacy_vga_finish_text(struct legacy_vga_device *v,
                                          const uint8_t glyphs[256 * 16])
{
    return v->h->vga_kind != IA64VMM_VGA_INTEL_GEN6 || igd_gen6_finish(v, glyphs);
}
static inline bool legacy_vga_enter_text(struct legacy_vga_device *v,
                                         const uint8_t glyphs[256 * 16])
{
    if (!legacy_vga_leave_gop(v))
        return false;
    legacy_vga_load_text_font(v->h->legacy_io_base, (volatile uint8_t *)(uintptr_t)v->h->vga_base,
                              glyphs);
    legacy_vga_program_text_mode(v->h->legacy_io_base);
    return legacy_vga_finish_text(v, glyphs);
}
/* Kinds with a documented way back to the inherited GOP mode. */
static inline bool legacy_vga_can_restore_gop(const struct ia64vmm_handoff *h)
{
    return (h->flags & IA64VMM_HF_LEGACY_VGA) &&
           (h->vga_kind == IA64VMM_VGA_INTEL_GEN6 || h->vga_kind == IA64VMM_VGA_BOCHS);
}
static inline bool legacy_vga_restore_gop(struct legacy_vga_device *v)
{
    const struct ia64vmm_handoff *h = v->h;
    if (h->vga_kind == IA64VMM_VGA_INTEL_GEN6)
        return igd_gen6_restore(v);
    if (h->vga_kind != IA64VMM_VGA_BOCHS)
        return false;
    /* QEMU include/hw/display/bochs-vbe.h: ENABLED 0x01 | LFB_ENABLED 0x40 |
     * NOCLEARMEM 0x80 re-enters the retained XRES/YRES/BPP mode. */
    if (h->vga_control_size >= 0x516)
        legacy_vga_wr16(h->vga_control_base + 0x508, 0xc1);
    else {
        legacy_vga_port_write(h, 0x1ce, 2, 4);
        legacy_vga_port_write(h, 0x1cf, 2, 0xc1);
    }
    legacy_vga_note(v, "bochs restore VBE ENABLE=c1");
    return true;
}

/* --- Real-hardware probe (VMM option vga_probe=N) -------------------------
 * Text page for a photograph: readable text, 16 colour cells and the whole
 * font prove the plane, plane-2 font and palette; a countdown proves the
 * screen is live. Then the saved GOP state is restored. */
static inline void legacy_vga_text_put(const struct legacy_vga_device *v, unsigned row,
                                       unsigned column, uint8_t attribute, const char *text)
{
    uint64_t cells = v->h->vga_base + 0x18000u;
    for (; *text && column < 80; ++text, ++column)
        legacy_vga_wr16(cells + (row * 80u + column) * 2u,
                        (uint16_t)(attribute << 8 | (uint8_t)*text));
}
static inline void legacy_vga_probe_page(const struct legacy_vga_device *v)
{
    const struct ia64vmm_handoff *h = v->h;
    uint64_t cells = h->vga_base + 0x18000u;
    struct legacy_vga_line l;
    for (unsigned i = 0; i < 80u * 25u; ++i)
        legacy_vga_wr16(cells + i * 2u, 0x0720);
    l.used = 0;
    legacy_vga_line_str(&l, " IA-64 VMM legacy VGA text probe   kind=");
    legacy_vga_line_str(&l, h->vga_kind == IA64VMM_VGA_INTEL_GEN6 ? "intel-gen6"
                            : h->vga_kind == IA64VMM_VGA_BOCHS    ? "bochs"
                                                                  : "other");
    legacy_vga_line_str(&l, " ");
    legacy_vga_line_hex(&l, h->vga_vendor, 4);
    legacy_vga_line_str(&l, ":");
    legacy_vga_line_hex(&l, h->vga_device, 4);
    if (h->vga_kind == IA64VMM_VGA_INTEL_GEN6)
        legacy_vga_line_str(&l, v->igd.index ? " pipe B" : " pipe A");
    while (l.used < 80)
        legacy_vga_line_str(&l, " ");
    legacy_vga_text_put(v, 0, 0, 0x1f, l.text);
    legacy_vga_text_put(v, 2, 0, 0x07,
                        "Readable text, 16 colour cells and all 256 glyphs below mean that the");
    legacy_vga_text_put(v, 3, 0, 0x07,
                        "VGA plane, the plane-2 font and the palette work on this display.");
    legacy_vga_text_put(v, 5, 0, 0x07, "colours");
    for (unsigned c = 0; c < 16; ++c) {
        char text[4] = {' ', "0123456789ABCDEF"[c], ' ', 0};
        legacy_vga_text_put(v, 5, 8 + c * 4, (uint8_t)(c << 4 | (c < 8 ? 0x0f : 0x00)), text);
    }
    for (unsigned g = 0; g < 256; ++g)
        legacy_vga_wr16(cells + ((7u + g / 32u) * 80u + 8u + (g % 32u) * 2u) * 2u,
                        (uint16_t)(0x0700 | g));
    if (h->vga_kind == IA64VMM_VGA_INTEL_GEN6) {
        const struct legacy_vga_igd_pipe *q = &v->igd.pipe[v->igd.index];
        unsigned p = v->igd.index;
        l.used = 0;
        igd_field(&l, "VGACNTRL", igd_rd(v, IGD_VGACNTRL));
        igd_field(&l, "PIPESRC", igd_rd(v, IGD_PIPESRC(p)));
        igd_field(&l, "PF_CTL", igd_rd(v, IGD_PF_CTL(p)));
        legacy_vga_text_put(v, 16, 0, 0x0f, l.text);
        l.used = 0;
        igd_field(&l, "PF_WIN_SZ", igd_rd(v, IGD_PF_WIN_SZ(p)));
        igd_field(&l, "DSPCNTR", igd_rd(v, IGD_DSPCNTR(p)));
        igd_field(&l, "GOP-DSPCNTR", q->dspcntr);
        legacy_vga_text_put(v, 17, 0, 0x0f, l.text);
        l.used = 0;
        igd_field(&l, "LVDS", v->igd.lvds);
        igd_field(&l, "GGC", v->igd.ggc);
        igd_field(&l, "DEIIR", igd_rd(v, IGD_DEIIR));
        legacy_vga_text_put(v, 18, 0, 0x0f, l.text);
        static const char *const verdict[3] = {"-", "ok", "BAD"};
        l.used = 0;
        legacy_vga_line_str(&l, "GOP GR10=");
        legacy_vga_line_hex(&l, v->igd.gr10, 2);
        legacy_vga_line_str(&l, " GR11=");
        legacy_vga_line_hex(&l, v->igd.gr11, 2);
        legacy_vga_line_str(&l, " paged=");
        legacy_vga_line_str(&l, verdict[v->igd.diag_paged % 3]);
        legacy_vga_line_str(&l, " planar=");
        legacy_vga_line_str(&l, verdict[v->igd.diag_planar % 3]);
        legacy_vga_line_str(&l, " text=");
        legacy_vga_line_str(&l, verdict[v->igd.diag_text % 3]);
        legacy_vga_line_str(&l, " font=");
        legacy_vga_line_str(&l, verdict[v->igd.diag_font % 3]);
        legacy_vga_text_put(v, 19, 0, 0x0f, l.text);
    }
    legacy_vga_text_put(
        v, 22, 0, 0x07,
        "The firmware restores the GOP screen next; the VMM summary lists the trace.");
}
static inline void legacy_vga_probe_countdown(const struct legacy_vga_device *v, unsigned seconds)
{
    struct legacy_vga_line l;
    l.used = 0;
    legacy_vga_line_str(&l, " probe hold: ");
    legacy_vga_line_dec(&l, seconds);
    legacy_vga_line_str(&l, " s left ");
    while (l.used < 80)
        legacy_vga_line_str(&l, " ");
    legacy_vga_text_put(v, 24, 0, 0x70, l.text);
}
/* Does a store to the GOP framebuffer's first dword change VGA plane memory?
 * Answers whether VMM drawing to the inherited framebuffer can clobber the
 * VGA planes on this IGD. The dword and the planes are put back. */
static inline void legacy_vga_probe_alias(const struct legacy_vga_device *v)
{
    const struct ia64vmm_handoff *h = v->h;
    uint64_t io = h->legacy_io_base;
    uint32_t planes[2] = {0, 0};
    if (h->fb_format == IA64VMM_FB_NONE || !h->fb_size)
        return;
    uint32_t saved = legacy_vga_rd32(h->fb_base);
    legacy_vga_indexed_write(io, 0x3c4, 0x3c5, 0x04, 0x06);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x05, 0x00);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x06, 0x04);
    for (unsigned pass = 0; pass < 2; ++pass) {
        if (pass)
            legacy_vga_wr32(h->fb_base, saved ^ 0x5aa5c33cu);
        for (unsigned plane = 0; plane < 4; ++plane) {
            legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x04, (uint8_t)plane);
            planes[pass] |= (uint32_t)legacy_vga_rd8(h->vga_base) << (plane * 8);
        }
    }
    legacy_vga_wr32(h->fb_base, saved);
    legacy_vga_indexed_write(io, 0x3c4, 0x3c5, 0x04, 0x02);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x04, 0x00);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x05, 0x10);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x06, 0x0e);
    struct legacy_vga_line l;
    legacy_vga_line_start(&l, "probe alias A0000 planes3..0=");
    legacy_vga_line_hex(&l, planes[0], 8);
    legacy_vga_line_str(&l, " fb[0]=");
    legacy_vga_line_hex(&l, saved, 8);
    legacy_vga_line_str(&l, " after fb store=");
    legacy_vga_line_hex(&l, planes[1], 8);
    legacy_vga_line_str(&l, planes[0] == planes[1] ? " (no alias at fb+0)"
                                                   : " (ALIAS: fb+0 is VGA memory)");
    legacy_vga_line_emit(v, &l);
    legacy_vga_text_put(v, 20, 0, 0x0e,
                        planes[0] == planes[1] ? "fb+0 store: no VGA plane alias"
                                               : "fb+0 store: ALIASES VGA plane memory");
}
/* --- Probe diagnostics for the VGA memory path (Intel gen6) -------------
 * Run between the shared text programming and the finish step, so they
 * report even when the font readback then fails. Every byte written is
 * read first and put back. Planar access: SR04=06 sequential, GR05=00,
 * GR06=04 (A0000-AFFFF), SR02 = one plane, GR04 = plane read (PRM Vol3 Pt1
 * §1.2.4, §1.2.6, §1.3.6-§1.3.8). */
static inline void igd_planar(uint64_t io)
{
    legacy_vga_indexed_write(io, 0x3c4, 0x3c5, 0x04, 0x06);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x05, 0x00);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x06, 0x04);
}
static inline void igd_text_state(uint64_t io)
{
    legacy_vga_indexed_write(io, 0x3c4, 0x3c5, 0x02, 0x03);
    legacy_vga_indexed_write(io, 0x3c4, 0x3c5, 0x04, 0x02);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x04, 0x00);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x05, 0x10);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x06, 0x0e);
}
static inline uint8_t igd_plane_read(const struct legacy_vga_device *v, unsigned plane,
                                     uint32_t offset)
{
    legacy_vga_indexed_write(v->h->legacy_io_base, 0x3ce, 0x3cf, 0x04, (uint8_t)plane);
    return legacy_vga_rd8(v->h->vga_base + offset);
}
static inline void igd_plane_write(const struct legacy_vga_device *v, unsigned plane,
                                   uint32_t offset, uint8_t value)
{
    legacy_vga_indexed_write(v->h->legacy_io_base, 0x3c4, 0x3c5, 0x02, (uint8_t)(1u << plane));
    legacy_vga_wr8(v->h->vga_base + offset, value);
}
static inline void igd_probe_diag(struct legacy_vga_device *v, const uint8_t glyphs[256 * 16])
{
    struct legacy_vga_igd *s = &v->igd;
    uint64_t io = v->h->legacy_io_base, memory = v->h->vga_base;
    struct legacy_vga_line l;
    /* 1. Planes 0-3 at four offsets: distinct byte per plane and offset,
     * read back through each read map. At offset 1000 the same four bytes
     * are read once more through the GOP's GR10/GR11 paging, whose linear
     * view would show them at 4*1000+plane if VGA memory is stored with
     * planes interleaved per dword. */
    static const uint32_t offsets[4] = {0x0000, 0x1000, 0x8000, 0xffff};
    unsigned bad = 0;
    uint32_t paged = 0, expect_paged = 0;
    igd_planar(io);
    legacy_vga_line_start(&l, "diag planar w/r");
    for (unsigned k = 0; k < 4; ++k) {
        uint8_t saved[4], wrote[4], got[4];
        for (unsigned p = 0; p < 4; ++p)
            saved[p] = igd_plane_read(v, p, offsets[k]);
        for (unsigned p = 0; p < 4; ++p) {
            wrote[p] = (uint8_t)(0x10u * (p + 1) + k);
            igd_plane_write(v, p, offsets[k], wrote[p]);
        }
        for (unsigned p = 0; p < 4; ++p) {
            got[p] = igd_plane_read(v, p, offsets[k]);
            if (got[p] != wrote[p])
                ++bad;
        }
        if (k == 1 && (s->gr10 & 1)) {
            legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x10, s->gr10);
            legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x11, s->gr11);
            for (unsigned p = 0; p < 4; ++p) {
                paged |= (uint32_t)legacy_vga_rd8(memory + 4u * offsets[k] + p) << (8 * (3 - p));
                expect_paged |= (uint32_t)wrote[p] << (8 * (3 - p));
            }
            legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x10, 0x00);
            legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x11, 0x00);
        }
        for (unsigned p = 0; p < 4; ++p)
            igd_plane_write(v, p, offsets[k], saved[p]);
        legacy_vga_line_str(&l, " ");
        legacy_vga_line_hex(&l, offsets[k], 4);
        legacy_vga_line_str(&l, ":");
        for (unsigned p = 0; p < 4; ++p)
            legacy_vga_line_hex(&l, wrote[p], 2);
        legacy_vga_line_str(&l, "/");
        for (unsigned p = 0; p < 4; ++p)
            legacy_vga_line_hex(&l, got[p], 2);
    }
    s->diag_planar = bad ? 2 : 1;
    legacy_vga_line_str(&l, bad ? " BAD" : " ok");
    if (s->gr10 & 1) {
        s->diag_paged = paged == expect_paged ? 1 : 2;
        legacy_vga_line_str(&l, " paged@4000=");
        legacy_vga_line_hex(&l, paged, 8);
        legacy_vga_line_str(&l, paged == expect_paged ? " =interleaved" : " other");
    }
    legacy_vga_line_emit(v, &l);
    /* 2. B8000 in text odd/even mode: a char/attr word per offset, read
     * back through B8000 and as planes 0/1 at offset and offset+1. */
    static const uint32_t cells[3] = {0x0000, 0x1000, 0x7ffe};
    bad = 0;
    legacy_vga_line_start(&l, "diag text");
    for (unsigned k = 0; k < 3; ++k) {
        uint64_t at = memory + 0x18000u + cells[k];
        uint16_t word = (uint16_t)((0x1eu + k) << 8 | (0x41u + k)), saved, back;
        uint8_t pl[2][2];
        igd_text_state(io);
        saved = legacy_vga_rd16(at);
        legacy_vga_wr16(at, word);
        back = legacy_vga_rd16(at);
        igd_planar(io);
        for (unsigned p = 0; p < 2; ++p)
            for (unsigned b = 0; b < 2; ++b)
                pl[p][b] = igd_plane_read(v, p, cells[k] + b);
        igd_text_state(io);
        legacy_vga_wr16(at, saved);
        if (back != word)
            ++bad;
        legacy_vga_line_str(&l, " ");
        legacy_vga_line_hex(&l, cells[k], 4);
        legacy_vga_line_str(&l, ":");
        legacy_vga_line_hex(&l, word, 4);
        legacy_vga_line_str(&l, ">");
        legacy_vga_line_hex(&l, back, 4);
        legacy_vga_line_str(&l, " p0=");
        legacy_vga_line_hex(&l, pl[0][0], 2);
        legacy_vga_line_hex(&l, pl[0][1], 2);
        legacy_vga_line_str(&l, " p1=");
        legacy_vga_line_hex(&l, pl[1][0], 2);
        legacy_vga_line_hex(&l, pl[1][1], 2);
    }
    s->diag_text = bad ? 2 : 1;
    legacy_vga_line_str(&l, bad ? " BAD" : " ok");
    legacy_vga_line_emit(v, &l);
    /* 3. The first 16 font bytes of three glyphs in the font-load
     * configuration (SR04=07, GR04=02, GR05=00, GR06=00): '=' matches the
     * glyph table, '!' differs. */
    static const uint8_t chars[3] = {0x00, 0x41, 0xdb};
    bad = 0;
    legacy_vga_indexed_write(io, 0x3c4, 0x3c5, 0x04, 0x07);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x04, 0x02);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x05, 0x00);
    legacy_vga_indexed_write(io, 0x3ce, 0x3cf, 0x06, 0x00);
    legacy_vga_line_start(&l, "diag font");
    for (unsigned k = 0; k < 3; ++k) {
        bool same = true;
        legacy_vga_line_str(&l, " ");
        legacy_vga_line_hex(&l, chars[k], 2);
        legacy_vga_line_str(&l, ":");
        for (unsigned row = 0; row < 16; ++row) {
            uint8_t got = legacy_vga_rd8(memory + chars[k] * 32u + row);
            same = same && got == glyphs[chars[k] * 16u + row];
            legacy_vga_line_hex(&l, got, 2);
        }
        legacy_vga_line_str(&l, same ? "=" : "!");
        if (!same)
            ++bad;
    }
    s->diag_font = bad ? 2 : 1;
    igd_text_state(io);
    legacy_vga_line_emit(v, &l);
}
/* Returns true when the display is back in the inherited GOP mode. */
static inline bool legacy_vga_probe(struct legacy_vga_device *v, const uint8_t glyphs[256 * 16])
{
    const struct ia64vmm_handoff *h = v->h;
    struct legacy_vga_line l;
    legacy_vga_line_start(&l, "probe hold=");
    legacy_vga_line_dec(&l, h->vga_probe_sec);
    legacy_vga_line_str(&l, "s kind=");
    legacy_vga_line_dec(&l, h->vga_kind);
    legacy_vga_line_str(&l, " legacy_vga=");
    legacy_vga_line_dec(&l, !!(h->flags & IA64VMM_HF_LEGACY_VGA));
    legacy_vga_line_emit(v, &l);
    if (!legacy_vga_can_restore_gop(h)) {
        legacy_vga_note(
            v, "probe skipped: this display kind has no documented VGA enter/restore pair");
        return true;
    }
    /* legacy_vga_enter_text with the diagnostics before the finish step. */
    if (!legacy_vga_leave_gop(v)) {
        legacy_vga_note(v, "probe: VGA text not entered; GOP kept (see FAIL above)");
        return true;
    }
    legacy_vga_load_text_font(h->legacy_io_base, (volatile uint8_t *)(uintptr_t)h->vga_base,
                              glyphs);
    legacy_vga_program_text_mode(h->legacy_io_base);
    if (h->vga_kind == IA64VMM_VGA_INTEL_GEN6)
        igd_probe_diag(v, glyphs);
    if (!legacy_vga_finish_text(v, glyphs)) {
        legacy_vga_note(v, "probe: VGA text not entered; GOP kept (see FAIL above)");
        return true;
    }
    legacy_vga_probe_page(v);
    if (h->vga_kind == IA64VMM_VGA_INTEL_GEN6)
        legacy_vga_probe_alias(v);
    legacy_vga_note(v, "probe holding VGA text page");
    for (unsigned left = h->vga_probe_sec; left; --left) {
        legacy_vga_probe_countdown(v, left);
        legacy_vga_delay_us(v, 1000000u);
    }
    bool ok = legacy_vga_restore_gop(v);
    legacy_vga_note(v, ok ? "probe done: GOP restored" : "probe done: GOP restore INCOMPLETE");
    return ok;
}

#undef LEGACY_VGA_ARRAY_SIZE
#endif
