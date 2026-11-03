/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The guest sees exactly its vCPUs. Walk every ACPI
 * description of processors (MADT Local SAPICs, SRAT processor affinity,
 * DSDT/SSDT Processor objects) and require exactly `expected` of each, all
 * enabled, with lengths and checksums consistent.
 * `expected` is
 * CPUDESC_EXPECTED (mk/qemu-apps.mk builds the required counts); CPU i has LID id
 * i and eid 0, so entry i of the MADT and SRAT carries id i, eid 0 and ACPI
 * processor id i, the Processor objects carry ids 0 .. expected - 1, and
 * SMBIOS describes the model's populated sockets with no spare capacity:
 * Montecito has up to two cores/socket, Madison one, one thread/core. */
#ifndef CPUDESC_EXPECTED
#define CPUDESC_EXPECTED 1
#endif
#include <efi.h>
#include "sockets.h"
#include "../../../include/ia64vmm/platform.h"

static EFI_SYSTEM_TABLE *st;
static unsigned failures;
void *memcpy(void *dest, const void *source, UINTN size)
{
    unsigned char *d = dest;
    const unsigned char *s = source;
    while (size--)
        *d++ = *s++;
    return dest;
}
static const EFI_GUID acpi20 = {
    0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
static const EFI_GUID smbios_guid = SMBIOS_TABLE_GUID;
static void say(const char *s)
{
    while (*s)
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_CONSOLE) = (unsigned char)*s++;
}
static void number(UINT64 v)
{
    char b[21];
    int i = 20;
    b[i] = 0;
    do
        b[--i] = (char)('0' + v % 10);
    while (v /= 10);
    say(b + i);
}
static void hex(UINT64 v)
{
    char b[19];
    b[0] = '0';
    b[1] = 'x';
    for (int i = 0; i < 16; ++i)
        b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
    b[18] = 0;
    say(b);
}
static void check(int ok, const char *what, UINT64 value)
{
    say(ok ? "CPUDESC PASS " : "CPUDESC FAIL ");
    say(what);
    say(" = ");
    number(value);
    say("\n");
    failures += !ok;
}
static UINT64 le(const unsigned char *p, unsigned n)
{
    UINT64 v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (UINT64)p[i] << (i * 8);
    return v;
}
static int same(const void *a, const void *b, unsigned n)
{
    const unsigned char *p = a, *q = b;
    for (unsigned i = 0; i < n; ++i)
        if (p[i] != q[i])
            return 0;
    return 1;
}
static int valid(const unsigned char *p)
{
    if (!p)
        return 0;
    UINT64 n = le(p + 4, 4);
    unsigned sum = 0;
    if (n < 36 || n > 65536)
        return 0;
    for (UINT64 i = 0; i < n; ++i)
        sum += p[i];
    return !(sum & 255);
}
static const unsigned char *table(const unsigned char *xsdt, const char *sig)
{
    if (!valid(xsdt))
        return 0;
    for (UINTN i = 36; i + 8 <= le(xsdt + 4, 4); i += 8) {
        const unsigned char *t = (void *)(UINTN)le(xsdt + i, 8);
        if (same(t, sig, 4))
            return t;
    }
    return 0;
}
/* Count subtable entries of one type; -1 when the walk does not end exactly
 * at the table length or a matching entry is disabled (flags bit 0 at
 * flag_offset; 0 = no flags). */
static long entries(const unsigned char *t, unsigned first, unsigned type, unsigned flag_offset)
{
    long count = 0;
    UINT64 n = le(t + 4, 4), at = first;
    while (at < n) {
        if (at + 2 > n || t[at + 1] < 2 || t[at + 1] > n - at)
            return -1;
        if (t[at] == type) {
            if (flag_offset && (flag_offset + 4 > t[at + 1] || !(le(t + at + flag_offset, 4) & 1)))
                return -1;
            ++count;
        }
        at += t[at + 1];
    }
    return at == n ? count : -1;
}
/* AML DefProcessor := 5B 83 PkgLength NameString ProcID PblkAddr PblkLen;
 * count CPUn objects and mark their ProcIDs (ACPI 2.0 §16.2.5.2). */
static unsigned char proc_id_seen[256];
static unsigned processors(const unsigned char *t)
{
    unsigned found = 0;
    if (!valid(t))
        return 0;
    for (UINTN i = 36; i + 4 < le(t + 4, 4); ++i)
        if (t[i] == 0x5b && t[i + 1] == 0x83) {
            UINTN name = i + 2 + 1 + (t[i + 2] >> 6);
            if (name + 2 < le(t + 4, 4) && t[name] == 'C' && t[name + 1] == 'P') {
                ++found;
                if (name + 4 < le(t + 4, 4))
                    ++proc_id_seen[t[name + 4]];
            }
        }
    return found;
}
/* Entry `index` (0-based) of the given type in a subtable walk. */
static const unsigned char *nth(const unsigned char *t, unsigned first, unsigned type,
                                unsigned index)
{
    UINT64 n = le(t + 4, 4), at = first;
    while (at + 2 <= n && t[at + 1] >= 2 && t[at + 1] <= n - at) {
        if (t[at] == type && !index--)
            return t + at;
        at += t[at + 1];
    }
    return 0;
}
/* SMBIOS 2.x entry point (DSP0134 §5.2.1), then type 4 records (§7.5):
 * Status +0x18 (bit 6 populated, bits 2:0 = 1 enabled), Core Count +0x23,
 * Core Enabled +0x24, Thread Count +0x25. */
static void smbios_processors(UINT64 expected)
{
    const unsigned char *ep = 0, *p, *end;
    unsigned records = 0, enabled = 0, cores = 0, threads = 0, walk = 0;
    unsigned bad_sockets = 0, previous = 0;
    /* Read the architectural family independently of the firmware tables. */
    UINT64 index = 3, id;
    __asm__ volatile("mov %0=cpuid[%1]" : "=r"(id) : "r"(index));
    unsigned width = ((id >> 24) & 255) == 32 ? 2 : 1;
    for (UINTN i = 0; i < st->NumberOfTableEntries; ++i)
        if (same(&smbios_guid, &st->ConfigurationTable[i].VendorGuid, 16))
            ep = st->ConfigurationTable[i].VendorTable;
    check(ep && same(ep, "_SM_", 4) && same(ep + 16, "_DMI_", 5), "SMBIOS entry point", ep != 0);
    if (!ep || !same(ep + 16, "_DMI_", 5))
        return;
    p = (const unsigned char *)(UINTN)le(ep + 24, 4);
    end = p + le(ep + 22, 2);
    for (UINT64 i = 0, count = le(ep + 28, 2); i < count && p + 4 <= end; ++i) {
        const unsigned char *s;
        if (p[1] < 4 || p + p[1] > end) {
            walk = 1;
            break;
        }
        if (p[0] == 4) {
            ++records;
            if (p[1] >= 0x26 && (p[0x18] & 0x40) && (p[0x18] & 7) == 1) {
                bad_sockets += !cpudesc_socket_valid(width, previous, p[0x23], p[0x24], p[0x25]);
                previous = p[0x23];
                enabled += p[0x24];
                cores += p[0x23];
                threads += p[0x25];
            } else
                ++bad_sockets;
        }
        if (p[0] == 127)
            break;
        for (s = p + p[1]; s + 1 < end && (s[0] || s[1]); ++s) {
        }
        p = s + 2;
    }
    check(!walk, "SMBIOS structure walk", walk);
    check(!bad_sockets, "SMBIOS per-socket population", bad_sockets);
    check(records == (expected + width - 1) / width, "SMBIOS processor (type 4) records", records);
    check(enabled == expected, "SMBIOS enabled cores", enabled);
    check(cores == expected, "SMBIOS core count (no spare capacity)", cores);
    check(threads == expected, "SMBIOS thread count", threads);
}
EFI_STATUS _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
    say("CPUDESC BEGIN expected=");
    number(CPUDESC_EXPECTED);
    say("\n");
    (void)image;
    st = system;
    const unsigned expected = CPUDESC_EXPECTED;
    say("CPUDESC expected processors = ");
    number(expected);
    say("\n");
    const unsigned char *rsdp = 0, *xsdt = 0;
    for (UINTN i = 0; i < st->NumberOfTableEntries; ++i)
        if (same(&acpi20, &st->ConfigurationTable[i].VendorGuid, 16))
            rsdp = st->ConfigurationTable[i].VendorTable;
    if (rsdp)
        xsdt = (void *)(UINTN)le(rsdp + 24, 8);
    check(valid(xsdt), "XSDT checksum", valid(xsdt));
    say("CPUDESC INFO XSDT at ");
    hex((UINT64)(UINTN)xsdt);
    say("\n");
    const unsigned char *madt = table(xsdt, "APIC"), *srat = table(xsdt, "SRAT");
    check(valid(madt), "MADT checksum", valid(madt));
    long lsapics = valid(madt) ? entries(madt, 44, 7, 8) : -1;
    check(lsapics == (long)expected, "MADT enabled Local SAPIC entries", (UINT64)lsapics);
    long iosapics = valid(madt) ? entries(madt, 44, 6, 0) : -1;
    check(iosapics >= 1, "MADT I/O SAPIC entries after the Local SAPICs", (UINT64)iosapics);
    check(valid(srat), "SRAT checksum", valid(srat));
    long affinity = valid(srat) ? entries(srat, 48, 0, 4) : -1;
    check(affinity == (long)expected, "SRAT enabled processor affinity entries", (UINT64)affinity);
    /* ACPI 2.0 §5.2.10.4 Local SAPIC: ACPI id +2, id +3, eid +4; SRAT
     * processor affinity: id +3, eid +8. */
    unsigned bad_madt = 0, bad_srat = 0;
    for (unsigned i = 0; i < expected; ++i) {
        const unsigned char *l = lsapics == (long)expected ? nth(madt, 44, 7, i) : 0;
        const unsigned char *a = affinity == (long)expected ? nth(srat, 48, 0, i) : 0;
        bad_madt += !l || l[2] != i || l[3] != i || l[4] != 0;
        bad_srat += !a || a[3] != i || a[8] != 0;
    }
    check(!bad_madt, "MADT Local SAPICs not (ACPI id i, id i, eid 0)", bad_madt);
    check(!bad_srat, "SRAT processors not (id i, eid 0)", bad_srat);
    unsigned objects = 0;
    const unsigned char *fadt = table(xsdt, "FACP");
    if (valid(fadt)) {
        UINT64 dsdt = le(fadt + 40, 4);
        if (le(fadt + 4, 4) >= 148 && le(fadt + 140, 8))
            dsdt = le(fadt + 140, 8);
        objects += processors((void *)(UINTN)dsdt);
    }
    if (valid(xsdt))
        for (UINTN i = 36; i + 8 <= le(xsdt + 4, 4); i += 8) {
            const unsigned char *t = (void *)(UINTN)le(xsdt + i, 8);
            if (same(t, "SSDT", 4)) {
                check(valid(t), "SSDT checksum", valid(t));
                objects += processors(t);
            }
        }
    check(objects == expected, "ACPI Processor objects", objects);
    unsigned bad_ids = 0;
    for (unsigned i = 0; i < 256; ++i)
        bad_ids += proc_id_seen[i] != (i < expected);
    check(!bad_ids, "Processor object ids not exactly 0 .. N-1", bad_ids);
    smbios_processors(expected);
    say(failures ? "CPUDESC FAILED\n" : "CPUDESC PASSED\n");
    st->RuntimeServices->ResetSystem(EfiResetShutdown, failures ? EFI_ABORTED : EFI_SUCCESS, 0, 0);
    return failures ? EFI_ABORTED : EFI_SUCCESS;
}
