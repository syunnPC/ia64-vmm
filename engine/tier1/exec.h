/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Generated-code/runtime contract: R14-relative execution context,
 * point and frame records, budget accounting and runtime stub ABI.
 */
#ifndef IA64VMM_TIER1_EXEC_H
#define IA64VMM_TIER1_EXEC_H
#include "engine/tier1/tier1.h"
#include "engine/hostwin/hostwin.h"

/* ---- point and frame records ---------------------------------- */

/* enum t1_point_kind (exit, checkpoint, site, helper) is public: tier1.h. */
enum t1_point_timing { T1_BEFORE_EFFECT, T1_AFTER_COMMIT, T1_AFTER_COMPLETION };
/*
 * Where a frame element's value is at a point. HOME: the home already holds
 * it (only under the must-analysis, see below). CONST: value. REG: x->regs[reg]. SPILL:
 * x->spill[slot]. REG_PLUS/SPILL_PLUS: that plus value (mod 2^64), e.g. a
 * pre-increment base recovered from the post-incremented register; this
 * addition is the only recomputation restoration performs.
 */
enum t1_loc_kind {
    T1_LOC_HOME,
    T1_LOC_CONST,
    T1_LOC_REG,
    T1_LOC_SPILL,
    T1_LOC_REG_PLUS,
    T1_LOC_SPILL_PLUS
};
struct t1_loc {
    uint8_t kind, reg;
    uint16_t slot;
    uint64_t value;
}; /* constant or addend */
/* element: an architectural element id (tier1.h T1_ELEMENT_*).
 * Frames cover possible architectural writes and W, the homes helpers may
 * change through operand materialization or instruction outputs. A frame
 * may source LIVEIN(e) from HOME only when every path to the point keeps
 * that original home intact, including earlier loop iterations. The IR
 * verifier checks this independently. Other frame values are pseudo-uses
 * through DCE and allocation. Flags are never in a frame. */
struct t1_frame_entry {
    uint16_t element;
    uint8_t kind, reg;
    uint16_t slot;
    uint64_t value;
};
_Static_assert(sizeof(struct t1_frame_entry) == 16, "compact immutable frame entry");

static inline struct t1_frame_entry t1_frame_entry_make(uint16_t element, const struct t1_loc *loc)
{
    return (struct t1_frame_entry){element, loc->kind, loc->reg, loc->slot, loc->value};
}

static inline struct t1_loc t1_frame_loc(const struct t1_frame_entry *f)
{
    return (struct t1_loc){f->kind, f->reg, f->slot, f->value};
}
/*
 * Timing and resume target per kind:
 *   EXIT        AFTER_COMPLETION  target ip/slot of the exit
 *   CHECKPOINT  AFTER_COMPLETION  the region header
 *   SITE        BEFORE_EFFECT     the memory instruction itself
 *   HELPER      BEFORE_EFFECT     the helper instruction itself
 *   AFTER_COMMIT is not produced (reserved for RSE/atomics).
 */
struct t1_point {
    uint8_t kind, timing;
    uint8_t access_size, access_write,
        addr_reg;         /* sites: width, direction, host reg of the guest address */
    uint8_t end;          /* EXIT: enum t1_end_class */
    uint16_t site;        /* sites: region site k (slot region->site_base + k) */
    uint32_t host_offset; /* sites: offset of the access instruction */
    uint32_t force_offset, force_resume; /* verification: forced-trap RIP and resume RIP */
    uint32_t pre_offset;                 /* undo log: the store's pre-load, or 0 */
    uint64_t ip;
    uint8_t slot;            /* resume target (BEFORE_EFFECT: the instruction) */
    uint16_t retired_offset; /* retired count at the point */
    uint32_t frame_first, frame_count;
    uint32_t restore_offset; /* native home restore in region code, 0: use the table */
    uint32_t frame_restore;  /* the first frame_restore entries are not HOME;
                                                      the HOME entries follow them */
    uint16_t insn;           /* index into the region's instruction copy */
    /* Restore plan: the entries to restore in the order
     * static GR, stacked GR, static NaT, stacked NaT, PR, LC, LSB, other; plan[k] is
     * the end of class k (0..6), pr_mask the PR bits they restore. */
    uint16_t plan[7];
    uint64_t pr_mask;
    /* The register-stack frame at the point: its frame
     * base lies delta registers above the entry's (registers that calls
     * inside the region preserved), and its CFM is cfm with rse set, the
     * entry's without. */
    uint64_t cfm;
    uint8_t delta, rse;
};

/* ---- struct t1_exec: per-vCPU execution context, R14 ---------------- */

/* Window tag slots per vCPU. Each production region uses its own range of
 * slots (t1_site_base), so regions entered alternately keep their tags. */
#define T1_SITE_SLOTS 512u
/* Native access-log ring entries; a power of two. */
#define T1_SLOG_ENTRIES 256u
#define T1_SLOG_MASK (T1_SLOG_ENTRIES - 1u)
/* Spill slots: R14-relative, never on the stack. */
#define T1_SPILL_SLOTS 64u
/* Values of struct t1_exec.force[] for unarmed points: never reaches 0. */
#define T1_FORCE_UNARMED 0u
/* Entry-image slots: every stacked register's value
 * and NaT at most once each. */
#define T1_MAX_FRAME_IN (2u * IA64_N_STACKED_PHYS)

struct t1_region;
struct t1_exec;
struct t1_fast_node;
/* Shared-stub callback: finish the captured point and return the next
 * native body address, or zero to unwind the outer entry. */
typedef uintptr_t (*t1_handoff_fn)(struct t1_exec *x);
struct t1_exec {
    /* ---- addressed by generated code and the runtime stubs ---- */
    /* Captured host GPRs, AMD64 numbering (x86enc.h). Generated exits store
     * their frame's register sources; miss/helper stubs preserve all 11
     * allocatable registers for retry; the trap client
     * copies all 16 from struct hostwin_trap. Frames name only allocatable
     * registers. */
    uint64_t regs[16];
    uint64_t rflags, rip; /* trap capture; RIP = resume target after a fill */
    uint64_t raw_r13;     /* R13 at the point */
    uint32_t point;       /* point id of the exit/miss/trap being handled */
    /* The dirty registers below the entry's frame base: at entry
     * v->s.rse_dirty; an in-region alloc that spills lowers it (helper.c),
     * so a point's dirty partition is entry_dirty plus its delta. Each
     * alloc's code compares it before its helper (emit.c). */
    uint32_t entry_dirty;
    /* A taken call skips an entry RNaT collection slot even when sol=0
     * (SDM Vol.2 Table 6-2); matched returns do not undo that step. */
    uint32_t bsp_called;
    /* Tier1 owns observed_epoch; every site[] tag is valid for that epoch.
     * Entry invalidates all tags when hostwin_bind_epoch(hw) changes.
     * t1_window_miss binds into a temporary, invalidates on epoch change,
     * then publishes the current site. No bind occurs before its access.
     * Site k of region r uses slot r->site_base + k. */
    uint64_t observed_epoch;
    uint64_t poison;                        /* hostwin_poison_va(): forced traps load from it */
    struct hostwin_tag site[T1_SITE_SLOTS]; /* tag compared by `cmp r11, [r14 + site[s].tag]` */
    uint64_t spill[T1_SPILL_SLOTS];
    /* Entry image of the region's stacked LIVEIN elements
     * (frame-relative homes). Slot k belongs to the region's k-th
     * stacked LIVEIN element, a value T1_ELEMENT_GR(32 + o) or a NaT
     * T1_ELEMENT_NAT(32 + o) (o: frame offset after rotation); the
     * dispatcher fills it from the homes at v->s.rse_bof before the entry,
     * values as 64 bits and NaTs as 0 or 1 in the slot's low byte. Generated
     * code reads stacked LIVEINs only from here (`[r14 + X_FRAME_IN(k)]`)
     * and never addresses a stacked home. */
    uint64_t frame_in[T1_MAX_FRAME_IN];
    /* Verification builds only (T1_COMPILE_VERIFY / T1_COMPILE_STORE_LOG). */
    uint64_t force[T1_MAX_POINTS]; /* remaining count of the armed occurrence; 0 = unarmed */
    uint64_t slog_n;               /* next ring index, masked by T1_SLOG_MASK */
    uint64_t slog_total;           /* records appended since entry; > T1_SLOG_ENTRIES = overflow */
    uint64_t slog_addr[T1_SLOG_ENTRIES];  /* pre-increment guest address */
    uint64_t slog_val[T1_SLOG_ENTRIES];   /* stored value, or loaded bytes with ACCESS_LOG */
    uint32_t slog_point[T1_SLOG_ENTRIES]; /* width is a property of the point */
    /* T1_COMPILE_UNDO_LOG: the bytes the
     * store of record i replaced, zero-extended from its width. The site
     * loads them into undo_pre right before the store (R11 only); the
     * store-log append copies undo_pre into slog_pre[i], borrowing R13 for
     * two instructions and restoring it from undo_r13 (no fault or exit
     * can happen in between: only R14-relative accesses). */
    uint64_t slog_pre[T1_SLOG_ENTRIES];
    uint64_t undo_pre, undo_r13;

    /* ---- runtime bookkeeping ----
     * The shared exit stub also reads budget_entry and handoff. */
    struct vcpu *v;
    struct t1_vcpu *tv;
    struct hostwin_vcpu *hw;
    const struct t1_region *region;
    uint64_t budget_entry;  /* R13 passed to the region entry */
    uint64_t retired_entry; /* v->retired at region entry */
    /* The register stack at region entry: frame base (homes of stacked
     * elements throughout the region) and ar.bsp. */
    uint64_t entry_bsp, entry_cfm;
    unsigned entry_bof;
    /* Cached helper results, indexed by frame-relative element. FR and
     * non-LC AR homes remain authoritative. */
    uint64_t helper_out[T1_ELEMENT_LIMIT];
    /* Region id that published site[s]'s tag (window-miss statistics). */
    uint32_t site_owner[T1_SITE_SLOTS];
    uint32_t helper_exit;
    uint32_t helper_notification; /* source helper point + 1, zero if none */
    t1_handoff_fn handoff;
    void *handoff_arg;
    struct t1_fast_node *fast;
};

/* R14-relative displacements used by the emitter and the stubs. */
#define X_REGS offsetof(struct t1_exec, regs)
#define X_RFLAGS offsetof(struct t1_exec, rflags)
#define X_RIP offsetof(struct t1_exec, rip)
#define X_RAW_R13 offsetof(struct t1_exec, raw_r13)
#define X_POINT offsetof(struct t1_exec, point)
#define X_ENTRY_DIRTY offsetof(struct t1_exec, entry_dirty)
#define X_POISON offsetof(struct t1_exec, poison)
#define X_SITE(k) (offsetof(struct t1_exec, site) + (k) * sizeof(struct hostwin_tag))
#define X_SITE_TAG(k) (X_SITE(k) + offsetof(struct hostwin_tag, tag))
#define X_SITE_DELTA(k) (X_SITE(k) + offsetof(struct hostwin_tag, delta))
#define X_SPILL(k) (offsetof(struct t1_exec, spill) + (k) * sizeof(uint64_t))
#define X_FRAME_IN(k) (offsetof(struct t1_exec, frame_in) + (k) * sizeof(uint64_t))
#define X_FORCE(k) (offsetof(struct t1_exec, force) + (k) * sizeof(uint64_t))
#define X_SLOG_N offsetof(struct t1_exec, slog_n)
#define X_SLOG_TOTAL offsetof(struct t1_exec, slog_total)
#define X_SLOG_ADDR offsetof(struct t1_exec, slog_addr)
#define X_SLOG_VAL offsetof(struct t1_exec, slog_val)
#define X_SLOG_POINT offsetof(struct t1_exec, slog_point)
#define X_SLOG_PRE offsetof(struct t1_exec, slog_pre)
#define X_UNDO_PRE offsetof(struct t1_exec, undo_pre)
#define X_UNDO_R13 offsetof(struct t1_exec, undo_r13)
#define X_SLOG_MASK T1_SLOG_MASK
#define X_HANDOFF offsetof(struct t1_exec, handoff)
#define X_BUDGET_ENTRY offsetof(struct t1_exec, budget_entry)
_Static_assert(offsetof(struct t1_exec, v) <= INT32_MAX, "disp32 reach");
_Static_assert(X_REGS % 8 == 0 && X_RFLAGS % 8 == 0 && X_RIP % 8 == 0 && X_SITE(0) % 8 == 0 &&
                   X_SPILL(0) % 8 == 0 && X_FORCE(0) % 8 == 0 && X_SLOG_ADDR % 8 == 0 &&
                   X_SLOG_VAL % 8 == 0 && X_FRAME_IN(0) % 8 == 0 && X_SLOG_PRE % 8 == 0 &&
                   X_UNDO_PRE % 8 == 0 && X_UNDO_R13 % 8 == 0,
               "naturally aligned fields");
_Static_assert((T1_SLOG_ENTRIES & T1_SLOG_MASK) == 0, "ring size is a power of two");

/* ---- event checkpoint (request/kick ABI) ---- */
/*
 * The checkpoint reads the vCPU's requested_exit and irq_kick words
 * (vcpu_kick) with one 64-bit R12-relative load, `mov r11, [r12 +
 * T1_VCPU_CKPT] ; test r11, r11 ; jnz ckpt`, and consumes nothing: the
 * outer loop does (Tier0's loop top in the product; the harness engine
 * t1_run at the CHECKPOINT exit). Inside a loop body it is the only
 * R12-relative memory operand (static check, tests/tier1/program_tests.c).
 * The layout below is what that single load relies on.
 */
#define T1_VCPU_CKPT offsetof(struct vcpu, requested_exit)
_Static_assert(sizeof(((struct vcpu *)0)->requested_exit) == 4 &&
                   sizeof(((struct vcpu *)0)->irq_kick) == 4 &&
                   offsetof(struct vcpu, irq_kick) == T1_VCPU_CKPT + 4 && T1_VCPU_CKPT % 8 == 0 &&
                   T1_VCPU_CKPT <= INT32_MAX,
               "requested_exit and irq_kick form one aligned 8-byte word");

/* ---- budget and retired accounting (budget.c, one definition) ---- */
/*
 *   consumed(P)  = budget_entry - raw_R13 + P.retired_offset
 *   remaining(P) = budget_entry - consumed(P)
 *   v->retired   = retired_entry + consumed(P)
 * P.retired_offset = instructions of P's block completed before P (0 for
 * exits after a terminator and for checkpoints). Exact on every path by the
 * block-start invariant (budget_entry - raw R13 = instructions
 * completed since entry). A fill/retry or miss/retry leaves raw R13
 * unchanged. Only budget.c implements these; the materializer and the
 * dispatcher both call them.
 */
uint64_t t1_point_consumed(const struct t1_exec *x, const struct t1_point *p);
uint64_t t1_point_remaining(const struct t1_exec *x, const struct t1_point *p);

/* ---- entry ABI and runtime stubs ---- */
/*
 * Outer entry: SysV RDI, RSI, RDX -> R12, R13, R14. Prologue pushes RBX,
 * RBP, R12-R15 and subtracts 8 (RSP = 0 mod 16 in the body, no pushes, no
 * red zone); one shared epilogue. Returns T1_RET_POINT with x->point and
 * x->raw_r13 set; IP/slot/retired are left to t1_materialize. If handoff is
 * set, the shared exit stub calls it with the point already captured. A
 * nonzero result is a native body entered without another prologue, with
 * R12/R14 preserved and R13 reloaded from budget_entry. Only the final
 * handoff unwinds the one outer frame. The callback may publish quiescence:
 * no return address into a retired region remains on the stack.
 */
typedef uint32_t (*t1_entry_fn)(struct vcpu *v, uint64_t budget, struct t1_exec *x);
enum t1_ret { T1_RET_POINT = 1 };
/* t1_window_miss / t1_trap_resolve return value: 0 = retry the access
 * (the stub restores registers and jumps back), non-zero = exit at x->point. */
enum t1_resume { T1_RESUME_RETRY = 0, T1_RESUME_EXIT = 1 };
/* Called from miss_k with the allocatable registers in x->regs. */
uint32_t t1_window_miss(struct t1_exec *x);
/* The hostwin trap client (async-signal-safe: reads and writes
 * x only). Accepts a fault only while a region executes and only at a
 * site's host_offset or a forced trap's force_offset; otherwise false (the
 * backend chains to the previous handler). */
bool t1_trap_client(struct hostwin_trap *t);
/* The trap stub calls it in normal context. */
uint32_t t1_trap_resolve(struct t1_exec *x);
/* One materializer for every point kind; never touches v->fault. In
 * verification builds the dispatcher calls the restore observer (tier1.h)
 * right after it returns, before any lower-tier step, delivery, timer poll
 * or region entry; TRAP_RETRY and natural fill-and-retry do not
 * materialize. */
void t1_materialize(struct t1_exec *x, const struct t1_point *p);
/* The register-stack state of point p on v: CFM, frame base, dirty
 * registers and ar.bsp, from the entry's (SDM Vol.2 §6.5.1, Table 6-2: a
 * call preserves sol registers below the new frame, a return releases
 * them). */
void t1_point_frame(const struct t1_exec *x, const struct t1_point *p, struct vcpu *v);
#endif
