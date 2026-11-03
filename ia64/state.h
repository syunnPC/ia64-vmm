/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_STATE_H
#define IA64VMM_IA64_STATE_H

#include "ia64vmm/engine.h"
#include "ia64/decode/decode.h"
#include "ia64/pal.h"

/* Version 4: model identity and enlarged model TR/TC storage change the layout. */
#define IA64_STATE_VERSION 4u
#define IA64_N_STACKED_PHYS 96u
#define IA64_IMPL_VA_MSB 60u
#define IA64_IMPL_PA_MSB                                                                           \
    49u /* 50-bit physical addresses (Montecito; platform.h IA64VMM_PHYS_ADDR_BITS) */
/* Separate instruction/data translation banks; PAL describes both levels. */
/* Maximum storage over the immutable Madison/Montecito profiles.
 * Fork target/ia64/cpu.c model definitions and cpu.h IA64_TLB_MAX. */
#define IA64_ITR_COUNT 64u
#define IA64_DTR_COUNT 64u
#define IA64_TC_ENTRIES 128u
#define IA64_RID_BITS 24u
#define IA64_KEY_BITS 24u
#define IA64_PAGE_SIZE_MASK UINT64_C(0x155557000)
#define IA64_PURGE_PAGE_SIZE_MASK IA64_PAGE_SIZE_MASK
#define IA64_PSR_BE (UINT64_C(1) << 1)
#define IA64_PSR_IC (UINT64_C(1) << 13)
#define IA64_PSR_I (UINT64_C(1) << 14)
#define IA64_PSR_DT (UINT64_C(1) << 17)
#define IA64_PSR_SI (UINT64_C(1) << 23)
#define IA64_PSR_DI (UINT64_C(1) << 22)
#define IA64_PSR_IS (UINT64_C(1) << 34)
#define IA64_PSR_MC (UINT64_C(1) << 35)
#define IA64_PSR_IT (UINT64_C(1) << 36)
#define IA64_PSR_RT (UINT64_C(1) << 27)
#define IA64_PSR_ED (UINT64_C(1) << 43)
#define IA64_PSR_BN (UINT64_C(1) << 44)
#define IA64_PSR_PK (UINT64_C(1) << 15)
#define IA64_PSR_DB (UINT64_C(1) << 24)
#define IA64_PSR_LP (UINT64_C(1) << 25)
#define IA64_PSR_TB (UINT64_C(1) << 26)
#define IA64_PSR_ID (UINT64_C(1) << 37)
#define IA64_PSR_DA (UINT64_C(1) << 38)
#define IA64_PSR_DD (UINT64_C(1) << 39)
#define IA64_PSR_SS (UINT64_C(1) << 40)
#define IA64_PSR_IA (UINT64_C(1) << 45)
/* Bit positions checked numerically against SDM Rev 2.3 Vol.2 Table 3-2, so
 * code and tests that share these macros cannot share a wrong definition
 * (DA/DD once sat on bits 37/38: the bits of ID/DA). */
_Static_assert(IA64_PSR_BE == UINT64_C(1) << 1 && IA64_PSR_IC == UINT64_C(1) << 13 &&
                   IA64_PSR_I == UINT64_C(1) << 14 && IA64_PSR_PK == UINT64_C(1) << 15 &&
                   IA64_PSR_DT == UINT64_C(1) << 17 && IA64_PSR_SI == UINT64_C(1) << 23 &&
                   IA64_PSR_DB == UINT64_C(1) << 24 && IA64_PSR_LP == UINT64_C(1) << 25 &&
                   IA64_PSR_TB == UINT64_C(1) << 26 && IA64_PSR_RT == UINT64_C(1) << 27 &&
                   IA64_PSR_IS == UINT64_C(1) << 34 && IA64_PSR_MC == UINT64_C(1) << 35 &&
                   IA64_PSR_IT == UINT64_C(1) << 36 && IA64_PSR_ID == UINT64_C(1) << 37 &&
                   IA64_PSR_DA == UINT64_C(1) << 38 && IA64_PSR_DD == UINT64_C(1) << 39 &&
                   IA64_PSR_SS == UINT64_C(1) << 40 && IA64_PSR_ED == UINT64_C(1) << 43 &&
                   IA64_PSR_BN == UINT64_C(1) << 44 && IA64_PSR_IA == UINT64_C(1) << 45,
               "PSR bit positions (SDM Vol.2 Table 3-2)");
/* Interpreter boundary routing for SDM Vol.2 §§5.5, 7.1. */
#define IA64_PSR_DEBUG_MODES (IA64_PSR_DB | IA64_PSR_LP | IA64_PSR_TB | IA64_PSR_SS)

enum ia64_ar {
    AR_RSC = 16,
    AR_BSP = 17,
    AR_BSPSTORE = 18,
    AR_RNAT = 19,
    AR_FCR = 21,
    AR_EFLAG = 24,
    AR_CSD = 25,
    AR_SSD = 26,
    AR_CFLG = 27,
    AR_FSR = 28,
    AR_FIR = 29,
    AR_FDR = 30,
    AR_CCV = 32,
    AR_UNAT = 36,
    AR_FPSR = 40,
    AR_ITC = 44,
    AR_PFS = 64,
    AR_LC = 65,
    AR_EC = 66,
};
enum ia64_cr {
    CR_DCR = 0,
    CR_ITM = 1,
    CR_IVA = 2,
    CR_PTA = 8,
    CR_IPSR = 16,
    CR_ISR = 17,
    CR_IIP = 19,
    CR_IFA = 20,
    CR_ITIR = 21,
    CR_IIPA = 22,
    CR_IFS = 23,
    CR_IIM = 24,
    CR_IHA = 25,
    CR_LID = 64,
    CR_IVR = 65,
    CR_TPR = 66,
    CR_EOI = 67,
    CR_IRR0 = 68,
    CR_ITV = 72,
    CR_PMV = 73,
    CR_CMCV = 74,
    CR_LRR0 = 80,
    CR_LRR1 = 81,
};

/* SDM Vol.1 §5: no host float type can hold this architectural format. */
struct ia64_fr {
    uint64_t significand;
    uint32_t exponent; /* 17 bits, including the NaTVal encoding */
    bool sign;
};
struct ia64_tlb_entry {
    uint64_t va, pte, itir;
    uint32_t rid;
    unsigned page_shift;
    bool valid;
};

struct ia64_state {
    unsigned version;
    /* Reset-time mirror of immutable cfg.cpu_model for state-only queries. */
    unsigned cpu_model;
    uint64_t gr[128];
    bool nat[128];
    struct ia64_fr fr[128];
    uint64_t pr, br[8], ar[128], cr[128];
    uint64_t psr, cfm, ip;
    unsigned slot;
    uint64_t bank_gr[16];
    bool bank_nat[16];
    uint64_t rr[8], pkr[16], dbr[8], ibr[8], pmc[256], pmd[256];
    /* Lazy RSE, SDM Vol.2 §6: dirty registers precede BOF in the ring;
     * spilled registers are invalid (Montecito has no clean partition). */
    unsigned rse_bof, rse_dirty;
    /* SDM Vol.2 §§6.6–6.8: an unloaded prefix belongs to the current
     * frame, while interruption handlers execute with CFLE cleared. */
    unsigned rse_missing;
    bool rse_cfle, rse_completion_pending;
    uint64_t rse_completion_psr, rse_completion_ip;
    unsigned rse_completion_slot;
    bool rse_completion_demoted;
    uint64_t msr[1024]; /* Implementation-defined MSR bank, readback only. */
    struct ia64_tlb_entry tr[2][IA64_ITR_COUNT], tc[2][IA64_TC_ENTRIES]; /* I-side 0, D-side 1. */
    unsigned tc_next[2];
    struct ia64_pal_state pal;
};

enum ia64_fault_kind {
    IA64_FAULT_NONE,
    IA64_FAULT_ILLEGAL,
    IA64_FAULT_NAT,
    IA64_FAULT_RESERVED,
    IA64_FAULT_PRIVILEGED,
    IA64_FAULT_BREAK,
    IA64_FAULT_UNALIGNED,
    IA64_FAULT_UNSUPPORTED_DATA,
    IA64_FAULT_UNIMPL_DATA,
    IA64_FAULT_ALT_DTLB,
    IA64_FAULT_PAGE_NOT_PRESENT,
    IA64_FAULT_DATA_NAT,
    IA64_FAULT_PRIVILEGED_REG,
    IA64_FAULT_NESTED_DTLB,
    IA64_FAULT_KEY_MISS,
    IA64_FAULT_FP_DISABLED,
    IA64_FAULT_FP,
    IA64_FAULT_FP_TRAP,
    IA64_FAULT_ALT_ITLB,
    IA64_FAULT_ITLB,
    IA64_FAULT_DTLB,
    IA64_FAULT_VHPT,
    IA64_FAULT_KEY_PERMISSION,
    IA64_FAULT_ACCESS_RIGHTS,
    IA64_FAULT_ACCESS_BIT,
    IA64_FAULT_DIRTY_BIT,
    IA64_FAULT_INST_NAT,
    IA64_FAULT_UNIMPL_INST,
    IA64_FAULT_DEBUG,
    IA64_FAULT_LOWER_PRIVILEGE,
    IA64_FAULT_TAKEN_BRANCH,
    IA64_FAULT_SINGLE_STEP,
    IA64_FAULT_DISABLED_ISA,
    IA64_FAULT_VIRTUALIZATION,
    /* A machine check raised by the instruction (Table 4-1 TR overlap);
     * delivered to PALE_CHECK, not through the IVT. */
    IA64_FAULT_MACHINE_CHECK,
};
enum ia64_step_kind {
    IA64_STEP_COMPLETE,
    IA64_STEP_BRANCH,
    IA64_STEP_FAULT,
    IA64_STEP_EXIT,
};
/* Destination-register faults are before-effect; mandatory RSE spills may
 * advance BSPSTORE before a later reference faults and restart at that cursor.
 * Helpers otherwise return before-effect faults or a committed completion. FP_TRAP
 * and LP/TB/SS are completing faults: the engine retires it before interruption delivery.
 * The engine alone advances IP/slot. This is the Tier0 FaultSite boundary. */
struct ia64_result {
    enum ia64_step_kind kind;
    enum ia64_fault_kind fault;
    uint64_t info; /* fault operand, branch target, or exit diagnostic */
    enum vcpu_exit_reason exit;
    const char *detail;
    unsigned next_slot;     /* Branch/rfi resume slot; ordinary branches use 0. */
    uint64_t fault_isr;     /* Access qualifiers for mandatory RSE memory faults. */
    bool translation_fault; /* MMU supplied qualifiers and translation defaults. */
};
struct ia64_fault_site {
    enum ia64_fault_kind kind;
    uint64_t ip, raw, info;
    unsigned slot;
    bool effect_committed;
};
struct ia64_trace_entry {
    uint64_t ip, raw;
    unsigned slot;
    Ia64Opcode opcode;
};

/* MMU change log:
 * every write of a TLB entry (insertion, purge, replacement, including the
 * VHPT walker's insertions), of RR or PKR, and every reset appends one
 * change. Consumers (hostwin_enter, Tier1's fetch-guard cache) remember the
 * sequence they last saw; equal means "no change", a distance of at most
 * IA64_MMU_LOG means "replay mmu_log", anything else means "full diff".
 * PSR is never logged: consumers compare the PSR fields they depend on.
 * Writers publish after each effective mutation. */
#define IA64_MMU_LOG 16u
enum ia64_mmu_change_kind { IA64_MMU_TC, IA64_MMU_TR, IA64_MMU_RR, IA64_MMU_PKR, IA64_MMU_ALL };
/* side: 0 = instruction, 1 = data (struct ia64_state tr[side], tc[side]);
 * index: the TC/TR entry, RR or PKR number; both 0 for IA64_MMU_ALL. */
struct ia64_mmu_change {
    uint8_t kind, side, index;
};

/* Indices survive diagnostic vCPU copies; no cache points into s.
 * Four-KiB tags keep the hit path independent of TR/TC capacity/page size. */
#define IA64_MICROTLB_ENTRIES 16u
struct ia64_microtlb_entry {
    uint64_t page;
    unsigned index;
    bool tr, valid;
};
struct ia64_microtlb {
    uint64_t seq;
    struct ia64_microtlb_entry entry[2][IA64_MICROTLB_ENTRIES];
};
/* Derived miss index: hash by RID, page size and aligned VA. Nodes use
 * storage indices, so diagnostic vCPU copies cannot retain stale pointers. */
#define IA64_TLB_BUCKETS 256u
#define IA64_TLB_NODES (IA64_ITR_COUNT + IA64_TC_ENTRIES)
struct ia64_tlb_index {
    uint64_t seq, shifts[2];
    uint16_t head[2][IA64_TLB_BUCKETS], next[2][IA64_TLB_NODES];
    uint16_t bucket[2][IA64_TLB_NODES];
    uint8_t shift[2][IA64_TLB_NODES], count[2][64];
    bool ready;
};
struct ia64_timer_cache {
    uint64_t host, span;
    bool valid;
};
struct ia32_vcpu_private;

struct lockstep_access; /* Record is engine-private (interp.h). */
struct vcpu {
    struct ia64_state s;
    struct vcpu_config cfg;
    /* Per-CPU MMIO controller state, outside architectural state. TR buffer
     * writes complete at the engine boundary, like other platform effects. */
    uint64_t pal_tr_selector, pal_tr_buffer, pal_tr_valid;
    int64_t pal_tr_status;
    bool pal_tr_pending;
    /* Native PAL hardware resources, read and written through the
     * per-processor service registers like pal_tr_* (platform.h): XR0, the
     * PALE_PMI and SAL PMI entrypoints, the PAL_MC_EXPECTED indicator, and
     * the INIT/machine check and PMI last entered. Pending PAL-based
     * interruptions, independent of IRR, TPR and PSR.i:
     * INIT, PMI vectors 0-15 and a machine check, held as its
     * IA64VMM_PAL_EVENT_* bits (0 = none). Owned by the executor. */
    uint64_t pal_xr0, pal_pale_pmi, pal_sale_pmi, pal_event, pal_pmi_vector;
    /* VMM-private PAL min-state capture and boundary service request.
     * Figure 11-2 through BR1, then four old-context RSE words; SAL owns
     * the remainder starting at offset 0x200. No Intel-private XR encoding. */
    uint64_t pal_min_state[62], pal_state_address;
    unsigned pal_state_pending;
    bool pal_state_valid;
    uint16_t pal_pmi_pending, pal_mca_pending;
    bool pal_expected, pal_init_pending;
    struct ia64_fault_site fault;
    /* requested_exit and irq_kick are one aligned 8-byte pair: Tier1's event
     * checkpoint reads both with one load (engine/tier1/exec.h asserts it). */
    unsigned requested_exit; /* zero = none; reason + 1 otherwise */
    unsigned irq_kick;       /* asynchronous mailbox yield, outside lockstep state */
    /* Product engine and SMP state. None of these is architectural
     * state, so IA64_STATE_VERSION is unchanged; tests/tier1/compare.c lists
     * each with its reason. */
    /* MMU change log (above). Written only by this vCPU's executor; never
     * decreases while the struct vcpu exists (vcpu_reset logs IA64_MMU_ALL
     * and keeps the count), so an unchanged count means unchanged contents.
     * vcpu_create starts it in a 2^32 range of its own. */
    struct ia64_microtlb microtlb;
    struct ia64_tlb_index tlb_index;
    struct ia64_timer_cache timer_cache;
    uint64_t mmu_seq;
    struct ia64_mmu_change mmu_log[IA64_MMU_LOG]; /* mmu_log[seq % IA64_MMU_LOG] */
    /* Instruction-side sequence: advanced with
     * mmu_seq by every change but a D-side TC or TR entry, i.e. by those an
     * instruction fetch translation can depend on; same 2^32 range. */
    uint64_t mmu_iseq;
    /* Data-side sequence: all changes except I-side TC/TR entries.
     * Same lifetime and initial range as mmu_seq. */
    uint64_t mmu_dseq;
    /* Global-purge log cursor: sequence of the last purge-log entry applied
     * to this vCPU's TC (release store by the owner). */
    uint64_t purge_seen;
    /* Code-reclamation epoch this executor last passed outside region code
     * (release store by the owner). */
    uint64_t code_quiescent;
    /* Per-vCPU decoded-bundle cache; NULL until the
     * cache becomes per vCPU. Owned by engine/cache. */
    void *decoded;
    /* Engine-private per-vCPU state of the upper tier (Tier1: its struct
     * t1_vcpu), set and cleared by engine/engine.c; NULL otherwise. */
    void *upper;
    /* Idle bracket: 1 while the executor sleeps in
     * hal_idle_until, sequentially consistent stores by the owner. */
    unsigned parked;
    /* 1 while the executor runs ia64_smp_sync_i,
     * sequentially consistent stores by the owner. */
    unsigned sync_i_waiting;
    /* IA64VMM_SVC_PORT_SELECT latch, moved from platform_state;
     * guest-visible platform state. */
    uint32_t service_port;
    uint64_t retired, clock_origin, clock_offset;
    unsigned trace_count, trace_used, trace_next;
    uint64_t loader_sample_counter; /* trace= enables wall-time loader PC samples. */
    struct ia64_trace_entry *trace;
    const struct gm_region *physical_region;
    uint64_t physical_page;
    uint64_t last_successful_bundle, timer_last;
    uint64_t in_service[4];
    bool timer_armed, timer_fired;
    /* Private engine interception: all helper/RSE/PAL memory and clock
     * references participate in lockstep without reissuing device I/O. */
    const struct gm_region *(*memory_hook)(struct vcpu *, gpa_t, unsigned);
    uint64_t (*clock_hook)(const struct vcpu *, uint64_t);
    /* Lockstep access hook: begin supplies replayed read bytes and returns true to suppress
     * the RAM access; completed records/compares its architectural result.
     * Calls follow all fault checks, keep gm_region.kind, and omit fetches. */
    bool (*access_hook)(struct vcpu *, struct lockstep_access *, bool completed);
    void *check_context;
    /* Reused diagnostic workspace, owned and freed with this vCPU. */
    void *check_workspace;
    size_t check_workspace_pages;
    /* Validator entry history is per executor, keyed by translated PA. */
    gpa_t code_page;
    uint64_t code_entry_generation;
    /* NULL until native attach.
     * Engine-owned VMCB/VMCS handle, native/FP buffers, shadow generations,
     * descriptor notification ticket and publication state. Not architectural
     * state; IA64_STATE_VERSION stays 4. The IA-32 engine resets/detaches it before
     * architectural reset/free, on the pinned executor. Never shallow-copy
     * this pointer into an executing lockstep clone. */
    struct ia32_vcpu_private *native;
};

/* Appends one change to v's MMU log. Every writer of
 * s.tr, s.tc, s.rr or s.pkr calls it after the write. */
static inline void ia64_mmu_changed(struct vcpu *v, enum ia64_mmu_change_kind kind, unsigned side,
                                    unsigned index)
{
    v->mmu_log[v->mmu_seq % IA64_MMU_LOG] =
        (struct ia64_mmu_change){(uint8_t)kind, (uint8_t)side, (uint8_t)index};
    ++v->mmu_seq;
    if (side == 0 || kind > IA64_MMU_TR)
        ++v->mmu_iseq;
    if (side == 1 || kind > IA64_MMU_TR)
        ++v->mmu_dseq;
}

unsigned ia64_gr_index(const struct ia64_state *s, unsigned reg);
unsigned ia64_fr_index(const struct ia64_state *s, unsigned reg);
unsigned ia64_pr_index(const struct ia64_state *s, unsigned reg);
uint64_t ia64_gr_read(const struct ia64_state *s, unsigned reg);
bool ia64_nat_read(const struct ia64_state *s, unsigned reg);
struct ia64_gr_value {
    uint64_t value;
    bool nat;
};
/* A value and its NaT belong to the same rotating/stacked home. Resolve
 * that home once for consumers of both, retaining architectural GR0 even
 * when diagnostic state has nonzero bits in its physical storage. */
static inline struct ia64_gr_value ia64_gr_read_value(const struct ia64_state *s, unsigned reg)
{
    if (!reg)
        return (struct ia64_gr_value){0};
    unsigned home = reg < 32 ? reg : ia64_gr_index(s, reg);
    return (struct ia64_gr_value){s->gr[home], s->nat[home]};
}
void ia64_gr_write(struct ia64_state *s, unsigned reg, uint64_t value, bool nat);
bool ia64_pr_read(const struct ia64_state *s, unsigned reg);
void ia64_pr_write(struct ia64_state *s, unsigned reg, bool value);
uint64_t ia64_pr_pack(const struct ia64_state *s);
void ia64_pr_unpack(struct ia64_state *s, uint64_t value, uint64_t mask);
void ia64_rotate(struct ia64_state *s);
uint64_t ia64_itc_read(const struct vcpu *v);
uint64_t ia64_itc_read_unhooked(const struct vcpu *v);
/* Host-clock ITC at one counter sample; phase is the fractional numerator. */
uint64_t ia64_itc_read_host(const struct vcpu *v, uint64_t host, uint64_t hz, uint64_t *phase);
/* Engine-owned opaque break callbacks require independent replay support. */
bool ia64_break_hook_installed(void);
void ia64_itc_write(struct vcpu *v, uint64_t value);
void ia64_psr_write(struct ia64_state *s, uint64_t value);

/* Private debug APIs; the hosted harness can call the trace knob when it
 * gains --trace=N support. */
bool ia64_set_trace(struct vcpu *v, unsigned count);
void ia64_trace_record(struct vcpu *v, const Ia64Instruction *insn);
void ia64_trace_dump(const struct vcpu *v);
const char *ia64_fault_name(enum ia64_fault_kind kind);
void ia64_instruction_dump(const struct vcpu *v);

#endif
