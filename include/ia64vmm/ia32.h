/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native transport contract.
 * Hardware exits are transport, never already-classified IA-64 faults.
 * No VBIOS policy, host CPUID answers or x86 event injection is inherited. */
#ifndef IA64VMM_IA32_H
#define IA64VMM_IA32_H

#include "ia64vmm/types.h"

/* Wrappers prevent confusing a byte offset, region-0 VA, nested physical
 * token and translated IA-64 GPA. Tokens are not GPA even in unpaged mode. */
typedef struct {
    uint32_t value;
} ia32_eip_t;
typedef struct {
    uint32_t value;
} ia32_linear_t;
typedef struct {
    uint64_t value;
} ia32_token_t;

enum ia32_backend_kind { IA32_BACKEND_NONE, IA32_BACKEND_SVM, IA32_BACKEND_VMX };
enum ia32_exit_phase {
    IA32_PHASE_UNKNOWN,   /* raw exit not yet classified; never replay blindly */
    IA32_PHASE_BEFORE,    /* no instruction effects committed */
    IA32_PHASE_COMPLETED, /* instruction completed, resume at next EIP */
    IA32_PHASE_PARTIAL,   /* subaccesses committed, including partial REP */
};
enum ia32_exit_reason {
    IA32_EXIT_UNIMPLEMENTED,
    IA32_EXIT_ENTRY_FAILURE,
    IA32_EXIT_NESTED_FAULT,
    IA32_EXIT_EXCEPTION,
    IA32_EXIT_IO,
    IA32_EXIT_CPUID,
    IA32_EXIT_CLOCK,
    IA32_EXIT_SYSTEM,
    IA32_EXIT_STEP,
    IA32_EXIT_HOST_IRQ,
    IA32_EXIT_HOST_NMI,
    IA32_EXIT_DEADLINE,
    IA32_EXIT_KICK,
    IA32_EXIT_SHUTDOWN,
    IA32_EXIT_OTHER,
};
enum ia32_access_kind {
    IA32_ACCESS_UNKNOWN,
    IA32_ACCESS_FETCH,
    IA32_ACCESS_READ,
    IA32_ACCESS_WRITE,
    IA32_ACCESS_RMW,
    IA32_ACCESS_DESCRIPTOR,
    IA32_ACCESS_PRIVATE_WALK, /* host bookkeeping, not an IA-64 reference */
};

/* APM Vol.2 App.B attribute encoding, plus explicit unusable state for
 * VMX. base/limit are effective cached values; no descriptor normalization
 * or accessed-bit repair is implied. The IA-32 engine retains original IA-64 fields. */
struct ia32_segment {
    uint64_t base;
    uint32_t limit;
    uint16_t selector, attrib;
    bool unusable;
};

#define IA32_FP_BYTES 512u
/* Legacy FXSAVE layout: FTW bits name physical registers, slots are ST(i)
 * order. Consumers must rotate by TOP. Not a host XSAVE area or the
 * long-mode FXSAVE64 pointer format. Reserved bytes are initialized. */
struct ia32_fp_image {
    uint8_t bytes[IA32_FP_BYTES];
} IA64VMM_ALIGNED(16);
_Static_assert(sizeof(struct ia32_fp_image) == 512 && _Alignof(struct ia32_fp_image) >= 16,
               "legacy guest FP image");

enum ia32_gpr {
    IA32_EAX,
    IA32_ECX,
    IA32_EDX,
    IA32_EBX,
    IA32_ESP,
    IA32_EBP,
    IA32_ESI,
    IA32_EDI,
    IA32_GPRS
};
struct ia32_native_state {
    uint32_t gpr[IA32_GPRS];
    ia32_eip_t eip;
    uint32_t eflag;               /* semantic guest flags, never private stepping TF */
    uint64_t cflg, kr0, kr1, kr2; /* virtual state, not hardware CR0/3/4 */
    struct ia32_segment es, cs, ss, ds, fs, gs, ldtr, tr, gdtr, idtr;
    uint64_t dr[4], dr6, dr7; /* native debug snapshot; IA-64 IBR/DBR remain in v->s */
    uint8_t cpl;              /* PSR.cpl; VMX requires cpl == SS.DPL, else import fails */
    struct ia32_fp_image fp;
    /* Baseline VMX/SVM restart state, not semantic EFLAG. Export/import
     * preserves STI/MOV-SS blocking and debug state across host exits. */
    uint32_t interruptibility; /* raw VMX guest interruptibility; zero on SVM */
    uint64_t pending_debug;    /* raw VMX pending-debug exceptions; zero on SVM */
    uint64_t interrupt_shadow; /* raw SVM INTERRUPT_SHADOW; zero on VMX */
};

#define IA32_DIRTY_GPR (UINT64_C(1) << 0)
#define IA32_DIRTY_IP (UINT64_C(1) << 1)
#define IA32_DIRTY_FLAGS (UINT64_C(1) << 2)
#define IA32_DIRTY_SEGMENTS (UINT64_C(1) << 3)
#define IA32_DIRTY_CONTROL (UINT64_C(1) << 4)
#define IA32_DIRTY_DEBUG (UINT64_C(1) << 5)
#define IA32_DIRTY_FP (UINT64_C(1) << 6)
#define IA32_DIRTY_SHADOW (UINT64_C(1) << 7)
#define IA32_DIRTY_ALL UINT64_C(0xff)

struct ia32_generations {
    uint64_t mapping, protection, code, ownership;
};
struct ia32_event {
    bool valid, error_valid;
    uint8_t vector, type; /* raw backend event type, tagged by backend */
    uint32_t error;
};
struct ia32_access {
    enum ia32_access_kind kind;
    bool linear_valid, token_valid, gpa_valid, width_valid;
    ia32_linear_t linear; /* first failing byte when classified */
    ia32_token_t token;   /* raw nested fault address */
    gpa_t gpa;            /* only after authoritative IA-64 translation */
    uint32_t width;
    bool locked, rep;
};
struct ia32_native_exit {
    enum ia32_exit_reason reason;
    enum ia32_exit_phase phase;
    enum ia32_backend_kind backend;
    uint64_t raw_reason, qualification, info2, dirty;
    uint32_t hardware_eflag, interruptibility; /* raw state, separate from semantic EFLAG */
    ia32_eip_t eip, next_eip;
    ia32_linear_t linear_ip;
    bool position_valid, next_eip_valid, length_valid;
    uint8_t length;
    struct ia32_access access;
    struct ia32_event exception, vectoring;
    /* Progress is descriptive, not a replay instruction. Full native state
     * after exit retains ECX/ESI/EDI/ESP and every committed subaccess. */
    uint64_t committed_iterations;
    bool iterations_valid;
    uint32_t prior_eflag;
    bool prior_eflag_valid;
    struct ia32_generations generations;
    const char *detail; /* static string, no host pointer exposed to guest */
    /* SVM: qualification=EXITINFO1, info2=EXITINFO2. VMX:
     * qualification=exit qualification, info2=guest physical address when
     * access.token_valid, otherwise zero. Instruction information is a
     * separate VMX field; undefined fields are zero with validity false. */
    uint32_t instruction_info;
    bool instruction_info_valid;
};

/* Capabilities describe mechanisms, not whole-Madison qualification. In
 * particular descriptor_read_exit does not prove a one-shot rearm boundary.
 * Baseline means Sandy Bridge VMX / Zen 2 SVM; still probe each executor.
 * Optional bits never become unconditional setup requirements. */
struct ia32_capabilities {
    enum ia32_backend_kind backend;
    unsigned host_cpu;
    bool nested_paging, unrestricted_guest, nested_invalidate;
    /* Baseline VMX MTF: probe primary allowed-1 control bit 27; false on
     * SVM. If hidden by a backend, exact step fails before entry and SS/flag
     * notification gaps are reported. Other foundations are baseline and
     * individually probed; absent NPT/EPT/FP/bounded exit -> setup stop. */
    bool mtf, descriptor_read_exit, bounded_exit, legacy_fp;
    /* NRIP/decode assists are Zen 2 baseline, CPUID.8000000A-probed;
     * selective ASID flush is an optional accelerator. Missing NRIP/decode
     * assists use authoritative byte decoding; missing selective
     * flush uses TLB_CONTROL=1 (all ASIDs) before the next VMRUN. */
    bool nrip, decode_assists, flush_by_asid;
    /* Sandy Bridge baseline fields/control; probe IA32_VMX_BASIC[54] for
     * INS/OUTS information. If absent, decode the original instruction. */
    bool vmx_ins_outs_info;
    /* Optional EPT execute-only (EPT_VPID_CAP[0]): absent -> exact stop for
     * unrepresentable fetch-only rights, never broaden to readable. */
    bool ept_execute_only;
    /* Raw supported type masks: bit N is instruction type N. Probe
     * IA32_VMX_EPT_VPID_CAP. Baseline INVEPT needs type 1 or 2; prefer 1,
     * fall back to 2. Optional VPID/INVVPID: disable VPID if unusable. */
    uint32_t invept_types, invvpid_types;
    /* Optional newer lock controls, not present on either baseline.
     * Absent -> documented Lock intercept gap. Intel bus-lock
     * exit is after effects; it is detection, not a nullifying fault. */
    bool svm_bus_lock_threshold, vmx_bus_lock_exit, split_lock_ac;
};
struct ia32_setup_error {
    enum ia32_exit_reason reason; /* UNIMPLEMENTED or ENTRY_FAILURE */
    unsigned host_cpu;
    uint64_t raw;
    const char *detail;
};
struct ia32_run_limit {
    uint64_t deadline;     /* mandatory absolute hal_counter(), finite */
    uint64_t instructions; /* 0 = no count limit; >0 needs exact counting */
    bool step;             /* host-only instruction/REP-iteration boundary;
                                    unsupported mechanism fails before entry */
};
struct ia32_run_usage {
    uint64_t residence_ticks, instructions;
    bool instructions_valid; /* absent exact counting: false, instructions=0 */
};

#define IA32_MAP_READ (1u << 0)
#define IA32_MAP_WRITE (1u << 1)
#define IA32_MAP_EXEC (1u << 2)
enum ia32_memory_type { IA32_MEMORY_WB, IA32_MEMORY_UC, IA32_MEMORY_WC };
struct ia32_mapping {
    ia32_token_t token; /* one 4 KiB aligned native token page */
    hpa_t hpa;          /* 4 KiB aligned, validated guest-owned backing */
    unsigned rights;    /* no silently broadened rights/MA */
    enum ia32_memory_type memory_type;
    struct ia32_generations generations;
};

struct ia32_container;
/* Calls are synchronous on the pinned executor, outside IRQ context; none
 * dispatches guest events or services device accesses. create fails with
 * *out=NULL and unwinds CPU references/pages. Probe mutates no CPU ownership.
 * enter returns one exit with all guest GPR/segment/FP state in *state and
 * host FP restored, including on entry failure. Dirty marks changed groups
 * since import; export returns all groups, not just dirty ones. A host exit
 * keeps the ISA unchanged. Backend IRQ dispatch must follow FP restoration.
 *
 * A finite deadline exits independently of EFLAG.if/PSR.i; instructions>0
 * without exact counting fails before entry. Count completed instructions,
 * not exits/REP iterations; step exits may be PARTIAL until REP completes.
 * No unbounded internal retries. UNKNOWN phase requires caller classification
 * before replay; COMPLETED needs validated next EIP, never guessed length.
 * Invalidation success means effective before the next entry of this
 * container on its pinned CPU. All entry paths honor a recorded pending
 * flush; no entry may occur between mutation and successful recording.
 * Zen 2: arm VMCB TLB_CONTROL (all ASIDs if selective flush unavailable),
 * consumed at the next VMRUN; no INVLPGB requirement or INVLPGA-only claim
 * for nested walk caches. Sandy Bridge: INVEPT type 1 or 2, plus INVVPID
 * when enabled, before next entry. Caller acknowledges remote purges AFTER
 * success while stopped; every later entry must honor the pending flush.
 * VMX import fails with UNIMPLEMENTED and exact stop when PSR.cpl differs
 * from SSD.dpl (cpl != SS.DPL); never repair either descriptor or privilege.
 * Reset drops leases/launch state/maps and flushes tags by the same rule;
 * destroy releases this instance
 * only, after native execution stopped. Neither migrates a vCPU. */
struct ia32_backend_ops {
    bool (*probe)(unsigned host_cpu, struct ia32_capabilities *out, struct ia32_setup_error *error);
    bool (*create)(unsigned host_cpu, struct ia32_container **out, struct ia32_setup_error *error);
    bool (*import)(struct ia32_container *, const struct ia32_native_state *, uint64_t dirty,
                   struct ia32_setup_error *error);
    bool (*export)(struct ia32_container *, struct ia32_native_state *,
                   struct ia32_setup_error *error);
    /* The caller supplies leaves only after authoritative MMU/ownership checks.
     * Unrepresentable rights fail without publishing a permissive leaf.
     * Unmap is idempotent, a 4 KiB aligned [token, token+bytes) range without
     * wrap. Both obey the next-entry invalidation success rule above. */
    bool (*map)(struct ia32_container *, const struct ia32_mapping *,
                struct ia32_setup_error *error);
    bool (*unmap)(struct ia32_container *, ia32_token_t token, uint64_t bytes,
                  struct ia32_setup_error *error);
    struct ia32_native_exit (*enter)(struct ia32_container *, struct ia32_native_state *state,
                                     const struct ia32_run_limit *, struct ia32_run_usage *);
    bool (*invalidate)(struct ia32_container *, const struct ia32_generations *,
                       struct ia32_setup_error *error);
    bool (*reset)(struct ia32_container *, struct ia32_setup_error *error);
    void (*destroy)(struct ia32_container *);
};
/* The HAL provides the table. NULL weak default means unavailable, never
 * native success. Successful probe is still subject to qualification. */
const struct ia32_backend_ops *hal_ia32_backend(void);

#endif
