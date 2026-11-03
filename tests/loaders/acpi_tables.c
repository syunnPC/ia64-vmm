/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Check the complete ACPI image, including VGA combinations absent on hosted
 * boots. Expected resource fields come from the handoff and PCDP layout. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tests/firmware/acpi-map.h"
static struct acpi_image acpi;
static uint8_t facs[64];
int main(void)
{
    struct ia64vmm_handoff h = {
        .cpu_count = 1,
        .ram_range_count = 1,
        .ram = {{0, 0x20000000}},
        .ecam_base = IA64VMM_ECAM_BASE,
        .ecam_size = IA64VMM_ECAM_SIZE,
        .ecam_bus_last = 255,
        .iosapic_base = IA64VMM_IOSAPIC_BASE,
        .iosapic_gsi_count = IA64VMM_IOSAPIC_GSI_COUNT,
        .pib_base = IA64VMM_PIB_BASE,
        .acpi_pm_port_base = IA64VMM_ACPI_PM_PORT_BASE,
        .acpi_pm_port_size = IA64VMM_ACPI_PM_PORT_SIZE,
        .uart_base = IA64VMM_UART_BASE,
        .legacy_io_base = IA64VMM_LEGACY_IO_BASE,
        .vga_base = IA64VMM_VGA_MEMORY_BASE,
        .vga_size = IA64VMM_VGA_MEMORY_SIZE,
        .vga_bdf = 0x1235,
        .vga_vendor = 0x15ad,
        .vga_device = 0x0405,
    };
    const uint64_t base = 0x100000;
    for (unsigned mode = 0; mode < 3; ++mode)
        for (unsigned vga = 0; vga < 2; ++vga) {
            unsigned console = mode == 2;
            h.flags = (mode ? IA64VMM_HF_UART_PRESENT : 0) |
                      (console ? IA64VMM_HF_UART_CONSOLE : 0) | (vga ? IA64VMM_HF_LEGACY_VGA : 0);
            assert(test_acpi_build(&h, &acpi, base, facs, 0x200000));
            assert((acpi.uart != SIZE_MAX) == !!mode);
            assert((acpi.spcr != SIZE_MAX) == !!console);
            assert((acpi.hcdp != SIZE_MAX) == !!(console || vga));
            size_t length = (size_t)table_get(acpi.bytes + acpi.xsdt + 4, 4);
            assert(length == 36 + 8 * (5 + !!mode + console + !!(console || vga)));
            assert(!table_sum(acpi.bytes + acpi.xsdt, length));
            unsigned uart = 0, spcr = 0, pcdp = 0;
            for (size_t at = 36; at < length; at += 8) {
                uint64_t address = table_get(acpi.bytes + acpi.xsdt + at, 8);
                assert(address >= base && address - base < acpi.used);
                const uint8_t *p = acpi.bytes + address - base;
                size_t n = (size_t)table_get(p + 4, 4);
                assert(n >= 36 && n <= acpi.used - (address - base) && !table_sum(p, n));
                uart += !memcmp(p, "SSDT", 4);
                spcr += !memcmp(p, "SPCR", 4);
                pcdp += !memcmp(p, "HCDP", 4);
            }
            assert(uart == !!mode && spcr == console && pcdp == !!(console || vga));
            if (mode) {
                const uint8_t *p = acpi.bytes + acpi.uart;
                assert(table_get(p + 36 + 62, 8) == h.uart_base);
                assert(table_get(p + 36 + 70, 8) == h.uart_base + 7);
                assert(table_get(p + 36 + 86, 8) == 8);
            }
            if (console) {
                const uint8_t *p = acpi.bytes + acpi.spcr;
                assert(p[40] == 0 && p[41] == 8 && table_get(p + 44, 8) == h.uart_base);
                assert(p[52] == 0 && p[53] == 0 && table_get(p + 54, 4) == 0);
                assert(p[58] == 7 && p[60] == 1);
            }
            if (console || vga) {
                const uint8_t *p = acpi.bytes + acpi.hcdp;
                assert(table_get(p + 36, 4) == console);
                assert(table_get(p + 4, 4) == 40 + console * 48 + vga * 153);
                if (console)
                    assert(p[56] == 0 && table_get(p + 60, 8) == h.uart_base && p[81] == 4);
                if (vga) {
                    const uint8_t *d = p + 40 + console * 48;
                    assert(d[0] == 10 && d[1] == !console && table_get(d + 2, 2) == 153);
                    assert(table_get(d + 4, 2) == 1);
                    assert(d[11] == 0x12 && d[12] == 6 && d[13] == 5);
                    assert(table_get(d + 14, 2) == h.vga_device &&
                           table_get(d + 16, 2) == h.vga_vendor);
                    assert(table_get(d + 22, 8) == 0 && table_get(d + 30, 8) == h.legacy_io_base);
                    assert(d[39] == 2 && d[40] == 2);
                    assert(table_get(d + 41 + 16, 8) == h.vga_base);
                    assert(table_get(d + 41 + 24, 8) == h.vga_base + h.vga_size - 1);
                    assert(table_get(d + 41 + 40, 8) == h.vga_size &&
                           table_get(d + 41 + 48, 8) == 1);
                    assert(table_get(d + 97 + 16, 8) == 0x3b0 && table_get(d + 97 + 40, 8) == 0x30);
                }
            }
        }
    puts("ACPI table builder: six UART/VGA combinations, checksums and assigned resources passed");
    return 0;
}
