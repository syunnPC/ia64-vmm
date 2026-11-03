/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Conformance runner for the rule prover's models,
 * driven by tests/tier1/rules/conformance.py.
 *
 *   tier1-rule-runner ia64
 *     One IA-64 case per stdin line: ia64_decode_bundle() (including MLX
 *     fixup), then ia64_execute() for the selected slot, as in the interpreter.
 *       in:  lo hi slot pr lc maddr mword nin {reg value}*nin nout {reg}*nout
 *       out: kind opcode-name pr lc mword {value}*nout
 *     GRs are static (r0..r31, CFM.sof = 0), NaT bits clear, PSR = 0
 *     (physical data accesses, little-endian), memory is 64 KiB of RAM.
 *
 *   tier1-rule-runner x86 BLOB
 *     BLOB holds snippets assembled by conformance.py at 512-byte strides.
 *     Each snippet loads 16 GPR images (RSP and RDI excepted: RDI points
 *     at the image) and RFLAGS from the image, runs the instruction under
 *     test and stores everything back, plus a jcc outcome word.
 *       in:  index flags {gpr}*16 mem
 *       out: taken flags {gpr}*16 mem
 *     Only the status flags (CF PF AF ZF SF OF) are taken from the input;
 *     the other RFLAGS bits stay those of this process (TF, DF, AC clear).
 *
 *   tier1-rule-runner selftest
 *     Smoke test without z3 or a cross assembler (run by `make test-tier1`,
 *     also under SAN=1): a fixed add/cmp/ld bundle set and one x86 snippet.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "engine/cache/cache.h"
#include "ia64/semantics.h"
#include "ia64vmm/guest_mem.h"

#define RAM_BASE UINT64_C(0x100000)
#define RAM_SIZE 0x10000u
#define SNIPPET_STRIDE 512u
#define X86_STATUS_FLAGS UINT64_C(0x8d5) /* OF SF ZF AF PF CF */
static uint8_t ram[RAM_SIZE] __attribute__((aligned(4096)));

static const char *kind_name(enum ia64_step_kind k)
{
    switch (k) {
    case IA64_STEP_COMPLETE:
        return "complete";
    case IA64_STEP_BRANCH:
        return "branch";
    case IA64_STEP_FAULT:
        return "fault";
    case IA64_STEP_EXIT:
        return "exit";
    }
    return "?";
}

static struct vcpu *new_vcpu(void)
{
    if (!gm_add_region(&(struct gm_region){.base = RAM_BASE,
                                           .size = sizeof(ram),
                                           .kind = GM_RAM,
                                           .name = "tier1-rules",
                                           .hva = ram})) {
        fprintf(stderr, "tier1-rule-runner: cannot register RAM\n");
        exit(2);
    }
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *v = vcpu_create(&cfg);
    if (!v) {
        fprintf(stderr, "tier1-rule-runner: vcpu_create failed\n");
        exit(2);
    }
    vcpu_reset(v, NULL);
    return v;
}

static bool ram_word(uint64_t addr, uint64_t **word)
{
    if (addr < RAM_BASE || addr - RAM_BASE > RAM_SIZE - 8 || (addr & 7))
        return false;
    *word = (uint64_t *)(void *)(ram + (addr - RAM_BASE));
    return true;
}

/* One IA-64 case; returns false on a malformed line. */
static bool ia64_case(struct vcpu *v, char *line, FILE *out)
{
    unsigned long long lo, hi, pr, lc, maddr, mword;
    unsigned slot, nin, nout;
    int used;
    char *p = line;
    if (sscanf(p, "%llx %llx %u %llx %llx %llx %llx %u%n", &lo, &hi, &slot, &pr, &lc, &maddr,
               &mword, &nin, &used) != 8 ||
        slot > 2)
        return false;
    p += used;
    struct ia64_state *s = &v->s;
    memset(s->gr, 0, sizeof(s->gr));
    memset(s->nat, 0, sizeof(s->nat));
    s->pr = pr | 1;
    s->psr = 0;
    s->cfm = 0;
    s->ar[AR_LC] = lc;
    uint64_t *word;
    if (!ram_word(maddr, &word))
        return false;
    *word = mword;
    for (unsigned i = 0; i < nin; ++i) {
        unsigned reg;
        unsigned long long value;
        if (sscanf(p, "%u %llx%n", &reg, &value, &used) != 2 || reg == 0 || reg > 31)
            return false;
        p += used;
        s->gr[reg] = value;
    }
    unsigned outs[8];
    if (sscanf(p, "%u%n", &nout, &used) != 1 || nout > 8)
        return false;
    p += used;
    for (unsigned i = 0; i < nout; ++i) {
        if (sscanf(p, "%u%n", &outs[i], &used) != 1 || outs[i] > 31)
            return false;
        p += used;
    }
    struct ia64_bundle b;
    ia64_decode_bundle(lo, hi, RAM_BASE, &b);
    struct ia64_result r = ia64_execute(v, &b.insn[slot]);
    fprintf(out, "%s %s %llx %llx %llx", kind_name(r.kind),
            ia64_opcode_info(b.insn[slot].opcode)->name, (unsigned long long)s->pr,
            (unsigned long long)s->ar[AR_LC], (unsigned long long)*word);
    for (unsigned i = 0; i < nout; ++i)
        fprintf(out, " %llx", (unsigned long long)s->gr[outs[i]]);
    fputc('\n', out);
    return true;
}

static int run_ia64(void)
{
    struct vcpu *v = new_vcpu();
    static char line[4096];
    unsigned long n = 0;
    while (fgets(line, sizeof(line), stdin)) {
        ++n;
        if (!ia64_case(v, line, stdout)) {
            fprintf(stderr, "tier1-rule-runner: bad ia64 line %lu: %s", n, line);
            return 2;
        }
    }
    vcpu_destroy(v);
    return 0;
}

/* Image layout shared with conformance.py: gpr[16] in ModRM order, then
 * rflags, the jcc outcome, and the memory word load/store snippets address
 * as [rdi + 144]. */
struct x86_image {
    uint64_t gpr[16], rflags, taken, mem;
};
typedef void snippet_fn(struct x86_image *);

static uint8_t *map_code(const uint8_t *code, size_t size)
{
    size_t len = (size + 4095) & ~(size_t)4095;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return NULL;
    memcpy(mem, code, size);
    if (mprotect(mem, len, PROT_READ | PROT_EXEC)) {
        munmap(mem, len);
        return NULL;
    }
    return mem;
}

static void run_snippet(const uint8_t *code, struct x86_image *img, uint64_t status)
{
    img->rflags =
        (__builtin_ia32_readeflags_u64() & ~X86_STATUS_FLAGS) | (status & X86_STATUS_FLAGS);
    img->taken = 0;
    snippet_fn *fn;
    memcpy(&fn, &code, sizeof(fn)); /* object -> function pointer */
    fn(img);
}

static int run_x86(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = size > 0 ? malloc((size_t)size) : NULL;
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "tier1-rule-runner: cannot read %s\n", path);
        return 2;
    }
    fclose(f);
    uint8_t *code = map_code(buf, (size_t)size);
    free(buf);
    if (!code) {
        perror("tier1-rule-runner: mmap");
        return 2;
    }
    unsigned count = (unsigned)((size_t)size / SNIPPET_STRIDE);
    static char line[4096];
    unsigned long n = 0;
    while (fgets(line, sizeof(line), stdin)) {
        ++n;
        unsigned idx;
        unsigned long long flags;
        int used;
        char *p = line;
        struct x86_image img;
        bool ok = sscanf(p, "%u %llx%n", &idx, &flags, &used) == 2 && idx < count;
        p += ok ? used : 0;
        for (unsigned i = 0; ok && i < 16; ++i) {
            unsigned long long g;
            ok = sscanf(p, "%llx%n", &g, &used) == 1;
            p += ok ? used : 0;
            img.gpr[i] = g;
        }
        unsigned long long m = 0;
        ok = ok && sscanf(p, "%llx%n", &m, &used) == 1;
        img.mem = m;
        if (!ok) {
            fprintf(stderr, "tier1-rule-runner: bad x86 line %lu: %s", n, line);
            return 2;
        }
        run_snippet(code + (size_t)idx * SNIPPET_STRIDE, &img, flags);
        printf("%llu %llx", (unsigned long long)img.taken,
               (unsigned long long)(img.rflags & X86_STATUS_FLAGS));
        for (unsigned i = 0; i < 16; ++i)
            printf(" %llx", (unsigned long long)img.gpr[i]);
        printf(" %llx\n", (unsigned long long)img.mem);
    }
    munmap(code, ((size_t)size + 4095) & ~(size_t)4095);
    return 0;
}

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "tier1-rule-runner selftest: %s:%u: %s\n", __FILE__, __LINE__, #c);    \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/*
 * Smoke-test bundles (GNU as for IA-64) and an x86 snippet in harness format.
 *   { .mii  add r10 = r2, r3 ; cmp4.ltu p6, p7 = r2, r3 ; nop.i 0 }
 *   { .mmi  ld4 r10 = [r2], 8 ; nop.m 0 ; nop.i 0 }
 */
static const uint64_t selftest_bundles[][2] = {
    {UINT64_C(0x1060200006085001), UINT64_C(0x0004000000690e0c)},
    {UINT64_C(0x0000141004205009), UINT64_C(0x0004000000000200)},
};

/* push rbx..r15 (callee-saved), push rdi, load rax/rcx/rflags from the
 * image, add rax, rcx, store rax/rflags, pop, ret. */
static const uint8_t selftest_snippet[] = {
    0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x57, 0x48, 0x8b,
    0x07, 0x48, 0x8b, 0x4f, 0x08, 0xff, 0xb7, 0x80, 0x00, 0x00, 0x00, 0x9d, 0x48,
    0x01, 0xc8, 0x9c, 0x8f, 0x87, 0x80, 0x00, 0x00, 0x00, 0x48, 0x89, 0x07, 0x5f,
    0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5d, 0x5b, 0xc3,
};

static int selftest(void)
{
    struct vcpu *v = new_vcpu();
    struct ia64_state *s = &v->s;
    char line[256];
    FILE *mem = tmpfile();
    CHECK(mem != NULL);

    snprintf(line, sizeof(line), "%llx %llx 0 1 0 %llx 0 2 2 %llx 3 5 1 10\n",
             (unsigned long long)selftest_bundles[0][0], (unsigned long long)selftest_bundles[0][1],
             (unsigned long long)RAM_BASE, (unsigned long long)UINT64_C(0xffffffff00000007));
    CHECK(ia64_case(v, line, mem));
    CHECK(s->gr[10] == UINT64_C(0xffffffff0000000c));
    snprintf(line, sizeof(line), "%llx %llx 1 1 0 %llx 0 2 2 %llx 3 5 0\n",
             (unsigned long long)selftest_bundles[0][0], (unsigned long long)selftest_bundles[0][1],
             (unsigned long long)RAM_BASE, (unsigned long long)UINT64_C(0xffffffff00000007));
    CHECK(ia64_case(v, line, mem));
    CHECK(((s->pr >> 6) & 3) == 2); /* 7 <u 5 is false: p6 = 0, p7 = 1 */

    snprintf(line, sizeof(line), "%llx %llx 0 1 0 %llx 123456789abcdef0 1 2 %llx 1 10\n",
             (unsigned long long)selftest_bundles[1][0], (unsigned long long)selftest_bundles[1][1],
             (unsigned long long)RAM_BASE + 64, (unsigned long long)RAM_BASE + 64);
    CHECK(ia64_case(v, line, mem));
    CHECK(s->gr[10] == UINT64_C(0x9abcdef0) && s->gr[2] == RAM_BASE + 72);
    fclose(mem);
    vcpu_destroy(v);

    uint8_t *code = map_code(selftest_snippet, sizeof(selftest_snippet));
    CHECK(code != NULL);
    struct x86_image img = {.gpr = {[0] = UINT64_MAX, [1] = 1}};
    run_snippet(code, &img, 0);
    CHECK(img.gpr[0] == 0 && (img.rflags & 0x45) == 0x45); /* ZF, PF, CF */
    munmap(code, 4096);
    printf("tier1-rule-runner: selftest passed\n");
    return 0;
}

/* Pure FP-int model conformance: decoded instructions and exact FR tags. */
static int run_fpi(void)
{
    struct vcpu *v = new_vcpu();
    unsigned long long x[19];
    while (
        scanf(
            "%llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx",
            &x[0], &x[1], &x[2], &x[3], &x[4], &x[5], &x[6], &x[7], &x[8], &x[9], &x[10], &x[11],
            &x[12], &x[13], &x[14], &x[15], &x[16], &x[17], &x[18]) == 19) {
        memset(&v->s, 0, sizeof(v->s));
        v->s.cfm = 64;
        v->s.pr = x[3] | 1;
        v->s.psr = x[8];
        v->s.gr[41] = x[4];
        v->s.gr[40] = x[5];
        v->s.nat[41] = x[6];
        v->s.nat[40] = x[7];
        v->s.ar[AR_FPSR] = x[9];
        for (unsigned f = 10; f < 14; ++f) {
            v->s.fr[f].significand = x[10 + 2 * (f - 10)];
            v->s.fr[f].exponent = x[11 + 2 * (f - 10)] & 0x1ffff;
            v->s.fr[f].sign = (x[11 + 2 * (f - 10)] >> 17) & 1;
        }
        struct ia64_bundle b;
        ia64_decode_bundle(x[0], x[1], RAM_BASE, &b);
        if (x[2] > 2)
            return 2;
        struct ia64_result r = ia64_execute(v, &b.insn[x[2]]);
        printf("%s %s %llx %x %llx %llx %llx %llx\n", kind_name(r.kind),
               ia64_opcode_info(b.insn[x[2]].opcode)->name, (unsigned long long)v->s.gr[40],
               v->s.nat[40], (unsigned long long)v->s.fr[10].significand,
               (unsigned long long)(v->s.fr[10].exponent | ((uint64_t)v->s.fr[10].sign << 17)),
               (unsigned long long)v->s.psr, (unsigned long long)v->s.ar[AR_FPSR]);
    }
    vcpu_destroy(v);
    return feof(stdin) ? 0 : 2;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "fpi"))
        return run_fpi();
    if (argc == 2 && !strcmp(argv[1], "ia64"))
        return run_ia64();
    if (argc == 3 && !strcmp(argv[1], "x86"))
        return run_x86(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "selftest"))
        return selftest();
    fprintf(stderr, "usage: tier1-rule-runner ia64 | x86 BLOB | selftest\n");
    return 2;
}
