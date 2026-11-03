/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * build/bench-jobdump: capture benchmark jobs (tests/bench/job.h) and measure
 * them in the hosted Tier1 harness.
 *
 *   bench-jobdump --dump=PROGRAM --insns=N1,N2,... --out=FILE IMAGES...
 *     PROGRAM: speed, gather_phys, gather_virt or shladd_xor (guest_setup,
 *     as in tests/tier1/bench.c); csuite:K or fpsuite:K selects kernel K of
 *     csuite.bin or fpsuite.bin. Each Ni produces a variant with about Ni
 *     instructions. An interpreter run supplies its exact retired count and
 *     final values for the wrapper checks; guest programs also run guest_check.
 *   bench-jobdump --run=ENGINE --job=FILE --variants=I,J,... IMAGES...
 *     Run compiled-C variants sequentially in one process with interp, tier0
 *     or tier1 (production regions). Report wall time, retired instructions,
 *     checks, Tier1 coverage and compile_ns (TIER1_STATS=1), validator stays
 *     by cause, bundles compared, and window misses by tag-slot owner.
 *     T1H_REGISTERED=1 uses the product executor registration lifetime;
 *     T1H_HOSTWIN_CHECK=0 selects production host-window context checks.
 *   bench-jobdump --dump=... | --run=... --fp=soft|exact|experimental|approx
 *     soft uses software arithmetic; exact (default) uses checked host helpers.
 *     experimental also enables direct FP code; approx additionally permits
 *     host double at other guest precisions. Experimental modes may differ.
 *     Set before any vCPU exists.
 *   bench-jobdump --run=ENGINE ... --fp-stats
 *     Print "fpstats variant V": scalar/parallel candidate counts by outcome
 *     (<class>_<reason>) and remaining FP arithmetic ("other"). Counts cover
 *     the interpreter semantic layer, including Tier0/Tier1 helpers, and
 *     reset before each run.
 *   bench-jobdump --run=ENGINE ... --dump-mem=ADDR:WORDS:FILE
 *     Write WORDS little-endian 64-bit RAM words at ADDR to FILE after the
 *     last variant; repeatable. Obtain symbol addresses from the program ELF
 *     (e.g. nm build/bench/fpsuite/fpsuite.elf); tools/bench/fp_error.py passes
 *     fpsuite_out/fpsuite_out_count. The jobrun line ends with final ar.fpsr/r8.
 *   bench-jobdump --run=ENGINE --regions ...
 *     Print Tier0/Tier1 code arenas and live regions (code/loop ranges, size,
 *     entries) after the last variant to classify perf samples by address
 *     (tools/bench/t1_perf.py --job).
 *   bench-jobdump --run=ENGINE ... --boundary=on|off|check
 *     Tier0's boundary cache as in the product (on, the default), disabled,
 *     or with every hit compared with the full path.
 *   bench-jobdump --run=tier1 --profile ...
 *     Untimed coverage diagnostics per variant: lower-tier opcodes decoded
 *     without side effects, Tier1 subset membership (t1_opcode_class), consecutive
 *     lower-tier runs between regions, first opcodes after regions, and compile
 *     attempts (reject reasons, region sizes/loops, formation end, compile time).
 *
 * Images are found by basename, as tier1-test does (tests/tier1/main.c).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "core/log_internal.h"
#include "engine/cache/validity.h"
#include "engine/cache/cache.h"
#include "engine/tier0/tier0.h"
#include "engine/tier1/tier1.h"
#include "tests/tier1/engines.h"
#include "tests/tier1/engine_tier1.h"
#include "tests/tier1/engine_tier0t1.h"
#include "tests/tier1/hostwin_linux.h"
#include "tests/tier1/programs.h"
#include "tests/tier1/storelog.h"
#include "engine/tier1/form.h"
#include "engine/tier1/region.h"
#include "engine/tier0/internal.h"
#include "ia64vmm/hal.h"
#include "ia64/semantics.h"
#include "ia64/hostfp.h"
#include "tests/bench/job.h"

#define CSUITE_IMAGE "csuite.bin"
#define FPSUITE_IMAGE "fpsuite.bin"
#define MAX_DUMP_MEM 4
#define CSUITE_STACK UINT64_C(0x1f0000) /* sp below the backing store */

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t thread_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* Tier1 compile time (compile_ticks, hal_counter ticks) as " compile_ns N";
 * " compile_ns off" in builds without TIER1_STATS=1, which do not measure it. */
static void print_compile_ns(uint64_t ticks)
{
    if (IA64VMM_TIER1_STATS)
        printf(" compile_ns %.0f", (double)ticks * 1e9 / (double)hal_counter_hz());
    else
        printf(" compile_ns off");
}

/* A program: an guest program or a kernel of the compiled-C or FP suite. */
struct prog {
    const struct guest_program *guest; /* NULL: csuite or fpsuite */
    const char *image;
    unsigned kernel;
};

static bool opt_fp_stats;
static struct dump_mem {
    uint64_t addr, words;
    const char *file;
} dump_mem[MAX_DUMP_MEM];
static unsigned ndump_mem;

static bool parse_prog(const char *name, struct prog *p)
{
    memset(p, 0, sizeof(*p));
    if (!strncmp(name, "csuite:", 7)) {
        p->image = CSUITE_IMAGE;
        p->kernel = (unsigned)strtoul(name + 7, NULL, 0);
        return true;
    }
    if (!strncmp(name, "fpsuite:", 8)) {
        p->image = FPSUITE_IMAGE;
        p->kernel = (unsigned)strtoul(name + 8, NULL, 0);
        return true;
    }
    p->guest = guest_program(name);
    return p->guest != NULL;
}

/* Parameters of a guest run of about `target` instructions (cf. bench.c). */
static struct guest_params guest_params_for(const struct guest_program *p, uint64_t target)
{
    struct guest_params q = guest_completion_params(p);
    if (p->kind == GUEST_SHLADD_XOR) {
        q.n = (unsigned)(target / 6 + 1);
    } else if (p->kind == GUEST_GATHER) {
        q.n = (p->flags & GUEST_STRIDE) ? 40 : 64;
        q.bench = true;
        q.passes = (unsigned)(target / (q.n * 12) + 1);
    }
    return q;
}

/* Sets v up for prog with iteration parameter `iters` (guest: from the
 * target count; csuite and fpsuite: the kernel's iteration count). */
static void setup(const struct prog *p, uint64_t iters, struct vcpu *v, struct guest_params *q)
{
    if (p->guest) {
        *q = guest_params_for(p->guest, iters);
        guest_setup(p->guest, q, v);
        return;
    }
    t1h_load_image(p->image);
    struct ia64_entry_regs regs = {
        .ip = T1H_CODE,
        .psr = IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN,
        .bsp = T1H_BSP,
        .bspstore = T1H_BSP,
    };
    regs.gr[2] = p->kernel;
    regs.gr[3] = iters;
    regs.gr[12] = CSUITE_STACK;
    vcpu_reset(v, &regs);
    v->s.cr[CR_IVA] = T1H_IVT;
}

struct run {
    struct vcpu_exit exit;
    uint64_t retired, ns;
    struct t1_stats t1;
};

static struct run run_engine(const struct t1h_engine *e, struct vcpu *v, struct t1h_logs *logs)
{
    (void)logs;
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, e, v);
    /* Production-shaped regions on both Tier1 engines: the v1 harness
     * engine and the product path `tier0+tier1` (checked execution). */
    bool tier1 = !strcmp(e->name, "tier1") || !strcmp(e->name, "tier0+tier1");
    if (!strcmp(e->name, "tier1"))
        t1h_tier1_set_flags(0);
    if (!strcmp(e->name, "tier0+tier1"))
        t1h_tier0t1_set_flags(0);
    struct run r = {0};
    struct t1_stats before = {0};
    if (tier1)
        t1_get_stats(&before);
    uint64_t t0 = now_ns();
    do {
        r.exit = t1h_engine_run(&ev, UINT64_C(1) << 40);
    } while (r.exit.reason == VCPU_EXIT_BUDGET);
    r.ns = now_ns() - t0;
    r.retired = v->retired;
    if (tier1) {
        t1_get_stats(&r.t1);
        r.t1.tier1_retired -= before.tier1_retired;
        r.t1.lower_retired -= before.lower_retired;
        r.t1.compiles -= before.compiles;
        r.t1.rejects -= before.rejects;
        r.t1.entries -= before.entries;
    }
    t1h_engine_release(&ev);
    return r;
}

static void add_check(struct bj_variant *var, unsigned where, unsigned index, uint64_t value,
                      bool reloc)
{
    T1H_CHECK(var->ncheck < BJ_MAX_CHECK);
    var->check[var->ncheck++] = (struct bj_check){where, index, value, reloc};
}

static uint64_t value_of(const struct vcpu *v, unsigned where, unsigned index)
{
    switch (where) {
    /* At the end break the interpreter has delivered the interruption
     * (PSR.bn = 0): bank-1 r16-r31 are read through t1h_bank1. */
    case BJ_GR:
        return index >= 16 ? t1h_bank1(v, index) : v->s.gr[index];
    case BJ_BANK0:
        return t1h_bank0(v, index);
    case BJ_STACKED:
        return ia64_gr_read(&v->s, index);
    default:
        return v->s.pr;
    }
}

/* The final values a run must reproduce (programs.c guest_check's subjects). */
static void checks(const struct prog *p, const struct vcpu *v, struct bj_variant *var)
{
    static const unsigned speed[][2] = {{BJ_GR, 8}, {BJ_GR, 9}};
    static const unsigned shladd_xor[][2] = {{BJ_GR, 16}, {BJ_GR, 17}};
    static const unsigned gather[][2] = {{BJ_GR, 14}, {BJ_GR, 15}, {BJ_GR, 22}, {BJ_GR, 9}};
    static const unsigned virt[][2] = {
        {BJ_BANK0, 20}, {BJ_BANK0, 21}, {BJ_BANK0, 22}, {BJ_BANK0, 29}};
    static const unsigned cs[][2] = {{BJ_GR, 8}, {BJ_GR, 9}};
    const unsigned (*list)[2] = cs;
    size_t n = 2;
    if (p->guest && p->guest->kind == GUEST_SPEED)
        list = speed;
    if (p->guest && p->guest->kind == GUEST_SHLADD_XOR)
        list = shladd_xor;
    if (p->guest && p->guest->kind == GUEST_GATHER)
        list = gather, n = 4;
    for (size_t i = 0; i < n; ++i)
        add_check(var, list[i][0], list[i][1], value_of(v, list[i][0], list[i][1]), false);
    if (p->guest && p->guest->kind == GUEST_GATHER) {
        add_check(var, BJ_STACKED, 34, ia64_gr_read(&v->s, 34), true);
        if (p->guest->flags & GUEST_VIRT)
            for (size_t i = 0; i < 4; ++i)
                add_check(var, virt[i][0], virt[i][1], value_of(v, virt[i][0], virt[i][1]), false);
    }
}

static unsigned vrn_of_rid(const struct vcpu *v, uint32_t rid)
{
    for (unsigned r = 0; r < 8; ++r)
        if (((v->s.rr[r] >> 8) & 0xffffff) == rid)
            return r;
    T1H_CHECK(!"TR rid matches no region register");
    return 0;
}

static bool in_ram(uint64_t a)
{
    return a >= T1H_RAM_BASE && a < T1H_RAM_BASE + T1H_RAM_SIZE;
}

/* Entry state, memory and relocations of the job after setup. */
static size_t capture(const struct prog *p, const struct vcpu *v, struct bj_job *j, uint8_t *data,
                      size_t data_off)
{
    const struct ia64_state *s = &v->s;
    T1H_CHECK(s->cfm == 0 && s->slot == 0);
    j->ip = s->ip;
    j->psr = s->psr;
    j->rsc = s->ar[AR_RSC];
    T1H_CHECK(s->ar[AR_BSP] == s->ar[AR_BSPSTORE]);
    j->bspstore = s->ar[AR_BSPSTORE];
    j->rnat = s->ar[AR_RNAT];
    j->fpsr = s->ar[AR_FPSR];
    j->lc = s->ar[AR_LC];
    j->ec = s->ar[AR_EC];
    j->pfs = s->ar[AR_PFS];
    j->unat = s->ar[AR_UNAT];
    j->ccv = s->ar[AR_CCV];
    j->iva = s->cr[CR_IVA];
    j->pta = s->cr[CR_PTA];
    j->dcr = s->cr[CR_DCR];
    j->pr = s->pr;
    j->nstacked = p->guest && p->guest->kind == GUEST_GATHER ? 8 : 0;
    /* gr[16..31] are bank-1 and bank0[] bank-0 values whatever PSR.bn is
     * (s->gr holds the current bank, s->bank_gr the other one). */
    bool bn = s->psr & IA64_PSR_BN;
    for (unsigned i = 1; i < 32; ++i)
        j->gr[i] = i >= 16 && !bn ? s->bank_gr[i - 16] : s->gr[i];
    for (unsigned i = 0; i < 16; ++i)
        j->bank0[i] = bn ? s->bank_gr[i] : s->gr[16 + i];
    for (unsigned i = 0; i < 8; ++i)
        j->br[i] = s->br[i];
    for (unsigned i = 0; i < 128; ++i)
        T1H_CHECK(!s->nat[i]);
    bool virt = s->psr & (IA64_PSR_IT | IA64_PSR_DT | IA64_PSR_RT);
    if (virt) {
        j->rr_mask = 0x3f; /* programs.c gather_virtual_mmu: rr0-rr5 */
        for (unsigned r = 0; r < 8; ++r)
            j->rr[r] = s->rr[r];
        for (unsigned side = 0; side < 2; ++side)
            for (unsigned slot = 0; slot < 8; ++slot) {
                const struct ia64_tlb_entry *t = &s->tr[side][slot];
                if (!t->valid)
                    continue;
                T1H_CHECK(j->ntr < BJ_MAX_TR);
                j->tr[j->ntr++] = (struct bj_tr){
                    side, slot, t->va | (uint64_t)vrn_of_rid(v, t->rid) << 61, t->pte, t->itir};
            }
        /* TC purges before and after every run: the table's VA range in
         * region 2 and the output pages in region 3 (their VA payload is
         * their PA, so it moves with the window). */
        for (unsigned i = 0; i < T1H_TABLE_PAGES; ++i)
            j->purge[j->npurge++] =
                (struct bj_purge){(UINT64_C(2) << 61) | (T1H_TABLE_VA + i * 4096u), 12, 0};
        for (unsigned i = 0; i < T1H_OUT_PAGES; ++i)
            j->purge[j->npurge++] =
                (struct bj_purge){(UINT64_C(3) << 61) | (T1H_OUT + i * 4096u), 12, 1};
        j->reloc_flags = BJ_RELOC_TR;
        j->reloc_gr = 1u << 3;            /* output VA payload = PA */
        j->reloc_bank0 = 1u << (30 - 16); /* table PA - table VA payload */
        const uint8_t *vhpt = t1h_ram() + (T1H_VHPT - T1H_RAM_BASE);
        for (unsigned o = 0; o < 0x8000; o += 8) {
            uint64_t w;
            memcpy(&w, vhpt + o, 8);
            if (!w)
                continue;
            T1H_CHECK(j->nreloc_word < BJ_MAX_RELOC);
            j->reloc_word[j->nreloc_word++] = T1H_VHPT + o;
        }
    } else {
        j->reloc_flags = BJ_RELOC_IP | BJ_RELOC_IVA | (in_ram(j->bspstore) ? BJ_RELOC_BSPSTORE : 0);
        if (p->guest && p->guest->kind == GUEST_GATHER)
            j->reloc_gr = 1u << 2 | 1u << 3 | 1u << 6 | 1u << 11; /* table, out, status, index */
        if (!p->guest)
            j->reloc_gr = 1u << 12; /* stack pointer */
    }
    /* Nonzero pages of the harness RAM, merged into runs. */
    const uint8_t *ram = t1h_ram();
    size_t off = data_off;
    for (uint64_t page = 0; page < T1H_RAM_PAGES; ++page) {
        bool nonzero = false;
        for (unsigned i = 0; i < 4096 && !nonzero; ++i)
            nonzero = ram[page * 4096 + i] != 0;
        if (!nonzero)
            continue;
        uint64_t gpa = T1H_RAM_BASE + page * 4096;
        if (j->nmem && j->mem[j->nmem - 1].gpa + j->mem[j->nmem - 1].len == gpa) {
            j->mem[j->nmem - 1].len += 4096;
        } else {
            T1H_CHECK(j->nmem < BJ_MAX_MEM);
            j->mem[j->nmem++] = (struct bj_mem){gpa, 4096, off};
        }
        memcpy(data + (off - sizeof(*j)), ram + page * 4096, 4096);
        off += 4096;
    }
    return off;
}

/* speed (speed.bin) has no IVT; the guest IVT bytes of shladd_xor.bin go to 0x108000
 * so the wrapper can catch the end break (IVA is not observable before). */
static void speed_ivt(struct vcpu *v)
{
    FILE *f = fopen(t1h_image("shladd_xor.bin"), "rb");
    T1H_CHECK(f != NULL);
    static uint8_t image[0x10000];
    size_t n = fread(image, 1, sizeof(image), f);
    fclose(f);
    T1H_CHECK(n == 0x10000);
    memcpy(t1h_ram() + (T1H_IVT - T1H_RAM_BASE), image + 0x8000, 0x8000);
    code_notify_vmm_write(T1H_IVT, 0x8000);
    v->s.cr[CR_IVA] = T1H_IVT;
}

static int dump(const char *program, const char *insns, const char *out)
{
    struct prog p;
    if (!parse_prog(program, &p)) {
        fprintf(stderr, "jobdump: unknown program %s\n", program);
        return 2;
    }
    static struct bj_job job;
    static uint8_t data[T1H_RAM_SIZE];
    static uint8_t first_ram[T1H_RAM_SIZE];
    snprintf(job.name, sizeof(job.name), "%s", program);
    const struct t1h_engine *interp = t1h_engine_named("interp");
    size_t end = 0;
    char *list = strdup(insns);
    unsigned index = 0;
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        uint64_t target = strtoull(tok, NULL, 0);
        uint64_t iters = target;
        struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
        if (!p.guest) {
            /* Calibrate: retired instructions of 1 and 2 iterations. */
            uint64_t r[2];
            for (unsigned k = 0; k < 2; ++k) {
                struct vcpu *c = vcpu_create(&cfg);
                static struct t1h_logs lg;
                t1h_bind(c, &lg);
                struct guest_params unused;
                setup(&p, k + 1, c, &unused);
                struct run run = run_engine(interp, c, &lg);
                T1H_CHECK(run.exit.reason == VCPU_EXIT_DEBUG && lg.last_break_imm == T1H_BREAK_END);
                r[k] = run.retired;
                t1h_unbind(c);
                vcpu_destroy(c);
            }
            uint64_t per = r[1] - r[0];
            iters = target > r[0] ? (target - r[0]) / per + 1 : 1;
        }
        struct vcpu *v = vcpu_create(&cfg);
        static struct t1h_logs logs;
        memset(&logs, 0, sizeof(logs));
        t1h_bind(v, &logs);
        struct guest_params q = {0};
        setup(&p, iters, v, &q);
        if (p.guest && p.guest->kind == GUEST_SPEED)
            speed_ivt(v);
        T1H_CHECK(index < BJ_MAX_VARIANT);
        struct bj_variant *var = &job.variant[index];
        if (index == 0) {
            end = capture(&p, v, &job, data, sizeof(job));
            memcpy(first_ram, t1h_ram(), T1H_RAM_SIZE);
        } else {
            /* Variants differ only in the overridden register. */
            T1H_CHECK(!memcmp(first_ram, t1h_ram(), T1H_RAM_SIZE));
        }
        unsigned gr = !p.guest                            ? 3
                      : p.guest->kind == GUEST_GATHER     ? 9
                      : p.guest->kind == GUEST_SHLADD_XOR ? 8
                                                          : 0;
        var->gr_index = gr;
        var->gr_value = gr ? v->s.gr[gr] : 0;
        if (index && !gr) {
            fprintf(stderr, "jobdump: %s has a single variant\n", program);
            return 2;
        }
        for (unsigned i = 1; i < 16; ++i)
            T1H_CHECK(i == gr || job.gr[i] == v->s.gr[i]);
        struct run run = run_engine(interp, v, &logs);
        if (run.exit.reason != VCPU_EXIT_DEBUG || logs.last_break_imm != T1H_BREAK_END) {
            fprintf(stderr, "jobdump: %s: exit %u, break %#llx\n", program, run.exit.reason,
                    (unsigned long long)logs.last_break_imm);
            return 1;
        }
        if (p.guest) {
            char why[256];
            if (!guest_check(p.guest, &q, v, &run.exit, &logs, why, sizeof(why))) {
                fprintf(stderr, "jobdump: %s: guest_check: %s\n", program, why);
                return 1;
            }
        }
        var->retired = run.retired;
        checks(&p, v, var);
        printf("jobdump %s variant %u iters %llu retired %llu r8 %#llx interp %.1f Minsn/s\n",
               program, index, (unsigned long long)(gr ? var->gr_value : 0),
               (unsigned long long)run.retired, (unsigned long long)v->s.gr[8],
               run.retired * 1e3 / (double)run.ns);
        t1h_unbind(v);
        vcpu_destroy(v);
        ++index;
    }
    free(list);
    job.nvariant = index;
    job.size = end;
    FILE *f = fopen(out, "wb");
    T1H_CHECK(f != NULL);
    T1H_CHECK(fwrite(&job, sizeof(job), 1, f) == 1);
    T1H_CHECK(fwrite(data, 1, end - sizeof(job), f) == end - sizeof(job));
    T1H_CHECK(fclose(f) == 0);
    return 0;
}

/* Entry registers of a job variant (vcpu_reset + the job's state), RAM
 * untouched: later runs of a job keep their translations, as in the
 * wrapper, which loads a job's memory once per process. */
static void job_regs(const struct bj_job *j, const struct bj_variant *var, struct vcpu *v)
{
    struct ia64_entry_regs regs = {
        .ip = j->ip, .psr = j->psr, .bsp = j->bspstore, .bspstore = j->bspstore, .rsc = j->rsc};
    bool bn = j->psr & IA64_PSR_BN;
    for (unsigned i = 1; i < 32; ++i)
        regs.gr[i] = i >= 16 && !bn ? j->bank0[i - 16] : j->gr[i];
    if (var->gr_index)
        regs.gr[var->gr_index] = var->gr_value;
    vcpu_reset(v, &regs);
    struct ia64_state *s = &v->s;
    for (unsigned r = 0; r < 8; ++r)
        if (j->rr_mask & (1u << r))
            s->rr[r] = j->rr[r];
    for (uint64_t i = 0; i < j->ntr; ++i) {
        const struct bj_tr *t = &j->tr[i];
        unsigned ps = (unsigned)(t->itir >> 2) & 0x3f;
        s->tr[t->side][t->slot] =
            (struct ia64_tlb_entry){.va = t->va & ia64_low_mask(61) & ~ia64_low_mask(ps),
                                    .pte = t->pte,
                                    .itir = t->itir,
                                    .rid = (uint32_t)((s->rr[t->va >> 61] >> 8) & 0xffffff),
                                    .page_shift = ps,
                                    .valid = true};
    }
    s->ar[AR_RNAT] = j->rnat;
    s->ar[AR_FPSR] = j->fpsr;
    s->ar[AR_LC] = j->lc;
    s->ar[AR_EC] = j->ec;
    s->ar[AR_PFS] = j->pfs;
    s->ar[AR_UNAT] = j->unat;
    s->ar[AR_CCV] = j->ccv;
    s->cr[CR_IVA] = j->iva;
    s->cr[CR_PTA] = j->pta;
    s->cr[CR_DCR] = j->dcr;
    s->pr = j->pr;
    for (unsigned i = 0; i < 16; ++i)
        s->bank_gr[i] = bn ? j->bank0[i] : j->gr[16 + i];
    for (unsigned i = 0; i < 8; ++i)
        s->br[i] = j->br[i];
}

/* ---- --profile (Tier1): where the coverage goes ---------------------- */

#define PROF_BUCKETS 12u /* run lengths 1, 2-3, 4-7, ..., >= 2048 */
#define PROF_NAMES 64u
#define PROF_REGIONS 256u
struct prof_count {
    const char *reason;
    int op;
    uint64_t n;
};
static struct {
    uint64_t steps, subset, undecoded, op[IA64_OP_COUNT], after_op[IA64_OP_COUNT];
    uint64_t entries_seen, run, runs[PROF_BUCKETS], run_insns[PROF_BUCKETS];
    struct prof_count reject[PROF_NAMES], end[PROF_NAMES];
    unsigned nreject, nend;
    uint64_t compiled, rejected;
    /* Lower-tier stores (physical mode) to pages that hold code at the
     * time of the store: per page, count and the stored byte range. */
    struct prof_cstore {
        gpa_t page;
        uint64_t n;
        uint64_t lo, hi;
    } cstore[8];
    unsigned ncstore;
} P;
/* Compiled regions of the whole process, with their entry count at the
 * start of the current variant. */
static uint32_t prof_region[PROF_REGIONS];
static uint64_t prof_region_entries0[PROF_REGIONS];
static unsigned prof_nregion;

static unsigned bucket(uint64_t n)
{
    unsigned b = 0;
    while (n > 1 && b + 1 < PROF_BUCKETS) {
        n >>= 1;
        ++b;
    }
    return b;
}

static void prof_close_run(void)
{
    if (!P.run)
        return;
    ++P.runs[bucket(P.run)];
    P.run_insns[bucket(P.run)] += P.run;
    P.run = 0;
}

static void tally(struct prof_count *t, unsigned *n, const char *reason, int op)
{
    if (!reason)
        reason = "-";
    for (unsigned i = 0; i < *n; ++i)
        if (t[i].op == op && !strcmp(t[i].reason, reason)) {
            ++t[i].n;
            return;
        }
    if (*n < PROF_NAMES)
        t[(*n)++] = (struct prof_count){reason, op, 1};
}

static void prof_lower(struct vcpu *v, void *arg)
{
    (void)arg;
    struct t1_stats s;
    t1_get_stats(&s);
    bool after_region = s.entries != P.entries_seen;
    if (after_region) {
        prof_close_run();
        P.entries_seen = s.entries;
    }
    ++P.run;
    ++P.steps;
    Ia64Instruction in;
    if (!storelog_decode(v, v->s.ip, v->s.slot, &in) || (unsigned)in.opcode >= IA64_OP_COUNT) {
        ++P.undecoded;
        return;
    }
    ++P.op[in.opcode];
    int cls = t1_opcode_class(in.opcode);
    if (cls >= 0)
        ++P.subset;
    if (cls == T1_IC_STORE && !(v->s.psr & IA64_PSR_DT) &&
        ((v->s.pr >> ia64_pr_index(&v->s, in.qp)) & 1)) {
        unsigned size = in.opcode == IA64_OP_ST1 || in.opcode == IA64_OP_ST1REL   ? 1
                        : in.opcode == IA64_OP_ST2 || in.opcode == IA64_OP_ST2REL ? 2
                        : in.opcode == IA64_OP_ST4 || in.opcode == IA64_OP_ST4REL ? 4
                                                                                  : 8;
        gpa_t a = ia64_gr_read(&v->s, in.operands.memory.base) & ~(UINT64_C(1) << 63);
        gpa_t page = a & ~UINT64_C(4095);
        if (code_page_has_code(page)) {
            unsigned i = 0;
            while (i < P.ncstore && P.cstore[i].page != page)
                ++i;
            if (i == P.ncstore && P.ncstore < 8)
                P.cstore[P.ncstore++] = (struct prof_cstore){page, 0, a, a};
            if (i < P.ncstore) {
                ++P.cstore[i].n;
                if (a < P.cstore[i].lo)
                    P.cstore[i].lo = a;
                if (a + size - 1 > P.cstore[i].hi)
                    P.cstore[i].hi = a + size - 1;
            }
        }
    }
    if (after_region)
        ++P.after_op[in.opcode];
}

static void prof_compile(const struct vcpu *v, enum t1_compile_status st,
                         const struct t1_compile_report *rep, void *arg)
{
    (void)arg;
    Ia64Instruction in;
    if (st == T1_COMPILED) {
        ++P.compiled;
        if (prof_nregion < PROF_REGIONS) {
            prof_region_entries0[prof_nregion] = 0;
            prof_region[prof_nregion++] = rep->region_id;
        }
        bool loop = rep->end_reason && !strcmp(rep->end_reason, "back-edge");
        int op = !loop && storelog_decode(v, rep->end_ip, rep->end_slot, &in) ? (int)in.opcode : -1;
        tally(P.end, &P.nend, rep->end_reason, op);
    } else {
        ++P.rejected;
        int op = storelog_decode(v, v->s.ip, v->s.slot, &in) ? (int)in.opcode : -1;
        tally(P.reject, &P.nreject, rep->reject_reason, op);
    }
}

static const char *op_name(int op)
{
    return op < 0 ? "-" : ia64_opcode_info((Ia64Opcode)op)->name;
}

static void prof_reset(void)
{
    memset(&P, 0, sizeof(P));
    struct t1_stats s;
    t1_get_stats(&s);
    P.entries_seen = s.entries;
    for (unsigned i = 0; i < prof_nregion; ++i) {
        const struct t1_region *r = t1_region_get(prof_region[i]);
        prof_region_entries0[i] = r ? r->entries : 0;
    }
}

static void prof_print(const char *job, unsigned variant, const struct t1_stats *d,
                       uint64_t retired, const struct vcpu *v)
{
    prof_close_run();
    /* Code-page invalidations (whole-process counters, cumulative). */
    struct code_validity_stats cv;
    code_validity_get_stats(&cv);
    struct code_cache_stats cc;
    code_cache_get_stats(&cc);
    struct t1_stats t;
    t1_get_stats(&t);
    printf("profile-code %s var %u validity_invalidations %llu cache_invalidated_pages %llu "
           "region_invalidations %llu compiles %llu rejects %llu\n",
           job, variant, (unsigned long long)cv.invalidations,
           (unsigned long long)cc.invalidated_pages, (unsigned long long)t.invalidations,
           (unsigned long long)t.compiles, (unsigned long long)t.rejects);
    for (unsigned i = 0; i < P.ncstore; ++i)
        printf("profile-codestore %s var %u page %#llx stores %llu bytes %#llx-%#llx\n", job,
               variant, (unsigned long long)P.cstore[i].page, (unsigned long long)P.cstore[i].n,
               (unsigned long long)P.cstore[i].lo, (unsigned long long)P.cstore[i].hi);
    printf("profile %s var %u retired %llu tier1 %llu lower %llu entries %llu lower_steps %llu "
           "subset_lower %llu undecoded %llu compiled %llu rejected %llu\n",
           job, variant, (unsigned long long)retired, (unsigned long long)d->tier1_retired,
           (unsigned long long)d->lower_retired, (unsigned long long)d->entries,
           (unsigned long long)P.steps, (unsigned long long)P.subset,
           (unsigned long long)P.undecoded, (unsigned long long)P.compiled,
           (unsigned long long)P.rejected);
    /* Lower-tier opcodes, most frequent first (all with >= 0.1 % of the steps). */
    static bool done[IA64_OP_COUNT];
    memset(done, 0, sizeof(done));
    for (;;) {
        int best = -1;
        for (unsigned o = 0; o < IA64_OP_COUNT; ++o)
            if (!done[o] && P.op[o] && (best < 0 || P.op[o] > P.op[best]))
                best = (int)o;
        if (best < 0 || P.op[best] * 1000 < P.steps)
            break;
        done[best] = true;
        printf("profile-op %s var %u %s %llu subset %d after_region %llu\n", job, variant,
               op_name(best), (unsigned long long)P.op[best],
               t1_opcode_class((Ia64Opcode)best) >= 0, (unsigned long long)P.after_op[best]);
    }
    for (unsigned b = 0; b < PROF_BUCKETS; ++b)
        if (P.runs[b])
            printf("profile-run %s var %u len %llu-%s runs %llu insns %llu\n", job, variant,
                   (unsigned long long)(UINT64_C(1) << b), b + 1 < PROF_BUCKETS ? "" : "inf",
                   (unsigned long long)P.runs[b], (unsigned long long)P.run_insns[b]);
    for (unsigned i = 0; i < P.nreject; ++i)
        printf("profile-reject %s var %u n %llu at %s: %s\n", job, variant,
               (unsigned long long)P.reject[i].n, op_name(P.reject[i].op), P.reject[i].reason);
    for (unsigned i = 0; i < P.nend; ++i)
        printf("profile-end %s var %u n %llu at %s: %s\n", job, variant,
               (unsigned long long)P.end[i].n, op_name(P.end[i].op), P.end[i].reason);
    for (unsigned i = 0; i < prof_nregion; ++i) {
        const struct t1_region *r = t1_region_get(prof_region[i]);
        if (!r || r->entries <= prof_region_entries0[i])
            continue;
        Ia64Instruction in;
        bool loop = r->report.end_reason && !strcmp(r->report.end_reason, "back-edge");
        int op = !loop && storelog_decode(v, r->report.end_ip, r->report.end_slot, &in)
                     ? (int)in.opcode
                     : -1;
        /* No frame base: one region serves its (header, CFM) at every BOF. */
        printf(
            "profile-region %s var %u ip %#llx cfm %#llx insns %u blocks %u sites %u entries %llu "
            "valid %d end %s at %s",
            job, variant, (unsigned long long)r->c.ip, (unsigned long long)r->c.cfm, r->ninsns,
            r->nblocks, r->nsites, (unsigned long long)(r->entries - prof_region_entries0[i]),
            r->valid, r->report.end_reason ? r->report.end_reason : "-", op_name(op));
        print_compile_ns(__atomic_load_n(&r->report.compile_ticks, __ATOMIC_RELAXED));
        printf("\n");
    }
}

/* Load RAM once; restore entry state (registers, RRs, TRs, PTA) per variant
 * and run to the end marker, checking retired count/final values against the
 * dump. One engine binding: first run cold (including translation), later warm. */
/* --regions: the arenas and the live regions (bench.c prints the same for
 * the guest programs). */
static void print_regions(void)
{
    printf("jobdump-arena tier1 %#llx %#llx tier0 %#llx %#llx\n",
           (unsigned long long)(uintptr_t)t1_arena_stub_page(),
           (unsigned long long)((uintptr_t)t1_arena_stub_page() + t1_cfg.code_bytes),
           (unsigned long long)(uintptr_t)tier0_arena.code,
           (unsigned long long)((uintptr_t)tier0_arena.code + tier0_arena.code_pages * 4096));
    for (uint32_t id = 1; id <= t1_cfg.max_regions; ++id) {
        const struct t1_region *r = t1_region_get(id);
        if (!r || !r->valid)
            continue;
        printf(
            "jobdump-region %u code %#llx size %zu loop %u %u insns %u sites %u entries %llu retired %llu "
            "ip %#llx pages",
            id, (unsigned long long)(uintptr_t)r->code, r->code_size, r->loop_start, r->loop_end,
            r->ninsns, r->nsites, (unsigned long long)r->entries,
            (unsigned long long)r->retired_insns, (unsigned long long)r->c.ip);
        for (unsigned p = 0; p < r->npages; ++p)
            printf(" %#llx", (unsigned long long)r->page[p].pa);
        unsigned restore = 0;
        for (unsigned p = 0; p < r->npoints; ++p)
            if (r->point[p].restore_offset && (!restore || r->point[p].restore_offset < restore))
                restore = r->point[p].restore_offset;
        printf(" restore %u %zu frame-in %u %zu", restore, restore ? r->code_size : 0,
               r->frame_in_offset, r->frame_in_offset ? (restore ? restore : r->code_size) : 0);
        printf("\n");
    }
#if IA64VMM_TIER1_STATS
    /* Transitions between regions. */
    uint64_t mask = (UINT64_C(1) << T1_LINK_ID_BITS) - 1;
    for (unsigned slot = 0; slot < T1_VCPU_SLOTS; ++slot) {
        const struct t1_vcpu *tv = t1_vcpus[slot];
        for (unsigned k = 0; tv && k < T1_TRANSITIONS; ++k)
            if (tv->transitions[k].count)
                printf("jobdump-transition r%llu.%u -> r%llu count %llu\n",
                       (unsigned long long)(tv->transitions[k].from & mask),
                       tv->transitions[k].point, (unsigned long long)(tv->transitions[k].to & mask),
                       (unsigned long long)tv->transitions[k].count);
    }
#endif
}

/* The candidate FP operations of vCPU 0 by outcome, and the other FP
 * arithmetic (ia64_hostfp_stats). */
static void print_fp_stats(unsigned variant)
{
    static const char *const cls[IA64_HOSTFP_CLASSES] = {"scalar", "parallel"};
    struct ia64_hostfp_stats st;
    ia64_hostfp_stats(0, &st);
    printf("fpstats variant %u", variant);
    for (unsigned c = 0; c < IA64_HOSTFP_CLASSES; ++c)
        for (unsigned r = 0; r < IA64_HOSTFP_REASONS; ++r)
            printf(" %s_%s %llu", cls[c], ia64_hostfp_reason_name((enum ia64_hostfp_reason)r),
                   (unsigned long long)st.candidate[c][r]);
    printf(" other %llu\n", (unsigned long long)st.other);
}

/* --dump-mem: guest RAM words after the last variant. */
static void write_dump_mem(const uint8_t *ram)
{
    for (unsigned i = 0; i < ndump_mem; ++i) {
        const struct dump_mem *d = &dump_mem[i];
        T1H_CHECK(d->addr >= T1H_RAM_BASE && d->addr % 8 == 0 && d->words <= T1H_RAM_SIZE / 8 &&
                  d->addr + d->words * 8 <= T1H_RAM_BASE + T1H_RAM_SIZE);
        FILE *f = fopen(d->file, "wb");
        T1H_CHECK(f != NULL);
        T1H_CHECK(fwrite(ram + (d->addr - T1H_RAM_BASE), 8, d->words, f) == d->words);
        T1H_CHECK(fclose(f) == 0);
    }
}

static int run_job(const char *engine_name, const char *file, const char *variants, bool profile,
                   bool regions)
{
    const struct t1h_engine *e = t1h_engine_named(engine_name);
    T1H_CHECK(e != NULL);
    FILE *f = fopen(file, "rb");
    T1H_CHECK(f != NULL);
    static uint8_t buf[sizeof(struct bj_job) + T1H_RAM_SIZE];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    const struct bj_job *j = (const void *)buf;
    T1H_CHECK(n >= sizeof(*j) && n == j->size);
    struct prog p;
    T1H_CHECK(parse_prog(j->name, &p));
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *v = vcpu_create(&cfg);
    static struct t1h_logs logs;
    t1h_bind(v, &logs);
    t1h_load_image(p.guest ? p.guest->image : p.image); /* then replaced by the job's RAM */
    uint8_t *ram = t1h_ram();
    memset(ram, 0, T1H_RAM_SIZE);
    for (uint64_t i = 0; i < j->nmem; ++i)
        memcpy(ram + (j->mem[i].gpa - T1H_RAM_BASE), buf + j->mem[i].offset, j->mem[i].len);
    code_notify_vmm_write(T1H_RAM_BASE, T1H_RAM_SIZE);
    job_regs(j, &j->variant[0], v);
    struct t1h_engine_vcpu ev;
    t1h_engine_bind(&ev, e, v);
    /* Production-shaped regions on both Tier1 engines (as run_engine). */
    bool tier1 = !strcmp(e->name, "tier1") || !strcmp(e->name, "tier0+tier1");
    /* Match the product executor lifetime for native-edge measurements. */
    const char *registration = getenv("T1H_REGISTERED");
    bool registered = tier1 && registration && strcmp(registration, "0");
    if (registered && !tier0_arena_initialized())
        T1H_CHECK(tier0_configure(TIER0_CODE_DEFAULT_BYTES));
    if (registered)
        code_register_vcpu(v);
    if (!strcmp(e->name, "tier1"))
        t1h_tier1_set_flags(0);
    if (!strcmp(e->name, "tier0+tier1"))
        t1h_tier0t1_set_flags(0);
    profile = profile && !strcmp(e->name, "tier1");
    if (profile) {
        t1h_tier1_set_lower_observer(prof_lower, NULL);
        t1_test_set_compile_observer(prof_compile, NULL);
    }
    int status = 0;
    char *list = strdup(variants);
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        unsigned variant = (unsigned)atoi(tok);
        T1H_CHECK(variant < j->nvariant);
        const struct bj_variant *var = &j->variant[variant];
        job_regs(j, var, v);
        t1h_engine_restored(&ev);
        if (registered)
            code_quiescent(v);
        memset(&logs, 0, sizeof(logs));
        ia64_hostfp_stats_reset();
        struct t1_stats before = {0}, after = {0};
        struct code_validity_stats valid_before, valid_after;
        struct code_stay_stats stay_before, stay_after;
        code_validity_get_stats(&valid_before);
        code_stay_get_stats(&stay_before);
        if (tier1)
            t1_get_stats(&before);
        if (profile)
            prof_reset();
        struct vcpu_exit exit;
        uint64_t cpu0 = thread_ns(), t0 = now_ns();
        do {
            exit = t1h_engine_run(&ev, UINT64_C(1) << 40);
            if (registered)
                code_quiescent(v);
        } while (exit.reason == VCPU_EXIT_BUDGET);
        uint64_t ns = now_ns() - t0;
        uint64_t cpu_ns = thread_ns() - cpu0;
        code_validity_get_stats(&valid_after);
        code_stay_get_stats(&stay_after);
        if (tier1)
            t1_get_stats(&after);
        bool ok = exit.reason == VCPU_EXIT_DEBUG && logs.last_break_imm == T1H_BREAK_END &&
                  v->retired == var->retired;
        for (uint64_t i = 0; i < var->ncheck; ++i)
            ok = ok && value_of(v, (unsigned)var->check[i].where, (unsigned)var->check[i].index) ==
                           var->check[i].value;
        printf("jobrun %s %s var %u retired %llu ns %llu ok %d", e->name, j->name, variant,
               (unsigned long long)v->retired, (unsigned long long)ns, ok);
        if (tier1)
            printf(" tier1_retired %llu lower_retired %llu compiles %llu rejects %llu entries %llu",
                   (unsigned long long)(after.tier1_retired - before.tier1_retired),
                   (unsigned long long)(after.lower_retired - before.lower_retired),
                   (unsigned long long)(after.compiles - before.compiles),
                   (unsigned long long)(after.rejects - before.rejects),
                   (unsigned long long)(after.entries - before.entries));
        if (tier1)
            print_compile_ns(after.compile_ticks - before.compile_ticks);
        printf(" cpu_ns %llu validations %llu validation_mismatches %llu",
               (unsigned long long)cpu_ns,
               (unsigned long long)(valid_after.comparisons - valid_before.comparisons),
               (unsigned long long)(valid_after.mismatches - valid_before.mismatches));
        if (tier1)
            printf(" exits_exit %llu exits_checkpoint %llu exits_site %llu exits_helper %llu",
                   (unsigned long long)(after.exits_by_kind[0] - before.exits_by_kind[0]),
                   (unsigned long long)(after.exits_by_kind[1] - before.exits_by_kind[1]),
                   (unsigned long long)(after.exits_by_kind[2] - before.exits_by_kind[2]),
                   (unsigned long long)(after.exits_by_kind[3] - before.exits_by_kind[3]));
        printf(
            " stays_page %llu stays_reset %llu stays_event %llu stays_epoch %llu stays_fetch %llu "
            "bundles_compared %llu",
            (unsigned long long)(stay_after.page - stay_before.page),
            (unsigned long long)(stay_after.reset - stay_before.reset),
            (unsigned long long)(stay_after.event - stay_before.event),
            (unsigned long long)(stay_after.epoch - stay_before.epoch),
            (unsigned long long)(stay_after.fetch - stay_before.fetch),
            (unsigned long long)(stay_after.bundles_compared - stay_before.bundles_compared));
        if (tier1)
            printf(
                " window_misses %llu misses_same_region %llu misses_other_region %llu chainable %llu "
                "chain_links %llu succ_hits %llu native_links %llu native_batches %llu "
                "native_plans %llu native_reuses %llu",
                (unsigned long long)(after.window_misses - before.window_misses),
                (unsigned long long)(after.window_misses_same_region -
                                     before.window_misses_same_region),
                (unsigned long long)(after.window_misses_other_region -
                                     before.window_misses_other_region),
                (unsigned long long)(after.chainable - before.chainable),
                (unsigned long long)(after.chain_links - before.chain_links),
                (unsigned long long)(after.succ_hits - before.succ_hits),
                (unsigned long long)(after.chain_native_links - before.chain_native_links),
                (unsigned long long)(after.chain_native_batches - before.chain_native_batches),
                (unsigned long long)(after.chain_native_plans - before.chain_native_plans),
                (unsigned long long)(after.chain_native_reuses - before.chain_native_reuses));
        printf(" fpsr %#018llx r8 %#llx", (unsigned long long)v->s.ar[AR_FPSR],
               (unsigned long long)v->s.gr[8]);
        printf("\n");
        if (opt_fp_stats)
            print_fp_stats(variant);
        if (profile) {
            struct t1_stats d = after;
            d.tier1_retired -= before.tier1_retired;
            d.lower_retired -= before.lower_retired;
            d.entries -= before.entries;
            prof_print(j->name, variant, &d, v->retired, v);
        }
        if (!ok)
            status = 1;
    }
    free(list);
    write_dump_mem(ram);
    if (regions && tier1)
        print_regions();
    if (registered)
        code_unregister_vcpu(v);
    if (profile) {
        t1h_tier1_set_lower_observer(NULL, NULL);
        t1_test_set_compile_observer(NULL, NULL);
    }
    t1h_engine_release(&ev);
    t1h_unbind(v);
    vcpu_destroy(v);
    return status;
}

int main(int argc, char **argv)
{
    log_set_level(LOG_ERROR);
    t1h_set_ram_allocator(hostwin_linux_ram);
    if (!hostwin_linux_init()) {
        fprintf(stderr, "jobdump: hostwin_linux_init failed\n");
        return 1;
    }
    code_validity_set_track_hook(hostwin_code_page_event);
    tier0_test_set_boundary_mode(TIER0_BOUNDARY_ON);
    const char *program = NULL, *insns = NULL, *out = NULL, *engine = NULL, *job = NULL;
    const char *variants = "0";
    bool profile = false, regions = false;
    unsigned fp_flags = 0;
    int i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); ++i) {
        if (!strncmp(argv[i], "--dump=", 7))
            program = argv[i] + 7;
        else if (!strncmp(argv[i], "--insns=", 8))
            insns = argv[i] + 8;
        else if (!strncmp(argv[i], "--out=", 6))
            out = argv[i] + 6;
        else if (!strncmp(argv[i], "--run=", 6))
            engine = argv[i] + 6;
        else if (!strncmp(argv[i], "--job=", 6))
            job = argv[i] + 6;
        else if (!strncmp(argv[i], "--variants=", 11))
            variants = argv[i] + 11;
        else if (!strcmp(argv[i], "--profile"))
            profile = true;
        else if (!strcmp(argv[i], "--regions"))
            regions = true;
        else if (!strncmp(argv[i], "--fp=", 5)) {
            const char *mode = argv[i] + 5;
            if (!strcmp(mode, "soft"))
                fp_flags = IA64_HOSTFP_CFG_SOFT;
            else if (!strcmp(mode, "exact"))
                fp_flags = 0;
            else if (!strcmp(mode, "experimental"))
                fp_flags = IA64_HOSTFP_CFG_EXPERIMENTAL;
            else if (!strcmp(mode, "approx"))
                fp_flags = IA64_HOSTFP_CFG_EXPERIMENTAL | IA64_HOSTFP_CFG_APPROX;
            else {
                fprintf(stderr, "jobdump: --fp=soft|exact|experimental|approx\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--fp-stats"))
            opt_fp_stats = true;
        else if (!strncmp(argv[i], "--dump-mem=", 11)) {
            char *end;
            struct dump_mem d = {.addr = strtoull(argv[i] + 11, &end, 0)};
            if (*end == ':')
                d.words = strtoull(end + 1, &end, 0);
            if (*end != ':' || !end[1] || ndump_mem == MAX_DUMP_MEM) {
                fprintf(stderr, "jobdump: --dump-mem=ADDR:WORDS:FILE (at most %d)\n", MAX_DUMP_MEM);
                return 2;
            }
            d.file = end + 1;
            dump_mem[ndump_mem++] = d;
        } else if (!strncmp(argv[i], "--boundary=", 11)) {
            /* Tier0's boundary cache: on as in the
             * product (default), off, or checked against the full path. */
            const char *mode = argv[i] + 11;
            if (!strcmp(mode, "on"))
                tier0_test_set_boundary_mode(TIER0_BOUNDARY_ON);
            else if (!strcmp(mode, "off"))
                tier0_test_set_boundary_mode(TIER0_BOUNDARY_OFF);
            else if (!strcmp(mode, "check"))
                tier0_test_set_boundary_mode(TIER0_BOUNDARY_CHECKED);
            else {
                fprintf(stderr, "jobdump: --boundary=on|off|check\n");
                return 2;
            }
        } else {
            fprintf(stderr, "jobdump: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    t1h_set_images(argc - i, argv + i);
    ia64_hostfp_configure(fp_flags | (opt_fp_stats ? IA64_HOSTFP_CFG_CLASSIFY : 0));
    if (program && insns && out)
        return dump(program, insns, out);
    if (engine && job)
        return run_job(engine, job, variants, profile, regions);
    fprintf(
        stderr,
        "usage: bench-jobdump --dump=PROGRAM --insns=N,... --out=FILE [--fp=MODE] IMAGES...\n"
        "       bench-jobdump --run=ENGINE --job=FILE --variants=I,J,... [--profile] [--regions] "
        "[--fp=MODE] [--fp-stats] [--dump-mem=ADDR:WORDS:FILE] IMAGES...\n");
    return 2;
}
