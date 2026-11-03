/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vCPU and execution-engine interface.
 *
 * The IA-64 architectural state (ia64/state.h) is private to ia64/ and
 * engine/. core/ and platform/ use only this header.
 *
 * Engines: ENGINE_INTERP is the exact reference interpreter (also the
 * helper layer Tier0 calls into); ENGINE_TIER0 is the AMD64 code generator;
 * ENGINE_TIER1 is Tier0 with the optimizing Tier1 as its upper tier.
 * All run the same per-instruction semantics; an unimplemented or
 * unsupported instruction/feature always stops the vCPU with
 * VCPU_EXIT_UNIMPLEMENTED and a state dump, never executes as a NOP.
 */
#ifndef IA64VMM_ENGINE_H
#define IA64VMM_ENGINE_H

#include "ia64vmm/types.h"
#include "ia64vmm/cpu_model.h"
#include "ia64vmm/ia32.h"

struct vcpu;

enum engine_kind {
    ENGINE_INTERP,
    ENGINE_TIER0,
    ENGINE_LOCKSTEP, /* hosted diagnostics: Tier0 checked against INTERP per region */
    /* Tier0 as the outer loop, Tier1 entered
     * from its lookups; engine=tier1. */
    ENGINE_TIER1,
};

struct vcpu_config {
    unsigned index; /* 0 = BSP */
    uint64_t lid;   /* zero or platform CPU_LID(index); id=index, eid=0 */
    enum engine_kind engine;
    uint64_t itc_hz; /* guest ITC frequency; must match what PAL/SAL report */
    bool itc_icount; /* hosted diagnostics: ITC advances per retired instruction
                                     (deterministic) instead of following hal_counter() */
    gpa_t pal_entry; /* GPA of the firmware's PAL entry bundle, whose first
                                     instruction is `break.m 0x100000`; 0 = no PAL. The PAL
                                     procedures run VMM-side (ia64/pal.c) when that break
                                     executes at this IP or at a PAL_COPY_PAL copy of it.
                                     The same break at any other IP raises the architectural
                                     Break Instruction fault. */
    /* Physical entrypoints PALE_INIT and PALE_CHECK of a firmware whose PAL
     * takes INIT and machine checks; 0 = none: INIT
     * and machine checks then stop the vCPU. */
    gpa_t pale_init, pale_check;
    /* Selected once before vCPU/cache creation; never changed live. */
    enum ia64_cpu_model cpu_model;
    /* Actual virtual topology; zero defaults to an independent core/socket. */
    unsigned cores_per_socket;
    /* Populated vCPUs, to bound the last virtual package; zero
     * preserves the independent-socket default for existing callers. */
    unsigned cpu_count;
};

/* Register values the platform hands to the firmware entry point; every
 * other register gets its architectural power-on value (SDM Vol.2 §11.2). */
struct ia64_entry_regs {
    uint64_t ip;     /* bundle address, slot 0 */
    uint64_t psr;    /* normally 0 (physical mode, CPL0, interrupts off) */
    uint64_t gr[32]; /* static GRs r0..r31 (r0 ignored) */
    uint64_t br0;
    /* Platform entry backing store and legacy I/O base (platform.h). */
    uint64_t bsp, bspstore, rsc, k0;
};

enum vcpu_exit_reason {
    VCPU_EXIT_BUDGET,        /* instruction budget exhausted; just call again */
    VCPU_EXIT_HALT,          /* PAL_HALT_LIGHT: idle until an unmasked external source (even
                                     with PSR.i=0) or vcpu_next_timer_deadline. halt_retry_us adds
                                     a bounded retry for guest ITC polling;
                                     default 1000, zero disables that compatibility retry. */
    VCPU_EXIT_SHUTDOWN,      /* platform requested power-off/reset; see vm state */
    VCPU_EXIT_UNIMPLEMENTED, /* instruction/feature not implemented: fatal */
    VCPU_EXIT_BAD_ACCESS, /* physical access outside every region, or other VMM-detected guest error */
    VCPU_EXIT_DEBUG,      /* break/single-step requested by the harness */
};

struct vcpu_exit {
    enum vcpu_exit_reason reason;
    uint64_t ip; /* bundle address of the stopping instruction */
    unsigned slot;
    uint64_t info;      /* reason-specific (e.g. faulting GPA, raw instruction bits) */
    const char *detail; /* static string, may be NULL */
};

struct vcpu *vcpu_create(const struct vcpu_config *cfg);
/* PAL idle wake eligibility, independent of PSR.i; does not consume IVR. */
bool vcpu_halt_wake_pending(const struct vcpu *v);
/* An INIT, PMI or machine check is pending, masked or not. */
bool vcpu_pal_interrupt_pending(const struct vcpu *v);
/* Release the stopped CPU before platform teardown. */
void vcpu_destroy(struct vcpu *v);
void vcpu_reset(struct vcpu *v, const struct ia64_entry_regs *regs);

/* Runs until the budget (in retired IA-64 instructions, approximate for
 * Tier0) is used or an exit condition occurs. Pending external interrupts
 * and the ITC/ITM timer are evaluated inside. */
struct vcpu_exit engine_run(struct vcpu *v, uint64_t budget);

/* Platform hook for `break` instructions that are not the PAL entry
 * (e.g. FPSWA `break.m 0x100001`, debug bridges 0x100002..4). Called with
 * the 21-bit immediate and the bundle address before the Break Instruction
 * fault is raised; returning true means the platform emulated the
 * instruction (execution continues with the next slot), false means the
 * architectural fault is delivered. NULL (default) = always fault. */
typedef bool (*vcpu_break_hook_fn)(struct vcpu *v, uint64_t imm, uint64_t ip);
void engine_set_break_hook(vcpu_break_hook_fn hook);

/* Asynchronous requests (from MMIO handlers on the same vCPU, or later from
 * other CPUs): take effect at the next instruction boundary. */
void vcpu_request_exit(struct vcpu *v, enum vcpu_exit_reason reason);
/* Mailbox kick: yield at an engine boundary without replacing a fatal
 * exit or changing architectural/lockstep state. Safe from host IRQs. */
void vcpu_kick(struct vcpu *v);

/* Marks an external interrupt vector (16..255) pending in the vCPU's
 * LSAPIC model (cr.irr); delivered per cr.tpr/psr.i/in-service state
 * (SDM Vol.2 §5.8). Used by platform interrupt sources and IPIs. */
void vcpu_raise_external(struct vcpu *v, unsigned vector);

/* Earliest host counter value at which this vCPU has timer work
 * (ITM match), or UINT64_MAX. Used to bound hal_idle_until(). */
uint64_t vcpu_next_timer_deadline(const struct vcpu *v);

/* Human-readable architectural state dump through the log. */
void vcpu_dump(const struct vcpu *v);

/* Current instruction position after a service-window diagnostic exit. */
uint64_t vcpu_instruction_pointer(const struct vcpu *v, unsigned *slot);
/* Uses the normal fetch MMU (may fill a VHPT TC); failure does not deliver a fault. */
bool vcpu_instruction_physical_address(struct vcpu *v, uint64_t *pa);

/* Diagnostic ring of the last N instruction attempts (0 disables, max 65536). */
bool ia64_set_trace(struct vcpu *v, unsigned count);
/* Read-only progress sample for the opt-in sample_sec log,
 * with the last delivered fault (name NULL when none). */
void vcpu_progress(const struct vcpu *v, uint64_t *ip, unsigned *slot, uint64_t *psr,
                   uint64_t *retired, const char **fault, uint64_t *fault_ip, uint64_t *fault_info,
                   uint64_t *faults);
/* Diagnostics: read 8 guest bytes at a virtual address through
 * the vCPU's current translations (tpa-style query, no fault delivery). */
bool vcpu_read_virtual(struct vcpu *v, uint64_t va, uint64_t *value);
/* Diagnostics: stop at the first delivery of a fault kind (0 = off). */
void vcpu_set_stop_fault(unsigned kind);
void vcpu_set_log_faults(unsigned count);
/* Diagnostics: break_ip=HEX[:N][+HEX[:N]...] (up to 4) logs the vCPU
 * state the first N times slot 0 of the bundle at ip is about to execute;
 * the run goes on. break_mem=HEX adds the 8 bytes at that guest physical
 * address to each such log. */
void vcpu_set_break_ip(uint64_t ip, unsigned count);
void vcpu_set_break_mem(uint64_t pa);

/* --- code coherence, implemented by engine/cache ---------------------- */

void code_notify_vmm_write(gpa_t gpa, uint64_t len);
void code_notify_cpu_write(gpa_t gpa, uint64_t len);
/* Qualified RAM helpers bracket notification and the write effect
 * against tracked-source preparation. end releases only a newly taken lock;
 * untracked RAM retains the bitmap fast path. No guest wait in between. */
bool code_cpu_write_begin(gpa_t gpa, uint64_t len);
void code_cpu_write_end(bool acquired);
/* gm_write uses the same tracked-source bracket; the weak fallback
 * retains code_notify_vmm_write when no engine is linked. Same end above. */
bool code_vmm_write_begin(gpa_t gpa, uint64_t len);

/* Page validity: polling drivers require COMPARE_ALWAYS until
 * storage interrupt routing calls code_epoch_bump(CODE_DEVICE_STORAGE). */
enum code_validity_mode {
    CODE_VALIDITY_COMPARE_ALWAYS,
    CODE_VALIDITY_EPOCH_COMPARE,
    CODE_VALIDITY_NONE,
    CODE_VALIDITY_HW_DIRTY,
};
enum code_device_class { CODE_DEVICE_OTHER, CODE_DEVICE_STORAGE };
#define CODE_SNAPSHOT_DEFAULT_BYTES (UINT64_C(48) * 1024 * 1024)
struct code_validity_stats {
    uint64_t comparisons, mismatches, invalidations, snapshot_bytes;
    uint64_t epoch_bumps, evictions, comparison_ticks;
    uint64_t metadata_bytes; /* Slab/hash pages count against the snapshot budget. */
};
/* Configure only while execution is stopped; budget is in whole 4 KiB pages.
 * HW_DIRTY reports unavailable and selects COMPARE_ALWAYS. */
bool code_validity_configure(enum code_validity_mode mode, uint64_t budget);
/* Stopped setup, after engine/window allocation and before any source read:
 * apply the reserve-derived default; explicit user budgets skip this call. */
uint64_t code_validity_reserve_budget(uint64_t free_bytes, uint64_t reserve_bytes);
/* Remaining Tier1 allocations, queried at stopped setup (zero if disabled). */
uint64_t engine_tier1_reserve_bytes(void);
bool code_validity_parse_mode(const char *value, size_t length, enum code_validity_mode *mode);
void code_epoch_bump(enum code_device_class device_class);
void code_validity_get_stats(struct code_validity_stats *out);
void code_validity_dump_stats(void);
/* Stopped setup: assign Tier1 the reserve left after snapshots/lazy Tier0. */
void engine_tier1_memory_budget(uint64_t free_bytes, uint64_t snapshot_bytes,
                                uint64_t reserve_bytes);

/* Tier0 arena lifecycle, stopped executors only. Parse tier0_code_mib=
 * (default derived from the VMM reserve, engine/tier0/tier0.h), allocate
 * code/metadata, and fail setup on exhaustion. */
bool tier0_configure_cmdline(const char *cmdline);
void tier0_destroy(void);

/* engine=tier1 lifecycle, stopped executors only. engine_tier1_configure
 * parses tier1_code_mib= (1..1024, default 16), tier1_hot= (loop-head
 * threshold, default 64; trace heads 16 times it), tier1_slice= (wall-clock
 * slice, default 16384), tier1_verify= (passes, the default: the IR is
 * verified after every optimizer pass; final: only the optimized IR),
 * tier1_chain= (on, the default: region chaining; off) and
 * hostwin_check= (0/1; 1 is not available in vmm.elf), and
 * registers Tier1 as Tier0's upper tier. Setup errors: code_validity=none,
 * a binary without Tier1, a host without a window backend. Each vCPU of
 * engine=tier1 is attached after vcpu_create and detached before
 * vcpu_destroy (other engines: no-ops). engine_dump_stats prints the one
 * `tier1 linked= compiles= entries= ...` line, for every engine. */
bool engine_tier1_configure(const char *cmdline, enum code_validity_mode validity);
/* engine=lockstep tier1=on:
 * engine_tier1_configure for a lockstep vCPU, whose Tier1 regions are
 * compiled checked and every execution of which is checked against the
 * interpreter (engine/tier1/checked.c); unit tests only (a setup error in
 * vmm.elf). engine_vcpu_attach then attaches lockstep vCPUs too. */
bool engine_tier1_lockstep_configure(const char *cmdline, enum code_validity_mode validity);
bool engine_vcpu_attach(struct vcpu *v);
void engine_vcpu_detach(struct vcpu *v);
void engine_tier1_destroy(void);
void engine_dump_stats(void);

/* Host IEEE execution of IA-64 FP arithmetic where bit-identical
 * (ia64/hostfp.h), set before vCPUs run: fp_approx=1 (precision may differ,
 * requires fp_experimental=1), fp_host=0 (soft-float only), fp_check=1 (verify each
 * host result on the soft-float), fp_stats=1 (statistics, also without host
 * execution; engine_fp_log prints a vCPU's line). fp_experimental=1 enables
 * unproven direct Tier1 FP expansions; the default keeps exact helpers. */
#define ENGINE_FP_APPROX 1u
#define ENGINE_FP_SOFT 2u
#define ENGINE_FP_CHECK 4u
#define ENGINE_FP_CLASSIFY 8u
#define ENGINE_FP_EXPERIMENTAL 16u
void engine_fp_configure(unsigned flags);
void engine_fp_log(const struct vcpu *v);

/* Host counter ticks
 * the engines spent translating (Tier0 regions plus Tier1 compiles) since
 * start, for the executor's milestone lines. Measured only behind the
 * statistics gates IA64VMM_TIER0_STATS / IA64VMM_TIER1_STATS; returns false
 * (no measurement, *ticks untouched) otherwise, and also when Tier1
 * compiled without IA64VMM_TIER1_STATS (the total would miss its part).
 * Defined in engine/engine.c; core/vm.c keeps a weak default returning
 * false for binaries without it. */
bool engine_compile_ticks(uint64_t *ticks);

/* The legacy engine_run,
 * vcpu_exit, progress and bundle-IP APIs keep their IA-64 meanings. */
enum vcpu_isa { VCPU_ISA_IA64, VCPU_ISA_IA32 };
struct vcpu_position {
    enum vcpu_isa isa;
    uint64_t ip;    /* IA-64 bundle or zero-extended IA-32 linear byte IP */
    unsigned slot;  /* IA-32: 0, never a bundle slot */
    ia32_eip_t eip; /* IA-32 CS-relative offset, when eip_valid */
    bool eip_valid;
};
struct vcpu_run_budget {
    uint64_t ia64_instructions;   /* unchanged approximate IA-64 retirement budget */
    struct ia32_run_limit native; /* wall-clock bound mandatory even with no exact count */
};
enum vcpu_run_stop { VCPU_RUN_EXIT, VCPU_RUN_YIELD };
enum vcpu_yield_reason {
    VCPU_YIELD_IA64_BUDGET,
    VCPU_YIELD_NATIVE_DEADLINE,
    VCPU_YIELD_NATIVE_BUDGET,
    VCPU_YIELD_KICK
};
struct vcpu_run_exit {
    enum vcpu_run_stop stop;
    enum vcpu_exit_reason reason; /* meaningful for EXIT; fatal stops outrank yields */
    enum vcpu_yield_reason yield; /* meaningful for YIELD; never a guest interruption */
    struct vcpu_position position;
    uint64_t info;
    const char *detail;
    bool native_exit_valid;
    struct ia32_native_exit native_exit;
    uint64_t ia64_retired; /* this call only; native never increments v->retired */
    struct ia32_run_usage native_usage;
};
/* Common outer dispatcher: br.ia/rfi/JMPE materialize at ISA
 * boundaries; a refill/kick keeps IA-32 active. Deadline bounds the whole
 * call (including IA-64 residence and refill loops), independent of guest
 * IF/PSR.i. Zero IA-64 budget/expired deadline yields without execution;
 * unsupported exact native count stops before entry. Yield returns a
 * restorable instruction/REP boundary after host FP restore and mailbox/
 * purge work. No repeated MMIO/atomic/committed REP effect on resume.
 * The weak default ALWAYS stops UNIMPLEMENTED without changing state. */
struct vcpu_run_exit engine_run_bounded(struct vcpu *v, const struct vcpu_run_budget *budget);

/* Per-vCPU engine-owned private data (v->native in ia64/state.h). The IA-32
 * engine invokes attach on the pinned executor after vcpu_create, reset BEFORE
 * vcpu_reset, detach BEFORE vcpu_destroy/platform teardown. Reset/detach
 * require stopped execution and host FP ownership; reset withdraws maps,
 * tags and pending one-shot notification/write leases. Attach failure
 * leaves v->native=NULL and unwinds all allocations/host CPU references;
 * detach is idempotent for NULL. No state is shared with lockstep copies.
 * No table is linked without the IA-32 engine. */
struct engine_ia32_ops {
    bool (*attach)(struct vcpu *, unsigned host_cpu, struct ia32_setup_error *);
    bool (*reset)(struct vcpu *, struct ia32_setup_error *);
    void (*detach)(struct vcpu *);
    struct vcpu_run_exit (*run)(struct vcpu *, const struct vcpu_run_budget *);
};
const struct engine_ia32_ops *engine_ia32_glue(void); /* NULL weak default */

struct vcpu_isa_snapshot {
    struct vcpu_position position;
    uint64_t sequence, ia64_retired, native_residence_ticks, native_instructions;
    bool native_instructions_valid, native_state_valid, native_exit_valid;
    struct ia32_native_state native_state;
    struct ia32_native_exit native_exit;
    uint8_t instruction[15], bytes_valid; /* fetched safely, never speculative guest reads */
    uint16_t prefix_summary;              /* SDM Vol.2 Fig.9-3 instruction-intercept encoding */
};
/* Executor publishes after FP restoration/state materialization; readers
 * consume a coherent immutable snapshot, never a live VMCS/VMCB. false:
 * no published snapshot, *out untouched; caller kicks for a fresh sample.
 * The IA-32 engine owns publication and ISA-aware fault/trace formatting. */
bool vcpu_snapshot_isa(const struct vcpu *v, struct vcpu_isa_snapshot *out);
void vcpu_dump_isa(const struct vcpu *v); /* materialized/stopped executor only */

#endif
