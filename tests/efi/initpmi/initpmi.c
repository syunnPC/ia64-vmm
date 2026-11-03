/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * initpmi.efi: INIT, PMI and machine checks on the native firmware,
 * as an OS uses them.
 *   init     OS_INIT and OS_MCA registered with SAL_SET_VECTORS (SAL §9);
 *            AP 1, woken through OS_BOOT_RENDEZ, sends INIT (delivery mode
 *            101b) through the PIB to the BSP while it waits with known
 *            registers. OS_INIT records its hand-off state (SAL §5.3, SDM
 *            Vol.2 §11.4.2) and resumes the interrupted context, which
 *            then finds every register intact (PAL_MC_RESUME).
 *   pmi-sal  PAL_PMI_ENTRYPOINT with this application's SAL PMI handler;
 *            a PMI with SAL vector 2 to itself reaches it with GR24-GR29
 *            and BR0 of SDM Vol.2 §11.5.2, and the context resumes.
 *   pmi-pal  a PMI with PAL vector 5 never reaches SAL and resumes.
 *   mca      an itr.d overlapping a DTR machine checks (Table 4-1):
 *            OS_MCA (SAL §4.8) sees a TLB check, not continuable, and
 *            resumes a new context; the second DTR was never inserted.
 *   purge    a ptc.l over that DTR with PSR.ic = 1 machine checks
 *            (Table 4-1) by its srlz.i: OS_MCA sees a continuable TLB
 *            check without a precise IP and the collected IIP between the
 *            ptc.l and the srlz.i, and resumes a new context; the DTR is
 *            still there.
 * Output: "INITPMI PASS <case>" or "INITPMI FAIL <case> <detail> <value>",
 * then "INITPMI END pass=P fail=F" and ResetSystem(shutdown).
 * initpmi-broken-resume.efi (-DINITPMI_BROKEN_RESUME) returns from OS_INIT
 * claiming a new context, so BR1 is not restored and init must fail.
 */
#include <efi.h>
#include "initpmi.h"

#define SAL_SET_VECTORS 0x01000000ull
#define SAL_FREQ_BASE 0x01000012ull
#define VECTOR_OS_MCA 0
#define VECTOR_OS_INIT 1
#define VECTOR_BOOT_RENDEZ 2
#define UC (1ull << 63)
#define PSR_IC (1ull << 13)
#define PSR_I (1ull << 14)
#define PSR_MC (1ull << 35)
#define PSR_KEPT ((1ull << 35) | (1ull << 4) | (1ull << 5) | (1ull << 15)) /* mc mfl mfh pk */
#define PSR_BN (1ull << 44)
#define WAIT (10000ull * 400000ull) /* any bounded wait: 10 s of a 400 MHz ITC */
#define PMI_PAL_WAIT (20ull * 400000ull)
#define BIT(n) (1ull << (n))
/* Processor State Parameter (SDM Vol.2 Figure 11-11). */
enum {
    PSP_ME = 4,
    PSP_MN = 5,
    PSP_SY = 6,
    PSP_CO = 7,
    PSP_CI = 8,
    PSP_US = 9,
    PSP_PI = 13,
    PSP_PM = 14,
    PSP_IN = 16,
    PSP_RS = 17,
    PSP_CM = 18,
    PSP_GR = 31,
    PSP_TC = 60
};
#define OVERLAP_VA 0xa000000010000000ull
#define OVERLAP_PS 16u
#define OVERLAP_SLOT 30u

struct pal_result {
    UINT64 status, x, y, z;
};
extern struct pal_result model_pal_call(UINT64, UINT64, UINT64, UINT64, UINT64);
extern void initpmi_os_init(void), initpmi_os_init_slave(void), initpmi_os_mca(void),
    initpmi_sale_pmi(void);
extern UINT64 initpmi_wait(volatile UINT64 *flag, volatile UINT64 *ready, UINT64 ticks,
                           UINT64 ipi_address, UINT64 ipi_data);
extern UINT64 initpmi_overlap(UINT64 va, UINT64 pte, UINT64 itir, UINT64 slot, UINT64 purge);
extern void initpmi_purge(UINT64 va, UINT64 ps);
extern char initpmi_wait_loop[], initpmi_wait_end[], initpmi_ptc[], initpmi_ptc_srlz[];
extern struct initpmi_record initpmi_records[3];
extern struct initpmi_pmi initpmi_pmi_record;

static EFI_SYSTEM_TABLE *gST;
static unsigned passes, failures;
static struct {
    UINT64 pal_proc, sal_proc, sal_gp, wake, pib;
    UINT64 lid[2];
    unsigned cpus;
} fw;
static volatile UINT64 ready, ap_done, ap_op;

static void put(const char *s)
{
    CHAR16 buf[128];
    UINTN n = 0;
    while (*s && n < 127)
        buf[n++] = (CHAR16)(unsigned char)*s++;
    buf[n] = 0;
    gST->ConOut->OutputString(gST->ConOut, buf);
}
static void put_x(UINT64 v)
{
    char buf[19] = "0x";
    for (int i = 0; i < 16; ++i)
        buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
    buf[18] = 0;
    put(buf);
}
static unsigned case_failures;
static void check(const char *name, int ok, const char *what, UINT64 value)
{
    if (ok)
        return;
    ++case_failures;
    put("INITPMI FAIL ");
    put(name);
    put(" ");
    put(what);
    put(" ");
    put_x(value);
    put("\r\n");
}
static void finish_case(const char *name)
{
    if (case_failures)
        ++failures;
    else {
        ++passes;
        put("INITPMI PASS ");
        put(name);
        put("\r\n");
    }
    case_failures = 0;
}
static UINT64 le(const UINT8 *p, unsigned n)
{
    UINT64 v = 0;
    for (unsigned i = 0; i < n; ++i)
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
static void *config_table(EFI_GUID g)
{
    for (UINTN i = 0; i < gST->NumberOfTableEntries; ++i)
        if (same(&gST->ConfigurationTable[i].VendorGuid, &g, sizeof(g)))
            return gST->ConfigurationTable[i].VendorTable;
    return NULL;
}
static UINT64 rd_itc(void)
{
    UINT64 v;
    __asm__ volatile("mov %0=ar.itc;;" : "=r"(v)::"memory");
    return v;
}
static UINT64 ld_acq(volatile UINT64 *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void st_rel(volatile UINT64 *p, UINT64 v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

/* SAL system table (SAL §3.2.7): entrypoints (type 0), AP wake-up (type 5).
 * MADT (ACPI 2.0 §5.2.10.4): Local SAPICs and the PIB base override. */
static int discover(void)
{
    EFI_GUID sal_guid = SAL_SYSTEM_TABLE_GUID, acpi_guid = ACPI_20_TABLE_GUID;
    static const UINT8 size[6] = {48, 32, 16, 32, 16, 16};
    const UINT8 *t = config_table(sal_guid);
    if (!t || !same(t, "SST_", 4))
        return 0;
    UINT64 n = le(t + 4, 4), at = 96, count = le(t + 10, 2);
    for (UINT64 i = 0; i < count && at < n && t[at] <= 5; ++i) {
        if (t[at] == 0) {
            fw.pal_proc = le(t + at + 8, 8);
            fw.sal_proc = le(t + at + 16, 8);
            fw.sal_gp = le(t + at + 24, 8);
        }
        if (t[at] == 5)
            fw.wake = le(t + at + 8, 8);
        at += size[t[at]];
    }
    const UINT8 *rsdp = config_table(acpi_guid);
    if (!rsdp)
        return 0;
    const UINT8 *xsdt = (const UINT8 *)(UINTN)le(rsdp + 24, 8);
    for (UINT64 i = 36; i + 8 <= le(xsdt + 4, 4); i += 8) {
        const UINT8 *m = (const UINT8 *)(UINTN)le(xsdt + i, 8);
        if (!same(m, "APIC", 4))
            continue;
        fw.pib = le(m + 36, 4);
        for (UINT64 a = 44; a + 2 <= le(m + 4, 4) && m[a + 1] >= 2; a += m[a + 1]) {
            if (m[a] == 5)
                fw.pib = le(m + a + 4, 8);
            if (m[a] == 7 && (le(m + a + 8, 4) & 1) && fw.cpus < 2)
                fw.lid[fw.cpus++] = (UINT64)m[a + 3] << 24 | (UINT64)m[a + 4] << 16;
        }
    }
    return fw.pal_proc && fw.sal_proc && fw.wake && fw.pib;
}
struct sal_ret {
    UINT64 status, v0, v1, v2;
};
typedef struct sal_ret (*sal_fn)(UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64);
static struct sal_ret sal(UINT64 a, UINT64 b, UINT64 c, UINT64 d, UINT64 e, UINT64 f, UINT64 g,
                          UINT64 h)
{
    static UINT64 descriptor[2] __attribute__((aligned(16)));
    descriptor[0] = fw.sal_proc;
    descriptor[1] = fw.sal_gp;
    return ((sal_fn)(UINTN)descriptor)(a, b, c, d, e, f, g, h);
}
/* A function's code address and gp: its IA-64 descriptor. */
static const UINT64 *descriptor_of(void (*f)(void))
{
    return (const UINT64 *)(UINTN)f;
}
static UINT64 pib_address(UINT64 lid)
{
    return UC | fw.pib | ((lid >> 24) & 255) << 12 | ((lid >> 16) & 255) << 4;
}

/* OS_BOOT_RENDEZ handler on AP 1 (SAL §3.2.6): physical mode, its own
 * stack, gp registered; it returns into the firmware's wait loop. */
static void initpmi_ap(void)
{
    if (ld_acq(&ap_op) != 1)
        return;
    UINT64 start = rd_itc();
    while (!ld_acq(&ready) && rd_itc() - start < WAIT)
        __asm__ volatile("hint @pause" ::: "memory");
    __asm__ volatile("mf;;" ::: "memory");
    *(volatile UINT64 *)(UINTN)pib_address(fw.lid[0]) = 5u << 8; /* INIT: SDM Vol.2 §5.8.4.2 */
    st_rel(&ap_done, 1);
}

/* The OS handlers' common hand-off checks (SAL §4.8, §5.3). */
static void handler_state(const char *name, const struct initpmi_record *r, UINT64 gp,
                          UINT64 function)
{
    check(name, r->r1 == gp, "gp", r->r1);
    check(name, r->r8 == fw.pal_proc, "pal_proc", r->r8);
    check(name, r->r9 == fw.sal_proc, "sal_proc", r->r9);
    check(name, r->r10 == fw.sal_gp, "sal_gp", r->r10);
    check(name, r->r11 == 0, "reason", r->r11);
    check(name, r->r12 != 0 && !(r->r12 & 15), "return", r->r12);
    check(name, (r->r17 & UC) && !(r->r17 & 511), "min-state", r->r17);
    check(name, r->r16 >= r->r17 + 0x1d0 && r->r16 < r->r17 + 1024 && !(r->r16 & 7), "sal area",
          r->r16);
    check(name, r->r19 == 0, "rendezvous", r->r19);
    check(name, r->r20 == function, "function", r->r20);
    check(name, (r->psr & PSR_MC) && !(r->psr & ~PSR_KEPT), "psr", r->psr);
    check(name, !(r->rsc & 3), "rsc mode", r->rsc);
    check(name, r->lid == fw.lid[0], "lid", r->lid);
    check(name,
          (r->r18 & BIT(PSP_MN)) && (r->r18 & BIT(PSP_GR)) && (r->r18 & BIT(PSP_RS)) &&
              !(r->r18 & (BIT(PSP_ME) | BIT(PSP_US) | BIT(PSP_CM))),
          "psp", r->r18);
}

static void case_init(void)
{
    const char *name = "init";
    struct initpmi_record *r = &initpmi_records[0];
    if (fw.cpus < 2) {
        check(name, 0, "cpus", fw.cpus);
        finish_case(name);
        return;
    }
    const UINT64 *ap = descriptor_of(initpmi_ap);
    check(name, !sal(SAL_SET_VECTORS, VECTOR_BOOT_RENDEZ, ap[0], ap[1], 0, 0, 0, 0).status,
          "rendez", 0);
    st_rel(&ap_op, 1);
    __asm__ volatile("mf;;" ::: "memory");
    *(volatile UINT64 *)(UINTN)pib_address(fw.lid[1]) = fw.wake;
    UINT64 changed = initpmi_wait(&r->count, &ready, WAIT, 0, 0);
    check(name, changed == 0, "registers or timeout", changed);
    check(name, r->count == 1 && initpmi_records[1].count == 0, "monarch entries", r->count);
    handler_state(name, r, descriptor_of(initpmi_os_init)[1], 2);
    check(name,
          (r->r18 & BIT(PSP_IN)) && (r->r18 & BIT(PSP_CO)) && (r->r18 & BIT(PSP_CI)) &&
              !(r->r18 & BIT(PSP_TC)),
          "init psp", r->r18);
    /* The min-state area (SDM Vol.2 Figure 11-2): IIP in the wait loop,
     * IPSR with collection, bank 1 and no PSR.i, the covered frame, the
     * waiting GR4-GR7 and bank 1 GR16-GR31. */
    const UINT64 *a = r->area;
    check(name, a[0x198 / 8] >= (UINTN)initpmi_wait_loop && a[0x198 / 8] < (UINTN)initpmi_wait_end,
          "iip", a[0x198 / 8]);
    check(name, (a[0x1a0 / 8] & (PSR_IC | PSR_BN)) == (PSR_IC | PSR_BN) && !(a[0x1a0 / 8] & PSR_I),
          "ipsr", a[0x1a0 / 8]);
    check(name, (a[0x1a8 / 8] >> 63) && (a[0x1a8 / 8] & 127) == 22, "ifs", a[0x1a8 / 8]);
    check(name, (a[0x180 / 8] & WAIT_PR_MASK) == WAIT_PR, "pr", a[0x180 / 8]);
    for (unsigned g = 4; g <= 7; ++g)
        check(name, a[g] == (UINT64)SIG(g), "gr", g);
    for (unsigned g = 16; g <= 31; ++g)
        check(name, a[0x100 / 8 + g - 16] == (UINT64)SIG(g), "bank1 gr", g);
    UINT64 start = rd_itc();
    while (!ld_acq(&ap_done) && rd_itc() - start < WAIT) {
    }
    check(name, ld_acq(&ap_done) == 1, "ap", 0);
    finish_case(name);
}

static void case_pmi_sal(void)
{
    const char *name = "pmi-sal";
    struct initpmi_pmi *p = &initpmi_pmi_record;
    UINT64 handler = descriptor_of(initpmi_sale_pmi)[0];
    struct pal_result rp = model_pal_call(fw.pal_proc, 32, handler, 0, 0);
    check(name, rp.status == 0 && !(handler & 255), "PAL_PMI_ENTRYPOINT", rp.status);
    ready = 0;
    UINT64 changed = initpmi_wait(&p->count, &ready, WAIT, pib_address(fw.lid[0]), 2u << 8 | 2);
    check(name, changed == 0, "registers or timeout", changed);
    check(name, p->count == 1 && p->r24 == 2, "vector", p->r24);
    check(name, p->r25 == initpmi_records[0].r17, "xr0", p->r25);
    check(name, p->r28 == (UINT64)SIG(1), "b1", p->r28);
    check(name, (p->r29 & WAIT_PR_MASK) == WAIT_PR, "predicates", p->r29);
    check(name, p->b0 >= fw.pal_proc && p->b0 < fw.pal_proc + 32768, "return", p->b0);
    check(name, !(p->psr & ~PSR_KEPT) && !(p->psr & PSR_MC), "psr", p->psr);
    check(name, !(p->rsc & 3), "rsc mode", p->rsc);
    finish_case(name);
}

static void case_pmi_pal(void)
{
    const char *name = "pmi-pal";
    static volatile UINT64 never;
    ready = 0;
    UINT64 changed =
        initpmi_wait(&never, &ready, PMI_PAL_WAIT, pib_address(fw.lid[0]), 2u << 8 | 5);
    check(name, changed == WAIT_TIMEOUT, "registers", changed);
    check(name, initpmi_pmi_record.count == 1, "sal handler", initpmi_pmi_record.count);
    finish_case(name);
}

static void case_mca(void)
{
    const char *name = "mca";
    struct initpmi_record *r = &initpmi_records[2];
    static UINT64 page[8192 / 8] __attribute__((aligned(8192)));
    UINT64 pte = ((UINTN)page & ~0xfffull) | 0x661, itir = OVERLAP_PS << 2;
    UINT64 recovered = initpmi_overlap(OVERLAP_VA, pte, itir, OVERLAP_SLOT, 0);
    check(name, recovered == 1, "recovered", recovered);
    check(name, r->count == 1, "entries", r->count);
    handler_state(name, r, descriptor_of(initpmi_os_mca)[1], 1);
    check(name,
          (r->r18 & BIT(PSP_TC)) && (r->r18 & BIT(PSP_CI)) &&
              !(r->r18 & (BIT(PSP_IN) | BIT(PSP_CO) | BIT(PSP_SY))),
          "mca psp", r->r18);
    /* PAL_VM_TR_READ (261): the first DTR is there, the overlap is not. */
    static UINT64 buffer[4] __attribute__((aligned(8)));
    struct pal_result t0 = model_pal_call(fw.pal_proc, 261, OVERLAP_SLOT, 1, (UINTN)buffer);
    check(name, t0.status == 0 && (buffer[0] & ~1ull) == (pte & ~1ull), "dtr", buffer[0]);
    struct pal_result t1 = model_pal_call(fw.pal_proc, 261, OVERLAP_SLOT + 1, 1, (UINTN)buffer);
    check(name, t1.status == 0 && !(buffer[0] & 1), "overlap inserted", buffer[0]);
    initpmi_purge(OVERLAP_VA, OVERLAP_PS);
    finish_case(name);
}

static void case_purge(void)
{
    const char *name = "purge";
    struct initpmi_record *r = &initpmi_records[2];
    static UINT64 page[8192 / 8] __attribute__((aligned(8192)));
    UINT64 pte = ((UINTN)page & ~0xfffull) | 0x661, itir = OVERLAP_PS << 2, count = r->count;
    UINT64 recovered = initpmi_overlap(OVERLAP_VA, pte, itir, OVERLAP_SLOT, 1);
    check(name, recovered == 1, "recovered", recovered);
    check(name, r->count == count + 1, "entries", r->count);
    handler_state(name, r, descriptor_of(initpmi_os_mca)[1], 1);
    /* SDM Vol.2 Table 11-7: the purge completed and the saved state is all
     * valid, so continuable (co, ci); IIP is not the purge: no pi, pm. */
    check(name,
          (r->r18 & BIT(PSP_TC)) && (r->r18 & BIT(PSP_CI)) && (r->r18 & BIT(PSP_CO)) &&
              !(r->r18 & (BIT(PSP_IN) | BIT(PSP_SY) | BIT(PSP_PI) | BIT(PSP_PM))),
          "purge psp", r->r18);
    /* Collected (PSR.ic = 1): the interrupted code is at or after the
     * ptc.l and, at the latest, the srlz.i (Table 4-1 footnote h). */
    const UINT64 *a = r->area;
    check(name, a[0x198 / 8] >= (UINTN)initpmi_ptc && a[0x198 / 8] <= (UINTN)initpmi_ptc_srlz,
          "iip", a[0x198 / 8]);
    check(name, (a[0x1a0 / 8] & PSR_IC) != 0, "ipsr", a[0x1a0 / 8]);
    /* PAL_VM_TR_READ (261): TC purges do not remove TRs (SDM Vol.2 §4.1.1.2). */
    static UINT64 buffer[4] __attribute__((aligned(8)));
    struct pal_result t0 = model_pal_call(fw.pal_proc, 261, OVERLAP_SLOT, 1, (UINTN)buffer);
    check(name, t0.status == 0 && (buffer[0] & ~1ull) == (pte & ~1ull), "dtr", buffer[0]);
    initpmi_purge(OVERLAP_VA, OVERLAP_PS);
    finish_case(name);
}

/* crt0-efi-ia64.S calls _entry after self-relocation. */
EFI_STATUS EFIAPI _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    (void)image;
    gST = st;
    st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
    put("INITPMI BEGIN\r\n");
    if (!discover()) {
        put("INITPMI FAIL tables\r\n");
        ++failures;
    } else {
        const UINT64 *init = descriptor_of(initpmi_os_init),
                     *slave = descriptor_of(initpmi_os_init_slave);
        const UINT64 *mca = descriptor_of(initpmi_os_mca);
        UINT64 status =
            sal(SAL_SET_VECTORS, VECTOR_OS_INIT, init[0], init[1], 0, slave[0], slave[1], 0)
                .status |
            sal(SAL_SET_VECTORS, VECTOR_OS_MCA, mca[0], mca[1], 0, 0, 0, 0).status;
        if (status) {
            put("INITPMI FAIL set-vectors\r\n");
            ++failures;
        } else {
            case_init();
            case_pmi_sal();
            case_pmi_pal();
            case_mca();
            case_purge();
        }
    }
    put("INITPMI END pass=");
    put_x(passes);
    put(" fail=");
    put_x(failures);
    put("\r\n");
    st->RuntimeServices->ResetSystem(EfiResetShutdown, failures ? EFI_ABORTED : EFI_SUCCESS, 0,
                                     NULL);
    return EFI_SUCCESS;
}
