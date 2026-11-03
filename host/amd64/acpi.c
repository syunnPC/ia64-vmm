/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/amd64.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

static uint16_t pm1a, pm1b, sleep_a, sleep_b;
static uint32_t smi_cmd;
static uint8_t acpi_enable;
static const struct boot_info *layout;
static uint64_t little(const uint8_t *p, unsigned n)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static const uint8_t *physical(uint64_t pa, size_t n)
{
    if (!layout || !n || n > UINT64_MAX - pa)
        return NULL;
    for (unsigned i = 0; i < layout->mem_range_count; ++i) {
        const struct boot_mem_range *m = &layout->mem[i];
        if (pa >= m->base && pa - m->base <= m->size && n <= m->size - (pa - m->base))
            return m->hva + (pa - m->base);
    }
    return NULL;
}
static bool checksum(const uint8_t *p, size_t n)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < n; ++i)
        sum += p[i];
    return sum == 0;
}
static const uint8_t *sdt(uint64_t pa, const char *signature)
{
    const uint8_t *p = physical(pa, 36);
    if (!p || (signature && memcmp(p, signature, 4)))
        return NULL;
    uint64_t len = little(p + 4, 4);
    if (len < 36 || len > 1048576)
        return NULL;
    p = physical(pa, len);
    return p && checksum(p, len) ? p : NULL;
}
static bool aml_integer(const uint8_t **p, const uint8_t *end, uint16_t *value)
{
    if (*p == end)
        return false;
    uint8_t op = *(*p)++;
    if (op == 0 || op == 1) {
        *value = op;
        return true;
    }
    unsigned n = op == 0x0a ? 1 : op == 0x0b ? 2 : op == 0x0c ? 4 : op == 0x0e ? 8 : 0;
    if (!n || (size_t)(end - *p) < n)
        return false;
    uint64_t v = little(*p, n);
    *p += n;
    if (v > 7)
        return false;
    *value = v;
    return true;
}
static bool find_s5(const uint8_t *dsdt)
{
    const uint8_t *end = dsdt + little(dsdt + 4, 4);
    /* Best-effort support for the conventional Name(_S5_, Package(...))
     * AML encoding. Do not attempt a general AML interpreter. */
    for (const uint8_t *p = dsdt + 36; end - p > 7; ++p) {
        if (*p != 0x08)
            continue;
        const uint8_t *name = p + 1;
        if (*name == '\\')
            ++name;
        if (end - name < 6 || memcmp(name, "_S5_", 4) || name[4] != 0x12)
            continue;
        const uint8_t *length = name + 5;
        unsigned following = *length >> 6;
        if ((size_t)(end - length) < following + 2u)
            continue;
        uint32_t package_length = following ? *length & 0xf : *length & 0x3f;
        for (unsigned i = 0; i < following; ++i)
            package_length |= (uint32_t)length[i + 1] << (4 + 8 * i);
        if (package_length > (uint32_t)(end - length) || package_length < following + 2u)
            continue;
        const uint8_t *q = length + following + 1;
        const uint8_t *package_end = length + package_length;
        if (*q++ < 2)
            continue;
        uint16_t a, b;
        if (aml_integer(&q, package_end, &a) && aml_integer(&q, package_end, &b)) {
            sleep_a = a << 10;
            sleep_b = b << 10;
            return true;
        }
    }
    return false;
}
static uint16_t gas_port(const uint8_t *gas)
{
    uint64_t address = little(gas + 4, 8);
    return gas[0] == 1 && gas[1] >= 16 && gas[2] == 0 && address <= 0xffff ? (uint16_t)address : 0;
}
const uint8_t *amd64_acpi_table_index(const struct boot_info *bi, const char *signature,
                                      unsigned index)
{
    layout = bi;
    if (!bi->acpi_rsdp)
        return NULL;
    const uint8_t *rsdp = physical(bi->acpi_rsdp, 20);
    if (!rsdp || memcmp(rsdp, "RSD PTR ", 8) || !checksum(rsdp, 20))
        return NULL;
    uint64_t root_pa = little(rsdp + 16, 4);
    unsigned step = 4;
    if (rsdp[15] >= 2) {
        const uint8_t *extended = physical(bi->acpi_rsdp, 36);
        if (extended) {
            uint64_t len = little(extended + 20, 4);
            const uint8_t *full = len >= 36 && len <= 4096 ? physical(bi->acpi_rsdp, len) : NULL;
            if (full && checksum(full, len) && little(full + 24, 8)) {
                root_pa = little(full + 24, 8);
                step = 8;
            }
        }
    }
    const uint8_t *root = sdt(root_pa, step == 8 ? "XSDT" : "RSDT");
    if (!root)
        return NULL;
    const uint8_t *end = root + little(root + 4, 4);
    for (const uint8_t *p = root + 36; (size_t)(end - p) >= step; p += step) {
        const uint8_t *table = sdt(little(p, step), signature);
        if (table && !index--)
            return table;
    }
    return NULL;
}
const uint8_t *amd64_acpi_table(const struct boot_info *bi, const char *signature)
{
    return amd64_acpi_table_index(bi, signature, 0);
}
const uint8_t *amd64_acpi_dsdt(const struct boot_info *bi)
{
    const uint8_t *fadt = amd64_acpi_table(bi, "FACP");
    if (!fadt || little(fadt + 4, 4) < 44)
        return NULL;
    uint64_t pa = little(fadt + 40, 4);
    if (little(fadt + 4, 4) >= 148 && little(fadt + 140, 8))
        pa = little(fadt + 140, 8);
    return sdt(pa, "DSDT");
}
void amd64_acpi_init(const struct boot_info *bi)
{
    pm1a = pm1b = 0;
    const uint8_t *fadt = amd64_acpi_table(bi, "FACP");
    if (!fadt)
        return;
    uint64_t len = little(fadt + 4, 4);
    if (len < 116)
        return;
    uint64_t dsdt_pa = little(fadt + 40, 4);
    if (len >= 148 && little(fadt + 140, 8))
        dsdt_pa = little(fadt + 140, 8);
    const uint8_t *dsdt = sdt(dsdt_pa, "DSDT");
    if (!dsdt || !find_s5(dsdt)) {
        LOGW("ACPI _S5 package unavailable; power-off will halt");
        return;
    }
    uint64_t a = little(fadt + 64, 4), b = little(fadt + 68, 4);
    if (a > 0xffff || b > 0xffff || fadt[89] != 2)
        return;
    pm1a = a;
    pm1b = b;
    if (len >= 196) {
        uint16_t x = gas_port(fadt + 172), y = gas_port(fadt + 184);
        if (x)
            pm1a = x;
        if (y)
            pm1b = y;
    }
    smi_cmd = little(fadt + 48, 4);
    acpi_enable = fadt[52];
    if (pm1a)
        LOGI("ACPI S5 PM1a_CNT=%04x PM1b_CNT=%04x", (unsigned)pm1a, (unsigned)pm1b);
}
void amd64_acpi_poweroff(void)
{
    if (!pm1a) {
        LOGW("ACPI S5 unavailable; halting");
        return;
    }
    if (!(hal_port_in(pm1a, 2) & 1)) {
        if (!smi_cmd || smi_cmd > 0xffff || !acpi_enable)
            return;
        hal_port_out(smi_cmd, acpi_enable, 1);
        uint64_t deadline = hal_counter() + hal_counter_hz() * 3;
        while (!(hal_port_in(pm1a, 2) & 1)) {
            if (hal_counter() >= deadline) {
                LOGW("ACPI enable timed out; halting");
                return;
            }
            __asm__ volatile("pause");
        }
    }
    uint16_t value = (hal_port_in(pm1a, 2) & ~0x3c00u) | sleep_a | (1u << 13);
    hal_port_out(pm1a, value, 2);
    if (pm1b)
        hal_port_out(pm1b, (hal_port_in(pm1b, 2) & ~0x3c00u) | sleep_b | (1u << 13), 2);
}
