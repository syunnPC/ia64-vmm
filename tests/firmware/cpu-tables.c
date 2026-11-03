/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MADT Local SAPIC entries, SRAT
 * processor entries, DSDT processor objects and SMBIOS processor records
 * list exactly cpu_count processors, ids 0..N-1, eid 0, none disabled; each
 * Local SAPIC's ACPI Processor ID names one DSDT Processor object.
 * SMBIOS type 4 groups them in populated, model-bounded sockets. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tests/firmware/acpi-map.h"
#include "firmware/new/smbios/smbios.h"

static struct acpi_image acpi;
static struct smbios_image smb;
static uint8_t facs[64];
static const char hex[] = "0123456789ABCDEF";

/* ACPI 3.0b §17.2.4: PkgLength counts its own encoding bytes. */
static size_t package(const uint8_t *p, size_t *encoding)
{
    unsigned extra = p[0] >> 6;
    size_t length = extra ? p[0] & 15u : p[0] & 63u;
    for (unsigned i = 0; i < extra; ++i)
        length |= (size_t)p[1 + i] << (4 + 8 * i);
    *encoding = 1 + extra;
    return length;
}
/* What the tables say about each processor, for the cross-checks below. */
static int madt_proc[256], dsdt_seen[256];

/* ACPI 2.0 (errata) Processor Local SAPIC structure, EDK2 Acpi20.h: 12
 * bytes; ACPI Processor ID at 2 names the DSDT Processor object, LID id/eid
 * at 3/4, flags at 8. ids 0..N-1 in order, eid 0, all enabled. */
static unsigned madt(void)
{
    const uint8_t *m = acpi.bytes + acpi.madt;
    assert(m[8] == 1 && acpi.bytes[acpi.fadt + 8] == 3 &&
           table_get(acpi.bytes + acpi.fadt + 4, 4) == 244);
    unsigned found = 0;
    for (size_t at = 44; at < table_get(m + 4, 4); at += m[at + 1]) {
        const uint8_t *p = m + at;
        if (p[0] != 7)
            continue;
        assert(p[1] == 12 && p[3] == found && p[4] == 0 && !p[5] && !p[6] && !p[7] &&
               table_get(p + 8, 4) == 1);
        madt_proc[found++] = p[2];
    }
    return found;
}
/* The interim ACPI 2.0 SRAT, revision 1 (as the reference firmware); its
 * processor entries name the MADT's id/eid, all in domain 0, which the
 * one-locality SLIT covers. */
static unsigned srat(unsigned cpus)
{
    const uint8_t *r = acpi.bytes + acpi.srat;
    assert(r[8] == 1 && table_get(r + 36, 4) == 1);
    const uint8_t *s = acpi.bytes + acpi.slit;
    assert(s[8] == 1 && table_get(s + 4, 4) == 45 && table_get(s + 36, 8) == 1 && s[44] == 10);
    unsigned found = 0;
    for (size_t at = 48; at < table_get(r + 4, 4); at += r[at + 1]) {
        const uint8_t *p = r + at;
        if (p[0] != 0)
            continue;
        assert(p[1] == 16 && p[2] == 0 && p[3] == found && p[3] < cpus &&
               table_get(p + 4, 4) == 1 && p[8] == 0 && !p[9] && !p[10] && !p[11]);
        ++found;
    }
    return found;
}
/* DSDT: Name(_S5_, Package) then Scope(\_SB_) holding the Processor objects
 * and PCI0. A processor object carries no _STA or other body (present and
 * enabled, ACPI 3.0b §6.3.7). */
static unsigned dsdt(void)
{
    const uint8_t *a = acpi.bytes;
    size_t end = (size_t)table_get(a + 4, 4), at = 36, encoding;
    assert(a[at] == 0x08 && !memcmp(a + at + 1, "_S5_", 4) && a[at + 5] == 0x12);
    at += 6;
    at += package(a + at, &encoding);
    assert(a[at] == 0x10);
    size_t scope_end = at + 1 + package(a + at + 1, &encoding);
    at += 1 + encoding;
    assert(scope_end == end && a[at] == '\\' && !memcmp(a + at + 1, "_SB_", 4));
    at += 5;
    unsigned found = 0;
    while (at < scope_end) {
        assert(a[at] == 0x5b && (a[at + 1] == 0x83 || a[at + 1] == 0x82));
        size_t length = package(a + at + 2, &encoding);
        const uint8_t *body = a + at + 2 + encoding;
        if (a[at + 1] == 0x83) {
            char name[4] = {'C', 'P', hex[found >> 4], hex[found & 15]};
            assert(length == encoding + 10 && !memcmp(body, name, 4) &&
                   table_get(body + 5, 4) == 0 && body[9] == 0);
            /* ProcId: each names exactly one MADT Local SAPIC (ACPI 2.0 §5.2.10.13). */
            assert(!dsdt_seen[body[4]]++);
            ++found;
        } else
            assert(!memcmp(body, "PCI0", 4));
        at += 2 + length;
    }
    assert(at == scope_end);
    return found;
}
/* SMBIOS 2.8 §7.5: type 4, populated sockets and all actual cores enabled. */
static unsigned processors(unsigned model, unsigned count)
{
    unsigned found = 0;
    for (size_t at = 0; at < smb.used;) {
        const uint8_t *p = smb.bytes + at;
        if (p[0] == 4) {
            char socket[] = {'v', 'C', 'P', 'U', ' ', hex[found >> 4], hex[found & 15], 0};
            unsigned width = model || count == 1 ? 1 : 2;
            unsigned cores = count - found * width;
            if (cores > width)
                cores = width;
            assert(p[1] == 42 && p[24] == 0x41 && p[35] == cores && p[36] == cores &&
                   p[37] == cores && p[4] == 1 && !strcmp((const char *)p + p[1], socket));
            assert(p[6] == 0xb8 && table_get(p + 8, 8) == (model ? UINT64_C(0x000000011f010504)
                                                                 : UINT64_C(0x0000000520000704)));
            /* DSP0134 2.8 type 4: Other upgrade, Itanium 2 family. */
            assert(p[25] == 1 && table_get(p + 40, 2) == 0xb8);
            /* DSP0134 type 4 reports bus and processor speeds in MHz. */
            assert(table_get(p + 18, 2) == 400 && table_get(p + 20, 2) == 1600 &&
                   table_get(p + 22, 2) == 1600);
            assert(!(table_get(p + 38, 2) & 0x10));
            ++found;
        }
        at += p[1];
        while (smb.bytes[at] || smb.bytes[at + 1])
            ++at;
        at += 2;
    }
    return found;
}

int main(void)
{
    struct ia64vmm_handoff h = {
        .ram_range_count = 1,
        .ram = {{0, 0x10000000}},
        .firmware_size = 0x800000,
        .ecam_base = IA64VMM_ECAM_BASE,
        .ecam_size = IA64VMM_ECAM_SIZE,
        .ecam_bus_first = 0,
        .ecam_bus_last = 0,
        .iosapic_base = IA64VMM_IOSAPIC_BASE,
        .iosapic_gsi_base = 16,
        .iosapic_gsi_count = 24,
        .pib_base = IA64VMM_PIB_BASE,
        .acpi_pm_port_base = IA64VMM_ACPI_PM_PORT_BASE,
        .acpi_pm_port_size = IA64VMM_ACPI_PM_PORT_SIZE,
        .legacy_io_base = IA64VMM_LEGACY_IO_BASE,
        .legacy_io_size = IA64VMM_LEGACY_IO_SIZE,
    };
    h.size = sizeof(h);
    for (unsigned model = 0; model < 2; ++model) {
        ia64vmm_handoff_cpu_model(&h, model);
        for (unsigned count = 1; count <= 34; ++count) {
            h.cpu_count = count <= 32 ? count : count == 33 ? 64 : 256;
            assert(test_acpi_build(&h, &acpi, UINT64_C(0x180000000), facs, UINT64_C(0x190000000)));
            assert(smbios_build(&h, &smb, 0x100000));
            memset(dsdt_seen, 0, sizeof(dsdt_seen));
            assert(madt() == h.cpu_count && srat(h.cpu_count) == h.cpu_count &&
                   dsdt() == h.cpu_count &&
                   processors(model, h.cpu_count) == (model ? h.cpu_count : (h.cpu_count + 1) / 2));
            for (unsigned i = 0; i < h.cpu_count; ++i)
                assert(dsdt_seen[madt_proc[i]] == 1);
            printf(
                "cpu tables: model %u, %u CPUs: MADT (12-byte), SRAT, DSDT list exactly ids 0..%u, all enabled; "
                "MADT ACPI Processor IDs = DSDT ProcIds; populated SMBIOS sockets\n",
                model, h.cpu_count, h.cpu_count - 1);
        }
    }
    h.cpu_count = 0;
    assert(!test_acpi_build(&h, &acpi, UINT64_C(0x180000000), facs, UINT64_C(0x190000000)) &&
           !smbios_build(&h, &smb, 0x100000));
    return 0;
}
