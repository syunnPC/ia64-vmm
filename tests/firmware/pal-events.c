/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The native firmware's INIT, PMI and machine check paths under the
 * interpreter, Tier0 and lockstep: PALE_INIT/PALE_CHECK, SALE_ENTRY with
 * SAL_INIT and SAL_CHECK, PALE_PMI and SALE_PMI, PAL_MC_RESUME and the
 * registrations every processor makes. Expectations follow SDM Rev.2.3
 * Vol.2 §11.3-§11.5 and SAL 245359-005 §4-§6. OS_INIT, OS_MCA and a SAL
 * PMI handler are addresses where this harness takes over, inspects the
 * hand-off state and returns as SAL §4.8.1, §5.4 and SDM §11.5.3 define. */
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/state.h"
#include "ia64/fp.h"
#include "ia64/rse.h"
#include "ia64/interrupt.h"
#include "ia64vmm/platform.h"
#include "ia64vmm/interrupts.h"
#include "ia64vmm/guest_mem.h"
#include "core/log_internal.h"
#include "firmware/new/sal/mc.h"

#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            fprintf(stderr, "PAL events line %d: %s\n", __LINE__, #e);                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define UC (UINT64_C(1) << 63)
#define RAM_BASE UINT64_C(0x100000)
#define RAM_SIZE UINT64_C(0x200000)
#define CTX UINT64_C(0x280000)      /* the interrupted code */
#define RSE_AREA UINT64_C(0x290000) /* its backing store */
#define NEW_AREA UINT64_C(0x2a0000) /* a new context's min-state area */
#define OS_INIT UINT64_C(0x2b0000)
#define OS_INIT_SLAVE UINT64_C(0x2b0100)
#define OS_MCA UINT64_C(0x2b0200)
#define TEST_SALE_PMI UINT64_C(0x2b0400)
#define RET UINT64_C(0x2b0800)
#define OS_GP UINT64_C(0x2c0000)
#define SAL_PROC UINT64_C(0x2d0000)
#define SAL_GP UINT64_C(0x2d8000)
#define BIT(n) (UINT64_C(1) << (n))
/* Processor State Parameter fields (Figure 11-11, Table 11-7). */
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
    PSP_EX = 19,
    PSP_CR = 20,
    PSP_AR = 25,
    PSP_B0 = 30,
    PSP_TC = 60
};
/* Everything this processor saves is valid: rs, cr, pc, dr, tr, rr, ar,
 * br, pr, fp, b1, b0, gr (bits 17, 20-31) and the registered area (mn).
 * Without an area, bank 0 (the SALE_ENTRY parameters) and the ARs (RSC)
 * are not. */
#define PSP_STATE (BIT(PSP_MN) | BIT(PSP_RS) | (UINT64_C(0xfff) << PSP_CR))
#define PSP_UNSAVED (PSP_STATE & ~(BIT(PSP_MN) | BIT(PSP_AR) | BIT(PSP_B0)))
#define KEPT (IA64_PSR_MC | IA64_PSR_MFL | IA64_PSR_MFH | IA64_PSR_PK)
#define RI (UINT64_C(3) << 41) /* PSR.ri is the slot; this CPU keeps it in ip/slot */

static uint8_t ram[RAM_SIZE];
static struct vcpu *cpu;
static struct ia64_state saved;
static uint64_t pal, pale_pmi, sale_pmi, sale_entry, cpu_init, mc_block, min_state, image_end;
static unsigned resets, panics, pib_writes;
static uint64_t reset_type, panic_code, debug_value, pib_address, pib_value, watch;
static bool watched;

static bool pal_register(uint64_t off)
{
    return off == IA64VMM_SVC_PAL_XR0 || off == IA64VMM_SVC_PAL_EXPECTED ||
           off == IA64VMM_SVC_PAL_PALE_PMI || off == IA64VMM_SVC_PAL_SALE_PMI ||
           off == IA64VMM_SVC_PAL_EVENT || off == IA64VMM_SVC_PAL_PMI_VECTOR ||
           off == IA64VMM_SVC_PAL_STATE_SAVE || off == IA64VMM_SVC_PAL_STATE_RESUME;
}
static uint64_t service_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    (void)opaque;
    CHECK(size == 8 && v == cpu && pal_register(off));
    return ia64_pal_service_read(v, (unsigned)off);
}
static void service_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    (void)opaque;
    CHECK(size == 8 && v == cpu);
    if (pal_register(off))
        ia64_pal_service_write(v, (unsigned)off, value);
    else if (off == IA64VMM_SVC_RESET_STATUS)
        CHECK(value == 0);
    else if (off == IA64VMM_SVC_RESET) {
        ++resets;
        reset_type = value;
        vcpu_request_exit(v, VCPU_EXIT_DEBUG);
    } else if (off == IA64VMM_SVC_PANIC) {
        ++panics;
        panic_code = value;
        vcpu_request_exit(v, VCPU_EXIT_DEBUG);
    } else if (off == IA64VMM_SVC_DEBUG)
        debug_value = value;
    else
        CHECK(false);
}
static const struct gm_mmio_ops service_ops = {service_read, service_write};
static void pib_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    (void)opaque;
    (void)v;
    CHECK(size == 8);
    ++pib_writes;
    pib_address = off;
    pib_value = value;
}
static const struct gm_mmio_ops pib_ops = {.write = pib_write};

static void load(const char *path)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    CHECK(!fseek(f, 0, SEEK_END));
    long length = ftell(f);
    CHECK(length > 0);
    rewind(f);
    uint8_t *elf = malloc((size_t)length);
    CHECK(elf && fread(elf, 1, (size_t)length, f) == (size_t)length);
    fclose(f);
    const Elf64_Ehdr *eh = (const void *)elf;
    CHECK(!memcmp(eh->e_ident, ELFMAG, SELFMAG) && eh->e_machine == EM_IA_64);
    for (unsigned i = 0; i < eh->e_phnum; ++i) {
        const Elf64_Phdr *ph = (const void *)(elf + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;
        CHECK(ph->p_vaddr >= RAM_BASE && ph->p_vaddr + ph->p_memsz <= CTX &&
              ph->p_filesz <= ph->p_memsz);
        memcpy(ram + (ph->p_vaddr - RAM_BASE), elf + ph->p_offset, ph->p_filesz);
        if (ph->p_vaddr + ph->p_memsz > image_end)
            image_end = ph->p_vaddr + ph->p_memsz;
    }
    for (unsigned i = 0; i < eh->e_shnum; ++i) {
        const Elf64_Shdr *sh = (const void *)(elf + eh->e_shoff + i * eh->e_shentsize);
        if (sh->sh_type != SHT_SYMTAB)
            continue;
        const Elf64_Shdr *strings =
            (const void *)(elf + eh->e_shoff + sh->sh_link * eh->e_shentsize);
        for (uint64_t at = 0; at < sh->sh_size; at += sizeof(Elf64_Sym)) {
            const Elf64_Sym *sym = (const void *)(elf + sh->sh_offset + at);
            const char *name = (const char *)elf + strings->sh_offset + sym->st_name;
            const struct {
                const char *name;
                uint64_t *value;
            } wanted[] = {{"pal_proc_entry", &pal},     {"pale_pmi", &pale_pmi},
                          {"sale_pmi", &sale_pmi},      {"sale_entry", &sale_entry},
                          {"sal_cpu_init", &cpu_init},  {"sal_mc", &mc_block},
                          {"sal_min_state", &min_state}};
            for (unsigned w = 0; w < sizeof(wanted) / sizeof(wanted[0]); ++w)
                if (!strcmp(name, wanted[w].name))
                    *wanted[w].value = sym->st_value;
            if (!strcmp(name, "pale_init"))
                cpu->cfg.pale_init = sym->st_value;
            if (!strcmp(name, "pale_check"))
                cpu->cfg.pale_check = sym->st_value;
        }
    }
    CHECK(pal && pale_pmi && sale_pmi && sale_entry && cpu_init && mc_block && min_state &&
          cpu->cfg.pale_init && cpu->cfg.pale_check);
    free(elf);
}
static void put(uint64_t address, uint64_t value)
{
    CHECK(gm_write(address, &value, 8));
}
static uint64_t word(uint64_t address)
{
    uint64_t value;
    CHECK(gm_read(address & ~UC, &value, 8));
    return value;
}

/* Steps at least once until IP reaches ip:slot; false at a service reset
 * or panic. Pending interruptions are taken before the first step. */
static bool run_to(uint64_t ip, unsigned slot)
{
    for (unsigned steps = 0; steps < 200000; ++steps) {
        struct vcpu_exit out = engine_run(cpu, 1);
        ia64_pal_complete_services(cpu);
        if (out.reason == VCPU_EXIT_DEBUG && (resets || panics))
            return false;
        if (out.reason != VCPU_EXIT_BUDGET) {
            fprintf(stderr, "exit %u at %llx:%u\n", out.reason, (unsigned long long)out.ip,
                    out.slot);
            vcpu_dump(cpu);
            CHECK(false);
        }
        if (cpu->s.ip == watch && cpu->s.slot == 0)
            watched = true;
        if (cpu->s.ip == ip && cpu->s.slot == slot)
            return true;
    }
    CHECK(false);
    return false;
}
static bool in_image(uint64_t address)
{
    return address >= RAM_BASE && address < image_end && !(address & 15);
}

/* A processor after reset with its registrations: sal_cpu_init called
 * as the IPL and an AP call it (physical mode, bank 1, C convention). The
 * OS registered its handlers (SAL_SET_VECTORS) and SAL published PAL_PROC,
 * SAL_PROC and its gp. */
static void boot(bool registered)
{
    vcpu_reset(cpu, NULL);
    resets = panics = pib_writes = 0;
    reset_type = panic_code = debug_value = 0;
    watch = 0;
    watched = false;
    struct sal_mc mc = {.os_mca = {OS_MCA, OS_GP, 0},
                        .os_init = {{OS_INIT, OS_GP, 0}, {OS_INIT_SLAVE, OS_GP + 8, 0}},
                        .pal_proc = pal,
                        .sal_proc = SAL_PROC,
                        .sal_gp = SAL_GP};
    CHECK(gm_write(mc_block, &mc, sizeof(mc)));
    if (!registered)
        return;
    cpu->s.psr = IA64_PSR_BN;
    cpu->s.ip = cpu_init;
    cpu->s.slot = 0;
    cpu->s.br[0] = RET;
    cpu->s.ar[AR_RSC] = 0;
    cpu->s.ar[AR_BSP] = cpu->s.ar[AR_BSPSTORE] = RSE_AREA + 0x4000;
    CHECK(run_to(RET, 0));
    CHECK(ia64_pal_service_read(cpu, IA64VMM_SVC_PAL_XR0) == (UC | min_state));
    CHECK(ia64_pal_service_read(cpu, IA64VMM_SVC_PAL_PALE_PMI) == pale_pmi);
    CHECK(ia64_pal_service_read(cpu, IA64VMM_SVC_PAL_SALE_PMI) == sale_pmi);
    CHECK(!(min_state & 4095) && !(sale_pmi & 255));
}
/* The interrupted context: distinct values and NaTs in both banks, a
 * six-register frame, branch, application and FP registers. */
static void context(uint64_t ip, unsigned slot, uint64_t psr)
{
    struct ia64_state *s = &cpu->s;
    s->ip = ip;
    s->slot = slot;
    s->psr = psr;
    for (unsigned i = 1; i < 16; ++i) {
        s->gr[i] = 0x1000 + i;
        s->nat[i] = i == 5 || i == 9;
    }
    for (unsigned i = 0; i < 16; ++i) {
        uint64_t b1 = 0x2000 + i, b0 = 0x3000 + i;
        bool n1 = i == 4 || i == 9, n0 = i == 2 || i == 8 || i == 15;
        if (psr & IA64_PSR_BN) {
            s->gr[16 + i] = b1;
            s->nat[16 + i] = n1;
            s->bank_gr[i] = b0;
            s->bank_nat[i] = n0;
        } else {
            s->gr[16 + i] = b0;
            s->nat[16 + i] = n0;
            s->bank_gr[i] = b1;
            s->bank_nat[i] = n1;
        }
    }
    s->pr = UINT64_C(0x5555aaaa5555aaab);
    for (unsigned i = 0; i < 8; ++i) {
        s->br[i] = 0x4000 + 16 * i;
        s->ar[i] = 0xa000 + i;
    }
    s->ar[AR_UNAT] = UINT64_C(0x123456789abcdef0);
    s->ar[AR_RSC] = 3;
    s->ar[AR_PFS] = 0x777;
    s->ar[AR_LC] = 55;
    s->ar[AR_EC] = 9;
    s->ar[AR_CCV] = 0xccc;
    s->ar[AR_RNAT] = 0;
    s->ar[AR_BSP] = s->ar[AR_BSPSTORE] = RSE_AREA;
    s->rse_bof = 0;
    s->rse_dirty = 0;
    s->rse_missing = 0;
    s->rse_cfle = false;
    s->cfm = 6 | (3u << 7);
    for (unsigned i = 0; i < 6; ++i)
        ia64_gr_write(s, 32 + i, 0x6000 + i, i == 1);
    s->fr[2] = (struct ia64_fr){.significand = UINT64_C(0x8000000000000123), .exponent = 0x1003e};
    s->cr[CR_IIP] = 0x11;
    s->cr[CR_IPSR] = 0x22;
    s->cr[CR_IFS] = 0;
    saved = *s;
}
/* SAL §4.8, §5.3 on SDM §11.3.2/§11.4.2: the OS handler's entry state. */
static void handler_entry(uint64_t gp, unsigned function, uint64_t psp)
{
    const struct ia64_state *s = &cpu->s;
    CHECK(!(s->psr & IA64_PSR_BN) && s->psr == (IA64_PSR_MC | (saved.psr & KEPT)));
    CHECK(s->gr[1] == gp && s->gr[8] == pal && s->gr[9] == SAL_PROC && s->gr[10] == SAL_GP);
    CHECK(s->gr[11] == 0 && in_image(s->gr[12]));
    uint64_t area = s->gr[17];
    CHECK(area == (UC | min_state));
    CHECK(s->gr[16] >= area + 0x1d0 && s->gr[16] < area + 1024 && !(s->gr[16] & 7));
    CHECK(s->gr[18] == psp && s->gr[19] == 0 && s->gr[20] == function);
    for (unsigned i = 16; i <= 20; ++i)
        CHECK(!s->nat[i]);
    /* RSE: enforced lazy mode, a zero-size frame, the RSC otherwise kept. */
    CHECK(s->ar[AR_RSC] == (saved.ar[AR_RSC] & ~UINT64_C(3)) && s->cfm == 0);
    CHECK(s->cr[CR_IFS] == (BIT(63) | saved.cfm));
    /* Unchanged: other branch, application and FP registers, bank 1. */
    for (unsigned i = 2; i < 8; ++i)
        CHECK(s->br[i] == saved.br[i]);
    const unsigned ars[] = {0,       1,      2,     3,     4,      5,           6,      7,
                            AR_UNAT, AR_PFS, AR_LC, AR_EC, AR_CCV, AR_BSPSTORE, AR_RNAT};
    for (unsigned i = 0; i < sizeof(ars) / sizeof(ars[0]); ++i)
        CHECK(s->ar[ars[i]] == saved.ar[ars[i]]);
    CHECK(!memcmp(&s->fr[2], &saved.fr[2], sizeof(s->fr[2])));
    for (unsigned i = 0; i < 16; ++i) {
        uint64_t b1 = saved.psr & IA64_PSR_BN ? saved.gr[16 + i] : saved.bank_gr[i];
        CHECK(s->bank_gr[i] == b1);
    }
    /* The complete min-state area, SDM Figure 11-2/11-3. */
    bool bank1 = saved.psr & IA64_PSR_BN;
    uint64_t nat = 0;
    for (unsigned i = 1; i < 16; ++i) {
        CHECK(saved.nat[i] || word(area + 8 * i) == saved.gr[i]);
        nat |= (uint64_t)saved.nat[i] << i;
    }
    for (unsigned i = 0; i < 16; ++i) {
        uint64_t b0 = bank1 ? saved.bank_gr[i] : saved.gr[16 + i],
                 b1 = bank1 ? saved.gr[16 + i] : saved.bank_gr[i];
        bool n0 = bank1 ? saved.bank_nat[i] : saved.nat[16 + i],
             n1 = bank1 ? saved.nat[16 + i] : saved.bank_nat[i];
        CHECK(n0 || word(area + 0x80 + 8 * i) == b0);
        nat |= (uint64_t)n0 << (16 + i);
        CHECK(n1 || word(area + 0x100 + 8 * i) == b1);
        nat |= (uint64_t)n1 << (32 + i);
    }
    CHECK((word(area) & UINT64_C(0x0000fffffffffffe)) == nat);
    CHECK(word(area + 0x180) == saved.pr && word(area + 0x188) == saved.br[0] &&
          word(area + 0x190) == saved.ar[AR_RSC] && word(area + 0x1c8) == saved.br[1]);
    CHECK(word(area + 0x198) == saved.ip);
    CHECK(word(area + 0x1a0) == (saved.psr | (uint64_t)saved.slot << 41));
    CHECK(word(area + 0x1a8) == (BIT(63) | saved.cfm));
}
/* The OS returns to SAL (SAL §4.8.1, §5.4). */
static void handler_return(int64_t status, uint64_t context, uint64_t area)
{
    struct ia64_state *s = &cpu->s;
    s->gr[8] = (uint64_t)status;
    s->gr[9] = s->gr[10];
    s->gr[10] = context;
    s->gr[22] = area;
    s->ip = s->gr[12];
    s->slot = 0;
}
/* Every register of the interrupted context is back (bank 0 GR24-GR31
 * excepted when bank 0 is current: PAL's). A register with its NaT bit set
 * has no defined value (st8.spill stores 0 for it). */
static bool same_gr(uint64_t value, bool nat, uint64_t want, bool want_nat)
{
    return nat == want_nat && (nat || value == want);
}
static void restored(void)
{
    const struct ia64_state *s = &cpu->s;
    CHECK(s->ip == saved.ip && s->slot == saved.slot && (s->psr & ~RI) == saved.psr);
    bool bank1 = saved.psr & IA64_PSR_BN;
    for (unsigned i = 1; i < 32; ++i)
        if (bank1 || i < 24)
            CHECK(same_gr(s->gr[i], s->nat[i], saved.gr[i], saved.nat[i]));
    for (unsigned i = 0; i < 16; ++i)
        if (!bank1 || i < 8)
            CHECK(same_gr(s->bank_gr[i], s->bank_nat[i], saved.bank_gr[i], saved.bank_nat[i]));
    CHECK(s->pr == saved.pr && s->cfm == saved.cfm);
    for (unsigned i = 0; i < 8; ++i)
        CHECK(s->br[i] == saved.br[i]);
    const unsigned ars[] = {0,       1,      2,      3,     4,     5,      6,      7,
                            AR_UNAT, AR_RSC, AR_PFS, AR_LC, AR_EC, AR_CCV, AR_BSP, AR_BSPSTORE};
    for (unsigned i = 0; i < sizeof(ars) / sizeof(ars[0]); ++i)
        CHECK(s->ar[ars[i]] == saved.ar[ars[i]]);
    for (unsigned i = 0; i < 6; ++i)
        CHECK(ia64_gr_read(s, 32 + i) == 0x6000 + i && ia64_nat_read(s, 32 + i) == (i == 1));
    CHECK(!memcmp(&s->fr[2], &saved.fr[2], sizeof(s->fr[2])));
}
static uint64_t active(void)
{
    return word(mc_block + SAL_MC_ACTIVE);
}
/* Code bytes at address and a length_cs whose checksum matches them, off
 * by `wrong` (SAL §9 SAL_SET_VECTORS). */
static uint64_t checksummed(uint64_t address, unsigned wrong)
{
    uint8_t code[32];
    unsigned sum = 0;
    for (unsigned i = 0; i < 32; ++i) {
        code[i] = (uint8_t)(i * 7 + 1);
        sum += code[i];
    }
    CHECK(gm_write(address, code, sizeof(code)));
    uint64_t checksum = (uint8_t)(0x100 - (sum & 0xff) + wrong);
    return checksum << 40 | BIT(32) | 32;
}
/* PALE_INIT/PALE_CHECK without a min-state area (SDM Vol.2 §11.3.2,
 * §11.4.2): nothing saved, GR16 and GR17 0, everything but bank 0 GR16-
 * GR20 and RSC.mode untouched, CFM covered into IFS. */
static void unsaved_entry(unsigned function, uint64_t psp)
{
    const struct ia64_state *s = &cpu->s;
    CHECK(!(s->psr & IA64_PSR_BN) && s->psr == (IA64_PSR_MC | (saved.psr & KEPT)));
    CHECK(s->gr[16] == 0 && s->gr[17] == 0 && s->gr[18] == psp && s->gr[19] == 0 &&
          s->gr[20] == function);
    CHECK(s->ar[AR_RSC] == (saved.ar[AR_RSC] & ~UINT64_C(3)) && s->cfm == 0);
    CHECK(s->cr[CR_IFS] == (BIT(63) | saved.cfm) && s->pr == saved.pr);
    bool collected = saved.psr & IA64_PSR_IC;
    CHECK(s->cr[CR_IIP] == (collected ? saved.ip : 0x11));
    CHECK(s->cr[CR_IPSR] == (collected ? saved.psr | (uint64_t)saved.slot << 41 : 0x22));
    for (unsigned i = 1; i < 16; ++i)
        CHECK(same_gr(s->gr[i], s->nat[i], saved.gr[i], saved.nat[i]));
    for (unsigned i = 0; i < 16; ++i) {
        uint64_t b1 = saved.psr & IA64_PSR_BN ? saved.gr[16 + i] : saved.bank_gr[i];
        bool n1 = saved.psr & IA64_PSR_BN ? saved.nat[16 + i] : saved.bank_nat[i];
        CHECK(same_gr(s->bank_gr[i], s->bank_nat[i], b1, n1));
        if (i < 5 || i > 7)
            continue;
        uint64_t b0 = saved.psr & IA64_PSR_BN ? saved.bank_gr[i] : saved.gr[16 + i];
        bool n0 = saved.psr & IA64_PSR_BN ? saved.bank_nat[i] : saved.nat[16 + i];
        CHECK(same_gr(s->gr[16 + i], s->nat[16 + i], b0, n0));
    }
    for (unsigned i = 0; i < 8; ++i)
        CHECK(s->br[i] == saved.br[i]);
    CHECK(!memcmp(&s->fr[2], &saved.fr[2], sizeof(s->fr[2])));
}

static void test_init(void)
{
    const uint64_t psp = PSP_STATE | BIT(PSP_IN) | BIT(PSP_CO) | BIT(PSP_CI);
    /* PSR.ic = 1: resumed in place, from bank 1 and from bank 0. */
    const uint64_t modes[] = {IA64_PSR_IC | IA64_PSR_BN | IA64_PSR_PK | IA64_PSR_MFH | IA64_PSR_DFH,
                              IA64_PSR_IC};
    for (unsigned m = 0; m < 2; ++m) {
        boot(true);
        context(CTX, 1, modes[m]);
        CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
        CHECK(run_to(OS_INIT, 0));
        handler_entry(OS_GP, 2, psp);
        CHECK(active() == 1);
        handler_return(0, 0, cpu->s.gr[17]);
        CHECK(run_to(saved.ip, saved.slot));
        restored();
        CHECK(!active() && !resets && !panics);
    }
    /* A processor entering SAL_INIT while another one is in it is a
     * non-monarch (§5.1). */
    boot(true);
    put(mc_block + SAL_MC_ACTIVE, 1);
    context(CTX, 1, IA64_PSR_IC | IA64_PSR_BN);
    CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
    CHECK(run_to(OS_INIT_SLAVE, 0));
    handler_entry(OS_GP + 8, 2, psp);
    CHECK(active() == 2);
    handler_return(0, 0, cpu->s.gr[17]);
    CHECK(run_to(saved.ip, saved.slot));
    restored();
    CHECK(active() == 1);
    /* OS_INIT asks for a warm boot (§5.4); the processor leaves. Any other
     * status is outside §5.4: SAL stops with it on the debug line. */
    const int64_t status[] = {-1, 5};
    for (unsigned i = 0; i < 2; ++i) {
        boot(true);
        context(CTX, 1, IA64_PSR_IC | IA64_PSR_BN);
        CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
        CHECK(run_to(OS_INIT, 0));
        handler_return(status[i], 0, cpu->s.gr[17]);
        CHECK(!run_to(saved.ip, saved.slot) && !active());
        if (!i)
            CHECK(resets == 1 && reset_type == 1 && !panics);
        else
            CHECK(panics == 1 && panic_code == 0x47334952 && debug_value == 5 && !resets);
    }
    /* PAL_MC_RESUME refuses the area OS_INIT returned (not uncacheable):
     * SAL stops with PAL's status. */
    boot(true);
    context(CTX, 1, IA64_PSR_IC | IA64_PSR_BN);
    CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
    CHECK(run_to(OS_INIT, 0));
    handler_return(0, 0, cpu->s.gr[17] & ~UC);
    CHECK(!run_to(saved.ip, saved.slot) && panics == 1 && panic_code == 0x4733524d &&
          debug_value == (uint64_t)-2 && !active());
    /* No valid OS_INIT: warm boot (§5.1, §5.5). The checksum covers
     * length_cs{31:0} bytes plus length_cs{47:40} (SAL_SET_VECTORS); an
     * uncacheable entry (bit 63) is checked and entered at that address.
     * An odd generation is a registration in progress: not valid. */
    for (unsigned c = 0; c < 6; ++c) {
        boot(true);
        uint64_t entry = c == 3 ? 0 : c == 4 ? UC | OS_INIT : OS_INIT;
        uint64_t length_cs = checksummed(OS_INIT, c == 2);
        put(mc_block + SAL_MC_OS_INIT, entry);
        put(mc_block + SAL_MC_OS_INIT + 16, c == 0 ? 0 : length_cs);
        if (c == 5)
            put(mc_block + SAL_MC_GENERATION, 3);
        context(CTX, 1, IA64_PSR_IC | IA64_PSR_BN);
        CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
        bool entered = run_to(entry ? entry : OS_INIT, 0);
        CHECK(entered == (c < 2 || c == 4));
        if (!entered)
            CHECK(resets == 1 && reset_type == 1 && !active());
        else {
            handler_entry(OS_GP, 2, psp);
            handler_return(0, 0, cpu->s.gr[17]);
            CHECK(run_to(saved.ip, saved.slot));
            restored();
        }
    }
    /* SDM Vol.2 §11.3.3: resume the handler and retain its interrupted
     * context, including a valid IFS that cover must not overwrite. */
    for (unsigned bank = 0; bank < 2; ++bank) {
        for (unsigned slot = 0; slot < 3; ++slot) {
            boot(true);
            context(CTX, slot, bank ? IA64_PSR_BN : 0);
            cpu->s.cr[CR_IIP] = CTX + 0x200;
            cpu->s.cr[CR_IPSR] = IA64_PSR_IC | IA64_PSR_BN | BIT(41);
            cpu->s.cr[CR_IFS] = BIT(63) | 4;
            saved = cpu->s;
            CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
            CHECK(run_to(OS_INIT, 0));
            handler_return(0, 0, cpu->s.gr[17]);
            CHECK(run_to(saved.ip, saved.slot));
            restored();
            CHECK(cpu->s.cr[CR_IIP] == saved.cr[CR_IIP]);
            CHECK(cpu->s.cr[CR_IPSR] == saved.cr[CR_IPSR]);
            CHECK(cpu->s.cr[CR_IFS] == saved.cr[CR_IFS]);
            for (unsigned i = 8; i < 16; ++i) {
                CHECK(same_gr(cpu->s.gr[16 + i], cpu->s.nat[16 + i], saved.gr[16 + i],
                              saved.nat[16 + i]));
                CHECK(same_gr(cpu->s.bank_gr[i], cpu->s.bank_nat[i], saved.bank_gr[i],
                              saved.bank_nat[i]));
            }
        }
    }
    /* Without a registered min-state area SALE_ENTRY gets mn = 0 and
     * SAL_INIT warm boots: no OS_INIT hand-off state exists (§5.1, §5.3). */
    boot(false);
    context(CTX, 1, IA64_PSR_IC | IA64_PSR_BN);
    CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
    CHECK(run_to(sale_entry, 0));
    unsaved_entry(2, PSP_UNSAVED | BIT(PSP_IN) | BIT(PSP_CO) | BIT(PSP_CI));
    watch = OS_INIT;
    CHECK(!run_to(OS_INIT, 0) && !watched && resets == 1 && reset_type == 1 && !panics &&
          !active());
}

/* SDM Vol.2 §11.3.3: a new handler has independently supplied return
 * resources. Its private PAL area and BR1 are ignored. */
static void test_new_context(void)
{
    for (unsigned bank = 0; bank < 2; ++bank) {
        boot(true);
        context(CTX, 1, IA64_PSR_IC | IA64_PSR_BN);
        CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
        CHECK(run_to(OS_INIT, 0));
        for (unsigned off = 0; off < 0x200; off += 8)
            put(NEW_AREA + off, off < 0x1c8 ? word(cpu->s.gr[17] + off) : UINT64_MAX);
        put(NEW_AREA + 0x198, CTX + 0x100);
        put(NEW_AREA + 0x1a0, (bank ? IA64_PSR_BN : 0) | BIT(42));
        put(NEW_AREA + 0x1b0, CTX + 0x200);
        put(NEW_AREA + 0x1b8, IA64_PSR_IC | IA64_PSR_BN | BIT(41));
        put(NEW_AREA + 0x1c0, BIT(63) | 4);
        handler_return(0, 1, UC | NEW_AREA);
        CHECK(run_to(CTX + 0x100, 2));
        CHECK((cpu->s.psr & ~RI) == (bank ? IA64_PSR_BN : 0));
        CHECK(cpu->s.cr[CR_IIP] == CTX + 0x200);
        CHECK(cpu->s.cr[CR_IPSR] == (IA64_PSR_IC | IA64_PSR_BN | BIT(41)));
        CHECK(cpu->s.cr[CR_IFS] == (BIT(63) | 4));
        CHECK(cpu->s.cfm == saved.cfm && !cpu->s.rse_completion_pending);
        CHECK(cpu->s.br[1] != UINT64_MAX && !cpu->pal_state_valid);
    }
    /* Return twice: PAL_MC_RESUME resumes a zero-frame handler, whose
     * ordinary rfi then resumes the original process in slot 1. */
    boot(true);
    context(CTX, 2, 0);
    cpu->s.cfm = 0;
    cpu->s.cr[CR_IIP] = CTX + 0x200;
    cpu->s.cr[CR_IPSR] = IA64_PSR_IC | IA64_PSR_BN | BIT(41);
    cpu->s.cr[CR_IFS] = 0;
    saved = cpu->s;
    uint64_t nop = UINT64_C(1) << 27, rfi = UINT64_C(8) << 27;
    put(CTX, 0x11 | (nop << 5) | (nop << 46));
    put(CTX + 8, (nop >> 18) | (rfi << 23));
    CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
    CHECK(run_to(OS_INIT, 0));
    handler_return(0, 0, cpu->s.gr[17]);
    CHECK(run_to(CTX, 2));
    CHECK(run_to(CTX + 0x200, 1));
    CHECK(cpu->s.psr == (IA64_PSR_IC | IA64_PSR_BN | BIT(41)) && cpu->s.cfm == 0);
}

/* A wrapped rotating frame crosses an RNAT slot while PAL covers it. */
static void test_rotating_frame(void)
{
    boot(true);
    context(CTX, 1, IA64_PSR_BN);
    struct ia64_state *s = &cpu->s;
    s->cfm = 16 | (8u << 7) | (2u << 14) | (7u << 18) | (UINT64_C(13) << 25) | (UINT64_C(9) << 32);
    s->rse_bof = 91;
    s->rse_dirty = 5;
    s->ar[AR_BSP] = RSE_AREA + 0x1d8;
    s->ar[AR_BSPSTORE] = RSE_AREA + 0x1b0;
    for (unsigned i = 0; i < 16; ++i)
        ia64_gr_write(s, 32 + i, 0x8000 + i, (i & 3) == 1);
    saved = *s;
    CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
    CHECK(run_to(OS_INIT, 0));
    CHECK(s->rse_bof == 11 && s->rse_dirty == 21);
    CHECK(s->ar[AR_BSP] == RSE_AREA + 0x260);
    handler_return(0, 0, s->gr[17]);
    CHECK(run_to(saved.ip, saved.slot));
    CHECK(s->cfm == saved.cfm && s->pr == saved.pr);
    CHECK(s->rse_bof == saved.rse_bof && s->rse_dirty == saved.rse_dirty);
    CHECK(s->ar[AR_BSP] == saved.ar[AR_BSP] && s->ar[AR_BSPSTORE] == saved.ar[AR_BSPSTORE]);
    for (unsigned i = 0; i < 16; ++i)
        CHECK(ia64_gr_read(s, 32 + i) == 0x8000 + i && ia64_nat_read(s, 32 + i) == ((i & 3) == 1));
}

/* A nested ordinary interruption in the event handler must not consume
 * the interrupted br.ret's single-step completion (Vol.2 §§6.8, 7.1). */
static void test_nested_completion(void)
{
    boot(true);
    context(CTX, 1, IA64_PSR_BN | IA64_PSR_IC);
    struct ia64_state *s = &cpu->s;
    s->rse_missing = 2;
    s->rse_cfle = true;
    s->ar[AR_BSPSTORE] = RSE_AREA + 16;
    s->rse_completion_pending = true;
    s->rse_completion_psr = IA64_PSR_BN | IA64_PSR_SS;
    s->rse_completion_ip = CTX - 16;
    s->rse_completion_slot = 2;
    saved = *s;
    CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
    CHECK(run_to(OS_INIT, 0));
    const uint64_t iva = CTX + 0x8000, handler = iva + 0x3000;
    uint64_t nop = UINT64_C(1) << 27, rfi = UINT64_C(8) << 27;
    put(handler, 0x11 | (nop << 5) | (nop << 46));
    put(handler + 8, (nop >> 18) | (rfi << 23));
    s->cr[CR_IVA] = iva;
    ia64_psr_write(s, IA64_PSR_MC | IA64_PSR_IC | IA64_PSR_I);
    CHECK(vcpu_accept_interrupt(cpu, 0x40) && ia64_external_deliver(cpu));
    CHECK(s->ip == handler && ia64_ivr_read(cpu) == 0x40);
    ia64_eoi(cpu);
    CHECK(run_to(OS_INIT, 0));
    CHECK(ia64_rse_resume(cpu).kind == IA64_STEP_COMPLETE);
    CHECK(s->ip == OS_INIT && !s->rse_completion_pending);
    ia64_psr_write(s, IA64_PSR_MC);
    handler_return(0, 0, s->gr[17]);
    CHECK(run_to(saved.ip, saved.slot));
    CHECK(s->rse_missing == 2 && s->rse_cfle && s->rse_completion_pending);
    CHECK(s->rse_completion_psr == saved.rse_completion_psr);
    CHECK(s->rse_completion_ip == saved.rse_completion_ip && s->rse_completion_slot == 2);
}

/* SDM Vol.2 §6.8: an interrupted mandatory-fill sequence keeps its
 * unloaded prefix and deferred br.ret completion across PAL's cover. */
static void test_incomplete_frame(void)
{
    for (unsigned mode = 0; mode < 4; ++mode) {
        unsigned collect = mode & 1;
        bool active_fill = mode & 2;
        boot(true);
        context(CTX, 1, IA64_PSR_BN | (collect ? IA64_PSR_IC : 0));
        cpu->s.rse_missing = 2;
        cpu->s.rse_cfle = active_fill;
        cpu->s.ar[AR_BSPSTORE] = RSE_AREA + 16;
        cpu->s.rse_completion_pending = true;
        cpu->s.rse_completion_psr = IA64_PSR_BN;
        cpu->s.rse_completion_ip = CTX - 16;
        cpu->s.rse_completion_slot = 2;
        cpu->s.rse_completion_demoted = true;
        saved = cpu->s;
        CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
        CHECK(run_to(OS_INIT, 0));
        CHECK(!cpu->s.rse_missing && !cpu->s.rse_cfle && !cpu->s.rse_completion_pending);
        handler_return(0, 0, cpu->s.gr[17]);
        CHECK(run_to(saved.ip, saved.slot));
        CHECK(cpu->s.rse_missing == 2 && cpu->s.rse_cfle == active_fill);
        CHECK(cpu->s.ar[AR_BSPSTORE] == saved.ar[AR_BSPSTORE]);
        CHECK(cpu->s.rse_completion_pending && cpu->s.rse_completion_demoted);
        CHECK(cpu->s.rse_completion_psr == saved.rse_completion_psr);
        CHECK(cpu->s.rse_completion_ip == saved.rse_completion_ip);
        CHECK(cpu->s.rse_completion_slot == saved.rse_completion_slot);
        if (!active_fill) {
            CHECK(ia64_rse_resume(cpu).kind == IA64_STEP_COMPLETE && cpu->s.rse_missing == 2);
            cpu->s.cr[CR_IIP] = saved.ip;
            cpu->s.cr[CR_IPSR] = IA64_PSR_BN | IA64_PSR_IC;
            cpu->s.cr[CR_IFS] = 0;
            Ia64Instruction rfi = {.opcode = IA64_OP_RFI, .unit = IA64_UNIT_B, .valid = true};
            CHECK(ia64_execute(cpu, &rfi).kind == IA64_STEP_BRANCH);
        }
        put(RSE_AREA, 0x6000);
        put(RSE_AREA + 8, 0x6001);
        cpu->s.ar[AR_RNAT] = BIT(((RSE_AREA + 8) >> 3) & 63);
        CHECK(ia64_rse_resume(cpu).kind == IA64_STEP_COMPLETE);
        CHECK(!cpu->s.rse_missing && !cpu->s.rse_cfle && !cpu->s.rse_completion_pending);
        CHECK(ia64_gr_read(&cpu->s, 32) == 0x6000 && !ia64_nat_read(&cpu->s, 32));
        CHECK(ia64_gr_read(&cpu->s, 33) == 0x6001 && ia64_nat_read(&cpu->s, 33));
    }
}

/* itr.d dtr[r10] = r9 over DTR 0's page, with PSR.ic = 0 (Table 4-1). */
static void overlap(uint64_t ip)
{
    const uint64_t nop = UINT64_C(1) << 27, self = UINT64_C(4) << 37;
    const uint64_t itr =
        (UINT64_C(1) << 37) | (UINT64_C(0x0e) << 27) | (UINT64_C(10) << 20) | (UINT64_C(9) << 13);
    uint64_t low = 0x11 | (itr << 5) | (nop << 46), high = (nop >> 18) | (self << 23);
    put(ip, low);
    put(ip + 8, high);
    struct ia64_state *s = &cpu->s;
    s->rr[0] = s->rr[1] = 0x100030;
    s->tr[1][0] = (struct ia64_tlb_entry){.va = 0x900000,
                                          .rid = 0x1000,
                                          .page_shift = 20,
                                          .itir = 80,
                                          .pte = 0x900661,
                                          .valid = true};
    s->gr[9] = 0x940661;
    s->gr[10] = 1;
    s->nat[9] = false;
    s->cr[CR_IFA] = (UINT64_C(1) << 61) | 0x940000;
    s->cr[CR_ITIR] = 18u << 2;
    saved = *s;
}
static void test_machine_check(void)
{
    /* The overlapping insert has not completed. PAL hands the isolated
     * TLB check to software without asserting continuable (Table 11-8). */
    const uint64_t psp = PSP_STATE | BIT(PSP_CI) | BIT(PSP_TC);
    for (unsigned expected = 0; expected < 2; ++expected) {
        boot(true);
        context(CTX + 0x40, 0, IA64_PSR_BN | IA64_PSR_MFL);
        overlap(CTX + 0x40);
        ia64_pal_service_write(cpu, IA64VMM_SVC_PAL_EXPECTED, expected);
        CHECK(run_to(OS_MCA, 0));
        handler_entry(OS_GP, 1, psp | (expected ? BIT(PSP_EX) : 0));
        CHECK(!cpu->s.tr[1][1].valid && cpu->s.tr[1][0].pte == 0x900661);
        /* A new context (§11.3.3, SAL §4.5): the bundle after the itr, with
         * collection on and GR4 changed; the old area stays untouched. */
        for (unsigned off = 0; off < 0x1d0; off += 8)
            put(NEW_AREA + off, word(cpu->s.gr[17] + off));
        put(NEW_AREA + 0x198, CTX + 0x80);
        put(NEW_AREA + 0x1a0, IA64_PSR_BN | IA64_PSR_IC);
        put(NEW_AREA + 0x20, 0x4444);
        put(NEW_AREA, word(NEW_AREA) & ~BIT(4));
        handler_return(0, 1, UC | NEW_AREA);
        CHECK(run_to(CTX + 0x80, 0));
        const struct ia64_state *s = &cpu->s;
        CHECK((s->psr & ~RI) == (IA64_PSR_BN | IA64_PSR_IC) && s->gr[4] == 0x4444 && !s->nat[4]);
        CHECK(s->gr[3] == 0x1003 && s->nat[5] && s->gr[16] == 0x2000 && s->nat[20] && !s->nat[21]);
        CHECK(s->bank_gr[3] == 0x3003 && s->bank_nat[2] && s->cfm == saved.cfm &&
              s->pr == saved.pr);
        CHECK(s->br[0] == saved.br[0] && s->ar[AR_UNAT] == saved.ar[AR_UNAT] &&
              s->ar[AR_RSC] == saved.ar[AR_RSC]);
        CHECK(!s->tr[1][1].valid && !resets && !panics);
    }
    /* An ic=0 machine check returns to the interrupted instruction. */
    boot(true);
    context(CTX + 0x40, 0, IA64_PSR_BN);
    overlap(CTX + 0x40);
    CHECK(run_to(OS_MCA, 0));
    handler_return(0, 0, cpu->s.gr[17]);
    CHECK(run_to(CTX + 0x40, 0) && !panics);
    restored();
    /* PSR.mc = 1 holds the machine check; it is taken, collected, once
     * PSR.mc is 0 again, at whatever instruction comes then. Still only an
     * isolated TLB check: nothing about that instruction (Table 11-7). */
    boot(true);
    context(CTX + 0x40, 0, IA64_PSR_BN | IA64_PSR_MC);
    overlap(CTX + 0x40);
    struct vcpu_exit out = engine_run(cpu, 1);
    CHECK(out.reason == VCPU_EXIT_BUDGET && cpu->pal_mca_pending && !cpu->s.tr[1][1].valid);
    cpu->s.psr = IA64_PSR_BN | IA64_PSR_IC;
    cpu->s.ip = CTX + 0x80;
    cpu->s.slot = 0;
    saved = cpu->s;
    CHECK(run_to(OS_MCA, 0));
    handler_entry(OS_GP, 1, psp);
    CHECK(!(cpu->s.gr[18] & (BIT(PSP_CO) | BIT(PSP_SY) | BIT(PSP_PI) | BIT(PSP_PM))));
    handler_return(0, 0, cpu->s.gr[17]);
    CHECK(run_to(saved.ip, saved.slot));
    restored();
    CHECK(!resets && !panics);
    /* SAL §4.8.1: -1 warm boot, -2 cold boot, -3 halt; SAL stops with the
     * status on the debug line. */
    const int64_t status[] = {-1, -2, -3};
    for (unsigned i = 0; i < 3; ++i) {
        boot(true);
        context(CTX + 0x40, 0, IA64_PSR_BN);
        overlap(CTX + 0x40);
        CHECK(run_to(OS_MCA, 0));
        handler_return(status[i], 0, cpu->s.gr[17]);
        CHECK(!run_to(CTX + 0x40, 0));
        if (i < 2)
            CHECK(resets == 1 && reset_type == (i == 0 ? 1 : 0) && !panics);
        else
            CHECK(panics == 1 && panic_code == 0x47334d48 && debug_value == (uint64_t)-3 &&
                  !resets);
    }
    /* SAL §4.1: an uncacheable OS_MCA (bit 63) with its checksum is
     * checked and entered at that address. Not registered, a failed
     * checksum or a registration in progress: SAL_CHECK halts. */
    for (unsigned c = 0; c < 4; ++c) {
        boot(true);
        uint64_t length_cs = checksummed(OS_MCA, c == 2);
        put(mc_block + SAL_MC_OS_MCA, c == 1 ? 0 : UC | OS_MCA);
        put(mc_block + SAL_MC_OS_MCA + 16, length_cs);
        if (c == 3)
            put(mc_block + SAL_MC_GENERATION, 1);
        context(CTX + 0x40, 0, IA64_PSR_BN);
        overlap(CTX + 0x40);
        if (!c) {
            CHECK(run_to(UC | OS_MCA, 0));
            handler_entry(OS_GP, 1, psp);
        } else
            CHECK(!run_to(UC | OS_MCA, 0) && panics == 1 && panic_code == 0x47334d4e &&
                  debug_value == psp);
    }
    /* Without a min-state area SALE_ENTRY gets mn = 0 and SAL_CHECK halts:
     * no OS_MCA hand-off state exists (SAL §4.8). */
    boot(false);
    context(CTX + 0x40, 0, IA64_PSR_BN);
    overlap(CTX + 0x40);
    CHECK(run_to(sale_entry, 0));
    unsaved_entry(1, PSP_UNSAVED | BIT(PSP_CI) | BIT(PSP_TC));
    watch = OS_MCA;
    CHECK(!run_to(OS_MCA, 0) && !watched && panics == 1 && panic_code == 0x47334d53 &&
          debug_value == (PSP_UNSAVED | BIT(PSP_CI) | BIT(PSP_TC)));
}

/* ptc.l, ptc.g or ptc.ga (M45 x6 0x09-0x0b) r10, r9 alone in the first
 * instruction group of an M;;MI;; bundle at ip, with r10 = 0x900000 in
 * region 1 and r9 = 1 MiB << 2: over DTR 0 at 0x940000 (4 KiB) and a DTC
 * beside it, both RID 0x1000. The next bundle branches to itself. */
static void purge_overlap(uint64_t ip, unsigned x6)
{
    const uint64_t nop = UINT64_C(1) << 27, self = UINT64_C(4) << 37;
    const uint64_t ptc =
        (UINT64_C(1) << 37) | ((uint64_t)x6 << 27) | (UINT64_C(10) << 20) | (UINT64_C(9) << 13);
    put(ip, 0x0b | (ptc << 5) | (nop << 46));
    put(ip + 8, (nop >> 18) | (nop << 23));
    put(ip + 16, 0x11 | (nop << 5) | (nop << 46));
    put(ip + 24, (nop >> 18) | (self << 23));
    struct ia64_state *s = &cpu->s;
    s->rr[0] = s->rr[1] = 0x100030;
    s->tr[1][0] = (struct ia64_tlb_entry){.va = 0x940000,
                                          .rid = 0x1000,
                                          .page_shift = 12,
                                          .itir = 48,
                                          .pte = 0x940661,
                                          .valid = true};
    s->tc[1][0] = (struct ia64_tlb_entry){.va = 0x950000,
                                          .rid = 0x1000,
                                          .page_shift = 12,
                                          .itir = 48,
                                          .pte = 0x950661,
                                          .valid = true};
    s->gr[9] = 20u << 2;
    s->gr[10] = (UINT64_C(1) << 61) | 0x900000;
    s->nat[9] = s->nat[10] = false;
    saved = *s;
}
static void test_purge_machine_check(void)
{
    /* Table 4-1: a local purge over a TR must machine check; the TC part
     * is still purged and the TR stays (Vol.2 §4.1.1.2, §5.2.2.2.3). The
     * machine check follows the completed purge (footnote h): with PSR.ic
     * = 1 the min-state area holds the next instruction, slot 1, and
     * resuming that context continues there without a second one. The
     * purge completed and every saved resource is valid: an isolated,
     * continuable TLB check (co); pi and pm stay 0, IIP is not the purge
     * (Table 11-7). */
    const uint64_t psp = PSP_STATE | BIT(PSP_CI) | BIT(PSP_TC);
    for (unsigned x6 = 0x09; x6 <= 0x0b; ++x6) {
        boot(true);
        context(CTX + 0x40, 0, IA64_PSR_IC | IA64_PSR_BN | IA64_PSR_MFH);
        purge_overlap(CTX + 0x40, x6);
        saved.slot = 1;
        CHECK(run_to(OS_MCA, 0));
        handler_entry(OS_GP, 1, psp | BIT(PSP_CO));
        const struct ia64_state *s = &cpu->s;
        CHECK(s->tr[1][0].valid && s->tr[1][0].pte == 0x940661 && !s->tc[1][0].valid);
        handler_return(0, 0, cpu->s.gr[17]);
        CHECK(run_to(saved.ip, saved.slot));
        restored();
        watch = OS_MCA;
        CHECK(run_to(CTX + 0x50, 0) && !watched && !cpu->pal_mca_pending && !resets && !panics);
    }
    /* An ic=0 purge resumes after the completed purge. */
    boot(true);
    context(CTX + 0x40, 0, IA64_PSR_BN);
    purge_overlap(CTX + 0x40, 0x09);
    CHECK(run_to(OS_MCA, 0));
    saved.slot = 1;
    handler_entry(OS_GP, 1, psp);
    CHECK(cpu->s.tr[1][0].valid && !cpu->s.tc[1][0].valid);
    handler_return(0, 0, cpu->s.gr[17]);
    CHECK(run_to(CTX + 0x40, 1) && !panics);
    /* Without a min-state area nothing is saved: not continuable even with
     * PSR.ic = 1, and SAL_CHECK halts (SAL §4.8). */
    boot(false);
    context(CTX + 0x40, 0, IA64_PSR_IC | IA64_PSR_BN);
    purge_overlap(CTX + 0x40, 0x09);
    saved.slot = 1;
    CHECK(run_to(sale_entry, 0));
    unsaved_entry(1, PSP_UNSAVED | BIT(PSP_CI) | BIT(PSP_TC));
    watch = OS_MCA;
    CHECK(!run_to(OS_MCA, 0) && !watched && panics == 1 && panic_code == 0x47334d53);
}

static void test_pmi(void)
{
    /* A SAL vector through SAL's own SALE_PMI, which returns at once. */
    boot(true);
    context(CTX, 1, IA64_PSR_IC | IA64_PSR_BN | IA64_PSR_PK);
    watch = sale_pmi;
    CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_PMI, 2));
    CHECK(run_to(saved.ip, saved.slot) && watched);
    restored();
    /* §11.5.2: GR24 the vector, GR25 XR0, GR26-GR29 the saved RSC, BR0,
     * BR1 and predicates, BR0 PAL's return; bank 0 GR16-GR23 and the other
     * registers as interrupted; RSC.mode = 0; PSR mc/mfl/mfh/pk only. */
    boot(true);
    ia64_pal_service_write(cpu, IA64VMM_SVC_PAL_SALE_PMI, TEST_SALE_PMI);
    context(CTX, 1, IA64_PSR_IC | IA64_PSR_BN | IA64_PSR_MFL);
    CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_PMI, 3));
    CHECK(run_to(TEST_SALE_PMI, 0));
    const struct ia64_state *s = &cpu->s;
    CHECK(s->psr == IA64_PSR_MFL && s->gr[24] == 3 && s->gr[25] == (UC | min_state));
    CHECK(s->gr[26] == saved.ar[AR_RSC] && s->gr[27] == saved.br[0] && s->gr[28] == saved.br[1] &&
          s->gr[29] == saved.pr);
    CHECK(s->br[0] >= pal && s->br[0] < pal + 32768 && !(s->br[0] & 15));
    CHECK(s->ar[AR_RSC] == (saved.ar[AR_RSC] & ~UINT64_C(3)) && s->cfm == saved.cfm);
    for (unsigned i = 0; i < 8; ++i)
        CHECK(s->gr[16 + i] == saved.bank_gr[i]);
    for (unsigned i = 1; i < 16; ++i)
        CHECK(same_gr(s->gr[i], s->nat[i], saved.gr[i], saved.nat[i]));
    for (unsigned i = 0; i < 16; ++i)
        CHECK(same_gr(s->bank_gr[i], s->bank_nat[i], saved.gr[16 + i], saved.nat[16 + i]));
    cpu->s.ip = s->br[0];
    cpu->s.slot = 0;
    CHECK(run_to(saved.ip, saved.slot));
    restored();
    /* PAL vectors 4-15 never reach SAL; vector 0 without a SAL handler
     * returns (§11.5.1). */
    boot(true);
    ia64_pal_service_write(cpu, IA64VMM_SVC_PAL_SALE_PMI, TEST_SALE_PMI);
    watch = TEST_SALE_PMI;
    context(CTX, 1, IA64_PSR_IC | IA64_PSR_BN);
    CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_PMI, 9));
    CHECK(run_to(saved.ip, saved.slot) && !watched);
    restored();
    ia64_pal_service_write(cpu, IA64VMM_SVC_PAL_SALE_PMI, 0);
    CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_PMI, 0));
    CHECK(run_to(saved.ip, saved.slot) && !watched);
    restored();
}

/* PAL_MC_RESUME's set_cmci (SDM Vol.2 PAL_MC_RESUME): the CMC vector
 * through this processor's PIB entry unless CMCV masks it; invalid
 * arguments return -2. Called as SAL would, from the OS_INIT hand-off. */
static void test_resume_arguments(void)
{
    for (unsigned masked = 0; masked < 2; ++masked) {
        boot(true);
        context(CTX, 1, IA64_PSR_IC | IA64_PSR_BN);
        CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
        CHECK(run_to(OS_INIT, 0));
        cpu->s.cr[CR_CMCV] = 0x35 | (masked ? BIT(16) : 0);
        cpu->s.gr[28] = 26;
        cpu->s.gr[29] = 1;
        cpu->s.gr[30] = cpu->s.gr[17];
        cpu->s.gr[31] = 0;
        cpu->s.br[0] = RET;
        cpu->s.ip = pal;
        cpu->s.slot = 0;
        CHECK(run_to(saved.ip, saved.slot));
        CHECK(pib_writes == !masked);
        if (!masked)
            CHECK(pib_address == 0 && pib_value == 0x35);
    }
    boot(true);
    context(CTX, 1, IA64_PSR_IC | IA64_PSR_BN);
    CHECK(vcpu_accept_pal_interrupt(cpu, IA64_IRQ_INIT, 0));
    CHECK(run_to(OS_INIT, 0));
    /* Beyond the limit, misaligned, cacheable; set_cmci and new_context
     * other than 0 or 1 are reserved. */
    const uint64_t area = UC | min_state;
    const uint64_t bad[][3] = {{0, UC | ((UINT64_C(1) << 50) - 0x200), 0},
                               {0, UC | (min_state + 0x100), 0},
                               {0, min_state, 0},
                               {2, area, 0},
                               {0, area, 2}};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        cpu->s.gr[28] = 26;
        cpu->s.gr[29] = bad[i][0];
        cpu->s.gr[30] = bad[i][1];
        cpu->s.gr[31] = bad[i][2];
        cpu->s.br[0] = RET;
        cpu->s.ip = pal;
        cpu->s.slot = 0;
        CHECK(run_to(RET, 0) && cpu->s.gr[8] == (uint64_t)-2);
    }
}

int main(int argc, char **argv)
{
    log_set_level(LOG_ERROR);
    CHECK(argc >= 2 && argc <= 4);
    struct vcpu_config cfg = {.cpu_count = 1, .itc_icount = true};
    if (argc > 2)
        cfg.engine = !strcmp(argv[2], "tier0")      ? ENGINE_TIER0
                     : !strcmp(argv[2], "lockstep") ? ENGINE_LOCKSTEP
                                                    : ENGINE_INTERP;
    if (argc > 3)
        cfg.cpu_model = !strcmp(argv[3], "madison") ? IA64_CPU_MADISON : IA64_CPU_MONTECITO;
    cpu = vcpu_create(&cfg);
    CHECK(cpu);
    load(argv[1]);
    struct gm_region region = {
        .base = RAM_BASE, .size = RAM_SIZE, .kind = GM_RAM, .hva = ram, .name = "ram"};
    CHECK(gm_add_region(&region));
    struct gm_region service = {
        .base = IA64VMM_SERVICE_BASE, .size = 4096, .kind = GM_EMULATED, .ops = &service_ops};
    CHECK(gm_add_region(&service));
    struct gm_region pib = {
        .base = IA64VMM_PIB_BASE, .size = 4096, .kind = GM_EMULATED, .ops = &pib_ops};
    CHECK(gm_add_region(&pib));
    test_init();
    test_new_context();
    test_rotating_frame();
    test_nested_completion();
    test_incomplete_frame();
    test_machine_check();
    test_purge_machine_check();
    test_pmi();
    test_resume_arguments();
    vcpu_destroy(cpu);
    puts(
        "PAL events: PALE_INIT/PALE_CHECK, SAL_INIT/SAL_CHECK, PALE_PMI/SALE_PMI and PAL_MC_RESUME passed");
    return 0;
}
