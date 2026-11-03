/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "tables.h"
static size_t table(struct acpi_image *a, const char *signature, size_t size, unsigned revision)
{
    size_t at = (a->used + 7) & ~(size_t)7;
    if (at > sizeof(a->bytes) || size > sizeof(a->bytes) - at)
        return SIZE_MAX;
    uint8_t *p = a->bytes + at;
    table_zero(p, size);
    table_text(p, signature, 4);
    table_put(p + 4, size, 4);
    p[8] = (uint8_t)revision;
    table_text(p + 10, "IA64VM", 6);
    table_text(p + 16, "VMMNATIV", 8);
    table_put(p + 24, 1, 4);
    table_text(p + 28, "VMM ", 4);
    table_put(p + 32, 1, 4);
    a->used = at + size;
    return at;
}
static void checksum(struct acpi_image *a, size_t at)
{
    uint8_t *p = a->bytes + at;
    p[9] = (uint8_t)-table_sum(p, (size_t)table_get(p + 4, 4));
}
static void gas(uint8_t *p, unsigned width, unsigned access, uint64_t address)
{
    p[0] = 1;
    p[1] = (uint8_t)width;
    p[3] = (uint8_t)access;
    table_put(p + 4, address, 8);
}
static size_t contribution(struct acpi_image *a, const uint8_t *bytes, size_t size)
{
    if (!size)
        return SIZE_MAX;
    size_t at = (a->used + 7) & ~(size_t)7;
    if (at > sizeof(a->bytes) || size > sizeof(a->bytes) - at)
        return SIZE_MAX;
    for (size_t i = 0; i < size; ++i)
        a->bytes[at + i] = bytes[i];
    a->used = at + size;
    return at;
}
/* UEFI §7.2.3 EFI_MEMORY_DESCRIPTOR: DescriptorSize is a stride, not
 * sizeof a compiler structure. Reserved, unusable and device space do not
 * describe OS RAM. ACPI §5.2.16.2 keeps runtime, ACPI and PAL RAM in the
 * locality too; affinity does not make those pages allocatable. */
static size_t memory_affinity(uint8_t *out, const uint8_t *map, size_t bytes, size_t stride)
{
    size_t count = 0;
    uint64_t previous_end = 0;
    for (size_t at = 0; at < bytes; at += stride) {
        const uint8_t *d = map + at;
        uint64_t base = table_get(d + 8, 8), pages = table_get(d + 24, 8);
        if (!pages || base % 4096 || pages > (UINT64_MAX - base) / 4096)
            return SIZE_MAX;
        switch (table_get(d, 4)) {
        case 1:
        case 2: /* EfiLoaderCode, EfiLoaderData */
        case 3:
        case 4: /* EfiBootServicesCode, EfiBootServicesData */
        case 5:
        case 6: /* EfiRuntimeServicesCode, EfiRuntimeServicesData */
        case 7: /* EfiConventionalMemory */
        case 9:
        case 10: /* EfiACPIReclaimMemory, EfiACPIMemoryNVS */
        case 13: /* EfiPalCode */
            break;
        default:
            continue;
        }
        uint64_t size = pages * 4096;
        if (count && base == previous_end) {
            if (out) {
                uint8_t *p = out + (count - 1) * 40;
                table_put(p + 16, table_get(p + 16, 8) + size, 8);
            }
        } else {
            if (++count > ACPI_IMAGE_CAPACITY / 40)
                return SIZE_MAX;
            if (out) {
                uint8_t *p = out + (count - 1) * 40;
                p[0] = 1;
                p[1] = 40;
                table_put(p + 8, base, 8);
                table_put(p + 16, size, 8);
                table_put(p + 28, 1, 4);
            }
        }
        previous_end = base + size;
    }
    return count;
}
bool acpi_build(const struct ia64vmm_handoff *h, struct acpi_image *a, uint64_t base,
                uint8_t facs[64], uint64_t facs_base, const void *memory_map, size_t map_size,
                size_t descriptor_size)
{
    if (!memory_map || !map_size || descriptor_size < 40 || map_size % descriptor_size)
        return false;
    size_t memory_count = memory_affinity(NULL, memory_map, map_size, descriptor_size);
    if (!memory_count || memory_count == SIZE_MAX)
        return false;
    if (!h || !a || !facs || base % 16 || facs_base % 64 || base > UINT64_MAX - sizeof(a->bytes) ||
        facs_base > UINT64_MAX - 64 || !h->cpu_count || h->cpu_count > TABLE_MAX_CPUS ||
        !h->ram_range_count || h->ram_range_count > IA64VMM_HANDOFF_MAX_RAM ||
        h->acpi_pm_port_base != IA64VMM_ACPI_PM_PORT_BASE ||
        h->acpi_pm_port_size != IA64VMM_ACPI_PM_PORT_SIZE || h->ecam_base % (UINT64_C(1) << 20) ||
        h->ecam_bus_last > 255 || h->ecam_bus_first > h->ecam_bus_last ||
        h->ecam_size < ((uint64_t)(h->ecam_bus_last + 1) << 20) ||
        h->ecam_size > UINT64_MAX - h->ecam_base || h->iosapic_base % 4096 ||
        !h->iosapic_gsi_count || h->pib_base % (UINT64_C(1) << 20))
        return false;
    uint64_t last = 0;
    for (unsigned i = 0; i < h->ram_range_count; ++i) {
        if (!h->ram[i].size || h->ram[i].base % 4096 || h->ram[i].size % 4096 ||
            h->ram[i].base < last || h->ram[i].size > UINT64_MAX - h->ram[i].base)
            return false;
        last = h->ram[i].base + h->ram[i].size;
    }
    table_zero(a, sizeof(*a));
    table_zero(facs, 64);
    table_text(facs, "FACS", 4);
    table_put(facs + 4, 64, 4);
    facs[32] = 1;
    size_t aml_size;
    if (!acpi_build_dsdt(h, a->bytes + 36, sizeof(a->bytes) - 36, &aml_size))
        return false;
    /* table() clears its allocation, so retain the emitted AML separately by
     * initializing only the header here. */
    a->dsdt = 0;
    a->used = 36 + aml_size;
    table_text(a->bytes, "DSDT", 4);
    table_put(a->bytes + 4, a->used, 4);
    a->bytes[8] = 2;
    table_text(a->bytes + 10, "IA64VM", 6);
    table_text(a->bytes + 16, "VMMNATIV", 8);
    table_put(a->bytes + 24, 1, 4);
    table_text(a->bytes + 28, "VMM ", 4);
    table_put(a->bytes + 32, 1, 4);
    checksum(a, 0);
    /* ACPI 2.0: FADT revision 3 and MADT revision 1 with the 12-byte
     * Processor Local SAPIC structure from the errata (EDK2 Acpi20.h).
     * SRAT revision 1 uses the interim ACPI 2.0 layout; the domain-zero
     * fields used here have the same layout in revision 2. */
    a->fadt = table(a, "FACP", 244, 3);
    a->madt = table(a, "APIC", 44 + 16 + 12 + h->cpu_count * 12, 1);
    a->mcfg = table(a, "MCFG", 60, 1);
    a->srat = table(a, "SRAT", 48 + h->cpu_count * 16 + memory_count * 40, 1);
    a->slit = table(a, "SLIT", 45, 1);
    struct console_tables console;
    console_tables_build(&console, h);
    a->uart = contribution(a, console.uart, console.uart_size);
    a->spcr = contribution(a, console.spcr, console.spcr_size);
    a->hcdp = contribution(a, console.hcdp, console.hcdp_size);
    if ((console.uart_size && a->uart == SIZE_MAX) || (console.spcr_size && a->spcr == SIZE_MAX) ||
        (console.hcdp_size && a->hcdp == SIZE_MAX))
        return false;
    size_t entries = 5 + !!console.uart_size + !!console.spcr_size + !!console.hcdp_size;
    a->xsdt = table(a, "XSDT", 36 + entries * 8, 1);
    if (a->fadt == SIZE_MAX || a->madt == SIZE_MAX || a->mcfg == SIZE_MAX || a->srat == SIZE_MAX ||
        a->slit == SIZE_MAX || a->xsdt == SIZE_MAX)
        return false;
    uint8_t *p = a->bytes + a->fadt;
    if (facs_base <= UINT32_MAX)
        table_put(p + 36, facs_base, 4);
    if (base <= UINT32_MAX)
        table_put(p + 40, base, 4);
    p[45] = 4;
    table_put(p + 46, 9, 2);
    uint64_t pm = h->acpi_pm_port_base;
    table_put(p + 56, pm, 4);
    table_put(p + 64, pm + 4, 4);
    table_put(p + 76, pm + 8, 4);
    p[88] = 4;
    p[89] = 2;
    p[91] = 4;
    table_put(p + 96, 0xffff, 2);
    table_put(p + 98, 0xffff, 2);
    if (h->flags & IA64VMM_HF_I8042_ENABLED)
        table_put(p + 109, 3, 2);
    /* Fixed buttons, reset register and software sleep; no fictitious WBINVD. */
    table_put(p + 112, (1u << 4) | (1u << 5) | (1u << 10) | (1u << 13), 4);
    gas(p + 116, 8, 1, pm + 12);
    p[128] = 1;
    table_put(p + 132, facs_base, 8);
    table_put(p + 140, base, 8);
    gas(p + 148, 32, 2, pm);
    gas(p + 172, 16, 2, pm + 4);
    gas(p + 208, 32, 3, pm + 8);
    checksum(a, a->fadt);
    table_put(a->bytes + a->madt + 36, h->pib_base, 4);
    p = a->bytes + a->madt + 44;
    p[0] = 6;
    p[1] = 16;
    table_put(p + 4, h->iosapic_gsi_base, 4);
    table_put(p + 8, h->iosapic_base, 8);
    p += 16;
    p[0] = 5;
    p[1] = 12;
    table_put(p + 4, h->pib_base, 8);
    p += 12;
    /* ACPI Processor ID (offset 2) is the ProcId of the DSDT's Processor
     * object CPxx (aml.c); id/eid = LID, which SAL and the VMM share. */
    for (unsigned i = 0; i < h->cpu_count; ++i, p += 12) {
        p[0] = 7;
        p[1] = 12;
        p[2] = (uint8_t)i;
        p[3] = (uint8_t)i;
        table_put(p + 8, 1, 4);
    }
    checksum(a, a->madt);
    p = a->bytes + a->mcfg;
    table_put(p + 44, h->ecam_base, 8);
    p[54] = (uint8_t)h->ecam_bus_first;
    p[55] = (uint8_t)h->ecam_bus_last;
    checksum(a, a->mcfg);
    p = a->bytes + a->srat;
    table_put(p + 36, 1, 4);
    p += 48;
    /* One uniform domain; memory follows GetMemoryMap, including its holes. */
    for (unsigned i = 0; i < h->cpu_count; ++i, p += 16) {
        p[0] = 0;
        p[1] = 16;
        p[3] = (uint8_t)i;
        table_put(p + 4, 1, 4);
    }
    memory_affinity(p, memory_map, map_size, descriptor_size);
    checksum(a, a->srat);
    p = a->bytes + a->slit;
    table_put(p + 36, 1, 8);
    p[44] = 10;
    checksum(a, a->slit);
    size_t offsets[] = {a->fadt, a->madt, a->mcfg, a->srat, a->slit, a->uart, a->spcr, a->hcdp};
    p = a->bytes + a->xsdt;
    size_t entry = 0;
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
        if (offsets[i] != SIZE_MAX)
            table_put(p + 36 + entry++ * 8, base + offsets[i], 8);
    checksum(a, a->xsdt);
    a->rsdp = (a->used + 15) & ~(size_t)15;
    if (a->rsdp > sizeof(a->bytes) - 36)
        return false;
    p = a->bytes + a->rsdp;
    table_text(p, "RSD PTR ", 8);
    table_text(p + 9, "IA64VM", 6);
    p[15] = 2;
    table_put(p + 20, 36, 4);
    table_put(p + 24, base + a->xsdt, 8);
    p[8] = (uint8_t)-table_sum(p, 20);
    p[32] = (uint8_t)-table_sum(p, 36);
    a->used = a->rsdp + 36;
    return true;
}
