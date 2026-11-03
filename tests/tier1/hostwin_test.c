/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * build/tier1-hostwin-test: host-window tests.
 *
 * Own binary: it installs the process-wide SIGSEGV handler, forks children
 * for chaining, panics and the map limit, and links with
 * -Wl,--wrap=ia64_translate so that every translation hostwin makes, with
 * its reference kind, is visible here (probes are
 * IA64_REF_DATA checks, never IA64_REF_PROBE).
 *
 * This fixture supplies its own trap client with
 * hand-written accessors at known RIPs and a trampoline: the "C
 * accessor" reaches guest RAM through windows with
 * the observed-epoch site caches.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "tests/tier1/hostwin_linux.h"
#include "engine/hostwin/internal.h"
#include "engine/cache/cache.h"
#include "engine/cache/validity.h"
#include "ia64/mmu.h"
#include "ia64/tlb.h"
#include "ia64vmm/engine.h"
#include "ia64vmm/guest_mem.h"
#include "core/log_internal.h"

#define PAGE 4096u
#define RAM_BYTES (UINT64_C(80) << 20)
#define ELIG UINT64_C(0x8000000)    /* page-eligibility layout (setup_memory) */
#define CODE_PA UINT64_C(0x3000000) /* pages the code-event tests make code */

static unsigned checks, tests;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            fprintf(stderr, "hostwin_test: %s:%d: check failed: %s\n", __FILE__, __LINE__, #c);    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

/* ---- ia64_translate test double (link-time wrap) ---------------------- */

struct ia64_result __real_ia64_translate(struct vcpu *v, uint64_t address,
                                         enum ia64_reference reference, unsigned rights,
                                         unsigned level, bool speculative,
                                         struct ia64_translation *out);
struct translate_call {
    uint64_t address;
    enum ia64_reference reference;
    unsigned rights;
};
static struct {
    uint64_t calls, probe_refs;
    unsigned n;
    struct translate_call log[16];
} xl;
struct ia64_result __wrap_ia64_translate(struct vcpu *v, uint64_t address,
                                         enum ia64_reference reference, unsigned rights,
                                         unsigned level, bool speculative,
                                         struct ia64_translation *out)
{
    ++xl.calls;
    if (reference == IA64_REF_PROBE || reference == IA64_REF_PROBE_FAULT)
        ++xl.probe_refs;
    if (xl.n < 16)
        xl.log[xl.n++] = (struct translate_call){address, reference, rights};
    return __real_ia64_translate(v, address, reference, rights, level, speculative, out);
}
static void xl_reset(void)
{
    xl.n = 0;
}

/* ---- recording / failure-injecting backend around the Linux one --------- */

static struct {
    const struct hostwin_backend *real;
    uintptr_t map_host;
    gpa_t map_pa;
    bool map_writable;
    uint64_t maps, unmaps, protects;
    unsigned fail_map, fail_unmap_page, fail_unmap_slot, fail_protect; /* fail the next n */
    void *map_ctx, *unmap_ctx, *protect_ctx[4], *release_ctx; /* the contexts last passed */
} wb;
static enum hostwin_map_status wb_map(void *ctx, uintptr_t host_page, gpa_t pa_page, bool writable)
{
    CHECK(!(host_page & (PAGE - 1)) && !(pa_page & (PAGE - 1)));
    wb.map_host = host_page;
    wb.map_pa = pa_page;
    wb.map_writable = writable;
    ++wb.maps;
    wb.map_ctx = ctx;
    if (wb.fail_map) {
        --wb.fail_map;
        return HOSTWIN_MAP_ERROR;
    }
    return wb.real->map(ctx, host_page, pa_page, writable);
}
static bool wb_unmap(void *ctx, uintptr_t host_page, uint64_t bytes)
{
    CHECK(!(host_page & (PAGE - 1)) && bytes && !(bytes & (PAGE - 1)));
    ++wb.unmaps;
    wb.unmap_ctx = ctx;
    if (bytes == PAGE && wb.fail_unmap_page) {
        --wb.fail_unmap_page;
        return false;
    }
    if (bytes > PAGE && wb.fail_unmap_slot) {
        --wb.fail_unmap_slot;
        return false;
    }
    return wb.real->unmap(ctx, host_page, bytes);
}
static bool wb_protect(void *ctx, uintptr_t host_page)
{
    wb.protect_ctx[wb.protects++ % 4] = ctx;
    if (wb.fail_protect) {
        --wb.fail_protect;
        return false;
    }
    return wb.real->protect_ro(ctx, host_page);
}
static bool wb_reserve(void *ctx, unsigned w, unsigned slots, uintptr_t *base, uintptr_t *poison)
{
    return wb.real->reserve(ctx, w, slots, base, poison);
}
static void wb_release(void *ctx, uintptr_t base, unsigned w, unsigned slots)
{
    wb.release_ctx = ctx;
    wb.real->release(ctx, base, w, slots);
}
static void wb_flush(void *ctx, uintptr_t page, bool sync)
{
    wb.real->flush(ctx, page, sync);
}
static const struct hostwin_backend wrapped = {
    .reserve = wb_reserve,
    .release = wb_release,
    .map = wb_map,
    .unmap = wb_unmap,
    .protect_ro = wb_protect,
    .flush = wb_flush,
};

/* ---- machine ------------------------------------------------------------ */

static uint8_t *ram;
static struct vcpu *v;

static uint64_t mmio_read(void *opaque, struct vcpu *vc, uint64_t offset, unsigned size)
{
    (void)opaque;
    (void)vc;
    (void)offset;
    (void)size;
    return 0;
}
static void mmio_write(void *opaque, struct vcpu *vc, uint64_t offset, uint64_t value,
                       unsigned size)
{
    (void)opaque;
    (void)vc;
    (void)offset;
    (void)value;
    (void)size;
}
static const struct gm_mmio_ops mmio_ops = {mmio_read, mmio_write};

static void add(gpa_t base, uint64_t size, enum gm_kind kind, uint8_t *hva)
{
    struct gm_region r = {.base = base, .size = size, .kind = kind, .name = "test", .hva = hva};
    if (kind == GM_EMULATED) {
        r.hva = NULL;
        r.ops = &mmio_ops;
    }
    CHECK(gm_add_region(&r));
}
static uint8_t *aligned_buffer(size_t bytes)
{
    void *p = NULL;
    CHECK(!posix_memalign(&p, PAGE, bytes));
    memset(p, 0, bytes);
    return p;
}
static uint8_t *backing[8];

/* Main RAM [0, 80 MiB) plus the eligibility layout at ELIG:
 *   +0x00000 RAM [0, 0x800) then GM_EMULATED [0x800, 0x1000): RAM/MMIO in one page
 *   +0x10000 RAM [0, 0x800) then a hole
 *   +0x20000 a hole [0, 0x800), RAM [0x800, 0x2000): page +0x21000 is whole RAM
 *   +0x30000 RAM [0, 0x1c00): page +0x31000 is a partial last page
 *   +0x40000 RAM with 2 KiB-misaligned backing
 *   +0x50000 GM_FIRMWARE, +0x60000 GM_DIRECT_MMIO
 *   +0x70000 GM_RAM whose backing is not a hostwin_linux_ram memfd (unbacked)
 *   +0x80000 a hole */
static void setup_memory(void)
{
    ram = hostwin_linux_ram(0, RAM_BYTES);
    CHECK(ram != NULL);
    add(0, RAM_BYTES, GM_RAM, ram);
    uint8_t *b = hostwin_linux_ram(ELIG, PAGE);
    CHECK(b != NULL);
    add(ELIG, 0x800, GM_RAM, b);
    add(ELIG + 0x800, 0x800, GM_EMULATED, NULL);
    CHECK((b = hostwin_linux_ram(ELIG + 0x10000, PAGE)) != NULL);
    add(ELIG + 0x10000, 0x800, GM_RAM, b);
    CHECK((b = hostwin_linux_ram(ELIG + 0x20000, 2 * PAGE)) != NULL);
    add(ELIG + 0x20800, 0x1800, GM_RAM, b + 0x800);
    CHECK((b = hostwin_linux_ram(ELIG + 0x30000, 2 * PAGE)) != NULL);
    add(ELIG + 0x30000, 0x1c00, GM_RAM, b);
    backing[0] = aligned_buffer(3 * PAGE);
    add(ELIG + 0x40000, 2 * PAGE, GM_RAM, backing[0] + 0x800);
    backing[1] = aligned_buffer(PAGE);
    add(ELIG + 0x50000, PAGE, GM_FIRMWARE, backing[1]);
    backing[2] = aligned_buffer(PAGE);
    add(ELIG + 0x60000, PAGE, GM_DIRECT_MMIO, backing[2]);
    backing[3] = aligned_buffer(PAGE);
    add(ELIG + 0x70000, PAGE, GM_RAM, backing[3]);
    /* Overlapping RAM ranges are refused by the backend. */
    CHECK(hostwin_linux_ram(PAGE, PAGE) == NULL && hostwin_linux_ram(1, PAGE) == NULL);
}

/* Like vcpu_reset: the whole MMU state is new, mmu_seq keeps counting and
 * logs IA64_MMU_ALL. */
static void reset_vcpu(void)
{
    uint64_t seq = v->mmu_seq;
    memset(v, 0, sizeof(*v));
    v->mmu_seq = seq;
    v->s.version = IA64_STATE_VERSION;
    v->s.cpu_model = v->cfg.cpu_model = IA64_CPU_MADISON;
    v->s.psr = IA64_PSR_IC;
    for (unsigned i = 0; i < 8; ++i)
        v->s.rr[i] = (UINT64_C(0x100) + i) << 8 | 12 << 2;
    ia64_mmu_changed(v, IA64_MMU_ALL, 0, 0);
}

/* Direct writes of TR/TC/RR/PKR log as the CPU's writers do:
 * every instance here runs the completeness check, which panics on
 * an unlogged change. */
static void logged_entry(const struct ia64_tlb_entry *e)
{
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned i = 0; i < IA64_DTR_COUNT; ++i)
            if (e == &v->s.tr[side][i]) {
                ia64_mmu_changed(v, IA64_MMU_TR, side, i);
                return;
            }
        for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i)
            if (e == &v->s.tc[side][i]) {
                ia64_mmu_changed(v, IA64_MMU_TC, side, i);
                return;
            }
    }
    CHECK(!"entry outside the vCPU's TLB");
}
static void set_rr(unsigned i, uint64_t value)
{
    v->s.rr[i] = value;
    ia64_mmu_changed(v, IA64_MMU_RR, 0, i);
}
static void set_pkr(unsigned i, uint64_t value)
{
    v->s.pkr[i] = value;
    ia64_mmu_changed(v, IA64_MMU_PKR, 0, i);
}

static struct hostwin_vcpu *make(unsigned w, unsigned slots)
{
    struct hostwin_config c = {.w = w, .slots = slots, .check = true};
    struct hostwin_vcpu *hw = hostwin_create(&c, &wrapped, hostwin_linux_ctx());
    CHECK(hw != NULL);
    return hw;
}
static struct hostwin_stats stats(const struct hostwin_vcpu *hw)
{
    struct hostwin_stats s;
    hostwin_get_stats(hw, &s);
    return s;
}

/* PTE (SDM Vol.2 §4.1.1.5): p, ma, a, d, pl, ar, ppn. */
static uint64_t pte(gpa_t pa, unsigned ma, unsigned ar, unsigned pl, bool a, bool d)
{
    return 1 | (uint64_t)ma << 2 | (uint64_t)a << 5 | (uint64_t)d << 6 | (uint64_t)pl << 7 |
           (uint64_t)ar << 9 | (pa & ((UINT64_C(1) << 50) - PAGE));
}
static uint64_t rw(gpa_t pa)
{
    return pte(pa, IA64_MA_WB, 2, 0, true, true);
}
static uint64_t itir(unsigned ps, uint32_t key)
{
    return (uint64_t)ps << 2 | (uint64_t)key << 8;
}
static void tc(uint64_t va, uint64_t p, uint64_t it)
{
    ia64_tc_insert(v, va, 1, p, it);
}
static void tr(unsigned i, uint64_t va, uint64_t p, uint64_t it)
{
    unsigned ps = (it >> 2) & 63;
    v->s.tr[1][i] =
        (struct ia64_tlb_entry){.va = va & ((UINT64_C(1) << 61) - 1) & ~((UINT64_C(1) << ps) - 1),
                                .rid = (uint32_t)(v->s.rr[va >> 61] >> 8) & 0xffffff,
                                .pte = p,
                                .itir = it,
                                .page_shift = ps,
                                .valid = true};
    logged_entry(&v->s.tr[1][i]);
}
static void virtual_mode(void)
{
    v->s.psr |= IA64_PSR_DT;
}
/* The page becomes code the way the interpreter's fetch makes it: through
 * prepare() (code_page_enter would skip it while the vCPU's entry history
 * still names the page). */
static bool make_code(gpa_t pa)
{
    uint64_t low, high, generation;
    return code_source_read(v, pa, &low, &high, &generation).kind == IA64_STEP_COMPLETE &&
           code_page_has_code(pa & ~(uint64_t)(PAGE - 1));
}

static uint64_t host_of(struct hostwin_vcpu *hw, uint64_t gva)
{
    struct hostwin_tag t;
    CHECK(hostwin_bind(hw, v, gva, &t) == HOSTWIN_OK);
    return gva + t.delta;
}
static enum hostwin_fill fill(struct hostwin_vcpu *hw, uint64_t gva, unsigned size, bool write,
                              enum hostwin_exit_reason *why)
{
    struct hostwin_tag t;
    CHECK(hostwin_bind(hw, v, gva, &t) == HOSTWIN_OK);
    *why = (enum hostwin_exit_reason) - 1;
    return hostwin_resolve(hw, v, gva, size, write, why);
}
#define MAPPED(hw, gva, size, write)                                                               \
    do {                                                                                           \
        enum hostwin_exit_reason w_;                                                               \
        CHECK(fill(hw, gva, size, write, &w_) == HOSTWIN_MAPPED);                                  \
    } while (0)
#define EXITS(hw, gva, size, write, reason)                                                        \
    do {                                                                                           \
        enum hostwin_exit_reason w_;                                                               \
        CHECK(fill(hw, gva, size, write, &w_) == HOSTWIN_EXIT && w_ == (reason));                  \
    } while (0)

/* Protection of the host page holding addr per /proc/self/maps ("rw-s",
 * "r--s", "---p"), or "" if unmapped. No stdio and no allocation, so it
 * also works with the process at its map limit. */
static const char *prot_of(uintptr_t addr)
{
    static char buffer[1 << 16], perms[8];
    perms[0] = 0;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    size_t used = 0;
    for (bool found = false; !found;) {
        ssize_t n = read(fd, buffer + used, sizeof(buffer) - used);
        if (n <= 0)
            break;
        size_t end = used + (size_t)n, at = 0;
        for (char *nl; !found && (nl = memchr(buffer + at, '\n', end - at));
             at = (size_t)(nl - buffer) + 1) {
            char *p;
            unsigned long lo = strtoul(buffer + at, &p, 16), hi = strtoul(p + 1, &p, 16);
            if (addr >= lo && addr < hi) {
                memcpy(perms, p + 1, 4);
                perms[4] = 0;
                found = true;
            }
        }
        memmove(buffer, buffer + at, end - at);
        used = end - at;
    }
    close(fd);
    return perms;
}
static uint64_t reservation_vmas(void)
{
    struct hostwin_linux_stats s;
    hostwin_linux_get_stats(&s);
    return s.reservation_vmas;
}

/* ---- trap client, accessors, trampoline ------------------------ */

struct hostwin_trap hwt_cap;
uint64_t hwt_gva; /* the accessor's gva (Tier1: x->regs[P.addr_reg]) */
uint8_t hwt_exited;
uint64_t hwt_regs_ok;
static struct hostwin_vcpu *hwt_hw;
static enum hostwin_exit_reason hwt_why;
static uint64_t hwt_traps, hwt_bad_va;
_Static_assert(__builtin_offsetof(struct hostwin_trap, rip) == 128 &&
                   __builtin_offsetof(struct hostwin_trap, rflags) == 136,
               "trampoline offsets");

uint64_t hwt_ld1(uint64_t host), hwt_ld2(uint64_t host), hwt_ld4(uint64_t host),
    hwt_ld8(uint64_t host);
void hwt_st1(uint64_t host, uint64_t value), hwt_st2(uint64_t host, uint64_t value);
void hwt_st4(uint64_t host, uint64_t value), hwt_st8(uint64_t host, uint64_t value);
uint64_t hwt_ld8_regs(uint64_t host);
void hwt_tramp(void), hwt_exit_landing(void);
extern char hwt_ld1_at[], hwt_ld2_at[], hwt_ld4_at[], hwt_ld8_at[];
extern char hwt_st1_at[], hwt_st2_at[], hwt_st4_at[], hwt_st8_at[], hwt_ld8_regs_at[];
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl hwt_ld1, hwt_ld1_at, hwt_ld2, hwt_ld2_at, hwt_ld4, hwt_ld4_at, hwt_ld8, hwt_ld8_at\n"
    ".globl hwt_st1, hwt_st1_at, hwt_st2, hwt_st2_at, hwt_st4, hwt_st4_at, hwt_st8, hwt_st8_at\n"
    "hwt_ld1:\nhwt_ld1_at: movzx eax, byte ptr [rdi]\n ret\n"
    "hwt_ld2:\nhwt_ld2_at: movzx eax, word ptr [rdi]\n ret\n"
    "hwt_ld4:\nhwt_ld4_at: mov eax, dword ptr [rdi]\n ret\n"
    "hwt_ld8:\nhwt_ld8_at: mov rax, qword ptr [rdi]\n ret\n"
    "hwt_st1:\nhwt_st1_at: mov byte ptr [rdi], sil\n ret\n"
    "hwt_st2:\nhwt_st2_at: mov word ptr [rdi], si\n ret\n"
    "hwt_st4:\nhwt_st4_at: mov dword ptr [rdi], esi\n ret\n"
    "hwt_st8:\nhwt_st8_at: mov qword ptr [rdi], rsi\n ret\n"
    /* Every other GPR and ZF must survive the trap/resolve/retry path. */
    ".globl hwt_ld8_regs, hwt_ld8_regs_at\n"
    "hwt_ld8_regs:\n"
    "  push rbx\n push rbp\n push r12\n push r13\n push r14\n push r15\n"
    "  mov rbx, 0x1111\n mov rbp, 0x2222\n mov r12, 0x3333\n mov r13, 0x4444\n"
    "  mov r14, 0x5555\n mov r15, 0x6666\n mov r8, 0x7777\n mov r9, 0x8888\n"
    "  mov r10, 0x9999\n mov r11, 0xaaaa\n mov rcx, 0xbbbb\n mov rsi, 0xcccc\n"
    "  xor edx, edx\n"
    "hwt_ld8_regs_at:\n"
    "  mov rax, [rdi]\n"
    "  jnz 1f\n"
    "  cmp rbx, 0x1111\n jne 1f\n cmp rbp, 0x2222\n jne 1f\n cmp r12, 0x3333\n jne 1f\n"
    "  cmp r13, 0x4444\n jne 1f\n cmp r14, 0x5555\n jne 1f\n cmp r15, 0x6666\n jne 1f\n"
    "  cmp r8, 0x7777\n jne 1f\n cmp r9, 0x8888\n jne 1f\n cmp r10, 0x9999\n jne 1f\n"
    "  cmp r11, 0xaaaa\n jne 1f\n cmp rcx, 0xbbbb\n jne 1f\n cmp rsi, 0xcccc\n jne 1f\n"
    "  mov edx, 1\n"
    "1:\n"
    "  pop r15\n pop r14\n pop r13\n pop r12\n pop rbp\n pop rbx\n"
    "  mov [rip + hwt_regs_ok], rdx\n"
    "  ret\n"
    /* An exit lands here with the accessor's stack: return 0, flagged. */
    ".globl hwt_exit_landing\n"
    "hwt_exit_landing:\n"
    "  mov byte ptr [rip + hwt_exited], 1\n"
    "  xor eax, eax\n"
    "  ret\n"
    /* Normal context after the signal: resolve, restore, resume. */
    ".globl hwt_tramp\n"
    "hwt_tramp:\n"
    "  and rsp, -16\n"
    "  call hwt_resolve\n"
    "  mov rsp, [rip + hwt_cap + 32]\n"
    "  push qword ptr [rip + hwt_cap + 136]\n"
    "  popfq\n"
    "  mov rax, [rip + hwt_cap + 0]\n  mov rcx, [rip + hwt_cap + 8]\n"
    "  mov rdx, [rip + hwt_cap + 16]\n mov rbx, [rip + hwt_cap + 24]\n"
    "  mov rbp, [rip + hwt_cap + 40]\n mov rsi, [rip + hwt_cap + 48]\n"
    "  mov rdi, [rip + hwt_cap + 56]\n mov r8, [rip + hwt_cap + 64]\n"
    "  mov r9, [rip + hwt_cap + 72]\n  mov r10, [rip + hwt_cap + 80]\n"
    "  mov r11, [rip + hwt_cap + 88]\n mov r12, [rip + hwt_cap + 96]\n"
    "  mov r13, [rip + hwt_cap + 104]\n mov r14, [rip + hwt_cap + 112]\n"
    "  mov r15, [rip + hwt_cap + 120]\n"
    "  jmp qword ptr [rip + hwt_cap + 128]\n"
    ".att_syntax prefix\n");

struct site_kind {
    const char *rip;
    unsigned size;
    bool write;
};
static const struct site_kind *kind_of(uint64_t rip)
{
    static const struct site_kind kinds[] = {
        {hwt_ld1_at, 1, false}, {hwt_ld2_at, 2, false},      {hwt_ld4_at, 4, false},
        {hwt_ld8_at, 8, false}, {hwt_ld8_regs_at, 8, false}, {hwt_st1_at, 1, true},
        {hwt_st2_at, 2, true},  {hwt_st4_at, 4, true},       {hwt_st8_at, 8, true}};
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i)
        if (rip == (uint64_t)(uintptr_t)kinds[i].rip)
            return &kinds[i];
    return NULL;
}

/* The trap client: async-signal-safe, writes only hwt_cap. */
static bool client(struct hostwin_trap *t)
{
    if (!kind_of(t->rip))
        return false;
    hwt_cap = *t;
    t->rip = (uint64_t)(uintptr_t)hwt_tramp;
    return true;
}

/* Normal context: MAPPED retries the access at the same RIP;
 * EXIT leaves through the landing. */
void hwt_resolve(void);
void hwt_resolve(void)
{
    const struct site_kind *k = kind_of(hwt_cap.rip);
    ++hwt_traps;
    /* The host fault is at the exact access address, not its page. */
    if (hwt_cap.fault_va != hwt_cap.gpr[7] || hwt_cap.write != k->write)
        ++hwt_bad_va;
    if (hostwin_resolve(hwt_hw, v, hwt_gva, k->size, k->write, &hwt_why) == HOSTWIN_MAPPED)
        return;
    hwt_cap.rip = (uint64_t)(uintptr_t)hwt_exit_landing;
}

/* The "C accessor": per-site (tag, delta) caches with the observed-epoch
 * protocol exactly as t1_window_miss, and the native access. */
#define SITES 16
struct accessor {
    struct hostwin_vcpu *hw;
    struct hostwin_tag site[SITES];
    uint64_t observed, misses;
};
static void acc_enter(struct accessor *a)
{
    for (unsigned k = 0; k < SITES; ++k)
        a->site[k].tag = HOSTWIN_TAG_INVALID;
    a->observed = hostwin_bind_epoch(a->hw);
    hostwin_enter(a->hw, v);
    hwt_hw = a->hw;
}
static bool acc_miss(struct accessor *a, unsigned k, uint64_t gva, unsigned size)
{
    ++a->misses;
    if (gva & (size - 1))
        return false;
    struct hostwin_tag tmp = {0, 0};
    if (hostwin_bind(a->hw, v, gva, &tmp) == HOSTWIN_SLOW)
        return false;
    uint64_t e = hostwin_bind_epoch(a->hw);
    if (e != a->observed)
        for (unsigned i = 0; i < SITES; ++i)
            a->site[i].tag = HOSTWIN_TAG_INVALID;
    a->observed = e;
    a->site[k] = tmp; /* publish the current site last */
    return true;
}
/* true: completed natively (value for loads); false: exit, why in hwt_why. */
static bool acc(struct accessor *a, unsigned k, uint64_t gva, unsigned size, bool write,
                uint64_t *value)
{
    while ((gva >> hostwin_bits(a->hw)) != a->site[k].tag) {
        if (!acc_miss(a, k, gva, size)) {
            hwt_why = HW_EXIT_GUEST_FAULT;
            return false;
        }
    }
    uint64_t host = gva + a->site[k].delta;
    hwt_gva = gva;
    hwt_exited = 0;
    uint64_t r = 0;
    switch (size * 2 + write) {
    case 2:
        r = hwt_ld1(host);
        break;
    case 4:
        r = hwt_ld2(host);
        break;
    case 8:
        r = hwt_ld4(host);
        break;
    case 16:
        r = hwt_ld8(host);
        break;
    case 3:
        hwt_st1(host, *value);
        break;
    case 5:
        hwt_st2(host, *value);
        break;
    case 9:
        hwt_st4(host, *value);
        break;
    case 17:
        hwt_st8(host, *value);
        break;
    default:
        CHECK(!"size");
    }
    if (hwt_exited)
        return false;
    if (!write)
        *value = r;
    return true;
}
static uint64_t ram_value(gpa_t pa, unsigned size)
{
    uint64_t x = 0;
    memcpy(&x, ram + pa, size);
    return x;
}

/* ---- tests -------------------------------------------------------------- */

static void test_bind_lru_epoch(void)
{
    reset_vcpu();
    virtual_mode();
    struct hostwin_vcpu *hw = make(36, 3);
    CHECK(hostwin_bits(hw) == 36);
    uint64_t base = host_of(hw, 0); /* slot 0 base: gva 0, tag 0 */
    CHECK(!(base & ((UINT64_C(1) << 36) - 1)) && hostwin_owns(base) &&
          hostwin_owns(hostwin_poison_va(hw)));
    CHECK(!hostwin_owns(hostwin_poison_va(hw) + PAGE) && !hostwin_owns(base - 1));
    struct hostwin_tag t, t2;
    uint64_t g = (UINT64_C(5) << 36) + 0x1238;
    tc(g, rw(0x1000), itir(12, 0));
    CHECK(hostwin_bind(hw, v, g, &t) == HOSTWIN_OK && t.tag == 5);
    uintptr_t s1 = (uintptr_t)(g + t.delta) & ~((UINT64_C(1) << 36) - 1);
    CHECK(s1 == base + (UINT64_C(1) << 36) && ((g + t.delta) & 0xfffffffff) == (g & 0xfffffffff));
    /* Re-binding a bound tag: same pair, no new binding, no epoch change. */
    CHECK(hostwin_bind(hw, v, g + 8, &t2) == HOSTWIN_OK && t2.tag == t.tag && t2.delta == t.delta);
    CHECK(stats(hw).binds == 2 && stats(hw).rebinds == 0 && hostwin_bind_epoch(hw) == 0);
    CHECK(hostwin_bind(hw, v, UINT64_C(7) << 36, &t) == HOSTWIN_OK); /* third slot */
    CHECK(hostwin_bind_epoch(hw) == 0);
    /* Fill something in tag 5's slot; then make tag 0 recent and bind a
     * fourth tag: LRU evicts tag 5, unmaps its range, epoch + 1. */
    enum hostwin_exit_reason why;
    CHECK(hostwin_resolve(hw, v, g & ~7ull, 8, false, &why) == HOSTWIN_MAPPED);
    CHECK(stats(hw).live_mappings == 1 && !strcmp(prot_of(s1 + 0x1000), "rw-s"));
    CHECK(hostwin_bind(hw, v, 0x40, &t) == HOSTWIN_OK &&
          hostwin_bind(hw, v, UINT64_C(7) << 36, &t) == HOSTWIN_OK);
    CHECK(hostwin_bind(hw, v, UINT64_C(9) << 36, &t) == HOSTWIN_OK && t.tag == 9);
    CHECK((uintptr_t)((UINT64_C(9) << 36) + t.delta) == s1);
    CHECK(hostwin_bind_epoch(hw) == 1 && stats(hw).rebinds == 1 && stats(hw).live_mappings == 0);
    CHECK(!strcmp(prot_of(s1 + 0x1000), "---p"));
    /* The old tag is gone: a resolve for it is defensive STALE_SLOT and has
     * no translation side effect. */
    uint64_t calls = xl.calls;
    CHECK(hostwin_resolve(hw, v, g & ~7ull, 8, false, &why) == HOSTWIN_EXIT &&
          why == HW_EXIT_STALE_SLOT);
    CHECK(xl.calls == calls);
    /* Nothing but rebinding moves the epoch: enter, resolve, drops. */
    set_rr(0, v->s.rr[0] ^ 0x100);
    hostwin_enter(hw, v);
    hostwin_invalidate(hw);
    tc(0x2000, rw(0x2000), itir(12, 0));
    MAPPED(hw, 0x2000, 8, true);
    CHECK(hostwin_bind_epoch(hw) == 1);
    /* Virtually, region 4..7 addresses bind (VRN is part of the tag). */
    CHECK(hostwin_bind(hw, v, UINT64_C(0xe000000000001000), &t) == HOSTWIN_OK &&
          t.tag == UINT64_C(0xe000000000001000) >> 36);
    /* Never bound physically: bit 63 (UC alias) and pa >= 2^50. */
    v->s.psr &= ~IA64_PSR_DT;
    CHECK(hostwin_bind(hw, v, UINT64_C(1) << 63, &t) == HOSTWIN_SLOW);
    CHECK(hostwin_bind(hw, v, UINT64_C(1) << 50, &t) == HOSTWIN_SLOW);
    CHECK(hostwin_bind(hw, v, (UINT64_C(1) << 50) - 8, &t) == HOSTWIN_OK);
    hostwin_destroy(hw);
    CHECK(!hostwin_owns(base));
    ++tests;
}

/* The window-miss protocol with more tags than slots (window-tags shape, B side): every access
 * completes, and whenever the observed epoch equals bind_epoch, every
 * cached site tag is still bound with the same delta. */
static void test_observed_epoch(void)
{
    reset_vcpu();
    virtual_mode();
    struct hostwin_vcpu *hw = make(40, 8);
    struct accessor a = {.hw = hw};
    enum { TAGS = 12 };
    for (unsigned k = 0; k < TAGS; ++k) {
        uint64_t va = ((uint64_t)(k % 3) << 61) | ((uint64_t)(k + 1) << 40);
        tc(va, rw(0x100000 + k * PAGE), itir(12, 0));
        uint64_t x = k * 0x1111;
        memcpy(ram + 0x100000 + k * PAGE + 8 * k, &x, 8);
    }
    acc_enter(&a);
    for (unsigned round = 0; round < 3; ++round) {
        for (unsigned k = 0; k < TAGS; ++k) {
            uint64_t va = ((uint64_t)(k % 3) << 61) | ((uint64_t)(k + 1) << 40);
            uint64_t x = 0;
            CHECK(acc(&a, k, va + 8 * k, 8, false, &x) && x == k * 0x1111);
            CHECK(a.observed == hostwin_bind_epoch(hw));
        }
        /* Probe the invariant: binding a cached tag again must neither
         * rebind (epoch) nor return a different pair. */
        for (unsigned k = 0; k < TAGS; ++k) {
            if (a.site[k].tag == HOSTWIN_TAG_INVALID)
                continue;
            uint64_t e = hostwin_bind_epoch(hw);
            struct hostwin_tag t;
            CHECK(hostwin_bind(hw, v, a.site[k].tag << 40, &t) == HOSTWIN_OK);
            CHECK(hostwin_bind_epoch(hw) == e && t.tag == a.site[k].tag &&
                  t.delta == a.site[k].delta);
        }
    }
    CHECK(stats(hw).rebinds >= 4 && hwt_bad_va == 0);
    hostwin_destroy(hw);
    ++tests;
}

/* A C accessor reaches guest RAM through windows with
 * zero ia64_translate calls after warm-up; also the register/flags
 * preservation of the trap path and the test drop (host-fill). */
static void test_accessor_warm(void)
{
    for (int mode = 0; mode < 2; ++mode) {
        reset_vcpu();
        if (mode)
            virtual_mode();
        struct hostwin_vcpu *hw = make(40, 8);
        struct accessor a = {.hw = hw};
        enum { N = 16 };
        uint64_t gva[N];
        for (unsigned i = 0; i < N; ++i) {
            gpa_t pa = 0x200000 + i * 3 * PAGE;
            if (mode) {
                /* Two regions (VRN 1 and 6), TC and TR sources. */
                gva[i] = (i & 1 ? UINT64_C(6) << 61 : UINT64_C(1) << 61) |
                         (UINT64_C(0x5000000) + i * 0x10000);
                if (i < 4)
                    tr(i, gva[i], rw(pa), itir(12, 0));
                else
                    tc(gva[i], rw(pa), itir(12, 0));
            } else {
                gva[i] = pa;
            }
            uint64_t x = 0xa5a5000000000000ull | i;
            memcpy(ram + pa + 0x18, &x, 8);
        }
        uint64_t before = 0;
        for (unsigned pass = 0; pass < 3; ++pass) {
            if (pass == 1)
                before = xl.calls;
            acc_enter(&a);
            uint64_t traps = hwt_traps, fills = stats(hw).fills;
            for (unsigned i = 0; i < N; ++i) {
                uint64_t x = 0;
                CHECK(acc(&a, i % SITES, gva[i] + 0x18, 8, false, &x) &&
                      x == (0xa5a5000000000000ull | i));
                x = i * 7;
                CHECK(acc(&a, i % SITES, gva[i] + 0x20, 4, true, &x));
                CHECK(ram_value((gpa_t)(0x200000 + i * 3 * PAGE + 0x20), 4) == i * 7);
            }
            if (pass)
                CHECK(hwt_traps == traps && stats(hw).fills == fills);
        }
        CHECK(xl.calls == before && hwt_bad_va == 0);
        /* One translation per miss, no permission probe. */
        CHECK(stats(hw).translate_calls == (mode ? N : 0) && stats(hw).probe_calls == 0);
        /* host-fill: a dropped shadow refaults, resolves, retries. */
        hostwin_test_drop(hw, gva[3] + 0x18);
        CHECK(stats(hw).live_mappings == N - 1);
        acc_enter(&a);
        uint64_t traps = hwt_traps, x = 0;
        CHECK(acc(&a, 0, gva[3] + 0x18, 8, false, &x) && x == (0xa5a5000000000000ull | 3));
        CHECK(hwt_traps == traps + 1 && stats(hw).live_mappings == N);
        /* Every GPR and ZF survive trap -> resolve -> retry. */
        hostwin_test_drop(hw, gva[5]);
        hwt_gva = gva[5] + 0x18;
        CHECK(hwt_ld8_regs(host_of(hw, gva[5] + 0x18)) == (0xa5a5000000000000ull | 5) &&
              hwt_regs_ok == 1);
        hostwin_destroy(hw);
    }
    ++tests;
}

/* Cold fills at page offsets 1, 2, 4, 8, 4088 and 4096 - size per width
 * (aligned ones): the fault is at the exact address, translation and the
 * reported gva are the original address, backend operations get pages. */
static void test_offsets(void)
{
    static const unsigned offsets[] = {0, 1, 2, 4, 8, 4088};
    for (int mode = 0; mode < 2; ++mode) {
        reset_vcpu();
        struct hostwin_vcpu *hw = make(40, 8);
        struct accessor a = {.hw = hw};
        uint64_t va = UINT64_C(0x2000000000300000); /* VRN 1 */
        gpa_t pa = 0x400000;
        if (mode) {
            virtual_mode();
            tc(va, rw(pa), itir(12, 0));
        }
        uint64_t g0 = mode ? va : pa;
        for (unsigned size = 1; size <= 8; size *= 2)
            for (unsigned j = 0; j < 7; ++j) {
                unsigned off = j < 6 ? offsets[j] : PAGE - size;
                if (off & (size - 1))
                    continue;
                hostwin_invalidate(hw);
                acc_enter(&a);
                for (unsigned b = 0; b < size; ++b)
                    ram[pa + off + b] = (uint8_t)(0x40 + off + b);
                xl_reset();
                uint64_t x = 0, traps = hwt_traps;
                CHECK(acc(&a, 0, g0 + off, size, false, &x) && x == ram_value(pa + off, size));
                CHECK(hwt_traps == traps + 1 && hwt_bad_va == 0);
                CHECK(wb.map_pa == pa && wb.map_host == (uintptr_t)((g0 + off + a.site[0].delta) &
                                                                    ~(uint64_t)(PAGE - 1)));
                if (mode)
                    CHECK(xl.n == 1 && xl.log[0].address == g0 + off);
                else
                    CHECK(xl.n == 0);
                /* A store at the same offset after a read fill of a page with
                 * PTE.d = 1: already writable, no further trap. */
                x = 0x5a;
                CHECK(acc(&a, 0, g0 + off, size, true, &x) && hwt_traps == traps + 1 &&
                      ram[pa + off] == 0x5a);
            }
        /* Unaligned accesses never reach a fill (the lower tier
         * raises Unaligned Data Reference); resolve also refuses them. */
        uint64_t x = 0;
        CHECK(!acc(&a, 1, g0 + 3, 4, false, &x));
        enum hostwin_exit_reason why;
        uint64_t calls = xl.calls;
        CHECK(hostwin_resolve(hw, v, g0 + 3, 4, false, &why) == HOSTWIN_EXIT &&
              why == HW_EXIT_GUEST_FAULT);
        CHECK(hostwin_resolve(hw, v, g0, 3, false, &why) == HOSTWIN_EXIT &&
              why == HW_EXIT_GUEST_FAULT);
        CHECK(xl.calls == calls);
        hostwin_destroy(hw);
    }
    ++tests;
}

/* Physical fills are WB without consulting an attribute and make no
 * translation. */
static void test_physical(void)
{
    reset_vcpu();
    struct hostwin_vcpu *hw = make(40, 8);
    /* A D-side TC entry with UC attribute for the same number must not
     * matter in physical mode. */
    tc(0x500000, pte(0x500000, IA64_MA_UC, 2, 0, true, true), itir(12, 0));
    uint64_t calls = xl.calls;
    MAPPED(hw, 0x500008, 8, false);
    CHECK(xl.calls == calls && stats(hw).translate_calls == 0 && wb.map_writable);
    /* A tag bound virtually, resolved after PSR.dt = 0 (a caller that
     * skipped bind): bit 63 is UC, >= 2^50 unimplemented; never a shadow,
     * never a translation. */
    struct hostwin_tag t;
    virtual_mode();
    CHECK(hostwin_bind(hw, v, (UINT64_C(1) << 63) | 0x1000, &t) == HOSTWIN_OK);
    CHECK(hostwin_bind(hw, v, UINT64_C(1) << 50, &t) == HOSTWIN_OK);
    v->s.psr &= ~IA64_PSR_DT;
    enum hostwin_exit_reason why;
    CHECK(hostwin_resolve(hw, v, (UINT64_C(1) << 63) | 0x1000, 8, false, &why) == HOSTWIN_EXIT &&
          why == HW_EXIT_ATTRIBUTE);
    CHECK(hostwin_resolve(hw, v, UINT64_C(1) << 50, 8, false, &why) == HOSTWIN_EXIT &&
          why == HW_EXIT_GUEST_FAULT);
    CHECK(hostwin_bind(hw, v, (UINT64_C(1) << 63) | 0x1000, &t) == HOSTWIN_SLOW);
    CHECK(xl.calls == calls && stats(hw).live_mappings == 1);
    hostwin_destroy(hw);
    ++tests;
}

/* Order of checks for each fault kind: step 1 (any guest fault) before step 2
 * (attribute) before step 3 (permission split) before step 4 (page
 * eligibility) before step 5 (code store). No exit leaves a shadow. */
static void test_fill_order(void)
{
    reset_vcpu();
    virtual_mode();
    struct hostwin_vcpu *hw = make(40, 8);
    uint64_t va = UINT64_C(0x4000000000000000); /* VRN 2 */
    unsigned n = 0;
    /* One page per case: pte, access, expected. */
#define CASE(p, it, write, reason)                                                                 \
    do {                                                                                           \
        uint64_t a_ = va + (uint64_t)(++n) * 0x10000;                                              \
        tc(a_, p, it);                                                                             \
        xl_reset();                                                                                \
        EXITS(hw, a_ + 8, 8, write, reason);                                                       \
        CHECK(stats(hw).live_mappings == 0);                                                       \
    } while (0)
    /* Step 1 faults: page not present, NaTPage, access rights (ar 0 = read
     * only, pl 0 at cpl 0 is fine; write), access bit, dirty bit. */
    CASE(rw(0x600000) & ~UINT64_C(1), itir(12, 0), false, HW_EXIT_GUEST_FAULT);
    CHECK(xl.n == 1); /* no probe after a failed check */
    CASE(pte(0x600000, IA64_MA_NAT, 2, 0, true, true), itir(12, 0), false, HW_EXIT_GUEST_FAULT);
    CASE(pte(0x600000, IA64_MA_WB, 0, 0, true, true), itir(12, 0), true, HW_EXIT_GUEST_FAULT);
    CASE(pte(0x600000, IA64_MA_WB, 2, 0, false, true), itir(12, 0), false, HW_EXIT_GUEST_FAULT);
    CASE(pte(0x600000, IA64_MA_WB, 2, 0, true, false), itir(12, 0), true, HW_EXIT_GUEST_FAULT);
    /* Privilege: pl 0 page at cpl 3. */
    v->s.psr |= UINT64_C(3) << 32;
    CASE(rw(0x600000), itir(12, 0), false, HW_EXIT_GUEST_FAULT);
    v->s.psr &= ~(UINT64_C(3) << 32);
    /* Key miss / key permission (PSR.pk = 1). */
    v->s.psr |= IA64_PSR_PK;
    set_pkr(0, 1 | (uint64_t)0x77 << 8);
    CASE(rw(0x600000), itir(12, 0x55), false, HW_EXIT_GUEST_FAULT); /* key miss */
    set_pkr(1, 1 | 4 | 2 | (uint64_t)0x66 << 8);                    /* rd, wd */
    CASE(rw(0x600000), itir(12, 0x66), false, HW_EXIT_GUEST_FAULT);
    /* Step 1 precedes step 2: a key fault on a UC page is a guest fault. */
    CASE(pte(0x600000, IA64_MA_UC, 2, 0, true, true), itir(12, 0x66), false, HW_EXIT_GUEST_FAULT);
    /* Step 3 precedes step 4: rd = 1, wd = 0 on an MMIO page; a store is a
     * permission split, not NOT_RAM. */
    set_pkr(2, 1 | 4 | (uint64_t)0x44 << 8);
    CASE(rw(ELIG + 0x800), itir(12, 0x44), true, HW_EXIT_PERMISSION_SPLIT);
    v->s.psr &= ~IA64_PSR_PK;
    /* Step 2: UC, UCE, WC on RAM. */
    CASE(pte(0x600000, IA64_MA_UC, 2, 0, true, true), itir(12, 0), false, HW_EXIT_ATTRIBUTE);
    CASE(pte(0x600000, IA64_MA_UCE, 2, 0, true, true), itir(12, 0), false, HW_EXIT_ATTRIBUTE);
    CASE(pte(0x600000, IA64_MA_WC, 2, 0, true, true), itir(12, 0), true, HW_EXIT_ATTRIBUTE);
    /* Step 2 precedes step 4: UC page that is MMIO. */
    CASE(pte(ELIG + 0x60000, IA64_MA_UC, 2, 0, true, true), itir(12, 0), false, HW_EXIT_ATTRIBUTE);
    /* Step 4 for a WB mapping of MMIO and of a partial page. */
    CASE(rw(ELIG + 0x60000), itir(12, 0), false, HW_EXIT_NOT_RAM);
    CASE(rw(ELIG + 0x31000), itir(12, 0), false, HW_EXIT_PARTIAL_PAGE);
    /* Unbacked RAM (no memfd behind the region): step 6. */
    CASE(rw(ELIG + 0x70000), itir(12, 0), false, HW_EXIT_UNBACKED);
    /* TLB miss without a VHPT: Alternate Data TLB; with PSR.ic = 0 the
     * nested form; VHPT enabled but its page unmapped: VHPT fault. */
    xl_reset();
    EXITS(hw, va + 0x900000, 8, false, HW_EXIT_GUEST_FAULT);
    v->s.psr &= ~IA64_PSR_IC;
    EXITS(hw, va + 0x900000, 8, false, HW_EXIT_GUEST_FAULT);
    v->s.psr |= IA64_PSR_IC;
    v->s.cr[CR_PTA] = UINT64_C(0x4000000010000000) | 15 << 2 | 1;
    set_rr(2, v->s.rr[2] | 1);
    EXITS(hw, va + 0x900000, 8, false, HW_EXIT_GUEST_FAULT);
    CHECK(stats(hw).live_mappings == 0 && xl.probe_refs == 0);
    /* Step 5: a store to a code page exits, a load maps it read-only;
     * step 4 precedes it: a code page that is partial stays PARTIAL. */
    CHECK(make_code(CODE_PA));
    CHECK(make_code(ELIG + 0x31000));
    CHECK(code_page_has_code(CODE_PA) && code_page_has_code(ELIG + 0x31000));
    CASE(rw(CODE_PA), itir(12, 0), true, HW_EXIT_CODE_PAGE_STORE);
    uint64_t ro = stats(hw).fills_ro;
    MAPPED(hw, va + (uint64_t)n * 0x10000 + 8, 8, false);
    CHECK(stats(hw).fills_ro == ro + 1 && !wb.map_writable);
    hostwin_invalidate(hw);
    CASE(rw(ELIG + 0x31000), itir(12, 0), true, HW_EXIT_PARTIAL_PAGE);
    code_cache_flush();
    hostwin_destroy(hw);
    ++tests;
#undef CASE
}

/* The four READ/WRITE combinations, including
 * PKR rd = 1, wd = 0 in both orders, and PTE.d = 0 / PTE.a = 0. The contract requires one IA64_REF_DATA translation with the access's own right per
 * miss; the other right comes from ia64_lasting_rights, never a probe. */
static void test_permissions(void)
{
    reset_vcpu();
    virtual_mode();
    v->s.psr |= IA64_PSR_PK;
    set_pkr(0, 1 | (uint64_t)0x10 << 8);         /* RW */
    set_pkr(1, 1 | 2 | (uint64_t)0x11 << 8);     /* wd: read only */
    set_pkr(2, 1 | 4 | (uint64_t)0x12 << 8);     /* rd: write only */
    set_pkr(3, 1 | 4 | 2 | (uint64_t)0x13 << 8); /* neither */
    struct hostwin_vcpu *hw = make(40, 8);
    uint64_t va = UINT64_C(0x6000000000000000);
    for (unsigned k = 0; k < 4; ++k)
        for (unsigned order = 0; order < 2; ++order) {
            uint64_t a = va + (k * 2 + order + 1) * 0x10000;
            tc(a, rw(0x700000 + (k * 2 + order) * PAGE), itir(12, 0x10 + k));
            for (unsigned step = 0; step < 2; ++step) {
                bool write = (step == 0) == (order == 1); /* order 0: load then store */
                uint64_t ro = stats(hw).fills_ro;
                xl_reset();
                enum hostwin_exit_reason why;
                enum hostwin_fill f = fill(hw, a + 16, 8, write, &why);
                CHECK(xl.probe_refs == 0);
                switch (k) {
                case 0: /* readable and writable: one RW shadow */
                    CHECK(f == HOSTWIN_MAPPED && stats(hw).fills_ro == ro && wb.map_writable);
                    CHECK(xl.n == 1 && xl.log[0].reference == IA64_REF_DATA);
                    CHECK(xl.log[0].rights == (write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ));
                    break;
                case 1: /* read only: load maps RO, store is a guest fault */
                    if (write)
                        CHECK(f == HOSTWIN_EXIT && why == HW_EXIT_GUEST_FAULT && xl.n == 1);
                    else
                        CHECK(f == HOSTWIN_MAPPED && stats(hw).fills_ro == ro + 1 &&
                              !wb.map_writable);
                    break;
                case 2: /* write only: never a shadow; load faults, store splits */
                    CHECK(f == HOSTWIN_EXIT &&
                          why == (write ? HW_EXIT_PERMISSION_SPLIT : HW_EXIT_GUEST_FAULT));
                    CHECK(!strcmp(prot_of((uintptr_t)host_of(hw, a)), "---p"));
                    break;
                default: /* neither */
                    CHECK(f == HOSTWIN_EXIT && why == HW_EXIT_GUEST_FAULT);
                }
            }
        }
    v->s.psr &= ~IA64_PSR_PK;
    /* PTE.d = 0: a read maps read-only, the lasting rights have no W (an
     * IA64_REF_PROBE check would skip the dirty bit and map RW). */
    uint64_t a = va + 0x200000;
    tc(a, pte(0x780000, IA64_MA_WB, 2, 0, true, false), itir(12, 0));
    MAPPED(hw, a, 8, false);
    CHECK(!wb.map_writable && !strcmp(prot_of((uintptr_t)host_of(hw, a)), "r--s"));
    EXITS(hw, a, 8, true, HW_EXIT_GUEST_FAULT);
    /* Through the trap path: the RO shadow faults on a store and exits. */
    struct accessor acc_ = {.hw = hw};
    acc_enter(&acc_);
    uint64_t x = 1;
    CHECK(!acc(&acc_, 0, a + 8, 8, true, &x) && hwt_why == HW_EXIT_GUEST_FAULT);
    CHECK(acc(&acc_, 0, a + 8, 8, false, &x));
    /* PTE.a = 0: the read check itself faults. */
    a = va + 0x210000;
    tc(a, pte(0x790000, IA64_MA_WB, 2, 0, false, true), itir(12, 0));
    EXITS(hw, a, 8, false, HW_EXIT_GUEST_FAULT);
    /* PSR.da = 1 suppresses both faults for the one access it covers (SDM
     * Vol.2 Table 3-2). Such rights never become a shadow.
     * PTE.d = 0: a read maps read-only, a store completes only in the
     * lower tier (PERMISSION_SPLIT); PTE.a = 0: no shadow for either.
     * PSR.da is not part of the context: clearing it drops nothing,
     * and without it the same accesses are the guest's Dirty/Access-bit
     * faults. */
    v->s.psr |= IA64_PSR_DA;
    hostwin_enter(hw, v);
    uint64_t live = stats(hw).live_mappings, drops = stats(hw).context_full_drops;
    tc(va + 0x220000, pte(0x7a0000, IA64_MA_WB, 2, 0, true, false), itir(12, 0));
    MAPPED(hw, va + 0x220000, 8, false);
    CHECK(!wb.map_writable && !strcmp(prot_of((uintptr_t)host_of(hw, va + 0x220000)), "r--s"));
    EXITS(hw, va + 0x220000, 8, true, HW_EXIT_PERMISSION_SPLIT);
    tc(va + 0x230000, pte(0x7b0000, IA64_MA_WB, 2, 0, false, true), itir(12, 0));
    EXITS(hw, va + 0x230000, 8, false, HW_EXIT_PERMISSION_SPLIT);
    EXITS(hw, va + 0x230000, 8, true, HW_EXIT_PERMISSION_SPLIT);
    CHECK(!strcmp(prot_of((uintptr_t)host_of(hw, va + 0x230000)), "---p"));
    v->s.psr &= ~IA64_PSR_DA;
    hostwin_enter(hw, v);
    CHECK(stats(hw).live_mappings == live + 1 && stats(hw).context_full_drops == drops);
    EXITS(hw, va + 0x220000, 8, true, HW_EXIT_GUEST_FAULT);
    EXITS(hw, va + 0x230000, 8, false, HW_EXIT_GUEST_FAULT);
    CHECK(xl.probe_refs == 0);
    hostwin_destroy(hw);
    ++tests;
}

/* The independent reference fill decision, recomputed here with
 * __real_ia64_translate: the access check, then a second IA64_REF_DATA
 * check of the other right with the same PA and WB. lasting: that second
 * check (and the access's own right) evaluated with PSR.da = 0, the rule
 * for lasting rights. Only virtual WB RAM pages that are not code reach it. */
struct decision {
    enum hostwin_fill fill;
    enum hostwin_exit_reason why;
    bool writable, access_faulted;
};
static bool second_check(uint64_t gva, unsigned rights, unsigned cpl,
                         const struct ia64_translation *t)
{
    struct ia64_translation t2;
    struct ia64_result r = __real_ia64_translate(v, gva, IA64_REF_DATA, rights, cpl, false, &t2);
    return r.kind == IA64_STEP_COMPLETE && !t2.deferred && t2.pa == t->pa && t2.ma == IA64_MA_WB;
}
static struct decision old_decision(uint64_t gva, bool write, bool lasting)
{
    unsigned cpl = (unsigned)(v->s.psr >> 32) & 3;
    struct ia64_translation t;
    struct ia64_result r = __real_ia64_translate(
        v, gva, IA64_REF_DATA, write ? IA64_ACCESS_WRITE : IA64_ACCESS_READ, cpl, false, &t);
    if (r.kind != IA64_STEP_COMPLETE || t.deferred)
        return (struct decision){HOSTWIN_EXIT, HW_EXIT_GUEST_FAULT, false, true};
    if (t.ma != IA64_MA_WB)
        return (struct decision){HOSTWIN_EXIT, HW_EXIT_ATTRIBUTE, false, false};
    uint64_t psr = v->s.psr;
    if (lasting)
        v->s.psr &= ~IA64_PSR_DA;
    bool readable = (!write && !lasting) || second_check(gva, IA64_ACCESS_READ, cpl, &t);
    bool writable = (write && !lasting) || second_check(gva, IA64_ACCESS_WRITE, cpl, &t);
    v->s.psr = psr;
    if (!readable || (lasting && write && !writable))
        return (struct decision){HOSTWIN_EXIT, HW_EXIT_PERMISSION_SPLIT, false, false};
    return (struct decision){HOSTWIN_MAPPED, (enum hostwin_exit_reason) - 1, writable, false};
}

/* Read-only, write-only, PKR denial and key miss, privilege, missing A/D
 * with and without the PSR.da suppression, not present, NaTPage and a
 * missing translation; reads and writes. With PSR.da = 0 the result is
 * exactly the independent reference one; with PSR.da = 1 it is the independent reference decision
 * with the rights taken without da, so no right only da grants becomes a
 * shadow. One translation per miss, and a guest-fault exit exactly when
 * the access check faults: the rights query adds none. */
static void test_lasting_rights(void)
{
    reset_vcpu();
    virtual_mode();
    set_pkr(0, 1 | (uint64_t)0x10 << 8);         /* RW */
    set_pkr(1, 1 | 2 | (uint64_t)0x11 << 8);     /* wd */
    set_pkr(2, 1 | 4 | (uint64_t)0x12 << 8);     /* rd */
    set_pkr(3, 1 | 4 | 2 | (uint64_t)0x13 << 8); /* rd, wd */
    struct hostwin_vcpu *hw = make(40, 8);
    const uint64_t va = UINT64_C(0x6000000000000000) | 0x40000000;
    const gpa_t pa = 0xb00000;
    /* key 0: PSR.pk = 0; 1..4: PKR 0..3; 5: a key no PKR holds. */
    static const uint32_t keys[6] = {0, 0x10, 0x11, 0x12, 0x13, 0x55};
    unsigned cases = 0, differ = 0, mapped = 0, splits = 0, faults = 0;
    for (unsigned cpl = 0; cpl < 4; cpl += 3)
        for (unsigned ar = 0; ar < 8; ++ar)
            for (unsigned pl = 0; pl < 4; ++pl)
                for (unsigned ad = 0; ad < 4; ++ad)
                    for (unsigned k = 0; k < 6; ++k)
                        for (unsigned da = 0; da < 2; ++da)
                            for (unsigned write = 0; write < 2; ++write) {
                                v->s.psr = (v->s.psr &
                                            ~(IA64_PSR_PK | IA64_PSR_DA | (UINT64_C(3) << 32))) |
                                           (k ? IA64_PSR_PK : 0) | (da ? IA64_PSR_DA : 0) |
                                           ((uint64_t)cpl << 32);
                                hostwin_invalidate(hw);
                                tc(va, pte(pa, IA64_MA_WB, ar, pl, ad & 1, ad & 2),
                                   itir(12, keys[k]));
                                struct decision old = old_decision(va + 8, write, false),
                                                want = old_decision(va + 8, write, true);
                                xl_reset();
                                enum hostwin_exit_reason why;
                                enum hostwin_fill f = fill(hw, va + 8, 8, write, &why);
                                CHECK(xl.n == 1 && xl.log[0].reference == IA64_REF_DATA &&
                                      xl.probe_refs == 0);
                                CHECK(f == want.fill &&
                                      (f == HOSTWIN_MAPPED ? wb.map_writable == want.writable
                                                           : why == want.why));
                                CHECK((f == HOSTWIN_EXIT && why == HW_EXIT_GUEST_FAULT) ==
                                      want.access_faulted);
                                if (!da)
                                    CHECK(f == old.fill &&
                                          (f == HOSTWIN_MAPPED ? wb.map_writable == old.writable
                                                               : why == old.why));
                                else {
                                    /* Never more than before: a shadow only where the old decision
             * made one, writable only where it was writable. */
                                    CHECK(f != HOSTWIN_MAPPED ||
                                          (old.fill == HOSTWIN_MAPPED &&
                                           (!wb.map_writable || old.writable)));
                                    differ += f != old.fill ||
                                              (f == HOSTWIN_MAPPED ? wb.map_writable != old.writable
                                                                   : why != old.why);
                                }
                                ++cases;
                                mapped += f == HOSTWIN_MAPPED;
                                splits += f == HOSTWIN_EXIT && why == HW_EXIT_PERMISSION_SPLIT;
                                faults += f == HOSTWIN_EXIT && why == HW_EXIT_GUEST_FAULT;
                            }
    /* Every outcome occurs, and PSR.da changes some (the single-translation cases). */
    CHECK(cases == 2 * 8 * 4 * 4 * 6 * 2 * 2 && mapped && splits && faults && differ);
    /* Not present, NaTPage, UC, and no translation at all, with da = 1. */
    v->s.psr = (v->s.psr & ~(IA64_PSR_PK | (UINT64_C(3) << 32))) | IA64_PSR_DA;
    static const struct {
        uint64_t pte;
        enum hostwin_exit_reason why;
    } other[] = {{0, HW_EXIT_GUEST_FAULT},
                 {1 | 7 << 2 | 0x60 | 2 << 9, HW_EXIT_GUEST_FAULT},
                 {1 | IA64_MA_UC << 2 | 0x60 | 2 << 9, HW_EXIT_ATTRIBUTE}};
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned write = 0; write < 2; ++write) {
            hostwin_invalidate(hw);
            tc(va, other[i].pte | (other[i].pte & 1 ? pa : 0), itir(12, 0));
            xl_reset();
            EXITS(hw, va + 8, 8, write, other[i].why);
            CHECK(xl.n == 1 && old_decision(va + 8, write, true).why == other[i].why);
        }
    EXITS(hw, va + 0x100000, 8, false, HW_EXIT_GUEST_FAULT);
    v->s.psr &= ~IA64_PSR_DA;
    CHECK(stats(hw).probe_calls == 0);
    fprintf(stderr,
            "hostwin_test: lasting rights: %u cases, %u mapped, %u splits, %u guest faults, "
            "%u changed by PSR.da = 1\n",
            cases, mapped, splits, faults, differ);
    hostwin_destroy(hw);
    ++tests;
}

/* No shadow for pages not wholly inside one
 * page-aligned GM_RAM region; every access to them exits. */
static void test_eligibility(void)
{
    reset_vcpu();
    struct hostwin_vcpu *hw = make(40, 8);
    struct accessor a = {.hw = hw};
    static const struct {
        uint64_t offset;
        enum hostwin_exit_reason why;
    } cases[] = {{0x00000, HW_EXIT_PARTIAL_PAGE}, {0x007f8, HW_EXIT_PARTIAL_PAGE},
                 {0x00800, HW_EXIT_NOT_RAM},      {0x10000, HW_EXIT_PARTIAL_PAGE},
                 {0x10800, HW_EXIT_NOT_RAM},      {0x20000, HW_EXIT_NOT_RAM},
                 {0x20800, HW_EXIT_PARTIAL_PAGE}, {0x31000, HW_EXIT_PARTIAL_PAGE},
                 {0x31bf8, HW_EXIT_PARTIAL_PAGE}, {0x31c00, HW_EXIT_NOT_RAM},
                 {0x40000, HW_EXIT_PARTIAL_PAGE}, {0x41000, HW_EXIT_PARTIAL_PAGE},
                 {0x50000, HW_EXIT_NOT_RAM},      {0x60000, HW_EXIT_NOT_RAM},
                 {0x70000, HW_EXIT_UNBACKED},     {0x80000, HW_EXIT_NOT_RAM}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        for (int write = 0; write < 2; ++write) {
            EXITS(hw, ELIG + cases[i].offset, 8, write, cases[i].why);
            CHECK(stats(hw).live_mappings == 0);
            /* And through the trap path, repeatedly: never a shadow. */
            acc_enter(&a);
            uint64_t x = 0;
            CHECK(!acc(&a, 0, ELIG + cases[i].offset, 8, write, &x) && hwt_why == cases[i].why);
        }
    /* The whole-RAM neighbours do map. */
    MAPPED(hw, ELIG + 0x21000, 8, true);
    MAPPED(hw, ELIG + 0x30ff8, 8, false);
    CHECK(stats(hw).live_mappings == 2);
    hostwin_destroy(hw);
    ++tests;
}

/* Entry diff, field by field, and the selective invalidation. */
static void test_entry_diff(void)
{
    reset_vcpu();
    virtual_mode();
    struct hostwin_vcpu *hw = make(40, 8);
    const uint64_t r3 = UINT64_C(3) << 61, r5 = UINT64_C(5) << 61;
    /* Shadows: A (VRN 3, TC), B (VRN 5, TC, same payload as A but another
     * RID), C (VRN 3, TC, elsewhere), D (VRN 3, TR 2). */
    uint64_t A = r3 | 0x10000000, B = r5 | 0x10000000, C = r3 | 0x20000000, D = r3 | 0x30000000;
    tc(A, rw(0x800000), itir(12, 0));
    tc(B, rw(0x801000), itir(12, 0));
    tc(C, rw(0x802000), itir(12, 0));
    tr(2, D, rw(0x803000), itir(12, 0));
#define FILL_ALL()                                                                                 \
    do {                                                                                           \
        hostwin_enter(hw, v);                                                                      \
        MAPPED(hw, A, 8, false);                                                                   \
        MAPPED(hw, B, 8, false);                                                                   \
        MAPPED(hw, C, 8, false);                                                                   \
        MAPPED(hw, D, 8, false);                                                                   \
        CHECK(stats(hw).live_mappings == 4);                                                       \
    } while (0)
    FILL_ALL();
    uint64_t epoch = hostwin_bind_epoch(hw);
    /* Unchanged context, and tc_next alone: no drop. */
    hostwin_enter(hw, v);
    v->s.tc_next[1] += 5;
    hostwin_enter(hw, v);
    v->s.psr ^= IA64_PSR_I; /* a PSR bit outside the context */
    hostwin_enter(hw, v);
    v->s.psr ^= IA64_PSR_DA; /* outside under the single-translation contract */
    hostwin_enter(hw, v);
    v->s.psr ^= IA64_PSR_DA;
    hostwin_enter(hw, v);
    CHECK(stats(hw).live_mappings == 4 && stats(hw).context_full_drops == 0);
    /* Each register field drops everything, bindings stay. kind: the log
     * entry of an RR or PKR write (PSR is never logged). */
    struct {
        uint64_t *word, bit;
        int kind;
        unsigned index;
    } fields[] = {{&v->s.rr[0], 0x100, IA64_MMU_RR, 0}, {&v->s.rr[7], 0x100, IA64_MMU_RR, 7},
                  {&v->s.pkr[0], 1, IA64_MMU_PKR, 0},   {&v->s.pkr[15], 0x100, IA64_MMU_PKR, 15},
                  {&v->s.psr, IA64_PSR_PK, -1, 0},      {&v->s.psr, UINT64_C(1) << 32, -1, 0},
                  {&v->s.psr, UINT64_C(2) << 32, -1, 0}};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        uint64_t drops = stats(hw).context_full_drops;
        *fields[i].word ^= fields[i].bit;
        if (fields[i].kind >= 0)
            ia64_mmu_changed(v, (enum ia64_mmu_change_kind)fields[i].kind, 0, fields[i].index);
        hostwin_enter(hw, v);
        CHECK(stats(hw).live_mappings == 0 && stats(hw).context_full_drops == drops + 1);
        *fields[i].word ^= fields[i].bit;
        if (fields[i].kind >= 0)
            ia64_mmu_changed(v, (enum ia64_mmu_change_kind)fields[i].kind, 0, fields[i].index);
        FILL_ALL();
    }
    v->s.psr &= ~IA64_PSR_DT; /* PSR.dt */
    hostwin_enter(hw, v);
    CHECK(stats(hw).live_mappings == 0);
    virtual_mode();
    FILL_ALL();
    CHECK(hostwin_bind_epoch(hw) == epoch);
    /* TC entry of A purged (valid = 0): only A goes. */
    const struct ia64_tlb_entry *e = ia64_tlb_find(&v->s, A, 1);
    CHECK(e && e >= &v->s.tc[1][0] && e < &v->s.tc[1][IA64_TC_ENTRIES]);
    ((struct ia64_tlb_entry *)e)->valid = false;
    logged_entry(e);
    uint64_t diffs = stats(hw).entry_diffs;
    hostwin_enter(hw, v);
    CHECK(stats(hw).live_mappings == 3 && stats(hw).entry_diffs == diffs + 1);
    CHECK(!strcmp(prot_of((uintptr_t)host_of(hw, A)), "---p") &&
          !strcmp(prot_of((uintptr_t)host_of(hw, B)), "rw-s"));
    /* Any field of an entry: pte (ar) of B's entry. */
    e = ia64_tlb_find(&v->s, B, 1);
    ((struct ia64_tlb_entry *)e)->pte ^= UINT64_C(1) << 9;
    logged_entry(e);
    hostwin_enter(hw, v);
    CHECK(stats(hw).live_mappings == 2 && !strcmp(prot_of((uintptr_t)host_of(hw, B)), "---p"));
    ((struct ia64_tlb_entry *)e)->pte ^= UINT64_C(1) << 9;
    logged_entry(e);
    hostwin_enter(hw, v);
    /* A TR overlapping a shadowed TC VA: C is sourced from a TC entry; a
     * 256 MiB TR covering C (and not D) is inserted in TR slot 5: C lies in
     * it (matches() with shift 12) and goes; D (sourced from TR 2) stays. */
    tc(A, rw(0x800000), itir(12, 0));
    MAPPED(hw, A, 8, false);
    MAPPED(hw, B, 8, false);
    CHECK(stats(hw).live_mappings == 4);
    tr(5, r3 | 0x20000000, rw(0x10000000), itir(28, 0));
    hostwin_enter(hw, v);
    CHECK(stats(hw).live_mappings == 3 && !strcmp(prot_of((uintptr_t)host_of(hw, C)), "---p"));
    CHECK(!strcmp(prot_of((uintptr_t)host_of(hw, D)), "rw-s"));
    v->s.tr[1][5].valid = false;
    logged_entry(&v->s.tr[1][5]);
    hostwin_enter(hw, v);
    /* TR vs TC sources: purging TR 2 drops D only. */
    v->s.tr[1][2].valid = false;
    logged_entry(&v->s.tr[1][2]);
    hostwin_enter(hw, v);
    CHECK(stats(hw).live_mappings == 2 && !strcmp(prot_of((uintptr_t)host_of(hw, D)), "---p"));
    /* VRN != 0 and RIDs: a new entry for VRN 5's RID at the payload A and B
     * share drops B only; then with VRN 3 and 5 sharing one RID, an entry
     * at that payload drops both (VRN is stripped, RID decides). */
    tc(B | 0x1000, rw(0x804000), itir(16, 0)); /* 64 KiB covers B's page */
    hostwin_enter(hw, v);
    CHECK(stats(hw).live_mappings == 1 && !strcmp(prot_of((uintptr_t)host_of(hw, A)), "rw-s"));
    set_rr(5, (v->s.rr[5] & 0xff) | (v->s.rr[3] & ~UINT64_C(0xff)));
    hostwin_enter(hw, v); /* RR change: full drop */
    tc(A, rw(0x800000), itir(12, 0));
    MAPPED(hw, A, 8, false);
    MAPPED(hw, B, 8, false); /* same RID and payload: the same entry */
    CHECK(stats(hw).live_mappings == 2);
    tc(A | 0x2000, rw(0x805000), itir(16, 0));
    hostwin_enter(hw, v);
    CHECK(stats(hw).live_mappings == 0);
    /* Physical shadows do not depend on the TLB. */
    v->s.psr &= ~IA64_PSR_DT;
    hostwin_enter(hw, v);
    MAPPED(hw, 0x806000, 8, true);
    for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i) {
        v->s.tc[1][i].valid = false;
        logged_entry(&v->s.tc[1][i]);
    }
    tc(0x806000, rw(0x806000), itir(28, 0));
    hostwin_enter(hw, v);
    CHECK(stats(hw).live_mappings == 1);
    CHECK(hostwin_bind_epoch(hw) == epoch);
    hostwin_destroy(hw);
    ++tests;
#undef FILL_ALL
}

/* A shadow records the TR or TC slot that sourced it, for
 * every TR index and every TC index: invalidating exactly that slot drops
 * it, changing any other slot does not. A miss (no entry, VHPT walker
 * off) is a guest-fault exit that records nothing. */
static void test_entry_sources(void)
{
    reset_vcpu();
    virtual_mode();
    struct hostwin_vcpu *hw = make(40, 8);
    const uint64_t base = (UINT64_C(4) << 61) | 0x7000000;
    struct ia64_tlb_entry *slots[IA64_DTR_COUNT + IA64_TC_ENTRIES];
    for (unsigned i = 0; i < IA64_DTR_COUNT; ++i)
        slots[i] = &v->s.tr[1][i];
    for (unsigned i = 0; i < IA64_TC_ENTRIES; ++i)
        slots[IA64_DTR_COUNT + i] = &v->s.tc[1][i];
    const unsigned n = IA64_DTR_COUNT + IA64_TC_ENTRIES;
    for (unsigned k = 0; k < n; ++k) {
        uint64_t gva = base + (uint64_t)k * PAGE;
        if (k < IA64_DTR_COUNT) {
            tr(k, gva, rw(0x900000 + k * PAGE), itir(12, 0));
        } else {
            *slots[k] = (struct ia64_tlb_entry){.va = gva & ((UINT64_C(1) << 61) - 1) &
                                                      ~(uint64_t)(PAGE - 1),
                                                .rid = (uint32_t)(v->s.rr[4] >> 8) & 0xffffff,
                                                .pte = rw(0x900000 + k * PAGE),
                                                .itir = itir(12, 0),
                                                .page_shift = 12,
                                                .valid = true};
            logged_entry(slots[k]);
        }
        CHECK(ia64_tlb_find(&v->s, gva, 1) == slots[k]);
        hostwin_enter(hw, v);
        MAPPED(hw, gva, 8, false);
        CHECK(stats(hw).live_mappings == 1 && wb.map_pa == 0x900000 + k * PAGE);
        /* Another slot changes (a valid entry elsewhere): the shadow stays. */
        struct ia64_tlb_entry *other = slots[(k + 1) % n], saved = *other;
        *other = (struct ia64_tlb_entry){.va = 0x3000000,
                                         .rid = 0x400,
                                         .pte = rw(0xa00000),
                                         .itir = itir(12, 0),
                                         .page_shift = 12,
                                         .valid = true};
        logged_entry(other);
        hostwin_enter(hw, v);
        CHECK(stats(hw).live_mappings == 1);
        *other = saved;
        logged_entry(other);
        hostwin_enter(hw, v);
        CHECK(stats(hw).live_mappings == 1);
        /* Its own slot is invalidated: exactly this shadow goes. */
        slots[k]->valid = false;
        logged_entry(slots[k]);
        hostwin_enter(hw, v);
        CHECK(stats(hw).live_mappings == 0 &&
              !strcmp(prot_of((uintptr_t)host_of(hw, gva)), "---p"));
    }
    uint64_t faults = stats(hw).exits[HW_EXIT_GUEST_FAULT];
    EXITS(hw, base + (uint64_t)n * PAGE, 8, false, HW_EXIT_GUEST_FAULT);
    CHECK(stats(hw).live_mappings == 0 && stats(hw).exits[HW_EXIT_GUEST_FAULT] == faults + 1);
    hostwin_destroy(hw);
    ++tests;
}

/* The VHPT walker insertion made by resolve's own access check is a
 * context change handled right after step 1: it purges an
 * overlapping TC entry that sources another shadow. tc[]/tc_next change
 * exactly as one interpreter check would. */
static void test_walker_insertion(void)
{
    reset_vcpu();
    virtual_mode();
    struct hostwin_vcpu *hw = make(40, 8);
    const uint64_t r2 = UINT64_C(2) << 61;
    set_rr(2, UINT64_C(0x222) << 8 | 16 << 2 | 1);     /* ps 64 KiB, VHPT on */
    v->s.cr[CR_PTA] = (r2 | 0x40000000) | 15 << 2 | 1; /* short format, 32 KiB */
    uint64_t X
    = r2 | 0x12340000, Y = X
    +0x3000;                          /* same 64 KiB page */
    tc(X, rw(0x900000), itir(12, 0)); /* 4 KiB entry for X */
    MAPPED(hw, X, 8, false);
    /* VHPT: map its page with a TR, write a 64 KiB PTE for Y. */
    uint64_t h = ia64_thash(&v->s, Y);
    tr(0, h, rw(0xa00000 + (h & 0x7000)), itir(12, 0));
    gpa_t pte_pa = 0xa00000 + (h & 0x7fff);
    uint64_t p = rw(0x910000);
    memcpy(ram + pte_pa, &p, 8);
    unsigned next = v->s.tc_next[1];
    uint64_t diffs = stats(hw).entry_diffs;
    MAPPED(hw, Y, 8, false);
    CHECK(v->s.tc_next[1] == next + 1 && stats(hw).entry_diffs >= diffs + 2);
    /* X's entry was purged by the 64 KiB insertion: X's shadow is gone,
     * Y's shadow maps 0x913000. */
    CHECK(stats(hw).live_mappings == 1 && wb.map_pa == 0x913000);
    CHECK(!strcmp(prot_of((uintptr_t)host_of(hw, X)), "---p"));
    hostwin_destroy(hw);
    ++tests;
}

/* Code events: tracked -> read-only aliases (in every instance, before
 * the snapshot is read); untracked -> nothing; the next window store
 * upgrades in one fill. */
static struct hostwin_vcpu *snap_hw;
static uintptr_t snap_host;
static int snap_seen;
static void snapshot_probe(gpa_t page, enum code_test_stage stage)
{
    if (stage != CODE_TEST_SNAPSHOT || page != CODE_PA)
        return;
    /* Taken right after prepare() read the snapshot: the alias must
     * already have been read-only. */
    snap_seen = !strcmp(prot_of(snap_host), "r--s") ? 1 : -1;
}
/* Lock order: code_lock, then the instance
 * lock. A fill checks for code under its instance lock, so that check must
 * never take code_lock (link-time wrap). Without code validity there is no
 * tracked set to check: the fill stops instead of mapping. */
static const struct hostwin_vcpu *lock_probe;
void __real_code_lock(void);
void __wrap_code_lock(void);
void __wrap_code_lock(void)
{
    if (lock_probe)
        CHECK(!__atomic_load_n(&lock_probe->lock, __ATOMIC_RELAXED));
    __real_code_lock();
}
static void test_validity_off(void)
{
    reset_vcpu();
    struct hostwin_vcpu *hw = make(40, 8);
    CHECK(code_validity_configure(CODE_VALIDITY_NONE, CODE_SNAPSHOT_DEFAULT_BYTES));
    lock_probe = hw;
    enum hostwin_exit_reason why;
    CHECK(fill(hw, 0x500008, 8, true, &why) == HOSTWIN_EXIT && why == HW_EXIT_BACKEND);
    lock_probe = NULL;
    CHECK(hostwin_stop_detail(hw) && v->requested_exit == VCPU_EXIT_BAD_ACCESS + 1);
    CHECK(stats(hw).live_mappings == 0);
    CHECK(code_validity_configure(CODE_VALIDITY_COMPARE_ALWAYS, CODE_SNAPSHOT_DEFAULT_BYTES));
    hostwin_destroy(hw);
    reset_vcpu();
    ++tests;
}

static void test_code_events(void)
{
    reset_vcpu();
    struct hostwin_vcpu *hw = make(40, 8), *hw2 = make(36, 2);
    code_validity_set_track_hook(hostwin_code_page_event);
    code_validity_set_test_hook(snapshot_probe);
    /* Aliases of CODE_PA: one physical in hw2, two virtual (two tags) in hw. */
    MAPPED(hw2, CODE_PA, 8, true);
    uintptr_t h3 = (uintptr_t)host_of(hw2, CODE_PA);
    virtual_mode();
    uint64_t va = UINT64_C(0x8000000000000000) | UINT64_C(0x77000000000);
    uint64_t vb = UINT64_C(0x2000000000000000) | UINT64_C(0x99000000000);
    tc(va, rw(CODE_PA), itir(12, 0));
    tc(vb, rw(CODE_PA), itir(12, 0));
    MAPPED(hw, va, 8, true);
    MAPPED(hw, vb, 8, false);
    uintptr_t h2 = (uintptr_t)host_of(hw, va), h2b = (uintptr_t)host_of(hw, vb);
    CHECK(!strcmp(prot_of(h2), "rw-s") && !strcmp(prot_of(h2b), "rw-s") &&
          !strcmp(prot_of(h3), "rw-s"));
    /* Becomes code: code_page_enter -> prepare -> TRACKED, before the
     * snapshot is read (snapshot_probe runs right after it was taken). */
    snap_hw = hw;
    snap_host = h2;
    snap_seen = 0;
    uint64_t events = stats(hw).code_events, events2 = stats(hw2).code_events;
    CHECK(make_code(CODE_PA));
    CHECK(code_page_has_code(CODE_PA) && snap_seen == 1);
    CHECK(stats(hw).code_events == events + 1 && stats(hw2).code_events == events2 + 1);
    CHECK(!strcmp(prot_of(h2), "r--s") && !strcmp(prot_of(h2b), "r--s") &&
          !strcmp(prot_of(h3), "r--s"));
    /* A store through the window now exits (the lower tier stores with
     * notification); loads stay native. */
    struct accessor a = {.hw = hw};
    acc_enter(&a);
    uint64_t x = 0x99, traps = hwt_traps;
    CHECK(!acc(&a, 0, va + 16, 8, true, &x) && hwt_why == HW_EXIT_CODE_PAGE_STORE);
    CHECK(acc(&a, 0, va + 16, 8, false, &x) && hwt_traps == traps + 1);
    /* Untracked (a CPU write notification): no action, still read-only. */
    uint64_t fills = stats(hw).fills;
    code_notify_cpu_write(CODE_PA + 16, 8);
    CHECK(!code_page_has_code(CODE_PA) && !strcmp(prot_of(h2), "r--s"));
    CHECK(stats(hw).code_events == events + 2);
    /* The window store completes after exactly one fill. */
    x = 0x1234;
    traps = hwt_traps;
    CHECK(acc(&a, 0, va + 16, 8, true, &x) && ram_value(CODE_PA + 16, 8) == 0x1234);
    CHECK(stats(hw).fills == fills + 1 && hwt_traps == traps + 1 && !strcmp(prot_of(h2), "rw-s"));
    CHECK(acc(&a, 0, va + 24, 8, true, &x) && hwt_traps == traps + 1);
    /* Tracked again; then the validity cache is flushed: UNTRACKED from
     * invalidate_page, the aliases stay read-only. */
    CHECK(make_code(CODE_PA));
    CHECK(!strcmp(prot_of(h2), "r--s"));
    code_cache_flush();
    CHECK(!code_page_has_code(CODE_PA) && !strcmp(prot_of(h2), "r--s"));
    code_validity_set_test_hook(NULL);
    code_validity_set_track_hook(NULL);
    /* Unregistered, tracking is inert: the RW alias stays RW. */
    MAPPED(hw, va, 8, true);
    events = stats(hw).code_events;
    CHECK(make_code(CODE_PA) && !strcmp(prot_of(h2), "rw-s"));
    CHECK(stats(hw).code_events == events);
    code_cache_flush();
    hostwin_destroy(hw2);
    hostwin_destroy(hw);
    ++tests;
}

/* Per-CPU contexts: the bare-metal
 * backend gives each host CPU's private root its own context. Every
 * backend operation of an instance carries the instance's context: its
 * fills and drops, the protection of a newly tracked code page in every
 * instance, and its release. (The Linux backend ignores the context.) */
static void test_contexts(void)
{
    reset_vcpu();
    static int root[2];
    struct hostwin_config c = {.w = 36, .slots = 2, .check = true};
    struct hostwin_vcpu *hw[2];
    for (unsigned i = 0; i < 2; ++i)
        CHECK((hw[i] = hostwin_create(&c, &wrapped, &root[i])) != NULL);
    code_validity_set_track_hook(hostwin_code_page_event);
    for (unsigned i = 0; i < 2; ++i) {
        MAPPED(hw[i], CODE_PA, 8, true);
        CHECK(wb.map_ctx == &root[i]);
    }
    uint64_t protects = wb.protects;
    CHECK(make_code(CODE_PA));
    CHECK(wb.protects == protects + 2);
    void *first = wb.protect_ctx[protects % 4], *second = wb.protect_ctx[(protects + 1) % 4];
    CHECK((first == &root[0] && second == &root[1]) || (first == &root[1] && second == &root[0]));
    for (unsigned i = 0; i < 2; ++i) {
        hostwin_test_drop(hw[i], CODE_PA);
        CHECK(wb.unmap_ctx == &root[i]);
        MAPPED(hw[i], CODE_PA, 8, false);
        hostwin_invalidate(hw[i]);
        CHECK(wb.unmap_ctx == &root[i]);
    }
    code_validity_set_track_hook(NULL);
    code_cache_flush();
    for (unsigned i = 0; i < 2; ++i) {
        hostwin_destroy(hw[i]);
        CHECK(wb.release_ctx == &root[i]);
    }
    ++tests;
}

/* Test injector and its place in the order (hostwin.h). */
static void test_inject(void)
{
    reset_vcpu();
    virtual_mode();
    struct hostwin_vcpu *hw = make(40, 8);
    uint64_t va = UINT64_C(0x2000000000000000) | 0x700000;
    for (unsigned i = 0; i < 3; ++i)
        tc(va + i * PAGE, rw(0x700000 + i * PAGE), itir(12, 0));
    hostwin_test_inject(hw, 2);
    MAPPED(hw, va, 8, false);
    uint64_t calls = xl.calls;
    EXITS(hw, va + PAGE, 8, false, HW_EXIT_INJECTED);
    CHECK(xl.calls == calls && stats(hw).exits[HW_EXIT_INJECTED] == 1);
    MAPPED(hw, va + PAGE, 8, false);
    MAPPED(hw, va + 2 * PAGE, 8, false);
    hostwin_test_inject(hw, 1);
    hostwin_test_inject(hw, 0);
    MAPPED(hw, va + 2 * PAGE, 8, true);
    hostwin_destroy(hw);
    ++tests;
}

/* VMA accounting (mapping count): an isolated shadow costs 2 VMAs, a
 * run of consecutive pages merges; the reservation never exceeds
 * 2 * live + 1. At HOSTWIN_MAX_SHADOWS the instance drops its shadows. */
static void test_vma_accounting(void)
{
    reset_vcpu();
    struct hostwin_vcpu *hw = make(40, 8);
    CHECK(reservation_vmas() == 1);
    /* Odd pages: each shadow has unmapped neighbours on both sides. */
    for (unsigned i = 0; i < 100; ++i)
        MAPPED(hw, (gpa_t)(2 * i + 1) * PAGE, 8, false);
    CHECK(stats(hw).live_mappings == 100 && reservation_vmas() == 2 * 100 + 1);
    hostwin_invalidate(hw);
    CHECK(reservation_vmas() == 1 && stats(hw).live_mappings == 0);
    for (unsigned i = 0; i < 100; ++i)
        MAPPED(hw, (gpa_t)0x1000000 + i * PAGE, 8, true);
    CHECK(reservation_vmas() == 3);
    /* A read-only page inside the run splits it: + 2. */
    hostwin_code_page_event(0x1000000 + 50 * PAGE, CODE_PAGE_TRACKED);
    CHECK(reservation_vmas() == 5 && stats(hw).live_mappings == 100);
    hostwin_invalidate(hw);
    uint64_t drops = stats(hw).shadow_drops;
    for (unsigned i = 0; i < HOSTWIN_MAX_SHADOWS; ++i)
        MAPPED(hw, (gpa_t)(2 * i + 1) * PAGE, 1, false);
    CHECK(stats(hw).live_mappings == HOSTWIN_MAX_SHADOWS);
    CHECK(reservation_vmas() == 2 * HOSTWIN_MAX_SHADOWS + 1);
    MAPPED(hw, (gpa_t)(2 * HOSTWIN_MAX_SHADOWS + 1) * PAGE, 1, false);
    CHECK(stats(hw).live_mappings == 1 && reservation_vmas() == 3);
    CHECK(stats(hw).shadow_drops == drops + HOSTWIN_MAX_SHADOWS);
    /* An upgrade of an existing shadow at the cap does not drop. */
    hostwin_invalidate(hw);
    for (unsigned i = 0; i < HOSTWIN_MAX_SHADOWS; ++i)
        MAPPED(hw, (gpa_t)(2 * i + 1) * PAGE, 1, false);
    MAPPED(hw, PAGE, 1, true);
    CHECK(stats(hw).live_mappings == HOSTWIN_MAX_SHADOWS);
    hostwin_destroy(hw);
    CHECK(reservation_vmas() == 0);
    /* Admission: a reservation must be able to grow to its bound. */
    struct hostwin_linux_stats ls;
    hostwin_linux_get_stats(&ls);
    CHECK(ls.max_map_count > 0 &&
          hostwin_linux_vmas_per_reservation() == 2 * HOSTWIN_MAX_SHADOWS + 1);
    ++tests;
}

/* Backend failures, deterministic through the wrapper:
 * protect_ro -> unmap of the page -> unmap of the slot; never a writable
 * alias of a code page, never a stale shadow. */
static void test_backend_failures(void)
{
    reset_vcpu();
    struct hostwin_vcpu *hw = make(40, 8);
    /* Three writable shadows, consecutive pages of one slot. */
    MAPPED(hw, CODE_PA, 8, true);
    MAPPED(hw, CODE_PA + PAGE, 8, true);
    MAPPED(hw, CODE_PA + 2 * PAGE, 8, true);
    uintptr_t h0 = (uintptr_t)host_of(hw, CODE_PA);
    /* protect fails, the page unmap succeeds: that alias is gone. */
    wb.fail_protect = 1;
    hostwin_code_page_event(CODE_PA + PAGE, CODE_PAGE_TRACKED);
    CHECK(!strcmp(prot_of(h0 + PAGE), "---p") && !strcmp(prot_of(h0), "rw-s"));
    CHECK(stats(hw).live_mappings == 2);
    /* Each failed backend operation is counted (VER: the product runs
     * report backend_failures next to the tier1 line, tier1stats.py). */
    CHECK(stats(hw).backend_failures == 1);
    /* protect and the page unmap fail: the whole slot goes. */
    MAPPED(hw, CODE_PA + PAGE, 8, false);
    hostwin_code_page_event(CODE_PA + PAGE, CODE_PAGE_UNTRACKED);
    MAPPED(hw, CODE_PA + PAGE, 8, true);
    wb.fail_protect = 1;
    wb.fail_unmap_page = 1;
    hostwin_code_page_event(CODE_PA + PAGE, CODE_PAGE_TRACKED);
    CHECK(stats(hw).live_mappings == 0 && !strcmp(prot_of(h0), "---p") &&
          !strcmp(prot_of(h0 + 2 * PAGE), "---p"));
    CHECK(stats(hw).backend_failures == 3);
    /* A selective drop whose page unmap fails also falls back to the slot. */
    virtual_mode();
    hostwin_enter(hw, v);
    uint64_t va = UINT64_C(0x2000000000000000) | 0x900000;
    tc(va, rw(0x900000), itir(12, 0));
    tc(va + PAGE, rw(0x901000), itir(12, 0));
    MAPPED(hw, va, 8, false);
    MAPPED(hw, va + PAGE, 8, false);
    v->s.tc[1][(v->s.tc_next[1] - 1) % IA64_TC_ENTRIES].valid = false;
    logged_entry(&v->s.tc[1][(v->s.tc_next[1] - 1) % IA64_TC_ENTRIES]);
    wb.fail_unmap_page = 1;
    hostwin_enter(hw, v);
    CHECK(stats(hw).live_mappings == 0 && !wb.fail_unmap_page && !wb.fail_protect);
    CHECK(stats(hw).backend_failures == 4);
    hostwin_destroy(hw);
    ++tests;
}

/* ---- children ----------------------------------------------------------- */

/* fds: NULL, or a pipe whose write end becomes the child's stderr. Returns
 * the wait status; out receives the child's stderr. */
static int run_child(void (*body)(void), char *out, size_t out_size)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    fflush(NULL);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        alarm(60); /* a looping handler fails the test */
        body();
        _exit(0);
    }
    close(fds[1]);
    size_t used = 0;
    for (ssize_t n; used + 1 < out_size && (n = read(fds[0], out + used, out_size - 1 - used)) > 0;)
        used += (size_t)n;
    out[used] = 0;
    char sink[256];
    while (read(fds[0], sink, sizeof(sink)) > 0) {
    }
    close(fds[0]);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    return status;
}
static char child_out[1 << 16];
#define CHILD_EXITED(s, code) (WIFEXITED(s) && WEXITSTATUS(s) == (code))

static volatile uintptr_t outside_address = 8;
static void prev_siginfo(int sig, siginfo_t *info, void *ctx)
{
    (void)sig;
    (void)info;
    (void)ctx;
    _exit(42);
}
static void prev_plain(int sig)
{
    (void)sig;
    _exit(43);
}
static struct sigaction initial; /* SIGSEGV disposition at start-up (ASan's under SAN=1) */
static int chain_kind;
static void chain_body(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    if (chain_kind == 0) {
        sa.sa_sigaction = prev_siginfo;
        sa.sa_flags = SA_SIGINFO;
    } else if (chain_kind == 1)
        sa.sa_handler = prev_plain;
    else
        sa.sa_handler = SIG_DFL;
    if (chain_kind <= 2)
        CHECK(sigaction(SIGSEGV, &sa, NULL) == 0);
    else
        CHECK(sigaction(SIGSEGV, &initial, NULL) == 0);
    /* Installed twice: the second call must not chain to itself. */
    CHECK(hostwin_linux_init() && hostwin_linux_init());
    *(volatile uint64_t *)outside_address = 1;
}
/* A fault inside a reservation the client declines (unknown RIP) and a
 * poison-page load also chain to the previous handler. */
static void declined_body(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = prev_siginfo;
    sa.sa_flags = SA_SIGINFO;
    CHECK(sigaction(SIGSEGV, &sa, NULL) == 0);
    CHECK(hostwin_linux_init());
    struct hostwin_vcpu *hw = make(36, 1);
    (void)*(volatile uint64_t *)hostwin_poison_va(hw);
}
static void noclient_body(void)
{
    reset_vcpu();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = prev_plain;
    CHECK(sigaction(SIGSEGV, &sa, NULL) == 0);
    CHECK(hostwin_linux_init());
    hostwin_set_trap_client(NULL);
    struct hostwin_vcpu *hw = make(36, 1);
    (void)hwt_ld8(host_of(hw, 0x1000));
}

static void test_chaining(void)
{
    chain_kind = 0;
    CHECK(CHILD_EXITED(run_child(chain_body, child_out, sizeof(child_out)), 42));
    chain_kind = 1;
    CHECK(CHILD_EXITED(run_child(chain_body, child_out, sizeof(child_out)), 43));
    chain_kind = 2;
    int s = run_child(chain_body, child_out, sizeof(child_out));
    CHECK(WIFSIGNALED(s) && WTERMSIG(s) == SIGSEGV);
#if defined(__SANITIZE_ADDRESS__)
    chain_kind = 3;
    s = run_child(chain_body, child_out, sizeof(child_out));
    CHECK(WIFEXITED(s) && WEXITSTATUS(s) != 0 && strstr(child_out, "AddressSanitizer: SEGV"));
#endif
    CHECK(CHILD_EXITED(run_child(declined_body, child_out, sizeof(child_out)), 42));
    CHECK(CHILD_EXITED(run_child(noclient_body, child_out, sizeof(child_out)), 43));
    ++tests;
}

/* Explicit backend errors panic with the statistics (never a silent slow
 * path): map ERROR, and an unmap failure of a whole slot. */
static void map_error_body(void)
{
    reset_vcpu();
    struct hostwin_vcpu *hw = make(40, 8);
    MAPPED(hw, 0x10000, 8, false);
    wb.fail_map = 1;
    enum hostwin_exit_reason why;
    (void)fill(hw, 0x20000, 8, false, &why);
    _exit(3);
}
static void slot_error_body(void)
{
    reset_vcpu();
    struct hostwin_vcpu *hw = make(36, 1);
    MAPPED(hw, 0x10000, 8, false);
    wb.fail_unmap_slot = 1;
    struct hostwin_tag t;
    (void)hostwin_bind(hw, v, UINT64_C(1) << 36, &t); /* rebinding must unmap */
    _exit(3);
}

/* The real kernel limit, in a child: fill the process's VMA budget with
 * mprotect splits of a scratch mapping until ENOMEM, then (b) a code event
 * on the middle page of a merged run (protect and unmap of one page both
 * need a new VMA) still leaves no writable alias, and (a) a fill panics
 * with the VMA statistics. The scratch mapping is released at exit so
 * that ASan's leak check can run. */
static uint8_t *eater;
static size_t eater_bytes;
static void eater_release(void)
{
    if (eater)
        munmap(eater, eater_bytes);
    eater = NULL;
}
static void eat_vmas(void)
{
    struct hostwin_linux_stats ls;
    hostwin_linux_get_stats(&ls);
    eater_bytes = ((size_t)ls.max_map_count + 16) * 2 * PAGE;
    eater = mmap(NULL, eater_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(eater != MAP_FAILED);
    atexit(eater_release);
    for (size_t i = 1;; i += 2) {
        CHECK(i * PAGE < eater_bytes);
        if (mprotect(eater + i * PAGE, PAGE, PROT_READ)) {
            CHECK(errno == ENOMEM);
            break;
        }
    }
}
static void enomem_body(void)
{
    reset_vcpu();
    struct hostwin_vcpu *hw = make(40, 8);
    for (unsigned i = 0; i < 3; ++i)
        MAPPED(hw, CODE_PA + i * PAGE, 8, true);
    uintptr_t h = (uintptr_t)host_of(hw, CODE_PA);
    CHECK(reservation_vmas() == 3); /* before, merged run, after */
    struct hostwin_linux_stats ls;
    hostwin_linux_get_stats(&ls);
    uint64_t failures = ls.failures;
    eat_vmas();
    /* (b) protect_ro of the run's middle page needs a split: ENOMEM; so
     * may the page unmap; the whole-slot replacement still succeeds. */
    hostwin_code_page_event(CODE_PA + PAGE, CODE_PAGE_TRACKED);
    hostwin_linux_get_stats(&ls);
    CHECK(ls.failures > failures && ls.last_errno == ENOMEM);
    CHECK(strcmp(prot_of(h + PAGE), "rw-s") != 0 && stats(hw).live_mappings < 3);
    /* (a) fills of isolated pages: the map fails, hostwin panics. */
    for (unsigned i = 0; i < 64; ++i) {
        enum hostwin_exit_reason why;
        (void)fill(hw, (gpa_t)0x100000 + 2 * i * PAGE, 8, false, &why);
    }
    _exit(3);
}

static void test_panics(void)
{
    int s = run_child(map_error_body, child_out, sizeof(child_out));
    CHECK(CHILD_EXITED(s, 1) && strstr(child_out, "[panic] hostwin: backend map failed") &&
          strstr(child_out, "VMA bound"));
    s = run_child(slot_error_body, child_out, sizeof(child_out));
    CHECK(CHILD_EXITED(s, 1) &&
          strstr(child_out, "[panic] hostwin: backend unmap of a whole slot failed"));
    s = run_child(enomem_body, child_out, sizeof(child_out));
    if (!CHILD_EXITED(s, 1) || !strstr(child_out, "[panic] hostwin: backend map failed"))
        fprintf(stderr, "hostwin_test: enomem child status %#x:\n%s", (unsigned)s, child_out);
    CHECK(CHILD_EXITED(s, 1) && strstr(child_out, "hostwin_linux: protect_ro at") &&
          strstr(child_out, "hostwin_linux: map at") && strstr(child_out, "(ENOMEM)") &&
          strstr(child_out, "[panic] hostwin: backend map failed"));
    ++tests;
}

int main(void)
{
    log_set_level(LOG_ERROR);
    CHECK(sigaction(SIGSEGV, NULL, &initial) == 0);
    wb.real = hostwin_linux_backend();
    CHECK(hostwin_linux_init() && hostwin_linux_init());
    hostwin_set_trap_client(client);
    v = calloc(1, sizeof(*v));
    CHECK(v != NULL);
    setup_memory();
    test_bind_lru_epoch();
    test_observed_epoch();
    test_accessor_warm();
    test_offsets();
    test_physical();
    test_fill_order();
    test_permissions();
    test_lasting_rights();
    test_eligibility();
    test_entry_diff();
    test_entry_sources();
    test_walker_insertion();
    test_code_events();
    test_contexts();
    test_validity_off();
    test_inject();
    test_vma_accounting();
    test_backend_failures();
    test_chaining();
    test_panics();
    CHECK(xl.probe_refs == 0);
    printf("hostwin_test: %u tests, %u checks passed\n", tests, checks);
    return 0;
}
