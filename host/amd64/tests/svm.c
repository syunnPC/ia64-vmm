/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hosted checks of the SVM container's data structures. Offsets and bit
 * values are written here as literals from AMD64 APM Vol.2 Appendix B and
 * §15.10/§15.11, independently of host/amd64/svm.h. Every check runs once
 * on the product's output and once on a broken copy that it must reject.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/svm.h"

static unsigned failures;
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #e);                           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static uint16_t r16(const uint8_t *p, unsigned o)
{
    uint16_t v;
    memcpy(&v, p + o, 2);
    return v;
}
static uint32_t r32(const uint8_t *p, unsigned o)
{
    uint32_t v;
    memcpy(&v, p + o, 4);
    return v;
}
static uint64_t r64(const uint8_t *p, unsigned o)
{
    uint64_t v;
    memcpy(&v, p + o, 8);
    return v;
}
static void w32(uint8_t *p, unsigned o, uint32_t v)
{
    memcpy(p + o, &v, 4);
}
static void w64(uint8_t *p, unsigned o, uint64_t v)
{
    memcpy(p + o, &v, 8);
}

/* --- VMCB --------------------------------------------------------------- */

static void real_mode_call(struct x86rm_regs *r)
{
    /* The registers platform/vbios.c loads for the POST call. */
    memset(r, 0, sizeof(*r));
    r->cs = (struct x86rm_segment){0xf000, 0x9b, 0xffff, 0xf0000};
    r->ds = r->ss = r->fs = r->gs = (struct x86rm_segment){0, 0x93, 0xffff, 0};
    r->es = (struct x86rm_segment){0xf000, 0x93, 0xffff, 0xf0000};
    r->gdtr = (struct x86rm_segment){0, 0, 0xffff, 0};
    r->idtr = (struct x86rm_segment){0, 0, 0x3ff, 0};
    r->rip = 0xe000;
    r->rsp = 0x7c00;
    r->rflags = 0x202;
    r->cr0 = 0x10;
    r->rax = 0x0010;
    r->rdx = 0xffff;
}
static bool segment_is(const uint8_t *v, unsigned o, uint16_t sel, uint16_t attr, uint32_t limit,
                       uint64_t base)
{
    return r16(v, o) == sel && r16(v, o + 2) == attr && r32(v, o + 4) == limit &&
           r64(v, o + 8) == base;
}

/* NULL when the VMCB is the real-mode container, else what differs. */
static const char *vmcb_problem(const uint8_t *v)
{
    if (r32(v, 0x000) || r32(v, 0x004))
        return "CR/DR intercepts";
    if (r32(v, 0x008) != 0xffffffffu)
        return "exception intercepts";
    /* INTR 0, NMI 1, CPUID 18, INVD 22, HLT 24, INVLPGA 26, IOIO 27,
     * MSR 28, SHUTDOWN 31 (Table B-1, offset 00Ch). */
    if (r32(v, 0x00c) != 0x9d440003u)
        return "intercept word 0Ch";
    /* VMRUN 0, VMMCALL 1, VMLOAD 2, VMSAVE 3, STGI 4, CLGI 5, SKINIT 6,
     * WBINVD 9, XSETBV 13 (offset 010h; exit code 8Dh): VMRUN does not
     * switch XCR0, so a guest's XSETBV would change the host's. */
    if (r32(v, 0x010) != 0x227fu)
        return "intercept word 10h";
    if (r64(v, 0x040) != 0x1000000 || r64(v, 0x048) != 0x2000000)
        return "IOPM/MSRPM base";
    if (r32(v, 0x058) != 1)
        return "ASID";
    if (!(r32(v, 0x060) & (1u << 24)))
        return "V_INTR_MASKING";
    if (r64(v, 0x090) != 1)
        return "NP_ENABLE";
    if (r64(v, 0x0b0) != 0x3000000)
        return "N_CR3";
    if (r64(v, 0x0a8))
        return "EVENTINJ";
    if (r64(v, 0x4d0) != 0x1000)
        return "guest EFER (SVME only)";
    if (r64(v, 0x558) != 0x10)
        return "CR0 (ET only: PE=0, PG=0, caches on)";
    if (r64(v, 0x550) || r64(v, 0x548))
        return "CR3/CR4";
    if (r64(v, 0x570) != 0x202 || r64(v, 0x578) != 0xe000 || r64(v, 0x5d8) != 0x7c00 ||
        r64(v, 0x5f8) != 0x10)
        return "RFLAGS/RIP/RSP/RAX";
    if (!segment_is(v, 0x410, 0xf000, 0x9b, 0xffff, 0xf0000))
        return "CS";
    if (!segment_is(v, 0x400, 0xf000, 0x93, 0xffff, 0xf0000))
        return "ES";
    if (!segment_is(v, 0x420, 0, 0x93, 0xffff, 0) || !segment_is(v, 0x430, 0, 0x93, 0xffff, 0) ||
        !segment_is(v, 0x440, 0, 0x93, 0xffff, 0) || !segment_is(v, 0x450, 0, 0x93, 0xffff, 0))
        return "SS/DS/FS/GS";
    if (!segment_is(v, 0x460, 0, 0, 0xffff, 0) || !segment_is(v, 0x480, 0, 0, 0x3ff, 0))
        return "GDTR/IDTR";
    if (!segment_is(v, 0x470, 0, 0x82, 0xffff, 0) || !segment_is(v, 0x490, 0, 0x83, 0xffff, 0))
        return "LDTR/TR";
    if (v[0x4cb])
        return "CPL";
    if (r64(v, 0x560) != 0x400 || r64(v, 0x568) != 0xffff0ff0)
        return "DR7/DR6";
    if (r64(v, 0x668) != 0x0007040600070406ull)
        return "G_PAT";
    return NULL;
}

static void test_vmcb(void)
{
    static uint8_t vmcb[4096];
    memset(vmcb, 0x5a, sizeof(vmcb));
    const struct svm_vmcb_params p = {0x1000000, 0x2000000, 0x3000000, 1};
    svm_vmcb_init(vmcb, &p);
    struct x86rm_regs r, back;
    real_mode_call(&r);
    svm_vmcb_put_regs(vmcb, &r);
    const char *problem = vmcb_problem(vmcb);
    if (problem)
        fprintf(stderr, "vmcb: %s\n", problem);
    CHECK(!problem);
    svm_vmcb_get_regs(vmcb, &back);
    CHECK(!memcmp(&back.cs, &r.cs, sizeof(r.cs)) && back.rip == r.rip && back.rsp == r.rsp &&
          back.rflags == r.rflags && back.cr0 == r.cr0 &&
          !memcmp(&back.idtr, &r.idtr, sizeof(r.idtr)));
    /* Broken variants: each single change must be caught. */
    static const struct {
        unsigned offset, width;
        uint64_t xor;
        const char *what;
    } broken[] = {
        {0x558, 8, 1, "CR0.PE set"},
        {0x558, 8, 1ull << 31, "CR0.PG set"},
        {0x060, 4, 1u << 24, "V_INTR_MASKING clear"},
        {0x00c, 4, 1u << 24, "HLT intercept clear"},
        {0x00c, 4, 1u << 0, "INTR intercept clear"},
        {0x090, 8, 1, "nested paging off"},
        {0x4d0, 8, 0x1000, "guest EFER.SVME clear"},
        {0x008, 4, 1u << 13, "#GP intercept clear"},
        {0x010, 4, 1u << 1, "VMMCALL intercept clear"},
        {0x410, 2, 0x0001, "CS selector"},
        {0x010, 4, 1u << 13, "XSETBV intercept clear"},
    };
    for (unsigned i = 0; i < sizeof(broken) / sizeof(broken[0]); ++i) {
        static uint8_t copy[4096];
        memcpy(copy, vmcb, sizeof(copy));
        if (broken[i].width == 8)
            w64(copy, broken[i].offset, r64(copy, broken[i].offset) ^ broken[i].xor);
        else if (broken[i].width == 4)
            w32(copy, broken[i].offset, r32(copy, broken[i].offset) ^ (uint32_t)broken[i].xor);
        else {
            uint16_t v = r16(copy, broken[i].offset) ^ (uint16_t)broken[i].xor;
            memcpy(copy + broken[i].offset, &v, 2);
        }
        if (!vmcb_problem(copy)) {
            fprintf(stderr, "vmcb broken variant not caught: %s\n", broken[i].what);
            ++failures;
        }
    }
}

/* --- exit decoding ------------------------------------------------------- */

static void test_exits(void)
{
    static uint8_t v[4096];
    struct x86rm_exit e;
    /* IOIO (APM §15.10.2): IN word from 1CEh, next RIP in EXITINFO2. */
    memset(v, 0, sizeof(v));
    w64(v, 0x070, 0x7b);
    w64(v, 0x078, (0x1ceull << 16) | 0x20 | 1);
    w64(v, 0x080, 0x365c);
    svm_decode_exit(v, false, &e);
    CHECK(e.reason == X86RM_EXIT_IO && e.port == 0x1ce && e.size == 2 && e.in && !e.string &&
          e.next_rip_valid && e.next_rip == 0x365c);
    w64(v, 0x078, (0xcfcull << 16) | 0x40 | 0x8 | 0x4);
    svm_decode_exit(v, false, &e);
    CHECK(e.size == 4 && !e.in && e.string && e.rep && e.port == 0xcfc);
    /* CPUID: without NRIPS no next RIP; with NRIPS the VMCB's nRIP. */
    memset(v, 0, sizeof(v));
    w64(v, 0x070, 0x72);
    w64(v, 0x0c8, 0x1234);
    svm_decode_exit(v, false, &e);
    CHECK(e.reason == X86RM_EXIT_CPUID && !e.next_rip_valid);
    svm_decode_exit(v, true, &e);
    CHECK(e.next_rip_valid && e.next_rip == 0x1234);
    w64(v, 0x070, 0x60); /* INTR has no instruction: never an nRIP */
    svm_decode_exit(v, true, &e);
    CHECK(e.reason == X86RM_EXIT_INTR && !e.next_rip_valid);
    w64(v, 0x070, 0x4d);
    w64(v, 0x078, 0x18);
    svm_decode_exit(v, false, &e);
    CHECK(e.reason == X86RM_EXIT_EXCEPTION && e.vector == 13 && e.info1 == 0x18);
    w64(v, 0x070, 0x400);
    w64(v, 0x078, 0x07);
    w64(v, 0x080, 0xd0000);
    svm_decode_exit(v, false, &e);
    CHECK(e.reason == X86RM_EXIT_NPF && e.gpa == 0xd0000 && e.npf_write && e.npf_present &&
          !e.npf_fetch);
    w64(v, 0x070, UINT64_MAX);
    svm_decode_exit(v, false, &e);
    CHECK(e.reason == X86RM_EXIT_INVALID);
    const struct {
        uint64_t code;
        enum x86rm_exit_reason reason;
    } table[] = {
        {0x7c, X86RM_EXIT_MSR},      {0x78, X86RM_EXIT_HLT},    {0x81, X86RM_EXIT_VMMCALL},
        {0x76, X86RM_EXIT_INVD},     {0x89, X86RM_EXIT_WBINVD}, {0x61, X86RM_EXIT_NMI},
        {0x7f, X86RM_EXIT_SHUTDOWN}, {0x80, X86RM_EXIT_OTHER},  {0x8d, X86RM_EXIT_OTHER},
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        w64(v, 0x070, table[i].code);
        svm_decode_exit(v, false, &e);
        CHECK(e.reason == table[i].reason);
    }
}

/* --- nested page table ------------------------------------------------------ */

struct pool {
    void *pages[64];
    unsigned used, limit;
};
static void *pool_alloc(void *ctx)
{
    struct pool *p = ctx;
    if (p->used == p->limit)
        return NULL;
    void *page = aligned_alloc(4096, 4096);
    memset(page, 0, 4096);
    p->pages[p->used++] = page;
    return page;
}
static uint64_t pool_pa(void *ctx, const void *va)
{
    (void)ctx;
    return (uintptr_t)va;
}
static void *pool_va(void *ctx, uint64_t pa)
{
    (void)ctx;
    return (void *)(uintptr_t)pa;
}
static void pool_free(struct pool *p)
{
    for (unsigned i = 0; i < p->used; ++i)
        free(p->pages[i]);
    p->used = 0;
}

struct run {
    uint64_t gpa, size, hpa, flags;
};
struct runs {
    struct run r[64];
    unsigned n;
    uint64_t leaves[4];
};
static void collect(void *arg, uint64_t gpa, uint64_t size, uint64_t entry)
{
    struct runs *s = arg;
    uint64_t hpa = entry & NPT_ADDR & ~(size - 1),
             flags = entry & (NPT_P | NPT_W | NPT_U | NPT_PWT | NPT_PCD);
    ++s->leaves[size == 4096 ? 1 : size == (1ull << 21) ? 2 : 3];
    struct run *last = s->n ? &s->r[s->n - 1] : NULL;
    if (last && last->gpa + last->size == gpa && last->hpa + last->size == hpa &&
        last->flags == flags) {
        last->size += size;
        return;
    }
    if (s->n < 64)
        s->r[s->n++] = (struct run){gpa, size, hpa, flags};
}

static uint8_t low[0xa0000] __attribute__((aligned(4096)));
static uint8_t shadow[0xa000] __attribute__((aligned(4096)));
static uint8_t stub[0x10000] __attribute__((aligned(4096)));

static const struct x86rm_mapping product_map[] = {
    {0, 0xa0000, low, X86RM_MAP_WRITE},
    {0xa0000, 0x20000, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC},
    {0xc0000, 0xa000, shadow, X86RM_MAP_WRITE},
    {0xf0000, 0x10000, stub, 0},
    {0x80000000, 0x1000000, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC},
    {0x81010000, 0x1000, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC},
    {0xf800000000ull, 0x400000000ull, NULL, X86RM_MAP_WRITE | X86RM_MAP_UC}, /* a 16 GiB BAR1 */
};
/* What the map must turn into, written out: P|U, W where writable,
 * PCD|PWT where UC; private ranges at their buffers, the rest identity. */
static bool npt_matches(const struct runs *got)
{
    const uint64_t rw = NPT_P | NPT_U | NPT_W, uc = rw | NPT_PCD | NPT_PWT;
    const struct run expected[] = {
        {0, 0xa0000, (uintptr_t)low, rw},
        {0xa0000, 0x20000, 0xa0000, uc},
        {0xc0000, 0xa000, (uintptr_t)shadow, rw},
        {0xf0000, 0x10000, (uintptr_t)stub, NPT_P | NPT_U},
        {0x80000000, 0x1000000, 0x80000000, uc},
        {0x81010000, 0x1000, 0x81010000, uc},
        {0xf800000000ull, 0x400000000ull, 0xf800000000ull, uc},
    };
    if (got->n != sizeof(expected) / sizeof(expected[0]))
        return false;
    for (unsigned i = 0; i < got->n; ++i)
        if (memcmp(&got->r[i], &expected[i], sizeof(expected[i])))
            return false;
    return true;
}
static uint64_t *leaf_slot(uint64_t root, uint64_t gpa)
{
    uint64_t *t = pool_va(NULL, root);
    for (unsigned level = 4; level > 1; --level) {
        uint64_t entry = t[(gpa >> (12 + 9 * (level - 1))) & 511];
        if (!(entry & NPT_P) || (entry & NPT_PS))
            return NULL;
        t = pool_va(NULL, entry & NPT_ADDR);
    }
    return &t[(gpa >> 12) & 511];
}

static void test_npt(void)
{
    struct pool pool = {.limit = 64};
    struct svm_npt_ops ops = {pool_alloc, pool_pa, pool_va, &pool, true};
    uint64_t root = 0;
    CHECK(svm_npt_build(product_map, sizeof(product_map) / sizeof(product_map[0]), &ops, &root));
    struct runs got = {0};
    svm_npt_enumerate(root, &ops, collect, &got);
    CHECK(npt_matches(&got));
    /* 4 KiB leaves for the 1 MiB and BAR2, 2 MiB for BAR0, 1 GiB for BAR1. */
    CHECK(got.leaves[1] == 0xa0 + 0x20 + 0xa + 0x10 + 1 && got.leaves[2] == 8 &&
          got.leaves[3] == 16);
    /* Broken variants on the built table: an extra page at D0000 (the
     * absent option ROM area), the VGA window made WB, the stub segment
     * made writable. */
    uint64_t *slot = leaf_slot(root, 0xd0000);
    CHECK(slot && !*slot);
    if (slot) {
        *slot = 0xd0000 | NPT_P | NPT_U | NPT_W;
        struct runs bad = {0};
        svm_npt_enumerate(root, &ops, collect, &bad);
        CHECK(!npt_matches(&bad));
        *slot = 0;
    }
    uint64_t *vga = leaf_slot(root, 0xb8000), *code = leaf_slot(root, 0xff000);
    CHECK(vga && code);
    if (vga && code) {
        uint64_t saved = *vga;
        *vga &= ~(NPT_PCD | NPT_PWT);
        struct runs bad = {0};
        svm_npt_enumerate(root, &ops, collect, &bad);
        CHECK(!npt_matches(&bad));
        *vga = saved;
        saved = *code;
        *code |= NPT_W;
        struct runs bad2 = {0};
        svm_npt_enumerate(root, &ops, collect, &bad2);
        CHECK(!npt_matches(&bad2));
        *code = saved;
    }
    struct runs again = {0};
    svm_npt_enumerate(root, &ops, collect, &again);
    CHECK(npt_matches(&again));
    pool_free(&pool);
    /* Without Page1GB the 16 GiB BAR takes 2 MiB leaves. */
    ops.pages_1g = false;
    CHECK(svm_npt_build(product_map, sizeof(product_map) / sizeof(product_map[0]), &ops, &root));
    struct runs small = {0};
    svm_npt_enumerate(root, &ops, collect, &small);
    CHECK(npt_matches(&small) && small.leaves[3] == 0 && small.leaves[2] == 8 + 8192);
    pool_free(&pool);
    /* Refusals: overlap, misalignment, beyond 48 bits, pool exhaustion. */
    const struct x86rm_mapping overlap[] = {{0, 0x2000, low, X86RM_MAP_WRITE},
                                            {0x1000, 0x1000, NULL, 0}};
    CHECK(!svm_npt_build(overlap, 2, &ops, &root));
    pool_free(&pool);
    const struct x86rm_mapping large_overlap[] = {{0x80000000, 0x200000, NULL, 0},
                                                  {0x80100000, 0x1000, NULL, 0}};
    CHECK(!svm_npt_build(large_overlap, 2, &ops, &root));
    pool_free(&pool);
    const struct x86rm_mapping odd[] = {{0x800, 0x1000, NULL, 0}};
    CHECK(!svm_npt_build(odd, 1, &ops, &root));
    pool_free(&pool);
    const struct x86rm_mapping far[] = {{1ull << 48, 0x1000, NULL, 0}};
    CHECK(!svm_npt_build(far, 1, &ops, &root));
    pool_free(&pool);
    pool.limit = 3;
    CHECK(!svm_npt_build(product_map, 4, &ops, &root));
    pool_free(&pool);
}

/* --- IOPM and MSRPM -------------------------------------------------------- */

static bool passed(unsigned port)
{
    return (port >= 0x3b0 && port <= 0x3df) || port == 0x80 || (port >= 0xe000 && port < 0xe080);
}
static bool iopm_matches(const uint8_t *iopm)
{
    /* APM §15.10.1: one bit per port, 1 = intercept; 12 KiB, and the bits
     * past FFFFh (wrapping multi-byte accesses) intercept as well. */
    for (unsigned bit = 0; bit < 12288 * 8; ++bit) {
        bool intercept = (iopm[bit / 8] >> (bit & 7)) & 1;
        if (intercept != !(bit < 65536 && passed(bit)))
            return false;
    }
    return true;
}
static void test_permission_maps(void)
{
    static uint8_t pass[8192], iopm[12288], msrpm[8192];
    for (unsigned p = 0; p < 65536; ++p)
        if (passed(p))
            pass[p / 8] |= 1u << (p & 7);
    memset(iopm, 0, sizeof(iopm));
    svm_iopm_build(iopm, pass);
    CHECK(iopm_matches(iopm));
    iopm[0x1ce / 8] &= ~(1u << (0x1ce & 7)); /* broken: DISPI index passed */
    CHECK(!iopm_matches(iopm));
    svm_iopm_build(iopm, pass);
    iopm[8192] &= ~1u; /* broken: wrap bit 65536 clear */
    CHECK(!iopm_matches(iopm));
    memset(msrpm, 0, sizeof(msrpm));
    svm_msrpm_build(msrpm);
    bool all = true;
    for (unsigned i = 0; i < sizeof(msrpm); ++i)
        all &= msrpm[i] == 0xff;
    CHECK(all);
    msrpm[0x800 / 4] &= ~1u; /* broken: one MSR read passed */
    all = true;
    for (unsigned i = 0; i < sizeof(msrpm); ++i)
        all &= msrpm[i] == 0xff;
    CHECK(!all);
}

int main(void)
{
    test_vmcb();
    test_exits();
    test_npt();
    test_permission_maps();
    if (failures) {
        fprintf(stderr, "test-svm: %u failure(s)\n", failures);
        return 1;
    }
    printf(
        "test-svm: VMCB real-mode state and intercepts, exit decoding, nested page table, IOPM/MSRPM "
        "(each with its broken variants) passed\n");
    return 0;
}
