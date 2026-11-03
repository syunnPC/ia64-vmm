/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register-model test for firmware/new/vga-text.h.
 *
 * A model of the Sandy Bridge display registers the sequence uses (double-
 * buffered plane/fitter/source state latched at frame boundaries, VGACNTRL,
 * palettes), the VGA register file and planar memory, and the IGD config
 * registers records every access in order. A monitor checks the documented
 * ordering rules while the model runs. The same model replays recorded
 * traces: the unmodified trace must replay clean, and every mutant (one
 * documented step dropped, moved or corrupted) must be rejected. Bochs and
 * VMware traces must equal their access lists without the IGD sequence. */
#define LEGACY_VGA_HOOKS
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t legacy_vga_rd8(uint64_t a);
static uint16_t legacy_vga_rd16(uint64_t a);
static uint32_t legacy_vga_rd32(uint64_t a);
static void legacy_vga_wr8(uint64_t a, uint8_t v);
static void legacy_vga_wr16(uint64_t a, uint16_t v);
static void legacy_vga_wr32(uint64_t a, uint32_t v);
static void legacy_vga_wr64(uint64_t a, uint64_t v);
static uint64_t legacy_vga_ticks(void);
#include "firmware/new/vga-text.h"
#include "firmware/include/ui/vgafont.c"
#include "core/log_internal.h"

#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #e);                                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

enum { EV_R, EV_W, EV_FRAME };
struct event {
    uint8_t type, width;
    uint64_t address, value;
};
enum {
    V_MSR_ORDER = 1u << 0,     /* CRTC or VGA memory used before MSR enabled them */
    V_PLANES_AT_VGA = 1u << 1, /* a hires plane live when VGACNTRL enabled VGA */
    V_PF_AT_VGA = 1u << 2,     /* fitter not live/full-panel when VGA enabled */
    V_SRC_AT_VGA = 1u << 3,    /* pipe source not 640x400 when VGA enabled */
    V_SR01_AT_VGA = 1u << 4,   /* screen not off at the VGACNTRL enable write */
    V_SRC_NO_PF = 1u << 5,     /* PIPESRC changed on a running pipe without fitter */
    V_FORBIDDEN = 1u << 6,     /* write outside the documented register set */
    V_PIPE_SELECT = 1u << 7,   /* VGACNTRL pipe select differs from the panel pipe */
};

#define IO IA64VMM_LEGACY_IO_BASE
#define ECAM IA64VMM_ECAM_BASE
#define SERVICE IA64VMM_SERVICE_BASE
#define BAR0 0xe0000000ull
#define BAR2 0xd0000000ull
#define FB 0xd0000000ull
#define IGD_BDF 0x10u
#define FRAME_TICKS 6666667ull                              /* 60 Hz at the 400 MHz ITC */
#define LOG_MATCH_COLUMNS_FOR_TEST (LOG_MATCH_COLUMNS - 8u) /* minus "[guest] " */

struct pipe_live {
    uint32_t conf, src, dspcntr, curcntr, dvscntr, pf_ctl, pf_pos, pf_sz;
    bool dsp_armed, cur_armed, dvs_armed, pf_armed, src_pending, conf_pending;
};
struct model {
    unsigned kind;
    bool recording, frozen, vga_dead, alias;
    struct event *events;
    size_t count, capacity;
    uint64_t now, next_frame;
    uint16_t command, ggc;
    uint8_t msr, seq_index, seq[8], gr_index, gr[32], crtc_index, crtc[32], ar_index, ar[32];
    bool ar_data;
    uint8_t dac_write, dac_part, dac[256][3];
    uint8_t plane[4][65536];
    uint32_t fb0;
    uint32_t mmio[0x100000 / 4];
    struct pipe_live live[2];
    unsigned violations;
    unsigned panel_pipe;
    uint64_t service[2];
};
static struct model m;
static uint8_t glyphs[256 * 16];
static const uint8_t *expected_font = glyphs; /* what text_ready compares */
static struct legacy_vga_device ctx;
static struct ia64vmm_handoff handoff;
static char trace_text[256][192];
static unsigned trace_lines;
static void trace_sink(const char *line)
{
    CHECK(strlen(line) < sizeof(trace_text[0]));
    CHECK(!strncmp(line, "vga: ", 5));
    if (trace_lines < 256)
        strcpy(trace_text[trace_lines++], line);
}
static bool traced(const char *needle)
{
    for (unsigned i = 0; i < trace_lines; ++i)
        if (strstr(trace_text[i], needle))
            return true;
    return false;
}

static void record(uint8_t type, uint8_t width, uint64_t address, uint64_t value)
{
    if (!m.recording)
        return;
    if (m.count == m.capacity) {
        m.capacity = m.capacity ? m.capacity * 2 : 65536;
        m.events = realloc(m.events, m.capacity * sizeof(*m.events));
        CHECK(m.events);
    }
    m.events[m.count++] = (struct event){type, width, address, value};
}
static uint32_t *reg(uint32_t offset)
{
    return &m.mmio[offset / 4];
}

/* Start of vertical blank on every running pipe: armed double-buffered
 * registers latch (SNB PRM Vol3 Pt2 §1.1.1, §3.5, §4.6-§4.10). */
static void frame(void)
{
    record(EV_FRAME, 0, 0, 0);
    for (unsigned p = 0; p < 2; ++p) {
        struct pipe_live *l = &m.live[p];
        if (!(l->conf & IGD_ENABLE))
            continue;
        if (l->dsp_armed)
            l->dspcntr = *reg(IGD_DSPCNTR(p));
        if (l->cur_armed)
            l->curcntr = *reg(IGD_CURCNTR(p));
        if (l->dvs_armed)
            l->dvscntr = *reg(IGD_DVSCNTR(p));
        if (l->pf_armed) {
            l->pf_ctl = *reg(IGD_PF_CTL(p));
            l->pf_pos = *reg(IGD_PF_WIN_POS(p));
            l->pf_sz = *reg(IGD_PF_WIN_SZ(p));
        }
        if (l->src_pending)
            l->src = *reg(IGD_PIPESRC(p));
        if (l->conf_pending)
            l->conf = *reg(IGD_PIPECONF(p));
        l->dsp_armed = l->cur_armed = l->dvs_armed = l->pf_armed = l->src_pending =
            l->conf_pending = false;
        ++*reg(IGD_FRMCOUNT(p));
    }
}
static uint64_t legacy_vga_ticks(void)
{
    m.now += 4000; /* 10 us per call */
    while (!m.frozen && m.now >= m.next_frame) {
        m.next_frame += FRAME_TICKS;
        frame();
    }
    return m.now;
}

/* VGA planar memory: GR06 map, SR02 write mask, SR04/GR05 odd/even, GR04
 * read map; MSR bit 1 gates the whole A0000-BFFFF aperture (PRM Vol3 Pt1
 * §1.1.4, §1.2, §1.3). */
static bool vga_window(uint64_t a, uint32_t *offset)
{
    static const uint32_t base[4] = {0xa0000, 0xa0000, 0xb0000, 0xb8000};
    static const uint32_t size[4] = {0x20000, 0x10000, 0x8000, 0x8000};
    unsigned map = (m.gr[6] >> 2) & 3;
    if (m.vga_dead || !(m.msr & 2) || a < base[map] || a >= base[map] + size[map])
        return false;
    *offset = (uint32_t)(a - base[map]);
    return true;
}
/* GR10 bit 0 with GR06 map 01: A0000-AFFFF goes to stolen memory page
 * GR10[7:4]:GR11[6:0] without the VGA swizzle (PRM Vol3 Pt1 §1.3.11/12).
 * The model stores VGA memory with the four planes interleaved per dword
 * (linear byte 4*offset+plane), which reproduces gen6 VGA font mapping
 * exactly (test_gen6_paged). Beyond 256 KiB: other stolen memory. */
static uint8_t *paged(uint32_t offset)
{
    static uint8_t other;
    if (!(m.gr[0x10] & 1) || ((m.gr[6] >> 2) & 3) != 1)
        return NULL;
    uint32_t linear =
        ((uint32_t)(m.gr[0x10] >> 4) << 23 | (uint32_t)(m.gr[0x11] & 0x7f) << 16) + offset;
    other = 0;
    return linear < 0x40000 ? &m.plane[linear & 3][linear >> 2] : &other;
}
static void vga_mem_write(uint64_t a, uint8_t v)
{
    uint32_t offset;
    if (m.kind == IA64VMM_VGA_INTEL_GEN6 && (m.msr & 3) != 3)
        m.violations |= V_MSR_ORDER;
    if (!vga_window(a, &offset))
        return;
    if (paged(offset)) {
        *paged(offset) = v;
        return;
    }
    unsigned mask = m.seq[2] & 15;
    if (!(m.seq[4] & 4)) {
        mask &= (offset & 1) ? 0xa : 0x5;
        offset &= ~1u;
    }
    for (unsigned p = 0; p < 4; ++p)
        if (mask & (1u << p))
            m.plane[p][offset & 0xffff] = v;
}
static uint8_t vga_mem_read(uint64_t a)
{
    uint32_t offset;
    if (!vga_window(a, &offset))
        return 0xff;
    if (paged(offset))
        return *paged(offset);
    unsigned p = m.gr[4] & 3;
    if (m.gr[5] & 0x10) {
        p = (p & 2) | (offset & 1);
        offset &= ~1u;
    }
    return m.plane[p][offset & 0xffff];
}
static void dac_entry(void)
{
    /* PRM Vol3 Pt2 §2.1 bits 22/21: VGA palette writes update palette A/B;
     * 6-bit DAC values shift up two bits (bit 20 clear). */
    uint32_t vga = *reg(IGD_VGACNTRL), rgb = 0;
    for (unsigned c = 0; c < 3; ++c)
        rgb = rgb << 8 | (uint32_t)(m.dac[m.dac_write][c] << 2);
    if (m.kind == IA64VMM_VGA_INTEL_GEN6) {
        if (!(vga & 0x00400000u))
            *reg(IGD_PALETTE(0) + m.dac_write * 4u) = rgb;
        if (!(vga & 0x00200000u))
            *reg(IGD_PALETTE(1) + m.dac_write * 4u) = rgb;
    }
}
static void port_write(unsigned port, uint8_t v)
{
    bool cga = (m.msr & 1) != 0;
    if ((port == 0x3d4 || port == 0x3d5) && m.kind == IA64VMM_VGA_INTEL_GEN6 && !cga) {
        m.violations |= V_MSR_ORDER;
        return;
    }
    switch (port) {
    case 0x3c2:
        m.msr = v;
        break;
    case 0x3c4:
        m.seq_index = v;
        break;
    case 0x3c5:
        m.seq[m.seq_index & 7] = v;
        break;
    case 0x3ce:
        m.gr_index = v;
        break;
    case 0x3cf:
        m.gr[m.gr_index & 31] = v;
        break;
    case 0x3d4:
        m.crtc_index = v;
        break;
    case 0x3d5:
        m.crtc[m.crtc_index & 31] = v;
        break;
    case 0x3c0:
        if (m.ar_data)
            m.ar[m.ar_index & 31] = v;
        else
            m.ar_index = v;
        m.ar_data = !m.ar_data;
        break;
    case 0x3c6:
        break;
    case 0x3c8:
        m.dac_write = v;
        m.dac_part = 0;
        break;
    case 0x3c9:
        m.dac[m.dac_write][m.dac_part] = v;
        if (++m.dac_part == 3) {
            dac_entry();
            m.dac_part = 0;
            ++m.dac_write;
        }
        break;
    default:
        m.violations |= V_FORBIDDEN;
        break;
    }
}
static uint8_t port_read(unsigned port)
{
    if ((port == 0x3d4 || port == 0x3d5) && m.kind == IA64VMM_VGA_INTEL_GEN6 && !(m.msr & 1))
        return 0xff;
    switch (port) {
    case 0x3cc:
        return m.msr;
    case 0x3c4:
        return m.seq_index;
    case 0x3c5:
        return m.seq[m.seq_index & 7];
    case 0x3ce:
        return m.gr_index;
    case 0x3cf:
        return m.gr[m.gr_index & 31];
    case 0x3d4:
        return m.crtc_index;
    case 0x3d5:
        return m.crtc[m.crtc_index & 31];
    case 0x3da:
        m.ar_data = false;
        return 0;
    default:
        return 0xff;
    }
}
/* VGACNTRL enable: the conditions of PRM Vol3 Pt2 §2.1 at that moment. */
static void vga_enable_checks(uint32_t value)
{
    unsigned p = m.panel_pipe;
    const struct pipe_live *l = &m.live[p];
    uint32_t width = (*reg(IGD_HTOTAL(p)) & 0xfff) + 1, height = (*reg(IGD_VTOTAL(p)) & 0xfff) + 1;
    if (((value >> 29) & 1) != p)
        m.violations |= V_PIPE_SELECT;
    if ((l->dspcntr & IGD_ENABLE) || (l->curcntr & IGD_CURSOR_MODE) || (l->dvscntr & IGD_ENABLE))
        m.violations |= V_PLANES_AT_VGA;
    if (!(l->pf_ctl & IGD_ENABLE) || l->pf_pos || l->pf_sz != (width << 16 | height))
        m.violations |= V_PF_AT_VGA;
    if (l->src != IGD_VGA_SOURCE)
        m.violations |= V_SRC_AT_VGA;
    if (!(m.seq[1] & 0x20))
        m.violations |= V_SR01_AT_VGA;
}
static void mmio_write(uint32_t offset, uint32_t value)
{
    for (unsigned p = 0; p < 2; ++p) {
        struct pipe_live *l = &m.live[p];
        if (offset == IGD_DSPCNTR(p) || offset == IGD_CURCNTR(p) || offset == IGD_DVSCNTR(p) ||
            offset == IGD_PF_CTL(p) || offset == IGD_PF_WIN_POS(p)) {
            *reg(offset) = value;
            return;
        }
        if (offset == IGD_DSPSURF(p)) {
            *reg(offset) = value;
            l->dsp_armed = true;
            return;
        }
        if (offset == IGD_CURBASE(p)) {
            *reg(offset) = value;
            l->cur_armed = true;
            return;
        }
        if (offset == IGD_DVSSURF(p)) {
            *reg(offset) = value;
            l->dvs_armed = true;
            return;
        }
        if (offset == IGD_PF_WIN_SZ(p)) {
            *reg(offset) = value;
            l->pf_armed = true;
            return;
        }
        if (offset == IGD_PIPESRC(p)) {
            /* PRM §1.1.2: on the fly only with panel fitting enabled. */
            if ((l->conf & IGD_ENABLE) && !(l->pf_ctl & IGD_ENABLE))
                m.violations |= V_SRC_NO_PF;
            *reg(offset) = value;
            l->src_pending = true;
            return;
        }
        if (offset == IGD_PIPECONF(p)) {
            /* Only the palette mode (25:24) may change. */
            if ((value ^ *reg(offset)) & ~IGD_PIPE_PALETTE)
                m.violations |= V_FORBIDDEN;
            *reg(offset) = value;
            l->conf_pending = true;
            return;
        }
        if (offset >= IGD_PALETTE(p) && offset < IGD_PALETTE(p) + 1024) {
            *reg(offset) = value;
            return;
        }
    }
    if (offset == IGD_VGACNTRL) {
        if ((*reg(offset) & IGD_VGA_DISABLE) && !(value & IGD_VGA_DISABLE))
            vga_enable_checks(value);
        *reg(offset) = value;
        return;
    }
    m.violations |= V_FORBIDDEN;
}
static uint64_t model_read(uint64_t a, unsigned width)
{
    if (a >= IO && a < IO + 0x4000000) {
        CHECK(width == 1);
        return port_read((unsigned)(a - IO) & 0xfff);
    }
    if (a >= ECAM + (IGD_BDF << 12) && a < ECAM + (IGD_BDF << 12) + 4096) {
        unsigned r = (unsigned)(a - ECAM - (IGD_BDF << 12));
        CHECK(width == 2);
        return r == 4 ? m.command : r == 0x50 ? m.ggc : 0xffff;
    }
    if (a >= 0xa0000 && a < 0xc0000) {
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= (uint64_t)vga_mem_read(a + i) << (8 * i);
        return value;
    }
    if (a >= BAR0 && a < BAR0 + 0x100000) {
        CHECK(width == 4);
        return *reg((uint32_t)(a - BAR0));
    }
    if (a == FB && width == 4)
        return m.fb0;
    CHECK(!"read outside the model");
    return 0;
}
static void model_write(uint64_t a, unsigned width, uint64_t v)
{
    if (a >= IO && a < IO + 0x4000000) {
        CHECK(width == 1);
        port_write((unsigned)(a - IO) & 0xfff, (uint8_t)v);
        return;
    }
    if (a >= ECAM + (IGD_BDF << 12) && a < ECAM + (IGD_BDF << 12) + 4096) {
        CHECK(width == 2 && a == ECAM + (IGD_BDF << 12) + 4);
        m.command = (uint16_t)v;
        return;
    }
    if (a >= 0xa0000 && a < 0xc0000) {
        for (unsigned i = 0; i < width; ++i)
            vga_mem_write(a + i, (uint8_t)(v >> (8 * i)));
        return;
    }
    if (a >= BAR0 && a < BAR0 + 0x100000) {
        CHECK(width == 4);
        mmio_write((uint32_t)(a - BAR0), (uint32_t)v);
        return;
    }
    if (a == BAR2 + 0x508 && width == 2)
        return; /* Bochs VBE ENABLE */
    if ((a == SERVICE + IA64VMM_SVC_PORT_SELECT || a == SERVICE + IA64VMM_SVC_PORT_DATA) &&
        width == 8) {
        m.service[a == SERVICE + IA64VMM_SVC_PORT_DATA] = v;
        return;
    }
    if (a == FB && width == 4) {
        m.fb0 = (uint32_t)v;
        if (m.alias)
            for (unsigned p = 0; p < 4; ++p)
                m.plane[p][0] = (uint8_t)(v >> (8 * p));
        return;
    }
    CHECK(!"write outside the model");
}
static uint8_t legacy_vga_rd8(uint64_t a)
{
    uint64_t v = model_read(a, 1);
    record(EV_R, 1, a, v);
    return (uint8_t)v;
}
static uint16_t legacy_vga_rd16(uint64_t a)
{
    uint64_t v = model_read(a, 2);
    record(EV_R, 2, a, v);
    return (uint16_t)v;
}
static uint32_t legacy_vga_rd32(uint64_t a)
{
    uint64_t v = model_read(a, 4);
    record(EV_R, 4, a, v);
    return (uint32_t)v;
}
static void legacy_vga_wr8(uint64_t a, uint8_t v)
{
    record(EV_W, 1, a, v);
    model_write(a, 1, v);
}
static void legacy_vga_wr16(uint64_t a, uint16_t v)
{
    record(EV_W, 2, a, v);
    model_write(a, 2, v);
}
static void legacy_vga_wr32(uint64_t a, uint32_t v)
{
    record(EV_W, 4, a, v);
    model_write(a, 4, v);
}
static void legacy_vga_wr64(uint64_t a, uint64_t v)
{
    record(EV_W, 8, a, v);
    model_write(a, 8, v);
}

/* gen6 state left by the UEFI GOP: LVDS on transcoder/pipe A, 1366x768
 * BGRX primary plane, VGA plane disabled, I/O decode off, MSR at reset. */
static void reset_model(unsigned kind, unsigned panel_pipe)
{
    free(m.events);
    memset(&m, 0, sizeof(m));
    m.kind = kind;
    m.recording = true;
    m.next_frame = FRAME_TICKS;
    m.panel_pipe = panel_pipe;
    m.command = 0x0006;
    m.ggc = 0x0209;
    m.msr = 0x00;
    m.seq[1] = 0x01;
    /* A VBIOS-built GOP left VGA paging on (GR10 bit 0); see
     * test_gen6_paged. Bochs and VMware have no GR10. */
    if (kind == IA64VMM_VGA_INTEL_GEN6)
        m.gr[0x10] = 0x01;
    *reg(IGD_VGACNTRL) = 0x80002900u;
    *reg(IGD_PCH_LVDS) = 0x80300300u | (uint32_t)panel_pipe << 29;
    *reg(IGD_DEISR) = 0x00000080u;
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned i = 0; i < 256; ++i)
            *reg(IGD_PALETTE(p) + i * 4) = (i * 0x010101u) ^ p;
    unsigned p = panel_pipe;
    *reg(IGD_PIPECONF(p)) = m.live[p].conf = 0xc0000050u;
    *reg(IGD_HTOTAL(p)) = (1525u << 16) | 1365u;
    *reg(IGD_VTOTAL(p)) = (789u << 16) | 767u;
    *reg(IGD_PIPESRC(p)) = m.live[p].src = (1365u << 16) | 767u;
    *reg(IGD_DSPCNTR(p)) = m.live[p].dspcntr = 0x98004000u;
    *reg(IGD_DSPSURF(p)) = 0x00100000u;
    *reg(IGD_FRMCOUNT(p)) = 0x1000u;
    memset(&handoff, 0, sizeof(handoff));
    handoff.flags = IA64VMM_HF_LEGACY_VGA;
    handoff.itc_hz = IA64VMM_ITC_HZ;
    handoff.legacy_io_base = IO;
    handoff.service_base = SERVICE;
    handoff.ecam_base = ECAM;
    handoff.vga_base = IA64VMM_VGA_MEMORY_BASE;
    handoff.vga_size = IA64VMM_VGA_MEMORY_SIZE;
    handoff.vga_bdf = IGD_BDF;
    handoff.vga_kind = (uint16_t)kind;
    handoff.vga_vendor = kind == IA64VMM_VGA_INTEL_GEN6 ? 0x8086
                         : kind == IA64VMM_VGA_BOCHS    ? 0x1234
                                                        : 0x15ad;
    handoff.vga_device = kind == IA64VMM_VGA_INTEL_GEN6 ? 0x0126
                         : kind == IA64VMM_VGA_BOCHS    ? 0x1111
                                                        : 0x0405;
    handoff.vga_control_base = kind == IA64VMM_VGA_INTEL_GEN6 ? BAR0
                               : kind == IA64VMM_VGA_BOCHS    ? BAR2
                                                              : 0x2140;
    handoff.vga_control_size = kind == IA64VMM_VGA_INTEL_GEN6 ? 0x400000
                               : kind == IA64VMM_VGA_BOCHS    ? 0x1000
                                                              : 16;
    handoff.fb_base = FB;
    handoff.fb_size = 0x400000;
    handoff.fb_format = IA64VMM_FB_BGRX8888;
    memset(&ctx, 0, sizeof(ctx));
    ctx.h = &handoff;
    ctx.trace = trace_sink;
    trace_lines = 0;
}

/* Complete state the GOP (and a restore) owns, for equality checks. */
struct snapshot {
    uint32_t mmio[0x100000 / 4];
    struct pipe_live live[2];
    uint16_t command;
    uint8_t msr, sr01, gr10, gr11;
};
static struct snapshot initial;
static void take(struct snapshot *s)
{
    memcpy(s->mmio, m.mmio, sizeof(m.mmio));
    memcpy(s->live, m.live, sizeof(m.live));
    s->command = m.command;
    s->msr = m.msr;
    s->sr01 = m.seq[1];
    s->gr10 = m.gr[0x10];
    s->gr11 = m.gr[0x11];
}
static bool restored(const struct snapshot *s)
{
    for (unsigned i = 0; i < 0x100000 / 4; ++i) {
        uint32_t offset = i * 4;
        if (offset == IGD_FRMCOUNT(0) || offset == IGD_FRMCOUNT(1))
            continue;
        /* VGACNTRL must read disabled with every other field as before. */
        if (m.mmio[i] != s->mmio[i])
            return false;
    }
    for (unsigned p = 0; p < 2; ++p) {
        const struct pipe_live *a = &m.live[p], *b = &s->live[p];
        if (a->conf != b->conf || a->src != b->src || a->dspcntr != b->dspcntr ||
            a->curcntr != b->curcntr || a->dvscntr != b->dvscntr || a->pf_ctl != b->pf_ctl ||
            a->pf_pos != b->pf_pos || a->pf_sz != b->pf_sz)
            return false;
    }
    return m.command == s->command && m.msr == s->msr && m.seq[1] == s->sr01 &&
           m.gr[0x10] == s->gr10 && m.gr[0x11] == s->gr11;
}
/* VGA text visible on the panel pipe with the expected font and palette. */
static bool text_ready(unsigned p)
{
    const struct pipe_live *l = &m.live[p];
    uint32_t vga = *reg(IGD_VGACNTRL);
    if ((vga & IGD_VGA_DISABLE) || ((vga >> 29) & 1) != p)
        return false;
    if ((m.seq[1] & 0x20) || m.msr != 0x67 || (m.command & 3) != 3)
        return false;
    /* VGA paging off: A0000-AFFFF planar for an OS (mode 12h, bootvid). */
    if (m.gr[0x10] & 1)
        return false;
    if ((l->dspcntr & IGD_ENABLE) || !(l->pf_ctl & IGD_ENABLE) || l->src != IGD_VGA_SOURCE)
        return false;
    if (l->conf & IGD_PIPE_PALETTE)
        return false;
    for (unsigned at = 0; at < 256 * 32; ++at)
        if (m.plane[2][at] != ((at & 31) < 16 ? expected_font[(at >> 5) * 16 + (at & 31)] : 0))
            return false;
    /* Light grey (attr 7 -> DAC 07) and white (attr 15 -> DAC 3f). */
    return *reg(IGD_PALETTE(p) + 0x07 * 4) == 0xa8a8a8u &&
           *reg(IGD_PALETTE(p) + 0x3f * 4) == 0xfcfcfcu;
}

/* Replay writes, 3DA reads (AR flip-flop) and frame boundaries into a fresh
 * model; return its monitor result. */
static unsigned replay(const struct event *events, size_t count, unsigned kind, unsigned panel_pipe)
{
    reset_model(kind, panel_pipe);
    m.recording = false;
    m.frozen = true;
    for (size_t i = 0; i < count; ++i) {
        const struct event *e = &events[i];
        if (e->type == EV_FRAME)
            frame();
        else if (e->type == EV_W)
            model_write(e->address, e->width, e->value);
        else if (e->address == IO + legacy_vga_port_address(0, 0x3da))
            (void)model_read(e->address, 1);
    }
    return m.violations;
}
static struct event *copy_events(size_t *count)
{
    struct event *copy = malloc(m.count * sizeof(*copy));
    CHECK(copy);
    memcpy(copy, m.events, m.count * sizeof(*copy));
    *count = m.count;
    return copy;
}
static size_t find_write(const struct event *e, size_t n, size_t from, uint64_t address,
                         int64_t value)
{
    for (size_t i = from; i < n; ++i)
        if (e[i].type == EV_W && e[i].address == address &&
            (value < 0 || e[i].value == (uint64_t)value))
            return i;
    CHECK(!"write not found");
    return 0;
}
static size_t drop(struct event *e, size_t n, size_t at)
{
    memmove(&e[at], &e[at + 1], (n - at - 1) * sizeof(*e));
    return n - 1;
}
static void move_before(struct event *e, size_t from, size_t to)
{
    struct event saved = e[from];
    CHECK(to < from);
    memmove(&e[to + 1], &e[to], (from - to) * sizeof(*e));
    e[to] = saved;
}

static void test_intel_enter_and_restore(unsigned p)
{
    reset_model(IA64VMM_VGA_INTEL_GEN6, p);
    take(&initial);
    CHECK(legacy_vga_enter_text(&ctx, glyphs));
    CHECK(!m.violations && text_ready(p));
    CHECK(ctx.igd.index == p && traced(p ? "igd pipe B (LVDS)" : "igd pipe A (LVDS)"));
    CHECK(traced("igd check font_bad=0/8192 text_bad=0/4"));
    CHECK(traced(" MSR 00->67=67") && traced(" CMD 0006->0007=0007"));
    /* The other pipe's palette writes are disabled, its palette untouched. */
    uint32_t vga = *reg(IGD_VGACNTRL);
    CHECK((vga & 0xff) == 0x8e && ((vga >> 23) & 1) == p &&
          (vga & (p ? 0x00400000u : 0x00200000u)));
    CHECK((vga & 0x00002900u) == 0x00002900u);
    /* The recorded trace replays clean in a fresh model. */
    size_t count;
    struct event *events = copy_events(&count);
    CHECK(replay(events, count, IA64VMM_VGA_INTEL_GEN6, p) == 0 && text_ready(p));
    free(events);
    reset_model(IA64VMM_VGA_INTEL_GEN6, p);
    CHECK(legacy_vga_enter_text(&ctx, glyphs));
    size_t enter = m.count;
    CHECK(legacy_vga_restore_gop(&ctx));
    CHECK(!m.violations && restored(&initial) && traced(" ok"));
    events = copy_events(&count);
    CHECK(replay(events, count, IA64VMM_VGA_INTEL_GEN6, p) == 0 && restored(&initial));
    (void)enter;
    free(events);
}

/* Every documented step is load-bearing: each mutant fails the monitor or
 * the final-state check. */
static void test_mutants(void)
{
    const unsigned p = 0;
    reset_model(IA64VMM_VGA_INTEL_GEN6, p);
    CHECK(legacy_vga_enter_text(&ctx, glyphs));
    size_t n0, n;
    struct event *base = copy_events(&n0), *e = malloc(n0 * sizeof(*e));
    CHECK(e);
    const uint64_t msr = IO + legacy_vga_port_address(0, 0x3c2),
                   sr_index = IO + legacy_vga_port_address(0, 0x3c4);
    const uint64_t sr_data = IO + legacy_vga_port_address(0, 0x3c5);
#define FRESH() (memcpy(e, base, n0 * sizeof(*e)), n = n0)
    /* M1: no MSR enable before CRTC/font access. */
    FRESH();
    n = drop(e, n, find_write(e, n, 0, msr, 0x67));
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) & V_MSR_ORDER);
    /* M2: primary plane left enabled. */
    FRESH();
    n = drop(e, n, find_write(e, n, 0, BAR0 + IGD_DSPCNTR(p), 0x18004000));
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) & V_PLANES_AT_VGA);
    /* M3: plane disable never armed by DSPSURF. */
    FRESH();
    n = drop(e, n, find_write(e, n, 0, BAR0 + IGD_DSPSURF(p), -1));
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) & V_PLANES_AT_VGA);
    /* M4: VGA enabled before the fitter is programmed. */
    FRESH();
    size_t vga = find_write(e, n, 0, BAR0 + IGD_VGACNTRL, -1);
    move_before(e, vga, find_write(e, n, 0, BAR0 + IGD_PF_CTL(p), -1));
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) & V_PF_AT_VGA);
    /* M5: screen not off at the VGACNTRL enable (drop the SR01 data write
     * that sets bit 5 right before it). */
    FRESH();
    vga = find_write(e, n, 0, BAR0 + IGD_VGACNTRL, -1);
    size_t sr = vga;
    while (!(e[sr].type == EV_W && e[sr].address == sr_data && (e[sr].value & 0x20)))
        --sr;
    n = drop(e, n, sr);
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) & V_SR01_AT_VGA);
    /* M6: screen never switched back on. */
    FRESH();
    for (sr = n; --sr;)
        if (e[sr].type == EV_W && e[sr].address == sr_data && e[sr].value == 0x01)
            break;
    n = drop(e, n, sr);
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) == 0 && !text_ready(p));
    /* M7: wrong pipe source. */
    FRESH();
    e[find_write(e, n, 0, BAR0 + IGD_PIPESRC(p), IGD_VGA_SOURCE)].value = 0x02cf018fu;
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) & V_SRC_AT_VGA);
    /* M8: one corrupted font byte. */
    FRESH();
    for (size_t i = 0; i < n; ++i)
        if (e[i].type == EV_W && e[i].address == 0xa0000 + 33 * 32 + 3) {
            e[i].value ^= 0x10;
            break;
        }
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) == 0 && !text_ready(p));
    /* M9: a stray pipe timing write. */
    FRESH();
    e[find_write(e, n, 0, BAR0 + IGD_PIPESRC(p), -1)].address = BAR0 + IGD_HTOTAL(p);
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) & V_FORBIDDEN);
    /* M10: fitter never armed (PF_WIN_SZ dropped). */
    FRESH();
    n = drop(e, n, find_write(e, n, 0, BAR0 + IGD_PF_WIN_SZ(p), -1));
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) & (V_PF_AT_VGA | V_SRC_NO_PF));
    /* M11: no vblank between plane disable and VGA enable. */
    FRESH();
    size_t from = find_write(e, n, 0, BAR0 + IGD_DSPCNTR(p), -1);
    vga = find_write(e, n, 0, BAR0 + IGD_VGACNTRL, -1);
    for (size_t i = vga; i > from; --i)
        if (e[i].type == EV_FRAME) {
            n = drop(e, n, i);
            --vga;
        }
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) & V_PLANES_AT_VGA);
    /* M12: source changed before the fitter is live. */
    FRESH();
    move_before(e, find_write(e, n, 0, BAR0 + IGD_PIPESRC(p), -1),
                find_write(e, n, 0, BAR0 + IGD_PF_CTL(p), -1));
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) & V_SRC_NO_PF);
    /* M13: wrong pipe select in VGACNTRL. */
    FRESH();
    e[find_write(e, n, 0, BAR0 + IGD_VGACNTRL, -1)].value |= 1u << 29;
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) & V_PIPE_SELECT);
    (void)sr_index;
    free(base);

    /* Restore mutants on the full enter+restore trace. */
    reset_model(IA64VMM_VGA_INTEL_GEN6, p);
    take(&initial);
    CHECK(legacy_vga_enter_text(&ctx, glyphs) && legacy_vga_restore_gop(&ctx));
    base = copy_events(&n0);
    e = realloc(e, n0 * sizeof(*e));
    CHECK(e);
    size_t restore_at = find_write(base, n0, 0, BAR0 + IGD_VGACNTRL, -1) + 1;
    /* R1: primary plane not re-enabled. */
    FRESH();
    n = drop(e, n, find_write(e, n, restore_at, BAR0 + IGD_DSPCNTR(p), 0x98004000));
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) == 0 && !restored(&initial));
    /* R2: VGA plane left enabled. */
    FRESH();
    n = drop(e, n, find_write(e, n, restore_at, BAR0 + IGD_VGACNTRL, -1));
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) == 0 && !restored(&initial));
    /* R3: GOP palette not restored (entry 0x3f). */
    FRESH();
    n = drop(e, n, find_write(e, n, restore_at, BAR0 + IGD_PALETTE(p) + 0x3f * 4, -1));
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) == 0 && !restored(&initial));
    /* R4: fitter left armed on. */
    FRESH();
    n = drop(e, n, find_write(e, n, restore_at, BAR0 + IGD_PF_WIN_SZ(p), -1));
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) == 0 && !restored(&initial));
    /* R5: MSR left at text values. */
    FRESH();
    n = drop(e, n, find_write(e, n, restore_at, msr, 0x00));
    CHECK(replay(e, n, IA64VMM_VGA_INTEL_GEN6, p) == 0 && !restored(&initial));
#undef FRESH
    free(base);
    free(e);
}

/* Precondition and runtime failures keep or restore the GOP state. */
static void test_intel_failures(void)
{
    /* GGC.IVD=1: no write at all. */
    reset_model(IA64VMM_VGA_INTEL_GEN6, 0);
    m.ggc |= 2;
    take(&initial);
    CHECK(!legacy_vga_enter_text(&ctx, glyphs));
    for (size_t i = 0; i < m.count; ++i)
        CHECK(m.events[i].type != EV_W);
    CHECK(restored(&initial) && traced("GGC.IVD=1"));
    /* Undersized BAR0. */
    reset_model(IA64VMM_VGA_INTEL_GEN6, 0);
    handoff.vga_control_size = 0x80000;
    CHECK(!legacy_vga_enter_text(&ctx, glyphs) && traced("GTTMMADR"));
    for (size_t i = 0; i < m.count; ++i)
        CHECK(m.events[i].type != EV_W);
    /* LVDS off and no active pipe: reads only. */
    reset_model(IA64VMM_VGA_INTEL_GEN6, 0);
    *reg(IGD_PCH_LVDS) = 0;
    *reg(IGD_DSPCNTR(0)) = m.live[0].dspcntr = 0;
    take(&initial);
    CHECK(!legacy_vga_enter_text(&ctx, glyphs) && traced("no single panel pipe"));
    for (size_t i = 0; i < m.count; ++i)
        CHECK(m.events[i].type != EV_W);
    CHECK(restored(&initial));
    /* LVDS on but its pipe is not running. */
    reset_model(IA64VMM_VGA_INTEL_GEN6, 0);
    *reg(IGD_PIPECONF(0)) = m.live[0].conf = 0x80000050u;
    take(&initial);
    CHECK(!legacy_vga_enter_text(&ctx, glyphs) && traced("panel pipe not running") &&
          restored(&initial));
    /* Interlaced pipe. */
    reset_model(IA64VMM_VGA_INTEL_GEN6, 0);
    *reg(IGD_PIPECONF(0)) = m.live[0].conf = 0xc0600050u;
    take(&initial);
    CHECK(!legacy_vga_enter_text(&ctx, glyphs) && traced("interlaced") && restored(&initial));
    /* Frame counter stuck: the first vblank wait times out and every step
     * already taken is undone. */
    reset_model(IA64VMM_VGA_INTEL_GEN6, 0);
    m.frozen = true;
    take(&initial);
    CHECK(!legacy_vga_enter_text(&ctx, glyphs) && traced("vbl TIMEOUT") &&
          traced("VBLANK-MISSING"));
    CHECK(*reg(IGD_DSPCNTR(0)) == 0x98004000u && m.command == 0x0006 && m.seq[1] == 0x01);
    m.frozen = false;
    for (unsigned i = 0; i < 3; ++i)
        frame();
    CHECK(restored(&initial));
    /* A0000-BFFFF not decoded by the IGD: the readback fails and the GOP
     * state comes back. */
    reset_model(IA64VMM_VGA_INTEL_GEN6, 0);
    m.vga_dead = true;
    take(&initial);
    CHECK(!legacy_vga_enter_text(&ctx, glyphs) && traced("font_bad=8192/8192") &&
          traced("VGA memory readback"));
    CHECK(!m.violations && restored(&initial));
    /* Cursor, sprite and 10-bit palette mode: disabled/legacy, then back. */
    reset_model(IA64VMM_VGA_INTEL_GEN6, 0);
    *reg(IGD_CURCNTR(0)) = m.live[0].curcntr = 0x04000027u;
    *reg(IGD_CURBASE(0)) = 0x00200000u;
    *reg(IGD_DVSCNTR(0)) = m.live[0].dvscntr = 0x80000000u;
    *reg(IGD_PIPECONF(0)) = m.live[0].conf = 0xc1000050u;
    take(&initial);
    CHECK(legacy_vga_enter_text(&ctx, glyphs) && !m.violations && text_ready(0));
    CHECK(!(m.live[0].curcntr & IGD_CURSOR_MODE) && !(m.live[0].dvscntr & IGD_ENABLE));
    CHECK(legacy_vga_restore_gop(&ctx) && !m.violations && restored(&initial));
}

/* The real-hardware probe: page, alias check, countdown, restore. */
static void test_probe(void)
{
    for (unsigned alias = 0; alias < 2; ++alias) {
        reset_model(IA64VMM_VGA_INTEL_GEN6, 0);
        m.alias = alias;
        m.fb0 = 0x11223344u;
        handoff.vga_probe_sec = 2;
        take(&initial);
        uint64_t start = m.now;
        CHECK(legacy_vga_probe(&ctx, glyphs));
        CHECK(m.now - start >= 2 * IA64VMM_ITC_HZ);
        CHECK(!m.violations && restored(&initial) && m.fb0 == 0x11223344u);
        CHECK(traced("probe hold=2s kind=3 legacy_vga=1") && traced("probe done: GOP restored"));
        CHECK(traced(alias ? "(ALIAS: fb+0 is VGA memory)" : "(no alias at fb+0)"));
        /* The page and countdown went to the text planes (char/attr). */
        CHECK(m.plane[0][1 * 2] == 'I' && m.plane[1][1 * 2] == 0x1f);
        CHECK(m.plane[0][(24 * 80 + 13) * 2] == '1' && m.plane[1][24 * 80 * 2] == 0x70);
        /* Glyph 'A' (65): row 7 + 65/32, column 8 + (65%32)*2, attr 07. */
        CHECK(m.plane[0][(9 * 80 + 10) * 2] == 'A' && m.plane[1][(9 * 80 + 10) * 2] == 0x07);
    }
    /* Kinds without a documented restore are skipped untouched. */
    reset_model(IA64VMM_VGA_VMWARE, 0);
    handoff.vga_probe_sec = 2;
    CHECK(legacy_vga_probe(&ctx, glyphs) && traced("probe skipped"));
    for (size_t i = 0; i < m.count; ++i)
        CHECK(m.events[i].type != EV_W);
    /* Bochs: enter, hold, then VBE ENABLE|LFB|NOCLEARMEM. */
    reset_model(IA64VMM_VGA_BOCHS, 0);
    handoff.vga_probe_sec = 1;
    m.msr = 0x67;
    CHECK(legacy_vga_probe(&ctx, glyphs));
    CHECK(m.count && m.events[m.count - 1].type == EV_W &&
          m.events[m.count - 1].address == BAR2 + 0x508 && m.events[m.count - 1].value == 0xc1);
}

/* Bochs and VMware: exactly the accesses without the IGD sequence (command RMW, the
 * kind's leave writes, then the unchanged shared font and text programming). */
static void expect_prefix(const struct event *want, size_t count)
{
    CHECK(m.count >= count);
    for (size_t i = 0; i < count; ++i)
        CHECK(m.events[i].type == want[i].type && m.events[i].width == want[i].width &&
              m.events[i].address == want[i].address &&
              (want[i].type == EV_R || m.events[i].value == want[i].value));
}
static void test_legacy_kinds(void)
{
    static const unsigned kinds[3] = {IA64VMM_VGA_BOCHS, IA64VMM_VGA_BOCHS, IA64VMM_VGA_VMWARE};
    for (unsigned k = 0; k < 3; ++k) {
        reset_model(kinds[k], 0);
        m.msr = 0x67;
        if (k == 1)
            handoff.vga_control_size = 0; /* no BAR2: real 1ce/1cf */
        CHECK(legacy_vga_enter_text(&ctx, glyphs));
        /* No Intel step and no trace line for these kinds. */
        CHECK(!trace_lines);
        size_t total = m.count;
        struct event *got = copy_events(&total);
        const uint64_t cmd = ECAM + (IGD_BDF << 12) + 4, sel = SERVICE + IA64VMM_SVC_PORT_SELECT,
                       data = SERVICE + IA64VMM_SVC_PORT_DATA;
        struct event prefix[16] = {{EV_R, 2, cmd, 0}, {EV_W, 2, cmd, 0x0007}};
        size_t count = 2;
        if (k == 0)
            prefix[count++] = (struct event){EV_W, 2, BAR2 + 0x508, 0};
        if (k == 1) {
            prefix[count++] = (struct event){EV_W, 8, sel, 0x1ce | 2u << 16};
            prefix[count++] = (struct event){EV_W, 8, data, 4};
            prefix[count++] = (struct event){EV_W, 8, sel, 0x1cf | 2u << 16};
            prefix[count++] = (struct event){EV_W, 8, data, 0};
        }
        if (k == 2) {
            const uint64_t v[4][2] = {{0x2140, 1}, {0x2141, 3}, {0x2140, 45}, {0x2141, 1}};
            for (unsigned i = 0; i < 4; ++i) {
                prefix[count++] = (struct event){EV_W, 8, sel, v[i][0] | 4u << 16};
                prefix[count++] = (struct event){EV_W, 8, data, v[i][1]};
            }
        }
        expect_prefix(prefix, count);
        /* The rest is the shared font+text programming, nothing more. */
        reset_model(kinds[k], 0);
        m.msr = 0x67;
        legacy_vga_load_text_font(IO, (volatile uint8_t *)(uintptr_t)0xa0000, glyphs);
        legacy_vga_program_text_mode(IO);
        CHECK(total == count + m.count);
        for (size_t i = 0; i < m.count; ++i)
            CHECK(got[count + i].type == m.events[i].type &&
                  got[count + i].address == m.events[i].address &&
                  got[count + i].value == m.events[i].value &&
                  got[count + i].width == m.events[i].width);
        for (size_t i = 0; i < total; ++i)
            CHECK(got[i].address < BAR0 || got[i].address >= BAR0 + 0x400000);
        free(got);
    }
}

/* A gen6 GOP fixture with a 640x480 source, panel fitter and GR10 paging.
 * Paging exposes an interleaved view instead of VGA planar addressing.
 * The font check must distinguish those views; clearing GR10/GR11 enables
 * planar writes and the probe must restore the saved paging state. */
static void gen6_paged_state(void)
{
    reset_model(IA64VMM_VGA_INTEL_GEN6, 0);
    m.msr = 0x67;
    m.command = 0x0007;
    *reg(IGD_PIPESRC(0)) = m.live[0].src = 0x027f01dfu;
    *reg(IGD_PF_CTL(0)) = m.live[0].pf_ctl = 0x80800000u;
    *reg(IGD_PF_WIN_SZ(0)) = m.live[0].pf_sz = 0x05560300u;
    *reg(IGD_DSPCNTR(0)) = m.live[0].dspcntr = 0x98000000u;
    *reg(IGD_DSPSURF(0)) = 0;
}
static void test_gen6_paged(void)
{
    gen6_paged_state();
    /* The pre-fix order: font load and text programming with paging left
     * on, then the old check configuration (GR04=2, GR05=0, GR06=04). */
    legacy_vga_load_text_font(IO, (volatile uint8_t *)(uintptr_t)0xa0000, vgafont16);
    legacy_vga_program_text_mode(IO);
    legacy_vga_indexed_write(IO, 0x3ce, 0x3cf, 0x04, 0x02);
    legacy_vga_indexed_write(IO, 0x3ce, 0x3cf, 0x05, 0x00);
    legacy_vga_indexed_write(IO, 0x3ce, 0x3cf, 0x06, 0x04);
    unsigned bad = 0, nonzero = 0;
    for (unsigned at = 0; at < 8192; ++at) {
        uint8_t want = (at & 31) < 16 ? vgafont16[(at >> 5) * 16 + (at & 31)] : 0;
        nonzero += want != 0;
        if (legacy_vga_rd8(0xa0000 + at) != want)
            ++bad;
    }
    CHECK(nonzero == 2322 && bad == 2650);
    /* The font itself is intact in plane 2: only the paged read is raw. */
    for (unsigned at = 0; at < 8192; ++at)
        CHECK(m.plane[2][at] == ((at & 31) < 16 ? vgafont16[(at >> 5) * 16 + (at & 31)] : 0));
    /* The fixed sequence on the same state: paging off, check clean,
     * VGA text up, and the restore puts GR10 back. */
    gen6_paged_state();
    take(&initial);
    expected_font = vgafont16;
    CHECK(legacy_vga_enter_text(&ctx, vgafont16) && !m.violations && text_ready(0));
    CHECK(traced(" GR10 01->00=00 GR11 00->00=00") && traced("font_bad=0/8192 text_bad=0/4"));
    CHECK(traced(" MSR 67->67=67") && traced("PIPESRC 027f01df->027f018f=027f018f"));
    CHECK(legacy_vga_restore_gop(&ctx) && restored(&initial) && traced(" GR10 00->01=01"));
    /* The probe on gen6 IGD state: the planes behind the paged view, text
     * through B8000, the three glyphs, all within the 30-line summary. */
    gen6_paged_state();
    handoff.vga_probe_sec = 1;
    take(&initial);
    CHECK(legacy_vga_probe(&ctx, vgafont16) && !m.violations && restored(&initial));
    CHECK(traced(
        "diag planar w/r 0000:10203040/10203040 1000:11213141/11213141 "
        "8000:12223242/12223242 ffff:13233343/13233343 ok paged@4000=11213141 =interleaved"));
    CHECK(traced("diag text 0000:1e41>1e41 p0=4100 p1=1e00 1000:1f42>1f42 p0=4200 p1=1f00 "
                 "7ffe:2043>2043 p0=4300 p1=2000 ok"));
    char font_line[160];
    int used = snprintf(font_line, sizeof(font_line), "vga: diag font");
    static const uint8_t probed[3] = {0x00, 0x41, 0xdb};
    for (unsigned k = 0; k < 3; ++k) {
        used += snprintf(font_line + used, sizeof(font_line) - (size_t)used, " %02x:", probed[k]);
        for (unsigned r = 0; r < 16; ++r)
            used += snprintf(font_line + used, sizeof(font_line) - (size_t)used, "%02x",
                             vgafont16[probed[k] * 16 + r]);
        used += snprintf(font_line + used, sizeof(font_line) - (size_t)used, "=");
    }
    bool font_seen = false;
    for (unsigned i = 0; i < trace_lines; ++i)
        font_seen = font_seen || !strcmp(trace_text[i], font_line);
    CHECK(font_seen && strstr(font_line, " 41:0000103") && strstr(font_line, " db:ffffffff"));
    CHECK(ctx.igd.diag_planar == 1 && ctx.igd.diag_paged == 1 && ctx.igd.diag_text == 1 &&
          ctx.igd.diag_font == 1);
    /* The summary keeps 30 lines of 160 characters after "[guest] ". */
    CHECK(trace_lines <= 30);
    for (unsigned i = 0; i < trace_lines; ++i)
        CHECK(strlen(trace_text[i]) <= LOG_MATCH_COLUMNS_FOR_TEST);
    if (getenv("VGA_IGD_SHOW"))
        for (unsigned i = 0; i < trace_lines; ++i)
            puts(trace_text[i]);
    /* Every probed byte came back: font plane and text cells as before. */
    for (unsigned at = 0; at < 8192; ++at)
        CHECK(m.plane[2][at] == ((at & 31) < 16 ? vgafont16[(at >> 5) * 16 + (at & 31)] : 0));
    /* The diagnostic row reflects the synthetic plane checks. */
    char row[81] = {0};
    for (unsigned c = 0; c < 80; ++c)
        row[c] = (char)m.plane[0][(19 * 80 + c) * 2];
    CHECK(!strncmp(row, "GOP GR10=01 GR11=00 paged=ok planar=ok text=ok font=ok", 54));
    /* A broken plane-2 write path is visible in the diagnostics. */
    gen6_paged_state();
    handoff.vga_probe_sec = 1;
    m.vga_dead = true;
    CHECK(legacy_vga_probe(&ctx, vgafont16) && traced(" BAD") && traced("!") &&
          ctx.igd.diag_planar == 2);
    CHECK(traced("probe: VGA text not entered") && restored(&initial));
    expected_font = glyphs;
}

int main(void)
{
    /* Never 0xff, so an undecoded aperture (reads 0xff) mismatches every byte. */
    for (unsigned i = 0; i < sizeof(glyphs); ++i)
        glyphs[i] = (uint8_t)((i * 37u + 11u) % 255u);
    test_intel_enter_and_restore(0);
    test_intel_enter_and_restore(1);
    test_mutants();
    test_intel_failures();
    test_probe();
    test_gen6_paged();
    test_legacy_kinds();
    free(m.events);
    puts(
        "vga-igd: gen6 sequence, monitor, 18 mutants, failure paths, probe and Bochs/VMware traces passed");
    return 0;
}
