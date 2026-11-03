/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * benchwrap.efi: runs benchmark jobs (tests/bench/job.h) on the platform
 * that boots it, the QEMU fork's TCG with its reference firmware, and
 * times each run with guest ITC (see docs/testing.md).
 *
 * Each job is a program image plus the machine state the hosted Tier1
 * harness starts it with (tests/tier1/programs.c), captured by
 * tests/bench/jobdump.c together with the interpreter's retired
 * instruction count and final register values. The wrapper owns guest
 * physical [0x100000, 0x200000) like the harness's RAM, copies the job
 * there, sets region registers, TRs and TC purges, and enter.S enters the
 * job with rfi; the job's end `break.i 0x1ffffe` reaches the Break vector
 * of the job's IVT, where the wrapper's copy holds bench_exit_stub. The
 * only bytes that differ from the harness's RAM are that vector (never
 * reached by a harness run, which stops at the break) and, for speed, an IVT
 * (speed.bin has none; IVA is not observable before the break).
 *
 * The ITC frequency is the one the platform reports: SAL_FREQ_BASE
 * (platform base) times PAL_FREQ_RATIOS' ITC ratio (SAL specification,
 * SDM Vol.2 §11.10: the Linux itc_freq computation).
 *
 * Output: `BENCH ...` lines on the console, then ResetSystem(shutdown).
 */
#include <efi.h>
#include "tests/bench/job.h"
#include "ctx.h"

static EFI_SYSTEM_TABLE *gST;

/* -fno-builtin: GCC may still call these for struct copies. Word loops:
 * job loads copy up to 1 MiB, which is slow under the interpreter. */
void *memset(void *d, int c, UINTN n)
{
    unsigned char *p = d;
    if (!c && !((UINTN)p & 7))
        for (; n >= 8; n -= 8, p += 8)
            *(UINT64 *)p = 0;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, UINTN n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if (!(((UINTN)p | (UINTN)q) & 7))
        for (; n >= 8; n -= 8, p += 8, q += 8)
            *(UINT64 *)p = *(const UINT64 *)q;
    while (n--)
        *p++ = *q++;
    return d;
}

/* While the steps run, console text is kept here and printed after the
 * last step: console output (serial + the firmware's framebuffer text
 * console) costs seconds per line under the VMM, which would sit
 * between the runs the host's wall clock brackets. */
static char deferred[65536];
static UINTN deferred_used;
static int deferring;

static void output(const char *s)
{
    CHAR16 buf[128];
    UINTN n = 0;
    while (*s) {
        buf[n++] = (CHAR16)(unsigned char)*s++;
        if (n == 126 || *s == 0) {
            buf[n] = 0;
            gST->ConOut->OutputString(gST->ConOut, buf);
            n = 0;
        }
    }
}

static void puts8(const char *s)
{
    if (!deferring) {
        output(s);
        return;
    }
    while (*s && deferred_used + 1 < sizeof(deferred))
        deferred[deferred_used++] = *s++;
    deferred[deferred_used] = 0;
}

static void flush_deferred(void)
{
    deferring = 0;
    output(deferred);
    deferred_used = 0;
}

static void puthex(UINT64 v)
{
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xf];
    buf[18] = 0;
    puts8(buf);
}

/* Decimal, right-aligned in `width` columns (0: as is), so that plans
 * with different instruction counts print text of equal length. */
static void putdecw(UINT64 u, int width)
{
    char buf[24];
    int i = 23;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (23 - i < width && i > 0)
        buf[--i] = '0';
    puts8(&buf[i]);
}

static void putdec(UINT64 u)
{
    putdecw(u, 0);
}

static void kvw(const char *key, UINT64 v, int width)
{
    puts8(" ");
    puts8(key);
    puts8("=");
    putdecw(v, width);
}

static void kv(const char *key, UINT64 v, int dec)
{
    puts8(" ");
    puts8(key);
    puts8("=");
    if (dec)
        putdec(v);
    else
        puthex(v);
}

#define READ(reg)                                                                                  \
    ({                                                                                             \
        UINT64 _v;                                                                                 \
        __asm__ volatile("mov %0 = " reg : "=r"(_v));                                              \
        _v;                                                                                        \
    })

static inline UINT64 itc(void)
{
    UINT64 v;
    __asm__ volatile("mov %0 = ar.itc" : "=r"(v));
    return v;
}

/* ---- SAL / PAL frequency ------------------------------------------------ */

UINT64 bench_pal_static(UINT64 index, UINT64 a1, UINT64 a2, UINT64 a3, UINT64 entry, UINT64 *out3);
void bench_run(void *ctx);
extern char bench_exit_stub[], bench_exit_stub_end[], bench_resume[];
extern const unsigned char bench_jobs[], bench_jobs_end[];

struct sal_ret {
    INT64 status;
    UINT64 v0, v1, v2;
};
typedef struct sal_ret (*sal_fn)(UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64);

static EFI_GUID sal_guid = {
    0xeb9d2d32, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    const unsigned char *p = (const void *)a, *q = (const void *)b;
    for (int i = 0; i < 16; i++)
        if (p[i] != q[i])
            return 0;
    return 1;
}

static UINT64 itc_hz;

static void frequency(void)
{
    const unsigned char *systab = NULL;
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++)
        if (guid_eq(&gST->ConfigurationTable[i].VendorGuid, &sal_guid))
            systab = gST->ConfigurationTable[i].VendorTable;
    if (!systab) {
        puts8("BENCH-ERROR no SAL system table\r\n");
        return;
    }
    /* SAL 3.2 §3.2.1: 0x60-byte header, then typed entries; type 0 is the
     * entry point descriptor {type, 7 reserved, pal_proc, sal_proc, gp}. */
    UINT16 count = *(const UINT16 *)(systab + 10);
    const unsigned char *e = systab + 0x60;
    UINT64 pal = 0, sal = 0, gp = 0;
    for (UINT16 i = 0; i < count; i++) {
        static const UINT8 size[] = {48, 32, 16, 32, 16, 16};
        if (e[0] == 0) {
            pal = *(const UINT64 *)(e + 8);
            sal = *(const UINT64 *)(e + 16);
            gp = *(const UINT64 *)(e + 24);
        }
        if (e[0] > 5)
            break;
        e += size[e[0]];
    }
    UINT64 desc[2] = {sal, gp}, out[3] = {0};
    struct sal_ret r =
        ((sal_fn)(void *)desc)(0x01000012, 0, 0, 0, 0, 0, 0, 0); /* SAL_FREQ_BASE, platform */
    UINT64 status = bench_pal_static(14, 0, 0, 0, pal, out);     /* PAL_FREQ_RATIOS */
    UINT64 num = out[2] >> 32, den = out[2] & 0xffffffffu;
    if (r.status || status || !den) {
        puts8("BENCH-ERROR frequency:");
        kv("sal_status", (UINT64)r.status, 0);
        kv("pal_status", status, 0);
        puts8("\r\n");
        return;
    }
    itc_hz = r.v0 * num / den;
    puts8("BENCH-FREQ");
    kv("hz", itc_hz, 1);
    kv("base", r.v0, 1);
    kv("itc_num", num, 1);
    kv("itc_den", den, 1);
    puts8("\r\n");
}

/* ---- environment probe --------------------------------------------------- */

static void environment(void)
{
    puts8("BENCH-ENV");
    kv("psr", READ("psr"), 0);
    kv("iva", READ("cr.iva"), 0);
    kv("pta", READ("cr.pta"), 0);
    kv("dcr", READ("cr.dcr"), 0);
    kv("tpr", READ("cr.tpr"), 0);
    kv("itv", READ("cr.itv"), 0);
    kv("rsc", READ("ar.rsc"), 0);
    kv("bspstore", READ("ar.bspstore"), 0);
    /* An IA-64 function pointer is the address of its descriptor {entry, gp}. */
    kv("bench_run", *(const UINT64 *)(const void *)bench_run, 0);
    kv("bench_resume", (UINT64)bench_resume, 0);
    kv("exit_stub", (UINT64)bench_exit_stub, 0);
    puts8("\r\nBENCH-ENV");
    for (UINT64 r = 0; r < 8; r++) {
        UINT64 v;
        __asm__ volatile("mov %0 = rr[%1]" : "=r"(v) : "r"(r << 61));
        char key[4] = {'r', 'r', (char)('0' + r), 0};
        kv(key, v, 0);
    }
    puts8("\r\n");
}

/* ---- the job window -------------------------------------------------------- */

static UINT64 window_base, delta;

static void show_map(void)
{
    UINTN size = 0, key, dsize;
    UINT32 dver;
    gST->BootServices->GetMemoryMap(&size, NULL, &key, &dsize, &dver);
    size += 8 * dsize;
    EFI_MEMORY_DESCRIPTOR *map;
    if (gST->BootServices->AllocatePool(EfiLoaderData, size, (void **)&map) != EFI_SUCCESS ||
        gST->BootServices->GetMemoryMap(&size, map, &key, &dsize, &dver) != EFI_SUCCESS)
        return;
    for (UINTN o = 0; o < size; o += dsize) {
        EFI_MEMORY_DESCRIPTOR *d = (void *)((char *)map + o);
        if (d->PhysicalStart >= 0x10000000)
            continue;
        puts8("BENCH-MAP");
        kv("start", d->PhysicalStart, 0);
        kv("pages", d->NumberOfPages, 1);
        kv("type", d->Type, 1);
        puts8("\r\n");
    }
}

/* The job window: 1 MiB at the first free 1 MiB-aligned address from
 * 16 MiB on (guest physical 0x100000 belongs to the firmware on both
 * platforms). */
static int window(void)
{
    for (EFI_PHYSICAL_ADDRESS at = 0x1000000; at < 0x10000000; at += BJ_RAM_SIZE) {
        EFI_PHYSICAL_ADDRESS want = at;
        if (gST->BootServices->AllocatePages(AllocateAddress, EfiLoaderData, BJ_RAM_SIZE / 4096,
                                             &want) == EFI_SUCCESS) {
            window_base = at;
            delta = at - BJ_RAM_BASE;
            puts8("BENCH-WINDOW");
            kv("base", window_base, 0);
            puts8("\r\n");
            return 1;
        }
    }
    puts8("BENCH-ERROR no free 1 MiB window below 256 MiB\r\n");
    show_map();
    return 0;
}

static void flush_icache(UINT64 start, UINT64 len)
{
    for (UINT64 a = start & ~31ull; a < start + len; a += 32)
        __asm__ volatile("fc.i %0" : : "r"(a) : "memory");
    __asm__ volatile(";; sync.i ;; srlz.i ;;" : : : "memory");
}

static long loaded = -1;

static void load(const struct bj_job *j, long index)
{
    memset((void *)window_base, 0, BJ_RAM_SIZE);
    for (UINT64 i = 0; i < j->nmem; i++)
        memcpy((void *)(j->mem[i].gpa + delta), (const char *)j + j->mem[i].offset, j->mem[i].len);
    for (UINT64 i = 0; i < j->nreloc_word; i++)
        *(UINT64 *)(j->reloc_word[i] + delta) += delta;
    /* The IVT is at the same window offset whether cr.iva is physical or
     * (gather-virt) a TR-mapped VA equal to the harness PA. */
    UINT64 n = (UINT64)(bench_exit_stub_end - bench_exit_stub);
    memcpy((void *)(window_base + (j->iva - BJ_RAM_BASE) + 0x2c00), bench_exit_stub, n);
    flush_icache(window_base, BJ_RAM_SIZE);
    loaded = index;
}

/* ---- MMU state around a job ---------------------------------------------- */

static UINT64 saved_rr[8];

static void set_rr(UINT64 r, UINT64 v)
{
    __asm__ volatile("mov rr[%0] = %1 ;; srlz.d ;;" : : "r"(r << 61), "r"(v) : "memory");
}

static void purge(const struct bj_job *j)
{
    for (UINT64 i = 0; i < j->npurge; i++)
        __asm__ volatile("ptc.l %0, %1 ;; srlz.d ;;"
                         :
                         : "r"(j->purge[i].va + (j->purge[i].reloc ? delta : 0)),
                           "r"(j->purge[i].ps << 2)
                         : "memory");
    __asm__ volatile("srlz.i ;;" : : : "memory");
}

static void insert_tr(const struct bj_tr *t)
{
    UINT64 psr;
    __asm__ volatile("mov %0 = psr ;; rsm psr.i | psr.ic ;; srlz.d ;;" : "=r"(psr) : : "memory");
    __asm__ volatile("mov cr.itir = %0 ;; mov cr.ifa = %1 ;;"
                     :
                     : "r"(t->itir), "r"(t->va)
                     : "memory");
    if (t->side == 0)
        __asm__ volatile("itr.i itr[%0] = %1 ;; srlz.i ;;"
                         :
                         : "r"(t->slot), "r"(t->pte)
                         : "memory");
    else
        __asm__ volatile("itr.d dtr[%0] = %1 ;; srlz.d ;;"
                         :
                         : "r"(t->slot), "r"(t->pte)
                         : "memory");
    __asm__ volatile("mov psr.l = %0 ;; srlz.i ;;" : : "r"(psr) : "memory");
}

static void remove_tr(const struct bj_tr *t)
{
    UINT64 ps = (t->itir & 0xfc);
    if (t->side == 0)
        __asm__ volatile("ptr.i %0, %1 ;; srlz.i ;;" : : "r"(t->va), "r"(ps) : "memory");
    else
        __asm__ volatile("ptr.d %0, %1 ;; srlz.d ;;" : : "r"(t->va), "r"(ps) : "memory");
}

static void mmu_enter(const struct bj_job *j)
{
    for (UINT64 r = 0; r < 8; r++)
        if (j->rr_mask & (1ull << r)) {
            __asm__ volatile("mov %0 = rr[%1]" : "=r"(saved_rr[r]) : "r"(r << 61));
            set_rr(r, j->rr[r]);
        }
    purge(j);
    for (UINT64 i = 0; i < j->ntr; i++) {
        struct bj_tr t = j->tr[i];
        if (j->reloc_flags & BJ_RELOC_TR)
            t.pte += delta;
        insert_tr(&t);
    }
}

static void mmu_leave(const struct bj_job *j)
{
    for (UINT64 i = 0; i < j->ntr; i++)
        remove_tr(&j->tr[i]);
    purge(j);
    for (UINT64 r = 0; r < 8; r++)
        if (j->rr_mask & (1ull << r))
            set_rr(r, saved_rr[r]);
}

/* ---- one step ------------------------------------------------------------- */

static UINT64 ctx[CTX_SIZE / 8] __attribute__((aligned(64)));
#define C(off) ctx[(off) / 8]

static UINT64 final_value(UINT64 where, UINT64 index)
{
    switch (where) {
    case BJ_GR:
        return C(OUT_GR + 8 * index);
    case BJ_BANK0:
        return C(OUT_BANK0 + 8 * (index - 16));
    case BJ_STACKED:
        return C(OUT_STACKED + 8 * (index - 32));
    default:
        return C(OUT_PR);
    }
}

static unsigned failures;

static void step(const struct bj_file *f, UINT64 s)
{
    const struct bj_step *st = &f->step[s];
    const struct bj_job *j = (const void *)((const char *)f + f->job_offset[st->job]);
    const struct bj_variant *var = &j->variant[st->variant];
    if (loaded != (long)st->job)
        load(j, (long)st->job);

    memset(ctx, 0, sizeof(ctx));
    for (int i = 0; i < 32; i++)
        C(CTX_GR + 8 * i) = j->gr[i];
    if (var->gr_index)
        C(CTX_GR + 8 * var->gr_index) = var->gr_value;
    for (int i = 0; i < 16; i++)
        C(CTX_BANK0 + 8 * i) = j->bank0[i];
    for (int i = 0; i < 8; i++)
        C(CTX_BR + 8 * i) = j->br[i];
    C(CTX_PR) = j->pr;
    C(CTX_IP) = j->ip;
    C(CTX_PSR) = j->psr;
    C(CTX_RSC) = j->rsc;
    C(CTX_FPSR) = j->fpsr;
    C(CTX_LC) = j->lc;
    C(CTX_EC) = j->ec;
    C(CTX_PFS) = j->pfs;
    C(CTX_UNAT) = j->unat;
    C(CTX_CCV) = j->ccv;
    C(CTX_IVA) = j->iva;
    C(CTX_PTA) = j->pta;
    C(CTX_DCR) = j->dcr;
    C(CTX_NSTACKED) = j->nstacked;
    C(CTX_BSPSTORE) = j->bspstore;
    C(CTX_RNAT) = j->rnat;
    for (int i = 0; i < 32; i++)
        if (j->reloc_gr & (1ull << i))
            C(CTX_GR + 8 * i) += delta;
    for (int i = 0; i < 16; i++)
        if (j->reloc_bank0 & (1ull << i))
            C(CTX_BANK0 + 8 * i) += delta;
    if (j->reloc_flags & BJ_RELOC_IP)
        C(CTX_IP) += delta;
    if (j->reloc_flags & BJ_RELOC_IVA)
        C(CTX_IVA) += delta;
    if (j->reloc_flags & BJ_RELOC_BSPSTORE)
        C(CTX_BSPSTORE) += delta;

    UINT64 k[8];
    k[0] = READ("ar.k0");
    k[1] = READ("ar.k1");
    k[2] = READ("ar.k2");
    k[3] = READ("ar.k3");
    k[4] = READ("ar.k4");
    k[5] = READ("ar.k5");
    k[6] = READ("ar.k6");
    k[7] = READ("ar.k7");
    mmu_enter(j);
    /* Exit path: physical mode, interruptions off, bank 1 (enter.S). */
    __asm__ volatile("mov ar.k5 = %0 ;; mov ar.k6 = %1 ;; mov ar.k7 = %2 ;;"
                     :
                     : "r"(1ull << 44), "r"((UINT64)bench_resume), "r"((UINT64)ctx)
                     : "memory");
    bench_run(ctx);
    __asm__ volatile("mov ar.k0 = %0 ;; mov ar.k1 = %1 ;; mov ar.k2 = %2 ;; mov ar.k3 = %3 ;;"
                     :
                     : "r"(k[0]), "r"(k[1]), "r"(k[2]), "r"(k[3])
                     : "memory");
    __asm__ volatile("mov ar.k4 = %0 ;; mov ar.k5 = %1 ;; mov ar.k6 = %2 ;; mov ar.k7 = %3 ;;"
                     :
                     : "r"(k[4]), "r"(k[5]), "r"(k[6]), "r"(k[7])
                     : "memory");
    mmu_leave(j);
    /* bench_resume returns with PSR.i clear until ar.k1 (the firmware's
     * interrupt context pointer) is back and the job's RRs/TRs are gone. */
    if (C(SAV_PSR) & (1ull << 14))
        __asm__ volatile("ssm psr.i ;; srlz.d ;;" : : : "memory");

    /* With PSR.ic = 0 (speed: vcpu_reset's PSR = 0) the break collects no
     * IIM; the final values alone show the run completed. */
    int ok = !(j->psr & (1ull << 13)) || C(OUT_IIM) == BREAK_END;
    for (UINT64 i = 0; i < var->ncheck; i++)
        if (final_value(var->check[i].where, var->check[i].index) !=
            var->check[i].value + (var->check[i].reloc ? delta : 0))
            ok = 0;
    /* One console line (< 80 columns): step, job, variant, retired
     * instructions, ITC ticks, ok, and the guest IVT counters
     * (tests/tier1/programs/ivt.inc bank-0 r20 Data TLB, r21 Alternate Data
     * TLB, r22 Dirty-Bit, r29 Page Not Present). */
    puts8("BENCH step=");
    putdecw(s, 3);
    puts8(" job=");
    puts8(j->name);
    kvw("var", st->variant, 1);
    kvw("retired", var->retired, 11);
    kvw("itc", C(OUT_ITC1) - C(OUT_ITC0), 12);
    kvw("ok", (UINT64)ok, 1);
    puts8(" f=");
    static const int fault_reg[] = {20, 21, 22, 29};
    for (int i = 0; i < 4; i++) {
        if (i)
            puts8(",");
        putdec(C(OUT_BANK0 + 8 * (fault_reg[i] - 16)));
    }
    puts8("\r\n");
    if (!ok) {
        puts8("BENCH-CHECK");
        kv("iim", C(OUT_IIM), 0);
        kv("iip", C(OUT_IIP), 0);
        puts8("\r\n");
    }
    if (!ok) {
        ++failures;
        for (UINT64 i = 0; i < var->ncheck; i++) {
            puts8("BENCH-CHECK");
            kv("where", var->check[i].where, 1);
            kv("index", var->check[i].index, 1);
            kv("want", var->check[i].value + (var->check[i].reloc ? delta : 0), 0);
            kv("got", final_value(var->check[i].where, var->check[i].index), 0);
            puts8("\r\n");
        }
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    (void)image;
    gST = st;
    st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
    puts8("BENCH-START\r\n");
    environment();
    frequency();
    const struct bj_file *f = (const void *)bench_jobs;
    if (f->magic != BJ_MAGIC || f->version != BJ_VERSION) {
        puts8("BENCH-ERROR bad job file\r\n");
    } else if (itc_hz && window()) {
        deferring = 1;
        for (UINT64 s = 0; s < f->nstep; s++) {
            step(f, s);
#ifdef BENCH_FLUSH_EACH_STEP
            /* Diagnostics only (mk/bench.mk BENCH_WRAP_STEPS): the line leaves
             * the guest between two steps, outside the timed run. */
            deferring = 0;
            output(deferred);
            deferred_used = 0;
            deferred[0] = 0;
            deferring = 1;
#endif
        }
        flush_deferred();
        puts8("BENCH-DONE");
        kv("steps", f->nstep, 1);
        kv("failures", failures, 1);
        puts8("\r\n");
    }
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS;
}

/* gnu-efi's crt0-efi-ia64.S self-relocates and calls _entry. */
EFI_STATUS EFIAPI _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    return efi_main(image, st);
}
