/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_AMD64_VMX_IA32_H
#define IA64VMM_AMD64_VMX_IA32_H

#include "host/amd64/vmx.h"
#include "ia64vmm/ia32.h"

/* Pure IA-32 VMCS transport, Intel SDM Vol.3C chapters 27-30,
 * Vol.3D Appendices A-C. No real-mode policy or descriptor repair. */
bool vmx_ia32_controls(struct vmx_caps *, struct ia32_capabilities *, struct ia32_setup_error *);
bool vmx_ia32_validate(const struct ia32_native_state *, struct ia32_setup_error *);
bool vmx_ia32_control_state(const struct vmx_caps *, const struct ia32_native_state *,
                            struct ia32_setup_error *);
unsigned vmx_ia32_put_state(const struct vmx_caps *, const struct ia32_native_state *,
                            struct vmx_field *, unsigned max);
void vmx_ia32_get_state(vmx_vmread_fn, void *, struct ia32_native_state *);
struct vmx_ia32_exit_fields {
    uint32_t reason, intr_info, intr_error, vectoring_info, vectoring_error;
    uint32_t length, instruction_info, hardware_eflag, interruptibility;
    uint64_t qualification, token, linear;
};
struct ia32_native_exit vmx_ia32_decode_exit(const struct vmx_ia32_exit_fields *,
                                             const struct ia32_native_state *, bool ins_outs_info);

#endif
