/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * smp.efi: guest SMP test application.
 *
 * The BSP starts APs with the wake vector from
 * the SAL system table type-5 descriptor, SAL_SET_VECTORS(OS_BOOT_RENDEZ,
 * entry, gp, 0, 0, 0, 0) with code length 0, and one 8-byte PIB store
 * per AP at PIB + (id << 12 | eid << 4) (SDM Vol.2 §5.8.4). Every later
 * case wakes the APs it needs again through the same rendezvous: the AP
 * runs the case's AP side inside the OS_BOOT_RENDEZ handler and returns to
 * the firmware's wait loop. Processors, the PIB and the wake vector come
 * from firmware tables only (MADT with the type-5 address override, SAL
 * system table, SMBIOS); the BSP is the processor running efi_main.
 *
 * Output (EFI ConOut, BSP only; APs never call EFI): one line per case,
 * "PASS <case>", "FAIL <case> <detail>" or "SKIP <case> <reason>",
 * measurements as "INFO <case> key=value ...", framed by "SMP-APP BEGIN"
 * and "SMP-APP END pass=P fail=F skip=S cpus=N". tests/smp/smplog.py
 * parses it.
 *
 * Ported in part from the IA-64 QEMU fork's
 * tests/functional/ia64/apps/smp-app.c (same author, GPL-2.0-or-later):
 * the SAL system table walk, the wake store and the rendezvous handler
 * shape (the AP returns into the firmware loop and is woken again).
 *
 * AP code rules: the handler runs with PSR.i = 0, PSR.ac possibly 1 and the
 * firmware's reset FP-disable bits, so everything an AP
 * touches is naturally aligned and AP paths use no integer multiply, divide
 * or modulo by a non-constant (the FP unit does those on IA-64). Shared
 * per-CPU data sits on its own 128-byte line.
 */
#include <efi.h>
#include "smp.h"

/* ---- compile-time parameters (mk/smp.mk builds the variants) ---------- */
enum {
    C_SAL_AP_WAKE,
    C_SET_VECTORS,
    C_MADT_EXACT,
    C_AP_START,
    C_AP_ENTRY_STATE,
    C_REPEAT,
    C_IPI,
    C_ITC,
    C_MP,
    C_SB,
    C_SEM_SB,
    C_FETCHADD,
    C_CMPXCHG_LOCK,
    C_XCHG_LOCK,
    C_PURGE_REMOTE,
    C_PURGE_HALTED,
    C_COUNT
};
#define BIT(c) (1u << (c))
#define CASES_BOOT (BIT(C_SAL_AP_WAKE) | BIT(C_SET_VECTORS) | BIT(C_MADT_EXACT))
#define CASES_AP (BIT(C_AP_START) | BIT(C_AP_ENTRY_STATE) | BIT(C_REPEAT))
#define CASES_LITMUS                                                                               \
    (BIT(C_MP) | BIT(C_SB) | BIT(C_SEM_SB) | BIT(C_FETCHADD) | BIT(C_CMPXCHG_LOCK) |               \
     BIT(C_XCHG_LOCK))
#define CASES_PURGE (BIT(C_PURGE_REMOTE) | BIT(C_PURGE_HALTED))
#define CASES_ALL ((1u << C_COUNT) - 1)

/* Profiles: "all" (default), "long" (10^6), "quick" (10^3, for lockstep and
 * smp_serialize smoke runs), "ipi-itc" (start-up, IPI and ITC cases only: what
 * runs under smp_serialize=1, where a ptc.g stops the VM), "no-purge" (all but
 * the purge cases), "tlb-purge" (start-up and purge cases). Each broken_*
 * variant runs the cases its defect breaks plus their prerequisites. */
#if defined(SMP_VARIANT_long)
#define SMP_PROFILE "long"
#define SMP_ITERS 1000000u
#elif defined(SMP_VARIANT_quick)
#define SMP_PROFILE "quick"
#define SMP_ITERS 1000u
#define SMP_IPI_ROUNDS 1000u
#elif defined(SMP_VARIANT_ipi_itc)
#define SMP_PROFILE "ipi-itc"
#define SMP_CASES (CASES_BOOT | CASES_AP | BIT(C_IPI) | BIT(C_ITC))
#elif defined(SMP_VARIANT_restart)
/* Profile ipi-itc twice, an in-VMM restart in between. */
#define SMP_PROFILE "ipi-itc"
#define SMP_RESTART 1
#define SMP_CASES (CASES_BOOT | CASES_AP | BIT(C_IPI) | BIT(C_ITC))
#elif defined(SMP_VARIANT_no_purge)
#define SMP_PROFILE "no-purge"
#define SMP_CASES (CASES_ALL & ~CASES_PURGE)
#elif defined(SMP_VARIANT_tlb_purge)
#define SMP_PROFILE "tlb-purge"
#define SMP_CASES (CASES_BOOT | CASES_AP | CASES_PURGE)
#elif defined(SMP_VARIANT_broken_wake_vector)
#define SMP_PROFILE "broken-wake-vector"
#define SMP_BROKEN_WAKE_VECTOR 1
#define SMP_CASES CASES_BOOT
#elif defined(SMP_VARIANT_broken_set_vectors)
#define SMP_PROFILE "broken-set-vectors"
#define SMP_BROKEN_SET_VECTORS 1
#define SMP_CASES CASES_BOOT
#elif defined(SMP_VARIANT_broken_smbios)
#define SMP_PROFILE "broken-smbios"
#define SMP_BROKEN_SMBIOS 1
#define SMP_CASES CASES_BOOT
#elif defined(SMP_VARIANT_broken_nocallin)
#define SMP_PROFILE "broken-nocallin"
#define SMP_BROKEN_NOCALLIN 1
#define SMP_CASES (CASES_BOOT | CASES_AP)
#elif defined(SMP_VARIANT_broken_lid)
#define SMP_PROFILE "broken-lid"
#define SMP_BROKEN_LID 1
#define SMP_CASES (CASES_BOOT | CASES_AP)
#elif defined(SMP_VARIANT_broken_entry_gp)
#define SMP_PROFILE "broken-entry-gp"
#define SMP_BROKEN_ENTRY_GP 1
#define SMP_CASES (CASES_BOOT | CASES_AP)
#elif defined(SMP_VARIANT_broken_repeat)
#define SMP_PROFILE "broken-repeat"
#define SMP_BROKEN_REPEAT 1
#define SMP_CASES (CASES_BOOT | CASES_AP)
#elif defined(SMP_VARIANT_broken_ipi)
#define SMP_PROFILE "broken-ipi"
#define SMP_BROKEN_IPI 1
#define SMP_CASES (CASES_BOOT | CASES_AP | BIT(C_IPI))
#elif defined(SMP_VARIANT_broken_itc)
#define SMP_PROFILE "broken-itc"
#define SMP_BROKEN_ITC 1
#define SMP_CASES (CASES_BOOT | CASES_AP | BIT(C_ITC))
#elif defined(SMP_VARIANT_broken_mp)
#define SMP_PROFILE "broken-mp"
#define SMP_BROKEN_MP 1
#define SMP_CASES (CASES_BOOT | CASES_AP | BIT(C_MP))
#elif defined(SMP_VARIANT_broken_sb)
#define SMP_PROFILE "broken-sb"
#define SMP_BROKEN_SB 1
#define SMP_CASES (CASES_BOOT | CASES_AP | BIT(C_SB))
#elif defined(SMP_VARIANT_broken_sem_sb)
#define SMP_PROFILE "broken-sem-sb"
#define SMP_BROKEN_SEM_SB 1
#define SMP_CASES (CASES_BOOT | CASES_AP | BIT(C_SEM_SB))
#elif defined(SMP_VARIANT_broken_nonatomic)
#define SMP_PROFILE "broken-nonatomic"
#define SMP_BROKEN_NONATOMIC 1
#define SMP_CASES (CASES_BOOT | CASES_AP | BIT(C_FETCHADD) | BIT(C_CMPXCHG_LOCK) | BIT(C_XCHG_LOCK))
#elif defined(SMP_VARIANT_broken_purge_local)
#define SMP_PROFILE "broken-purge-local"
#define SMP_BROKEN_PURGE_LOCAL 1
#define SMP_CASES (CASES_BOOT | CASES_AP | CASES_PURGE)
#elif defined(SMP_VARIANT_all) || !defined(SMP_PROFILE)
#define SMP_PROFILE "all"
#endif
#ifndef SMP_CASES
#define SMP_CASES CASES_ALL
#endif
#ifndef SMP_ITERS
#define SMP_ITERS 100000u /* litmus iterations; lock increments per CPU */
#endif
#ifndef SMP_IPI_ROUNDS
#define SMP_IPI_ROUNDS 10000u
#endif
#define SMP_ITC_RELEASES 1000u /* itc-skew common-release rounds */
#define SMP_ITC_TRIPS 256u     /* itc-skew round trips per AP */
#define SMP_REPEAT_ROUNDS 3u
#define SMP_MAX_CPUS 64u
#define SMP_IPI_VECTOR 0xf0u /* not the wake vector; any vector > TPR 0 */
#define SPURIOUS 15u         /* cr.ivr with nothing pending (SDM Vol.2 §5.8.3.2) */
#define SAL_SET_VECTORS 0x01000000ull
#define SAL_VECTOR_OS_BOOT_RENDEZ 2u

#define PSR_AC (1ull << 3)
#define PSR_IC (1ull << 13)
#define PSR_I (1ull << 14)
#define PSR_DT (1ull << 17)
#define PSR_DFL (1ull << 18)
#define PSR_DFH (1ull << 19)
#define PSR_RT (1ull << 27)
#define PSR_IT (1ull << 36)
#define PHYS_UC (1ull << 63) /* physical-mode UC attribute, SDM Vol.2 §4.4.2 */

/* GCC emits these for structure copies; there is no libc here. */
void *memcpy(void *d, const void *s, UINTN n)
{
    UINT8 *p = d;
    const UINT8 *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}
void *memset(void *d, int c, UINTN n)
{
    UINT8 *p = d;
    while (n--)
        *p++ = (UINT8)c;
    return d;
}

/* ---- primitives -------------------------------------------------------- */
static inline UINT64 rd_lid(void)
{
    UINT64 v;
    __asm__ volatile("mov %0=cr.lid;;" : "=r"(v)::"memory");
    return v;
}
static inline UINT64 rd_itc(void)
{
    UINT64 v;
    __asm__ volatile("mov %0=ar.itc;;" : "=r"(v)::"memory");
    return v;
}
static inline UINT64 rd_psr(void)
{
    UINT64 v;
    __asm__ volatile("mov %0=psr;;" : "=r"(v)::"memory");
    return v;
}
static inline UINT64 rd_gp(void)
{
    UINT64 v;
    __asm__ volatile("mov %0=gp;;" : "=r"(v));
    return v;
}
static inline void mf(void)
{
    __asm__ volatile("mf;;" ::: "memory");
}
static inline void cpu_pause(void)
{
    __asm__ volatile("hint @pause;;" ::: "memory");
}
static inline UINT64 ld_acq(volatile UINT64 *p)
{
    UINT64 v;
    __asm__ volatile("ld8.acq %0=[%1];;" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline void st_rel(volatile UINT64 *p, UINT64 v)
{
    __asm__ volatile("st8.rel [%0]=%1;;" ::"r"(p), "r"(v) : "memory");
}
static inline UINT64 fetchadd8_acq(volatile UINT64 *p)
{
    UINT64 v;
    __asm__ volatile("fetchadd8.acq %0=[%1],1;;" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline UINT64 rd_ivr(void)
{
    UINT64 v;
    __asm__ volatile("mov %0=cr.ivr;;\n\tsrlz.d;;" : "=r"(v)::"memory");
    return v;
}
/* SDM Vol.2 §5.8.3.4: EOI, then a data serialization before the next IVR read. */
static inline void wr_eoi(void)
{
    __asm__ volatile("mov cr.eoi=r0;;\n\tsrlz.d;;" ::: "memory");
}
static inline void wr_tpr(UINT64 v)
{
    __asm__ volatile("mov cr.tpr=%0;;\n\tsrlz.d;;" ::"r"(v) : "memory");
}
static inline INT64 since(UINT64 t)
{
    return (INT64)(rd_itc() - t);
}

/* ---- firmware discovery ------------------------------------------------ */
struct smp_cpu {
    UINT64 lid; /* cr.lid form: id << 24 | eid << 16 */
    UINT64 pib; /* this CPU's PIB slot, physical UC */
    UINT64 acpi_id;
};
static struct smp_cpu cpus[SMP_MAX_CPUS];
static UINT32 ncpu;      /* enabled MADT Local SAPIC entries */
static UINT32 bsp = ~0u; /* index of the BSP in cpus[] */
static UINT64 itc_hz;
static UINT64 wake_vector;
static UINT64 registered_entry, registered_gp;
static int registered;

static struct {
    const UINT8 *madt, *sal;
    UINT64 pib;
    int pib_override;
    UINT32 madt_lsapic, madt_disabled, madt_overflow;
    UINT64 sal_proc, sal_gp, pal_proc;
    int sal_status; /* 0 ok, else a reason code below */
    UINT32 wake_descriptors;
    UINT64 wake_mechanism;
    int smbios_status;
    UINT64 smbios_cpus, smbios_records;
    int lid_ids_unique;
} fw;

static EFI_SYSTEM_TABLE *gST;
static EFI_HANDLE gImage;
static UINT8 *memmap;
static UINTN memmap_size, memmap_desc;

/* ---- output (BSP only) ------------------------------------------------- */
static char line[512];
static UINTN line_len;
static UINT32 n_pass, n_fail, n_skip;
/* A case may find several failures (one per CPU, per round, per litmus
 * form); they collect here and end in one FAIL line (run_case). */
static char detail[400];
static UINTN detail_len;
static int to_detail, case_failures, case_skipped;

static void flush_line(void)
{
    CHAR16 buf[128];
    UINTN i = 0, n;

    line[line_len++] = '\r';
    line[line_len++] = '\n';
    for (n = 0; n < line_len; ++n) {
        buf[i++] = (CHAR16)(UINT8)line[n];
        if (i == 127 || n + 1 == line_len) {
            buf[i] = 0;
            gST->ConOut->OutputString(gST->ConOut, buf);
            i = 0;
        }
    }
    line_len = 0;
}
static void put(const char *s)
{
    if (to_detail)
        while (*s && detail_len < sizeof(detail) - 1)
            detail[detail_len++] = *s++;
    else
        while (*s && line_len < sizeof(line) - 3)
            line[line_len++] = *s++;
}
static void put_u(UINT64 v)
{
    char b[21];
    int i = 20;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    put(b + i);
}
static void put_i(INT64 v)
{
    if (v < 0) {
        put("-");
        put_u((UINT64)-v);
    } else
        put_u((UINT64)v);
}
static void put_x(UINT64 v)
{
    char b[19];
    int i;
    b[0] = '0';
    b[1] = 'x';
    for (i = 0; i < 16; ++i)
        b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
    b[18] = 0;
    put(b);
}
static void kv_u(const char *k, UINT64 v)
{
    put(" ");
    put(k);
    put("=");
    put_u(v);
}
static void kv_i(const char *k, INT64 v)
{
    put(" ");
    put(k);
    put("=");
    put_i(v);
}
static void kv_x(const char *k, UINT64 v)
{
    put(" ");
    put(k);
    put("=");
    put_x(v);
}
static void info_begin(const char *name)
{
    put("INFO ");
    put(name);
}
static void skip(const char *name, const char *why)
{
    put("SKIP ");
    put(name);
    put(" ");
    put(why);
    flush_line();
    ++n_skip;
    case_skipped = 1;
}
/* A failure's text: fail_begin, put*, fail_end. */
static void fail_begin(const char *name)
{
    (void)name;
    to_detail = 1;
    if (case_failures++)
        put("; ");
}
static void fail_end(void)
{
    to_detail = 0;
}
static void fail(const char *name, const char *why)
{
    fail_begin(name);
    put(why);
    fail_end();
}
/* The one result line of a case. */
static void case_end(const char *name)
{
    if (case_skipped) {
    } else if (case_failures) {
        UINTN i;
        put("FAIL ");
        put(name);
        put(" ");
        for (i = 0; i < detail_len && line_len < sizeof(line) - 3; ++i)
            line[line_len++] = detail[i];
        flush_line();
        ++n_fail;
    } else {
        put("PASS ");
        put(name);
        flush_line();
        ++n_pass;
    }
    detail_len = 0;
    case_failures = case_skipped = 0;
}

/* ---- table helpers ------------------------------------------------------ */
static UINT64 le(const UINT8 *p, unsigned n)
{
    UINT64 v = 0;
    unsigned i;
    for (i = 0; i < n; ++i)
        v |= (UINT64)p[i] << (8 * i);
    return v;
}
static int same(const void *a, const void *b, UINTN n)
{
    const UINT8 *p = a, *q = b;
    while (n--)
        if (*p++ != *q++)
            return 0;
    return 1;
}
static UINT8 sum8(const UINT8 *p, UINTN n)
{
    UINT8 s = 0;
    while (n--)
        s = (UINT8)(s + *p++);
    return s;
}
static void *config_table(EFI_GUID g)
{
    UINTN i;
    for (i = 0; i < gST->NumberOfTableEntries; ++i)
        if (same(&gST->ConfigurationTable[i].VendorGuid, &g, sizeof(g)))
            return gST->ConfigurationTable[i].VendorTable;
    return NULL;
}
static int acpi_valid(const UINT8 *t)
{
    UINT64 n;
    if (!t)
        return 0;
    n = le(t + 4, 4);
    return n >= 36 && n <= 0x100000 && sum8(t, n) == 0;
}
static const UINT8 *acpi_find(const char *sig)
{
    EFI_GUID g = ACPI_20_TABLE_GUID;
    const UINT8 *rsdp = config_table(g), *sdt;
    UINT64 n, i, width;
    if (!rsdp || !same(rsdp, "RSD PTR ", 8))
        return NULL;
    if (rsdp[15] >= 2 && le(rsdp + 24, 8)) {
        sdt = (const UINT8 *)(UINTN)le(rsdp + 24, 8);
        width = 8;
    } else {
        sdt = (const UINT8 *)(UINTN)le(rsdp + 16, 4);
        width = 4;
    }
    if (!acpi_valid(sdt))
        return NULL;
    n = le(sdt + 4, 4);
    for (i = 36; i + width <= n; i += width) {
        const UINT8 *t = (const UINT8 *)(UINTN)le(sdt + i, (unsigned)width);
        if (t && same(t, sig, 4) && acpi_valid(t))
            return t;
    }
    return NULL;
}

/* MADT (ACPI 2.0 §5.2.10.4): Local SAPIC type 7 (id at +3, eid at +4, flags
 * at +8, bit 0 enabled), Local APIC address override type 5 (+4, 8 bytes),
 * which replaces the 32-bit address at +36 as the PIB base. */
static void discover_madt(void)
{
    const UINT8 *t = acpi_find("APIC");
    UINT64 n, at;
    fw.madt = t;
    if (!t)
        return;
    n = le(t + 4, 4);
    fw.pib = le(t + 36, 4);
    for (at = 44; at + 2 <= n && t[at + 1] >= 2 && t[at + 1] <= n - at; at += t[at + 1]) {
        const UINT8 *e = t + at;
        if (e[0] == 5 && e[1] >= 12) {
            fw.pib = le(e + 4, 8);
            fw.pib_override = 1;
        } else if (e[0] == 7 && e[1] >= 12) {
            ++fw.madt_lsapic;
            if (!(le(e + 8, 4) & 1)) {
                ++fw.madt_disabled;
                continue;
            }
            if (ncpu == SMP_MAX_CPUS) {
                ++fw.madt_overflow;
                continue;
            }
            cpus[ncpu].lid = (UINT64)e[3] << 24 | (UINT64)e[4] << 16;
            cpus[ncpu].acpi_id = e[2];
            ++ncpu;
        }
    }
}

/* SAL 245359-005 §3.2: 96-byte header, then descriptors whose length
 * depends on their type; type 0 entrypoint (PAL_PROC +8, SAL_PROC +16,
 * gp +24), type 5 AP wake-up (mechanism +1, vector +8). The table's bytes
 * sum to zero over its length. */
enum { SAL_OK, SAL_MISSING, SAL_HEADER, SAL_CHECKSUM, SAL_DESCRIPTOR, SAL_NO_ENTRY };
static void discover_sal(void)
{
    EFI_GUID g = SAL_SYSTEM_TABLE_GUID;
    const UINT8 *t = config_table(g);
    UINT64 n, at, count, i;
    static const UINT8 size[6] = {48, 32, 16, 32, 16, 16};
    fw.sal = t;
    if (!t) {
        fw.sal_status = SAL_MISSING;
        return;
    }
    n = le(t + 4, 4);
    count = le(t + 10, 2);
    if (!same(t, "SST_", 4) || n < 96 || n > 0x10000) {
        fw.sal_status = SAL_HEADER;
        return;
    }
    if (sum8(t, n)) {
        fw.sal_status = SAL_CHECKSUM;
        return;
    }
    for (i = 0, at = 96; i < count; ++i) {
        if (at >= n || t[at] > 5 || size[t[at]] > n - at) {
            fw.sal_status = SAL_DESCRIPTOR;
            return;
        }
        if (t[at] == 0) {
            fw.pal_proc = le(t + at + 8, 8);
            fw.sal_proc = le(t + at + 16, 8);
            fw.sal_gp = le(t + at + 24, 8);
        } else if (t[at] == 5) {
            ++fw.wake_descriptors;
            fw.wake_mechanism = t[at + 1];
            wake_vector = le(t + at + 8, 8);
        }
        at += size[t[at]];
    }
    if (at != n)
        fw.sal_status = SAL_DESCRIPTOR;
    else if (!fw.sal_proc)
        fw.sal_status = SAL_NO_ENTRY;
}

/* SMBIOS 2.x (DSP0134 §7.5): processor information, type 4. A record whose
 * socket is populated and whose CPU is enabled (status bit 6, bits 2:0 = 1)
 * provides Core Enabled (+0x24) cores of Thread Count / Core Count
 * (+0x25, +0x23) threads each; records shorter than 0x26 bytes predate
 * those fields and count one processor. The reference firmware describes
 * one socket of N cores, the new firmware N one-core sockets. */
enum { SMB_OK, SMB_MISSING, SMB_ENTRY, SMB_WALK };
static void discover_smbios(void)
{
    EFI_GUID g = SMBIOS_TABLE_GUID;
    const UINT8 *ep = config_table(g), *p, *end;
    UINT64 count, i;
    if (!ep) {
        fw.smbios_status = SMB_MISSING;
        return;
    }
    if (!same(ep, "_SM_", 4) || !same(ep + 16, "_DMI_", 5) || sum8(ep + 16, 15)) {
        fw.smbios_status = SMB_ENTRY;
        return;
    }
    p = (const UINT8 *)(UINTN)le(ep + 24, 4);
    end = p + le(ep + 22, 2);
    count = le(ep + 28, 2);
    for (i = 0; i < count && p + 4 <= end; ++i) {
        const UINT8 *s;
        if (p[1] < 4 || p + p[1] > end) {
            fw.smbios_status = SMB_WALK;
            return;
        }
        if (p[0] == 4 && p[1] >= 0x1a && (p[0x18] & 0x40) && (p[0x18] & 7) == 1) {
            UINT64 cpus_here = 1;
            ++fw.smbios_records;
            if (p[1] >= 0x26 && p[0x24] && p[0x24] != 0xff) {
                UINT64 cores = p[0x23], threads = p[0x25];
                cpus_here = p[0x24];
                if (cores && cores != 0xff && threads >= cores && threads != 0xff)
                    cpus_here *= threads / cores;
            }
            fw.smbios_cpus += cpus_here;
        }
        if (p[0] == 127)
            break;
        for (s = p + p[1]; s + 1 < end && (s[0] || s[1]); ++s) {
        }
        p = s + 2;
    }
}

static void discover_memmap(void)
{
    UINTN key = 0, size = 0;
    UINT32 version = 0;
    EFI_STATUS st;
    st = gST->BootServices->GetMemoryMap(&size, NULL, &key, &memmap_desc, &version);
    if (st != EFI_BUFFER_TOO_SMALL)
        return;
    size += 16 * memmap_desc;
    if (gST->BootServices->AllocatePool(EfiLoaderData, size, (void **)&memmap) != EFI_SUCCESS)
        return;
    st = gST->BootServices->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)memmap, &key, &memmap_desc,
                                         &version);
    if (st == EFI_SUCCESS)
        memmap_size = size;
}

/* "Inside RAM": covered by a write-back descriptor that is not MMIO,
 * MMIO port space or unusable memory (UEFI 2.x §7.2 GetMemoryMap). */
static int in_ram(UINT64 addr, UINT64 len)
{
    UINTN at;
    if (!memmap_size)
        return 0;
    for (at = 0; at + memmap_desc <= memmap_size; at += memmap_desc) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(memmap + at);
        UINT64 end = d->PhysicalStart + (d->NumberOfPages << 12);
        if (d->Type == EfiMemoryMappedIO || d->Type == EfiMemoryMappedIOPortSpace ||
            d->Type == EfiUnusableMemory || !(d->Attribute & EFI_MEMORY_WB))
            continue;
        if (addr >= d->PhysicalStart && addr - d->PhysicalStart <= end - d->PhysicalStart &&
            len <= end - addr)
            return 1;
    }
    return 0;
}

/* ---- shared state between the BSP and the APs ------------------------- */
#define LINE __attribute__((aligned(128)))
struct smp_raw smp_raw[SMP_RAW_SLOTS] LINE; /* written by entry.S */

enum { OP_NONE, OP_CALLIN, OP_IPI, OP_ITC, OP_LITMUS, OP_LOCK, OP_PURGE, OP_SPIN };
static struct {
    volatile UINT64 seq; /* command sequence; st.rel by the BSP */
    volatile UINT64 op, kind, form, iters, a, b, nparts;
} cmd LINE;

/* Call-in log: every OP_CALLIN entry appends its own cr.lid, so the BSP can
 * check that the processor that called in is the one its PIB store named. */
#define CALLIN_LOG 1024u
static volatile UINT64 callin_count LINE;
static struct {
    volatile UINT64 lid, seq, valid, pad;
} callin_log[CALLIN_LOG];

struct slot {
    volatile UINT64 entered, done; /* command seq, st.rel by the CPU */
    volatile UINT64 part;          /* participant number for this command */
    volatile UINT64 r[13];         /* case results */
    volatile UINT64 ack LINE;      /* handshakes towards this CPU */
    volatile UINT64 ping, pong, tm, t;
};
static struct slot slots[SMP_MAX_CPUS] LINE;

struct pair {
    volatile UINT64 go LINE;   /* iteration number, st.rel by role 0 */
    volatile UINT64 start;     /* ITC deadline both roles wait for */
    volatile UINT64 done LINE; /* iteration number, st.rel by role 1 */
    volatile UINT64 res1;
    volatile UINT64 x LINE; /* litmus locations, one line each */
    volatile UINT64 y LINE;
    volatile UINT64 hist[4] LINE; /* role 0 only */
    volatile UINT64 bad;
};
static struct pair pairs[SMP_MAX_CPUS / 2];
static volatile UINT64 sweep[64];   /* role 1 deadline offsets, ITC ticks */
static volatile UINT64 litmus_lead; /* ITC ticks from go to the deadline */

static volatile UINT64 bar LINE;        /* fetchadd barrier counter */
static volatile UINT64 spin_count LINE; /* OP_SPIN: APs busy until the restart */
static volatile UINT64 itc_release LINE;
static volatile UINT64 lock_word LINE;
static volatile UINT64 lock_counter LINE;
static volatile UINT64 fa8_counter LINE;
static volatile UINT32 fa4_counter LINE;

static UINT64 seq_next;
static UINT32 started[SMP_MAX_CPUS], nstarted; /* cpu indices; [0] = BSP */
static struct smp_raw first_entry[SMP_MAX_CPUS];
static int ap_lost;

static int cpu_index(UINT64 lid)
{
    UINT32 i;
    for (i = 0; i < ncpu; ++i)
        if (cpus[i].lid == lid)
            return (int)i;
    return -1;
}

static void barrier(UINT64 target)
{
    fetchadd8_acq(&bar);
    while (ld_acq(&bar) < target)
        cpu_pause();
}

/* ---- AP sides ----------------------------------------------------------- */
static void send_ipi(UINT32 cpu, UINT64 vector)
{
    /* The data of every store must be visible to the target before it can
     * take the interrupt; mf orders them before the UC store. */
    mf();
    *(volatile UINT64 *)(UINTN)cpus[cpu].pib = vector; /* delivery mode 0 (INT) */
}

/* ipi-ping-pong: a pings b, b answers; both poll cr.ivr with PSR.i 0. */
static void ap_ipi(UINT32 me)
{
    struct slot *s = &slots[me];
    UINT32 a = (UINT32)cmd.a, b = (UINT32)cmd.b;
    UINT64 rounds = cmd.iters, timeout = itc_hz, v, r, t0, last;
    UINT64 strays = 0, dups = 0, lost = 0, done_rounds = 0;
    UINT64 lat_min = ~0ull, lat_max = 0, lat_sum = 0;

    wr_tpr(0);
    while ((v = rd_ivr()) != SPURIOUS) {
        wr_eoi();
        ++strays;
    }
    if (me == b) {
        st_rel(&s->ack, cmd.seq);
        last = rd_itc();
        for (;;) {
            v = rd_ivr();
            if (v == SMP_IPI_VECTOR) {
                wr_eoi();
#if SMP_BROKEN_IPI
                if (done_rounds == (rounds >> 1)) {
                    ++done_rounds;
                    last = rd_itc();
                    continue;
                }
#endif
                send_ipi(a, SMP_IPI_VECTOR);
                ++done_rounds;
                last = rd_itc();
            } else if (v != SPURIOUS) {
                wr_eoi();
                ++strays;
            } else if (ld_acq(&slots[a].ack) == cmd.seq) {
                break; /* a finished or gave up */
            } else if (since(last) > (INT64)(timeout << 1)) {
                lost = 1;
                break;
            } else {
                cpu_pause();
            }
        }
    } else {
        while (ld_acq(&slots[b].ack) != cmd.seq)
            cpu_pause();
        for (r = 1; r <= rounds; ++r) {
            UINT64 lat;
            t0 = rd_itc();
            send_ipi(b, SMP_IPI_VECTOR);
            for (;;) {
                v = rd_ivr();
                if (v == SMP_IPI_VECTOR) {
                    wr_eoi();
                    break;
                }
                if (v != SPURIOUS) {
                    wr_eoi();
                    ++strays;
                    continue;
                }
                if (since(t0) > (INT64)timeout) {
                    lost = r;
                    break;
                }
            }
            if (lost)
                break;
            lat = rd_itc() - t0;
            ++done_rounds;
            lat_sum += lat;
            if (lat < lat_min)
                lat_min = lat;
            if (lat > lat_max)
                lat_max = lat;
        }
        st_rel(&s->ack, cmd.seq);
    }
    /* Anything of ours still arriving within 1 ms is a duplicate delivery. */
    t0 = rd_itc();
    while (since(t0) < (INT64)(itc_hz >> 10)) {
        v = rd_ivr();
        if (v == SPURIOUS)
            continue;
        wr_eoi();
        if (v == SMP_IPI_VECTOR)
            ++dups;
        else
            ++strays;
    }
    s->r[0] = done_rounds;
    s->r[1] = lost;
    s->r[2] = strays;
    s->r[3] = dups;
    s->r[4] = lat_min;
    s->r[5] = lat_max;
    s->r[6] = lat_sum;
}

/* itc-skew, AP side. Phase 1: read ar.itc as soon as the common
 * release flag is seen. Phase 2: round trips with the BSP; with one shared
 * time base the BSP's reading tm must lie between this CPU's t0 (before
 * the ping) and t1 (after the pong). */
static void ap_itc(UINT32 me)
{
    struct slot *s = &slots[me];
    UINT64 k, t0, t1, tm, rtt, viol = 0, rtt_min = ~0ull;
    INT64 off_at_min = 0, worst = 0;
    for (k = 1; k <= SMP_ITC_RELEASES; ++k) {
        while (ld_acq(&itc_release) != k)
            cpu_pause();
        s->t = rd_itc();
        st_rel(&s->ack, k);
    }
    for (k = 1; k <= SMP_ITC_TRIPS; ++k) {
        t0 = rd_itc();
#if SMP_BROKEN_ITC
        t0 += itc_hz >> 4; /* a CPU whose ITC runs 62.5 ms ahead */
#endif
        st_rel(&s->ping, k);
        while (ld_acq(&s->pong) != k)
            cpu_pause();
        t1 = rd_itc();
#if SMP_BROKEN_ITC
        t1 += itc_hz >> 4;
#endif
        tm = s->tm;
        rtt = t1 - t0;
        if ((INT64)(tm - t0) < 0 || (INT64)(t1 - tm) < 0) {
            INT64 by = (INT64)(tm - t0) < 0 ? (INT64)(tm - t0) : (INT64)(tm - t1);
            ++viol;
            if ((by < 0 ? -by : by) > (worst < 0 ? -worst : worst))
                worst = by;
        }
        if (rtt < rtt_min) {
            rtt_min = rtt;
            off_at_min = (INT64)(tm - t0) - (INT64)(rtt >> 1);
        }
    }
    s->r[0] = viol;
    s->r[1] = rtt_min;
    s->r[2] = (UINT64)off_at_min;
    s->r[3] = (UINT64)worst;
}

/* Litmus bodies: one asm block each so nothing is inserted between the
 * accesses under test. Outcome bits as documented per case. */
static UINT64 litmus_body(UINT64 kind, UINT64 form, UINT32 role, struct pair *p)
{
    volatile UINT64 *mine = role ? &p->y : &p->x, *other = role ? &p->x : &p->y;
    UINT64 r = 0, r2 = 0;
    switch (kind) {
    case C_MP:
        /* SDM Vol.2 Part II §2.2.1.3, Table 2-2: x = data, y = flag. */
        if (!role) {
#if SMP_BROKEN_MP
            __asm__ volatile("st8.rel [%1]=%2;;\n\tst8 [%0]=%2;;" ::"r"(&p->x), "r"(&p->y),
                             "r"(1ull)
                             : "memory");
#else
            __asm__ volatile("st8 [%0]=%2;;\n\tst8.rel [%1]=%2;;" ::"r"(&p->x), "r"(&p->y),
                             "r"(1ull)
                             : "memory");
#endif
        } else {
            __asm__ volatile("ld8.acq %0=[%2];;\n\tld8 %1=[%3];;"
                             : "=&r"(r), "=r"(r2)
                             : "r"(&p->y), "r"(&p->x)
                             : "memory");
            r = r << 1 | r2; /* flag << 1 | data */
        }
        break;
    case C_SB:
        /* Part II §2.2.1.5, Table 2-4. */
#if SMP_BROKEN_SB
        __asm__ volatile("st8 [%1]=%3;;\n\tld8 %0=[%2];;"
                         : "=r"(r)
                         : "r"(mine), "r"(other), "r"(1ull)
                         : "memory");
#else
        __asm__ volatile("st8 [%1]=%3;;\n\tmf;;\n\tld8 %0=[%2];;"
                         : "=r"(r)
                         : "r"(mine), "r"(other), "r"(1ull)
                         : "memory");
#endif
        break;
    case C_SEM_SB:
        /* Semaphore store-buffering. Forms 0 and 1: st1, then a cmpxchg1
         * whose ar.ccv (256) never equals a zero-extended byte, so it only
         * reads. Form 0 (.rel): the architecture orders the store before
         * the semaphore (Part II Figure 2-1), so both reads seeing 0 is
         * forbidden. Form 1 (.acq): Figure 2-1 does not order an unordered
         * store before an acquire, so o00 is architecturally allowed; it is
         * forbidden by the VMM's own rule (a semaphore is a locked
         * RMW, which drains the store buffer, also on the read-only path) and is checked as that rule. Form 2: Table
         * 2-12, st.rel x; xchg8 x; ld y (semaphores do not bypass locally,
         * Part II §2.2.1.10). */
        if (form < 2) {
#if SMP_BROKEN_SEM_SB
            __asm__ volatile("st1 [%1]=%3;;\n\tld1 %0=[%2];;"
                             : "=r"(r)
                             : "r"(mine), "r"(other), "r"(1ull)
                             : "memory");
#else
            if (form == 0)
                __asm__ volatile(
                    "st1 [%1]=%3;;\n\tmov ar.ccv=%4;;\n\tcmpxchg1.rel %0=[%2],%3,ar.ccv;;"
                    : "=r"(r)
                    : "r"(mine), "r"(other), "r"(1ull), "r"(256ull)
                    : "memory");
            else
                __asm__ volatile(
                    "st1 [%1]=%3;;\n\tmov ar.ccv=%4;;\n\tcmpxchg1.acq %0=[%2],%3,ar.ccv;;"
                    : "=r"(r)
                    : "r"(mine), "r"(other), "r"(1ull), "r"(256ull)
                    : "memory");
#endif
        } else {
#if SMP_BROKEN_SEM_SB
            __asm__ volatile("st8.rel [%2]=%4;;\n\tld8 %0=[%2];;\n\tld8 %1=[%3];;"
                             : "=&r"(r), "=r"(r2)
                             : "r"(mine), "r"(other), "r"(1ull)
                             : "memory");
#else
            __asm__ volatile("st8.rel [%2]=%4;;\n\txchg8 %0=[%2],%5;;\n\tld8 %1=[%3];;"
                             : "=&r"(r), "=r"(r2)
                             : "r"(mine), "r"(other), "r"(1ull), "r"(2ull)
                             : "memory");
#endif
            r = (r == 1) | (r2 != 0) << 1; /* own xchg saw own st.rel; load saw peer */
        }
        break;
    }
    return r;
}

/* One participant of a two-CPU litmus pair. Role 0 resets the locations,
 * publishes an ITC deadline and the iteration number; both roles wait for
 * the deadline (role 1 shifted by a sweeping offset so the two bodies
 * overlap at varying distances), run the body, and role 0 classifies. */
static void litmus_participant(UINT32 part)
{
    struct pair *p = &pairs[part >> 1];
    UINT32 role = part & 1;
    UINT64 kind = cmd.kind, form = cmd.form, iters = cmd.iters, i, r, t;
    for (i = 1; i <= iters; ++i) {
        if (!role) {
            p->x = 0;
            p->y = 0;
            p->start = rd_itc() + litmus_lead;
            st_rel(&p->go, i);
            t = p->start;
        } else {
            while (ld_acq(&p->go) != i)
                cpu_pause();
            t = p->start + sweep[i & 63];
        }
        while ((INT64)(rd_itc() - t) < 0) {
        }
        r = litmus_body(kind, form, role, p);
        if (role) {
            p->res1 = r;
            st_rel(&p->done, i);
        } else {
            UINT64 o;
            while (ld_acq(&p->done) != i)
                cpu_pause();
            if (kind == C_MP)
                o = p->res1;
            else if (kind == C_SEM_SB && form == 2) {
                if ((r & 1) == 0 || (p->res1 & 1) == 0)
                    ++p->bad;
                o = (r >> 1) << 1 | (p->res1 >> 1);
            } else
                o = r << 1 | p->res1;
            ++p->hist[o & 3];
        }
    }
}

/* Lost-update counters: fetchadd, a cmpxchg spinlock and an xchg
 * spinlock around a plain counter (Part II §2.1.3, §2.4). */
static void lock_participant(UINT32 me)
{
    UINT64 kind = cmd.kind, iters = cmd.iters, i, v, contended = 0;
    barrier(cmd.nparts);
    for (i = 0; i < iters; ++i) {
        if (kind == C_FETCHADD) {
#if SMP_BROKEN_NONATOMIC
            __asm__ volatile("ld8 %0=[%1];;\n\tadds %0=1,%0;;\n\tst8 [%1]=%0;;"
                             : "=&r"(v)
                             : "r"(&fa8_counter)
                             : "memory");
            __asm__ volatile("ld4 %0=[%1];;\n\tadds %0=1,%0;;\n\tst4 [%1]=%0;;"
                             : "=&r"(v)
                             : "r"(&fa4_counter)
                             : "memory");
#else
            __asm__ volatile("fetchadd8.acq %0=[%1],1;;" : "=r"(v) : "r"(&fa8_counter) : "memory");
            __asm__ volatile("fetchadd4.rel %0=[%1],1;;" : "=r"(v) : "r"(&fa4_counter) : "memory");
#endif
            continue;
        }
        for (;;) {
#if SMP_BROKEN_NONATOMIC
            /* Test, then set: two processors can both see 0. */
            __asm__ volatile("ld8.acq %0=[%1];;" : "=r"(v) : "r"(&lock_word) : "memory");
            if (!v) {
                __asm__ volatile("st8 [%0]=%1;;" ::"r"(&lock_word), "r"(1ull) : "memory");
                break;
            }
#else
            if (kind == C_CMPXCHG_LOCK)
                __asm__ volatile("mov ar.ccv=r0;;\n\tcmpxchg8.acq %0=[%1],%2,ar.ccv;;"
                                 : "=r"(v)
                                 : "r"(&lock_word), "r"(1ull)
                                 : "memory");
            else
                __asm__ volatile("xchg8 %0=[%1],%2;;"
                                 : "=r"(v)
                                 : "r"(&lock_word), "r"(1ull)
                                 : "memory");
            if (!v)
                break;
#endif
            ++contended;
            while (lock_word)
                cpu_pause();
        }
        __asm__ volatile("ld8 %0=[%1];;\n\tadds %0=1,%0;;\n\tst8 [%1]=%0;;"
                         : "=&r"(v)
                         : "r"(&lock_counter)
                         : "memory");
        st_rel(&lock_word, 0);
    }
    slots[me].r[0] = contended;
}

/* Purge cases. Every participant maps PURGE_VA (region
 * 1, RID PURGE_RID, 8 KiB) to purge_page with a TC entry of its own, using
 * the same region register value so the BSP's ptc.ga names the entries of
 * all of them (Vol.3 ptc.g: the purge matches RR[va].rid and va). "Present"
 * is decided twice: by an access (smp_probe, a speculative load that
 * defers on a miss) and by tak. */
#define PURGE_VA 0x2000000005000000ull
#define PURGE_PS 13u
#define PURGE_RID 0x3fffcull
#define PURGE_RR (PURGE_RID << 8 | PURGE_PS << 2) /* ve 0: no VHPT walk */
#define PURGE_PTE_FLAGS 0x661ull                  /* p, WB, a, d, PL0, RWX */
#define PURGE_MAGIC 0x5350505552474531ull
enum { PURGE_P1 = 1, PURGE_ARM, PURGE_CHECK };
/* PE images are relocated on 4 KiB boundaries. Promising 8 KiB C
 * alignment lets GCC fold the runtime offset to zero after relocation. */
static volatile UINT64 purge_page[1024] __attribute__((aligned(4096)));
static volatile UINT64 purge_flag LINE;
/* The loader may place the image on any 4 KiB boundary: the entry maps the
 * 8 KiB frame holding purge_page and the probes use purge_page's offset. */
static UINT64 purge_addr(void)
{
    return PURGE_VA + ((UINT64)(UINTN)purge_page & ((1ull << PURGE_PS) - 1));
}
extern void smp_tc_insert(UINT64 va, UINT64 pte, UINT64 itir);
extern UINT64 smp_probe(UINT64 va, UINT64 *value);
extern UINT64 smp_set_rr(UINT64 va, UINT64 value);
static inline UINT64 tak(UINT64 va)
{
    UINT64 k;
    __asm__ volatile("tak %0=%1;;" : "=r"(k) : "r"(va) : "memory");
    return k;
}
static inline void ptc_l(UINT64 va)
{
    __asm__ volatile("ptc.l %0,%1;;\n\tsrlz.d;;" ::"r"(va), "r"((UINT64)PURGE_PS << 2) : "memory");
}

/* Maps PURGE_VA and uses it; r[0] = access hit, r[1] = value read, r[2] = tak. */
static void purge_arm(volatile UINT64 *r)
{
    UINT64 v = 0;
    smp_tc_insert(PURGE_VA,
                  ((UINT64)(UINTN)purge_page & ~((1ull << PURGE_PS) - 1)) | PURGE_PTE_FLAGS,
                  PURGE_PS << 2);
    r[0] = smp_probe(purge_addr(), &v);
    r[1] = v;
    r[2] = tak(purge_addr());
}
/* After the purge: r[3] = access hit (must be 0), r[4] = tak (must be 1). */
static void purge_check(volatile UINT64 *r)
{
    UINT64 v = 0;
    __asm__ volatile("srlz.d;;" ::: "memory");
    r[3] = smp_probe(purge_addr(), &v);
    r[4] = tak(purge_addr());
    ptc_l(PURGE_VA); /* leave nothing behind, whatever the result */
}
static void ap_purge(UINT32 me)
{
    struct slot *s = &slots[me];
    UINT64 kind = cmd.kind, old = smp_set_rr(PURGE_VA, PURGE_RR);
    if (kind != PURGE_CHECK)
        purge_arm(s->r);
    if (kind == PURGE_P1) {
        /* Running and spinning while the BSP purges. */
        st_rel(&s->ack, cmd.seq);
        while (ld_acq(&purge_flag) != cmd.seq)
            cpu_pause();
    }
    if (kind != PURGE_ARM)
        purge_check(s->r);
    /* PURGE_ARM leaves its entry; it is tagged with PURGE_RID and stays
     * findable when PURGE_CHECK restores that RID after the halt. */
    smp_set_rr(PURGE_VA, old);
}

/* The OS_BOOT_RENDEZ handler proper, entered from smp_ap_entry with the
 * firmware's b0, gp and stack. It must return: the firmware waits for the
 * next wake (SAL §3.2.2.1), which is how every case reaches its APs. */
void smp_ap_main(void);
void smp_ap_main(void)
{
    UINT64 lid = rd_lid() & 0xffff0000ull;
    UINT64 seq = ld_acq(&cmd.seq), op = cmd.op;
    int me = cpu_index(lid);
#if SMP_BROKEN_NOCALLIN
    if (me == (int)ncpu - 1 && me != (int)bsp)
        return; /* the last AP never calls in */
#endif
#if SMP_BROKEN_REPEAT
    /* The last AP ignores the second repeat-rendezvous wake (sequence
     * numbers: one per AP in ap-start, then one per round). */
    if (me == (int)ncpu - 1 && op == OP_CALLIN && seq == ncpu + 1)
        return;
#endif
    if (op == OP_CALLIN) {
        UINT64 n = fetchadd8_acq(&callin_count);
        if (n < CALLIN_LOG) {
#if SMP_BROKEN_LID
            callin_log[n].lid = lid ^ (1ull << 24); /* reports the wrong processor */
#else
            callin_log[n].lid = lid;
#endif
            callin_log[n].seq = seq;
            st_rel(&callin_log[n].valid, 1);
        }
    }
    if (me < 0)
        return;
    st_rel(&slots[me].entered, seq);
    switch (op) {
    case OP_IPI:
        ap_ipi((UINT32)me);
        break;
    case OP_ITC:
        ap_itc((UINT32)me);
        break;
    case OP_LITMUS:
        litmus_participant((UINT32)slots[me].part);
        break;
    case OP_LOCK:
        lock_participant((UINT32)me);
        break;
    case OP_PURGE:
        ap_purge((UINT32)me);
        break;
    case OP_SPIN:
        for (;;) {
            fetchadd8_acq(&spin_count);
            cpu_pause();
        }
    default:
        break;
    }
    st_rel(&slots[me].done, seq);
}
extern void smp_ap_entry(void);

/* ---- BSP side: rounds ---------------------------------------------------- */
static UINT64 issue(UINT64 op, const UINT32 *targets, UINT32 n)
{
    UINT32 i;
    UINT64 s = ++seq_next;
    cmd.op = op;
    st_rel(&cmd.seq, s);
    for (i = 0; i < n; ++i)
        send_ipi(targets[i], wake_vector);
    return s;
}
/* Returns the first target that did not set *field to s in time, or -1. */
static int wait_seq(const UINT32 *targets, UINT32 n, UINT64 s, int done_field, UINT64 ticks)
{
    UINT64 t0 = rd_itc();
    UINT32 i;
    for (i = 0; i < n; ++i) {
        volatile UINT64 *f = done_field ? &slots[targets[i]].done : &slots[targets[i]].entered;
        while (ld_acq(f) != s) {
            if (since(t0) > (INT64)ticks)
                return (int)targets[i];
            cpu_pause();
        }
    }
    return -1;
}

/* APs of the current started set, excluding the BSP. */
static UINT32 aps(UINT32 *out, UINT32 max)
{
    UINT32 i, n = 0;
    for (i = 0; i < max; ++i)
        out[i] = 0;
    for (i = 1; i < nstarted && n < max; ++i)
        out[n++] = started[i];
    return n;
}

static int ap_case_skip(const char *name, UINT32 need)
{
    if (ncpu < 2) {
        skip(name, "1 CPU");
        return 1;
    }
    if (ap_lost) {
        skip(name, "an AP was lost in an earlier case");
        return 1;
    }
    if (nstarted < 2) {
        skip(name, "ap-start failed");
        return 1;
    }
    if (nstarted < need) {
        put("SKIP ");
        put(name);
        put(" ");
        put_u(nstarted);
        put(" CPUs (needs ");
        put_u(need);
        put(")");
        flush_line();
        ++n_skip;
        case_skipped = 1;
        return 1;
    }
    return 0;
}

static void fail_timeout(const char *name, const char *what, int cpu)
{
    fail_begin(name);
    put(what);
    put(" timeout cpu=");
    put_u((UINT64)cpu);
    put(" lid=");
    put_x(cpus[cpu].lid);
    fail_end();
    ap_lost = 1;
}

/* ---- cases ------------------------------------------------------------------ */
static void case_sal_ap_wake(const char *name)
{
    static const char *const why[] = {"",
                                      "no SAL system table",
                                      "bad SAL header",
                                      "bad SAL checksum",
                                      "bad SAL descriptor list",
                                      "no SAL entrypoint descriptor"};
#if SMP_BROKEN_WAKE_VECTOR
    const UINT64 expect = 0xfe;
#else
    const UINT64 expect = 0xff;
#endif
    info_begin(name);
    kv_u("descriptors", fw.wake_descriptors);
    kv_u("mechanism", fw.wake_mechanism);
    kv_x("vector", wake_vector);
    kv_x("sal_proc", fw.sal_proc);
    kv_x("pal_proc", fw.pal_proc);
    flush_line();
    if (fw.sal_status != SAL_OK)
        fail(name, why[fw.sal_status]);
    else if (fw.wake_descriptors != 1)
        fail(name, "not exactly one type-5 descriptor");
    else if (fw.wake_mechanism != 0)
        fail(name, "mechanism is not 0 (external interrupt)");
    else if (wake_vector != expect) {
        fail_begin(name);
        put("vector=");
        put_x(wake_vector);
        fail_end();
    }
}

static void case_set_vectors(const char *name)
{
    struct sal_ret {
        UINT64 status, v0, v1, v2;
    } ret;
    typedef struct sal_ret (*sal_fn)(UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64,
                                     UINT64);
    static UINT64 sal_desc[2] __attribute__((aligned(16)));
    const UINT64 *handler = (const UINT64 *)(UINTN)smp_ap_entry;
    EFI_GUID lip = LOADED_IMAGE_PROTOCOL;
    EFI_LOADED_IMAGE *li = NULL;
    UINT64 base = 0, size = 0, type = SAL_VECTOR_OS_BOOT_RENDEZ, fn;

    if (fw.sal_status != SAL_OK) {
        fail(name, "no SAL entrypoint");
        return;
    }
    if (gST->BootServices->HandleProtocol(gImage, &lip, (void **)&li) == EFI_SUCCESS && li) {
        base = (UINT64)(UINTN)li->ImageBase;
        size = li->ImageSize;
    }
    info_begin(name);
    kv_x("entry", handler[0]);
    kv_x("gp", handler[1]);
    kv_x("live_gp", rd_gp());
    kv_x("image", base);
    kv_x("image_size", size);
    flush_line();
    /* The descriptor must be this image's: entry inside it, gp ours. */
    if (handler[1] != rd_gp() || handler[0] < base || handler[0] - base >= size) {
        fail(name, "handler descriptor does not belong to this image");
        return;
    }
#if SMP_BROKEN_SET_VECTORS
    type = 99; /* no such vector type: SAL must refuse it */
#endif
    sal_desc[0] = fw.sal_proc;
    sal_desc[1] = fw.sal_gp;
    /* An IA-64 function pointer is the address of an {entry, gp} pair; the
     * empty asm keeps GCC from folding the call into a direct branch to
     * the data symbol. */
    fn = (UINT64)(UINTN)sal_desc;
    __asm__ volatile("" : "+r"(fn));
    /* SAL 245359-005 §9 SAL_SET_VECTORS with length_cs = 0. */
    ret = ((sal_fn)(UINTN)fn)(SAL_SET_VECTORS, type, handler[0], handler[1], 0, 0, 0, 0);
    if (ret.status != 0) {
        fail_begin(name);
        put("status=");
        put_i((INT64)ret.status);
        fail_end();
        return;
    }
    registered = 1;
    registered_entry = handler[0];
    registered_gp = handler[1];
}

static void case_madt_exact(const char *name)
{
    static const char *const why[] = {"", "no SMBIOS table", "bad SMBIOS entry point",
                                      "bad SMBIOS structure list"};
    UINT64 smbios = fw.smbios_cpus;
#if SMP_BROKEN_SMBIOS
    smbios += 1;
#endif
    info_begin(name);
    kv_u("lsapic", fw.madt_lsapic);
    kv_u("enabled", ncpu + fw.madt_overflow);
    kv_u("disabled", fw.madt_disabled);
    kv_u("smbios", smbios);
    kv_u("smbios_records", fw.smbios_records);
    kv_x("pib", fw.pib);
    kv_u("pib_override", (UINT64)fw.pib_override);
    flush_line();
    if (!fw.madt)
        fail(name, "no MADT");
    else if (fw.madt_disabled) {
        fail_begin(name);
        put_u(fw.madt_disabled);
        put(" disabled Local SAPIC entries");
        fail_end();
    } else if (fw.smbios_status)
        fail(name, why[fw.smbios_status]);
    else if (smbios != ncpu + fw.madt_overflow) {
        fail_begin(name);
        put("smbios=");
        put_u(smbios);
        put(" madt=");
        put_u(ncpu + fw.madt_overflow);
        fail_end();
    }
}

static void case_ap_start(const char *name)
{
    UINT32 i;
    nstarted = 0;
    if (ncpu < 2) {
        skip(name, "1 CPU");
        return;
    }
    if (bsp == ~0u) {
        fail(name, "BSP cr.lid not in the MADT");
        return;
    }
    if (!registered) {
        fail(name, "no OS_BOOT_RENDEZ registration");
        return;
    }
    if (!fw.lid_ids_unique) {
        fail(name, "MADT LID ids not unique");
        return;
    }
    if (fw.wake_descriptors != 1 || fw.wake_mechanism != 0 || wake_vector < 16 ||
        wake_vector > 255) {
        fail(name, "no usable SAL wake vector");
        return;
    }
    started[nstarted++] = bsp;
    for (i = 0; i < ncpu; ++i) {
        UINT64 base = callin_count, s, t0;
        UINT32 target = i;
        int late;
        if (i == bsp)
            continue;
        t0 = rd_itc();
        s = issue(OP_CALLIN, &target, 1);
        /* Within 1 s of ITC time: the first call-in after the store. */
        while (ld_acq(&callin_count) == base && since(t0) <= (INT64)itc_hz)
            cpu_pause();
        if (ld_acq(&callin_count) == base) {
            fail_begin(name);
            put("no call-in cpu=");
            put_u(i);
            put(" lid=");
            put_x(cpus[i].lid);
            fail_end();
            continue;
        }
        while (!ld_acq(&callin_log[base].valid) && since(t0) <= (INT64)itc_hz)
            cpu_pause();
        if (callin_log[base].lid != cpus[i].lid || callin_log[base].seq != s) {
            fail_begin(name);
            put("lid mismatch cpu=");
            put_u(i);
            put(" expected=");
            put_x(cpus[i].lid);
            put(" reported=");
            put_x(callin_log[base].lid);
            fail_end();
            /* Wait for that handler to finish before the next wake. */
            (void)wait_seq(&target, 1, s, 1, itc_hz);
            continue;
        }
        late = wait_seq(&target, 1, s, 1, itc_hz);
        if (late >= 0) {
            fail_timeout(name, "return", late);
            return;
        }
        first_entry[i] = smp_raw[(cpus[i].lid >> 24) & 0xff];
        info_begin(name);
        kv_u("cpu", i);
        kv_x("lid", cpus[i].lid);
        kv_i("callin_ticks", (INT64)(first_entry[i].itc - t0));
        flush_line();
        started[nstarted++] = i;
    }
}

static void case_ap_entry_state(const char *name)
{
    UINT32 i, j, ap[SMP_MAX_CPUS], n;
    UINT64 expect_gp = registered_gp;
#if SMP_BROKEN_ENTRY_GP
    expect_gp += 8;
#endif
    if (ap_case_skip(name, 2))
        return;
    n = aps(ap, SMP_MAX_CPUS);
    for (i = 0; i < n; ++i) {
        const struct smp_raw *r = &first_entry[ap[i]];
        info_begin(name);
        kv_u("cpu", ap[i]);
        kv_x("psr", r->psr);
        kv_u("ic", !!(r->psr & PSR_IC));
        kv_u("i", !!(r->psr & PSR_I));
        kv_u("dt", !!(r->psr & PSR_DT));
        kv_u("rt", !!(r->psr & PSR_RT));
        kv_u("it", !!(r->psr & PSR_IT));
        kv_u("ac", !!(r->psr & PSR_AC));
        kv_u("dfl", !!(r->psr & PSR_DFL));
        kv_u("dfh", !!(r->psr & PSR_DFH));
        flush_line();
        info_begin(name);
        kv_u("cpu", ap[i]);
        kv_x("gp", r->gp);
        kv_x("sp", r->sp);
        kv_x("b0", r->b0);
        kv_x("bsp", r->bsp);
        kv_x("bspstore", r->bspstore);
        kv_x("rsc", r->rsc);
        kv_x("pfs", r->pfs);
        kv_x("tpr", r->tpr);
        kv_x("iva", r->iva);
        flush_line();
        if (r->psr & (PSR_DT | PSR_RT | PSR_IT)) {
            fail_begin(name);
            put("not physical mode cpu=");
            put_u(ap[i]);
            fail_end();
        }
        if (r->psr & PSR_I) {
            fail_begin(name);
            put("PSR.i set cpu=");
            put_u(ap[i]);
            fail_end();
        }
        if (r->gp != expect_gp) {
            fail_begin(name);
            put("gp=");
            put_x(r->gp);
            put(" registered=");
            put_x(expect_gp);
            put(" cpu=");
            put_u(ap[i]);
            fail_end();
        }
        if ((r->sp & 15) || !in_ram(r->sp - 16, 32)) {
            fail_begin(name);
            put("sp=");
            put_x(r->sp);
            put(" not 16-byte aligned in RAM cpu=");
            put_u(ap[i]);
            fail_end();
        }
        if (!r->b0 || (r->b0 & 15) || !in_ram(r->b0, 16)) {
            fail_begin(name);
            put("b0=");
            put_x(r->b0);
            put(" no return bundle in RAM cpu=");
            put_u(ap[i]);
            fail_end();
        }
        for (j = 0; j < i; ++j) {
            const struct smp_raw *q = &first_entry[ap[j]];
            if (q->sp == r->sp || q->bspstore == r->bspstore) {
                fail_begin(name);
                put("cpus ");
                put_u(ap[j]);
                put(" and ");
                put_u(ap[i]);
                put(" share a stack or backing store");
                fail_end();
            }
        }
    }
}

static void case_repeat(const char *name)
{
    UINT32 ap[SMP_MAX_CPUS], n, round, i, k;
    UINT64 before[SMP_MAX_CPUS];
    int bad = 0;
    if (ap_case_skip(name, 2))
        return;
    n = aps(ap, SMP_MAX_CPUS);
    for (round = 1; round <= SMP_REPEAT_ROUNDS && !bad; ++round) {
        UINT64 base = callin_count, s, t0 = rd_itc();
        int late;
        for (i = 0; i < n; ++i)
            before[i] = smp_raw[(cpus[ap[i]].lid >> 24) & 0xff].entries;
        s = issue(OP_CALLIN, ap, n);
        while (ld_acq(&callin_count) < base + n && since(t0) <= (INT64)itc_hz)
            cpu_pause();
        late = wait_seq(ap, n, s, 1, itc_hz);
        if (late >= 0 || ld_acq(&callin_count) != base + n) {
            fail_begin(name);
            put("round ");
            put_u(round);
            put(": ");
            put_u(ld_acq(&callin_count) - base);
            put(" of ");
            put_u(n);
            put(" APs called in");
            fail_end();
            bad = 1;
            ap_lost = 1;
            break;
        }
        /* Each started AP exactly once, nobody else. */
        for (i = 0; i < n; ++i) {
            UINT32 seen = 0;
            for (k = 0; k < n; ++k)
                if (callin_log[base + k].lid == cpus[ap[i]].lid && callin_log[base + k].seq == s)
                    ++seen;
            if (seen != 1 || smp_raw[(cpus[ap[i]].lid >> 24) & 0xff].entries != before[i] + 1) {
                fail_begin(name);
                put("round ");
                put_u(round);
                put(" cpu=");
                put_u(ap[i]);
                put(" call-ins=");
                put_u(seen);
                put(" entries+=");
                put_u(smp_raw[(cpus[ap[i]].lid >> 24) & 0xff].entries - before[i]);
                fail_end();
                bad = 1;
            }
        }
    }
}

static void case_ipi(const char *name)
{
    UINT32 t[2];
    UINT64 s;
    int late;
    struct slot *a, *b;
    if (ap_case_skip(name, 3))
        return;
    t[0] = started[1];
    t[1] = started[2];
    cmd.a = t[0];
    cmd.b = t[1];
    cmd.iters = SMP_IPI_ROUNDS;
    slots[t[0]].ack = slots[t[1]].ack = 0;
    s = issue(OP_IPI, t, 2);
    late = wait_seq(t, 2, s, 1, itc_hz * 120);
    if (late >= 0) {
        fail_timeout(name, "done", late);
        return;
    }
    a = &slots[t[0]];
    b = &slots[t[1]];
    info_begin(name);
    kv_u("rounds", a->r[0]);
    kv_u("answered", b->r[0]);
    kv_u("vector", SMP_IPI_VECTOR);
    if (a->r[0]) {
        kv_u("rtt_min", a->r[4]);
        kv_u("rtt_max", a->r[5]);
        kv_u("rtt_mean", a->r[6] / a->r[0]);
    }
    kv_u("strays", a->r[2] + b->r[2]);
    kv_u("dups", a->r[3] + b->r[3]);
    flush_line();
    if (a->r[1] || b->r[1] || a->r[0] != SMP_IPI_ROUNDS) {
        fail_begin(name);
        put("lost IPI: a completed ");
        put_u(a->r[0]);
        put(" of ");
        put_u(SMP_IPI_ROUNDS);
        put(" rounds, stopped at round ");
        put_u(a->r[1]);
        put(", b answered ");
        put_u(b->r[0]);
        fail_end();
    } else if (a->r[3] || b->r[3]) {
        fail_begin(name);
        put("duplicated IPI: ");
        put_u(a->r[3] + b->r[3]);
        fail_end();
    }
}

static void case_itc(const char *name)
{
    UINT32 ap[SMP_MAX_CPUS], n, i;
    UINT64 s, k, spread_min = ~0ull, spread_max = 0, spread_sum = 0, oneway = 0;
    int late = -1, bad = 0;
    if (ap_case_skip(name, 2))
        return;
    n = aps(ap, SMP_MAX_CPUS);
    itc_release = 0;
    for (i = 0; i < n; ++i)
        slots[ap[i]].ack = slots[ap[i]].ping = slots[ap[i]].pong = 0;
    s = issue(OP_ITC, ap, n);
    /* Phase 1: common release (the BSP's own reading follows its st.rel). */
    for (k = 1; k <= SMP_ITC_RELEASES && late < 0; ++k) {
        UINT64 t_rel = rd_itc(), t_b, lo, hi;
        st_rel(&itc_release, k);
        t_b = rd_itc();
        lo = hi = t_b;
        for (i = 0; i < n; ++i) {
            UINT64 t0 = rd_itc(), t;
            while (ld_acq(&slots[ap[i]].ack) != k) {
                if (since(t0) > (INT64)(itc_hz * 10)) {
                    late = (int)ap[i];
                    break;
                }
                cpu_pause();
            }
            if (late >= 0)
                break;
            t = slots[ap[i]].t;
            if ((INT64)(t - t_rel) < 0)
                ++oneway; /* saw the flag before it was set */
            if ((INT64)(t - lo) < 0)
                lo = t;
            if ((INT64)(t - hi) > 0)
                hi = t;
        }
        if (late >= 0)
            break;
        spread_sum += hi - lo;
        if (hi - lo < spread_min)
            spread_min = hi - lo;
        if (hi - lo > spread_max)
            spread_max = hi - lo;
    }
    if (late >= 0) {
        fail_timeout(name, "release", late);
        return;
    }
    /* Phase 2: round trips, one AP after the other. */
    for (i = 0; i < n && late < 0; ++i) {
        struct slot *sl = &slots[ap[i]];
        for (k = 1; k <= SMP_ITC_TRIPS; ++k) {
            UINT64 t0 = rd_itc();
            while (ld_acq(&sl->ping) != k) {
                if (since(t0) > (INT64)(itc_hz * 10)) {
                    late = (int)ap[i];
                    break;
                }
                cpu_pause();
            }
            if (late >= 0)
                break;
            sl->tm = rd_itc();
            st_rel(&sl->pong, k);
        }
    }
    if (late >= 0) {
        fail_timeout(name, "round trip", late);
        return;
    }
    late = wait_seq(ap, n, s, 1, itc_hz * 10);
    if (late >= 0) {
        fail_timeout(name, "done", late);
        return;
    }
    info_begin(name);
    kv_u("releases", SMP_ITC_RELEASES);
    kv_u("spread_min", spread_min);
    kv_u("spread_max", spread_max);
    kv_u("spread_mean", spread_sum / SMP_ITC_RELEASES);
    kv_u("oneway_violations", oneway);
    kv_u("hz", itc_hz);
    flush_line();
    for (i = 0; i < n; ++i) {
        struct slot *sl = &slots[ap[i]];
        info_begin(name);
        kv_u("cpu", ap[i]);
        kv_u("trips", SMP_ITC_TRIPS);
        kv_u("violations", sl->r[0]);
        kv_u("rtt_min", sl->r[1]);
        kv_i("offset_at_rtt_min", (INT64)sl->r[2]);
        kv_i("worst", (INT64)sl->r[3]);
        flush_line();
        if (sl->r[0])
            bad = 1;
    }
    if (oneway || bad) {
        fail_begin(name);
        put("ITC not one time base: ");
        put_u(oneway);
        put(" one-way and ");
        for (k = 0, i = 0; i < n; ++i)
            k += slots[ap[i]].r[0];
        put_u(k);
        put(" round-trip causality violations");
        fail_end();
    }
}

static void case_litmus(const char *name, UINT64 kind, UINT64 form, const char *label,
                        UINT32 forbidden)
{
    UINT32 t[SMP_MAX_CPUS], nparts = nstarted & ~1u, npairs = nparts >> 1, i;
    UINT64 s, hist[4] = {0, 0, 0, 0}, bad = 0, unit;
    int late;
    for (i = 0; i < npairs; ++i) {
        struct pair *p = &pairs[i];
        p->go = p->done = p->x = p->y = p->res1 = p->bad = 0;
        p->hist[0] = p->hist[1] = p->hist[2] = p->hist[3] = 0;
    }
    /* Role 1 waits for the deadline shifted by -32..+31 steps of about 5 ns. */
    unit = itc_hz / 200000000u;
    if (!unit)
        unit = 1;
    for (i = 0; i < 64; ++i)
        sweep[i] = (UINT64)(((INT64)i - 32) * (INT64)unit);
    litmus_lead = itc_hz / 100000u; /* 10 us: role 1 sees go before the deadline */
    cmd.kind = kind;
    cmd.form = form;
    cmd.iters = SMP_ITERS;
    for (i = 0; i < SMP_MAX_CPUS; ++i)
        t[i] = 0;
    for (i = 1; i < nparts; ++i) {
        t[i - 1] = started[i];
        slots[started[i]].part = i;
    }
    s = issue(OP_LITMUS, t, nparts - 1);
    litmus_participant(0);
    late = wait_seq(t, nparts - 1, s, 1, itc_hz * 600);
    if (late >= 0) {
        fail_timeout(name, "done", late);
        return;
    }
    for (i = 0; i < npairs; ++i) {
        hist[0] += pairs[i].hist[0];
        hist[1] += pairs[i].hist[1];
        hist[2] += pairs[i].hist[2];
        hist[3] += pairs[i].hist[3];
        bad += pairs[i].bad;
    }
    info_begin(name);
    put(" form=");
    put(label);
    kv_u("iters", SMP_ITERS);
    kv_u("pairs", npairs);
    kv_u("o00", hist[0]);
    kv_u("o01", hist[1]);
    kv_u("o10", hist[2]);
    kv_u("o11", hist[3]);
    put(" forbidden=o");
    put_u(forbidden >> 1);
    put_u(forbidden & 1);
    if (kind == C_SEM_SB && form == 2)
        kv_u("xchg_not_own", bad);
    flush_line();
    if (hist[forbidden] || bad) {
        fail_begin(name);
        put(label);
        put(": forbidden outcome o");
        put_u(forbidden >> 1);
        put_u(forbidden & 1);
        put(" seen ");
        put_u(hist[forbidden]);
        put(" times in ");
        put_u(SMP_ITERS * npairs);
        if (bad) {
            put(", own xchg missed own st.rel ");
            put_u(bad);
        }
        fail_end();
    }
}

static void case_lock(const char *name, UINT64 kind)
{
    UINT32 ap[SMP_MAX_CPUS], n, i;
    UINT64 s, expect, contended = 0;
    int late;
    n = aps(ap, SMP_MAX_CPUS);
    bar = 0;
    lock_word = lock_counter = fa8_counter = 0;
    fa4_counter = 0;
    cmd.kind = kind;
    cmd.iters = SMP_ITERS;
    cmd.nparts = n + 1;
    s = issue(OP_LOCK, ap, n);
    lock_participant(bsp);
    late = wait_seq(ap, n, s, 1, itc_hz * 600);
    if (late >= 0) {
        fail_timeout(name, "done", late);
        return;
    }
    for (i = 0; i < nstarted; ++i)
        contended += slots[started[i]].r[0];
    expect = (UINT64)SMP_ITERS * nstarted;
    info_begin(name);
    kv_u("cpus", nstarted);
    kv_u("per_cpu", SMP_ITERS);
    kv_u("expected", expect);
    if (kind == C_FETCHADD) {
        kv_u("fetchadd8", fa8_counter);
        kv_u("fetchadd4", fa4_counter);
    } else {
        kv_u("counter", lock_counter);
        kv_u("contended", contended);
    }
    flush_line();
    if (kind == C_FETCHADD ? (fa8_counter != expect || fa4_counter != (UINT32)expect)
                           : (lock_counter != expect || lock_word != 0)) {
        fail_begin(name);
        put("lost updates: ");
        put_u(expect - (kind == C_FETCHADD ? fa8_counter : lock_counter));
        put(" of ");
        put_u(expect);
        fail_end();
    }
}

static void global_purge(UINT64 seq)
{
    UINT64 old = smp_set_rr(PURGE_VA, PURGE_RR);
#if SMP_BROKEN_PURGE_LOCAL
    ptc_l(PURGE_VA); /* forgets the other processors */
#else
    /* ptc.ga ends its group; the st.rel after it is the release Part II
     * §5.2.2.2.3 requires before remote processors are guaranteed to have
     * dropped the translation. */
    __asm__ volatile("ptc.ga %0,%1;;\n\tsrlz.d;;" ::"r"(PURGE_VA), "r"((UINT64)PURGE_PS << 2)
                     : "memory");
#endif
    st_rel(&purge_flag, seq);
    smp_set_rr(PURGE_VA, old);
}

static void purge_judge(const char *name, UINT32 cpu, volatile UINT64 *r, int armed)
{
    if (armed && (r[0] != 1 || r[1] != PURGE_MAGIC || r[2] != 0)) {
        fail_begin(name);
        put("cpu=");
        put_u(cpu);
        put(" mapping not usable before the purge: hit=");
        put_u(r[0]);
        put(" value=");
        put_x(r[1]);
        put(" tak=");
        put_x(r[2]);
        fail_end();
    } else if (r[3] != 0 || r[4] != 1) {
        fail_begin(name);
        put("cpu=");
        put_u(cpu);
        put(" stale translation after ptc.ga: access hit=");
        put_u(r[3]);
        put(" tak=");
        put_x(r[4]);
        fail_end();
    }
}

/* Purge of a running mapping: B uses a TC mapping; A purges with ptc.ga, then st.rel; B ld.acq,
 * srlz.d, accesses: the access must miss on every B and on A itself. */
static void case_purge_remote(const char *name)
{
    UINT32 ap[SMP_MAX_CPUS], n, i;
    UINT64 s, r[5] = {0, 0, 0, 0, 0}, old, hits = 0;
    int late = -1;
    if (ap_case_skip(name, 2))
        return;
    n = aps(ap, SMP_MAX_CPUS);
    purge_page[0] = PURGE_MAGIC;
    info_begin(name);
    kv_x("page_pa", (UINT64)(UINTN)purge_page);
    kv_x("physical_value", purge_page[0]);
    kv_x("probe_va", purge_addr());
    flush_line();
    purge_flag = 0;
    for (i = 0; i < n; ++i)
        slots[ap[i]].ack = 0;
    cmd.kind = PURGE_P1;
    s = issue(OP_PURGE, ap, n);
    for (i = 0; i < n && late < 0; ++i) {
        UINT64 t0 = rd_itc();
        while (ld_acq(&slots[ap[i]].ack) != s) {
            if (since(t0) > (INT64)(itc_hz * 10)) {
                late = (int)ap[i];
                break;
            }
            cpu_pause();
        }
    }
    old = smp_set_rr(PURGE_VA, PURGE_RR);
    purge_arm(r);
    smp_set_rr(PURGE_VA, old);
    if (late >= 0) {
        st_rel(&purge_flag, s);
        fail_timeout(name, "armed", late);
        return;
    }
    global_purge(s);
    old = smp_set_rr(PURGE_VA, PURGE_RR);
    purge_check(r);
    smp_set_rr(PURGE_VA, old);
    late = wait_seq(ap, n, s, 1, itc_hz * 10);
    if (late >= 0) {
        fail_timeout(name, "done", late);
        return;
    }
    info_begin(name);
    kv_u("cpus", n + 1);
    kv_u("source_hit_before", r[0]);
    kv_u("source_hit_after", r[3]);
    for (i = 0; i < n; ++i)
        hits += slots[ap[i]].r[3];
    kv_u("remote_hits_after", hits);
    flush_line();
    purge_judge(name, bsp, r, 1);
    for (i = 0; i < n; ++i)
        purge_judge(name, ap[i], slots[ap[i]].r, 1);
}

/* Purge of a halted processor's mapping: the APs map and use PURGE_VA, return to the firmware and halt in
 * PAL_HALT_LIGHT; the BSP purges; the wake that follows must find the
 * entries gone. A control round without the purge first shows that an
 * entry survives the halt on this platform (TC entries may be dropped at
 * any time: the processor alone controls their replacement, Vol.2 §4.1.1.2;
 * that would make the check vacuous). */
static int purge_round(const char *name, const UINT32 *ap, UINT32 n, int purge)
{
    UINT64 s, t0;
    int late;
    UINT32 i;
    cmd.kind = PURGE_ARM;
    s = issue(OP_PURGE, ap, n);
    late = wait_seq(ap, n, s, 1, itc_hz * 10);
    if (late >= 0) {
        fail_timeout(name, "arm", late);
        return -1;
    }
    t0 = rd_itc();
    while (since(t0) < (INT64)(itc_hz / 100))
        cpu_pause(); /* 10 ms: halted by now */
    if (purge)
        global_purge(s);
    cmd.kind = PURGE_CHECK;
    s = issue(OP_PURGE, ap, n);
    late = wait_seq(ap, n, s, 1, itc_hz * 10);
    if (late >= 0) {
        fail_timeout(name, "check", late);
        return -1;
    }
    for (i = 0; i < n; ++i)
        if (slots[ap[i]].r[0] != 1 || slots[ap[i]].r[1] != PURGE_MAGIC) {
            purge_judge(name, ap[i], slots[ap[i]].r, 1);
            return -1;
        }
    return 0;
}
static void case_purge_halted(const char *name)
{
    UINT32 ap[SMP_MAX_CPUS], n, i, kept = 0;
    if (ap_case_skip(name, 2))
        return;
    n = aps(ap, SMP_MAX_CPUS);
    purge_page[0] = PURGE_MAGIC;
    info_begin(name);
    kv_x("page_pa", (UINT64)(UINTN)purge_page);
    kv_x("physical_value", purge_page[0]);
    kv_x("probe_va", purge_addr());
    flush_line();
    if (purge_round(name, ap, n, 0))
        return;
    for (i = 0; i < n; ++i)
        kept += slots[ap[i]].r[3] == 1 && slots[ap[i]].r[4] == 0;
    info_begin(name);
    kv_u("control_kept", kept);
    kv_u("aps", n);
    if (kept != n) {
        flush_line();
        skip(name, "TC entries do not survive the firmware rendezvous here (control round)");
        return;
    }
    if (purge_round(name, ap, n, 1)) {
        flush_line();
        return;
    }
    for (i = 0, kept = 0; i < n; ++i)
        kept += slots[ap[i]].r[3];
    kv_u("hits_after_purge", kept);
    flush_line();
    for (i = 0; i < n; ++i)
        purge_judge(name, ap[i], slots[ap[i]].r, 1);
}

/* ---- main -------------------------------------------------------------------- */
static const char *const names[C_COUNT] = {
    "sal-ap-wake",
    "set-vectors-boot-rendez",
    "madt-exact",
    "ap-start",
    "ap-entry-state",
    "repeat-rendezvous",
    "ipi-ping-pong",
    "itc-skew",
    "mp",
    "sb",
    "sem-sb",
    "fetchadd",
    "cmpxchg-lock",
    "xchg-lock",
    "purge-remote",
    "purge-halted",
};

static void run_case(UINT32 c)
{
    const char *name = names[c];
    switch (c) {
    case C_SAL_AP_WAKE:
        case_sal_ap_wake(name);
        break;
    case C_SET_VECTORS:
        case_set_vectors(name);
        break;
    case C_MADT_EXACT:
        case_madt_exact(name);
        break;
    case C_AP_START:
        case_ap_start(name);
        break;
    case C_AP_ENTRY_STATE:
        case_ap_entry_state(name);
        break;
    case C_REPEAT:
        case_repeat(name);
        break;
    case C_IPI:
        case_ipi(name);
        break;
    case C_ITC:
        case_itc(name);
        break;
    case C_PURGE_REMOTE:
        case_purge_remote(name);
        break;
    case C_PURGE_HALTED:
        case_purge_halted(name);
        break;
    default:
        if (ap_case_skip(name, 2))
            return;
        if (c == C_MP)
            case_litmus(name, C_MP, 0, "st.rel/ld.acq", 2);
        else if (c == C_SB)
            case_litmus(name, C_SB, 0, "st/mf/ld", 0);
        else if (c == C_SEM_SB) {
            case_litmus(name, C_SEM_SB, 0, "cmpxchg1.rel", 0);
            if (!ap_lost)
                case_litmus(name, C_SEM_SB, 1, "cmpxchg1.acq", 0);
            if (!ap_lost)
                case_litmus(name, C_SEM_SB, 2, "st.rel/xchg8/ld", 0);
        } else
            case_lock(name, c);
        break;
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    UINT32 c, i, j;
    UINT64 lid = rd_lid() & 0xffff0000ull, t0, psr = rd_psr();
    UINT32 cases = SMP_CASES;

    gST = st;
    gImage = image;
    st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
    put("SMP-APP BEGIN profile=");
    put(SMP_PROFILE);
    kv_x("cases", cases);
    kv_u("iters", SMP_ITERS);
    kv_u("ipi_rounds", SMP_IPI_ROUNDS);
    flush_line();

    /* ITC rate: a 100 ms Stall measured in ITC ticks. */
    t0 = rd_itc();
    st->BootServices->Stall(100000);
    itc_hz = (rd_itc() - t0) * 10;
    if (!itc_hz)
        itc_hz = 1;

    discover_madt();
    discover_sal();
    discover_smbios();
    discover_memmap();
    for (i = 0; i < ncpu; ++i) {
        /* The PIB is uncacheable (SDM Vol.2 §5.8.4); physical mode reaches
         * it with bit 63 set. */
        cpus[i].pib =
            (fw.pib + ((cpus[i].lid >> 24 & 0xff) << 12 | (cpus[i].lid >> 16 & 0xff) << 4)) |
            ((psr & PSR_DT) ? 0 : PHYS_UC);
        if (cpus[i].lid == lid)
            bsp = i;
    }
    fw.lid_ids_unique = 1;
    for (i = 0; i < ncpu; ++i)
        for (j = 0; j < i; ++j)
            if ((cpus[i].lid >> 24) == (cpus[j].lid >> 24))
                fw.lid_ids_unique = 0;
    info_begin("env");
    kv_u("cpus", ncpu);
    kv_x("bsp_lid", lid);
    kv_u("bsp_index", bsp);
    kv_x("bsp_psr", psr);
    kv_u("itc_hz", itc_hz);
    kv_u("memmap", memmap_size != 0);
    flush_line();

    for (c = 0; c < C_COUNT; ++c) {
        /* Prerequisites run whenever a later case needs them. */
        UINT32 need = cases;
        if (cases & ~CASES_BOOT)
            need |= BIT(C_SET_VECTORS) | BIT(C_AP_START);
        if (need & BIT(c)) {
            run_case(c);
            case_end(names[c]);
        }
    }
    put("SMP-APP END");
    kv_u("pass", n_pass);
    kv_u("fail", n_fail);
    kv_u("skip", n_skip);
    kv_u("cpus", ncpu);
    flush_line();
#ifdef SMP_RESTART
    /* The first boot ends with ResetSystem(EfiResetWarm) while the APs run
     * a busy loop of this application; the next boot repeats the cases on
     * APs that start reset-held again. A nonvolatile variable tells the
     * boots apart. */
    {
        static EFI_GUID guid = {
            0x2e8d4f60, 0x7c11, 0x4b2a, {0x8f, 0x3d, 0x61, 0x5a, 0x0c, 0x92, 0xe4, 0x17}};
        UINT32 attributes = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                            EFI_VARIABLE_RUNTIME_ACCESS,
               value = 0, found;
        UINTN size = sizeof(value);
        if (st->RuntimeServices->GetVariable(L"SmpRestart", &guid, &found, &size, &value) ==
            EFI_NOT_FOUND) {
            value = 1;
            if (EFI_ERROR(st->RuntimeServices->SetVariable(L"SmpRestart", &guid, attributes,
                                                           sizeof(value), &value)))
                return EFI_DEVICE_ERROR;
            UINT32 ap[SMP_MAX_CPUS], n = aps(ap, ncpu);
            UINT64 spins = 0;
            if (n) {
                UINT64 s = issue(OP_SPIN, ap, n), c0;
                if (wait_seq(ap, n, s, 0, itc_hz) >= 0)
                    return EFI_DEVICE_ERROR;
                c0 = ld_acq(&spin_count);
                while ((spins = ld_acq(&spin_count) - c0) < 1000u * n)
                    cpu_pause();
            }
            put("SMP-APP RESTART");
            kv_u("aps_spinning", n);
            kv_u("spins", spins);
            flush_line();
            st->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
        }
    }
#endif
    return n_fail ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

/* crt0-efi-ia64.S calls _entry after self-relocation. */
EFI_STATUS EFIAPI _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    return efi_main(image, st);
}
