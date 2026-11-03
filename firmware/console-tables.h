/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Console table contributions for the native firmware. PCDP layout and VGA translation descriptor
 * fields are ported from firmware/firmware.c (reference fork origin).
 * ACPI 6.5 SPCR and PCDP 1.0; the debug UART has no interrupt. */
#ifndef IA64_FIRMWARE_CONSOLE_TABLES_H
#define IA64_FIRMWARE_CONSOLE_TABLES_H
#include "ia64vmm/platform.h"
#include "uart-aml.h"
struct console_tables {
    uint8_t hcdp[241], spcr[80], uart[132];
    size_t hcdp_size, spcr_size, uart_size;
};
static inline void console_table_store(uint8_t *p, uint64_t value, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        p[i] = (uint8_t)(value >> (i * 8));
}
static inline void console_table_checksum(uint8_t *table, size_t n)
{
    unsigned sum = 0;
    table[9] = 0;
    for (size_t i = 0; i < n; ++i)
        sum += table[i];
    table[9] = (uint8_t)(0 - sum);
}
static inline void console_table_header(uint8_t *p, const char *signature, size_t n,
                                        uint8_t revision)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = signature[i];
    console_table_store(p + 4, n, 4);
    p[8] = revision;
    for (unsigned i = 0; i < 6; ++i)
        p[10 + i] = "IA64VM"[i];
    for (unsigned i = 0; i < 8; ++i)
        p[16 + i] = "CONSOLE "[i];
    console_table_store(p + 24, 1, 4);
    console_table_store(p + 28, 0x4d4d5649, 4);
    console_table_store(p + 32, 1, 4);
}
static inline void console_tables_build(struct console_tables *t, const struct ia64vmm_handoff *h)
{
    uint8_t *bytes = (uint8_t *)t;
    for (size_t i = 0; i < sizeof(*t); ++i)
        bytes[i] = 0;
    unsigned console = !!(h->flags & IA64VMM_HF_UART_CONSOLE);
    unsigned vga = !!(h->flags & IA64VMM_HF_LEGACY_VGA);
    if (h->flags & IA64VMM_HF_UART_PRESENT) {
        size_t n = uart_aml_build(t->uart + 36, sizeof(t->uart) - 36, h->uart_base);
        t->uart_size = 36 + n;
        console_table_header(t->uart, "SSDT", t->uart_size, 2);
        console_table_checksum(t->uart, t->uart_size);
    }
    if (!console && !vga)
        return;
    t->hcdp_size = 40 + console * 48 + vga * 153;
    console_table_header(t->hcdp, "HCDP", t->hcdp_size, 3);
    console_table_store(t->hcdp + 36, console, 4);
    if (console) {
        uint8_t *u = t->hcdp + 40;
        u[1] = 8;
        u[2] = 1;
        u[3] = 1;
        console_table_store(u + 8, 115200, 8);
        u[17] = 8;
        console_table_store(u + 20, h->uart_base, 8);
        console_table_store(u + 28, 0x0105d041, 4);
        console_table_store(u + 36, 1843200, 4);
        u[40] = 2;
        u[41] = 4; /* Primary, polling, ACPI PNP0501. */
        t->spcr_size = sizeof(t->spcr);
        console_table_header(t->spcr, "SPCR", t->spcr_size, 1);
        t->spcr[41] = 8;
        console_table_store(t->spcr + 44, h->uart_base, 8);
        t->spcr[58] = 7;
        t->spcr[60] = 1;
        console_table_store(t->spcr + 64, 0xffffffff, 4);
        console_table_checksum(t->spcr, t->spcr_size);
    }
    if (vga) {
        uint8_t *d = t->hcdp + 40 + console * 48;
        d[0] = 10;
        d[1] = !console;
        console_table_store(d + 2, 153, 2);
        /* The service text console precedes GraphicsConsole in ConOut. */
        console_table_store(d + 4, 1, 2);
        d[6] = 1;
        console_table_store(d + 8, 34, 2);
        d[11] = h->vga_bdf >> 8;
        d[12] = (h->vga_bdf >> 3) & 31;
        d[13] = h->vga_bdf & 7;
        console_table_store(d + 14, h->vga_device, 2);
        console_table_store(d + 16, h->vga_vendor, 2);
        console_table_store(d + 30, h->legacy_io_base, 8);
        d[39] = 2;
        d[40] = 2;
        for (unsigned i = 0; i < 2; ++i) {
            uint8_t *a = d + 41 + i * 56;
            a[0] = 0x8b;
            console_table_store(a + 1, 53, 2);
            a[3] = i;
            a[4] = 0x0d;
            a[5] = i ? 3 : 1;
            a[6] = 1;
            uint64_t base = i ? 0x3b0 : h->vga_base, size = i ? 0x30 : h->vga_size;
            console_table_store(a + 16, base, 8);
            console_table_store(a + 24, base + size - 1, 8);
            console_table_store(a + 40, size, 8);
            if (!i)
                console_table_store(a + 48, 1, 8);
        }
    }
    console_table_checksum(t->hcdp, t->hcdp_size);
}
#endif
