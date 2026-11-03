/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_SEMANTICS_H
#define IA64VMM_IA64_SEMANTICS_H
#include "ia64/state.h"

enum ia64_family {
    IA64_FAMILY_NONE,
    IA64_FAMILY_INTEGER,
    IA64_FAMILY_SIMD,
    IA64_FAMILY_BRANCH,
    IA64_FAMILY_MEMORY,
    IA64_FAMILY_SYSTEM,
    IA64_FAMILY_FP
};
enum ia64_nat_rule {
    IA64_NAT_UNKNOWN,
    IA64_NAT_SOURCE_R2,
    IA64_NAT_SOURCE_R3,
    IA64_NAT_SOURCES_R2_R3,
    IA64_NAT_SOURCES_R1_R2_R3,
    IA64_NAT_CLEAR,
    IA64_NAT_SET_IF_R3_SET,
    IA64_NAT_PRESERVE_ON_FULL_ALAT,
    IA64_NAT_R3_OR_UNIMPLEMENTED_VA
};
struct ia64_opcode_info {
    const char *name;
    enum ia64_family family;
    enum ia64_nat_rule nat_rule;
    bool writes_gr;
    bool predicate_pair;
    bool loop_branch;
};
const struct ia64_opcode_info *ia64_opcode_info(Ia64Opcode opcode);
bool ia64_opcode_supported(Ia64Opcode opcode);
bool ia64_opcode_reachable(Ia64Opcode opcode);
struct ia64_result ia64_execute(struct vcpu *v, const Ia64Instruction *insn);
struct ia64_result ia64_integer(struct vcpu *v, const Ia64Instruction *insn);
struct ia64_result ia64_simd(struct vcpu *v, const Ia64Instruction *insn);
struct ia64_result ia64_branch(struct vcpu *v, const Ia64Instruction *insn);
struct ia64_result ia64_system(struct vcpu *v, const Ia64Instruction *insn);
struct ia64_result ia64_system_registers(struct vcpu *v, const Ia64Instruction *insn);
struct ia64_result ia64_memory(struct vcpu *v, const Ia64Instruction *insn);

static inline struct ia64_result ia64_complete(void)
{
    return (struct ia64_result){.kind = IA64_STEP_COMPLETE};
}
static inline struct ia64_result ia64_fault(enum ia64_fault_kind fault, uint64_t info)
{
    return (struct ia64_result){.kind = IA64_STEP_FAULT, .fault = fault, .info = info};
}
static inline struct ia64_result ia64_exit(enum vcpu_exit_reason exit, uint64_t info,
                                           const char *detail)
{
    return (struct ia64_result){
        .kind = IA64_STEP_EXIT, .exit = exit, .info = info, .detail = detail};
}
static inline struct ia64_result ia64_unimplemented(const Ia64Instruction *insn)
{
    return ia64_exit(VCPU_EXIT_UNIMPLEMENTED, insn->raw, ia64_opcode_info(insn->opcode)->name);
}
#endif
