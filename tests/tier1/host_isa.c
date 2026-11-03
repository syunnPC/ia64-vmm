/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Intel SDM Vol.2 CPUID/BMI entries and Itanium SDM Rev.2.3 Vol.3:
 * full-width shift saturation, field clipping, NaT and qualifiers. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include "tests/tier1/um_t1.h"
#include "tests/hal/hal_linux.h"
#include "engine/tier1/x86enc.h"
#include "engine/tier0/internal.h"
#include "ia64vmm/hal.h"

static unsigned fake_mask, fake_max, fake_ext;
static void fake_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b, uint32_t *c,
                       uint32_t *d)
{
    UM_CHECK(!subleaf);
    *a = *b = *c = *d = 0;
    switch (leaf) {
    case 0:
        *a = fake_max;
        break;
    case 1:
        UM_CHECK(fake_max >= 1);
        if (fake_mask & HOST_ISA_MOVBE)
            *c = 1u << 22;
        break;
    case 7:
        UM_CHECK(fake_max >= 7);
        if (fake_mask & HOST_ISA_BMI1)
            *b |= 1u << 3;
        if (fake_mask & HOST_ISA_BMI2)
            *b |= 1u << 8;
        break;
    case 0x80000000:
        *a = fake_ext;
        break;
    case 0x80000001:
        UM_CHECK(fake_ext >= 0x80000001);
        if (fake_mask & HOST_ISA_LZCNT)
            *c = 1u << 5;
        break;
    default:
        UM_CHECK(false);
    }
}
static void probes(void)
{
    for (fake_mask = 0; fake_mask <= HOST_ISA_ALL; ++fake_mask) {
        fake_max = 7;
        fake_ext = 0x80000001;
        UM_CHECK(host_isa_probe(fake_cpuid) == fake_mask);
        fake_max = 1;
        UM_CHECK(host_isa_probe(fake_cpuid) == (fake_mask & (HOST_ISA_MOVBE | HOST_ISA_LZCNT)));
        fake_max = 0;
        fake_ext = 0x80000000;
        UM_CHECK(!host_isa_probe(fake_cpuid));
    }
}

/* Independent Intel SDM Vol.2 op -> CPUID audit; do not derive this from
 * the selector's feature table, which is itself under test. */
static const unsigned audit_isa[T1_X86_OP_COUNT] = {
    [T1_X86_SHLX64] = HOST_ISA_BMI2, [T1_X86_SHRX64] = HOST_ISA_BMI2,
    [T1_X86_SARX64] = HOST_ISA_BMI2, [T1_X86_RORX64] = HOST_ISA_BMI2,
    [T1_X86_ANDN64] = HOST_ISA_BMI1, [T1_X86_BEXTR64] = HOST_ISA_BMI1,
    [T1_X86_BLSI64] = HOST_ISA_BMI1, [T1_X86_BLSMSK64] = HOST_ISA_BMI1,
    [T1_X86_BLSR64] = HOST_ISA_BMI1, [T1_X86_LZCNT64] = HOST_ISA_LZCNT,
};

static void boot_policy(void)
{
    static const struct {
        const char *cmdline;
        unsigned mask;
        bool valid;
    } cases[] = {
        {"", 15, true},
        {"engine=tier1", 15, true},
        {"host_isa_mask=0", 0, true},
        {"host_isa_mask=15", 15, true},
        {"xhost_isa_mask=0", 15, true},
        {" engine=tier0\thost_isa_mask=6\nvcpus=2 ", 6, true},
        {"host_isa_mask=3 host_isa_mask=8", 3, true},
        {"host_isa_mask=", 0, false},
        {"host_isa_mask=16", 0, false},
        {"host_isa_mask=-1", 0, false},
        {"host_isa_mask=0xf", 0, false},
        {"host_isa_mask=1x", 0, false},
        {"host_isa_mask=4294967296", 0, false},
    };
    struct host_isa_cpu_report cpus[] = {{15, true}, {15, true}, {15, true}};
    for (unsigned n = 0; n < sizeof(cases) / sizeof(*cases); ++n) {
        unsigned got = 99;
        int result = host_isa_resolve(cases[n].cmdline, cpus, 3, &got);
        UM_CHECK(result == (cases[n].valid ? 1 : -1));
        UM_CHECK(got == (cases[n].valid ? cases[n].mask : 99));
    }
    for (unsigned a = 0; a <= HOST_ISA_ALL; ++a)
        for (unsigned b = 0; b <= HOST_ISA_ALL; ++b)
            for (unsigned mask = 0; mask <= HOST_ISA_ALL; ++mask) {
                char cmdline[32];
                snprintf(cmdline, sizeof(cmdline), "host_isa_mask=%u", mask);
                cpus[0].features = a;
                cpus[1].features = b;
                unsigned got = 99;
                UM_CHECK(host_isa_resolve(cmdline, cpus, 3, &got) == 1);
                UM_CHECK(got == (a & b & mask));
            }
    /* Each executor can be the only one missing a capability. */
    for (unsigned cpu = 0; cpu < 3; ++cpu)
        for (unsigned bit = 1; bit <= HOST_ISA_MOVBE; bit <<= 1) {
            for (unsigned n = 0; n < 3; ++n)
                cpus[n].features = HOST_ISA_ALL;
            cpus[cpu].features &= ~bit;
            unsigned got = 99;
            UM_CHECK(host_isa_resolve("", cpus, 3, &got) == 1);
            UM_CHECK(got == (HOST_ISA_ALL & ~bit));
        }
    for (unsigned missing = 0; missing < 3; ++missing) {
        cpus[missing].reported = false;
        unsigned got = 99;
        UM_CHECK(host_isa_resolve("", cpus, 3, &got) == 0 && got == 99);
        UM_CHECK(host_isa_resolve("host_isa_mask=0", cpus, 3, &got) == 0 && got == 99);
        cpus[missing].reported = true;
    }
    unsigned got = 99;
    UM_CHECK(host_isa_resolve("", NULL, 0, &got) == 0 && got == 99);
    for (unsigned op = 0; op < T1_X86_OP_COUNT; ++op)
        UM_CHECK(t1_x86_required_isa(op) == audit_isa[op]);
}

static void native_encodings(void)
{
    static const uint64_t values[] = {0,
                                      1,
                                      63,
                                      64,
                                      65,
                                      UINT64_MAX,
                                      UINT64_C(1) << 63,
                                      UINT64_C(0x1122334455667788),
                                      0x4000,
                                      0x4001,
                                      0xff3f,
                                      0xffff};
    void *code = hal_alloc_pages(1, HAL_PAGE_EXEC);
    UM_CHECK(code);
    for (unsigned op = T1_X86_SHLX64; op <= T1_X86_RORX64; ++op) {
        if (audit_isa[op] & ~hal_host_isa())
            continue;
        hal_code_set_writable(code, 1);
        struct t1_x86_buf b = {.data = code, .capacity = 4096};
        struct t1_x86_insn i = {.op = T1_X86_MOV64_RR, .opnd = {{T1_R10, 0}, {T1_RDX, 0}}};
        t1_x86_emit(&b, &i, NULL);
        i = (struct t1_x86_insn){.op = T1_X86_MOV64_RR, .opnd = {{T1_RAX, 0}, {T1_RDI, 0}}};
        t1_x86_emit(&b, &i, NULL);
        i = (struct t1_x86_insn){.op = T1_X86_MOV64_RI64,
                                 .opnd = {{T1_R11, 0}, {0, T1_XF_ALL | 2}}};
        t1_x86_emit(&b, &i, NULL);
        i = (struct t1_x86_insn){.op = T1_X86_PUSH64_R, .opnd = {{T1_R11, 0}}};
        t1_x86_emit(&b, &i, NULL);
        i.op = T1_X86_POPFQ;
        t1_x86_emit(&b, &i, NULL);
        i = (struct t1_x86_insn){.op = op, .opnd = {{T1_RAX, 0}, {T1_RSI, 0}}};
        if (op == T1_X86_RORX64)
            i.opnd[1].imm = 17;
        t1_x86_emit(&b, &i, NULL);
        b.data[b.size++] = 0x9c; /* PUSHFQ: observe exactly the instruction's flags. */
        i = (struct t1_x86_insn){.op = T1_X86_POP64_R, .opnd = {{T1_R8, 0}}};
        t1_x86_emit(&b, &i, NULL);
        i = (struct t1_x86_insn){.op = T1_X86_STORE64,
                                 .opnd = {{0, 0}, {T1_R8, 0}},
                                 .mem = {T1_R10, T1_X86_NOREG, 0, 0}};
        t1_x86_emit(&b, &i, NULL);
        i.op = T1_X86_RET;
        t1_x86_emit(&b, &i, NULL);
        UM_CHECK(!b.failed);
        hal_code_set_executable(code, 1);
        for (unsigned x = 0; x < sizeof(values) / sizeof(*values); ++x)
            for (unsigned y = 0; y < sizeof(values) / sizeof(*values); ++y) {
                uint64_t a = values[x], v = values[y], flags, expected = 0, f = 0;
                unsigned count = v & 63;
                switch (op) {
                case T1_X86_SHLX64:
                    expected = a << count;
                    break;
                case T1_X86_SHRX64:
                    expected = a >> count;
                    break;
                case T1_X86_SARX64:
                    expected = a >> count;
                    if ((a >> 63) && count)
                        expected |= UINT64_MAX << (64 - count);
                    break;
                case T1_X86_ANDN64:
                    expected = ~a & v;
                    break;
                case T1_X86_BEXTR64: {
                    unsigned start = v & 255, len = (v >> 8) & 255;
                    if (start < 64) {
                        if (len > 64 - start)
                            len = 64 - start;
                        expected =
                            (a >> start) & (len == 64 ? UINT64_MAX : (UINT64_C(1) << len) - 1);
                    }
                    break;
                }
                case T1_X86_BLSI64:
                    expected = (0 - v) & v;
                    if (v)
                        f |= T1_XF_CF;
                    break;
                case T1_X86_BLSMSK64:
                    expected = (v - 1) ^ v;
                    if (!v)
                        f |= T1_XF_CF;
                    break;
                case T1_X86_BLSR64:
                    expected = (v - 1) & v;
                    if (!v)
                        f |= T1_XF_CF;
                    break;
                case T1_X86_LZCNT64:
                    expected = 64;
                    for (unsigned bit = 0; bit < 64; ++bit)
                        if ((v >> bit) & 1)
                            expected = 63 - bit;
                    if (!v)
                        f |= T1_XF_CF;
                    break;
                case T1_X86_RORX64:
                    expected = (a >> 17) | (a << 47);
                    break;
                default:
                    UM_CHECK(false);
                }
                if (!expected)
                    f |= T1_XF_ZF;
                if (expected >> 63)
                    f |= T1_XF_SF;
                const struct t1_x86_info *info = t1_x86_info(op);
                if (!info->flags_def && !info->flags_undef)
                    f = T1_XF_ALL;
                uint64_t got = ((uint64_t (*)(uint64_t, uint64_t, uint64_t *))code)(a, v, &flags);
                UM_CHECK(got == expected);
                UM_CHECK(!((flags ^ f) & (T1_XF_ALL & ~info->flags_undef)));
            }
    }
    hal_free_pages(code, 1);
}

/* Disassemble at instruction boundaries rather than searching for byte
 * sequences, which can also occur in immediates or branch displacements. */
static void tier0_gates(unsigned allowed)
{
    static const struct {
        Ia64Opcode op;
        unsigned x86op;
    } cases[] = {
        {IA64_OP_ANDCM, T1_X86_ANDN64}, {IA64_OP_EXTRU, T1_X86_BEXTR64},
        {IA64_OP_SHL, T1_X86_SHLX64},   {IA64_OP_SHRU, T1_X86_SHRX64},
        {IA64_OP_SHR, T1_X86_SARX64},   {IA64_OP_SHRP_IMM, T1_X86_RORX64},
    };
    static const struct {
        const char *name;
        unsigned op;
    } mnemonics[] = {
        {"andn", T1_X86_ANDN64},   {"bextr", T1_X86_BEXTR64},   {"shlx", T1_X86_SHLX64},
        {"shrx", T1_X86_SHRX64},   {"sarx", T1_X86_SARX64},     {"rorx", T1_X86_RORX64},
        {"blsi", T1_X86_BLSI64},   {"blsmsk", T1_X86_BLSMSK64}, {"blsr", T1_X86_BLSR64},
        {"lzcnt", T1_X86_LZCNT64},
    };
    char path[] = "build/host-isa-XXXXXX";
    int fd = mkstemp(path);
    UM_CHECK(fd >= 0);
    FILE *file = fdopen(fd, "wb");
    UM_CHECK(file);
    for (unsigned n = 0; n < sizeof(cases) / sizeof(*cases); ++n) {
        Ia64Instruction i = {
            .opcode = cases[n].op,
            .valid = true,
            .address = UM_CODE,
            .slot = 1,
            .unit = IA64_UNIT_I,
            .operands.integer = {
                .destination = 8, .source1 = 9, .source2 = 10, .immediate = 17 | (23 << 6)}};
        if (i.opcode == IA64_OP_SHRP_IMM)
            i.operands.integer.source1 = 10;
        struct tier0_region r = {.count = 1, .insns = &i, .slots = {1}};
        struct ia64_state state = {0};
        uint8_t bytes[4096];
        struct x86_buffer b = {.data = bytes, .capacity = sizeof(bytes)};
        tier0_emit(&b, &r, &state);
        UM_CHECK(!b.failed && fwrite(bytes, 1, b.size, file) == b.size);
    }
    UM_CHECK(!fclose(file));
    int fds[2];
    UM_CHECK(!pipe(fds));
    pid_t child = fork();
    UM_CHECK(child >= 0);
    if (!child) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(fds[1]);
        const char *objdump = getenv("OBJDUMP");
        if (!objdump || !*objdump)
            objdump = "objdump";
        execlp(objdump, objdump, "-D", "-b", "binary", "-m", "i386:x86-64", "--no-show-raw-insn",
               path, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    file = fdopen(fds[0], "r");
    UM_CHECK(file);
    unsigned ops[T1_X86_OP_COUNT] = {0}, decoded = 0, forbidden = 0;
    char line[512], mnemonic[32];
    size_t address;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, " %zx: %31s", &address, mnemonic) != 2)
            continue;
        ++decoded;
        UM_CHECK(strcmp(mnemonic, "(bad)") && strcmp(mnemonic, ".byte"));
        for (unsigned m = 0; m < sizeof(mnemonics) / sizeof(*mnemonics); ++m)
            if (!strcmp(mnemonic, mnemonics[m].name)) {
                unsigned op = mnemonics[m].op;
                ++ops[op];
                forbidden |= audit_isa[op] & ~allowed;
            }
    }
    UM_CHECK(!fclose(file));
    int status;
    UM_CHECK(waitpid(child, &status, 0) == child);
    UM_CHECK(!unlink(path));
    UM_CHECK(WIFEXITED(status) && !WEXITSTATUS(status) && decoded && !forbidden);
    for (unsigned n = 0; n < sizeof(cases) / sizeof(*cases); ++n)
        UM_CHECK((ops[cases[n].x86op] != 0) == ((allowed & audit_isa[cases[n].x86op]) != 0));
}

static unsigned inspect(uint32_t id, unsigned allowed, bool unqualified)
{
    const uint8_t *code;
    size_t size, lo, hi;
    UM_CHECK(t1_test_region_code(id, &code, &size, &lo, &hi));
    unsigned seen = 0, ops[T1_X86_OP_COUNT] = {0};
    for (size_t at = 0; at < size;) {
        struct t1_x86_decoded d;
        UM_CHECK(t1_x86_decode(code + at, size - at, &d));
        unsigned need = audit_isa[d.op];
        UM_CHECK(!(need & ~allowed));
        seen |= need;
        ++ops[d.op];
        at += d.length;
    }
    if (unqualified && (allowed & HOST_ISA_BMI1)) {
        UM_CHECK(ops[T1_X86_BLSI64] && ops[T1_X86_BLSMSK64] && ops[T1_X86_BLSR64]);
        UM_CHECK(ops[T1_X86_BEXTR64]);
    }
    if (unqualified && (allowed & HOST_ISA_BMI2))
        UM_CHECK(ops[T1_X86_RORX64]);
    return seen;
}

static void differential(void)
{
    umt_init();
    struct ienc_bundle b[] = {
        {IENC_STOP(IENC_MII),
         {ienc_nop_m(), ienc_alu(1, IENC_ANDCM, 18, 16, 17), ienc_shl(1, 19, 16, 17)}},
        {IENC_STOP(IENC_MII),
         {ienc_nop_m(), ienc_shr(1, 20, 16, 17, true), ienc_shr(1, 21, 16, 17, false)}},
        {IENC_STOP(IENC_MII),
         {ienc_nop_m(), ienc_extr(1, 22, 16, 17, 23, false), ienc_alu(1, IENC_SUB, 23, 0, 16)}},
        {IENC_STOP(IENC_MII),
         {ienc_nop_m(), ienc_alu(1, IENC_AND, 23, 23, 16), ienc_adds(1, 24, -1, 16)}},
        {IENC_STOP(IENC_MII),
         {ienc_nop_m(), ienc_alu(1, IENC_XOR, 24, 24, 16), ienc_adds(1, 25, -1, 16)}},
        {IENC_STOP(IENC_MII),
         {ienc_nop_m(), ienc_alu(1, IENC_AND, 25, 25, 16), ienc_depz(1, 26, 16, 47, 17)}},
        {IENC_STOP(IENC_MII),
         {ienc_nop_m(), ienc_extr(1, 27, 16, 17, 47, false), ienc_alu(1, IENC_OR, 26, 26, 27)}},
        {IENC_STOP(IENC_MIB),
         {ienc_nop_m(), ienc_adds(0, 27, 0, 0), ienc_br_cond(0, 0x1000 - 7 * 16)}},
    };
    const uint64_t values[] = {
        0, 1, 2, UINT64_MAX, UINT64_C(1) << 63, UINT64_C(0x1122334455667788)};
    const uint64_t counts[] = {0, 1, 31, 32, 63, 64, 65, 255, UINT64_MAX};
    unsigned old = hal_linux_host_isa_mask(HOST_ISA_ALL), runs = 0;
    for (unsigned qualifier = 1; qualifier <= 2; ++qualifier) {
        if (qualifier == 2)
            for (unsigned row = 0; row < sizeof(b) / sizeof(*b); ++row)
                for (unsigned slot = 0; slot < 3; ++slot)
                    b[row].slot[slot] &= ~UINT64_C(63);
        for (unsigned mask = 0; mask <= HOST_ISA_ALL; ++mask) {
            tier0_flush();
            t1_flush();
            hal_linux_host_isa_mask(mask);
            unsigned allowed = hal_host_isa();
            tier0_gates(allowed);
            um_emit(UM_CODE, b, sizeof(b) / sizeof(*b));
            struct vcpu *v = um_vcpu(UM_CODE), *ref = um_vcpu(UM_CODE);
            struct t1_compile_report report;
            uint32_t id = umt_compile(v, 0, 0, 0, &report, "host ISA");
            UM_CHECK(id);
            unsigned seen = inspect(id, allowed, qualifier == 2);
            UM_CHECK((seen & (HOST_ISA_BMI1 | HOST_ISA_BMI2)) ==
                     (allowed & (HOST_ISA_BMI1 | HOST_ISA_BMI2)));
            for (unsigned x = 0; x < sizeof(values) / sizeof(*values); ++x)
                for (unsigned c = 0; c < sizeof(counts) / sizeof(*counts); ++c)
                    for (unsigned mode = 0; mode < 4; ++mode) {
                        vcpu_reset(v, NULL);
                        v->s.ip = UM_CODE;
                        v->s.pr = mode & 1 ? 1 : 3;
                        um_gr(v, 16, values[x], mode & 2);
                        um_gr(v, 17, counts[c], false);
                        struct um_snapshot snap;
                        um_snapshot_take(&snap, v);
                        struct umt_run run;
                        umt_case(v, ref, &snap, 0, 24, id, NULL, &run, false, "host ISA Tier1");
                        um_snapshot_restore(&snap, v);
                        struct vcpu_exit out = tier0_run(v, 24);
                        struct um_run no_captures = {0};
                        UM_CHECK(
                            !um_compare(&snap, ref, &no_captures, v, &out, 24, "host ISA Tier0"));
                        um_snapshot_free(&snap);
                        ++runs;
                    }
            umt_release(v);
            vcpu_destroy(v);
            vcpu_destroy(ref);
        }
    }
    tier0_flush();
    t1_flush();
    hal_linux_host_isa_mask(old);
    printf("host_isa: probe masks, feature gating, %u interp/Tier0/Tier1 cases passed\n", runs);
}

void host_isa_tests(void)
{
    probes();
    boot_policy();
    native_encodings();
    differential();
}
