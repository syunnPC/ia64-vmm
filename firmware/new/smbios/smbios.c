/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SMBIOS 2.8: describe the virtual system, without inventing DIMMs, board
 * serial numbers, UUIDs, host sockets or physical error-correction hardware.
 */
#include "smbios.h"
static uint8_t *record(struct smbios_image *s, unsigned type, size_t size,
                       const char *const *strings, unsigned count)
{
    size_t total = size + 1 + (count == 0);
    for (unsigned i = 0; i < count; ++i) {
        for (size_t j = 0; strings[i][j]; ++j)
            ++total;
        ++total;
    }
    if (total > sizeof(s->bytes) - s->used)
        return NULL;
    uint8_t *p = s->bytes + s->used;
    table_zero(p, total);
    p[0] = (uint8_t)type;
    p[1] = (uint8_t)size;
    table_put(p + 2, s->count, 2);
    size_t at = size;
    for (unsigned i = 0; i < count; ++i) {
        for (size_t j = 0; strings[i][j]; ++j)
            p[at++] = (uint8_t)strings[i][j];
        ++at;
    }
    s->used += total;
    ++s->count;
    if (total > s->largest)
        s->largest = (uint16_t)total;
    return p;
}
bool smbios_build(const struct ia64vmm_handoff *h, struct smbios_image *s, uint64_t address)
{
    if (!h || !s || !ia64vmm_handoff_cpu_valid(h) || !h->cpu_count ||
        h->cpu_count > TABLE_MAX_CPUS || address > UINT32_MAX ||
        sizeof(s->bytes) > UINT64_C(0x100000000) - address || !h->firmware_size ||
        h->firmware_size > UINT64_C(16) * 1024 * 1024)
        return false;
    struct ia64vmm_handoff normalized;
    ia64vmm_handoff_copy(&normalized, h);
    h = &normalized;
    table_zero(s, sizeof(*s));
    const char *bios[] = {"IA64-VMM", "native firmware", "10/08/2026"};
    uint8_t *p = record(s, 0, 24, bios, 3);
    if (!p)
        return false;
    p[4] = 1;
    p[5] = 2;
    p[8] = 3;
    p[9] = (uint8_t)((h->firmware_size + 65535) / 65536 - 1);
    table_put(p + 10, 1u << 3, 8);
    p[19] = 1u << 3;      /* UEFI supported. */
    p[22] = p[23] = 0xff; /* No embedded controller. */
    const char *system[] = {"IA64-VMM", "Virtual IA-64 machine", "native firmware"};
    p = record(s, 1, 27, system, 3);
    if (!p)
        return false;
    p[4] = 1;
    p[5] = 2;
    p[6] = 3;
    p[24] = 2;
    unsigned width = ia64_model_socket_width(h->cpu_report.model, h->cpu_count);
    unsigned sockets = (h->cpu_count + width - 1) / width;
    for (unsigned i = 0; i < sockets; ++i) {
        static const char hex[] = "0123456789ABCDEF";
        char socket[] = {'v', 'C', 'P', 'U', ' ', hex[i >> 4], hex[i & 15], 0};
        const char *cpu[] = {socket, "IA64-VMM", h->cpu_report.brand};
        p = record(s, 4, 42, cpu, 3);
        if (!p)
            return false;
        p[4] = 1;
        p[5] = 3;
        p[6] = 0xb8;
        p[7] = 2;
        /* VMM CPUID[3]/[4], rather than the AMD64 host CPUID. */
        table_put(p + 8, h->cpu_report.cpuid3 | (h->cpu_report.cpuid4 << 32), 8);
        p[16] = 3;
        table_put(
            p + 18,
            IA64VMM_PLATFORM_BASE_HZ * IA64VMM_BUS_RATIO_NUM / IA64VMM_BUS_RATIO_DEN / 1000000, 2);
        uint64_t mhz =
            IA64VMM_PLATFORM_BASE_HZ * IA64VMM_PROC_RATIO_NUM / IA64VMM_PROC_RATIO_DEN / 1000000;
        table_put(p + 20, mhz, 2);
        table_put(p + 22, mhz, 2);
        p[24] = 0x41;
        p[25] = 1;
        for (unsigned j = 26; j < 32; ++j)
            p[j] = 0xff;
        unsigned cores = ia64_model_socket_cores(h->cpu_report.model, h->cpu_count, i * width);
        p[35] = p[36] = p[37] = (uint8_t)cores;
        table_put(p + 40, 0xb8, 2); /* ProcessorFamily2: Itanium 2. */
        table_put(p + 38, (1u << 2) | (cores > 1 ? 1u << 3 : 0), 2);
    }
    /* System boot status: no error has been reported to firmware. */
    if (!record(s, 32, 11, NULL, 0) || !record(s, 127, 4, NULL, 0))
        return false;
    p = s->entry;
    table_text(p, "_SM_", 4);
    p[5] = 31;
    p[6] = 2;
    p[7] = 8;
    table_put(p + 8, s->largest, 2);
    table_text(p + 16, "_DMI_", 5);
    table_put(p + 22, s->used, 2);
    table_put(p + 24, address, 4);
    table_put(p + 28, s->count, 2);
    p[30] = 0x28;
    p[21] = (uint8_t)-table_sum(p + 16, 15);
    p[4] = (uint8_t)-table_sum(p, 31);
    return true;
}
