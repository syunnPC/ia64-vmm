/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
/*
 * Conformance of the generated rule tables (rules.h format 2), driven by tests/tier1/rules/conformance.py.
 *
 * Reads gen.py's tables (build/tier1/isel_rules.h, fold_rules.h), not rule
 * text, and evaluates derived immediates with t1_iexpr_eval. Comparing native
 * execution with conformance.py's sem_ia64 checks the proved rule -> table ->
 * encoder path. Matching must honour ranges/sets, relations, ties, fixed
 * registers, derived immediates/displacements and memory window registers.
 *
 *   tier1-table-runner isel
 *     One case per stdin line:
 *       in:  rule nimm {imm}*nimm nin {value}*nin status mem
 *       out: 0                                    (the table rejects the immediates)
 *            1 label status {out}*nout {in}*nin mem
 *     label: 0 fell through, 1 $T, 2 $F; status: OF SF ZF AF PF CF of
 *     RFLAGS; {in} are the input registers after the template (tied
 *     inputs are clobbered); mem: the memory word a LOAD/STORE rule
 *     addresses through its window (its address input is any 64-bit value:
 *     the window register is set to word - address).
 *
 *   tier1-table-runner fold
 *       in:  rule nimm {imm}*nimm
 *       out: 0 | 1 nres {op result nkids kid0 kid1 kid2 bind has_imm imm}*nres
 *
 *   tier1-table-runner iexpr
 *       in:  op a b        (one rules.h t1_iexpr_op on two values)
 *       out: value
 *
 *   tier1-table-runner fpx
 *     One FP expansion case (build/tier1/fpfast_rules.h, emitted by the
 *     product's walker t1_fpx_emit) per stdin line:
 *       in:  rule sf mf qp fpsr a.sig a.meta b.sig b.meta c.sig c.meta d.sig d.meta
 *       out: 0                       (the rule needs FMA3, which the host lacks)
 *            1 exit sig meta mf      (exit: 0 $BAIL, 1 $DONE; a result
 *                                     slot not written keeps 0xa5a5a5a5a5a5a5a5)
 *
 *   tier1-table-runner selftest
 *     Without z3 (`make test-tier1`, also SAN=1): execute each isel rule with
 *     accepted immediates, instantiate each fold rule, and check sample values.
 *     Run each FP expansion with qp = 0 (destination unchanged).
 *
 * All numbers are hexadecimal (immediates and values as 64-bit two's
 * complement).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sched.h>
#include "build/tier1/isel_rules.h"
#include "build/tier1/fold_rules.h"
#include "engine/tier1/x86enc.h"
#include "tests/tier1/fpx_native.h"

#define STATUS_FLAGS UINT64_C(0x8d5) /* OF SF ZF AF PF CF */
#define CODE_BYTES 4096u
static unsigned native_isa;
static bool supported(const struct t1_rule *r)
{
    for (unsigned t = 0; t < r->ntmpl; ++t)
        if (t1_x86_required_isa(r->tmpl[t].x86op) & ~native_isa)
            return false;
    return true;
}

/* Shared with the generated code: rdi points at it. */
struct image {
    uint64_t gpr[16], rflags, taken, mem;
};
#define IMG_RFLAGS 128
#define IMG_TAKEN 136
typedef void code_fn(struct image *);

/* Registers a rule's binders may get: all but RSP and RDI (the image). */
static const uint8_t pool[] = {T1_RAX, T1_RCX, T1_RDX, T1_RBX, T1_RSI, T1_RBP, T1_R8,
                               T1_R9,  T1_R10, T1_R11, T1_R12, T1_R13, T1_R14, T1_R15};

static uint8_t *code_page;

/* ---- matching: what the table accepts ---- */

/* The pattern's immediate constraints and the rule's relation checks. */
static bool accepts(const struct t1_pat *pat, unsigned npat, const struct t1_iexpr *expr,
                    const struct t1_icheck *check, unsigned ncheck, const int64_t *imm,
                    unsigned nimm)
{
    for (unsigned i = 0; i < npat; ++i) {
        const struct t1_pat *p = &pat[i];
        /* Literal immediates (imm_min == imm_max, no bind) are matched
         * against the IR op's own imm by the matcher; not modelled here. */
        if (p->ir_op == T1_PAT_VALUE || p->bind == T1_BINDER_NONE)
            continue;
        if (p->bind >= nimm)
            return false;
        int64_t v = imm[p->bind];
        if (v < p->imm_min || v > p->imm_max)
            return false;
        if (p->imm_set && (v < 0 || v > 31 || !(p->imm_set >> v & 1)))
            return false;
    }
    for (unsigned i = 0; i < ncheck; ++i) {
        int64_t v = t1_iexpr_eval(expr, check[i].expr, imm);
        if (v < check[i].min || v > check[i].max)
            return false;
    }
    return true;
}

static unsigned imm_binders(const struct t1_pat *pat, unsigned npat)
{
    unsigned n = 0;
    for (unsigned i = 0; i < npat; ++i)
        if (pat[i].ir_op != T1_PAT_VALUE && pat[i].bind != T1_BINDER_NONE && pat[i].bind + 1u > n)
            n = pat[i].bind + 1u;
    return n;
}

static unsigned in_binders(const struct t1_pat *pat, unsigned npat)
{
    unsigned n = 0;
    for (unsigned i = 0; i < npat; ++i)
        if (pat[i].ir_op == T1_PAT_VALUE && pat[i].bind + 1u > n)
            n = pat[i].bind + 1u;
    return n;
}

/* ---- instantiation: registers, immediates, code ---- */

struct inst {
    const struct t1_rule *r;
    const int64_t *imm;
    uint8_t in[32], out[32], temp[32], window;
    unsigned nin;
};

static bool next_free(uint32_t *used, unsigned *next, uint8_t *dst)
{
    while (*next < sizeof(pool) && (*used >> pool[*next] & 1))
        ++*next;
    if (*next == sizeof(pool))
        return false;
    *dst = pool[*next];
    *used |= 1u << pool[*next];
    return true;
}

static bool assign(struct inst *x)
{
    const struct t1_rule *r = x->r;
    uint32_t used = 0;
    memset(x->in, 0xff, sizeof(x->in));
    memset(x->out, 0xff, sizeof(x->out));
    memset(x->temp, 0xff, sizeof(x->temp));
    x->window = 0xff;
    for (unsigned i = 0; i < r->nfixed; ++i) {
        unsigned kind = r->fixed[i].binder >> 5, idx = r->fixed[i].binder & 31;
        uint8_t *slot = kind == T1_OP_IN    ? &x->in[idx]
                        : kind == T1_OP_OUT ? &x->out[idx]
                                            : &x->temp[idx];
        *slot = r->fixed[i].x86reg;
        used |= 1u << r->fixed[i].x86reg;
    }
    unsigned next = 0;
    for (unsigned i = 0; i < x->nin; ++i)
        if (x->in[i] == 0xff && !next_free(&used, &next, &x->in[i]))
            return false;
    for (unsigned i = 0; i < r->ntie; ++i)
        x->out[r->tie[i].out] = x->in[r->tie[i].in];
    for (unsigned i = 0; i < r->nout; ++i)
        if (x->out[i] == 0xff && !next_free(&used, &next, &x->out[i]))
            return false;
    for (unsigned i = 0; i < r->ntemp; ++i)
        if (x->temp[i] == 0xff && !next_free(&used, &next, &x->temp[i]))
            return false;
    return next_free(&used, &next, &x->window);
}

static uint8_t reg_of(const struct inst *x, unsigned kind, unsigned idx)
{
    switch (kind) {
    case T1_OP_IN:
        return x->in[idx];
    case T1_OP_OUT:
        return x->out[idx];
    case T1_OP_TEMP:
        return x->temp[idx];
    case T1_OP_WINDOW:
        return x->window;
    default:
        return T1_X86_NOREG;
    }
}

static uint8_t binder_reg(const struct inst *x, uint8_t id)
{
    return id == T1_BINDER_NONE ? T1_X86_NOREG : reg_of(x, id >> 5, id & 31);
}

/* An IMM operand's value: literal, immediate binder or derived. */
static int64_t imm_of(const struct inst *x, const struct t1_opnd *o)
{
    if (o->index == T1_BINDER_NONE)
        return o->imm;
    if (T1_IMM_IS_EXPR(o->index))
        return t1_iexpr_eval(x->r->expr, T1_IMM_EXPR_INDEX(o->index), x->imm);
    return x->imm[o->index];
}

static void op0(struct t1_x86_buf *b, uint16_t op, uint8_t r, int64_t imm,
                const struct t1_x86_mem *m)
{
    struct t1_x86_insn i = {.op = op};
    i.opnd[0].reg = r;
    i.opnd[1].imm = imm;
    if (m)
        i.mem = *m;
    t1_x86_emit(b, &i, NULL);
}

static void raw(struct t1_x86_buf *b, const uint8_t *bytes, size_t n)
{
    if (b->failed || n > b->capacity - b->size) {
        b->failed = true;
        return;
    }
    memcpy(b->data + b->size, bytes, n);
    b->size += n;
}

/* Builds prologue, the template, label stubs and epilogue into buf. */
static bool build(const struct inst *x, uint8_t *buf, size_t cap, size_t *len)
{
    const struct t1_rule *r = x->r;
    struct t1_x86_buf b = {buf, 0, cap, false};
    static const uint8_t saved[] = {T1_RBX, T1_RBP, T1_R12, T1_R13, T1_R14, T1_R15, T1_RDI};
    for (unsigned i = 0; i < sizeof(saved); ++i)
        op0(&b, T1_X86_PUSH64_R, saved[i], 0, NULL);
    for (unsigned i = 0; i < sizeof(pool); ++i) {
        struct t1_x86_mem m = {T1_RDI, T1_X86_NOREG, 0, (int32_t)(8 * pool[i])};
        op0(&b, T1_X86_LOAD64, pool[i], 0, &m);
    }
    struct t1_x86_mem fl = {T1_RDI, T1_X86_NOREG, 0, IMG_RFLAGS};
    op0(&b, T1_X86_PUSH64_M, 0, 0, &fl);
    op0(&b, T1_X86_POPFQ, 0, 0, NULL);

    size_t fix[16];
    unsigned fix_label[16], nfix = 0;
    for (unsigned t = 0; t < r->ntmpl; ++t) {
        const struct t1_tmpl *m = &r->tmpl[t];
        struct t1_x86_insn i = {.op = m->x86op};
        int label = -1;
        for (unsigned o = 0; o < m->nopnd && o < 2; ++o) {
            const struct t1_opnd *op = &m->opnd[o];
            switch (op->kind) {
            case T1_OP_IN:
            case T1_OP_OUT:
            case T1_OP_TEMP:
                i.opnd[o].reg = reg_of(x, op->kind, op->index);
                break;
            case T1_OP_IMM:
                i.opnd[o].imm = imm_of(x, op);
                break;
            case T1_OP_LABEL:
                label = op->index;
                break;
            case T1_OP_MEM: {
                int64_t disp = m->disp;
                if (m->disp_expr != T1_TMPL_DISP_NONE)
                    disp += t1_iexpr_eval(r->expr, T1_IMM_EXPR_INDEX(m->disp_expr), x->imm);
                if (disp < INT32_MIN || disp > INT32_MAX)
                    return false;
                unsigned scale = m->scale_shift & 0x80 ? (unsigned)x->imm[m->scale_shift & 0x7f]
                                                       : m->scale_shift;
                if (scale > 3)
                    return false;
                i.mem = (struct t1_x86_mem){binder_reg(x, m->base), binder_reg(x, m->index),
                                            (uint8_t)scale, (int32_t)disp};
                break;
            }
            default:
                return false;
            }
        }
        size_t f;
        t1_x86_emit(&b, &i, &f);
        if (label >= 0) {
            if (nfix == 16)
                return false;
            fix[nfix] = f;
            fix_label[nfix++] = (unsigned)label;
        }
    }
    /* Fall-through: taken stays 0 (preset). Labels store 1 ($T) or 2 ($F). */
    size_t exit_fix[2], stub[2];
    struct t1_x86_insn jmp = {.op = T1_X86_JMP};
    size_t fall_fix;
    t1_x86_emit(&b, &jmp, &fall_fix);
    for (unsigned l = 0; l < 2; ++l) {
        stub[l] = b.size;
        struct t1_x86_mem tk = {T1_RDI, T1_X86_NOREG, 0, IMG_TAKEN};
        op0(&b, T1_X86_STORE64_I32, 0, (int64_t)l + 1, &tk);
        t1_x86_emit(&b, &jmp, &exit_fix[l]);
    }
    size_t end = b.size;
    /* pushfq ; pop qword [rdi + IMG_RFLAGS] (outside T1_X86_OPS: harness only). */
    static const uint8_t save_flags[] = {0x9c, 0x8f, 0x87, IMG_RFLAGS, 0, 0, 0};
    raw(&b, save_flags, sizeof(save_flags));
    for (unsigned i = 0; i < sizeof(pool); ++i) {
        struct t1_x86_mem m = {T1_RDI, T1_X86_NOREG, 0, (int32_t)(8 * pool[i])};
        struct t1_x86_insn st = {.op = T1_X86_STORE64, .mem = m};
        st.opnd[1].reg = pool[i];
        t1_x86_emit(&b, &st, NULL);
    }
    for (unsigned i = sizeof(saved); i-- > 0;)
        op0(&b, T1_X86_POP64_R, saved[i], 0, NULL);
    op0(&b, T1_X86_RET, 0, 0, NULL);
    for (unsigned k = 0; k < nfix; ++k)
        t1_x86_patch_rel32(&b, fix[k], stub[fix_label[k] & 1]);
    t1_x86_patch_rel32(&b, fall_fix, end);
    t1_x86_patch_rel32(&b, exit_fix[0], end);
    t1_x86_patch_rel32(&b, exit_fix[1], end);
    *len = b.size;
    return !b.failed;
}

static bool run(const struct inst *x, struct image *img)
{
    static uint8_t buf[CODE_BYTES];
    size_t len;
    if (!build(x, buf, sizeof(buf), &len))
        return false;
    if (mprotect(code_page, CODE_BYTES, PROT_READ | PROT_WRITE))
        return false;
    memcpy(code_page, buf, len);
    if (mprotect(code_page, CODE_BYTES, PROT_READ | PROT_EXEC))
        return false;
    code_fn *fn;
    memcpy(&fn, &code_page, sizeof(fn)); /* object -> function pointer */
    fn(img);
    return true;
}

static bool is_memory_rule(const struct t1_rule *r)
{
    return r->pat[0].ir_op == T1_IR_LOAD || r->pat[0].ir_op == T1_IR_STORE;
}

/* Runs one case; returns false if the table rejects the immediates. */
static bool isel_case(const struct t1_rule *r, const int64_t *imm, unsigned nimm,
                      const uint64_t *in, unsigned nin, uint64_t status, uint64_t mem,
                      struct image *img, struct inst *x, bool *ok)
{
    *ok = true;
    if (!supported(r))
        return false;
    if (!accepts(r->pat, r->npat, r->expr, r->check, r->ncheck, imm, nimm))
        return false;
    *x = (struct inst){.r = r, .imm = imm, .nin = nin};
    if (!assign(x)) {
        *ok = false;
        return true;
    }
    memset(img, 0, sizeof(*img));
    for (unsigned i = 0; i < sizeof(pool); ++i)
        img->gpr[pool[i]] = UINT64_C(0x5a5a5a5a00000000) | pool[i]; /* havoc'd outputs/temps */
    for (unsigned i = 0; i < nin; ++i)
        img->gpr[x->in[i]] = in[i];
    img->mem = mem;
    if (is_memory_rule(r))
        img->gpr[x->window] = (uint64_t)(uintptr_t)&img->mem - in[r->pat[1].bind];
    img->rflags = (__builtin_ia32_readeflags_u64() & ~STATUS_FLAGS) | (status & STATUS_FLAGS);
    img->taken = 0;
    *ok = run(x, img);
    return true;
}

static bool read_hex(char **p, uint64_t *v)
{
    char *end;
    unsigned long long x = strtoull(*p, &end, 16);
    if (end == *p)
        return false;
    *p = end;
    *v = x;
    return true;
}

static int run_isel(void)
{
    static char line[8192];
    unsigned long n = 0;
    while (fgets(line, sizeof(line), stdin)) {
        ++n;
        char *p = line;
        uint64_t idx = 0, nimm = 0, nin, status, mem, v = 0;
        int64_t imm[32];
        uint64_t in[32];
        bool good = read_hex(&p, &idx) && idx < t1_rule_count && read_hex(&p, &nimm) && nimm <= 32;
        for (unsigned i = 0; good && i < nimm; ++i) {
            good = read_hex(&p, &v);
            imm[i] = (int64_t)v;
        }
        good = good && read_hex(&p, &nin) && nin <= 32;
        for (unsigned i = 0; good && i < nin; ++i)
            good = read_hex(&p, &in[i]);
        good = good && read_hex(&p, &status) && read_hex(&p, &mem);
        if (!good) {
            fprintf(stderr, "tier1-table-runner: bad isel line %lu: %s", n, line);
            return 2;
        }
        const struct t1_rule *r = &t1_rules[idx];
        struct image img;
        struct inst x;
        bool ok;
        if (!isel_case(r, imm, (unsigned)nimm, in, (unsigned)nin, status, mem, &img, &x, &ok)) {
            puts("0");
            continue;
        }
        if (!ok) {
            fprintf(stderr,
                    "tier1-table-runner: rule %s: cannot instantiate (registers or encoding)\n",
                    r->name);
            return 2;
        }
        printf("1 %llx %llx", (unsigned long long)img.taken,
               (unsigned long long)(img.rflags & STATUS_FLAGS));
        for (unsigned i = 0; i < r->nout; ++i)
            printf(" %llx", (unsigned long long)img.gpr[x.out[i]]);
        for (unsigned i = 0; i < nin; ++i)
            printf(" %llx", (unsigned long long)img.gpr[x.in[i]]);
        printf(" %llx\n", (unsigned long long)img.mem);
    }
    return 0;
}

static int run_fold(void)
{
    static char line[4096];
    unsigned long n = 0;
    while (fgets(line, sizeof(line), stdin)) {
        ++n;
        char *p = line;
        uint64_t idx = 0, nimm = 0, v = 0;
        int64_t imm[32];
        bool good =
            read_hex(&p, &idx) && idx < t1_fold_rule_count && read_hex(&p, &nimm) && nimm <= 32;
        for (unsigned i = 0; good && i < nimm; ++i) {
            good = read_hex(&p, &v);
            imm[i] = (int64_t)v;
        }
        if (!good) {
            fprintf(stderr, "tier1-table-runner: bad fold line %lu: %s", n, line);
            return 2;
        }
        const struct t1_fold_rule *f = &t1_fold_rules[idx];
        if (!accepts(f->pat, f->npat, f->expr, f->check, f->ncheck, imm, (unsigned)nimm)) {
            puts("0");
            continue;
        }
        printf("1 %x", f->nres);
        for (unsigned i = 0; i < f->nres; ++i) {
            const struct t1_fold_node *r = &f->res[i];
            bool has = r->ir_op != T1_PAT_VALUE && r->imm != T1_BINDER_NONE;
            int64_t val = has ? t1_iexpr_eval(f->expr, T1_IMM_EXPR_INDEX(r->imm), imm) : 0;
            printf(" %x %x %x %x %x %x %x %x %llx", r->ir_op, r->result, r->nkids, r->kid[0],
                   r->kid[1], r->kid[2], r->bind, has, (unsigned long long)val);
        }
        putchar('\n');
    }
    return 0;
}

static int run_iexpr(void)
{
    static char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        char *p = line;
        uint64_t op, a, b;
        if (!read_hex(&p, &op) || op >= T1_IX_COUNT || op < T1_IX_ADD || !read_hex(&p, &a) ||
            !read_hex(&p, &b)) {
            fprintf(stderr, "tier1-table-runner: bad iexpr line: %s", line);
            return 2;
        }
        const struct t1_iexpr x[3] = {
            {T1_IX_IMM, 0, 0, 0}, {T1_IX_IMM, 1, 0, 0}, {(uint8_t)op, 0, 1, 0}};
        const int64_t imm[2] = {(int64_t)a, (int64_t)b};
        printf("%llx\n", (unsigned long long)t1_iexpr_eval(x, 2, imm));
    }
    return 0;
}

/* ---- selftest ---- */

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "tier1-table-runner selftest: %s:%u: %s\n", __FILE__, __LINE__, #c);   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* Candidate immediates of one pattern node (by its layout). */
static unsigned candidates(const struct t1_pat *p, int64_t *out)
{
    unsigned n = 0;
    switch (p->ir_op) {
    case T1_IR_EXTRU:
    case T1_IR_EXTR:
    case T1_IR_DEPZ: {
        static const int pl[][2] = {{0, 8},   {4, 8},  {8, 56}, {60, 8}, {0, 64},
                                    {10, 20}, {0, 16}, {0, 32}, {63, 1}, {1, 63}};
        for (unsigned i = 0; i < sizeof(pl) / sizeof(pl[0]); ++i)
            out[n++] = pl[i][0] | pl[i][1] << 8;
        return n;
    }
    case T1_IR_LOAD:
    case T1_IR_STORE:
        out[n++] = 1;
        out[n++] = 2;
        out[n++] = 4;
        out[n++] = 8;
        out[n++] = 8 | 1 << 8 | 5 << 16;
        return n;
    default: {
        static const int64_t v[] = {0,  1,  2,  3,   4,  7,  8,          16,
                                    32, 63, 64, 100, -1, -5, 0x7fffffff, -0x7fffffff};
        out[n++] = p->imm_min;
        out[n++] = p->imm_max;
        for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); ++i)
            out[n++] = v[i];
        return n;
    }
    }
}

/* Finds immediates the table accepts by trying candidate combinations. */
static bool find_imms(const struct t1_pat *pat, unsigned npat, const struct t1_iexpr *expr,
                      const struct t1_icheck *check, unsigned ncheck, int64_t *imm, unsigned nimm)
{
    int64_t cand[32][24];
    unsigned ncand[32] = {0}, at[32] = {0};
    for (unsigned i = 0; i < npat; ++i)
        if (pat[i].ir_op != T1_PAT_VALUE && pat[i].bind != T1_BINDER_NONE)
            ncand[pat[i].bind] = candidates(&pat[i], cand[pat[i].bind]);
    for (unsigned long tries = 0; tries < 100000; ++tries) {
        for (unsigned i = 0; i < nimm; ++i)
            imm[i] = cand[i][at[i]];
        if (accepts(pat, npat, expr, check, ncheck, imm, nimm))
            return true;
        unsigned k = 0;
        while (k < nimm && ++at[k] == ncand[k])
            at[k++] = 0;
        if (k == nimm)
            return false;
    }
    return false;
}

static const struct t1_rule *rule(const char *name)
{
    for (unsigned i = 0; i < t1_rule_count; ++i)
        if (!strcmp(t1_rules[i].name, name))
            return &t1_rules[i];
    return NULL;
}

/* ---- fpx ---- */
static bool fpx_run(const struct t1_fpx_rule *r, int64_t sf, int64_t mf, struct fpx_block *blk,
                    unsigned *exit)
{
    static uint8_t buf[CODE_BYTES];
    return fpx_native_run(r, sf, mf, blk, exit, buf, code_page, CODE_BYTES);
}

static int run_fpx(void)
{
    static char line[1024];
    bool fma3 = fpx_host_fma3();
    while (fgets(line, sizeof(line), stdin)) {
        char *p = line;
        uint64_t v[3 + T1_FPX_INPUTS];
        for (unsigned k = 0; k < 3 + T1_FPX_INPUTS; ++k)
            if (!read_hex(&p, &v[k]))
                return 2;
        if (v[0] >= t1_fpx_rule_count)
            return 2;
        const struct t1_fpx_rule *r = &t1_fpx_rules[v[0]];
        if (r->fma3 && !fma3) {
            puts("0");
            continue;
        }
        struct fpx_block blk;
        for (unsigned k = 0; k < T1_FPX_INPUTS; ++k)
            blk.in[k] = v[3 + k];
        unsigned exit;
        if (!fpx_run(r, (int64_t)v[1], (int64_t)v[2], &blk, &exit))
            return 2;
        printf("1 %x %llx %llx %llx\n", exit, (unsigned long long)blk.out[T1_FPX_SIG],
               (unsigned long long)blk.out[T1_FPX_META], (unsigned long long)blk.out[T1_FPX_MF]);
        fflush(stdout);
    }
    return 0;
}

/* qp = 0: every expansion stores the destination unchanged, no modified bit. */
static unsigned fpx_selftest(bool *ok)
{
    unsigned ran = 0;
    bool fma3 = fpx_host_fma3();
    for (unsigned i = 0; i < t1_fpx_rule_count; ++i) {
        const struct t1_fpx_rule *r = &t1_fpx_rules[i];
        if (r->fma3 && !fma3)
            continue;
        struct fpx_block blk = {{0}, {0}};
        blk.in[T1_FPX_D_SIG] = UINT64_C(0x8000000000000001);
        blk.in[T1_FPX_D_META] = 0x2abcd;
        unsigned exit;
        if (!fpx_run(r, 1, 16, &blk, &exit) || exit != 1 ||
            blk.out[T1_FPX_SIG] != blk.in[T1_FPX_D_SIG] ||
            blk.out[T1_FPX_META] != blk.in[T1_FPX_D_META] || blk.out[T1_FPX_MF]) {
            fprintf(stderr, "tier1-table-runner selftest: expansion %s with qp = 0\n", r->name);
            *ok = false;
        }
        ++ran;
    }
    return ran;
}

static int selftest(void)
{
    unsigned ran = 0;
    for (unsigned i = 0; i < t1_rule_count; ++i) {
        const struct t1_rule *r = &t1_rules[i];
        if (!supported(r))
            continue;
        unsigned nimm = imm_binders(r->pat, r->npat), nin = in_binders(r->pat, r->npat);
        int64_t imm[32] = {0};
        uint64_t in[32];
        if (!find_imms(r->pat, r->npat, r->expr, r->check, r->ncheck, imm, nimm)) {
            fprintf(stderr, "tier1-table-runner selftest: no immediates accepted by %s\n", r->name);
            return 1;
        }
        for (unsigned k = 0; k < nin; ++k)
            in[k] = (k & 1) ? 1 : UINT64_C(0x0123456789abcdef) + k; /* I1 inputs see 0/1 or more */
        struct image img;
        struct inst x;
        bool ok;
        CHECK(isel_case(r, imm, nimm, in, nin, 0, UINT64_C(0xfedcba9876543210), &img, &x, &ok));
        if (!ok) {
            fprintf(stderr, "tier1-table-runner selftest: rule %s does not instantiate\n", r->name);
            return 1;
        }
        ++ran;
    }
    /* Spot checks of format-2 entries (values by hand from the SDM). */
    const struct t1_rule *r = rule("extru_field");
    CHECK(r && r->ncheck == 1 && r->nexpr > 0);
    int64_t f[1] = {4 | 8 << 8}; /* extr.u r = a, 4, 8 */
    uint64_t a[1] = {UINT64_C(0x1234)};
    struct image img;
    struct inst x;
    bool ok;
    CHECK(isel_case(r, f, 1, a, 1, 0, 0, &img, &x, &ok) && ok && img.gpr[x.out[0]] == 0x23);
    int64_t top[1] = {60 | 8 << 8}; /* p + l >= 64: rejected */
    CHECK(!isel_case(r, top, 1, a, 1, 0, 0, &img, &x, &ok));
    r = rule("add_ri"); /* lea with a binder displacement */
    int64_t k[1] = {-8};
    uint64_t base[1] = {100};
    CHECK(r && isel_case(r, k, 1, base, 1, 0, 0, &img, &x, &ok) && ok && img.gpr[x.out[0]] == 92);
    r = rule("load2"); /* zero-extending 2-byte load through the window */
    int64_t m[1] = {2 | 1 << 8 | 7 << 16};
    uint64_t addr[1] = {UINT64_C(0xe000000000001000)};
    CHECK(r && isel_case(r, m, 1, addr, 1, 0, UINT64_C(0x1111222233348765), &img, &x, &ok) && ok &&
          img.gpr[x.out[0]] == 0x8765);
    r = rule("store1");
    uint64_t sv[2] = {addr[0], UINT64_C(0xabcd)};
    int64_t m1[1] = {1};
    CHECK(r && isel_case(r, m1, 1, sv, 2, 0, UINT64_C(0x1111222233344455), &img, &x, &ok) && ok &&
          img.mem == UINT64_C(0x11112222333444cd));
    unsigned folds = 0;
    for (unsigned i = 0; i < t1_fold_rule_count; ++i) {
        const struct t1_fold_rule *fr = &t1_fold_rules[i];
        int64_t imm[32] = {0};
        if (!find_imms(fr->pat, fr->npat, fr->expr, fr->check, fr->ncheck, imm,
                       imm_binders(fr->pat, fr->npat))) {
            fprintf(stderr, "tier1-table-runner selftest: no immediates accepted by fold %s\n",
                    fr->name);
            return 1;
        }
        for (unsigned j = 0; j < fr->nres; ++j)
            if (fr->res[j].ir_op != T1_PAT_VALUE && fr->res[j].imm != T1_BINDER_NONE)
                (void)t1_iexpr_eval(fr->expr, T1_IMM_EXPR_INDEX(fr->res[j].imm), imm);
        ++folds;
    }
    bool fpx_ok = true;
    unsigned fpx = fpx_selftest(&fpx_ok);
    if (!fpx_ok)
        return 1;
    printf(
        "tier1-table-runner: selftest passed (%u isel rules executed, %u fold rules instantiated, "
        "%u FP expansions run)\n",
        ran, folds, fpx);
    return 0;
}

int main(int argc, char **argv)
{
    /* Native snippets can migrate too; intersect the process's CPUs. */
    cpu_set_t allowed, one;
    if (sched_getaffinity(0, sizeof(allowed), &allowed))
        return 2;
    struct host_isa_cpu_report reports[CPU_SETSIZE];
    unsigned count = 0;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed))
            continue;
        CPU_ZERO(&one);
        CPU_SET(cpu, &one);
        if (sched_setaffinity(0, sizeof(one), &one))
            return 2;
        reports[count++] = (struct host_isa_cpu_report){host_isa_probe(host_isa_cpuid), true};
    }
    if (sched_setaffinity(0, sizeof(allowed), &allowed))
        return 2;
    const char *mask = getenv("IA64VMM_HOST_ISA_MASK");
    char cmdline[64] = "";
    if (mask) {
        int n = snprintf(cmdline, sizeof(cmdline), "host_isa_mask=%s", mask);
        if (!*mask || strspn(mask, "0123456789") != strlen(mask) || n < 0 ||
            (size_t)n >= sizeof(cmdline))
            return 2;
    }
    if (host_isa_resolve(cmdline, reports, count, &native_isa) != 1)
        return 2;
    if (argc == 2 && !strcmp(argv[1], "supported")) {
        for (unsigned r = 0; r < t1_rule_count; ++r)
            if (supported(&t1_rules[r]))
                puts(t1_rules[r].name);
        return 0;
    }
    code_page = mmap(NULL, CODE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code_page == MAP_FAILED) {
        perror("tier1-table-runner: mmap");
        return 2;
    }
    int rc = 2;
    if (argc == 2 && !strcmp(argv[1], "isel"))
        rc = run_isel();
    else if (argc == 2 && !strcmp(argv[1], "fold"))
        rc = run_fold();
    else if (argc == 2 && !strcmp(argv[1], "iexpr"))
        rc = run_iexpr();
    else if (argc == 2 && !strcmp(argv[1], "fpx"))
        rc = run_fpx();
    else if (argc == 2 && !strcmp(argv[1], "selftest"))
        rc = selftest();
    else
        fprintf(stderr, "usage: tier1-table-runner isel | fold | iexpr | fpx | selftest\n");
    munmap(code_page, CODE_BYTES);
    return rc;
}
