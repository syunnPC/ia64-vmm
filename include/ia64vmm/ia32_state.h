/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA32_STATE_H
#define IA64VMM_IA32_STATE_H
#include "ia64vmm/ia32.h"

struct ia64_state;
struct ia64_fr;

/* Semantic mapping contract, SDM Vol.1 Ch.6 / Vol.2 Ch.9–10.
 * These masks name bits updated on materialization; other AR storage is
 * preserved. Hardware-bound fields contain only defined bits and constants.
 * CFLG/KRs are virtual state, never hardware CRs or descriptor pointers. */
#define IA32_EFLAG_DEFINED UINT64_C(0x003f7fd5)
#define IA32_FCR_DEFINED UINT64_C(0x0000ff8000001f3f)
#define IA32_FSR_DEFINED UINT64_C(0x0000003f5555ffff)
#define IA32_FIR_DEFINED UINT64_C(0x07ffffffffffffff)
#define IA32_FDR_DEFINED UINT64_C(0x0000ffffffffffff)
#define IA32_CFLG_DEFINED UINT64_C(0x000007ffe00501ff)

/* ILLEGAL/PRIVILEGED/RESERVED/DISABLED select the corresponding IA-64
 * fault; FP_PENDING selects IA_32_Exception(16). UNDEFINED and
 * UNREPRESENTABLE require VCPU_EXIT_UNIMPLEMENTED, never native success. */
enum ia32_state_status {
    IA32_STATE_OK,
    IA32_STATE_UNDEFINED,       /* mapped NaT, out-of-range FP or undefined entry PSR */
    IA32_STATE_UNREPRESENTABLE, /* exact stop, not a fabricated guest exception */
    IA32_STATE_ILLEGAL,
    IA32_STATE_PRIVILEGED,
    IA32_STATE_RESERVED,
    IA32_STATE_DISABLED,
    IA32_STATE_FP_PENDING,
};
enum ia32_segment_id {
    IA32_SEG_ES,
    IA32_SEG_CS,
    IA32_SEG_SS,
    IA32_SEG_DS,
    IA32_SEG_FS,
    IA32_SEG_GS,
    IA32_SEG_LDT,
    IA32_SEG_TSS,
    IA32_SEG_GDT,
    IA32_SEG_COUNT,
};

/* attrib is APM-format, with ignored IA-64 fields removed (especially the
 * native long-mode bit). Usable caches are encoded as supplied. loaded=true
 * clears ignored descriptor fields as an IA-32 descriptor load does;
 * otherwise their original IA-64 storage survives a hardware round trip.
 * A loaded native AVL bit is discarded. Null unusable data/LDT segments keep
 * the original cache with P cleared; other unusable segments, 64-bit bases,
 * native L, unexpected attributes and unencodable limits fail exactly. */
void ia32_segment_decode(uint64_t descriptor, uint16_t selector, bool system,
                         struct ia32_segment *out);
enum ia32_state_status ia32_segment_encode(const struct ia32_segment *, uint64_t original,
                                           bool system, bool loaded, uint64_t *out);
enum ia32_state_status ia32_x87_encode(const struct ia64_fr *, uint8_t out[10]);
void ia32_x87_decode(const uint8_t in[10], struct ia64_fr *out);
/* Physical full FTW for environment/save consumers (Vol.4 FXSAVE table). */
uint16_t ia32_x87_tag_word(const struct ia32_fp_image *);

/* Export initializes private restart/debug fields and reserved FP bytes.
 * Import is atomic on failure, updates only dirty semantic groups, clears
 * mapped NaTs and rotates physical x87/TOP. A hardware exit keeps the ISA
 * and PSR.mfl unchanged.
 * With PSR.dfl=1, FR data stays private in the IA-64 source and FP imports
 * are ignored. The caller must enforce Disabled FP faults before any FP,
 * MMX or SSE instruction; even integer execution needs PSR.dfh=0.
 * GDTD exports only base and effective limit; native GDTR requires a
 * 16-bit limit. Import preserves GDTD's other fields, including G. KR1 must
 * equal the encoded TR whenever DIRTY_CONTROL or DIRTY_SEGMENTS is set.
 * loaded_segments is a bitset of ia32_segment_id, requiring DIRTY_SEGMENTS.
 * Preserve the IA-64 source while native owns state: it retains ignored AR
 * and descriptor fields. DIRTY_DEBUG/SHADOW are transport only; IBR/DBR and
 * PSR.cpl remain authoritative and are never inferred from native DR/SS. */
enum ia32_state_status ia32_state_export(const struct ia64_state *, struct ia32_native_state *);
enum ia32_state_status ia32_state_import(struct ia64_state *, const struct ia32_native_state *,
                                         uint64_t dirty, unsigned loaded_segments,
                                         bool instruction_completed);
/* Full materialization immediately before a guest interruption or JMPE.
 * Apply the architectural exit MFL rule even without observed FP use; keep
 * PSR.is=1 until the caller has collected interrupted IPSR or commits JMPE. */
enum ia32_state_status ia32_state_leave(struct ia64_state *, const struct ia32_native_state *,
                                        unsigned loaded_segments, bool instruction_completed);
/* instruction_completed: a full IA-32 instruction completed since the
 * previous materialization, even a branch to itself; BEFORE/PARTIAL/UNKNOWN
 * phases alone are not completions. A successful import with a completion
 * clears PSR.id (Vol.2 Table 3-2/§5.3); failure is atomic. EFLAG.rf comes
 * from native flags, including a partially executed REP. */

enum ia32_record_kind {
    IA32_RECORD_EXCEPTION,
    IA32_RECORD_SOFTWARE_INT,
    IA32_RECORD_INSTRUCTION,
    IA32_RECORD_GATE,
    IA32_RECORD_SYSTEM_FLAG,
    IA32_RECORD_LOCK,
};
struct ia32_record_input {
    enum ia32_record_kind kind;
    enum ia32_exit_phase phase;
    ia32_linear_t source, next;
    bool next_valid;
    uint8_t vector;
    uint16_t code; /* classified SDM error/intercept/concurrent trap code */
    bool execute, read, write, ifa_valid;
    uint64_t ifa;
    uint64_t iim; /* stripped opcode bytes, raw gate, or prior EFLAG */
};
struct ia32_interruption {
    bool valid, iipa_valid, ifa_valid, iim_valid, transition;
    enum ia32_exit_phase phase;
    uint16_t ivt;
    uint64_t isr, iip, iipa, ifa, iim;
};
/* Build records only from classified events; raw backend exits must first
 * pass instruction/access policy. UNKNOWN or missing completed position
 * fails; PARTIAL faults retain the current byte IP. No delivery/IDT access. */
enum ia32_state_status ia32_make_record(const struct ia32_record_input *,
                                        struct ia32_interruption *);

enum ia32_entry_kind { IA32_ENTRY_BRANCH, IA32_ENTRY_RFI };
/* State boundary only: caller validates the decoded br.ia predicate/slot,
 * invalidates engine ALAT/cache metadata, and serializes rfi I/D mappings.
 * rfi consumes CR.IPSR/IIP/IFS, including its normal bank restoration;
 * br.ia never switches banks. Target code/FP/translation checks belong to
 * the first IA-32 instruction, after this commit. completion retains the
 * original 64-bit target for br.ia completion traps. No host entry occurs. */
enum ia32_state_status ia32_state_enter(struct ia64_state *, enum ia32_entry_kind, uint64_t target,
                                        struct ia32_native_state *,
                                        struct ia32_interruption *completion);
/* Invoke check BEFORE reading a JMPE memory operand, then jmpe after that
 * access succeeds. operand is the zero-extended decoded r/m16/32 or absolute
 * disp16/32, next is sequential EIP, data_breakpoints names DBR0..3.
 * A completed JMPE materializes state before changing ISA, has no fence,
 * and produces a post-transition concurrent debug trap when required. */
enum ia32_state_status ia32_state_jmpe_check(const struct ia64_state *,
                                             const struct ia32_native_state *);
enum ia32_state_status ia32_state_jmpe(struct ia64_state *, const struct ia32_native_state *,
                                       uint32_t operand, ia32_eip_t next, unsigned data_breakpoints,
                                       struct ia32_interruption *completion);
/* Memory form: the reader resolves/reads the decoded operand only after
 * transition-disable and pending-numeric checks. A reader failure propagates
 * without a transition; its context retains the classified memory fault.
 * Register/absolute forms use ia32_state_jmpe directly. */
typedef enum ia32_state_status (*ia32_jmpe_reader)(void *context, uint32_t *operand);
enum ia32_state_status ia32_state_jmpe_memory(struct ia64_state *, const struct ia32_native_state *,
                                              ia32_jmpe_reader, void *context, ia32_eip_t next,
                                              unsigned data_breakpoints,
                                              struct ia32_interruption *);
#endif
