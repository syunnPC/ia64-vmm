/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Base.h>
#include <IndustryStandard/Acpi20.h>
#include "tests/firmware/acpi-map.h"
#include "firmware/new/smbios/smbios.h"

static struct acpi_image acpi;
static struct smbios_image smb;
static uint8_t facs[64];
static void dump(const char *directory, const char *name, const void *p, size_t size)
{
    char path[1024];
    assert(snprintf(path, sizeof(path), "%s/%s.dat", directory, name) > 0);
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(p, 1, size, f) == size && fclose(f) == 0);
}
/* PCI0._CRS QWord memory descriptors of the DSDT at bytes[0] covering [min, max]. */
static unsigned dsdt_windows(const uint8_t *bytes, uint64_t min, uint64_t max)
{
    unsigned found = 0;
    size_t length = (size_t)table_get(bytes + 4, 4);
    for (size_t at = 36; at + 46 <= length; ++at)
        found += bytes[at] == 0x8a && bytes[at + 1] == 43 && bytes[at + 2] == 0 &&
                 bytes[at + 3] == 0 && table_get(bytes + at + 14, 8) == min &&
                 table_get(bytes + at + 22, 8) == max &&
                 table_get(bytes + at + 38, 8) == max - min + 1;
    return found;
}
static bool dsdt_name(const uint8_t *bytes, const char name[4])
{
    size_t length = (size_t)table_get(bytes + 4, 4);
    for (size_t i = 36; i + 4 <= length; ++i)
        if (!memcmp(bytes + i, name, 4))
            return true;
    return false;
}

int main(int argc, char **argv)
{
    struct ia64vmm_handoff h = {
        .cpu_count = 4,
        .ram_range_count = 2,
        .ram = {{0, 0x10000000}, {UINT64_C(0x100000000), 0x20000000}},
        .firmware_size = 0x800000,
        .ecam_base = IA64VMM_ECAM_BASE,
        .ecam_size = IA64VMM_ECAM_SIZE,
        .ecam_bus_first = 32,
        .ecam_bus_last = 47,
        .iosapic_base = IA64VMM_IOSAPIC_BASE,
        .iosapic_gsi_base = 16,
        .iosapic_gsi_count = 24,
        .pib_base = IA64VMM_PIB_BASE,
        .acpi_pm_port_base = IA64VMM_ACPI_PM_PORT_BASE,
        .acpi_pm_port_size = IA64VMM_ACPI_PM_PORT_SIZE,
        .pci_mmio_base = 0x80000000,
        .pci_mmio_size = 0x10000000,
        .pci_mmio64_base = UINT64_C(0x120000000),
        .pci_mmio64_size = UINT64_C(0x200000000),
        .pci_io_base = 0x3000,
        .pci_io_size = 0x1000,
        .legacy_io_base = IA64VMM_LEGACY_IO_BASE,
        .legacy_io_size = IA64VMM_LEGACY_IO_SIZE,
    };
    const uint64_t base = UINT64_C(0x180000000), facs_base = UINT64_C(0x190000000);
    assert(test_acpi_build(&h, &acpi, base, facs, facs_base));
    assert(table_get(acpi.bytes + acpi.rsdp + 24, 8) == base + acpi.xsdt);
    assert(table_sum(acpi.bytes + acpi.rsdp, 20) == 0 &&
           table_sum(acpi.bytes + acpi.rsdp, 36) == 0);
    size_t offsets[] = {acpi.fadt, acpi.madt, acpi.mcfg, acpi.srat, acpi.slit};
    const char *names[] = {"FACP", "APIC", "MCFG", "SRAT", "SLIT"};
    for (unsigned i = 0; i < 5; ++i) {
        const uint8_t *p = acpi.bytes + offsets[i];
        assert(table_get(acpi.bytes + acpi.xsdt + 36 + i * 8, 8) == base + offsets[i]);
        size_t length = (size_t)table_get(p + 4, 4);
        assert(length >= 36 && length <= acpi.used - offsets[i] && table_sum(p, length) == 0);
        if (argc == 2)
            dump(argv[1], names[i], p, length);
    }
    const uint8_t *f = acpi.bytes + acpi.fadt;
    assert(table_get(f + 4, 4) == sizeof(EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE));
    assert(table_get(f + OFFSET_OF(EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE, XFirmwareCtrl), 8) ==
           facs_base);
    assert(table_get(f + OFFSET_OF(EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE, XDsdt), 8) == base);
    assert(table_get(f + OFFSET_OF(EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE, FirmwareCtrl), 4) ==
           0);
    assert(table_get(f + OFFSET_OF(EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE, Dsdt), 4) == 0);
    assert(table_get(f + OFFSET_OF(EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE, Pm1aCntBlk), 4) ==
           0x2004);
    assert(f[OFFSET_OF(EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE, XPm1aEvtBlk)] == 1);
    assert(f[OFFSET_OF(EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE, XPm1aEvtBlk) + 3] == 2);
    assert(f[8] == EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_REVISION);
    const uint8_t *m = acpi.bytes + acpi.madt;
    assert(m[8] == EFI_ACPI_2_0_MULTIPLE_APIC_DESCRIPTION_TABLE_REVISION);
    size_t at = sizeof(EFI_ACPI_2_0_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER), cpus = 0;
    while (at < table_get(m + 4, 4)) {
        const uint8_t *p = m + at;
        assert(p[1] && p[1] <= table_get(m + 4, 4) - at);
        if (p[0] == 7) {
            /* ACPI 2.0 errata layout: 12 bytes, the only length the 2003 HAL accepts. */
            assert(p[1] == sizeof(EFI_ACPI_2_0_PROCESSOR_LOCAL_SAPIC_STRUCTURE) && p[1] == 12);
            assert(p[2] == cpus && p[3] == cpus && table_get(p + 8, 4) == 1);
            ++cpus;
        } else if (p[0] == 5)
            assert(table_get(p + 4, 8) == h.pib_base);
        else {
            assert(p[0] == 6);
            assert(table_get(p + 8, 8) == h.iosapic_base);
        }
        at += p[1];
    }
    assert(cpus == 4);
    assert(table_get(acpi.bytes + acpi.mcfg + 44, 8) == h.ecam_base);
    assert(acpi.bytes[acpi.mcfg + 54] == 32 && acpi.bytes[acpi.mcfg + 55] == 47);
    assert(table_get(acpi.bytes + acpi.srat + 48 + 4 * 16 + 40 + 8, 8) == h.ram[1].base);
    assert(table_get(acpi.bytes + acpi.slit + 36, 8) == 1 && acpi.bytes[acpi.slit + 44] == 10);
    assert(table_get(facs + 4, 4) == 64);
    assert(table_sum(acpi.bytes, (size_t)table_get(acpi.bytes + 4, 4)) == 0);
    assert(table_sum(acpi.bytes + acpi.xsdt, 76) == 0);
    if (argc == 2) {
        dump(argv[1], "DSDT", acpi.bytes, (size_t)table_get(acpi.bytes + 4, 4));
        dump(argv[1], "XSDT", acpi.bytes + acpi.xsdt, 76);
        dump(argv[1], "RSDP", acpi.bytes + acpi.rsdp, 36);
        dump(argv[1], "FACS", facs, 64);
        static const unsigned counts[] = {1, 2, 4, 64, 256};
        struct ia64vmm_handoff varied = h;
        for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
            char name[32];
            varied.cpu_count = counts[i];
            assert(test_acpi_build(&varied, &acpi, base, facs, facs_base));
            snprintf(name, sizeof(name), "DSDT-%u", counts[i]);
            dump(argv[1], name, acpi.bytes, (size_t)table_get(acpi.bytes + 4, 4));
        }
        varied.cpu_count = 4;
        varied.irq_version = IA64VMM_HANDOFF_IRQ_VERSION;
        varied.irq_route_count = 2;
        varied.irq_routes[0] = (struct ia64vmm_irq_route){.bdf = 0x2008, .pin = 0, .gsi = 16};
        varied.irq_routes[1] = (struct ia64vmm_irq_route){.bdf = 0x2110, .pin = 3, .gsi = 39};
        varied.irq_bridge_count = 1;
        varied.irq_bridges[0] = (struct ia64vmm_irq_bridge){.bdf = 0x2018, .secondary = 33};
        varied.flags |= IA64VMM_HF_UART_PRESENT;
        varied.uart_base = IA64VMM_UART_BASE;
        assert(test_acpi_build(&varied, &acpi, base, facs, facs_base));
        dump(argv[1], "DSDT-bridges", acpi.bytes, (size_t)table_get(acpi.bytes + 4, 4));
        assert(acpi.uart != SIZE_MAX);
        dump(argv[1], "SSDT-uart", acpi.bytes + acpi.uart,
             (size_t)table_get(acpi.bytes + acpi.uart + 4, 4));
        varied.cpu_count = 256;
        varied.irq_bridge_count = 0;
        varied.irq_route_count = 255;
        for (unsigned i = 0; i < varied.irq_route_count; ++i)
            varied.irq_routes[i] = (struct ia64vmm_irq_route){
                .bdf = (uint16_t)(0x2000 | i), .pin = (uint8_t)(i % 4), .gsi = 16 + i % 24};
        assert(test_acpi_build(&varied, &acpi, base, facs, facs_base));
        dump(argv[1], "DSDT-large", acpi.bytes, (size_t)table_get(acpi.bytes + 4, 4));
    }
    /* The root decodes A0000-FFFFF for an assigned legacy VGA only. */
    assert(!dsdt_windows(acpi.bytes, 0xa0000, 0xfffff) &&
           dsdt_windows(acpi.bytes, 0x80000000, 0x8fffffff) == 1);
    h.flags |= IA64VMM_HF_LEGACY_VGA;
    assert(test_acpi_build(&h, &acpi, base, facs, facs_base));
    if (argc == 2)
        dump(argv[1], "DSDT-vga", acpi.bytes, (size_t)table_get(acpi.bytes + 4, 4));
    assert(dsdt_windows(acpi.bytes, 0xa0000, 0xfffff) == 1 &&
           dsdt_windows(acpi.bytes, 0x80000000, 0x8fffffff) == 1);
    assert(table_sum(acpi.bytes, (size_t)table_get(acpi.bytes + 4, 4)) == 0);
    h.flags &= ~IA64VMM_HF_LEGACY_VGA;
    assert(!dsdt_name(acpi.bytes, "PS2K") && !dsdt_name(acpi.bytes, "PS2M"));
    h.flags |= IA64VMM_HF_I8042_ENABLED | IA64VMM_HF_I8042_KEYBOARD_ONLY;
    assert(test_acpi_build(&h, &acpi, base, facs, facs_base));
    assert(dsdt_name(acpi.bytes, "PS2K") && !dsdt_name(acpi.bytes, "PS2M"));
    h.flags &= ~IA64VMM_HF_I8042_KEYBOARD_ONLY;
    assert(test_acpi_build(&h, &acpi, base, facs, facs_base));
    assert(dsdt_name(acpi.bytes, "PS2K") && dsdt_name(acpi.bytes, "PS2M"));
    h.flags &= ~IA64VMM_HF_I8042_ENABLED;
    assert(test_acpi_build(&h, &acpi, base, facs, facs_base));
    h.size = sizeof(h);
    ia64vmm_handoff_cpu_model(&h, IA64_CPU_MONTECITO);
    assert(smbios_build(&h, &smb, 0x100000));
    assert(table_sum(smb.entry, 31) == 0 && table_sum(smb.entry + 16, 15) == 0);
    at = cpus = 0;
    unsigned count = 0;
    while (at < smb.used) {
        const uint8_t *p = smb.bytes + at;
        assert(p[1] >= 4 && p[1] <= smb.used - at);
        assert(table_get(p + 2, 2) == count++);
        if (p[0] == 4) {
            assert(p[6] == 0xb8 && p[24] == 0x41 && table_get(p + 22, 2) == 1600);
            ++cpus;
        }
        at += p[1];
        while (at + 1 < smb.used && (smb.bytes[at] || smb.bytes[at + 1]))
            ++at;
        assert(at + 1 < smb.used);
        at += 2;
    }
    assert(at == smb.used && count == smb.count && cpus == (h.cpu_count + 1) / 2);
    static const uint64_t processor_id[] = {UINT64_C(0x0000000520000704),
                                            UINT64_C(0x000000011f010504)};
    static const char *const cpu_brand[] = {"VMM Montecito-compatible IA-64 CPU 1.60GHz 12MB",
                                            "VMM Madison-compatible IA-64 CPU 1.60GHz 3MB"};
    for (unsigned model = 0; model < 2; ++model) {
        ia64vmm_handoff_cpu_model(&h, model);
        assert(smbios_build(&h, &smb, 0x100000));
        for (size_t offset = 0; offset < smb.used;) {
            const uint8_t *p = smb.bytes + offset;
            if (p[0] == 4) {
                assert(table_get(p + 8, 8) == processor_id[model]);
                const char *brand = (const char *)p + p[1];
                for (unsigned string = 1; string < p[16]; ++string)
                    while (*brand++) {
                    }
                assert(!strcmp(brand, cpu_brand[model]));
                unsigned cores = model ? 1 : 2;
                assert(p[35] == cores && p[36] == cores && p[37] == cores);
            }
            offset += p[1];
            while (smb.bytes[offset] || smb.bytes[offset + 1])
                ++offset;
            offset += 2;
        }
    }
    h.cpu_report.version++;
    assert(!smbios_build(&h, &smb, 0x100000));
    ia64vmm_handoff_cpu_model(&h, IA64_CPU_MONTECITO);
    assert(!smbios_build(&h, &smb, UINT64_C(0x100000000)));
    h.cpu_count = TABLE_MAX_CPUS;
    assert(test_acpi_build(&h, &acpi, base, facs, facs_base));
    h.size = sizeof(h);
    ia64vmm_handoff_cpu_model(&h, IA64_CPU_MONTECITO);
    assert(smbios_build(&h, &smb, 0x100000));
    h.cpu_count++;
    assert(!test_acpi_build(&h, &acpi, base, facs, facs_base));
    h.cpu_count = 1;
    h.pci_mmio64_base = UINT64_MAX - 7;
    assert(!test_acpi_build(&h, &acpi, base, facs, facs_base));
    h.pci_mmio64_size = h.pci_mmio_size = h.pci_io_size = 0;
    assert(test_acpi_build(&h, &acpi, base, facs, facs_base));
    h.ecam_size = 16u << 20; /* A bus-zero window cannot reach bus 47. */
    assert(!test_acpi_build(&h, &acpi, base, facs, facs_base));
    h.ecam_size = IA64VMM_ECAM_SIZE;
    size_t length;
    uint8_t short_buffer[8];
    assert(!acpi_build_dsdt(&h, short_buffer, sizeof(short_buffer), &length) && length == 0);
    h.ram[1].base = h.ram[0].size - 4096;
    assert(!test_acpi_build(&h, &acpi, base, facs, facs_base));
    puts("ACPI/SMBIOS checksum, layout, topology, holes and boundary checks passed");
    return 0;
}
