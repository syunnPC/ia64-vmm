/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Emit MIR as AMD64 bytes, recording site/force/resume offsets and loop extent.
 * Static LIVEINs load R12-relative homes; stacked LIVEINs load entry-image
 * slots through R14, recorded in frame_in for the dispatcher. Generated code
 * never addresses stacked homes directly, including across ring wraparound.
 *
 * Jump-only blocks alias their targets; fall-through jumps are removed.
 * Check every pseudo-op expansion against its MIR def/clobber set; undeclared
 * register or flag writes fail compilation.
 */
#include "engine/tier1/codegen.h"
#include "engine/tier1/restore.h"
#include "engine/tier1/window_loop.h"
#include "engine/tier1/x86sse.h"
#include "engine/tier1/fpfast.h"
#include "engine/tier1/form.h"
#include "ia64/hostfp.h"
#include "ia64vmm/hal.h"
#include "engine/tier1/memory.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"
#include <stddef.h>

#define NOREG T1_X86_NOREG

struct em {
    const struct t1_mir *m;
    struct t1_emitted *o;
    struct t1_x86_buf b;
    uint32_t *block_off; /* per MIR block: byte offset, UINT32_MAX until placed */
    uint16_t *alias;     /* per MIR block: the block it stands for */
    unsigned nfix;
    unsigned ncold;
    unsigned wrapper_budget; /* optional rel32 calls after mandatory fixups */
    bool wrapper_counted;
    /* Expansion check: registers and flags the current pseudo-op may write. */
    uint16_t allow_regs, allow_flags;
    bool checking;
    bool skip_next;                       /* the jmp after an inverted jcc */
    uint8_t frame_slot[T1_ELEMENT_PR(0)]; /* stacked element -> entry-image slot + 1 */
    unsigned site_base;                   /* x->site[] slot of site 0 */
    t1_value admitted_address;
    unsigned admitted_site, admitted_align;
    char *err;
    size_t err_size;
    bool failed;
    bool versioned, fast_loop;
    struct t1_window_loop window;
    /* Read below nfix and ncold only: t1_emit clears the fields above. */
    struct {
        uint32_t at;
        uint16_t block;
    } fix[8192];
    /* Cold stubs to emit after the body. */
    struct cold {
        uint8_t kind;
        uint32_t point;
        uint32_t from_fix[2];
        uint32_t check;
    } cold[2048];
};

size_t t1_emit_state_bytes(void)
{
    return sizeof(struct em);
}

enum { COLD_MISS, COLD_FORCE_SITE, COLD_FORCE_HELPER, COLD_CKPT };

static bool em_fail(struct em *E, const char *what, unsigned a)
{
    if (!E->failed && E->err_size)
        vmm_snprintf(E->err, E->err_size, "emit: %s (%u)", what, a);
    E->failed = true;
    return false;
}

/* ---- raw instructions ---- */

static void check_clobbers(struct em *E, uint16_t regs, uint16_t flags, unsigned op)
{
    if (E->checking && ((regs & ~E->allow_regs) || (flags & ~E->allow_flags)))
        em_fail(E, "expansion writes outside its declared clobbers", op);
}

static void check_writes(struct em *E, const struct t1_x86_insn *i)
{
    if (!E->checking)
        return;
    const struct t1_x86_info *x = t1_x86_info(i->op);
    uint16_t regs = x->implicit_def;
    for (unsigned k = 0; k < x->nopnd && k < 2; ++k) {
        uint8_t s = x->opnd[k];
        if (s == T1_XO_R64_W || s == T1_XO_R64_RW || s == T1_XO_R32_W || s == T1_XO_R32_RW ||
            s == T1_XO_R8_W)
            regs |= T1_X86_REGBIT(i->opnd[k].reg);
    }
    uint16_t flags = (uint16_t)(x->flags_def | x->flags_undef);
    check_clobbers(E, regs, flags, i->op);
}

static size_t put(struct em *E, const struct t1_x86_insn *i)
{
    size_t fix = SIZE_MAX;
    check_writes(E, i);
    t1_x86_emit(&E->b, i, &fix);
    if (E->b.failed)
        em_fail(E, "encoding", i->op);
    return fix;
}

static struct t1_x86_insn I(uint16_t op) {
    return (struct t1_x86_insn){
        .op = op,
        .opnd = {{NOREG, 0}, {NOREG, 0}},
        .mem = {NOREG, NOREG, 0, 0},
    };
} static struct t1_x86_mem M(uint8_t base, int32_t disp) {
    return (struct t1_x86_mem){base, NOREG, 0, disp};
} static void rr(struct em *E, uint16_t op, uint8_t a, uint8_t b)
{
    struct t1_x86_insn i = I(op);
    i.opnd[0].reg = a;
    i.opnd[1].reg = b;
    put(E, &i);
}
static void ri(struct em *E, uint16_t op, uint8_t a, int64_t imm)
{
    struct t1_x86_insn i = I(op);
    i.opnd[0].reg = a;
    i.opnd[1].imm = imm;
    put(E, &i);
}
/* reg <- mem (loads, cmp/or with a memory source). */
static void rm(struct em *E, uint16_t op, uint8_t a, struct t1_x86_mem mem)
{
    struct t1_x86_insn i = I(op);
    i.opnd[0].reg = a;
    i.mem = mem;
    put(E, &i);
}
/* mem <- reg (stores). */
static void mr(struct em *E, uint16_t op, struct t1_x86_mem mem, uint8_t b)
{
    struct t1_x86_insn i = I(op);
    i.mem = mem;
    i.opnd[1].reg = b;
    put(E, &i);
}
static void mi(struct em *E, uint16_t op, struct t1_x86_mem mem, int64_t imm)
{
    struct t1_x86_insn i = I(op);
    i.mem = mem;
    i.opnd[1].imm = imm;
    put(E, &i);
}
static void m_only(struct em *E, uint16_t op, struct t1_x86_mem mem)
{
    struct t1_x86_insn i = I(op);
    i.mem = mem;
    put(E, &i);
}

/* A rel32 branch to an emitted block (resolved at the end). */
static void branch_block(struct em *E, uint16_t op, uint16_t block)
{
    struct t1_x86_insn i = I(op);
    size_t f = put(E, &i);
    if (f == SIZE_MAX || E->failed)
        return;
    if (E->nfix >= sizeof(E->fix) / sizeof(E->fix[0])) {
        em_fail(E, "fixup limit", 0);
        return;
    }
    E->fix[E->nfix].at = (uint32_t)f;
    E->fix[E->nfix++].block = block;
}
/* A rel32 branch to an absolute address (a stub), patched at placement. */
static void branch_abs(struct em *E, uint16_t op, uintptr_t target)
{
    struct t1_x86_insn i = I(op);
    size_t f = put(E, &i);
    if (f == SIZE_MAX || E->failed)
        return;
    if (E->o->nabs >= sizeof(E->o->abs_fix) / sizeof(E->o->abs_fix[0])) {
        em_fail(E, "stub fixup limit", 0);
        return;
    }
    E->o->abs_fix[E->o->nabs] = (uint32_t)f;
    E->o->abs_target[E->o->nabs++] = target;
}
/* A rel32 branch to a byte offset known later (cold stubs). */
static uint32_t branch_later(struct em *E, uint16_t op)
{
    struct t1_x86_insn i = I(op);
    size_t f = put(E, &i);
    return f == SIZE_MAX ? UINT32_MAX : (uint32_t)f;
}
static void patch(struct em *E, uint32_t at, uint32_t target)
{
    if (at != UINT32_MAX)
        t1_x86_patch_rel32(&E->b, at, target);
}

/* Reserve the existing absolute-fixup capacity for exits before spending
 * any on shorter wrapper calls. Count lazily: regions with no calls need
 * no scan, and a full mandatory table keeps the old indirect-call path. */
static unsigned wrapper_budget(struct em *E)
{
    if (!E->wrapper_counted) {
        E->wrapper_counted = true;
        E->wrapper_budget = sizeof(E->o->abs_fix) / sizeof(E->o->abs_fix[0]);
        if (E->versioned)
            --E->wrapper_budget; /* cloned checkpoint */
        for (uint32_t k = 0; k < E->m->ninsns; ++k) {
            const struct t1_mir_insn *in = &E->m->insn[k];
            unsigned reserve =
                in->op == T1_MIR_CHECKPOINT || in->op == T1_MIR_HGUARD || in->op == T1_MIR_EXIT ||
                (in->op == T1_MIR_FORCE && E->m->point[in->point].kind == T1_PT_HELPER);
            E->wrapper_budget = reserve < E->wrapper_budget ? E->wrapper_budget - reserve : 0;
        }
    }
    return E->wrapper_budget;
}

/* Only the shared helper and miss wrappers use this preservation ABI.
 * Check the call's effective writes rather than the raw SysV CALL entry.
 * Wrappers and region code share the arena, whose 1 GiB size limit keeps
 * every target in signed rel32 range, including after code recycling. */
static void call_wrapper(struct em *E, uintptr_t target)
{
    if (wrapper_budget(E)) {
        --E->wrapper_budget;
        check_clobbers(E, T1_WRAPPER_CLOBBERS, T1_XF_ALL, T1_X86_CALL_REL);
        bool checking = E->checking;
        E->checking = false;
        branch_abs(E, T1_X86_CALL_REL, target);
        E->checking = checking;
        return;
    }
    ri(E, T1_X86_MOV64_RI64, T1_R11, (int64_t)target);
    struct t1_x86_insn i = I(T1_X86_CALL_R);
    i.opnd[0].reg = T1_R11;
    check_clobbers(E, T1_WRAPPER_CLOBBERS, T1_XF_ALL, i.op);
    bool checking = E->checking;
    E->checking = false;
    put(E, &i);
    E->checking = checking;
}

/* ---- pseudo-op expansions ---- */

static void allow(struct em *E, uint16_t regs, uint16_t flags)
{
    E->allow_regs = regs;
    E->allow_flags = flags;
    E->checking = true;
}

/* A stacked LIVEIN: its entry-image slot (exec.h frame_in), values as 64
 * bits, NaTs as 0/1 in the low byte. */
static void load_frame_in(struct em *E, uint8_t reg, uint16_t element)
{
    unsigned k = E->frame_slot[element];
    if (!k) {
        if (E->o->nframe_in >= T1_MAX_FRAME_IN) {
            em_fail(E, "entry-image slots", element);
            return;
        }
        E->o->frame_in[E->o->nframe_in++] = element;
        k = E->frame_slot[element] = (uint8_t)E->o->nframe_in;
    }
    struct t1_x86_mem slot = M(T1_R14, (int32_t)X_FRAME_IN(k - 1));
    if (element >= T1_ELEMENT_NAT(32))
        rm(E, T1_X86_LOAD8ZX, reg, slot);
    else
        rm(E, T1_X86_LOAD64, reg, slot);
}

static void load_home(struct em *E, uint8_t reg, uint16_t element)
{
    uint32_t off;
    unsigned bit, bytes;
    if (t1_element_stacked(element)) {
        load_frame_in(E, reg, element);
        return;
    }
    if (!t1_element_home(element, &off, &bit, &bytes)) {
        em_fail(E, "LIVEIN of a non-element", element);
        return;
    }
    if (element >= T1_ELEMENT_FR_META(2) && element <= T1_ELEMENT_FR_META(127)) {
        rm(E, T1_X86_LOAD32, reg, M(T1_R12, (int32_t)off));
        rm(E, T1_X86_LOAD8ZX, T1_R11,
           M(T1_R12,
             (int32_t)(off + offsetof(struct ia64_fr, sign) - offsetof(struct ia64_fr, exponent))));
        ri(E, T1_X86_SHL64_RI, T1_R11, 17);
        rr(E, T1_X86_OR64_RR, reg, T1_R11);
        return;
    }
    if (bytes == 1) {
        rm(E, T1_X86_LOAD8ZX, reg, M(T1_R12, (int32_t)off));
        return;
    }
    rm(E, T1_X86_LOAD64, reg, M(T1_R12, (int32_t)off));
    if (element >= T1_ELEMENT_PR(0) && element < T1_ELEMENT_LC) {
        if (bit)
            ri(E, T1_X86_SHR64_RI, reg, bit);
        if (bit != 63)
            ri(E, T1_X86_AND64_RI, reg, 1);
    }
}

static struct t1_x86_mem spill_mem(unsigned slot)
{
    return M(T1_R14, (int32_t)X_SPILL(slot));
}

/* ---- Experimental FP helper expansions (fpfast.h) ----
 *
 * Before the helper call, the expansion of an fma-family or fpma-family
 * helper emits the conditionally checked expansion of fpfast.rules the
 * instruction selects (opcode, precision, f0 as f2, the host's FMA3 and
 * fp_approx). It reads its inputs through fpx_in: frame locations, homes
 * outside the frame, FPSR's home and the f0/f1 images; it stores the
 * helper's results to helper_out and jumps past the call, or jumps to the
 * call having stored nothing. R11 and xmm0-xmm3 are scratch; MXCSR holds
 * the VMM's control (1F80H).
 */
struct fpfast {
    struct em *E;
    uint32_t point;
    const struct t1_insn *ti;
    bool failed;
    unsigned nbail, ndone;
    uint32_t bail[96], done[8];
};
static struct t1_x86_mem helper_out(uint16_t element)
{
    return M(T1_R14, (int32_t)(offsetof(struct t1_exec, helper_out) + 8u * element));
}
/* R11 = FR, FR meta or PR element e from its home (load_home with R11
 * as the only register). */
static void fp_home(struct fpfast *F, uint16_t e)
{
    struct em *E = F->E;
    uint32_t off;
    unsigned bit, bytes;
    bool fr = e >= T1_ELEMENT_FR(2) && e <= T1_ELEMENT_FR(127);
    bool meta = e >= T1_ELEMENT_FR_META(2) && e <= T1_ELEMENT_FR_META(127);
    bool pr = e >= T1_ELEMENT_PR(1) && e < T1_ELEMENT_LC;
    if ((!fr && !meta && !pr) || !t1_element_home(e, &off, &bit, &bytes)) {
        F->failed = true;
        return;
    }
    if (meta) {
        rm(E, T1_X86_LOAD8ZX, T1_R11,
           M(T1_R12,
             (int32_t)(off + offsetof(struct ia64_fr, sign) - offsetof(struct ia64_fr, exponent))));
        ri(E, T1_X86_SHL64_RI, T1_R11, 17);
        rm(E, T1_X86_OR32_RM, T1_R11, M(T1_R12, (int32_t)off));
        return;
    }
    rm(E, T1_X86_LOAD64, T1_R11, M(T1_R12, (int32_t)off));
    if (pr) {
        if (bit)
            ri(E, T1_X86_SHR64_RI, T1_R11, bit);
        if (bit != 63)
            ri(E, T1_X86_AND64_RI, T1_R11, 1);
    }
}
/* R11 = element e's value at the point: a register, spill slot or
 * constant of the frame, else the home (exec.h: an element outside the
 * frame has its home's value). */
static void fp_value(struct fpfast *F, uint16_t e)
{
    const struct t1_mir *m = F->E->m;
    const struct t1_mir_point *P = &m->point[F->point];
    for (uint32_t k = P->frame_first; k < P->frame_first + P->frame_count; ++k) {
        if (m->frame[k].element != e)
            continue;
        const struct t1_loc *l = &m->frame_loc[k];
        if (l->kind == T1_LOC_REG)
            rr(F->E, T1_X86_MOV64_RR, T1_R11, l->reg);
        else if (l->kind == T1_LOC_SPILL)
            rm(F->E, T1_X86_LOAD64, T1_R11, spill_mem(l->slot));
        else if (l->kind == T1_LOC_CONST)
            ri(F->E, T1_X86_MOV64_RI64, T1_R11, (int64_t)l->value);
        else if (l->kind == T1_LOC_HOME)
            fp_home(F, e);
        else
            F->failed = true;
        return;
    }
    fp_home(F, e);
}
static size_t fpx_x86(void *ctx, const struct t1_x86_insn *i)
{
    return put(((struct fpfast *)ctx)->E, i);
}
static void fpx_in(void *ctx, unsigned input)
{
    struct fpfast *F = ctx;
    struct em *E = F->E;
    const struct t1_insn *ti = F->ti;
    const IA64FloatingOperands *op = &ti->insn.operands.floating;
    if (input == T1_FPX_QP) {
        if (ti->qp)
            fp_value(F, T1_ELEMENT_PR(ti->qp));
        else
            ri(E, T1_X86_MOV64_RI64, T1_R11, 1);
        return;
    }
    if (input == T1_FPX_FPSR) {
        rm(E, T1_X86_LOAD64, T1_R11,
           M(T1_R12, (int32_t)(offsetof(struct vcpu, s.ar) + 8u * AR_FPSR)));
        return;
    }
    /* a = f2 (source1), b = f3 (source2), c = f4 (auxiliary1), d = f1. */
    unsigned role = (input - T1_FPX_A_SIG) / 2,
             field[4] = {op->source1, op->source2, op->auxiliary1, op->destination};
    bool meta = (input - T1_FPX_A_SIG) & 1;
    if (field[role] < 2) {
        int64_t one = meta ? IA64_FP_BIAS : (int64_t)IA64_FP_INTEGER_BIT;
        ri(E, T1_X86_MOV64_RI64, T1_R11, field[role] ? one : 0);
        return;
    }
    unsigned f = ti->fr[role == 3 ? 0 : role + 1];
    fp_value(F, meta ? T1_ELEMENT_FR_META(f) : T1_ELEMENT_FR(f));
}
static int32_t fpx_out(void *ctx, unsigned slot)
{
    const struct t1_insn *ti = ((struct fpfast *)ctx)->ti;
    uint16_t e = slot == T1_FPX_SIG    ? T1_ELEMENT_FR(ti->fr[0])
                 : slot == T1_FPX_META ? T1_ELEMENT_FR_META(ti->fr[0])
                                       : T1_ELEMENT_PSR_MF;
    return helper_out(e).disp;
}

/* The expansion; false when the instruction has none (the plain call). */
static bool fp_fast(struct fpfast *F, const struct t1_insn *ti)
{
    const IA64FloatingOperands *op = &ti->insn.operands.floating;
    unsigned opts = ia64_hostfp_options();
    if (opts & IA64_HOSTFP_OPT_SOFT)
        return false;
    if (!(opts & IA64_HOSTFP_OPT_EXPERIMENTAL))
        return false;
    const struct t1_fpx_rule *r =
        t1_fpx_select(ti->insn.opcode, op->precision, !op->source1, opts & IA64_HOSTFP_OPT_FMA3,
                      opts & IA64_HOSTFP_OPT_APPROX);
    if (!r)
        return false;
    F->ti = ti;
    struct t1_fpx_io io = {&F->E->b,
                           fpx_x86,
                           fpx_in,
                           fpx_out,
                           F,
                           {op->status_field & 3,
                            op->destination < 32 ? (int64_t)IA64_PSR_MFL : (int64_t)IA64_PSR_MFH}};
    return t1_fpx_emit(r, &io, F->bail, &F->nbail, sizeof(F->bail) / sizeof(F->bail[0]), F->done,
                       &F->ndone, sizeof(F->done) / sizeof(F->done[0])) &&
           !F->failed;
}

/* One move of a parallel copy; R11 carries memory-to-memory values. */
static void move(struct em *E, const struct t1_loc *d, const struct t1_loc *s)
{
    if (d->kind == T1_LOC_REG) {
        if (s->kind == T1_LOC_REG) {
            if (s->reg != d->reg)
                rr(E, T1_X86_MOV64_RR, d->reg, s->reg);
        } else if (s->kind == T1_LOC_SPILL)
            rm(E, T1_X86_LOAD64, d->reg, spill_mem(s->slot));
        else if (s->value <= UINT32_MAX)
            ri(E, T1_X86_MOV32_RI32, d->reg, (int64_t)s->value);
        else if ((int64_t)s->value >= INT32_MIN && (int64_t)s->value <= INT32_MAX)
            ri(E, T1_X86_MOV64_RI32, d->reg, (int64_t)s->value);
        else
            ri(E, T1_X86_MOV64_RI64, d->reg, (int64_t)s->value);
        return;
    }
    struct t1_x86_mem dm = spill_mem(d->slot);
    if (s->kind == T1_LOC_REG) {
        mr(E, T1_X86_STORE64, dm, s->reg);
        return;
    }
    if (s->kind == T1_LOC_SPILL) {
        if (s->slot == d->slot)
            return;
        rm(E, T1_X86_LOAD64, T1_R11, spill_mem(s->slot));
        mr(E, T1_X86_STORE64, dm, T1_R11);
        return;
    }
    if ((int64_t)s->value >= INT32_MIN && (int64_t)s->value <= INT32_MAX) {
        mi(E, T1_X86_STORE64_I32, dm, (int64_t)s->value);
        return;
    }
    ri(E, T1_X86_MOV64_RI64, T1_R11, (int64_t)s->value);
    mr(E, T1_X86_STORE64, dm, T1_R11);
}

static bool loc_eq(const struct t1_loc *a, const struct t1_loc *b)
{
    if (a->kind != b->kind)
        return false;
    if (a->kind == T1_LOC_REG)
        return a->reg == b->reg;
    if (a->kind == T1_LOC_SPILL)
        return a->slot == b->slot;
    return a->value == b->value;
}

/* An allocated parallel-copy location. Constants never name a destination
 * and therefore share one unqueried source-count entry. */
static unsigned copy_location(const struct t1_loc *l)
{
    if (l->kind == T1_LOC_REG && l->reg < T1_X86_NREGS)
        return l->reg;
    if (l->kind == T1_LOC_SPILL && l->slot < T1_SPILL_SLOTS)
        return T1_X86_NREGS + l->slot;
    return T1_X86_NREGS + T1_SPILL_SLOTS;
}

/* First pending move at or after from, or 256. */
static unsigned copy_first(const uint64_t *bits, unsigned words, unsigned from)
{
    unsigned w = from / 64;
    if (w >= words)
        return 256;
    uint64_t b = bits[w] & (UINT64_MAX << (from % 64));
    for (;;) {
        if (b)
            return 64 * w + (unsigned)__builtin_ctzll(b);
        if (++w == words)
            return 256;
        b = bits[w];
    }
}

struct copy_move {
    struct t1_loc d, s;
    bool done;
};

/* Keep the reader graph and its work arrays out of the usual small-copy
 * path. Both resolvers consume the same compact, normalized move list. */
static __attribute__((noinline)) void pcopy_large(struct em *E, struct copy_move *mv, unsigned n)
{
    enum { NLOC = T1_X86_NREGS + T1_SPILL_SLOTS, SCRATCH = T1_X86_NREGS + T1_SPILL_SCRATCH };
    _Static_assert(NLOC <= UINT8_MAX, "parallel-copy location indexes fit in one byte");
    struct {
        uint8_t di, si;
        uint16_t dnext, snext;
    } link[256];
    uint16_t readers[NLOC + 1], dhead[NLOC + 1], shead[NLOC + 1];
    bool constants = true;
    for (unsigned k = 0; k < n; ++k)
        constants &= mv[k].s.kind == T1_LOC_CONST;
    if (constants) {
        for (unsigned k = 0; k < n; ++k)
            move(E, &mv[k].d, &mv[k].s);
        E->checking = false;
        return;
    }
    uint64_t ready[4] = {0}, pending[4] = {0};
    readers[SCRATCH] = 0;
    dhead[SCRATCH] = shead[SCRATCH] = UINT16_MAX;
    for (unsigned k = 0; k < n; ++k) {
        unsigned d = copy_location(&mv[k].d), s = copy_location(&mv[k].s);
        if (d == NLOC || d == SCRATCH || (s == NLOC && mv[k].s.kind != T1_LOC_CONST)) {
            em_fail(E, "parallel copy location", k);
            return;
        }
        link[k].di = (uint8_t)d;
        link[k].si = (uint8_t)s;
        /* Only entries touched by this group need initialization. */
        readers[d] = readers[s] = 0;
        dhead[d] = dhead[s] = shead[d] = shead[s] = UINT16_MAX;
    }
    for (unsigned k = 0; k < n; ++k) {
        unsigned d = link[k].di, s = link[k].si;
        ++readers[s];
        link[k].dnext = dhead[d];
        dhead[d] = (uint16_t)k;
        link[k].snext = shead[s];
        shead[s] = (uint16_t)k;
        pending[k / 64] |= UINT64_C(1) << (k % 64);
    }
    for (unsigned k = 0; k < n; ++k)
        if (!readers[link[k].di])
            ready[k / 64] |= UINT64_C(1) << (k % 64);
    unsigned words = (n + 63) / 64, cursor = 0;
    for (unsigned left = n; left;) {
        unsigned k = copy_first(ready, words, cursor);
        if (k == 256)
            k = copy_first(ready, words, 0);
        if (k != 256) {
            move(E, &mv[k].d, &mv[k].s);
            mv[k].done = true;
            uint64_t bit = UINT64_C(1) << (k % 64);
            ready[k / 64] &= ~bit;
            pending[k / 64] &= ~bit;
            unsigned source = link[k].si;
            if (!--readers[source])
                for (unsigned q = dhead[source]; q != UINT16_MAX; q = link[q].dnext)
                    if (!mv[q].done)
                        ready[q / 64] |= UINT64_C(1) << (q % 64);
            --left;
            cursor = k + 1;
            continue;
        }
        /* A cycle: save the same first pending destination as the direct
         * resolver. Each original source list is redirected at most once;
         * no legal destination is the reserved cycle-breaking slot. */
        k = copy_first(pending, words, 0);
        struct t1_loc scratch = {T1_LOC_SPILL, 0, T1_SPILL_SCRATCH, 0};
        unsigned saved = link[k].di;
        move(E, &scratch, &mv[k].d);
        for (unsigned q = shead[saved]; q != UINT16_MAX; q = link[q].snext)
            if (!mv[q].done) {
                mv[q].s = scratch;
                link[q].si = SCRATCH;
                --readers[saved];
                ++readers[SCRATCH];
            }
        for (unsigned q = dhead[saved]; q != UINT16_MAX; q = link[q].dnext)
            if (!mv[q].done)
                ready[q / 64] |= UINT64_C(1) << (q % 64);
        cursor = 0;
    }
    E->checking = false;
}

/* Sequentializes a parallel copy: a move whose destination no pending move
 * still reads goes first; a cycle is broken through scratch storage. */
static void pcopy(struct em *E, const struct t1_mir_insn *in)
{
    struct copy_move mv[256];
    unsigned n = 0;
    if (in->ncopies > 256) {
        em_fail(E, "parallel copy too large", in->ncopies);
        return;
    }
    uint16_t dregs = T1_X86_REGBIT(T1_R11);
    bool register_only = false, scratch_checked = false;
    for (uint32_t c = 0; c < in->ncopies; ++c) {
        const struct t1_mir_copy *cp = &E->m->copy[in->copies_first + c];
        if (loc_eq(&cp->dloc, &cp->sloc))
            continue;
        mv[n].d = cp->dloc;
        mv[n].s = cp->sloc;
        mv[n].done = false;
        if (cp->dloc.kind == T1_LOC_REG)
            dregs |= T1_X86_REGBIT(cp->dloc.reg);
        ++n;
    }
    allow(E, dregs, 0);
    if (n > 16) {
        pcopy_large(E, mv, n);
        return;
    }
    for (unsigned left = n; left;) {
        bool progress = false;
        for (unsigned k = 0; k < n; ++k) {
            if (mv[k].done)
                continue;
            bool read = false;
            for (unsigned q = 0; q < n && !read; ++q)
                if (q != k && !mv[q].done && loc_eq(&mv[q].s, &mv[k].d))
                    read = true;
            if (read)
                continue;
            move(E, &mv[k].d, &mv[k].s);
            mv[k].done = true;
            --left;
            progress = true;
        }
        if (progress || !left)
            continue;
        /* Only cycles need scratch. Test the original normalized moves
         * once, before any source is replaced with a scratch location. */
        if (!scratch_checked) {
            register_only = true;
            for (unsigned q = 0; q < n && register_only; ++q)
                register_only = mv[q].d.kind == T1_LOC_REG && mv[q].s.kind == T1_LOC_REG &&
                                mv[q].d.reg != T1_R11 && mv[q].s.reg != T1_R11;
            scratch_checked = true;
        }
        /* A cycle: park one destination's current value in scratch
         * storage and let its readers read it from there. */
        for (unsigned k = 0; k < n; ++k) {
            if (mv[k].done)
                continue;
            /* With register-only moves nothing can borrow R11 while it
             * holds a cycle value. A pending R11 source makes that
             * component acyclic, so it drains before the next cycle.
             * General copies retain the slot: memory-to-memory moves
             * and large constants also need R11. */
            struct t1_loc scratch = register_only
                                        ? (struct t1_loc){T1_LOC_REG, T1_R11, 0, 0}
                                        : (struct t1_loc){T1_LOC_SPILL, 0, T1_SPILL_SCRATCH, 0};
            move(E, &scratch, &mv[k].d);
            for (unsigned q = 0; q < n; ++q)
                if (!mv[q].done && loc_eq(&mv[q].s, &mv[k].d))
                    mv[q].s = scratch;
            break;
        }
    }
    E->checking = false;
}

static uint8_t reg_of(const struct t1_mir_opnd *o)
{
    if (o->kind == T1_MO_PREG)
        return (uint8_t)o->vreg;
    return o->fixed;
}

static void site(struct em *E, uint32_t i, const struct t1_mir_insn *in)
{
    unsigned flags = (unsigned)(in->opnd[2].imm & 0xff),
             bytes = (unsigned)((in->opnd[2].imm >> 8) & 0xff);
    unsigned s = (unsigned)((uint64_t)in->opnd[2].imm >> 16);
    bool store = flags & T1_SITE_STORE;
    uint8_t a = in->opnd[1].fixed, d = in->opnd[0].fixed;
    t1_value address = in->opnd[1].kind == T1_MO_VREG && in->opnd[1].vreg < E->m->nvregs
                           ? E->m->vreg[in->opnd[1].vreg].value
                           : T1_VALUE_NONE;
    bool reuse = !E->m->flags && address != T1_VALUE_NONE && address == E->admitted_address;
    unsigned delta_site = reuse ? E->admitted_site : s;
    uint32_t check = (uint32_t)E->b.size;
    allow(E, T1_X86_REGBIT(T1_R11) | (store ? 0 : T1_X86_REGBIT(d)), T1_XF_ALL);
    uint32_t miss1 = UINT32_MAX, miss2 = UINT32_MAX;
    if (!reuse && !E->fast_loop) {
        rr(E, T1_X86_MOV64_RR, T1_R11, a);
        ri(E, T1_X86_SHR64_RI, T1_R11, E->m->window_bits);
        rm(E, T1_X86_CMP64_RM, T1_R11, M(T1_R14, (int32_t)X_SITE_TAG(E->site_base + s)));
        miss1 = branch_later(E, T1_X86_JCC + T1_CC_NE);
    }
    if (!E->fast_loop && bytes > 1 && !(flags & T1_SITE_ALIGN_PROVEN) &&
        (!reuse || bytes > E->admitted_align)) {
        ri(E, T1_X86_TEST64_RI, a, bytes - 1);
        miss2 = branch_later(E, T1_X86_JCC + T1_CC_NE);
    }
    /* A completed same-address access admitted this tag. No intervening
     * operation can bind another window. Fault resolution never rebinds;
     * each follower retains its own access, permission check and fault RIP.
     * Reload the leader's delta: pure expansions may have overwritten R11. */
    rm(E, T1_X86_LOAD64, T1_R11, M(T1_R14, (int32_t)X_SITE_DELTA(E->site_base + delta_site)));
    struct t1_x86_mem at = {a, T1_R11, 0, 0};
    uint32_t pre = 0;
    if (store && (flags & T1_SITE_STORE_LOG) && (E->m->flags & T1_COMPILE_UNDO_LOG)) {
        /* Undo log: the bytes the store will replace,
         * zero-extended, through the same window address and with R11 only;
         * the store-log append after the store files them with its record.
         * The pre-load faults where the store would (same page, before any
         * effect) and resumes at itself (trap.c T1_RIP_PRE). */
        pre = (uint32_t)E->b.size;
        uint16_t op = bytes == 8   ? T1_X86_LOAD64
                      : bytes == 4 ? T1_X86_LOAD32
                      : bytes == 2 ? T1_X86_LOAD16ZX
                                   : T1_X86_LOAD8ZX;
        rm(E, op, T1_R11, at);
        mr(E, T1_X86_STORE64, M(T1_R14, (int32_t)X_UNDO_PRE), T1_R11);
        rm(E, T1_X86_LOAD64, T1_R11, M(T1_R14, (int32_t)X_SITE_DELTA(E->site_base + s)));
    }
    uint32_t host = (uint32_t)E->b.size;
    if (store) {
        uint16_t op = bytes == 8   ? T1_X86_STORE64
                      : bytes == 4 ? T1_X86_STORE32
                      : bytes == 2 ? T1_X86_STORE16
                                   : T1_X86_STORE8;
        mr(E, op, at, d);
    } else {
        uint16_t op = bytes == 8   ? T1_X86_LOAD64
                      : bytes == 4 ? T1_X86_LOAD32
                      : bytes == 2 ? T1_X86_LOAD16ZX
                                   : T1_X86_LOAD8ZX;
        rm(E, op, d, at);
    }
    E->checking = false;
    struct t1_emit_point *p = &E->o->pt[in->point];
    if (E->fast_loop)
        p->fast_offset = host;
    else {
        p->host_offset = host;
        p->pre_offset = pre;
        p->addr_reg = a;
    }
    E->admitted_address = address;
    E->admitted_site = delta_site;
    E->admitted_align = reuse && E->admitted_align > bytes ? E->admitted_align : bytes;
    if (miss1 == UINT32_MAX && miss2 == UINT32_MAX)
        return;
    if (E->ncold >= sizeof(E->cold) / sizeof(E->cold[0])) {
        em_fail(E, "cold stub limit", 0);
        return;
    }
    /* Tag and alignment misses have the same point and retry address. */
    E->cold[E->ncold++] = (struct cold){COLD_MISS, in->point, {miss1, miss2}, check};
    (void)i;
}

static void slog(struct em *E, const struct t1_mir_insn *in)
{
    uint8_t a = in->opnd[0].fixed, v = in->opnd[1].fixed;
    unsigned point = (unsigned)in->opnd[2].imm;
    bool store = point < E->m->ir->npoints && E->m->ir->point[point].access_write;
    bool undo = store && (E->m->flags & T1_COMPILE_UNDO_LOG) != 0;
    /* The undo copy borrows R13 (the budget counter, never allocated) for
     * two instructions and restores it: the expansion's net writes are R11
     * and the flags, as its MIR row declares, but the check sees R13. */
    allow(E, T1_X86_REGBIT(T1_R11) | (undo ? T1_X86_REGBIT(T1_R13) : 0), T1_XF_ALL);
    rm(E, T1_X86_LOAD64, T1_R11, M(T1_R14, (int32_t)X_SLOG_N));
    mr(E, T1_X86_STORE64, ((struct t1_x86_mem){T1_R14, T1_R11, 3, (int32_t)X_SLOG_ADDR}), a);
    mr(E, T1_X86_STORE64, ((struct t1_x86_mem){T1_R14, T1_R11, 3, (int32_t)X_SLOG_VAL}), v);
    mi(E, T1_X86_STORE32_I32, ((struct t1_x86_mem){T1_R14, T1_R11, 2, (int32_t)X_SLOG_POINT}),
       in->opnd[2].imm);
    if (undo) {
        mr(E, T1_X86_STORE64, M(T1_R14, (int32_t)X_UNDO_R13), T1_R13);
        rm(E, T1_X86_LOAD64, T1_R13, M(T1_R14, (int32_t)X_UNDO_PRE));
        mr(E, T1_X86_STORE64, ((struct t1_x86_mem){T1_R14, T1_R11, 3, (int32_t)X_SLOG_PRE}),
           T1_R13);
        rm(E, T1_X86_LOAD64, T1_R13, M(T1_R14, (int32_t)X_UNDO_R13));
    }
    struct t1_x86_insn inc = I(T1_X86_INC64_R);
    inc.opnd[0].reg = T1_R11;
    put(E, &inc);
    ri(E, T1_X86_AND64_RI, T1_R11, X_SLOG_MASK);
    mr(E, T1_X86_STORE64, M(T1_R14, (int32_t)X_SLOG_N), T1_R11);
    m_only(E, T1_X86_INC64_M, M(T1_R14, (int32_t)X_SLOG_TOTAL));
    E->checking = false;
}

/* The helper point's alloc (form.h T1_IC_ALLOC): 96 less the top of its
 * new frame above the entry base, so it spills (SDM Vol.2 §6.5) exactly
 * when x->entry_dirty exceeds the result; -1 for another helper. */
static int alloc_room(const struct t1_mir *m, uint32_t point)
{
    const struct t1_form *f = m->ir->form;
    if (!f || point >= m->ir->npoints || m->ir->point[point].insn >= f->ninsns)
        return -1;
    const struct t1_insn *ti = &f->insn[m->ir->point[point].insn];
    if (ti->cls != T1_IC_ALLOC)
        return -1;
    const struct t1_fstate *fs = &f->fs[ti->fs_taken];
    return IA64_N_STACKED_PHYS - (int)(fs->delta + (fs->cfm & 127));
}

/* An exit never resumes its body: capture exactly the recovery frame's
 * register sources. Helper/miss wrappers retain their full live-through
 * save set, and trap exits already have the complete trap capture. */
static void capture_point(struct em *E, uint32_t point)
{
    const struct t1_mir_point *p = &E->m->point[point];
    uint16_t regs = 0;
    for (unsigned k = 0; k < p->frame_count; ++k) {
        const struct t1_loc *l = &E->m->frame_loc[p->frame_first + k];
        if (l->kind == T1_LOC_REG || l->kind == T1_LOC_REG_PLUS)
            regs |= T1_X86_REGBIT(l->reg);
    }
    for (unsigned r = 0; r < T1_X86_NREGS; ++r)
        if (regs & T1_X86_REGBIT(r))
            mr(E, T1_X86_STORE64, M(T1_R14, (int32_t)(X_REGS + 8u * r)), (uint8_t)r);
    mi(E, T1_X86_STORE32_I32, M(T1_R14, (int32_t)X_POINT), point);
    branch_abs(E, T1_X86_JMP, t1_stub.exit_nosave);
}

static void expand(struct em *E, uint32_t i)
{
    const struct t1_mir *m = E->m;
    const struct t1_mir_insn *in = &m->insn[i];
    switch (in->op) {
    case T1_MIR_HREAD:
        allow(E, T1_X86_REGBIT(in->opnd[0].fixed), T1_XF_NONE);
        rm(E, T1_X86_LOAD64, in->opnd[0].fixed,
           M(T1_R14, (int32_t)(offsetof(struct t1_exec, helper_out) + 8 * in->opnd[1].imm)));
        E->checking = false;
        break;
    case T1_MIR_BSPCALL:
        allow(E, 0, T1_XF_NONE);
        mi(E, T1_X86_STORE32_I32, M(T1_R14, (int32_t)offsetof(struct t1_exec, bsp_called)), 1);
        E->checking = false;
        break;
    case T1_MIR_HGUARD: {
        allow(E, T1_R11_ONLY, T1_XF_ALL);
        rm(E, T1_X86_LOAD32, T1_R11, M(T1_R14, (int32_t)offsetof(struct t1_exec, helper_exit)));
        ri(E, T1_X86_CMP64_RI, T1_R11, 0);
        uint32_t skip = branch_later(E, T1_X86_JCC + T1_CC_E);
        capture_point(E, in->point);
        patch(E, skip, (uint32_t)E->b.size);
        E->checking = false;
        break;
    }
    case T1_MIR_LIVEIN:
        allow(E, T1_X86_REGBIT(in->opnd[0].fixed) | T1_X86_REGBIT(T1_R11), T1_XF_ALL);
        load_home(E, in->opnd[0].fixed, (uint16_t)in->opnd[1].imm);
        E->checking = false;
        break;
    case T1_MIR_PCOPY:
        pcopy(E, in);
        break;
    case T1_MIR_CHARGE:
        allow(E, T1_X86_REGBIT(T1_R13), T1_XF_ALL);
        ri(E, T1_X86_SUB64_RI, T1_R13, in->opnd[0].imm);
        E->checking = false;
        break;
    case T1_MIR_CHECKPOINT: {
        allow(E, T1_X86_REGBIT(T1_R11), T1_XF_ALL);
        if (m->flags & T1_COMPILE_VERIFY)
            m_only(E, T1_X86_DEC64_M, M(T1_R14, (int32_t)X_FORCE(in->point))); /* back-edge count */
        ri(E, T1_X86_CMP64_RI, T1_R13, in->opnd[0].imm);
        uint32_t f1 = branch_later(E, T1_X86_JCC + T1_CC_B);
        /* requested_exit and irq_kick in one load (exec.h T1_VCPU_CKPT). */
        rm(E, T1_X86_LOAD64, T1_R11, M(T1_R12, (int32_t)T1_VCPU_CKPT));
        rr(E, T1_X86_TEST64_RR, T1_R11, T1_R11);
        uint32_t f2 = branch_later(E, T1_X86_JCC + T1_CC_NE);
        E->checking = false;
        if (E->ncold >= sizeof(E->cold) / sizeof(E->cold[0])) {
            em_fail(E, "cold stub limit", 0);
            return;
        }
        /* Both checks leave through the same checkpoint frame. */
        E->cold[E->ncold++] = (struct cold){COLD_CKPT, in->point, {f1, f2}, 0};
        break;
    }
    case T1_MIR_EXIT:
        allow(E, 0, 0);
        capture_point(E, in->point);
        E->checking = false;
        break;
    case T1_MIR_FORCE: {
        allow(E, 0, T1_XF_ALL);
        m_only(E, T1_X86_DEC64_M, M(T1_R14, (int32_t)X_FORCE(in->point)));
        uint32_t f = branch_later(E, T1_X86_JCC + T1_CC_E);
        E->checking = false;
        bool helper = m->point[in->point].kind == T1_PT_HELPER;
        if (E->ncold >= sizeof(E->cold) / sizeof(E->cold[0])) {
            em_fail(E, "cold stub limit", 0);
            return;
        }
        /* A forced trap resumes right after the counter (one dynamic
         * occurrence decrements it once). Between the counter and the
         * site's tag check there may be stores of values evicted by the
         * site's own result (the allocator never places anything else
         * there); they change no register, so the frame is the same at
         * both RIPs, and they must run on the retry too. */
        E->o->pt[in->point].force_resume = (uint32_t)E->b.size;
        E->cold[E->ncold++] = (struct cold){helper ? COLD_FORCE_HELPER : COLD_FORCE_SITE,
                                            in->point,
                                            {f, UINT32_MAX},
                                            (uint32_t)E->b.size};
        break;
    }
    case T1_MIR_SITE:
        site(E, i, in);
        break;
    case T1_MIR_SLOG:
        slog(E, in);
        break;
    case T1_MIR_HELPER: {
        allow(E, T1_WRAPPER_CLOBBERS, T1_XF_ALL);
        struct fpfast F = {.E = E, .point = in->point};
        size_t start = E->b.size;
        bool fast = in->nopnd > 3 && in->opnd[3].kind == T1_MO_IMM && in->opnd[3].imm &&
                    fp_fast(&F, &m->ir->form->insn[m->ir->op[m->ir->point[in->point].op].imm]);
        if (!fast)
            E->b.size = start; /* none or unusable locations: the plain call */
        for (unsigned k = 0; fast && k < F.nbail; ++k)
            patch(E, F.bail[k], (uint32_t)E->b.size);
        /* An alloc calls its helper only when it spills. */
        int room = alloc_room(m, in->point);
        uint32_t skip = UINT32_MAX;
        if (room >= 0) {
            rm(E, T1_X86_LOAD32, T1_R11, M(T1_R14, (int32_t)X_ENTRY_DIRTY));
            ri(E, T1_X86_CMP32_RI, T1_R11, room);
            skip = branch_later(E, T1_X86_JCC + T1_CC_BE);
        }
        mi(E, T1_X86_STORE32_I32, M(T1_R14, (int32_t)X_POINT), in->point);
        call_wrapper(E, t1_stub.helper);
        patch(E, skip, (uint32_t)E->b.size);
        for (unsigned k = 0; fast && k < F.ndone; ++k)
            patch(E, F.done[k], (uint32_t)E->b.size);
        E->checking = false;
        break;
    }
    case T1_MIR_FENCE: {
        allow(E, 0, 0);
        struct t1_x86_insn f = I(T1_X86_MFENCE);
        put(E, &f);
        E->checking = false;
        break;
    }
    case T1_MIR_BLOCK:
        break;
    default:
        em_fail(E, "pseudo-op without expansion", in->op);
    }
}

static uint16_t branch_target(const struct em *E, unsigned b)
{
    b = E->alias[b];
    return (uint16_t)(b + (E->fast_loop && E->window.member[b] ? E->m->nblocks : 0));
}

/* An x86 MIR instruction with its allocated operands. */
static void x86(struct em *E, uint32_t i, uint16_t next_block)
{
    const struct t1_mir *m = E->m;
    const struct t1_mir_insn *in = &m->insn[i];
    const struct t1_x86_info *x = t1_x86_info(in->op);
    if (x->attr & T1_XA_BRANCH) {
        uint16_t target = branch_target(E, (unsigned)in->opnd[0].imm);
        if (in->op == T1_X86_JMP) {
            if (target != next_block)
                branch_block(E, T1_X86_JMP, target);
            return;
        }
        /* jcc A ; jmp B: with A next, the inverse jcc to B. */
        const struct t1_mir_insn *nx = i + 1 < m->ninsns ? &m->insn[i + 1] : NULL;
        if (in->op >= T1_X86_JCC && in->op < T1_X86_JCC + 16 && nx && nx->op == T1_X86_JMP &&
            target == next_block) {
            uint16_t other = branch_target(E, (unsigned)nx->opnd[0].imm);
            if (other != next_block)
                branch_block(E, (uint16_t)(T1_X86_JCC + ((in->op - T1_X86_JCC) ^ 1)), other);
            E->skip_next = true;
            return;
        }
        branch_block(E, in->op, target);
        return;
    }
    struct t1_x86_insn e = I(in->op);
    for (unsigned k = 0; k < x->nopnd && k < 2; ++k) {
        const struct t1_mir_opnd *o = &in->opnd[k];
        switch (o->kind) {
        case T1_MO_VREG:
        case T1_MO_PREG:
            e.opnd[k].reg = reg_of(o);
            break;
        case T1_MO_IMM:
            e.opnd[k].imm = o->imm;
            break;
        case T1_MO_MEM:
            e.mem.base = in->mem.base_kind == T1_MO_VREG   ? m->memreg[i].base
                         : in->mem.base_kind == T1_MO_PREG ? (uint8_t)in->mem.base
                                                           : NOREG;
            e.mem.index = in->mem.index_kind == T1_MO_VREG   ? m->memreg[i].index
                          : in->mem.index_kind == T1_MO_PREG ? (uint8_t)in->mem.index
                                                             : NOREG;
            e.mem.scale_shift = in->mem.scale_shift;
            e.mem.disp = in->mem.disp;
            break;
        default:
            break;
        }
    }
    if (x->opnd[1] == T1_XO_CL)
        e.opnd[1].reg = T1_RCX;
    /* A copy the allocator coalesced. */
    if (in->op == T1_X86_MOV64_RR && e.opnd[0].reg == e.opnd[1].reg)
        return;
    put(E, &e);
}

/* ---- cold stubs ---- */

static void cold_stubs(struct em *E)
{
    for (unsigned k = 0; k < E->ncold && !E->failed; ++k) {
        const struct cold *c = &E->cold[k];
        uint32_t here = (uint32_t)E->b.size;
        patch(E, c->from_fix[0], here);
        patch(E, c->from_fix[1], here);
        switch (c->kind) {
        case COLD_MISS:
            mi(E, T1_X86_STORE32_I32, M(T1_R14, (int32_t)X_POINT), c->point);
            call_wrapper(E, t1_stub.miss);
            patch(E, branch_later(E, T1_X86_JMP), c->check);
            break;
        case COLD_FORCE_SITE: {
            /* TRAP actions fault here (force_offset) and resume at the tag
             * check; MISS_EXIT reads a valid word instead (x->poison set by
             * the dispatcher) and invalidates the site's tag. */
            const struct t1_mir_insn *s = &E->m->insn[E->m->point[c->point].insn];
            unsigned site_idx = (unsigned)((uint64_t)s->opnd[2].imm >> 16);
            rm(E, T1_X86_LOAD64, T1_R11, M(T1_R14, (int32_t)X_POISON));
            E->o->pt[c->point].force_offset = (uint32_t)E->b.size;
            rm(E, T1_X86_LOAD64, T1_R11, M(T1_R11, 0));
            mi(E, T1_X86_STORE64_I32, M(T1_R14, (int32_t)X_SITE_TAG(E->site_base + site_idx)), -1);
            patch(E, branch_later(E, T1_X86_JMP), c->check);
            break;
        }
        case COLD_FORCE_HELPER:
        case COLD_CKPT:
            capture_point(E, c->point);
            break;
        }
    }
}

/* ---- driver ---- */

static uint16_t resolve_alias(const struct t1_mir *m, uint16_t b, unsigned depth)
{
    const struct t1_mir_block *blk = &m->block[b];
    if (depth > m->nblocks || blk->count != 1)
        return b;
    const struct t1_mir_insn *in = &m->insn[blk->first];
    if (in->op != T1_X86_JMP)
        return b;
    return resolve_alias(m, (uint16_t)in->opnd[0].imm, depth + 1);
}

/* These guards only read cached admission. A failure takes the original
 * loop without translating or touching a later guest access early. */
static void window_guard(struct em *E)
{
    const struct t1_window_loop *p = &E->window;
    unsigned first = E->site_base + p->access[0].site;
    unsigned second = E->site_base + p->access[1].site;
    rm(E, T1_X86_LOAD64, T1_R11, M(T1_R14, (int32_t)X_SITE_TAG(first)));
    rm(E, T1_X86_CMP64_RM, T1_R11, M(T1_R14, (int32_t)X_SITE_TAG(second)));
    branch_block(E, T1_X86_JCC + T1_CC_NE, p->header);
    for (unsigned k = 0; k < 2; ++k) {
        uint8_t a = p->access[k].reg;
        unsigned site = E->site_base + p->access[k].site;
        if (p->access[k].bytes > 1) {
            ri(E, T1_X86_TEST64_RI, a, p->access[k].bytes - 1);
            branch_block(E, T1_X86_JCC + T1_CC_NE, p->header);
        }
        rr(E, T1_X86_MOV64_RR, T1_R11, a);
        ri(E, T1_X86_SHR64_RI, T1_R11, E->m->window_bits);
        rm(E, T1_X86_CMP64_RM, T1_R11, M(T1_R14, (int32_t)X_SITE_TAG(site)));
        branch_block(E, T1_X86_JCC + T1_CC_NE, p->header);
        rm(E, T1_X86_LEA64, T1_R11, M(a, (int32_t)p->access[k].span));
        ri(E, T1_X86_SHR64_RI, T1_R11, E->m->window_bits);
        rm(E, T1_X86_CMP64_RM, T1_R11, M(T1_R14, (int32_t)X_SITE_TAG(site)));
        branch_block(E, T1_X86_JCC + T1_CC_NE, p->header);
    }
    branch_block(E, T1_X86_JMP, (uint16_t)(p->header + E->m->nblocks));
}

static bool emit_attempt(const struct t1_mir *m, struct t1_emitted *o, char *err, size_t err_size,
                         unsigned site_base, const struct t1_window_loop *plan)
{
    struct t1_workspace *ws = m->ir->ws;
    memset(o, 0, sizeof(*o));
    o->ws = ws;
    bool versioned = plan != NULL;
    struct t1_window_loop window;
    if (versioned)
        window = *plan;
    o->cap = (size_t)m->ninsns * 48 + (size_t)m->ir->npoints * 64 + 8192 + (versioned ? 8192 : 0);
    o->pages = (o->cap + 4095) / 4096;
    o->buf = t1_ws_alloc(ws, o->pages, 0);
    o->pt_pages =
        ((m->ir->npoints ? m->ir->npoints : 1) * sizeof(struct t1_emit_point) + 4095) / 4096;
    o->pt = t1_ws_alloc(ws, o->pt_pages, HAL_PAGE_ZERO);
    /* The output arrays outlive the emitter's state and block arrays. */
    size_t ep = (sizeof(struct em) + 4095) / 4096;
    struct em *E = o->buf && o->pt ? t1_ws_alloc(ws, ep, 0) : NULL;
    unsigned nb = m->nblocks * (versioned ? 2 : 1);
    size_t bp = (nb * sizeof(uint32_t) + m->nblocks * sizeof(uint16_t) + 4095) / 4096;
    uint8_t *blocks = E ? t1_ws_alloc(ws, bp ? bp : 1, 0) : NULL;
    if (!blocks) {
        t1_ws_free(ws, E, ep);
        t1_emit_free(o);
        if (err_size)
            vmm_snprintf(err, err_size, "emit: no memory");
        return false;
    }
    memset(E, 0, offsetof(struct em, fix));
    E->m = m;
    E->o = o;
    E->err = err;
    E->err_size = err_size;
    E->site_base = site_base;
    E->versioned = versioned;
    if (versioned)
        E->window = window;
    E->block_off = (uint32_t *)blocks;
    E->alias = (uint16_t *)(blocks + nb * sizeof(uint32_t));
    E->b = (struct t1_x86_buf){o->buf, 0, o->cap, false};
    for (unsigned b = 0; b < nb; ++b)
        E->block_off[b] = UINT32_MAX;
    for (unsigned b = 0; b < m->nblocks; ++b) {
        E->alias[b] = resolve_alias(m, (uint16_t)b, 0);
    }

    /* Prologue: not MIR, before any value exists. */
    static const uint8_t saved[] = {T1_RBX, T1_RBP, T1_R12, T1_R13, T1_R14, T1_R15};
    for (unsigned k = 0; k < sizeof(saved); ++k) {
        struct t1_x86_insn p = I(T1_X86_PUSH64_R);
        p.opnd[0].reg = saved[k];
        put(E, &p);
    }
    ri(E, T1_X86_SUB64_RI, T1_RSP, 8);
    rr(E, T1_X86_MOV64_RR, T1_R12, T1_RDI);
    rr(E, T1_X86_MOV64_RR, T1_R13, T1_RSI);
    rr(E, T1_X86_MOV64_RR, T1_R14, T1_RDX);
    o->native_entry = (uint32_t)E->b.size;

    /* loop_start/loop_end: the first innermost loop body:
     * the blocks of the first deepest loop, from its first to its last. */
    o->loop_start = o->loop_end = 0;
    bool in_loop_seen = false;
    uint8_t deepest = 0;
    uint16_t innermost = T1_NO_BLOCK;
    for (unsigned b = 0; b < m->nblocks; ++b)
        if (m->block[b].depth > deepest) {
            deepest = m->block[b].depth;
            innermost = m->block[b].loop_header;
        }
    for (unsigned pass = 0; pass < (versioned ? 2u : 1u); ++pass) {
        E->fast_loop = pass != 0;
        if (pass)
            in_loop_seen = false;
        for (unsigned b = 0; b < m->nblocks && !E->failed; ++b) {
            if (E->alias[b] != b || (pass && !window.member[b]))
                continue;
            /* The next emitted block (aliases emit nothing). */
            uint16_t next = UINT16_MAX;
            for (unsigned n = b + 1; n < m->nblocks; ++n)
                if (E->alias[n] == n && (!pass || window.member[n])) {
                    next = (uint16_t)(n + pass * m->nblocks);
                    break;
                }
            const struct t1_mir_block *blk = &m->block[b];
            E->block_off[b + pass * m->nblocks] = (uint32_t)E->b.size;
            bool innermost_block = blk->loop && blk->loop_header == innermost;
            if (innermost_block && !in_loop_seen) {
                o->loop_start = (uint32_t)E->b.size;
                in_loop_seen = true;
            }
            E->skip_next = false;
            E->admitted_address = T1_VALUE_NONE;
            for (uint32_t i = blk->first; i < blk->first + blk->count && !E->failed; ++i) {
                if (E->skip_next) {
                    E->skip_next = false;
                    continue;
                }
                if (!pass && versioned && b == window.entry && i == blk->term)
                    window_guard(E);
                unsigned op = m->insn[i].op;
                if (t1_mir_is_pseudo(op)) {
                    if (op != T1_MIR_SITE && op != T1_MIR_CHARGE && op != T1_MIR_LIVEIN &&
                        op != T1_MIR_PCOPY && op != T1_MIR_FENCE)
                        E->admitted_address = T1_VALUE_NONE;
                    expand(E, i);
                } else {
                    if (t1_x86_info(op)->attr & (T1_XA_CALL | T1_XA_BRANCH))
                        E->admitted_address = T1_VALUE_NONE;
                    x86(E, i, next);
                }
            }
            if (innermost_block)
                o->loop_end = (uint32_t)E->b.size;
        }
    }
    E->fast_loop = false;
    for (unsigned b = 0; b < m->nblocks; ++b)
        if (E->alias[b] != b) {
            E->block_off[b] = E->block_off[E->alias[b]];
            if (versioned && window.member[b])
                E->block_off[b + m->nblocks] = E->block_off[E->alias[b] + m->nblocks];
        }
    for (unsigned k = 0; k < E->nfix && !E->failed; ++k) {
        uint32_t t = E->block_off[E->fix[k].block];
        if (t == UINT32_MAX) {
            em_fail(E, "branch to an unplaced block", E->fix[k].block);
            break;
        }
        patch(E, E->fix[k].at, t);
    }
    cold_stubs(E);
    o->size = E->b.size;
    if (!E->failed)
        t1_restore_emit(m, o);
    o->site_base = E->site_base;
    /* Static host instruction count of the loop body. */
    o->loop_host_insns = 0;
    for (uint32_t at = o->loop_start; at < o->loop_end && !E->failed;) {
        struct t1_x86_decoded d;
        if (!t1_x86_decode(o->buf + at, o->loop_end - at, &d)) {
            em_fail(E, "undecodable loop byte", at);
            break;
        }
        at += d.length;
        ++o->loop_host_insns;
    }
    bool failed = E->failed;
    t1_ws_free(ws, blocks, bp ? bp : 1);
    t1_ws_free(ws, E, ep);
    if (failed) {
        t1_emit_free(o);
        return false;
    }
    return true;
}

bool t1_emit(const struct t1_mir *m, struct t1_emitted *o, char *err, size_t err_size)
{
    unsigned site_base = t1_site_base(m->ir->nsites, m->flags);
    struct t1_window_loop window;
    if (!t1_window_loop_plan(m, &window))
        return emit_attempt(m, o, err, err_size, site_base, NULL);
    if (emit_attempt(m, o, err, err_size, site_base, &window))
        return true;
    /* Optional cloning must not reject an otherwise compilable region. */
    if (err_size)
        err[0] = 0;
    return emit_attempt(m, o, err, err_size, site_base, NULL);
}

void t1_emit_place(const struct t1_emitted *e, uint8_t *dst)
{
    memcpy(dst, e->buf, e->size);
    for (unsigned k = 0; k < e->nabs; ++k) {
        uint32_t at = e->abs_fix[k];
        int64_t rel = (int64_t)e->abs_target[k] - (int64_t)((uintptr_t)dst + at + 4);
        int32_t r32 = (int32_t)rel;
        memcpy(dst + at, &r32, 4);
    }
}

void t1_emit_free(struct t1_emitted *e)
{
    if (e->pt)
        t1_ws_free(e->ws, e->pt, e->pt_pages);
    if (e->buf)
        t1_ws_free(e->ws, e->buf, e->pages);
    e->buf = NULL;
    e->pt = NULL;
}
