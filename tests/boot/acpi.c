/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "boot/acpi.h"
#include "boot/ranges.h"
#include "host/amd64/amd64.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"

/* ACPI 2.0 §§5.2.4.3, 5.2.5–5.2.8: RSDP revision 0 has only
 * RsdtAddress; RSDT entries and FADT.DSDT are 32-bit physical addresses.
 * XSDT entries and X_DSDT are 64-bit. FACS is shared firmware state in
 * NVS (§5.2.9), so neither FIRMWARE_CTRL form may be relocated. */
#define FOUR_G (UINT64_C(1) << 32)
static struct boot_info bi;
static uint8_t tables[0x6000];
static _Alignas(4096) uint8_t pool[0x10000];
static size_t used;
static uint64_t table_base, pool_base;
uint64_t amd64_hhdm;
static jmp_buf panic_env;
static bool expect_panic;
static const char *panic_message;

void hal_panic(const char *message)
{
    if (!expect_panic) {
        fprintf(stderr, "unexpected panic: %s\n", message);
        assert(0);
    }
    panic_message = message;
    longjmp(panic_env, 1);
}
void *hal_alloc_pages(size_t pages, unsigned flags)
{
    assert(pages <= (sizeof(pool) - used) / 4096);
    void *p = pool + used;
    used += pages * 4096;
    if (flags & HAL_PAGE_ZERO)
        memset(p, 0, pages * 4096);
    return p;
}
void vmm_log(enum log_level level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}
static uint64_t get(const uint8_t *p, unsigned bytes)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void put(uint8_t *p, unsigned bytes, uint64_t v)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = v >> (8 * i);
}
static unsigned sum(const uint8_t *p, size_t n)
{
    uint8_t v = 0;
    for (size_t i = 0; i < n; ++i)
        v += p[i];
    return v;
}
static void seal(uint8_t *p, size_t n, unsigned off)
{
    p[off] = 0;
    p[off] = -sum(p, n);
}
static void sdt(unsigned off, const char *sig, size_t n, unsigned revision)
{
    memcpy(tables + off, sig, 4);
    put(tables + off + 4, 4, n);
    tables[off + 8] = revision;
    seal(tables + off, n, 9);
}
static void fixture(bool extended, bool xsdt, size_t fadt_len, bool x_dsdt, bool high_source)
{
    memset(&bi, 0, sizeof(bi));
    memset(tables, 0, sizeof(tables));
    table_base = high_source ? 2 * FOUR_G + 0x100000 : 0x100000;
    assert(!high_source || xsdt);
    bi.mem[bi.mem_range_count++] =
        (struct boot_mem_range){table_base, sizeof(tables), BOOT_MEM_ACPI_RECLAIMABLE, tables};
    bi.mem[bi.mem_range_count++] =
        (struct boot_mem_range){0x80000000, 0x40000000, BOOT_MEM_USABLE, NULL};
    bi.mem[bi.mem_range_count++] =
        (struct boot_mem_range){FOUR_G, 0x40000000, BOOT_MEM_USABLE, NULL};
    bi.acpi_rsdp = table_base;
    memcpy(tables, "RSD PTR ", 8);
    tables[15] = extended ? 2 : 0;
    if (xsdt) {
        put(tables + 24, 8, table_base + 0x1000);
    } else
        put(tables + 16, 4, table_base + 0x1000);
    if (extended)
        put(tables + 20, 4, 36);
    seal(tables, 20, 8);
    if (extended)
        seal(tables, 36, 32);
    unsigned step = xsdt ? 8 : 4;
    sdt(0x1000, xsdt ? "XSDT" : "RSDT", 36 + 4 * step, 1);
    /* A repeated entry must still refer to one copied object. */
    put(tables + 0x1000 + 36, step, table_base + 0x2000);
    put(tables + 0x1000 + 36 + step, step, table_base + 0x2000);
    put(tables + 0x1000 + 36 + 2 * step, step, table_base + 0x5000);
    seal(tables + 0x1000, 36 + 4 * step, 9);
    sdt(0x2000, "FACP", fadt_len, fadt_len >= 148 ? 3 : 1);
    /* Preserve OEM identity and fixed hardware fields during extension. */
    for (unsigned i = 10; i < 36; ++i)
        tables[0x2000 + i] = i;
    for (unsigned i = 44; i < 116 && i < fadt_len; ++i)
        tables[0x2000 + i] = i;
    put(tables + 0x2000 + 36, 4, 0x12340000); /* Original FACS in NVS. */
    put(tables + 0x2000 + 40, 4, high_source ? 0 : table_base + 0x3000);
    if (fadt_len >= 140) {
        put(tables + 0x2000 + 132, 8, FOUR_G + 0x12340000);
    }
    if (x_dsdt) {
        assert(fadt_len >= 148);
        put(tables + 0x2000 + 140, 8, table_base + 0x3000);
        /* Prefer X_DSDT even when DSDT holds a different valid table. */
        if (!high_source)
            put(tables + 0x2000 + 40, 4, table_base + 0x4000);
    }
    seal(tables + 0x2000, fadt_len, 9);
    sdt(0x3000, "DSDT", 40, 2);
    tables[0x3000 + 36] = 0xa3;
    seal(tables + 0x3000, 40, 9);
    sdt(0x4000, "DSDT", 40, 1);
    sdt(0x5000, "SSDT", 40, 2);
    tables[0x5000 + 36] = 0x01;
    seal(tables + 0x5000, 40, 9);
}
static uint8_t *copied(uint64_t pa)
{
    assert(pa >= pool_base && pa - pool_base < used);
    return pool + (pa - pool_base);
}
static void allocator(uint64_t base)
{
    used = 0;
    memset(pool, 0xcc, sizeof(pool));
    pool_base = base;
    amd64_hhdm = (uintptr_t)pool - base;
}
static void run_case(bool extended, bool xsdt, size_t fadt_len, bool x_dsdt, bool high_source,
                     bool high_dest, bool reserved)
{
    fixture(extended, xsdt, fadt_len, x_dsdt, high_source);
    if (reserved)
        bi.mem[0].type = BOOT_MEM_RESERVED;
    uint64_t base;
    assert(boot_reserve_range(&bi, 0x8000000, &base));
    assert(base == FOUR_G + 0x38000000);
    allocator(high_dest ? FOUR_G + 0x200000 : 0x200000);
    uint8_t before[sizeof(tables)];
    memcpy(before, tables, sizeof(tables));
    amd64_copy_acpi(&bi);
    uint8_t *r = copied(bi.acpi_rsdp);
    /* ACPI 2.0 Table 5-3: revision 2, 36 bytes, both checksums. */
    assert(!sum(r, 20) && r[15] == 2 && get(r + 20, 4) == 36 && !sum(r, 36));
    assert(get(r + 16, 4) == 0);
    assert(!memcmp(r + 9, before + 9, 6));
    for (unsigned i = 33; i < 36; ++i)
        assert(r[i] == 0);
    uint8_t *p = copied(get(r + 24, 8));
    /* Table 5-7: 64-bit entries, including duplicate and null entries. */
    assert(!memcmp(p, "XSDT", 4) && get(p + 4, 4) == 68 && p[8] == 1 && !sum(p, 68));
    assert(!memcmp(p + 10, before + 0x1000 + 10, 26));
    uint64_t fadt = get(p + 36, 8);
    assert(get(p + 44, 8) == fadt && get(p + 60, 8) == 0);
    uint8_t *ssdt = copied(get(p + 52, 8));
    assert(!memcmp(ssdt, "SSDT", 4) && ssdt[36] == 0x01 && !sum(ssdt, 40));
    p = copied(fadt);
    size_t len = fadt_len < 148 ? 244 : fadt_len;
    /* Table 5-8: ACPI 2.0 FADT ends at byte 243; X_DSDT is at 140. */
    assert(!memcmp(p, "FACP", 4) && get(p + 4, 4) == len && p[8] == 3 && !sum(p, len));
    assert(!memcmp(p + 10, before + 0x2000 + 10, 30));
    assert(!memcmp(p + 44, before + 0x2000 + 44, fadt_len < 140 ? fadt_len - 44 : 96));
    if (fadt_len >= 148)
        assert(!memcmp(p + 148, before + 0x2000 + 148, fadt_len - 148));
    assert(get(p + 36, 4) == 0x12340000);
    assert(get(p + 132, 8) == (fadt_len >= 140 ? FOUR_G + 0x12340000 : 0));
    uint64_t dsdt = get(p + 140, 8);
    assert(get(p + 40, 4) == (high_dest ? 0 : dsdt));
    if (fadt_len < 148)
        for (unsigned i = 148; i < 244; ++i)
            assert(p[i] == 0);
    p = copied(dsdt);
    assert(!memcmp(p, "DSDT", 4) && p[36] == 0xa3 && !sum(p, 40));
    assert(!memcmp(before, tables, sizeof(tables)));
}
static void rejected(const char *message)
{
    allocator(FOUR_G);
    uint64_t original = bi.acpi_rsdp;
    expect_panic = true;
    if (!setjmp(panic_env)) {
        amd64_copy_acpi(&bi);
        assert(0);
    }
    expect_panic = false;
    assert(strstr(panic_message, message));
    assert(bi.acpi_rsdp == original);
}
int main(void)
{
    for (unsigned high = 0; high < 2; ++high) {
        run_case(false, false, 116, false, false, high, false);
        run_case(true, false, 244, true, false, high, false);
        run_case(true, true, 116, false, false, high, false);
        run_case(true, true, 140, false, false, high, false);
        run_case(true, true, 147, false, false, high, false);
        run_case(true, true, 148, false, false, high, false);
        run_case(true, true, 244, false, false, high, false);
        run_case(true, true, 244, true, false, high, false);
        run_case(true, true, 276, true, true, high, false);
        run_case(false, false, 116, false, false, high, true);
    }
    /* Table 5-3: an RSDT-only extended RSDP gets the 36-byte revision-2 form. */
    fixture(true, false, 116, false, false);
    tables[15] = 3;
    put(tables + 20, 4, 52);
    tables[51] = 0xa5;
    seal(tables, 20, 8);
    seal(tables, 52, 32);
    allocator(FOUR_G);
    amd64_copy_acpi(&bi);
    uint8_t *r = copied(bi.acpi_rsdp);
    assert(r[15] == 2 && get(r + 20, 4) == 36 && !sum(r, 20) && !sum(r, 36));
    /* Preserve extensions of an existing XSDT-based RSDP. */
    fixture(true, true, 244, true, false);
    tables[15] = 3;
    put(tables + 20, 4, 52);
    tables[51] = 0xa5;
    seal(tables, 20, 8);
    seal(tables, 52, 32);
    allocator(FOUR_G);
    amd64_copy_acpi(&bi);
    r = copied(bi.acpi_rsdp);
    assert(r[15] == 3 && get(r + 20, 4) == 52 && r[51] == 0xa5 && !sum(r, 20) && !sum(r, 52));
    /* An empty legacy root also becomes a valid high XSDT. */
    fixture(false, false, 116, false, false);
    sdt(0x1000, "RSDT", 36, 1);
    allocator(FOUR_G);
    amd64_copy_acpi(&bi);
    uint8_t *root = copied(get(copied(bi.acpi_rsdp) + 24, 8));
    assert(!memcmp(root, "XSDT", 4) && get(root + 4, 4) == 36 && !sum(root, 36));
    /* Table 5-4 requires a complete header, bounded length and checksum. */
    fixture(true, true, 244, true, false);
    tables[0x5000 + 9]++;
    rejected("invalid ACPI table");
    fixture(false, false, 116, false, false);
    put(tables + 0x1000 + 36, 4, 0xdead0000);
    seal(tables + 0x1000, 52, 9);
    rejected("outside mapped physical ranges");
    fixture(true, true, 244, true, false);
    bi.mem[0].size = 0x5000;
    bi.mem[bi.mem_range_count++] =
        (struct boot_mem_range){table_base + 0x4f00, 0x1100, BOOT_MEM_RESERVED, NULL};
    rejected("outside mapped physical ranges");
    fixture(true, true, 244, true, false);
    bi.mem[0].size = 0x5010; /* Root entry has no complete mapped header. */
    rejected("outside mapped physical ranges");
    fixture(true, true, 244, true, false);
    put(tables + 0x5000 + 4, 4, 35);
    rejected("invalid ACPI table");
    fixture(true, true, 244, true, false);
    put(tables + 0x5000 + 4, 4, 0x2000);
    rejected("invalid ACPI table");
    fixture(false, false, 116, false, false);
    sdt(0x1000, "SSDT", 52, 1);
    rejected("invalid ACPI root table");
    fixture(true, true, 244, true, false);
    sdt(0x1000, "XSDT", 67, 1);
    rejected("invalid ACPI root table");
    fixture(true, true, 244, true, false);
    sdt(0x2000, "FACP", 43, 1);
    rejected("truncated ACPI FADT");
    bi.acpi_rsdp = 0;
    allocator(FOUR_G);
    amd64_copy_acpi(&bi);
    assert(!used && !bi.acpi_rsdp);
    puts("boot ACPI: PASS");
}
