/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "boot/acpi.h"
#include "host/amd64/amd64.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

struct acpi_copy {
    uint64_t source, dest;
};
static struct acpi_copy copies[BOOT_MAX_MEM_RANGES];
static unsigned count;
static const struct boot_info *layout;
static uint64_t read_le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}
static void write_le(uint8_t *p, unsigned n, uint64_t v)
{
    for (unsigned i = 0; i < n; ++i)
        p[i] = v >> (i * 8);
}
static const uint8_t *source(uint64_t pa, uint64_t n)
{
    if (!n || pa > UINT64_MAX - n)
        return NULL;
    for (unsigned i = 0; i < layout->mem_range_count; ++i) {
        const struct boot_mem_range *m = &layout->mem[i];
        if (pa >= m->base && pa - m->base < m->size && n <= m->size - (pa - m->base))
            return m->hva ? m->hva + (pa - m->base) : NULL;
    }
    return NULL;
}
static bool checksum(const uint8_t *p, size_t n)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < n; ++i)
        sum += p[i];
    return !sum;
}
static void fix_checksum(uint8_t *p, size_t n, unsigned offset)
{
    p[offset] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < n; ++i)
        sum += p[i];
    p[offset] = 0 - sum;
}
static const uint8_t *rsdp_source(size_t *n, bool *extended)
{
    const uint8_t *p = source(layout->acpi_rsdp, 20);
    if (!p || memcmp(p, "RSD PTR ", 8) || !checksum(p, 20))
        hal_panic("invalid Limine RSDP");
    *extended = p[15] >= 2;
    *n = 20;
    if (*extended) {
        p = source(layout->acpi_rsdp, 36);
        if (!p)
            hal_panic("truncated extended RSDP");
        *n = read_le(p + 20, 4);
        p = *n >= 36 && *n <= 4096 ? source(layout->acpi_rsdp, *n) : NULL;
        if (!p || !checksum(p, *n))
            hal_panic("invalid extended RSDP checksum");
    }
    return p;
}
static const uint8_t *table_source(uint64_t pa)
{
    const uint8_t *p = source(pa, 36);
    if (!p)
        hal_panic("ACPI table is outside mapped physical ranges");
    size_t n = read_le(p + 4, 4);
    p = n >= 36u && n <= 1048576 ? source(pa, n) : NULL;
    if (!p || !checksum(p, n))
        hal_panic("invalid ACPI table while copying");
    return p;
}
static uint8_t *copy_bytes(const uint8_t *p, size_t n, size_t size)
{
    uint8_t *copy = hal_alloc_pages(IA64VMM_ALIGN_UP(size, 4096) / 4096, HAL_PAGE_ZERO);
    if (!copy)
        hal_panic("cannot copy ACPI tables to reservation");
    memcpy(copy, p, n);
    return copy;
}
static uint64_t copy_table(uint64_t pa)
{
    if (!pa)
        return 0;
    for (unsigned i = 0; i < count; ++i)
        if (copies[i].source == pa)
            return copies[i].dest;
    const uint8_t *original = table_source(pa);
    size_t n = read_le(original + 4, 4);
    if (count == IA64VMM_ARRAY_SIZE(copies))
        hal_panic("too many ACPI tables to copy");
    bool fadt = !memcmp(original, "FACP", 4);
    if (fadt && n < 44)
        hal_panic("truncated ACPI FADT");
    size_t size = fadt && n < 148 ? 244 : n;
    uint8_t *p = copy_bytes(original, n, size);
    uint64_t dest = (uintptr_t)p - amd64_hhdm;
    copies[count++] = (struct acpi_copy){pa, dest};
    if (fadt) {
        /* ACPI 2.0 §5.2.8, Table 5-8: DSDT (40) and X_DSDT (140).
         * Extend through byte 243 when X_DSDT is absent; §5.2.2 requires
         * an updated Length/Revision. Preserve all hardware fields.
         * FIRMWARE_CTRL/X_FIRMWARE_CTRL retain the shared FACS in NVS
         * (§5.2.9), including its global lock and waking vector. */
        if (size != n) {
            write_le(p + 4, 4, size);
            if (p[8] < 3)
                p[8] = 3;
        }
        uint64_t dsdt = read_le(p + 40, 4);
        if (n >= 148 && read_le(p + 140, 8))
            dsdt = read_le(p + 140, 8);
        dsdt = copy_table(dsdt);
        write_le(p + 40, 4, dsdt <= UINT32_MAX ? dsdt : 0);
        write_le(p + 140, 8, dsdt);
        /* ACPI 2.0 §5.2.5: checksum includes the extended fields. */
        fix_checksum(p, size, 9);
    }
    return dest;
}
void amd64_copy_acpi(struct boot_info *bi)
{
    if (!bi->acpi_rsdp)
        return;
    layout = bi;
    count = 0;
    size_t n;
    bool extended;
    const uint8_t *original = rsdp_source(&n, &extended);
    unsigned step = extended && read_le(original + 24, 8) ? 8 : 4;
    uint64_t root = read_le(original + (step == 8 ? 24 : 16), step);
    const uint8_t *root_source = table_source(root);
    size_t len = read_le(root_source + 4, 4);
    if (memcmp(root_source, step == 8 ? "XSDT" : "RSDT", 4) || (len - 36) % step)
        hal_panic("invalid ACPI root table");
    size_t entries = (len - 36) / step;
    size_t root_len = 36 + entries * 8;
    uint8_t *p = copy_bytes(root_source, 36, root_len);
    uint64_t owned = (uintptr_t)p - amd64_hhdm;
    copies[count++] = (struct acpi_copy){root, owned};
    /* ACPI 2.0 §5.2.7, Table 5-7: XSDT Signature, Length, Revision
     * and 64-bit Entry array. OEM and creator fields retain their values. */
    memcpy(p, "XSDT", 4);
    write_le(p + 4, 4, root_len);
    p[8] = 1;
    for (size_t i = 0; i < entries; ++i)
        write_le(p + 36 + i * 8, 8, copy_table(read_le(root_source + 36 + i * step, step)));
    /* ACPI 2.0 §5.2.5: SDT Checksum covers the entire Length. */
    fix_checksum(p, root_len, 9);
    size_t rsdp_len = step == 4 ? 36 : n;
    uint8_t *rsdp = copy_bytes(original, extended ? rsdp_len : 20, rsdp_len);
    /* ACPI 2.0 §5.2.4.3, Table 5-3: Revision, Length, RsdtAddress,
     * XsdtAddress and both checksums. Newly appended Reserved bytes are
     * zero (§5.2.1); private readers use only the XSDT. */
    if (step == 4)
        rsdp[15] = 2;
    write_le(rsdp + 20, 4, rsdp_len);
    write_le(rsdp + 16, 4, 0);
    write_le(rsdp + 24, 8, owned);
    fix_checksum(rsdp, 20, 8);
    fix_checksum(rsdp, rsdp_len, 32);
    bi->acpi_rsdp = (uintptr_t)rsdp - amd64_hhdm;
    LOGI("relocation ACPI RSDP=%016llx tables=%u (XSDT)", (unsigned long long)bi->acpi_rsdp, count);
    /* No bootloader-backed pointer remains in this boot-only walker. */
    memset(copies, 0, sizeof(copies));
    layout = NULL;
}
