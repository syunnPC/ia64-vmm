/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * build/tier1-hostwin-region-test: host-window integration with Tier1's trap
 * client. The mock uses struct t1_exec; static assertions pin assembly offsets.
 * Each iteration loads two words and stores one, with patterned GPRs and a
 * known CF to verify capture and restoration.
 *
 * The signal-safe client saves registers and flags, then redirects to the
 * resolver trampoline. Real faults resume at the access; forced faults resume
 * at the tag check. TRAP_EXIT must leave before effect. Retry restores every
 * GPR except RSP/R14, including scratch R11, and restores RFLAGS.
 * Tests cover forced faults, real cold fills, >=9 tags over 8 shadow slots,
 * interleaved instances and teardown under the default vm.max_map_count.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tier1/hostwin_linux.h"
#include "engine/tier1/exec.h"
#include "ia64vmm/guest_mem.h"
#include "core/log_internal.h"

#define PAGE 4096u
#define RAM_BYTES (UINT64_C(64) << 20)
#define W 20u /* the mock's window width (the shr immediate below) */
#define NSITES 3u
#define EXIT_POINT 3u

static unsigned checks, tests;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            fprintf(stderr, "hostwin_region_test: %s:%d: check failed: %s\n", __FILE__, __LINE__,  \
                    #c);                                                                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

/* ---- the struct t1_exec offsets the assembly hard-codes ----------------- */

#define SPILL_START 0  /* x->spill[]: start index */
#define SPILL_SUM 1    /* start sum */
#define SPILL_RSP 4    /* body RSP, stashed at entry */
#define SPILL_FLAGS 5  /* 5..7: RFLAGS seen at check_k */
#define SPILL_ARRAY 10 /* 10..12: guest-address arrays of sites 0..2 */
#define SPILL_COUNT 13 /* end index (exclusive) */
_Static_assert(offsetof(struct t1_exec, regs) == 0 && offsetof(struct t1_exec, rflags) == 128 &&
                   offsetof(struct t1_exec, rip) == 136 && offsetof(struct t1_exec, point) == 152 &&
                   offsetof(struct t1_exec, poison) == 176,
               "mock region: t1_exec header layout");
_Static_assert(X_SITE_TAG(0) == 184 && X_SITE_DELTA(0) == 192 && sizeof(struct hostwin_tag) == 16,
               "mock region: x->site[] layout");
_Static_assert(X_SPILL(0) == 8376 && X_FORCE(0) == 10424, "mock region: spill and force layout");

/* GPR patterns of rcx, rdx, rdi, r8, r9, r10 at region entry; each grows
 * by 1 per iteration, so a register the trap path failed to capture (or
 * restored from an older save) cannot hold the expected value. */
#define P_RCX UINT64_C(0x1c1c1c1c1c1c1c1c)
#define P_RDX UINT64_C(0xd2d2d2d2d2d2d2d2)
#define P_RDI UINT64_C(0x7d17d17d17d17d17)
#define P_R8 UINT64_C(0x0808080808080808)
#define P_R9 UINT64_C(0x9999999999999999)
#define P_R10 UINT64_C(0xa1a1a1a1a1a1a1a1)
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

uint32_t mock_region(struct vcpu *v, uint64_t budget, struct t1_exec *x);
uint32_t mock_window_miss(struct t1_exec *x);
uint32_t mock_trap_resolve(struct t1_exec *x);
void mock_trap_entry(void);
extern char mock_access0[], mock_access1[], mock_access2[];
extern char mock_check0[], mock_check1[], mock_check2[];
extern char mock_force_at0[], mock_force_at1[], mock_force_at2[];

#define S_(x) #x
#define S(x) S_(x)
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".macro SAVE_ALL\n"
    "  mov [r14 + 0], rax\n  mov [r14 + 8], rcx\n  mov [r14 + 16], rdx\n  mov [r14 + 24], rbx\n"
    "  mov [r14 + 40], rbp\n  mov [r14 + 48], rsi\n  mov [r14 + 56], rdi\n  mov [r14 + 64], r8\n"
    "  mov [r14 + 72], r9\n   mov [r14 + 80], r10\n  mov [r14 + 88], r11\n  mov [r14 + 96], r12\n"
    "  mov [r14 + 104], r13\n mov [r14 + 120], r15\n"
    ".endm\n"
    ".macro RESTORE_ALL\n"
    "  mov rax, [r14 + 0]\n  mov rcx, [r14 + 8]\n  mov rdx, [r14 + 16]\n  mov rbx, [r14 + 24]\n"
    "  mov rbp, [r14 + 40]\n  mov rsi, [r14 + 48]\n  mov rdi, [r14 + 56]\n  mov r8, [r14 + 64]\n"
    "  mov r9, [r14 + 72]\n   mov r10, [r14 + 80]\n  mov r11, [r14 + 88]\n  mov r12, [r14 + 96]\n"
    "  mov r13, [r14 + 104]\n mov r15, [r14 + 120]\n"
    ".endm\n"
    /* One memory site with the mock's flag observations. */
    ".macro SITE k, store\n"
    "  stc\n"
    "  dec qword ptr [r14 + 10424 + 8 * \\k]\n"
    "  jz mock_force\\k\n"
    ".globl mock_check\\k\n"
    "mock_check\\k:\n"
    "  pushfq\n"
    "  pop qword ptr [r14 + 8376 + 8 * (5 + \\k)]\n"
    "  mov r11, rsi\n"
    "  shr r11, " S(
        W) "\n"
           "  cmp r11, qword ptr [r14 + 184 + 16 * \\k]\n"
           "  jne mock_miss\\k\n"
           "  test rsi, 7\n"
           "  jnz mock_miss\\k\n"
           "  mov r11, qword ptr [r14 + 192 + 16 * \\k]\n"
           ".globl mock_access\\k\n"
           "mock_access\\k:\n"
           "  .if \\store\n"
           "  mov qword ptr [rsi + r11], rbp\n"
           "  .else\n"
           "  mov rax, qword ptr [rsi + r11]\n"
           "  .endif\n"
           ".endm\n"
           ".macro STUBS k\n"
           "mock_miss\\k:\n"
           "  mov dword ptr [r14 + 152], \\k\n"
           "  SAVE_ALL\n"
           "  mov rdi, r14\n"
           "  call mock_window_miss\n"
           "  test eax, eax\n"
           "  jnz mock_exit_saved\n"
           "  RESTORE_ALL\n"
           "  jmp mock_check\\k\n"
           "mock_force\\k:\n"
           "  mov r11, qword ptr [r14 + 176]\n"
           ".globl mock_force_at\\k\n"
           "mock_force_at\\k:\n"
           "  mov r11, qword ptr [r11]\n"
           "  ud2\n"
           ".endm\n"
           /* uint32_t mock_region(struct vcpu *v, uint64_t budget, struct t1_exec *x):
     * the entry ABI; RSP = 0 mod 16 in the body, no pushes there. */
           ".globl mock_region\n"
           "mock_region:\n"
           "  push rbx\n  push rbp\n  push r12\n  push r13\n  push r14\n  push r15\n"
           "  sub rsp, 8\n"
           "  mov r12, rdi\n  mov r13, rsi\n  mov r14, rdx\n"
           "  mov qword ptr [r14 + 8376 + 8 * 4], rsp\n"
           "  mov rbx, qword ptr [r14 + 8376 + 8 * 0]\n"
           "  mov rbp, qword ptr [r14 + 8376 + 8 * 1]\n"
           "  mov r15, qword ptr [r14 + 8376 + 8 * 13]\n"
           "  movabs rcx, 0x1c1c1c1c1c1c1c1c\n  movabs rdx, 0xd2d2d2d2d2d2d2d2\n"
           "  movabs rdi, 0x7d17d17d17d17d17\n  movabs r8, 0x0808080808080808\n"
           "  movabs r9, 0x9999999999999999\n  movabs r10, 0xa1a1a1a1a1a1a1a1\n"
           "  xor eax, eax\n  xor esi, esi\n"
           "mock_loop:\n"
           "  cmp rbx, r15\n"
           "  jae mock_done\n"
           "  mov r11, qword ptr [r14 + 8376 + 8 * 10]\n"
           "  mov rsi, qword ptr [r11 + rbx * 8]\n"
           "  SITE 0, 0\n"
           "  add rbp, rax\n"
           "  mov r11, qword ptr [r14 + 8376 + 8 * 11]\n"
           "  mov rsi, qword ptr [r11 + rbx * 8]\n"
           "  SITE 1, 0\n"
           "  add rbp, rax\n"
           "  mov r11, qword ptr [r14 + 8376 + 8 * 12]\n"
           "  mov rsi, qword ptr [r11 + rbx * 8]\n"
           "  SITE 2, 1\n"
           "  inc rbx\n"
           "  sub r13, 3\n"
           "  add rcx, 1\n  add rdx, 1\n  add rdi, 1\n  add r8, 1\n  add r9, 1\n  add r10, 1\n"
           "  jmp mock_loop\n"
           "mock_done:\n"
           "  mov dword ptr [r14 + 152], 3\n"
           "  SAVE_ALL\n"
           "mock_exit_saved:\n"
           "  add rsp, 8\n"
           "  pop r15\n  pop r14\n  pop r13\n  pop r12\n  pop rbp\n  pop rbx\n"
           "  mov eax, 1\n"
           "  ret\n"
           /* Normal context after the signal. */
           ".globl mock_trap_entry\n"
           "mock_trap_entry:\n"
           "  mov rdi, r14\n"
           "  call mock_trap_resolve\n"
           "  test eax, eax\n"
           "  jnz mock_exit_saved\n"
           "  RESTORE_ALL\n"
           "  push qword ptr [r14 + 128]\n"
           "  popfq\n"
           "  jmp qword ptr [r14 + 136]\n"
           "  STUBS 0\n  STUBS 1\n  STUBS 2\n"
           ".att_syntax prefix\n");

/* ---- machine ------------------------------------------------------------ */

static uint8_t *ram;
static struct vcpu *v;

/* Like vcpu_reset: mmu_seq keeps counting and logs IA64_MMU_ALL (the
 * instances run the MMU-log completeness check). */
static void reset_vcpu(void)
{
    uint64_t seq = v->mmu_seq;
    struct vcpu_config cfg = v->cfg;
    memset(v, 0, sizeof(*v));
    v->cfg = cfg;
    v->mmu_seq = seq;
    v->s.version = IA64_STATE_VERSION;
    v->s.cpu_model = v->cfg.cpu_model;
    v->s.psr = IA64_PSR_IC; /* physical data accesses: tags are pa >> W */
    ia64_mmu_changed(v, IA64_MMU_ALL, 0, 0);
}

static uint64_t word_at(gpa_t pa)
{
    uint64_t x;
    memcpy(&x, ram + pa, 8);
    return x;
}
static void set_word(gpa_t pa, uint64_t x)
{
    memcpy(ram + pa, &x, 8);
}
static uint64_t pattern(gpa_t pa)
{
    return pa * UINT64_C(0x9e3779b97f4a7c15) ^ UINT64_C(0x5bd1e995);
}

/* ---- one mock vCPU: x, its instance, its program -------------------------- */

enum force_action { ACT_RETRY, ACT_EXIT };
struct mock {
    struct t1_exec x;
    struct hostwin_vcpu *hw;
    uint64_t *gva[NSITES]; /* per-site guest addresses, by iteration */
    uint64_t count;
    uint64_t budget_entry;
    /* Armed forced trap (one at a time). */
    bool armed;
    unsigned arm_site;
    uint64_t arm_occurrence;
    enum force_action action;
    /* Observations. */
    uint64_t misses, traps, forced, exits, bad;
    uint64_t last_miss_key, last_trap_key; /* (iteration, site) of the last miss/trap, +1 */
    uint64_t trap_fault_va, trap_write;
    bool force_state_ok;
    /* Model LRU by hostwin_bind order; native hits do not update recency.
     * New tags take the first free slot or evict the least recently bound;
     * only eviction moves bind_epoch. */
    unsigned slots;
    uint64_t model_tag[HOSTWIN_MAX_SLOTS], model_stamp[HOSTWIN_MAX_SLOTS], model_clock;
    bool model_used[HOSTWIN_MAX_SLOTS];
    uint64_t model_binds, model_evictions, model_mismatch;
};
static struct mock *volatile current; /* the executing mock: Tier1's t1_current */

/* RIP table: sorted, searched by binary search in the
 * signal handler; built once in normal context. */
struct rip_entry {
    uint64_t rip;
    unsigned site;
    bool forced;
};
static struct rip_entry rip_table[2 * NSITES];
static unsigned rip_count;
static int rip_cmp(const void *a, const void *b)
{
    const struct rip_entry *x = a, *y = b;
    return x->rip < y->rip ? -1 : x->rip > y->rip;
}
static void build_rip_table(void)
{
    char *access[NSITES] = {mock_access0, mock_access1, mock_access2};
    char *forced[NSITES] = {mock_force_at0, mock_force_at1, mock_force_at2};
    for (unsigned k = 0; k < NSITES; ++k) {
        rip_table[rip_count++] = (struct rip_entry){(uint64_t)(uintptr_t)access[k], k, false};
        rip_table[rip_count++] = (struct rip_entry){(uint64_t)(uintptr_t)forced[k], k, true};
    }
    qsort(rip_table, rip_count, sizeof(rip_table[0]), rip_cmp);
}
static const struct rip_entry *rip_lookup(uint64_t rip)
{
    unsigned lo = 0, hi = rip_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (rip_table[mid].rip == rip)
            return &rip_table[mid];
        if (rip_table[mid].rip < rip)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}
static uint64_t check_rip(unsigned k)
{
    char *check[NSITES] = {mock_check0, mock_check1, mock_check2};
    return (uint64_t)(uintptr_t)check[k];
}

/* Async-signal-safe; reads the table, writes x and m only. */
static bool mock_client(struct hostwin_trap *t)
{
    struct mock *m = current;
    if (!m)
        return false;
    const struct rip_entry *e = rip_lookup(t->rip);
    if (!e)
        return false;
    struct t1_exec *x = &m->x;
    for (unsigned i = 0; i < 16; ++i)
        x->regs[i] = t->gpr[i];
    x->rflags = t->rflags;
    x->point = e->site;
    x->rip = e->forced ? check_rip(e->site) : t->rip;
    m->trap_fault_va = t->fault_va;
    m->trap_write = t->write;
    if (e->forced)
        ++m->forced;
    t->rip = (uint64_t)(uintptr_t)mock_trap_entry;
    return true;
}

/* The reference: the program run sequentially in C on guest RAM (loads
 * read RAM, the store writes the running sum). Returns the final sum;
 * optionally the sum and last load before (iteration i, site k). */
struct ref_point {
    uint64_t sum, rax;
};
static uint64_t reference(const struct mock *m, uint64_t start, uint64_t sum, bool write,
                          uint64_t at_i, unsigned at_k, struct ref_point *at)
{
    uint64_t rax = 0;
    for (uint64_t i = start; i < m->count; ++i)
        for (unsigned k = 0; k < NSITES; ++k) {
            if (at && i == at_i && k == at_k)
                *at = (struct ref_point){sum, rax};
            if (k < 2) {
                rax = word_at(m->gva[k][i]);
                sum += rax;
            } else if (write) {
                set_word(m->gva[k][i], sum);
            }
        }
    return sum;
}

/* Checks the full register state captured at a forced trap of site k in
 * iteration i. */
static bool forced_state_ok(struct mock *m, unsigned k)
{
    const struct t1_exec *x = &m->x;
    uint64_t i = x->regs[RBX];
    struct ref_point at = {0, 0};
    (void)reference(m, x->spill[SPILL_START], x->spill[SPILL_SUM], false, i, k, &at);
    bool ok = true;
    ok &= x->regs[RAX] == at.rax && x->regs[RBP] == at.sum;
    uint64_t d = i - x->spill[SPILL_START];
    ok &= x->regs[RCX] == P_RCX + d && x->regs[RDX] == P_RDX + d && x->regs[RDI] == P_RDI + d;
    ok &= x->regs[R8] == P_R8 + d && x->regs[R9] == P_R9 + d && x->regs[R10] == P_R10 + d;
    ok &= x->regs[RSI] == m->gva[k][i] && x->regs[R11] == x->poison;
    ok &= x->regs[R12] == (uint64_t)(uintptr_t)v && x->regs[R14] == (uint64_t)(uintptr_t)x;
    ok &= x->regs[R13] == m->budget_entry - 3 * (i - x->spill[SPILL_START]);
    ok &= x->regs[R15] == m->count && x->regs[RSP] == x->spill[SPILL_RSP];
    /* stc ; dec 1 -> 0: CF from stc, ZF and PF from the zero result, SF,
     * OF and AF clear; DF and TF clear as always. */
    ok &= (x->rflags & UINT64_C(0xdd5)) == 0x45 && !(x->rflags & 0x100);
    ok &= m->trap_fault_va == x->poison && !m->trap_write;
    ok &= i + 1 - x->spill[SPILL_START] == m->arm_occurrence && k == m->arm_site;
    return ok;
}

/* hostwin_bind, checked against the LRU model: the slot hostwin chose (from
 * the returned delta) must be the model's, the epoch must move exactly on
 * an eviction. */
static enum hostwin_status model_bind(struct mock *m, uint64_t gva, struct hostwin_tag *out)
{
    uint64_t e0 = hostwin_bind_epoch(m->hw);
    enum hostwin_status st = hostwin_bind(m->hw, v, gva, out);
    if (st != HOSTWIN_OK)
        return st;
    uintptr_t first = hostwin_poison_va(m->hw) - ((uintptr_t)m->slots << W);
    uint64_t slot = (out->delta + (out->tag << W) - first) >> W;
    unsigned want = HOSTWIN_MAX_SLOTS;
    bool evict = false;
    for (unsigned i = 0; i < m->slots; ++i)
        if (m->model_used[i] && m->model_tag[i] == out->tag)
            want = i;
    if (want == HOSTWIN_MAX_SLOTS) {
        for (unsigned i = 0; i < m->slots && want == HOSTWIN_MAX_SLOTS; ++i)
            if (!m->model_used[i])
                want = i;
        if (want == HOSTWIN_MAX_SLOTS) {
            want = 0;
            for (unsigned i = 1; i < m->slots; ++i)
                if (m->model_stamp[i] < m->model_stamp[want])
                    want = i;
            evict = true;
            ++m->model_evictions;
        }
        m->model_used[want] = true;
        m->model_tag[want] = out->tag;
        ++m->model_binds;
    }
    m->model_stamp[want] = ++m->model_clock;
    if (slot != want || hostwin_bind_epoch(m->hw) - e0 != (uint64_t)evict)
        ++m->model_mismatch;
    return st;
}

uint32_t mock_trap_resolve(struct t1_exec *x)
{
    struct mock *m = current;
    unsigned k = x->point;
    if (x->rip == check_rip(k)) {
        /* Forced: no hostwin_resolve, no translation. */
        m->force_state_ok = forced_state_ok(m, k);
        return m->action == ACT_RETRY ? T1_RESUME_RETRY : T1_RESUME_EXIT;
    }
    ++m->traps;
    uint64_t key = (x->regs[RBX] * NSITES + k) + 1;
    if (key == m->last_trap_key)
        ++m->bad; /* two real traps for one access */
    m->last_trap_key = key;
    uint64_t gva = x->regs[RSI];
    bool write = k == 2;
    /* A real fault reports the host address of the access, never the gva. */
    if (m->trap_fault_va != gva + x->regs[R11] || m->trap_write != write)
        ++m->bad;
    enum hostwin_exit_reason why;
    if (hostwin_resolve(m->hw, v, gva, 8, write, &why) == HOSTWIN_MAPPED)
        return T1_RESUME_RETRY;
    ++m->exits;
    return T1_RESUME_EXIT;
}

/* The window-miss protocol, plus the invariant: whenever the observed epoch equals
 * bind_epoch, every cached site tag is still bound with its delta. */
uint32_t mock_window_miss(struct t1_exec *x)
{
    struct mock *m = current;
    unsigned k = x->point;
    uint64_t gva = x->regs[RSI];
    ++m->misses;
    uint64_t key = (x->regs[RBX] * NSITES + k) + 1;
    if (key == m->last_miss_key)
        ++m->bad; /* the retry after RETRY missed again */
    m->last_miss_key = key;
    if (gva & 7)
        return T1_RESUME_EXIT;
    struct hostwin_tag tmp = {0, 0};
    if (model_bind(m, gva, &tmp) == HOSTWIN_SLOW)
        return T1_RESUME_EXIT;
    uint64_t e = hostwin_bind_epoch(m->hw);
    if (e != x->observed_epoch)
        for (unsigned i = 0; i < T1_SITE_SLOTS; ++i)
            x->site[i].tag = HOSTWIN_TAG_INVALID;
    x->observed_epoch = e;
    x->site[k] = tmp; /* publish the current site last */
    /* Invariant probe (re-binding a bound tag only refreshes its LRU age;
     * it never rebinds, so it perturbs replacement order, not validity). */
    for (unsigned j = 0; j < NSITES; ++j) {
        if (x->site[j].tag == HOSTWIN_TAG_INVALID)
            continue;
        struct hostwin_tag t;
        if (model_bind(m, x->site[j].tag << W, &t) != HOSTWIN_OK ||
            hostwin_bind_epoch(m->hw) != e || t.delta != x->site[j].delta)
            ++m->bad;
    }
    return T1_RESUME_RETRY;
}

/* The dispatcher: region entry, and the lower tier for exits.
 * An exit at site k of iteration i is before effect: C performs sites
 * k..2 of that iteration (what the lower tier would execute until the
 * loop header) and re-enters at i + 1. */
static uint64_t run(struct mock *m, uint64_t start, uint64_t sum)
{
    struct t1_exec *x = &m->x;
    for (;;) {
        for (unsigned i = 0; i < T1_SITE_SLOTS; ++i)
            x->site[i].tag = HOSTWIN_TAG_INVALID;
        x->observed_epoch = hostwin_bind_epoch(m->hw);
        x->poison = hostwin_poison_va(m->hw);
        x->spill[SPILL_START] = start;
        x->spill[SPILL_SUM] = sum;
        x->spill[SPILL_COUNT] = m->count;
        for (unsigned k = 0; k < NSITES; ++k) {
            x->spill[SPILL_ARRAY + k] = (uint64_t)(uintptr_t)m->gva[k];
            x->force[k] = m->armed && m->arm_site == k ? m->arm_occurrence : T1_FORCE_UNARMED;
        }
        m->budget_entry = UINT64_C(1) << 40;
        for (unsigned i = 0; i < 16; ++i)
            x->regs[i] = UINT64_C(0xdeadbeef00000000) | i;
        hostwin_enter(m->hw, v);
        current = m;
        uint32_t ret = mock_region(v, m->budget_entry, x);
        current = NULL;
        CHECK(ret == 1);
        if (x->point == EXIT_POINT) {
            uint64_t d = m->count - start;
            CHECK(x->regs[RBX] == m->count && x->regs[RCX] == P_RCX + d &&
                  x->regs[R10] == P_R10 + d && x->regs[R8] == P_R8 + d &&
                  x->regs[R13] == m->budget_entry - 3 * d);
            return x->regs[RBP];
        }
        /* Exit before effect at site k. */
        ++m->exits;
        unsigned k = x->point;
        uint64_t i = x->regs[RBX];
        sum = x->regs[RBP];
        for (; k < NSITES; ++k) {
            if (k < 2)
                sum += word_at(m->gva[k][i]);
            else
                set_word(m->gva[k][i], sum);
        }
        start = i + 1;
        m->armed = false;
        if (start == m->count)
            return sum;
    }
}

static struct hostwin_vcpu *make(unsigned w, unsigned slots);
static bool make_mock(struct mock *m, unsigned slots)
{
    m->slots = slots;
    m->hw = make(W, slots);
    return m->hw != NULL;
}
static struct hostwin_vcpu *make(unsigned w, unsigned slots)
{
    struct hostwin_config c = {.w = w, .slots = slots, .check = true};
    return hostwin_create(&c, hostwin_linux_backend(), hostwin_linux_ctx());
}
static struct hostwin_stats stats(const struct hostwin_vcpu *hw)
{
    struct hostwin_stats s;
    hostwin_get_stats(hw, &s);
    return s;
}
static uint64_t exits_total(const struct hostwin_vcpu *hw)
{
    struct hostwin_stats s = stats(hw);
    uint64_t n = 0;
    for (unsigned i = 0; i <= HW_EXIT_STALE_SLOT; ++i)
        n += s.exits[i];
    return n;
}

/* Program layouts. Loads read [tag << W | offset] with offsets below 512
 * KiB (patterned words); the store writes a distinct word per iteration
 * above 512 KiB. */
static uint64_t *alloc_array(uint64_t n)
{
    uint64_t *a = calloc(n, 8);
    CHECK(a != NULL);
    return a;
}
/* pointer_list: compiler's window-tags shape (unit_runtime.c): site 0 reads a
 * pointer list in one tag, site 1 cycles through ntags others, and site 2
 * stores into the pointer list's tag (ntags + 1 tags in all). */
static void layout_shape(struct mock *m, uint64_t count, unsigned ntags, unsigned first_tag,
                         bool cycle, bool pointer_list)
{
    m->count = count;
    for (unsigned k = 0; k < NSITES; ++k)
        m->gva[k] = alloc_array(count);
    for (uint64_t i = 0; i < count; ++i)
        for (unsigned k = 0; k < NSITES; ++k) {
            unsigned tag =
                pointer_list
                    ? (k == 1 ? first_tag + 1 + (unsigned)(i % ntags) : first_tag)
                    : first_tag + (cycle ? (unsigned)((i * NSITES + k) % ntags) : k % ntags);
            uint64_t base = (uint64_t)tag << W;
            uint64_t a = k < 2 ? base + ((i * 40 + k * 8) % (512 << 10) & ~UINT64_C(7))
                               : base + (512 << 10) + 8 * i;
            m->gva[k][i] = a;
            if (k < 2)
                set_word(a, pattern(a));
        }
}
static void layout(struct mock *m, uint64_t count, unsigned ntags, unsigned first_tag, bool cycle)
{
    layout_shape(m, count, ntags, first_tag, cycle, false);
}
static void free_layout(struct mock *m)
{
    for (unsigned k = 0; k < NSITES; ++k)
        free(m->gva[k]);
}
/* Every iteration stored the running sum after its two loads. */
static bool stores_ok(const struct mock *m)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < m->count; ++i) {
        sum += word_at(m->gva[0][i]) + word_at(m->gva[1][i]);
        if (word_at(m->gva[2][i]) != sum)
            return false;
    }
    return true;
}

/* ---- tests -------------------------------------------------------------- */

/* forced-exit at the hostwin level: a forced load from the poison page traps
 * through the real SIGSEGV path with every GPR and the flags as at the
 * site, and TRAP_RETRY resumes exactly at check_k (one trap per armed
 * occurrence, flags restored, nothing executed twice); TRAP_EXIT leaves
 * before effect and the lower tier finishes with the same results. */
static void test_forced_traps(void)
{
    reset_vcpu();
    for (unsigned k = 0; k < NSITES; ++k)
        for (int action = ACT_RETRY; action <= ACT_EXIT; ++action) {
            struct mock *m = calloc(1, sizeof(*m));
            CHECK(m != NULL);
            CHECK(make_mock(m, 8));
            layout(m, 6, 3, 8, false);
            uint64_t ref = reference(m, 0, 0, true, 0, 0, NULL);
            uint64_t warm = run(m, 0, 0); /* warm: shadows filled */
            CHECK(warm == ref);
            uint64_t traps = m->traps;
            CHECK(traps == NSITES); /* one cold fill per page */
            m->armed = true;
            m->arm_site = k;
            m->arm_occurrence = m->count; /* the last execution of site k */
            m->action = (enum force_action)action;
            m->force_state_ok = false;
            uint64_t sum = run(m, 0, 0);
            CHECK(sum == ref && m->forced == 1 && m->force_state_ok);
            CHECK(m->traps == traps && m->bad == 0 && exits_total(m->hw) == 0 &&
                  m->model_mismatch == 0);
            CHECK(stores_ok(m));
            if (action == ACT_RETRY) {
                /* The resume landed after the force counter (it reached 0
                 * once and was not decremented again), with the captured
                 * flags (ZF from the counter, CF from stc). */
                CHECK(m->x.force[k] == 0 && m->exits == 0);
                CHECK((m->x.spill[SPILL_FLAGS + k] & 0xdd5) == 0x45);
            } else {
                CHECK(m->exits == 1);
            }
            hostwin_destroy(m->hw);
            free_layout(m);
            free(m);
        }
    ++tests;
}

/* window-tags at the hostwin level: 12 tags cycling through 8
 * slots, three sites per iteration, all inside one region execution: every
 * access completes in the region (no exit, never STALE_SLOT), each access
 * misses at most once and traps at most once, and the cached site tags
 * stay valid whenever the observed epoch matches. */
static void test_tags(void)
{
    /* {tags, slots, compiler's pointer-list shape} */
    static const unsigned shapes[][3] = {{9, 8, 0}, {12, 8, 0}, {16, 8, 0}, {12, 4, 0}, {9, 8, 1}};
    reset_vcpu();
    for (unsigned s = 0; s < sizeof(shapes) / sizeof(shapes[0]); ++s) {
        unsigned ntags = shapes[s][0], slots = shapes[s][1];
        bool plist = shapes[s][2];
        struct mock *m = calloc(1, sizeof(*m));
        CHECK(m != NULL);
        CHECK(make_mock(m, slots));
        layout_shape(m, plist ? 36 : 400, ntags, 4, true, plist);
        uint64_t ref = reference(m, 0, 0, false, 0, 0, NULL);
        uint64_t sum = run(m, 0, 0);
        CHECK(sum == ref && m->exits == 0 && m->bad == 0);
        struct hostwin_stats st = stats(m->hw);
        CHECK(exits_total(m->hw) == 0 && st.exits[HW_EXIT_STALE_SLOT] == 0);
        uint64_t accesses = m->count * NSITES;
        CHECK(m->misses <= accesses && m->traps <= accesses);
        /* The victim policy: every bind took the slot the LRU model
         * names and moved the epoch exactly on evictions. */
        CHECK(m->model_mismatch == 0 && st.binds == m->model_binds &&
              st.rebinds == m->model_evictions);
        /* Cyclic access over more tags than slots: LRU rebinds every time. */
        if (!plist)
            CHECK(st.rebinds >= accesses - slots - NSITES);
        CHECK(stores_ok(m));
        printf(
            "hostwin_region_test: window-tags %u%s tags / %u slots: %llu accesses, %llu misses, %llu cold-fill "
            "traps, %llu binds, %llu rebinds (= the LRU model), 0 exits\n",
            ntags + plist, plist ? " (pointer-list shape)" : "", slots,
            (unsigned long long)accesses, (unsigned long long)m->misses,
            (unsigned long long)m->traps, (unsigned long long)st.binds,
            (unsigned long long)st.rebinds);
        hostwin_destroy(m->hw);
        free_layout(m);
        free(m);
    }
    ++tests;
}

/* Two instances (two vCPUs) interleaved: the client selects x through the
 * current-executor pointer (hostwin_trap carries no instance), each
 * instance's poison page traps for its own region, and neither disturbs
 * the other's bindings or shadows. */
static void test_two_instances(void)
{
    reset_vcpu();
    struct mock *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
    CHECK(a && b);
    CHECK(make_mock(a, 8) && make_mock(b, 8));
    CHECK(hostwin_poison_va(a->hw) != hostwin_poison_va(b->hw));
    CHECK(hostwin_owns(hostwin_poison_va(a->hw)) && hostwin_owns(hostwin_poison_va(b->hw)));
    layout(a, 120, 10, 4, true);
    layout(b, 120, 10, 20, true);
    uint64_t ra = reference(a, 0, 0, false, 0, 0, NULL), rb = reference(b, 0, 0, false, 0, 0, NULL);
    uint64_t sa = 0, sb = 0, ca = a->count, cb = b->count;
    for (uint64_t at = 0; at < ca; at += 15) {
        a->count = at + 15 < ca ? at + 15 : ca;
        b->count = at + 15 < cb ? at + 15 : cb;
        if (at == 45) { /* a forced retry in each, mid-way */
            a->armed = b->armed = true;
            a->arm_site = 1;
            b->arm_site = 2;
            a->arm_occurrence = b->arm_occurrence = 7;
            a->action = b->action = ACT_RETRY;
        }
        sa = run(a, at, sa);
        sb = run(b, at, sb);
        a->armed = b->armed = false;
    }
    a->count = ca;
    b->count = cb;
    CHECK(sa == ra && sb == rb && a->forced == 1 && b->forced == 1);
    CHECK(a->bad == 0 && b->bad == 0 && a->exits == 0 && b->exits == 0);
    CHECK(a->model_mismatch == 0 && b->model_mismatch == 0);
    CHECK(exits_total(a->hw) == 0 && exits_total(b->hw) == 0);
    CHECK(stores_ok(a) && stores_ok(b));
    uintptr_t poison_a = hostwin_poison_va(a->hw);
    hostwin_destroy(a->hw);
    /* The survivor still runs; the destroyed instance owns nothing. */
    CHECK(!hostwin_owns(poison_a) && hostwin_owns(hostwin_poison_va(b->hw)));
    CHECK(run(b, 0, 0) == rb && b->bad == 0);
    hostwin_destroy(b->hw);
    free_layout(a);
    free_layout(b);
    free(a);
    free(b);
    ++tests;
}

/* Multi-instance teardown at the default vm.max_map_count (65530; the
 * admission is computed for it even where the host limit is higher, and
 * the measured VMAs must stay below it). */
static uint64_t process_vmas(void)
{
    struct hostwin_linux_stats s;
    hostwin_linux_get_stats(&s);
    return s.process_vmas;
}
static uint64_t reservation_vmas(void)
{
    struct hostwin_linux_stats s;
    hostwin_linux_get_stats(&s);
    return s.reservation_vmas;
}
static void fill_isolated(struct hostwin_vcpu *hw, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        gpa_t pa = (gpa_t)(2 * i + 1) * PAGE;
        struct hostwin_tag t;
        enum hostwin_exit_reason why;
        CHECK(hostwin_bind(hw, v, pa, &t) == HOSTWIN_OK);
        CHECK(hostwin_resolve(hw, v, pa, 8, (i & 1) != 0, &why) == HOSTWIN_MAPPED);
    }
}
static void test_teardown_default_limit(void)
{
    enum { DEFAULT_LIMIT = 65530 };
    reset_vcpu();
    hostwin_linux_set_map_limit(DEFAULT_LIMIT);
    CHECK(hostwin_linux_map_limit() <= DEFAULT_LIMIT);
    uint64_t base_vmas = process_vmas(), need = hostwin_linux_vmas_per_reservation();
    uint64_t expect = (DEFAULT_LIMIT - base_vmas - hostwin_linux_headroom()) / need;
    struct hostwin_vcpu *hw[8] = {0};
    unsigned n = 0;
    printf(
        "hostwin_region_test: expected below: two \"reservation refused\" lines (admission at %d)\n",
        DEFAULT_LIMIT);
    fflush(stdout);
    while (n < 8 && (hw[n] = make(36, 8)) != NULL)
        ++n;
    CHECK(n == expect && n >= 1);
    /* Each at its cap of isolated shadows: the worst case the admission
     * provided for, measured. */
    for (unsigned i = 0; i < n; ++i)
        fill_isolated(hw[i], HOSTWIN_MAX_SHADOWS);
    uint64_t peak = process_vmas();
    CHECK(reservation_vmas() == (uint64_t)n * need && peak <= DEFAULT_LIMIT);
    for (unsigned i = 0; i < n; ++i)
        CHECK(stats(hw[i]).live_mappings == HOSTWIN_MAX_SHADOWS);
    CHECK(make(36, 8) == NULL); /* refused, nothing leaked */
    uintptr_t poison0 = hostwin_poison_va(hw[0]),
              base0 = hostwin_poison_va(hw[0]) - ((uintptr_t)8 << 36);
    /* Interleaved teardown: destroy the first, code events reach only the
     * survivors, then the rest in reverse order. */
    hostwin_destroy(hw[0]);
    CHECK(!hostwin_owns(poison0) && !hostwin_owns(base0));
    uint64_t events[8];
    for (unsigned i = 1; i < n; ++i)
        events[i] = stats(hw[i]).code_events;
    hostwin_code_page_event(PAGE, CODE_PAGE_TRACKED);
    hostwin_code_page_event(PAGE, CODE_PAGE_UNTRACKED);
    for (unsigned i = 1; i < n; ++i)
        CHECK(stats(hw[i]).code_events == events[i] + 2);
    CHECK((hw[0] = make(36, 8)) != NULL); /* the freed admission is reusable */
    fill_isolated(hw[0], 100);
    for (unsigned i = n; i-- > 0;)
        hostwin_destroy(hw[i]);
    CHECK(reservation_vmas() == 0 && process_vmas() <= base_vmas + 16);
    /* Cycles: a registry or reservation leak would stop creation at 64. */
    for (unsigned c = 0; c < 100; ++c) {
        struct hostwin_vcpu *x = make(c & 1 ? 36 : W, 8), *y = make(36, 4);
        CHECK(x != NULL && y != NULL);
        fill_isolated(x, 64 + c);
        fill_isolated(y, 32);
        if (c & 1) {
            hostwin_destroy(x);
            hostwin_destroy(y);
        } else {
            hostwin_destroy(y);
            hostwin_destroy(x);
        }
    }
    CHECK(reservation_vmas() == 0 && process_vmas() <= base_vmas + 16);
    hostwin_linux_set_map_limit(0);
    printf(
        "hostwin_region_test: teardown at vm.max_map_count %d: %u instances admitted "
        "(%llu VMAs each), peak %llu process VMAs, back to %llu after 100 create/fill/destroy cycles\n",
        DEFAULT_LIMIT, n, (unsigned long long)need, (unsigned long long)peak,
        (unsigned long long)process_vmas());
    ++tests;
}

int main(void)
{
    log_set_level(LOG_ERROR);
    v = calloc(1, sizeof(*v));
    CHECK(v != NULL);
    CHECK(hostwin_linux_init());
    build_rip_table();
    hostwin_set_trap_client(mock_client);
    ram = hostwin_linux_ram(0, RAM_BYTES);
    CHECK(ram != NULL);
    struct gm_region r = {
        .base = 0, .size = RAM_BYTES, .kind = GM_RAM, .name = "region-test", .hva = ram};
    CHECK(gm_add_region(&r));
    test_forced_traps();
    test_tags();
    test_two_instances();
    test_teardown_default_limit();
    printf("hostwin_region_test: %u tests, %u checks passed\n", tests, checks);
    return 0;
}
