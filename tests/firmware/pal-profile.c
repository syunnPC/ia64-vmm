/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SDM Rev.2.3 Vol.1 §3.1.11; Vol.2 PAL procedure definitions. Literal
 * cache/version vectors follow fork target/ia64/{cpu.c,arch/pal.c}.
 * Execute native firmware instructions and the reference firmware's C
 * portal against the same expectations; no firmware byte comparisons. */
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/state.h"
#include "ia64/pal.h"
#include "ia64vmm/platform.h"
#include "ia64vmm/guest_mem.h"
#include "core/log_internal.h"
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            fprintf(stderr, "PAL profile line %d: %s\n", __LINE__, #e);                            \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define BUFFER UINT64_C(0x180000)
static uint8_t ram[0x100000];
static uint8_t image[32768];
static uint64_t count_offset, pmi_offset;
static uint64_t pal_entry = 0x100000;
static unsigned model, native, device_writes;
static struct vcpu *cpu;

static bool pal_register(uint64_t off)
{
    return off == IA64VMM_SVC_PAL_XR0 || off == IA64VMM_SVC_PAL_EXPECTED ||
           off == IA64VMM_SVC_PAL_PALE_PMI || off == IA64VMM_SVC_PAL_SALE_PMI ||
           off == IA64VMM_SVC_PAL_EVENT || off == IA64VMM_SVC_PAL_PMI_VECTOR;
}
static uint64_t service_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    (void)opaque;
    CHECK(size == 8);
    if (off == IA64VMM_SVC_TR_STATUS)
        return ia64_pal_tr_status(v);
    if (off == IA64VMM_SVC_TR_VALID)
        return ia64_pal_tr_valid(v);
    if (pal_register(off))
        return ia64_pal_service_read(v, (unsigned)off);
    CHECK(false);
    return 0;
}
static void service_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    (void)opaque;
    CHECK(size == 8);
    if (off == IA64VMM_SVC_TR_SELECT)
        ia64_pal_tr_select(v, value);
    else if (off == IA64VMM_SVC_TR_READ) {
        ia64_pal_tr_read(v, value);
        ia64_pal_complete_services(v);
        v->requested_exit = 0;
    } else if (off == IA64VMM_SVC_HALT)
        vcpu_request_exit(v, VCPU_EXIT_HALT);
    else if (pal_register(off))
        ia64_pal_service_write(v, (unsigned)off, value);
    else
        CHECK(false);
}
static const struct gm_mmio_ops service_ops = {service_read, service_write};

static void device_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    (void)opaque;
    (void)v;
    (void)off;
    (void)value;
    (void)size;
    ++device_writes;
}
static const struct gm_mmio_ops device_ops = {.write = device_write};

static void load(const char *path)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    Elf64_Ehdr eh;
    CHECK(fread(&eh, 1, sizeof(eh), f) == sizeof(eh));
    CHECK(!memcmp(eh.e_ident, ELFMAG, SELFMAG) && eh.e_machine == EM_IA_64);
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph;
        CHECK(!fseek(f, (long)(eh.e_phoff + i * eh.e_phentsize), SEEK_SET));
        CHECK(fread(&ph, 1, sizeof(ph), f) == sizeof(ph));
        if (ph.p_type != PT_LOAD)
            continue;
        CHECK(ph.p_vaddr == 0x100000 && ph.p_filesz == sizeof(image));
        CHECK(!fseek(f, (long)ph.p_offset, SEEK_SET));
        CHECK(fread(image, 1, sizeof(image), f) == sizeof(image));
    }
    for (unsigned i = 0; i < eh.e_shnum; ++i) {
        Elf64_Shdr sh, strings;
        CHECK(!fseek(f, (long)(eh.e_shoff + i * eh.e_shentsize), SEEK_SET));
        CHECK(fread(&sh, 1, sizeof(sh), f) == sizeof(sh));
        if (sh.sh_type != SHT_SYMTAB)
            continue;
        CHECK(!fseek(f, (long)(eh.e_shoff + sh.sh_link * eh.e_shentsize), SEEK_SET));
        CHECK(fread(&strings, 1, sizeof(strings), f) == sizeof(strings));
        for (uint64_t at = 0; at < sh.sh_size; at += sizeof(Elf64_Sym)) {
            Elf64_Sym sym;
            char name[64] = {0};
            CHECK(!fseek(f, (long)(sh.sh_offset + at), SEEK_SET));
            CHECK(fread(&sym, 1, sizeof(sym), f) == sizeof(sym));
            CHECK(!fseek(f, (long)(strings.sh_offset + sym.st_name), SEEK_SET));
            CHECK(fread(name, 1, sizeof(name) - 1, f) > 0);
            if (!strcmp(name, "pal_cpu_count"))
                count_offset = sym.st_value - 0x100000;
            if (!strcmp(name, "pale_pmi"))
                pmi_offset = sym.st_value - 0x100000;
        }
    }
    CHECK(count_offset && count_offset < sizeof(image) - 8 && pmi_offset &&
          pmi_offset < sizeof(image));
    CHECK(!fclose(f));
}
static void put(uint64_t address, uint64_t value)
{
    CHECK(gm_write(address, &value, 8));
}
static uint64_t word(uint64_t address)
{
    uint64_t value;
    CHECK(gm_read(address, &value, 8));
    return value;
}

static void call(unsigned index, uint64_t a, uint64_t b, uint64_t c, int64_t status, uint64_t x,
                 uint64_t y, uint64_t z)
{
    cpu->s.ip = pal_entry;
    cpu->s.slot = 0;
    cpu->s.cfm = (index & 256) ? 4 : 0;
    cpu->s.ar[AR_PFS] = 0;
    cpu->s.br[0] = 0x170000;
    cpu->requested_exit = 0;
    ia64_gr_write(&cpu->s, 28, index, false);
    if (index & 256)
        ia64_gr_write(&cpu->s, 32, index, false);
    unsigned first = (index & 256) ? 33 : 29;
    ia64_gr_write(&cpu->s, first, a, false);
    ia64_gr_write(&cpu->s, first + 1, b, false);
    ia64_gr_write(&cpu->s, first + 2, c, false);
    for (unsigned i = 8; i < 12; ++i)
        ia64_gr_write(&cpu->s, i, UINT64_MAX, true);
    if (native) {
        for (unsigned steps = 0; cpu->s.ip != 0x170000; ++steps) {
            if (steps == 200000)
                fprintf(stderr, "native PAL %u did not return\n", index);
            CHECK(steps < 200000);
            struct vcpu_exit out = engine_run(cpu, 1);
            if (out.reason == VCPU_EXIT_HALT && index == 29)
                continue;
            if (out.reason != VCPU_EXIT_BUDGET) {
                fprintf(stderr, "native PAL %u exit %u at %llx\n", index, out.reason,
                        (unsigned long long)cpu->s.ip);
                vcpu_dump(cpu);
            }
            CHECK(out.reason == VCPU_EXIT_BUDGET);
        }
    } else
        CHECK(ia64_pal_dispatch(cpu).kind == IA64_STEP_COMPLETE);
    uint64_t expected[4] = {(uint64_t)status, x, y, z};
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t got = ia64_gr_read(&cpu->s, 8 + i);
        if (got != expected[i])
            fprintf(stderr,
                    "model=%u native=%u PAL=%u args=%llx,%llx,%llx r%u=%llx expected=%llx\n", model,
                    native, index, (unsigned long long)a, (unsigned long long)b,
                    (unsigned long long)c, 8 + i, (unsigned long long)got,
                    (unsigned long long)expected[i]);
        CHECK(got == expected[i] && !ia64_nat_read(&cpu->s, 8 + i));
    }
}
/* Literal fields from EDK2 IndustryStandard/Pal.h PAL_TEST_INFO_INFO,
 * PAL_PLATFORM_INFO, and SDM Rev.2.3 Vol.2 Figs.11-14/44. The cache-array
 * operation (p.2:377) does not require backing memory; the fork's cacheless
 * implementation leaves memory unchanged. */
static void regression_vectors(const char *only)
{
#define RULE(name) (!only || !strcmp(only, name))
    if (RULE("layout")) {
        call(258, BUFFER, UINT64_C(0x0100000000001000), 1, 0, 4, 0, 0);
        call(258, BUFFER, UINT64_C(0x0200000000001000), 1, -2, 0, 0, 0);
        call(258, BUFFER, UINT64_C(0x1000000000001000), 1, -2, 0, 0, 0);
        call(258, BUFFER, UINT64_C(0x0080000000000000), 1, -2, 0, 0, 0);
        call(258, UINT64_C(0x3fffffffff000), UINT64_C(0x0100000000001000), 1, 0, 4, 0, 0);
        call(258, UINT64_C(0x3fffffffff001), UINT64_C(0x0100000000001000), 1, -2, 0, 0, 0);
    }
    if (RULE("mode")) {
        uint64_t saved = cpu->s.psr;
        cpu->s.psr = saved | (UINT64_C(1) << 19); /* PSR.dfh is independent of data translation. */
        call(258, BUFFER, 0x1000, 1, 0, 4, 0, 0);
        cpu->s.psr = saved | IA64_PSR_DT;
        call(258, BUFFER, 0x1000, 1, -2, 0, 0, 0);
        cpu->s.psr = saved;
    }
    if (RULE("test-info")) {
        call(37, 0, 0, 0, 0, 0, 1, 0);
        call(37, 1, 0, 0, 0, 0, 1, 0);
        call(37, 2, 0, 0, -2, 0, 0, 0);
    }
    if (RULE("line")) {
        const uint64_t pattern = UINT64_C(0x0123456789abcdef);
        for (unsigned i = 0; i < 18; ++i)
            put(BUFFER - 8 + 8 * i, pattern + i);
        for (unsigned dt = 0; dt < 2; ++dt) {
            uint64_t saved = cpu->s.psr;
            if (dt)
                cpu->s.psr |= IA64_PSR_DT;
            call(31, BUFFER + 37, ~pattern, 0, 0, 0, 0, 0);
            for (unsigned i = 0; i < 18; ++i)
                CHECK(word(BUFFER - 8 + 8 * i) == pattern + i);
            call(31, 0xfee00000, pattern, 0, 0, 0, 0, 0);
            CHECK(!device_writes);
            call(31, 0x300000, pattern, 0, 0, 0, 0, 0); /* No RAM or device. */
            cpu->s.psr = saved;
            for (unsigned i = 0; i < 18; ++i)
                CHECK(word(BUFFER - 8 + 8 * i) == pattern + i);
            CHECK(!device_writes);
        }
        call(31, UINT64_C(1) << 50, 0, 0, -2, 0, 0, 0);
        call(31, BUFFER | (UINT64_C(1) << 63), 0, 0, -2, 0, 0, 0);
        call(31, BUFFER, 0, 1, -2, 0, 0, 0);
    }
    if (RULE("halt")) {
        put(BUFFER - 8, 123);
        put(BUFFER + 64, 456);
        for (unsigned i = 0; i < 8; ++i)
            put(BUFFER + 8 * i, UINT64_MAX);
        call(257, BUFFER, 0, 0, 0, 0, 0, 0);
        for (unsigned i = 0; i < 8; ++i) {
            uint64_t state = word(BUFFER + 8 * i);
            CHECK(((state >> 60) & 1) == (i == 0)); /* implemented */
            CHECK(((state >> 61) & 1) == (i == 0)); /* coherent */
            CHECK(!(state >> 62));
        }
        CHECK(word(BUFFER - 8) == 123 && word(BUFFER + 64) == 456);
        call(257, BUFFER + 1, 0, 0, -2, 0, 0, 0);
        call(28, 0, 0, 0, -1, 0, 0, 0);
        call(28, 1, 0, 0, -1, 0, 0, 0);
    }
    if (RULE("platform")) {
        /* Fixed windows: accept defaults and their bit-63 aliases; refuse
         * relocation. Addresses outside the implemented 50 bits are invalid. */
        const uint64_t defaults[] = {UINT64_C(0xfee00000), UINT64_C(0x3fffffc000000)};
        for (unsigned type = 0; type < 2; ++type) {
            call(16, type, defaults[type], 0, 0, 0, 0, 0);
            call(16, type, defaults[type] | (UINT64_C(1) << 63), 0, 0, 0, 0, 0);
            call(16, type, UINT64_C(1) << 50, 0, -2, 0, 0, 0);
            call(16, type, (UINT64_C(1) << 50) | (UINT64_C(1) << 63), 0, -2, 0, 0, 0);
            call(16, type, type ? 0x4000000 : 0x200000, 0, -1, 0, 0, 0);
        }
        call(16, 2, 0, 0, -2, 0, 0, 0);
        call(16, 0, 0xfee00000, 1, -2, 0, 0, 0);
    }
    if (RULE("copy")) {
        /* Pal.h PAL_PLATFORM_INFO: low 32 bits controllers, high 32 CPUs.
         * Also accept zero (SDM p.2:388 calls the argument reserved). */
        const uint64_t counts[] = {0, UINT64_C(0x100000001), UINT64_C(0x400000002), UINT64_MAX};
        for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i)
            call(30, 0, counts[i], 512, 0, native ? 32768 : 4096, native ? 8192 : 4096, 0);
        call(30, 1, UINT64_C(0x100000001), 512, -2, 0, 0, 0);
    }
    if (RULE("bus")) {
        /* Pal.h PAL_BUS_FEATURES: bit 30 would make locked transactions
         * non-atomic. Neither that control nor bus parking is modeled. */
        call(9, 0, 0, 0, 0, 0, 0, 0);
        call(10, UINT64_MAX, 0, 0, 0, 0, 0, 0);
        call(9, 0, 0, 0, 0, 0, 0, 0);
    }
    if (RULE("resources")) {
        /* SDM Vol.2 PAL_MC_REGISTER_MEM, PAL_MC_EXPECTED, PAL_PMI_ENTRYPOINT
         * and PAL_COPY_PAL set processor resources: native PAL holds them
         * in the per-processor service registers, so
         * a copied PAL sees the same XR0, indicator and entrypoints. */
        call(27, BUFFER | (UINT64_C(1) << 63), 0, 0, 0, 0, 0, 0);
        call(23, 1, 0, 0, 0, 0, 0, 0);
        call(32, BUFFER + 0x100, 0, 0, 0, 0, 0, 0);
        if (native) {
            CHECK(ia64_pal_service_read(cpu, IA64VMM_SVC_PAL_XR0) ==
                  (BUFFER | (UINT64_C(1) << 63)));
            CHECK(ia64_pal_service_read(cpu, IA64VMM_SVC_PAL_EXPECTED) == 1);
            CHECK(ia64_pal_service_read(cpu, IA64VMM_SVC_PAL_SALE_PMI) == BUFFER + 0x100);
            call(256, 0x1a0000, 32768, 0, 0, 0, 0, 0);
            CHECK(ia64_pal_service_read(cpu, IA64VMM_SVC_PAL_PALE_PMI) == 0x1a0000 + pmi_offset);
            uint64_t copied = pal_entry;
            pal_entry = 0x1a0000;
            call(23, 0, 0, 0, 0, 1, 0, 0);
            call(27, (BUFFER + 512) | (UINT64_C(1) << 63), 0, 0, 0, 0, 0, 0);
            pal_entry = copied;
            CHECK(ia64_pal_service_read(cpu, IA64VMM_SVC_PAL_XR0) ==
                  ((BUFFER + 512) | (UINT64_C(1) << 63)));
            CHECK(ia64_pal_service_read(cpu, IA64VMM_SVC_PAL_EXPECTED) == 0);
            /* copy_option 1 copies nothing; the PALE_PMI entrypoint still
             * becomes the target's. */
            uint64_t marker = 0x5a5a5a5a5a5a5a5a, at = 0;
            CHECK(gm_write(0x1c0000 + pmi_offset, &marker, 8));
            call(256, 0x1c0000, 32768, 1, 0, 0, 0, 0);
            CHECK(ia64_pal_service_read(cpu, IA64VMM_SVC_PAL_PALE_PMI) == 0x1c0000 + pmi_offset);
            CHECK(gm_read(0x1c0000 + pmi_offset, &at, 8) && at == marker);
        } else {
            CHECK(cpu->s.pal.mc_save_addr == (BUFFER | (UINT64_C(1) << 63)) &&
                  cpu->s.pal.mc_expected && cpu->s.pal.pmi_entry == BUFFER + 0x100);
            call(23, 0, 0, 0, 0, 1, 0, 0);
        }
    }
    if (RULE("ia32")) {
        ia64_gr_write(&cpu->s, 4, 123, false);
        call(33, 0, 0, 0, -1, 0, 0, 0);
        CHECK(ia64_gr_read(&cpu->s, 4) == UINT64_MAX && !ia64_nat_read(&cpu->s, 4));
    }
#undef RULE
}

static void vectors(void)
{
    struct vector {
        unsigned index;
        uint64_t a, b, c;
        int64_t status;
        uint64_t x, y, z;
    };
    const struct vector vectors[] = {
        {1, 1, 0, 0, 0, 0, 0, 0},
        {1, 0, 0, 0, -2, 0, 0, 0},
        {3, 0, 3, 1, 0, 0, 0, 0},
        {3, 3, 1, 0, -2, 0, 0, 0},
        {4, 0, 0, 0, 0, 3, model ? 4 : 5, 0},
        {5, 0, 0, 0, 0, 0x71, 0, 0},
        {6, 0, 0, 0, 0, 0, 0x100000001ULL, 0},
        {7, 0, 1, 0, 0, 0x202001, 0x1000, 0},
        {7, 0, 2, 0, 0, 0x202001, 0x1000, 0},
        {7, 1, 1, 0, 0, 0x500808001ULL, 0x155557000ULL, 0},
        {7, 1, 2, 0, 0, 0x500808001ULL, 0x155557000ULL, 0},
        {7, 2, 1, 0, -2, 0, 0, 0},
        {8, 0, 0, 0, 0, model ? 0x02043f3f080f1865ULL : 0x02041f1f080f1865ULL, 0x183c, 0},
        {9, 0, 0, 0, 0, 0, 0, 0},
        {10, ~0ULL, 0, 0, 0, 0, 0, 0},
        {9, 0, 0, 0, 0, 0, 0, 0},
        {11, 0, 0, 0, 0, 4, 4, 0},
        {12, 0, 0, 0, 0, 0, 0, 0},
        {13, 0, 0, 0, 0, 100000000, 0, 0},
        {14, 0, 0, 0, 0, 0x1000000001ULL, 0x400000001ULL, 0x400000001ULL},
        {15, BUFFER, 0, 0, 0, 0x08123004, 0, 0},
        {19, 0, 0, 0, 0, 96, 0, 0},
        {20, 0, 0, 0, 0, model ? 0x57301000573ULL : 0x96801000968ULL,
         model ? 0x57301000573ULL : 0x96801000968ULL, 0},
        {21, 0, 0, 0, 0, 0, 0, 0},
        {22, 0, 0, 0, 0, 0, 0, 0},
        {23, 1, 0, 0, 0, 0, 0, 0},
        {23, 0, 0, 0, 0, 1, 0, 0},
        {24, 0, 0, 0, -1, 0, 0, 0},
        {24, 1, BUFFER, 0, -1, 0, 0, 0},
        {25, 0, 0, 0, -6, 0, 0, 0},
        {25, 2, 0x100, 4, -6, 0, 0, 0},
        {25, 2, 0x300, 0, -2, 0, 0, 0},
        {26, 0, 0, 0, -2, 0, 0, 0},
        {27, BUFFER | 0x8000000000000000ULL, 0, 0, 0, 0, 0, 0},
        {28, 1, 0, 0, -1, 0, 0, 0},
        {30, 0, 0, 0, 0, native ? 32768 : 4096, native ? 8192 : 4096, 0},
        {30, 1, 0, 0, -2, 0, 0, 0},
        {32, BUFFER, 0, 0, 0, 0, 0, 0},
        {34, 0, 0, 0, 0, 0x155557000ULL, 0x155557000ULL, 0},
        {37, 0, 0, 0, 0, 0, 1, 0},
        {37, 1, 0, 0, 0, 0, 1, 0},
        {37, 2, 0, 0, -2, 0, 0, 0},
        {39, 0, 0, 0, 0, 0x11117f2f00ffULL, 7, 0},
        {39, 1, 0, 0, 0, 0, 0, 0},
        {39, 2, 0, 0, 0, 0x3fb0107, 0x307ff, 0},
        {39, 3, 0, 0, 0, 0, 2, 0},
        {41, 0, 0, 0, 1, 0, 0, 0},
        {41, 1, 0, 0, 1, 0, 0, 0},
        {41, 2, 0, 0, -2, 0, 0, 0},
        {257, BUFFER, 0, 0, 0, 0, 0, 0},
        {258, BUFFER, 0x1000, 1, 0, 4, 0, 0},
        {258, BUFFER, 0x0100000000001000ULL, 1, 0, 4, 0, 0},
        {258, BUFFER, 0x0200000000001000ULL, 1, -2, 0, 0, 0},
        {258, BUFFER, 0x1000, 0x101, -2, 0, 0, 0},
        {258, BUFFER, 0x1000, 0x10001, -2, 0, 0, 0},
        {261, 0, 0, BUFFER, 0, 0, 0, 0},
        {261, model ? 64 : 32, 0, BUFFER, -2, 0, 0, 0},
        {274, 16, 0, 0, 0, 1600000000, 0, 0},
        {274, 17, 0, 0, 0, model ? 3145728 : 12582912, 0, 0},
        {274, 18, 0, 0, 0, 400000000, 0, 0},
        {274, 19, 0, 0, -2, 0, 0, 0},
        {48, 0, 0, 0, -1, 0, 0, 0},
        {262, 0, 0, 0, -1, 0, 0, 0}};
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        const struct vector *v = &vectors[i];
        call(v->index, v->a, v->b, v->c, v->status, v->x, v->y, v->z);
    }
    const struct {
        unsigned level, type;
        uint64_t x[2], y[2], prot[2];
    } caches[] = {{0,
                   1,
                   {0x01ff06060400, 0x01ff06060400},
                   {0x310c0c00004000, 0x310c0c00004000},
                   {0x400c4c0000000040, 0x400c4c0000000040}},
                  {0,
                   2,
                   {0x010106060400, 0x010106060400},
                   {0x310c0c00004000, 0x310c0c00004000},
                   {0x400c4c0000000040, 0x400c4c0000000040}},
                  {1, 1, {0x07ff07070800, 0}, {0x31110c00100000, 0}, {0x400c510000000040, 0}},
                  {1,
                   2,
                   {0x050107070802, 0x050107070803},
                   {0x310f0c00040000, 0x310f0c00040000},
                   {0x400c4f0000000040, 0x400c4f0000000040}},
                  {2,
                   2,
                   {0x0e0107070c03, 0x0e0107070c03},
                   {0x31140c00c00000, 0x31120c00300000},
                   {0x400c540000000040, 0x400c520000000040}}};
    for (unsigned i = 0; i < sizeof(caches) / sizeof(caches[0]); ++i) {
        int status = model && i == 2 ? -2 : 0;
        call(2, caches[i].level, caches[i].type, 0, status, caches[i].x[model], caches[i].y[model],
             0);
        call(38, caches[i].level, caches[i].type, 0, status, caches[i].prot[model], 0, 0);
        call(2, caches[i].level, caches[i].type, 1, -2, 0, 0, 0);
        call(38, caches[i].level, caches[i].type, 1, -2, 0, 0, 0);
    }
    /* Unspecified PAL calls never succeed because the model changed. */
    for (unsigned n = 0; n < 512; ++n) {
        bool known = (n >= 1 && n <= 32) || n == 34 || (n >= 37 && n <= 39) ||
                     (n >= 41 && n <= 43) || (n >= 256 && n <= 258) || n == 261 || n == 274;
        if (!known)
            call(n, 0, 0, 0, -1, 0, 0, 0);
    }
    /* PAL_ENTER_IA_32_ENV has a special ABI: unavailable status is GR4,
     * rather than the ordinary GR8. EDK2 IndustryStandard/Pal.h (33). */
    ia64_gr_write(&cpu->s, 4, 123, false);
    call(33, 0, 0, 0, -1, 0, 0, 0);
    CHECK(ia64_gr_read(&cpu->s, 4) == UINT64_MAX && !ia64_nat_read(&cpu->s, 4));
    const unsigned reserved[] = {4, 5, 6, 8, 9, 11, 12, 13, 14, 19, 20, 21, 22};
    for (unsigned i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
        for (unsigned arg = 0; arg < 3; ++arg)
            call(reserved[i], arg == 0, arg == 1, arg == 2, -2, 0, 0, 0);
    for (unsigned set = 0; set <= 20; ++set) {
        int status = !set        ? 0
                     : set < 16  ? -2
                     : model     ? (set == 16 ? 0 : -8)
                     : set < 18  ? 1
                     : set == 18 ? 0
                                 : -8;
        /* Feature 41 is read-only; XR1-XR3 (42) are not implemented. */
        uint64_t recovery = native && !set ? UINT64_C(1) << 41 : 0;
        call(17, 0, set, 0, status, recovery, recovery, 0);
        call(18, UINT64_MAX, set, 0, status, 0, 0, 0);
        call(17, 0, set, 0, status, recovery, recovery, 0);
        call(18, 0, set, 0, status, 0, 0, 0);
        call(17, 0, set, 0, status, recovery, recovery, 0);
    }
    call(29, 0, 0, 0, 0, 0, 0, 0);
    if (!native)
        CHECK(cpu->requested_exit == VCPU_EXIT_HALT + 1);
    call(29, 1, 0, 0, -2, 0, 0, 0);
    call(15, BUFFER, 0, 0, 0, 0x08123004, 0, 0);
    const uint64_t masks[16] = {0x3fff, 0, 0, 0, 0x3ffff, 0, 0, 0, 0xf0, 0, 0, 0, 0xf0, 0, 0, 0};
    for (unsigned i = 0; i < 16; ++i)
        CHECK(word(BUFFER + 8 * i) == masks[i]);
    /* Self-test evicts both TCs but preserves translation registers. */
    cpu->s.tc[0][0].valid = cpu->s.tc[1][127].valid = true;
    cpu->s.tr[0][0].valid = cpu->s.tr[1][0].valid = true;
    call(258, BUFFER, 0x0100000000001000ULL, 1, 0, 4, 0, 0);
    CHECK(!cpu->s.tc[0][0].valid && !cpu->s.tc[1][127].valid && cpu->s.tr[0][0].valid &&
          cpu->s.tr[1][0].valid);
    call(274, 0, BUFFER, 0, 0, model ? 44 : 47, 0, 0);
    CHECK(!strcmp((char *)ram + BUFFER - 0x100000,
                  model ? "VMM Madison-compatible IA-64 CPU 1.60GHz 3MB"
                        : "VMM Montecito-compatible IA-64 CPU 1.60GHz 12MB"));
    /* Per-core private caches: cid must name this core, la this LID. */
    for (unsigned count = 1; count <= 5; ++count)
        for (unsigned index = 0; index < count; ++index) {
            cpu->cfg.index = index;
            cpu->cfg.cpu_count = count;
            cpu->cfg.cores_per_socket = model || count == 1 ? 1 : 2;
            cpu->s.cr[CR_LID] = IA64VMM_CPU_LID(index);
            put(0x100000 + count_offset, count);
            unsigned width = model || count == 1 ? 1 : 2, socket = index / width;
            unsigned cores = count - socket * width;
            if (cores > width)
                cores = width;
            uint64_t info =
                cores | (UINT64_C(1) << 16) | ((uint64_t)cores << 32) | ((uint64_t)socket << 48);
            call(42, UINT64_MAX, 0, 0, model ? -1 : 0, model ? 0 : info,
                 model ? 0 : (uint64_t)(index % width) << 32, model ? 0 : index);
            for (unsigned level = 0; level < 3; ++level)
                for (unsigned type = 1; type <= 2; ++type) {
                    int invalid = level == 2 && type == 1;
                    call(43, level, type, 0,
                         model     ? -1
                         : invalid ? -2
                                   : 0,
                         model || invalid ? 0 : 1,
                         model || invalid ? 0 : (uint64_t)(index % width) << 32,
                         model || invalid ? 0 : index);
                }
            call(274, 17, 0, 0, 0, (uint64_t)(model ? 3145728 : 12582912) * cores, 0, 0);
        }
    cpu->cfg.index = 0;
    cpu->cfg.cpu_count = 1;
    cpu->cfg.cores_per_socket = 1;
    cpu->s.cr[CR_LID] = 0;
    put(0x100000 + count_offset, 1);
    call(256, 0x1a0000, native ? 32768 : 4096, 0, 0, 0, 0, 0);
    if (native) {
        pal_entry = 0x1a0000;
        call(19, 0, 0, 0, 0, 96, 0, 0);
        call(17, 0, model ? 16 : 18, 0, 0, 0, 0, 0);
        call(274, 17, 0, 0, 0, model ? 3145728 : 12582912, 0, 0);
        pal_entry = 0x100000;
    }
}
int main(int argc, char **argv)
{
    log_set_level(LOG_ERROR);
    CHECK(argc >= 2 && argc <= 4);
    load(argv[1]);
    struct gm_region region = {.base = 0x100000, .size = sizeof(ram), .kind = GM_RAM, .hva = ram};
    CHECK(gm_add_region(&region));
    struct gm_region service = {
        .base = IA64VMM_SERVICE_BASE, .size = 4096, .kind = GM_EMULATED, .ops = &service_ops};
    CHECK(gm_add_region(&service));
    struct gm_region device = {
        .base = 0xfee00000, .size = 4096, .kind = GM_EMULATED, .ops = &device_ops};
    CHECK(gm_add_region(&device));
    for (model = 0; model < 2; ++model)
        for (native = 0; native < 2; ++native) {
            if (argc == 4 && native != (unsigned)atoi(argv[3]))
                continue;
            CHECK(gm_write(0x100000, image, sizeof(image)));
            struct vcpu_config cfg = {.cpu_model = model, .cpu_count = 1, .itc_icount = true};
            cpu = vcpu_create(&cfg);
            CHECK(cpu);
            struct ia64_entry_regs entry = {
                .ip = 0x100000, .psr = IA64_PSR_BN, .bsp = 0x190000, .bspstore = 0x190000};
            vcpu_reset(cpu, &entry);
            regression_vectors(argc >= 3 ? argv[2] : NULL);
            if (argc == 2)
                vectors();
            vcpu_destroy(cpu);
            printf(
                "PAL profile: model=%u native=%u full scalar/control/buffer/topology vectors passed\n",
                model, native);
        }
    return 0;
}
