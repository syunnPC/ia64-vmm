/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Region formation and lifting:
 * the entry contract, the formed region (decoded, rebased instructions
 * with register names resolved from the contract's CFM: static and
 * frame-relative), t1_form() (form.c) and t1_lift()
 * (lift.c).
 */
#ifndef IA64VMM_TIER1_FORM_H
#define IA64VMM_TIER1_FORM_H
#include "engine/tier1/ir.h"
#include "ia64/decode/decode.h"
#include "ia64/fp.h"

/* Entry contract: compile-time constants,
 * compared for lookup. No property of any live-in value is part of it.
 * psr holds the entry PSR
 * under psr_mask, and a vCPU matches when (v->s.psr & psr_mask) == psr.
 * cfm (under cfm_mask) is complete (sof, sol, sor, rrb.gr/fr/pr): register
 * names are resolved against it, and against the frames calls and allocs
 * of the region make (struct t1_fstate), at compile time.
 * The frame base is not part of it: stacked names are frame offsets from
 * the entry's base (tier1.h E_STK), mapped through v->s.rse_bof at entry
 * and wherever a home is touched, so one region serves its (header, CFM)
 * at every BOF. */
struct t1_contract {
    uint64_t ip; /* incl. the bit-63 alias */
    gpa_t entry_pa;
    uint64_t psr, cfm;
    uint64_t psr_mask; /* T1_CONTRACT_PSR_MASK */
    /* The CFM fields compared, (v->s.cfm & cfm_mask) == cfm: all of them,
     * or without sof, sol and sor (T1_CFM_FRAME) when the head is an alloc
     * and the rotating bases are zero: the alloc's frame does not depend
     * on them (SDM Vol.3 alloc; with rrb.gr = rrb.fr = rrb.pr = 0 no sor
     * check applies) and no instruction precedes it. */
    uint64_t cfm_mask;
    uint64_t hostwin_bits, host_features;
    uint32_t abi, flags; /* T1_ABI_VERSION; T1_COMPILE_* */
    uint8_t slot;
    unsigned cpu_model; /* CPU model, immutable, part of the compile/entry contract. */
    bool icount;
};
/* The PSR fields a contract compares: those compiled
 * code and formation read -- be, ic (the last-successful-bundle element,
 * lift.c), pk, dt, dfl and dfh (disabled FR banks, form.c classify), rt,
 * cpl (privileged AR writes; the fetch translation of the request), is,
 * it, bn (r16-r31 naming), vm -- and the lower-tier bits, which a contract
 * never holds. Not compared: up, ac (a misaligned site exits to the lower
 * tier, which applies PSR.ac), mfl and mfh (FR writes materialize the
 * dirty bits by OR), i (interrupts are delivered at Tier0 boundaries), sp,
 * pp, di, si, mc and ri (the slot is its own contract field; rfi can leave
 * ri stale). In-region helpers read the live PSR. */
#define T1_PSR_CPL (UINT64_C(3) << 32)
#define T1_PSR_VM (UINT64_C(1) << 46)
#define T1_CONTRACT_PSR_MASK                                                                       \
    (IA64_PSR_BE | IA64_PSR_IC | IA64_PSR_PK | IA64_PSR_DT | IA64_PSR_DFL | IA64_PSR_DFH |         \
     IA64_PSR_RT | T1_PSR_CPL | IA64_PSR_IS | IA64_PSR_IT | IA64_PSR_BN | T1_PSR_VM |              \
     T1_PSR_LOWER_TIER)
_Static_assert(T1_CONTRACT_PSR_MASK ==
                       ((UINT64_C(1) << 1) | (UINT64_C(1) << 13) | (UINT64_C(1) << 15) |
                        (UINT64_C(1) << 17) | (UINT64_C(3) << 18) | (UINT64_C(1) << 27) |
                        (UINT64_C(7) << 32) | (UINT64_C(1) << 36) | (UINT64_C(1) << 44) |
                        (UINT64_C(1) << 46) | T1_PSR_LOWER_TIER) &&
                   !(T1_CONTRACT_PSR_MASK & T1_PSR_MF_BITS),
               "Tier1 contract PSR fields (SDM Vol.2 Table 3-2)");

/* CFM.sof, sol and sor (SDM Vol.1 §3.1.7: bits 17:0). */
#define T1_CFM_FRAME UINT64_C(0x3ffff)

/* Instruction classes of the Tier1 subset. */
enum t1_insn_class {
    T1_IC_ALU,     /* integer, shift/bitfield: one GR result */
    T1_IC_CMP,     /* native cmp/cmp4 (normal, .unc, parallel) and tbit */
    T1_IC_LC,      /* native ar.lc moves (SDM Vol.3 mov ar) */
    T1_IC_FPI,     /* integer-only FR transfers, logic and multiply */
    T1_IC_GENERAL, /* table-driven register, FR and memory helpers */
    T1_IC_HELPER,  /* tnat: in-region ia64_execute */
    T1_IC_LOAD,
    T1_IC_STORE,
    T1_IC_BR,    /* br.cond, IP-relative */
    T1_IC_CLOOP, /* br.cloop */
    T1_IC_FENCE, /* mf */
    T1_IC_NOP,   /* nop (all units), brp */
    T1_IC_ALLOC, /* alloc: a new frame (form.c frame states) */
    T1_IC_CALL,  /* direct br.call/brl.call, guarded indirect br.call */
    T1_IC_RET,   /* matched or guarded observed br.ret */
    T1_IC_CHK,   /* chk.s: branch to recovery on a NaT */
    T1_IC_CHKA,  /* chk.a: recovery branch under the zero-entry ALAT policy */
    T1_IC_BRMOV, /* mov r = b, mov b = r */
};
/* Edge kinds of branches. BACK: to the head or to the header
 * of an inner loop. */
enum t1_edge_kind { T1_EDGE_NONE, T1_EDGE_INTERNAL, T1_EDGE_BACK, T1_EDGE_EXIT };

/* A register-stack frame a region instruction executes in: its CFM and the frame base, delta registers above the entry's
 * (the locals that calls inside the region preserved). Stacked registers
 * of the frame are named E_STK(delta + o), so every frame of a region
 * names its registers from the entry's base; delta + sof <= 96. call: the
 * call the frame belongs to (0: the entry's), so the copies of a callee
 * formation follows from two calls are distinct states, with their own
 * edges and returns. */
struct t1_fstate {
    uint64_t cfm;
    uint8_t delta, call;
};
#define T1_MAX_FSTATES 32u
/* Calls formation follows into their callee and returns it matches. */
#define T1_MAX_CALL_DEPTH 8u

/* One region instruction. insn is the immutable rebased copy: its address
 * is the executing guest IP of its bundle; in-region helpers
 * execute exactly this copy. Register fields are element register numbers
 * resolved from the contract's CFM (ia64_gr_index / ia64_pr_index with
 * rse_bof = 0): a static GR p, a stacked GR 32 + o (o: frame offset after
 * rotation), the rotated predicate; GR 0 = r0, PR 0 = p0. */
struct t1_insn {
    Ia64Instruction insn;
    gpa_t bundle_pa;  /* fetch address of the bundle */
    uint64_t next_ip; /* fall-through resume target */
    uint8_t next_slot;
    uint8_t cls;             /* enum t1_insn_class */
    uint8_t edge;            /* enum t1_edge_kind for T1_IC_BR / T1_IC_CLOOP */
    uint8_t page;            /* index into struct t1_form.page */
    uint8_t qp, p1, p2;      /* physical predicates (rrb.pr applied) */
    uint8_t dst, src1, src2; /* element GRs (ALU/CMP/HELPER: destination, source1, source2) */
    uint8_t fr[4];           /* physical FR r1, r2, r3, r4, fixed by CFM */
    uint8_t base, source;    /* element GRs (memory: base; store value / load increment) */
    bool leader;             /* starts a basic block */
    bool fall_exit;          /* fall-through leaves this captured instruction sequence */
    uint16_t
        target_insn; /* INTERNAL: index of the target instruction; BACK: of the header (0: the head) */
    uint64_t target; /* branch target IP (rebased); a matched br.ret: the return IP */
    /* Frame states (struct t1_form fs[]): the one it executes in, and the
     * one after it takes effect (alloc; the taken path of a call or
     * return; otherwise equal). */
    uint8_t fs, fs_taken;
    uint8_t breg; /* CALL: the link BR; RET: the BR it returns through; BRMOV: the BR */
};

struct t1_page {
    uint64_t ip_base;
    gpa_t pa;
    uint64_t generation;
};

/* Instructions formation scans past a loop region's first
 * back-edge to the head for a farther one; the region ends at the farthest. */
#define T1_LOOP_EXTEND 192u

struct t1_form {
    struct t1_contract c;
    unsigned ninsns, npages;
    bool loop; /* the head heads a loop; captured paths may end at different back-edges */
    /* A rejection that may not recur at the same head: the source could
     * not be read or has no code snapshot yet. */
    bool transient;
    /* The first instruction not in the region: the fall-off exit target,
     * and why the scan stopped (diagnostic). */
    uint64_t end_ip;
    uint8_t end_slot;
    uint8_t end_class; /* enum t1_end_class of the end at end_ip */
    uint8_t end_fs;    /* the frame state at end_ip */
    /* Frame states (fs[0]: the entry's); calls: the region holds a call
     * or a return. */
    unsigned nfs;
    struct t1_fstate fs[T1_MAX_FSTATES];
    bool calls;
    const char *end_reason;
    struct t1_page page[T1_MAX_PAGES];
    struct t1_insn insn[T1_MAX_INSNS];
};

/*
 * Two-pass region formation starting at v's ip/slot with the
 * contract taken from v. Reads bundles through code_source_read (recording
 * page generations) and decodes them with ia64_decode_bundle; never walks
 * the VHPT. T1_REJECTED: no region (reason in *reason); T1_RETRY: a page's
 * snapshot changed while forming. max_insns (<= T1_MAX_INSNS) bounds the
 * region, e.g. to retry with a shorter one after a lift limit.
 *  t1_request_from_vcpu, then t1_form_request.
 */
enum t1_compile_status t1_form(struct vcpu *v, uint32_t flags, unsigned max_insns,
                               struct t1_form *f, const char **reason);
/*
 * The executor half of a compile request (tier1.h):
 * the entry preconditions, the contract, and the source pages as I-TLB hits
 * (no walk), the entry page first, then the following VA pages while each
 * translates, then resident transfer targets on the head's straight path.
 * An ordinary branch's target translation is separate: only the bounded
 * tail probe may read its already tracked snapshot. false: no region can
 * start at v's state (*reason, the rejections t1_form reports). Does not
 * change architectural state or the MMU log; derived MMU caches may fill.
 */
bool t1_request_from_vcpu(struct vcpu *v, const struct t1_compile_options *opt,
                          unsigned hostwin_bits, struct t1_compile_request *rq,
                          const char **reason);
/*
 * Formation from a request: reads rq and guest code
 * through code_source_read_physical, never a struct vcpu. A page the scan
 * reaches that rq does not list ends the region there. names is a
 * workspace of the caller (a compile context): formation writes rq's
 * frame into it (cfm, and rse_bof = 0: stacked names are frame offsets)
 * and resolves register names against it with
 * ia64_gr_index / ia64_pr_index; nothing else is read.
 */
enum t1_compile_status t1_form_request(const struct t1_compile_request *rq,
                                       struct ia64_state *names, unsigned max_insns,
                                       struct t1_form *f, const char **reason);
/* The Tier1-subset class of an opcode (enum t1_insn_class), or -1 if the
 * opcode is not in the subset. The other boundary conditions
 * (frame, slot, encoding) are not part of it. */
int t1_opcode_class(Ia64Opcode op);
/* ld.s and ld.sa: a deferred exception leaves NaT instead of faulting (SDM
 * Vol.3 ld; the range test of ia64/load_store.c). */
/* ldfs, ldfd, ldf8, stfs, stfd, stf8: the access size of a native FP
 * window access (when not speculative or advanced, form.c), else 0. */
static inline unsigned t1_fp_access(Ia64Opcode op)
{
    switch (op) {
    case IA64_OP_LDFS:
    case IA64_OP_STFS:
        return 4;
    case IA64_OP_LDFD:
    case IA64_OP_STFD:
    case IA64_OP_LDF8:
    case IA64_OP_STF8:
        return 8;
    default:
        return 0;
    }
}
static inline bool t1_load_speculative(Ia64Opcode op)
{
    return (op >= IA64_OP_LD1S && op <= IA64_OP_LD8S) ||
           (op >= IA64_OP_LD1SA && op <= IA64_OP_LD8SA);
}
/* The enum t1_end_class of an instruction formation stopped at. */
unsigned t1_end_class(const Ia64Instruction *in);
/* stats.c: one formation stop at an instruction formation cannot take
 * (tier1.h t1_form_stops). */
void t1_form_stop_note(unsigned opcode, bool head);
/* stats.c: one call formation took (tier1.h t1_form_calls). */
void t1_form_call_note(enum t1_call_note kind);
/* false: formation rejects rq at its head instruction, which the head
 * bundle and rq decide (an instruction outside the subset, an undefined
 * template, slot 2 of an MLX bundle, a class M helper of a checked
 * region); *stop is that instruction's opcode (IA64_OP_COUNT for the
 * bundle). true also when the bundle cannot be read: formation decides. */
bool t1_request_head_allowed(const struct t1_compile_request *rq, unsigned *stop);
/* Reject known formation boundaries before requesting a hot exit trace.
 * This is a hint; compilation still reads and validates its own snapshot. */
bool t1_trace_head_allowed(struct vcpu *v, const struct t1_compile_options *opt);

/*
 * IA-64 -> IR in SSA form: Braun et al. per element, block
 * parameters at the loop headers, synthetic blocks for predicated memory
 * ops and fences, the qp = 0 effects, frames at every point (every
 * element of frame_set). On a limit (blocks, ops, points, sites) the
 * lift fails with the index of the instruction that did not fit in
 * *fail_insn, so the caller can re-form the region ending before it.
 */
bool t1_lift(const struct t1_form *f, struct t1_ir *ir, unsigned *fail_insn);
#endif
